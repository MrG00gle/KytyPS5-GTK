#include "launcher/gtk/services/controllerService.h"

#include "launcher/gtk/gobjectPtr.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <glib.h>
#include <map>
#include <utility>
#include <vector>

namespace kyty::gtk {

namespace {

// The poll intervals of the Qt launcher's ControllerPreview: 16 ms while a listener shows the live
// state (the diagram, binding capture), 100 ms for the lightbar color alone.
constexpr guint kListenerPollIntervalMs = 16;
constexpr guint kLightbarPollIntervalMs = 100;

// Diagram controls (the Qt launcher's BUTTONS table).
constexpr struct {
	SDL_GamepadButton button;
	const char*       control;
} kDiagramButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "Cross"},      {SDL_GAMEPAD_BUTTON_EAST, "Circle"},
    {SDL_GAMEPAD_BUTTON_WEST, "Square"},      {SDL_GAMEPAD_BUTTON_NORTH, "Triangle"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Down"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Left"},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Right"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3"},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3"},
    {SDL_GAMEPAD_BUTTON_START, "Options"},
};

// Pad sources (the Qt launcher's RAW_BUTTONS table; the ids of core::PadSources()).
constexpr struct {
	SDL_GamepadButton button;
	const char*       name;
} kPadSources[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "South"},
    {SDL_GAMEPAD_BUTTON_EAST, "East"},
    {SDL_GAMEPAD_BUTTON_WEST, "West"},
    {SDL_GAMEPAD_BUTTON_NORTH, "North"},
    {SDL_GAMEPAD_BUTTON_BACK, "Back"},
    {SDL_GAMEPAD_BUTTON_GUIDE, "Guide"},
    {SDL_GAMEPAD_BUTTON_START, "Start"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "LeftStick"},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RightStick"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LeftShoulder"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RightShoulder"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "DpadUp"},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "DpadDown"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "DpadLeft"},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "DpadRight"},
    {SDL_GAMEPAD_BUTTON_MISC1, "Misc1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "RightPaddle1"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "LeftPaddle1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "RightPaddle2"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "LeftPaddle2"},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad"},
};

float Axis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
	const int value = SDL_GetGamepadAxis(gamepad, axis);
	return static_cast<float>(value / (value < 0 ? 32768.0 : 32767.0));
}

class SdlControllerService final: public ControllerService {
public:
	~SdlControllerService() override {
		*m_alive = false;
		m_listeners.clear(); // Stop() calls no listener now
		Stop();
	}

	ListenerId AddListener(ChangedFn on_changed) override {
		const ListenerId id = m_next_listener++;
		m_listeners.emplace(id, std::move(on_changed));
		UpdateRunning();
		return id;
	}

	void RemoveListener(ListenerId id) override {
		if (m_listeners.erase(id) > 0) {
			UpdateRunning();
		}
	}

	void SetLightbarColor(std::optional<LightbarColor> color) override {
		if (color == m_color) {
			return;
		}
		m_color = color;
		UpdateRunning();
		for (auto& [id, gamepad]: m_gamepads) {
			ApplyColor(gamepad);
		}
	}

	void SetPaused(bool paused) override {
		if (paused == m_state.paused) {
			return;
		}
		m_state.paused = paused;
		UpdateRunning();
		Notify();
	}

	[[nodiscard]] const ControllerState& State() const override { return m_state; }

	[[nodiscard]] bool Polling() const override { return m_poll.Active(); }

private:
	[[nodiscard]] bool Wanted() const {
		return !m_state.paused && (!m_listeners.empty() || m_color.has_value());
	}

	[[nodiscard]] guint PollInterval() const {
		return m_listeners.empty() ? kLightbarPollIntervalMs : kListenerPollIntervalMs;
	}

	void UpdateRunning() {
		if (!Wanted()) {
			Stop();
		} else if (m_poll.Active() && m_poll_interval != PollInterval()) {
			ArmPoll(); // a listener came or the last one went: the other rate
		} else {
			Start();
		}
	}

	// Replaces the poll source with one at PollInterval().
	void ArmPoll() {
		m_poll_interval = PollInterval();
		m_poll          = SourceId(g_timeout_add(m_poll_interval, &OnPoll, this));
	}

	void Start() {
		if (m_poll.Active()) {
			return;
		}
		SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
		// The launcher has no SDL window, so events must arrive without focus.
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
			const std::string error = SDL_GetError();
			g_message("cannot start the controller service: %s", error.c_str());
			if (error != m_state.error) {
				m_state.error = error;
				Notify();
			}
			return;
		}
		m_initialized         = true;
		int             count = 0;
		SDL_JoystickID* ids   = SDL_GetGamepads(&count);
		for (int i = 0; ids != nullptr && i < count; i++) {
			OpenGamepad(ids[i]);
		}
		SDL_free(ids);
		ArmPoll();
		const bool had_error = !m_state.error.empty();
		m_state.error.clear();
		if (!Refresh() && had_error) {
			Notify();
		}
	}

	void Stop() {
		m_poll.Remove();
		for (auto& [id, gamepad]: m_gamepads) {
			SDL_CloseGamepad(gamepad);
		}
		m_gamepads.clear();
		if (m_initialized) {
			SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
			m_initialized = false;
		}
		ControllerState idle;
		idle.error  = m_state.error;
		idle.paused = m_state.paused;
		if (!(idle == m_state)) {
			m_state = std::move(idle);
			Notify();
		}
	}

	void OpenGamepad(SDL_JoystickID id) {
		if (m_gamepads.find(id) != m_gamepads.end()) {
			return;
		}
		if (SDL_Gamepad* gamepad = SDL_OpenGamepad(id); gamepad != nullptr) {
			m_gamepads.emplace(id, gamepad);
			ApplyColor(gamepad);
		}
	}

	void ApplyColor(SDL_Gamepad* gamepad) const {
		if (!m_color || !SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
		                                        SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false)) {
			return;
		}
		if (!SDL_SetGamepadLED(gamepad, m_color->red, m_color->green, m_color->blue)) {
			g_message("cannot set the controller lightbar: %s", SDL_GetError());
		}
	}

	static gboolean OnPoll(gpointer data) {
		static_cast<SdlControllerService*>(data)->Refresh();
		return G_SOURCE_CONTINUE;
	}

	// One SDL event pump serves the diagram, the capture, and the lightbar (it also delivers the
	// deferred LED commands of Bluetooth DualSense controllers). Returns whether listeners ran.
	bool Refresh() {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
				OpenGamepad(event.gdevice.which);
			} else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
				if (const auto found = m_gamepads.find(event.gdevice.which);
				    found != m_gamepads.end()) {
					SDL_CloseGamepad(found->second);
					m_gamepads.erase(found);
				}
			}
		}
		ControllerState state;
		state.error  = m_state.error;
		state.paused = m_state.paused;
		if (!m_gamepads.empty()) {
			Read(m_gamepads.begin()->second, state);
		}
		if (state == m_state) {
			return false;
		}
		m_state = std::move(state);
		Notify();
		return true;
	}

	static void Read(SDL_Gamepad* gamepad, ControllerState& state) {
		const char* name  = SDL_GetGamepadName(gamepad);
		state.device_name = name != nullptr ? name : "";
		for (const auto& source: kPadSources) {
			if (SDL_GamepadHasButton(gamepad, source.button) &&
			    SDL_GetGamepadButton(gamepad, source.button)) {
				state.raw_pressed.insert(source.name);
			}
		}
		const float left_trigger  = Axis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
		const float right_trigger = Axis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		if (left_trigger > 0.5f) {
			state.raw_pressed.insert("LeftTrigger");
		}
		if (right_trigger > 0.5f) {
			state.raw_pressed.insert("RightTrigger");
		}
		for (const auto& control: kDiagramButtons) {
			if (SDL_GetGamepadButton(gamepad, control.button)) {
				state.pressed.insert(control.control);
			}
		}
		const auto stick = [&](SDL_GamepadAxis x_axis, SDL_GamepadAxis y_axis,
		                       const std::string& prefix, float& x, float& y) {
			x = Axis(gamepad, x_axis);
			y = Axis(gamepad, y_axis);
			if (std::hypot(x, y) < 0.15f) {
				x = 0;
				y = 0;
			}
			if (x < -0.25f) {
				state.pressed.insert(prefix + "Left");
			}
			if (x > 0.25f) {
				state.pressed.insert(prefix + "Right");
			}
			if (y < -0.25f) {
				state.pressed.insert(prefix + "Up");
			}
			if (y > 0.25f) {
				state.pressed.insert(prefix + "Down");
			}
		};
		stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, "LeftStick", state.left_x,
		      state.left_y);
		stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, "RightStick", state.right_x,
		      state.right_y);
		if (left_trigger > 0.05f) {
			state.pressed.insert("L2");
		}
		if (right_trigger > 0.05f) {
			state.pressed.insert("R2");
		}
		bool touching = false;
		if (SDL_GetNumGamepadTouchpads(gamepad) > 0) {
			const int fingers = SDL_GetNumGamepadTouchpadFingers(gamepad, 0);
			for (int finger = 0; finger < fingers; finger++) {
				bool  down = false;
				float x    = 0;
				if (SDL_GetGamepadTouchpadFinger(gamepad, 0, finger, &down, &x, nullptr, nullptr) &&
				    down) {
					state.pressed.insert(x < 0.5f ? "TouchPad" : "TouchPadRight");
					touching = true;
				}
			}
		}
		if (!touching && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
			state.pressed.insert("TouchPad");
			state.pressed.insert("TouchPadRight");
		}
	}

	// Listeners may add or remove listeners, or change the color or pause state.
	void Notify() {
		std::vector<ListenerId> ids;
		ids.reserve(m_listeners.size());
		for (const auto& [id, fn]: m_listeners) {
			ids.push_back(id);
		}
		const auto alive = m_alive;
		for (const ListenerId id: ids) {
			const auto found = m_listeners.find(id);
			if (found == m_listeners.end()) {
				continue;
			}
			const ChangedFn fn = found->second;
			if (fn) {
				fn();
			}
			if (!*alive) {
				return;
			}
		}
	}

	std::map<ListenerId, ChangedFn>        m_listeners;
	ListenerId                             m_next_listener = 1;
	std::optional<LightbarColor>           m_color;
	ControllerState                        m_state;
	bool                                   m_initialized = false;
	std::map<SDL_JoystickID, SDL_Gamepad*> m_gamepads;
	SourceId                               m_poll;
	guint                                  m_poll_interval = 0;
	std::shared_ptr<bool>                  m_alive         = std::make_shared<bool>(true);
};

} // namespace

std::unique_ptr<ControllerService> MakeSdlControllerService() {
	return std::make_unique<SdlControllerService>();
}

} // namespace kyty::gtk
