#include "launcher/gtk/bindingCapture.h"

#include "launcher/core/inputMapping.h"
#include "launcher/core/keyNames.h"

#include <algorithm>
#include <glib.h>
#include <iterator>
#include <utility>

namespace kyty::gtk {

namespace {

// The modifiers that make a character key ambiguous (Ctrl, Alt, Super; Hyper and Meta are what
// some keyboard layouts report for them).
constexpr unsigned kCharacterModifiers =
    GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_HYPER_MASK | GDK_META_MASK;

// A key name of one character (KeyNameFromKeysym names printable keys by their character).
bool IsCharacterName(const std::string& name) {
	return g_utf8_validate(name.c_str(), static_cast<gssize>(name.size()), nullptr) != FALSE &&
	       g_utf8_strlen(name.c_str(), static_cast<gssize>(name.size())) == 1;
}

} // namespace

KeyCaptureResult ClassifyCapturedKey(uint32_t keyval, GdkModifierType state) {
	if (keyval == GDK_KEY_Escape) {
		return {KeyCapture::Cancelled, {}};
	}
	std::string name = core::KeyNameFromKeysym(keyval);
	if (name.empty()) {
		return {KeyCapture::Unsupported, {}};
	}
	if (core::IsReservedKeyName(name)) {
		return {KeyCapture::Reserved, {}};
	}
	if (IsCharacterName(name) && (static_cast<unsigned>(state) & kCharacterModifiers) != 0) {
		return {KeyCapture::Modified, {}};
	}
	if (!core::IsEmulatorKeyName(name)) {
		return {KeyCapture::Unsupported, {}};
	}
	return {KeyCapture::Bound, std::move(name)};
}

std::optional<std::string> MouseButtonBinding(unsigned button) {
	switch (button) {
		case 1: return std::string("Mouse:Left");
		case 2: return std::string("Mouse:Middle");
		case 3: return std::string("Mouse:Right");
		case 8: return std::string("Mouse:X1");
		case 9: return std::string("Mouse:X2");
		default: return std::nullopt;
	}
}

std::optional<std::string> PadCapture::Update(const std::set<std::string>& pressed) {
	std::set<std::string> still_held;
	std::set_intersection(m_held.begin(), m_held.end(), pressed.begin(), pressed.end(),
	                      std::inserter(still_held, still_held.end()));
	m_held = std::move(still_held);
	for (const auto& source: core::PadSources()) {
		const std::string id(source.id);
		if (pressed.count(id) != 0 && m_held.count(id) == 0) {
			return id;
		}
	}
	return std::nullopt;
}

} // namespace kyty::gtk
