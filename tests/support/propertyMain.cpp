// Shared main() of the property test executables (cmake/KytyTesting.cmake,
// kyty_add_property_executable).
//
//   <executable> <property number>   runs that property: exit 0 when it holds, 1 when it fails
//   <executable> --list               prints the registered properties
//
// An unknown number or a bad argument exits 2. RapidCheck prints "Using configuration: seed=<N>"
// before the property runs and the shrunk counterexample when it fails; rerunning with
// RC_PARAMS="seed=<N>" reproduces it (Requirement 17.9). Every run tests at least 100 cases
// (Requirement 17.8): max_success=100 is added to the caller's RC_PARAMS when it has none, and a
// smaller max_success is raised to 100. Every other RC_PARAMS setting, such as seed, is kept.

#include "support/property.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

constexpr long kMinSuccess = 100;

bool IsSpace(char c) {
	return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Returns RC_PARAMS with every max_success below kMinSuccess raised to it, or with
// max_success=<kMinSuccess> appended when it has none.
std::string MergeMaxSuccess(std::string params) {
	static constexpr std::string_view Key   = "max_success=";
	bool                              found = false;
	for (size_t begin = 0; begin < params.size();) {
		if (IsSpace(params[begin])) {
			begin++;
			continue;
		}
		size_t end = begin;
		while (end < params.size() && !IsSpace(params[end])) {
			end++;
		}
		const std::string_view token(params.data() + begin, end - begin);
		if (token.starts_with(Key)) {
			found = true;
			const std::string value(token.substr(Key.size()));
			char*             value_end = nullptr;
			errno                       = 0;
			const long number           = std::strtol(value.c_str(), &value_end, 10);
			if (!value.empty() && *value_end == '\0' && errno == 0 && number < kMinSuccess) {
				std::fprintf(
				    stderr,
				    "propertyMain: raising max_success=%s in RC_PARAMS to %ld (every property "
				    "runs at least %ld cases)\n",
				    value.c_str(), kMinSuccess, kMinSuccess);
				const std::string raised = std::string(Key) + std::to_string(kMinSuccess);
				params.replace(begin, end - begin, raised);
				end = begin + raised.size();
			}
		}
		begin = end;
	}
	if (!found) {
		params += params.empty() ? "" : " ";
		params += std::string(Key) + std::to_string(kMinSuccess);
	}
	return params;
}

void PrintRegistered(std::FILE* stream) {
	for (const auto& [number, entry]: KytyTests::Property::Registry()) {
		std::fprintf(stream, "  %d  %s\n", number, entry.name.c_str());
	}
}

int Usage(const char* program) {
	std::fprintf(stderr, "usage: %s <property number> | --list\n", program);
	return 2;
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		return Usage(argv[0]);
	}
	const std::string_view argument = argv[1];
	if (argument == "--list") {
		PrintRegistered(stdout);
		return 0;
	}

	char* number_end  = nullptr;
	errno             = 0;
	const long number = std::strtol(argv[1], &number_end, 10);
	if (argument.empty() || !std::isdigit(static_cast<unsigned char>(argument[0])) ||
	    *number_end != '\0' || errno != 0) {
		return Usage(argv[0]);
	}
	const auto& registry = KytyTests::Property::Registry();
	const auto  entry    = registry.find(static_cast<int>(number));
	if (number > 1000 || entry == registry.end()) {
		std::fprintf(stderr, "%s: unknown property %s; registered properties:\n", argv[0], argv[1]);
		PrintRegistered(stderr);
		return 2;
	}

	// RapidCheck reads RC_PARAMS when the first property runs.
	const char* params = std::getenv("RC_PARAMS");
	if (setenv("RC_PARAMS", MergeMaxSuccess(params != nullptr ? params : "").c_str(), 1) != 0) {
		std::perror("propertyMain: setenv RC_PARAMS");
		return 2;
	}
	return entry->second.body(entry->second.name.c_str()) ? 0 : 1;
}
