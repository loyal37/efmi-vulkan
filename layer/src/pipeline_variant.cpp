#include "pipeline_variant.h"
#include "../../frontend/gpu/dither_kills.h"
#include <set>

namespace efvk {

std::vector<VkDescriptorSetLayoutBinding> private_draw_bindings(uint32_t image_count)
{
    if(image_count>32)return {};
    std::vector<VkDescriptorSetLayoutBinding> bindings={
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};
    for(uint32_t i=0;i<image_count;++i)bindings.push_back({2+i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
    return bindings;
}

static bool prepare_fragment(const GraphicsRecipe& recipe,const PipelineLayoutInfo& source,
                             const FragmentPatch& patch,const VkPhysicalDeviceLimits& limits,uint32_t own_set,
                             size_t& stage,std::vector<uint32_t>& words,std::string& reason)
{
    auto fail=[&](const char* why){reason=why;return false;};
    if(patch.images.size()+patch.nulls.size()>32)return fail("fragment_patch_budget");
    stage=recipe.stages.size();
    if(patch.images.empty() && patch.nulls.empty() && !patch.no_dither)return true;
    std::set<std::pair<uint32_t,uint32_t>> seen;
    for(const auto* list:{&patch.images,&patch.nulls})for(const auto& image:*list){
        if(!seen.insert({image.set,image.binding}).second)return fail("fragment_patch_duplicate_source");
        if(image.set>=source.sets.size())return fail("fragment_patch_source_set");
        const auto& bindings=source.sets[image.set]->bindings;const auto found=bindings.find(image.binding);
        if(found==bindings.end() || found->second.count!=1 || found->second.type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           !(found->second.stages&VK_SHADER_STAGE_FRAGMENT_BIT))return fail("fragment_patch_source_layout");
    }
    // Relocation retains the original layouts. Count their descriptors as well
    // as the new images, even if the rewritten FS no longer reads an old slot.
    if(!patch.images.empty()){
        uint64_t total=patch.images.size(),fragment=total,resources=total;
        for(const auto& set:source.sets)for(const auto& pair:set->bindings){const auto& b=pair.second;
            if(b.type>VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)return fail("fragment_patch_extended_descriptor_type");
            const bool sampled=b.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                               b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            if(sampled)total+=b.count;
            if(b.stages&VK_SHADER_STAGE_FRAGMENT_BIT){if(sampled)fragment+=b.count;
                if(b.type!=VK_DESCRIPTOR_TYPE_SAMPLER)resources+=b.count;}
        }
        if(total>limits.maxDescriptorSetSampledImages || fragment>limits.maxPerStageDescriptorSampledImages ||
           resources>limits.maxPerStageResources)return fail("fragment_patch_descriptor_limits");
    }
    for(size_t i=0;i<recipe.stages.size();++i)if(recipe.stages[i].stage==VK_SHADER_STAGE_FRAGMENT_BIT){
        if(stage!=recipe.stages.size())return fail("fragment_patch_multiple_stages");stage=i;
    }
    if(stage==recipe.stages.size())return fail("fragment_patch_stage_missing");
    words=recipe.stages[stage].shader->code;
    if(!patch.nulls.empty() && !efmi::gpu::null_fragment_images(words,patch.nulls,words,reason))return false;
    if(!patch.images.empty()){
        std::vector<efmi::gpu::ImageBindingMove> moves;
        for(uint32_t i=0;i<patch.images.size();++i)moves.push_back({patch.images[i].set,patch.images[i].binding,own_set,2+i});
        if(!efmi::gpu::repoint_fragment_images(words,moves,words,reason))return false;
    }
    if(patch.no_dither && efmi::gpu::dither_kills(words)){
        std::vector<uint32_t> without;
        if(!efmi::gpu::remove_dither(words,without,reason))return false;
        words=std::move(without);
    }
    return true;
}

PipelineVariant::~PipelineVariant() { clear(); }

void PipelineVariant::clear()
{
    if (pipeline) d_.DestroyPipeline(dev_, pipeline, nullptr);
    if (layout) d_.DestroyPipelineLayout(dev_, layout, nullptr);
    for (auto s : sets_) if (s) d_.DestroyDescriptorSetLayout(dev_, s, nullptr);
    pipeline = VK_NULL_HANDLE; layout = VK_NULL_HANDLE; own_layout = VK_NULL_HANDLE; image_count=0; sets_.clear();
}

bool prepare_vertex_variant(const GraphicsRecipe& recipe, const PipelineLayoutInfo& source,
                            uint32_t max_sets, VertexRepoint& rewritten, std::string& reason)
{
    reason.clear(); rewritten = VertexRepoint();
    if (!recipe.complete) { reason = "recipe:" + recipe.reason; return false; }
    // The private set goes right above the pipeline's own sets. The instance window is at set 2, so
    // there are at least three; the game's pipelines have three or four.
    if (!source.complete || source.flags || source.sets.size() < 3 || source.sets.size() > 8 || source.sets.size() >= max_sets) {
        reason = "layout_requires_three_complete_sets_and_one_free_slot"; return false;
    }
    const uint32_t own = static_cast<uint32_t>(source.sets.size());
    for (const auto& s : source.sets) {
        if (!s || !s->complete || s->flags) { reason = "set_layout_unsupported"; return false; }
        for (const auto& pair : s->bindings) {
            if (!pair.second.immutable_samplers.empty() || pair.second.flags) {
                reason = "immutable_sampler_or_binding_flags"; return false;
            }
        }
    }
    size_t vs_index = recipe.stages.size();
    for (size_t i = 0; i < recipe.stages.size(); i++)
        if (recipe.stages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) vs_index = i;
    if (vs_index == recipe.stages.size() || !repoint_vertex(recipe.stages[vs_index].shader->code, own, rewritten)) {
        reason = "vertex_rewrite:" + rewritten.reason; return false;
    }
    auto buffer_binding = [&](uint32_t set, uint32_t binding, bool storage) {
        if (set >= source.sets.size()) return false;
        const auto& bindings = source.sets[set]->bindings;
        auto it = bindings.find(binding);
        if (it == bindings.end() || it->second.count != 1 || !(it->second.stages & VK_SHADER_STAGE_VERTEX_BIT)) return false;
        uint32_t type = it->second.type;
        return storage ? type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC :
                         type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    };
    if (!buffer_binding(rewritten.bone_set, rewritten.bone_binding, true) || !buffer_binding(2, 0, false)) {
        reason = "vertex_buffer_layout_mismatch"; return false;
    }
    return true;
}

namespace {
thread_local PFN_vkCreateGraphicsPipelines t_create = nullptr;
thread_local VkPipelineCache t_cache = VK_NULL_HANDLE;
VKAPI_ATTR VkResult VKAPI_CALL create_through_cache(VkDevice dev, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                                   const VkAllocationCallbacks* allocator, VkPipeline* out)
{
    return t_create(dev, t_cache, count, infos, allocator, out);
}
} // namespace

bool PipelineVariant::build(const GraphicsRecipe& recipe, const PipelineLayoutInfo& source,
                            const VertexInputRecipe& vertex, uint32_t max_sets,
                            const FragmentPatch& patch,const VkPhysicalDeviceLimits& limits)
{
    clear(); reason.clear(); result = VK_SUCCESS;
    VertexRepoint rewritten;
    if (patch.keep_vertex) {
        // Only the layout: the private set above the pipeline's own sets, no vertex rewrite.
        if (!recipe.complete) { reason = "recipe:" + recipe.reason; return false; }
        if (!source.complete || source.flags || source.sets.empty() || source.sets.size() > 8 || source.sets.size() >= max_sets) {
            reason = "layout_requires_complete_sets_and_one_free_slot"; return false;
        }
        for (const auto& s : source.sets) {
            if (!s || !s->complete || s->flags) { reason = "set_layout_unsupported"; return false; }
            for (const auto& pair : s->bindings)
                if (!pair.second.immutable_samplers.empty() || pair.second.flags) { reason = "immutable_sampler_or_binding_flags"; return false; }
        }
        // A texture-only draw moves images; a twin of the application's pipeline (D-133) only drops the dither;
        // a Mod's own mesh under the application's vertex stage (D-176) may change the vertex input alone.
        if (patch.images.empty() && patch.nulls.empty() && !patch.no_dither && !patch.own_geometry) {
            reason = "texture_only_variant_without_images"; return false;
        }
    } else if (!prepare_vertex_variant(recipe, source, max_sets, rewritten, reason)) return false;
    size_t fs_index=recipe.stages.size();std::vector<uint32_t> fragment;
    own_set = static_cast<uint32_t>(source.sets.size());
    if(!prepare_fragment(recipe,source,patch,limits,own_set,fs_index,fragment,reason))return false;
    image_count=static_cast<uint32_t>(patch.images.size()+patch.replacement_images);
    if(image_count>32){reason="fragment_patch_budget";return false;}
    bone_set = rewritten.bone_set; bone_binding = rewritten.bone_binding;
    size_t vs_index = 0;
    while (recipe.stages[vs_index].stage != VK_SHADER_STAGE_VERTEX_BIT) ++vs_index;
    auto good = [&](VkResult r, const char* what) {
        result = r;
        if (r != VK_SUCCESS) { reason = what; return false; }
        return true;
    };
    for (const auto& s : source.sets) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& pair : s->bindings) {
            const auto& b = pair.second;
            bindings.push_back({pair.first, static_cast<VkDescriptorType>(b.type), b.count, b.stages, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(bindings.size()); ci.pBindings = bindings.data();
        VkDescriptorSetLayout set = VK_NULL_HANDLE;
        if (!good(d_.CreateDescriptorSetLayout(dev_, &ci, nullptr, &set), "create_set_layout")) { clear(); return false; }
        sets_.push_back(set);
    }
    const auto own_bindings=private_draw_bindings(image_count);
    VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = static_cast<uint32_t>(own_bindings.size()); set_info.pBindings = own_bindings.data();
    if (!good(d_.CreateDescriptorSetLayout(dev_, &set_info, nullptr, &own_layout), "create_own_layout")) { clear(); return false; }
    sets_.push_back(own_layout);
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = static_cast<uint32_t>(sets_.size()); layout_info.pSetLayouts = sets_.data();
    layout_info.pushConstantRangeCount = static_cast<uint32_t>(source.pushes.size()); layout_info.pPushConstantRanges = source.pushes.data();
    if (!good(d_.CreatePipelineLayout(dev_, &layout_info, nullptr, &layout), "create_pipeline_layout")) { clear(); return false; }
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!good(recipe.render_pass->create(dev_, d_.CreateRenderPass, d_.CreateRenderPass2, &pass), "create_render_pass")) {
        clear(); return false;
    }
    std::vector<VkShaderModule> modules;
    for (size_t i = 0; i < recipe.stages.size(); i++) {
        const auto& code = i == vs_index && !patch.keep_vertex ? rewritten.words : (i==fs_index?fragment:recipe.stages[i].shader->code);
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * sizeof(uint32_t); ci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (!good(d_.CreateShaderModule(dev_, &ci, nullptr, &module), "create_shader_module")) break;
        modules.push_back(module);
    }
    if (modules.size() == recipe.stages.size())
        // The recipe takes a creation function of the Vulkan shape; the variant's pipeline cache (D-101)
        // reaches it through this thread's slot, the recipe's signature being one host and module share.
        t_create = d_.CreateGraphicsPipelines; t_cache = cache_;
        good(recipe.create(dev_, &create_through_cache, layout, pass, modules, &pipeline, &vertex), "create_variant_pipeline");
    for (auto m : modules) d_.DestroyShaderModule(dev_, m, nullptr);
    d_.DestroyRenderPass(dev_, pass, nullptr);
    if (result != VK_SUCCESS) { clear(); return false; }
    return true;
}

static bool geometry_ready(const DrawGeometry& g)
{
    if (!g.index || g.buffers.empty() || g.buffers.size() != g.offsets.size()) return false;
    for (auto b : g.buffers) if (!b) return false;
    return true;
}

bool emit_variant_draw(const VariantDispatch& d, VkCommandBuffer cmd, const PipelineVariant& variant,
                       VkDescriptorSet own, uint32_t instance_offset, const DrawGeometry& replacement,
                       const IndexedDraw& draw, VkPipeline original_pipeline, const DrawGeometry& original)
{
    const VariantDraw item = {&variant,own,instance_offset,&replacement,draw};
    return emit_variant_draws(d,cmd,&item,1,original_pipeline,original);
}

bool emit_variant_draws(const VariantDispatch& d, VkCommandBuffer cmd, const VariantDraw* items, size_t count,
                        VkPipeline original_pipeline, const DrawGeometry& original)
{
    if (count > 256 || (count && !items) || !cmd || !original_pipeline || !geometry_ready(original) ||
        !d.CmdBindPipeline || !d.CmdBindVertexBuffers || !d.CmdBindIndexBuffer || !d.CmdBindDescriptorSets || !d.CmdDrawIndexed)
        return false;
    for (size_t i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (!item.variant || !item.variant->pipeline || !item.variant->layout || !item.own || !item.geometry ||
            !item.draw.count || !item.draw.instances || !geometry_ready(*item.geometry) || (!item.draw.indexed && !d.CmdDraw) ||
            item.geometry->first_binding != original.first_binding || item.geometry->buffers.size() != original.buffers.size())
            return false;
    }
    if (!count) return true;
    for (size_t i = 0; i < count; ++i) {
        const auto& item = items[i]; const auto& replacement = *item.geometry; const auto& draw = item.draw;
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, item.variant->pipeline);
        d.CmdBindVertexBuffers(cmd, replacement.first_binding, static_cast<uint32_t>(replacement.buffers.size()),
                              replacement.buffers.data(), replacement.offsets.data());
        d.CmdBindIndexBuffer(cmd, replacement.index, replacement.index_offset, replacement.index_type);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, item.variant->layout, item.variant->own_set,
                               1, &item.own, 1, &item.instance_offset);
        if (draw.indexed) d.CmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw.vertex_offset, draw.first_instance);
        else d.CmdDraw(cmd, draw.count, draw.instances, draw.first, draw.first_instance);
    }
    d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, original_pipeline);
    d.CmdBindVertexBuffers(cmd, original.first_binding, static_cast<uint32_t>(original.buffers.size()),
                          original.buffers.data(), original.offsets.data());
    d.CmdBindIndexBuffer(cmd, original.index, original.index_offset, original.index_type);
    return true;
}

} // namespace efvk
