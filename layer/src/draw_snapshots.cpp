#include "draw_snapshots.h"
#include <algorithm>
#include <chrono>

namespace efvk { SnapshotTiming g_snapshot_timing; }
namespace {
uint64_t clock_us() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}

namespace efvk {
namespace {
VkDeviceSize aligned(VkDeviceSize value, VkDeviceSize alignment)
{
    alignment = std::max<VkDeviceSize>(alignment, 1);
    return (value + alignment - 1) / alignment * alignment;
}
}
DrawSnapshots::~DrawSnapshots() { clear(); }
void DrawSnapshots::clear()
{
    if (pool_) d_.DestroyDescriptorPool(device_, pool_, nullptr);
    if (layout_) d_.DestroyDescriptorSetLayout(device_, layout_, nullptr);
    if (mapped_) d_.UnmapMemory(device_, memory_);
    if (buffer_) d_.DestroyBuffer(device_, buffer_, nullptr);
    if (memory_) d_.FreeMemory(device_, memory_, nullptr);
    pool_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; buffer_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE;
    mapped_ = nullptr; sets_.clear(); image_owners_.clear(); bytes_ = 0; capacity_ = used_ = image_count_ = 0;
    shared_ = false; slice_capacity_ = slices_used_ = 0; slices_by_key_.clear();
}

bool DrawSnapshots::create(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceLimits& limits,
                           uint32_t capacity, uint32_t bone_bytes, uint32_t uniform_range,uint32_t image_count)
{
    if (capacity > 128) { reason = "snapshot dimensions exceed supported limits"; return false; }
    return create_storage(memory, limits, capacity, capacity, bone_bytes, uniform_range, image_count, false);
}

bool DrawSnapshots::create_shared(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceLimits& limits,
                                  uint32_t entries, uint32_t slices, uint32_t bone_bytes, uint32_t uniform_range, uint32_t image_count)
{
    if (!slices || slices > 128 || entries > 1024) { reason = "snapshot dimensions exceed supported limits"; return false; }
    return create_storage(memory, limits, entries, slices, bone_bytes, uniform_range, image_count, true);
}

bool DrawSnapshots::create_storage(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceLimits& limits,
                                   uint32_t capacity, uint32_t slices, uint32_t bone_bytes, uint32_t uniform_range,
                                   uint32_t image_count, bool shared)
{
    reason.clear();
    if (buffer_ || sealed_) { reason = "snapshot storage cannot be recreated"; return false; }
    if (!capacity || !bone_bytes || bone_bytes > limits.maxStorageBufferRange ||
        uniform_range < 256 || uniform_range > limits.maxUniformBufferRange || uniform_range > 65536 ||
        limits.minStorageBufferOffsetAlignment > (1u<<20) || limits.minUniformBufferOffsetAlignment > (1u<<20) ||
        image_count>32 || image_count>limits.maxPerStageDescriptorSampledImages || image_count>limits.maxDescriptorSetSampledImages ||
        image_count>limits.maxPerStageResources) {
        reason = "snapshot dimensions exceed supported limits"; return false;
    }
    bone_stride_ = aligned(bone_bytes, limits.minStorageBufferOffsetAlignment);
    entry_base_ = aligned(bone_stride_ * slices, limits.minUniformBufferOffsetAlignment);
    entry_stride_ = aligned(256, limits.minUniformBufferOffsetAlignment);
    bytes_ = entry_base_ + entry_stride_ * (capacity - 1) + uniform_range;
    if (bytes_ > (64ull<<20)) { reason = "snapshot byte budget exceeded"; bytes_ = 0; return false; }
    auto good = [&](VkResult result, const char* label) {
        if (result == VK_SUCCESS) return true;
        reason = label; clear(); return false;
    };
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes_; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (!good(d_.CreateBuffer(device_, &bi, nullptr, &buffer_), "create snapshot buffer")) return false;
    VkMemoryRequirements req; d_.GetBufferMemoryRequirements(device_, buffer_, &req);
    if (req.size > (128ull<<20)) { reason = "snapshot allocation budget exceeded"; clear(); return false; }
    // Host memory the device reads over the bus: a replaced draw reads its bone slice and entry once,
    // tens of kilobytes; device memory stays the application's and the textures' (D-094).
    uint32_t type = host_memory_type(memory, req.memoryTypeBits, false);
    if (type == UINT32_MAX) type = host_memory_type(memory, req.memoryTypeBits, true);
    if (type == UINT32_MAX) { reason = "host-coherent snapshot memory unavailable"; clear(); return false; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    if (!good(d_.AllocateMemory(device_, &ai, nullptr, &memory_), "allocate snapshot memory")) return false;
    if (!good(d_.BindBufferMemory(device_, buffer_, memory_, 0), "bind snapshot memory")) return false;
    void* mapped = nullptr;
    if (!good(d_.MapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped), "map snapshot memory")) return false;
    mapped_ = static_cast<uint8_t*>(mapped);
    std::memset(mapped_, 0, static_cast<size_t>(bytes_));
    const auto bindings=private_draw_bindings(image_count);
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(bindings.size()); li.pBindings = bindings.data();
    if (!good(d_.CreateDescriptorSetLayout(device_, &li, nullptr, &layout_), "create snapshot layout")) return false;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,capacity},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,capacity},
                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,capacity*image_count}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = capacity; pi.poolSizeCount = image_count?3:2; pi.pPoolSizes = sizes;
    if (!good(d_.CreateDescriptorPool(device_, &pi, nullptr, &pool_), "create snapshot pool")) return false;
    std::vector<VkDescriptorSetLayout> layouts(capacity,layout_); sets_.resize(capacity);
    VkDescriptorSetAllocateInfo si = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    si.descriptorPool = pool_; si.descriptorSetCount = capacity; si.pSetLayouts = layouts.data();
    if (!good(d_.AllocateDescriptorSets(device_, &si, sets_.data()), "allocate snapshot sets")) return false;
    for (uint32_t i = 0; i < capacity; ++i) {
        // With shared slices binding 0 is pointed at its slice when the slot is appended.
        VkDescriptorBufferInfo infos[] = {{buffer_,shared?0:bone_stride_*i,bone_bytes},{buffer_,entry_base_,uniform_range}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t j = 0; j < 2; ++j) {
            writes[j].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[j].dstSet=sets_[i];
            writes[j].dstBinding=j; writes[j].descriptorCount=1; writes[j].descriptorType=bindings[j].descriptorType;
            writes[j].pBufferInfo=&infos[j];
        }
        d_.UpdateDescriptorSets(device_,2,writes,0,nullptr);
    }
    image_owners_.resize(size_t(capacity)*image_count);
    capacity_ = capacity; bone_bytes_ = bone_bytes; image_count_=image_count;
    shared_ = shared; slice_capacity_ = slices; slices_used_ = 0; slices_by_key_.clear();
    return true;
}

namespace {
// Memory for ordinary resources: not protected, lazily allocated or AMD's device-coherent memory, each
// of which needs a feature the application may not have enabled.
bool ordinary_type(VkMemoryPropertyFlags flags)
{
    return !(flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                      VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD));
}
} // namespace

uint32_t host_memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t type_bits, bool device_local)
{
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // The CPU writes the arenas (tens of kilobytes per copy): cached host memory takes the copies at
    // memory speed, uncached write-combined memory at a fraction of it (D-099). Without the
    // device-local request, system memory is preferred over the device's host-visible window.
    uint32_t chosen = UINT32_MAX;
    int best = -1;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(type_bits & (1u << i)) || (flags & host) != host || !ordinary_type(flags)) continue;
        if (device_local && !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        const int score = ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 0) +
                          ((device_local || !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ? 1 : 0);
        if (score > best) { best = score; chosen = i; }
    }
    return chosen;
}

MeshPlacement mesh_placement(const VkPhysicalDeviceMemoryProperties& memory, uint32_t type_bits, bool can_copy)
{
    uint32_t largest = UINT32_MAX;
    for (uint32_t h = 0; h < memory.memoryHeapCount; ++h)
        if ((memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (largest == UINT32_MAX || memory.memoryHeaps[h].size > memory.memoryHeaps[largest].size))
            largest = h;
    const VkMemoryPropertyFlags mapped =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t direct = UINT32_MAX, device = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(type_bits & (1u << i)) || !ordinary_type(flags) || memory.memoryTypes[i].heapIndex != largest) continue;
        if ((flags & mapped) == mapped && direct == UINT32_MAX) direct = i;
        // Device memory the host does not see first: the host-visible kind is the scarcer one.
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            (device == UINT32_MAX || ((memory.memoryTypes[device].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                                      !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))))
            device = i;
    }
    if (direct != UINT32_MAX) return {MeshMemory::Mapped, direct};
    if (can_copy && device != UINT32_MAX) return {MeshMemory::Copied, device};
    return {MeshMemory::Host, host_memory_type(memory, type_bits, false)};
}

const char* mesh_memory_name(MeshMemory memory)
{
    return memory == MeshMemory::Mapped ? "mapped" : memory == MeshMemory::Copied ? "copied" : "host";
}

bool DrawSnapshots::append_shared(const uint8_t* bones, size_t size, uint64_t key, const InstanceEntry& entry, SnapshotSlot& output)
{
    return append_shared(bones, size, key, entry, {}, output);
}

bool DrawSnapshots::append_shared(const uint8_t* bones, size_t size, uint64_t key, const InstanceEntry& entry,
                                  const std::vector<SnapshotImage>& images, SnapshotSlot& output)
{
    reason.clear();
    if (!shared_ || sealed_ || !mapped_ || !bones || size != bone_bytes_) {
        reason = "shared snapshot is sealed, uncreated or has mismatched bone bytes"; return false;
    }
    return append_slice(bones, size, key, entry, images, output);
}

bool DrawSnapshots::append_rows(const uint8_t* rows, size_t size, uint64_t key, const InstanceEntry& entry,
                                const std::vector<SnapshotImage>& images, SnapshotSlot& output)
{
    reason.clear();
    if (!shared_ || sealed_ || !mapped_ || !rows || !size || size > bone_bytes_) {
        reason = "shared snapshot is sealed or uncreated, or the bone block does not fit a slice"; return false;
    }
    return append_slice(rows, size, key, entry, images, output);
}

bool DrawSnapshots::append_slice(const uint8_t* bones, size_t size, uint64_t key, const InstanceEntry& entry,
                                 const std::vector<SnapshotImage>& images, SnapshotSlot& output)
{
    if (used_ >= capacity_) { reason = "snapshot entries are full"; return false; }
    const auto known = slices_by_key_.find(key);
    const bool reuse = known != slices_by_key_.end();
    if (!reuse && slices_used_ >= slice_capacity_) { reason = "snapshot bone slices are full"; return false; }
    if (!valid_images(images)) return false;
    uint32_t slice = reuse ? known->second : slices_used_;
    uint64_t mark = clock_us();
    if (!reuse) {
        std::memcpy(mapped_ + bone_stride_ * slices_used_, bones, size);
        slices_by_key_.emplace(key, slices_used_++);
        const uint64_t now = clock_us(); g_snapshot_timing.copy_us += now - mark; mark = now;
    }
    VkDescriptorBufferInfo info = {buffer_, bone_stride_ * slice, size};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[used_]; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
    d_.UpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    { const uint64_t now = clock_us(); g_snapshot_timing.descriptor_us += now - mark; mark = now; }
    write_images(images);
    { const uint64_t now = clock_us(); g_snapshot_timing.images_us += now - mark; mark = now; }
    std::memcpy(mapped_ + entry_base_ + entry_stride_ * used_, entry.data(), entry.size());
    g_snapshot_timing.entry_us += clock_us() - mark;
    output = {sets_[used_], static_cast<uint32_t>(entry_stride_ * used_)};
    ++used_;
    return true;
}

bool DrawSnapshots::append_images(const InstanceEntry& entry, const std::vector<SnapshotImage>& images, SnapshotSlot& output)
{
    reason.clear();
    if (!shared_ || sealed_ || !mapped_) { reason = "shared snapshot is sealed or uncreated"; return false; }
    if (used_ >= capacity_) { reason = "snapshot entries are full"; return false; }
    if (!valid_images(images)) return false;
    VkDescriptorBufferInfo info = {buffer_, 0, bone_bytes_};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[used_]; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
    d_.UpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    write_images(images);
    std::memcpy(mapped_ + entry_base_ + entry_stride_ * used_, entry.data(), entry.size());
    output = {sets_[used_], static_cast<uint32_t>(entry_stride_ * used_)};
    ++used_;
    return true;
}

bool DrawSnapshots::append_external(VkBuffer bones, VkDeviceSize offset, VkDeviceSize size, const InstanceEntry& entry,
                                    const std::vector<SnapshotImage>& images, SnapshotSlot& output)
{
    reason.clear();
    if (!shared_ || sealed_ || !mapped_ || !bones || !size) { reason = "shared snapshot is sealed or uncreated, or no external bones"; return false; }
    if (used_ >= capacity_) { reason = "snapshot entries are full"; return false; }
    if (!valid_images(images)) return false;
    VkDescriptorBufferInfo info = {bones, offset, size};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[used_]; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
    d_.UpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    write_images(images);
    std::memcpy(mapped_ + entry_base_ + entry_stride_ * used_, entry.data(), entry.size());
    output = {sets_[used_], static_cast<uint32_t>(entry_stride_ * used_)};
    ++used_;
    return true;
}

bool DrawSnapshots::append(const std::vector<uint8_t>& bones, const InstanceEntry& entry, SnapshotSlot& output)
{
    return append(bones,entry,{},output);
}

bool DrawSnapshots::append(const std::vector<uint8_t>& bones, const InstanceEntry& entry,
                           const std::vector<SnapshotImage>& images,SnapshotSlot& output)
{
    reason.clear();
    if (shared_ || sealed_ || used_ >= capacity_ || bones.size() != bone_bytes_ || !mapped_ || images.size()!=image_count_) {
        reason = "snapshot is sealed, full, uncreated or has mismatched bone bytes or image count"; return false;
    }
    if (!valid_images(images)) return false;
    write_images(images);
    std::memcpy(mapped_ + bone_stride_ * used_, bones.data(), bones.size());
    std::memcpy(mapped_ + entry_base_ + entry_stride_ * used_, entry.data(), entry.size());
    output = {sets_[used_], static_cast<uint32_t>(entry_stride_ * used_)};
    ++used_;
    return true;
}

bool DrawSnapshots::valid_images(const std::vector<SnapshotImage>& images)
{
    if (images.size() != image_count_) { reason = "snapshot image count mismatch"; return false; }
    for (const auto& image : images) if (!image.view || !image.owner) { reason = "snapshot image requires ready view and owner"; return false; }
    return true;
}

void DrawSnapshots::write_images(const std::vector<SnapshotImage>& images)
{
    if(image_count_){
        VkDescriptorImageInfo infos[32]{};VkWriteDescriptorSet writes[32]{};
        for(uint32_t i=0;i<image_count_;++i){
            image_owners_[size_t(used_)*image_count_+i]=images[i].owner;
            infos[i]={VK_NULL_HANDLE,images[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets_[used_];writes[i].dstBinding=2+i;
            writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[i].pImageInfo=&infos[i];
        }
        d_.UpdateDescriptorSets(device_,image_count_,writes,0,nullptr);
    }
}
} // namespace efvk
