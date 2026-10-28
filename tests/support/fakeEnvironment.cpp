#include "support/fakeEnvironment.h"

namespace KytyTests {

void FakeEnvironment::Set(std::string_view name, std::string_view value) {
	m_variables.insert_or_assign(std::string(name), std::string(value));
}

void FakeEnvironment::Unset(std::string_view name) {
	const auto found = m_variables.find(name);
	if (found != m_variables.end()) {
		m_variables.erase(found);
	}
}

void FakeEnvironment::AddExistingPath(const std::filesystem::path& path) {
	m_existing.insert(path.lexically_normal());
}

void FakeEnvironment::RemoveExistingPath(const std::filesystem::path& path) {
	m_existing.erase(path.lexically_normal());
}

std::optional<std::string> FakeEnvironment::Get(std::string_view name) const {
	const auto found = m_variables.find(name);
	if (found == m_variables.end()) {
		return std::nullopt;
	}
	return found->second;
}

bool FakeEnvironment::FileExists(const std::filesystem::path& path) const {
	return m_existing.contains(path.lexically_normal());
}

} // namespace KytyTests
