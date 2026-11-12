// Whole-character replacement from a character package: every matched section of one mod, with one
// merged skeleton per character instance and optional static materials.
// Inactive unless a character package is explicitly configured.
#pragma once
#include "common.h"

#include <memory>
#include <string>

namespace efvk {
struct Device;
struct CmdBuf;
struct DrawRec;
class JsonWriter;
class CharacterBackend;
void character_device_setup(Device*, const VkDeviceCreateInfo*, VkInstance, PFN_vkGetInstanceProcAddr);
std::shared_ptr<CharacterBackend> character_create(Device*, const VkPhysicalDeviceProperties&,
                                                   const VkPhysicalDeviceMemoryProperties&);
bool character_usable(CharacterBackend&, std::string& reason);
bool character_ready(CharacterBackend&);
void character_stop(CharacterBackend&);
// True when the original draw is to be dropped: its replacement was recorded, or the mod hides it.
bool character_record(CharacterBackend&, CmdBuf*, const DrawRec&);
// A draw outside the index's path (non-indexed, indirect): hunting and the ShaderOverride sections that name its
// modules (docs/vulkan-native-mods.md). True when the draw is to be dropped.
bool character_shader_draw(CharacterBackend&, CmdBuf*, const DrawRec&);
void character_retire(CharacterBackend&, uint32_t pipeline_id);
// Once per present. An interval without draws does not count as a frame of the character.
void character_frame(CharacterBackend&, bool had_draws);
void character_status(CharacterBackend&, JsonWriter&);
void character_enable(CharacterBackend&, bool);
} // namespace efvk
