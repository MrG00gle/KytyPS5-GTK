// Example tests of the Launcher_Core version helpers (src/launcher/core/version.h, task 10.4):
// numeric version comparison and requiredSystemSoftwareVersion formatting.

#include "launcher/core/version.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kyty::core::CompareVersions;
using kyty::core::FormatSystemSoftwareVersion;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "VersionTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckCompare(std::string_view a, std::string_view b, int expected) {
	const int result = CompareVersions(a, b);
	Check(result == expected, "CompareVersions(\"" + std::string(a) + "\", \"" + std::string(b) +
	                              "\") is " + std::to_string(result) + ", expected " +
	                              std::to_string(expected));
	const int reverse = CompareVersions(b, a);
	Check(reverse == -expected, "CompareVersions(\"" + std::string(b) + "\", \"" + std::string(a) +
	                                "\") is " + std::to_string(reverse) + ", expected " +
	                                std::to_string(-expected));
}

void TestNumericComponents() {
	CheckCompare("1.2", "1.10", -1);
	CheckCompare("2", "10", -1);
	CheckCompare("1.2.3", "1.2.3", 0);
	CheckCompare("1.2.3", "1.2.4", -1);
	CheckCompare("1.3", "1.2.9", 1);
	CheckCompare("01.000", "01.001", -1);
	CheckCompare("01.010", "01.009", 1);
	CheckCompare("0", "0", 0);
}

void TestLeadingZeros() {
	CheckCompare("01.002", "1.2", 0);
	CheckCompare("1.010", "1.10", 0);
	CheckCompare("000", "0", 0);
	CheckCompare("0001.0", "1", 0);
	CheckCompare("1.01", "1.1", 0);
	CheckCompare("1.09", "1.1", 1);
}

void TestMissingComponents() {
	CheckCompare("1", "1.0", 0);
	CheckCompare("1", "1.0.0.0", 0);
	CheckCompare("1", "1.0.1", -1);
	CheckCompare("1.1", "1", 1);
	CheckCompare("1.", "1", 0);
	CheckCompare("1..2", "1.0.2", 0);
	CheckCompare("", "0", 0);
	CheckCompare("", "", 0);
	CheckCompare("", "1", -1);
	CheckCompare(".", "", 0);
}

void TestLongComponents() {
	CheckCompare("123456789012345678901234567890", "123456789012345678901234567891", -1);
	CheckCompare("99999999999999999999999", "100000000000000000000000", -1);
	CheckCompare("000000000000000000000000001", "1", 0);
	CheckCompare("1.18446744073709551616", "1.18446744073709551615", 1);
	CheckCompare("18446744073709551616", "18446744073709551616", 0);
}

void TestNonNumericComponents() {
	CheckCompare("1.0a", "1.0b", -1);
	CheckCompare("1.0A", "1.0a", 0);
	CheckCompare("1.beta", "1.alpha", 1);
	CheckCompare("1.beta.2", "1.BETA.10", -1);
	CheckCompare("v1", "v1", 0);
	// A numeric component orders before a non-numeric one.
	CheckCompare("1.5", "1.5a", -1);
	CheckCompare("1.10", "1.9a", -1);
	CheckCompare("1", "1.a", -1);
	CheckCompare("1.-1", "1.0", 1);
	CheckCompare(" 1", "1", 1);
}

// Sorting needs a strict weak ordering: antisymmetric and transitive, with transitive equality.
void TestOrderIsConsistent() {
	const std::vector<std::string> samples = {
	    "",     "0",    "1",     "1.0",  "01",   "1.0.1", "1.2",   "1.10", "2",
	    "10",   "10a",  "2a",    "a",    "A",    "b",     "1.a",   "1.A",  "1.5a",
	    "1.9a", "1..2", "1.0.2", "0001", "1.b2", "1.b10", "09.99", "9.990"};
	for (const auto& a: samples) {
		Check(CompareVersions(a, a) == 0, "\"" + a + "\" equals itself");
		for (const auto& b: samples) {
			Check(CompareVersions(a, b) == -CompareVersions(b, a),
			      "antisymmetry for \"" + a + "\" and \"" + b + "\"");
			for (const auto& c: samples) {
				const int  ab   = CompareVersions(a, b);
				const int  bc   = CompareVersions(b, c);
				const int  ac   = CompareVersions(a, c);
				const auto text = "\"" + a + "\", \"" + b + "\", \"" + c + "\"";
				if (ab < 0 && bc < 0) {
					Check(ac < 0, "transitivity of < for " + text);
				}
				if (ab == 0 && bc == 0) {
					Check(ac == 0, "transitivity of == for " + text);
				}
				if (ab == 0 && bc < 0) {
					Check(ac < 0, "equal versions order alike for " + text);
				}
			}
		}
	}
}

void CheckFormat(std::string_view raw, std::string_view expected) {
	const auto result = FormatSystemSoftwareVersion(raw);
	Check(result == expected, "FormatSystemSoftwareVersion(\"" + std::string(raw) + "\") is \"" +
	                              result + "\", expected \"" + std::string(expected) + "\"");
}

void TestFormatSystemSoftwareVersion() {
	CheckFormat("0x0150000000000000", "1.50");
	CheckFormat("0x0123450000000000", "1.23.45");
	CheckFormat("0x1000000000000000", "10.00");
	CheckFormat("0x0000010000000000", "0.00.01");
	CheckFormat("0x9999990000000000", "99.99.99");
	CheckFormat("0x015000ABCDEFabcd", "1.50");
	CheckFormat("0x0700000000000001", "7.00");
	CheckFormat("0x0702000123456789", "7.02");
	// An upper-case prefix is accepted, as in the Qt launcher.
	CheckFormat("0X0150000000000000", "1.50");
	CheckFormat("0X0123450000000000", "1.23.45");
	CheckFormat("0X015000ABCDEFabcd", "1.50");
}

void TestFormatRejectsMalformed() {
	const std::vector<std::string> malformed = {
	    "",
	    "0x",
	    "0X",
	    "0X015000000000000",    // 17 characters
	    "1x0150000000000000",   // wrong prefix
	    "1X0150000000000000",   // wrong prefix
	    "0y0150000000000000",   // wrong prefix
	    "OX0150000000000000",   // letter O, not zero
	    "000150000000000000",   // no prefix
	    "x00150000000000000",   // wrong prefix
	    "0x015000000000000",    // 17 characters
	    "0x01500000000000000",  // 19 characters
	    "0x01A0000000000000",   // MMmmpp not decimal
	    "0x015 000000000000",   // MMmmpp not decimal
	    "0x-150000000000000",   // MMmmpp not decimal
	    "0x015000000000000G",   // tail not hexadecimal
	    "0x015000 000000000",   // tail not hexadecimal
	    " 0x0150000000000000",  // leading space
	    "0x0150000000000000 ",  // trailing space
	    "0x01500000000000000x", // too long
	    "1.50",
	};
	for (const auto& raw: malformed) {
		CheckFormat(raw, "");
	}
	CheckFormat(std::string_view("0x0150000000000000\0", 19), "");
	CheckFormat(std::string_view("0x015000000000000\0", 18), "");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("VersionTests");
	TestNumericComponents();
	TestLeadingZeros();
	TestMissingComponents();
	TestLongComponents();
	TestNonNumericComponents();
	TestOrderIsConsistent();
	TestFormatSystemSoftwareVersion();
	TestFormatRejectsMalformed();
	std::printf("VersionTests: all tests passed\n");
	return 0;
}
