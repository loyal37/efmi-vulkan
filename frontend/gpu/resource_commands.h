#pragma once
#include "../assets/allocation_budget.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <map>
#include <atomic>
#include <mutex>
#include <vector>

namespace efmi::gpu {
#define EFMI_RESOURCE_COMMAND_FUNCS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements2) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdCopyBuffer) X(CmdFillBuffer) X(CmdPipelineBarrier) \
    X(CmdCopyImage) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
    X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdResolveImage) \
    X(CreateFence) X(DestroyFence) X(QueueSubmit) X(GetFenceStatus) X(WaitForFences) \
    X(ResetCommandPool) X(ResetFences)
struct ResourceDispatch {
    VkResult (VKAPI_PTR *SetDeviceLoaderData)(VkDevice, void*) = nullptr;
#define X(name) PFN_vk##name name=nullptr;
    EFMI_RESOURCE_COMMAND_FUNCS(X)
#undef X
};
struct ResourceDevice {
    VkDevice device=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queue_family=0;
    ResourceDispatch dispatch;
    // Image execution requires Vulkan 1.1 and an explicitly owned graphics
    // queue. Enabled features are the logical device's, not merely supported ones.
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures enabled_features{};
    // Actual logical-device extension enablement, never physical support alone.
    bool enabled_depth_range_unrestricted=false;
    VkQueueFlags queue_flags=0;
    uint32_t api_version=0;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetImageFormatProperties=nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetFormatProperties=nullptr;
    // Core1.3 or enabled KHR_format_feature_flags2 with its physical query.
    bool format_features2=false;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetFormatProperties2=nullptr;
};
struct ResourceBudgets {
    std::shared_ptr<assets::AllocationBudget> resident=std::make_shared<assets::AllocationBudget>(512ull<<20);
    std::shared_ptr<assets::AllocationBudget> staging=std::make_shared<assets::AllocationBudget>(128ull<<20);
};
class ResourceCommands;
class ShaderCommands;
// Device lifetime and host serialization are the caller's responsibility.
// These are mutable resources; an owner preserves allocation, not old contents.
class BufferResource {
public:
    BufferResource()=default;
    BufferResource(const BufferResource&)=delete;
    BufferResource& operator=(const BufferResource&)=delete;
    ~BufferResource();
    VkBuffer buffer() const {return buffer_;}
    VkDeviceSize size() const {return size_;}
    VkBufferUsageFlags usage() const {return usage_;}
private:
    friend class ResourceCommands;
    friend class ResourceSubmission;
    ResourceDevice device_;
    std::shared_ptr<const uint8_t> domain_;
    std::shared_ptr<assets::AllocationBudget> budget_;
    VkBuffer buffer_=VK_NULL_HANDLE;
    VkDeviceMemory memory_=VK_NULL_HANDLE;
    // allocation_ is charged to the budget; a kept upload buffer is not (idle_ holds its size).
    VkDeviceSize size_=0,allocation_=0,idle_=0;
    VkBufferUsageFlags usage_=0;
    bool coherent_=false;
};
using Buffer=std::shared_ptr<BufferResource>;
struct ImageDescription {
    VkImageType type=VK_IMAGE_TYPE_2D;
    VkFormat format=VK_FORMAT_UNDEFINED;
    VkExtent3D extent{1,1,1};
    uint32_t mips=1,layers=1;
    VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage=0;
    VkImageCreateFlags flags=0;
};
class ImageResource {
public:
    ImageResource()=default;
    ImageResource(const ImageResource&)=delete;
    ImageResource& operator=(const ImageResource&)=delete;
    ~ImageResource();
    VkImage image() const {return image_;}
    const ImageDescription& description() const {return description_;}
    VkExtent3D extent(uint32_t mip=0) const;
    VkImageSubresourceRange full_range() const;
private:
    friend class ResourceCommands;
    friend class ImageViewResource;
    ResourceDevice device_;
    std::shared_ptr<const uint8_t> domain_;
    std::shared_ptr<assets::AllocationBudget> budget_;
    ImageDescription description_;
    VkImage image_=VK_NULL_HANDLE;
    VkDeviceMemory memory_=VK_NULL_HANDLE;
    VkDeviceSize allocation_=0;
    // True only after a successful submission. Discard never commits layouts.
    bool initialized_=false;
    // An imported image the caller owns: valid in this layout before and after every
    // submission that uses it, and never destroyed here.
    VkImageLayout external_layout_=VK_IMAGE_LAYOUT_UNDEFINED;
    bool external_=false;
};
using Image=std::shared_ptr<ImageResource>;
class ImageViewResource {
public:
    ImageViewResource()=default;
    ImageViewResource(const ImageViewResource&)=delete;
    ImageViewResource& operator=(const ImageViewResource&)=delete;
    ~ImageViewResource();
    VkImageView view() const {return view_;}
    const Image& owner() const {return image_;}
    VkFormat format() const {return format_;}
    VkImageViewType type() const {return type_;}
    const VkImageSubresourceRange& range() const {return range_;}
    VkImageUsageFlags usage() const {return usage_;}
private:
    friend class ResourceCommands;
    Image image_;
    VkImageView view_=VK_NULL_HANDLE;
    VkFormat format_=VK_FORMAT_UNDEFINED;
    VkImageViewType type_=VK_IMAGE_VIEW_TYPE_2D;
    VkImageSubresourceRange range_{};
    VkImageUsageFlags usage_=0;
};
using ImageView=std::shared_ptr<ImageViewResource>;
// The command pools (with their one command buffer) and fences of completed submissions, for the
// next submissions of the same ResourceCommands (D-108): creating and destroying both for every
// submission cost about 50 us per frame for a Mod with GPU work at every Present. Shared by the
// submissions, which may outlive their ResourceCommands; what it holds goes with the last owner.
struct SubmissionRecycler {
    ResourceDevice device;
    std::mutex mutex;
    std::vector<std::pair<VkCommandPool,VkCommandBuffer>> pools;
    std::vector<VkFence> fences;
    // Host-visible upload buffers of completed submissions, for the next writes: a scope that writes
    // what it wrote last frame (INI parameters, a panel's constants) allocates no memory (BUG-058).
    // While kept they are not charged to the staging budget, which counts uploads in flight.
    std::vector<Buffer> staging;
    VkDeviceSize staging_bytes=0;
    ~SubmissionRecycler();
};
class ResourceSubmission {
public:
    ResourceSubmission()=default;
    ResourceSubmission(const ResourceSubmission&)=delete;
    ResourceSubmission& operator=(const ResourceSubmission&)=delete;
    ~ResourceSubmission();
    bool ready();
    void wait();
private:
    friend class ResourceCommands;
    friend class ShaderCommands;
    ResourceDevice device_;
    std::shared_ptr<SubmissionRecycler> recycler_;
    VkCommandPool pool_=VK_NULL_HANDLE;
    VkCommandBuffer command_=VK_NULL_HANDLE;
    VkFence fence_=VK_NULL_HANDLE;
    bool submitted_=false,complete_=false;
    std::vector<Buffer> retained_;
    std::vector<Buffer> staging_; // upload buffers handed back to the recycler once complete
    // Descriptor sets, pipelines, views and their owners share fence lifetime.
    std::vector<std::shared_ptr<void>> shader_owners_;
    std::map<Image,VkImageLayout,std::owner_less<Image>> image_layouts_;
};

// An explicit outside-render-pass execution domain. Records private primary
// command buffers and never touches the application's render pass or bindings.
// All operations and QueueSubmit calls are serialized by the caller on exactly
// one queue. No application-owned resources or cross-queue use is implied.
// Allocation/recording/readback are not suitable for an intercepted draw hook.
// The caller keeps returned submissions until complete; they retain resources
// even when INI references or their entire generation have been released.
class ResourceCommands {
public:
    ResourceCommands(ResourceDevice,ResourceBudgets={});
    ~ResourceCommands();
    ResourceCommands(const ResourceCommands&)=delete;
    ResourceCommands& operator=(const ResourceCommands&)=delete;
    Buffer create(VkDeviceSize,VkBufferUsageFlags,const void* initial=nullptr,size_t initial_size=0,bool zero=false);
    void copy(const Buffer& source,VkDeviceSize source_offset,const Buffer& destination,VkDeviceSize destination_offset,VkDeviceSize size);
    void fill(const Buffer&,VkDeviceSize offset,VkDeviceSize size,uint32_t word);
    void write(const Buffer&,VkDeviceSize offset,const void*,size_t);
    Image create_image(ImageDescription);
    // A caller-owned image (a swap chain image) for this domain: one mip, one layer, single
    // sample. Its contents are taken from `layout`, and each submission returns it there.
    Image import_image(VkImage,ImageDescription,VkImageLayout layout);
    // View creation does not initialize pixels or emit descriptors. Once a
    // submission using an image completes, its entire allocation is GENERAL.
    // Layout changes inside a pending batch are private and discarded with it.
    ImageView view(const Image&,VkImageViewType,VkFormat,VkImageSubresourceRange,VkImageUsageFlags usage=0);
    void copy(const Image& source,const Image& destination,const VkImageCopy&);
    // Bit-preserving single-sample R16/R32 color/depth transfers use a resident
    // buffer, without CPU mapping, submission or waiting. Unknown float bits
    // entering D32 require VK_EXT_depth_range_unrestricted to be enabled.
    void copy_color_depth(const Image& source,const Image& destination,const VkImageCopy&);
    void copy(const Buffer& source,const Image& destination,const VkBufferImageCopy&);
    void copy(const Image& source,const Buffer& destination,const VkBufferImageCopy&);
    void clear(const Image&,VkImageSubresourceRange,const VkClearColorValue&);
    void clear(const Image&,VkImageSubresourceRange,const VkClearDepthStencilValue&);
    void resolve(const Image& source,const Image& destination,const VkImageResolve&);
    // Host transfer data uses Vulkan's explicit row/image pitch; offset is zero.
    // transfer_size excludes bufferOffset and includes necessary row/slice padding.
    VkDeviceSize transfer_size(const Image&,const VkBufferImageCopy&) const;
    void write(const Image&,VkBufferImageCopy,const void*,size_t);
    std::vector<uint8_t> readback(VkQueue,const Image&,VkBufferImageCopy);
    // Semaphores the submission waits on (with their stages) and signals, for a host that
    // orders it against application work such as a presentation.
    struct SubmitSync {
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<VkSemaphore> signals;
    };
    std::shared_ptr<ResourceSubmission> submit(VkQueue,const SubmitSync& sync={});
    // Work recorded and not yet submitted.
    bool pending() const {return bool(pending_);}
    // How often a command buffer was asked for to record into, since the domain was made: a caller that
    // reads it before and after a call knows whether that call recorded any GPU work.
    uint64_t recorded() const {return recorded_;}
    // Another queue of the same family for later submissions. The caller has waited for every earlier
    // submission of this domain; recorded, unsubmitted work goes to the new queue.
    void rebind(VkQueue queue){queue_=queue;}
    // Synchronous feedback required by original store. Only explicit hosts
    // owning queue synchronization may call this; never an implicit draw wait.
    std::vector<uint8_t> readback(VkQueue,const Buffer&,VkDeviceSize offset,VkDeviceSize size);
    // Feedback without a wait (D-147): `size` bytes of `source` copied into a host buffer by the pending
    // submission; feedback_ready gives them once that submission completed, false before.
    struct Feedback {Buffer buffer;VkDeviceSize size=0;std::shared_ptr<ResourceSubmission> submission;};
    Feedback feedback(const Buffer& source,VkDeviceSize offset,VkDeviceSize size);
    bool feedback_ready(Feedback&,std::vector<uint8_t>& bytes);
    void discard();
    // Driver allocations this domain made, and what it reused instead (BUG-058): device memory
    // allocations, descriptor pools, and upload buffers and descriptor sets taken again.
    struct Stats {std::atomic<uint64_t> allocations{0},descriptor_pools{0},staging_reused{0},sets_reused{0};};
    const Stats& stats() const {return stats_;}
private:
    friend class ShaderCommands;
    Buffer allocate(VkDeviceSize,VkBufferUsageFlags,bool staging);
    // An upload buffer of at least the size for the pending submission: a recycled one when one fits.
    Buffer stage(VkDeviceSize);
    void validate(const Buffer&,VkDeviceSize,VkDeviceSize) const;
    void validate(const Image&) const;
    VkImageSubresourceRange range(const Image&,VkImageSubresourceRange) const;
    void region(const Image&,VkImageSubresourceLayers,VkOffset3D,VkExtent3D) const;
    void transition(const Image&,VkImageLayout);
    void write_stage(const Buffer&,const void*,size_t);
    std::vector<uint8_t> read_stage(const Buffer&,VkDeviceSize);
    VkCommandBuffer command();
    void barrier(VkPipelineStageFlags destination=VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VkAccessFlags access=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    ResourceDevice device_;
    ResourceBudgets budgets_;
    std::shared_ptr<const uint8_t> domain_=std::make_shared<const uint8_t>(0);
    std::shared_ptr<ResourceSubmission> pending_;
    uint64_t recorded_=0;
    std::shared_ptr<SubmissionRecycler> recycler_;
    Buffer image_copy_scratch_;
    VkQueue queue_=VK_NULL_HANDLE;
    Stats stats_;
};
}
