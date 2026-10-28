// Example tests of the QSettings-compatible INI codec (src/launcher/core/iniFile.h, task 13.1):
// the 7 Qt fixtures in tests/fixtures/settings re-serialize byte for byte; every escape, "@@",
// @Invalid(), @String, @ByteArray, @Variant, one-element and empty lists, %XX keys, sections,
// sorting, arrays with size, unknown keys, UTF-8 output, and malformed input. The expected texts
// marked "QSettings wrote" are what QSettings::IniFormat in Qt 6.11.2 wrote for the same keys and
// values, or when it rewrote the same input after changing one other key.

#include "launcher/core/atomicFile.h"
#include "launcher/core/iniFile.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kyty::core::IniFile;
using kyty::core::IniValue;
using kyty::core::IniValueKind;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "IniFileTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// Bytes made printable for a message: line breaks kept, other control and non-ASCII bytes as <xx>.
std::string Show(std::string_view bytes) {
	static const char kHex[] = "0123456789abcdef";
	std::string       text;
	for (const char c: bytes) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte == '\n') {
			text += "\\n\n";
		} else if (byte < 0x20 || byte >= 0x7F) {
			text += '<';
			text += kHex[byte >> 4u];
			text += kHex[byte & 0xFu];
			text += '>';
		} else {
			text += c;
		}
	}
	return text;
}

void CheckText(const std::string& actual, std::string_view expected, const std::string& what) {
	Check(actual == expected,
	      what + ": got\n" + Show(actual) + "\n---- expected\n" + Show(expected) + "\n----");
}

void CheckString(const IniFile& file, std::string_view key, std::string_view expected) {
	const auto value = file.GetString(key);
	Check(value.has_value(), "no string at " + std::string(key));
	Check(*value == expected,
	      std::string(key) + " is '" + Show(*value) + "', expected '" + Show(expected) + "'");
}

void CheckList(const IniFile& file, std::string_view key,
               const std::vector<std::string>& expected) {
	const auto value = file.GetStringList(key);
	Check(value.has_value(), "no string list at " + std::string(key));
	std::string shown;
	for (const auto& element: *value) {
		shown += "[" + Show(element) + "]";
	}
	Check(*value == expected, std::string(key) + " is " + shown);
}

void CheckKind(const IniFile& file, std::string_view key, IniValueKind kind) {
	const IniValue* value = file.Find(key);
	Check(value != nullptr, "missing " + std::string(key));
	Check(value->kind == kind, "kind of " + std::string(key));
}

void CheckRaw(const IniFile& file, std::string_view key, std::string_view expected) {
	const auto raw = file.GetRaw(key);
	Check(raw.has_value(), "missing " + std::string(key));
	CheckText(*raw, expected, "raw value of " + std::string(key));
}

std::string ReadFixture(const std::string& name) {
	const auto path =
	    std::filesystem::path(KYTY_TEST_SOURCE_DIR) / "tests/fixtures/settings" / name;
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "cannot read " + path.string());
	return *bytes;
}

// ---- The Qt fixtures

void TestFixturesRoundTrip() {
	for (const char* name: {"qt6-empty-lists.ini", "qt6-game-entries.ini",
	                        "qt6-global-all-fields.ini", "qt6-legacy-game-dir.ini",
	                        "qt6-portable.ini", "qt6-special-values.ini", "qt6-user-store.ini"}) {
		const std::string bytes = ReadFixture(name);
		const auto        file  = IniFile::Parse(bytes);
		Check(!file.HasFormatError(), std::string(name) + ": format error");
		Check(file.Entries().size() > 2, std::string(name) + ": entries");
		CheckText(file.Serialize(), bytes, std::string(name) + " re-serialized");
		Check(IniFile::Parse(file.Serialize()) == file, std::string(name) + ": parse again");
	}
}

void TestFixtureValues() {
	const auto special = IniFile::Parse(ReadFixture("qt6-special-values.ini"));
	CheckString(special, "GameConfigurations/1/audio_input_device",
	            "Микрофон (USB), \"Front\"; L=R \\ #1");
	CheckString(special, "GameConfigurations/1/name", "@Leading at");
	CheckString(special, "GameConfigurations/1/user_name", "@Kyty, \"=;\\é");
	CheckString(special, "GameConfigurations/1/printf_output_file", "@kyty=log;1.txt");
	CheckString(special, "GameConfigurations/1/shader_log_folder", "logs\\shaders;v=1");
	CheckString(special, "GameConfigurations/2/audio_input_device", "@Mic");
	CheckString(special, "GameConfigurations/2/basedir",
	            "/games/PS5, extra/\"Q\" a=b;c\\d/PPSA09876");
	CheckString(special, "GameConfigurations/2/name", "Astro, \"Bot\"; Rescue=1 \\ ✓");
	CheckString(special, "GameConfigurations/3/name", "Line1\nLine2\tTabbed  Trailing space ");
	CheckString(special, "GameConfigurations/4/basedir", "/games/ゲーム/Ünïcødé/PPSA00002");
	CheckString(special, "GameConfigurations/4/user_name", " Kyty ");
	CheckString(special, "GlobalConfiguration/controller_color", "#ff8800");
	CheckString(special, "Launcher/addons_dir", "/games/add-ons, \"DLC\"; a=b\\c ü");
	CheckList(special, "Launcher/game_dirs",
	          {"/games/PS5, extra", "/games/\"quoted\" dir", "/games/a=b;c",
	           "/games/back\\slash\\dir", "/games/ゲーム/Ünïcødé", "/games/@at"});
	CheckList(special, "GameConfigurations/1/host_input_mapping",
	          {"Cross=J", "L3=Left Shift", "Square=,", "Circle=;", "Triangle==", "R3=\\", "L1='",
	           "TouchPad=\"", "@Share=ü"});
	// A one-element list is written like a string and reads back as one.
	CheckKind(special, "GameConfigurations/2/host_input_mapping", IniValueKind::String);
	CheckList(special, "GameConfigurations/2/host_input_mapping", {"Square=,"});
	CheckList(special, "GameConfigurations/4/host_input_mapping",
	          {" leading space", "trailing space ", "@", "@@double"});
	CheckKind(special, "GameConfigurations/4/host_input_mapping", IniValueKind::StringList);
	Check(special.ArraySize("GameConfigurations") == 4, "special-values: 4 game configurations");
	// The window geometries are kept verbatim.
	for (const char* key: {"MainDialog/geometry", "ConfigurationEditDialog/geometry"}) {
		CheckKind(special, key, IniValueKind::Opaque);
		Check(!special.GetString(key).has_value(), std::string(key) + " is not a string");
		Check(!special.GetStringList(key).has_value(), std::string(key) + " is not a list");
	}
	CheckRaw(special, "MainDialog/geometry",
	         "@ByteArray(\\x1\\xd9\\xd0\\xcb\\0\\x3\\0\\0\\0\\0\\0\\0\\0\\0\\0&"
	         "\\0\\0\\x4O\\0\\0\\x2\\xf9\\0"
	         "\\0\\0\\x2\\0\\0\\0(\\0\\0\\x4M\\0\\0\\x2\\xf7\\0\\0\\0\\0\\0\\0\\0\\0\\x3 "
	         "\\0\\0\\0\\x2\\0\\0\\0(\\0\\0\\x4M\\0\\0\\x2\\xf7)");

	const auto empty = IniFile::Parse(ReadFixture("qt6-empty-lists.ini"));
	for (const char* key: {"Launcher/game_dirs", "GlobalConfiguration/host_input_mapping",
	                       "GameConfigurations/1/host_input_mapping"}) {
		CheckKind(empty, key, IniValueKind::Invalid);
		CheckList(empty, key, {});
		CheckString(empty, key, "");
		CheckRaw(empty, key, "@Invalid()");
	}
	CheckString(empty, "GlobalConfiguration/audio_input_device", "");
	CheckList(empty, "GlobalConfiguration/audio_input_device", {""});
	CheckString(empty, "GameConfigurations/1/name", "");
	Check(empty.ArraySize("GameConfigurations") == 1, "empty-lists: 1 game configuration");

	const auto legacy = IniFile::Parse(ReadFixture("qt6-legacy-game-dir.ini"));
	CheckString(legacy, "Launcher/game_dir", "/home/user/Games/PS5, legacy");
	Check(legacy.Find("Launcher/game_dirs") == nullptr, "legacy: no game_dirs");
	CheckString(legacy, "GameConfigurations/1/game_path", "PPSA01234");

	const auto portable = IniFile::Parse(ReadFixture("qt6-portable.ini"));
	Check(portable.ArraySize("GameConfigurations") == 0, "portable: size=0");
	CheckList(portable, "Launcher/game_dirs", {"/home/user/Games/PS5"});

	const auto user = IniFile::Parse(ReadFixture("qt6-user-store.ini"));
	CheckList(user, "Launcher/game_dirs", {"/home/user/Games/PS5", "/mnt/storage/PS5 Games"});
	CheckString(user, "MainDialog/theme", "1");
	CheckString(user, "Launcher/grid_icon_width", "256");
}

// ---- Writing: the same keys and values that the QSettings probe set

constexpr std::string_view kQtWrite =
    "[General]\n"
    "root=1\n"
    "\n"
    "[%General]\n"
    "upper=1\n"
    "\n"
    "[%General]\n"
    "exact=1\n"
    "\n"
    "[a]\n"
    "b\\c=1\n"
    "x=1\n"
    "\n"
    "[a-b]\n"
    "y=1\n"
    "\n"
    "[%General]\n"
    "lower=1\n"
    "\n"
    "[k]\n"
    "UPPER=1\n"
    "a-b_c.d%7Ee=1\n"
    "back%5Cslash=1\n"
    "double\\slash=1\n"
    "eq%3Dsemi%3Bquote%22brk%5B%5D=1\n"
    "per%25cent=1\n"
    "sp%20ace=1\n"
    "%E9=1\n"
    "%U30B2=1\n"
    "%UD83D%UDE00=1\n"
    "\n"
    "[s]\n"
    "1-x=1\n"
    "1\\x=1\n"
    "10\\x=1\n"
    "2\\x=1\n"
    "B=1\n"
    "_=1\n"
    "a=1\n"
    "size=3\n"
    "%UD83D%UDE00=1\n"
    "%UFF00=1\n"
    "\n"
    "[sp%20grp]\n"
    "x=1\n"
    "\n"
    "[w]\n"
    "at=@@foo\n"
    "at_invalid_text=@@Invalid()\n"
    "at_list=@@, @@a, @@@b, c\n"
    "at_only=@@\n"
    "at_paren=@@Foo(x)\n"
    "backslash_end=a\\\\\n"
    "bool=true\n"
    "bytearray=\"@ByteArray("
    "\\0\\x1\\x2\\x3\\x4\\x5\\x6\\a\\b\\t\\n\\v\\f\\r\\xe\\xf\\x10\\x11\\x12\\x13"
    "\\x14\\x15\\x16\\x17\\x18\\x19\\x1a\\x1b\\x1c\\x1d\\x1e\\x1f "
    "!\\\"#$%&'()*+,-./"
    "0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
    "\\x7f\\x80\\x81\\x82\\x83\\x84\\x85\\x86\\x87\\x88\\x89\\x8a\\x8b\\x8c\\x8d\\x8e\\x8f\\x90\\x9"
    "1\\x92"
    "\\x93\\x94\\x95\\x96\\x97\\x98\\x99\\x9a\\x9b\\x9c\\x9d\\x9e\\x9f\\xa0\\xa1\\xa2\\xa3\\xa4\\xa"
    "5\\xa6"
    "\\xa7\\xa8\\xa9\\xaa\\xab\\xac\\xad\\xae\\xaf\\xb0\\xb1\\xb2\\xb3\\xb4\\xb5\\xb6\\xb7\\xb8\\xb"
    "9\\xba"
    "\\xbb\\xbc\\xbd\\xbe\\xbf\\xc0\\xc1\\xc2\\xc3\\xc4\\xc5\\xc6\\xc7\\xc8\\xc9\\xca\\xcb\\xcc\\xc"
    "d\\xce"
    "\\xcf\\xd0\\xd1\\xd2\\xd3\\xd4\\xd5\\xd6\\xd7\\xd8\\xd9\\xda\\xdb\\xdc\\xdd\\xde\\xdf\\xe0\\xe"
    "1\\xe2"
    "\\xe3\\xe4\\xe5\\xe6\\xe7\\xe8\\xe9\\xea\\xeb\\xec\\xed\\xee\\xef\\xf0\\xf1\\xf2\\xf3\\xf4\\xf"
    "5\\xf6"
    "\\xf7\\xf8\\xf9\\xfa\\xfb\\xfc\\xfd\\xfe\\xff,;=\\\" a)\"\n"
    "bytearray_comma=\"@ByteArray(a,b)\"\n"
    "bytearray_list=@ByteArray(x), y\n"
    "bytearray_plain=@ByteArray(abc)\n"
    "bytearray_space=@ByteArray( a )\n"
    "ctrl="
    "\\x1\\x2\\x3\\x4\\x5\\x6\\a\\b\\t\\n\\v\\f\\r\\xe\\xf\\x10\\x11\\x12\\x13\\x14\\x15\\x16\\x17"
    "\\x18\\x19\\x1a\\x1b\\x1c\\x1d\\x1e\\x1f\177\302\200\302\237\302\240\n"
    "ctrl_digit=\\x1\\x61\\x62\\x31\\x32g\n"
    "emoji=\360\237\230\200 \357\274\200\n"
    "hash=#x [y] z\n"
    "int=42\n"
    "invalid=@Invalid()\n"
    "l_empty=@Invalid()\n"
    "l_one=a\n"
    "l_one_empty=\n"
    "l_space_last=a, \n"
    "l_two_empty=, \n"
    "lead_tab=\\tx\n"
    "nul=@String(a\\0\\x62\\x31)\n"
    "nul_at=@String(@x\\0y)\n"
    "nul_list=a, @String(b\\0\\x63)\n"
    "nul_one_list=@String(b\\0\\x63)\n"
    "quote_lead=\\\"ab\n"
    "quote_only=a\\\"b\n"
    "s_empty=\n"
    "spaces=\"  \"\n"
    "trail_tab=x\\t\n"
    "\n"
    "[%E9grp]\n"
    "x=1\n";

IniFile QtWriteModel() {
	IniFile    file;
	const auto set = [&](std::string_view key, std::string_view value) {
		Check(file.SetString(key, value), "set " + std::string(key));
	};
	const auto set_list = [&](std::string_view key, std::vector<std::string> values) {
		Check(file.SetStringList(key, std::move(values)), "set list " + std::string(key));
	};
	set("w/s_empty", "");
	set_list("w/l_one_empty", {""});
	set_list("w/l_two_empty", {"", ""});
	set_list("w/l_one", {"a"});
	set_list("w/l_empty", {});
	set_list("w/l_space_last", {"a", ""});
	std::string control;
	for (char c = 1; c < 0x20; c++) {
		control += c;
	}
	control += "\x7F"
	           "\xC2\x80"
	           "\xC2\x9F"
	           "\xC2\xA0";
	set("w/ctrl", control);
	set("w/ctrl_digit", "\x01"
	                    "ab12g");
	set("w/nul", std::string("a\0b1", 4));
	set("w/nul_at", std::string("@x\0y", 4));
	set_list("w/nul_list", {"a", std::string("b\0c", 3)});
	set_list("w/nul_one_list", {std::string("b\0c", 3)});
	set("w/at", "@foo");
	set("w/at_only", "@");
	set("w/at_paren", "@Foo(x)");
	set("w/at_invalid_text", "@Invalid()");
	set_list("w/at_list", {"@", "@a", "@@b", "c"});
	set("w/lead_tab", "\tx");
	set("w/trail_tab", "x\t");
	set("w/spaces", "  ");
	set("w/quote_only", "a\"b");
	set("w/quote_lead", "\"ab");
	set("w/hash", "#x [y] z");
	set("w/emoji", "\xF0\x9F\x98\x80 \xEF\xBC\x80");
	set("w/backslash_end", "a\\");
	std::string bytes;
	for (int byte = 0; byte < 256; byte++) {
		bytes += static_cast<char>(byte);
	}
	bytes += ",;=\" a";
	Check(file.Set("w/bytearray", IniValue::FromByteArray(bytes)), "set w/bytearray");
	Check(file.Set("w/bytearray_plain", IniValue::FromByteArray("abc")), "set bytearray_plain");
	Check(file.Set("w/bytearray_comma", IniValue::FromByteArray("a,b")), "set bytearray_comma");
	Check(file.Set("w/bytearray_space", IniValue::FromByteArray(" a ")), "set bytearray_space");
	// QSettings wrote a QVariantList of a QByteArray and a QString.
	Check(file.SetRaw("w/bytearray_list", "@ByteArray(x), y"), "set bytearray_list");
	Check(file.Set("w/invalid", IniValue::Invalid()), "set w/invalid");
	set("w/int", "42");
	set("w/bool", "true");
	for (const char* key: {"k/sp ace",
	                       "k/per%cent",
	                       "k/back\\slash",
	                       "k/\xC3\xA9",
	                       "k/\xE3\x82\xB2",
	                       "k/\xF0\x9F\x98\x80",
	                       "k/eq=semi;quote\"brk[]",
	                       "k/a-b_c.d~e",
	                       "k/UPPER",
	                       "k//double//slash/",
	                       "general/lower",
	                       "GENERAL/upper",
	                       "General/exact",
	                       "root",
	                       "a/b/c",
	                       "a-b/y",
	                       "a/x",
	                       "\xC3\xA9grp/x",
	                       "sp grp/x",
	                       "s/1/x",
	                       "s/10/x",
	                       "s/2/x",
	                       "s/\xEF\xBC\x80",
	                       "s/\xF0\x9F\x98\x80",
	                       "s/B",
	                       "s/a",
	                       "s/_",
	                       "s/1-x"}) {
		set(key, "1");
	}
	set("s/size", "3");
	return file;
}

void TestWriteMatchesQt() {
	const IniFile model = QtWriteModel();
	CheckText(model.Serialize(), kQtWrite, "QSettings wrote");
	// "general" and "GENERAL" are written as [%General] as well and read back as "General" (Qt
	// loses their letter case); everything else reads back as set.
	const IniFile parsed = IniFile::Parse(kQtWrite);
	Check(!parsed.HasFormatError(), "QSettings output: format error");
	for (const auto& [key, value]: model.Entries()) {
		const bool general = key.starts_with("general/") || key.starts_with("GENERAL/");
		Check(general || (parsed.Find(key) != nullptr && *parsed.Find(key) == value),
		      "read back " + Show(key));
	}
	CheckString(parsed, "General/lower", "1");
	CheckString(parsed, "General/upper", "1");
	Check(parsed.Find("general/lower") == nullptr, "general/lower is General/lower");
	Check(parsed.Entries().size() == model.Entries().size(), "read back: entry count");
	// QSettings wrote this after reading kQtWrite and setting one other key.
	std::string            reread(kQtWrite);
	const std::string_view generals = "[%General]\nupper=1\n\n[%General]\nexact=1\n\n";
	reread.erase(reread.find(generals), generals.size());
	reread.erase(reread.find("\n[%General]\nlower=1\n"), 20);
	reread.insert(reread.find("[a]\n"), "[%General]\nexact=1\nlower=1\nupper=1\n\n");
	CheckText(parsed.Serialize(), reread, "QSettings rewrote");
}

// ---- Reading: QSettings rewrote each input after setting one other key (that key left out)

struct QtReadCase {
	const char*      name;
	std::string_view input;
	std::string_view rewritten;
	bool             format_error;
	// Whether the rewritten text reads back to itself. It does not when a section is named
	// "general" in another letter case: [%General] reads back as "General" and joins that section.
	bool stable;
};

void TestReadMatchesQt() {
	const QtReadCase cases[] = {
	    {"byte order mark, CR LF, CR, and a quoted line break",
	     "\357\273\277[S]\r\na=1\r\nb=\"x\r\ny\"\r\nc=2\rd=3\n\re=4",
	     "[S]\na=1\nb=x\\r\\ny\nc=2\nd=3\ne=4\n", false, true},
	    {"[a\\b] k= wins over [a] b\\k= (first)", "[a\\b]\nk=2\n[a]\nb\\k=3\n", "[a]\nb\\k=2\n",
	     true, true},
	    {"[a\\b] k= wins over [a] b\\k= (second)", "[a]\nb\\k=3\n[a\\b]\nk=2\n", "[a]\nb\\k=2\n",
	     true, true},
	    {"sections, keys, and malformed lines",
	     "[Sec]\n"
	     "dup=1\n"
	     "=emptykey\n"
	     "dup=2\n"
	     "[Sec]\n"
	     "dup=3\n"
	     "[Sec2\n"
	     "x=1\n"
	     "[%general]\n"
	     "y=1\n"
	     "[GENERAL]\n"
	     "z=1\n"
	     "[%GENERAL]\n"
	     "w=1\n"
	     "[sp%20ace]\n"
	     "k%20ey=1\n"
	     "k%U00E9=1\n"
	     "k%Ud83d%Ude00=1\n"
	     "k%zz=1\n"
	     "k%=1\n"
	     "k%4=1\n"
	     "k%e9=1\n"
	     "k%2F2=1\n"
	     "k%5C=1\n"
	     "k%U12=1\n"
	     "k%+1=1\n"
	     "k%0x=1\n"
	     "k% 1=1\n"
	     "\303\251raw=1\n"
	     "\377badkey=1\n"
	     "  spaced key  =1\n"
	     "[  trimmed  ]\n"
	     "t=1\n"
	     "[after]junk\n"
	     "aj=1\n"
	     "[]\n"
	     "emptysec=1\n"
	     "[ab\n"
	     "cd]\n"
	     "q=1\n"
	     "[last]\n"
	     "inv=\377\376|\343\201|\300\257|\355\240\200|ok\n"
	     "m6=\"unterminated, x\n"
	     "n=a,\n",
	     "[General]\n"
	     "emptysec=1\n"
	     "z=1\n"
	     "\n"
	     "[%General]\n"
	     "w=1\n"
	     "\n"
	     "[Sec]\n"
	     "=emptykey\n"
	     "dup=3\n"
	     "\n"
	     "[Sec2]\n"
	     "x=1\n"
	     "\n"
	     "[ab]\n"
	     "q=1\n"
	     "\n"
	     "[after]\n"
	     "aj=1\n"
	     "\n"
	     "[%General]\n"
	     "y=1\n"
	     "\n"
	     "[last]\n"
	     "inv=\357\277\275\357\277\275|\357\277\275\357\277\275|\357\277\275\357\277\275|"
	     "\357\277\275\357\277\275\357\277\275|ok\n"
	     "m6=\"unterminated, x\\nn=a,\\n\"\n"
	     "\n"
	     "[sp%20ace]\n"
	     "k%01=1\n"
	     "k%20ey=1\n"
	     "k%25=1\n"
	     "k%250x=1\n"
	     "k%254=1\n"
	     "k%25U12=1\n"
	     "k%25zz=1\n"
	     "k\\2=1\n"
	     "k%5C=1\n"
	     "k%E9=1\n"
	     "k%UD83D%UDE00=1\n"
	     "spaced%20key=1\n"
	     "%E9raw=1\n"
	     "%UFFFDbadkey=1\n"
	     "\n"
	     "[trimmed]\n"
	     "t=1\n",
	     true, false},
	    {"empty keys", "=root empty\n[a]\n=x\n", "[General]\n=root empty\n\n[a]\n=x\n", false,
	     true},
	    {"backslashes in keys", "[a]\n\\k=1\nk\\=2\nx\\\\y=3\n", "[a]\n\\k=1\nx\\\\y=3\n", true,
	     true},
	    {"key escapes and ill-formed UTF-8",
	     "[q]\n"
	     "k%-1=1\n"
	     "k%U0x41=1\n"
	     "k%U 41 =1\n"
	     "k%1 =1\n"
	     "k%0X=1\n"
	     "k%U+041=1\n"
	     "v1=abc\343\n"
	     "v2=a\343\\x41b\n"
	     "v3=a\343\201\"b\"\n"
	     "v4=\360\237\230|\n"
	     "v5=\364\220\200\200|\n"
	     "v6=\343\201, b\n"
	     "k;x=1\n"
	     "v7=1\n",
	     "[q]\n"
	     "k%25-1=1\n"
	     "k%250X=1\n"
	     "k%251=1\n"
	     "k%25U%2041=1\n"
	     "kA=1\n"
	     "v1=abc\n"
	     "v2=a\320\233\n"
	     "v3=a\357\277\275\n"
	     "v4=\357\277\275\357\277\275\357\277\275|\n"
	     "v5=\357\277\275\357\277\275\357\277\275\357\277\275|\n"
	     "v6=, \357\277\275\n"
	     "v7=1\n",
	     true, true},
	    {"non-characters, @String in a list, and line continuation",
	     "[n]\n"
	     "v1=a\357\277\276b\n"
	     "v2=\357\267\220|\357\277\277\n"
	     "v3=\364\217\277\277\n"
	     "v4=@String(x), b\n"
	     "v5=\\x\n"
	     "v6=a  \\\n"
	     "v7=\"a\"b  \n",
	     "[n]\n"
	     "v1=a\357\277\276b\n"
	     "v2=\357\267\220|\357\277\277\n"
	     "v3=\364\217\277\277\n"
	     "v4=x, b\n"
	     "v5=\n"
	     "v6=\"a  v7=ab  \"\n",
	     false, true},
	    {"U+FEFF first in a value",
	     "[b]\n"
	     "k=\357\273\277x\n"
	     "k2=\"\357\273\277y\"\n"
	     "k3=a, \357\273\277b\n"
	     "k4=\\t\357\273\277z\n"
	     "k5=q\357\273\277r\n"
	     "\357\273\277key=1\n",
	     "[b]\n"
	     "k=x\n"
	     "k2=y\n"
	     "k3=a, \357\273\277b\n"
	     "k4=\\tz\n"
	     "k5=q\357\273\277r\n"
	     "key=1\n",
	     false, true},
	};
	for (const auto& test: cases) {
		const auto file = IniFile::Parse(test.input);
		Check(file.HasFormatError() == test.format_error,
		      std::string(test.name) + ": format error");
		CheckText(file.Serialize(), test.rewritten, test.name);
		// The documents need not be equal ("[]" "x=1" gives the key "/x", which is written as "x"
		// in [General]), but the text should be stable.
		if (test.stable) {
			CheckText(IniFile::Parse(test.rewritten).Serialize(), test.rewritten,
			          std::string(test.name) + ", read again");
		}
	}
}

constexpr std::string_view kQtRead =
    "top=1\n"
    "; comment line\n"
    "  ; indented comment\n"
    "# hash line\n"
    "[General]\n"
    "g=2\n"
    "[Sec]\n"
    "a = spaced value   ; trailing comment\n"
    "b=\"quoted ; semi\" ; c\n"
    "c=\\x41\\x4a\\101\\q\\?\\'z\\x\n"
    "c2=\\x1F600|\\xd83d\\xde00|\\x41g|\\0|\\08|\\a\\b\\f\\n\\r\\t\\v\n"
    "d=x\\\n"
    "continued\n"
    "e=\"multi\n"
    "line\"\n"
    "noequals\n"
    "=emptykey\n"
    "  =spacekey\n"
    "f=@Invalid()\n"
    "f2=@Invalid(x)\n"
    "h=@String(abc)\n"
    "h2=@String(abc\n"
    "i=@Foo(bar)\n"
    "i2=@foo\n"
    "i3=\"@ByteArray(a,b)\"\n"
    "i4=@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\bNotAType\\0)\n"
    "j=@@x, @y, z\n"
    "j2=@@x, @@y\n"
    "k=a, @Invalid(), b\n"
    "k2=a, @ByteArray(q), b\n"
    "k3=a, @String(s), @@t\n"
    "l=@ByteArray(abc)\n"
    "l2=@ByteArray(a\\x80\\xff\\0b)\n"
    "m=   \n"
    "m2=\t x \t\n"
    "m3=  \"  q  \"  \n"
    "m4=\"a\"b\"c\"\n"
    "m5=a\"b,c\"d, e\n"
    "n=a,\n"
    "n2=,\n"
    "n3= , \n"
    "n4=a ,b , c\n"
    "o=%41\n";

void TestValuesAndDeviations() {
	const auto file = IniFile::Parse(kQtRead);
	Check(file.HasFormatError(), "noequals and '# hash line' are format errors");
	CheckString(file, "top", "1");
	CheckString(file, "g", "2");
	CheckString(file, "Sec/a", "spaced value");
	CheckString(file, "Sec/b", "quoted ; semi");
	CheckString(file, "Sec/c", "AJA?'z");
	CheckString(file, "Sec/c2",
	            std::string("\xEF\x98\x80|\xF0\x9F\x98\x80|Ag|") + '\0' + "|" + '\0' +
	                "8|\a\b\f\n\r\t\v");
	CheckString(file, "Sec/d", "xcontinued");
	CheckString(file, "Sec/e", "multi\nline");
	CheckKind(file, "Sec/f", IniValueKind::Invalid);
	CheckString(file, "Sec/h", "abc");
	CheckString(file, "Sec/h2", "@String(abc");
	CheckString(file, "Sec/i2", "@foo");
	CheckList(file, "Sec/j", {"@x", "@y", "z"});
	CheckList(file, "Sec/j2", {"@x", "@y"});
	CheckList(file, "Sec/k3", {"a", "s", "@t"});
	CheckString(file, "Sec/m", "");
	CheckString(file, "Sec/m2", "x");
	CheckString(file, "Sec/m3", "  q  ");
	CheckString(file, "Sec/m4", "abc");
	CheckList(file, "Sec/m5", {"ab,cd", "e"});
	CheckList(file, "Sec/n", {"a", ""});
	CheckList(file, "Sec/n2", {"", ""});
	CheckList(file, "Sec/n3", {"", ""});
	CheckList(file, "Sec/n4", {"a", "b", "c"});
	CheckString(file, "Sec/o", "%41");
	// The empty key "=..." is kept as "Sec/" (the later line wins) but no API key reaches it.
	Check(file.Entries().count("Sec/") == 1 && file.Entries().at("Sec/").text == "spacekey",
	      "empty key Sec/");
	Check(file.Find("Sec/") == nullptr && file.Find("Sec") == nullptr, "Sec/ is not an API key");
	// Opaque values keep their text; QSettings itself writes these four differently.
	CheckRaw(file, "Sec/f2", "@Invalid(x)");                                     // Qt: @@Invalid(x)
	CheckRaw(file, "Sec/i", "@Foo(bar)");                                        // Qt: @@Foo(bar)
	CheckRaw(file, "Sec/i4", "@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\bNotAType\\0)"); // Qt: @Invalid()
	CheckRaw(file, "Sec/l2", "@ByteArray(a\\x80\\xff\\0b)"); // Qt: @ByteArray(a\x80\xff\0\x62)
	CheckRaw(file, "Sec/i3", "\"@ByteArray(a,b)\"");
	CheckRaw(file, "Sec/k", "a, @Invalid(), b");
	CheckRaw(file, "Sec/k2", "a, @ByteArray(q), b");
	for (const char* key:
	     {"Sec/f2", "Sec/i", "Sec/i3", "Sec/i4", "Sec/k", "Sec/k2", "Sec/l", "Sec/l2"}) {
		CheckKind(file, key, IniValueKind::Opaque);
		Check(!file.GetString(key).has_value() && !file.GetStringList(key).has_value(),
		      std::string(key) + " has no string value");
	}
	CheckText(file.Serialize(),
	          "[General]\n"
	          "g=2\n"
	          "top=1\n"
	          "\n"
	          "[Sec]\n"
	          "=spacekey\n"
	          "a=spaced value\n"
	          "b=\"quoted ; semi\"\n"
	          "c=AJA?'z\n"
	          "c2=@String(\357\230\200|\360\237\230\200|Ag|\\0|\\0\\x38|\\a\\b\\f\\n\\r\\t\\v)\n"
	          "d=xcontinued\n"
	          "e=multi\\nline\n"
	          "f=@Invalid()\n"
	          "f2=@Invalid(x)\n"
	          "h=abc\n"
	          "h2=@@String(abc\n"
	          "i=@Foo(bar)\n"
	          "i2=@@foo\n"
	          "i3=\"@ByteArray(a,b)\"\n"
	          "i4=@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\bNotAType\\0)\n"
	          "j=@@x, @@y, z\n"
	          "j2=@@x, @@y\n"
	          "k=a, @Invalid(), b\n"
	          "k2=a, @ByteArray(q), b\n"
	          "k3=a, s, @@t\n"
	          "l=@ByteArray(abc)\n"
	          "l2=@ByteArray(a\\x80\\xff\\0b)\n"
	          "m=\n"
	          "m2=x\n"
	          "m3=\"  q  \"\n"
	          "m4=abc\n"
	          "m5=\"ab,cd\", e\n"
	          "n=a, \n"
	          "n2=, \n"
	          "n3=, \n"
	          "n4=a, b, c\n"
	          "o=%41\n",
	          "read and rewritten");
}

// ---- Lists, opaque values, and the setters

void TestLists() {
	IniFile file;
	Check(file.SetStringList("l/empty", {}), "set empty list");
	CheckKind(file, "l/empty", IniValueKind::Invalid);
	CheckRaw(file, "l/empty", "@Invalid()");
	CheckList(file, "l/empty", {});
	Check(file.SetStringList("l/one", {"a, b"}), "set one-element list");
	CheckKind(file, "l/one", IniValueKind::String);
	CheckRaw(file, "l/one", "\"a, b\"");
	CheckList(file, "l/one", {"a, b"});
	CheckString(file, "l/one", "a, b");
	Check(file.SetStringList("l/one_empty", {""}), "set [\"\"]");
	CheckRaw(file, "l/one_empty", "");
	CheckList(file, "l/one_empty", {""});
	Check(file.SetStringList("l/two", {" x", "@y"}), "set two-element list");
	CheckKind(file, "l/two", IniValueKind::StringList);
	CheckRaw(file, "l/two", "\" x\", @@y");
	Check(!file.GetString("l/two").has_value(), "a list is not a string");
	Check(IniValue::FromStringList({"a", "b"}).list.size() == 2, "FromStringList keeps two");
	const auto reread = IniFile::Parse(file.Serialize());
	Check(reread == file, "lists read back");
	CheckList(reread, "l/two", {" x", "@y"});
}

void TestOpaqueAndRaw() {
	IniFile file;
	Check(file.SetRaw("o/variant", "@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\bNotAType\\0)"),
	      "set variant");
	CheckKind(file, "o/variant", IniValueKind::Opaque);
	Check(file.SetRaw("o/spaced", " \t@ByteArray(x)  "), "set spaced raw");
	CheckRaw(file, "o/spaced", "@ByteArray(x)");
	// Trailing spaces stay in an opaque text when they change what it reads as: after a quote they
	// are part of the element ("a"b  reads as "ab  ").
	Check(file.SetRaw("o/quoted", "@ByteArray(x), \"a\"b  "), "set opaque list with a quote");
	CheckRaw(file, "o/quoted", "@ByteArray(x), \"a\"b  ");
	Check(file.SetRaw("o/unquoted", "@ByteArray(x), b  "), "set opaque list");
	CheckRaw(file, "o/unquoted", "@ByteArray(x), b");
	// SetRaw takes any complete value: a plain raw text is stored as what it reads as.
	Check(file.SetRaw("o/plain", "\"a,b\""), "set plain raw");
	CheckKind(file, "o/plain", IniValueKind::String);
	CheckString(file, "o/plain", "a,b");
	Check(file.SetRaw("o/list", "a, b"), "set raw list");
	CheckList(file, "o/list", {"a", "b"});
	// Not one complete value.
	for (const char* raw:
	     {"x;y", "\"open", "ends with a backslash\\", "a\nb=c", "a\rb", "@ByteArray(a;b)"}) {
		Check(!file.SetRaw("o/bad", raw), "SetRaw rejects '" + Show(raw) + "'");
		Check(!IniValue::FromRaw(raw).has_value(), "FromRaw rejects '" + Show(raw) + "'");
	}
	Check(file.Find("o/bad") == nullptr, "nothing stored for rejected raw text");
	// An Opaque value must be what its text reads as.
	IniValue fake;
	fake.kind = IniValueKind::Opaque;
	fake.text = "plain";
	Check(!file.Set("o/fake", fake), "an Opaque that reads as a string is rejected");
	fake.text = " @ByteArray(x)";
	Check(!file.Set("o/fake", fake), "an Opaque with a leading space is rejected");
	fake.text = "@ByteArray(x)";
	Check(file.Set("o/fake", fake), "an Opaque that reads as itself is stored");
	const auto value = IniValue::FromRaw("\"@ByteArray(a,b)\"");
	Check(value.has_value() && value->kind == IniValueKind::Opaque &&
	          value->text == "\"@ByteArray(a,b)\"",
	      "quoted @ByteArray");
	CheckText(IniValue::FromByteArray(std::string("a\0\x01"
	                                              "1b",
	                                              5))
	              .text,
	          "@ByteArray(a\\0\\x1\\x31\\x62)",
	          "FromByteArray escapes a hex digit after an escape");
	const auto reread = IniFile::Parse(file.Serialize());
	Check(!reread.HasFormatError(), "opaque values: format error");
	Check(reread == file, "opaque values read back");
}

void TestSetters() {
	IniFile file;
	Check(!file.SetString("", "x"), "empty key");
	Check(!file.SetString("///", "x"), "key of slashes");
	Check(file.Find("") == nullptr, "find empty key");
	Check(file.SetString("//a//b/", "1"), "key with extra slashes");
	Check(file.Entries().count("a/b") == 1 && file.Entries().size() == 1, "key normalized to a/b");
	CheckString(file, "a/b", "1");
	CheckString(file, "/a/b//", "1");
	Check(file.SetString("a/b", "2"), "replace");
	CheckString(file, "a/b", "2");
	// Invalid UTF-8 from the API is replaced (U+FFFD for each maximal invalid subpart).
	Check(file.SetString("u/\xFF", "x\xE3\x81y"), "set invalid UTF-8");
	CheckString(file, "u/\xEF\xBF\xBD", "x\xEF\xBF\xBDy");
	CheckRaw(file, "u/\xFF", "x\xEF\xBF\xBDy");
	Check(!file.GetString("missing").has_value() && !file.GetStringList("missing").has_value() &&
	          !file.GetRaw("missing").has_value(),
	      "missing key");
	CheckText(file.Serialize(), "[a]\nb=2\n\n[u]\n%UFFFD=x\357\277\275y\n", "setters serialized");
}

void TestArraysAndRemove() {
	auto file = IniFile::Parse(ReadFixture("qt6-game-entries.ini"));
	Check(file.ArraySize("GameConfigurations") == 4, "game-entries: 4 entries");
	Check(file.ArraySize("/GameConfigurations/") == 4, "array key normalized");
	CheckText(IniFile::ArrayKey("GameConfigurations", 0, "name"), "GameConfigurations/1/name",
	          "ArrayKey");
	Check(file.GetString(IniFile::ArrayKey("GameConfigurations", 3, "game_path")).has_value(),
	      "element 4 has a game_path");
	file.Remove("GameConfigurations");
	Check(file.ArraySize("GameConfigurations") == 0, "removed array");
	for (const auto& [key, value]: file.Entries()) {
		Check(!key.starts_with("GameConfigurations"), "left over " + key);
	}
	Check(file.Find("GlobalConfiguration/user_name") != nullptr, "other groups stay");
	Check(file.SetArraySize("A", 2), "SetArraySize");
	Check(file.SetString(IniFile::ArrayKey("A", 0, "name"), "first"), "element 1");
	Check(file.SetString(IniFile::ArrayKey("A", 1, "name"), "second"), "element 2");
	Check(file.ArraySize("A") == 2, "array A");
	CheckString(file, "A/2/name", "second");
	CheckRaw(file, "A/size", "2");
	for (const auto& [text, size]:
	     std::initializer_list<std::pair<const char*, int>> {{"3", 3},
	                                                         {" 3 ", 3},
	                                                         {"+3", 3},
	                                                         {"-1", 0},
	                                                         {"x", 0},
	                                                         {"3x", 0},
	                                                         {"", 0},
	                                                         {"99999999999", 0}}) {
		Check(file.SetString("B/size", text), "set B/size");
		Check(file.ArraySize("B") == size, std::string("ArraySize of '") + text + "'");
	}
	Check(file.SetStringList("B/size", {"1", "2"}), "set B/size list");
	Check(file.ArraySize("B") == 0, "ArraySize of a list");
	Check(file.ArraySize("missing") == 0, "missing array");
	// Remove takes the key and every key below it, and nothing that only shares a prefix.
	Check(file.SetString("A2/x", "1"), "set A2/x");
	file.Remove("A");
	Check(file.Find("A/size") == nullptr && file.Find("A/1/name") == nullptr, "A removed");
	CheckString(file, "A2/x", "1");
	file.Remove("A2/x");
	Check(file.Find("A2/x") == nullptr, "single key removed");
	file.Remove("");
	Check(file.Entries().empty(), "empty key removes everything");
	CheckText(file.Serialize(), "", "empty file");
}

void TestUnknownKeysKept() {
	const std::string bytes = ReadFixture("qt6-special-values.ini");
	auto              file  = IniFile::Parse(bytes);
	Check(file.SetString("Launcher/grid_view", "true"), "change grid_view");
	std::string expected = bytes;
	const auto  at       = expected.find("grid_view=false");
	Check(at != std::string::npos, "fixture has grid_view=false");
	expected.replace(at, 15, "grid_view=true");
	CheckText(file.Serialize(), expected, "one key changed, every other key kept");
	// A key nobody knows, in a new group, lands in sorted position and survives a rewrite.
	Check(file.SetString("Unknown Group/new key", "v"), "add unknown");
	const auto reread = IniFile::Parse(file.Serialize());
	Check(reread == file, "unknown keys read back");
	CheckString(reread, "Unknown Group/new key", "v");
	Check(file.Serialize().find("\n[MainDialog]\n") <
	          file.Serialize().find("\n[Unknown%20Group]\n"),
	      "unknown group sorted after MainDialog");
}

void TestUtf8Output() {
	const auto file = IniFile::Parse("\xEF\xBB\xBF[G]\r\nname=ゲーム\r\nlist=Ü, ✓\r\n");
	const auto out  = file.Serialize();
	CheckText(out, "[G]\nlist=Ü, ✓\nname=ゲーム\n", "UTF-8, no byte order mark, LF");
	Check(out.find('\r') == std::string::npos, "no CR");
	IniFile written;
	Check(written.SetString("G/ゲーム", "Ünïcødé\r\n"), "set non-ASCII");
	CheckText(written.Serialize(), "[G]\n%U30B2%U30FC%U30E0=Ünïcødé\\r\\n\n",
	          "non-ASCII key and value");
}

void TestFormatErrors() {
	Check(!IniFile::Parse("").HasFormatError() && IniFile::Parse("").Entries().empty(),
	      "empty input");
	Check(!IniFile::Parse("; only a comment\n\n").HasFormatError(), "comment only");
	Check(!IniFile::Parse("[a]\n  ; c\nk=1 ; c\n\n").HasFormatError(), "comments");
	Check(IniFile::Parse("[a]\nnoequals\n").HasFormatError(), "line without '='");
	Check(IniFile::Parse("[a\nk=1\n").HasFormatError(), "header without ']'");
	Check(IniFile::Parse("[a/b]\nk=1\n").HasFormatError(), "section name with '/'");
	Check(IniFile::Parse("[a%2Fb]\nk=1\n").HasFormatError(), "section name with %2F");
	Check(!IniFile::Parse("[a%5Cb]\nk=1\n").HasFormatError(), "section name with %5C");
	CheckString(IniFile::Parse("[a%5Cb]\nk=1\n"), "a\\b/k", "1");
	CheckString(IniFile::Parse("[a/b]\nk=1\n"), "a/b/k", "1");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("IniFileTests");
	TestFixturesRoundTrip();
	TestFixtureValues();
	TestWriteMatchesQt();
	TestReadMatchesQt();
	TestValuesAndDeviations();
	TestLists();
	TestOpaqueAndRaw();
	TestSetters();
	TestArraysAndRemove();
	TestUnknownKeysKept();
	TestUtf8Output();
	TestFormatErrors();
	std::printf("IniFileTests: all tests passed\n");
	return 0;
}
