#include "graphics/presentation/window/vulkanDeviceSelection.h"

namespace Libs::Graphics {

namespace {

// VK_VERSION_MAJOR, VK_VERSION_MINOR, and VK_VERSION_PATCH.
std::string VersionText(uint32_t version, bool with_patch) {
	auto text = std::to_string(version >> 22u) + "." + std::to_string((version >> 12u) & 0x3ffu);
	if (with_patch) {
		text += "." + std::to_string(version & 0xfffu);
	}
	return text;
}

int TypeRank(VulkanDeviceType type) {
	switch (type) {
		case VulkanDeviceType::DiscreteGpu: return 0;
		case VulkanDeviceType::IntegratedGpu: return 1;
		case VulkanDeviceType::VirtualGpu: return 2;
		case VulkanDeviceType::Cpu: return 3;
		case VulkanDeviceType::Other: break;
	}
	return 4;
}

} // namespace

std::string VulkanVersionRejection(uint32_t supported_version, uint32_t required_version) {
	return "Vulkan " + VersionText(required_version, false) +
	       " is required, but device supports only " + VersionText(supported_version, true);
}

std::string UnsupportedRejection(std::string_view feature_or_extension) {
	return std::string(feature_or_extension) + " is not supported";
}

std::string NoUniversalQueueRejection() {
	return "No universal graphics, compute, and presentation queue";
}

std::optional<size_t> SelectVulkanDevice(const std::vector<VulkanDeviceCandidate>& candidates) {
	std::optional<size_t> best;
	for (size_t index = 0; index < candidates.size(); index++) {
		if (!candidates[index].rejections.empty()) {
			continue;
		}
		if (!best || TypeRank(candidates[index].type) < TypeRank(candidates[*best].type)) {
			best = index;
		}
	}
	return best;
}

std::string DescribeVulkanDeviceRejections(const std::vector<VulkanDeviceCandidate>& candidates) {
	if (candidates.empty()) {
		return "no Vulkan physical device found";
	}
	std::string text = "Could not find suitable device:";
	for (const auto& candidate: candidates) {
		if (candidate.rejections.empty()) {
			continue;
		}
		text += "\n  " + candidate.name + ": ";
		for (size_t i = 0; i < candidate.rejections.size(); i++) {
			text += (i == 0 ? "" : "; ") + candidate.rejections[i];
		}
	}
	return text;
}

} // namespace Libs::Graphics
