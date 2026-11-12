#include "bone_merge.h"
#include "merged_skeleton.h"
#include "draw_snapshots.h"
#include "json.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace efvk {
extern const uint32_t bone_merge_spirv[];
extern const size_t bone_merge_spirv_words;

#define EFVK_BONE_FUNCS(X) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) X(CmdUpdateBuffer) X(CmdFillBuffer) X(CmdCopyBuffer) \
    X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory)

namespace {
constexpr uint32_t kCommandBuffers = 32; // batches of ours in flight (several per frame when the game submits in parts)
constexpr uint32_t kJobsPerSubmission = 1024;
constexpr uint32_t kQueues = 4;          // graphics queues a game may submit on
constexpr uint32_t kImplicitBytes = MergedSkeleton::kImplicitBones * MergedSkeleton::kBytesPerBone; // 24576, within vkCmdUpdateBuffer's limit
}

// What the rings and remaps outlive the merge with: the device entry points and the timelines that
// say which submissions completed. One timeline per queue: a timeline's signals must happen in
// increasing order, which two queues executing independently do not keep (BUG-052).
struct BoneContext {
#define X(n) PFN_vk##n n = nullptr;
    EFVK_BONE_FUNCS(X)
#undef X
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    struct QueueTimeline { VkQueue queue = VK_NULL_HANDLE; VkSemaphore timeline = VK_NULL_HANDLE; uint64_t submitted = 0; };
    QueueTimeline queues[kQueues];
    std::atomic<uint32_t> queue_count{0}; // entries below are complete once counted (release)
    std::atomic<uint64_t> ring_bytes{0};  // device memory of every ring, which may outlive the merge
    // The queue's index, made on its first submission (under the merge's lock); UINT32_MAX when the
    // device has more graphics queues than the layer follows.
    uint32_t index_of(VkQueue queue)
    {
        const uint32_t count = queue_count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i) if (queues[i].queue == queue) return i;
        if (count >= kQueues || !CreateSemaphore) return UINT32_MAX;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; si.pNext = &type;
        VkSemaphore timeline = VK_NULL_HANDLE;
        if (CreateSemaphore(device, &si, nullptr, &timeline) != VK_SUCCESS) return UINT32_MAX;
        queues[count].queue = queue; queues[count].timeline = timeline; queues[count].submitted = 0;
        queue_count.store(count + 1, std::memory_order_release);
        return count;
    }
    uint64_t completed(uint32_t index) const
    {
        if (index >= queue_count.load(std::memory_order_acquire)) return 0;
        uint64_t value = 0;
        return GetSemaphoreCounterValue(device, queues[index].timeline, &value) == VK_SUCCESS ? value : 0;
    }
    bool done(const BoneSubmission& s) const { return s.queue == UINT32_MAX || s.value <= completed(s.queue); }
    // Waits on the host for a submission of ours, briefly; false when it did not complete in time.
    bool wait(const BoneSubmission& s, uint64_t nanoseconds) const
    {
        if (done(s)) return true;
        if (!WaitSemaphores) return false;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1; wi.pSemaphores = &queues[s.queue].timeline; wi.pValues = &s.value;
        return WaitSemaphores(device, &wi, nanoseconds) == VK_SUCCESS;
    }
    ~BoneContext()
    {
        const uint32_t count = queue_count.load();
        for (uint32_t i = 0; i < count; ++i) if (queues[i].timeline) DestroySemaphore(device, queues[i].timeline, nullptr);
    }
};

namespace {
struct Push { uint32_t source_rows0, source_rows1, destination, pose_rows, first, count, flags, remap_offset; float scale; };

VkDeviceSize aligned(VkDeviceSize value, VkDeviceSize alignment) { alignment = std::max<VkDeviceSize>(alignment, 1); return (value + alignment - 1) / alignment * alignment; }
uint32_t device_local_type(const VkPhysicalDeviceMemoryProperties& p, uint32_t bits)
{
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i) if (bits >> i & 1) return i;
    return UINT32_MAX;
}
const std::vector<float>& identity_rows()
{
    static const std::vector<float> rows = [] {
        std::vector<float> out(MergedSkeleton::kImplicitBones * 12, 0.0f);
        for (uint32_t b = 0; b < MergedSkeleton::kImplicitBones; ++b) { out[b * 12 + 0] = 1; out[b * 12 + 5] = 1; out[b * 12 + 10] = 1; }
        return out;
    }();
    return rows;
}
} // namespace

bool BoneRing::grow()
{
    const uint32_t index = chunk_count.load(std::memory_order_relaxed);
    if (index >= kChunks || !ctx || memory_type == UINT32_MAX) return false;
    VkDevice dev = ctx->device;
    Chunk chunk;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = slot_stride * kSlotsPerChunk;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (ctx->CreateBuffer(dev, &bi, nullptr, &chunk.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req; ctx->GetBufferMemoryRequirements(dev, chunk.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type;
    if (!(req.memoryTypeBits >> memory_type & 1) || ctx->AllocateMemory(dev, &ai, nullptr, &chunk.memory) != VK_SUCCESS) {
        ctx->DestroyBuffer(dev, chunk.buffer, nullptr); return false;
    }
    if (ctx->BindBufferMemory(dev, chunk.buffer, chunk.memory, 0) != VK_SUCCESS) {
        ctx->DestroyBuffer(dev, chunk.buffer, nullptr); ctx->FreeMemory(dev, chunk.memory, nullptr); return false;
    }
    chunks_[index] = chunk;
    held.resize(size_t(index + 1) * kSlotsPerChunk, 0);
    written.resize(size_t(index + 1) * kSlotsPerChunk, std::vector<BoneSubmission>(kQueues));
    chunk_count.store(index + 1, std::memory_order_release);
    if (bytes) bytes->fetch_add(req.size, std::memory_order_relaxed);
    return true;
}
uint32_t BoneRing::acquire()
{
    std::lock_guard<std::mutex> lock(mx);
    for (;;) {
        const uint32_t slots = chunk_count.load(std::memory_order_relaxed) * kSlotsPerChunk;
        for (uint32_t s = 1; s < slots; ++s) {
            if (held[s]) continue;
            bool done = true;
            for (const auto& w : written[s]) done = done && ctx->done(w);
            if (done) { held[s] = 1; return s; }
        }
        if (!grow()) return UINT32_MAX;
    }
}
void BoneRing::release(uint32_t slot) { std::lock_guard<std::mutex> lock(mx); if (slot < held.size()) held[slot] = 0; }
BoneRing::~BoneRing()
{
    if (!ctx) return;
    // A copy of a batch that was never submitted, or of a generation that went, may still run: the
    // ring waits for its last writes before it goes.
    for (const auto& slot : written) for (const auto& w : slot) if (!ctx->done(w)) ctx->wait(w, 1000ull * 1000 * 1000);
    const uint32_t count = chunk_count.load();
    for (uint32_t i = 0; i < count; ++i) {
        if (chunks_[i].buffer) ctx->DestroyBuffer(ctx->device, chunks_[i].buffer, nullptr);
        if (chunks_[i].memory) ctx->FreeMemory(ctx->device, chunks_[i].memory, nullptr);
    }
}
BoneRemaps::~BoneRemaps()
{
    if (!ctx) return;
    if (buffer) ctx->DestroyBuffer(ctx->device, buffer, nullptr);
    if (memory) ctx->FreeMemory(ctx->device, memory, nullptr);
}

BoneMerge::BoneMerge(Device* device, const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceMemoryProperties& memory)
    : ctx_(std::make_shared<BoneContext>()), memory_(memory)
{
    ctx_->device = device->handle;
    storage_alignment_ = std::max<VkDeviceSize>(properties.limits.minStorageBufferOffsetAlignment, 16);
    bool missing = false;
#define X(n) ctx_->n = reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle, "vk" #n)); missing |= !ctx_->n;
    EFVK_BONE_FUNCS(X)
#undef X
    if (missing) { reason_ = "missing a device entry point"; return; }
    if (!device->timeline_semaphores) { reason_ = "the device has no timeline semaphores"; return; }
    ctx_->GetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device->owned_gdpa(device->handle, "vkGetSemaphoreCounterValue"));
    if (!ctx_->GetSemaphoreCounterValue)
        ctx_->GetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device->owned_gdpa(device->handle, "vkGetSemaphoreCounterValueKHR"));
    ctx_->WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(device->owned_gdpa(device->handle, "vkWaitSemaphores"));
    if (!ctx_->WaitSemaphores) ctx_->WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(device->owned_gdpa(device->handle, "vkWaitSemaphoresKHR"));
    if (!ctx_->GetSemaphoreCounterValue || !ctx_->WaitSemaphores) { reason_ = "missing timeline semaphore entry points"; return; }
    if (!make_pipeline()) return;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = device->material_family;
    if (ctx_->CreateCommandPool(ctx_->device, &pi, nullptr, &pool_) != VK_SUCCESS) { reason_ = "cannot create the bone command pool"; return; }
    std::vector<VkCommandBuffer> cbs(kCommandBuffers);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = kCommandBuffers;
    if (ctx_->AllocateCommandBuffers(ctx_->device, &ai, cbs.data()) != VK_SUCCESS) { reason_ = "cannot allocate the bone command buffers"; return; }
    // Dispatchable objects a layer makes get the loader's dispatch pointer from the layer (the layers
    // below read it; without it the validation layer faulted in the layer entry).
    if (device->set_loader_data) for (auto cb : cbs) device->set_loader_data(device->handle, cb);
    slots_.resize(kCommandBuffers);
    for (uint32_t i = 0; i < kCommandBuffers; ++i) {
        slots_[i].cb = cbs[i];
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kJobsPerSubmission * 3};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = kJobsPerSubmission; dp.poolSizeCount = 1; dp.pPoolSizes = &size;
        if (ctx_->CreateDescriptorPool(ctx_->device, &dp, nullptr, &slots_[i].pool) != VK_SUCCESS) { reason_ = "cannot create a bone descriptor pool"; return; }
    }
    usable_ = true;
}

BoneMerge::~BoneMerge()
{
    VkDevice dev = ctx_->device;
    // A batch of ours the device may still run (the backend went with the generation exchange).
    for (const auto& s : slots_) if (!ctx_->done(s.last)) ctx_->wait(s.last, 1000ull * 1000 * 1000);
    for (auto& s : slots_) if (s.pool) ctx_->DestroyDescriptorPool(dev, s.pool, nullptr);
    if (pool_) ctx_->DestroyCommandPool(dev, pool_, nullptr);
    if (pipeline_) ctx_->DestroyPipeline(dev, pipeline_, nullptr);
    if (layout_) ctx_->DestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) ctx_->DestroyDescriptorSetLayout(dev, set_layout_, nullptr);
}

bool BoneMerge::make_pipeline()
{
    VkDevice dev = ctx_->device;
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3; li.pBindings = bindings;
    if (ctx_->CreateDescriptorSetLayout(dev, &li, nullptr, &set_layout_) != VK_SUCCESS) { reason_ = "bone set layout"; return false; }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout_; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
    if (ctx_->CreatePipelineLayout(dev, &pl, nullptr, &layout_) != VK_SUCCESS) { reason_ = "bone pipeline layout"; return false; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bone_merge_spirv_words * 4; mi.pCode = bone_merge_spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (ctx_->CreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) { reason_ = "bone shader module"; return false; }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
    ci.layout = layout_;
    const VkResult result = ctx_->CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_);
    ctx_->DestroyShaderModule(dev, module, nullptr);
    if (result != VK_SUCCESS) { reason_ = "bone compute pipeline"; return false; }
    return true;
}

std::shared_ptr<BoneRing> BoneMerge::ring(uint32_t bones, std::string& why)
{
    if (!usable_) { why = reason_; return nullptr; }
    if (!bones || bones > 65536) { why = "bone ring: invalid skeleton"; return nullptr; }
    auto r = std::make_shared<BoneRing>();
    r->ctx = ctx_; r->bones = bones; r->bytes = &ctx_->ring_bytes;
    r->slot_bytes = VkDeviceSize(MergedSkeleton::kImplicitBones + 2ull * bones) * MergedSkeleton::kBytesPerBone;
    r->slot_stride = aligned(r->slot_bytes, storage_alignment_);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = r->slot_stride * BoneRing::kSlotsPerChunk;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer probe = VK_NULL_HANDLE;
    if (ctx_->CreateBuffer(ctx_->device, &bi, nullptr, &probe) != VK_SUCCESS) { why = "bone ring: create buffer"; return nullptr; }
    VkMemoryRequirements req; ctx_->GetBufferMemoryRequirements(ctx_->device, probe, &req);
    ctx_->DestroyBuffer(ctx_->device, probe, nullptr);
    r->memory_type = device_local_type(memory_, req.memoryTypeBits);
    {
        std::lock_guard<std::mutex> lock(r->mx);
        if (!r->grow()) { why = "bone ring: allocate memory"; return nullptr; }
    }
    rings_.fetch_add(1, std::memory_order_relaxed);
    return r;
}

std::shared_ptr<BoneRemaps> BoneMerge::remaps(const std::vector<std::vector<uint16_t>>& tables, std::string& why)
{
    if (!usable_) { why = reason_; return nullptr; }
    auto r = std::make_shared<BoneRemaps>();
    r->ctx = ctx_;
    std::vector<uint32_t> words;
    for (const auto& table : tables) {
        r->offsets.push_back(table.empty() ? UINT32_MAX : uint32_t(words.size()));
        for (uint16_t source : table) words.push_back(source);
    }
    if (words.empty()) words.resize(4, 0); // a valid binding for the jobs that use no table
    VkDevice dev = ctx_->device;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = words.size() * 4; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (ctx_->CreateBuffer(dev, &bi, nullptr, &r->buffer) != VK_SUCCESS) { why = "bone remaps: create buffer"; return nullptr; }
    VkMemoryRequirements req; ctx_->GetBufferMemoryRequirements(dev, r->buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = host_memory_type(memory_, req.memoryTypeBits, false);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = host_memory_type(memory_, req.memoryTypeBits, true);
    if (ai.memoryTypeIndex == UINT32_MAX || ctx_->AllocateMemory(dev, &ai, nullptr, &r->memory) != VK_SUCCESS) { why = "bone remaps: allocate memory"; return nullptr; }
    if (ctx_->BindBufferMemory(dev, r->buffer, r->memory, 0) != VK_SUCCESS) { why = "bone remaps: bind memory"; return nullptr; }
    void* mapped = nullptr;
    if (ctx_->MapMemory(dev, r->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) { why = "bone remaps: map memory"; return nullptr; }
    std::memcpy(mapped, words.data(), words.size() * 4);
    ctx_->UnmapMemory(dev, r->memory);
    return r;
}

void BoneMerge::fail(const std::string& why)
{
    failures_.fetch_add(1, std::memory_order_relaxed);
    if (why != error_) logf("bones: %s", why.c_str());
    error_ = why;
}

bool BoneMerge::record(Slot& slot, const BoneBatch& batch, std::string& why)
{
    VkCommandBuffer cmd = slot.cb;
    if (ctx_->ResetCommandBuffer(cmd, 0) != VK_SUCCESS || ctx_->ResetDescriptorPool(ctx_->device, slot.pool, 0) != VK_SUCCESS) { why = "bone command buffer reset"; return false; }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (ctx_->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { why = "bone command buffer begin"; return false; }
    // What came before on this queue (earlier batches' imports, copies and the draws that read the
    // snapshots) and, through the batch's waits, on the other queue, comes first.
    {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        ctx_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    // A persistent skeleton's first use: zero (bones no component imports stay zero, as in the CPU
    // master) and the implicit identity matrices written.
    bool initialized_any = false;
    std::set<BoneRing*> seen;
    auto initialize = [&](BoneRing& ring) {
        if (!seen.insert(&ring).second) return;
        bool fresh;
        { std::lock_guard<std::mutex> lock(ring.mx); fresh = !ring.initialized; ring.initialized = true; }
        if (!fresh) return;
        initialized_any = true;
        ctx_->CmdFillBuffer(cmd, ring.buffer(0), ring.offset(0), ring.slot_stride, 0);
        VkMemoryBarrier fill{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; fill.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; fill.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ctx_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &fill, 0, nullptr, 0, nullptr);
        ctx_->CmdUpdateBuffer(cmd, ring.buffer(0), ring.offset(0), kImplicitBytes, identity_rows().data());
    };
    for (const auto& job : batch.jobs) initialize(*job.ring);
    for (const auto& copy : batch.copies) initialize(*copy.ring);
    if (initialized_any) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ctx_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    // The imports into the persistent skeletons, in the order the recordings drew them.
    if (!batch.jobs.empty()) {
        ctx_->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        for (const BoneJob& job : batch.jobs) {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = slot.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set_layout_;
            if (ctx_->AllocateDescriptorSets(ctx_->device, &ai, &set) != VK_SUCCESS) { why = "bone descriptor set"; return false; }
            const VkDescriptorBufferInfo infos[3] = {
                {job.source, job.source_offset, job.source_size},
                {job.remaps ? job.remaps->buffer : job.ring->buffer(0), 0, VK_WHOLE_SIZE},
                {job.ring->buffer(0), job.ring->offset(0), job.ring->slot_bytes}};
            VkWriteDescriptorSet writes[3] = {};
            for (uint32_t b = 0; b < 3; ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[b].dstSet = set; writes[b].dstBinding = b;
                writes[b].descriptorCount = 1; writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[b].pBufferInfo = &infos[b];
            }
            ctx_->UpdateDescriptorSets(ctx_->device, 3, writes, 0, nullptr);
            Push push{};
            push.source_rows0 = job.rows[0]; push.source_rows1 = job.rows[1];
            push.destination = MergedSkeleton::kStaticRows; push.pose_rows = job.ring->bones * MergedSkeleton::kRowsPerBone;
            push.first = job.first; push.count = job.count;
            const uint32_t table = job.table == UINT32_MAX ? job.component : job.table;
            push.flags = (job.poses & 3) | (job.remap && job.remaps && table < job.remaps->offsets.size() &&
                                            job.remaps->offsets[table] != UINT32_MAX ? 4u : 0u);
            push.remap_offset = push.flags & 4 ? job.remaps->offsets[table] : 0;
            push.scale = 1.0f;
            ctx_->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
            ctx_->CmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            ctx_->CmdDispatch(cmd, (job.count + 63) / 64, 1, 1);
            dispatches_.fetch_add(1, std::memory_order_relaxed);
        }
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ctx_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    // Each recording's snapshot of the skeletons as the batch's imports left them.
    for (const BoneCopy& copy : batch.copies) {
        VkBufferCopy region{copy.ring->offset(0), copy.ring->offset(copy.slot), copy.ring->slot_bytes};
        ctx_->CmdCopyBuffer(cmd, copy.ring->buffer(0), copy.ring->buffer(copy.slot), 1, &region);
        copies_.fetch_add(1, std::memory_order_relaxed);
    }
    // The recordings' draws read the snapshots in the application's command buffers after this one.
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ctx_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &m, 0, nullptr, 0, nullptr);
    if (ctx_->EndCommandBuffer(cmd) != VK_SUCCESS) { why = "bone command buffer end"; return false; }
    return true;
}

bool BoneMerge::prepare(VkQueue queue, std::vector<BoneBatch>& batches, VkSemaphore& semaphore, uint64_t& value)
{
    bool any = false;
    for (auto& batch : batches) { batch.cb = VK_NULL_HANDLE; any = any || !batch.jobs.empty() || !batch.copies.empty(); }
    if (!any) return false;
    std::lock_guard<std::mutex> lock(mx_);
    if (!usable_) return false;
    const uint32_t index = ctx_->index_of(queue);
    if (index == UINT32_MAX) { fail("bone dispatches: more graphics queues than the layer follows"); return false; }
    auto& timeline = ctx_->queues[index];
    const BoneSubmission mine{index, timeline.submitted + 1};
    bool recorded = false;
    for (auto& batch : batches) {
        if (batch.jobs.empty() && batch.copies.empty()) continue;
        if (batch.jobs.size() > kJobsPerSubmission) { fail("bone dispatches: more imports in one batch than a command buffer of ours holds"); batch.jobs.resize(kJobsPerSubmission); }
        Slot& slot = slots_[submissions_total_ % kCommandBuffers];
        if (!ctx_->done(slot.last)) {
            // As many batches of ours behind the device as there are command buffers: wait for the oldest, briefly.
            slot_waits_.fetch_add(1, std::memory_order_relaxed);
            if (!ctx_->wait(slot.last, 500ull * 1000 * 1000)) { fail("bone submission ring stalled"); continue; }
        }
        std::string why;
        if (!record(slot, batch, why)) { fail(why); continue; }
        slot.last = mine; ++submissions_total_;
        batch.cb = slot.cb; recorded = true;
        for (const auto& copy : batch.copies) {
            std::lock_guard<std::mutex> ring_lock(copy.ring->mx);
            if (copy.slot < copy.ring->written.size()) copy.ring->written[copy.slot][index] = mine;
        }
        for (const auto& job : batch.jobs) if (job.component < 64) job.ring->submitted.fetch_or(1ull << job.component, std::memory_order_acq_rel);
        jobs_.fetch_add(batch.jobs.size(), std::memory_order_relaxed);
        submissions_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!recorded) return false;
    timeline.submitted = mine.value;
    semaphore = timeline.timeline; value = mine.value;
    return true;
}

void BoneMerge::finish(VkQueue queue, VkSemaphore semaphore, uint64_t value, bool submitted)
{
    if (submitted) return;
    std::lock_guard<std::mutex> lock(mx_);
    const uint32_t index = ctx_->index_of(queue);
    if (index == UINT32_MAX || ctx_->queues[index].timeline != semaphore) return;
    failed_calls_.fetch_add(1, std::memory_order_relaxed);
    // Nothing of the call reached the queue: its command buffers are free again and the next call takes
    // its value. A snapshot marked with the value is reused once the next call's signal completes.
    if (ctx_->queues[index].submitted == value) ctx_->queues[index].submitted = value - 1;
    for (auto& slot : slots_) if (slot.last.queue == index && slot.last.value == value) slot.last = BoneSubmission{};
}

void BoneMerge::status(JsonWriter& w)
{
    w.key("bones"); w.begin_object();
    w.kb("gpu", live_gpu_bones()); w.kb("usable", usable_); w.kv("reason", reason_);
    w.kn("jobs", jobs_.load()); w.kn("dispatches", dispatches_.load()); w.kn("copies", copies_.load()); w.kn("borrowed", borrowed_.load());
    w.kn("submissions", submissions_.load()); w.kn("rings", rings_.load()); w.kn("ring_bytes", ctx_->ring_bytes.load());
    w.kn("slot_waits", slot_waits_.load()); w.kn("failures", failures_.load()); w.kn("failed_calls", failed_calls_.load());
    const uint32_t count = ctx_->queue_count.load();
    w.kn("queues", count);
    uint64_t submitted = 0, completed = 0;
    for (uint32_t i = 0; i < count; ++i) { submitted += ctx_->queues[i].submitted; completed += ctx_->completed(i); }
    w.kn("submitted", submitted); w.kn("completed", completed);
    { std::lock_guard<std::mutex> lock(mx_); w.kv("last_error", error_); }
    w.end_object();
}

} // namespace efvk
