#include "m2_backend.h"
#include "character_backend.h"
#include "character_module.h"
#include "character_package.h"
#include "m2_mesh.h"
#include "state.h"
#include "json.h"
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <shared_mutex>

namespace efvk {
struct M2GpuMesh {
    VkDevice device;
    SnapshotDispatch d;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    void* mapped=nullptr;
    DrawGeometry geometry;
    M2GpuMesh(VkDevice dev,const SnapshotDispatch& dispatch):device(dev),d(dispatch){}
    ~M2GpuMesh(){
        if(mapped)d.UnmapMemory(device,memory);
        if(buffer)d.DestroyBuffer(device,buffer,nullptr);
        if(memory)d.FreeMemory(device,memory,nullptr);
    }
    bool create(const M2Mesh& mesh,const VkPhysicalDeviceMemoryProperties& properties,std::string& reason){
        auto good=[&](VkResult result,const char* text){if(result==VK_SUCCESS)return true;reason=text;return false;};
        uint64_t offsets[]={0,mesh.position.size(),mesh.position.size()+mesh.texcoord.size(),
                           mesh.position.size()+mesh.texcoord.size()+mesh.blend.size()};
        VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size=offsets[3]+mesh.index.size();ci.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if(!good(d.CreateBuffer(device,&ci,nullptr,&buffer),"create mesh buffer"))return false;
        VkMemoryRequirements req;d.GetBufferMemoryRequirements(device,buffer,&req);
        if(req.size>(32ull<<20)){reason="mesh allocation budget exceeded";return false;}
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<properties.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags &
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
        if(type==UINT32_MAX){reason="host-coherent mesh memory unavailable";return false;}
        VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(!good(d.AllocateMemory(device,&ai,nullptr,&memory),"allocate mesh memory") ||
           !good(d.BindBufferMemory(device,buffer,memory,0),"bind mesh memory") ||
           !good(d.MapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped),"map mesh memory"))return false;
        const std::vector<uint8_t>* streams[]={&mesh.position,&mesh.texcoord,&mesh.blend,&mesh.index};
        for(uint32_t i=0;i<4;++i)std::memcpy(static_cast<uint8_t*>(mapped)+offsets[i],streams[i]->data(),streams[i]->size());
        geometry={0,{buffer,buffer,buffer,buffer},{offsets[0],offsets[1],offsets[2],offsets[0]},buffer,offsets[3],VK_INDEX_TYPE_UINT32};
        return true;
    }
};

class M2Backend {
public:
    Device* device;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VariantDispatch draw;
    SnapshotDispatch storage;
    M2Mesh profile;
    std::unique_ptr<VariantCache> cache;
    std::atomic<bool> enabled{false}, ready{false};
    std::atomic<uint64_t> matched{0},replaced{0},pending{0},unsupported{0},unreadable{0},exhausted{0},batches{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::string last_reason;
    bool stopped=false;
    std::thread worker;
    std::shared_ptr<const M2GpuMesh> mesh;
    std::deque<std::shared_ptr<DrawSnapshots>> available;
    std::vector<std::weak_ptr<DrawSnapshots>> all;
    M2Backend(Device* dev,const VkPhysicalDeviceProperties& p,const VkPhysicalDeviceMemoryProperties& m)
        :device(dev),properties(p),memory(m){}
    ~M2Backend(){stop();}
    void note(const std::string& reason){std::lock_guard<std::mutex> lock(mutex);last_reason=reason;}
    bool init(){
        if(!profile.load(g_cfg.m2_package,last_reason))return false;
#define X(n) draw.n=reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle,"vk" #n));
        EFVK_VARIANT_FUNCS(X)
#undef X
#define X(n) storage.n=reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle,"vk" #n));if(!storage.n){last_reason="missing vk" #n;return false;}
        EFVK_SNAPSHOT_FUNCS(X)
#undef X
        // RenderPass2 is optional; the recipe builder rejects it if the actual function is absent.
        if(!draw.CreateGraphicsPipelines || !draw.CreateRenderPass || !draw.CmdDrawIndexed){last_reason="missing graphics entry";return false;}
        cache=std::make_unique<VariantCache>(device->handle,draw,properties.limits.maxBoundDescriptorSets,128,properties.limits);
        enabled=g_cfg.m2_enabled;
        worker=std::thread([this]{prepare();});return true;
    }
    void stop(){
        {std::lock_guard<std::mutex> lock(mutex);stopped=true;enabled=false;wake.notify_all();}
        if(worker.joinable())worker.join();
        if(cache)cache->stop();
        std::deque<std::shared_ptr<DrawSnapshots>> old;
        std::shared_ptr<const M2GpuMesh> old_mesh;
        {std::lock_guard<std::mutex> lock(mutex);old.swap(available);old_mesh.swap(mesh);ready=false;}
    }
    void prepare(){
        try{
            auto created=std::make_shared<M2GpuMesh>(device->handle,storage);std::string reason;
            if(!created->create(profile,memory,reason)){note(reason);return;}
            {std::lock_guard<std::mutex> lock(mutex);mesh=created;}
            while(true){
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    all.erase(std::remove_if(all.begin(),all.end(),[](const auto& p){return p.expired();}),all.end());
                    if(stopped)return;
                    if(available.size()>=2 || all.size()>=8){wake.wait_for(lock,std::chrono::milliseconds(100));continue;}
                }
                auto snapshots=std::make_shared<DrawSnapshots>(device->handle,storage);
                uint32_t bytes=(512+2*profile.bones)*48;
                if(!snapshots->create(memory,properties.limits,16,bytes)){note(snapshots->reason);return;}
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(stopped)return;
                    all.push_back(snapshots);available.push_back(std::move(snapshots));ready=true;
                }
            }
        }catch(const std::exception& e){note(e.what());}
    }
    std::shared_ptr<M2Command> command(){
        std::lock_guard<std::mutex> lock(mutex);
        if(stopped || available.empty() || !mesh)return {};
        auto cmd=std::make_shared<M2Command>();cmd->backend=this;cmd->snapshots=available.front();available.pop_front();cmd->mesh=mesh;
        wake.notify_one();return cmd;
    }
    bool record(CmdBuf*,const DrawRec&);
};

bool M2Backend::record(CmdBuf* cb,const DrawRec& r)
{
    if(!enabled.load(std::memory_order_relaxed) || !cache || r.count!=profile.match_count || r.first!=profile.match_first)
        return false;
    auto refuse=[&](std::atomic<uint64_t>& counter,const std::string& reason){++counter;note(reason);return false;};
    InstanceEntry entry;
    std::vector<uint8_t> current((profile.component_count+1)*48),previous(current.size());
    VariantResult variant;
    DrawGeometry original;
    {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        auto ib=device->bufs.find(r.ib);
        if(ib==device->bufs.end() || (r.index_size!=2 && r.index_size!=4))return false;
        uint64_t start=uint64_t(r.first)*r.index_size,bytes=uint64_t(r.count)*r.index_size;
        if(r.ib_off>ib->second.size || start>ib->second.size-r.ib_off || bytes>ib->second.size-r.ib_off-start)return false;
        std::vector<uint8_t> indices(static_cast<size_t>(bytes));
        if(read_buffer_locked(device,ib->second,r.ib_off+start,bytes,indices.data())!=SRC_MAPPED ||
           crc32c(0,indices.data(),indices.size())!=profile.hash)return false;
        ++matched;
        if(cb->secondary || !cb->in_rp || cb->dyn_rp_count || cb->m2_extended_bind || r.instances!=1 ||
           r.first_instance!=0 || r.vertex_offset!=0 || r.sets[3].set || r.sets[3].pushed)
            return refuse(unsupported,"M2 requires primary classic direct single-instance draws and no live set 3 or extended binds");
        if(!ready.load())return refuse(pending,"M2 resources are preparing");
        auto pipe=device->pipes.find(r.pipeline);
        if(pipe==device->pipes.end() || !pipe->second.recipe || !pipe->second.layout)
            return refuse(unsupported,"M2 source pipeline metadata missing");
        VertexInputRecipe vertex;std::string reason;
        if(!profile.vertex_input(pipe->second.recipe->vertex,vertex,reason))return refuse(unsupported,reason);
        variant=cache->request(pipe->second.id,pipe->second.recipe,pipe->second.layout,vertex);
        if(!variant.variant){
            if(variant.state==VariantState::Queued || variant.state==VariantState::Building)
                return refuse(pending,"M2 variant is preparing");
            return refuse(unsupported,"M2 variant: "+variant.reason);
        }
        std::vector<BoundRange> ranges;
        for(uint32_t set: {2u,variant.variant->bone_set}){
            const auto& bound=r.sets[set];
            if(bound.dyn_count && uint64_t(bound.dyn_first)+bound.dyn_count>cb->dyn.size())
                return refuse(unreadable,"M2 dynamic offsets incomplete");
            collect_buffer_ranges_locked(device,bound,bound.dyn_count?cb->dyn.data()+bound.dyn_first:nullptr,bound.dyn_count,set,ranges);
        }
        const BoundRange *instance=nullptr,*bones=nullptr;
        for(const auto& range:ranges){
            if(range.element!=0)continue;
            if(range.set==2 && range.binding==0 && (range.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
               range.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))instance=&range;
            if(range.set==variant.variant->bone_set && range.binding==variant.variant->bone_binding &&
               (range.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || range.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))bones=&range;
        }
        auto read_range=[&](const BoundRange* range,uint64_t offset,size_t size,uint8_t* output){
            if(!range || offset>range->size || size>range->size-offset)return false;
            auto buffer=device->bufs.find(range->buffer);
            return buffer!=device->bufs.end() && read_buffer_locked(device,buffer->second,range->off+offset,size,output)==SRC_MAPPED;
        };
        if(!read_range(instance,0,entry.size(),entry.data()))return refuse(unreadable,"M2 instance entry is not mapped");
        uint32_t offsets[2];std::memcpy(offsets,entry.data()+80,8);
        if(offsets[0]<MergedSkeleton::kStaticRows || offsets[1]<MergedSkeleton::kStaticRows)
            return refuse(unsupported,"M2 initial component path requires two dynamic poses");
        if(!read_range(bones,uint64_t(offsets[0])*16,current.size(),current.data()) ||
           !read_range(bones,uint64_t(offsets[1])*16,previous.size(),previous.data()))
            return refuse(unreadable,"M2 component palette is not mapped");
        original.index=r.ib;original.index_offset=r.ib_off;
        original.index_type=r.index_size==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;
        for(uint32_t i=0;i<4;++i){
            if(!r.vb[i].buf)return refuse(unsupported,"M2 original vertex bindings incomplete");
            original.buffers.push_back(r.vb[i].buf);original.offsets.push_back(r.vb[i].off);
        }
    }
    // This first package is restricted to a component whose vertices use only its own palette.
    // Snapshot each draw independently, avoiding cross-submit master updates and instance mixing.
    MergedSkeleton skeleton;std::string reason;std::vector<BoneComponent> components;
    if(profile.component_first)components.push_back({0,profile.component_first});
    uint32_t selected=static_cast<uint32_t>(components.size());
    components.push_back({profile.component_first,profile.component_count});
    uint32_t end=profile.component_first+profile.component_count;
    if(end<profile.bones)components.push_back({end,profile.bones-end});
    InstanceEntry patched;
    if(!skeleton.configure(profile.bones,1,components,reason) || !skeleton.begin_update(0,reason) ||
       skeleton.import_component(0,selected,0,entry,{current.data(),current.size()},{previous.data(),previous.size()}, {},1,reason)==BoneImportResult::Rejected ||
       !skeleton.patch_entry(0,entry,patched,reason))return refuse(unreadable,"M2 bone import: "+reason);
    const auto held=owner_for(cb->owners,this,[this]{return std::shared_ptr<CommandOwner>(command());});
    if(!held)return refuse(exhausted,"M2 snapshot pool is at capacity or refilling");
    M2Command* const owner=static_cast<M2Command*>(held.get());owner->used=true;
    SnapshotSlot slot;
    if(!owner->snapshots->append(skeleton.bytes(),patched,slot))return refuse(exhausted,owner->snapshots->reason);
    // Establish ownership before recording any private handle. No allocations after the draw
    // transaction starts, so an exception cannot cause an original draw after a partial replacement.
    owner->variants.push_back(variant.variant);
    IndexedDraw draw_indices;draw_indices.count=profile.indices;
    if(!emit_variant_draw(draw,cb->handle,*variant.variant,slot.set,slot.offset,owner->mesh->geometry,
                          draw_indices,r.bound?r.bound:r.pipeline,original))return refuse(unsupported,"M2 draw transaction refused");
    ++replaced;return true;
}

// ---------------------------------------------------------------------------
// The character backend and its exchange (docs/hot-swap.md).
//
// Every call into the backend in use holds Device::character_mx shared, so taking the lock
// exclusively is the moment in which no thread is inside it: the staged backend takes its place
// there, between two draws. The backend that was in use is stopped and destroyed on a thread of
// its own, because that joins threads; what command buffers still hold of it stays alive with them.

namespace {
constexpr uint64_t kStagingMicroseconds = 30ull * 1000 * 1000; // a staged backend that is not ready by then is dropped

struct ModuleState {
    std::mutex mx;
    uint32_t generation = 0;      // exchanges completed in this process
    std::string path, version;    // of the code in use; no path: the copy built into the layer
    std::string staged_path, staged_version, error;
    uint64_t staged_us = 0;
    int enabled = -1;             // what the probe last asked for; -1: the configured value
    std::vector<std::thread> janitors;
} g_module;

void host_log(const char* text) { logf("%s", text); }
uint8_t host_read_buffer(Device* dev, const BufInfo& b, uint64_t off, uint64_t size, uint8_t* out)
{
    return static_cast<uint8_t>(read_buffer_locked(dev, b, off, size, out));
}

void module_status(JsonWriter& w)
{
    std::lock_guard<std::mutex> lock(g_module.mx);
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "%016llx", static_cast<unsigned long long>(EFVK_ABI));
    w.key("module"); w.begin_object();
    w.kn("generation", g_module.generation);
    w.kb("builtin", g_module.path.empty());
    w.kv("version", g_module.version);
    w.kv("path", g_module.path);
    w.kv("staged", g_module.staged_path);
    w.kv("last_error", g_module.error);
    w.kv("abi", stamp);
    w.end_object();
}

// The staged backend takes over (`promote`) or is given up.
void exchange(Device* device, bool promote)
{
    std::shared_ptr<CharacterBackend> old;
    const CharacterModule* old_module = nullptr;
    std::string version;
    {
        std::unique_lock<std::shared_mutex> lock(device->character_mx);
        if (!device->character_staged) return;
        if (promote) {
            old = std::move(device->character); old_module = device->character_module;
            device->character = std::move(device->character_staged); device->character_module = device->staged_module;
            version = device->character_module->version;
        } else {
            old = std::move(device->character_staged); old_module = device->staged_module;
        }
        device->character_staged.reset(); device->staged_module = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_module.mx);
    if (promote) {
        ++g_module.generation; g_module.path = g_module.staged_path; g_module.version = version; g_module.error.clear();
        logf("replacement module %u in use: %s (%s)", g_module.generation, g_module.path.c_str(), version.c_str());
    } else {
        g_module.error = "staged backend was not ready in time: " + g_module.staged_path;
        logf("replacement module given up, not ready in time: %s", g_module.staged_path.c_str());
    }
    g_module.staged_path.clear(); g_module.staged_version.clear();
    if (old) g_module.janitors.emplace_back([old, old_module]() mutable { old_module->stop(*old); old.reset(); });
}
} // namespace

bool m2_record(Device* device,CmdBuf* cb,const DrawRec& r){
    if(device->character_mode){
        std::shared_lock<std::shared_mutex> lock(device->character_mx);
        return device->character && device->character_module->record(*device->character,cb,r);
    }
    if(!device->m2)return false;
    try{return device->m2->record(cb,r);}
    catch(const std::exception& e){++device->m2->unsupported;device->m2->note(e.what());return false;}
}

// A non-indexed or indirect draw while the module wants every draw (docs/vulkan-native-mods.md): true drops it.
bool m2_shader_draw(Device* device,CmdBuf* cb,const DrawRec& r){
    if(!device->character_mode || !device->shader_draws.load(std::memory_order_relaxed))return false;
    std::shared_lock<std::shared_mutex> lock(device->character_mx);
    return device->character && device->character_module->shader_draw && device->character_module->shader_draw(*device->character,cb,r);
}

VkPipeline m2_bind(Device* device,CmdBuf* cb,VkPipeline pipeline){
    if(!device->character_mode)return pipeline;
    std::shared_lock<std::shared_mutex> lock(device->character_mx);
    return device->character && device->character_module->bind ? device->character_module->bind(*device->character,cb,pipeline) : pipeline;
}

void m2_created(Device* device,VkPipeline pipeline){
    if(!device->character_mode)return;
    std::shared_lock<std::shared_mutex> lock(device->character_mx);
    if(device->character && device->character_module->created)device->character_module->created(*device->character,pipeline);
}

void m2_pass_end(Device* device,CmdBuf* cb){
    if(!device->character_mode)return;
    std::shared_lock<std::shared_mutex> lock(device->character_mx);
    if(device->character && device->character_module->pass_end)device->character_module->pass_end(*device->character,cb);
}

void m2_create(Device* device,const VkPhysicalDeviceProperties& properties,const VkPhysicalDeviceMemoryProperties& memory)
{
    if(!character_configured() || g_cfg.mode!=Mode::Probe || !g_cfg.track_resources)return;
    // The file says which backend it is for: a whole character or the single-component package.
    // A directory names several character packages (one per Mod, lazy on the GPU); a Mods folder
    // names the character Mods themselves (D-078).
    std::error_code directory_error;
    if(!g_cfg.m2_mods.empty() || is_character_package(g_cfg.m2_package) ||
       std::filesystem::is_directory(std::filesystem::u8path(g_cfg.m2_package),directory_error)){
        device->physical_properties=properties;device->memory_properties=memory;
        device->character_module=builtin_character_module();
        device->character=device->character_module->create(device,properties,memory);
        device->character_mode=true;
        std::lock_guard<std::mutex> lock(g_module.mx);
        g_module.generation=0;g_module.path.clear();g_module.version=device->character_module->version;
        g_module.staged_path.clear();g_module.error.clear();g_module.enabled=-1;
        return;
    }
    try{
        auto backend=std::make_shared<M2Backend>(device,properties,memory);
        device->m2=backend;
        if(!backend->init()){logf("M2 initialization refused: %s",backend->last_reason.c_str());return;}
        logf("M2 package configured, match=%08x indices=%u enabled=%u; resources and variants prepare asynchronously",
             backend->profile.hash,backend->profile.match_count,backend->enabled.load()?1:0);
    }catch(const std::exception& e){
        if(device->m2){device->m2->enabled=false;device->m2->note(e.what());}
        logf("M2 initialization exception: %s",e.what());
    }
}
void m2_stop(Device* device){
    if(device->character_mode){
        std::shared_ptr<CharacterBackend> active,staged;
        const CharacterModule* active_module=nullptr;const CharacterModule* staged_module=nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(device->character_mx);
            device->character_stopped=true;
            active.swap(device->character);staged.swap(device->character_staged);
            active_module=device->character_module;staged_module=device->staged_module;device->staged_module=nullptr;
        }
        if(active)active_module->stop(*active);
        if(staged)staged_module->stop(*staged);
        // Backends that were exchanged earlier finish their own stop before the device goes.
        std::vector<std::thread> janitors;
        {std::lock_guard<std::mutex> lock(g_module.mx);janitors.swap(g_module.janitors);}
        for(auto& thread:janitors)if(thread.joinable())thread.join();
        active.reset();staged.reset();
    }
    if(device->m2)device->m2->stop();
}
void m2_retire(Device* device,VkPipeline handle){
    if(!device->character_mode && (!device->m2 || !device->m2->cache))return;
    uint32_t id=0;
    {std::shared_lock<std::shared_mutex> lock(device->res_mx);auto p=device->pipes.find(handle);if(p!=device->pipes.end())id=p->second.id;}
    if(!id)return;
    if(device->character_mode){
        std::shared_lock<std::shared_mutex> lock(device->character_mx);
        if(device->character)device->character_module->retire(*device->character,id);
        if(device->character_staged)device->staged_module->retire(*device->character_staged,id);
    }
    else device->m2->cache->retire(id);
}
void m2_seal(Device* device,uint32_t count,const VkCommandBuffer* buffers){
    if(!device->m2 && !device->character_mode)return;
    for(uint32_t i=0;i<count;++i)if(auto* cb=cb_of(device,buffers[i]))
        for(auto& owner:cb->owners)if(owner && owner->seal() && device->m2)++device->m2->batches;
}
VkResult m2_submit(Device* device,VkQueue queue,SubmitWork& work){
    if(!device->character_mode)return VK_SUCCESS;
    std::shared_lock<std::shared_mutex> lock(device->character_mx);
    auto put=[&](const std::shared_ptr<CharacterBackend>& backend,const CharacterModule* module){
        const size_t signals=work.signals.size();size_t prefixes=0;
        for(const auto& b:work.batches)prefixes+=b.prefix.size();
        const VkResult result=module->submit(*backend,queue,work);
        size_t after=0;for(const auto& b:work.batches)after+=b.prefix.size();
        if(after!=prefixes || work.signals.size()!=signals)work.owners.emplace_back(backend,module);
        return result;
    };
    if(device->character){
        const VkResult result=put(device->character,device->character_module);
        if(result!=VK_SUCCESS)return result;
    }
    // A staged backend brings its images up through the same queues before it takes over. If that
    // fails it never becomes ready and is given up; the application's submission is not its concern.
    if(device->character_staged)put(device->character_staged,device->staged_module);
    return VK_SUCCESS;
}
void m2_submitted(Device*,VkQueue queue,SubmitWork& work,VkResult result){
    for(auto& owner:work.owners)if(owner.second && owner.second->submitted)owner.second->submitted(*owner.first,queue,work,result);
    work.owners.clear();
}
void m2_frame(Device* device,bool had_draws,PresentWork* present){
    if(!device->character_mode)return;
    bool promote=false,late=false;
    {
        std::shared_lock<std::shared_mutex> lock(device->character_mx);
        if(device->character)device->character_module->frame(*device->character,had_draws,present);
        if(device->character_staged){
            promote=device->staged_module->ready(*device->character_staged);
            if(!promote){
                std::lock_guard<std::mutex> state(g_module.mx);
                late=host_time_us()-g_module.staged_us>kStagingMicroseconds;
            }
        }
    }
    if(promote || late)exchange(device,promote);
}
void m2_status(Device* device,JsonWriter& w){
    if(device && device->character_mode){
        std::shared_lock<std::shared_mutex> lock(device->character_mx);
        w.key("replacement");w.begin_object();
        if(device->character)device->character_module->status(*device->character,w);
        else{w.kv("kind","character");w.kb("configured",false);w.kb("enabled",false);w.kb("ready",false);}
        module_status(w);
        // The layer's work inside the application's batches (D-127).
        w.key("submission_work");w.begin_object();
        w.kn("prefixed_batches",device->prefixed_batches.load());w.kn("widened_waits",device->widened_waits.load());
        w.kn("prefix_alone",device->prefix_alone.load());w.end_object();
        w.kn("present_gpu_waits",device->present_waits.load()); // BUG-062: stays 0
        w.end_object();return;
    }
    w.key("replacement");w.begin_object();auto backend=device?device->m2:nullptr;
    w.kb("configured",bool(backend));w.kb("enabled",backend && backend->enabled.load());
    w.kb("ready",backend && backend->ready.load());
    if(backend){
        w.kn("matched",backend->matched.load());w.kn("replaced",backend->replaced.load());w.kn("pending",backend->pending.load());
        w.kn("unsupported",backend->unsupported.load());w.kn("unreadable",backend->unreadable.load());
        w.kn("exhausted",backend->exhausted.load());w.kn("sealed_batches",backend->batches.load());
        std::lock_guard<std::mutex> lock(backend->mutex);w.kv("last_reason",backend->last_reason);
        w.kn("available_batches",backend->available.size());
    }
    w.end_object();
}
std::string probe_m2_set(int enabled,bool advance){
    DeviceRef ref;JsonWriter w;w.begin_object();
    if(ref.dev && ref.dev->character_mode){
        if(enabled>=0){
            {std::lock_guard<std::mutex> lock(g_module.mx);g_module.enabled=enabled;}
            std::shared_lock<std::shared_mutex> lock(ref.dev->character_mx);
            if(ref.dev->character)ref.dev->character_module->enable(*ref.dev->character,enabled!=0);
        }
        if(advance)m2_frame(ref.dev,true);
        w.kb("ok",true);w.key("result");w.begin_object();m2_status(ref.dev,w);w.end_object();
    }
    else if(!ref.dev || !ref.dev->m2 || !ref.dev->m2->cache){w.kb("ok",false);w.kv("error","M2 package is not configured");}
    else{
        if(enabled>=0)ref.dev->m2->enabled=enabled!=0;
        w.kb("ok",true);w.key("result");w.begin_object();m2_status(ref.dev,w);w.end_object();
    }
    w.end_object();return w.out;
}
std::string probe_m2_module(const std::string& path,const std::string& package){
    DeviceRef ref;JsonWriter w;w.begin_object();
    Device* device=ref.dev;
    auto fail=[&](const std::string& why){
        {std::lock_guard<std::mutex> lock(g_module.mx);g_module.error=why+": "+path;}
        logf("replacement module refused: %s: %s",why.c_str(),path.c_str());
        w.kb("ok",false);w.kv("error",why);w.end_object();return w.out;
    };
    if(!device || !device->character_mode)return fail("this process has no character backend");
    {
        std::shared_lock<std::shared_mutex> lock(device->character_mx);
        if(device->character_staged)return fail("another module is being staged");
    }
    std::wstring wide(path.size()+1,L'\0');
    const int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,&wide[0],static_cast<int>(wide.size()));
    if(path.empty() || chars<=0)return fail("module path is not valid UTF-8");
    const HMODULE library=LoadLibraryW(wide.c_str());
    if(!library)return fail("module cannot be loaded, error "+std::to_string(GetLastError()));
    const auto entry=reinterpret_cast<CharacterModuleEntry>(GetProcAddress(library,EFVK_MODULE_ENTRY));
    // One staging at a time; the module copies the settings while its entry runs and keeps the
    // table of functions, which therefore lives as long as the process.
    static HostApi api;
    Config config=g_cfg;
    if(!package.empty())config.m2_package=package;
    api.size=sizeof(HostApi);api.abi=EFVK_ABI;api.config=&config;api.log=host_log;api.crc32c=crc32c;
    api.read_buffer_locked=host_read_buffer;api.collect_buffer_ranges_locked=collect_buffer_ranges_locked;
    api.gpu_bones=[]{return g_cfg.m2_gpu_bones;};
    api.notice=[](int level,const char* key,const char* text){notice(static_cast<NoticeLevel>(level),key,text);};
    const CharacterModule* module=entry?entry(&api):nullptr;
    api.config=nullptr;
    if(!module || module->size!=sizeof(CharacterModule) || module->abi!=EFVK_ABI){
        // Nothing of it ran beyond the entry, so it may go again.
        FreeLibrary(library);
        return fail("module was built against other structures of the layer; this layer cannot use it");
    }
    std::shared_ptr<CharacterBackend> staged=module->create(device,device->physical_properties,device->memory_properties);
    std::string reason;
    if(!staged || !module->usable(*staged,reason)){
        if(staged){module->stop(*staged);staged.reset();}
        return fail("module's backend refused: "+reason);
    }
    int enabled=-1;
    {std::lock_guard<std::mutex> lock(g_module.mx);enabled=g_module.enabled;}
    if(enabled>=0)module->enable(*staged,enabled!=0);
    {
        std::unique_lock<std::shared_mutex> lock(device->character_mx);
        if(device->character_staged || device->character_stopped){
            const bool stopped=device->character_stopped;
            lock.unlock();module->stop(*staged);staged.reset();
            return fail(stopped?"the device is being destroyed":"another module is being staged");
        }
        device->character_staged=staged;device->staged_module=module;
    }
    {
        std::lock_guard<std::mutex> lock(g_module.mx);
        g_module.staged_path=path;g_module.staged_version=module->version;g_module.staged_us=host_time_us();g_module.error.clear();
    }
    logf("replacement module staged: %s (%s)",path.c_str(),module->version);
    w.kb("ok",true);w.key("result");w.begin_object();w.kb("staged",true);w.kv("version",module->version);module_status(w);w.end_object();
    w.end_object();return w.out;
}
} // namespace efvk
