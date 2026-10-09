#ifndef ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_PLAN_LAYOUT_HPP
#define ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_PLAN_LAYOUT_HPP

#include <cstdint>
#include <string>
#include <vector>

class TStreamerElement;

namespace duckdb::rootlake
{
uint64_t FixedArrayLength(TStreamerElement* element, std::vector<uint32_t>* dimensions = nullptr);
bool FixedPrimitiveWidth(TStreamerElement* element, uint32_t& width, std::string& reason);
}

#endif
