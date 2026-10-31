#pragma once

// Game controllers through SDL (task 19.5; plan D23; LFI-22, LFI-23, LFI-24; Requirement 11.15),
// ported from the Qt launcher's ControllerPreview (src/launcher/src/controllerPreview.cpp): the
// lightbar color (the configured color, or the settings editor's live preview), the pressed state
// for the DualSense diagram, and the pad sources for binding capture. An interface, so that the
// visual harness and the UI tests can replace it.
//
// The SDL implementation initializes SDL_INIT_GAMEPAD only while it polls, and polls (SDL events
// and the first connected gamepad on a GLib timeout) only while it has a listener or a lightbar
// color and is not paused: every 16 ms while it has a listener, every 100 ms for the lightbar color
// alone (the Qt launcher's rates). While a game runs the launcher pauses it (LFI-23): no polling,
// no LED writes, and the subsystem is shut down, so the Emulator has the controllers to itself.
// Gamepads that connect while it polls are opened (SDL_EVENT_GAMEPAD_ADDED) and get the lightbar
// color at once (SDL_SetGamepadLED, where the gamepad has an RGB LED). Listeners run on the main
// thread when the state changes; they may add and remove listeners.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace kyty::gtk {

struct LightbarColor {
	uint8_t red   = 0;
	uint8_t green = 0;
	uint8_t blue  = 0;

	friend bool operator==(const LightbarColor&, const LightbarColor&) = default;
};

struct ControllerState {
	// The first connected gamepad's name; "" when none is connected.
	std::string device_name;
	// Pressed controls of the DualSense diagram, by the names of the Qt launcher's diagram: the
	// face, shoulder, and D-pad controls ("Cross", "L1", "Up", ..., "L3", "R3", "Options"), "L2"
	// and "R2" past 5 % travel, "LeftStickLeft/Right/Up/Down" and "RightStick..." past a quarter of
	// the travel (with a 15 % dead zone), and "TouchPad"/"TouchPadRight" for a finger on the left
	// or right half of the touch pad or a pad click (both halves).
	std::set<std::string> pressed;
	// Pressed pad sources by their core::PadSources() ids ("South", "LeftShoulder", ...), with
	// "LeftTrigger" and "RightTrigger" past half the travel: what binding capture assigns.
	std::set<std::string> raw_pressed;
	// Stick positions from -1 to 1 (0 inside the dead zone).
	float left_x  = 0;
	float left_y  = 0;
	float right_x = 0;
	float right_y = 0;
	// Why the controller subsystem could not start (SDL's message); "" otherwise.
	std::string error;
	// A game runs: nothing is polled.
	bool paused = false;

	friend bool operator==(const ControllerState&, const ControllerState&) = default;
};

class ControllerService {
public:
	using ListenerId = uint64_t; // never 0
	using ChangedFn  = std::function<void()>;

	virtual ~ControllerService() = default;

	// on_changed runs after every change of State() until the listener is removed.
	virtual ListenerId AddListener(ChangedFn on_changed) = 0;
	virtual void       RemoveListener(ListenerId id)     = 0;

	// The lightbar color for every connected controller, or none (the LED is left as it is).
	virtual void SetLightbarColor(std::optional<LightbarColor> color) = 0;

	// While a game runs (LFI-23).
	virtual void SetPaused(bool paused) = 0;

	[[nodiscard]] virtual const ControllerState& State() const = 0;

	// The poll timer exists (a listener or a lightbar color, and not paused).
	[[nodiscard]] virtual bool Polling() const = 0;
};

std::unique_ptr<ControllerService> MakeSdlControllerService();

} // namespace kyty::gtk
