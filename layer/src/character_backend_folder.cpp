// The folder generation (D-131, docs/folder-generation.md): the Mods folder's one INI generation and its
// packages made, its Present, and the F10 exchange of every package at one presentation.
#include "character_backend_internal.h"

namespace efvk {
// What does not show, on the screen as in the log (docs/architecture-review-2.1.md, R-11): a folder
// that did not load, and the Mods and inputs left out with their reasons. At the first load and after
// every exchange.
void FolderGeneration::announce() const
{
    const auto mod = [](const std::string& ini) { return std::filesystem::u8path(ini).parent_path().filename().u8string(); };
    if (!program && !empty) notice(NoticeLevel::Dire, "", ("EFMI Vulkan: no Mod is loaded: " + reason).c_str());
    size_t shown = 0;
    for (const auto& m : left_out)
        if (shown++ < 6) notice(NoticeLevel::Warning, "", ("EFMI Vulkan: " + mod(m.first) + ": not loaded: " + m.second).c_str());
    for (const auto& m : left_inputs)
        if (shown++ < 6) notice(NoticeLevel::Warning, "", ("EFMI Vulkan: " + mod(m.first) + ": " + std::filesystem::u8path(m.first).filename().u8string() +
                                                          " left out: " + m.second).c_str());
    if (shown > 6) notice(NoticeLevel::Warning, "", ("EFMI Vulkan: " + std::to_string(shown - 6) + " more Mods or files not loaded, named in efmi_vk_layer.log").c_str());
    if (program && (!left_out.empty() || !left_inputs.empty()))
        notice(NoticeLevel::Info, "", ("EFMI Vulkan: " + std::to_string(packages.size()) + " Mods loaded, " +
                                       std::to_string(left_out.size() + left_inputs.size()) + " left out").c_str());
}

void FolderGeneration::note(const std::string& text)
{
    std::lock_guard<std::mutex> lock(note_mx);
    if (text == last_note) return;
    last_note = text; logf("folder generation: %s", text.c_str());
}

void FolderGeneration::save()
{
    if (saving.valid() && saving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (saving.valid()) saving.get();
    auto keep = generation;
    saving = std::async(std::launch::async, [keep] {
        std::string error;
        if (keep && !keep->save(error)) logf("folder generation settings: %s", error.c_str());
    });
}

void FolderGeneration::stop_now()
{
    if (saving.valid()) saving.get();
    for (auto& p : packages) if (p) p->stop();
    if (presenter) presenter->stop();
    if (program) { std::string error; if (!program->save(error)) logf("folder generation shutdown settings: %s", error.c_str()); }
}

namespace {
// A settings line `$\namespace\variable = value`: its key (before " =") and its namespace.
bool settings_line(std::string line, std::string& key, std::string& space, std::string& rest)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const size_t equals = line.find(" =");
    if (line.size() < 3 || line[0] != '$' || line[1] != '\\' || equals == std::string::npos) return false;
    key = line.substr(0, equals); rest = line.substr(equals);
    const size_t last = key.rfind('\\');
    if (last <= 1) return false;
    space = key.substr(2, last - 2);
    return true;
}
std::string lower_utf8(const std::filesystem::path& path)
{
    std::wstring wide = path.wstring();
    std::transform(wide.begin(), wide.end(), wide.begin(), ::towlower);
    return std::filesystem::path(wide).u8string();
}
// What a Mod's own generation saved (D-105, `<namespace>-<folder>.ini`) becomes the folder generation's
// settings the first time it runs, so the toggles a player set stay set. A generation of one Mod named
// each other INI of the Mod's folder by its path from that folder (`$\k\1.ini\z`, a panel several Mods
// ship); the folder generation names it by its path from the Mods folder's parent, as the original's
// d3dx_user.ini does (`$\mods\<Mod>\k\1.ini\z`), so those lines are renamed. 2.0.0 to 2.0.3 adopted
// them under the old names, which nothing reads and which no longer tell the Mods apart: such a file is
// adopted again once, its own lines first.
void adopt_mod_settings(const std::filesystem::path& settings, const std::string& name, const std::filesystem::path& mods)
{
    std::error_code error;
    const auto target = settings / (name + ".ini");
    const auto root = mods.parent_path();
    std::string reason, key, space, rest;
    std::vector<std::pair<std::string, std::string>> adopted; // key, line
    std::set<std::string> old_names;                          // the per-Mod names of other INIs
    for (const auto& ini : CharacterProgram::mods_characters(mods, reason)) {
        const auto folder = ini.parent_path();
        std::ifstream in(settings / (CharacterProgram::settings_name(ini, CharacterProgram::declared_namespace(ini)) + ".ini"), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!settings_line(line, key, space, rest)) continue;
            const auto file = folder / std::filesystem::u8path(space);
            if (space.size() > 4 && !_stricmp(space.c_str() + space.size() - 4, ".ini") && std::filesystem::is_regular_file(file, error)) {
                old_names.insert(lower_utf8(std::filesystem::u8path(space)));
                key = "$\\" + lower_utf8(file.lexically_relative(root)) + key.substr(2 + space.size());
            }
            adopted.emplace_back(key, key + rest);
        }
    }
    std::vector<std::string> lines;
    std::set<std::string> keys;
    if (std::filesystem::exists(target, error)) {
        std::ifstream in(target, std::ios::binary);
        std::string line;
        bool stale = false;
        while (std::getline(in, line)) {
            if (!settings_line(line, key, space, rest)) continue;
            if (old_names.count(lower_utf8(std::filesystem::u8path(space)))) { stale = true; continue; }
            if (keys.insert(key).second) lines.push_back(key + rest);
        }
        if (!stale) return;
    } else if (adopted.empty()) return;
    for (const auto& line : adopted) if (keys.insert(line.first).second) lines.push_back(line.second);
    std::filesystem::create_directories(settings, error);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << "; EFMI Vulkan persistent original INI variables\n[Constants]\n";
    for (const auto& line : lines) out << line << '\n';
    logf("folder generation: settings adopted from the Mods' own files into %s", target.u8string().c_str());
}
}

std::shared_ptr<FolderGeneration> folder_create(Device* device, const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceMemoryProperties& memory,
                                                TextureResidency* textures, std::shared_ptr<VariantCache> variants, Translucency* translucency, BoneMerge* bones)
{
    auto folder = std::make_shared<FolderGeneration>();
    const auto load_fixes = [&] {
        folder->shader_fixes = load_shader_fixes(device, variants ? variants->native_shaders() : nullptr,
            folder->program && folder->program->runtime() ? folder->program->runtime()->shader_depth_filters() : std::map<uint32_t, int>{});
    };
    folder->device = device;
    CharacterProgram::FolderSource source;
    source.mods = mods_folder();
    source.core = g_cfg.m2_core.empty() ? source.mods.parent_path() / "Core" / "EFMI" : absolute_folder(g_cfg.m2_core);
    source.settings = g_cfg.m2_settings.empty() ? module_folder() / "settings" : absolute_folder(g_cfg.m2_settings);
    source.settings_name = "mods-folder";
    try { adopt_mod_settings(source.settings, source.settings_name, source.mods); }
    catch (const std::exception& e) { logf("folder generation: settings not adopted: %s", e.what()); }
    const auto gpu = [device, &memory, variants](const std::filesystem::path& working, std::string& why) {
        return OwnedPresenter::runtime(device, memory, working, why, variants ? variants->pipeline_cache() : VK_NULL_HANDLE);
    };
    std::vector<CharacterProgram::FolderMod> mods;
    std::vector<std::pair<std::filesystem::path, std::string>> left_inputs;
    source.left_inputs = &left_inputs;
    source.generation = &folder->generation;
    const auto began = host_time_us();
    // Every INI gone (removed, disabled): the original's reload gives a configuration without them, and
    // so does this one. A folder without a character Mod is a folder as any other (D-174).
    if (!CharacterProgram::folder_has_inputs(source.mods)) {
        folder->empty = true; folder->reason = "the Mods folder holds no enabled INI";
        logf("folder generation: %s", folder->reason.c_str());
        load_fixes();
        return folder;
    }
    if (!CharacterProgram::discover_folder(source, mods, folder->reason, gpu) || !folder->generation) {
        if (folder->reason.empty()) folder->reason = "the folder's generation was not made";
        logf("folder generation refused: %s", folder->reason.c_str());
        for (auto& m : mods) if (!m.reason.empty()) folder->left_out.push_back({m.ini.u8string(), m.reason});
        folder->generation.reset();
        load_fixes();
        return folder;
    }
    folder->program = folder->generation.get();
    load_fixes();
    if (folder->program->gpu()) folder->presenter = std::make_shared<OwnedPresenter>(device, folder->program->gpu());
    else if (!folder->program->gpu_reason().empty()) logf("owned GPU work unavailable: %s", folder->program->gpu_reason().c_str());
    // The discard gates as the load left them (D-133); the live generation's reach the draws (BUG-070:
    // 2.0.7 set them from a reload candidate while it loaded).
    folder->gates = folder->program->discard_gates();
    {
        // What the original overlay would show after a load; the generation carries on (D-067).
        const auto loaded = folder->program->status();
        if (loaded.diagnostics) logf("INI loaded with %zu original warning(s)", loaded.diagnostics);
        for (const auto& text : loaded.diagnostic_messages) logf("INI warning: %s", text.c_str());
    }
    size_t plain = 0;
    for (auto& m : mods) {
        const auto path = m.ini.u8string();
        if (!m.program) {
            folder->left_out.push_back({path, m.reason});
            logf("character package refused: %s: %s", path.c_str(), m.reason.c_str());
            continue;
        }
        auto package = std::make_shared<CharacterGeneration>(device, properties, memory, path, true, textures, variants, translucency, bones);
        const bool without_skeleton = m.plain;
        if (!package->init_adopted(std::move(m.program), std::move(m.package), folder->presenter, folder->plan_mx)) {
            folder->left_out.push_back({path, package->last_reason});
            logf("character package refused: %s: %s", path.c_str(), package->last_reason.c_str());
            package->abandon();
            continue;
        }
        plain += without_skeleton;
        folder->packages.push_back(std::move(package));
    }
    for (const auto& item : left_inputs) {
        folder->left_inputs.push_back({item.first.u8string(), item.second});
        logf("folder generation: input left out: %s: %s", item.first.u8string().c_str(), item.second.c_str());
    }
    logf("folder generation: %zu packages (%zu of them without a skeleton), %zu left out, loaded in %llu ms", folder->packages.size(), plain,
         folder->left_out.size(), static_cast<unsigned long long>((host_time_us() - began) / 1000));
    return folder;
}

// The folder generation's part of a presentation: its INI Present once, under the lock its packages'
// plans take, and its saves (D-131).
void folder_frame(FolderGeneration& folder, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& base)
{
    const uint64_t now = host_time_us();
    if (had_draws) {
        if (folder.last_frame_us && now > folder.last_frame_us) {
            const float seconds = static_cast<float>(now - folder.last_frame_us) * 1.0e-6f;
            folder.frame_seconds.store(std::min(std::max(seconds, 0.001f), 0.25f), std::memory_order_relaxed);
        }
        folder.last_frame_us = now;
        folder.frame.fetch_add(1, std::memory_order_relaxed);
    }
    folder.presents.fetch_add(1, std::memory_order_relaxed);
    if (!folder.program) return;
    ThreadTrace* const trace = folder.device->trace ? folder.device->trace() : nullptr;
    StageMark locking(trace, "folder Present (generation lock)");
    std::lock_guard<std::mutex> lock(*folder.plan_mx);
    // What the layer did not do as the original does since the last presentation (LayerGap, D-137): each
    // in the log, the first three of a generation on the screen, then one line for the rest.
    if (const uint32_t count = folder.program->runtime_gap_count(); count != folder.gaps_seen) {
        for (const auto& text : folder.program->runtime_gaps_since(folder.gaps_seen)) {
            logf("folder generation: not as the original: %s", text.c_str());
            if (folder.gaps_shown < 3) notice(NoticeLevel::Warning, text.c_str(), ("EFMI Vulkan: " + text).c_str());
            else if (folder.gaps_shown == 3) notice(NoticeLevel::Notice, "", "EFMI Vulkan: more that the layer does not do as the original is in efmi_vk_layer.log");
            ++folder.gaps_shown;
        }
        folder.gaps_seen = count;
    }
    StageMark stage(trace, "folder Present (owned scope, INI Present, owned submission)");
    if (!ini_present(*folder.program, folder.presenter.get(), folder.frame.load(), folder.frame_seconds.load(std::memory_order_relaxed),
                     had_draws, present, base, now, [&folder](const std::string& text) { folder.note(text); }))
        return;
    // The discard gates the presentation's assignments left (D-133): any one set turns the dither off.
    folder.gates = folder.program->discard_gates();
    g_dither_off.store(std::any_of(folder.gates.begin(), folder.gates.end(), [](const auto& g) { return g.value != 0; }), std::memory_order_relaxed);
    g_dither_gated.store(!folder.gates.empty(), std::memory_order_relaxed);
    if (now - folder.save_checked_us >= g_cfg.settings_save_seconds * 1000000) {
        folder.save_checked_us = now;
        if (folder.program->settings_dirty()) folder.save();
    }
}

void CharacterBackend::retire_folder(std::shared_ptr<FolderGeneration> old)
{
    if (old) folder_retiring.push_back(std::async(std::launch::async, [old = std::move(old)] {
        try { old->stop(); }
        catch (const std::exception& e) { logf("folder generation retirement: %s", e.what()); }
    }));
}

// D-131: the folder generation's presentation work and its F10 (presenting thread). The successor loads in
// the background; packages the frames draw (activated ones) bring their successors up, with the images
// they show; once they are ready every package is exchanged at this one point, a Mod added becoming a
// package and one gone stopping, and the old generation stops off this thread.
void folder_frame_and_exchange(CharacterBackend& b, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& input)
{
    if (b.folder) folder_frame(*b.folder, had_draws, present, input);
    // A discard gate there now and not before (the start, RabbitFX added by F10, a module exchange): the
    // twins of the pipelines the game already has are prepared off this thread (D-133).
    const bool gated = g_dither_gated.load(std::memory_order_relaxed);
    if (!gated) b.twins_swept = false;
    else if (!b.twins_swept && !(b.twins_sweep.valid() && b.twins_sweep.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) {
        if (b.twins_sweep.valid()) b.twins_sweep.get();
        b.twins_swept = true;
        b.twins_sweep = std::async(std::launch::async, [&b] {
            try { prepare_twins(b); } catch (const std::exception& e) { logf("character dither: twins not prepared: %s", e.what()); }
        });
    }
    for (auto it = b.folder_retiring.begin(); it != b.folder_retiring.end();) {
        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) { it->get(); it = b.folder_retiring.erase(it); }
        else ++it;
    }
    // The live package of a path, and whether a frame drew it.
    // A refused successor: every package keeps its generation, and says so in its own status.
    auto refuse = [&b](const std::string& why) {
        ++b.folder_rejected; b.folder_error = why;
        for (auto& c : b.controllers) {
            std::unique_lock<std::shared_mutex> lock(c->mutex);
            if (c->folder_member && !c->stopped) { ++c->rejected; c->reload_error = why; }
        }
        // The original's F10 always loads what it can; a refused one of the layer's is shown (D-137).
        notice(NoticeLevel::Warning, "", ("> Reload failed, the Mods loaded before stay: " + why).c_str());
        b.folder_reload_began_us = 0;
    };
    auto live = [&b](const std::string& path, bool& activated) -> CharacterController* {
        for (auto& c : b.controllers) {
            std::shared_lock<std::shared_mutex> lock(c->mutex);
            if (c->path == path && !c->stopped && c->active) { activated = c->active->activated.load(); return c.get(); }
        }
        activated = false; return nullptr;
    };
    if (b.folder_loading.valid() && b.folder_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto candidate = b.folder_loading.get();
        if (!candidate || (!candidate->program && !candidate->empty)) {
            refuse(candidate && !candidate->reason.empty() ? candidate->reason : "folder generation refused");
            logf("INI reload refused; current folder generation retained: %s", b.folder_error.c_str());
            b.retire_folder(std::move(candidate));
        } else {
            b.folder_staged = std::move(candidate); b.folder_staged_at = host_time_us();
            for (auto& package : b.folder_staged->packages) {
                bool activated = false;
                CharacterController* controller = live(package->package_path, activated);
                if (!controller || !activated) continue;
                std::shared_ptr<CharacterGeneration> current;
                { std::shared_lock<std::shared_mutex> lock(controller->mutex); current = controller->active; }
                if (current) package->keep_mesh(*current);
                std::lock_guard<std::mutex> hold(package->mod_mx); package->activate();
                if (package->textures && current) package->textures->preload_matching(package->image_ids, current->image_ids);
            }
        }
    }
    if (b.folder_staged) {
        bool bad_image = false, ready = true;
        for (auto& package : b.folder_staged->packages) {
            bad_image |= package->textures && package->textures->any_failed(package->image_ids);
            if (package->activated.load() && !generation_ready(*package)) ready = false;
        }
        if (bad_image) {
            refuse("INI candidate texture failed to load; current folder generation retained");
            logf("%s", b.folder_error.c_str()); b.retire_folder(std::move(b.folder_staged));
        } else if (ready) {
            auto staged = std::move(b.folder_staged);
            if (b.folder && b.folder->program && staged->program) {
                b.folder->program->suspend_persistence();
                staged->program->inherit_persistent(*b.folder->program);
            }
            std::map<std::string, std::shared_ptr<CharacterGeneration>> by_path;
            for (auto& package : staged->packages) by_path[package->package_path] = package;
            std::vector<CharacterController*> gone;
            {
                // Every package at once: no draw sees one package exchanged and another not yet.
                std::vector<std::unique_lock<std::shared_mutex>> locks;
                for (auto& c : b.controllers) locks.emplace_back(c->mutex);
                for (auto& c : b.controllers) {
                    if (!c->folder_member || c->stopped) continue;
                    const auto found = by_path.find(c->path);
                    if (found == by_path.end()) { gone.push_back(c.get()); continue; }
                    found->second->enabled = c->active ? c->active->enabled.load() : g_cfg.m2_enabled;
                    c->active = found->second; ++c->generation; ++c->reloads; c->reload_error.clear();
                    by_path.erase(found);
                }
            }
            for (auto* c : gone) { c->stop_later(); logf("character package removed with its Mod: %s", c->path.c_str()); }
            for (auto& added : by_path) {
                CharacterController* back = nullptr;
                for (auto& c : b.controllers) if (c->folder_member && c->path == added.first) back = c.get();
                if (back && back->revive(added.second)) { logf("character package back with its Mod: %s", added.first.c_str()); continue; }
                if (b.controllers.size() >= kMaxPackages) { logf("character packages: no room for %s (4096 at most)", added.first.c_str()); break; }
                auto controller = std::make_unique<CharacterController>();
                controller->path = added.first; controller->lazy = true; controller->folder_member = true; controller->active = added.second;
                logf("character package found by F10: %s", added.first.c_str());
                std::unique_lock<std::shared_mutex> lock(b.index_mx);
                b.controllers.push_back(std::move(controller));
            }
            staged->save();
            // An F10 pressed while this one loaded reloads again, as each press does in the original (BUG-069).
            if (b.folder && b.folder->program && b.folder->program->take_reload_request()) {
                b.folder_reload_again = true; logf("folder generation: an F10 pressed while it loaded reloads again");
            }
            auto old = std::move(b.folder); b.folder = std::move(staged); ++b.folder_reloads; b.folder_error.clear();
            logf("folder generation %llu Ready; F10 exchange complete for %zu packages", static_cast<unsigned long long>(b.folder_reloads),
                 b.folder->packages.size());
            b.folder->announce();
            // As the original says it after its ReloadConfig (IniHandler.cpp): from the key to the exchange.
            if (b.folder_reload_began_us) {
                char text[64];
                snprintf(text, sizeof(text), "> Reloaded config in %.3fs", double(host_time_us() - b.folder_reload_began_us) / 1e6);
                notice(NoticeLevel::Info, "", text);
                b.folder_reload_began_us = 0;
            }
            b.retire_folder(std::move(old));
        } else if (host_time_us() - b.folder_staged_at > 30000000) {
            refuse("INI candidate resource upload failed or timed out; current folder generation retained");
            logf("%s", b.folder_error.c_str()); b.retire_folder(std::move(b.folder_staged));
        }
    }
    // Without a loaded generation (the folder refused at the start, or every character Mod was gone at the
    // last reload) there is no reload_config binding to take the key: it is the host's then, as the
    // original reloads whatever it holds. F10 did nothing in that state before, and the game had to be
    // started again after a Mod was repaired (docs/architecture-review-2.1.md, L-02).
    bool host_reload = false;
    if (b.folder && !b.folder->program) {
        const bool down = input.foreground && input.keys[VK_F10] && !input.keys[VK_CONTROL] && !input.keys[VK_SHIFT] && !input.keys[VK_MENU];
        host_reload = down && !b.host_f10_down;
        b.host_f10_down = down;
    } else b.host_f10_down = false;
    // A request is taken only when no reload runs: one pressed meanwhile waits for it (BUG-069).
    if (b.folder && !b.folder_loading.valid() && !b.folder_staged &&
        (b.folder->program ? (std::exchange(b.folder_reload_again, false) || b.folder->program->take_reload_request()) : host_reload)) {
        logf("folder generation: F10 reload starts");
        if (!b.folder_reload_began_us) b.folder_reload_began_us = host_time_us();
        Device* const d = b.device; const auto p = b.properties; const auto m = b.memory;
        TextureResidency* const textures = b.textures.get(); auto variants = b.variants; Translucency* const translucency = b.translucency.get();
        BoneMerge* const bones = b.bone_merge.get();
        auto current = b.folder;
        b.folder_loading = std::async(std::launch::async, [d, p, m, textures, variants, translucency, bones, current] {
            // The settings the successor reads are those of now.
            std::string reason;
            if (current->program && !current->program->save(reason)) logf("folder generation settings before reload: %s", reason.c_str());
            return folder_create(d, p, m, textures, variants, translucency, bones);
        });
    }
}

} // namespace efvk
