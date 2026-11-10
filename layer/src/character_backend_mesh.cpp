// A package's mesh on the device (GpuMesh): its streams read from their files into device memory, mapped
// or through a copy on the layer's upload queue (ISSUE-011, BUG-075).
#include "character_backend_internal.h"

namespace efvk {
namespace {
// A test's child process may set it: the host-visible device memory taken for a heap of its own of
// 256 MiB, as a card without resizable BAR shows it (BUG-075), so that the copy is exercised here.
bool test_small_bar()
{
    static const bool small = GetEnvironmentVariableW(L"EFMI_VK_TEST_SMALL_BAR", nullptr, 0) != 0;
    return small;
}

VkPhysicalDeviceMemoryProperties as_placed(const VkPhysicalDeviceMemoryProperties& properties)
{
    VkPhysicalDeviceMemoryProperties seen = properties;
    if (!test_small_bar() || seen.memoryHeapCount >= VK_MAX_MEMORY_HEAPS) return seen;
    const uint32_t window = seen.memoryHeapCount++;
    seen.memoryHeaps[window] = {256ull << 20, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    for (uint32_t i = 0; i < seen.memoryTypeCount; ++i)
        if ((seen.memoryTypes[i].propertyFlags & visible) == visible) seen.memoryTypes[i].heapIndex = window;
    return seen;
}
} // namespace

namespace character_internal {
// The package's streams go from their files straight into the mapped memory, as the original reads
// a buffer file into the resource it creates and keeps no copy (ISSUE-011); where the host does not
// see the device's memory, into a staging buffer that a copy on the layer's upload queue empties.
bool GpuMesh::create(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, std::string& reason)
{
    auto good = [&](VkResult result, const char* text) { if (result == VK_SUCCESS) return true; reason = text; return false; };
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // Room behind the last stream: an element may reach past its file's stride (kCharacterOvershoot).
    const size_t padding = 256;
    const size_t bytes = static_cast<size_t>(package.payload_bytes);
    ci.size = bytes + padding; ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!good(d.CreateBuffer(device, &ci, nullptr, &buffer), "create character mesh buffer")) return false;
    VkMemoryRequirements req; d.GetBufferMemoryRequirements(device, buffer, &req);
    if (req.size > (768ull << 20)) { reason = "character mesh allocation budget exceeded"; return false; }
    // A whole character is read by the device in every pass of every frame: it belongs in device
    // memory. Host memory works, but far slower; it is the fallback, and the status says which.
    const bool can_copy = owner && owner->upload_queue && owner->upload_family != UINT32_MAX && !owner->material_queues.empty();
    const MeshPlacement place = mesh_placement(as_placed(properties), req.memoryTypeBits, can_copy);
    if (place.type == UINT32_MAX) { reason = "character mesh memory unavailable"; return false; }
    placement = place.memory; device_local = placement != MeshMemory::Host;
    if (placement == MeshMemory::Copied) {
        // The upload family writes it, the application's graphics family reads it.
        const uint32_t families[2] = {owner->material_family, owner->upload_family};
        d.DestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE;
        ci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (families[0] != families[1]) { ci.sharingMode = VK_SHARING_MODE_CONCURRENT; ci.queueFamilyIndexCount = 2; ci.pQueueFamilyIndices = families; }
        if (!good(d.CreateBuffer(device, &ci, nullptr, &buffer), "create character mesh buffer")) return false;
        d.GetBufferMemoryRequirements(device, buffer, &req);
        if (!(req.memoryTypeBits & (1u << place.type))) { reason = "character mesh memory type changed with its copy usage"; return false; }
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = place.type;
    if (!good(d.AllocateMemory(device, &ai, nullptr, &memory), "allocate character mesh memory") ||
        !good(d.BindBufferMemory(device, buffer, memory, 0), "bind character mesh memory")) return false;
    if (placement == MeshMemory::Copied) return copy(package, properties, owner, bytes + padding, reason);
    void* mapped = nullptr;
    if (!good(d.MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map character mesh memory")) return false;
    const bool read = package.read_payload(static_cast<uint8_t*>(mapped), reason);
    std::memset(static_cast<uint8_t*>(mapped) + bytes, 0, padding);
    d.UnmapMemory(device, memory);
    return read;
}

#define EFVK_MESH_COPY_FUNCS(X) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
// The files into a staging buffer in system memory, then one copy on the upload queue (D-103); the
// worker waits for it, so the mesh is published complete. The staging memory goes right after.
bool GpuMesh::copy(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, VkDeviceSize size,
                   std::string& reason)
{
    struct {
#define X(n) PFN_vk##n n = nullptr;
        EFVK_MESH_COPY_FUNCS(X)
#undef X
    } f;
#define X(n) f.n = reinterpret_cast<PFN_vk##n>(owner->owned_gdpa(device, "vk" #n)); if (!f.n) { reason = "missing vk" #n " for the mesh copy"; return false; }
    EFVK_MESH_COPY_FUNCS(X)
#undef X
    VkBuffer stage = VK_NULL_HANDLE; VkDeviceMemory stage_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    auto done = [&](bool result) {
        if (fence) f.DestroyFence(device, fence, nullptr);
        if (pool) f.DestroyCommandPool(device, pool, nullptr);
        if (stage) d.DestroyBuffer(device, stage, nullptr);
        if (stage_memory) d.FreeMemory(device, stage_memory, nullptr);
        return result;
    };
    auto good = [&](VkResult result, const char* text) { if (result == VK_SUCCESS) return true; reason = text; return false; };
    VkBufferCreateInfo si = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; si.size = size; si.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!good(d.CreateBuffer(device, &si, nullptr, &stage), "create character mesh staging buffer")) return done(false);
    VkMemoryRequirements req; d.GetBufferMemoryRequirements(device, stage, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = host_memory_type(properties, req.memoryTypeBits, false);
    if (ai.memoryTypeIndex == UINT32_MAX) { reason = "character mesh staging memory unavailable"; return done(false); }
    void* mapped = nullptr;
    if (!good(d.AllocateMemory(device, &ai, nullptr, &stage_memory), "allocate character mesh staging memory") ||
        !good(d.BindBufferMemory(device, stage, stage_memory, 0), "bind character mesh staging memory") ||
        !good(d.MapMemory(device, stage_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map character mesh staging memory")) return done(false);
    const size_t bytes = static_cast<size_t>(package.payload_bytes);
    const bool read = package.read_payload(static_cast<uint8_t*>(mapped), reason);
    std::memset(static_cast<uint8_t*>(mapped) + bytes, 0, static_cast<size_t>(size) - bytes);
    d.UnmapMemory(device, stage_memory);
    if (!read) return done(false);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pi.queueFamilyIndex = owner->upload_family;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
    if (!good(f.CreateCommandPool(device, &pi, nullptr, &pool), "create character mesh copy pool")) return done(false);
    ci.commandPool = pool;
    if (!good(f.AllocateCommandBuffers(device, &ci, &command), "allocate character mesh copy")) return done(false);
    if (owner->set_loader_data && !good(owner->set_loader_data(device, command), "initialize character mesh copy dispatch")) return done(false);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!good(f.BeginCommandBuffer(command, &bi), "begin character mesh copy")) return done(false);
    const VkBufferCopy region = {0, 0, size};
    f.CmdCopyBuffer(command, stage, buffer, 1, &region);
    // The vertex and index reads of every queue that draws it after the worker saw the fence.
    VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    f.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         (owner->upload_flags & VK_QUEUE_GRAPHICS_BIT) ? VK_PIPELINE_STAGE_VERTEX_INPUT_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    if (!good(f.EndCommandBuffer(command), "end character mesh copy")) return done(false);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!good(f.CreateFence(device, &fi, nullptr, &fence), "create character mesh copy fence")) return done(false);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    {
        std::lock_guard<std::mutex> lock(owner->upload_mx);
        submitted = good(f.QueueSubmit(owner->upload_queue, 1, &submit, fence), "submit character mesh copy");
    }
    if (!submitted) return done(false);
    return done(good(f.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for character mesh copy"));
}
#undef EFVK_MESH_COPY_FUNCS
} // namespace character_internal

} // namespace efvk
