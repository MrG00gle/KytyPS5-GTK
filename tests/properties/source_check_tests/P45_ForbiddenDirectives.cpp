// Feature: linux-gtk-modernization, Property 45: Source check reports every forbidden directive
//
// Validates Requirement 5.10. Each case writes 1-3 files of 0-40 self-contained fragments to a
// fresh temporary directory and scans it with ScanTree(). Fragment kinds:
//   1. code lines over identifiers that include forbidden names and look-alikes;
//   2. line comments holding directive text;
//   3. block comments of 1-3 lines holding directive lines;
//   4. one-line string literals holding directive text, "/*", and "//";
//   5. raw strings spanning lines with directive text;
//   6. allowed directives;
//   7. look-alike conditionals (look-alike names, forbidden names only in comments and literals);
//   8. forbidden conditionals: one of the six directive kinds, random '#' spacing, a forbidden name
//      somewhere in a random expression, an optional trailing comment, and 0-3 backslash
//      continuations at random positions.
// Only kind 8 is expected, at its first line, and only kind 8 has a line that ends with a
// backslash. The forbidden names come from ForbiddenMacros(), so this file never spells them.

#include "source_check/platformMacroScanner.h"
#include "support/property.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace SourceCheck = KytyTests::SourceCheck;

using Location = std::pair<std::string, uint32_t>;

struct Fragment {
	std::vector<std::string> lines;
	bool                     forbidden = false;
};

const std::vector<std::string> kConditionalKinds = {"if",   "ifdef",   "ifndef",
                                                    "elif", "elifdef", "elifndef"};
const std::vector<std::string> kNeutralNames     = {
    "KYTY_PLATFORM_LINUX", "FOO",        "BAR_2", "value", "count",
    "__linux__",           "__x86_64__", "R",     "u8",    "L"};

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes, which
// would make the later fragment kinds, kind 8 included, rare.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

std::string PickOf(const std::vector<std::string>& values) {
	return *rc::gen::elementOf(values);
}

bool Chance() {
	return *rc::gen::arbitrary<bool>();
}

std::string ForbiddenName() {
	const auto names = SourceCheck::ForbiddenMacros();
	return std::string(names[Choose<size_t>(0, names.size())]);
}

// A different identifier that contains a forbidden name or differs from one in case.
std::string LookAlike() {
	std::string name = ForbiddenName();
	switch (Choose(0, 6)) {
		case 0: return name + "X";
		case 1: return "MY" + name;
		case 2: return name + "_2";
		case 3: return "$" + name;
		case 4: return name + "0";
		default:
			std::transform(name.begin(), name.end(), name.begin(), [](char c) {
				return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
			});
			return name;
	}
}

std::string AnyIdentifier() {
	switch (Choose(0, 3)) {
		case 0: return ForbiddenName();
		case 1: return LookAlike();
		default: return PickOf(kNeutralNames);
	}
}

std::string HashPrefix() {
	return PickOf({"#", " #", "\t#", "  #  ", "# ", "#\t", "/* c */ #", "#/**/"});
}

// The text of a forbidden conditional, used as data by the comment and literal kinds. It contains
// no quote, backslash, or "*/".
std::string DirectiveText() {
	return "#" + PickOf(kConditionalKinds) + " " +
	       (Chance() ? "defined(" + ForbiddenName() + ")" : ForbiddenName());
}

// An if or elif operand; it names `name`.
std::string Operand(const std::string& name) {
	switch (Choose(0, 6)) {
		case 0: return "defined(" + name + ")";
		case 1: return "defined " + name;
		case 2: return "!defined(" + name + ")";
		case 3: return name;
		case 4: return "(" + name + " || 0)";
		default: return name + " >= 1'0";
	}
}

Fragment CodeLines() {
	Fragment   fragment;
	const auto count = Choose(1, 4);
	for (int i = 0; i < count; i++) {
		switch (Choose(0, 4)) {
			case 0:
				fragment.lines.push_back("int " + AnyIdentifier() + " = " + AnyIdentifier() +
				                         " + 1'000;");
				break;
			case 1:
				fragment.lines.push_back("\tcall(" + AnyIdentifier() + ", " + AnyIdentifier() +
				                         ");");
				break;
			case 2:
				fragment.lines.push_back("x = " + AnyIdentifier() + " * 0x1p+3; # ifdef " +
				                         ForbiddenName());
				break;
			default:
				fragment.lines.push_back("auto " + PickOf(kNeutralNames) +
				                         "_v = " + AnyIdentifier() + ";");
				break;
		}
	}
	return fragment;
}

Fragment LineComment() {
	return {{PickOf({"// ", "/// ", "  //", "x = 1; // "}) + DirectiveText()}};
}

Fragment BlockComment() {
	const std::string prefix = PickOf({"", "x = 1; ", "  "});
	switch (Choose(1, 4)) {
		case 1: return {{prefix + "/* " + DirectiveText() + " */"}};
		case 2: return {{prefix + "/* " + DirectiveText(), DirectiveText() + " */"}};
		default: return {{prefix + "/*", DirectiveText(), "*/"}};
	}
}

Fragment StringLiteral() {
	const std::string text = DirectiveText() + PickOf({" /* ", " // ", " /* // "});
	switch (Choose(0, 3)) {
		case 0: return {{"const char* s = \"" + text + "\";"}};
		case 1: return {{"char c = '\"'; auto s = u8\"" + text + "\";"}};
		default: return {{"puts(\"\\\"" + text + "\");"}};
	}
}

Fragment RawString() {
	const std::string delimiter = PickOf({"", "kyty", "x"});
	Fragment          fragment;
	fragment.lines.push_back("auto r = " + PickOf({"", "u8", "L", "u", "U"}) + "R\"" + delimiter +
	                         "(" + (Chance() ? DirectiveText() : ""));
	const auto count = Choose(1, 4);
	for (int i = 0; i < count; i++) {
		// )" ends the raw string only when the delimiter is empty.
		fragment.lines.push_back((!delimiter.empty() && Chance() ? ")\" " : "") + DirectiveText());
	}
	fragment.lines.push_back(")" + delimiter + "\";");
	return fragment;
}

Fragment AllowedDirective() {
	const std::string name = ForbiddenName();
	const std::string hash = HashPrefix();
	switch (Choose(0, 10)) {
		case 0: return {{hash + "if KYTY_PLATFORM == KYTY_PLATFORM_LINUX"}};
		case 1: return {{hash + "ifdef OTHER"}};
		case 2: return {{hash + "define " + name + " 1"}};
		case 3: return {{hash + "error " + name + " is not supported"}};
		case 4: return {{hash + "error don't use " + name}};
		case 5: return {{hash + "include \"" + name + ".h\""}};
		case 6: return {{hash + "else"}};
		case 7: return {{hash + "endif // " + name}};
		case 8: return {{hash + "undef " + name}};
		default: return {{hash + "pragma " + name}};
	}
}

Fragment LookAlikeConditional() {
	const std::string kind = PickOf(kConditionalKinds);
	const std::string hash = HashPrefix();
	switch (Choose(0, 6)) {
		case 0: return {{hash + kind + " " + LookAlike()}};
		case 1:
			return {{hash + kind + " defined(" + LookAlike() + ") /* " + ForbiddenName() + " */"}};
		case 2: return {{hash + kind + " " + PickOf(kNeutralNames) + " // " + ForbiddenName()}};
		case 3: return {{hash + kind + " __has_include(\"" + ForbiddenName() + ".h\")"}};
		case 4: return {{hash + kind + " 1" + ForbiddenName() + " || 0x1p+" + ForbiddenName()}};
		default: return {{hash + kind + " '" + ForbiddenName() + "' == " + LookAlike()}};
	}
}

Fragment ForbiddenConditional() {
	const std::string kind = PickOf(kConditionalKinds);
	const std::string hash = HashPrefix();
	const std::string name = ForbiddenName();
	std::string       condition;
	if (kind == "if" || kind == "elif") {
		const auto count     = Choose(1, 5);
		const auto forbidden = Choose(0, count);
		for (int i = 0; i < count; i++) {
			if (i > 0) {
				condition += Chance() ? " || " : " && ";
			}
			condition += Operand(i == forbidden ? name : AnyIdentifier());
		}
	} else {
		condition = name;
	}
	std::string text = hash + kind + PickOf({" ", "  ", "\t", " /* c */ "}) + condition;
	if (Chance()) {
		text += PickOf({" // trailing comment", " /* trailing */", "\t// " + AnyIdentifier()});
	}

	// Splices removed by translation phase 2 do not change the directive, wherever they are.
	const size_t        first = hash.size();
	std::vector<size_t> splits;
	const auto          continuations = Choose(0, 4);
	for (int i = 0; i < continuations; i++) {
		splits.push_back(Choose(first, text.size()));
	}
	std::sort(splits.begin(), splits.end());
	splits.erase(std::unique(splits.begin(), splits.end()), splits.end());

	Fragment fragment;
	fragment.forbidden = true;
	size_t begin       = 0;
	for (const size_t split: splits) {
		fragment.lines.push_back(text.substr(begin, split - begin) + "\\");
		begin = split;
	}
	fragment.lines.push_back(text.substr(begin));
	return fragment;
}

Fragment MakeFragment() {
	switch (Choose(1, 9)) {
		case 1: return CodeLines();
		case 2: return LineComment();
		case 3: return BlockComment();
		case 4: return StringLiteral();
		case 5: return RawString();
		case 6: return AllowedDirective();
		case 7: return LookAlikeConditional();
		default: return ForbiddenConditional();
	}
}

class TempDir {
public:
	TempDir() {
		std::string pattern = (std::filesystem::temp_directory_path() / "kyty_p45_XXXXXX").string();
		RC_ASSERT(mkdtemp(pattern.data()) != nullptr);
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

} // namespace

KYTY_PROPERTY(45, "Source check reports every forbidden directive") {
	return rc::check(name, []() {
		const TempDir      dir;
		std::set<Location> expected;
		const auto         file_count = Choose(1, 4);
		for (int f = 0; f < file_count; f++) {
			const std::string path = "src/f" + std::to_string(f) + PickOf({".cpp", ".h", ".inc"});
			const std::string eol  = Chance() ? "\n" : "\r\n";
			std::string       text;
			uint32_t          line           = 1;
			const auto        fragment_count = *rc::gen::inRange(0, 41); // grows with the size
			for (int i = 0; i < fragment_count; i++) {
				const Fragment fragment = MakeFragment();
				if (fragment.forbidden) {
					expected.emplace(path, line);
				}
				for (const auto& fragment_line: fragment.lines) {
					text += fragment_line + eol;
					line++;
				}
			}
			// Sometimes the last line has no line end.
			if (!text.empty() && Chance()) {
				text.resize(text.size() - eol.size());
			}
			RC_LOG() << "--- " << path << " ---\n" << text << "\n";
			std::filesystem::create_directories(dir.Path() / "src");
			std::ofstream out(dir.Path() / path, std::ios::binary);
			out << text;
			out.close();
			RC_ASSERT(static_cast<bool>(out));
		}

		const std::vector<std::string> subdirs = {"src"};
		std::string                    error;
		const auto findings = SourceCheck::ScanTree(dir.Path(), subdirs, &error);
		RC_ASSERT(error.empty());
		std::set<Location> found;
		for (const auto& finding: findings) {
			found.emplace(finding.path, finding.line);
		}
		RC_ASSERT(found == expected);
		RC_ASSERT(findings.size() == expected.size());
		RC_TAG(expected.empty()       ? "0 expected"
		       : expected.size() < 10 ? "1-9 expected"
		                              : "10+ expected");
	});
}
