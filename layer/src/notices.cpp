// The overlay's notices (D-137), drawn over the presented image as 3DMigoto draws its notices
// (Overlay.cpp DrawNotices): from the top left, a line each, the text in its level's colour on a black
// backing of 75 %, the next line 5 pixels lower. 3DMigoto's notifications font is Arial; GDI draws the
// lines here with it, which also shows a Mod's name in any script. The text image is drawn into the
// presented image by a render pass of its own on the presenting queue, after the presentation's other
// work, which the presentation then waits for.
#include "notices.h"

#include "character_module.h"
#include "json.h"
#include "state.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

namespace efvk {

extern const uint32_t overlay_vertex_spirv[];
extern const size_t overlay_vertex_spirv_words;
extern const uint32_t overlay_fragment_spirv[];
extern const size_t overlay_fragment_spirv_words;

namespace {

struct Raster {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // premultiplied
};

// Overlay.cpp log_levels' colours: Red, OrangeRed, Orange, LimeGreen.
const uint8_t kColours[4][3] = {{255, 0, 0}, {255, 69, 0}, {255, 165, 0}, {50, 205, 50}};

std::wstring wide(const std::string& text)
{
    std::wstring out(text.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<size_t>(n) - 1 : 0);
    return out;
}

// The lines in Arial of `pixels` height, as many as fit `max_width` by `max_height`. GDI draws white on
// black; its coverage becomes the alpha of the level's colour over the backing.
Raster rasterize(const std::vector<NoticeLine>& lines, uint32_t pixels, uint32_t max_width, uint32_t max_height)
{
    Raster out;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return out;
    HFONT font = CreateFontW(-static_cast<int>(pixels), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    struct Line { std::wstring text; SIZE size; NoticeLevel level; };
    std::vector<Line> measured;
    uint32_t width = 0, height = 0;
    for (const auto& line : lines) {
        Line l{wide(line.text), {}, line.level};
        if (!GetTextExtentPoint32W(dc, l.text.c_str(), static_cast<int>(l.text.size()), &l.size) || l.size.cy <= 0) continue;
        const uint32_t next = height + (measured.empty() ? 0 : 5) + static_cast<uint32_t>(l.size.cy);
        if (next > max_height) break;
        height = next;
        width = std::max(width, std::min(max_width, static_cast<uint32_t>(l.size.cx) + 3));
        measured.push_back(std::move(l));
    }
    if (width && height) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
            std::memset(bits, 0, size_t(width) * height * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            int y = 0;
            for (const auto& l : measured) {
                TextOutW(dc, 0, y, l.text.c_str(), static_cast<int>(l.text.size()));
                y += l.size.cy + 5;
            }
            GdiFlush();
            out.width = width; out.height = height;
            out.rgba.assign(size_t(width) * height * 4, 0);
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            y = 0;
            for (const auto& l : measured) {
                const uint8_t* colour = kColours[static_cast<int>(l.level)];
                const uint32_t right = std::min(width, static_cast<uint32_t>(l.size.cx) + 3);
                for (int row = y; row < y + l.size.cy && row < static_cast<int>(height); ++row)
                    for (uint32_t x = 0; x < right; ++x) {
                        const uint8_t* p = src + (size_t(row) * width + x) * 4;
                        const float cover = std::max({p[0], p[1], p[2]}) / 255.0f;
                        const float alpha = cover + 0.75f * (1.0f - cover);
                        uint8_t* d = &out.rgba[(size_t(row) * width + x) * 4];
                        for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(colour[c] * cover + 0.5f);
                        d[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
                    }
                y += l.size.cy + 5;
            }
            SelectObject(dc, old_bitmap);
        }
        if (bitmap) DeleteObject(bitmap);
    }
    if (old_font) SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    return out;
}

bool srgb(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

#define EFVK_NOTICE_FUNCS(X)                                                                                              \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(AllocateMemory) X(FreeMemory)   \
    X(MapMemory) X(UnmapMemory) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)          \
    X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateRenderPass)                 \
    X(DestroyRenderPass) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)                        \
    X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)              \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines)                  \
    X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool)           \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdBeginRenderPass)          \
    X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport)                 \
    X(CmdSetScissor) X(CmdDraw) X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences)          \
    X(CreateSemaphore) X(DestroySemaphore) X(QueueSubmit) X(CmdCopyImageToBuffer)

struct Slot {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize staging_size = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t raster = 0; // the raster its image holds
};

struct Target {
    uint32_t generation = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};

struct Pass { VkRenderPass pass = VK_NULL_HANDLE; VkPipeline pipeline = VK_NULL_HANDLE; };

struct Overlay {
    Device* dev = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    struct {
#define X(n) PFN_vk##n n = nullptr;
        EFVK_NOTICE_FUNCS(X)
#undef X
    } f;
    bool usable = false;
    std::string error;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    std::map<VkFormat, Pass> passes;
    std::map<VkImage, Target> targets;
    // One per presentation whose overlay work the GPU has not finished. The work on a presented image
    // waits for that image's own rendering, so as many are pending as the swap chain has images in
    // flight, and a frame generator presents several for one rendered frame: a slot is made when none
    // is free, never waited for (BUG-092, BUG-062).
    std::vector<std::unique_ptr<Slot>> slots;
    bool holding = false; // a slot has a text image
    std::vector<NoticeLine> lines;
    uint64_t version = 0;   // of the lines
    uint64_t rastered = 0;  // the raster's number
    uint32_t raster_pixels = 0;
    VkExtent2D raster_bounds = {};
    Raster raster;
};

// More than any swap chain has images; past it a presentation goes without the notices and is counted.
constexpr uint32_t kSlotLimit = 32;

std::mutex g_mx; // the overlays; one presenting thread at a time per device in practice
std::map<Device*, std::unique_ptr<Overlay>> g_overlays;
struct {
    uint64_t drawn = 0, busy = 0, unsupported = 0, failed = 0, lines = 0;
    std::string last, error;
} g_stats;

// A test's child process may ask for the presented image with the notices on it, copied once
// (EFMI_VK_TEST_NOTICES_READBACK: the file; the image needs transfer use, the sandbox's
// --readable-swapchain), and for a notice at the start (EFMI_VK_TEST_NOTICE: its text, as information).
std::wstring test_value(const wchar_t* name)
{
    wchar_t value[1024];
    const DWORD n = GetEnvironmentVariableW(name, value, 1024);
    return n && n < 1024 ? std::wstring(value, n) : std::wstring();
}
struct {
    std::wstring path = test_value(L"EFMI_VK_TEST_NOTICES_READBACK");
    std::wstring trigger = test_value(L"EFMI_VK_TEST_NOTICES_READBACK_TRIGGER");
    bool recorded = false, written = false;
    int slot = -1;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
} g_readback;

uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want &&
            !(memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                                     VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)))
            return i;
    return UINT32_MAX;
}

// One more slot: a fence (signalled), a command buffer of its own pool and a descriptor set; its text
// image comes with its first use. Null when the device gives none, or at the limit.
Slot* make_slot(Overlay& o)
{
    if (o.slots.size() >= kSlotLimit) return nullptr;
    auto made = std::make_unique<Slot>();
    Slot& s = *made;
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; ci.queueFamilyIndex = o.dev->material_family;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkDescriptorSetAllocateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    di.descriptorPool = o.descriptors; di.descriptorSetCount = 1; di.pSetLayouts = &o.set_layout;
    bool ok = o.f.CreateFence(o.device, &fi, nullptr, &s.fence) == VK_SUCCESS &&
              o.f.CreateCommandPool(o.device, &ci, nullptr, &s.pool) == VK_SUCCESS;
    if (ok) {
        ai.commandPool = s.pool;
        ok = o.f.AllocateCommandBuffers(o.device, &ai, &s.cb) == VK_SUCCESS &&
             (!o.dev->set_loader_data || o.dev->set_loader_data(o.device, s.cb) == VK_SUCCESS) &&
             o.f.AllocateDescriptorSets(o.device, &di, &s.set) == VK_SUCCESS; // last: the pool frees no single set
    }
    if (!ok) {
        if (s.pool) o.f.DestroyCommandPool(o.device, s.pool, nullptr);
        if (s.fence) o.f.DestroyFence(o.device, s.fence, nullptr);
        return nullptr;
    }
    o.slots.push_back(std::move(made));
    return o.slots.back().get();
}

bool init(Overlay& o)
{
    auto fail = [&o](const char* why) { o.error = why; return false; };
#define X(n) o.f.n = reinterpret_cast<PFN_vk##n>(o.dev->owned_gdpa(o.device, "vk" #n)); if (!o.f.n) return fail("missing vk" #n);
    EFVK_NOTICE_FUNCS(X)
#undef X
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1; sl.pBindings = &binding;
    if (o.f.CreateDescriptorSetLayout(o.device, &sl, nullptr, &o.set_layout) != VK_SUCCESS) return fail("descriptor set layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &o.set_layout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    if (o.f.CreatePipelineLayout(o.device, &pl, nullptr, &o.layout) != VK_SUCCESS) return fail("pipeline layout");
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotLimit};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kSlotLimit; dp.poolSizeCount = 1; dp.pPoolSizes = &size;
    if (o.f.CreateDescriptorPool(o.device, &dp, nullptr, &o.descriptors) != VK_SUCCESS) return fail("descriptor pool");
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = overlay_vertex_spirv_words * 4; sm.pCode = overlay_vertex_spirv;
    if (o.f.CreateShaderModule(o.device, &sm, nullptr, &o.vertex) != VK_SUCCESS) return fail("vertex shader");
    sm.codeSize = overlay_fragment_spirv_words * 4; sm.pCode = overlay_fragment_spirv;
    if (o.f.CreateShaderModule(o.device, &sm, nullptr, &o.fragment) != VK_SUCCESS) return fail("fragment shader");
    if (!make_slot(o)) return fail("command buffer, fence or descriptor set");
    return true;
}

Pass* pass_for(Overlay& o, VkFormat format)
{
    auto found = o.passes.find(format);
    if (found != o.passes.end()) return &found->second;
    Pass p;
    VkAttachmentDescription a = {};
    a.format = format; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    // The image's earlier writes: the presentation's semaphores waited at this stage.
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0; dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1; rp.pAttachments = &a; rp.subpassCount = 1; rp.pSubpasses = &sub; rp.dependencyCount = 1; rp.pDependencies = &dep;
    if (o.f.CreateRenderPass(o.device, &rp, nullptr, &p.pass) != VK_SUCCESS) return nullptr;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = o.vertex; stages[0].pName = "vs_main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = o.fragment; stages[1].pName = "ps_main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};
    // Premultiplied over the frame; the frame's alpha stays.
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &blend;
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = o.layout; gp.renderPass = p.pass; gp.subpass = 0;
    if (o.f.CreateGraphicsPipelines(o.device, VK_NULL_HANDLE, 1, &gp, nullptr, &p.pipeline) != VK_SUCCESS) {
        o.f.DestroyRenderPass(o.device, p.pass, nullptr);
        return nullptr;
    }
    return &(o.passes[format] = p);
}

void destroy_target(Overlay& o, Target& t)
{
    if (t.framebuffer) o.f.DestroyFramebuffer(o.device, t.framebuffer, nullptr);
    if (t.view) o.f.DestroyImageView(o.device, t.view, nullptr);
    // The presentation that waited for it may still be queued: destroyed a few presentations later.
    if (t.done) retire_semaphore(o.dev, t.done, o.f.DestroySemaphore);
    t = {};
}

Target* target_for(Overlay& o, VkImage image, const SwapchainInfo& chain, Pass& pass)
{
    auto& t = o.targets[image];
    if (t.view && (t.generation != chain.generation || t.pass != pass.pass)) destroy_target(o, t);
    if (t.view) return &t;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = chain.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (o.f.CreateImageView(o.device, &vi, nullptr, &t.view) != VK_SUCCESS) { t = {}; return nullptr; }
    VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass.pass; fb.attachmentCount = 1; fb.pAttachments = &t.view;
    fb.width = chain.extent.width; fb.height = chain.extent.height; fb.layers = 1;
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (o.f.CreateFramebuffer(o.device, &fb, nullptr, &t.framebuffer) != VK_SUCCESS ||
        o.f.CreateSemaphore(o.device, &si, nullptr, &t.done) != VK_SUCCESS) {
        destroy_target(o, t);
        return nullptr;
    }
    t.generation = chain.generation; t.pass = pass.pass;
    return &t;
}

void release_slot_image(Overlay& o, Slot& s)
{
    if (s.view) o.f.DestroyImageView(o.device, s.view, nullptr);
    if (s.image) o.f.DestroyImage(o.device, s.image, nullptr);
    if (s.image_memory) o.f.FreeMemory(o.device, s.image_memory, nullptr);
    if (s.mapped) o.f.UnmapMemory(o.device, s.staging_memory);
    if (s.staging) o.f.DestroyBuffer(o.device, s.staging, nullptr);
    if (s.staging_memory) o.f.FreeMemory(o.device, s.staging_memory, nullptr);
    s.view = VK_NULL_HANDLE; s.image = VK_NULL_HANDLE; s.image_memory = VK_NULL_HANDLE; s.mapped = nullptr;
    s.staging = VK_NULL_HANDLE; s.staging_memory = VK_NULL_HANDLE; s.staging_size = 0; s.width = s.height = 0; s.raster = 0;
}

// The slot's text image and staging buffer, large enough for the raster. Its fence was signalled.
bool slot_sized(Overlay& o, Slot& s, const Raster& r)
{
    if (s.image && s.width >= r.width && s.height >= r.height) return true;
    release_slot_image(o, s);
    const uint32_t width = std::max<uint32_t>(r.width, 256), height = std::max<uint32_t>(r.height, 64);
    const auto& memory = o.dev->memory_properties;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(width) * height * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (o.f.CreateBuffer(o.device, &bi, nullptr, &s.staging) != VK_SUCCESS) return false;
    VkMemoryRequirements req; o.f.GetBufferMemoryRequirements(o.device, s.staging, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || o.f.AllocateMemory(o.device, &ai, nullptr, &s.staging_memory) != VK_SUCCESS ||
        o.f.BindBufferMemory(o.device, s.staging, s.staging_memory, 0) != VK_SUCCESS ||
        o.f.MapMemory(o.device, s.staging_memory, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS) { release_slot_image(o, s); return false; }
    s.staging_size = bi.size;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {width, height, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (o.f.CreateImage(o.device, &ii, nullptr, &s.image) != VK_SUCCESS) { release_slot_image(o, s); return false; }
    o.f.GetImageMemoryRequirements(o.device, s.image, &req);
    ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (ai.memoryTypeIndex == UINT32_MAX || o.f.AllocateMemory(o.device, &ai, nullptr, &s.image_memory) != VK_SUCCESS ||
        o.f.BindImageMemory(o.device, s.image, s.image_memory, 0) != VK_SUCCESS) { release_slot_image(o, s); return false; }
    vi.image = s.image;
    if (o.f.CreateImageView(o.device, &vi, nullptr, &s.view) != VK_SUCCESS) { release_slot_image(o, s); return false; }
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, s.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.set; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w.pImageInfo = &image;
    o.f.UpdateDescriptorSets(o.device, 1, &w, 0, nullptr);
    s.width = width; s.height = height;
    return true;
}

bool draw(Overlay& o, PresentWork* present, const SwapchainInfo& chain, VkImage image)
{
    Pass* pass = pass_for(o, chain.format);
    if (!pass) { g_stats.error = "overlay pipeline for the swap chain's format"; return false; }
    Target* target = target_for(o, image, chain, *pass);
    if (!target) { g_stats.error = "overlay target of the swap chain image"; return false; }
    // The first slot the GPU is done with, or one more: every presented image carries the notices, as
    // the original draws them at every Present. Nothing waits here (BUG-062); the first free one keeps
    // the text images to as many as were pending at once.
    Slot* slot = nullptr;
    for (auto& s : o.slots)
        if (o.f.GetFenceStatus(o.device, s->fence) == VK_SUCCESS) { slot = s.get(); break; }
    if (!slot) slot = make_slot(o);
    if (!slot) { ++g_stats.busy; return true; }
    const Raster& r = o.raster;
    if (!slot_sized(o, *slot, r)) { g_stats.error = "overlay text image"; return false; }
    o.holding = true;
    if (o.f.ResetCommandPool(o.device, slot->pool, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (o.f.BeginCommandBuffer(slot->cb, &bi) != VK_SUCCESS) return false;
    if (slot->raster != o.rastered) {
        for (uint32_t row = 0; row < r.height; ++row)
            std::memcpy(static_cast<uint8_t*>(slot->mapped) + size_t(row) * r.width * 4, &r.rgba[size_t(row) * r.width * 4], size_t(r.width) * 4);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = slot->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        o.f.CmdPipelineBarrier(slot->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy = {};
        copy.bufferRowLength = r.width; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {r.width, r.height, 1};
        o.f.CmdCopyBufferToImage(slot->cb, slot->staging, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        o.f.CmdPipelineBarrier(slot->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        slot->raster = o.rastered;
    }
    const uint32_t width = std::min(r.width, chain.extent.width), height = std::min(r.height, chain.extent.height);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass->pass; rb.framebuffer = target->framebuffer; rb.renderArea = {{0, 0}, {width, height}};
    o.f.CmdBeginRenderPass(slot->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = {0.0f, 0.0f, float(r.width), float(r.height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {width, height}};
    o.f.CmdSetViewport(slot->cb, 0, 1, &viewport);
    o.f.CmdSetScissor(slot->cb, 0, 1, &scissor);
    o.f.CmdBindPipeline(slot->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->pipeline);
    o.f.CmdBindDescriptorSets(slot->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o.layout, 0, 1, &slot->set, 0, nullptr);
    const uint32_t encoded = srgb(chain.format) ? 1u : 0u;
    o.f.CmdPushConstants(slot->cb, o.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &encoded);
    o.f.CmdDraw(slot->cb, 3, 1, 0, 0);
    o.f.CmdEndRenderPass(slot->cb);
    const bool four_bytes = chain.format == VK_FORMAT_B8G8R8A8_UNORM || chain.format == VK_FORMAT_B8G8R8A8_SRGB ||
                            chain.format == VK_FORMAT_R8G8B8A8_UNORM || chain.format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!g_readback.path.empty() && !g_readback.recorded && four_bytes && (chain.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (g_readback.trigger.empty() || GetFileAttributesW(g_readback.trigger.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(chain.extent.width) * chain.extent.height * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements req = {};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        if (o.f.CreateBuffer(o.device, &bi, nullptr, &g_readback.buffer) == VK_SUCCESS) {
            o.f.GetBufferMemoryRequirements(o.device, g_readback.buffer, &req);
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memory_type(o.dev->memory_properties, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (g_readback.buffer && ai.memoryTypeIndex != UINT32_MAX && o.f.AllocateMemory(o.device, &ai, nullptr, &g_readback.memory) == VK_SUCCESS &&
            o.f.BindBufferMemory(o.device, g_readback.buffer, g_readback.memory, 0) == VK_SUCCESS) {
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            o.f.CmdPipelineBarrier(slot->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {chain.extent.width, chain.extent.height, 1};
            o.f.CmdCopyImageToBuffer(slot->cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer, 1, &copy);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0;
            o.f.CmdPipelineBarrier(slot->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            g_readback.recorded = true;
            for (size_t i = 0; i < o.slots.size(); ++i)
                if (o.slots[i].get() == slot) g_readback.slot = static_cast<int>(i);
            g_readback.width = chain.extent.width; g_readback.height = chain.extent.height; g_readback.format = chain.format;
        }
    }
    if (o.f.EndCommandBuffer(slot->cb) != VK_SUCCESS) return false;
    std::vector<VkPipelineStageFlags> stages(present->waits.size(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = static_cast<uint32_t>(present->waits.size()); si.pWaitSemaphores = present->waits.data();
    si.pWaitDstStageMask = stages.data(); si.commandBufferCount = 1; si.pCommandBuffers = &slot->cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &target->done;
    if (o.f.ResetFences(o.device, 1, &slot->fence) != VK_SUCCESS) return false;
    if (o.f.QueueSubmit(present->queue, 1, &si, slot->fence) != VK_SUCCESS) {
        // The fence was reset and nothing will signal it: the slot is made usable again.
        o.f.DestroyFence(o.device, slot->fence, nullptr); slot->fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        o.f.CreateFence(o.device, &fi, nullptr, &slot->fence);
        g_stats.error = "overlay submission";
        return false;
    }
    present->waits = {target->done};
    present->replaced = true;
    ++g_stats.drawn;
    return true;
}

// Waits for the overlay's own work (outside a presentation: the swap chain or the device is going).
void idle(Overlay& o)
{
    for (auto& s : o.slots)
        if (s->fence) o.f.WaitForFences(o.device, 1, &s->fence, VK_TRUE, 1000000000ull);
}

// No notice is shown any more: the text images of the slots the GPU is done with are given back.
void release_text(Overlay& o)
{
    bool held = false;
    for (auto& s : o.slots) {
        if (!s->image && !s->staging) continue;
        if (o.f.GetFenceStatus(o.device, s->fence) == VK_SUCCESS) release_slot_image(o, *s);
        else held = true;
    }
    o.holding = held;
}

void destroy(Overlay& o)
{
    if (!o.usable && !o.f.DestroyFence) return;
    idle(o);
    for (auto& t : o.targets) destroy_target(o, t.second);
    o.targets.clear();
    for (auto& s : o.slots) {
        release_slot_image(o, *s);
        if (s->pool) o.f.DestroyCommandPool(o.device, s->pool, nullptr);
        if (s->fence) o.f.DestroyFence(o.device, s->fence, nullptr);
    }
    o.slots.clear(); o.holding = false;
    for (auto& p : o.passes) {
        if (p.second.pipeline) o.f.DestroyPipeline(o.device, p.second.pipeline, nullptr);
        if (p.second.pass) o.f.DestroyRenderPass(o.device, p.second.pass, nullptr);
    }
    o.passes.clear();
    if (o.vertex) o.f.DestroyShaderModule(o.device, o.vertex, nullptr);
    if (o.fragment) o.f.DestroyShaderModule(o.device, o.fragment, nullptr);
    if (o.descriptors) o.f.DestroyDescriptorPool(o.device, o.descriptors, nullptr);
    if (o.layout) o.f.DestroyPipelineLayout(o.device, o.layout, nullptr);
    if (o.set_layout) o.f.DestroyDescriptorSetLayout(o.device, o.set_layout, nullptr);
    o.usable = false;
}

} // namespace

void notices_present(Device* dev, PresentWork* present)
{
    if (!dev || dev->passive || !present || !present->info || !present->info->swapchainCount || !present->queue) return;
    static const bool test_notice = [] {
        const std::wstring text = test_value(L"EFMI_VK_TEST_NOTICE");
        if (text.empty()) return false;
        char utf8[3072] = {};
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        notice(NoticeLevel::Info, "test notice", utf8);
        return true;
    }();
    (void)test_notice;
    std::lock_guard<std::mutex> lock(g_mx);
    auto& slot = g_overlays[dev];
    if (slot && slot->usable && g_readback.recorded && !g_readback.written && g_readback.slot >= 0 &&
        size_t(g_readback.slot) < slot->slots.size() &&
        slot->f.GetFenceStatus(slot->device, slot->slots[size_t(g_readback.slot)]->fence) == VK_SUCCESS) {
        void* data = nullptr;
        const size_t bytes = size_t(g_readback.width) * g_readback.height * 4;
        if (slot->f.MapMemory(slot->device, g_readback.memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
            std::ofstream out(g_readback.path, std::ios::binary);
            out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            std::ofstream meta(g_readback.path + L".json");
            meta << "{\"width\":" << g_readback.width << ",\"height\":" << g_readback.height << ",\"format\":" << int(g_readback.format) << "}\n";
            slot->f.UnmapMemory(slot->device, g_readback.memory);
        }
        slot->f.DestroyBuffer(slot->device, g_readback.buffer, nullptr); slot->f.FreeMemory(slot->device, g_readback.memory, nullptr);
        g_readback.buffer = VK_NULL_HANDLE; g_readback.memory = VK_NULL_HANDLE; g_readback.written = true;
    }
    // Nothing shown yet on this device and nothing to show: no Vulkan object is made.
    if (!slot) {
        std::vector<NoticeLine> lines;
        uint64_t version = 0;
        if (!notices_visible(lines, version) || lines.empty()) return;
        slot = std::make_unique<Overlay>();
        slot->dev = dev; slot->device = dev->handle;
        slot->usable = init(*slot);
        if (!slot->usable) { g_stats.error = "overlay: " + slot->error; logf("notices: no overlay: %s", slot->error.c_str()); }
    }
    Overlay& o = *slot;
    notices_visible(o.lines, o.version);
    g_stats.lines = o.lines.size();
    if (o.usable && o.lines.empty() && o.holding) release_text(o);
    if (!o.usable || o.lines.empty()) return;
    // The presenting queue draws it: one of the graphics family the layer's work knows.
    if (std::find(dev->material_queues.begin(), dev->material_queues.end(), present->queue) == dev->material_queues.end()) {
        ++g_stats.unsupported; return;
    }
    SwapchainInfo chain;
    {
        std::lock_guard<std::mutex> swap(dev->swap_mx);
        const auto found = dev->swapchains.find(present->info->pSwapchains[0]);
        if (found == dev->swapchains.end()) return;
        chain = found->second;
    }
    const uint32_t index = present->info->pImageIndices ? present->info->pImageIndices[0] : UINT32_MAX;
    if (index >= chain.images.size() || !(chain.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !chain.extent.width || !chain.extent.height) {
        ++g_stats.unsupported; return;
    }
    // The size of 3DMigoto's notification font at 1080 lines, scaled with the image.
    const uint32_t pixels = std::clamp<uint32_t>(chain.extent.height / 54, 14, 48);
    if (o.rastered != o.version || o.raster_pixels != pixels || o.raster_bounds.width != chain.extent.width ||
        o.raster_bounds.height != chain.extent.height) {
        o.raster = rasterize(o.lines, pixels, chain.extent.width, chain.extent.height);
        o.rastered = o.version; o.raster_pixels = pixels; o.raster_bounds = chain.extent;
        if (!o.lines.empty()) g_stats.last = o.lines.front().text;
    }
    if (!o.raster.width || !o.raster.height) return;
    if (!draw(o, present, chain, chain.images[index])) ++g_stats.failed;
}

void notices_swapchain_gone(Device* dev, const std::vector<VkImage>& images)
{
    std::lock_guard<std::mutex> lock(g_mx);
    const auto found = g_overlays.find(dev);
    if (found == g_overlays.end() || !found->second || !found->second->usable) return;
    Overlay& o = *found->second;
    bool any = false;
    for (const auto image : images) any |= o.targets.count(image) != 0;
    if (!any) return;
    idle(o);
    for (const auto image : images) {
        const auto t = o.targets.find(image);
        if (t == o.targets.end()) continue;
        destroy_target(o, t->second);
        o.targets.erase(t);
    }
}

void notices_device_gone(Device* dev)
{
    std::lock_guard<std::mutex> lock(g_mx);
    const auto found = g_overlays.find(dev);
    if (found == g_overlays.end()) return;
    if (found->second) destroy(*found->second);
    g_overlays.erase(found);
}

void notices_status(JsonWriter& w)
{
    std::vector<std::string> recent;
    const uint64_t queued = notices_queued(recent);
    std::lock_guard<std::mutex> lock(g_mx);
    w.key("notices"); w.begin_object();
    w.kn("queued", queued);
    w.key("recent"); w.begin_array();
    for (const auto& text : recent) w.str(text);
    w.end_array();
    w.kn("lines", g_stats.lines); w.kn("drawn", g_stats.drawn); w.kn("busy", g_stats.busy);
    // The slots made (as many as were pending at once at most) and what their text images hold now.
    uint64_t slots = 0, text_bytes = 0;
    for (const auto& overlay : g_overlays) {
        if (!overlay.second) continue;
        slots += overlay.second->slots.size();
        for (const auto& s : overlay.second->slots)
            text_bytes += s->staging_size + (s->image ? uint64_t(s->width) * s->height * 4 : 0);
    }
    w.kn("slots", slots); w.kn("text_bytes", text_bytes);
    w.kn("unsupported", g_stats.unsupported); w.kn("failed", g_stats.failed);
    w.kv("last", g_stats.last); w.kv("error", g_stats.error);
    w.kb("readback", g_readback.written);
    w.end_object();
}

} // namespace efvk
