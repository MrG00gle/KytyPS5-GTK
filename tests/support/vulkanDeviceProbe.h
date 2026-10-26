#pragma once

// GPU_Test skip path (Requirement 17.6). A GPU test calls KytyTests::ExitIfNoVulkanDevice() before
// it creates its own Vulkan instance. When the Vulkan loader is missing, instance creation fails,
// or the instance reports zero physical devices, the test prints
//   <test_name>: no Vulkan device available (<reason>)
// and exits with 77, the SKIP_RETURN_CODE of every gpu-labeled CTest, without running static
// destructors. Otherwise the probe instance is destroyed and the function returns.

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <vulkan/vulkan_core.h>

namespace KytyTests {

inline constexpr int kNoVulkanDeviceExitCode = 77;

[[noreturn]] inline void ExitNoVulkanDevice(const char* test_name, const std::string& reason) {
	std::printf("%s: no Vulkan device available (%s)\n", test_name, reason.c_str());
	std::fflush(nullptr);
	std::_Exit(kNoVulkanDeviceExitCode);
}

inline void ExitIfNoVulkanDevice(const char* test_name) {
	// A test that sets up Vulkan several times probes only once.
	static bool found = false;
	if (found) {
		return;
	}
	// The loader stays loaded: the test loads it again right after this check.
	void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (loader == nullptr) {
		const char* error = dlerror();
		ExitNoVulkanDevice(test_name, std::string("cannot load libvulkan.so.1: ") +
		                                  (error != nullptr ? error : "unknown error"));
	}
	const auto get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
	if (get_instance_proc_addr == nullptr) {
		ExitNoVulkanDevice(test_name, "libvulkan.so.1 has no vkGetInstanceProcAddr");
	}
	const auto create_instance =
	    reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc_addr(nullptr, "vkCreateInstance"));
	if (create_instance == nullptr) {
		ExitNoVulkanDevice(test_name, "the Vulkan loader has no vkCreateInstance");
	}

	VkApplicationInfo app {};
	app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = test_name;
	app.apiVersion       = VK_API_VERSION_1_3;

	VkInstanceCreateInfo instance_info {};
	instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app;

	VkInstance     instance = VK_NULL_HANDLE;
	const VkResult created  = create_instance(&instance_info, nullptr, &instance);
	if (created != VK_SUCCESS || instance == VK_NULL_HANDLE) {
		ExitNoVulkanDevice(test_name,
		                   "vkCreateInstance failed with VkResult " + std::to_string(created));
	}

	const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
	    get_instance_proc_addr(instance, "vkDestroyInstance"));
	const auto enumerate_physical_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
	    get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices"));
	if (destroy_instance == nullptr || enumerate_physical_devices == nullptr) {
		ExitNoVulkanDevice(test_name, "the Vulkan instance has no vkDestroyInstance or "
		                              "vkEnumeratePhysicalDevices");
	}

	uint32_t       device_count = 0;
	const VkResult enumerated   = enumerate_physical_devices(instance, &device_count, nullptr);
	destroy_instance(instance, nullptr);
	if (enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) {
		ExitNoVulkanDevice(test_name, "vkEnumeratePhysicalDevices failed with VkResult " +
		                                  std::to_string(enumerated));
	}
	if (device_count == 0) {
		ExitNoVulkanDevice(test_name, "vkEnumeratePhysicalDevices reports zero physical devices");
	}
	found = true;
}

} // namespace KytyTests
