#pragma once

// Property test registration (Requirements 17.8, 17.9). Each file
// tests/properties/<group>/P<NN>_<name>.cpp defines one property:
//
//   // Feature: linux-gtk-modernization, Property 7: <title>
//   KYTY_PROPERTY(7, "<title>") {
//       return rc::check(name, [](const std::vector<int>& values) { RC_ASSERT(...); });
//   }
//
// The number is decimal, without leading zeros. The body receives
// name = "Feature: linux-gtk-modernization, Property <n>: <title>" and returns the result of
// rc::check(name, ...). tests/support/propertyMain.cpp runs the property that its argument selects.
// The registry lives in a function-local static, so this header needs no .cpp file of its own.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <rapidcheck.h>
#include <string>
#include <utility>

namespace KytyTests::Property {

using Body = bool (*)(const char* name);

struct Entry {
	std::string name;
	Body        body = nullptr;
};

inline std::map<int, Entry>& Registry() {
	static std::map<int, Entry> registry;
	return registry;
}

inline bool Register(int number, const char* title, Body body) {
	auto name =
	    "Feature: linux-gtk-modernization, Property " + std::to_string(number) + ": " + title;
	if (!Registry().try_emplace(number, Entry {std::move(name), body}).second) {
		std::fprintf(stderr, "property %d is registered twice\n", number);
		std::abort();
	}
	return true;
}

} // namespace KytyTests::Property

#define KYTY_PROPERTY(number, title)                                                               \
	static bool                        KytyPropertyBody_##number(const char* name);                \
	[[maybe_unused]] static const bool KytyPropertyRegistered_##number =                           \
	    ::KytyTests::Property::Register(number, title, &KytyPropertyBody_##number);                \
	static bool KytyPropertyBody_##number(const char* name)
