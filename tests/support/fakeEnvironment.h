#pragma once

// An Environment with explicit variables and a set of paths that exist. Nothing is read from the
// real process environment or file system.

#include "launcher/core/environment.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace KytyTests {

class FakeEnvironment final: public kyty::core::Environment {
public:
	FakeEnvironment()           = default;
	~FakeEnvironment() override = default;

	void Set(std::string_view name, std::string_view value);
	void Unset(std::string_view name);

	// Paths are compared in lexically normal form ("a/./b" is "a/b").
	void AddExistingPath(const std::filesystem::path& path);
	void RemoveExistingPath(const std::filesystem::path& path);

	[[nodiscard]] std::optional<std::string> Get(std::string_view name) const override;
	[[nodiscard]] bool FileExists(const std::filesystem::path& path) const override;

private:
	std::map<std::string, std::string, std::less<>> m_variables;
	std::set<std::filesystem::path>                 m_existing;
};

} // namespace KytyTests
