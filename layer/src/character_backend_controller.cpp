// One package's generations (CharacterController): the live one's Present, the F10 exchange of a package
// that is no member of a folder generation, its stop and its return; and the F10 rescan of the configured
// packages (BUG-037).
#include "character_backend_internal.h"

namespace efvk {
void CharacterController::retire(std::shared_ptr<CharacterGeneration> old)
{
    if (old) retiring.push_back(std::async(std::launch::async, [old = std::move(old)] {
        try { old->stop(); } catch (const std::exception& e) { logf("character generation retirement: %s", e.what()); }
    }));
}

// Device teardown: everything stops here, waiting for what it must.
void CharacterController::stop()
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (stopped) { if (stopping.valid()) stopping.get(); return; }
    stopped = true;
    if (loading.valid()) { auto candidate = loading.get(); if (candidate) candidate->stop(); }
    if (staged) staged->stop();
    if (active) active->stop();
    for (auto& future : retiring) future.get();
    retiring.clear(); staged.reset();
}

// The package's Mod went from the folder (an F10 rescan, BUG-062). 3DMigoto's ReloadConfig, which runs
// in Present, only releases what the new configuration no longer has and leaves the destruction to
// the D3D11 runtime once the GPU is done with it: it never waits there. Here the presenting thread only
// takes the package out (no draw or Present reaches it once `stopped` is set); its generations stop on
// a thread of their own, where waiting for their GPU work (the Present work submitted this very frame,
// which completes only after this presentation went) holds nothing that work depends on.
void CharacterController::stop_later()
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (stopped) return;
    stopped = true;
    // The stop owns the generations: the controller keeps nothing of the Mod that went (BUG-068).
    stopping = std::async(std::launch::async, [loading = std::move(loading), staged = std::move(staged), active = std::move(active),
                                               retiring = std::move(retiring), path = path]() mutable {
        try {
            if (loading.valid()) { auto candidate = loading.get(); if (candidate) candidate->stop(); }
            if (staged) staged->stop();
            if (active) active->stop();
            for (auto& future : retiring) future.get();
        } catch (const std::exception& e) { logf("character package stop (%s): %s", path.c_str(), e.what()); }
    });
    retiring.clear(); staged.reset(); active.reset();
}

// The Mod of a stopped folder package is back (an F10 found it again): the controller takes its new
// package in the same slot, a new generation for the draws' index. Draws index the controllers
// without a lock, so slots are never removed; 2.0.7 added one per return and ran out at 64 (BUG-068).
// Only once the earlier stop is done; otherwise the caller takes a new slot.
bool CharacterController::revive(std::shared_ptr<CharacterGeneration> package)
{
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (!stopped || !folder_member) return false;
    if (stopping.valid()) {
        if (stopping.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        stopping.get();
    }
    active = std::move(package); staged.reset(); retiring.clear();
    ++generation; ++reloads; reload_error.clear(); stopped = false;
    return true;
}

void CharacterController::frame(bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& input)
{
    if (folder_member) {
        std::shared_lock<std::shared_mutex> lock(mutex);
        if (!stopped && active) generation_frame(*active, had_draws, present, input);
        return;
    }
    {
        // The generation's Present work holds the controller shared: the submitting threads go
        // on while it runs (BUG-034); the exchange below takes the lock exclusively.
        std::shared_lock<std::shared_mutex> lock(mutex);
        if (stopped) return;
        if (active) generation_frame(*active, had_draws, present, input);
    }
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (stopped) return;
    if (!active) {
        // A package found by an F10 rescan of the Mods folder comes up here (BUG-037), as the
        // packages of the folder came up at the start: read in the background, taken as it is.
        if (loading.valid() && loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto candidate = loading.get();
            if (!candidate) { logf("character package refused: %s", path.c_str()); stopped = true; return; }
            active = std::move(candidate); ++generation;
            logf("character package added by F10: %s", path.c_str());
        }
        return;
    }
    for (auto it = retiring.begin(); it != retiring.end();) {
        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) { it->get(); it = retiring.erase(it); }
        else ++it;
    }
    if (loading.valid() && loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto candidate = loading.get(); std::string reason;
        if (!candidate || !generation_usable(*candidate, reason)) {
            ++rejected; reload_error = reason.empty() ? "INI generation preparation failed" : reason;
            logf("INI reload refused; current generation retained: %s", reload_error.c_str()); retire(std::move(candidate));
        } else {
            staged = std::move(candidate); staged_at = host_time_us();
            // A lazy package already on the GPU brings its successor up at once, with the textures
            // the live generation shows: the same images, unless a file changed (BUG-061).
            if (active->activated.load()) {
                staged->keep_mesh(*active);
                std::lock_guard<std::mutex> hold(staged->mod_mx); staged->activate();
                if (staged->textures) staged->textures->preload_matching(staged->image_ids, active->image_ids);
            }
        }
    }
    if (staged) {
        // A candidate whose texture file does not load is refused and the live generation kept.
        const bool bad_image = staged->textures && staged->textures->any_failed(staged->image_ids);
        // A lazy package never matched yet needs nothing on the GPU to take over.
        if (bad_image) {
            ++rejected; reload_error = "INI candidate texture failed to load; current generation retained";
            logf("%s", reload_error.c_str()); retire(std::move(staged));
        } else if (generation_ready(*staged) || (staged->lazy && !staged->activated.load())) {
            staged->enabled = active->enabled.load();
            if (active->program && staged->program) {
                active->program->suspend_persistence();
                staged->program->inherit_persistent(*active->program);
            }
            staged->save_requested = true; staged->wake.notify_one();
            auto old = std::move(active); active = std::move(staged); ++generation; ++reloads; reload_error.clear();
            logf("INI generation %llu Ready; F10 exchange complete", static_cast<unsigned long long>(generation));
            retire(std::move(old));
        } else if (host_time_us() - staged_at > 30000000) {
            ++rejected; reload_error = "INI candidate resource upload failed or timed out; current generation retained";
            logf("%s", reload_error.c_str()); retire(std::move(staged));
        }
    }
    if (active->program && active->program->take_reload_request()) reload_taken = true;
    if (reload_taken && !loading.valid() && !staged) {
        auto previous = active;
        loading = std::async(std::launch::async, [previous] {
            std::string reason;
            if (!previous->program->save(reason)) {
                auto failed = std::make_shared<CharacterGeneration>(previous->device, previous->properties, previous->memory,
                                                                    previous->package_path, previous->lazy, previous->textures,
                                                                    previous->variants, previous->translucency, previous->bones_gpu);
                failed->note("Cannot preserve settings before reload: " + reason); return failed;
            }
            return generation_create(previous->device, previous->properties, previous->memory, previous->package_path,
                                     previous->lazy, previous->textures, previous->variants, previous->translucency, previous->bones_gpu,
                                     previous.get());
        });
    }
}

// F10 reads the Mods folder again, as the original does (BUG-037): a Mod added since the start
// (a new folder, a folder enabled) becomes a package, read in the background; a package whose Mod
// is gone (folder removed, renamed, disabled) stops. The packages that stay reload their INIs
// through their own F10 handling. Presenting thread.
void character_rescan(CharacterBackend& b)
{
    std::string reason;
    const auto paths = configured_packages(reason);
    const std::set<std::string> present(paths.begin(), paths.end());
    std::set<std::string> known;
    for (auto& c : b.controllers) {
        bool live = false;
        { std::shared_lock<std::shared_mutex> lock(c->mutex); live = !c->stopped; }
        if (!live) continue;
        if (present.count(c->path)) { known.insert(c->path); continue; }
        c->stop_later();
        logf("character package removed with its Mod: %s", c->path.c_str());
    }
    for (const auto& path : paths) {
        if (known.count(path)) continue;
        if (b.controllers.size() >= kMaxPackages) { logf("character packages: no room for %s (4096 at most)", path.c_str()); break; }
        auto controller = std::make_unique<CharacterController>();
        controller->path = path; controller->lazy = true;
        Device* const d = b.device; const auto p = b.properties; const auto m = b.memory;
        TextureResidency* const textures = b.textures.get(); auto variants = b.variants; Translucency* const translucency = b.translucency.get();
        BoneMerge* const bones = b.bone_merge.get();
        controller->loading = std::async(std::launch::async, [d, p, m, path, textures, variants, translucency, bones] {
            return generation_create(d, p, m, path, true, textures, variants, translucency, bones);
        });
        logf("character package found by F10: %s", path.c_str());
        std::unique_lock<std::shared_mutex> lock(b.index_mx);
        b.controllers.push_back(std::move(controller));
    }
}

} // namespace efvk
