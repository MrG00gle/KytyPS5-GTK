#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDEVICESELECTION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDEVICESELECTION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

// VkPhysicalDeviceType, without the Vulkan headers.
enum class VulkanDeviceType { Other, IntegratedGpu, DiscreteGpu, VirtualGpu, Cpu };

// One enumerated physical device and why it cannot be used (empty when it qualifies).
struct VulkanDeviceCandidate {
	std::string              name;
	VulkanDeviceType         type = VulkanDeviceType::Other;
	std::vector<std::string> rejections;
};

// The rejection reasons, worded in one place. Versions use the VK_MAKE_API_VERSION encoding.
std::string VulkanVersionRejection(uint32_t supported_version, uint32_t required_version);
std::string UnsupportedRejection(std::string_view feature_or_extension);
std::string NoUniversalQueueRejection();

// The qualifying candidate whose type ranks highest (DISCRETE > INTEGRATED > VIRTUAL > CPU >
// OTHER); ties keep the enumeration order. nullopt when no candidate qualifies.
std::optional<size_t> SelectVulkanDevice(const std::vector<VulkanDeviceCandidate>& candidates);

// The error for a failed selection: every rejected candidate with its reasons, or
// "no Vulkan physical device found" for an empty list.
std::string DescribeVulkanDeviceRejections(const std::vector<VulkanDeviceCandidate>& candidates);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_VULKANDEVICESELECTION_H_
