#pragma once
#include "shader_commands.h"

namespace efmi::gpu {
// Native Vulkan modules keep their descriptor sets and entry points. The original application's
// module supplies the interface contract; no DX11 register remapping is performed here.
bool validate_native_shader(const std::vector<uint32_t>& words, VkShaderStageFlagBits stage,
                            const ShaderHost& host, const VkPhysicalDeviceFeatures& features, uint32_t api, std::string& reason);
bool native_shader_interface(const std::vector<uint32_t>& original, const std::vector<uint32_t>& replacement,
                             VkShaderStageFlagBits stage, const std::string& entry, std::string& reason);
}
