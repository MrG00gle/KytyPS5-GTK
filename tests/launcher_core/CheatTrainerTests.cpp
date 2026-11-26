// Example tests of the MC4 and SHN conversion (src/launcher/core/cheatTrainer.h; upstream KytyPS5
// commit 91c5820c): AES-256 against the FIPS 197 C.3 and NIST SP 800-38A F.2.6 vectors (both
// checked with OpenSSL), lenient base64 as Qt's QByteArray::fromBase64, the fixture trainers of
// tests/fixtures/cheats (the SHN file written for these tests, the MC4 file made from a trainer
// with "openssl enc -aes-256-cbc -a" under the format's key and IV), the mapping of every trainer
// field to cheat JSON, the XML features a trainer may use, and malformed inputs.

#include "launcher/core/atomicFile.h"
#include "launcher/core/cheatTrainer.h"
#include "launcher/core/cheats.h"
#include "support/noDisplay.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kyty::core::CheatProblemKind;
using kyty::core::ConvertCheatTrainer;
using kyty::core::ParseCheatTrainer;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CheatTrainerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::vector<uint8_t> Bytes(std::string_view hex) {
	std::vector<uint8_t> bytes;
	for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
		bytes.push_back(
		    static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
	}
	return bytes;
}

template <std::size_t N>
std::array<uint8_t, N> Array(std::string_view hex) {
	const auto             bytes = Bytes(hex);
	std::array<uint8_t, N> array {};
	Check(bytes.size() == N, "a key or IV of " + std::to_string(N) + " bytes");
	std::copy(bytes.begin(), bytes.end(), array.begin());
	return array;
}

std::string Fixture(std::string_view name) {
	const auto path =
	    std::filesystem::path(KYTY_TEST_SOURCE_DIR) / "tests/fixtures/cheats" / std::string(name);
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "read " + path.string());
	return *bytes;
}

std::string CheatNames(const kyty::core::CheatDocument& document) {
	std::string names;
	for (const auto& cheat: document.cheats) {
		names += (names.empty() ? "" : ", ") + cheat.name;
	}
	return names;
}

void TestAes() {
	// FIPS 197 C.3 (one block; CBC with a zero IV is the block cipher itself).
	auto block = Bytes("8ea2b7ca516745bfeafc49904b496089");
	kyty::core::DecryptAes256Cbc(
	    block, Array<32>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"), {});
	Check(block == Bytes("00112233445566778899aabbccddeeff"), "FIPS 197 C.3");

	// SP 800-38A F.2.6, with five more bytes after the last whole block.
	auto data = Bytes("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
	                  "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"
	                  "0102030405");
	kyty::core::DecryptAes256Cbc(
	    data, Array<32>("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
	    Array<16>("000102030405060708090a0b0c0d0e0f"));
	Check(data == Bytes("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
	                    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710"
	                    "0102030405"),
	      "SP 800-38A F.2.6, the partial block unchanged");

	std::vector<uint8_t> empty;
	kyty::core::DecryptAes256Cbc(empty, {}, {});
	auto short_data = Bytes("000102");
	kyty::core::DecryptAes256Cbc(short_data, {}, {});
	Check(empty.empty() && short_data == Bytes("000102"), "less than a block is left as it is");
}

void TestBase64() {
	using kyty::core::DecodeBase64Lenient;
	for (const auto& [text, bytes]: std::vector<std::pair<std::string, std::string>> {
	         {"", ""},
	         {"TWFu", "Man"},
	         {"TWE=", "Ma"},
	         {"TQ==", "M"},
	         {"TWE", "Ma"}, // the bits that do not fill a byte are dropped
	         {"T", ""},
	         {"T W\r\nF\tu", "Man"},
	         {"TW!F*u", "Man"},  // other characters are skipped
	         {"T=W=F=u", "Man"}, // '=' too
	         {"-_", ""},         // the URL alphabet is not base64's
	         {"+/+/", "\xFB\xFF\xBF"},
	         {"AAEC/w==", std::string("\x00\x01\x02\xFF", 4)},
	     }) {
		Check(DecodeBase64Lenient(text) == bytes, "base64 '" + text + "'");
	}
}

std::string Trainer(const std::string& cheats,
                    const std::string& attributes = R"(Game="G" Moder="M" Cusa="PPSA01234" )"
                                                    R"(Version="01.000" Process="eboot.bin")") {
	return "<Trainer " + attributes + ">" + cheats + "</Trainer>";
}

const char kLine[] = "<Cheatline><Offset>0x10</Offset><Section>0</Section><ValueOn>90</ValueOn>"
                     "<ValueOff>C3</ValueOff></Cheatline>";

void TestFixtures() {
	const auto shn = ParseCheatTrainer(Fixture("PPSA01001_01.000_alpha.shn"), false, "alpha.shn");
	Check(shn.has_value(), "the SHN fixture converts");
	Check(shn->id == "PPSA01001" && shn->name == "Alpha Game" && shn->version == "01.000" &&
	          shn->process == "eboot.bin" &&
	          shn->credits == std::vector<std::string> {"Kyty Tests"},
	      "the trainer's attributes");
	Check(CheatNames(*shn) == "Infinite Health, Max Coins & Gems",
	      "the cheats: " + CheatNames(*shn));
	Check(shn->cheats[0].memory == R"([{"offset":"0x1A2B3C","on":"909090","off":"0F85C3"}])" &&
	          shn->cheats[1].memory == R"([{"offset":"0x4D5E","on":"FFFF","off":"0000"},)"
	                                   R"({"offset":"4D62","on":"E703","off":"0000"}])",
	      "the writes: " + shn->cheats[0].memory + " " + shn->cheats[1].memory);
	Check(shn->cheats[0].enabled && shn->cheats[1].enabled, "no enabled member");
	Check(!kyty::core::ValidateRemoteImport(*shn, "PPSA01001", "01.000", "eboot.bin"),
	      "the SHN fixture can be imported for Alpha Game");

	const auto mc4_text = Fixture("PPSA01001_01.000_alpha.mc4");
	const auto mc4      = ParseCheatTrainer(mc4_text, true, "alpha.mc4");
	Check(mc4.has_value(), "the MC4 fixture decrypts and converts");
	Check(mc4->id == "PPSA01001" && mc4->version == "01.000" && mc4->process == "eboot.bin",
	      "the MC4 trainer's attributes");
	Check(CheatNames(*mc4) == "Moon Jump, Unlimited Time, Freeze Enemies",
	      "the MC4 cheats: " + CheatNames(*mc4));
	Check(mc4->cheats[0].memory == R"([{"offset":"0x2F00","on":"0000803F","off":"00000040"}])" &&
	          mc4->cheats[2].memory == R"([{"offset":"0x51C4","on":"C3","off":"55"}])",
	      "the MC4 writes");
	Check(!kyty::core::ValidateRemoteImport(*mc4, "PPSA01001", "01.000", "eboot.bin"),
	      "the MC4 fixture can be imported for Alpha Game");
	std::string one_line;
	for (const char c: mc4_text) {
		if (c != '\n') {
			one_line += c;
		}
	}
	Check(ConvertCheatTrainer(one_line, true) == ConvertCheatTrainer(mc4_text, true),
	      "line breaks in the base64 text do not matter");

	// The padding after the root's end tag is not read; a cut file is not a trainer.
	Check(!ConvertCheatTrainer(mc4_text.substr(0, mc4_text.size() / 2), true), "half an MC4 file");
	Check(!ConvertCheatTrainer(Fixture("PPSA01001_01.000_alpha.shn"), true),
	      "an SHN file read as MC4");
	Check(!ConvertCheatTrainer(mc4_text, false), "an MC4 file read as SHN");
	Check(!ConvertCheatTrainer("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", true),
	      "base64 of something else");
}

void TestMapping() {
	Check(
	    ConvertCheatTrainer(Trainer(std::string(R"(<Cheat Text="A">)") + kLine + "</Cheat>"),
	                        false) ==
	        std::string(R"({"name":"G","id":"PPSA01234","version":"01.000","process":"eboot.bin",)"
	                    R"("credits":["M"],"mods":[{"name":"A","memory":[{"offset":"0x10",)"
	                    R"("on":"90","off":"C3"}]}]})"),
	    "the converted document, member by member");
	Check(ConvertCheatTrainer("<Trainer/>", false) ==
	          std::string(R"({"name":"","id":"","version":"","process":"","credits":[""],)"
	                      R"("mods":[]})"),
	      "missing attributes are empty");

	const auto convert = [](const std::string& cheats) {
		const auto document = ParseCheatTrainer(Trainer(cheats), false, "t.shn");
		Check(document.has_value(), "converts: " + cheats);
		return *document;
	};
	auto document = convert(
	    R"(<Cheat Text="A"><Cheatline><Offset> 0x10 </Offset><Section>1</Section><ValueOn>9-0</ValueOn>)"
	    R"(<ValueOff>C-3</ValueOff><Description>x</Description></Cheatline></Cheat>)"
	    R"(<Cheat Text="B"><Line><Offset>0-1</Offset><Absolute>false</Absolute><ValueOn>90</ValueOn>)"
	    R"(<ValueOff>C3</ValueOff><Offset>0x20</Offset></Line></Cheat>)"
	    R"(<Info><Cheat Text="nested, skipped"/></Info><Cheat Text="C"/>)");
	Check(CheatNames(document) == "A, B, C",
	      "only the root's Cheat children: " + CheatNames(document));
	Check(document.cheats[0].memory == R"([{"offset":"0x10","section":"1","on":"90","off":"C3"}])",
	      "trimmed values, a section other than 0, dashes only out of the values, unknown fields "
	      "ignored: " +
	          document.cheats[0].memory);
	Check(document.cheats[1].memory ==
	          R"([{"offset":"0x20","absolute":true,"on":"90","off":"C3"}])",
	      "any element in a Cheat is a write, Absolute is true whatever it says, a repeated field "
	      "keeps the last value: " +
	          document.cheats[1].memory);
	Check(document.cheats[2].memory == "[]", "a Cheat without writes");
	const auto addressing = [&](const std::string& line) {
		const auto problem = kyty::core::ValidateRemoteImport(
		    convert(R"(<Cheat Text="A">)" + line + "</Cheat>"), "PPSA01234", "01.000", "eboot.bin");
		return problem && problem->kind == CheatProblemKind::AddressingFeatures &&
		       problem->message ==
		           "This cheat file uses memory addressing features that are not supported yet.";
	};
	Check(addressing("<L><Offset>1</Offset><Section>2</Section><ValueOn>90</ValueOn>"
	                 "<ValueOff>C3</ValueOff></L>"),
	      "a section-relative write is refused on import");
	Check(addressing("<L><Offset>1</Offset><Absolute/><ValueOn>90</ValueOn><ValueOff>C3</ValueOff>"
	                 "</L>"),
	      "an absolute write is refused on import");

	// XML features: a byte order mark, the declaration, a document type, comments, processing
	// instructions, entities, character references, CDATA, single quotes, line breaks, and what
	// follows the root.
	document = *ParseCheatTrainer(
	    "\xEF\xBB\xBF<?xml version=\"1.0\"?>\r\n<!DOCTYPE Trainer [<!ELEMENT Trainer ANY>]>"
	    "<!-- c --><?pi x?><Trainer Cusa='PPSA01234' Game=\"A &amp; B\tC\r\nD\">"
	    "<Cheat Text=\"&lt;&#65;&#x263A;&quot;&apos;&gt;\"><!-- c --><Cheatline><?pi?>"
	    "<Offset><![CDATA[0x]]>&#49;0</Offset><ValueOn>\n90\r\n</ValueOn><ValueOff>c3</ValueOff>"
	    "</Cheatline></Cheat ></Trainer >garbage <not closed",
	    false, "t.shn");
	Check(document.id == "PPSA01234" && document.name == "A & B C D",
	      "attribute values: '" + document.name + "'");
	Check(document.cheats.size() == 1 && document.cheats[0].name == "<A\xE2\x98\xBA\"'>",
	      "entities and character references: " + CheatNames(document));
	Check(document.cheats[0].memory == R"([{"offset":"0x10","on":"90","off":"c3"}])",
	      "CDATA and references in values: " + document.cheats[0].memory);
	document = *ParseCheatTrainer(Trainer("<Cheat Text=\"\xC3\xA9\xFF\"/>"), false, "t.shn");
	Check(document.cheats[0].name == "\xC3\xA9\xEF\xBF\xBD", "ill-formed UTF-8 becomes U+FFFD");

	// A cheat without a name is not a valid cheat document.
	const auto unnamed =
	    ParseCheatTrainer(Trainer(std::string("<Cheat>") + kLine + "</Cheat>"), false, "t.shn");
	Check(!unnamed && unnamed.error().kind == CheatProblemKind::EntryWithoutName &&
	          unnamed.error().message == "t.shn: Invalid cheat entry 1 (it has no name).",
	      "a Cheat without Text");
}

void TestMalformed() {
	std::string deep;
	for (int i = 0; i < 300; i++) {
		deep += "<a>";
	}
	for (int i = 0; i < 300; i++) {
		deep += "</a>";
	}
	for (const std::string& text: std::vector<std::string> {
	         "",
	         "   ",
	         "not XML",
	         "text<Trainer/>",
	         "<Other/>",
	         "<trainer/>",
	         "<Trainer>",
	         "<Trainer",
	         "<Trainer a",
	         "<Trainer a=\"1\"",
	         "<Trainer a=\"x/>",
	         "<Trainer a=1/>",
	         "<Trainer a=\"1\"b=\"2\"/>",
	         "<Trainer a=\"1\" a=\"2\"/>",
	         "<Trainer a=\"<\"/>",
	         "<Trainer a=\"&bogus;\"/>",
	         "<1Trainer/>",
	         "<Trainer></Trainers>",
	         "<Trainer></>",
	         "<Trainer><Cheat></Trainer>",
	         "<Trainer>&bogus;</Trainer>",
	         "<Trainer>& </Trainer>",
	         "<Trainer>&#0;</Trainer>",
	         "<Trainer>&#xD800;</Trainer>",
	         "<Trainer>&#x110000;</Trainer>",
	         "<Trainer>&#x;</Trainer>",
	         "<Trainer>&#1a;</Trainer>",
	         "<Trainer>&#x123456789;</Trainer>",
	         "<Trainer>&averyveryverylongname;</Trainer>",
	         "<Trainer><![CDATA[x</Trainer>",
	         "<Trainer><!-- x</Trainer>",
	         "<Trainer><?pi</Trainer>",
	         "<Trainer><!x></Trainer>",
	         "<!-- unclosed",
	         "<?xml version=\"1.0\"",
	         "<!DOCTYPE Trainer",
	         "<!DOCTYPE a><!DOCTYPE b><Trainer/>",
	         "<Trainer><Cheat Text=\"a\"><Line><Offset><b>1</b></Offset></Line></Cheat></Trainer>",
	         "<Trainer>" + deep + "</Trainer>",
	     }) {
		Check(!ConvertCheatTrainer(text, false), "not a trainer: " + text.substr(0, 80));
		const auto problem = ParseCheatTrainer(text, false, "PPSA01234_01.000_x.shn");
		Check(!problem && problem.error().kind == CheatProblemKind::InvalidXml &&
		          problem.error().file == "PPSA01234_01.000_x.shn" &&
		          problem.error().message == "PPSA01234_01.000_x.shn: Invalid cheat XML.",
		      "InvalidXml names the file: " + text.substr(0, 80));
	}
	// A deep element that is not a write's value is fine below the depth limit.
	std::string nested = "<Trainer><Info>";
	for (int i = 0; i < 200; i++) {
		nested += "<a>";
	}
	for (int i = 0; i < 200; i++) {
		nested += "</a>";
	}
	Check(ConvertCheatTrainer(nested + "</Info></Trainer>", false).has_value(),
	      "200 nested elements");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CheatTrainerTests");
	TestAes();
	TestBase64();
	TestFixtures();
	TestMapping();
	TestMalformed();
	std::printf("CheatTrainerTests: all tests passed\n");
	return 0;
}
