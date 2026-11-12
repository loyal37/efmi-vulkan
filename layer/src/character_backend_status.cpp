// The status the probe reads: the backend's, the folder generation's, a package's and its counters.
#include "character_backend_internal.h"

namespace efvk {
static void controller_status(CharacterController& c, JsonWriter& w)
{
    std::shared_lock<std::shared_mutex> lock(c.mutex);
    w.kn("ini_generation", c.generation); w.kn("ini_reloads", c.reloads); w.kn("ini_reload_rejected", c.rejected);
    w.kb("ini_reload_pending", c.loading.valid() || bool(c.staged)); w.kv("ini_reload_error", c.reload_error);
    w.kb("stopped", c.stopped);
    if (c.active) generation_status(*c.active, w);
}
// One package's fields at the top as before: the first one on the GPU, else the first. Every package
// under "packages" when there are more.
void character_status(CharacterBackend& b, JsonWriter& w)
{
    CharacterController* primary = b.controllers.empty() ? nullptr : b.controllers.front().get();
    for (auto& c : b.controllers) {
        std::shared_lock<std::shared_mutex> lock(c->mutex);
        if (c->active && c->active->activated.load()) { primary = c.get(); break; }
    }
    if (primary) controller_status(*primary, w);
    else w.kv("package_reason", b.reason);
    w.key("record"); w.begin_object();
    w.kn("calls", b.record_calls.load()); w.kn("microseconds", b.record_us.load());
    w.kn("frame_calls", b.frame_calls.load()); w.kn("frame_microseconds", b.frame_us.load());
    w.kn("hash_calls", b.hash_calls.load()); w.kn("hash_microseconds", b.hash_us.load()); w.kn("hash_misses", b.hash_misses.load());
    w.kn("full_hashes", g_hash_full.load()); w.kn("full_hash_bytes", g_hash_bytes.load());
    w.kn("full_new", g_hash_new.load()); w.kn("full_generation", g_hash_gen.load()); w.kn("full_fingerprint", g_hash_quick.load()); w.kn("full_due", g_hash_due.load());
    w.kn("fingerprint_reads", g_quick_reads.load()); w.kn("fingerprint_skipped", g_quick_skipped.load());
    w.kn("fingerprint_read_microseconds", g_quick_read_us.load());
    w.kn("mesh_uploads", g_mesh_uploads.load()); w.kn("meshes_kept", g_meshes_kept.load());
    w.kb("pipeline_cache", b.pipeline_cache != VK_NULL_HANDLE); w.kn("pipeline_cache_loaded_bytes", b.pipeline_cache_loaded);
    if (b.variants) { w.kn("variant_waits", b.variants->waits()); w.kn("variant_wait_microseconds", b.variants->wait_microseconds()); }
    w.kb("upload_queue", b.device && b.device->upload_queue != VK_NULL_HANDLE);
    w.kn("snapshot_copy_us", g_snapshot_timing.copy_us.load()); w.kn("snapshot_descriptor_us", g_snapshot_timing.descriptor_us.load());
    w.kn("snapshot_images_us", g_snapshot_timing.images_us.load()); w.kn("snapshot_entry_us", g_snapshot_timing.entry_us.load());
    w.end_object();
    if (b.translucency) {
        // EFMITTL's captures and composites at the ends of passes (D-113).
        w.key("translucency"); w.begin_object();
        w.kn("captures", b.translucency->captures()); w.kn("composites", b.translucency->composites());
        const auto layers = b.translucency->memory();
        w.kn("layer_sets", layers.sets); w.kn("layers", layers.layers); w.kn("layer_bytes", layers.bytes);
        w.kv("last_error", b.translucency->last_error()); w.end_object();
    }
    if (b.bone_merge) b.bone_merge->status(w);
    if (b.textures) b.textures->status(w);
    if (b.folder) {
        // D-131: the folder generation, its reloads and the Mods it left out.
        w.key("folder_generation"); w.begin_object();
        w.kn("packages", b.folder->packages.size()); w.kn("presents", b.folder->presents.load());
        w.kn("reloads", b.folder_reloads); w.kn("rejected", b.folder_rejected); w.kv("reload_error", b.folder_error);
        w.kb("loading", b.folder_loading.valid()); w.kb("staged", b.folder_staged != nullptr);
        w.key("left_out"); w.begin_array();
        for (const auto& m : b.folder->left_out) { w.begin_object(); w.kv("ini", m.first); w.kv("reason", m.second); w.end_object(); }
        w.end_array();
        w.key("left_inputs"); w.begin_array();
        for (const auto& m : b.folder->left_inputs) { w.begin_object(); w.kv("ini", m.first); w.kv("reason", m.second); w.end_object(); }
        w.end_array();
        if (b.folder->presenter) b.folder->presenter->status(w);
        if (b.folder->program) {
            const auto ini = b.folder->program->status();
            w.key("ini"); w.begin_object(); w.kn("presents", ini.presents); w.kn("draws", ini.draws); w.kn("diagnostics", ini.diagnostics); w.end_object();
        }
        {
            std::lock_guard<std::mutex> lock(*b.folder->plan_mx);
            w.key("discard_gates"); w.begin_array();
            for (const auto& g : b.folder->gates) {
                w.begin_object(); w.kn("index", g.index); w.kn("component", g.component); w.key("value"); w.raw(std::to_string(g.value)); w.end_object();
            }
            w.end_array();
        }
        w.kb("dither_off", g_dither_off.load());
        {
            std::shared_lock<std::shared_mutex> lock(b.twins_mx);
            w.kn("dither_twins", b.twins.size()); w.kn("dither_twins_failed", b.twins_failed.size());
        }
        w.kn("dither_twin_binds", b.twin_binds.load());
        w.kn("dither_twins_prepared", b.twins_prepared.load());
        w.end_object();
    }
    {
        const auto engine = efmi::frontend::engine_lock_stats();
        w.key("engine_lock"); w.begin_object();
        w.kn("entries", engine.entries); w.kn("waits", engine.waits); w.kn("wait_us", engine.wait_us);
        w.end_object();
    }
    if (b.controllers.size() > 1) {
        w.key("packages"); w.begin_array();
        for (auto& c : b.controllers) {
            w.begin_object(); w.kv("path", c->path); controller_status(*c, w); w.end_object();
        }
        w.end_array();
    }
}

void generation_status(CharacterGeneration& backend, JsonWriter& w)
{
    if (backend.presenter) backend.presenter->status(w);
    if (backend.program && !backend.program->gpu_reason().empty()) w.kv("owned_gpu_unavailable", backend.program->gpu_reason());
    if (backend.program) {
        const auto status = backend.program->status();
        w.key("ini"); w.begin_object();
        w.kn("draws", status.draws); w.kn("presents", status.presents); w.kn("replays", status.replays); w.kn("key_events", status.key_events);
        w.kn("draw_execute_us", status.draw_execute_us); w.kn("draw_compile_us", status.draw_compile_us);
        {
            auto commands = status.commands;
            std::sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
            if (commands.size() > 16) commands.resize(16);
            w.key("commands"); w.begin_array();
            for (const auto& c : commands) {
                w.begin_object(); w.kv("name", std::get<0>(c)); w.kn("calls", std::get<1>(c)); w.kn("microseconds", std::get<2>(c)); w.end_object();
            }
            w.end_array();
        }
        w.kn("bindings", status.bindings); w.kn("saves", status.saves); w.kb("dirty", status.dirty);
        w.kb("reload_requested", status.reload_requested);
        w.kn("diagnostics", status.diagnostics);
        w.key("diagnostic_messages"); w.begin_array();
        for (const auto& text : status.diagnostic_messages) w.str(text.c_str());
        w.end_array();
        w.key("variables"); w.begin_object();
        for (const auto& value : status.variables) { w.key(value.first.c_str()); if (std::isfinite(value.second)) w.raw(std::to_string(value.second)); else w.null(); }
        w.end_object(); w.end_object();
    }
    w.kv("kind", "character");
    w.kb("configured", bool(backend.cache));
    w.kb("enabled", backend.enabled.load());
    w.kb("ready", backend.ready.load());
    // The names the single-component backend reports, so that one reader serves both.
    uint64_t unsupported = 0, unreadable = backend.counters[C_ENTRY].load() + backend.counters[C_BONES].load();
    for (uint32_t c = C_STATE; c <= C_BINDINGS; ++c) unsupported += backend.counters[c].load();
    unsupported += backend.counters[C_MATERIAL].load() + backend.counters[C_IMAGE].load();
    w.kn("matched", backend.counters[C_MATCHED].load());
    w.kn("replaced", backend.counters[C_REPLACED].load());
    w.kn("pending", backend.counters[C_PENDING].load());
    w.kn("unsupported", unsupported);
    w.kn("unreadable", unreadable);
    w.kn("exhausted", backend.counters[C_EXHAUSTED].load());
    w.kn("sealed_batches", backend.batches->load());
    w.key("counters"); w.begin_object();
    for (uint32_t c = 0; c < C_COUNT; ++c) w.kn(kCounterNames[c], backend.counters[c].load());
    w.end_object();
    w.kn("frame", backend.frame.load()); w.kn("presents", backend.presents.load());
    w.kn("sections", backend.package.sections.size());
    // The parts of a character the package's skeleton has; none: a package without one (D-174).
    w.kn("components", backend.package.components.size());
    w.kn("package_bytes", backend.package.payload_bytes);
    w.kn("payload_held_bytes", backend.package.payload.size()); // in host memory (ISSUE-011: none once loaded)
    w.kn("mesh_upload_microseconds", backend.mesh_upload_us.load());
    w.kb("materials", backend.package.materials);
    w.kn("material_profiles", backend.package.profiles.size());
    w.kn("image_count", backend.image_ids.size());
    {
        // Of this package's images: resident now (published), failed, and their bytes on disk.
        uint64_t resident = 0, failed = 0, bytes = 0;
        if (backend.textures)
            for (const auto id : backend.image_ids) {
                if (id == TextureResidency::kNone) continue; // a file that does not load, never declared
                if (backend.textures->resident(id)) { ++resident; bytes += backend.textures->file_bytes(id); }
                else if (backend.textures->failed(id)) ++failed;
            }
        w.kn("images_ready", resident); w.kn("images_failed", failed); w.kn("image_resident_bytes", bytes);
    }
    w.kb("dimension_zero_policy", backend.package.material_vendor != 0);
    w.kn("material_graphics_queues", backend.device->material_queues.size());
    w.kn("material_graphics_family", backend.device->material_family);
    {
        std::lock_guard<std::mutex> lock(backend.pool_mx);
        w.kv("last_reason", backend.last_reason);
        w.kn("available_batches", backend.available.size());
        w.kb("mesh_in_device_memory", backend.mesh && backend.mesh->device_local);
        w.kv("mesh_memory", backend.mesh ? mesh_memory_name(backend.mesh->placement) : "");
    }
    {
        std::lock_guard<std::mutex> lock(backend.mod_mx);
        uint32_t used = 0, custom = 0;
        for (const auto& identity : backend.identities) { used += identity.used; custom += identity.used && identity.custom; }
        w.kn("identities", used);
        w.kn("identities_replaced", custom);
        w.kn("lod", backend.lod);
        w.kb("gate_open", backend.gate_open); w.kn("gate_images", backend.frame_images.size());
        w.key("stage_us"); w.begin_object();
        for (uint32_t s = 0; s < S_COUNT; ++s) w.kn(kStageNames[s], backend.stage_us[s].load());
        w.end_object();
        w.kn("material_derived_profiles", backend.derived_profiles.size());
        w.kb("lazy", backend.lazy);
        w.kn("texture_queries", backend.texture_queries);
        w.kn("texture_role_queries", backend.role_queries);
        w.kn("texture_role_draws", backend.role_draws);
        w.kn("source_facts_unknown", backend.facts_unknown);
        w.kn("identify_components", backend.program ? backend.program->identify_components() : 0);
        w.kn("texture_unknown", backend.texture_unknown);
        if (backend.texture_reason) w.kv("texture_unknown_reason", backend.texture_reason);
        w.kb("activated", backend.activated.load());
        w.kn("material_rule_mismatches", backend.rule_mismatches);
    }
}

} // namespace efvk
