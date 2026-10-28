#include "launcher/core/environment.h"

#include <cstdlib>
#include <system_error>

namespace kyty::core {

namespace {

class PosixEnvironment final: public Environment {
public:
	[[nodiscard]] std::optional<std::string> Get(std::string_view name) const override {
		if (name.empty() || name.find('=') != std::string_view::npos ||
		    name.find('\0') != std::string_view::npos) {
			return std::nullopt;
		}
		const std::string name_text(name);
		const char*       value = std::getenv(name_text.c_str());
		if (value == nullptr) {
			return std::nullopt;
		}
		return std::string(value);
	}

	[[nodiscard]] bool FileExists(const std::filesystem::path& path) const override {
		std::error_code ec;
		const bool      exists = std::filesystem::exists(path, ec);
		return exists && !ec;
	}
};

} // namespace

std::unique_ptr<Environment> MakePosixEnvironment() {
	return std::make_unique<PosixEnvironment>();
}

} // namespace kyty::core
