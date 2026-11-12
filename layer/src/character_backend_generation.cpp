// A package's generation (CharacterGeneration): its load from the Mod's INI or as a member of the folder
// generation, its activation on the device, the worker that brings up its mesh and snapshot batches, its
// notes, its stop, and the Present of a generation of its own.
#include "character_backend_internal.h"

namespace efvk {
namespace {
// Bone copies per arena: kArenaSlices, fewer for a large skeleton so that a batch (one arena per image
// count) keeps its 64 MiB budget. A 1299-bone Mod needs 146 KiB per copy; with 64 copies in each of
// nine arenas every batch was refused and the package never drew (BUG-049).
uint32_t arena_slices(uint32_t bone_bytes, size_t arenas)
{
    const uint64_t per_arena = (64ull << 20) / std::max<size_t>(arenas, 1);
    const uint64_t fixed = uint64_t(kArenaEntries) * 256 + 65536 + 4096; // entries, uniform range, alignment
    const uint64_t stride = (uint64_t(bone_bytes) + 255) & ~uint64_t(255);
    if (!stride || per_arena <= fixed) return kArenaMinSlices;
    return static_cast<uint32_t>(std::clamp<uint64_t>((per_arena - fixed) / stride, kArenaMinSlices, kArenaSlices));
}

// A test's hook, in the test programs' own processes only: while the variable is set, a package's
// activation fails (the scene sets and clears it around the activation it means).
bool test_activation_fails()
{
    static const bool test_process = [] {
        wchar_t executable[32768] = {};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto name = std::filesystem::path(executable).filename();
        return !_wcsicmp(name.c_str(), L"efmi_vk_variant_gpu.exe") || !_wcsicmp(name.c_str(), L"efmi_vk_sandbox.exe");
    }();
    wchar_t value[4];
    return test_process && GetEnvironmentVariableW(L"EFMI_VK_TEST_ACTIVATION_FAILS", value, 4) != 0;
}
} // namespace

namespace character_internal {
float ini_seconds()
{
    static const uint64_t origin = host_time_us();
    return float((host_time_us() - origin) * 1.0e-6);
}

// Meshes put on the device from their files, and those a successor generation kept from the live one.
std::atomic<uint64_t> g_mesh_uploads{0}, g_meshes_kept{0};
} // namespace character_internal

void CharacterGeneration::worker_ended()
{
    std::lock_guard<std::mutex> lock(pool_mx);
    if (stopped) return;
    ready = false;
    worker_failures = std::min<uint32_t>(worker_failures + 1, 6);
    retry_us.store(host_time_us() + (500000ull << worker_failures), std::memory_order_relaxed);
    worker_failed.store(true, std::memory_order_release); // the worker's last act
}

// From a matched draw while the worker is down: at its time the worker again, with the mesh it had.
void CharacterGeneration::retry_activation()
{
    if (host_time_us() < retry_us.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(pool_mx);
    if (stopped || !worker_failed.load(std::memory_order_acquire)) return;
    if (worker.joinable()) worker.join(); // it has returned: worker_ended was its last act
    if (mesh) kept_mesh = mesh;
    worker_failed.store(false, std::memory_order_release);
    logf("character package activation tried again: %s", package_path.c_str());
    worker = std::thread([this] { prepare(); });
}

void CharacterGeneration::note(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(pool_mx); last_reason = reason;
    if (logged_reasons.size() >= 16 || std::find(logged_reasons.begin(), logged_reasons.end(), reason) != logged_reasons.end()) return;
    logged_reasons.push_back(reason);
    logf("character package %s: %s", package_path.c_str(), reason.c_str());
    // What the layer cannot do as the original does is shown, not only logged (D-137): once per Mod and
    // reason for the process, the first three of a generation.
    if (logged_reasons.size() <= 3) {
        const std::string mod = std::filesystem::u8path(package_path).parent_path().filename().u8string();
        const std::string text = "EFMI Vulkan: " + mod + ": " + reason + " (that part keeps the game's look)";
        notice(NoticeLevel::Warning, (package_path + "\n" + reason).c_str(), text.c_str());
    }
}

// What the original does for the Mod's sections and the layer does not (CharacterProgram::gaps), shown
// at load as the refusals above are (D-137): every one in the log, the first three as notices.
void CharacterGeneration::show_gaps()
{
    if (!program) return;
    const std::string mod = std::filesystem::u8path(package_path).parent_path().filename().u8string();
    size_t shown = 0;
    for (const auto& gap : program->gaps()) {
        logf("character package %s: %s", package_path.c_str(), gap.c_str());
        if (shown++ < 3) notice(NoticeLevel::Warning, (package_path + "\n" + gap).c_str(), ("EFMI Vulkan: " + mod + ": " + gap).c_str());
    }
}

bool CharacterGeneration::init()
{
    const bool discovered = is_mod_ini(package_path);
    if (!discovered && !package.load(package_path, last_reason)) return false;
    if (discovered || CharacterProgram::configured(package_path)) {
        program = std::make_unique<CharacterProgram>();
        const auto gpu = [this](const std::filesystem::path& working, std::string& why) {
            return OwnedPresenter::runtime(device, memory, working, why, variants ? variants->pipeline_cache() : VK_NULL_HANDLE);
        };
        if (discovered) {
            // The Mod's INI itself: the package is made from what the generation declares (D-078).
            CharacterProgram::Source source;
            try { source = mod_source(package_path); } catch (const std::exception& e) { last_reason = e.what(); return false; }
            if (!program->discover(source, package, last_reason, gpu)) return false;
            for (const auto& note : program->library_notes()) logf("%s", note.c_str());
            // The measured dimension policy, on the device it was measured on only.
            if (g_cfg.m2_dimension_zero_vendor && g_cfg.m2_dimension_zero_vendor == properties.vendorID &&
                g_cfg.m2_dimension_zero_device == properties.deviceID) {
                package.material_vendor = properties.vendorID; package.material_device = properties.deviceID;
            }
            logf("character Mod read from its INI: %s: %zu mesh files, %zu images, %zu sections, %zu mesh sections left to the game",
                 package_path.c_str(), package.streams.size(), package.images.size(), package.sections.size(), program->left_to_game());
        } else if (!program->init(package_path, package, last_reason, gpu)) return false;
        show_gaps();
        if (program->gpu()) presenter = std::make_shared<OwnedPresenter>(device, program->gpu());
        else if (!program->gpu_reason().empty()) logf("owned GPU work unavailable: %s", program->gpu_reason().c_str());
        {
            // What the original overlay would show after a load; the generation carries on (D-067).
            const auto loaded = program->status();
            if (loaded.diagnostics) logf("INI loaded with %zu original warning(s)", loaded.diagnostics);
            for (const auto& text : loaded.diagnostic_messages) logf("INI warning: %s", text.c_str());
        }
    }
    return init_device();
}

// A character Mod of a folder generation (D-131): its program (a view of the shared generation),
// its package and the folder's presenter and lock, made by the folder; the device side as above.
bool CharacterGeneration::init_adopted(std::unique_ptr<CharacterProgram> adopted, CharacterPackage made, std::shared_ptr<OwnedPresenter> shared,
                                       std::shared_ptr<std::mutex> lock)
{
    program = std::move(adopted); package = std::move(made); presenter = std::move(shared); plan_mx = std::move(lock);
    for (const auto& note : program->library_notes()) logf("%s", note.c_str());
    show_gaps();
    if (g_cfg.m2_dimension_zero_vendor && g_cfg.m2_dimension_zero_vendor == properties.vendorID &&
        g_cfg.m2_dimension_zero_device == properties.deviceID) {
        package.material_vendor = properties.vendorID; package.material_device = properties.deviceID;
    }
    logf("character Mod of the folder generation: %s: %zu mesh files, %zu images, %zu sections, %zu mesh sections left to the game",
         package_path.c_str(), package.streams.size(), package.images.size(), package.sections.size(), program->left_to_game());
    return init_device();
}

bool CharacterGeneration::init_device()
{
#define X(n) draw.n = reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle, "vk" #n));
    EFVK_VARIANT_FUNCS(X)
#undef X
#define X(n) storage.n = reinterpret_cast<PFN_vk##n>(device->owned_gdpa(device->handle, "vk" #n)); \
    if (!storage.n) { last_reason = "missing vk" #n; return false; }
    EFVK_SNAPSHOT_FUNCS(X)
#undef X
    if (!draw.CreateGraphicsPipelines || !draw.CreateRenderPass || !draw.CmdDrawIndexed) { last_reason = "missing graphics entry"; return false; }
    for (const auto& c : package.components) ranges.push_back({c.first, c.count});
    bone_bytes = (MergedSkeleton::kImplicitBones + 2 * package.bones) * static_cast<uint32_t>(MergedSkeleton::kBytesPerBone);
    if (package.materials && !init_materials()) return false;
    if (program || package.materials) {
        // Rules a fragment module gives (D-076) can use any count up to the slots one draw binds; a
        // live INI chooses them at run time, so a program gets every count of the budget.
        // Allocate these arenas before first drawing.
        uint32_t maximum = program ? 8 : 0;
        for (const auto& pixels : package.pixels) maximum = std::max(maximum, static_cast<uint32_t>(std::min<size_t>(pixels.size(), 64)));
        for (const auto& profile : package.profiles) {
            uint32_t count = 0;
            for (const auto& rule : profile.rules) count += rule.kind == CharacterMaterialRule::Image;
            maximum = std::max(maximum, count);
        }
        if (maximum > 8) { last_reason = "runtime material slots exceed the image arena budget"; return false; }
        for (uint32_t i = 0; i <= maximum; ++i) image_counts.insert(i);
    }
    // One variant per pipeline and mesh draw. A character is drawn by some hundred pipelines once
    // the feature variants of its shaders are counted; a full cache would refuse the rest for good.
    cache = variants; // the device's variant cache (D-103); null refuses the generation
    if (!cache) { last_reason = "variant cache unavailable"; return false; }
    enabled = g_cfg.m2_enabled;
    if (!lazy) {
        // A successor of a package that is not lazy activates here, on its loading thread.
        if (predecessor) keep_mesh(*predecessor);
        activate();
    }
    predecessor = nullptr;
    return true;
}

// Before a successor of `live` activates (F10): the original releases its buffers on a reload and
// reads their files again at first use; the layer keeps the live mesh when every stream is the
// same bytes at the same place, as it keeps images by their files (BUG-061). `live`'s package is
// read under its mod_mx, which its plans hold while they adopt bone streams.
void CharacterGeneration::keep_mesh(CharacterGeneration& live)
{
    std::shared_ptr<const GpuMesh> held;
    {
        std::lock_guard<std::mutex> plans(live.mod_mx);
        if (!package.same_payload(live.package)) return;
        std::lock_guard<std::mutex> lock(live.pool_mx); held = live.mesh;
    }
    if (!held) return;
    std::lock_guard<std::mutex> lock(pool_mx);
    kept_mesh = std::move(held);
    activation_bytes = 32ull << 20; // the snapshot batches only
}

// The largest device-local heap's budget minus its use (VK_EXT_memory_budget); false without it.
bool CharacterGeneration::device_local_free(Device* device, uint64_t& free_bytes, uint64_t& heap_bytes)
{
    if (!device->memory_budget || !device->memory_properties2) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget;
    device->memory_properties2(device->phys, &properties);
    bool found = false;
    for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i) {
        const auto& heap = properties.memoryProperties.memoryHeaps[i];
        if (!(heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || (found && heap.size <= heap_bytes)) continue;
        found = true; heap_bytes = heap.size;
        free_bytes = budget.heapBudget[i] > budget.heapUsage[i] ? budget.heapBudget[i] - budget.heapUsage[i] : 0;
    }
    return found;
}

// A lazy package activates only while the device keeps a reserve beyond what it takes: the game's
// own memory comes first, and a character that does not fit stays the game's (D-092). Checked
// about once a second while it waits; logged once every 30 seconds.
bool CharacterGeneration::memory_allows_activation()
{
    const uint64_t now = frame.load(std::memory_order_relaxed);
    if (now < next_memory_check) return false;
    next_memory_check = now + 60;
    // With pageable device memory the system pages what is used least, ours first by priority, as
    // WDDM does for 3DMigoto, whose resources are created at first use whatever the free memory:
    // a character waiting here stayed the game's mesh, or appeared late (D-122).
    if (device->pageable_memory) return true;
    uint64_t free_bytes = 0, heap_bytes = 0;
    if (!device_local_free(device, free_bytes, heap_bytes)) return true;
    const uint64_t reserve = g_cfg.m2_reserve_mb ? g_cfg.m2_reserve_mb << 20 : std::max<uint64_t>(512ull << 20, heap_bytes / 16);
    const uint64_t activating = textures ? textures->activating_bytes.load() : 0;
    if (free_bytes >= activation_bytes + reserve + activating) return true;
    const uint64_t clock = host_time_us();
    if (!memory_logged_us || clock - memory_logged_us > 30000000) {
        memory_logged_us = clock;
        logf("character package waits for device memory: %s needs %llu MiB and keeps %llu MiB in reserve, %llu MiB free",
             package_path.c_str(), static_cast<unsigned long long>(activation_bytes >> 20),
             static_cast<unsigned long long>(reserve >> 20), static_cast<unsigned long long>(free_bytes >> 20));
    }
    return false;
}

void CharacterGeneration::activate()
{
    if (activated.exchange(true)) return;
    if (textures) {
        textures->activating_bytes += activation_bytes;
        // Textures come on their first bind (D-094); a test may ask for all of them at once.
        if (g_cfg.m2_texture_preload) textures->preload(image_ids);
    }
    if (lazy) logf("character package activated on its first match: %s", package_path.c_str());
    worker = std::thread([this] { prepare(); });
}

// After a refused init: nothing of the generation runs. The worker was not started yet.
void CharacterGeneration::abandon()
{
    enabled = false;
    presenter.reset();
    program.reset();
}

void CharacterGeneration::stop_now()
{
    { std::lock_guard<std::mutex> lock(pool_mx); stopped = true; enabled = false; wake.notify_all(); }
    if (worker.joinable()) worker.join();
    if (presenter) presenter->stop();
    if (program && !folder_member()) { std::string reason; if (!program->save(reason)) note("INI shutdown settings: " + reason); }
    // Once: an image goes when no generation declares it any more (BUG-061).
    if (textures && !images_forgotten) { textures->forget(image_ids); images_forgotten = true; }
    std::deque<std::shared_ptr<CharacterCommand>> old;
    std::shared_ptr<const GpuMesh> old_mesh;
    { std::lock_guard<std::mutex> lock(pool_mx); old.swap(available); old_mesh.swap(mesh); ready = false; }
}

void CharacterGeneration::prepare()
{
    // The activation gate counts this package until its mesh is on the device (D-092).
    struct Activating {
        TextureResidency* textures; uint64_t bytes;
        ~Activating() { if (textures) textures->activating_bytes.fetch_sub(std::min(textures->activating_bytes.load(), bytes)); }
    } activating{textures, activation_bytes};
    try {
        // A test's hook (the scene sets and clears it in its own process): this activation fails.
        if (test_activation_fails()) throw std::runtime_error("test: the activation was made to fail");
        std::shared_ptr<const GpuMesh> kept;
        { std::lock_guard<std::mutex> lock(pool_mx); kept = std::move(kept_mesh); }
        if (kept) {
            ++g_meshes_kept;
            logf("character mesh kept from the generation before: %llu bytes, no mesh file changed",
                 static_cast<unsigned long long>(package.payload_bytes));
            std::lock_guard<std::mutex> lock(pool_mx); mesh = std::move(kept);
        } else {
            auto created = std::make_shared<GpuMesh>(device->handle, storage);
            std::string reason;
            const uint64_t began = host_time_us();
            if (!created->create(package, memory, device, reason)) { note(reason); worker_ended(); return; }
            mesh_upload_us = host_time_us() - began; ++g_mesh_uploads;
            logf("character mesh uploaded: %llu bytes in %s memory (%s) from %s in %llu us", static_cast<unsigned long long>(package.payload_bytes),
                 created->device_local ? "device" : "host", mesh_memory_name(created->placement),
                 package.sources.empty() ? "the package" : "the files", static_cast<unsigned long long>(mesh_upload_us.load()));
            std::lock_guard<std::mutex> lock(pool_mx); mesh = created;
        }
        activating.~Activating(); activating.textures = nullptr;
        while (true) {
            if (program && !folder_member() && save_requested.exchange(false)) {
                std::string error;
                if (!program->save(error)) note("INI settings: " + error);
            }
            {
                std::unique_lock<std::mutex> lock(pool_mx);
                all.erase(std::remove_if(all.begin(), all.end(), [](const auto& p) { return p.expired(); }), all.end());
                if (stopped) return;
                if (available.size() >= 2 || all.size() >= kBatchesAhead) { wake.wait_for(lock, std::chrono::milliseconds(100)); continue; }
            }
            std::string why;
            auto batch = make_batch(why);
            if (!batch) { note(why); worker_ended(); return; }
            {
                std::lock_guard<std::mutex> lock(pool_mx);
                if (stopped) return;
                all.push_back(batch); available.push_back(std::move(batch)); ready = true;
                worker_failures = 0;
            }
        }
    } catch (const std::exception& e) { note(e.what()); worker_ended(); }
}

// A batch of snapshot arenas for one recording: one arena per image count of the package.
std::shared_ptr<CharacterCommand> CharacterGeneration::make_batch(std::string& why)
{
    auto batch = std::make_shared<CharacterCommand>();
    ++batches_made;
    batch->generation = generation_key; batch->backend = generation_key.get(); batch->batches = batches;
    { std::lock_guard<std::mutex> lock(pool_mx); batch->keep = mesh; }
    uint64_t total_bytes = 0;
    const uint32_t slices = arena_slices(bone_bytes, image_counts.size());
    for (uint32_t count : image_counts) {
        auto snapshots = std::make_shared<DrawSnapshots>(device->handle, storage);
        if (!snapshots->create_shared(memory, properties.limits, kArenaEntries, slices, bone_bytes, 65536, count)) { why = snapshots->reason; return {}; }
        total_bytes += snapshots->size();
        if (total_bytes > (64ull << 20)) { why = "character material arenas exceed the batch budget"; return {}; }
        if (count) batch->material_snapshots.emplace(count, std::move(snapshots));
        else batch->snapshots = std::move(snapshots);
    }
    return batch;
}

// The recording's batch: a prepared one, or one made here when the frames in flight used them up
// (as 3DMigoto creates a resource where it is first needed): the draw is the Mod's either way,
// never the game's mesh for want of a batch. Up to kBatchesMax per package.
std::shared_ptr<CharacterCommand> CharacterGeneration::command()
{
    {
        std::lock_guard<std::mutex> lock(pool_mx);
        if (stopped || !mesh) return {};
        if (!available.empty()) {
            auto cmd = available.front(); available.pop_front();
            wake.notify_one();
            return cmd;
        }
        all.erase(std::remove_if(all.begin(), all.end(), [](const auto& p) { return p.expired(); }), all.end());
        if (all.size() >= kBatchesMax) return {};
    }
    std::string why;
    auto batch = make_batch(why);
    if (!batch) { note(why); return {}; }
    ++counters[C_BATCH_MADE];
    std::lock_guard<std::mutex> lock(pool_mx);
    if (stopped) return {};
    all.push_back(batch); wake.notify_one();
    return batch;
}

std::shared_ptr<CharacterGeneration> generation_create(Device* device, const VkPhysicalDeviceProperties& properties,
                                                   const VkPhysicalDeviceMemoryProperties& memory,
                                                   const std::string& path, bool lazy, TextureResidency* textures,
                                                   std::shared_ptr<VariantCache> variants, Translucency* translucency, BoneMerge* bones,
                                                   CharacterGeneration* predecessor)
{
    auto backend = std::make_shared<CharacterGeneration>(device, properties, memory, path, lazy, textures, std::move(variants), translucency, bones);
    backend->predecessor = predecessor;
    try {
        if (!backend->init()) {
            // BUG-026: a refusal can come after the INI program or its presenter were made; a refused
            // backend keeps neither, so no later presentation or draw reaches a half-made program.
            backend->abandon();
            logf("character package refused: %s", backend->last_reason.c_str()); return backend;
        }
        logf("character package configured: %u sections, %u components, %u bones, %u draws, enabled=%u",
             static_cast<uint32_t>(backend->package.sections.size()), static_cast<uint32_t>(backend->package.components.size()),
             backend->package.bones, static_cast<uint32_t>(backend->package.draws.size()), backend->enabled.load() ? 1 : 0);
        if (backend->program && backend->program->outside_meshes())
            logf("character package: %zu referenced mesh file(s) outside the package stay with the game's draws",
                 backend->program->outside_meshes());
    } catch (const std::exception& e) {
        backend->enabled = false; backend->note(e.what());
        logf("character package exception: %s", e.what());
    }
    return backend;
}

void generation_stop(CharacterGeneration& backend) { backend.stop(); }

// The INI generation's Present for one presentation: its lists in the owned scope ordered before the
// presentation, or a replay of the last one for a presentation without a draw (BUG-036). The caller
// holds the lock the generation's plans take. True when the lists ran for a rendered frame.
bool ini_present(CharacterProgram& program, OwnedPresenter* presenter, uint64_t frame, float frame_seconds, bool had_draws,
                 PresentWork* present, const efmi::frontend::RuntimeInput& base, uint64_t now, const std::function<void(const std::string&)>& note)
{
    // The keys, window and cursor are sampled once per presentation for every package.
    auto input = base;
    input.frame_seconds = frame_seconds;
    input.ticks_us = now;
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
    auto present_began = std::chrono::steady_clock::now();
    auto present_phase = [&](const char* name) {
        const auto at = std::chrono::steady_clock::now();
        program.note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(at - present_began).count())); present_began = at;
    };
#else
    auto present_phase = [](const char*) {};
#endif
    std::string reason, owned_reason;
    if (!had_draws) {
        // The first Present that draws into the back buffer kept nothing to replay: its frame's
        // presentations go without (one frame, a panel opening); every later one has it.
        if (!presenter || !presenter->last_drew_backbuffer() || !program.replayable()) return false;
        const bool owned = presenter->begin(present, owned_reason);
        if (!owned_reason.empty()) note(owned_reason);
        if (!owned) return false;
        const bool executed = program.replay(input, frame, ini_seconds(), reason);
        if (!executed) note("INI Present replay: " + reason);
        std::string submission;
        presenter->finish(present, executed, submission, true);
        if (!submission.empty()) note(submission);
        return false;
    }
    // With a presentation the generation's GPU commands run in a scope ordered before it.
    const bool owned = presenter && presenter->begin(present, owned_reason);
    present_phase("present owned begin");
    if (!owned_reason.empty()) note(owned_reason);
    // What a replay needs is kept only where the last Present drew into the back buffer (D-108).
    const bool keep_replay = presenter && presenter->last_drew_backbuffer();
    const bool executed = program.present(input, frame, ini_seconds(), reason, keep_replay);
    present_phase("present ini (whole)");
    if (!executed) note("INI Present: " + reason);
    // The first refusal of owned Present work ends the generation's GPU side; keep its reason.
    static std::atomic<bool> logged{false};
    if (owned && !executed && !logged.exchange(true)) logf("INI Present refused with owned GPU work: %s", reason.c_str());
    if (owned) {
        std::string submission;
        presenter->finish(present, executed, submission);
        if (!submission.empty()) note(submission);
    }
    present_phase("present owned finish");
    return true;
}
void generation_frame(CharacterGeneration& backend, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& base)
{
    // The Present lists run once per rendered frame, as the original's run once per frame. A
    // presentation without a draw in its interval presents the same frame again (frame generation,
    // a frame presented again): what the Mod drew into the back buffer at the last presentation
    // (a key panel) is drawn into this image again by a replay of that Present, the INI's
    // variables untouched (BUG-036). The character's frame clock, which identities, the hashing
    // policy and the frame gate count in, advances with the rendered frames only.
    const uint64_t now = host_time_us();
    if (had_draws) {
        // The INI's frame time is the rendered frames' interval.
        if (backend.last_frame_us && now > backend.last_frame_us) {
            const float seconds = static_cast<float>(now - backend.last_frame_us) * 1.0e-6f;
            backend.frame_seconds.store(std::min(std::max(seconds, 0.001f), 0.25f), std::memory_order_relaxed);
        }
        backend.last_frame_us = now;
        backend.frame.fetch_add(1, std::memory_order_relaxed);
    }
    backend.presents.fetch_add(1, std::memory_order_relaxed);
    // A folder generation's packages leave the shared generation's Present to the folder (D-131).
    if (backend.program && !backend.folder_member()) {
        ThreadTrace* const trace = backend.device->trace ? backend.device->trace() : nullptr;
        StageMark locking(trace, "character Present (generation lock)");
        std::lock_guard<std::mutex> lock(backend.mod_mx);
        if (ini_present(*backend.program, backend.presenter.get(), backend.frame.load(), backend.frame_seconds.load(std::memory_order_relaxed),
                        had_draws, present, base, now, [&backend](const std::string& text) { backend.note(text); })) {
            // As the original's Present: every settings_auto_save_interval seconds the persistent variables
            // are saved if they changed (BUG-058: a panel animating persist variables had them saved every
            // frame). Reloads and the shutdown save at once.
            if (now - backend.save_checked_us >= g_cfg.settings_save_seconds * 1000000) {
                backend.save_checked_us = now;
                if (backend.program->settings_dirty()) { backend.save_requested = true; backend.wake.notify_one(); }
            }
        }
    }
}

bool generation_usable(CharacterGeneration& backend, std::string& reason)
{
    std::lock_guard<std::mutex> lock(backend.pool_mx);
    reason = backend.last_reason;
    return bool(backend.cache);
}

bool generation_ready(CharacterGeneration& backend)
{
    // Mesh and arenas in place, and no image of the generation still on its way (a reload candidate
    // brings up what the live generation shows before it takes over, D-094).
    return backend.cache && backend.ready.load() && (!backend.textures || !backend.textures->loading(backend.image_ids));
}

void generation_enable(CharacterGeneration& backend, bool enabled) { backend.enabled = enabled && bool(backend.cache); }

} // namespace efvk
