# ROOT access architecture

## Scope

`read_root`, `read_root_dataset`, `root_build_index`, and
`root_build_dataset_index` use the same semantic access model. Metadata-only
functions keep their own catalog path because they do not decode ROOT entries.
Histogram reads remain a specialized strategy selected by the direct-scan
mode, but use the same bind and lifetime boundaries.

## Layers

1. `PathResolver` resolves a logical path into immutable `PathLevel` metadata.
2. `RootPathReader` is the facade used by direct, dataset, and index consumers.
3. `SerializedReadPlan` contains exactly one `SerializedProjectionState`.
4. `SerializedBasketReader` executes that plan against bounded entry bytes.
5. `RootObjectReader` is the validation oracle and automatic fallback.
6. The consumer materializes DuckDB rows or index metadata from the common
   `RootPrimitiveValue` representation.

The serialized reader never owns or aliases the fallback object. Validation
opens an isolated ROOT context, so branch addresses and container scratch
cannot affect the projected reader.

## Explicit states

`RootScanBindData::scan_mode` is a `std::variant`. A direct scan is therefore
exactly one of semantic, browse, direct primitive branch, primitive tree,
histogram, or empty. It cannot be histogram and browse at the same time.

`SerializedReadPlan::projection` is also a `std::variant`. A plan is either
rejected (optionally with one reason) or selects one executable projection shape. The old
combination `supported=false` plus a live projection enum is not representable.

`RootPathReaderStartResult` and `RootPathReadResult` use exclusive route enums.
A validation mismatch can record that bytes decoded successfully while still
selecting object fallback, without contradictory `serialized=true` and
`fallback=true` flags.

## Ownership and lifetime

`RootObjectContext` is noncopyable and immovable. `RootObjectReader` owns it by
`std::unique_ptr`, so moving the reader does not relocate the address slot that
ROOT stores in `TBranch`. Cleanup clears ownership state before calling external
ROOT destructors and catches exceptions at the ABI boundary.

`SerializedBasketReader` owns collection and leaf scratch. Its destructor,
`Reset`, and `ReleaseBindings` are `noexcept`; branch detachment and dictionary
destruction cannot escape cleanup.

## Untrusted bytes

`CheckedByteCursor` is the boundary for serialized headers and variable-length
payloads. It uses subtraction-based range checks, advances only after a
successful read, and leaves the cursor unchanged on failure. Multiplication and
container growth are checked before converting to `size_t` or reserving memory.

## Shared policy

`RootAccessOptions` is embedded by all three execution families. It owns reader
mode, validation count, entry/value limits, tree cache size, fallback branch
policy, dictionary cleanup, and operation name. `Validate()` is the single
validation point for the byte and value safety limits.

## CERT C++ mapping

| Concern | Design response |
| --- | --- |
| DCL57-CPP, ERR55-CPP | Destructors and cleanup functions are `noexcept`; external ROOT cleanup is contained. |
| ERR56-CPP, ERR57-CPP | State is committed with RAII/variants; resource ownership is cleared before external cleanup. |
| ERR59-CPP | No exception is allowed to cross ROOT cleanup or destructor boundaries. |
| EXP54-CPP, MEM50-CPP | ROOT branch address slots have stable lifetime and are detached before destruction. |
| EXP63-CPP | Moved-from readers are valid only for reset/rebind; accessors tolerate an empty owner. |
| MEM51-CPP | Dictionary-created objects and scratch collections have one explicit owner. |
| CTR50-CPP, CTR52-CPP | Byte cursor and size arithmetic validate bounds before indexing, pointer arithmetic, or allocation. |
| OOP50-CPP through OOP57-CPP | Runtime alternatives use composition and `std::variant`, not an inheritance hierarchy with virtual lifetime hazards. |

## Fallback contract

In `reader_mode='auto'`, a rejected plan, runtime layout mismatch, failed safety
check, or validation mismatch activates `RootObjectReader` and emits one warning
per schema/path/reason. In `reader_mode='serialized'`, the same condition is a
hard error. In `reader_mode='object'`, serialized decoding is not started.

Custom streamers, pointer fields, and unknown schema evolution are never guessed.
They either have an explicit serialized plan or follow the fallback contract.
