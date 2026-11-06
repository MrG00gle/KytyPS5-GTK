#pragma once

// Internal to Launcher_Core: include it only from .cpp files, because it includes
// nlohmann/json.hpp, which kyty_launcher_core links PRIVATE (public headers include only std and
// core headers).
//
// With exceptions disabled, Json::parse(..., nullptr, false) says that content is not valid JSON
// but not why. JsonErrorCapture is a SAX handler on the public nlohmann::json_sax interface that
// accepts every value and records the parser's error message, so DescribeJsonError can name the
// reason (compatibility.cpp, cheats.cpp; plan D25).

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace kyty::core {

// Json is nlohmann::json or nlohmann::ordered_json.
template <typename Json>
class JsonErrorCapture final: public nlohmann::json_sax<Json> {
public:
	using Base = nlohmann::json_sax<Json>;

	bool null() override { return true; }
	bool boolean(bool /*value*/) override { return true; }
	bool number_integer(typename Base::number_integer_t /*value*/) override { return true; }
	bool number_unsigned(typename Base::number_unsigned_t /*value*/) override { return true; }
	bool number_float(typename Base::number_float_t /*value*/,
	                  const typename Base::string_t& /*text*/) override {
		return true;
	}
	bool string(typename Base::string_t& /*value*/) override { return true; }
	bool binary(typename Base::binary_t& /*value*/) override { return true; }
	bool start_object(std::size_t /*elements*/) override { return true; }
	bool key(typename Base::string_t& /*value*/) override { return true; }
	bool end_object() override { return true; }
	bool start_array(std::size_t /*elements*/) override { return true; }
	bool end_array() override { return true; }

	// Records the error text and stops the parse.
	bool parse_error(std::size_t /*position*/, const std::string& /*last_token*/,
	                 const typename Json::exception& error) override {
		m_message = error.what();
		return false;
	}

	// The parser's message for the first error; empty when the content parsed.
	[[nodiscard]] const std::string& Message() const { return m_message; }

private:
	std::string m_message;
};

// The nlohmann parser's message for the error in json without its "[json.exception.<kind>.<id>] "
// prefix ("parse error at line 1, column 1: ..."; phase 8 plan D9), or "invalid JSON" when it
// reports none.
template <typename Json>
std::string DescribeJsonError(std::string_view json) {
	JsonErrorCapture<Json> capture;
	Json::sax_parse(json.begin(), json.end(), &capture);
	std::string message = capture.Message();
	if (constexpr std::string_view kPrefix = "[json.exception."; message.starts_with(kPrefix)) {
		if (const auto end = message.find("] "); end != std::string::npos) {
			message.erase(0, end + 2);
		}
	}
	return message.empty() ? std::string("invalid JSON") : message;
}

} // namespace kyty::core
