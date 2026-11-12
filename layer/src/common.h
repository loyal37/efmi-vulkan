// Shared declarations of the EFMI Vulkan layer.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The layer defines its own entry points and calls down through function
// pointers only, so the prototypes of the loader are not wanted.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define EFVK_VERSION "2.1.5"
#define EFVK_PROTOCOL 1
#define EFVK_LAYER_NAME "VK_LAYER_EFMI_probe"

namespace efvk {

// What the layer does once it is loaded. Set once at start-up from the
// environment variable EFMI_VK_MODE.
enum class Mode {
    Passthrough, // forwards everything, hooks only instance / device creation and present
    Probe,       // read-only tracking, frame capture and the named pipe
};

// How the DLL got its place in front of the Vulkan calls of the process.
enum class Entry {
    Layer,  // the Vulkan loader loaded it as a layer and chains it in
    Inject, // something loaded it with LoadLibrary; it redirects the exports of vulkan-1.dll itself
};

struct Config {
    Mode mode = Mode::Probe;
    Entry entry = Entry::Layer;
    std::string m2_package; // explicit experimental mesh package; empty keeps ordinary probe behavior
    // Directory generation (D-078): the EFMI Mods folder whose character Mods run without packages,
    // the Core folder they run with (default: Core\EFMI beside the Mods folder) and the project-owned
    // folder their settings are saved in (default: settings beside the replacement module).
    std::string m2_mods, m2_core, m2_settings;
    // The measured DX11 result for a 2D file at a 3D shader image on one device (zero: the
    // dx11-invalid-2d-to-3d-zero policy, docs/character-materials.md), as "vendor:device"; the
    // directory generation applies it only on that device. The evidence is checked by the tooling.
    uint32_t m2_dimension_zero_vendor = 0, m2_dimension_zero_device = 0;
    bool m2_enabled = false;
    uint64_t m2_reserve_mb = 0; // device-local memory kept free for the application; 0: automatic (D-092, D-094)
    uint64_t m2_identity_expiry_ms = 10000; // an object not drawn this long is forgotten (D-100); tests shorten it
    // Persistent INI variables are saved at most this often while they change, as the original's
    // settings_auto_save_interval ([System] of d3dx.ini, 60 by default and in the installation).
    uint64_t settings_save_seconds = 60;
    bool shader_cache = true; // translated CustomShader stages kept in settings/shader-cache (D-126)
    uint64_t m2_texture_budget_mb = 0; // cap of the textures' device memory; 0: half the device-local heap (D-094)
    uint64_t m2_texture_reduce_mb = 16; // a texture file this large loads without its top mip level when memory is tight; 0: never
    bool m2_texture_preload = false;    // every image of a package demanded at its activation (tests); default on first bind
    bool m2_pageable = true;            // VK_EXT_memory_priority and VK_EXT_pageable_device_local_memory when supported (D-095)
    bool m2_gpu_bones = true;           // the merged skeleton imported by compute dispatches, not CPU reads (D-117); the probe can switch it
    bool m2_owned = true; // owned GPU execution (Present work, GPU work in draws); m2_owned = 0 turns it off
    // D-131: the Mods folder (m2_mods) as one INI generation, its character Mods' packages carved from it;
    // m2_folder_generation = 0 keeps a generation per character Mod.
    bool m2_folder_generation = true;
    bool keep_log_open = true;
    uint64_t unmap_snapshot_limit = 16ull << 20; // largest staging buffer copied when its memory is unmapped
    uint32_t shadow_usage = 0;                   // VkBufferUsageFlags that get a CPU shadow (set in util.cpp)
    // Probe mode keeps descriptor, image and shader metadata by default. Switching this off leaves
    // draws, buffers and hashes, for the case that the extra tracking costs too much in a real process.
    bool track_resources = true;
    // Upper bound for SPIR-V kept in memory. Past it, modules keep their hash and size only.
    uint64_t shader_budget = 512ull << 20;
    // Probe mode hashes mip 0 of an image when a buffer is copied over the whole of it, so that a
    // texture can be told from another of the same size and format. An upload above the limit is
    // counted and not read. Reading upload memory costs time while a scene loads; 0 switches it off.
    bool image_hash = true;
    uint64_t image_hash_limit = 64ull << 20;
    // Opt-in hot-path measurements; adds clocks/counters and must be compared with profiling off.
    bool profile_tracker = false;
    // At unmap every buffer with the transfer-source bit that lies wholly inside the mapped range and
    // within unmap_snapshot_limit is copied. Off restricts the copy to staging buffers, that is to
    // buffers that are transfer buffers and nothing else. 0.7.4 had that restriction as its default,
    // on the belief that the game's mesh pools were what was being copied; its own counters showed
    // in the game that no pool is ever unmapped (docs/unmap-snapshots.md), so the default is back.
    bool unmap_snapshot_all = true;
};

extern Config g_cfg;
// A character package, a directory of them or a Mods folder (D-078) is configured.
inline bool character_configured() { return !g_cfg.m2_package.empty() || !g_cfg.m2_mods.empty(); }

// What happened while attaching; reported by the status command.
struct AttachInfo {
    std::atomic<uint32_t> loader_found{0};        // vulkan-1.dll was seen
    std::atomic<uint32_t> loader_was_loaded{0};   // it was already in the process when the DLL arrived
    std::atomic<uint32_t> exports_redirected{0};  // export table entries pointed at the hooks
    std::atomic<uint32_t> instances_created{0};
    std::atomic<uint32_t> devices_created{0};
    std::atomic<uint64_t> passive_calls{0};       // hooked calls for a device created before attaching
};

extern AttachInfo g_attach;

// One export of vulkan-1.dll to redirect: where it should lead, and where to
// store the address it had.
struct ExportHook {
    const char* name;
    void* hook;
    void** original;
};

// layer.cpp
void collect_export_hooks(std::vector<ExportHook>& out);
void exports_resolved();

// bootstrap.cpp
void bootstrap_start();

void util_init(HMODULE self);
void logf(const char* fmt, ...);

// Notices on the overlay (D-137), as the original's LogOverlay: logged, and shown at the top left of the
// presented image for the time of their level (Overlay.cpp log_levels), at most ten at once, the most
// severe first. A non-empty `key` shows the notice once for the process.
enum class NoticeLevel : int { Dire = 0, Warning = 1, Notice = 2, Info = 3 };
void notice(NoticeLevel level, const char* key, const char* text);
struct NoticeLine {
    NoticeLevel level;
    std::string text; // UTF-8
};
// The lines to show now; a line's time starts at its first showing, as the original's does. False when
// they are the ones of `version` (then `lines` is left as it was); `version` follows them.
bool notices_visible(std::vector<NoticeLine>& lines, uint64_t& version);
// Notices queued since the start, and the last few texts.
uint64_t notices_queued(std::vector<std::string>& recent);
const std::wstring& module_dir();
const std::string& exe_name();

// CRC-32C over a byte range, continuing from a previous result (0 to start).
uint32_t crc32c(uint32_t seed, const void* data, size_t size);
bool crc32c_uses_hardware();

// Cache the page policy at map time. Unknown/mixed ranges use the ordinary copy path.
bool mapped_range_is_write_combined(const void* data, size_t size);
bool streaming_reads_available();
// A write-combined source must not overlap the ordinary CPU destination. Cached shadows
// use memmove, preserving valid self-copies. Both paths copy exactly the requested bytes.
void copy_buffer_bytes(void* dst, const void* src, size_t size, bool write_combined);
// Logs a hardware fault anywhere in the process with its module, offset and stack (once installed).
void install_crash_log();
// The merged skeleton's import on the GPU as set now (m2_gpu_bones; the probe changes it at run time): the
// layer answers from its settings, a module from the host's, so an exchange is not needed to compare.
bool live_gpu_bones();

#ifdef EFVK_NO_PROBE
// The release build carries no probe (scripts/build.py release): no pipe, nothing outside the game reads
// or changes it, no module exchange.
inline void ipc_start() {}
#else
void ipc_start();
#endif

} // namespace efvk
