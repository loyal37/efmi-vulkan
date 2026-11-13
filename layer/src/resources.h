// Resource metadata, descriptor sets, templates and push descriptors.
// Device maps are guarded by res_mx; recorded push contents are immutable.
#pragma once

#include "common.h"

#include <map>
#include <memory>

namespace efvk {

struct ShaderInfo {
    uint32_t id = 0;
    uint32_t hash = 0;
    uint32_t size = 0;          // bytes of the module; known even when the code was not kept
    std::vector<uint32_t> code; // empty when the bytecode budget was exhausted at creation
    // Total of retained bytes of the owning device; given back when this object dies.
    std::shared_ptr<std::atomic<uint64_t>> retained_total;

    ShaderInfo() = default;
    ShaderInfo(const ShaderInfo&) = delete;
    ShaderInfo& operator=(const ShaderInfo&) = delete;
    ~ShaderInfo()
    {
        if (retained_total)
            retained_total->fetch_sub(code.size() * 4, std::memory_order_relaxed);
    }
};

struct ShaderStage {
    uint32_t stage = 0;
    std::string entry;
    std::shared_ptr<const ShaderInfo> shader;
};

// What is known about the bytes of mip 0, layer 0 of an image.
enum ImageContent : uint8_t {
    IMG_NONE = 0,        // no copy from a buffer to mip 0, layer 0 was seen
    IMG_HASHED = 1,      // content_crc is the CRC-32C of the level as last uploaded, rows without padding
    IMG_PARTIAL = 2,     // the last upload covered only a part of the level
    IMG_UNREADABLE = 3,  // the source bytes were not CPU-visible when the copy was submitted
    IMG_UNSUPPORTED = 4, // the layer does not know the size of a texel of this format
    IMG_TOO_LARGE = 5,   // above the configured limit
    IMG_STATES = 6,
};

struct ImageInfo {
    uint32_t id = 0;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent = {};
    uint32_t levels = 0, layers = 0, samples = 0, usage = 0, flags = 0;
    uint8_t content = IMG_NONE;
    uint32_t content_crc = 0;
    // With IMG_HASHED: the original's data hash of the same upload (D-077): the first
    // width * height * layers bytes of the level when that is not longer, else content_crc.
    uint32_t dx11_data_crc = 0;
    uint32_t content_uploads = 0; // copies from a buffer to mip 0, layer 0
    uint64_t content_bytes = 0;   // size of the level without row padding, when the format is known
};

struct ImageViewInfo {
    uint32_t id = 0, image_id = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping components = {};
    VkImageSubresourceRange range = {};
};

struct BufferViewInfo {
    uint32_t id = 0, buffer_id = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t offset = 0, range = 0;
};

// Same-handle ownership for immutable samplers in asynchronous pipeline variants.
// Armed only when the application destroys the sampler; released before device destruction.
template<class Handle, class Destroy> struct ResourceLifetime {
    VkDevice device = VK_NULL_HANDLE;
    Handle handle = VK_NULL_HANDLE;
    Destroy destroy = nullptr;
    bool requested = false;
    ~ResourceLifetime() { if (requested && destroy) destroy(device, handle, nullptr); }
};
using SamplerLifetime = ResourceLifetime<VkSampler, PFN_vkDestroySampler>;
using SetLayoutLifetime = ResourceLifetime<VkDescriptorSetLayout, PFN_vkDestroyDescriptorSetLayout>;

struct SamplerInfo {
    uint32_t id = 0;
    uint32_t mag = 0, min = 0, mipmap = 0;
    uint32_t address[3] = {};
    std::shared_ptr<SamplerLifetime> lifetime;
};

struct DescriptorBinding {
    uint32_t type = 0, count = 0, stages = 0, flags = 0;
    std::vector<VkSampler> immutable_samplers;
    std::vector<std::shared_ptr<SamplerLifetime>> immutable_owners;
};

struct DescriptorLayoutInfo {
    uint32_t id = 0, flags = 0;
    bool complete = true;
    std::map<uint32_t, DescriptorBinding> bindings;
    std::shared_ptr<SetLayoutLifetime> lifetime;
    // Equal contents, equal id (interned at creation), so two layouts compare in one step; 0 when
    // the layout is not completely known.
    uint64_t canonical = 0;
};

struct PipelineLayoutInfo {
    uint32_t id = 0, flags = 0;
    bool complete = true;
    std::vector<std::shared_ptr<const DescriptorLayoutInfo>> sets;
    std::vector<VkPushConstantRange> pushes;
    // Per set i, the interned id of what layout compatibility for set i compares: the flags, the push
    // constant ranges and the canonical ids of sets 0 to i; 0 from the first unknown set on.
    std::vector<uint64_t> prefix;
};

struct DescriptorElement {
    uint32_t type = 0;
    bool known = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkBufferView buffer_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t buffer_id = 0, image_view_id = 0, buffer_view_id = 0, sampler_id = 0;
    uint64_t offset = 0, range = 0;
    uint32_t image_layout = 0;
};

struct DescriptorSetInfo {
    uint32_t id = 0;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::shared_ptr<const DescriptorLayoutInfo> layout;
    // Sparse values avoid allocating one object per unused bindless array element.
    std::map<uint32_t, uint32_t> counts;
    std::map<std::pair<uint32_t, uint32_t>, DescriptorElement> values;
    uint64_t generation = 0;
    const char* unsupported = nullptr;
};

struct DescriptorTemplateInfo {
    VkDescriptorUpdateTemplateType type = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t set = 0;
    bool complete = true;
    std::shared_ptr<const DescriptorLayoutInfo> layout;
    std::shared_ptr<const PipelineLayoutInfo> pipeline_layout;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

struct Device;
struct CmdBuf;
struct SetBind;
struct PipeInfo;
class JsonWriter;

void on_create_shader(Device*, VkShaderModule, const VkShaderModuleCreateInfo*);
void on_destroy_shader(Device*, VkShaderModule);
void track_stages_locked(Device*, PipeInfo&, uint32_t, const VkPipelineShaderStageCreateInfo*);
// Ids of the shaders a pipeline refers to; pass them to forget_shaders_locked after dropping the pipeline.
std::vector<uint32_t> shader_ids_of(const PipeInfo&);
void forget_shaders_locked(Device*, const std::vector<uint32_t>& ids);
void on_create_image(Device*, VkImage, const VkImageCreateInfo*);
void on_destroy_image(Device*, VkImage);
void on_create_image_view(Device*, VkImageView, const VkImageViewCreateInfo*);
void on_destroy_image_view(Device*, VkImageView);
void on_create_buffer_view(Device*, VkBufferView, const VkBufferViewCreateInfo*);
void on_destroy_buffer_view(Device*, VkBufferView);
void on_create_sampler(Device*, VkSampler, const VkSamplerCreateInfo*, const VkAllocationCallbacks* = nullptr);
bool on_destroy_sampler(Device*, VkSampler);
void on_create_set_layout(Device*, VkDescriptorSetLayout, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks* = nullptr);
bool on_destroy_set_layout(Device*, VkDescriptorSetLayout);
void on_create_pipeline_layout(Device*, VkPipelineLayout, const VkPipelineLayoutCreateInfo*);
void on_destroy_pipeline_layout(Device*, VkPipelineLayout);
void on_allocate_sets(Device*, const VkDescriptorSetAllocateInfo*, const VkDescriptorSet*);
void on_free_sets(Device*, uint32_t, const VkDescriptorSet*);
void on_clear_descriptor_pool(Device*, VkDescriptorPool);
void on_update_sets(Device*, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
void on_create_descriptor_template(Device*, VkDescriptorUpdateTemplate, const VkDescriptorUpdateTemplateCreateInfo*);
void on_destroy_descriptor_template(Device*, VkDescriptorUpdateTemplate);
void on_update_template(Device*, VkDescriptorSet, VkDescriptorUpdateTemplate, const void*);
void on_push_descriptors(Device*, CmdBuf*, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                         const VkWriteDescriptorSet*);
void on_push_template(Device*, CmdBuf*, VkDescriptorUpdateTemplate, VkPipelineLayout, uint32_t, const void*);
void on_bind_sets(Device*, CmdBuf*, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                  const VkDescriptorSet*, uint32_t, const uint32_t*);
void on_unsupported_descriptor_bind(CmdBuf*, VkPipelineBindPoint, const char*);
void write_descriptor_set_locked(JsonWriter&, Device*, const SetBind&, const std::vector<uint32_t>&,
                                 VkPipeline pipeline = VK_NULL_HANDLE, uint32_t slot = 0);

// A uniform or storage buffer range a bound set gives a draw, dynamic offset applied.
struct BoundRange {
    uint8_t set = 0;
    uint32_t binding = 0, element = 0, type = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint32_t buffer_id = 0;
    uint64_t off = 0, size = 0;
};
// Appends the ranges of live buffers the set at `slot` holds; `dyn` are the dynamic offsets given
// with its bind. Ranges that do not lie inside their buffer are left out. Caller holds res_mx.
void collect_buffer_ranges_locked(Device*, const SetBind&, const uint32_t* dyn, uint32_t dyn_count, uint32_t slot,
                                  std::vector<BoundRange>& out);
void write_stages(JsonWriter&, const PipeInfo&);

// Size of one texel block of a format: width and height in texels, bytes. False: not known here.
bool format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& block_bytes);
const char* image_content_name(uint8_t state);
// Hashes the image uploads a submitted command buffer contains and stores the result with the images.
void apply_image_writes(Device*, CmdBuf*);
std::string probe_shader(uint32_t, bool, std::vector<uint8_t>&);

} // namespace efvk
