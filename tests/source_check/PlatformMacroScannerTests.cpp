// Example tests of the platform macro scanner (tests/source_check/platformMacroScanner.h,
// Requirements 5.7 and 5.10). The forbidden names come from ForbiddenMacros(), so this file never
// spells them.

#include "source_check/platformMacroScanner.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace SourceCheck = KytyTests::SourceCheck;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "PlatformMacroScannerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Macro(size_t index) {
	return std::string(SourceCheck::ForbiddenMacros()[index]);
}

std::string Describe(const std::vector<SourceCheck::Finding>& findings) {
	std::string text;
	for (const auto& finding: findings) {
		text +=
		    "\n  " + finding.path + ":" + std::to_string(finding.line) + ": " + finding.directive;
	}
	return text.empty() ? " (none)" : text;
}

// Checks that scanning text reports exactly the given lines.
void CheckLines(const std::string& name, const std::string& text,
                const std::vector<uint32_t>& lines) {
	const auto            findings = SourceCheck::ScanText("case.cpp", text);
	std::vector<uint32_t> found;
	for (const auto& finding: findings) {
		found.push_back(finding.line);
	}
	Check(found == lines,
	      name + ": unexpected findings for\n" + text + "\nfound:" + Describe(findings));
}

void TestForbiddenMacros() {
	const auto names = SourceCheck::ForbiddenMacros();
	Check(names.size() == 7, "ForbiddenMacros() lists the seven names of Requirement 5.7");
	for (size_t i = 0; i < names.size(); i++) {
		CheckLines("each name in #ifdef", "#ifdef " + Macro(i) + "\n#endif\n", {1});
		CheckLines("each name in #if defined()", "\n#if defined(" + Macro(i) + ")\n#endif\n", {2});
	}
}

void TestContinuation() {
	const std::string name = Macro(0);
	CheckLines("condition split by a continuation",
	           "int a;\n#if defined(FOO) || \\\n    defined(" + name + ")\n#endif\n", {2});
	CheckLines("identifier split by a continuation",
	           "#ifdef " + name.substr(0, 3) + "\\\n" + name.substr(3) + "\nint b;\n#ifdef " +
	               name + "\n",
	           {1, 4});
	CheckLines("directive name split by a continuation, CRLF line ends",
	           "#el\\\r\nif " + name + "\r\n#endif\r\n", {1});
	CheckLines("spaces between the backslash and the newline", "#if \\  \n" + name + "\n", {1});
	CheckLines("continued line comment hides the next line",
	           "// note \\\n#ifdef " + name + "\n#ifdef " + name + "\n", {3});
}

void TestComments() {
	const std::string name = Macro(6);
	CheckLines("directive in a block comment", "/*\n#if " + name + "\n*/\nint a;\n", {});
	CheckLines("directive in a line comment", "// #ifdef " + name + "\n", {});
	CheckLines("comment between # and if", "#/* c */if " + name + "\n", {1});
	CheckLines("comments around the directive", "/* lead */ # /* x */ ifdef " + name + " // tail\n",
	           {1});
	CheckLines("multi-line comment before #", "/* a\n */ #ifdef " + name + "\n", {2});
	CheckLines("name only in a comment of the condition",
	           "#if 0 /* " + name + " */\n#ifdef OTHER // " + name + "\n", {});
	CheckLines("multi-line comment inside the condition", "#if 0 /* x\n */ || " + name + "\n", {1});
	CheckLines("# after a token is no directive", "int a; #ifdef " + name + "\n", {});
}

void TestLiterals() {
	const std::string name = Macro(1);
	CheckLines("directive in a string literal",
	           "const char* s = \"#if " + name + " /* //\";\n#ifdef " + name + "\n", {2});
	CheckLines("escaped quote and character literals",
	           "const char* s = \"\\\"#if\"; char c = '\"';\n#ifndef " + name + "\n", {2});
	CheckLines("directive in a raw string",
	           "auto r = R\"kyty(\n#ifdef " + name + "\n)\" still raw\n)kyty\";\n#ifdef " + name +
	               "\n",
	           {5});
	CheckLines("prefixed raw string, no splicing inside",
	           "auto r = u8R\"(\\\n#if " + name + "\n)\";\n", {});
	CheckLines("name in a string of the condition",
	           "#if __has_include(\"" + name + ".h\")\n#elif L'x' == '" + name + "'\n", {});
	CheckLines("R without a quote is an identifier", "#if R || " + name + "\n", {1});
}

void TestElifdefAndElifndef() {
	CheckLines("#elifdef and #elifndef",
	           "#if 0\n#elifdef " + Macro(2) + "\n#elifndef " + Macro(3) + "\n#else\n#endif\n",
	           {2, 3});
	CheckLines("#elif with spaces after #", "#if 0\n#   elif\t!defined(" + Macro(4) + ")\n#endif\n",
	           {2});
}

void TestLookAlikes() {
	const std::string win  = Macro(0);
	const std::string apl  = Macro(6);
	const std::string text = "#ifdef " + win + "X\n#if defined(MY" + apl + ")\n#if " + Macro(2) +
	                         "_X || $" + win + "\n#ifdef " + Macro(1) + "$\n#if " + win + "0\n";
	CheckLines("look-alike identifiers", text, {});
}

void TestOtherDirectives() {
	const std::string name = Macro(5);
	CheckLines("non-conditional directives",
	           "#define " + name + " 1\n#error " + name + " is not supported\n#undef " + name +
	               "\n#pragma " + name + "\n#include \"" + name + ".h\"\n#ifdefx " + name + "\n",
	           {});
	CheckLines("unterminated character literal in #error ends at the line end",
	           "#error don't use " + name + "\n#ifdef " + name + "\n", {2});
}

void TestPpNumbers() {
	const std::string name = Macro(3);
	CheckLines("digit separator opens no character literal",
	           "#if 1'0 == 10 && defined(" + name + ")\n", {1});
	CheckLines("pp-number with the name as a suffix", "#if 1" + name + " || 0x1p+" + name + "\n",
	           {});
	CheckLines("pp-number with an exponent sign", "#if 1e+" + name + " || .5 || " + name + "\n",
	           {1});
}

void TestLineNumbersAndDirectiveText() {
	const std::string name = Macro(4);
	const std::string text =
	    "\xEF\xBB\xBF#ifndef " + name + "\r\n\r\n  #  if 1 || \\\n " + name + " // c\n";
	const auto findings = SourceCheck::ScanText("dir/file.h", text);
	Check(findings.size() == 2, "two findings after a byte order mark:" + Describe(findings));
	Check(findings[0].path == "dir/file.h" && findings[0].line == 1 &&
	          findings[0].directive == "#ifndef " + name,
	      "first finding:" + Describe(findings));
	Check(findings[1].line == 3 && findings[1].directive == "#  if 1 ||  " + name + " // c",
	      "second finding:" + Describe(findings));
	CheckLines("directive at the end of the text without a newline", "int a;\n#ifdef " + name, {2});
}

class TempDir {
public:
	TempDir() {
		std::string pattern =
		    (std::filesystem::temp_directory_path() / "kyty_source_check_XXXXXX").string();
		Check(mkdtemp(pattern.data()) != nullptr, "mkdtemp");
		m_path = pattern;
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}
	TempDir(const TempDir&)            = delete;
	TempDir& operator=(const TempDir&) = delete;

	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};

void Write(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
	Check(static_cast<bool>(out), "write " + path.string());
}

void TestScanTree() {
	const std::string name = Macro(6);
	const TempDir     dir;
	const auto&       root = dir.Path();
	Write(root / "src" / "b.cpp", "int b;\n#ifdef " + name + "\n");
	Write(root / "src" / "sub" / "a.h", "#if " + name + "\n");
	std::string binary = "#ifdef " + name + "\n";
	binary.push_back('\0');
	Write(root / "src" / "blob.bin", binary);
	Write(root / "tests" / "t.inc", "#elif defined(" + name + ")\n");
	Write(root / "outside" / "o.cpp", "#ifdef " + name + "\n");
	std::filesystem::create_symlink(root / "outside" / "o.cpp", root / "src" / "link.cpp");
	std::filesystem::create_directory_symlink(root / "outside", root / "tests" / "linkdir");

	const std::vector<std::string> subdirs = {"src", "tests"};
	std::string                    error;
	const auto                     findings = SourceCheck::ScanTree(root, subdirs, &error);
	Check(error.empty(), "ScanTree error: " + error);
	Check(findings.size() == 3, "binary files and symlinks are skipped:" + Describe(findings));
	Check(findings[0].path == "src/b.cpp" && findings[0].line == 2,
	      "src/b.cpp:2:" + Describe(findings));
	Check(findings[1].path == "src/sub/a.h" && findings[1].line == 1,
	      "src/sub/a.h:1:" + Describe(findings));
	Check(findings[2].path == "tests/t.inc" && findings[2].line == 1,
	      "tests/t.inc:1:" + Describe(findings));

	const std::vector<std::string> missing = {"src", "missing"};
	error.clear();
	const auto partial = SourceCheck::ScanTree(root, missing, &error);
	Check(error.find("missing: not a directory") != std::string::npos,
	      "missing subdir error: " + error);
	Check(partial.size() == 2, "the other subdirs are still scanned:" + Describe(partial));
}

} // namespace

int main() {
	TestForbiddenMacros();
	TestContinuation();
	TestComments();
	TestLiterals();
	TestElifdefAndElifndef();
	TestLookAlikes();
	TestOtherDirectives();
	TestPpNumbers();
	TestLineNumbersAndDirectiveText();
	TestScanTree();
	std::printf("PlatformMacroScannerTests: all tests passed\n");
	return 0;
}
