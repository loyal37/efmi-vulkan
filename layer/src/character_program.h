#pragma once
#include "character_package.h"
#include "../../frontend/xxmi/runtime.h"
#include "../../frontend/xxmi/owned_runtime.h"
#include <algorithm>
#include <functional>
#include <string>
#include <map>
#include <unordered_map>
#include <set>
#include <tuple>

namespace efmi::frontend { struct CharacterDiscovery; }

namespace efvk {
// A parsed original INI generation and its file-to-renderer resource catalogue.
// A missing sidecar keeps the existing static/sandbox package behavior.
class CharacterProgram {
public:
    static bool configured(const std::string& package_path);
    // `gpu` makes the owned runtime for this generation (null keeps a CPU generation). With one,
    // the Mod folder's other enabled INIs join the generation, and the GPU commands of loading
    // stay recorded until the first Present (D-072).
    using GpuFactory = std::function<std::shared_ptr<efmi::frontend::OwnedRuntime>(const std::filesystem::path& working, std::string& why)>;
    bool init(const std::string& package_path, CharacterPackage&, std::string& reason, const GpuFactory& gpu = {});
    // Directory generation (D-078): no package file. The Mod's INI is loaded as for a package and the
    // package is made from what the original engine declares: the contract from the generation
    // (frontend CharacterDiscovery), every mesh file of its resources as a stream, its DDS files as
    // the image catalogue. Bone streams are found by the live plans. `core` is Core\EFMI.
    struct Source {
        std::filesystem::path ini, core, settings;
        std::string mod_namespace;
        // The Mods folder, where the libraries the Mod calls by namespace are found (D-112); empty: none.
        std::filesystem::path mods;
        // Offline, without a GPU: a Mod with CustomShader sections loads with them declared, so its
        // sections and draw plans are known; the lists that run them refuse as outside the owned domain.
        bool declarations_only = false;
    };
    bool discover(const Source&, CharacterPackage&, std::string& reason, const GpuFactory& gpu = {});
    // A folder generation (D-131, docs/folder-generation.md): the Mods folder's character Mods in one
    // generation, as the original loads the whole folder into one space. Per character Mod (in the
    // order of `mods_characters`) a program that shares the generation, made from its discovery as
    // `discover` makes one, or the reason it has none. False with `reason` when the generation does not
    // load. `settings_name` names the generation's settings file in `settings`.
    struct FolderSource {
        std::filesystem::path mods, core, settings;
        std::string settings_name = "mods";
        bool declarations_only = false;
        // When given: the inputs that are no character Mod's and that the folder was loaded without,
        // each with why (the folder did not load with it).
        std::vector<std::pair<std::filesystem::path, std::string>>* left_inputs = nullptr;
        // When given: the generation itself, a program without a package (D-174). What a presentation, the
        // settings and an F10 need is the generation's, whether or not the folder has a package.
        std::shared_ptr<CharacterProgram>* generation = nullptr;
    };
    struct FolderMod {
        std::filesystem::path ini;
        std::string mod_namespace;                 // declared, else the original's implicit one (the INI's path)
        std::unique_ptr<CharacterProgram> program; // null when the Mod has no package
        CharacterPackage package;
        std::string reason;
        // A package without a skeleton (D-174): the mesh sections of the INIs of one folder outside every
        // character Mod's folder. `ini` is that folder's first INI in the original's order.
        bool plain = false;
    };
    static bool discover_folder(const FolderSource&, std::vector<FolderMod>&, std::string& reason, const GpuFactory& gpu = {});
    // The Mod's index in its folder generation (RuntimeDraw::mod); UINT32_MAX in a generation of one Mod.
    uint32_t folder_index() const { return folder_index_; }
    // The namespace an INI declares (`namespace = ...`), lower case; empty when it declares none.
    static std::string declared_namespace(const std::filesystem::path& ini);
    // The settings file name of a generation of one Mod (D-105): its namespace (else its file name) and
    // its Mod folder, in plain characters.
    static std::string settings_name(const std::filesystem::path& ini, const std::string& mod_namespace);
    // The character Mods of an EFMI Mods folder: every enabled INI (no part of its path starts with
    // DISABLED, as the original skips them) whose lists call the Core's InitializeMergedSkeleton (D-177).
    static std::vector<std::filesystem::path> mods_characters(const std::filesystem::path& mods, std::string& reason);
    static std::vector<std::filesystem::path> native_library_files(const std::filesystem::path& mods);
    // Whether the Mods folder holds an enabled INI at all: without one there is nothing to load.
    static bool folder_has_inputs(const std::filesystem::path& mods);
    // The version the installed Core declares in its main.ini (`global $version`), 0 when not found.
    static float core_version(const std::filesystem::path& core);
    // What `vs` reads for a bound vertex shader under the installed Core (BUG-090), NaN for nothing.
    static float core_vertex_filter(const std::filesystem::path& core);
    // The libraries a generation of `inputs` calls (D-112): every enabled INI of the Mods folder outside
    // every character Mod's folder that declares a namespace the inputs reference (`CommandList\NS\...`,
    // `$\NS\...`) and that neither an input nor `known` (the extension's own libraries) declares; then
    // those they call. `notes` names the ones left out and why.
    static std::vector<std::filesystem::path> libraries(const std::filesystem::path& mods, const std::vector<std::filesystem::path>& inputs,
                                                        const std::vector<std::string>& known, std::vector<std::string>& notes,
                                                        bool one_generation = false);
    // The libraries the generation loaded and those it left out (D-112), for the log.
    const std::vector<std::string>& library_notes() const { return library_notes_; }
    // The libraries the Mod calls by namespace (D-112), by namespace: what library_folder answers from.
    const std::map<std::string, std::filesystem::path>& called_libraries() const { return libraries_; }
    // What the original does for the Mod's sections and the layer does not (CharacterDiscovery::gaps).
    const std::vector<std::string>& gaps() const { return gaps_; }
    // What the layer did not do as the original does while the generation runs (IniRuntime::gap_count):
    // how many texts there were, read without a lock, and those after the first `seen`.
    uint32_t runtime_gap_count() const { return runtime_ ? runtime_->gap_count() : 0; }
    std::vector<std::string> runtime_gaps_since(uint32_t seen) const { return runtime_ ? runtime_->gaps_since(seen) : std::vector<std::string>{}; }
    // A package image whose file the original's loader makes nothing of (D-139): never declared, bound as nothing.
    bool image_unloadable(const std::string& path) const;
    // Whether a file lies under the Mod's own folders (its folder, the Core's, the libraries it calls); every file
    // in a generation of one Mod. A folder generation's catalogue holds every image of the folder (D-178).
    bool owned(const std::string& utf8_path) const { return owns(std::filesystem::u8path(utf8_path)); }
    // The folder of the library of this namespace the generation loaded (D-112), empty without one.
    std::filesystem::path library_folder(const std::string& name) const
    {
        const auto found = libraries_.find(name);
        return found == libraries_.end() ? std::filesystem::path{} : found->second.parent_path();
    }
    // EFMITTL is loaded with the interface its bridge knows (D-113): the renderer supplies its hook
    // facts and records its captures.
    bool efmittl() const { return efmittl_; }
    // A shader hook event of the game's frame for the generation's libraries (D-113).
    void library_hook(efmi::frontend::LibraryHook hook) { if (runtime_) runtime_->library_hook(hook); }
    const std::shared_ptr<efmi::frontend::OwnedRuntime>& gpu() const { return gpu_; }
    const std::string& gpu_reason() const { return gpu_reason_; }
    // Mesh files the generation's INIs reference that are not in the package's stream profile.
    size_t outside_meshes() const { return outside_meshes_; }
    // Discovery: mesh sections of the mod the contract has no part for (they stay the game's).
    size_t left_to_game() const { return left_to_game_; }
    // The Mod's identification threshold (CharacterDiscovery::identify_components); 0 unknown.
    uint32_t identify_components() const { return identify_components_; }
    // `vertex`: the vertex input of the pipeline the draw uses, where the blend-index element is found
    // for a bone stream first seen; without it (offline) the packager's rule applies.
    // `textures`: when the plan leaves the application's draw (skip false) but binds textures for it in
    // its pre phase, and nothing else, `output` is a list of one texture-only draw with those pixels
    // and *textures is true (D-083). Without the pointer such a plan is just the application's draw.
    void note_timing(const char* name, uint64_t microseconds) { if (runtime_) runtime_->note_timing(name, microseconds); }
    bool plan(const efmi::frontend::RuntimeDraw&, CharacterPackage&, CharacterList&, bool& skip, bool& added, std::string& reason,
              const VertexInputRecipe* vertex = nullptr, bool* textures = nullptr);
    bool present(const efmi::frontend::RuntimeInput&, uint64_t frame, float seconds, std::string& reason, bool keep_replay_state = true);
    bool replayable() const { return runtime_ && runtime_->replayable(); }
    // The last Present again, for another presentation of the same frame (BUG-036).
    bool replay(const efmi::frontend::RuntimeInput&, uint64_t frame, float seconds, std::string& reason);
    bool save(std::string& reason) { return !runtime_ || runtime_->save(reason); }
    bool settings_dirty() const { return runtime_ && runtime_->settings_dirty(); }
    bool take_reload_request() { return runtime_ && runtime_->take_reload_request(); }
    void suspend_persistence() { if (runtime_) runtime_->suspend_persistence(); }
    void inherit_persistent(const CharacterProgram& previous) { runtime_->inherit_persistent(*previous.runtime_); }
    efmi::frontend::RuntimeStatus status() const { return runtime_ ? runtime_->status() : efmi::frontend::RuntimeStatus{}; }
    // The keys of the generation's key actions (D-137 checks press them as a player does).
    std::vector<std::vector<int>> key_combos() const { return runtime_ ? runtime_->key_combos() : std::vector<std::vector<int>>{}; }
    // D-133: the IniParams elements ShaderRegex replacements gate a discard on, as the last presentation left them.
    std::vector<efmi::frontend::IniRuntime::DiscardGate> discard_gates() const
    {
        return runtime_ ? runtime_->discard_gates() : std::vector<efmi::frontend::IniRuntime::DiscardGate>{};
    }
private:
    // The generation from `options` and `extension`: the owned runtime's part when there is one, the
    // declarations of CustomShaders only otherwise when `declarations_only`.
    bool start(efmi::frontend::RuntimeConfig& options, std::shared_ptr<efmi::frontend::RuntimeExtension>, bool declarations_only, std::string& reason);
    // The package of a discovered Mod: its contract, every mesh file it declares as a stream, its DDS
    // files as the image catalogue (a folder Mod's: the files under its folders only).
    bool adopt(efmi::frontend::CharacterDiscovery& found, CharacterPackage&, std::string& reason);
    bool owns(const std::filesystem::path& file) const;
    uint32_t folder_index_ = UINT32_MAX;
    std::vector<std::wstring> folders_; // a folder Mod's: its folder, the Core's, the libraries it calls (lower case, with a separator)
    // Folders under `folders_` that are another package's (D-174): a package without a skeleton whose folder
    // holds other Mods' folders does not take their files.
    std::vector<std::wstring> foreign_;
    bool load(const std::filesystem::path& ini, const std::filesystem::path& core, const std::filesystem::path& settings,
              const std::string& settings_name, std::shared_ptr<efmi::frontend::RuntimeExtension>, const GpuFactory&, std::string& reason,
              const std::filesystem::path& mods = {}, bool declarations_only = false);
    void catalogue(CharacterPackage&, const std::map<std::wstring, uint32_t>& remaps);
    // Whether a texture file loads (D-139) with these loader flags, and as what format (D-140); one that
    // does not is recorded, the Mod told.
    bool image_checked(const std::filesystem::path& path, uint32_t loader_flags);
    std::shared_ptr<efmi::frontend::OwnedRuntime> gpu_;
    std::string gpu_reason_;
    std::vector<std::string> library_notes_;
    std::vector<std::string> gaps_;
    std::map<std::string, std::filesystem::path> libraries_; // loaded libraries by namespace
    bool efmittl_ = false;
    std::shared_ptr<efmi::frontend::IniRuntime> runtime_;
    std::map<std::wstring, uint32_t> streams_, images_;
    // Mesh files the generation declares that gave no buffer (BUG-077): bound, they bind nothing.
    std::set<std::wstring> absent_;
    // Texture files the original's loader makes no texture of (D-139): bound, they bind nothing.
    std::set<std::wstring> unloadable_;
    // The format each catalogued image loads as, by its key (path and loader flags), D-140.
    std::map<std::wstring, VkFormat> image_formats_;
    // The stream or image a plan item's resource names, by the resource object the compiler hands
    // out for a declaration (stable for the generation): the path key is made once per resource,
    // not per draw (it cost 10 us per matched draw).
    std::unordered_map<const void*, uint32_t> stream_cache_, image_cache_;
    efmi::assets::FilePlan file_plan_; // the last draw's, kept for its capacity (D-108)
    // A pixel slot's image (or kCharacterNone) while a plan is read: a few entries, kept sorted by
    // slot, no node per entry as a std::map has.
    struct PixelSlots {
        std::vector<std::pair<uint32_t, uint32_t>> values;
        uint32_t& operator[](uint32_t slot)
        {
            auto it = std::lower_bound(values.begin(), values.end(), slot, [](const auto& v, uint32_t s) { return v.first < s; });
            if (it == values.end() || it->first != slot) it = values.insert(it, {slot, 0});
            return it->second;
        }
        void erase(uint32_t slot)
        {
            auto it = std::lower_bound(values.begin(), values.end(), slot, [](const auto& v, uint32_t s) { return v.first < s; });
            if (it != values.end() && it->first == slot) values.erase(it);
        }
        bool empty() const { return values.empty(); }
        size_t size() const { return values.size(); }
        void clear() { values.clear(); }
        auto begin() const { return values.begin(); }
        auto end() const { return values.end(); }
    };
    // The containers a plan is read into, kept for their capacity (D-108).
    PixelSlots scratch_pixels_;
    std::vector<CharacterDraw> scratch_draws_;
    std::vector<std::vector<CharacterPixel>> scratch_material_;
    std::vector<uint8_t> scratch_signature_;
    std::string scratch_id_;
    uint32_t stream_index(const std::shared_ptr<const efmi::assets::FileResource>& resource);
    uint32_t image_index(const std::shared_ptr<const efmi::assets::FileResource>& resource);
    bool forget_plans_when_full(CharacterPackage& package, size_t adding);
    uint64_t plans_forgotten_ = 0;
    std::unordered_map<std::string, CharacterList> lists_;
    std::vector<std::pair<uint32_t, uint32_t>> index_bounds_;
    // Per index stream, the lowest and highest index of each block of kIndexBlock indices (D-148): a draw's
    // own index range is bounded by its blocks, and read exactly from the file only when that bound
    // reaches past a vertex stream.
    static constexpr uint32_t kIndexBlock = 64;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> index_blocks_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> exact_ranges_;
    std::set<uint32_t> past_logged_;
    // The lowest and highest index a draw's own range reads; `exact` reads the file for it. False when the
    // file cannot be read again.
    bool index_range(const CharacterPackage&, uint32_t stream, uint32_t first, uint32_t count, bool exact,
                     std::pair<uint32_t, uint32_t>& range);
    size_t outside_meshes_ = 0, left_to_game_ = 0;
    uint32_t identify_components_ = 0;
    std::map<std::tuple<uint32_t, std::string, uint32_t>, std::set<uint32_t>> element_offsets_;
};
}
