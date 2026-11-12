// Tracked state of one Vulkan device and the frame capture built from it.
#pragma once

#include "common.h"
#include "resources.h"
#include "pipeline_recipe.h"
#include "pacing.h"
#include "work.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace efvk {

class M2Backend;
class CharacterBackend;
struct PresentWork;
struct CommandOwner;
struct CharacterModule;

// Device functions the layer intercepts and tracks. One next-pointer is kept per
// name; promoted functions and their extension aliases are separate entries
// because a driver may offer only one of them.
#define EFVK_TRACKED_FUNCS(X)                                                                                          \
    X(DestroyDevice) X(QueuePresentKHR)                                                                                \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)                                              \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)                                                        \
    X(MapMemory2) X(MapMemory2KHR) X(UnmapMemory2) X(UnmapMemory2KHR)                                                  \
    X(CreateBuffer) X(DestroyBuffer) X(BindBufferMemory) X(BindBufferMemory2) X(BindBufferMemory2KHR)                  \
    X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline)                                            \
    X(CreateRenderPass) X(CreateRenderPass2) X(CreateRenderPass2KHR) X(DestroyRenderPass)                               \
    X(CreateFramebuffer) X(DestroyFramebuffer)                                                                         \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateImage) X(DestroyImage)                                        \
    X(CreateImageView) X(DestroyImageView) X(CreateBufferView) X(DestroyBufferView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)        \
    X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
    X(UpdateDescriptorSetWithTemplate) X(UpdateDescriptorSetWithTemplateKHR)                                           \
    X(CreateDescriptorUpdateTemplate) X(CreateDescriptorUpdateTemplateKHR)                                             \
    X(DestroyDescriptorUpdateTemplate) X(DestroyDescriptorUpdateTemplateKHR)                                           \
    X(CmdPushDescriptorSet) X(CmdPushDescriptorSetKHR)                                                                 \
    X(CmdPushDescriptorSetWithTemplate) X(CmdPushDescriptorSetWithTemplateKHR)                                         \
    X(CmdPushDescriptorSet2) X(CmdPushDescriptorSet2KHR)                                                               \
    X(CmdPushDescriptorSetWithTemplate2) X(CmdPushDescriptorSetWithTemplate2KHR)                                       \
    X(CmdSetDescriptorBufferOffsets2EXT) X(CmdBindDescriptorBufferEmbeddedSamplersEXT)                                \
    X(CmdBindDescriptorBufferEmbeddedSamplers2EXT)                                                                    \
    X(CmdSetDescriptorBufferOffsetsEXT) X(CmdBindDescriptorSets2) X(CmdBindDescriptorSets2KHR)                         \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(DestroyCommandPool) X(ResetCommandPool)                          \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer)                                                    \
    X(CmdBindPipeline) X(CmdBindIndexBuffer) X(CmdBindIndexBuffer2) X(CmdBindIndexBuffer2KHR)                          \
    X(CmdBindVertexBuffers) X(CmdBindVertexBuffers2) X(CmdBindVertexBuffers2EXT) X(CmdBindDescriptorSets)              \
    X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect)                                          \
    X(CmdDrawIndirectCount) X(CmdDrawIndirectCountKHR) X(CmdDrawIndexedIndirectCount) X(CmdDrawIndexedIndirectCountKHR) \
    X(CmdDispatch) X(CmdDispatchIndirect)                                                                              \
    X(CmdCopyBuffer) X(CmdCopyBuffer2) X(CmdCopyBuffer2KHR) X(CmdUpdateBuffer) X(CmdFillBuffer)                        \
    X(CmdCopyBufferToImage) X(CmdCopyBufferToImage2) X(CmdCopyBufferToImage2KHR)                                       \
    X(CmdBeginRenderPass) X(CmdBeginRenderPass2) X(CmdBeginRenderPass2KHR)                                             \
    X(CmdEndRenderPass) X(CmdEndRenderPass2) X(CmdEndRenderPass2KHR)                                                   \
    X(CmdNextSubpass) X(CmdNextSubpass2) X(CmdNextSubpass2KHR)                                                          \
    X(CmdBeginRendering) X(CmdBeginRenderingKHR) X(CmdEndRendering) X(CmdEndRenderingKHR)                              \
    X(CmdSetViewport) X(CmdSetScissor)                                                                                 \
    X(CmdExecuteCommands)                                                                                              \
    X(QueueSubmit) X(QueueSubmit2) X(QueueSubmit2KHR)

// Device functions that are only counted and passed on. They show which paths an
// application uses (descriptor updates, images, shaders, uploads) before tracking
// for them exists.
#define EFVK_COUNTED_FUNCS(X)                                                                                          \
    X(CreateDescriptorPool)                                                                                           \
    X(CmdBindDescriptorBuffersEXT) X(CmdPushConstants)                                                                 \
    X(CmdCopyImageToBuffer) X(CmdBlitImage) X(FlushMappedMemoryRanges)                                                  \
    X(CmdDrawMultiEXT) X(CmdDrawMultiIndexedEXT) X(CmdDrawMeshTasksEXT) X(CmdBindTransformFeedbackBuffersEXT)          \
    X(CmdBuildAccelerationStructuresKHR) X(CmdTraceRaysKHR)                                                           \
    X(WaitForFences) X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(QueueWaitIdle) X(DeviceWaitIdle)                 \
    X(WaitSemaphores) X(WaitSemaphoresKHR)

#define EFVK_DEVICE_FUNCS(X) EFVK_TRACKED_FUNCS(X) EFVK_COUNTED_FUNCS(X)

struct DeviceDispatch {
#define X(n) PFN_vk##n n = nullptr;
    EFVK_DEVICE_FUNCS(X)
#undef X
};

// Index of every intercepted device function, for the call counters.
enum HookIndex {
#define X(n) HI_##n,
    EFVK_DEVICE_FUNCS(X)
#undef X
    HI_COUNT
};

extern std::atomic<uint64_t> g_api_calls[HI_COUNT];
extern const char* const g_api_names[HI_COUNT];

// Every thread's last entry into the layer, for the stall watchdog: the hook, when, and whether the
// thread is still inside a hook that can wait (submission, presentation, fence and idle waits).
// Each slot on cache lines of its own: the thread writes it at every hook, and a neighbour's writes
// to a shared line would cost both threads the line at every call.
struct alignas(64) ThreadTrace {
    std::atomic<uint32_t> thread{0};
    std::atomic<int> hook{-1};
    std::atomic<uint64_t> since_us{0};
    std::atomic<int> inside{0};
    std::atomic<const char*> stage{nullptr}; // what the thread does inside the layer (a static text)
    // The thread's calls per hook, written by the thread alone (no locked add, no shared line); the
    // probe sums every slot and g_api_calls, which counts the threads without a slot.
    std::atomic<uint64_t> calls[HI_COUNT] = {};
};
// Every thread's calls of a hook so far.
uint64_t api_calls(int index);
ThreadTrace* thread_trace(); // this thread's slot; null when every slot is taken
// Names what the thread does from here to the end of the scope, for the stall watchdog.
struct StageMark {
    ThreadTrace* trace;
    const char* previous = nullptr;
    StageMark(ThreadTrace* t, const char* stage) : trace(t) { if (trace) previous = trace->stage.exchange(stage, std::memory_order_relaxed); }
    ~StageMark() { if (trace) trace->stage.store(previous, std::memory_order_relaxed); }
    StageMark(const StageMark&) = delete;
    StageMark& operator=(const StageMark&) = delete;
};

inline void count_call(int index)
{
    if (ThreadTrace* trace = thread_trace()) {
        auto& calls = trace->calls[index];
        calls.store(calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        trace->hook.store(index, std::memory_order_relaxed);
        trace->since_us.store(host_time_us(), std::memory_order_relaxed);
    } else {
        g_api_calls[index].fetch_add(1, std::memory_order_relaxed);
    }
}
// This thread is inside the application's vkQueuePresentKHR (BUG-062).
inline bool in_presentation()
{
    ThreadTrace* const t = thread_trace();
    return t && t->inside.load(std::memory_order_relaxed) > 0 && t->hook.load(std::memory_order_relaxed) == HI_QueuePresentKHR;
}
struct HookInside {
    ThreadTrace* trace = thread_trace();
    HookInside() { if (trace) trace->inside.fetch_add(1, std::memory_order_relaxed); }
    ~HookInside() { if (trace) trace->inside.fetch_sub(1, std::memory_order_relaxed); }
    HookInside(const HookInside&) = delete;
    HookInside& operator=(const HookInside&) = delete;
};
// A presentation returned to the application (the watchdog's clock).
void note_presented();
// A semaphore an owned presenter is done with, destroyed by destroy_retired once later
// presentations passed (`all`: every one, at device destruction). The first is inline: the
// replacement module calls it without a host function.
inline void retire_semaphore(Device* dev, VkSemaphore semaphore, PFN_vkDestroySemaphore destroy);
void destroy_retired(Device*, bool all);

// Remembers a function name the application asked vkGetDeviceProcAddr for.
void note_resolved(const char* name);

// Sorted, disjoint byte ranges.
struct IntervalSet {
    struct Range {
        uint64_t off, end;
    };
    std::vector<Range> v;

    void add(uint64_t off, uint64_t size);
    void remove(uint64_t off, uint64_t size);
    bool covers(uint64_t off, uint64_t size) const;
    uint64_t total() const;
};

// A swap chain as created: the facts owned presentation work needs (D-072).
struct SwapchainInfo {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    HWND window = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {};
    VkImageUsageFlags usage = 0;
    VkSwapchainCreateFlagsKHR flags = 0;
    uint32_t generation = 0; // counts recreations of this device's swap chains
    bool usage_added = false; // color attachment use added by the layer in character mode
    std::vector<VkImage> images;
};

struct MemInfo {
    uint64_t size = 0;
    uint32_t type_index = 0;
    uint8_t* mapped = nullptr;
    uint64_t map_off = 0;
    uint64_t map_size = 0;
    bool write_combined = false; // OS page policy of the current mapped range
    std::vector<VkBuffer> buffers;
};

struct BufInfo {
    uint32_t id = 0;
    uint64_t size = 0;
    uint32_t usage = 0;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t mem_off = 0;
    // CPU copy of what tracked writes put into the buffer; `known` says which bytes are valid.
    std::vector<uint8_t> shadow;
    IntervalSet known;
    uint64_t gen = 0;
    uint32_t unknown_writes = 0;
    // The last writes into the buffer by range (D-099): a region's generation is the newest write
    // that overlaps it, so a copy into one part of a pool buffer does not make every region in it
    // hash again. Writes older than the ring holds are forgotten and `gen_floor` rises to them.
    struct WriteRange { uint64_t off = 0, size = 0, gen = 0; };
    static constexpr uint32_t kWriteRanges = 32;
    std::array<WriteRange, kWriteRanges> writes = {};
    uint32_t write_count = 0, write_next = 0;
    uint64_t gen_floor = 0;
    void note_write(uint64_t off, uint64_t size)
    {
        ++gen;
        if (write_count >= kWriteRanges) gen_floor = writes[write_next].gen;
        writes[write_next] = {off, size, gen};
        write_next = (write_next + 1) % kWriteRanges;
        if (write_count < kWriteRanges) ++write_count;
    }
    uint64_t region_gen(uint64_t off, uint64_t size) const
    {
        uint64_t result = gen_floor;
        const uint32_t count = write_count < kWriteRanges ? write_count : kWriteRanges;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& w = writes[i];
            if (w.off < off + size && off < w.off + w.size && w.gen > result) result = w.gen;
        }
        return result;
    }
};

struct PipeInfo {
    struct Binding {
        uint32_t binding, stride, per_instance;
    };
    struct Attribute {
        uint32_t location, binding, format, offset;
    };
    uint32_t id = 0;
    bool compute = false;
    uint32_t topology = 0;
    uint32_t stages = 0;
    std::vector<Binding> bindings;
    std::vector<Attribute> attributes;
    std::vector<ShaderStage> shaders;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    // What a copy of the pipeline would have to reproduce besides the above.
    uint32_t create_flags = 0;
    uint32_t subpass = 0;
    bool specialized = false; // a stage has specialization constants
    std::vector<uint32_t> dynamic_states;
    std::shared_ptr<const GraphicsRecipe> recipe;
};

// Immutable attachment facts. Recording owns metadata only, never application Vulkan objects.
struct TargetView {
    bool known = false;
    VkImageView handle = VK_NULL_HANDLE; // the application's view, alive while its framebuffer is
    ImageViewInfo view;
    ImageInfo image;
};
struct FramebufferInfo {
    uint32_t id = 0, width = 0, height = 0, layers = 0, attachment_count = 0;
    bool known = false, imageless = false;
    std::vector<TargetView> views;
};
struct RenderTargets {
    bool known = false, dynamic = false;
    std::string reason;
    VkRect2D area{};
    uint32_t subpass = 0, layers = 0, view_mask = 0, flags = 0;
    std::shared_ptr<const FramebufferInfo> framebuffer;
    std::shared_ptr<const RenderPassRecipe> recipe;
    // Imageless and dynamic attachments are supplied at Begin, not CreateFramebuffer.
    std::vector<TargetView> views;
    std::vector<VkRenderingAttachmentInfo> dynamic_colors;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool has_depth = false, has_stencil = false;
};

void on_create_framebuffer(Device*, VkFramebuffer, const VkFramebufferCreateInfo*);
void on_destroy_framebuffer(Device*, VkFramebuffer);
void on_begin_targets(Device*, CmdBuf*, const VkRenderPassBeginInfo*);
void on_begin_targets(Device*, CmdBuf*, const VkRenderingInfo*);
// The classic render pass instance a recording is in (CmdBuf::pass, D-113); dynamic rendering has none.
void on_begin_pass(Device*, CmdBuf*, const VkRenderPassBeginInfo*);
void on_begin_pass(Device*, CmdBuf*, const VkRenderingInfo*);
void on_next_targets(Device*, CmdBuf*);
void write_render_targets(JsonWriter&, const RenderTargets&);

enum DrawKind : uint8_t {
    DK_DRAW = 0,
    DK_INDEXED = 1,
    DK_INDIRECT = 2,
    DK_INDEXED_INDIRECT = 3,
    DK_DISPATCH = 4,
    DK_DISPATCH_INDIRECT = 5,
};

enum ReadSrc : uint8_t {
    SRC_NONE = 0,   // the bytes are not available on the CPU
    SRC_MAPPED = 1, // read from the application's own memory mapping
    SRC_SHADOW = 2, // read from the layer's shadow copy
};

struct VbBind {
    VkBuffer buf = VK_NULL_HANDLE;
    uint64_t off = 0;
};

struct SetBind {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t id = 0; // distinguishes a set from a later allocation with the same handle
    uint32_t dyn_first = 0;
    uint32_t dyn_count = 0;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    // The handle the set was bound with; alive at least while the command buffer is being recorded.
    VkPipelineLayout layout_handle = VK_NULL_HANDLE;
    std::shared_ptr<const DescriptorSetInfo> pushed; // values at record time, retained by each draw
    const char* unsupported = nullptr;
};

constexpr uint32_t kMaxVb = 4;
constexpr uint32_t kMaxSets = 4;

// One draw or dispatch as it was recorded. Handles are resolved to ids at submit time.
struct DrawRec {
    uint8_t kind = 0;
    uint8_t index_size = 0;
    uint32_t count = 0;      // index count, vertex count, or draw count of an indirect call
    uint32_t instances = 0;
    uint32_t first = 0;      // first index or first vertex
    int32_t vertex_offset = 0;
    uint32_t first_instance = 0;
    uint32_t cb_id = 0;      // command buffer the call was recorded into
    uint32_t tid = 0;        // thread that recorded it
    uint32_t rp = 0;         // ordinal of the render pass inside the submitted command buffer, 0 when outside
    uint32_t targets = 0;    // one-based immutable render-target context, 0 outside/unobserved
    VkPipeline pipeline = VK_NULL_HANDLE;
    // What the driver has bound for the draw: `pipeline`, or its twin without the character dither
    // (D-133). Code that binds its own pipeline in between binds this one back.
    VkPipeline bound = VK_NULL_HANDLE;
    VkBuffer ib = VK_NULL_HANDLE;
    uint64_t ib_off = 0;
    VkBuffer indirect = VK_NULL_HANDLE;
    uint64_t indirect_off = 0;
    VbBind vb[kMaxVb];
    SetBind sets[kMaxSets];
};

// What a capture is asked to keep of the buffers its draws read.
struct SnapRequest {
    bool all = false;             // every indexed draw; otherwise the draws whose region hash is listed
    std::vector<uint32_t> hashes; // sorted
    uint64_t max_range = 65536;   // a uniform range above this size is listed but not read
    uint64_t budget = 32ull << 20; // bytes one capture may keep in the observed process
    // Follow: read `follow_count` 32-bit values at byte `follow_offset` of the uniform range bound at
    // (follow_set, follow_binding), and read `follow_bytes` at value * follow_stride of every storage
    // range of the draw that such a window fits in.
    bool follow = false;
    uint32_t follow_set = 0, follow_binding = 0, follow_offset = 0, follow_count = 0, follow_stride = 0;
    uint64_t follow_bytes = 0;
    // A range that large usually holds entries of many draws. The first `follow_entry` bytes are
    // this draw's own entry and are read and compared separately; 0: not wanted.
    uint64_t follow_entry = 0;
};

enum SnapKind : uint8_t {
    SNAP_UNIFORM = 0, // a uniform buffer range
    SNAP_FOLLOW = 1,  // a window of a storage range, located by a value read from a uniform range
    SNAP_ENTRY = 2,   // the first bytes of the uniform range the values are read from, compared on their own
};

// One range of a draw as the layer read it when the draw was recorded: identity and checksum only.
struct RecRange {
    uint8_t kind = SNAP_UNIFORM;
    uint8_t set = 0;
    uint8_t src = SRC_NONE;
    uint32_t binding = 0, element = 0, word = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t off = 0, size = 0;
    uint32_t crc = 0;
};

struct RecSnap {
    uint32_t draw = 0; // position in CmdBuf::draws
    std::vector<RecRange> ranges;
};

// A copy from a buffer to mip 0, layer 0 of an image, as recorded.
struct PendingImageWrite {
    VkImage dst = VK_NULL_HANDLE;
    VkBuffer src = VK_NULL_HANDLE;
    uint64_t src_off = 0;
    uint32_t row_length = 0, image_height = 0; // in texels; 0 means the extent of the copy
    int32_t ox = 0, oy = 0, oz = 0;
    uint32_t w = 0, h = 0, d = 0;
};

struct PendingWrite {
    VkBuffer dst = VK_NULL_HANDLE;
    uint64_t dst_off = 0;
    uint64_t size = 0;
    VkBuffer src = VK_NULL_HANDLE; // null for vkCmdUpdateBuffer / vkCmdFillBuffer
    uint64_t src_off = 0;
    bool have_data = false;
    std::vector<uint8_t> data;
};

struct CmdBuf {
    VkCommandBuffer handle = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t id = 0;
    bool secondary = false;
    uint32_t tid = 0;
    bool m2_extended_bind = false; // the initial replacement cannot reproduce Bind*2 size/stride state
    // The work of each replacement backend recorded into this command buffer (several character
    // packages share one recording), kept until a legal recording reset/free after GPU completion.
    std::vector<std::shared_ptr<CommandOwner>> owners;
    uint64_t character_frame = 0;  // the character backend's number for this recording; 0: none yet

    // Current bindings while recording. `gfx_bound` is what the driver has (DrawRec::bound).
    VkPipeline gfx_pipeline = VK_NULL_HANDLE;
    VkPipeline gfx_bound = VK_NULL_HANDLE;
    VkPipeline comp_pipeline = VK_NULL_HANDLE;
    VkBuffer ib = VK_NULL_HANDLE;
    uint64_t ib_off = 0;
    uint8_t index_size = 0;
    VbBind vb[kMaxVb];
    SetBind sets[kMaxSets];
    bool in_rp = false;
    uint32_t active_targets = 0;
    std::vector<RenderTargets> target_contexts;
    // The first viewport and scissor of the dynamic state (D-113): a draw the replacement records
    // again at the end of its render pass (a translucent capture) sets them as they were.
    VkViewport viewport{};
    VkRect2D scissor{};
    bool has_viewport = false, has_scissor = false;
    // The classic render pass instance being recorded (D-113), kept until the module's pass end call
    // after the application's vkCmdEndRenderPass. `serial` counts the instances of this recording.
    struct PassInstance {
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        std::shared_ptr<const FramebufferInfo> info;
        std::shared_ptr<const RenderPassRecipe> recipe;
        VkRect2D area{};
        uint32_t serial = 0;
        size_t first_draw = 0; // the first of `draws` recorded inside the instance
    } pass;
    SetBind comp_sets[kMaxSets];

    // What was recorded since the last begin / reset.
    std::vector<DrawRec> draws;
    std::vector<uint32_t> dyn;
    std::vector<PendingWrite> writes;
    std::vector<RecSnap> rec_snaps; // record-time reads of a capture that keeps buffer contents, by draw
    std::vector<PendingImageWrite> image_writes;
    std::vector<uint32_t> threads; // threads that recorded into this buffer or into executed secondaries
    uint32_t rp_count = 0;
    uint32_t dyn_rp_count = 0;
    uint32_t secondaries = 0;
    uint32_t copies = 0;

    void reset_recording();
};

struct FrameStats {
    uint64_t frame = 0;
    uint32_t submits = 0;
    uint32_t command_buffers = 0;
    uint32_t secondaries = 0;
    uint32_t render_passes = 0;
    uint32_t dynamic_render_passes = 0;
    uint32_t draws = 0;
    uint32_t indexed = 0;
    uint32_t indirect = 0;
    uint32_t dispatches = 0;
    uint32_t buffer_copies = 0;
    uint32_t unknown_writes = 0;
    uint32_t max_draws_per_cb = 0;
    std::vector<uint32_t> threads;
};

enum RecState : uint8_t {
    REC_NONE = 0,       // the draw was not read at record time (recorded before the capture began, or not selected then)
    REC_EQUAL = 1,      // same range, same bytes at record and at submit
    REC_DIFFERENT = 2,  // same range, the bytes changed between record and submit
    REC_OTHER = 3,      // the slot held another buffer range when the draw was recorded
    REC_UNREADABLE = 4, // the bytes were not CPU-visible at one of the two times
};

// One range of a selected draw as it is when its command buffer is submitted.
struct DrawSnap {
    uint8_t kind = SNAP_UNIFORM;
    uint8_t set = 0;
    uint8_t src = SRC_NONE;
    uint8_t rec = REC_NONE;
    uint8_t rec_src = SRC_NONE;
    uint32_t binding = 0, element = 0, word = 0, value = 0;
    uint32_t buf_id = 0;
    uint64_t off = 0, size = 0;
    uint32_t crc = 0, rec_crc = 0;
    int32_t blob = -1;             // position in Capture::blobs; -1 when the bytes were not kept
    const char* skipped = nullptr; // why nothing was read
};

struct SnapBlob {
    uint32_t buf_id = 0;
    uint64_t off = 0, size = 0;
    uint8_t src = SRC_NONE;
    uint32_t crc = 0;
    std::vector<uint8_t> data;
};

struct CapDraw {
    DrawRec r;
    uint32_t submit = 0;
    bool snap_selected = false;
    std::vector<DrawSnap> snaps;
    uint32_t ib_id = 0;
    uint32_t pipe_id = 0;
    uint32_t indirect_id = 0;
    uint32_t vb_id[kMaxVb] = {};
    uint32_t hash = 0;
    uint8_t hash_src = SRC_NONE;
    bool hash_valid = false;
    std::vector<uint32_t> dyn[kMaxSets];
    std::string descriptors[kMaxSets]; // immutable JSON snapshot taken at submit
};

struct Capture {
    std::vector<RenderTargets> target_contexts;
    uint64_t seq = 0;
    bool wait_draws = false;
    uint64_t skipped_empty_intervals = 0;
    // Descriptor contents are JSON text per draw and set. The budget bounds what one capture
    // may add to the memory of the observed process; past it, contents are omitted and counted.
    uint64_t descriptor_budget = 0;
    uint64_t descriptor_bytes = 0;
    uint32_t descriptors_omitted = 0;
    FrameStats stats;
    std::vector<CapDraw> draws;
    // Buffer contents kept at submit for the selected draws. One blob per distinct (buffer, offset,
    // size); `snap->budget` bounds their total, past it a range is still checksummed but not kept.
    std::shared_ptr<const SnapRequest> snap;
    std::vector<SnapBlob> blobs;
    std::map<std::tuple<uint32_t, uint64_t, uint64_t>, int32_t> blob_index;
    uint64_t snap_bytes = 0;
    uint32_t snap_omitted = 0;
    uint32_t snap_draws = 0;
    uint32_t rec_counts[5] = {};
};

struct Device {
    // The layer's per-thread trace (thread_trace), for code that runs in a replacement module.
    ThreadTrace* (*trace)() = nullptr;
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    // Stands for every device that was created before the DLL attached: calls are
    // forwarded through the original exports and nothing is tracked.
    bool passive = false;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    // What the layer's own code (owned GPU work, uploads, bones, translucency, the replacement module) gets
    // its device functions from: the next layer's, except the waits for the GPU, which count a wait made
    // inside vkQueuePresentKHR (BUG-062). 3DMigoto never waits for the GPU in Present (its F10 only
    // releases objects, the D3D11 runtime destroys them once the GPU is done); neither may the layer: work
    // a queue holds behind the presentation can complete only after it went.
    PFN_vkGetDeviceProcAddr owned_gdpa = nullptr;
    std::atomic<uint64_t> present_waits{0};
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    DeviceDispatch d;
    std::shared_ptr<M2Backend> m2;
    // The character backend in use and the functions of the code it belongs to. Calls into it hold
    // character_mx shared; exchanging it (m2_backend.cpp) takes the lock exclusively. `staged` is a
    // backend of other code that is getting ready to take over.
    bool character_mode = false; // set once at device creation
    bool character_stopped = false; // the device is going: no backend is staged any more
    std::shared_mutex character_mx;
    std::shared_ptr<CharacterBackend> character, character_staged;
    const CharacterModule* character_module = nullptr;
    const CharacterModule* staged_module = nullptr;
    uint64_t character_recordings = 0; // frames of the character, counted across backends
    // Semaphores the application signalled from a queue submission (D-127): a batch's wait for one of
    // them orders it after the other queue's work, so the stages of the work the layer puts at the front
    // of that batch are added to the wait. A semaphore of an image acquisition is left as it is.
    std::mutex queue_signal_mx;
    std::unordered_set<VkSemaphore> queue_signalled;
    std::atomic<uint64_t> prefixed_batches{0}, widened_waits{0}, prefix_alone{0};
    VkPhysicalDeviceProperties physical_properties = {};
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    // Queues in one unprotected graphics family. Uploads use the first active queue;
    // one-time semaphore waits publish immutable images to every other graphics queue.
    std::vector<VkQueue> material_queues;
    uint32_t material_family = 0;
    std::string material_queue_reason;
    // The layer's own upload queue (D-103): a queue of a transfer-capable family the application
    // did not take, requested at device creation. The characters' textures and meshes upload there,
    // never waiting for the application's submissions, so a draw can wait for one of them.
    VkQueue upload_queue = VK_NULL_HANDLE;
    uint32_t upload_family = UINT32_MAX;
    VkQueueFlags upload_flags = 0;
    std::mutex upload_mx; // its submissions, one at a time
    bool material_bc_enabled = false;
    PFN_vkGetPhysicalDeviceFormatProperties material_format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties material_image_properties = nullptr;
    // VK_EXT_memory_budget of the physical device, for the character packages' activation (D-092).
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory_properties2 = nullptr;
    bool memory_budget = false;

    char gpu_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    std::vector<std::string> extensions; // enabled at creation
    uint32_t api_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    uint32_t max_bound_descriptor_sets = 0;
    uint32_t max_layout_sets = 0; // most sets any pipeline layout of the application has had
    // The API version the application asked for, and whether VK_EXT_robustness2 nullDescriptor is
    // on: added by the layer in character mode when supported and not requested, or the
    // application's own. Owned GPU work needs it for D3D11 null views (D-071).
    uint32_t app_api_version = 0;
    bool null_descriptors = false, null_descriptors_added = false;
    bool draw_parameters = false, draw_parameters_added = false; // shaderDrawParameters, as for null descriptors
    // D-095: timeline semaphores (texture publication), VK_EXT_memory_priority and
    // VK_EXT_pageable_device_local_memory (the layer's textures page before the application's).
    bool timeline_semaphores = false, timeline_semaphores_added = false;
    bool memory_priority = false, memory_priority_added = false;
    bool pageable_memory = false, pageable_memory_added = false;
    // What owned GPU work may rely on: the core features the application enabled and, with core
    // 1.2 or VK_KHR_shader_float_controls enabled, the float control properties.
    VkPhysicalDeviceFeatures enabled_features = {};
    bool float_controls = false;
    VkPhysicalDeviceFloatControlsProperties float_properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    // Vulkan 1.3 (texelBufferAlignment always on): texel-buffer view offsets as these say (D-151).
    bool texel_alignment = false;
    VkPhysicalDeviceTexelBufferAlignmentProperties texel_alignment_properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties2 = nullptr;

    // Presentation: the swap chains of this device and the window behind each.
    std::mutex swap_mx;
    std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchains;
    // Semaphores owned presenters are done with (D-098): a presentation queued with one as its wait
    // may still be pending when the presenter stops, so they are destroyed on the presenting thread
    // a few presentations later.
    struct RetiredSemaphore { VkSemaphore semaphore; PFN_vkDestroySemaphore destroy; uint64_t frame; };
    std::mutex retired_mx;
    std::vector<RetiredSemaphore> retired_semaphores;

    // Memory, buffers, pipelines. Readers of mapped memory hold the lock shared;
    // unmap, free and destroy take it exclusively before the call goes down.
    std::shared_mutex res_mx;
    std::unordered_map<VkDeviceMemory, MemInfo> mems;
    std::unordered_map<VkBuffer, BufInfo> bufs;
    std::unordered_map<uint32_t, VkBuffer> buf_by_id;
    std::unordered_map<VkPipeline, PipeInfo> pipes;
    std::unordered_map<uint32_t, VkPipeline> pipe_by_id;
    std::unordered_map<VkRenderPass, std::shared_ptr<const RenderPassRecipe>> render_passes;
    std::unordered_map<VkFramebuffer, std::shared_ptr<const FramebufferInfo>> framebuffers;
    // Attachment snapshots are enabled only for a requested probe capture, not ordinary rendering.
    std::atomic<bool> capture_targets{false};
    uint32_t next_buf_id = 1;
    uint32_t next_pipe_id = 1;
    uint32_t next_resource_id = 1;
    std::unordered_map<VkShaderModule, std::shared_ptr<const ShaderInfo>> shaders;
    // Route 2 (docs/vulkan-native-mods.md): the character module wants every draw (a ShaderOverride names a SPIR-V
    // module, or hunting is enabled); set by the module on the presenting thread, read at every draw.
    std::atomic<bool> shader_draws{false};
    std::unordered_map<uint32_t, std::weak_ptr<const ShaderInfo>> shader_by_id;
    std::shared_ptr<std::atomic<uint64_t>> shader_bytes = std::make_shared<std::atomic<uint64_t>>(0); // SPIR-V kept
    uint64_t shaders_not_retained = 0; // modules whose code was dropped because the budget was exhausted
    // Uploads to mip 0, layer 0 by outcome (ImageContent), bytes hashed and time spent on it.
    std::atomic<uint64_t> image_uploads[IMG_STATES] = {};
    std::atomic<uint64_t> image_hash_bytes{0};
    std::atomic<uint64_t> image_hash_us{0};
    // How often graphics pipeline creation used a state, a flag or a chained structure.
    uint64_t graphics_pipelines_created = 0;
    std::map<std::string, uint64_t> pipeline_uses;
    std::unordered_map<VkImage, ImageInfo> images;
    std::unordered_map<VkImageView, ImageViewInfo> image_views;
    std::unordered_map<VkBufferView, BufferViewInfo> buffer_views;
    std::unordered_map<VkSampler, SamplerInfo> samplers;
    std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<const DescriptorLayoutInfo>> set_layouts;
    std::unordered_map<VkPipelineLayout, std::shared_ptr<const PipelineLayoutInfo>> pipeline_layouts;
    std::unordered_map<VkDescriptorSet, DescriptorSetInfo> descriptor_sets;
    // Sets of each pool, so that a pool reset does not have to look at every set of the device.
    std::unordered_map<VkDescriptorPool, std::unordered_set<VkDescriptorSet>> pool_sets;
    std::unordered_map<VkDescriptorUpdateTemplate, DescriptorTemplateInfo> descriptor_templates;

    // Command buffers. The content of one is touched only by the thread that
    // records it, or by the submitting thread once recording has ended.
    std::shared_mutex cb_mx;
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CmdBuf>> cbs;
    std::atomic<uint64_t> cb_epoch{1};
    uint32_t next_cb_id = 1;

    // The frame being assembled and the capture state machine.
    std::mutex frame_mx;
    std::mutex present_mx; // the presenting threads: one presentation's work at a time (on_present)
    FrameStats cur;
    FrameStats last;
    PresentPacing pacing; // bounded host intervals; protected by frame_mx
    TrackerWork work; // optional tracker timings; its bounded slow history has a separate lock
    // What happened to mapped buffers when their memory was unmapped: copied (staging), passed over
    // although an earlier version would have copied them, and shadows dropped because the
    // application could have written through the mapping unseen.
    std::atomic<uint64_t> unmap_taken{0}, unmap_taken_bytes{0};
    std::atomic<uint64_t> unmap_passed{0}, unmap_passed_bytes{0};
    std::atomic<uint64_t> unmap_invalidated{0};
    uint32_t max_threads = 0;
    uint32_t max_command_buffers = 0;
    uint32_t max_submits = 0;
    uint32_t max_secondaries = 0;
    uint64_t frame_no = 0;
    uint64_t total_draws = 0;
    int cap_state = 0; // 0 idle, 1 waiting for the next present, 2 recording this frame
    bool cap_wait_draws = false; // optional wait across intervals without graphics; never merges frames
    uint64_t cap_descriptor_budget = 0; // of the capture that is armed
    std::shared_ptr<const SnapRequest> cap_snap; // of the capture that is armed; null: keep no contents
    // The request of the frame being captured, for the threads that record draws. The flag is the
    // cheap test on the draw path; the pointer is read with std::atomic_load only when it is set.
    std::atomic<bool> snap_recording{false};
    std::shared_ptr<const SnapRequest> snap_live;
    std::shared_ptr<Capture> cap_building;
    std::shared_ptr<Capture> cap_last;
    uint64_t cap_seq = 0;
    HANDLE cap_event = nullptr;
};
inline void retire_semaphore(Device* dev, VkSemaphore semaphore, PFN_vkDestroySemaphore destroy)
{
    if (!semaphore || !destroy) return;
    std::lock_guard<std::mutex> lock(dev->retired_mx);
    dev->retired_semaphores.push_back({semaphore, destroy, dev->frame_no});
}

// Device registry.
Device* dev_create(VkDevice handle);
// The window of a Win32 surface created through this layer, or null.
HWND surface_window(VkSurfaceKHR surface);
void dev_destroy(Device* dev);
void dev_set_passive(Device* dev);
Device* dev_of_key(void* key);
template <typename T> inline Device* dev_of(T handle)
{
    return dev_of_key(*reinterpret_cast<void**>(handle));
}

// The device the probe answers for, kept alive while `DeviceRef` exists.
struct DeviceRef {
    Device* dev = nullptr;
    std::shared_lock<std::shared_mutex> lock;
    DeviceRef();
};

CmdBuf* cb_of(Device* dev, VkCommandBuffer cb);

// Resources.
void on_alloc_memory(Device* dev, VkDeviceMemory mem, const VkMemoryAllocateInfo* info);
void on_free_memory(Device* dev, VkDeviceMemory mem);
void on_map_memory(Device* dev, VkDeviceMemory mem, uint64_t off, uint64_t size, void* data);
void on_unmap_memory(Device* dev, VkDeviceMemory mem);
void on_create_buffer(Device* dev, VkBuffer buf, const VkBufferCreateInfo* info);
void on_destroy_buffer(Device* dev, VkBuffer buf);
void on_bind_buffer(Device* dev, VkBuffer buf, VkDeviceMemory mem, uint64_t off);
void on_create_graphics_pipeline(Device* dev, VkPipeline pipe, const VkGraphicsPipelineCreateInfo* info);
void on_create_compute_pipeline(Device* dev, VkPipeline pipe, const VkComputePipelineCreateInfo* info);
void on_destroy_pipeline(Device* dev, VkPipeline pipe);

// Command buffers.
void on_alloc_cbs(Device* dev, const VkCommandBufferAllocateInfo* info, const VkCommandBuffer* cbs);
void on_free_cbs(Device* dev, uint32_t count, const VkCommandBuffer* cbs);
void on_destroy_pool(Device* dev, VkCommandPool pool);
void on_reset_pool(Device* dev, VkCommandPool pool);
void on_copy_buffer(Device* dev, CmdBuf* cb, VkBuffer src, VkBuffer dst, uint64_t src_off, uint64_t dst_off, uint64_t size);
void on_update_buffer(Device* dev, CmdBuf* cb, VkBuffer dst, uint64_t off, uint64_t size, const void* data);
void on_copy_buffer_to_image(Device* dev, CmdBuf* cb, VkBuffer src, VkImage dst, uint64_t src_off, uint32_t row_length,
                             uint32_t image_height, const VkImageSubresourceLayers& layers, VkOffset3D offset,
                             VkExtent3D extent);
void on_fill_buffer(Device* dev, CmdBuf* cb, VkBuffer dst, uint64_t off, uint64_t size, uint32_t value);
void on_execute_commands(Device* dev, CmdBuf* cb, uint32_t count, const VkCommandBuffer* cbs);
// Called after an indexed draw was appended to cb->draws: reads what a running capture wants kept.
void on_record_indexed(Device* dev, CmdBuf* cb);

// Frame assembly.
void on_submit(Device* dev, uint32_t count, const VkCommandBuffer* cbs);
void on_present(Device* dev, PresentWork* present = nullptr);

// Reads `size` bytes of a buffer. The caller holds res_mx (shared is enough).
ReadSrc read_buffer_locked(Device* dev, const BufInfo& b, uint64_t off, uint64_t size, uint8_t* out);

// Probe requests, called on the pipe thread.
std::string probe_status();
std::string probe_capture(uint32_t timeout_ms, bool with_draws, uint64_t descriptor_budget,
                          std::shared_ptr<const SnapRequest> snap = nullptr, bool wait_draws = false);
std::string probe_snapshot(uint64_t seq, uint32_t id, std::vector<uint8_t>& payload);
// Changes the unmap settings of the running process; a negative value leaves a setting as it is.
std::string probe_set(int unmap_snapshot_all, int64_t unmap_snapshot_limit, int gpu_bones = -1);
std::string probe_buffers(uint32_t usage_mask, uint64_t min_size, uint32_t limit);
std::string probe_read_buffer(uint32_t id, uint64_t off, uint64_t size, std::vector<uint8_t>& payload);
std::string probe_hash(uint32_t id, uint64_t off, uint64_t size);
std::string probe_pipeline(uint32_t id);
std::string probe_api_usage();
// Samples the threads that entered the layer for `milliseconds` at `hz` (sampler.cpp): their stacks as
// module and offset, the most frequent first.
std::string probe_profile(uint32_t milliseconds, uint32_t hz);
// The ids of the threads that entered the layer (the watchdog's slots), at most `max`.
size_t thread_trace_ids(DWORD* out, size_t max);

} // namespace efvk
