// The merged skeleton's import on the GPU (D-117, D-127): the game's bone matrices of a drawn
// component are copied into the object's merged skeleton by a compute dispatch, as the Core's own
// importer does, instead of the CPU reading them from write-combined mapped memory (about 30 us per
// matched draw in the game).
//
// As the original: one persistent merged skeleton per object (the Core's ResourceMergedSkeletonDataRW),
// written only by imports. An import belongs to the recording that drew the component and runs in
// that recording's batch, when the game's bone data it reads is valid; the game's buffer is never read
// at any other time (BUG-060). The recording's draws read a snapshot of the skeleton copied in the same
// batch after the batch's imports, the GPU form of the CPU path's master and per-draw snapshots.
//
// The work goes in a command buffer of ours at the front of the application's batch (the host puts it
// there): the batch's waits order it after what the game orders its own work after, on whichever queue
// the game chose. Nothing of ours waits or signals across queues (D-091); one timeline per queue,
// signalled after the application's batches, says when our command buffers and snapshots can be reused.
#pragma once

#include "state.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace efvk {

class BoneMerge;
struct BoneContext;

// A submission of ours as a command buffer or a snapshot remembers it: the queue it went to and the
// value that queue's timeline reaches when it completed.
struct BoneSubmission { uint32_t queue = UINT32_MAX; uint64_t value = 0; };

// Whether a recording is still recorded and not yet submitted (D-127): another recording may take its
// imports only then, while the game's data they read is valid (until that recording completed). One per
// command owner: the epoch counts its recordings, the low bit says the current one was submitted.
struct BoneRecording {
    std::atomic<uint64_t> state{2};
    uint64_t epoch() const { return state.load(std::memory_order_acquire) >> 1; }
    bool pending(uint64_t epoch) const
    {
        const uint64_t s = state.load(std::memory_order_acquire);
        return s >> 1 == epoch && !(s & 1);
    }
    void submitted() { state.fetch_or(1, std::memory_order_acq_rel); }
    void next() { state.store(((state.load(std::memory_order_acquire) >> 1) + 1) << 1, std::memory_order_release); }
};

// One object's merged skeleton on the device: slot 0 is the persistent skeleton (the implicit identity
// matrices, then two poses of `bones` bones), the other slots are snapshots of it, one per recording
// that draws the object. A snapshot is reused once its recording was reset after completion and the
// copy into it completed. Slots come in chunks of eight; another chunk is made when all are taken.
struct BoneRing {
    static constexpr uint32_t kSlotsPerChunk = 8, kChunks = 8;
    uint32_t bones = 0;
    VkDeviceSize slot_bytes = 0, slot_stride = 0;
    VkBuffer buffer(uint32_t slot) const { return chunks_[slot / kSlotsPerChunk].buffer; }
    VkDeviceSize offset(uint32_t slot) const { return slot_stride * (slot % kSlotsPerChunk); }
    // A snapshot slot for a recording, held for it; UINT32_MAX when every slot of every chunk is taken.
    uint32_t acquire();
    void release(uint32_t slot); // its recording was reset after completion
    // Components (bit c for component c < 64) whose import a submitted batch carried.
    std::atomic<uint64_t> submitted{0};
    ~BoneRing();
private:
    friend class BoneMerge;
    struct Chunk { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; };
    bool grow(); // caller holds mx
    std::shared_ptr<BoneContext> ctx;
    uint32_t memory_type = UINT32_MAX;
    std::atomic<uint64_t>* bytes = nullptr; // the context's count of ring memory
    std::mutex mx;
    Chunk chunks_[kChunks];
    std::atomic<uint32_t> chunk_count{0}; // chunks below are complete once counted (release)
    std::vector<uint8_t> held;
    std::vector<std::vector<BoneSubmission>> written; // per slot, the last copy into it on each queue
    bool initialized = false;                         // the persistent skeleton cleared and its implicit rows written
};

// A package's LOD remap tables as the dispatches read them: every component's table in one buffer.
struct BoneRemaps {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<uint32_t> offsets; // per table as given, in uints; UINT32_MAX for an empty one
    ~BoneRemaps();
private:
    friend class BoneMerge;
    std::shared_ptr<BoneContext> ctx;
};

// One component's import into an object's persistent skeleton, as the recording that drew it took it.
struct BoneJob {
    std::shared_ptr<BoneRing> ring;
    std::shared_ptr<BoneRemaps> remaps; // null without a remap
    uint32_t component = 0;
    // The remap's table among `remaps`; by default the component's (one table per component). A package
    // has one per component and LOD (BUG-093).
    uint32_t table = UINT32_MAX;
    VkBuffer source = VK_NULL_HANDLE;  // the game's bone buffer as bound at the draw
    uint32_t source_id = 0;            // the tracker's id of that buffer (a handle may be reused)
    VkDeviceSize source_offset = 0, source_size = 0;
    uint32_t rows[2] = {0, 0};        // the entry's offsets, rows into the source range
    uint32_t first = 0, count = 0;    // the component's merged range
    uint32_t poses = 0;               // bit 0 current, bit 1 previous
    bool remap = false;
    std::shared_ptr<BoneRecording> recording; // the recording that drew the component, and its epoch then
    uint64_t epoch = 0;
};

// A recording's snapshot: copied from the ring's persistent skeleton in the recording's batch.
struct BoneCopy { std::shared_ptr<BoneRing> ring; uint32_t slot = 0; };

// The work in front of one application batch, and the command buffer of ours it was recorded in.
struct BoneBatch {
    std::vector<BoneJob> jobs;
    std::vector<BoneCopy> copies;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    void clear() { jobs.clear(); copies.clear(); cb = VK_NULL_HANDLE; }
};

class BoneMerge {
public:
    BoneMerge(Device*, const VkPhysicalDeviceProperties&, const VkPhysicalDeviceMemoryProperties&);
    ~BoneMerge();
    bool usable() const { return usable_; }
    const std::string& reason() const { return reason_; }
    // A source range's offset must be a multiple of this to be bound to the dispatch.
    VkDeviceSize alignment() const { return storage_alignment_; }
    std::shared_ptr<BoneRing> ring(uint32_t bones, std::string& why);
    std::shared_ptr<BoneRemaps> remaps(const std::vector<std::vector<uint16_t>>& tables, std::string& why);
    // One application submission on `queue` (the caller passes the queue's own submission thread):
    // each batch with work gets a command buffer of ours (`cb`), which the host puts at the front of the
    // application's batch; the host then signals `timeline` with `value` after the application's batches
    // in the same call. False when no batch has work (nothing to signal).
    bool prepare(VkQueue, std::vector<BoneBatch>& batches, VkSemaphore& timeline, uint64_t& value);
    // After the application's call: whether it went to the queue. A failed call leaves nothing of ours
    // pending, and its value is taken again by the next call. A signal of another merge's timeline (the
    // other copy of the module during an exchange) is not this one's.
    void finish(VkQueue, VkSemaphore timeline, uint64_t value, bool submitted);
    // Counts kept by the backend's draws and submissions.
    void note_borrowed(uint64_t jobs) { borrowed_.fetch_add(jobs, std::memory_order_relaxed); }
    void status(JsonWriter&);
private:
    struct Slot { VkCommandBuffer cb = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; BoneSubmission last; };
    bool make_pipeline();
    bool record(Slot&, const BoneBatch& batch, std::string& why);
    void fail(const std::string& why);
    std::shared_ptr<BoneContext> ctx_;
    VkPhysicalDeviceMemoryProperties memory_;
    VkDeviceSize storage_alignment_ = 16;
    bool usable_ = false;
    std::string reason_;
    std::mutex mx_; // the submission path and the error text
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<Slot> slots_;
    uint64_t submissions_total_ = 0;
    // Status.
    std::atomic<uint64_t> jobs_{0}, dispatches_{0}, copies_{0}, borrowed_{0}, submissions_{0}, rings_{0}, slot_waits_{0}, failures_{0},
        failed_calls_{0};
    std::string error_;
};

} // namespace efvk
