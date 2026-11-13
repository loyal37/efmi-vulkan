// CPU tests for ownership, ignored pointers, fail-closed rewrites and partial GPU-build cleanup.
#include "variant_fixture.h"
#include "m2_fixture.h"
#include "../frontend/gpu/dither_kills.h"
#include "../frontend/gpu/source_registers.h"
#include <spirv-tools/libspirv.h>
#include <filesystem>
#include "../layer/src/state.h"
#include "../layer/src/json.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

using namespace efvk;
using variant_fixture::RecipeFixture;
static int passed = 0, failed = 0;
static void check(bool ok, const char* label)
{
    if (ok) passed++; else { failed++; printf("FAIL: %s\n", label); }
}
template <typename T> static T h(uintptr_t v) { return reinterpret_cast<T>(v); }

static int allocation = 0, fail_at = 0;
static uintptr_t next_handle = 100;
static std::set<uintptr_t> live;
static bool original_fragment = false, vertex_rewritten = false;
template <typename T> static VkResult allocate(T* out)
{
    *out = VK_NULL_HANDLE;
    if (++allocation == fail_at) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = h<T>(++next_handle); live.insert(next_handle); return VK_SUCCESS;
}
template <typename T> static void release(T handle)
{
    check(live.erase(reinterpret_cast<uintptr_t>(handle)) == 1, "each private handle destroyed exactly once");
}
static VKAPI_ATTR VkResult VKAPI_CALL create_set(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* out) { return allocate(out); }
static VKAPI_ATTR void VKAPI_CALL destroy_set(VkDevice, VkDescriptorSetLayout s, const VkAllocationCallbacks*) { release(s); }
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice, const VkPipelineLayoutCreateInfo* p, const VkAllocationCallbacks*, VkPipelineLayout* out)
{
    check(p->setLayoutCount == 4 && p->pSetLayouts[3], "variant appends exactly one own set"); return allocate(out);
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice, VkPipelineLayout p, const VkAllocationCallbacks*) { release(p); }
static VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice, const VkRenderPassCreateInfo* p, const VkAllocationCallbacks*, VkRenderPass* out)
{
    check(p->attachmentCount == 2 && p->pSubpasses[0].pDepthStencilAttachment->attachment == 1 &&
          p->pDependencies[1].dstStageMask == VK_PIPELINE_STAGE_TRANSFER_BIT, "render pass arrays survive their source");
    return allocate(out);
}
static VKAPI_ATTR void VKAPI_CALL destroy_pass(VkDevice, VkRenderPass p, const VkAllocationCallbacks*) { release(p); }
static VKAPI_ATTR VkResult VKAPI_CALL create_shader(VkDevice, const VkShaderModuleCreateInfo* p, const VkAllocationCallbacks*, VkShaderModule* out)
{
    std::vector<uint32_t> code(p->pCode, p->pCode + p->codeSize / 4);
    original_fragment |= code == variant_fixture::shader(false);
    VertexRepoint r; repoint_vertex(variant_fixture::shader(true), 3, r);
    vertex_rewritten |= code == r.words;
    return allocate(out);
}
static VKAPI_ATTR void VKAPI_CALL destroy_shader(VkDevice, VkShaderModule p, const VkAllocationCallbacks*) { release(p); }
static VKAPI_ATTR VkResult VKAPI_CALL create_pipe(VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* p,
                                                const VkAllocationCallbacks*, VkPipeline* out)
{
    check(count == 1 && p->basePipelineIndex == -1 && p->pStages[0].pName && !strcmp(p->pStages[0].pName, "main"), "creation owns stage entries");
    check(p->pVertexInputState->pVertexBindingDescriptions[0].stride == 16 &&
          p->pVertexInputState->pVertexAttributeDescriptions[1].format == VK_FORMAT_R16G16B16A16_UINT, "variant changes stride and integer format");
    check(p->pDepthStencilState->depthCompareOp == VK_COMPARE_OP_LESS &&
          p->pColorBlendState->pAttachments[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          "depth and blending are reproduced");
    check(p->pViewportState->pViewports == nullptr && p->pViewportState->pScissors == nullptr &&
          p->pDynamicState->dynamicStateCount == 2, "ignored dynamic arrays are not borrowed");
    // Vulkan can return a live pipeline along with failure from a batch. It must still be destroyed.
    const bool partial_failure = allocation + 1 == fail_at;
    const int saved = fail_at; if (partial_failure) fail_at = 0;
    VkResult result = allocate(out); fail_at = saved;
    return partial_failure ? VK_ERROR_OUT_OF_DEVICE_MEMORY : result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pipe(VkDevice, VkPipeline p, const VkAllocationCallbacks*) { release(p); }

static VariantDispatch dispatch()
{
    VariantDispatch d;
    d.CreateDescriptorSetLayout = create_set; d.DestroyDescriptorSetLayout = destroy_set;
    d.CreatePipelineLayout = create_layout; d.DestroyPipelineLayout = destroy_layout;
    d.CreateRenderPass = create_pass; d.DestroyRenderPass = destroy_pass;
    d.CreateShaderModule = create_shader; d.DestroyShaderModule = destroy_shader;
    d.CreateGraphicsPipelines = create_pipe; d.DestroyPipeline = destroy_pipe;
    return d;
}

static void rewrite_tests()
{
    auto original = variant_fixture::shader(true);
    VertexRepoint out;
    check(repoint_vertex(original, 3, out) && out.bone_set == 0 && out.bone_binding == 7, "locate the two unnamed vertex buffers");
    size_t changed = 0;
    for (size_t i = 0; i < original.size(); i++) changed += original[i] != out.words[i];
    check(changed == 4, "rewrite changes exactly four decoration literals");
    auto fragment = variant_fixture::shader(false);
    check(!repoint_vertex(fragment, 3, out) && out.words.empty(), "reject fragment rewrite");
    check(!repoint_vertex(original, 2, out) && out.reason == "spirv_target_collision", "reject occupied target binding");
    auto broken = original; broken[5] = 0;
    check(!repoint_vertex(broken, 3, out), "reject zero instruction size");
    broken = original; broken.pop_back(); broken.push_back(0xffff0001);
    check(!repoint_vertex(broken, 3, out), "reject instruction past module end");
    broken = original; broken.push_back((2u << 16) | 73); broken.push_back(1);
    check(!repoint_vertex(broken, 3, out) && out.reason == "spirv_indirect_decoration", "reject grouped decorations without guessing");
    for (size_t i = 5; i < original.size(); i += original[i] >> 16) {
        if ((original[i] & 0xffff) == 71 && original[i + 2] == 34) {
            broken = original; broken.insert(broken.end(), original.begin() + i, original.begin() + i + 4); break;
        }
    }
    check(!repoint_vertex(broken, 3, out) && out.reason == "spirv_duplicate_decoration", "reject duplicate descriptor decorations");
}

static void ownership_tests()
{
    GraphicsRecipe owned;
    PipelineLayoutInfo layout;
    {
        RecipeFixture f;
        owned = f.capture(); layout = f.layout;
        check(owned.complete, "capture the complete fixture");
        f.atts[0].format = VK_FORMAT_UNDEFINED; f.ba.blendEnable = false; f.binding.stride = 123;
        f.dynamic[0] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }
    auto copied = owned; owned = GraphicsRecipe(); auto moved = std::move(copied);
    check(moved.render_pass->attachments[0].format == VK_FORMAT_R8G8B8A8_UNORM && moved.blend_attachments[0].blendEnable &&
          moved.vertex.bindings[0].stride == 12 && moved.dynamic[0] == VK_DYNAMIC_STATE_VIEWPORT,
          "nested state survives source destruction plus recipe copy/move");
    for (int failure = 1; failure <= 10; failure++) {
        allocation = 0; fail_at = failure == 10 ? 0 : failure;
        {
            PipelineVariant variant(h<VkDevice>(2), dispatch());
            bool ok = variant.build(moved, layout, RecipeFixture::mod_vertex(), 4);
            check(ok == (failure == 10), "variant reports every injected allocation failure");
            if (!ok) check(!variant.pipeline && !variant.layout && live.empty(), "failed build publishes no resources");
        }
        check(live.empty(), "build/destruction releases all private resources");
    }
    check(original_fragment && vertex_rewritten, "builder passes unchanged FS and rewritten VS to the driver");
    RecipeFixture f;
    VkSpecializationMapEntry entry = {7, 1, 2}; uint8_t bytes[4] = {5, 6, 7, 8};
    VkSpecializationInfo spec = {1, &entry, sizeof(bytes), bytes}; f.stages[0].pSpecializationInfo = &spec;
    auto specialized = f.capture(); entry.offset = 0; bytes[1] = 0;
    check(specialized.stages[0].entries[0].offset == 1 && specialized.stages[0].data[1] == 6,
          "specialization offsets and bytes are owned");
    VkViewport vp = {3, 4, 55, 66, .1f, .9f}; VkRect2D sc = {{7, 8}, {55, 66}};
    f.gp.pDynamicState = nullptr; f.vp.pViewports = &vp; f.vp.pScissors = &sc;
    VkSampleMask mask[2] = {0x1234, 0x9876}; f.ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT; f.ms.pSampleMask = mask;
    auto stat = f.capture(); vp.width = 0; sc.offset.x = 0; mask[1] = 0;
    check(stat.viewports[0].width == 55 && stat.scissors[0].offset.x == 7 && stat.sample_mask[1] == 0x9876,
          "static viewport/scissor and two-word sample mask are owned");
    JsonWriter w; moved.write(w);
    check(w.out.find("\"complete\":true") != std::string::npos && w.out.find("\"line_width_bits\":1065353216") != std::string::npos,
          "recipe export preserves floats losslessly");
}

static void ignored_pointer_tests()
{
    RecipeFixture f;
    f.gp.pTessellationState = h<const VkPipelineTessellationStateCreateInfo*>(1);
    f.vp.pViewports = h<const VkViewport*>(1); f.vp.pScissors = h<const VkRect2D*>(1);
    check(f.capture().complete, "never dereference ignored tessellation or dynamic viewport arrays");
    f.sub.pDepthStencilAttachment = nullptr; f.gp.pDepthStencilState = h<const VkPipelineDepthStencilStateCreateInfo*>(1);
    check(f.capture().complete && !f.capture().has_depth, "ignore depth state without a depth attachment");
    f.color.attachment = VK_ATTACHMENT_UNUSED; f.gp.pColorBlendState = h<const VkPipelineColorBlendStateCreateInfo*>(1);
    check(f.capture().complete && !f.capture().has_blend, "ignore blend state when every color slot is unused");
    f.rs.rasterizerDiscardEnable = VK_TRUE; f.gp.pViewportState = h<const VkPipelineViewportStateCreateInfo*>(1);
    f.gp.pMultisampleState = h<const VkPipelineMultisampleStateCreateInfo*>(1);
    check(f.capture().complete, "discarded rasterization ignores viewport and multisample pointers");
    f.dynamic[0] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT; f.gp.pVertexInputState = h<const VkPipelineVertexInputStateCreateInfo*>(1);
    check(!f.capture().complete && f.capture().reason == "extended_dynamic_state", "reject extended dynamics before reading ignored structs");
    f.gp.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR; f.gp.pStages = h<const VkPipelineShaderStageCreateInfo*>(1);
    check(!f.capture().complete, "reject pipeline library before touching absent stage state");
    VkRenderPassCreateInfo2 r2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    VkAttachmentReference2 ar = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2}; ar.attachment = VK_ATTACHMENT_UNUSED;
    VkSubpassDescription2 s2 = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2}; s2.colorAttachmentCount = 1; s2.pColorAttachments = &ar;
    s2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; s2.viewMask = 3;
    uint32_t mask = 3; r2.subpassCount = 1; r2.pSubpasses = &s2; r2.correlatedViewMaskCount = 1; r2.pCorrelatedViewMasks = &mask;
    auto rp2 = RenderPassRecipe::capture(r2); mask = 0; s2.viewMask = 0;
    check(rp2.complete && rp2.version2 && rp2.correlated[0] == 3 && rp2.subpasses[0].view_mask == 3,
          "render pass 2 multiview state is owned");
    ar.pNext = h<const void*>(1);
    check(!RenderPassRecipe::capture(r2).complete, "unknown attachment extension is not dropped");
}

static void tracked_lifetime_tests()
{
    Device d; RecipeFixture f;
    auto rp = h<VkRenderPass>(10); auto pipe = h<VkPipeline>(11);
    on_create_render_pass(&d, rp, &f.rp); f.gp.renderPass = rp;
    for (uint32_t i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = f.shaders[i].shader->code.size() * 4; ci.pCode = f.shaders[i].shader->code.data();
        f.stages[i].module = h<VkShaderModule>(20 + i); on_create_shader(&d, f.stages[i].module, &ci);
    }
    on_create_graphics_pipeline(&d, pipe, &f.gp);
    auto recipe = d.pipes.at(pipe).recipe;
    uint32_t old_id = recipe->render_pass->id;
    on_destroy_render_pass(&d, rp); on_create_render_pass(&d, rp, &f.rp);
    for (auto& stage : f.stages) on_destroy_shader(&d, stage.module);
    check(recipe->complete && recipe->render_pass->id == old_id && d.render_passes.at(rp)->id != old_id,
          "pipeline retains the old render pass when an application handle is reused");
    check(d.shaders.empty() && d.shader_bytes->load() > 0, "shader bytes survive module destruction");
    on_destroy_pipeline(&d, pipe); recipe.reset();
    check(d.shader_bytes->load() == 0, "recipe shares the shader budget and returns it on final release");
    const bool saved = g_cfg.track_resources; g_cfg.track_resources = false;
    on_create_render_pass(&d, h<VkRenderPass>(30), &f.rp);
    check(d.render_passes.count(h<VkRenderPass>(30)) == 0, "resources off does not retain recipes");
    g_cfg.track_resources = saved;
}

struct Emitted {
    char op; uintptr_t handle=0; uint32_t count=0,first=0,instances=0,first_instance=0; int32_t vertex_offset=0;
};
static std::vector<Emitted> emitted;
static VKAPI_ATTR void VKAPI_CALL bind_pipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline p)
{ emitted.push_back({'p',reinterpret_cast<uintptr_t>(p)}); }
static VKAPI_ATTR void VKAPI_CALL bind_vertices(VkCommandBuffer,uint32_t first,uint32_t count,const VkBuffer* b,const VkDeviceSize*)
{ emitted.push_back({'v',reinterpret_cast<uintptr_t>(b[0]),count,first}); }
static VKAPI_ATTR void VKAPI_CALL bind_indices(VkCommandBuffer,VkBuffer b,VkDeviceSize,VkIndexType)
{ emitted.push_back({'i',reinterpret_cast<uintptr_t>(b)}); }
static VKAPI_ATTR void VKAPI_CALL bind_sets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t first,
    uint32_t count,const VkDescriptorSet* sets,uint32_t,const uint32_t* offsets)
{ emitted.push_back({'s',reinterpret_cast<uintptr_t>(sets[0]),count,first,offsets[0]}); }
static VKAPI_ATTR void VKAPI_CALL emit_indexed(VkCommandBuffer,uint32_t count,uint32_t instances,uint32_t first,
    int32_t offset,uint32_t first_instance)
{ emitted.push_back({'d',0,count,first,instances,first_instance,offset}); }

static void draw_sequence_tests()
{
    VariantDispatch d;d.CmdBindPipeline=bind_pipeline;d.CmdBindVertexBuffers=bind_vertices;
    d.CmdBindIndexBuffer=bind_indices;d.CmdBindDescriptorSets=bind_sets;d.CmdDrawIndexed=emit_indexed;
    PipelineVariant variant(h<VkDevice>(1),d);
    variant.pipeline=h<VkPipeline>(2);variant.layout=h<VkPipelineLayout>(3);variant.own_set=3;
    const auto cmd=h<VkCommandBuffer>(4);const auto original_pipe=h<VkPipeline>(5);
    DrawGeometry original={2,{h<VkBuffer>(6),h<VkBuffer>(7)},{32,48},h<VkBuffer>(8),16,VK_INDEX_TYPE_UINT16};
    DrawGeometry replacement={2,{h<VkBuffer>(9),h<VkBuffer>(10)},{64,80},h<VkBuffer>(11),32,VK_INDEX_TYPE_UINT32};
    DrawGeometry other=replacement;other.index=h<VkBuffer>(12);other.buffers[0]=h<VkBuffer>(13);
    VariantDraw items[]={{&variant,h<VkDescriptorSet>(14),256,&replacement,{3,1,17,0,-2}},
                         {&variant,h<VkDescriptorSet>(15),512,&other,{9,2,31,6,4}}};
    auto reject=[&](const VariantDraw* draws,size_t count,const char* label){
        emitted.clear();check(!emit_variant_draws(d,cmd,draws,count,original_pipe,original) && emitted.empty(),label);
    };
    auto saved=items[1];items[1].own=VK_NULL_HANDLE;reject(items,2,"late missing descriptor rejects without any binding or drawing");
    items[1]=saved;items[1].variant=nullptr;reject(items,2,"late pending pipeline rejects entire sequence");
    items[1]=saved;items[1].draw.count=0;reject(items,2,"zero-count item must be removed by frontend before transaction");
    items[1]=saved;other.first_binding=0;reject(items,2,"all modified binding ranges must be restorable");other.first_binding=2;
    reject(nullptr,1,"null sequence rejected");reject(items,257,"sequence budget checked before dereferencing extra items");
    items[1]=saved;emitted.clear();
    check(emit_variant_draws(d,cmd,items,2,original_pipe,original) && emitted.size()==13,"two items and one final restore emitted");
    if(emitted.size()==13){
        check(emitted[4].op=='d' && emitted[4].count==3 && emitted[4].first==17 && emitted[4].vertex_offset==-2,
              "first range and negative base vertex preserved");
        check(emitted[9].op=='d' && emitted[9].count==9 && emitted[9].first==31 && emitted[9].instances==2 &&
              emitted[9].first_instance==6 && emitted[9].vertex_offset==4,"second independent range and instance arguments preserved");
        check(emitted[8].op=='s' && emitted[8].handle==15 && emitted[8].instances==512 && emitted[8].first==3,
              "private descriptor and dynamic offset can differ between items");
        check(emitted[6].handle==13 && emitted[7].handle==12,"geometry can change between frontend-resolved draws");
        check(emitted[10].op=='p' && emitted[10].handle==5 && emitted[11].op=='v' && emitted[11].handle==6 &&
              emitted[12].op=='i' && emitted[12].handle==8,"application pipeline and geometry restored once after all draws");
    }
    emitted.clear();
    check(emit_variant_draws(d,cmd,nullptr,0,original_pipe,original) && emitted.empty(),"explicit empty transaction skips without state changes");
    d.CmdDrawIndexed=nullptr;reject(items,2,"incomplete dispatch rejected before binding");
    // These are nonowned test handles, not objects allocated by PipelineVariant::build.
    variant.pipeline=VK_NULL_HANDLE;variant.layout=VK_NULL_HANDLE;
}

// SPIRV-Tools from the layer's folder (shipped with it), for the patched modules (D-133).
static bool spirv_valid(const std::vector<uint32_t>& words, std::string& error)
{
    static HMODULE tools = LoadLibraryW(L"SPIRV-Tools-shared.dll");
    if (!tools) { error = "SPIRV-Tools-shared.dll not found"; return false; }
    const auto create = reinterpret_cast<decltype(&spvContextCreate)>(GetProcAddress(tools, "spvContextCreate"));
    const auto destroy = reinterpret_cast<decltype(&spvContextDestroy)>(GetProcAddress(tools, "spvContextDestroy"));
    const auto validate = reinterpret_cast<decltype(&spvValidate)>(GetProcAddress(tools, "spvValidate"));
    const auto release = reinterpret_cast<decltype(&spvDiagnosticDestroy)>(GetProcAddress(tools, "spvDiagnosticDestroy"));
    if (!create || !destroy || !validate || !release) { error = "SPIRV-Tools entry points missing"; return false; }
    spv_context context = create(SPV_ENV_VULKAN_1_1);
    spv_diagnostic diagnostic = nullptr;
    spv_const_binary_t binary{words.data(), words.size()};
    const bool ok = validate(context, &binary, &diagnostic) == SPV_SUCCESS;
    if (!ok) error = diagnostic && diagnostic->error ? diagnostic->error : "invalid";
    if (diagnostic) release(diagnostic);
    destroy(context);
    return ok;
}

// D-133: the character dither in SPIR-V and the module without it.
static void dither_tests(bool corpus)
{
    using variant_fixture::Kill;
    const auto dither = variant_fixture::shader(false, false, 6, Kill::Dither);
    const auto cutout = variant_fixture::shader(false, false, 6, Kill::Cutout);
    const auto noise = variant_fixture::shader(false, false, 6, Kill::Noise);
    const auto foliage = variant_fixture::shader(false, false, 6, Kill::LodCutout);
    std::string error;
    check(spirv_valid(dither, error) && spirv_valid(cutout, error) && spirv_valid(noise, error) && spirv_valid(foliage, error),
          ("the kill fixtures are valid SPIR-V " + error).c_str());
    check(efmi::gpu::dither_kills(dither) == 1, "the character dither is found: noise against the instance's camera fade, then a kill");
    check(efmi::gpu::dither_kills(cutout) == 0 && efmi::gpu::dither_kills(noise) == 0 && efmi::gpu::dither_kills(variant_fixture::shader(false)) == 0,
          "an alpha test, a noise kill without the instance window and a shader without a kill are not the dither");
    // 2.0.6 in the game: the field's grass and bushes drew as solid quads with the anti-dither on, their
    // alpha test gone with the LOD cross-fade it shares a kill with. RabbitFX's patterns match neither.
    check(efmi::gpu::dither_kills(foliage) == 0,
          "the instance's LOD cross-fade and an alpha test in one kill (the field's foliage) are not the character dither");
    std::vector<uint32_t> patched, kept = {1, 2, 3};
    std::string reason;
    check(efmi::gpu::remove_dither(dither, patched, reason) && efmi::gpu::dither_kills(patched) == 0 && patched.size() == dither.size() + 3,
          ("the patched module keeps its code, the dither branch steered by a constant " + reason).c_str());
    check(spirv_valid(patched, error), ("the patched module is valid SPIR-V " + error).c_str());
    check(!efmi::gpu::remove_dither(cutout, kept, reason) && kept == std::vector<uint32_t>({1, 2, 3}) && !reason.empty(),
          "a module without the dither is refused and the output left as it was");
    if (!corpus) return;
    // The captured fragment modules of the game (local captures; the rule's ground truth, docs/rabbitfx-dither.md).
    const std::filesystem::path captures = "F:/efmi vulkan/captures";
    std::error_code ignored;
    if (!std::filesystem::is_directory(captures, ignored)) { printf("dither: captures absent, corpus skipped\n"); return; }
    std::set<std::vector<uint32_t>> seen;
    uint32_t modules = 0, dithered = 0, patched_valid = 0, foliage_dithered = 0;
    // The field's alpha-tested modules of the 2026-10-27 frame (vk-2026-10-27-field), each a kill of the
    // LOD cross-fade and a texture's alpha: never the character dither.
    const char* const alpha_tested[] = {"e1f74fbb", "ecadd0ba", "609efaa1", "da05b7c1", "b3f81f38", "948314b4", "ea45f5ae",
                                        "36e2a5c6", "91ed851e", "5e3605c5", "cc410c39"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(captures, ignored)) {
        if (!entry.is_regular_file(ignored) || entry.path().extension() != ".spv") continue;
        std::ifstream input(entry.path(), std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size < 20 || size % 4) continue;
        std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
        input.seekg(0); input.read(reinterpret_cast<char*>(words.data()), size);
        if (words[0] != 0x07230203 || !seen.insert(words).second) continue;
        ++modules;
        if (!efmi::gpu::dither_kills(words)) continue;
        ++dithered;
        const auto name = entry.path().filename().string();
        for (const char* hash : alpha_tested)
            if (name.find(hash) != std::string::npos) { ++foliage_dithered; printf("dither: alpha-tested %s taken for the dither\n", name.c_str()); }
        std::vector<uint32_t> out;
        if (efmi::gpu::remove_dither(words, out, reason) && efmi::gpu::dither_kills(out) == 0 && spirv_valid(out, error)) ++patched_valid;
    }
    printf("dither: %u captured modules, %u with the character dither, %u patched and valid\n", modules, dithered, patched_valid);
    check(foliage_dithered == 0, "no alpha-tested module of the field is taken for the character dither");
    // 28 of the earlier captures (the 2.0.6 rule took 38: ten were LOD cross-fades of the field, seven alone
    // and three with an alpha test) and 17 of the field frame of 2026-10-27.
    check(dithered == 45, "the captured modules with the character dither are the 45 the rule names (docs/rabbitfx-dither.md)");
    check(patched_valid == dithered, "every captured dithered module patches into valid SPIR-V without its dither");
}

// D-186 (LIMIT-027): the bones of the game's dash after-images, in a uniform block of 4096 vectors: the original Core's
// vs-cb4 or vs-cb3 bone source (MergedSkeleton_DetectBoneDataSource), found by the Core's order on the D-076 registers
// and moved to the private set's binding 0 as a read-only storage block of the same layout.
static std::vector<uint32_t> module_words(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size < 20 || size % 4) return {};
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    input.seekg(0); input.read(reinterpret_cast<char*>(words.data()), size);
    return input ? words : std::vector<uint32_t>{};
}

static void constant_bones_tests(bool corpus)
{
    std::string error, why;
    const auto original = m2_fixture::constant_bones_shader();
    check(spirv_valid(original, error), ("the after-image fixture is valid SPIR-V " + error).c_str());
    std::vector<efmi::gpu::SourceRegister> registers;
    check(efmi::gpu::source_registers(original, registers, why), "the after-image fixture's registers are read");
    const auto facts = efmi::gpu::core_source_facts(registers);
    check(facts.vs_config_slot == 2 && !facts.rolling_bones && facts.constant_slot == 3 && facts.constant_bones &&
          facts.constant_bones->set == 3 && facts.constant_bones->binding == 0,
          "the Core's facts of the fixture: the instance window at vs-cb2, the bones at vs-cb3 (set 3, binding 0)");
    VertexRepoint out;
    check(repoint_vertex(original, 4, out) && out.constant_slot == 3 && out.bone_set == 3 && out.bone_binding == 0,
          ("the rewrite takes the block at vs-cb3 for the bones " + out.reason).c_str());
    check(spirv_valid(out.words, error), ("the rewritten after-image module is valid SPIR-V " + error).c_str());
    check(out.words.size() == original.size() + 8, "the rewrite adds the two members' NonWritable and nothing else");
    registers.clear();
    const bool reread = efmi::gpu::source_registers(out.words, registers, why);
    const auto* moved = reread ? efmi::gpu::find_source_register(registers, 't', 0) : nullptr;
    const auto* window = reread ? efmi::gpu::find_source_register(registers, 'b', 2) : nullptr;
    check(moved && moved->kind == efmi::gpu::SourceRegister::BufferSrv && moved->set == 4 && moved->binding == 0 && moved->stride == 16,
          "the block is now a read-only storage buffer of 16-byte rows at the private set's binding 0");
    check(window && window->set == 4 && window->binding == 1, "and the instance window is at the private set's binding 1");
    check(!repoint_vertex(original, 3, out) && out.reason == "spirv_target_collision", "the private set cannot be at the block's own set");
    auto newer = original; newer[1] = 0x00010400;
    check(!repoint_vertex(newer, 4, out) && out.reason == "spirv_constant_bones_version", "a module after SPIR-V 1.3 has no BufferBlock to make");
    if (!corpus) return;
    // The game's modules of the dash (captures/dash-2.1.7-20261112, 2.1.7 in the game, 2026-11-12): the after-images'
    // three with their bones at vs-cb3 or vs-cb4, three other effects' without bones.
    const std::filesystem::path dash = "F:/efmi vulkan/captures/dash-2.1.7-20261112";
    std::error_code ignored;
    if (!std::filesystem::is_directory(dash, ignored)) { printf("constant bones: the dash capture is absent, skipped\n"); return; }
    const struct { const char* name; uint32_t slot; } modules[] = {
        {"5370913c", 3}, {"863d7ba9", 4}, {"5eb8bfbc", 4}, {"271587d4", 0}, {"30ceb263", 0}, {"7e7383a5", 0}};
    for (const auto& m : modules) {
        const auto words = module_words(dash / (std::string(m.name) + "-vs.spv"));
        registers.clear();
        const bool read = !words.empty() && efmi::gpu::source_registers(words, registers, why);
        const auto core = efmi::gpu::core_source_facts(registers);
        const bool accepted = repoint_vertex(words, 4, out);
        const std::string label = std::string("dash vertex module ") + m.name + ": ";
        if (m.slot) {
            check(read && core.vs_config_slot == 2 && !core.rolling_bones && core.constant_slot == m.slot && core.constant_bones &&
                  core.constant_bones->set == 3 && core.constant_bones->binding == 0,
                  (label + "the Core's bones at vs-cb" + std::to_string(m.slot) + ", set 3 binding 0").c_str());
            check(accepted && out.constant_slot == m.slot && out.bone_set == 3 && out.bone_binding == 0,
                  (label + "the rewrite takes that block " + out.reason).c_str());
            check(accepted && spirv_valid(out.words, error), (label + "valid SPIR-V after the rewrite " + error).c_str());
        } else {
            check(read && core.constant_slot == 0 && !core.rolling_bones, (label + "no bone source for the Core").c_str());
            check(!accepted && out.reason == "spirv_required_buffer_missing", (label + "refused as before").c_str());
        }
    }
    // Every captured vertex module: where the rewrite finds the bones in a block, the Core's facts name the same block.
    std::set<std::vector<uint32_t>> seen;
    uint32_t vertex = 0, in_block = 0, agree = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("F:/efmi vulkan/captures", ignored)) {
        if (!entry.is_regular_file(ignored) || entry.path().extension() != ".spv") continue;
        const auto words = module_words(entry.path());
        if (words.empty() || words[0] != 0x07230203 || !seen.insert(words).second) continue;
        const bool accepted = repoint_vertex(words, 8, out);
        if (!accepted && out.reason == "spirv_not_vertex") continue;
        ++vertex;
        if (!accepted || !out.constant_slot) continue;
        ++in_block;
        registers.clear();
        const auto core = efmi::gpu::source_registers(words, registers, why) ? efmi::gpu::core_source_facts(registers) : efmi::gpu::CoreSourceFacts{};
        if (core.constant_slot == out.constant_slot && core.constant_bones && core.constant_bones->set == out.bone_set &&
            core.constant_bones->binding == out.bone_binding) ++agree;
        else printf("constant bones: %s disagrees with the Core's facts\n", entry.path().string().c_str());
    }
    printf("constant bones: %u captured vertex modules, %u with the bones in a block, %u as the Core's facts\n", vertex, in_block, agree);
    check(in_block >= 3 && agree == in_block, "every captured vertex module with its bones in a block names the block the Core's facts name");
}

int main(int argc, char** argv)
{
    // Cross-check the production rewriter on previously exported, local SPIR-V modules.
    if (argc == 4 && !strcmp(argv[1], "--repoint-file")) {
        std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
        if (!input) return 2;
        auto size = input.tellg();
        if (size < 20 || size > (64 << 20) || static_cast<size_t>(size) % 4) return 2;
        std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
        input.seekg(0); input.read(reinterpret_cast<char*>(words.data()), size);
        if (!input) return 2;
        VertexRepoint out;
        if (!repoint_vertex(words, 3, out)) { printf("repoint rejected: %s\n", out.reason.c_str()); return 1; }
        std::ofstream output(argv[3], std::ios::binary);
        output.write(reinterpret_cast<const char*>(out.words.data()), out.words.size() * 4);
        return output ? 0 : 2;
    }
    util_init(GetModuleHandleW(nullptr));
    rewrite_tests(); ownership_tests(); ignored_pointer_tests(); tracked_lifetime_tests(); draw_sequence_tests();
    const bool corpus = argc == 2 && !strcmp(argv[1], "--corpus");
    dither_tests(corpus);
    constant_bones_tests(corpus);
    printf("recipe_test: %d/%d checks passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
