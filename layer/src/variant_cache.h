// Bounded asynchronous pipeline creation. Requests never wait for the driver.
#pragma once

#include "pipeline_variant.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace efvk {

enum class VariantState { Queued, Building, Ready, Failed, Saturated, Stopped };
struct VariantResult {
    VariantState state = VariantState::Queued;
    std::shared_ptr<const PipelineVariant> variant;
    std::string reason;
};

class VariantCache {
public:
    // `pipeline_cache`: the device's variant pipeline cache the variants are created through (D-101);
    // `builders`: the threads building variants (the backend uses `builders_for_device()`).
    VariantCache(VkDevice, const VariantDispatch&, uint32_t max_sets, size_t capacity = 128,
                 const VkPhysicalDeviceLimits& = {}, VkPipelineCache pipeline_cache = VK_NULL_HANDLE, unsigned builders = 1);
    // Half the cores, two to four: the variants a character needs when it first appears compile in parallel.
    static unsigned builders_for_device();
    ~VariantCache();
    VariantCache(const VariantCache&) = delete;
    VariantCache& operator=(const VariantCache&) = delete;
    // Identity is a monotonically assigned pipeline generation, not a reusable VkPipeline handle.
    // Recipes are owned immutable objects. The same identity must always refer to the same source.
    VariantResult request(uint64_t identity, std::shared_ptr<const GraphicsRecipe>,
                          std::shared_ptr<const PipelineLayoutInfo>, const VertexInputRecipe&, const FragmentPatch& = {});
    // As request, for a variant nothing draws with yet (D-133: the state a key turns on): built when no
    // requested variant waits. A later request or await of it moves it ahead with the others.
    VariantResult prepare(uint64_t identity, std::shared_ptr<const GraphicsRecipe>,
                          std::shared_ptr<const PipelineLayoutInfo>, const VertexInputRecipe&, const FragmentPatch& = {});
    // As request, then waiting (bounded) while the variant is queued or building (D-103): the draw
    // takes longer, as a shader's creation does under DX11, and the character is never the game's
    // mesh meanwhile. Returns the final state, or the pending one when the wait ran out.
    VariantResult await(uint64_t identity, std::shared_ptr<const GraphicsRecipe>, std::shared_ptr<const PipelineLayoutInfo>,
                        const VertexInputRecipe&, const FragmentPatch&, std::chrono::milliseconds timeout);
    uint64_t waits() const { return waits_.load(); }
    // Finished variants the cache let go of when it was full (L-08): the least recently asked for first.
    uint64_t evictions() const { return evictions_.load(); }
    uint64_t wait_microseconds() const { return wait_us_.load(); }
    // Removes every vertex variant of an original. Existing command-buffer references keep a
    // ready variant alive; an in-progress build may finish but cannot reinsert its retired entry.
    void retire(uint64_t identity);
    // Call before destroying the device. Also release every command-buffer/GPU owner before
    // vkDestroyDevice. Stop joins the worker; it is not for the draw or submit hot path.
    void stop();
    // The device's pipeline cache the variants are made through, for the owned runtime's pipelines too.
    VkPipelineCache pipeline_cache() const { return pipeline_cache_; }

private:
    struct Key {
        uint64_t identity;
        std::vector<uint32_t> vertex, fragment;
        bool operator<(const Key& other) const {
            if(identity!=other.identity)return identity<other.identity;
            if(vertex!=other.vertex)return vertex<other.vertex;
            return fragment<other.fragment;
        }
    };
    struct Job {
        Key key;
        std::shared_ptr<const GraphicsRecipe> recipe;
        std::shared_ptr<const PipelineLayoutInfo> layout;
        VertexInputRecipe vertex;
        FragmentPatch fragment;
        VariantResult result;
        bool prepared = false; // queued in later_
        uint64_t used = 0;     // the cache's clock when it was last asked for
    };
    static Key key_for(uint64_t, const VertexInputRecipe&, const FragmentPatch&);
    VariantResult submit(uint64_t identity, std::shared_ptr<const GraphicsRecipe>, std::shared_ptr<const PipelineLayoutInfo>,
                         const VertexInputRecipe&, const FragmentPatch&, bool prepared);
    void run();
    VkDevice dev_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VariantDispatch dispatch_;
    uint32_t max_sets_;
    VkPhysicalDeviceLimits limits_;
    size_t capacity_;
    std::mutex mutex_, stop_mutex_;
    std::condition_variable wake_, done_;
    std::atomic<uint64_t> waits_{0}, wait_us_{0}, evictions_{0};
    uint64_t clock_ = 0; // under mutex_
    // When full: the finished jobs least recently asked for go (an eighth of the capacity at a time), into
    // `released`, which the caller lets go of outside the lock. Caller holds mutex_.
    void evict(std::vector<std::shared_ptr<Job>>& released);
    bool stopped_ = false;
    std::map<Key, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_, later_;
    // Several builders: the variants a character needs when it first appears compile in parallel,
    // the driver compiling on each thread (D-101; one builder took 0.3-2 s per character switch).
    std::vector<std::thread> workers_;
};

} // namespace efvk
