// Shared pipeline variants and draw transactions; the optional M2 backend uses this path.
#pragma once

#include "pipeline_recipe.h"
#include "spirv_repoint.h"
#include "../../frontend/gpu/fragment_images.h"

namespace efvk {

#define EFVK_VARIANT_FUNCS(X) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(CreateRenderPass2) X(DestroyRenderPass) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdBindDescriptorSets) X(CmdDrawIndexed) X(CmdDraw)

struct VariantDispatch {
#define X(n) PFN_vk##n n = nullptr;
    EFVK_VARIANT_FUNCS(X)
#undef X
};

struct FragmentPatch {
    // Ordered images become private bindings 2+i; nulls consume no descriptors.
    // Neither list contains asset handles, component ids or inferred slot names.
    std::vector<efmi::gpu::ImageBinding> images, nulls;
    // The application's vertex stage unchanged: the variant only re-points fragment images (a
    // texture-only draw, D-083); the private set's buffers are bound but not read.
    bool keep_vertex = false;
    // Images a replaced fragment stage of the recipe reads at private bindings 2+i after `images`
    // (the translucent capture's opacity shader, D-113).
    uint32_t replacement_images = 0;
    // The fragment stage without the character dither (D-133, efmi::gpu::remove_dither), as RabbitFX's
    // replaced shader skips its discard while its gate is set. A module without the dither is unchanged.
    bool no_dither = false;
    // With keep_vertex: the variant draws a Mod's own buffers under the application's vertex stage (D-176),
    // so one for its vertex input alone, without an image moved, is a variant.
    bool own_geometry = false;
};
std::vector<VkDescriptorSetLayoutBinding> private_draw_bindings(uint32_t image_count);

// CPU-only eligibility check. A true result is not proof that the driver can create the variant.
bool prepare_vertex_variant(const GraphicsRecipe&, const PipelineLayoutInfo&, uint32_t max_sets,
                            VertexRepoint&, std::string& reason);

class PipelineVariant {
public:
    PipelineVariant(VkDevice dev, const VariantDispatch& dispatch, VkPipelineCache cache = VK_NULL_HANDLE) : dev_(dev), d_(dispatch), cache_(cache) {}
    ~PipelineVariant();
    PipelineVariant(const PipelineVariant&) = delete;
    PipelineVariant& operator=(const PipelineVariant&) = delete;
    // Do this on a worker with owned metadata; the caller keeps the original draw until ready.
    bool build(const GraphicsRecipe&, const PipelineLayoutInfo&, const VertexInputRecipe&, uint32_t max_sets,
               const FragmentPatch& = {}, const VkPhysicalDeviceLimits& = {});
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout own_layout = VK_NULL_HANDLE;
    uint32_t own_set = 0;
    uint32_t image_count = 0;
    uint32_t bone_set = 0, bone_binding = 0;
    std::string reason;
    VkResult result = VK_SUCCESS;
private:
    void clear();
    VkDevice dev_;
    VariantDispatch d_;
    VkPipelineCache cache_ = VK_NULL_HANDLE; // the device's variant pipeline cache (D-101), or none
    std::vector<VkDescriptorSetLayout> sets_;
};

struct DrawGeometry {
    uint32_t first_binding = 0;
    std::vector<VkBuffer> buffers;
    std::vector<VkDeviceSize> offsets;
    VkBuffer index = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
};

struct IndexedDraw {
    uint32_t count = 0, instances = 1, first = 0, first_instance = 0;
    int32_t vertex_offset = 0;
    // False: the original's Draw/DrawInstanced, `count` vertices from `first`, the index binding unused (D-142).
    bool indexed = true;
};

struct VariantDraw {
    // Caller retains every object through completion of the recorded command buffer.
    const PipelineVariant* variant = nullptr;
    VkDescriptorSet own = VK_NULL_HANDLE;
    uint32_t instance_offset = 0;
    const DrawGeometry* geometry = nullptr;
    IndexedDraw draw;
};

// Executes one frontend-resolved transaction. Validate the whole bounded sequence before
// emitting anything, then restore the application once. Empty sequences are intentional skips.
// No resource creation, allocation, INI evaluation or component-specific dispatch here.
bool emit_variant_draws(const VariantDispatch&, VkCommandBuffer, const VariantDraw*, size_t count,
                        VkPipeline original_pipeline, const DrawGeometry& original);

// No commands on failure. Leaves sets 0..own_set-1 and dynamic state untouched; restores every
// pipeline/vertex/index binding it changes. Private images belong to the supplied descriptor
// snapshot; its layout must match variant.image_count. Indirect draws are not supported.
bool emit_variant_draw(const VariantDispatch&, VkCommandBuffer, const PipelineVariant&, VkDescriptorSet own,
                       uint32_t instance_offset, const DrawGeometry& replacement, const IndexedDraw&,
                       VkPipeline original_pipeline, const DrawGeometry& original);

} // namespace efvk
