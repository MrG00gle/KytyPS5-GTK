#pragma once

// Process environment and file existence, behind an interface so that data path resolution
// (dataPaths.h, task 15.2) can be tested with KytyTests::FakeEnvironment.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::core {

class Environment {
public:
	virtual ~Environment() = default;

	// The value of the environment variable, or nullopt when it is not set. A set but empty
	// variable gives "".
	[[nodiscard]] virtual std::optional<std::string> Get(std::string_view name) const = 0;

	// std::filesystem::exists() without exceptions: false when the path does not exist or its
	// status cannot be read. Symbolic links are followed, so a dangling link does not exist.
	[[nodiscard]] virtual bool FileExists(const std::filesystem::path& path) const = 0;
};

// The real environment: getenv() and std::filesystem::exists(path, error_code). A name that is
// empty or contains '=' or a NUL byte is never set.
std::unique_ptr<Environment> MakePosixEnvironment();

} // namespace kyty::core
