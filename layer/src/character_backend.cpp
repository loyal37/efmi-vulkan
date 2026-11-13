// The character backend of a device: the folders and packages it is configured with, the device's setup,
// its creation and stop, and what the module is called for when a pipeline is retired, a pass ends, the
// application submits and a frame is presented. Its other parts: character_backend_internal.h.
#include "character_backend_internal.h"

namespace efvk {
namespace {
efmi::frontend::RuntimeInput sample_ini_input()
{
    efmi::frontend::RuntimeInput input;
    // Deterministic headless GPU tests supply input inside their own child
    // process only. Never send keyboard events or accept this path in the game.
    static const std::filesystem::path fixture = [] {
        wchar_t executable[32768] = {}, path[32768] = {};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto name = std::filesystem::path(executable).filename();
        if (_wcsicmp(name.c_str(), L"efmi_vk_variant_gpu.exe") && _wcsicmp(name.c_str(), L"efmi_vk_sandbox.exe")) return std::filesystem::path{};
        const DWORD length = GetEnvironmentVariableW(L"EFMI_VK_TEST_KEYS", path, 32768);
        return length && length < 32768 ? std::filesystem::path(path) : std::filesystem::path{};
    }();
    if (!fixture.empty()) {
        std::ifstream in(fixture); int foreground = 0, key = 0, count = 0;
        if (in >> foreground) input.foreground = foreground == 1;
        while (count++ < 256 && in >> key) if (key >= 0 && key < 256) input.keys[key] = true;
        return input;
    }
    DWORD foreground = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    input.foreground = foreground == GetCurrentProcessId();
    if (input.foreground) for (int key = 0; key < 256; ++key) input.keys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    return input;
}

// The character Mods of an EFMI Mods folder (D-078).
std::vector<std::string> mods_characters(std::string& reason)
{
    std::vector<std::string> found;
    for (const auto& path : CharacterProgram::mods_characters(mods_folder(), reason)) found.push_back(path.u8string());
    if (found.size() > kMaxPackages) { found.resize(kMaxPackages); logf("character Mods: more than 4096 in %s, the first 4096 are used", mods_folder().u8string().c_str()); }
    if (found.empty() && reason.empty()) reason = "no character Mod in " + mods_folder().u8string();
    return found;
}
} // namespace

namespace character_internal {
// Directory generation (D-078): a character Mod is named by its INI instead of a package file.
bool is_mod_ini(const std::string& path)
{
    const auto extension = std::filesystem::u8path(path).extension().wstring();
    return !_wcsicmp(extension.c_str(), L".ini");
}
// A configured folder; a relative one is taken from the layer's own folder, where its configuration
// is (the release names `../Mods` from EFMI\EFMI-Vulkan), never from the game's working directory.
std::filesystem::path absolute_folder(const std::string& path)
{
    std::error_code error;
    auto named = std::filesystem::u8path(path);
    if (named.is_relative()) named = module_folder() / named;
    auto folder = std::filesystem::absolute(named, error).lexically_normal();
    if (folder.has_filename()) return folder;
    return folder.parent_path();
}
// The configured Mods folder. `auto` (the release's) is the Mods folder of the EFMI folder the layer sits
// in or under: the first folder upward from the DLL's own that holds Mods and Core\EFMI, so the release
// works unpacked into EFMI (EFMI\EFMI-Vulkan) and into EFMI\Mods alike.
std::filesystem::path mods_folder()
{
    if (g_cfg.m2_mods != "auto") return absolute_folder(g_cfg.m2_mods);
    static const std::filesystem::path found = [] {
        const auto start = module_folder();
        std::error_code error;
        for (auto at = start; !at.empty() && at.has_relative_path(); at = at.parent_path())
            if (std::filesystem::is_directory(at / "Mods", error) && std::filesystem::is_directory(at / "Core" / "EFMI", error)) {
                logf("Mods folder found from the layer's folder: %s", (at / "Mods").u8string().c_str());
                return at / "Mods";
            }
        logf("Mods folder not found: no folder at or above %s holds Mods and Core\\EFMI. Put EFMI-Vulkan into the EFMI folder of the "
             "XXMI Launcher, or name the Mods folder in efmi_vk.cfg (m2_mods)", start.u8string().c_str());
        return std::filesystem::path{};
    }();
    return found;
}
std::filesystem::path module_folder()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_folder), &module))
        return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}
CharacterProgram::Source mod_source(const std::string& ini)
{
    CharacterProgram::Source source;
    source.ini = std::filesystem::absolute(std::filesystem::u8path(ini)).lexically_normal();
    const auto mods = mods_folder();
    source.core = g_cfg.m2_core.empty() ? mods.parent_path() / "Core" / "EFMI" : absolute_folder(g_cfg.m2_core);
    source.settings = g_cfg.m2_settings.empty() ? module_folder() / "settings" : absolute_folder(g_cfg.m2_settings);
    source.mod_namespace = CharacterProgram::declared_namespace(source.ini);
    source.mods = mods;
    return source;
}
std::vector<std::string> configured_packages(std::string& reason)
{
    if (!g_cfg.m2_mods.empty()) return mods_characters(reason);
    const std::string& path = g_cfg.m2_package;
    std::error_code error;
    const auto where = std::filesystem::u8path(path);
    if (!std::filesystem::is_directory(where, error)) return {path};
    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator(where, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".bin") continue;
        std::filesystem::path sidecar = entry.path();
        sidecar += ".runtime.json";
        if (std::filesystem::is_regular_file(sidecar, error)) found.push_back(entry.path().u8string());
    }
    std::sort(found.begin(), found.end());
    if (found.size() > 32) { found.resize(32); logf("character packages: more than 32 in %s, the first 32 are used", path.c_str()); }
    if (found.empty()) reason = "no live character package in " + path;
    return found;
}
} // namespace character_internal

void CharacterBackend::open_pipeline_cache()
{
    if (!device) return;
    const auto create = reinterpret_cast<PFN_vkCreatePipelineCache>(device->owned_gdpa(device->handle, "vkCreatePipelineCache"));
    destroy_pipeline_cache = reinterpret_cast<PFN_vkDestroyPipelineCache>(device->owned_gdpa(device->handle, "vkDestroyPipelineCache"));
    pipeline_cache_data = reinterpret_cast<PFN_vkGetPipelineCacheData>(device->owned_gdpa(device->handle, "vkGetPipelineCacheData"));
    if (!create || !destroy_pipeline_cache || !pipeline_cache_data) return;
    char name[64];
    snprintf(name, sizeof(name), "pipeline-cache-%04x-%04x.bin", device->physical_properties.vendorID, device->physical_properties.deviceID);
    const auto folder = g_cfg.m2_settings.empty() ? module_folder() / "settings" : absolute_folder(g_cfg.m2_settings);
    pipeline_cache_path = folder / name;
    std::vector<char> initial;
    {
        std::ifstream in(pipeline_cache_path, std::ios::binary);
        if (in) initial.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // The driver checks the header (its UUID among it) and ignores data that is not its own.
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initial.size(); info.pInitialData = initial.empty() ? nullptr : initial.data();
    if (create(device->handle, &info, nullptr, &pipeline_cache) != VK_SUCCESS && !initial.empty()) {
        info.initialDataSize = 0; info.pInitialData = nullptr;
        if (create(device->handle, &info, nullptr, &pipeline_cache) != VK_SUCCESS) pipeline_cache = VK_NULL_HANDLE;
        else initial.clear();
    }
    pipeline_cache_loaded = pipeline_cache ? initial.size() : 0;
    if (pipeline_cache) logf("variant pipeline cache: %llu bytes from %s", static_cast<unsigned long long>(pipeline_cache_loaded),
                             pipeline_cache_path.u8string().c_str());
}

// After every generation stopped (no variant is being built): the cache written beside the settings.
void CharacterBackend::close_pipeline_cache()
{
    if (!pipeline_cache) return;
    size_t size = 0;
    if (pipeline_cache_data(device->handle, pipeline_cache, &size, nullptr) == VK_SUCCESS && size) {
        std::vector<char> data(size);
        if (pipeline_cache_data(device->handle, pipeline_cache, &size, data.data()) == VK_SUCCESS) {
            std::error_code error;
            std::filesystem::create_directories(pipeline_cache_path.parent_path(), error);
            const auto temporary = pipeline_cache_path.wstring() + L".tmp";
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out.write(data.data(), static_cast<std::streamsize>(size)); }
            std::filesystem::rename(temporary, pipeline_cache_path, error);
            if (!error) pipeline_cache_saved = size;
            logf("variant pipeline cache: %llu bytes written to %s%s", static_cast<unsigned long long>(size),
                 pipeline_cache_path.u8string().c_str(), error ? " failed" : "");
        }
    }
    destroy_pipeline_cache(device->handle, pipeline_cache, nullptr);
    pipeline_cache = VK_NULL_HANDLE;
}

CharacterBackend::~CharacterBackend()
{
    if (twins_sweep.valid()) twins_sweep.get();
    for (auto& c : controllers) c->stop();
    if (folder_loading.valid()) { auto candidate = folder_loading.get(); if (candidate) candidate->stop(); }
    if (folder_staged) folder_staged->stop();
    if (folder) folder->stop();
    for (auto& f : folder_retiring) f.get();
    if (textures) textures->stop(); if (variants) variants->stop(); close_pipeline_cache();
}

void character_device_setup(Device* device, const VkDeviceCreateInfo* info, VkInstance instance, PFN_vkGetInstanceProcAddr gipa)
{
    device->material_queue_reason = "material queue discovery unavailable";
    if (!gipa) return;
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(device->owned_gdpa(device->handle, "vkGetDeviceQueue"));
    if (!families || !get_queue) return;
    uint32_t count = 0; families(device->phys, &count, nullptr);
    if (!count || count > 256) return;
    std::vector<VkQueueFamilyProperties> properties(count); families(device->phys, &count, properties.data());
    const VkDeviceQueueCreateInfo* chosen = nullptr;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        const auto& q = info->pQueueCreateInfos[i];
        if (q.queueFamilyIndex < properties.size() && (properties[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            logf("character graphics queue request: family=%u count=%u flags=%u", q.queueFamilyIndex, q.queueCount, q.flags);
            if (chosen || q.flags || !q.queueCount || q.queueCount > 32) {
                device->material_queue_reason = "materials require one unprotected graphics queue family, at most 32 queues";
                return;
            }
            chosen = &q;
        }
    }
    if (!chosen) { device->material_queue_reason = "no enabled graphics queue family"; return; }
    device->material_family = chosen->queueFamilyIndex;
    device->material_bc_enabled = info->pEnabledFeatures && info->pEnabledFeatures->textureCompressionBC;
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            device->material_bc_enabled = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(p)->features.textureCompressionBC != 0;
    // What owned GPU work may rely on: the core features the application enabled, and float
    // controls when core 1.2 or the extension is enabled on this device (D-072).
    if (info->pEnabledFeatures) device->enabled_features = *info->pEnabledFeatures;
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            device->enabled_features = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(p)->features;
    const uint32_t effective = std::min(device->app_api_version ? device->app_api_version : VK_API_VERSION_1_0, device->api_version);
    bool float_extension = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        float_extension |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (properties2 && (effective >= VK_API_VERSION_1_2 || float_extension)) {
        VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        query.pNext = &device->float_properties;
        properties2(device->phys, &query);
        device->float_controls = true;
    }
    if (properties2 && effective >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        query.pNext = &device->texel_alignment_properties;
        properties2(device->phys, &query);
        device->texel_alignment = true;
    }
    if (effective >= VK_API_VERSION_1_1)
        device->format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(gipa(instance, "vkGetPhysicalDeviceFormatProperties2"));
    device->material_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa(instance, "vkGetPhysicalDeviceFormatProperties"));
    device->material_image_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(gipa(instance, "vkGetPhysicalDeviceImageFormatProperties"));
    // The device-local memory still free, for activating character packages (D-092).
    if (effective >= VK_API_VERSION_1_1) {
        device->memory_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2"));
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
        uint32_t count = 0;
        if (enumerate && device->memory_properties2 && enumerate(device->phys, nullptr, &count, nullptr) == VK_SUCCESS && count) {
            std::vector<VkExtensionProperties> found(count);
            if (enumerate(device->phys, nullptr, &count, found.data()) == VK_SUCCESS)
                for (uint32_t i = 0; i < count; ++i)
                    device->memory_budget |= !strcmp(found[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }
    std::vector<VkQueue> queues;
    for (uint32_t i = 0; i < chosen->queueCount; ++i) {
        VkQueue queue = VK_NULL_HANDLE;
        get_queue(device->handle, device->material_family, i, &queue);
        if (!queue || (device->set_loader_data && device->set_loader_data(device->handle, queue) != VK_SUCCESS)) {
            device->material_queue_reason = "cannot initialize material queue dispatch"; return;
        }
        queues.push_back(queue);
    }
    device->material_queues = std::move(queues);
    device->material_queue_reason.clear();
}

std::shared_ptr<CharacterBackend> character_create(Device* d, const VkPhysicalDeviceProperties& p, const VkPhysicalDeviceMemoryProperties& m)
{
    auto backend = std::make_shared<CharacterBackend>();
    backend->device = d;
    backend->open_pipeline_cache();
    {
        VariantDispatch draw;
#define X(n) draw.n = reinterpret_cast<PFN_vk##n>(d->owned_gdpa(d->handle, "vk" #n));
        EFVK_VARIANT_FUNCS(X)
#undef X
        if (draw.CreateGraphicsPipelines && draw.CreateRenderPass && draw.CmdDrawIndexed)
            backend->variants = std::make_shared<VariantCache>(d->handle, draw, p.limits.maxBoundDescriptorSets, 4096, p.limits, backend->pipeline_cache,
                                                               VariantCache::builders_for_device());
        else logf("character variants unavailable: missing graphics entry");
    }
    backend->textures = std::make_unique<TextureResidency>(d, m);
    if (!backend->textures->usable()) logf("character textures unavailable: %s", backend->textures->reason().c_str());
    backend->translucency = std::make_unique<Translucency>(d, m);
    backend->bone_merge = std::make_unique<BoneMerge>(d, p, m);
    if (!backend->bone_merge->usable()) logf("character bones stay on the CPU: %s", backend->bone_merge->reason().c_str());
    std::error_code error;
    const bool directory = !g_cfg.m2_mods.empty() || std::filesystem::is_directory(std::filesystem::u8path(g_cfg.m2_package), error);
    backend->properties = p; backend->memory = m; backend->directory = directory;
    backend->controllers.reserve(kMaxPackages);
    if (!g_cfg.m2_mods.empty() && g_cfg.m2_folder_generation) {
        // One generation for the whole Mods folder (D-131): a controller per package carved from it.
        backend->folder = folder_create(d, p, m, backend->textures.get(), backend->variants, backend->translucency.get(), backend->bone_merge.get());
        for (auto& package : backend->folder->packages) {
            if (backend->controllers.size() >= kMaxPackages) { logf("character packages: no room for %s (4096 at most)", package->package_path.c_str()); break; }
            auto controller = std::make_unique<CharacterController>();
            controller->path = package->package_path; controller->lazy = true; controller->folder_member = true;
            controller->active = package;
            backend->controllers.push_back(std::move(controller));
        }
        if (backend->controllers.empty()) backend->reason = backend->folder->reason.empty() ? "no character Mod in the folder generation" : backend->folder->reason;
        backend->folder->announce();
        logf("character packages: %zu from the folder generation of %s", backend->controllers.size(), mods_folder().u8string().c_str());
        backend->rebuild_index();
        // The first frame's draws meet the generation's ShaderOverride sections (docs/vulkan-native-mods.md).
        shader_hooks_frame(*backend);
        // The first frame's draws see the gates of the generation loaded here (D-133).
        g_dither_off.store(std::any_of(backend->folder->gates.begin(), backend->folder->gates.end(), [](const auto& g) { return g.value != 0; }),
                           std::memory_order_relaxed);
        g_dither_gated.store(!backend->folder->gates.empty(), std::memory_order_relaxed);
        // A backend made while the game has its pipelines (a module exchange) prepares their twins here,
        // off the game's threads; one made with the device has none yet.
        prepare_twins(*backend);
        backend->twins_swept = g_dither_gated.load(std::memory_order_relaxed);
        return backend;
    }
    for (const auto& path : configured_packages(backend->reason)) {
        auto controller = std::make_unique<CharacterController>();
        controller->path = path; controller->lazy = directory;
        controller->active = generation_create(d, p, m, path, directory, backend->textures.get(), backend->variants, backend->translucency.get(),
                                               backend->bone_merge.get());
        backend->controllers.push_back(std::move(controller));
    }
    if (directory) logf("character packages: %zu from %s", backend->controllers.size(),
                        g_cfg.m2_mods.empty() ? g_cfg.m2_package.c_str() : g_cfg.m2_mods.c_str());
    backend->rebuild_index();
    return backend;
}
void character_stop(CharacterBackend& b)
{
    if (b.twins_sweep.valid()) b.twins_sweep.get();
    if (b.device) b.device->shader_draws.store(false, std::memory_order_relaxed);
    { std::unique_lock<std::shared_mutex> lock(b.index_mx); b.shader_hooks.reset(); }
    for (auto& c : b.controllers) c->stop();
    if (b.folder_loading.valid()) { auto candidate = b.folder_loading.get(); if (candidate) candidate->stop(); }
    if (b.folder_staged) { b.folder_staged->stop(); b.folder_staged.reset(); }
    if (b.folder) b.folder->stop();
    for (auto& f : b.folder_retiring) f.get();
    b.folder_retiring.clear();
    if (b.textures) b.textures->stop(); if (b.variants) b.variants->stop(); b.close_pipeline_cache();
}

void character_retire(CharacterBackend& b, uint32_t id)
{
    if (b.variants) {
        b.variants->retire(id);
        for (uint32_t k = 0; k < kCaptureKinds; ++k) b.variants->retire(capture_identity(id, CaptureKind(k)));
        b.variants->retire(twin_identity(id));
    }
    {
        std::unique_lock<std::shared_mutex> lock(b.twins_mx);
        for (auto it = b.twins.begin(); it != b.twins.end();)
            if (std::get<1>(it->first) == id) it = b.twins.erase(it); else ++it;
        for (auto it = b.twins_failed.begin(); it != b.twins_failed.end();)
            if (std::get<1>(*it) == id) it = b.twins_failed.erase(it); else ++it;
    }
    { std::unique_lock<std::shared_mutex> lock(g_dither_mx); g_dither_pipelines.erase(id); }
    if (b.translucency) b.translucency->retire(id);
    { std::lock_guard<std::mutex> lock(b.copy_mx); b.copy_facts.erase(id); }
    for (auto& c : b.controllers) {
        std::shared_lock<std::shared_mutex> lock(c->mutex);
        if (c->active) generation_retire(*c->active, id);
        if (c->staged) generation_retire(*c->staged, id);
    }
}
// After the application ended a classic render pass (D-113). The library's depth-copy hook: every
// draw of the game's copy shader in the pass is told to the generations that loaded EFMITTL (its pre
// list: `$copy_after_lit` when the lit pass was seen). Then the pass's captures and composite.
void character_pass_end(CharacterBackend& b, CmdBuf* cb)
{
    if (!b.enabled.load(std::memory_order_relaxed) || !b.translucency) return;
    b.translucency->pass_end(cb);
    if (!b.translucency->listening()) return;
    // The game's copy shader among the pass's draws without indices (its full-screen passes).
    bool copied = false;
    for (size_t i = cb->pass.first_draw; i < cb->draws.size() && !copied; ++i) {
        const DrawRec& r = cb->draws[i];
        if (r.kind != DK_DRAW || !r.pipeline) continue;
        uint32_t id = 0; bool known = false;
        std::vector<uint32_t> fragment;
        {
            std::shared_lock<std::shared_mutex> tracker(b.device->res_mx);
            const auto pipe = b.device->pipes.find(r.pipeline);
            if (pipe == b.device->pipes.end() || !pipe->second.recipe) continue;
            id = pipe->second.id;
            std::lock_guard<std::mutex> lock(b.copy_mx);
            const auto found = b.copy_facts.find(id);
            if (found != b.copy_facts.end()) { known = true; copied = found->second; }
            else for (const auto& stage : pipe->second.recipe->stages)
                if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) fragment = stage.shader->code;
        }
        if (!known) {
            copied = copy_fragment(fragment);
            std::lock_guard<std::mutex> lock(b.copy_mx);
            if (b.copy_facts.size() >= 8192) b.copy_facts.clear();
            b.copy_facts[id] = copied;
        }
    }
    if (!copied) return;
    // The hook's pre list sets one flag; once per pass is as many times as it matters.
    for (auto& c : b.controllers) {
        std::shared_ptr<CharacterGeneration> g;
        {
            std::shared_lock<std::shared_mutex> lock(c->mutex);
            if (c->stopped || !c->active || !c->active->program || !c->active->program->efmittl() ||
                !c->active->activated.load(std::memory_order_relaxed)) continue;
            g = c->active;
        }
        std::lock_guard<std::mutex> lock(g->mod_mx);
        if (g->program) g->program->library_hook(efmi::frontend::LibraryHook::CopyShader);
    }
}

VkResult character_submit(CharacterBackend& b, VkQueue q, SubmitWork& work)
{
    // The textures' uploads and publication on this queue, before any package's work (D-094).
    ThreadTrace* const trace = b.device && b.device->trace ? b.device->trace() : nullptr;
    if (b.textures) { StageMark stage(trace, "character submission: texture uploads"); const auto result = b.textures->before_submit(q); if (result != VK_SUCCESS) return result; }
    // The merged skeletons the submitted recordings read (D-127): per application batch, the imports its
    // recordings carry and their snapshots' copies, in a command buffer of ours the host puts at the front
    // of that batch, so the game's own waits order them on whichever queue the game chose.
    if (b.bone_merge && b.device && !work.batches.empty() &&
        std::find(b.device->material_queues.begin(), b.device->material_queues.end(), q) != b.device->material_queues.end()) {
        StageMark stage(trace, "character submission: bone imports");
        std::vector<const void*> keys;
        for (auto& c : b.controllers) {
            std::shared_lock<std::shared_mutex> lock(c->mutex);
            if (c->active) keys.push_back(c->active->generation_key.get());
            if (c->staged) keys.push_back(c->staged->generation_key.get());
        }
        thread_local std::vector<BoneBatch> batches;
        thread_local std::vector<CharacterCommand*> mine;
        batches.resize(work.batches.size());
        for (size_t i = 0; i < work.batches.size(); ++i) {
            BoneBatch& batch = batches[i]; batch.clear();
            mine.clear();
            for (CmdBuf* cb : work.batches[i].cbs)
                for (auto& owner : cb->owners)
                    if (owner && std::find(keys.begin(), keys.end(), owner->backend) != keys.end())
                        mine.push_back(static_cast<CharacterCommand*>(owner.get()));
            // The batch's own imports first; then an import taken from another recording, only while that
            // recording is still not submitted (an earlier batch of this call submits it first) and no
            // recording of this batch imports the component itself.
            auto has = [&](const BoneRing* ring, uint32_t component) {
                for (const auto& job : batch.jobs) if (job.ring.get() == ring && job.component == component) return true;
                return false;
            };
            for (auto* owner : mine)
                for (const auto& job : owner->bone_jobs) if (job.recording == owner->recording) batch.jobs.push_back(job);
            for (auto* owner : mine)
                for (const auto& job : owner->bone_jobs)
                    if (job.recording != owner->recording && job.recording && job.recording->pending(job.epoch) && !has(job.ring.get(), job.component))
                        batch.jobs.push_back(job);
            for (auto* owner : mine)
                for (const auto& held : owner->rings) batch.copies.push_back({held.first, held.second});
            for (auto* owner : mine) owner->recording->submitted();
        }
        VkSemaphore timeline = VK_NULL_HANDLE; uint64_t value = 0;
        if (b.bone_merge->prepare(q, batches, timeline, value)) {
            for (size_t i = 0; i < batches.size(); ++i) if (batches[i].cb) work.batches[i].prefix.push_back(batches[i].cb);
            work.signals.push_back({timeline, value});
        }
        for (auto& batch : batches) batch.clear(); // the rings and recordings are not kept past the call
    }
    for (auto& c : b.controllers) {
        std::shared_lock<std::shared_mutex> lock(c->mutex); if (c->stopped) continue;
        auto result = c->active ? generation_submit(*c->active, q) : VK_SUCCESS;
        if (result == VK_SUCCESS && c->staged) result = generation_submit(*c->staged, q);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
void character_submitted(CharacterBackend& b, VkQueue q, SubmitWork& work, VkResult result)
{
    if (!b.bone_merge) return;
    for (const auto& signal : work.signals) b.bone_merge->finish(q, signal.semaphore, signal.value, result == VK_SUCCESS);
}
// Each package runs its Present work in turn; a presenter waits for what the one before handed on.
void character_frame(CharacterBackend& b, bool had_draws, PresentWork* present)
{
    // The presentation's work of every package (or of the folder generation, D-131), timed for the status.
    struct Clock {
        CharacterBackend& b; uint64_t began;
        ~Clock() { b.frame_us += host_time_us() - began; ++b.frame_calls; }
    } clock{b, host_time_us()};
    if (had_draws) b.frames.fetch_add(1, std::memory_order_relaxed);
    if (b.textures) b.textures->frame();
    if (b.translucency) b.translucency->frame();
    // The keys, the window and the cursor, once for every package (they cost per package before).
    auto input = sample_ini_input();
    OwnedPresenter::sample(b.device, present, input);
    bool rescan = false;
    for (auto& c : b.controllers) {
        c->frame(had_draws, present, input);
        if (c->reload_taken) { c->reload_taken = false; rescan = true; }
    }
    if (b.folder) folder_frame_and_exchange(b, had_draws, present, input);
    else if (rescan && b.directory) character_rescan(b);
    // Route 2: the ShaderOverride hashes of the generation in place, the hunting keys of this presentation.
    shader_hooks_frame(b);
    if (b.index_stale()) b.rebuild_index();
}
bool character_usable(CharacterBackend& b, std::string& reason)
{
    reason = b.reason;
    bool any = false;
    for (auto& c : b.controllers) {
        std::shared_lock<std::shared_mutex> lock(c->mutex);
        std::string why;
        if (c->active && generation_usable(*c->active, why)) any = true;
        else if (reason.empty()) reason = why;
    }
    if (any) reason.clear();
    return any;
}
// Every usable package is ready; a lazy one counts as ready, it prepares on its first match.
bool character_ready(CharacterBackend& b)
{
    bool any = false;
    for (auto& c : b.controllers) {
        std::shared_lock<std::shared_mutex> lock(c->mutex);
        std::string why;
        if (!c->active || !generation_usable(*c->active, why)) continue;
        any = true;
        if (!c->lazy && !generation_ready(*c->active)) return false;
    }
    return any;
}
void character_enable(CharacterBackend& b, bool enabled)
{
    b.enabled.store(enabled, std::memory_order_relaxed);
    for (auto& c : b.controllers) { std::shared_lock<std::shared_mutex> lock(c->mutex); if (c->active) generation_enable(*c->active, enabled); }
}

const CharacterModule* builtin_character_module()
{
    static const CharacterModule module = [] {
        CharacterModule m;
        m.size = sizeof(CharacterModule); m.abi = EFVK_ABI; m.version = EFVK_VERSION;
        m.create = character_create; m.usable = character_usable; m.ready = character_ready; m.stop = character_stop;
        m.record = character_record; m.retire = character_retire; m.frame = character_frame; m.status = character_status;
        m.enable = character_enable; m.submit = character_submit; m.submitted = character_submitted; m.pass_end = character_pass_end;
        m.bind = character_bind;
        m.created = character_created;
        m.shader_draw = character_shader_draw;
        return m;
    }();
    return &module;
}

} // namespace efvk

#if defined(EFVK_CHARACTER_CACHE_TESTS)
#include "../../sandbox/character_cache_cases.h"
#endif
