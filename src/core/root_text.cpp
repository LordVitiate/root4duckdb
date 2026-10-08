#include "root4duckdb/core/root_text.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace duckdb::rootlake
{

std::string Hex64(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::string DoubleText(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

std::string CsvEscape(const std::string& value)
{
    const bool quote = value.empty() || value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote)
    {
        return value;
    }
    std::string result = "\"";
    for (const char character : value)
    {
        result += character == '\"' ? "\"\"" : std::string(1, character);
    }
    result += '\"';
    return result;
}

std::string JoinStrings(const std::vector<std::string>& values, const std::string& delimiter)
{
    std::string result;
    for (uint64_t index = 0; index < values.size(); ++index)
    {
        if (index)
        {
            result += delimiter;
        }
        result += values[index];
    }
    return result;
}

std::string SqlLiteral(const std::string& value)
{
    std::string result = "'";
    for (const char character : value)
    {
        result += character == '\'' ? "''" : std::string(1, character);
    }
    result += '\'';
    return result;
}

std::string RootOpenFailureMessage(const std::string& label, const std::vector<std::string>& failures)
{
    std::ostringstream message;
    message << label;
    const auto limit = std::min<size_t>(failures.size(), 4);
    for (size_t index = 0; index < limit; ++index)
    {
        message << "; " << failures[index];
    }
    if (failures.size() > limit)
    {
        message << "; ...";
    }
    return message.str();
}

} // namespace duckdb::rootlake
