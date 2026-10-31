#include "launcher/gtk/services/deviceLists.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstdint>
#include <glib.h>
#include <string>
#include <utility>
#include <vulkan/vulkan.h>

namespace kyty::gtk {

namespace {

std::string Message(const char* format, const char* argument) {
	const GCharPtr text(g_strdup_printf(format, argument));
	return std::string(text.View());
}

std::string VulkanFailure(const char* function, VkResult result) {
	const GCharPtr text(
	    g_strdup_printf(_("%s failed (VkResult %d)"), function, static_cast<int>(result)));
	return std::string(text.View());
}

class SdlDeviceLists final: public DeviceLists {
public:
	DeviceList VulkanDevices() override {
		SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
		// The offscreen driver never connects to a display, whatever SDL_VIDEO_DRIVER says.
		SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
		const bool video = SDL_InitSubSystem(SDL_INIT_VIDEO);
		SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
		DeviceList list;
		if (!video) {
			list.error = SDL_GetError();
		} else {
			if (SDL_Vulkan_LoadLibrary(nullptr)) {
				list = Enumerate();
				SDL_Vulkan_UnloadLibrary();
			} else {
				list.error = SDL_GetError();
			}
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
		if (!list.error.empty()) {
			g_message("cannot list the Vulkan devices: %s", list.error.c_str());
		}
		return list;
	}

	DeviceList RecordingDevices() override {
		SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
		DeviceList list;
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			list.error = SDL_GetError();
			g_message("cannot list the audio recording devices: %s", list.error.c_str());
			return list;
		}
		int                count   = 0;
		SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
		if (devices == nullptr) {
			list.error = SDL_GetError();
			g_message("cannot list the audio recording devices: %s", list.error.c_str());
		}
		for (int i = 0; devices != nullptr && i < count; i++) {
			const char* name = SDL_GetAudioDeviceName(devices[i]);
			if (name != nullptr &&
			    std::find(list.names.begin(), list.names.end(), name) == list.names.end()) {
				list.names.emplace_back(name);
			}
		}
		SDL_free(devices);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return list;
	}

private:
	// With SDL's Vulkan loader loaded.
	static DeviceList Enumerate() {
		DeviceList list;
		auto       get_instance_proc =
		    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
		if (get_instance_proc == nullptr) {
			list.error = SDL_GetError();
			return list;
		}
		auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
		    get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance"));
		if (create_instance == nullptr) {
			list.error = Message(_("the Vulkan loader has no %s"), "vkCreateInstance");
			return list;
		}
		VkApplicationInfo application {};
		application.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.pApplicationName = "kyty_launcher";
		application.apiVersion       = VK_API_VERSION_1_3;
		VkInstanceCreateInfo info {};
		info.sType              = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo   = &application;
		VkInstance     instance = VK_NULL_HANDLE;
		const VkResult created  = create_instance(&info, nullptr, &instance);
		if (created != VK_SUCCESS) {
			list.error = VulkanFailure("vkCreateInstance", created);
			return list;
		}
		auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
		    get_instance_proc(instance, "vkDestroyInstance"));
		auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
		    get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
		auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
		    get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
		if (enumerate == nullptr || properties == nullptr) {
			list.error = Message(_("the Vulkan loader has no %s"),
			                     enumerate == nullptr ? "vkEnumeratePhysicalDevices"
			                                          : "vkGetPhysicalDeviceProperties");
		} else {
			uint32_t                      count  = 0;
			VkResult                      result = enumerate(instance, &count, nullptr);
			std::vector<VkPhysicalDevice> devices(count);
			if (result == VK_SUCCESS && count > 0) {
				result = enumerate(instance, &count, devices.data());
				devices.resize(count);
			}
			if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
				list.error = VulkanFailure("vkEnumeratePhysicalDevices", result);
				devices.clear();
			}
			for (VkPhysicalDevice device: devices) {
				VkPhysicalDeviceProperties device_properties {};
				properties(device, &device_properties);
				list.names.emplace_back(device_properties.deviceName);
			}
		}
		if (destroy_instance != nullptr) {
			destroy_instance(instance, nullptr);
		}
		return list;
	}
};

} // namespace

std::unique_ptr<DeviceLists> MakeSdlDeviceLists() {
	return std::make_unique<SdlDeviceLists>();
}

} // namespace kyty::gtk
