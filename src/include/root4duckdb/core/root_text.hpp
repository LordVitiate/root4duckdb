#ifndef ROOT4DUCKDB_CORE_ROOT_TEXT_HPP
#define ROOT4DUCKDB_CORE_ROOT_TEXT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake
{

std::string Hex64(uint64_t value);
std::string DoubleText(double value);
std::string CsvEscape(const std::string& value);
std::string JoinStrings(const std::vector<std::string>& values, const std::string& delimiter);
std::string SqlLiteral(const std::string& value);
std::string RootOpenFailureMessage(const std::string& label, const std::vector<std::string>& failures);

} // namespace duckdb::rootlake

#endif
