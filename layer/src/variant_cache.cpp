#include "variant_cache.h"
#include <algorithm>
#include <stdexcept>

namespace efvk {

VariantCache::Key VariantCache::key_for(uint64_t identity, const VertexInputRecipe& vertex,const FragmentPatch& fragment)
{
    Key key{identity, {}};
    auto bindings = vertex.bindings;
    auto attributes = vertex.attributes;
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
    std::sort(attributes.begin(), attributes.end(), [](const auto& a, const auto& b) { return a.location < b.location; });
    key.vertex.push_back(static_cast<uint32_t>(bindings.size()));
    for (const auto& b : bindings) key.vertex.insert(key.vertex.end(), {b.binding, b.stride, uint32_t(b.inputRate)});
    key.vertex.push_back(static_cast<uint32_t>(attributes.size()));
    for (const auto& a : attributes) key.vertex.insert(key.vertex.end(), {a.location, a.binding, uint32_t(a.format), a.offset});
    key.fragment.push_back(fragment.replacement_images);
    for(const auto* list:{&fragment.images,&fragment.nulls}){
        key.fragment.push_back(static_cast<uint32_t>(list->size()));
        for(const auto& image:*list)key.fragment.insert(key.fragment.end(),{image.set,image.binding});
    }
    key.fragment.push_back(fragment.keep_vertex ? 1u : 0u);
    key.fragment.push_back(fragment.no_dither ? 1u : 0u);
    key.fragment.push_back(fragment.own_geometry ? 1u : 0u);
    return key;
}

unsigned VariantCache::builders_for_device()
{
    const unsigned cores = std::thread::hardware_concurrency();
    return std::min(4u, std::max(2u, cores / 2));
}

VariantCache::VariantCache(VkDevice dev, const VariantDispatch& dispatch, uint32_t max_sets, size_t capacity,
                           const VkPhysicalDeviceLimits& limits, VkPipelineCache pipeline_cache, unsigned builders)
    : dev_(dev), pipeline_cache_(pipeline_cache), dispatch_(dispatch), max_sets_(max_sets), limits_(limits), capacity_(std::min<size_t>(capacity, 4096))
{
    for (unsigned i = 0; i < std::max(1u, builders); ++i) workers_.emplace_back(&VariantCache::run, this);
}
VariantCache::~VariantCache() { stop(); }

VariantResult VariantCache::request(uint64_t identity, std::shared_ptr<const GraphicsRecipe> recipe,
                                   std::shared_ptr<const PipelineLayoutInfo> layout, const VertexInputRecipe& vertex,const FragmentPatch& fragment)
{
    return submit(identity, std::move(recipe), std::move(layout), vertex, fragment, false);
}

VariantResult VariantCache::prepare(uint64_t identity, std::shared_ptr<const GraphicsRecipe> recipe,
                                   std::shared_ptr<const PipelineLayoutInfo> layout, const VertexInputRecipe& vertex,const FragmentPatch& fragment)
{
    return submit(identity, std::move(recipe), std::move(layout), vertex, fragment, true);
}

VariantResult VariantCache::submit(uint64_t identity, std::shared_ptr<const GraphicsRecipe> recipe,
                                  std::shared_ptr<const PipelineLayoutInfo> layout, const VertexInputRecipe& vertex,
                                  const FragmentPatch& fragment, bool prepared)
{
    if (!identity || !recipe || !layout || vertex.bindings.size() > 32 || vertex.attributes.size() > 32 ||
        fragment.images.size()>32 || fragment.nulls.size()>32 || fragment.images.size()+fragment.nulls.size()>32)
        return {VariantState::Failed, {}, "invalid variant request"};
    auto key = key_for(identity, vertex,fragment);
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return {VariantState::Stopped, {}, "variant cache stopped"};
    auto found = jobs_.find(key);
    if (found != jobs_.end()) {
        // A prepared variant a draw now asks for goes ahead with the requested ones.
        const auto& job = found->second;
        if (!prepared && job->prepared) {
            job->prepared = false;
            later_.erase(std::remove(later_.begin(), later_.end(), job), later_.end());
            if (job->result.state == VariantState::Queued) { queue_.push_back(job); wake_.notify_one(); }
        }
        return job->result;
    }
    if (jobs_.size() >= capacity_) return {VariantState::Saturated, {}, "variant cache capacity reached"};
    auto job = std::make_shared<Job>();
    job->key = std::move(key); job->recipe = std::move(recipe); job->layout = std::move(layout); job->vertex = vertex;
    job->fragment=fragment; job->prepared = prepared;
    jobs_.emplace(job->key, job); (prepared ? later_ : queue_).push_back(job); wake_.notify_one();
    return job->result;
}

VariantResult VariantCache::await(uint64_t identity, std::shared_ptr<const GraphicsRecipe> recipe,
                                  std::shared_ptr<const PipelineLayoutInfo> layout, const VertexInputRecipe& vertex,
                                  const FragmentPatch& fragment, std::chrono::milliseconds timeout)
{
    auto result = request(identity, recipe, layout, vertex, fragment);
    if (result.state != VariantState::Queued && result.state != VariantState::Building) return result;
    const auto began = std::chrono::steady_clock::now();
    const auto key = key_for(identity, vertex, fragment);
    std::unique_lock<std::mutex> lock(mutex_);
    ++waits_;
    done_.wait_for(lock, timeout, [&] {
        if (stopped_) return true;
        auto found = jobs_.find(key);
        if (found == jobs_.end()) return true;
        const auto state = found->second->result.state;
        return state != VariantState::Queued && state != VariantState::Building;
    });
    auto found = jobs_.find(key);
    result = found != jobs_.end() ? found->second->result : VariantResult{VariantState::Stopped, {}, "variant retired while waited for"};
    lock.unlock();
    wait_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
    return result;
}

void VariantCache::retire(uint64_t identity)
{
    // GPU destruction may call into a driver. Do not perform it under the cache mutex.
    std::vector<std::shared_ptr<Job>> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->first.identity == identity) { released.push_back(it->second); it = jobs_.erase(it); }
            else ++it;
        }
        for (auto* queue : {&queue_, &later_})
            queue->erase(std::remove_if(queue->begin(), queue->end(),
                         [identity](const auto& job) { return job->key.identity == identity; }), queue->end());
        done_.notify_all();
    }
}

void VariantCache::stop()
{
    // Serializes concurrent stop callers without holding the worker's publication mutex.
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    std::map<Key, std::shared_ptr<Job>> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true; released.swap(jobs_); queue_.clear(); later_.clear(); wake_.notify_all();
        done_.notify_all();
    }
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

void VariantCache::run()
{
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopped_ || !queue_.empty() || !later_.empty(); });
            if (stopped_) return;
            auto& from = !queue_.empty() ? queue_ : later_;
            job = from.front(); from.pop_front(); job->prepared = false; job->result.state = VariantState::Building;
        }
        VariantResult result;
        try {
            auto variant = std::make_shared<PipelineVariant>(dev_, dispatch_, pipeline_cache_);
            if (variant->build(*job->recipe, *job->layout, job->vertex, max_sets_,job->fragment,limits_))
                result = {VariantState::Ready, std::move(variant), {}};
            else result = {VariantState::Failed, {}, variant->reason};
        } catch (const std::exception& e) { result = {VariantState::Failed, {}, e.what()}; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(job->key);
            if (!stopped_ && found != jobs_.end() && found->second == job) {
                job->result = std::move(result);
                job->recipe.reset(); job->layout.reset();
            }
            done_.notify_all();
        }
        // A retired build's temporary GPU resources are destroyed outside the lock here.
    }
}

} // namespace efvk
