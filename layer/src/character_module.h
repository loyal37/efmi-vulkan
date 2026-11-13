// The boundary between the loaded layer (the host: hooks and tracker) and the code that decides
// and records character replacements (the module). The host has that code built in and can take
// another copy of it from a DLL while the process runs (docs/hot-swap.md).
//
// Both sides are built from the same headers with the same compiler settings; EFVK_ABI is a stamp
// of those headers, and a module with another stamp is refused. Objects cross the boundary as they
// are (tracker structures, strings, vectors): both sides allocate from the process heap. Code of a
// module that was ever used stays loaded until the process ends, because objects it made may
// outlive the backend that made them.
// D-192 contract: the host saves/restores CmdBuf::gfx_bound around every graphics draw callback and
// its forwarded driver draw. A module may temporarily change it for hunting; it retains that pipeline's owner.
#pragma once

#include "character_backend.h"

#ifndef EFVK_ABI
#define EFVK_ABI 0ull // set by scripts/build.py from the shared headers
#endif

namespace efvk {

struct BufInfo;
struct SetBind;
struct BoundRange;
struct Config;

// What a module calls back for: the tracker's readers and the host's log.
struct HostApi {
    uint32_t size = 0;
    uint64_t abi = 0;
    const Config* config = nullptr; // read once while the entry function runs
    void (*log)(const char* text) = nullptr;
    uint32_t (*crc32c)(uint32_t seed, const void* data, size_t size) = nullptr;
    uint8_t (*read_buffer_locked)(Device*, const BufInfo&, uint64_t off, uint64_t size, uint8_t* out) = nullptr;
    void (*collect_buffer_ranges_locked)(Device*, const SetBind&, const uint32_t* dyn, uint32_t dyn_count, uint32_t slot,
                                         std::vector<BoundRange>& out) = nullptr;
    // The settings the probe changes in a running process (`set`), read live: the module's copy of the
    // configuration is the one its entry made (BUG-055).
    bool (*gpu_bones)() = nullptr;
    // The host's overlay notices (D-137): level as NoticeLevel, key and text as notice() takes them.
    void (*notice)(int level, const char* key, const char* text) = nullptr;
};

// The presentation a frame ends with, when the host saw one (vkQueuePresentKHR on `queue`). The
// module may draw into the image of the first swap chain before it is shown: it then waits for
// `waits` itself, replaces them with the semaphores the presentation is to wait for and sets
// `replaced`. Called on the presenting thread, which owns `queue` for the duration.
struct PresentWork {
    VkQueue queue = VK_NULL_HANDLE;
    const VkPresentInfoKHR* info = nullptr;
    std::vector<VkSemaphore> waits;
    bool replaced = false;
};

// One application submission call (vkQueueSubmit or vkQueueSubmit2) as the module sees it (D-127): per
// batch the recordings the host tracks, and the command buffers of the module's that the host puts at the
// front of that batch, after the batch's waits (whose stages it widens to cover them); the timelines the
// host signals after the application's batches in the same call. The host fills `cbs`, the module
// `prefix` and `signals`.
struct CharacterModule;
struct SubmitWork {
    struct Batch {
        std::vector<CmdBuf*> cbs;
        std::vector<VkCommandBuffer> prefix;
    };
    struct Signal { VkSemaphore semaphore = VK_NULL_HANDLE; uint64_t value = 0; };
    std::vector<Batch> batches;
    std::vector<Signal> signals;
    // The backends whose command buffers are in the call, and their code: kept by the host until it
    // reported the call, so an exchange in between cannot free what the queue is about to run.
    std::vector<std::pair<std::shared_ptr<CharacterBackend>, const CharacterModule*>> owners;
    bool any_prefix() const
    {
        for (const auto& b : batches) if (!b.prefix.empty()) return true;
        return false;
    }
};

// The functions of one copy of the replacement code.
struct CharacterModule {
    uint32_t size = 0;
    uint64_t abi = 0;
    const char* version = "";
    std::shared_ptr<CharacterBackend> (*create)(Device*, const VkPhysicalDeviceProperties&, const VkPhysicalDeviceMemoryProperties&) = nullptr;
    // False when the package or the device was refused; `reason` says why.
    bool (*usable)(CharacterBackend&, std::string& reason) = nullptr;
    // Mesh, arenas and images are in place: draws can be replaced from the first one on.
    bool (*ready)(CharacterBackend&) = nullptr;
    void (*stop)(CharacterBackend&) = nullptr;
    bool (*record)(CharacterBackend&, CmdBuf*, const DrawRec&) = nullptr;
    void (*retire)(CharacterBackend&, uint32_t pipeline_id) = nullptr;
    void (*frame)(CharacterBackend&, bool had_draws, PresentWork* present) = nullptr;
    void (*status)(CharacterBackend&, JsonWriter&) = nullptr;
    void (*enable)(CharacterBackend&, bool) = nullptr;
    // Before the application's submission on `queue`: the work its recordings need first. Submitted
    // ahead by the module: the texture uploads' publication and owned draw work; returned in `work` for
    // the host to put into the application's batches: the bone imports and snapshots (D-127).
    VkResult (*submit)(CharacterBackend&, VkQueue, SubmitWork& work) = nullptr;
    // After the application's call with the module's work in it: whether it reached the queue.
    void (*submitted)(CharacterBackend&, VkQueue, SubmitWork& work, VkResult result) = nullptr;
    // After the application's vkCmdEndRenderPass of the instance `cb->pass` names (D-113): work that
    // belongs to that pass and needs targets of its own (the translucent captures and composite).
    void (*pass_end)(CharacterBackend&, CmdBuf*) = nullptr;
    // At the application's vkCmdBindPipeline (graphics), on its recording thread: the pipeline to bind in
    // its place, or `pipeline` (D-133: its twin without the character dither while a discard gate is set).
    VkPipeline (*bind)(CharacterBackend&, CmdBuf*, VkPipeline pipeline) = nullptr;
    // After the application created a graphics pipeline, its record in Device::pipes complete (D-133:
    // the twin a discard gate needs is prepared then). The creating thread.
    void (*created)(CharacterBackend&, VkPipeline pipeline) = nullptr;
    // A non-indexed or indirect draw, while Device::shader_draws is set (docs/vulkan-native-mods.md): true when it is
    // to be dropped (a ShaderOverride's `handling = skip`, hunting's skip). Indexed draws reach the same through record.
    bool (*shader_draw)(CharacterBackend&, CmdBuf*, const DrawRec&) = nullptr;
};

// The copy this binary was built with.
const CharacterModule* builtin_character_module();

// Exported by a module DLL: `const CharacterModule* efmi_vk_character_module(const HostApi*)`.
// Null when the host's stamp or structure size is not the module's.
#define EFVK_MODULE_ENTRY "efmi_vk_character_module"
using CharacterModuleEntry = const CharacterModule* (*)(const HostApi*);

} // namespace efvk
