// EFMITTL's capture and composite on Vulkan (D-113, docs/efmittl-transparency.md): the GPU work of
// the library's custom shaders, recorded by the character renderer after the game's forward pass.
#pragma once

#include "state.h"
#include "command_owner.h"
#include "draw_snapshots.h"
#include "pipeline_variant.h"
#include "shader_hooks.h"
#include <chrono>
#include <filesystem>
#include <mutex>

namespace efvk {

// The four draws of one captured call, in the library's order of its custom shaders per face set:
// colour with the game's shaders, then opacity with the library's alpha shader.
enum class CaptureKind : uint32_t { ColourBack = 0, ColourFront = 1, OpacityBack = 2, OpacityFront = 3 };
constexpr uint32_t kCaptureKinds = 4;
// A variant cache identity of a capture recipe: the game pipeline's identity in the low 32 bits.
inline uint64_t capture_identity(uint32_t pipeline_id, CaptureKind kind) { return (uint64_t(uint32_t(kind) + 1) << 40) | pipeline_id; }

// The game pipeline as the library's capture custom shaders draw with it: one RGBA16F colour target
// and a D32 depth target of the layer's own, depth GREATER_OR_EQUAL with writes for colour (EQUAL
// without for opacity), the face set culled, no blending, RGB written (A for opacity), no stencil or
// depth bias; the game's dynamic states kept so its own stay valid. For opacity the fragment stage is
// `opacity` (the library's alpha shader, its images at private bindings 2 and 3 of set `own_set`).
std::shared_ptr<const GraphicsRecipe> capture_recipe(const GraphicsRecipe&, CaptureKind, const std::vector<uint32_t>* opacity,
                                                     std::string& why);

// A translucent call's shadow (D-185): its opacity (in steps of 1/1023), whether a mask gives it per texel, the mask's
// channel and inversion. As the library's shadow shaders read IniParams[180] (opacity.hlsli).
struct ShadowParams {
    uint32_t alpha = 1023, channel = 0;
    bool masked = false, invert = false;
    uint32_t key() const { return alpha | uint32_t(masked) << 10 | channel << 11 | uint32_t(invert) << 13; }
};
// A variant cache identity of a shadow recipe: the game pipeline's identity in the low 32 bits, the parameters above them.
inline uint64_t shadow_identity(uint32_t pipeline_id, const ShadowParams& p) { return 1ull << 62 | uint64_t(p.key()) << 32 | pipeline_id; }

class Translucency;
// One captured call, recorded at the end of its pass with the game's bindings of its draw.
struct TranslucentCapture {
    std::shared_ptr<const PipelineVariant> variants[kCaptureKinds];
    SnapshotSlot slots[2]; // colour, opacity: the private set and the instance entry's offset
    DrawGeometry geometry;
    IndexedDraw draw;
    struct Bound { VkPipelineLayout layout = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; std::vector<uint32_t> dyn; };
    std::vector<Bound> sets; // the game's sets below the private one, as bound at the draw
    VkViewport viewport{};
    VkRect2D scissor{};
    float params[4] = {1, 0, 0, 0}; // IniParams[180] of the call: alpha, masked, channel, invert
    uint32_t params_layer = 0;
    std::shared_ptr<void> layers; // the layer set of the pass's size its sets took views of
};

class Translucency {
public:
    struct Dispatch {
#define EFVK_TRANSLUCENCY_FUNCS(X) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CmdPipelineBarrier) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdDraw) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdUpdateBuffer) X(CmdCopyBufferToImage) X(CmdClearColorImage)
#define X(n) PFN_vk##n n = nullptr;
        EFVK_TRANSLUCENCY_FUNCS(X)
#undef X
    };
    Translucency(Device*, const VkPhysicalDeviceMemoryProperties&);
    ~Translucency();
    // The library's alpha and composite shaders compiled from its folder with the DXC beside the
    // layer, once; false with the reason when they do not compile.
    bool shaders(const std::filesystem::path& library, uint32_t own_set, std::string& why);
    const std::vector<uint32_t>* opacity_fragment(uint32_t own_set) const;
    // The capture recipe of a game pipeline (capture_recipe), made once per pipeline and kind; null
    // with the reason. retire() forgets a pipeline's (its variants go with the cache's retirement).
    std::shared_ptr<const GraphicsRecipe> recipe(const PipeInfo&, CaptureKind, uint32_t own_set, std::string& why);
    void retire(uint32_t pipeline_id);
    // The translucent shadow's fragment stage (D-185): the layer's own main over the library's opacity functions (its
    // shaders/opacity.hlsli: the mask read bilinearly at the UV, the interleaved gradient noise), so that a texel is kept
    // with a probability equal to the opacity there, as shadow_a.hlsl does. Compiled once with a mask (the UV at input
    // location 0, the mask at private binding 2 of `own_set`) and once without (no input, no image); the opacity, channel
    // and inversion are specialization constants 1, 3 and 4. False with the reason when they do not compile.
    bool shadow_shaders(const std::filesystem::path& library, uint32_t own_set, std::string& why);
    // The game's shadow pipeline drawing a translucent call: its vertex stage (the plain shadow one given the UV output when
    // the call has a mask, add_uv_output), the shadow fragment stage with the call's parameters, everything else the
    // game's. Once per pipeline and parameters; null with the reason.
    std::shared_ptr<const GraphicsRecipe> shadow_recipe(const PipeInfo&, const ShadowParams&, uint32_t own_set, std::string& why);
    uint64_t shadows() const { return shadows_; }
    void count_shadow() { ++shadows_; }
    // The layer set of a forward pass's size, made on first need and kept per size (a frame may hold
    // passes of two sizes); null with the reason. Its four full-size layers take memory only when a
    // pass first draws into them, and a set no pass used for a few seconds goes (frame()). The views
    // a capture's private sets take of it: the neutral screen-space mask and a call's parameter layer.
    std::shared_ptr<void> prepare(uint32_t width, uint32_t height, std::string& why);
    // Once per presented frame: releases the idle layer sets (recordings still using one keep it).
    void frame();
    struct Memory { uint32_t sets = 0, layers = 0; uint64_t bytes = 0; };
    Memory memory();
    static SnapshotImage neutral_view(const std::shared_ptr<void>& layers);
    static SnapshotImage params_view(const std::shared_ptr<void>& layers, uint32_t layer);
    static constexpr uint32_t kCalls = 64; // captured calls of one pass; the composite takes layer kCalls
    // This recording's queue of captures for its current pass; the caller holds no lock of ours.
    bool queue(CmdBuf*, TranslucentCapture&&, std::string& why);
    // The next parameter layer of the recording's pass (counts calls); false past kCalls.
    bool reserve(CmdBuf*, uint32_t& layer);
    // After the application ended `cb->pass`: its captures, then the composite into its colour target
    // over its depth, then the application's bindings again. Nothing when the pass queued none.
    void pass_end(CmdBuf*);
    // A generation that loaded EFMITTL drew (its hooks are then told the game's copy passes).
    void listen() { listening_.store(true, std::memory_order_relaxed); }
    bool listening() const { return listening_.load(std::memory_order_relaxed); }
    uint64_t captures() const { return captures_; }
    uint64_t composites() const { return composites_; }
    std::string last_error() { std::lock_guard<std::mutex> lock(mx_); return error_; }
private:
    struct Layers;
    struct Owner;
    Owner* owner(CmdBuf*, bool make);
    bool allocate_layer(Layers&, uint32_t layer, std::string& why);
    bool composite_pipeline(VkFormat colour, VkImageLayout, const std::vector<VkDynamicState>&, VkPipeline&, std::string& why);
    VkImageView depth_view(VkImage, uint32_t image_id, VkFormat, std::shared_ptr<const void>& keep, uint64_t& serial, std::string& why);
    VkFramebuffer colour_framebuffer(VkImageView, uint32_t view_id, VkFormat, VkImageLayout, uint32_t width, uint32_t height,
                                     std::shared_ptr<const void>& keep, std::string& why);
    void restore(CmdBuf*);
    void fail(const std::string& why);
    Device* device_;
    Dispatch d_;
    VkPhysicalDeviceMemoryProperties memory_;
    std::mutex mx_;
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<Layers>> layer_sets_; // by width, height
    uint64_t next_serial_ = 0;
    std::vector<uint32_t> alpha_, composite_vs_, composite_ps_;
    std::map<uint32_t, std::vector<uint32_t>> alpha_by_set_; // the alpha shader's images repointed to (own_set, 2/3)
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const GraphicsRecipe>> recipes_; // pipeline id, kind
    std::map<uint32_t, std::vector<uint32_t>> shadow_masked_, shadow_plain_;                  // by own set
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const GraphicsRecipe>> shadow_recipes_; // pipeline id, key
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    std::map<std::pair<uint32_t, uint32_t>, VkRenderPass> composite_passes_; // colour format, final layout
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, VkPipeline> composite_pipelines_; // format, layout, dynamic mask
    struct Held;
    std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<Held>> depth_views_, framebuffers_;
    std::atomic<uint64_t> captures_{0}, composites_{0}, shadows_{0};
    std::atomic<bool> listening_{false};
    std::string error_;
};

} // namespace efvk
