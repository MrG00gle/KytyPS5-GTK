// Vulkan device selection (task 25.3, Requirements 15.3 and 15.11) with fake device lists: the
// ranking DISCRETE > INTEGRATED > VIRTUAL > CPU (> OTHER), ties in enumeration order, rejected
// devices never chosen, the rejection messages, and the error for an empty list.

#include "graphics/presentation/window/vulkanDeviceSelection.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Libs::Graphics::DescribeVulkanDeviceRejections;
using Libs::Graphics::NoUniversalQueueRejection;
using Libs::Graphics::SelectVulkanDevice;
using Libs::Graphics::UnsupportedRejection;
using Libs::Graphics::VulkanDeviceCandidate;
using Libs::Graphics::VulkanDeviceType;
using Libs::Graphics::VulkanVersionRejection;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "VulkanDeviceSelectionTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckText(const std::string& actual, const std::string& expected, const std::string& what) {
	Check(actual == expected, what + " is '" + expected + "', got '" + actual + "'");
}

void CheckSelected(const std::vector<VulkanDeviceCandidate>& candidates,
                   std::optional<size_t> expected, const std::string& what) {
	const auto selected = SelectVulkanDevice(candidates);
	const auto text     = [](std::optional<size_t> index) {
		return index ? std::to_string(*index) : std::string("none");
	};
	Check(selected == expected, what + ": selected " + text(expected) + ", got " + text(selected));
}

// VK_MAKE_API_VERSION(0, major, minor, patch).
constexpr uint32_t Version(uint32_t major, uint32_t minor, uint32_t patch) {
	return (major << 22u) | (minor << 12u) | patch;
}

constexpr uint32_t VULKAN_1_3 = Version(1, 3, 0);

VulkanDeviceCandidate Device(const std::string& name, VulkanDeviceType type,
                             std::vector<std::string> rejections = {}) {
	return {name, type, std::move(rejections)};
}

const char* TypeName(VulkanDeviceType type) {
	switch (type) {
		case VulkanDeviceType::Other: return "OTHER";
		case VulkanDeviceType::IntegratedGpu: return "INTEGRATED";
		case VulkanDeviceType::DiscreteGpu: return "DISCRETE";
		case VulkanDeviceType::VirtualGpu: return "VIRTUAL";
		case VulkanDeviceType::Cpu: return "CPU";
	}
	return "?";
}

// From best to worst.
const std::vector<VulkanDeviceType> RANKED = {
    VulkanDeviceType::DiscreteGpu, VulkanDeviceType::IntegratedGpu, VulkanDeviceType::VirtualGpu,
    VulkanDeviceType::Cpu, VulkanDeviceType::Other};

// Every ordered pair of qualifying device types: the better type wins in either position, and the
// first one wins a tie.
void TestRankingPairs() {
	for (size_t a = 0; a < RANKED.size(); a++) {
		for (size_t b = 0; b < RANKED.size(); b++) {
			const std::vector<VulkanDeviceCandidate> candidates = {Device("first", RANKED[a]),
			                                                       Device("second", RANKED[b])};
			const auto what = std::string(TypeName(RANKED[a])) + " then " + TypeName(RANKED[b]);
			CheckSelected(candidates, b < a ? 1 : 0, what);
		}
	}
}

void TestRanking() {
	// A software device listed first (lavapipe, llvmpipe) loses to an integrated GPU.
	CheckSelected({Device("llvmpipe", VulkanDeviceType::Cpu),
	               Device("Intel(R) HD Graphics 530", VulkanDeviceType::IntegratedGpu)},
	              1, "CPU then INTEGRATED");
	// All five types, worst first: the discrete GPU wins.
	CheckSelected({Device("other", VulkanDeviceType::Other), Device("cpu", VulkanDeviceType::Cpu),
	               Device("virtual", VulkanDeviceType::VirtualGpu),
	               Device("integrated", VulkanDeviceType::IntegratedGpu),
	               Device("discrete", VulkanDeviceType::DiscreteGpu)},
	              4, "all five types, worst first");
	// Removing the best one each time gives the next type in the ranking.
	CheckSelected({Device("other", VulkanDeviceType::Other), Device("cpu", VulkanDeviceType::Cpu),
	               Device("virtual", VulkanDeviceType::VirtualGpu),
	               Device("integrated", VulkanDeviceType::IntegratedGpu)},
	              3, "no discrete GPU");
	CheckSelected({Device("other", VulkanDeviceType::Other), Device("cpu", VulkanDeviceType::Cpu),
	               Device("virtual", VulkanDeviceType::VirtualGpu)},
	              2, "no hardware GPU");
	CheckSelected({Device("other", VulkanDeviceType::Other), Device("cpu", VulkanDeviceType::Cpu)},
	              1, "CPU and OTHER");
	CheckSelected({Device("other", VulkanDeviceType::Other)}, 0, "OTHER only");
	CheckSelected({Device("lavapipe", VulkanDeviceType::Cpu)}, 0, "one qualifying device");
}

void TestTies() {
	CheckSelected({Device("gpu 0", VulkanDeviceType::DiscreteGpu),
	               Device("gpu 1", VulkanDeviceType::DiscreteGpu),
	               Device("gpu 2", VulkanDeviceType::DiscreteGpu)},
	              0, "three discrete GPUs");
	CheckSelected({Device("cpu", VulkanDeviceType::Cpu),
	               Device("igpu 1", VulkanDeviceType::IntegratedGpu),
	               Device("igpu 2", VulkanDeviceType::IntegratedGpu)},
	              1, "two integrated GPUs after a CPU device");
	// A rejected device does not take part in the tie.
	CheckSelected({Device("gpu 0", VulkanDeviceType::DiscreteGpu, {"depthBounds is not supported"}),
	               Device("gpu 1", VulkanDeviceType::DiscreteGpu),
	               Device("gpu 2", VulkanDeviceType::DiscreteGpu)},
	              1, "the first discrete GPU rejected");
}

void TestRejectedNeverChosen() {
	CheckSelected({Device("discrete", VulkanDeviceType::DiscreteGpu, {NoUniversalQueueRejection()}),
	               Device("integrated", VulkanDeviceType::IntegratedGpu,
	                      {UnsupportedRejection("tessellationShader")}),
	               Device("lavapipe", VulkanDeviceType::Cpu)},
	              2, "only the CPU device qualifies");
	CheckSelected(
	    {Device("discrete", VulkanDeviceType::DiscreteGpu,
	            {VulkanVersionRejection(Version(1, 2, 0), VULKAN_1_3)}),
	     Device("lavapipe", VulkanDeviceType::Cpu, {UnsupportedRejection("depthBounds")})},
	    std::nullopt, "every device rejected");
	CheckSelected({}, std::nullopt, "no device");
}

void TestRejectionMessages() {
	CheckText(VulkanVersionRejection(Version(1, 2, 189), VULKAN_1_3),
	          "Vulkan 1.3 is required, but device supports only 1.2.189", "Vulkan 1.2.189");
	CheckText(VulkanVersionRejection(Version(1, 1, 0), VULKAN_1_3),
	          "Vulkan 1.3 is required, but device supports only 1.1.0", "Vulkan 1.1.0");
	CheckText(VulkanVersionRejection(Version(1, 2, 4095), Version(1, 3, 7)),
	          "Vulkan 1.3 is required, but device supports only 1.2.4095",
	          "the required patch level is not named");
	CheckText(VulkanVersionRejection(Version(0, 1023, 0), Version(2, 0, 0)),
	          "Vulkan 2.0 is required, but device supports only 0.1023.0", "the widest minor");
	CheckText(UnsupportedRejection("dynamicRendering"), "dynamicRendering is not supported",
	          "a missing feature");
	CheckText(UnsupportedRejection("VK_EXT_depth_clip_control"),
	          "VK_EXT_depth_clip_control is not supported", "a missing extension");
	CheckText(NoUniversalQueueRejection(), "No universal graphics, compute, and presentation queue",
	          "no universal queue");
}

void TestDescriptions() {
	CheckText(DescribeVulkanDeviceRejections({}), "no Vulkan physical device found", "empty list");

	// Every device, in enumeration order, with its reasons in the order they were found.
	const std::vector<VulkanDeviceCandidate> rejected = {
	    Device("Old GPU", VulkanDeviceType::DiscreteGpu,
	           {VulkanVersionRejection(Version(1, 2, 0), VULKAN_1_3)}),
	    Device("llvmpipe (LLVM 20.1.8, 256 bits)", VulkanDeviceType::Cpu,
	           {UnsupportedRejection("depthBounds"),
	            UnsupportedRejection("VK_EXT_depth_clip_control"),
	            "Format vk::Format::eD32SfloatS8Uint cannot be used as depth buffer"}),
	    Device("Headless GPU", VulkanDeviceType::IntegratedGpu, {NoUniversalQueueRejection()})};
	CheckSelected(rejected, std::nullopt, "three rejected devices");
	CheckText(DescribeVulkanDeviceRejections(rejected),
	          "Could not find suitable device:"
	          "\n  Old GPU: Vulkan 1.3 is required, but device supports only 1.2.0"
	          "\n  llvmpipe (LLVM 20.1.8, 256 bits): depthBounds is not supported; "
	          "VK_EXT_depth_clip_control is not supported; "
	          "Format vk::Format::eD32SfloatS8Uint cannot be used as depth buffer"
	          "\n  Headless GPU: No universal graphics, compute, and presentation queue",
	          "three rejected devices");

	// One device, as with --gpu or an OpenXR headset.
	CheckText(DescribeVulkanDeviceRejections({Device("lavapipe", VulkanDeviceType::Cpu,
	                                                 {UnsupportedRejection("depthBounds")})}),
	          "Could not find suitable device:\n  lavapipe: depthBounds is not supported",
	          "one rejected device");

	// A device that qualifies has nothing to report.
	CheckText(DescribeVulkanDeviceRejections(
	              {Device("gpu", VulkanDeviceType::DiscreteGpu),
	               Device("cpu", VulkanDeviceType::Cpu, {NoUniversalQueueRejection()})}),
	          "Could not find suitable device:"
	          "\n  cpu: No universal graphics, compute, and presentation queue",
	          "a qualifying device is not listed");
}

} // namespace

int main() {
	TestRankingPairs();
	TestRanking();
	TestTies();
	TestRejectedNeverChosen();
	TestRejectionMessages();
	TestDescriptions();
	std::printf("VulkanDeviceSelectionTests: all cases passed\n");
	return 0;
}
