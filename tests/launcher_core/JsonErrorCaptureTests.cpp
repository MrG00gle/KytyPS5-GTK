// Example tests of the SAX error capture that Launcher_Core uses to word JSON parse errors
// (src/launcher/core/jsonErrorCapture.h, plan D25), for nlohmann::json and nlohmann::ordered_json:
// it accepts every value kind of the public nlohmann::json_sax interface (including a binary
// value, which only binary formats such as CBOR produce), records the parser's message and stops
// at the first error, and DescribeJsonError falls back to "invalid JSON" when the parser reports
// none.

#include "launcher/core/jsonErrorCapture.h"
#include "support/noDisplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "JsonErrorCaptureTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

template <typename Json>
void TestCapture(const std::string& what) {
	using kyty::core::DescribeJsonError;
	using kyty::core::JsonErrorCapture;

	// Every JSON value kind: null, booleans, a negative and a positive integer, a float, strings,
	// keys, arrays, and objects.
	JsonErrorCapture<Json> capture;
	const std::string      valid =
	    R"({"n":null,"t":true,"f":false,"i":-1,"u":1,"d":1.5,"s":"x","a":[{}],"o":{"k":[]}})";
	Check(Json::sax_parse(valid, &capture), what + ": valid JSON is accepted");
	Check(capture.Message().empty(), what + ": no message for valid JSON");

	// A CBOR byte string is a binary value.
	const std::vector<std::uint8_t> cbor = {0x42, 0x01, 0x02};
	Check(Json::sax_parse(cbor, &capture, Json::input_format_t::cbor),
	      what + ": a binary value is accepted");
	Check(capture.Message().empty(), what + ": no message for a binary value");

	// The first error stops the parse and is recorded.
	Check(!Json::sax_parse(std::string(R"({"a":[1,)"), &capture), what + ": an error stops");
	Check(capture.Message().find("parse error") != std::string::npos,
	      what + ": the parser's message: " + capture.Message());

	Check(DescribeJsonError<Json>("{\"a\":}").find("syntax error") != std::string::npos,
	      what + ": DescribeJsonError names the syntax error");
	Check(DescribeJsonError<Json>("{}") == "invalid JSON",
	      what + ": content without an error gives the fallback text");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("JsonErrorCaptureTests");
	TestCapture<nlohmann::json>("json");
	TestCapture<nlohmann::ordered_json>("ordered_json");
	std::printf("JsonErrorCaptureTests: all tests passed\n");
	return 0;
}
