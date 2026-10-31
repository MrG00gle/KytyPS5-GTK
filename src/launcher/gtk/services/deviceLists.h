#pragma once

// The host devices a settings editor offers (task 19.5; plan D22; Requirement 9.5): the Vulkan
// physical devices for the GPU selection and the audio recording devices for the audio input
// selection. An interface, so that the visual harness and the UI tests can replace it. The editor
// shows "Automatic" or "None" first and then these names; when a list cannot be read it shows only
// that entry and the error.
//
// The SDL implementation runs on the main thread, each time a settings editor opens:
// - VulkanDevices(): SDL's offscreen video driver (no display connection) and its Vulkan loader
//   (SDL_Vulkan_LoadLibrary), a Vulkan 1.3 instance without layers, extensions, or portability
//   enumeration (as the Qt launcher asked for), and the device names of vkEnumeratePhysicalDevices
//   in the loader's order, which is the order of the Emulator's --gpu index; then the instance, the
//   loader, and the video subsystem are released.
// - RecordingDevices(): SDL_GetAudioRecordingDevices and SDL_GetAudioDeviceName, in SDL's order,
//   each name once (as the Qt launcher).
// SDL never installs its signal handlers (SDL_HINT_NO_SIGNAL_HANDLERS).

#include <memory>
#include <string>
#include <vector>

namespace kyty::gtk {

struct DeviceList {
	std::vector<std::string> names;
	std::string              error; // why the list could not be read; "" when it was read
};

class DeviceLists {
public:
	virtual ~DeviceLists() = default;

	virtual DeviceList VulkanDevices()    = 0;
	virtual DeviceList RecordingDevices() = 0;
};

std::unique_ptr<DeviceLists> MakeSdlDeviceLists();

} // namespace kyty::gtk
