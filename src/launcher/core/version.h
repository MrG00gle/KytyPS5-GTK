#pragma once

// Version helpers for the game list (Requirements 7.8 and 7.12).

#include <string>
#include <string_view>

namespace kyty::core {

// Compares two dot-separated versions component by component and returns -1, 0, or 1.
// - Components made only of digits compare numerically: leading zeros are ignored and any length
//   works without overflow ("01.10" equals "1.010", "2" orders before "10").
// - A missing or empty component counts as 0 ("1" equals "1.0" and "1.").
// - Two components that are not both numeric compare as case-insensitive text
//   (CompareCaseInsensitive), except that a numeric component orders before a non-numeric one.
//   That exception keeps the order transitive (a strict weak ordering, as sorting needs): with a
//   plain text comparison, "2" < "10" (numeric), "10" < "10a" (text), and "10a" < "2" (text) would
//   form a cycle.
int CompareVersions(std::string_view a, std::string_view b);

// Formats a param.json requiredSystemSoftwareVersion. The input must be exactly "0x" or "0X", six
// decimal digits MMmmpp, and ten hexadecimal digits; the result is int(MM).mm, plus .pp when pp is
// not 00 ("0x0150000000000000" and "0X0150000000000000" give "1.50", "0x0123450000000000" gives
// "1.23.45"). Any other input gives "".
std::string FormatSystemSoftwareVersion(std::string_view raw);

} // namespace kyty::core
