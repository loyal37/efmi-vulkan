// The character backend's own types, and what its parts call of each other. The parts of what was one
// file, character_backend.cpp (docs/architecture-review-2.1.md, P4-2):
//
//   character_backend.cpp             the backend of a device: the folders and packages it is configured
//                                     with, the device's setup, creation and stop, a pipeline retired, a
//                                     pass ended, a submission, a presentation, the module's table
//   character_backend_index.cpp       the device's index of the packages' sections, the hash of a draw's
//                                     index region, the draw handed to the package it matches
//   character_backend_twins.cpp       the application's pipelines without the character dither (D-133)
//   character_backend_generation.cpp  a package's generation: its load, its activation, the worker with
//                                     the mesh and the snapshot batches, its notes, its stop, the Present
//                                     of a generation of its own
//   character_backend_mesh.cpp        a package's mesh on the device
//   character_backend_identity.cpp    the object a matched draw belongs to and its merged skeleton
//   character_backend_materials.cpp   the frame gate, texture queries, a fragment shader's material
//                                     rules and plans, the images of a draw
//   character_backend_record.cpp      a matched draw recorded as the Mod's
//   character_backend_folder.cpp      the folder generation: its load, its Present, its F10 exchange
//   character_backend_controller.cpp  one package's generations and their own F10 exchange
//   character_backend_status.cpp      the status
//
// Included by these files only; the public interface is character_backend.h. A member function a class
// only declares here is defined, with its comment, in the part that has its concern.
#pragma once
#include "character_backend.h"
#include "../../frontend/gpu/dither_kills.h"
#include "character_module.h"
#include "command_owner.h"
#include "draw_snapshots.h"
#include "variant_cache.h"
#include "character_package.h"
#include "character_program.h"
#include "character_present.h"
#include "texture_residency.h"
#include "character_materials.h"
#include "../../frontend/gpu/source_registers.h"
#include "../../frontend/gpu/texture_roles.h"
#include "texture_identity.h"
#include "translucency.h"
#include "bone_merge.h"
#include <functional>
#include "merged_skeleton.h"
#include "state.h"
#include "work.h"
#include "json.h"
#include "../../frontend/assets/resource_cache.h"

#include <algorithm>
#include <filesystem>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <tuple>
#include <set>
#include <future>
#include <fstream>

namespace efvk {
// What the one file kept in its unnamed namespace and more than one part uses now. A named namespace
// used the same way, so that a type is the same type in every part (CharacterCommand is found by
// dynamic_cast) and a function or variable exists once.
namespace character_internal {}
using namespace character_internal;
namespace character_internal {

// The configured folders and packages (character_backend.cpp).
bool is_mod_ini(const std::string& path);
std::filesystem::path module_folder();
std::filesystem::path absolute_folder(const std::string& path);
std::filesystem::path mods_folder();
CharacterProgram::Source mod_source(const std::string& ini);
std::vector<std::string> configured_packages(std::string& reason);
constexpr size_t kMaxPackages = 4096; // the folder generation's bound (D-141)

// Values of the core this follows (SpatialIdentification.ini): cell size of the position hash, base
// radius in cells per 1/120 s, and the number of distinct components that identifies the object.
constexpr float kCellSize = 0.125f;
constexpr float kBaseRadius = 3.0f;
constexpr uint32_t kIdentifyingComponents = 4;
// A new object's draws that would be the game's mesh before any of them was the Mod's are skipped for
// at most this many presents (half a second at 60): the Core's identification and a Mod's registration
// with the Core's API take frames the original shows nothing of, the game recording frames ahead.
constexpr uint64_t kFreshPresents = 30;
constexpr uint64_t kExpiryFrames = 4;
// Two clocks. The character's frame is one recording of the command buffer its draws are in: the
// game records all draws of a character for one frame into one command buffer, and it starts on
// the next frame before the previous one is presented, so a present is no boundary while
// recording. Presents after which something was drawn are counted too; they pace the periodic work
// and, together with time, the expiry: with frame generation presents come between the game's
// frames, and the game leaves the character out for some frames of a dash, so an identity is given
// up only after the core's two counted presents and this much time without a draw.
// An object not drawn this long (off screen, a cut) is forgotten: g_cfg.m2_identity_expiry_ms, 10 s by default (0.5 s until 1.7.0, D-100).
constexpr size_t kIdentities = 16;
constexpr uint32_t kArenaEntries = 256, kArenaSlices = 64, kArenaMinSlices = 8;
// Snapshot batches of a package: prepared ahead by its loader, and at most in all (frames in flight
// times the recordings of a frame that draw it, with a margin).
constexpr size_t kBatchesAhead = 8, kBatchesMax = 32;

// The INI's clock (character_backend_generation.cpp).
float ini_seconds();

enum Counter : uint32_t {
    C_MATCHED, C_REPLACED, C_ITEMS, C_HIDDEN, C_IDENTIFYING, C_INCOMPLETE, C_PENDING,
    C_STATE, C_INSTANCES, C_SET3, C_PIPELINE, C_VERTEX, C_VARIANT, C_BINDINGS,
    C_ENTRY, C_BONES, C_IMPORT, C_EXHAUSTED, C_IDENTITIES, C_SET3_RESTORED,
    C_MATERIAL, C_IMAGE, C_IMAGE_PENDING, C_MATERIAL_ITEMS, C_COMPAT_ITEMS, C_DERIVED, C_INHERITED,
    C_IDENTITY_NEW, C_IDENTITY_EXPIRED, C_TEXTURED, C_KEPT, C_OWNED, C_MEMORY, C_GATED, C_GATED_FRAMES, C_SNAPSHOT_COPIES, C_IDENTITY_ADOPTED,
    C_PREPARING, C_TRANSLUCENT, C_BONE_JOBS, C_BONE_RING, C_BATCH_MADE, C_HELD, C_ORIGINAL_FIRST, C_BONES_UNAVAILABLE, C_ACTIVATION, C_OWN_MESH, C_BESIDE, C_INSTANCED, C_COMPUTED, C_COUNT
};
const char* const kCounterNames[C_COUNT] = {
    "matched", "replaced", "items", "hidden", "identifying", "incomplete", "pending",
    "refused_state", "refused_instances", "refused_set3", "refused_pipeline", "refused_vertex", "refused_variant",
    "refused_bindings", "unreadable_entry", "unreadable_bones", "import_rejected", "exhausted", "identities_full",
    "restored_set3", "refused_material", "refused_image", "image_pending", "material_items", "dimension_zero_items",
    "material_derived", "material_inherited", "identities_created", "identities_expired", "textured",
    "kept_original", "owned_streams", "waiting_memory", "gated", "gated_frames", "snapshot_copies", "identities_adopted", "preparing",
    "translucent", "bone_jobs", "bone_ring_busy", "batches_made_at_draw", "held_before_shown", "original_before_shown",
    "bones_unavailable", "activation_failed", "own_mesh", "drawn_beside", "instanced_draws", "components_handed"
};
// What the layer assumes of the game's character draws (review L-07, D-183): the instance entry at set 2 binding 0
// in mapped memory, a pipeline the tracker knows, a primary command buffer in a classic render pass, the game's
// vertex and index bindings tracked, a vertex input and a vertex shader the variants can take. For the process: the
// character draws that met them and the refusals for them, and the first such refusal.
struct GameShape {
    std::atomic<uint64_t> fitting{0}, unfit{0};
    std::atomic<bool> told{false};
    std::mutex mx;
    std::string first; // under mx
};
GameShape& game_shape();
// Where a matched draw's time goes (D-099): microseconds per stage of record(), in the status.
enum Stage : uint32_t { S_LOCK, S_IDENTIFY, S_BONES, S_PLAN, S_MATERIALS, S_VARIANTS, S_OWNER, S_APPEND, S_SNAPSHOT, S_EMIT, S_COUNT };
const char* const kStageNames[S_COUNT] = {"lock", "identify", "bones", "plan", "materials", "variants", "owner", "append", "snapshot", "emit"};

struct GpuMesh {
    VkDevice device;
    SnapshotDispatch d;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool device_local = false;
    MeshMemory placement = MeshMemory::Host;
    GpuMesh(VkDevice dev, const SnapshotDispatch& dispatch) : device(dev), d(dispatch) {}
    ~GpuMesh()
    {
        if (buffer) d.DestroyBuffer(device, buffer, nullptr);
        if (memory) d.FreeMemory(device, memory, nullptr);
    }
    bool create(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, std::string& reason);
    bool copy(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, VkDeviceSize size,
              std::string& reason);
};

struct Identity {
    bool used = false, custom = false;
    int32_t cell[3] = {};
    uint64_t first_frame = 0, last_frame = 0, update_frame = 0; // in frames of the character
    uint64_t first_present = 0;      // counted presents when the identity appeared
    uint64_t last_present = 0;       // counted presents at the last draw
    uint64_t last_us = 0;            // host time of the last draw
    uint64_t id = 0, generation = 0; // generation changes whenever the master's bytes change
    uint64_t seen = 0;               // components met since this identity appeared
    MergedSkeleton skeleton;
    std::vector<uint8_t> coverage;   // per component: bit 0 current pose imported, bit 1 previous
    // The merged skeleton on the device (D-117, D-127), and whether this recording's draws of the object
    // import on the device.
    std::shared_ptr<BoneRing> ring;
    bool gpu_frame = false;
    bool shown = false;              // a draw of this object was the Mod's (kFreshPresents)
    // Per component, its latest import on the device as a recording carries it (the game's bone range,
    // the rows it read and that recording); empty where no recording carried one.
    std::vector<BoneJob> bone_sources;
};

// The index hashing's counters (character_backend_index.cpp).
extern std::atomic<uint64_t> g_hash_full, g_hash_bytes, g_hash_new, g_hash_gen, g_hash_quick, g_hash_due, g_quick_reads, g_quick_skipped;
extern std::atomic<uint64_t> g_quick_read_us;
// The plans this thread ran for replaced draws (character_backend_index.cpp, D-173).
extern thread_local uint64_t t_draw_plans;
// Meshes uploaded and kept (character_backend_generation.cpp).
extern std::atomic<uint64_t> g_mesh_uploads, g_meshes_kept;
// The discard gates' state and the pipelines with the character dither (character_backend_twins.cpp).
extern std::atomic<bool> g_dither_off, g_dither_gated;
extern std::shared_mutex g_dither_mx;
extern std::unordered_map<uint32_t, bool> g_dither_pipelines;
bool pipeline_dither(uint32_t id, const GraphicsRecipe& recipe);
// The variant identity of a pipeline's twin without the dither (captures take 1 to 3 << 40).
inline uint64_t twin_identity(uint32_t pipeline_id) { return (uint64_t(0x40) << 40) | pipeline_id; }
// The twin's fragment patch: the application's vertex stage and images, the dither branch steered away.
inline FragmentPatch twin_patch() { FragmentPatch patch; patch.keep_vertex = true; patch.no_dither = true; return patch; }
// The same patch the other way round, for the state the key turns on next (built behind requested work).
inline FragmentPatch other_state(FragmentPatch patch) { patch.no_dither = !patch.no_dither; return patch; }
struct BoneSite { bool ok = false; uint32_t set = 0, binding = 0; };
using StaticMaterial = CharacterMaterial;
// Rules of a fragment shader the package has no profile for, read off its module by the DX11
// register rule (D-076), prepared like a profile.
struct DerivedProfile {
    uint32_t crc = 0;
    std::vector<uint32_t> slots; // the material slots the rules were read for
    std::vector<uint32_t> code;
    std::vector<CharacterMaterialRule> rules;
    std::vector<StaticMaterial> plans;
};
// The character backend's part of a command buffer. The counter outlives the backend: a command
// may still be sealed after the code it came from was exchanged.
struct CharacterCommand : CommandOwner {
    std::shared_ptr<const void> generation;
    std::shared_ptr<DrawSnapshots> snapshots;
    std::shared_ptr<const void> keep; // the mesh
    std::vector<std::shared_ptr<const PipelineVariant>> variants;
    std::map<uint32_t, std::shared_ptr<DrawSnapshots>> material_snapshots;
    std::shared_ptr<std::atomic<uint64_t>> batches;
    // Owned GPU buffers the recording binds as vertex streams (D-087).
    std::vector<std::shared_ptr<void>> owned;
    // The snapshots of merged skeletons the recording's draws read (D-127), one per object, held until
    // the recording was reset after completion.
    std::vector<std::pair<std::shared_ptr<BoneRing>, uint32_t>> rings;
    // The object's snapshot for this recording, taken on its first draw; UINT32_MAX when the ring has none.
    uint32_t snapshot(const std::shared_ptr<BoneRing>& ring)
    {
        for (const auto& held : rings) if (held.first == ring) return held.second;
        const uint32_t slot = ring->acquire();
        if (slot != UINT32_MAX) rings.emplace_back(ring, slot);
        return slot;
    }
    // The imports this recording carries in front of it (D-127): one per object and component it drew, a
    // later import of a component replacing the earlier (a lower LOD replacing a higher), as the CPU
    // import does; and imports it took from a recording not yet submitted, for a component its draws read
    // that the object's skeleton never had. Kept until the recording is reset (a recording submitted again
    // imports again); `recording` tells this recording's own from those it took.
    std::vector<BoneJob> bone_jobs;
    std::shared_ptr<BoneRecording> recording = std::make_shared<BoneRecording>();
    bool has_bone_job(const BoneRing* ring, uint32_t component) const
    {
        for (const auto& have : bone_jobs)
            if (have.ring.get() == ring && have.component == component) return true;
        return false;
    }
    void add_bone_job(const BoneJob& job)
    {
        for (auto& have : bone_jobs)
            if (have.ring == job.ring && have.component == job.component) {
                // An import of the recording's own replaces any earlier one; one taken from another
                // recording never replaces one.
                if (job.recording == recording) have = job;
                return;
            }
        bone_jobs.push_back(job);
    }
    uint32_t idle_recordings = 0; // resets in a row that found it unused
    // A command buffer freed without a reset: nothing of its recording runs any more.
    ~CharacterCommand() override
    {
        recording->next();
        for (auto& held : rings) held.first->release(held.second);
    }
    void reset_after_completion() override
    {
        idle_recordings = used ? 0 : idle_recordings + 1;
        used = false; owned.clear();
        bone_jobs.clear(); recording->next();
        for (auto& held : rings) held.first->release(held.second);
        rings.clear();
        variants.clear(); snapshots->recycle_completed();
        for (auto& arena : material_snapshots) arena.second->recycle_completed();
    }
    bool seal() override
    {
        if (snapshots->sealed()) return false;
        snapshots->seal();
        for (auto& arena : material_snapshots) arena.second->seal();
        ++*batches;
        return true;
    }
};
// This code's owner in a recording (a package's snapshot batch, the twins a recording bound), as owner_for,
// except that another owner of this code stays while its backend keeps using the recording: every reset clears
// `used`, so dropping the others' unused owners at the first draw destroyed their arenas and had their workers
// make new ones for every recording (D-099, arcane 142 us per draw in the 1.6.8 session; BUG-089, the twins'
// bind before the first draw of a recording). A package's owner goes after kIdleRecordings resets without use
// (its generation was exchanged); any other stays, as D3D11 keeps what a pending command list uses whatever
// else is bound.
constexpr uint32_t kIdleRecordings = 8;
template <class Make>
std::shared_ptr<CommandOwner> character_owner_for(std::vector<std::shared_ptr<CommandOwner>>& owners, const void* backend, Make make)
{
    std::shared_ptr<CommandOwner> mine;
    for (auto it = owners.begin(); it != owners.end();) {
        if (*it && (*it)->backend == backend) { mine = *it; ++it; continue; }
        const auto* other = *it ? dynamic_cast<const CharacterCommand*>(it->get()) : nullptr;
        if (!*it || (other && !other->used && other->idle_recordings >= kIdleRecordings)) it = owners.erase(it);
        else ++it;
    }
    if (!mine && (mine = make())) owners.push_back(mine);
    return mine;
}
} // namespace character_internal

class CharacterGeneration {
public:
    Device* device;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VariantDispatch draw;
    SnapshotDispatch storage;
    CharacterPackage package;
    std::unique_ptr<CharacterProgram> program;
    // Owned GPU execution of the generation's Present work; destroyed before the program. A folder
    // generation's packages share one (D-131).
    std::shared_ptr<OwnedPresenter> presenter;
    // A package of a folder generation (D-131): the shared generation's Present, saves and F10 are the
    // folder's; its plans run one at a time with the other packages' under the folder's lock, as the
    // original's one context runs them.
    std::shared_ptr<std::mutex> plan_mx;
    bool folder_member() const { return plan_mx != nullptr; }
    // Owned GPU work inside replaced draws (P2, D-087) stopped after its first failure.
    bool draw_work_failed = false;
    // Owned work of replaced draws waits for the application's submission (set under mod_mx). Only
    // then does the submission take mod_mx (BUG-058): the Present path holds it while the driver
    // may wait for the GPU, and the GPU may wait for this very submission.
    std::atomic<bool> draw_work_waiting{false};
    // When the Present last looked for changed persistent variables (under mod_mx), as the original's
    // gSettingsSaveTime: the first save comes an interval after the generation started.
    uint64_t save_checked_us = host_time_us();
    std::atomic<bool> save_requested{false};
    std::vector<BoneComponent> ranges;
    uint32_t bone_bytes = 0;
    std::shared_ptr<VariantCache> cache; // the device's (D-103), shared by every package
    TextureResidency* textures = nullptr;              // the device's (D-094)
    BoneMerge* bones_gpu = nullptr;                    // the device's merged-skeleton dispatches (D-117)
    std::shared_ptr<BoneRemaps> bone_remaps;           // this package's LOD remap tables on the device
    bool bone_remaps_failed = false, bone_ring_failed = false;
    std::vector<TextureResidency::ImageId> image_ids;  // per package image
    std::atomic<uint64_t> batches_made{0};             // snapshot batches made, by the worker or at a draw (BUG-089)
    bool images_forgotten = false;                     // the declarations given back at stop
    std::vector<std::vector<StaticMaterial>> materials; // profile x package draw, prepared at setup
    std::unordered_map<uint32_t, uint32_t> material_profiles; // pipeline generation -> exact profile
    std::vector<uint32_t> material_slots;                          // DX11 pixel slots the package binds
    // Under the mod lock: the DX11 registers of each pipeline's fragment module (D-076), for texture
    // queries; `readable` false when the register rule could not read the module.
    struct FragmentRegisters { bool readable = false; std::vector<efmi::gpu::SourceRegister> list; };
    std::unordered_map<uint32_t, FragmentRegisters> fragment_registers;
    // Under the mod lock: the image samples of each pipeline's fragment module, for RabbitFX's
    // texture roles (D-082); `readable` false when the module is not read.
    struct FragmentSamples { bool readable = false; std::vector<efmi::gpu::RoleSample> list; uint32_t sets = 0; };
    std::unordered_map<uint32_t, FragmentSamples> fragment_samples;
    // Under the mod lock: the roles a pipeline's samples gave with the images of its referenced sets,
    // by the pipeline and those sets' identity and write generation (a write changes the generation),
    // so a draw with the same textures as an earlier one does not classify them again.
    struct RoleKey {
        uint32_t pipe = 0;
        uint64_t identity[kMaxSets] = {}, generation[kMaxSets] = {};
        bool operator==(const RoleKey& o) const
        {
            return pipe == o.pipe && std::equal(identity, identity + kMaxSets, o.identity) && std::equal(generation, generation + kMaxSets, o.generation);
        }
    };
    struct RoleKeyHash {
        size_t operator()(const RoleKey& k) const
        {
            uint64_t h = k.pipe * 0x9E3779B97F4A7C15ull;
            for (uint32_t i = 0; i < kMaxSets; ++i) h = (h ^ k.identity[i]) * 0x100000001B3ull ^ k.generation[i];
            return size_t(h ^ (h >> 29));
        }
    };
    struct RoleAnswer { int32_t roles[3] = {-1, -1, -1}; };
    std::unordered_map<RoleKey, RoleAnswer, RoleKeyHash> role_cache;
    // Under the mod lock: what the original Core reads off each pipeline's vertex shader (D-079):
    // the instance-config cb slot and whether the bones come from the rolling vs-t0 buffer; and the
    // DX11 registers of each graphics stage (vertex, hull, domain, geometry, pixel) for a Mod's
    // declaration queries (D-124), null where the stage is absent or the rule refused its SPIR-V.
    struct SourceFacts {
        int32_t vs_config_slot = -1; bool rolling_bones = false;
        std::array<std::shared_ptr<const std::vector<efmi::gpu::SourceRegister>>, 5> registers;
    };
    std::unordered_map<uint32_t, SourceFacts> source_facts;
    uint64_t facts_unknown = 0;
    uint64_t texture_queries = 0, texture_unknown = 0;
    uint64_t role_queries = 0, role_draws = 0; // RabbitFX role queries, and those that found a diffuse map
    const char* texture_reason = nullptr; // why the latest unknown answer was unknown
    std::unordered_map<uint32_t, uint32_t> derived_for_pipeline;   // pipeline generation -> derived profile or none
    std::vector<std::unique_ptr<DerivedProfile>> derived_profiles;
    uint32_t rule_mismatches = 0;                                  // package profiles the register rule disagrees with
    std::set<uint32_t> image_counts{0};
    // EFMITTL (D-113): the device's layers and captures, when the generation loaded the library with
    // its bridge; under the mod lock, which game pipelines have the lit character vertex shader.
    Translucency* translucency = nullptr;
    struct LibraryHooks { uint32_t vertex = 0; bool pixel = false; int32_t colour_slot = -1; bool narrow_first = false; };
    std::unordered_map<uint32_t, LibraryHooks> library_hook_facts;
    std::vector<std::shared_ptr<const PipelineVariant>> scratch_captures[kCaptureKinds];
    std::vector<uint8_t> capturing, capture_t4;               // per list draw: captured; its ps-t4 neutral
    std::vector<SnapshotImage> capture_masks;                 // per list draw: the opacity mask, if any
    std::pair<uint32_t, uint32_t> capture_t4_binding{0, 0};   // the draw's fragment module's DX11 t4
    uint32_t capture_sets = 0;                                // the draw pipeline's sets below the private one
    std::shared_ptr<void> capture_layers;                     // the layer set of the draw's pass
    struct WaitingCapture { uint32_t draw, kind; VertexInputRecipe vertex; std::shared_ptr<const GraphicsRecipe> recipe; FragmentPatch patch; };
    std::vector<WaitingCapture> waiting_captures;
    std::vector<SnapshotImage> scratch_capture_views;
    std::vector<TranslucentCapture> scratch_pending_captures;
    // How a draw's bones reach its private set: the CPU master copied into the arena, or the device's
    // ring slot bound (D-117).
    using AppendBones = std::function<bool(DrawSnapshots&, const std::vector<SnapshotImage>&, SnapshotSlot&)>;
    bool capture(CmdBuf* cb, const DrawRec& r, uint32_t own, const CharacterDraw& d, uint32_t i, const SnapshotSlot& colour,
                 const DrawGeometry& geometry, const IndexedDraw& args, const AppendBones& append_bones, CharacterCommand* owner);
    const LibraryHooks& library_hooks(const PipeInfo& pipe);
    std::atomic<bool> enabled{false}, ready{false};
    std::atomic<uint64_t> frame{1};
    std::atomic<uint64_t> presents{0}; // every presentation, the INI's frame counter (BUG-036)
    std::shared_ptr<std::atomic<uint64_t>> batches = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<const void> generation_key = std::make_shared<int>(0);
    std::atomic<uint64_t> counters[C_COUNT] = {};
    std::atomic<uint64_t> stage_us[S_COUNT] = {};
    std::atomic<float> frame_seconds{1.0f / 60};
    uint64_t last_frame_us = 0;

    // Resources prepared off the draw path.
    std::mutex pool_mx;
    std::condition_variable wake;
    bool stopped = false;
    std::thread worker;
    // The worker ended without the package on the device (its mesh, its first batch) or after it (a later
    // batch): the package's draws are the game's again, with the reason said, and the worker is started
    // again at `retry_us` (1 s later, then 2, 4 ... 32). Before, `activated` stayed set, `ready` never came,
    // and every matched draw was skipped: the character was neither the Mod's nor the game's until the
    // game was started again (docs/architecture-review-2.1.md, L-01).
    std::atomic<bool> worker_failed{false};
    std::atomic<uint64_t> retry_us{0};
    uint32_t worker_failures = 0; // under pool_mx
    void worker_ended();
    void retry_activation();
    std::shared_ptr<const GpuMesh> mesh;
    std::deque<std::shared_ptr<CharacterCommand>> available;
    std::vector<std::weak_ptr<CharacterCommand>> all;
    std::string last_reason;
    // The distinct reasons this generation logged (bounded): the status keeps only the latest, which
    // a refusal repeated every frame hides the first one behind.
    std::vector<std::string> logged_reasons;
    bool target_size_logged = false;

    // The mod's state, under the mod lock: one matched draw at a time.
    std::mutex mod_mx;
    uint32_t lod = 0;
    // Per section whose lists compute the component (D-182): the component and LOD they handed the Core at its last
    // draw, none before the first.
    std::vector<std::pair<uint32_t, uint32_t>> handed;
    std::array<Identity, kIdentities> identities;
    uint64_t next_identity = 1;
    uint32_t identity_logs = 0;
    std::unordered_map<uint32_t, BoneSite> sites;
    std::vector<uint8_t> current, previous;
    // The per-draw containers of record(), kept for their capacity: the allocations of fresh ones
    // were a sixth of the recording in a profile (D-108). record() holds mod_mx.
    std::vector<std::shared_ptr<const PipelineVariant>> scratch_variants;
    std::vector<const StaticMaterial*> scratch_material_plans;
    std::vector<std::vector<SnapshotImage>> scratch_material_views;
    std::vector<TextureResidency::ImageId> scratch_image_ids;
    std::vector<TextureResidency::Binding> scratch_image_bindings;
    // Capacity may survive a draw, ownership may not (BUG-042). In-flight commands have their own
    // references. Clear on every return and exception, including partially prepared transactions.
    struct ScratchScope {
        CharacterGeneration& owner;
        ~ScratchScope() {
            owner.scratch_variants.clear();
            owner.scratch_image_bindings.clear();
            for (auto& views : owner.scratch_material_views) views.clear();
        }
    };
    std::vector<uint32_t> scratch_restore_offsets;
    std::vector<BoundRange> scratch_bone_ranges;
    std::vector<DrawGeometry> scratch_geometry;
    std::vector<VariantDraw> scratch_items;
    DrawGeometry scratch_original;
    // The draw record() is in, for its texture queries (twenty per draw): its pipeline, the
    // pipeline's fragment registers and the last descriptor set looked up. Reset when record() returns.
    struct DrawLookups {
        VkPipeline pipeline = VK_NULL_HANDLE; const PipeInfo* pipe = nullptr;
        const void* registers = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE; uint64_t set_id = 0; const DescriptorSetInfo* contents = nullptr;
        // The answers of the pixel slot queries already made in this draw (the Core and RabbitFX ask
        // for the same slots again): bit per slot answered, and the answer.
        uint64_t answered[2] = {0, 0};
        efmi::frontend::RuntimeTexture answers[128];
    } lookups;
    // The package's frame gate (D-097): a frame replaces draws only while every image and pipeline
    // variant the package's draws needed in the frame before is in place, and the first bind that is
    // not closes it for the rest of the frame; so the passes of one object never mix the Mod's mesh
    // with the game's, and a texture on its way costs the package whole frames, never parts of one.
    uint64_t gate_frame = 0;
    bool gate_open = true, frame_variants_pending = false, last_variants_pending = false;
    std::vector<TextureResidency::ImageId> frame_images, last_frame_images; // bound this frame, the frame before
    void begin_gate_frame(uint64_t present);
    bool gate_logged = false;
    void frame_image(TextureResidency::ImageId id)
    {
        if (std::find(frame_images.begin(), frame_images.end(), id) == frame_images.end()) frame_images.push_back(id);
    }
    bool gated() { ++counters[C_GATED]; return false; }

    // `lazy`: one of several packages (a directory). Its mesh, arenas and images go to the GPU only
    // when one of its sections first matches a draw, so characters not on screen take no memory there.
    CharacterGeneration(Device* dev, const VkPhysicalDeviceProperties& p, const VkPhysicalDeviceMemoryProperties& m,
                        std::string path = g_cfg.m2_package, bool deferred = false, TextureResidency* residency = nullptr,
                        std::shared_ptr<VariantCache> variant_cache = nullptr, Translucency* layers = nullptr, BoneMerge* bones = nullptr)
        : device(dev), properties(p), memory(m), translucency(layers), textures(residency), variants(std::move(variant_cache)),
          package_path(std::move(path)), lazy(deferred) { bones_gpu = bones; }
    std::shared_ptr<VariantCache> variants; // the device's (D-103)
    std::string package_path;
    bool lazy = false;
    std::atomic<bool> activated{false};
    ~CharacterGeneration() { stop(); }

    void note(const std::string& reason);
    void show_gaps();
    // A fragment shader without a profile of its own keeps the application's images; logged once per
    // pipeline and slot set (derive caches the answer).
    uint32_t refuse_profile(const std::string& reason, uint32_t result) { note(reason); logf("%s", reason.c_str()); return result; }
    bool refuse(Counter counter, const std::string& reason) { ++counters[counter]; note(reason); return false; }
    // A refusal for what the layer assumes of the game's character draws (GameShape, D-183): when none of the
    // process's character draws met those assumptions, told once that the game is not the one the layer knows.
    bool refuse_shape(Counter counter, const std::string& reason);

    bool init();
    bool init_adopted(std::unique_ptr<CharacterProgram> adopted, CharacterPackage made, std::shared_ptr<OwnedPresenter> shared,
                      std::shared_ptr<std::mutex> lock);
    bool init_device();

    // Caller holds mod_mx, or init runs alone. The GPU side of the generation: images demanded, mesh
    // and arenas prepared by the worker. Once.
    // What activating takes in device-local memory: the images (at most the image budget), the mesh
    // and the snapshot batches made at once (D-092). Set when the package is configured.
    uint64_t activation_bytes = 0;
    std::atomic<uint64_t> mesh_upload_us{0}; // the mesh read from its files into device memory, once activated
    std::shared_ptr<const GpuMesh> kept_mesh;  // the live generation's, when no mesh file changed (keep_mesh)
    CharacterGeneration* predecessor = nullptr; // during init only: the live generation this one succeeds
    void keep_mesh(CharacterGeneration& live);
    uint64_t next_memory_check = 0, memory_logged_us = 0;
    static bool device_local_free(Device* device, uint64_t& free_bytes, uint64_t& heap_bytes);
    bool memory_allows_activation();
    void activate();
    void abandon();

    // Once, by whoever asks first; a caller meanwhile waits until it is done. A package is stopped by its
    // controller (its Mod gone, the device going) and with the folder generation it belongs to, on threads
    // of their own: 2.0.7 joined its worker from two of them at once and the second join's error ended the
    // game. The original releases each object once, its reference count deciding when (D3D11 runtime).
    std::once_flag stop_once;
    void stop() { std::call_once(stop_once, [this] { stop_now(); }); }
    void stop_now();
    void prepare();
    std::shared_ptr<CharacterCommand> make_batch(std::string& why);
    std::shared_ptr<CharacterCommand> command();

    bool init_materials();
    // The variant a (pipeline, draw, material plan) resolved to, kept per generation (D-106): the
    // variant cache's key building and lookup cost 5 us per replaced draw. Cleared with the plans.
    std::map<std::tuple<uint32_t, uint32_t, const StaticMaterial*, bool>, std::shared_ptr<const PipelineVariant>> variant_lookup;
    std::vector<FragmentPatch> scratch_patches; // a draw's material patch without the dither (D-133)
    bool refresh_materials();
    // `origin`: 0 the package's own profile or nothing to bind, 1 rules derived from the module, 2 the application's images.
    bool material_draws(const PipeInfo&, uint32_t section, const DrawRec&, uint64_t now, const CharacterList&,
                        std::vector<const StaticMaterial*>&, std::vector<std::vector<SnapshotImage>>&, uint32_t& origin);
    bool rule_rules(const std::vector<uint32_t>& code, std::vector<CharacterMaterialRule>& rules, std::string& reason) const;
    bool pixel_texture(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why);
    bool pixel_texture_uncached(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string&);
    bool bound_range(const SetBind& bind, uint32_t slot, uint32_t binding, const uint32_t* dyn, uint32_t dyn_count, BoundRange& out) const;
    const ImageInfo* bound_image(const DrawRec& r, uint32_t set_index, uint32_t binding, bool& empty, const char*& why,
                                 const ImageViewInfo** bound_view = nullptr);
    bool texture_roles(const DrawRec& r, int32_t roles[3], std::string& reason);
    uint32_t derive(const PipeInfo& pipe);
    Identity* identify(const InstanceEntry& entry, uint64_t now, uint64_t present);
    void acquire_ring(Identity& identity);
    bool ensure_remaps();
    BoneJob scratch_bone_job;
    // `lod`: which of the component's remaps the job reads (BUG-093).
    bool prepare_bone_job(Identity& identity, CharacterCommand& owner, uint32_t component_index, const CharacterComponent& component,
                          bool remapped, size_t needed, const BoundRange& bones, const uint32_t offsets[2], uint8_t dynamic, uint32_t lod);
    BoneSite site_of(const PipeInfo& pipe);
    const SourceFacts& facts_of(const PipeInfo& pipe, const BoneSite& site);
    static bool vertex_stride(const PipeInfo& pipe, unsigned slot, uint32_t& stride, std::string& why);
    // The draw matched `section_index` of the package (its hash, count and first index checked by the
    // device's index): recorded as the Mod's, hidden, or left to the game.
    // `instance`: the instance of an instanced draw this call takes (record_instances), whose entry lies that many
    // entries after the first; UINT32_MAX for a draw of one instance.
    bool record(CmdBuf* cb, const DrawRec& r, uint32_t section_index, uint32_t instance = UINT32_MAX);
    bool plan_draw(const efmi::frontend::RuntimeDraw& request, CharacterList& output, bool& skip, bool& added, std::string& reason,
                   const VertexInputRecipe* vertex, bool* textures);
    void submit_draw_work(VkQueue queue);
    static void render_targets(const PipeInfo& pipe, efmi::frontend::RuntimeDraw& request);
    // The application's draw made again with the Mod's textures (D-083). Caller holds res_mx and
    // mod_mx; `list` is a texture-only list of the program.
    bool record_textures(CmdBuf* cb, const DrawRec& r, const PipeInfo& pipe, uint32_t section, uint64_t present, const CharacterList& list);
    // An instanced draw of a section with a component (P2-4), as the Core's IterateInstances: each instance on its
    // own, with its own instance entry; one the Mod does not take is drawn alone as the application's. False: not
    // decomposed, the application's instanced draw stays.
    bool record_instances(CmdBuf* cb, const DrawRec& r, uint32_t section_index);
    // A section without a component whose lists draw a mesh of their own (D-176): the Mod's buffers and
    // textures in the game's pipeline with its vertex stage as it is. Caller holds res_mx and mod_mx, inside
    // record(); `list` is a list of the program with draws.
    bool record_own_mesh(CmdBuf* cb, const DrawRec& r, const PipeInfo& pipe, uint32_t section, uint64_t present, const CharacterList& list);
};

// character_backend_generation.cpp
std::shared_ptr<CharacterGeneration> generation_create(Device* device, const VkPhysicalDeviceProperties& properties,
                                                   const VkPhysicalDeviceMemoryProperties& memory,
                                                   const std::string& path, bool lazy, TextureResidency* textures,
                                                   std::shared_ptr<VariantCache> variants, Translucency* translucency, BoneMerge* bones,
                                                   CharacterGeneration* predecessor = nullptr);
void generation_stop(CharacterGeneration& backend);
bool ini_present(CharacterProgram& program, OwnedPresenter* presenter, uint64_t frame, float frame_seconds, bool had_draws,
                 PresentWork* present, const efmi::frontend::RuntimeInput& base, uint64_t now, const std::function<void(const std::string&)>& note);
void generation_frame(CharacterGeneration& backend, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& base);
bool generation_usable(CharacterGeneration& backend, std::string& reason);
bool generation_ready(CharacterGeneration& backend);
void generation_enable(CharacterGeneration&, bool);
// character_backend_record.cpp
VkResult generation_submit(CharacterGeneration& backend, VkQueue queue);
bool generation_record(CharacterGeneration& backend, CmdBuf* cb, const DrawRec& r, uint32_t section);
// character_backend_materials.cpp
void generation_retire(CharacterGeneration& backend, uint32_t pipeline_id);
// character_backend_status.cpp
void generation_status(CharacterGeneration&, JsonWriter&);

// D-131 (docs/folder-generation.md): the Mods folder's one INI generation and the packages carved from
// it, one per character Mod it could take. The generation's Present, saves and F10 are the folder's.
struct FolderGeneration {
    Device* device = nullptr;
    std::vector<std::shared_ptr<CharacterGeneration>> packages; // in the folder's order
    std::vector<std::pair<std::string, std::string>> left_out;   // a Mod's INI and why it has no package
    std::shared_ptr<OwnedPresenter> presenter;
    std::shared_ptr<std::mutex> plan_mx = std::make_shared<std::mutex>();
    // The folder's generation itself (D-174), a program without a package: a folder whose INIs have no mesh
    // section presents, saves and reloads all the same. `program` is it once the folder loaded.
    std::shared_ptr<CharacterProgram> generation;
    CharacterProgram* program = nullptr;
    std::atomic<uint64_t> frame{1}, presents{0};
    std::atomic<float> frame_seconds{1.0f / 60};
    uint64_t last_frame_us = 0, save_checked_us = host_time_us();
    std::vector<efmi::frontend::IniRuntime::DiscardGate> gates; // D-133, under plan_mx
    std::future<void> saving;
    bool empty = false; // the folder holds no enabled INI: a valid generation of nothing
    std::vector<std::pair<std::string, std::string>> left_inputs; // inputs of no character Mod the folder was loaded without, and why
    uint32_t gaps_seen = 0; size_t gaps_shown = 0;                 // LayerGap texts logged and shown, on the presenting thread
    void announce() const;
    std::string reason, last_note;
    std::mutex note_mx;
    void note(const std::string& text);
    void save();
    // Off the presenting thread: every package stops, then the owned runtime; the settings once. Once,
    // whoever asks first (the retirement after an exchange, the device going); the others wait for it.
    std::once_flag stop_once;
    void stop() { std::call_once(stop_once, [this] { stop_now(); }); }
    void stop_now();
};

// F10 owns another complete resource generation. Its CPU parsing/validation and
// Vulkan preparation run while the current generation continues to draw. Only
// a Ready candidate takes over; command buffers retain their immutable owners.
class CharacterController {
public:
    std::string path;
    bool lazy = false;
    std::shared_mutex mutex;
    std::shared_ptr<CharacterGeneration> active, staged;
    std::future<std::shared_ptr<CharacterGeneration>> loading;
    std::vector<std::future<void>> retiring;
    uint64_t generation = 0, reloads = 0, rejected = 0, staged_at = 0;
    std::string reload_error;
    bool stopped = false;
    bool reload_taken = false; // an F10 this presentation, for the folder rescan (presenting thread)
    // A package of the folder generation (D-131): it is exchanged with every other at the folder's
    // exchange, never on its own.
    bool folder_member = false;
    std::future<void> stopping; // stop_later's work, which stop() and the destructor wait for
    ~CharacterController() { stop(); }
    void retire(std::shared_ptr<CharacterGeneration> old);
    void stop();
    void stop_later();
    bool revive(std::shared_ptr<CharacterGeneration> package);
    void frame(bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& input);
};

// The module's backend: one controller per configured package. A directory names every live
// package in it (a package with its .runtime.json sidecar); they share the device and the frame,
// each matches only its own sections, and each goes to the GPU on its first match.
class CharacterBackend {
public:
    // The device's texture residency (D-094), shared by every package; stopped after the controllers.
    std::unique_ptr<TextureResidency> textures;
    // EFMITTL's layers, captures and composite (D-113), shared by every package that loaded it.
    std::unique_ptr<Translucency> translucency;
    // The merged skeletons' import by compute dispatches (D-117), shared by every package.
    std::unique_ptr<BoneMerge> bone_merge;
    // Which pipelines draw with the game's copy shader (the library's depth-copy hook), by identity.
    std::mutex copy_mx;
    std::unordered_map<uint32_t, bool> copy_facts;
    // The device's pipeline variants (D-103), shared by every package: a game pipeline with the same
    // vertex layout and material bindings compiles once for all of them. Stopped after the controllers.
    std::shared_ptr<VariantCache> variants;
    // Grown only by an F10 rescan of the Mods folder (BUG-037), within the capacity reserved at
    // creation, so a recording thread's index into it stays valid; a package whose Mod folder is
    // gone is stopped and stays in the list.
    std::vector<std::unique_ptr<CharacterController>> controllers;
    std::string reason; // why there is no package
    // D-131: the folder generation the packages come from; after an F10 its successor loads, waits for
    // the packages the frames draw to come up, and replaces it for every package at one presentation.
    std::shared_ptr<FolderGeneration> folder, folder_staged;
    std::future<std::shared_ptr<FolderGeneration>> folder_loading;
    std::vector<std::future<void>> folder_retiring;
    uint64_t folder_staged_at = 0, folder_reloads = 0, folder_rejected = 0;
    bool folder_reload_again = false; // an F10 pressed while the last one loaded (BUG-069)
    bool host_f10_down = false;       // F10 as the host sampled it while no generation is loaded (L-02)
    uint64_t folder_reload_began_us = 0; // the F10 reload that runs, for its notice (D-137)
    // D-133: the existing pipelines' twins, prepared when a discard gate appears (at the start, or RabbitFX
    // added by F10); presenting thread.
    bool twins_swept = false;
    std::future<void> twins_sweep;
    std::string folder_error;
    void retire_folder(std::shared_ptr<FolderGeneration> old);
    Device* device = nullptr;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    bool directory = false; // the packages are the Mods of a folder (D-078)
    // The variant pipeline cache every package's variants are created through (D-101), loaded from
    // the settings folder when the backend is made and written back when it stops: the variants a
    // character needed in one session compile from the driver's cached form in the next.
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    PFN_vkDestroyPipelineCache destroy_pipeline_cache = nullptr;
    PFN_vkGetPipelineCacheData pipeline_cache_data = nullptr;
    std::filesystem::path pipeline_cache_path;
    uint64_t pipeline_cache_loaded = 0, pipeline_cache_saved = 0;
    void open_pipeline_cache();
    void close_pipeline_cache();
    std::atomic<bool> enabled{true};  // off through the probe: no draw is hashed or asked for
    std::atomic<uint64_t> frames{0}; // presentations with draws, for the hash tables' rehash schedule
    // The recording path's time at the device level (D-099): every indexed draw asked, and of that
    // the index region hashing; the packages' stages are in their own status.
    std::atomic<uint64_t> record_calls{0}, record_us{0}, hash_calls{0}, hash_us{0}, hash_misses{0};
    std::atomic<uint64_t> frame_us{0}, frame_calls{0}; // character_frame: every package's presentation work
    // D-133: the twins of the application's pipelines without the character dither, by pipeline id.
    std::shared_mutex twins_mx;
    std::unordered_map<uint32_t, std::shared_ptr<const PipelineVariant>> twins;
    std::unordered_set<uint32_t> twins_failed;
    std::atomic<uint64_t> twin_binds{0}, twins_prepared{0};
    // The packages' sections by hash (D-096, D-097): a draw's index region is hashed once for every
    // package and looked up here, so only the packages with a section of that hash see the draw.
    // `counts` holds every index count a section matches, to pass over draws no section can match
    // without hashing them; a hash-only section (any count, a hidden game mesh) makes every indexed
    // draw hashed, and acts for an activated package only. Rebuilt when a controller's generation
    // changes (its sections may differ).
    struct Candidate { uint32_t package, section; };
    struct Index {
        std::unordered_map<uint32_t, std::vector<Candidate>> by_hash;
        std::set<uint32_t> counts;
        bool hash_only = false;
        std::vector<uint64_t> generations;
        std::vector<uint8_t> activated; // per controller at the rebuild: hash-only sections count for activated packages
    };
    std::shared_mutex index_mx;
    std::shared_ptr<const Index> index;
    void rebuild_index();
    bool index_stale();
    ~CharacterBackend();
};

// character_backend_folder.cpp
std::shared_ptr<FolderGeneration> folder_create(Device* device, const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceMemoryProperties& memory,
                                                TextureResidency* textures, std::shared_ptr<VariantCache> variants, Translucency* translucency, BoneMerge* bones);
void folder_frame(FolderGeneration& folder, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& base);
void folder_frame_and_exchange(CharacterBackend& b, bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& input);
// character_backend_controller.cpp
void character_rescan(CharacterBackend& b);
// character_backend_index.cpp
bool index_region_hash(Device* device, const DrawRec& r, uint64_t now, const CharacterBackend::Index& index, uint32_t& hash);
// character_backend_twins.cpp
void prepare_twins(CharacterBackend& b);
VkPipeline character_bind(CharacterBackend& b, CmdBuf* cb, VkPipeline pipeline);
void character_created(CharacterBackend& b, VkPipeline pipeline);

} // namespace efvk
