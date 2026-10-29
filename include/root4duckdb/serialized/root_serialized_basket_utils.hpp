#ifndef ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_BASKET_UTILS_HPP
#define ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_BASKET_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

class TBranch;
class TBasket;

namespace duckdb::rootlake
{

struct SerializedBasketRange
{
    int number = -1;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
};

// Requires the caller's bound physical branch.
bool ResolveSerializedBasket(TBranch* branch, uint64_t entry, SerializedBasketRange& range,
                             std::string& failure_reason);
// Requires a loaded basket covering the requested entry.
bool SliceSerializedBasket(TBasket* basket, uint64_t entry, uint64_t entry_begin, uint64_t max_entry_bytes,
                           bool& prepared, const uint8_t*& begin, size_t& size, std::string& failure_reason);

} // namespace duckdb::rootlake

#endif
