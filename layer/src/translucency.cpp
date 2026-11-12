#include "translucency.h"
#include "character_present.h"
#include "../../frontend/gpu/hlsl_compiler.h"
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace efvk {
namespace {
// SPIR-V words of the decorations the binding rewrite reads (SPIR-V 1.0 numbering).
constexpr uint32_t OpDecorate = 71, DecBinding = 33, DecSet = 34;
}

std::shared_ptr<const GraphicsRecipe> capture_recipe(const GraphicsRecipe& source, CaptureKind kind, const std::vector<uint32_t>* opacity,
                                                     std::string& why)
{
    if (!source.complete) { why = "capture: incomplete game recipe"; return nullptr; }
    const bool colour = kind == CaptureKind::ColourBack || kind == CaptureKind::ColourFront;
    const bool back = kind == CaptureKind::ColourBack || kind == CaptureKind::OpacityBack;
    if (!colour && (!opacity || opacity->empty())) { why = "capture: no opacity shader"; return nullptr; }
    auto out = std::make_shared<GraphicsRecipe>(source);
    out->flags = 0;
    auto pass = std::make_shared<RenderPassRecipe>();
    pass->complete = true;
    VkAttachmentDescription2 c = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    c.format = VK_FORMAT_R16G16B16A16_SFLOAT; c.samples = VK_SAMPLE_COUNT_1_BIT;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; c.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    c.initialLayout = c.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription2 dpt = c;
    dpt.format = VK_FORMAT_D32_SFLOAT; dpt.initialLayout = dpt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pass->attachments = {c, dpt};
    RenderSubpass sub;
    VkAttachmentReference2 cr = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    cr.attachment = 0; cr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; cr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.colors = {cr};
    sub.has_depth = true;
    sub.depth.attachment = 1; sub.depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; sub.depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    pass->subpasses = {sub};
    out->render_pass = pass; out->subpass = 0;
    out->raster.cullMode = back ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    out->raster.depthBiasEnable = VK_FALSE; out->raster.rasterizerDiscardEnable = VK_FALSE;
    out->has_depth = true;
    out->depth.depthTestEnable = VK_TRUE; out->depth.depthWriteEnable = colour ? VK_TRUE : VK_FALSE;
    out->depth.depthCompareOp = colour ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_EQUAL;
    out->depth.depthBoundsTestEnable = VK_FALSE; out->depth.stencilTestEnable = VK_FALSE;
    out->multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; out->multisample.sampleShadingEnable = VK_FALSE;
    out->multisample.alphaToCoverageEnable = VK_FALSE; out->multisample.alphaToOneEnable = VK_FALSE; out->sample_mask.clear();
    out->has_blend = true;
    out->blend.logicOpEnable = VK_FALSE; out->blend.attachmentCount = 1;
    VkPipelineColorBlendAttachmentState b = {};
    b.blendEnable = VK_FALSE;
    b.colorWriteMask = colour ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : VK_COLOR_COMPONENT_A_BIT;
    out->blend_attachments = {b};
    if (!colour) {
        bool replaced = false;
        for (auto& stage : out->stages) {
            if (stage.stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            auto shader = std::make_shared<ShaderInfo>();
            shader->code = *opacity; shader->size = uint32_t(opacity->size() * 4);
            stage.shader = std::move(shader); stage.entry = "main"; stage.entries.clear(); stage.data.clear();
            replaced = true;
        }
        if (!replaced) { why = "capture: the game pipeline has no fragment stage"; return nullptr; }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

namespace {
uint32_t device_memory_type(const VkPhysicalDeviceMemoryProperties& p, uint32_t bits)
{
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i) if (bits >> i & 1) return i;
    return UINT32_MAX;
}
// Rewrites the (set, binding) decorations of the variables named in `moves`; nothing else.
bool repoint_bindings(std::vector<uint32_t>& code, const std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>>& moves)
{
    std::unordered_map<uint32_t, size_t> set_word, binding_word;
    for (size_t at = 5; at < code.size();) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || at + count > code.size()) return false;
        if (op == OpDecorate && count >= 4) {
            if (code[at + 2] == DecSet) set_word[code[at + 1]] = at + 3;
            if (code[at + 2] == DecBinding) binding_word[code[at + 1]] = at + 3;
        }
        at += count;
    }
    size_t moved = 0;
    for (const auto& [id, s] : set_word) {
        const auto b = binding_word.find(id);
        if (b == binding_word.end()) continue;
        for (const auto& move : moves)
            if (code[s] == move.first.first && code[b->second] == move.first.second) {
                code[s] = move.second.first; code[b->second] = move.second.second; ++moved; break;
            }
    }
    return moved == moves.size();
}
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
// One image with its own device memory, and a view of it when it has one layer; adds the memory's
// size to `bytes`. False with the reason.
bool make_image(const Translucency::Dispatch& d, VkDevice dev, const VkPhysicalDeviceMemoryProperties& memory, Image& out, VkImageType type,
                VkFormat format, VkExtent3D extent, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint64_t& bytes,
                std::string& why)
{
    auto good = [&](VkResult r, const char* what) { if (r == VK_SUCCESS) return true; why = std::string("EFMITTL layers: ") + what; return false; };
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = type; ci.format = format; ci.extent = extent; ci.mipLevels = 1; ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!good(d.CreateImage(dev, &ci, nullptr, &out.image), "create image")) return false;
    VkMemoryRequirements req; d.GetImageMemoryRequirements(dev, out.image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = device_memory_type(memory, req.memoryTypeBits);
    if (ai.memoryTypeIndex == UINT32_MAX) { why = "EFMITTL layers: no memory type"; return false; }
    if (!good(d.AllocateMemory(dev, &ai, nullptr, &out.memory), "allocate image memory") ||
        !good(d.BindImageMemory(dev, out.image, out.memory, 0), "bind image memory")) return false;
    bytes += req.size;
    if (layers > 1) return true; // the parameter image's views are made per layer
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image; vi.viewType = type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return good(d.CreateImageView(dev, &vi, nullptr, &out.view), "create image view");
}
constexpr auto kIdleLayers = std::chrono::seconds(5);
} // namespace

struct Translucency::Held {
    std::function<void()> release;
    uint64_t serial = 0; // unique for the device's lifetime, unlike a handle
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    ~Held() { if (release) release(); }
};

struct Translucency::Layers {
    Translucency* t = nullptr;
    uint32_t width = 0, height = 0;
    Image colour[4][2], depth[4][2]; // [layer][0 back, 1 front], made when a pass first draws into the layer
    bool allocated[4] = {};
    Image spare_colour, spare_depth; // 1x1, in the composite's set for the layers not made (it reads only those in use)
    Image neutral, params;
    uint64_t bytes = 0;
    std::chrono::steady_clock::time_point used = std::chrono::steady_clock::now();
    std::vector<VkImageView> params_views;
    VkBuffer params_buffer = VK_NULL_HANDLE;
    VkDeviceMemory params_memory = VK_NULL_HANDLE;
    VkRenderPass clear_pass = VK_NULL_HANDLE, load_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffers[4][2] = {};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::map<std::pair<uint64_t, uint32_t>, VkDescriptorSet> composite_sets; // by the scene depth view's serial, the layers made
    ~Layers()
    {
        if (!t) return;
        const auto& d = t->d_; VkDevice dev = t->device_->handle;
        auto free_image = [&](Image& i) {
            if (i.view) d.DestroyImageView(dev, i.view, nullptr);
            if (i.image) d.DestroyImage(dev, i.image, nullptr);
            if (i.memory) d.FreeMemory(dev, i.memory, nullptr);
        };
        for (auto& f : framebuffers) for (auto fb : f) if (fb) d.DestroyFramebuffer(dev, fb, nullptr);
        if (clear_pass) d.DestroyRenderPass(dev, clear_pass, nullptr);
        if (load_pass) d.DestroyRenderPass(dev, load_pass, nullptr);
        for (auto& l : colour) for (auto& i : l) free_image(i);
        for (auto& l : depth) for (auto& i : l) free_image(i);
        free_image(spare_colour); free_image(spare_depth);
        for (auto v : params_views) if (v) d.DestroyImageView(dev, v, nullptr);
        free_image(neutral); free_image(params);
        if (params_buffer) d.DestroyBuffer(dev, params_buffer, nullptr);
        if (params_memory) d.FreeMemory(dev, params_memory, nullptr);
        if (pool) d.DestroyDescriptorPool(dev, pool, nullptr);
    }
};

// What a recording keeps of this work until it completed: the captures of its current pass and the
// objects its commands use.
struct Translucency::Owner : CommandOwner {
    uint32_t serial = 0, calls = 0;
    std::vector<TranslucentCapture> captures;
    std::vector<std::shared_ptr<const void>> keep;
    void reset_after_completion() override { used = false; serial = 0; calls = 0; captures.clear(); keep.clear(); }
    bool seal() override { return false; }
};

Translucency::Translucency(Device* device, const VkPhysicalDeviceMemoryProperties& memory) : device_(device), memory_(memory)
{
#define X(n) d_.n = reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle, "vk" #n));
    EFVK_TRANSLUCENCY_FUNCS(X)
#undef X
}

Translucency::~Translucency()
{
    VkDevice dev = device_->handle;
    layer_sets_.clear(); depth_views_.clear(); framebuffers_.clear();
    for (auto& p : composite_pipelines_) d_.DestroyPipeline(dev, p.second, nullptr);
    for (auto& p : composite_passes_) d_.DestroyRenderPass(dev, p.second, nullptr);
    if (composite_layout_) d_.DestroyPipelineLayout(dev, composite_layout_, nullptr);
    if (composite_set_layout_) d_.DestroyDescriptorSetLayout(dev, composite_set_layout_, nullptr);
}

bool Translucency::shaders(const std::filesystem::path& library, uint32_t own_set, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    if (alpha_.empty()) {
        try {
            efmi::gpu::HlslCompiler compiler(owned_tools_directory() / L"dxcompiler.dll");
            auto compile = [&](const wchar_t* file, char stage, std::vector<uint32_t>& out, efmi::gpu::HlslProgram* keep) {
                efmi::gpu::HlslRequest request;
                request.source = library / L"shaders" / file; request.working_directory = library; request.stage = stage;
                efmi::gpu::HlslProgram program; std::string reason;
                if (!compiler.compile(request, program, reason)) { why = "EFMITTL shader " + request.source.u8string() + ": " + reason; return false; }
                out = program.spirv; if (keep) *keep = std::move(program);
                return true;
            };
            efmi::gpu::HlslProgram composite;
            if (!compile(L"alpha.hlsl", 'p', alpha_, nullptr) || !compile(L"composite.hlsl", 'v', composite_vs_, nullptr) ||
                !compile(L"composite.hlsl", 'p', composite_ps_, &composite)) { alpha_.clear(); return false; }
            // The composite's set: the images its pixel shader declares, as the compiler bound them.
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (const auto& b : composite.bindings) {
                if (b.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { why = "EFMITTL composite: a resource other than an image"; alpha_.clear(); return false; }
                bindings.push_back({b.binding, b.type, b.count, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            }
            VkDescriptorSetLayoutCreateInfo si = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            si.bindingCount = uint32_t(bindings.size()); si.pBindings = bindings.data();
            VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            if (d_.CreateDescriptorSetLayout(device_->handle, &si, nullptr, &composite_set_layout_) != VK_SUCCESS) { why = "EFMITTL composite set layout"; alpha_.clear(); return false; }
            li.setLayoutCount = 1; li.pSetLayouts = &composite_set_layout_;
            if (d_.CreatePipelineLayout(device_->handle, &li, nullptr, &composite_layout_) != VK_SUCCESS) { why = "EFMITTL composite layout"; alpha_.clear(); return false; }
        } catch (const std::exception& e) { why = std::string("EFMITTL shaders: ") + e.what(); alpha_.clear(); return false; }
    }
    if (!alpha_by_set_.count(own_set)) {
        auto code = alpha_;
        const uint32_t mask = efmi::gpu::HlslCompiler::binding('p', 't', 100), params = efmi::gpu::HlslCompiler::binding('p', 't', 120);
        if (!repoint_bindings(code, {{{0, mask}, {own_set, 2}}, {{0, params}, {own_set, 3}}})) { why = "EFMITTL alpha shader bindings"; return false; }
        alpha_by_set_[own_set] = std::move(code);
    }
    return true;
}

const std::vector<uint32_t>* Translucency::opacity_fragment(uint32_t own_set) const
{
    const auto found = alpha_by_set_.find(own_set);
    return found == alpha_by_set_.end() ? nullptr : &found->second;
}

std::shared_ptr<const GraphicsRecipe> Translucency::recipe(const PipeInfo& pipe, CaptureKind kind, uint32_t own_set, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    const auto key = std::make_pair(pipe.id, uint32_t(kind));
    if (const auto found = recipes_.find(key); found != recipes_.end()) return found->second;
    if (!pipe.recipe) { why = "capture: the game pipeline has no recipe"; return nullptr; }
    const auto alpha = alpha_by_set_.find(own_set);
    auto made = capture_recipe(*pipe.recipe, kind, alpha == alpha_by_set_.end() ? nullptr : &alpha->second, why);
    if (made) { if (recipes_.size() >= 4096) recipes_.clear(); recipes_.emplace(key, made); }
    return made;
}

void Translucency::retire(uint32_t pipeline_id)
{
    std::lock_guard<std::mutex> lock(mx_);
    for (uint32_t k = 0; k < kCaptureKinds; ++k) recipes_.erase({pipeline_id, k});
    for (auto it = shadow_recipes_.lower_bound({pipeline_id, 0}); it != shadow_recipes_.end() && it->first.first == pipeline_id;)
        it = shadow_recipes_.erase(it);
}

// D-185. The library's shadow shaders (shadow_a.hlsl, shadow_b.hlsl) take the DX11 shadow vertex shaders' outputs register
// for register (the UV, four more interpolants, the instance); the game's Vulkan shadow vertex shaders output the position
// and at most the UV, so those cannot link. Their test is the library's opacity functions; the layer's main below calls
// them with the UV and the position alone, and takes the call's parameters as specialization constants where the library
// reads IniParams[180] (the shadow is drawn in its pass, where no parameter image can be written).
bool Translucency::shadow_shaders(const std::filesystem::path& library, uint32_t own_set, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    if (shadow_masked_.count(own_set)) return true;
    try {
        const auto folder = (g_cfg.m2_settings.empty() ? owned_tools_directory() / L"settings"
                                                       : std::filesystem::absolute(std::filesystem::u8path(g_cfg.m2_settings))) / L"efmittl-shadow";
        std::filesystem::create_directories(folder);
        static const char* const head[2] = {"#include \"opacity.hlsli\"\n#define SHADOW_MASKED 0\n",
                                            "#include \"opacity.hlsli\"\n#define SHADOW_MASKED 1\n"};
        static const char* const body =
            "#ifdef __spirv__\n"
            "[[vk::constant_id(1)]] const float shadow_alpha = 1.0;\n"
            "[[vk::constant_id(3)]] const uint shadow_channel = 0;\n"
            "[[vk::constant_id(4)]] const uint shadow_invert = 0;\n"
            "#else\n"
            "static const float shadow_alpha = 1.0;\n"
            "static const uint shadow_channel = 0;\n"
            "static const uint shadow_invert = 0;\n"
            "#endif\n"
            "#if SHADOW_MASKED\n"
            "void main(float2 uv : TEXCOORD0, float4 position : SV_Position)\n"
            "{\n"
            "    if (saturate(shadow_alpha * saturate(ttl_mask(uv, shadow_channel, shadow_invert != 0))) <= ttl_noise(position.xy)) discard;\n"
            "}\n"
            "#else\n"
            "void main(float4 position : SV_Position)\n"
            "{\n"
            "    if (saturate(shadow_alpha) <= ttl_noise(position.xy)) discard;\n"
            "}\n"
            "#endif\n";
        efmi::gpu::HlslCompiler compiler(owned_tools_directory() / L"dxcompiler.dll");
        std::vector<uint32_t> made[2];
        for (int masked = 0; masked < 2; ++masked) {
            const auto file = folder / (masked ? L"shadow_masked.hlsl" : L"shadow.hlsl");
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << head[masked] << body;
                if (!out) { why = "EFMITTL shadow shader: cannot write " + file.u8string(); return false; }
            }
            efmi::gpu::HlslRequest request;
            // The library's opacity.hlsli is found through the working directory, as an include beside the source would be.
            request.source = file; request.working_directory = library / L"shaders"; request.stage = 'p';
            efmi::gpu::HlslProgram program;
            std::string reason;
            if (!compiler.compile(request, program, reason)) { why = "EFMITTL shadow shader: " + reason; return false; }
            made[masked] = std::move(program.spirv);
        }
        const uint32_t mask = efmi::gpu::HlslCompiler::binding('p', 't', 100);
        if (!repoint_bindings(made[1], {{{0, mask}, {own_set, 2}}})) { why = "EFMITTL shadow shader bindings"; return false; }
        shadow_plain_[own_set] = std::move(made[0]);
        shadow_masked_[own_set] = std::move(made[1]);
    } catch (const std::exception& e) { why = std::string("EFMITTL shadow shader: ") + e.what(); return false; }
    return true;
}

namespace {
// The shadow fragment stage with a call's parameters as its specialization constants 1, 3 and 4.
void shadow_stage(RecipeStage& stage, const std::vector<uint32_t>& code, const ShadowParams& params)
{
    auto shader = std::make_shared<ShaderInfo>();
    shader->code = code; shader->size = uint32_t(code.size() * 4);
    stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT; stage.shader = std::move(shader); stage.entry = "main";
    stage.entries = {{1, 0, 4}, {3, 4, 4}, {4, 8, 4}};
    const float alpha = float(params.alpha) / 1023.0f;
    const uint32_t values[2] = {params.channel, params.invert ? 1u : 0u};
    stage.data.resize(12);
    std::memcpy(stage.data.data(), &alpha, 4);
    std::memcpy(stage.data.data() + 4, values, 8);
}
}

std::shared_ptr<const GraphicsRecipe> Translucency::shadow_recipe(const PipeInfo& pipe, const ShadowParams& params, uint32_t own_set, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    const auto key = std::make_pair(pipe.id, params.key());
    if (const auto found = shadow_recipes_.find(key); found != shadow_recipes_.end()) return found->second;
    if (!pipe.recipe || !pipe.recipe->complete) { why = "translucent shadow: the game pipeline has no complete recipe"; return nullptr; }
    const auto& shaders = params.masked ? shadow_masked_ : shadow_plain_;
    const auto fragment = shaders.find(own_set);
    if (fragment == shaders.end()) { why = "translucent shadow: no shadow shader"; return nullptr; }
    auto out = std::make_shared<GraphicsRecipe>(*pipe.recipe);
    bool vertex = false, replaced = false;
    for (auto& stage : out->stages) {
        if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT && stage.shader) {
            vertex = true;
            const uint32_t kind = shadow_vertex(stage.shader->code);
            if (!kind) { why = "translucent shadow: not a shadow vertex shader"; return nullptr; }
            // The plain shadow vertex shader has no UV output; a masked call reads its mask at the UV.
            if (params.masked && kind == 4) {
                auto shader = std::make_shared<ShaderInfo>();
                shader->id = stage.shader->id; shader->hash = stage.shader->hash; shader->code = stage.shader->code;
                std::string reason;
                if (!add_uv_output(shader->code, 1, reason)) { why = "translucent shadow: " + reason; return nullptr; }
                shader->size = uint32_t(shader->code.size() * 4);
                stage.shader = std::move(shader);
            }
        } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            shadow_stage(stage, fragment->second, params);
            replaced = true;
        }
    }
    if (!vertex) { why = "translucent shadow: the game pipeline has no vertex stage"; return nullptr; }
    // A depth-only pipeline without a fragment stage is given one.
    if (!replaced) { RecipeStage stage; shadow_stage(stage, fragment->second, params); out->stages.push_back(std::move(stage)); }
    if (shadow_recipes_.size() >= 4096) shadow_recipes_.clear();
    shadow_recipes_.emplace(key, out);
    return out;
}

std::shared_ptr<void> Translucency::prepare(uint32_t width, uint32_t height, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    if (const auto found = layer_sets_.find({width, height}); found != layer_sets_.end()) return found->second;
    if (!width || !height || width > 16384 || height > 16384) { why = "EFMITTL layers: invalid target size"; return nullptr; }
    auto l = std::make_shared<Layers>();
    l->t = this; l->width = width; l->height = height;
    VkDevice dev = device_->handle;
    auto good = [&](VkResult r, const char* what) { if (r == VK_SUCCESS) return true; why = std::string("EFMITTL layers: ") + what; return false; };
    auto image = [&](Image& out, VkImageType type, VkFormat format, VkExtent3D extent, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        return make_image(d_, dev, memory_, out, type, format, extent, layers, usage, aspect, l->bytes, why);
    };
    const VkExtent3D size{width, height, 1};
    // The four layers are made by the passes that draw into them (allocate_layer).
    if (!image(l->spare_colour, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {1, 1, 1}, 1, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !image(l->spare_depth, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, {1, 1, 1}, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT)) return nullptr;
    if (!image(l->neutral, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8_UNORM, size, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT) ||
        !image(l->params, VK_IMAGE_TYPE_1D, VK_FORMAT_R32G32B32A32_SFLOAT, {181, 1, 1}, kCalls + 1,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return nullptr;
    for (uint32_t i = 0; i <= kCalls; ++i) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = l->params.image; vi.viewType = VK_IMAGE_VIEW_TYPE_1D; vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, i, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (!good(d_.CreateImageView(dev, &vi, nullptr, &view), "create parameter view")) return nullptr;
        l->params_views.push_back(view);
    }
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (kCalls + 1) * 16; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!good(d_.CreateBuffer(dev, &bi, nullptr, &l->params_buffer), "create parameter buffer")) return nullptr;
        VkMemoryRequirements req; d_.GetBufferMemoryRequirements(dev, l->params_buffer, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
        ai.memoryTypeIndex = device_memory_type(memory_, req.memoryTypeBits);
        if (ai.memoryTypeIndex == UINT32_MAX || !good(d_.AllocateMemory(dev, &ai, nullptr, &l->params_memory), "allocate parameter memory") ||
            !good(d_.BindBufferMemory(dev, l->params_buffer, l->params_memory, 0), "bind parameter memory")) return nullptr;
        l->bytes += req.size;
    }
    // A layer's pass: cleared when the layer is first taken in a composite cycle, loaded after.
    for (int clear = 0; clear < 2; ++clear) {
        VkAttachmentDescription a[2] = {};
        a[0].format = VK_FORMAT_R16G16B16A16_SFLOAT; a[1].format = VK_FORMAT_D32_SFLOAT;
        for (auto& x : a) {
            x.samples = VK_SAMPLE_COUNT_1_BIT; x.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            x.storeOp = VK_ATTACHMENT_STORE_OP_STORE; x.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; x.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        a[0].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[1].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &cr; sub.pDepthStencilAttachment = &dr;
        VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 2; ri.pAttachments = a; ri.subpassCount = 1; ri.pSubpasses = &sub;
        if (!good(d_.CreateRenderPass(dev, &ri, nullptr, clear ? &l->clear_pass : &l->load_pass), "create layer pass")) return nullptr;
    }
    {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32 * 18};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 32; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
        if (!good(d_.CreateDescriptorPool(dev, &pi, nullptr, &l->pool), "create composite pool")) return nullptr;
    }
    // A new size usually means the old one is gone (a resize); recordings still using a set keep it.
    if (layer_sets_.size() >= 3) layer_sets_.erase(layer_sets_.begin());
    layer_sets_.emplace(std::make_pair(width, height), l);
    return l;
}

// Under mx_: layer `layer` of a set, both faces and their framebuffers, made when a pass first draws
// into it (most Mods use one or two of the four; each is about 100 MB at 2560x1600).
bool Translucency::allocate_layer(Layers& l, uint32_t layer, std::string& why)
{
    if (l.allocated[layer]) return true;
    VkDevice dev = device_->handle;
    const VkExtent3D size{l.width, l.height, 1};
    for (uint32_t f = 0; f < 2; ++f) {
        if (!make_image(d_, dev, memory_, l.colour[layer][f], VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, size, 1,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, l.bytes, why) ||
            !make_image(d_, dev, memory_, l.depth[layer][f], VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, size, 1,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, l.bytes, why)) return false;
        VkImageView views[2] = {l.colour[layer][f].view, l.depth[layer][f].view};
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = l.load_pass; fi.attachmentCount = 2; fi.pAttachments = views; fi.width = l.width; fi.height = l.height; fi.layers = 1;
        if (d_.CreateFramebuffer(dev, &fi, nullptr, &l.framebuffers[layer][f]) != VK_SUCCESS) { why = "EFMITTL layers: create layer framebuffer"; return false; }
    }
    l.allocated[layer] = true;
    return true;
}

void Translucency::frame()
{
    std::lock_guard<std::mutex> lock(mx_);
    if (layer_sets_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = layer_sets_.begin(); it != layer_sets_.end();)
        it = now - it->second->used > kIdleLayers ? layer_sets_.erase(it) : std::next(it);
}

Translucency::Memory Translucency::memory()
{
    std::lock_guard<std::mutex> lock(mx_);
    Memory m;
    for (const auto& [size, l] : layer_sets_) {
        ++m.sets; m.bytes += l->bytes;
        for (bool a : l->allocated) m.layers += a ? 1 : 0;
    }
    return m;
}

SnapshotImage Translucency::neutral_view(const std::shared_ptr<void>& layers)
{
    const auto* l = static_cast<const Layers*>(layers.get());
    return l ? SnapshotImage{l->neutral.view, layers} : SnapshotImage{};
}
SnapshotImage Translucency::params_view(const std::shared_ptr<void>& layers, uint32_t layer)
{
    const auto* l = static_cast<const Layers*>(layers.get());
    return l && layer < l->params_views.size() ? SnapshotImage{l->params_views[layer], layers} : SnapshotImage{};
}

Translucency::Owner* Translucency::owner(CmdBuf* cb, bool make)
{
    for (auto& o : cb->owners) if (o && o->backend == this) return static_cast<Owner*>(o.get());
    if (!make) return nullptr;
    auto o = std::make_shared<Owner>();
    o->backend = this;
    cb->owners.push_back(o);
    return o.get();
}

bool Translucency::reserve(CmdBuf* cb, uint32_t& layer)
{
    std::lock_guard<std::mutex> lock(mx_);
    Owner* o = owner(cb, true);
    if (o->serial != cb->pass.serial) { o->serial = cb->pass.serial; o->calls = 0; o->captures.clear(); }
    if (o->calls >= kCalls) return false;
    layer = o->calls++;
    o->used = true;
    return true;
}

bool Translucency::queue(CmdBuf* cb, TranslucentCapture&& capture, std::string& why)
{
    std::lock_guard<std::mutex> lock(mx_);
    Owner* o = owner(cb, true);
    if (o->serial != cb->pass.serial) { why = "EFMITTL capture outside the pass it was reserved in"; return false; }
    o->captures.push_back(std::move(capture));
    o->used = true;
    return true;
}

namespace {
VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                                   VkImageAspectFlags aspect, uint32_t layers = 1)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {aspect, 0, 1, 0, layers};
    return b;
}
bool depth_format(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
                                       f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
bool has_stencil(VkFormat f) { return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
}

VkImageView Translucency::depth_view(VkImage image, uint32_t image_id, VkFormat format, std::shared_ptr<const void>& keep, uint64_t& serial,
                                     std::string& why)
{
    const auto key = std::make_pair(uint64_t(image), image_id);
    auto found = depth_views_.find(key);
    if (found == depth_views_.end()) {
        if (depth_views_.size() >= 16) depth_views_.erase(depth_views_.begin()); // recordings still using one keep it
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format; vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        auto held = std::make_shared<Held>();
        if (d_.CreateImageView(device_->handle, &vi, nullptr, &held->view) != VK_SUCCESS) { why = "EFMITTL composite: scene depth view"; return VK_NULL_HANDLE; }
        const auto d = d_; VkDevice dev = device_->handle; VkImageView view = held->view;
        held->release = [d, dev, view] { d.DestroyImageView(dev, view, nullptr); };
        held->serial = ++next_serial_;
        found = depth_views_.emplace(key, std::move(held)).first;
    }
    keep = found->second; serial = found->second->serial;
    return found->second->view;
}

VkFramebuffer Translucency::colour_framebuffer(VkImageView view, uint32_t view_id, VkFormat format, VkImageLayout layout, uint32_t width,
                                               uint32_t height, std::shared_ptr<const void>& keep, std::string& why)
{
    const auto key = std::make_pair(uint64_t(view), view_id);
    auto found = framebuffers_.find(key);
    if (found == framebuffers_.end()) {
        if (framebuffers_.size() >= 16) framebuffers_.erase(framebuffers_.begin());
        auto pass = composite_passes_.find({uint32_t(format), uint32_t(layout)});
        if (pass == composite_passes_.end()) { why = "EFMITTL composite: no pass for the scene colour"; return VK_NULL_HANDLE; }
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = pass->second; fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = width; fi.height = height; fi.layers = 1;
        auto held = std::make_shared<Held>();
        if (d_.CreateFramebuffer(device_->handle, &fi, nullptr, &held->framebuffer) != VK_SUCCESS) { why = "EFMITTL composite: framebuffer"; return VK_NULL_HANDLE; }
        const auto d = d_; VkDevice dev = device_->handle; VkFramebuffer fb = held->framebuffer;
        held->release = [d, dev, fb] { d.DestroyFramebuffer(dev, fb, nullptr); };
        found = framebuffers_.emplace(key, std::move(held)).first;
    }
    keep = found->second;
    return found->second->framebuffer;
}

bool Translucency::composite_pipeline(VkFormat colour, VkImageLayout layout, const std::vector<VkDynamicState>& dynamic, VkPipeline& out,
                                      std::string& why)
{
    uint32_t mask = 0;
    for (auto s : dynamic) if (uint32_t(s) < 32) mask |= 1u << uint32_t(s);
    const auto key = std::make_tuple(uint32_t(colour), uint32_t(layout), mask);
    if (const auto found = composite_pipelines_.find(key); found != composite_pipelines_.end()) { out = found->second; return true; }
    VkDevice dev = device_->handle;
    auto pass = composite_passes_.find({uint32_t(colour), uint32_t(layout)});
    if (pass == composite_passes_.end()) {
        // The scene colour stays in the layout the game's pass left it in, before and after.
        VkAttachmentDescription a = {};
        a.format = colour; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = layout;
        VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &cr;
        VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = 1; ri.pAttachments = &a; ri.subpassCount = 1; ri.pSubpasses = &sub;
        VkRenderPass rp = VK_NULL_HANDLE;
        if (d_.CreateRenderPass(dev, &ri, nullptr, &rp) != VK_SUCCESS) { why = "EFMITTL composite pass"; return false; }
        pass = composite_passes_.emplace(std::make_pair(uint32_t(colour), uint32_t(layout)), rp).first;
    }
    VkShaderModule modules[2] = {};
    const std::vector<uint32_t>* code[2] = {&composite_vs_, &composite_ps_};
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code[i]->size() * 4; mi.pCode = code[i]->data();
        if (d_.CreateShaderModule(dev, &mi, nullptr, &modules[i]) != VK_SUCCESS) { why = "EFMITTL composite module"; for (auto m : modules) if (m) d_.DestroyShaderModule(dev, m, nullptr); return false; }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = modules[0]; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = modules[1]; stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // The library's blend: scene = S + T * scene for the colour, the scene's alpha kept.
    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE; ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    // The game's dynamic states, so that binding this pipeline invalidates none of its own.
    std::vector<VkDynamicState> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    for (auto s : dynamic) if (s != VK_DYNAMIC_STATE_VIEWPORT && s != VK_DYNAMIC_STATE_SCISSOR && uint32_t(s) <= VK_DYNAMIC_STATE_STENCIL_REFERENCE) states.push_back(s);
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = uint32_t(states.size()); dy.pDynamicStates = states.data();
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
    pi.layout = composite_layout_; pi.renderPass = pass->second; pi.subpass = 0;
    const VkResult result = d_.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &out);
    for (auto m : modules) d_.DestroyShaderModule(dev, m, nullptr);
    if (result != VK_SUCCESS) { why = "EFMITTL composite pipeline"; return false; }
    composite_pipelines_.emplace(key, out);
    return true;
}

// A refusal of a pass end: its captures are dropped (the part is not drawn in that pass), the reason
// logged once per distinct text.
void Translucency::fail(const std::string& why)
{
    if (why != error_) logf("translucency: %s", why.c_str());
    error_ = why;
}

void Translucency::restore(CmdBuf* cb)
{
    VkCommandBuffer cmd = cb->handle;
    // What the driver had bound: the application's pipeline or its twin without the dither (D-133).
    if (cb->gfx_pipeline) d_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cb->gfx_bound ? cb->gfx_bound : cb->gfx_pipeline);
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        const SetBind& s = cb->sets[i];
        if (!s.set || s.pushed || s.unsupported || !s.layout_handle || uint64_t(s.dyn_first) + s.dyn_count > cb->dyn.size()) continue;
        d_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout_handle, i, 1, &s.set, s.dyn_count,
                                 s.dyn_count ? cb->dyn.data() + s.dyn_first : nullptr);
    }
    for (uint32_t slot = 0; slot < kMaxVb; ++slot)
        if (cb->vb[slot].buf) d_.CmdBindVertexBuffers(cmd, slot, 1, &cb->vb[slot].buf, &cb->vb[slot].off);
    if (cb->ib) d_.CmdBindIndexBuffer(cmd, cb->ib, cb->ib_off, cb->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (cb->has_viewport) d_.CmdSetViewport(cmd, 0, 1, &cb->viewport);
    if (cb->has_scissor) d_.CmdSetScissor(cmd, 0, 1, &cb->scissor);
}

void Translucency::pass_end(CmdBuf* cb)
{
    // Most passes queued nothing: the recording's owner is touched by its own thread only.
    {
        const Owner* mine = nullptr;
        for (const auto& o : cb->owners) if (o && o->backend == this) { mine = static_cast<const Owner*>(o.get()); break; }
        if (!mine || mine->captures.empty()) return;
    }
    // The dynamic states of the application's pipeline, read before our lock (record() takes ours
    // under the tracker's).
    std::vector<VkDynamicState> dynamic;
    if (cb->gfx_pipeline) {
        std::shared_lock<std::shared_mutex> tracker(device_->res_mx);
        if (const auto p = device_->pipes.find(cb->gfx_pipeline); p != device_->pipes.end())
            for (auto s : p->second.dynamic_states) dynamic.push_back(VkDynamicState(s));
    }
    std::lock_guard<std::mutex> lock(mx_);
    Owner* o = owner(cb, false);
    if (!o || o->serial != cb->pass.serial || o->captures.empty()) { if (o && o->serial == cb->pass.serial) o->calls = 0; return; }
    auto captures = std::move(o->captures);
    o->captures.clear(); o->calls = 0;
    // The pass's captures took views of one layer set, the one of its colour target's size.
    const auto layers = std::static_pointer_cast<Layers>(captures.front().layers);
    for (const auto& c : captures) if (c.layers != captures.front().layers) { fail("EFMITTL: captures of one pass in two layer sets"); return; }
    const auto& pass = cb->pass;
    std::string why;
    // The forward pass's colour target 0 and its depth, with the layouts the pass leaves them in.
    if (!layers || !pass.info || !pass.info->known || !pass.recipe || pass.recipe->subpasses.size() != 1 || alpha_.empty()) {
        fail("EFMITTL: the forward pass's targets are not known"); return;
    }
    const auto& sub = pass.recipe->subpasses[0];
    if (sub.colors.empty() || !sub.has_depth) { fail("EFMITTL: the forward pass has no colour or depth target"); return; }
    const uint32_t ci = sub.colors[0].attachment, di = sub.depth.attachment;
    if (ci >= pass.info->views.size() || di >= pass.info->views.size() || ci >= pass.recipe->attachments.size() || di >= pass.recipe->attachments.size()) {
        fail("EFMITTL: forward pass attachments out of range"); return;
    }
    const TargetView& scene = pass.info->views[ci];
    const TargetView& sdepth = pass.info->views[di];
    const VkImageLayout colour_layout = pass.recipe->attachments[ci].finalLayout, depth_layout = pass.recipe->attachments[di].finalLayout;
    if (!scene.known || !sdepth.known || colour_layout == VK_IMAGE_LAYOUT_UNDEFINED || colour_layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
        depth_layout == VK_IMAGE_LAYOUT_UNDEFINED || depth_layout == VK_IMAGE_LAYOUT_PREINITIALIZED || !depth_format(VkFormat(sdepth.image.format)) ||
        !(sdepth.image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(scene.image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        scene.image.extent.width != layers->width || scene.image.extent.height != layers->height) {
        fail("EFMITTL: the forward pass's targets are not those the layers were made for"); return;
    }
    std::shared_ptr<const void> keep_depth, keep_fb;
    VkPipeline composite = VK_NULL_HANDLE;
    if (!composite_pipeline(VkFormat(scene.view.format), colour_layout, dynamic, composite, why)) { fail(why); return; }
    uint64_t depth_serial = 0;
    const VkImageView scene_depth = depth_view(sdepth.view.image, sdepth.view.image_id, VkFormat(sdepth.image.format), keep_depth, depth_serial, why);
    const VkFramebuffer scene_fb = scene_depth && scene.handle ? colour_framebuffer(scene.handle, scene.view.id, VkFormat(scene.view.format),
                                                                                    colour_layout, layers->width, layers->height, keep_fb, why)
                                                               : VK_NULL_HANDLE;
    if (!scene_fb) { fail(why); return; }
    const uint32_t used = uint32_t(std::min<size_t>(captures.size(), 4));
    for (uint32_t l = 0; l < used; ++l) if (!allocate_layer(*layers, l, why)) { fail(why); return; }
    layers->used = std::chrono::steady_clock::now();
    uint32_t made = 0;
    for (uint32_t l = 0; l < 4; ++l) made |= layers->allocated[l] ? 1u << l : 0u;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (const auto found = layers->composite_sets.find({depth_serial, made}); found != layers->composite_sets.end()) set = found->second;
    else {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = layers->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &composite_set_layout_;
        if (layers->composite_sets.size() >= 32 || d_.AllocateDescriptorSets(device_->handle, &ai, &set) != VK_SUCCESS) { fail("EFMITTL composite set"); return; }
        // t100-t107 colour, t108-t115 depth: layer 0 front, layer 0 back, layer 1 front, ...; t116 the scene depth; t120 IniParams.
        // A layer not made yet is a spare 1x1 image: the composite reads only the layers in use.
        std::vector<VkDescriptorImageInfo> colours, depths;
        for (uint32_t l = 0; l < 4; ++l)
            for (uint32_t f : {1u, 0u}) {
                const bool real = layers->allocated[l];
                colours.push_back({VK_NULL_HANDLE, real ? layers->colour[l][f].view : layers->spare_colour.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                depths.push_back({VK_NULL_HANDLE, real ? layers->depth[l][f].view : layers->spare_depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
        const VkDescriptorImageInfo scene_info{VK_NULL_HANDLE, scene_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo params_info{VK_NULL_HANDLE, layers->params_views[kCalls], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::vector<VkWriteDescriptorSet> writes;
        auto write = [&](uint32_t slot, uint32_t count, const VkDescriptorImageInfo* infos) {
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set; w.dstBinding = efmi::gpu::HlslCompiler::binding('p', 't', slot); w.descriptorCount = count;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w.pImageInfo = infos; writes.push_back(w);
        };
        write(100, 8, colours.data()); write(108, 8, depths.data()); write(116, 1, &scene_info); write(120, 1, &params_info);
        d_.UpdateDescriptorSets(device_->handle, uint32_t(writes.size()), writes.data(), 0, nullptr);
        layers->composite_sets.emplace(std::make_pair(depth_serial, made), set);
    }
    VkCommandBuffer cmd = cb->handle;
    // Parameters: each call's IniParams[180] at its layer, the composite's (layers in use) at kCalls.
    std::vector<float> values((kCalls + 1) * 4, 0.0f);
    for (const auto& c : captures) std::memcpy(&values[c.params_layer * 4], c.params, sizeof(c.params));
    values[kCalls * 4] = float(used);
    {
        VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkImageMemoryBarrier b[2] = {
            image_barrier(layers->params.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, kCalls + 1),
            image_barrier(layers->neutral.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT)};
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &m, 0, nullptr, 2, b);
    }
    d_.CmdUpdateBuffer(cmd, layers->params_buffer, 0, values.size() * sizeof(float), values.data());
    {
        VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    std::vector<VkBufferImageCopy> regions;
    auto region = [&](uint32_t layer) {
        VkBufferImageCopy r = {};
        r.bufferOffset = VkDeviceSize(layer) * 16; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        r.imageOffset = {180, 0, 0}; r.imageExtent = {1, 1, 1}; regions.push_back(r);
    };
    for (const auto& c : captures) region(c.params_layer);
    region(kCalls);
    d_.CmdCopyBufferToImage(cmd, layers->params_buffer, layers->params.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d_.CmdClearColorImage(cmd, layers->neutral.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &whole);
    {
        VkImageMemoryBarrier b[2] = {
            image_barrier(layers->params.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, kCalls + 1),
            image_barrier(layers->neutral.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT)};
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    }
    // The calls in their order: the first four take a layer each, cleared; the later ones share the last.
    const VkClearValue clears[2] = {VkClearValue{{{0, 0, 0, 0}}}, VkClearValue{{{0, 0, 0, 0}}}};
    for (size_t k = 0; k < captures.size(); ++k) {
        const auto& c = captures[k];
        const uint32_t layer = uint32_t(std::min<size_t>(k, 3));
        const bool fresh = k < 4;
        for (uint32_t face = 0; face < 2; ++face) {
            VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            bi.renderPass = fresh ? layers->clear_pass : layers->load_pass; bi.framebuffer = layers->framebuffers[layer][face];
            bi.renderArea = {{0, 0}, {layers->width, layers->height}}; bi.clearValueCount = 2; bi.pClearValues = clears;
            d_.CmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
            for (uint32_t opacity = 0; opacity < 2; ++opacity) {
                const auto kind = opacity ? (face ? CaptureKind::OpacityFront : CaptureKind::OpacityBack) : (face ? CaptureKind::ColourFront : CaptureKind::ColourBack);
                const auto& v = *c.variants[uint32_t(kind)];
                d_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipeline);
                for (uint32_t s = 0; s < c.sets.size(); ++s)
                    if (c.sets[s].set) d_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, s, 1, &c.sets[s].set,
                                                               uint32_t(c.sets[s].dyn.size()), c.sets[s].dyn.empty() ? nullptr : c.sets[s].dyn.data());
                d_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, v.own_set, 1, &c.slots[opacity].set, 1, &c.slots[opacity].offset);
                d_.CmdSetViewport(cmd, 0, 1, &c.viewport);
                d_.CmdSetScissor(cmd, 0, 1, &c.scissor);
                d_.CmdBindVertexBuffers(cmd, c.geometry.first_binding, uint32_t(c.geometry.buffers.size()), c.geometry.buffers.data(), c.geometry.offsets.data());
                d_.CmdBindIndexBuffer(cmd, c.geometry.index, c.geometry.index_offset, c.geometry.index_type);
                d_.CmdDrawIndexed(cmd, c.draw.count, c.draw.instances, c.draw.first, c.draw.vertex_offset, c.draw.first_instance);
            }
            d_.CmdEndRenderPass(cmd);
        }
        // Successive calls into a shared layer read and write the same targets.
        VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &m, 0, nullptr, 0, nullptr);
        captures_.fetch_add(1, std::memory_order_relaxed);
    }
    // The composite's set holds every layer (the unused ones hold nothing it uses) and the scene depth.
    {
        std::vector<VkImageMemoryBarrier> b;
        if (made != 0xf) {
            b.push_back(image_barrier(layers->spare_colour.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                                      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT));
            b.push_back(image_barrier(layers->spare_depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                                      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
        }
        for (uint32_t l = 0; l < 4; ++l)
            for (uint32_t f = 0; f < 2; ++f) {
                if (!layers->allocated[l]) continue;
                const bool drawn = l < used;
                b.push_back(image_barrier(layers->colour[l][f].image, drawn ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, drawn ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
                                          VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT));
                b.push_back(image_barrier(layers->depth[l][f].image, drawn ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, drawn ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
                                          VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
            }
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(VkFormat(sdepth.image.format)) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        b.push_back(image_barrier(sdepth.view.image, depth_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, aspect));
        VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; m.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &m, 0, nullptr,
                              uint32_t(b.size()), b.data());
    }
    {
        VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = composite_passes_.at({uint32_t(scene.view.format), uint32_t(colour_layout)});
        bi.framebuffer = scene_fb; bi.renderArea = {{0, 0}, {layers->width, layers->height}};
        d_.CmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        d_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite);
        d_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 0, 1, &set, 0, nullptr);
        const VkViewport viewport{0, 0, float(layers->width), float(layers->height), 0, 1};
        const VkRect2D scissor{{0, 0}, {layers->width, layers->height}};
        d_.CmdSetViewport(cmd, 0, 1, &viewport); d_.CmdSetScissor(cmd, 0, 1, &scissor);
        d_.CmdDraw(cmd, 3, 1, 0, 0);
        d_.CmdEndRenderPass(cmd);
    }
    {
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(VkFormat(sdepth.image.format)) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        VkImageMemoryBarrier b = image_barrier(sdepth.view.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depth_layout, VK_ACCESS_SHADER_READ_BIT,
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, aspect);
        d_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &b);
    }
    restore(cb);
    o->keep.push_back(layers); o->keep.push_back(keep_depth); o->keep.push_back(keep_fb);
    o->used = true;
    composites_.fetch_add(1, std::memory_order_relaxed);
    error_.clear();
}

} // namespace efvk
