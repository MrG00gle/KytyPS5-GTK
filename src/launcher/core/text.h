#pragma once

// UTF-8 text helpers for search, sorting, and display (Requirements 7.12, 7.13, 7.14).

#include <string>
#include <string_view>

namespace kyty::core {

// Removes the Unicode White_Space code points (U+0009-U+000D, U+0020, U+0085, U+00A0, U+1680,
// U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000), decoded as UTF-8, from both ends. Bytes
// that are not valid UTF-8 are not whitespace.
std::string_view TrimWhitespace(std::string_view text);

// True when needle occurs in haystack ignoring letter case. Both are compared code point by code
// point: two code points match when SDL_strcasecmp finds them equal (their Unicode full case
// foldings are equal: U+00C9 matches U+00E9, U+212A KELVIN SIGN matches k, U+1E9E matches U+00DF),
// and a byte of an ill-formed sequence matches only the same bytes. A code point whose folding has
// several code points never matches a part of it ("ss" does not find U+00DF), so needle + x never
// matches where needle does not (Requirement 7.14); SDL_strcasestr lacks that property, and it
// misses matches between code points of different UTF-8 lengths. An empty needle always matches.
// The comparison ends at an embedded NUL byte.
bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle);

// -1, 0, or 1 as a orders before, equal to, or after b ignoring letter case (SDL_strcasecmp, which
// folds Unicode case on UTF-8). The comparison ends at an embedded NUL byte.
int CompareCaseInsensitive(std::string_view a, std::string_view b);

// The text with each maximal subpart of an ill-formed UTF-8 sequence replaced by U+FFFD (the
// Unicode "substitution of maximal subparts" practice, as the WHATWG decoder does): overlong forms,
// surrogates (U+D800-U+DFFF), code points above U+10FFFF, stray continuation bytes, and truncated
// sequences. Valid text is returned unchanged.
std::string SanitizeUtf8(std::string_view bytes);

} // namespace kyty::core
