#include "launcher/core/version.h"

#include "launcher/core/text.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace kyty::core {

namespace {

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsHexDigit(char c) {
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsNumeric(std::string_view component) {
	return std::all_of(component.begin(), component.end(), IsDigit);
}

// The '.'-separated components; "" gives one empty component.
std::vector<std::string_view> SplitComponents(std::string_view version) {
	std::vector<std::string_view> components;
	for (;;) {
		const size_t dot = version.find('.');
		components.push_back(version.substr(0, dot));
		if (dot == std::string_view::npos) {
			return components;
		}
		version.remove_prefix(dot + 1);
	}
}

int CompareNumeric(std::string_view a, std::string_view b) {
	a.remove_prefix(std::min(a.find_first_not_of('0'), a.size()));
	b.remove_prefix(std::min(b.find_first_not_of('0'), b.size()));
	if (a.size() != b.size()) {
		return a.size() < b.size() ? -1 : 1;
	}
	const int result = a.compare(b);
	return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

int CompareComponents(std::string_view a, std::string_view b) {
	// An empty component is 0, which is numeric.
	const bool a_numeric = IsNumeric(a);
	const bool b_numeric = IsNumeric(b);
	if (a_numeric && b_numeric) {
		return CompareNumeric(a, b);
	}
	if (a_numeric != b_numeric) {
		return a_numeric ? -1 : 1;
	}
	return CompareCaseInsensitive(a, b);
}

} // namespace

int CompareVersions(std::string_view a, std::string_view b) {
	const auto a_parts = SplitComponents(a);
	const auto b_parts = SplitComponents(b);
	const auto count   = std::max(a_parts.size(), b_parts.size());
	for (size_t i = 0; i < count; i++) {
		// A missing component reads as "", which is 0.
		const auto a_part = i < a_parts.size() ? a_parts[i] : std::string_view {};
		const auto b_part = i < b_parts.size() ? b_parts[i] : std::string_view {};
		const int  result = CompareComponents(a_part, b_part);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

std::string FormatSystemSoftwareVersion(std::string_view raw) {
	constexpr size_t kLength = 2 + 6 + 10;
	if (raw.size() != kLength || raw[0] != '0' || (raw[1] != 'x' && raw[1] != 'X')) {
		return {};
	}
	const auto digits = raw.substr(2, 6);
	const auto tail   = raw.substr(8);
	if (!std::all_of(digits.begin(), digits.end(), IsDigit) ||
	    !std::all_of(tail.begin(), tail.end(), IsHexDigit)) {
		return {};
	}
	const int major = (digits[0] - '0') * 10 + (digits[1] - '0');
	auto      text  = std::to_string(major) + "." + std::string(digits.substr(2, 2));
	if (digits.substr(4, 2) != "00") {
		text += "." + std::string(digits.substr(4, 2));
	}
	return text;
}

} // namespace kyty::core
