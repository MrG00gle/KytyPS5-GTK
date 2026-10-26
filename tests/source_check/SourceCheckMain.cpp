// source_check --root <dir> <subdir>...
//
// Scans every regular file under <dir>/<subdir> for #if, #ifdef, #ifndef, #elif, #elifdef, and
// #elifndef directives whose condition names a macro of Requirement 5.7 (platformMacroScanner.h),
// and prints "path:line: <directive>" for each one, with the path relative to <dir>. The CTest
// source_check_platform_macros runs it on src and tests (Requirement 5.10).
//
// Exit status: 0 when nothing matches, 1 when a directive matches, 2 on a usage or I/O error.

#include "source_check/platformMacroScanner.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int Usage(const char* program) {
	std::fprintf(stderr, "usage: %s --root <dir> <subdir>...\n", program);
	return 2;
}

std::string Join(const std::vector<std::string>& values, std::string_view separator) {
	std::string text;
	for (const auto& value: values) {
		text += text.empty() ? "" : separator;
		text += value;
	}
	return text;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 4 || std::string_view(argv[1]) != "--root") {
		return Usage(argv[0]);
	}
	const std::string              root = argv[2];
	const std::vector<std::string> subdirs(argv + 3, argv + argc);

	std::string error;
	const auto  findings = KytyTests::SourceCheck::ScanTree(root, subdirs, &error);
	for (const auto& finding: findings) {
		std::printf("%s:%u: %s\n", finding.path.c_str(), finding.line, finding.directive.c_str());
	}

	std::vector<std::string> names;
	for (const auto name: KytyTests::SourceCheck::ForbiddenMacros()) {
		names.emplace_back(name);
	}
	if (!findings.empty()) {
		std::printf(
		    "source_check: %zu conditional directive(s) under %s name a removed-platform macro "
		    "(%s); Requirement 5.7 allows none\n",
		    findings.size(), Join(subdirs, ", ").c_str(), Join(names, ", ").c_str());
	}
	if (!error.empty()) {
		std::fflush(stdout);
		std::fprintf(stderr, "source_check: I/O error:\n%s\n", error.c_str());
		return 2;
	}
	if (!findings.empty()) {
		return 1;
	}
	std::printf("source_check: no conditional directive under %s names %s\n",
	            Join(subdirs, ", ").c_str(), Join(names, ", ").c_str());
	return 0;
}
