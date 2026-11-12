// Deterministic concurrency/lifetime tests: the fake driver can pause pipeline creation.
#include "variant_fixture.h"
#include "../layer/src/variant_cache.h"
#include "material_fixture.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <set>

using namespace efvk;
static int passed = 0, failed = 0;
static void check(bool ok, const char* name) { if (ok) ++passed; else { ++failed; printf("FAIL: %s\n", name); } }
template<class T> static T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
struct Driver {
    std::mutex mutex;
    std::condition_variable wake;
    uintptr_t next = 1;
    std::set<uintptr_t> live;
    uint32_t started = 0, finished = 0;
    bool pause = false, fail = false;
    template<class T> VkResult create(T* out) {
        std::lock_guard<std::mutex> lock(mutex); live.insert(next); *out = handle<T>(next++); return VK_SUCCESS;
    }
    template<class T> void destroy(T value) {
        std::lock_guard<std::mutex> lock(mutex); if (live.erase(reinterpret_cast<uintptr_t>(value)) != 1) std::abort();
    }
    void resume() { std::lock_guard<std::mutex> lock(mutex); pause = false; wake.notify_all(); }
    bool await_started(uint32_t n) {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_for(lock,std::chrono::seconds(5),[&]{return started>=n;});
    }
    size_t count() { std::lock_guard<std::mutex> lock(mutex); return live.size(); }
} driver;
#define CREATE(Name, Info, Type) \
static VKAPI_ATTR VkResult VKAPI_CALL create_##Name(VkDevice, const Info*, const VkAllocationCallbacks*, Type* out) { return driver.create(out); }
#define DESTROY(Name, Type) \
static VKAPI_ATTR void VKAPI_CALL destroy_##Name(VkDevice, Type h, const VkAllocationCallbacks*) { driver.destroy(h); }
CREATE(shader,VkShaderModuleCreateInfo,VkShaderModule)
CREATE(set,VkDescriptorSetLayoutCreateInfo,VkDescriptorSetLayout)
CREATE(layout,VkPipelineLayoutCreateInfo,VkPipelineLayout)
CREATE(pass,VkRenderPassCreateInfo,VkRenderPass)
DESTROY(shader,VkShaderModule)
DESTROY(set,VkDescriptorSetLayout)
DESTROY(layout,VkPipelineLayout)
DESTROY(pass,VkRenderPass)
DESTROY(pipe,VkPipeline)
static VKAPI_ATTR VkResult VKAPI_CALL create_pipe(VkDevice, VkPipelineCache, uint32_t,
    const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out)
{
    std::unique_lock<std::mutex> lock(driver.mutex); ++driver.started; driver.wake.notify_all();
    driver.wake.wait(lock,[]{return !driver.pause;});
    ++driver.finished;
    if (driver.fail) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    driver.live.insert(driver.next); *out = handle<VkPipeline>(driver.next++); return VK_SUCCESS;
}
static VariantDispatch dispatch()
{
    VariantDispatch d;
    d.CreateShaderModule=create_shader; d.DestroyShaderModule=destroy_shader;
    d.CreateDescriptorSetLayout=create_set; d.DestroyDescriptorSetLayout=destroy_set;
    d.CreatePipelineLayout=create_layout; d.DestroyPipelineLayout=destroy_layout;
    d.CreateRenderPass=create_pass; d.DestroyRenderPass=destroy_pass;
    d.CreateGraphicsPipelines=create_pipe; d.DestroyPipeline=destroy_pipe;
    return d;
}
static VariantResult await(VariantCache& cache,uint64_t id,std::shared_ptr<const GraphicsRecipe> recipe,
                           std::shared_ptr<const PipelineLayoutInfo> layout,const VertexInputRecipe& vertex,const FragmentPatch& fragment={})
{
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    VariantResult result;
    do {
        result=cache.request(id,recipe,layout,vertex,fragment);
        if (result.state!=VariantState::Queued && result.state!=VariantState::Building) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<end);
    return result;
}
static bool await_live(size_t count)
{
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(driver.count()!=count && std::chrono::steady_clock::now()<end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return driver.count()==count;
}
int main()
{
    variant_fixture::RecipeFixture f;
    auto recipe=std::make_shared<GraphicsRecipe>(f.capture());
    auto layout=std::make_shared<PipelineLayoutInfo>(f.layout);
    auto vertex=recipe->vertex;
    vertex.bindings.push_back({4,0,VK_VERTEX_INPUT_RATE_VERTEX});
    auto reordered=vertex;
    std::reverse(reordered.bindings.begin(),reordered.bindings.end());
    std::reverse(reordered.attributes.begin(),reordered.attributes.end());
    {
        driver.pause=true;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4,2);
        auto first=cache.request(1,recipe,layout,vertex);
        check(first.state==VariantState::Queued && !first.variant,"first request returns pending without driver wait");
        check(driver.await_started(1),"worker enters paused driver");
        auto again=cache.request(1,recipe,layout,reordered);
        check(again.state==VariantState::Building && !again.variant,"canonical vertex key deduplicates reordered declaration");
        auto changed=vertex; changed.bindings[0].stride+=4;
        check(cache.request(1,recipe,layout,changed).state==VariantState::Queued,"changed stride is a distinct variant");
        check(cache.request(2,recipe,layout,vertex).state==VariantState::Saturated,"bounded cache retains original path when full of pending builds");
        check(cache.request(0,recipe,layout,vertex).state==VariantState::Failed &&
              cache.request(3,{},layout,vertex).state==VariantState::Failed,"invalid identities and absent recipes rejected");
        driver.resume();
        auto ready=await(cache,1,recipe,layout,vertex);
        auto second=await(cache,1,recipe,layout,changed);
        check(ready.state==VariantState::Ready && second.state==VariantState::Ready &&
              ready.variant->pipeline!=second.variant->pipeline,"two complete variants publish atomically");
        check(driver.started==2,"duplicate requests cause no extra driver compilation");
        auto retained=ready.variant; ready.variant.reset(); second.variant.reset();
        cache.retire(1);
        check(await_live(6) && retained->pipeline,"retired variant lives only through command owner");
        retained.reset(); check(await_live(0),"final command owner releases all GPU objects");
        auto next=await(cache,2,recipe,layout,vertex);
        check(next.state==VariantState::Ready && driver.started==3,"new pipeline generation builds independently");
        next.variant.reset(); cache.stop();
        check(driver.count()==0 && cache.request(2,recipe,layout,vertex).state==VariantState::Stopped,
              "stop drains owned variants and rejects further requests");
    }
    {
        driver.pause=true;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4,2);
        cache.request(10,recipe,layout,vertex); check(driver.await_started(4),"retirement scenario starts build");
        cache.request(11,recipe,layout,vertex); cache.retire(11); cache.retire(10);
        auto replacement=cache.request(10,recipe,layout,vertex);
        check(replacement.state==VariantState::Queued,"retired in-progress key can be requested afresh");
        driver.resume();
        auto ready=await(cache,10,recipe,layout,vertex);
        check(ready.state==VariantState::Ready && driver.started==5,"canceled queue skipped and stale completion not reinserted");
        ready.variant.reset(); cache.stop(); check(driver.count()==0,"retired build resources all released");
    }
    {
        driver.fail=true;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4);
        auto result=await(cache,20,recipe,layout,vertex);
        check(result.state==VariantState::Failed && !result.variant && result.reason=="create_variant_pipeline",
              "driver failure publishes explicit failed state");
        check(cache.request(20,recipe,layout,vertex).state==VariantState::Failed && driver.started==6 && driver.count()==0,
              "failed variant is not retried on every draw and partial objects are released");
        driver.fail=false;
    }
    {
        driver.pause=true;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4);
        cache.request(30,recipe,layout,vertex); check(driver.await_started(7),"stop scenario starts build");
        cache.request(31,recipe,layout,vertex);
        std::atomic<bool> stopped{false};
        std::thread stopper([&]{cache.stop();stopped=true;});
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(cache.request(30,recipe,layout,vertex).state!=VariantState::Stopped && std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(cache.request(30,recipe,layout,vertex).state==VariantState::Stopped && !stopped.load(),
              "device teardown waits for an active driver call");
        driver.resume(); stopper.join();
        check(stopped && driver.started==7 && driver.count()==0,"stop cancels pending builds and joins worker before teardown");
    }
    {
        auto material=std::make_shared<GraphicsRecipe>(*recipe);auto fragment=std::make_shared<ShaderInfo>();
        fragment->code=material_fixture::shader(false);fragment->size=static_cast<uint32_t>(fragment->code.size()*4);
        material->stages[1].shader=fragment;
        auto material_layout=std::make_shared<PipelineLayoutInfo>(*layout);
        auto image_set=std::make_shared<DescriptorLayoutInfo>(*layout->sets[1]);
        image_set->bindings[6]={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,0,{}};
        material_layout->sets[1]=image_set;
        VkPhysicalDeviceLimits limits{};limits.maxDescriptorSetSampledImages=limits.maxPerStageDescriptorSampledImages=32;
        limits.maxPerStageResources=64;VariantCache cache(handle<VkDevice>(1),dispatch(),4,8,limits);
        FragmentPatch moved; moved.images={{1,6}};FragmentPatch nulled;nulled.nulls={{1,6}};
        auto a=await(cache,40,material,material_layout,vertex,moved),b=await(cache,40,material,material_layout,vertex,nulled);
        check(a.state==VariantState::Ready && b.state==VariantState::Ready && a.variant!=b.variant &&
              a.variant->image_count==1 && b.variant->image_count==0,"relocation and null masks have distinct cache identities and descriptor layouts");
        check(cache.request(40,material,material_layout,vertex,moved).variant==a.variant,"identical material contract shares compiled pipeline");
        auto duplicate=moved;duplicate.nulls=nulled.nulls;
        check(await(cache,40,material,material_layout,vertex,duplicate).state==VariantState::Failed,"overlapping null and image requests refuse");
        auto missing=moved;missing.images[0].binding=99;
        check(await(cache,40,material,material_layout,vertex,missing).state==VariantState::Failed,"missing material source layout refuses before driver work");
        VkPhysicalDeviceLimits small=limits;small.maxDescriptorSetSampledImages=1;
        PipelineVariant too_many(handle<VkDevice>(1),dispatch());
        check(!too_many.build(*material,*material_layout,vertex,4,moved,small) && too_many.reason=="fragment_patch_descriptor_limits",
              "descriptor limit includes retained original and new private image");
        cache.retire(40);cache.stop();check(a.variant->pipeline && b.variant->pipeline,"retired material variants survive through command owners");
        a.variant.reset();b.variant.reset();check(driver.count()==0,"all retired material objects release after command owners");
    }
    {
        driver.pause=true; const uint32_t before=driver.started;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4,8,{},VK_NULL_HANDLE,2);
        cache.request(50,recipe,layout,vertex); cache.request(51,recipe,layout,vertex);
        check(driver.await_started(before+2),"a cold transaction can occupy both builders before any wait");
        auto wait_started=[&](uint64_t count) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(cache.waits()<count && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return cache.waits()>=count;
        };
        auto retired=std::async(std::launch::async,[&]{return cache.await(50,recipe,layout,vertex,{},std::chrono::seconds(10));});
        check(wait_started(1),"retirement waiter enters the production bounded wait");
        cache.retire(50);
        const bool retired_woke=retired.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        check(retired_woke && retired.get().state==VariantState::Stopped,
              "retirement wakes a draw while the retired driver compilation is still paused");
        auto stopped=std::async(std::launch::async,[&]{return cache.await(51,recipe,layout,vertex,{},std::chrono::seconds(10));});
        check(wait_started(2),"shutdown waiter enters the production bounded wait");
        auto stopper=std::async(std::launch::async,[&]{cache.stop();});
        const bool stop_woke=stopped.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        check(stop_woke && stopped.get().state==VariantState::Stopped,
              "shutdown notifies draws before the driver finishes its outstanding compilations");
        driver.resume(); stopper.get();
        check(driver.count()==0,"parallel canceled builders release all temporary GPU objects");
    }
    {
        // D-133: a prepared variant (the state a key turns on) is built after the requested ones; a
        // request for it moves it ahead of the variants still only prepared.
        driver.pause=true;
        VariantCache cache(handle<VkDevice>(1),dispatch(),4);
        const uint32_t before=driver.started;
        cache.request(60,recipe,layout,vertex); check(driver.await_started(before+1),"preparation scenario occupies the builder");
        check(cache.prepare(61,recipe,layout,vertex).state==VariantState::Queued,"a prepared variant is queued");
        cache.request(62,recipe,layout,vertex);
        cache.prepare(63,recipe,layout,vertex); cache.request(63,recipe,layout,vertex);
        driver.resume();
        auto built=[&](uint64_t id){
            auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            VariantResult result;
            do {
                result=cache.prepare(id,recipe,layout,vertex);   // a prepare of a known variant moves nothing
                if (result.state==VariantState::Ready) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while(std::chrono::steady_clock::now()<end);
            return result.variant ? reinterpret_cast<uintptr_t>(result.variant->pipeline) : uintptr_t(0);
        };
        const auto p61=built(61),p62=built(62),p63=built(63);
        check(p61 && p62 && p63 && p62<p63 && p63<p61 && driver.started==before+4,
              "requested variants build before a prepared one, and a requested prepared one with them");
        cache.stop();
    }
    {
        // L-08: a full cache lets go of the finished variant least recently asked for, and builds the new one.
        VariantCache cache(handle<VkDevice>(1),dispatch(),4,2);
        auto changed=vertex; changed.bindings[0].stride+=4;
        auto a=await(cache,7,recipe,layout,vertex), b=await(cache,7,recipe,layout,changed);
        check(a.state==VariantState::Ready && b.state==VariantState::Ready,"two finished variants fill the cache");
        auto kept=a.variant;
        cache.request(7,recipe,layout,vertex); // asked for again: the other one is now the older
        const auto third=await(cache,8,recipe,layout,vertex);
        check(third.state==VariantState::Ready && cache.evictions()==1,"the third is built after the least recently asked for went");
        const auto started=driver.started;
        check(cache.request(7,recipe,layout,vertex).state==VariantState::Ready && driver.started==started,"the one asked for again stayed");
        check(kept && kept->pipeline,"a variant held by a command buffer lives on whatever the cache let go of");
        a.variant.reset(); b.variant.reset();
        cache.stop();
        kept.reset();
    }
    printf("variant cache: %d checks passed, %d failed\n",passed,failed);
    return failed?1:0;
}
