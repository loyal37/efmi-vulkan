#include "character_present.h"
#include "json.h"
#include <cstring>
#include <fstream>

namespace efvk {
namespace {
namespace gpu = efmi::gpu;
std::filesystem::path directory_of(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}
std::filesystem::path module_directory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_directory), &module))
        return {};
    return directory_of(module);
}
// The owned GPU tools are deployed beside the layer DLL. A replacement module exchanged into the
// game lives in another folder, so the layer's directory is tried after the module's own (D-099).
std::filesystem::path tools_directory()
{
    std::vector<std::filesystem::path> candidates{module_directory()};
    for (const wchar_t* name : {L"efmi_vk_inject.dll", L"efmi_vk_layer.dll"})
        if (const HMODULE host = GetModuleHandleW(name)) candidates.push_back(directory_of(host));
    for (const auto& directory : candidates)
        if (!directory.empty() && std::filesystem::is_regular_file(directory / L"dxcompiler.dll")) return directory;
    return candidates.front();
}
// The original's get_effective_dpi: the effective DPI of the window's nearest monitor, read with the
// thread made per-monitor aware for the call; 0 when GetDpiForMonitor is unavailable.
uint32_t effective_dpi(HWND window)
{
    using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static const GetDpi get = [] {
        HMODULE shcore = GetModuleHandleW(L"shcore.dll");
        if (!shcore) shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return shcore ? reinterpret_cast<GetDpi>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    }();
    static const SetAwareness set = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (!get) return 0;
    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const DPI_AWARENESS_CONTEXT old = set ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) : nullptr;
    UINT x = 0, y = 0;
    if (FAILED(get(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) x = 0;
    if (old) set(old);
    return x;
}
// GetIconInfo's hotspot of a cursor. A cursor handle keeps its hotspot, so the last one is kept
// instead of copying its bitmaps at every presentation.
POINT cursor_hotspot(HCURSOR cursor)
{
    static std::mutex mx;
    static HCURSOR last = nullptr;
    static POINT hotspot = {};
    std::lock_guard<std::mutex> lock(mx);
    if (cursor == last) return hotspot;
    ICONINFO icon = {};
    hotspot = {};
    if (cursor && GetIconInfo(cursor, &icon)) {
        hotspot = {static_cast<LONG>(icon.xHotspot), static_cast<LONG>(icon.yHotspot)};
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
    }
    last = cursor;
    return hotspot;
}
bool enabled(const Device* device, const char* extension)
{
    for (const auto& name : device->extensions) if (name == extension) return true;
    return false;
}
// The DXGI format the original back buffer view would report for this swap chain format.
DXGI_FORMAT back_buffer_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// A test-only copy of one presented image, honored only by the sandbox program.
std::filesystem::path readback_request()
{
    wchar_t executable[32768] = {}, value[32768] = {};
    GetModuleFileNameW(nullptr, executable, 32768);
    if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"efmi_vk_sandbox.exe")) return {};
    const DWORD length = GetEnvironmentVariableW(L"EFMI_VK_TEST_PRESENT_READBACK", value, 32768);
    return length && length < 32768 ? std::filesystem::path(value) : std::filesystem::path{};
}
} // namespace

std::filesystem::path owned_tools_directory() { return tools_directory(); }

std::shared_ptr<efmi::frontend::OwnedRuntime> OwnedPresenter::runtime(Device* device, const VkPhysicalDeviceMemoryProperties& memory,
                                                                         const std::filesystem::path& working, std::string& reason,
                                                                         VkPipelineCache pipeline_cache)
{
    try {
        if (!g_cfg.m2_owned) { reason = "owned GPU execution turned off (m2_owned = 0)"; return {}; }
        const auto tools = tools_directory();
        const auto compiler = tools / L"dxcompiler.dll", bytecode = tools / L"efmi-vkd3d-shader.dll", validator = tools / L"SPIRV-Tools-shared.dll";
        for (const auto& tool : {compiler, bytecode, validator})
            if (!std::filesystem::is_regular_file(tool)) { reason = "owned GPU tools are not deployed beside the layer: " + tool.filename().string(); return {}; }
        const uint32_t api = std::min(device->app_api_version ? device->app_api_version : VK_API_VERSION_1_0, device->api_version);
        if (api < VK_API_VERSION_1_1 || !device->material_image_properties || !device->material_format_properties) {
            reason = "owned GPU work needs Vulkan 1.1 and the device's format queries"; return {};
        }
        gpu::ResourceDevice resource;
        resource.device = device->handle; resource.memory = memory; resource.queue_family = device->material_family;
        resource.dispatch.SetDeviceLoaderData = device->set_loader_data;
#define X(n) resource.dispatch.n = reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle, "vk" #n)); \
        if (!resource.dispatch.n) { reason = "missing owned vk" #n; return {}; }
        EFMI_RESOURCE_COMMAND_FUNCS(X)
#undef X
        resource.physical = device->phys; resource.enabled_features = device->enabled_features;
        resource.enabled_depth_range_unrestricted = enabled(device, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
        resource.queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        resource.api_version = api;
        resource.GetImageFormatProperties = device->material_image_properties;
        resource.GetFormatProperties = device->material_format_properties;
        resource.format_features2 = device->format_properties2 && (api >= VK_API_VERSION_1_3 || enabled(device, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME));
        resource.GetFormatProperties2 = device->format_properties2;
        gpu::ShaderHost host;
        host.get_device_proc = device->next_gdpa; host.properties = device->physical_properties; host.validator = validator;
        host.float_controls = device->float_controls; host.float_properties = device->float_properties;
        host.null_descriptors = device->null_descriptors;
        host.texel_alignment = device->texel_alignment; host.texel_alignment_properties = device->texel_alignment_properties;
        host.shader_draw_parameters = device->draw_parameters;
        host.pipeline_cache = pipeline_cache;
        // The shipped CustomShader caches run through the exact translation (D-070). Without
        // denormal flushing the translation lowers it in software.
        efmi::frontend::OwnedRuntime::Tools owned;
        owned.compiler = compiler; owned.bytecode = bytecode;
        owned.mode = device->float_properties.shaderDenormFlushToZeroFloat32 ? gpu::DxbcFloat32Mode::Hardware : gpu::DxbcFloat32Mode::Software;
        // Beside the settings and the variant pipeline cache (D-126).
        if (g_cfg.shader_cache)
            owned.cache = (g_cfg.m2_settings.empty() ? tools / L"settings" : std::filesystem::absolute(std::filesystem::u8path(g_cfg.m2_settings))) / L"shader-cache";
        return std::make_shared<efmi::frontend::OwnedRuntime>(resource, host, owned, working);
    } catch (const std::exception& e) { reason = std::string("owned GPU runtime: ") + e.what(); return {}; }
}

void OwnedPresenter::sample(Device* device, const PresentWork* present, efmi::frontend::RuntimeInput& input)
{
    if (!device || !present || !present->info || !present->info->swapchainCount) return;
    HWND window = nullptr; VkExtent2D extent = {};
    {
        std::lock_guard<std::mutex> lock(device->swap_mx);
        const auto found = device->swapchains.find(present->info->pSwapchains[0]);
        if (found == device->swapchains.end()) return;
        window = found->second.window; extent = found->second.extent;
    }
    if (extent.width && extent.height && extent.width <= 65536 && extent.height <= 65536) {
        input.resolution_width = extent.width; input.resolution_height = extent.height;
    }
    RECT client = {}; POINT origin = {};
    if (window && GetClientRect(window, &client) && ClientToScreen(window, &origin))
        input.window = {true, origin.x, origin.y, client.right - client.left, client.bottom - client.top};
    input.effective_dpi = effective_dpi(input.window.present ? window : nullptr);
    CURSORINFO cursor = {sizeof(CURSORINFO)};
    if (GetCursorInfo(&cursor)) {
        const POINT hotspot = cursor_hotspot(cursor.hCursor);
        input.cursor = {(cursor.flags & CURSOR_SHOWING) != 0, cursor.ptScreenPos.x, cursor.ptScreenPos.y, hotspot.x, hotspot.y};
    }
}

OwnedPresenter::OwnedPresenter(Device* device, std::shared_ptr<efmi::frontend::OwnedRuntime> gpu)
    : device_(device), gpu_(std::move(gpu)), readback_(readback_request())
{
    CreateSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(device_->owned_gdpa(device_->handle, "vkCreateSemaphore"));
    DestroySemaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(device_->owned_gdpa(device_->handle, "vkDestroySemaphore"));
}

OwnedPresenter::~OwnedPresenter() { stop(); }

bool OwnedPresenter::begin(PresentWork* present, std::string& reason)
{
    current_ = nullptr;
    if (stopped_ || !present || !present->info || !present->info->swapchainCount || !present->queue) return false;
    SwapchainInfo chain;
    {
        std::lock_guard<std::mutex> lock(device_->swap_mx);
        const auto found = device_->swapchains.find(present->info->pSwapchains[0]);
        if (found == device_->swapchains.end()) return false;
        chain = found->second;
    }
    const uint32_t index = present->info->pImageIndices ? present->info->pImageIndices[0] : UINT32_MAX;
    if (index >= chain.images.size()) return false;
    try {
        auto& entry = images_[chain.images[index]];
        const auto format = back_buffer_format(chain.format);
        if (!entry.image || entry.generation != chain.generation) {
            gpu::ImageDescription description;
            description.format = chain.format; description.extent = {chain.extent.width, chain.extent.height, 1};
            description.usage = chain.usage;
            if (chain.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) description.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            entry.image = gpu_->commands().import_image(chain.images[index], description, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            entry.generation = chain.generation;
        }
        if (!entry.done) {
            VkSemaphoreCreateInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!CreateSemaphore_ || CreateSemaphore_(device_->handle, &info, nullptr, &entry.done) != VK_SUCCESS) {
                reason = "cannot create the presentation semaphore"; return false;
            }
        }
        if (!gpu_->scope_active()) {
            // The application presents on another queue than before (D-090): the owned work follows it
            // once every earlier submission completed, skipping this presentation while one still runs.
            if (gpu_->queue() && gpu_->queue() != present->queue && !follow(present->queue)) return false;
            gpu_->begin(present->queue);
        }
        if (format != DXGI_FORMAT_UNKNOWN && (chain.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            gpu_->backbuffer(entry.image, format, (chain.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
        else { gpu_->backbuffer(nullptr, DXGI_FORMAT_UNKNOWN, false); ++unsupported_; }
        current_ = &entry; ++presents_;
        return true;
    } catch (const std::exception& e) { reason = std::string("owned Present: ") + e.what(); last_error_ = reason; ++failed_; return false; }
}

void OwnedPresenter::finish(PresentWork* present, bool executed, std::string& reason, bool replay)
{
    try {
        gpu_->backbuffer(nullptr, DXGI_FORMAT_UNKNOWN, false);
        if (!replay) drew_backbuffer_ = executed && gpu_->scope_active() && gpu_->backbuffer_targeted();
        if (!gpu_->scope_active()) { current_ = nullptr; return; }
        // A scope whose work is to be dropped (the execution failed, or a command failed after recording part
        // of its work): discarded, and the next presentation's scope starts clean.
        if (!executed || gpu_->failed()) { gpu_->discard(); ++failed_; current_ = nullptr; return; }
        gpu::ResourceCommands::SubmitSync sync;
        if (current_) {
            sync.waits = present->waits;
            sync.stages.assign(sync.waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            sync.signals = {current_->done};
        }
        auto ticket = gpu_->finish(sync, present->queue);
        if (ticket && current_) {
            present->waits = {current_->done}; present->replaced = true; ++submitted_; if (replay) ++replays_;
            tickets_.push_back(std::move(ticket));
            if (!readback_.empty() && submitted_ == 20) readback(present, *current_);
        }
    } catch (const std::exception& e) { reason = std::string("owned Present submission: ") + e.what(); last_error_ = reason; ++failed_; }
    while (!tickets_.empty() && tickets_.front()->ready()) tickets_.pop_front();
    current_ = nullptr;
}

bool OwnedPresenter::follow(VkQueue queue)
{
    // Waiting here would hold the presenting thread, and the locks it holds, on the GPU (BUG-034).
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if ((*it)->ready()) it = tickets_.erase(it);
        else ++it;
    }
    if (!tickets_.empty()) { ++queue_waits_; return false; }
    gpu_->rebind(queue);
    ++queue_changes_;
    logf("owned GPU work follows the presentation to queue %p (change %llu)", static_cast<void*>(queue),
         static_cast<unsigned long long>(queue_changes_));
    return true;
}

bool OwnedPresenter::submit_pending(VkQueue queue, std::string& reason)
{
    try {
        if (stopped_ || gpu_->scope_active() || !gpu_->pending()) return true;
        gpu_->begin(queue);
        auto ticket = gpu_->finish({}, queue);
        if (ticket) { tickets_.push_back(std::move(ticket)); ++draw_submitted_; }
    } catch (const std::exception& e) {
        reason = std::string("owned draw work submission: ") + e.what(); last_error_ = reason; ++failed_;
        if (gpu_->scope_active()) gpu_->discard();
        return false;
    }
    while (!tickets_.empty() && tickets_.front()->ready()) tickets_.pop_front();
    return true;
}

void OwnedPresenter::readback(PresentWork* present, const Presented& entry)
{
    gpu_->begin(present->queue);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = entry.image->extent();
    const auto bytes = gpu_->commands().readback(present->queue, entry.image, region);
    gpu_->finish({}, present->queue);
    std::ofstream out(readback_, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream meta(readback_.wstring() + L".json");
    meta << "{\"width\":" << region.imageExtent.width << ",\"height\":" << region.imageExtent.height
         << ",\"format\":" << int(entry.image->description().format) << "}\n";
}

void OwnedPresenter::stop()
{
    if (stopped_) return;
    stopped_ = true;
    for (auto& ticket : tickets_) ticket->wait();
    tickets_.clear();
    if (gpu_ && gpu_->scope_active()) gpu_->discard();
    for (auto& entry : images_) {
        entry.second.image.reset();
        // The presentation that waits on it may still be pending: destroyed a few presentations later.
        if (entry.second.done && DestroySemaphore_) retire_semaphore(device_, entry.second.done, DestroySemaphore_);
    }
    images_.clear();
}

void OwnedPresenter::status(JsonWriter& w) const
{
    w.key("owned_present"); w.begin_object();
    w.kn("presents", presents_); w.kn("submitted", submitted_); w.kn("failed", failed_);
    w.kn("unsupported_formats", unsupported_); w.kn("images", images_.size()); w.kn("in_flight", tickets_.size());
    w.kn("draw_work_submitted", draw_submitted_); w.kn("replays", replays_);
    w.kn("queue_changes", queue_changes_); w.kn("queue_waits", queue_waits_);
    // What the owned work allocated from the driver, and reused instead (BUG-058).
    const auto& stats = gpu_->commands().stats();
    w.kn("allocations", stats.allocations.load()); w.kn("descriptor_pools", stats.descriptor_pools.load());
    w.kn("staging_reused", stats.staging_reused.load()); w.kn("sets_reused", stats.sets_reused.load());
    // What its CustomShaders cost at load: compiled from HLSL, translated from shipped caches.
    const auto shaders = gpu_->shader_stats();
    w.key("shaders"); w.begin_object();
    w.kn("compiled", shaders.compiled); w.kn("compile_us", shaders.compile_us);
    w.kn("translated", shaders.translated); w.kn("translate_us", shaders.translate_us); w.kn("cached", shaders.cached);
    w.end_object();
    w.kv("last_error", last_error_);
    w.end_object();
}
} // namespace efvk
