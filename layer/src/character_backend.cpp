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
namespace {
// Directory generation (D-078): a character Mod is named by its INI instead of a package file.
bool is_mod_ini(const std::string& path)
{
    const auto extension = std::filesystem::u8path(path).extension().wstring();
    return !_wcsicmp(extension.c_str(), L".ini");
}
std::filesystem::path module_folder();
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
std::filesystem::path module_folder();
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
}
namespace {
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
constexpr uint64_t kRehashFrames = 1024;
// Fragment shaders without a profile in the package whose rules are derived from the module.
constexpr size_t kDerivedProfiles = 256;

float ini_seconds()
{
    static const uint64_t origin = host_time_us();
    return float((host_time_us() - origin) * 1.0e-6);
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

enum Counter : uint32_t {
    C_MATCHED, C_REPLACED, C_ITEMS, C_HIDDEN, C_IDENTIFYING, C_INCOMPLETE, C_PENDING,
    C_STATE, C_INSTANCES, C_SET3, C_PIPELINE, C_VERTEX, C_VARIANT, C_BINDINGS,
    C_ENTRY, C_BONES, C_IMPORT, C_EXHAUSTED, C_IDENTITIES, C_SET3_RESTORED,
    C_MATERIAL, C_IMAGE, C_IMAGE_PENDING, C_MATERIAL_ITEMS, C_COMPAT_ITEMS, C_DERIVED, C_INHERITED,
    C_IDENTITY_NEW, C_IDENTITY_EXPIRED, C_TEXTURED, C_KEPT, C_OWNED, C_MEMORY, C_GATED, C_GATED_FRAMES, C_SNAPSHOT_COPIES, C_IDENTITY_ADOPTED,
    C_PREPARING, C_TRANSLUCENT, C_BONE_JOBS, C_BONE_RING, C_BATCH_MADE, C_HELD, C_ORIGINAL_FIRST, C_BONES_UNAVAILABLE, C_ACTIVATION, C_COUNT
};
const char* const kCounterNames[C_COUNT] = {
    "matched", "replaced", "items", "hidden", "identifying", "incomplete", "pending",
    "refused_state", "refused_instances", "refused_set3", "refused_pipeline", "refused_vertex", "refused_variant",
    "refused_bindings", "unreadable_entry", "unreadable_bones", "import_rejected", "exhausted", "identities_full",
    "restored_set3", "refused_material", "refused_image", "image_pending", "material_items", "dimension_zero_items",
    "material_derived", "material_inherited", "identities_created", "identities_expired", "textured",
    "kept_original", "owned_streams", "waiting_memory", "gated", "gated_frames", "snapshot_copies", "identities_adopted", "preparing",
    "translucent", "bone_jobs", "bone_ring_busy", "batches_made_at_draw", "held_before_shown", "original_before_shown",
    "bones_unavailable", "activation_failed"
};
// Where a matched draw's time goes (D-099): microseconds per stage of record(), in the status.
enum Stage : uint32_t { S_LOCK, S_IDENTIFY, S_BONES, S_PLAN, S_MATERIALS, S_VARIANTS, S_OWNER, S_APPEND, S_SNAPSHOT, S_EMIT, S_COUNT };
const char* const kStageNames[S_COUNT] = {"lock", "identify", "bones", "plan", "materials", "variants", "owner", "append", "snapshot", "emit"};

// A test's child process may set it: the host-visible device memory taken for a heap of its own of
// 256 MiB, as a card without resizable BAR shows it (BUG-075), so that the copy is exercised here.
bool test_small_bar()
{
    static const bool small = GetEnvironmentVariableW(L"EFMI_VK_TEST_SMALL_BAR", nullptr, 0) != 0;
    return small;
}

VkPhysicalDeviceMemoryProperties as_placed(const VkPhysicalDeviceMemoryProperties& properties)
{
    VkPhysicalDeviceMemoryProperties seen = properties;
    if (!test_small_bar() || seen.memoryHeapCount >= VK_MAX_MEMORY_HEAPS) return seen;
    const uint32_t window = seen.memoryHeapCount++;
    seen.memoryHeaps[window] = {256ull << 20, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    for (uint32_t i = 0; i < seen.memoryTypeCount; ++i)
        if ((seen.memoryTypes[i].propertyFlags & visible) == visible) seen.memoryTypes[i].heapIndex = window;
    return seen;
}

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
    // The package's streams go from their files straight into the mapped memory, as the original reads
    // a buffer file into the resource it creates and keeps no copy (ISSUE-011); where the host does not
    // see the device's memory, into a staging buffer that a copy on the layer's upload queue empties.
    bool create(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, std::string& reason)
    {
        auto good = [&](VkResult result, const char* text) { if (result == VK_SUCCESS) return true; reason = text; return false; };
        VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        // Room behind the last stream: an element may reach past its file's stride (kCharacterOvershoot).
        const size_t padding = 256;
        const size_t bytes = static_cast<size_t>(package.payload_bytes);
        ci.size = bytes + padding; ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (!good(d.CreateBuffer(device, &ci, nullptr, &buffer), "create character mesh buffer")) return false;
        VkMemoryRequirements req; d.GetBufferMemoryRequirements(device, buffer, &req);
        if (req.size > (768ull << 20)) { reason = "character mesh allocation budget exceeded"; return false; }
        // A whole character is read by the device in every pass of every frame: it belongs in device
        // memory. Host memory works, but far slower; it is the fallback, and the status says which.
        const bool can_copy = owner && owner->upload_queue && owner->upload_family != UINT32_MAX && !owner->material_queues.empty();
        const MeshPlacement place = mesh_placement(as_placed(properties), req.memoryTypeBits, can_copy);
        if (place.type == UINT32_MAX) { reason = "character mesh memory unavailable"; return false; }
        placement = place.memory; device_local = placement != MeshMemory::Host;
        if (placement == MeshMemory::Copied) {
            // The upload family writes it, the application's graphics family reads it.
            const uint32_t families[2] = {owner->material_family, owner->upload_family};
            d.DestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE;
            ci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (families[0] != families[1]) { ci.sharingMode = VK_SHARING_MODE_CONCURRENT; ci.queueFamilyIndexCount = 2; ci.pQueueFamilyIndices = families; }
            if (!good(d.CreateBuffer(device, &ci, nullptr, &buffer), "create character mesh buffer")) return false;
            d.GetBufferMemoryRequirements(device, buffer, &req);
            if (!(req.memoryTypeBits & (1u << place.type))) { reason = "character mesh memory type changed with its copy usage"; return false; }
        }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = place.type;
        if (!good(d.AllocateMemory(device, &ai, nullptr, &memory), "allocate character mesh memory") ||
            !good(d.BindBufferMemory(device, buffer, memory, 0), "bind character mesh memory")) return false;
        if (placement == MeshMemory::Copied) return copy(package, properties, owner, bytes + padding, reason);
        void* mapped = nullptr;
        if (!good(d.MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map character mesh memory")) return false;
        const bool read = package.read_payload(static_cast<uint8_t*>(mapped), reason);
        std::memset(static_cast<uint8_t*>(mapped) + bytes, 0, padding);
        d.UnmapMemory(device, memory);
        return read;
    }

#define EFVK_MESH_COPY_FUNCS(X) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
    // The files into a staging buffer in system memory, then one copy on the upload queue (D-103); the
    // worker waits for it, so the mesh is published complete. The staging memory goes right after.
    bool copy(const CharacterPackage& package, const VkPhysicalDeviceMemoryProperties& properties, Device* owner, VkDeviceSize size,
              std::string& reason)
    {
        struct {
#define X(n) PFN_vk##n n = nullptr;
            EFVK_MESH_COPY_FUNCS(X)
#undef X
        } f;
#define X(n) f.n = reinterpret_cast<PFN_vk##n>(owner->owned_gdpa(device, "vk" #n)); if (!f.n) { reason = "missing vk" #n " for the mesh copy"; return false; }
        EFVK_MESH_COPY_FUNCS(X)
#undef X
        VkBuffer stage = VK_NULL_HANDLE; VkDeviceMemory stage_memory = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        auto done = [&](bool result) {
            if (fence) f.DestroyFence(device, fence, nullptr);
            if (pool) f.DestroyCommandPool(device, pool, nullptr);
            if (stage) d.DestroyBuffer(device, stage, nullptr);
            if (stage_memory) d.FreeMemory(device, stage_memory, nullptr);
            return result;
        };
        auto good = [&](VkResult result, const char* text) { if (result == VK_SUCCESS) return true; reason = text; return false; };
        VkBufferCreateInfo si = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; si.size = size; si.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!good(d.CreateBuffer(device, &si, nullptr, &stage), "create character mesh staging buffer")) return done(false);
        VkMemoryRequirements req; d.GetBufferMemoryRequirements(device, stage, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
        ai.memoryTypeIndex = host_memory_type(properties, req.memoryTypeBits, false);
        if (ai.memoryTypeIndex == UINT32_MAX) { reason = "character mesh staging memory unavailable"; return done(false); }
        void* mapped = nullptr;
        if (!good(d.AllocateMemory(device, &ai, nullptr, &stage_memory), "allocate character mesh staging memory") ||
            !good(d.BindBufferMemory(device, stage, stage_memory, 0), "bind character mesh staging memory") ||
            !good(d.MapMemory(device, stage_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map character mesh staging memory")) return done(false);
        const size_t bytes = static_cast<size_t>(package.payload_bytes);
        const bool read = package.read_payload(static_cast<uint8_t*>(mapped), reason);
        std::memset(static_cast<uint8_t*>(mapped) + bytes, 0, static_cast<size_t>(size) - bytes);
        d.UnmapMemory(device, stage_memory);
        if (!read) return done(false);
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pi.queueFamilyIndex = owner->upload_family;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
        if (!good(f.CreateCommandPool(device, &pi, nullptr, &pool), "create character mesh copy pool")) return done(false);
        ci.commandPool = pool;
        if (!good(f.AllocateCommandBuffers(device, &ci, &command), "allocate character mesh copy")) return done(false);
        if (owner->set_loader_data && !good(owner->set_loader_data(device, command), "initialize character mesh copy dispatch")) return done(false);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!good(f.BeginCommandBuffer(command, &bi), "begin character mesh copy")) return done(false);
        const VkBufferCopy region = {0, 0, size};
        f.CmdCopyBuffer(command, stage, buffer, 1, &region);
        // The vertex and index reads of every queue that draws it after the worker saw the fence.
        VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
        f.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             (owner->upload_flags & VK_QUEUE_GRAPHICS_BIT) ? VK_PIPELINE_STAGE_VERTEX_INPUT_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        if (!good(f.EndCommandBuffer(command), "end character mesh copy")) return done(false);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!good(f.CreateFence(device, &fi, nullptr, &fence), "create character mesh copy fence")) return done(false);
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        {
            std::lock_guard<std::mutex> lock(owner->upload_mx);
            submitted = good(f.QueueSubmit(owner->upload_queue, 1, &submit, fence), "submit character mesh copy");
        }
        if (!submitted) return done(false);
        return done(good(f.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for character mesh copy"));
    }
#undef EFVK_MESH_COPY_FUNCS
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

struct IndexKey {
    VkBuffer buffer; uint64_t offset; uint32_t bytes;
    bool operator==(const IndexKey& o) const { return buffer == o.buffer && offset == o.offset && bytes == o.bytes; }
};
struct IndexKeyHash {
    size_t operator()(const IndexKey& k) const
    {
        return std::hash<uint64_t>()(reinterpret_cast<uint64_t>(k.buffer) ^ (k.offset * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.bytes) << 40));
    }
};
struct IndexHash { uint32_t buffer_id = 0; uint64_t gen = 0, next_full = 0; uint32_t hash = 0, quick = 0; uint64_t seen = 0, next_quick = 0; };
// Caller holds res_mx. The index bytes of one draw, hashed once for every package of the device
// (D-097) and then recognized by a short fingerprint; the full hash is repeated now and then because
// mapped pools change unseen. Each recording thread keeps its own table, so no draw waits for another.
// The table holds every region a scene draws (the open world draws thousands per frame, D-099); when
// it is full, the entries not seen for a while go, never the whole table: emptying it made every draw
// hash its whole region again (20 ms per frame in the 1.6.8 session).
constexpr size_t kHashTableEntries = 65536;
constexpr uint64_t kHashTableIdleFrames = 8;
// Full hashes by cause, device-wide (D-099): a region seen first, the buffer's generation changed
// (a copy into the buffer, anywhere in it), the edge fingerprint changed, or the periodic repeat.
std::atomic<uint64_t> g_hash_full{0}, g_hash_bytes{0}, g_hash_new{0}, g_hash_gen{0}, g_hash_quick{0}, g_hash_due{0}, g_quick_reads{0}, g_quick_skipped{0};
std::atomic<uint64_t> g_quick_read_us{0}; // microseconds of the edge fingerprint reads (write-combined memory)
// Meshes put on the device from their files, and those a successor generation kept from the live one.
std::atomic<uint64_t> g_mesh_uploads{0}, g_meshes_kept{0};
// D-133: a ShaderRegex replacement's discard gate is set (RabbitFX's IniParams[172].w = $censor): the
// character dither is not drawn, as the replaced shader skips its discard. Set at each presentation.
std::atomic<bool> g_dither_off{false};
// Whether the Mods have such a gate at all: then both states are made ahead of the key, as the original's
// replaced shader exists from the game's shader creation and the key only flips its branch.
std::atomic<bool> g_dither_gated{false};
// Whether a pipeline's fragment stage has the character dither, by the pipeline's id (read once).
std::shared_mutex g_dither_mx;
std::unordered_map<uint32_t, bool> g_dither_pipelines;
bool pipeline_dither(uint32_t id, const GraphicsRecipe& recipe)
{
    {
        std::shared_lock<std::shared_mutex> lock(g_dither_mx);
        const auto found = g_dither_pipelines.find(id);
        if (found != g_dither_pipelines.end()) return found->second;
    }
    bool dither = false;
    for (const auto& stage : recipe.stages)
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) dither = efmi::gpu::dither_kills(stage.shader->code) > 0;
    std::unique_lock<std::shared_mutex> lock(g_dither_mx);
    if (g_dither_pipelines.size() >= 16384) g_dither_pipelines.clear();
    g_dither_pipelines[id] = dither;
    return dither;
}
// The variant identity of a pipeline's twin without the dither (captures take 1 to 3 << 40).
inline uint64_t twin_identity(uint32_t pipeline_id) { return (uint64_t(0x40) << 40) | pipeline_id; }
// The twin's fragment patch: the application's vertex stage and images, the dither branch steered away.
inline FragmentPatch twin_patch() { FragmentPatch patch; patch.keep_vertex = true; patch.no_dither = true; return patch; }
// The same patch the other way round, for the state the key turns on next (built behind requested work).
inline FragmentPatch other_state(FragmentPatch patch) { patch.no_dither = !patch.no_dither; return patch; }
// A recording that bound twins keeps them until it completed: the application may destroy its pipeline
// meanwhile, and the variant cache retire the twin with it.
struct TwinOwner final : CommandOwner {
    std::vector<std::shared_ptr<const PipelineVariant>> held;
    void reset_after_completion() override { held.clear(); used = false; }
    bool seal() override { return false; }
};
struct IndexHashTable {
    std::unordered_map<IndexKey, IndexHash, IndexKeyHash> hashes;
    std::vector<uint8_t> scratch;
    uint64_t full_hashes = 0, trims = 0;
    void trim(uint64_t now)
    {
        for (auto it = hashes.begin(); it != hashes.end();) {
            if (it->second.seen + kHashTableIdleFrames < now) it = hashes.erase(it);
            else ++it;
        }
        // Still full (one frame draws more regions than the table holds): start over.
        if (hashes.size() >= kHashTableEntries) hashes.clear();
        ++trims;
    }
};
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
// This backend's owner in a recording, as owner_for, except that another backend's owner stays while
// that backend keeps using the recording: every reset clears `used`, so dropping the others' unused
// owners at the first draw destroyed their arenas and had their workers make new ones for every
// recording (D-099, arcane 142 us per draw in the 1.6.8 session). One goes after kIdleRecordings
// resets without use (its generation was exchanged), or when it is not this code's.
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
} // namespace

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
    std::unordered_map<uint32_t, bool> lit_vertex_facts;
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
    // One captured call (D-113): its opacity set and parameter layer, the game's sets and dynamic
    // state of the draw (`own`: the pipeline's sets below the private one), queued after the list.
    // Caller holds the mod lock.
    // How a draw's bones reach its private set: the CPU master copied into the arena, or the device's
    // ring slot bound (D-117).
    using AppendBones = std::function<bool(DrawSnapshots&, const std::vector<SnapshotImage>&, SnapshotSlot&)>;
    bool capture(CmdBuf* cb, const DrawRec& r, uint32_t own, const CharacterDraw& d, uint32_t i, const SnapshotSlot& colour,
                 const DrawGeometry& geometry, const IndexedDraw& args, const AppendBones& append_bones, CharacterCommand* owner)
    {
        uint32_t layer = 0;
        if (!translucency->reserve(cb, layer)) return refuse(C_STATE, "more translucent calls in one pass than the layers' parameters hold");
        std::vector<SnapshotImage> views{capture_masks[i].view ? capture_masks[i] : Translucency::neutral_view(capture_layers),
                                         Translucency::params_view(capture_layers, layer)};
        if (!views[0].view || !views[1].view) return refuse(C_IMAGE, "translucency layers missing");
        auto arena = owner->material_snapshots.find(2);
        if (arena == owner->material_snapshots.end()) return refuse(C_EXHAUSTED, "no two-image arena for the translucent opacity");
        TranslucentCapture c;
        if (!append_bones(*arena->second, views, c.slots[1])) return refuse(C_EXHAUSTED, arena->second->reason);
        c.slots[0] = colour;
        for (uint32_t k = 0; k < kCaptureKinds; ++k) { c.variants[k] = scratch_captures[k][i]; owner->variants.push_back(c.variants[k]); }
        c.geometry = geometry; c.draw = args;
        for (uint32_t s = 0; s < own && s < kMaxSets; ++s) {
            const SetBind& b = r.sets[s];
            TranslucentCapture::Bound bound;
            if (b.set && !b.pushed && !b.unsupported) {
                if (uint64_t(b.dyn_first) + b.dyn_count > cb->dyn.size()) return refuse(C_ENTRY, "translucent capture: dynamic offsets incomplete");
                bound.layout = b.layout_handle; bound.set = b.set;
                bound.dyn.assign(cb->dyn.begin() + b.dyn_first, cb->dyn.begin() + b.dyn_first + b.dyn_count);
            } else if (b.pushed || b.unsupported) return refuse(C_SET3, "translucent capture: a set that cannot be bound again");
            c.sets.push_back(std::move(bound));
        }
        const VkRect2D area = cb->pass.area;
        c.viewport = cb->has_viewport ? cb->viewport : VkViewport{float(area.offset.x), float(area.offset.y), float(area.extent.width), float(area.extent.height), 0, 1};
        c.scissor = cb->has_scissor ? cb->scissor : area;
        c.params[0] = d.alpha; c.params[1] = d.mask_image != kCharacterNone ? 1.0f : 0.0f;
        c.params[2] = float(d.mask_channel); c.params[3] = d.mask_invert ? 1.0f : 0.0f;
        c.params_layer = layer; c.layers = capture_layers;
        scratch_pending_captures.push_back(std::move(c));
        return true;
    }
    bool lit_vertex(const PipeInfo& pipe)
    {
        const auto found = lit_vertex_facts.find(pipe.id);
        if (found != lit_vertex_facts.end()) return found->second;
        bool lit = false;
        if (pipe.recipe)
            for (const auto& stage : pipe.recipe->stages)
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT && stage.shader) lit = lit_character_vertex(stage.shader->code);
        if (lit_vertex_facts.size() >= 8192) lit_vertex_facts.clear();
        return lit_vertex_facts.emplace(pipe.id, lit).first->second;
    }
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
    void worker_ended()
    {
        std::lock_guard<std::mutex> lock(pool_mx);
        if (stopped) return;
        ready = false;
        worker_failures = std::min<uint32_t>(worker_failures + 1, 6);
        retry_us.store(host_time_us() + (500000ull << worker_failures), std::memory_order_relaxed);
        worker_failed.store(true, std::memory_order_release); // the worker's last act
    }
    // From a matched draw while the worker is down: at its time the worker again, with the mesh it had.
    void retry_activation()
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
    void begin_gate_frame(uint64_t present)
    {
        gate_frame = present;
        last_frame_images.swap(frame_images); frame_images.clear();
        last_variants_pending = frame_variants_pending; frame_variants_pending = false;
        gate_open = !last_variants_pending;
        if (gate_open && textures && !textures->ensure(last_frame_images)) {
            // A texture of the frame before is not resident (evicted, demoted, or its load refused):
            // the recording waits for it, as 3DMigoto loads a Mod's texture when it is first bound,
            // rather than drawing the game's mesh meanwhile (the user's bar: a loaded Mod never shows
            // the original again). The frame goes to the game only when the wait failed.
            auto& bindings = scratch_image_bindings;
            textures->bind(last_frame_images, bindings);
            gate_open = textures->ensure(last_frame_images);
            if (!gate_open && !gate_logged) {
                gate_logged = true;
                std::string why;
                for (const auto& b : bindings) if (!b.view) { why = b.failed ? b.reason : "still loading after the wait"; break; }
                logf("character frame gated: textures of %zu images not resident (%s); the game draws this package this frame",
                     last_frame_images.size(), why.c_str());
            }
        }
        if (!gate_open) ++counters[C_GATED_FRAMES];
    }
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

    void note(const std::string& reason)
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
    void show_gaps()
    {
        if (!program) return;
        const std::string mod = std::filesystem::u8path(package_path).parent_path().filename().u8string();
        size_t shown = 0;
        for (const auto& gap : program->gaps()) {
            logf("character package %s: %s", package_path.c_str(), gap.c_str());
            if (shown++ < 3) notice(NoticeLevel::Warning, (package_path + "\n" + gap).c_str(), ("EFMI Vulkan: " + mod + ": " + gap).c_str());
        }
    }
    // A fragment shader without a profile of its own keeps the application's images; logged once per
    // pipeline and slot set (derive caches the answer).
    uint32_t refuse_profile(const std::string& reason, uint32_t result) { note(reason); logf("%s", reason.c_str()); return result; }
    bool refuse(Counter counter, const std::string& reason) { ++counters[counter]; note(reason); return false; }

    bool init()
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
    bool init_adopted(std::unique_ptr<CharacterProgram> adopted, CharacterPackage made, std::shared_ptr<OwnedPresenter> shared,
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
    bool init_device()
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

    // Caller holds mod_mx, or init runs alone. The GPU side of the generation: images demanded, mesh
    // and arenas prepared by the worker. Once.
    // What activating takes in device-local memory: the images (at most the image budget), the mesh
    // and the snapshot batches made at once (D-092). Set when the package is configured.
    uint64_t activation_bytes = 0;
    std::atomic<uint64_t> mesh_upload_us{0}; // the mesh read from its files into device memory, once activated
    std::shared_ptr<const GpuMesh> kept_mesh;  // the live generation's, when no mesh file changed (keep_mesh)
    CharacterGeneration* predecessor = nullptr; // during init only: the live generation this one succeeds

    // Before a successor of `live` activates (F10): the original releases its buffers on a reload and
    // reads their files again at first use; the layer keeps the live mesh when every stream is the
    // same bytes at the same place, as it keeps images by their files (BUG-061). `live`'s package is
    // read under its mod_mx, which its plans hold while they adopt bone streams.
    void keep_mesh(CharacterGeneration& live)
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
    uint64_t next_memory_check = 0, memory_logged_us = 0;
    // The largest device-local heap's budget minus its use (VK_EXT_memory_budget); false without it.
    static bool device_local_free(Device* device, uint64_t& free_bytes, uint64_t& heap_bytes)
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
    bool memory_allows_activation()
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

    void activate()
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
    void abandon()
    {
        enabled = false;
        presenter.reset();
        program.reset();
    }

    // Once, by whoever asks first; a caller meanwhile waits until it is done. A package is stopped by its
    // controller (its Mod gone, the device going) and with the folder generation it belongs to, on threads
    // of their own: 2.0.7 joined its worker from two of them at once and the second join's error ended the
    // game. The original releases each object once, its reference count deciding when (D3D11 runtime).
    std::once_flag stop_once;
    void stop() { std::call_once(stop_once, [this] { stop_now(); }); }
    void stop_now()
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

    void prepare()
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
    std::shared_ptr<CharacterCommand> make_batch(std::string& why)
    {
        auto batch = std::make_shared<CharacterCommand>();
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
    std::shared_ptr<CharacterCommand> command()
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

    bool init_materials();
    // The variant a (pipeline, draw, material plan) resolved to, kept per generation (D-106): the
    // variant cache's key building and lookup cost 5 us per replaced draw. Cleared with the plans.
    std::map<std::tuple<uint32_t, uint32_t, const StaticMaterial*, bool>, std::shared_ptr<const PipelineVariant>> variant_lookup;
    std::vector<FragmentPatch> scratch_patches; // a draw's material patch without the dither (D-133)
    bool refresh_materials()
    {
        variant_lookup.clear();
        // A live plan added a draw (a key, a texture section): every profile gets its plan. The rules
        // cover every slot (D-101), so nothing is derived again and the variants of the earlier draws
        // keep their patches.
        {
            std::set<uint32_t> slots(material_slots.begin(), material_slots.end());
            for (const auto& draw : package.pixels) for (const auto& pixel : draw) slots.insert(pixel.slot);
            material_slots.assign(slots.begin(), slots.end());
        }
        std::set<uint32_t> counts;
        std::string reason;
        if (!prepare_character_materials(package, materials, counts, reason)) return refuse(C_MATERIAL, reason);
        for (auto& derived : derived_profiles) {
            CharacterMaterialProfile profile{derived->code, derived->rules};
            if (!prepare_character_profile(package, profile, derived->plans, counts, reason)) return refuse(C_MATERIAL, reason);
        }
        for (auto count : counts) if (!image_counts.count(count)) return refuse(C_MATERIAL, "runtime material lacks a prepared arena");
        return true;
    }
    // `origin`: 0 the package's own profile or nothing to bind, 1 rules derived from the module, 2 the application's images.
    bool material_draws(const PipeInfo&, uint32_t section, const DrawRec&, uint64_t now, const CharacterList&,
                        std::vector<const StaticMaterial*>&, std::vector<std::vector<SnapshotImage>>&, uint32_t& origin);

    // Material rules of a fragment module for the slots the package binds, by the DX11 register rule
    // (D-076): a slot whose t register the module does not declare is absent, as an unread slot is
    // under DX11; a single image is bound there. False for a module the rule cannot read or a slot on
    // something else than one image (left to the application's images, never guessed).
    bool rule_rules(const std::vector<uint32_t>& code, std::vector<CharacterMaterialRule>& rules, std::string& reason) const
    {
        std::vector<efmi::gpu::SourceRegister> registers;
        if (!efmi::gpu::source_registers(code, registers, reason)) return false;
        rules.clear();
        // Every DX11 pixel slot, whatever the Mod binds today (D-101): rules that depended on the slots
        // bound so far were derived again whenever a plan bound a new one, and the variants compiled
        // again with them, the character flickering to the game's mesh each time (2-3 times after a
        // switch in the character menu).
        for (uint32_t slot = 0; slot < 128; ++slot) {
            const auto* r = efmi::gpu::find_source_register(registers, 't', slot);
            CharacterMaterialRule rule;
            rule.slot = slot;
            // A slot that holds no single image in this shader (nothing, a buffer, an array) takes no
            // file image here, as one the shader does not declare, and the rest of the slots still do.
            if (!r || r->kind != efmi::gpu::SourceRegister::ImageSrv || r->count != 1) {
                rule.kind = CharacterMaterialRule::Absent; rules.push_back(rule); continue;
            }
            // The device-bound compatibility policy of the package covers a 2D file at a 3D image.
            rule.kind = package.material_vendor && r->dim == 2 ? CharacterMaterialRule::ZeroFile : CharacterMaterialRule::Image;
            rule.set = r->set; rule.binding = r->binding;
            rules.push_back(rule);
        }
        return true;
    }

    // Caller holds mod_mx and res_mx shared. The application's texture at DX11 pixel slot `slot` of the
    // draw, by its original resource hash (D-077) and description (D-138): the slot's binding comes from
    // the fragment module's registers (D-076), the image from the tracked descriptor. A slot the module
    // does not declare reads nothing here; a resource whose hash cannot be known answers a bound slot
    // without a hash, so no hash section matches it and the draw is still replaced (counted in
    // texture_unknown, reason noted); one the layer cannot describe meets no match_* section.
    bool pixel_texture(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why)
    {
        // Inside record(): one draw's bindings do not change while it is planned, so a slot's answer
        // holds for the rest of the draw (D-108).
        const bool cacheable = lookups.pipeline == r.pipeline && lookups.pipe && slot < 128;
        if (cacheable && (lookups.answered[slot >> 6] >> (slot & 63) & 1)) {
            ++texture_queries; texture = lookups.answers[slot]; return true;
        }
        const bool answered = pixel_texture_uncached(r, slot, texture, why);
        if (cacheable && answered) { lookups.answered[slot >> 6] |= 1ull << (slot & 63); lookups.answers[slot] = texture; }
        return answered;
    }
    bool pixel_texture_uncached(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string&)
    {
        ++texture_queries;
        texture = {};
        bool& bound = texture.bound;
        uint32_t& hash = texture.hash;
        const auto unknown = [&](const char* why) { bound = true; hash = 0; ++texture_unknown; texture_reason = why; return true; };
        const PipeInfo* info = lookups.pipeline == r.pipeline ? lookups.pipe : nullptr;
        if (!info) {
            const auto pipe = device->pipes.find(r.pipeline);
            if (pipe != device->pipes.end()) info = &pipe->second;
        }
        if (!info || !info->recipe) return unknown("unknown pipeline");
        const FragmentRegisters* registers_known = lookups.pipe == info ? static_cast<const FragmentRegisters*>(lookups.registers) : nullptr;
        auto known = fragment_registers.end();
        if (!registers_known) known = fragment_registers.find(info->id);
        if (!registers_known && known == fragment_registers.end()) {
            if (fragment_registers.size() >= 8192) fragment_registers.clear();
            FragmentRegisters registers;
            registers.readable = true; // a pipeline without a fragment stage reads no texture
            for (const auto& stage : info->recipe->stages)
                if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                    std::string why;
                    registers.readable = efmi::gpu::source_registers(stage.shader->code, registers.list, why);
                }
            known = fragment_registers.emplace(info->id, std::move(registers)).first;
        }
        if (!registers_known) {
            registers_known = &known->second;
            if (lookups.pipe == info) lookups.registers = registers_known;
        }
        if (!registers_known->readable) return unknown("the fragment module is not read by the register rule");
        const auto* reg = efmi::gpu::find_source_register(registers_known->list, 't', slot);
        if (!reg) return true;
        if (reg->kind != efmi::gpu::SourceRegister::ImageSrv || reg->count != 1 || reg->set >= kMaxSets)
            return unknown("a pixel slot is not a single image");
        const char* why = nullptr;
        bool empty = false;
        const ImageInfo* image = bound_image(r, reg->set, reg->binding, empty, why);
        if (empty) return true; // a null view: an empty slot
        if (!image) return unknown(why);
        texture.described = dx11_texture_desc(*image, texture.description);
        if (!dx11_texture_hash(*image, hash)) return unknown("an image without a known original hash");
        bound = true;
        return true;
    }

    // Caller holds res_mx. The image the draw binds at element 0 of (set, binding): null with `empty`
    // for a null view, null with `why` when the layer cannot tell.
    // Caller holds res_mx. The buffer range at `binding`, element 0, of a bound set, by the rules of
    // collect_buffer_ranges_locked (the dynamic offset is the one of this element among the set's
    // dynamic bindings in binding order; the range lies inside a live buffer), without listing the
    // set's other ranges: the instance entry is binding 0 of set 2, read on every matched draw.
    bool bound_range(const SetBind& bind, uint32_t slot, uint32_t binding, const uint32_t* dyn, uint32_t dyn_count, BoundRange& out) const
    {
        auto dynamic_type = [](uint32_t t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; };
        const DescriptorSetInfo* contents = bind.pushed.get();
        if (!contents && bind.id) {
            const auto set = device->descriptor_sets.find(bind.set);
            if (set != device->descriptor_sets.end() && set->second.id == bind.id) contents = &set->second;
        }
        if (bind.unsupported || !contents || !contents->layout) return false;
        const auto layout = contents->layout->bindings.find(binding);
        if (layout == contents->layout->bindings.end()) return false;
        const uint32_t type = layout->second.type;
        if (!dynamic_type(type) && type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return false;
        uint32_t dynamic = 0;
        for (const auto& kv : contents->layout->bindings) {
            if (kv.first >= binding) break;
            if (dynamic_type(kv.second.type)) {
                const auto count = contents->counts.find(kv.first);
                if (count == contents->counts.end()) return false;
                dynamic += count->second;
            }
        }
        const auto value = contents->values.find({binding, 0});
        if (value == contents->values.end() || !value->second.known) return false;
        const DescriptorElement& e = value->second;
        const auto buffer = device->bufs.find(e.buffer);
        if (!e.buffer_id || buffer == device->bufs.end() || buffer->second.id != e.buffer_id) return false;
        const uint64_t offset = e.offset + (dynamic_type(type) && dynamic < dyn_count ? dyn[dynamic] : 0);
        const uint64_t size = buffer->second.size;
        if (offset > size) return false;
        const uint64_t range = e.range == VK_WHOLE_SIZE ? (e.offset <= size ? size - e.offset : 0) : e.range;
        if (range > size - offset) return false;
        out = BoundRange{};
        out.set = static_cast<uint8_t>(slot); out.binding = binding; out.element = 0; out.type = type;
        out.buffer = e.buffer; out.buffer_id = e.buffer_id; out.off = offset; out.size = range;
        return true;
    }
    const ImageInfo* bound_image(const DrawRec& r, uint32_t set_index, uint32_t binding, bool& empty, const char*& why)
    {
        empty = false;
        if (set_index >= kMaxSets) { why = "a set outside the tracked slots"; return nullptr; }
        const SetBind& bind = r.sets[set_index];
        const DescriptorSetInfo* contents = bind.pushed.get();
        if (!contents && bind.id) {
            if (lookups.pipe && lookups.set == bind.set && lookups.set_id == bind.id) contents = lookups.contents;
            else {
                const auto set = device->descriptor_sets.find(bind.set);
                if (set != device->descriptor_sets.end() && set->second.id == bind.id) contents = &set->second;
                if (lookups.pipe) { lookups.set = bind.set; lookups.set_id = bind.id; lookups.contents = contents; }
            }
        }
        if (bind.unsupported || !contents) { why = "the descriptor set of a pixel slot is not tracked"; return nullptr; }
        const auto value = contents->values.find({binding, 0});
        if (value == contents->values.end() || !value->second.known) { why = "a pixel slot's descriptor is unknown"; return nullptr; }
        if (!value->second.image_view) { empty = true; return nullptr; }
        const auto view = device->image_views.find(value->second.image_view);
        if (view == device->image_views.end() || view->second.id != value->second.image_view_id) { why = "unknown image view"; return nullptr; }
        const auto image = device->images.find(view->second.image);
        if (image == device->images.end() || image->second.id != view->second.image_id) { why = "unknown image"; return nullptr; }
        return &image->second;
    }

    // Caller holds res_mx and mod_mx. RabbitFX's roles in this draw (D-082): the fragment module's
    // samples and the images the draw binds at them. False when the module is not read.
    bool texture_roles(const DrawRec& r, int32_t roles[3], std::string& reason)
    {
        roles[0] = roles[1] = roles[2] = -1;
        const auto pipe = device->pipes.find(r.pipeline);
        if (pipe == device->pipes.end() || !pipe->second.recipe) { reason = "unknown pipeline"; return false; }
        auto known = fragment_samples.find(pipe->second.id);
        if (known == fragment_samples.end()) {
            if (fragment_samples.size() >= 8192) fragment_samples.clear();
            FragmentSamples samples;
            samples.readable = true; // a pipeline without a fragment stage samples nothing
            for (const auto& stage : pipe->second.recipe->stages)
                if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                    std::string why;
                    samples.readable = efmi::gpu::role_samples(stage.shader->code, samples.list, why);
                }
            for (const auto& sample : samples.list) if (sample.set < kMaxSets) samples.sets |= 1u << sample.set;
            known = fragment_samples.emplace(pipe->second.id, std::move(samples)).first;
        }
        if (!known->second.readable) { reason = "the fragment module is not read"; return false; }
        RoleKey key;
        key.pipe = pipe->second.id;
        for (uint32_t i = 0; i < kMaxSets; ++i) {
            if (!(known->second.sets >> i & 1)) continue;
            const SetBind& bind = r.sets[i];
            if (bind.pushed) {
                key.identity[i] = uint64_t(reinterpret_cast<uintptr_t>(bind.pushed.get())); key.generation[i] = bind.pushed->generation;
            } else if (bind.id && !bind.unsupported) {
                key.identity[i] = uint64_t(bind.id) << 1 | 1;
                const auto set = device->descriptor_sets.find(bind.set);
                if (set != device->descriptor_sets.end() && set->second.id == bind.id) key.generation[i] = set->second.generation + 1;
            }
        }
        ++role_queries;
        if (const auto cached = role_cache.find(key); cached != role_cache.end()) {
            std::copy(cached->second.roles, cached->second.roles + 3, roles);
            if (roles[0] >= 0) ++role_draws;
            return true;
        }
        const auto found = efmi::gpu::texture_roles(known->second.list, [&](const efmi::gpu::RoleSample& s) {
            efmi::gpu::RoleTexture texture;
            const char* why = nullptr;
            bool empty = false;
            if (const ImageInfo* image = bound_image(r, s.set, s.binding, empty, why)) {
                texture.known = true; texture.format = image->format;
                texture.width = image->extent.width; texture.height = image->extent.height; texture.levels = image->levels;
            }
            return texture;
        });
        roles[0] = found.diffuse; roles[1] = found.lightmap; roles[2] = found.normal;
        if (role_cache.size() >= 4096) role_cache.clear();
        std::copy(roles, roles + 3, role_cache[key].roles);
        if (found.diffuse >= 0) ++role_draws;
        return true;
    }

    // Caller holds res_mx. The derived profile of a fragment shader the package has no profile for,
    // or none: then the mesh is the mod's and the images stay the application's.
    uint32_t derive(const PipeInfo& pipe)
    {
        const auto known = derived_for_pipeline.find(pipe.id);
        if (known != derived_for_pipeline.end()) return known->second;
        if (derived_for_pipeline.size() >= 8192) derived_for_pipeline.clear();
        uint32_t& result = derived_for_pipeline[pipe.id];
        result = kCharacterNone;
        const std::vector<uint32_t>* code = nullptr;
        for (const auto& stage : pipe.recipe->stages)
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) code = &stage.shader->code;
        if (!code) return result;
        const uint32_t crc = crc32c(0, code->data(), code->size() * 4);
        for (uint32_t i = 0; i < derived_profiles.size(); ++i)
            if (derived_profiles[i]->crc == crc && derived_profiles[i]->code == *code) return result = i;
        std::string reason;
        auto created = std::make_unique<DerivedProfile>();
        if (!rule_rules(*code, created->rules, reason)) {
            char name[16];
            snprintf(name, sizeof(name), "%08x", crc);
            return refuse_profile(std::string("character material: fragment ") + name + " keeps the application's images: " + reason, result);
        }
        if (derived_profiles.size() >= kDerivedProfiles) return refuse_profile("character material: too many derived profiles", result);
        created->crc = crc; created->code = *code; created->slots = material_slots;
        CharacterMaterialProfile source;
        source.code = created->code; source.rules = created->rules;
        std::set<uint32_t> counts;
        if (!prepare_character_profile(package, source, created->plans, counts, reason)) {
            return refuse_profile("character material: pipeline " + std::to_string(pipe.id) + " derived profile refused: " + reason, result);
        }
        // The arenas of a command are made for the image counts known at setup.
        for (uint32_t count : counts)
            if (!image_counts.count(count))
                return refuse_profile("character material: derived profile of pipeline " + std::to_string(pipe.id) + " needs " +
                                      std::to_string(count) + " images, no arena has that count", result);
        size_t images = 0;
        for (const auto& rule : created->rules) if (rule.kind != CharacterMaterialRule::Absent) ++images;
        logf("character material: fragment %08x (pipeline %u) takes %zu image rules from the DX11 register rule", crc, pipe.id, images);
        derived_profiles.push_back(std::move(created));
        return result = static_cast<uint32_t>(derived_profiles.size() - 1);
    }

    // `now`: the character's frame; `present`: counted presents.
    Identity* identify(const InstanceEntry& entry, uint64_t now, uint64_t present)
    {
        float position[3];
        std::memcpy(position, entry.data() + 48, sizeof(position));
        int32_t cell[3];
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(position[i]) || std::fabs(position[i]) > 1.0e6f) return nullptr;
            cell[i] = static_cast<int32_t>(std::floor(position[i] / kCellSize));
        }
        const float scaled = std::ceil(kBaseRadius * (frame_seconds.load(std::memory_order_relaxed) * 120.0f));
        const int32_t radius = static_cast<int32_t>(std::min(std::max(scaled, kBaseRadius), 64.0f));
        const uint64_t clock = host_time_us();
        Identity* best = nullptr;
        int32_t best_distance = INT32_MAX;
        Identity* vacant = nullptr;
        Identity* weakest = nullptr;
        Identity* stale = nullptr;
        uint32_t weakest_seen = UINT32_MAX;
        // For the log when a new identity is made: the replaced object nearest to this place.
        int32_t near_distance = INT32_MAX;
        uint64_t near_frames = 0, near_us = 0;
        bool near_expired = false;
        for (auto& identity : identities) {
            int32_t distance = 0;
            for (int i = 0; i < 3; ++i) distance = std::max(distance, std::abs(cell[i] - identity.cell[i]));
            // Recordings of two threads may interleave, so a frame number can be behind the last one.
            const uint64_t gap = now > identity.last_frame ? now - identity.last_frame : identity.last_frame - now;
            const bool unseen = present - identity.last_present > kExpiryFrames;
            bool expired = false;
            if (identity.used && unseen && clock - identity.last_us > g_cfg.m2_identity_expiry_ms * 1000) {
                identity.used = false; expired = true;
                identity.ring.reset(); identity.gpu_frame = false; identity.bone_sources.clear();
                ++counters[C_IDENTITY_EXPIRED];
            }
            if ((identity.used || expired) && identity.custom && distance < near_distance) {
                near_distance = distance; near_frames = gap; near_us = clock - identity.last_us; near_expired = expired;
            }
            if (!identity.used) { if (!vacant) vacant = &identity; continue; }
            // Every component of one object has the same position within a frame, to a cell: the passes
            // of one object differ by that much (D-100; 9 of 42 new identities of a session were this).
            // Across frames the object may have moved, by at most the radius for each frame it was not
            // drawn in.
            const int32_t allowed = gap ? static_cast<int32_t>(std::min<uint64_t>(uint64_t(radius) * gap, 64)) : 1;
            if (distance <= allowed && distance < best_distance) { best = &identity; best_distance = distance; }
            uint32_t bits = 0;
            for (uint64_t value = identity.seen; value; value &= value - 1) ++bits;
            if (!identity.custom && bits < weakest_seen) { weakest = &identity; weakest_seen = bits; }
            if (unseen && (!stale || identity.last_present < stale->last_present)) stale = &identity;
        }
        if (!best) {
            // Farther than the radius in one frame (a dash, a camera cut, 4 to 7 metres in a frame in
            // the game): when exactly one identified object of this package has not been drawn since
            // the last presentation, it is that one, moved (D-100). A second instance appearing in
            // the very frame the first is not drawn is the rarer case; it is told apart a frame later.
            Identity* lone = nullptr;
            uint32_t lone_count = 0;
            for (auto& identity : identities)
                if (identity.used && identity.custom && identity.last_present < present) { lone = &identity; ++lone_count; }
            if (lone_count == 1) { best = lone; best_distance = 0; ++counters[C_IDENTITY_ADOPTED]; }
        }
        if (!best) {
            // Objects that merely share a mesh with the character must not keep it out: when every
            // slot is taken, the unidentified object seen with the fewest components gives way, and
            // failing that the object that has gone longest without a draw.
            if (!vacant) vacant = weakest;
            if (!vacant) vacant = stale;
            if (!vacant) return nullptr;
            ++counters[C_IDENTITY_NEW];
            if (near_distance != INT32_MAX && identity_logs < 200) {
                ++identity_logs;
                logf("character identity %llu at a new place: the nearest replaced object is %d cells away, last drawn %llu frames and %llu ms ago%s (radius %d)",
                     static_cast<unsigned long long>(next_identity), near_distance, static_cast<unsigned long long>(near_frames),
                     static_cast<unsigned long long>(near_us / 1000), near_expired ? ", expired" : "", radius);
            }
            std::string reason;
            if (!vacant->skeleton.configure(package.bones, 1, ranges, reason)) { note(reason); return nullptr; }
            vacant->used = true; vacant->custom = false; vacant->shown = false; vacant->seen = 0; vacant->bone_sources.clear();
            // Another object's skeleton on the device is not this one's (D-127).
            vacant->ring.reset(); vacant->gpu_frame = false;
            vacant->first_frame = now; vacant->first_present = present; vacant->update_frame = 0;
            vacant->id = next_identity++; vacant->generation = 0;
            vacant->coverage.assign(package.components.size(), 0);
            best = vacant;
        }
        std::memcpy(best->cell, cell, sizeof(cell));
        best->last_frame = now;
        best->last_present = present;
        best->last_us = clock;
        return best;
    }

    // The identity's merged skeleton on the device (D-117, D-127), made on first use; without one the
    // recording imports on the CPU as before. An object that imported on the CPU (the bones switched
    // there in between) starts its device skeleton again: what it held is older than the master.
    void acquire_ring(Identity& identity)
    {
        identity.gpu_frame = false;
        if (!bones_gpu || !bones_gpu->usable() || !live_gpu_bones() || bone_ring_failed) {
            if (identity.ring) { identity.ring.reset(); identity.bone_sources.clear(); }
            return;
        }
        if (!identity.ring) {
            std::string why;
            identity.ring = bones_gpu->ring(package.bones, why);
            if (!identity.ring) { bone_ring_failed = true; note("bone ring: " + why); return; }
        }
        identity.gpu_frame = true;
    }
    bool ensure_remaps()
    {
        if (bone_remaps || bone_remaps_failed) return bool(bone_remaps);
        std::vector<std::vector<uint16_t>> tables;
        for (const auto& c : package.components) tables.push_back(c.has_remap ? c.remap_lod1 : std::vector<uint16_t>{});
        std::string why;
        bone_remaps = bones_gpu->remaps(tables, why);
        if (!bone_remaps) { bone_remaps_failed = true; note("bone remaps: " + why); }
        return bool(bone_remaps);
    }
    // One component's import as a dispatch in front of the draw's recording (D-127), whatever happens to
    // the draw, as the Core imports in the draw's list; false when the source cannot be bound (its
    // bounds, alignment or remap table), and the CPU import follows for this draw.
    BoneJob scratch_bone_job;
    bool prepare_bone_job(Identity& identity, CharacterCommand& owner, uint32_t component_index, const CharacterComponent& component,
                          bool remapped, size_t needed, const BoundRange& bones, const uint32_t offsets[2], uint8_t dynamic)
    {
        for (uint32_t p = 0; p < 2; ++p)
            if ((dynamic >> p & 1) && (uint64_t(offsets[p]) * 16 > bones.size || needed > bones.size - uint64_t(offsets[p]) * 16)) return false;
        if (bones.off % bones_gpu->alignment()) return false;
        if (remapped && !ensure_remaps()) return false;
        BoneJob& job = scratch_bone_job;
        job.ring = identity.ring; job.remaps = remapped ? bone_remaps : nullptr; job.component = component_index;
        job.source = bones.buffer; job.source_id = bones.buffer_id; job.source_offset = bones.off; job.source_size = bones.size;
        job.rows[0] = offsets[0]; job.rows[1] = offsets[1];
        job.first = component.first; job.count = component.count; job.poses = dynamic; job.remap = remapped;
        job.recording = owner.recording; job.epoch = owner.recording->epoch();
        owner.used = true;
        owner.add_bone_job(job);
        if (identity.bone_sources.size() < package.components.size()) identity.bone_sources.resize(package.components.size());
        identity.bone_sources[component_index] = job;
        ++counters[C_BONE_JOBS];
        return true;
    }
    BoneSite site_of(const PipeInfo& pipe)
    {
        auto found = sites.find(pipe.id);
        if (found != sites.end()) return found->second;
        BoneSite site;
        if (pipe.recipe && pipe.layout) {
            for (const auto& stage : pipe.recipe->stages) {
                if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT || !stage.shader) continue;
                VertexRepoint out;
                if (repoint_vertex(stage.shader->code, static_cast<uint32_t>(pipe.layout->sets.size()), out) && out.bone_set < kMaxSets)
                    site = {true, out.bone_set, out.bone_binding};
            }
        }
        if (sites.size() >= 4096) sites.clear();
        sites[pipe.id] = site;
        return site;
    }

    // Caller holds mod_mx and res_mx. The Core's facts by its own rules on the vertex module's DX11
    // registers (D-076), kept only where they name what the renderer itself reads: the window at
    // set 2, binding 0 (the instance entry) and the bone buffer the bone site found. Otherwise none,
    // and the original Core leaves the mesh to the game.
    // The reference stays valid until the next call.
    const SourceFacts& facts_of(const PipeInfo& pipe, const BoneSite& site)
    {
        const auto known = source_facts.find(pipe.id);
        if (known != source_facts.end()) return known->second;
        if (source_facts.size() >= 4096) source_facts.clear();
        SourceFacts facts;
        if (pipe.recipe)
            for (const auto& stage : pipe.recipe->stages) {
                static const VkShaderStageFlagBits order[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
                const auto at = std::find(std::begin(order), std::end(order), stage.stage);
                if (at == std::end(order) || !stage.shader) continue;
                auto registers = std::make_shared<std::vector<efmi::gpu::SourceRegister>>();
                std::string why;
                if (!efmi::gpu::source_registers(stage.shader->code, *registers, why)) continue;
                facts.registers[size_t(at - std::begin(order))] = registers;
                if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
                const auto core = efmi::gpu::core_source_facts(*registers);
                if (core.config && core.config->set == 2 && core.config->binding == 0) {
                    facts.vs_config_slot = core.vs_config_slot;
                    facts.rolling_bones = core.rolling_bones && site.ok && core.bones->set == site.set && core.bones->binding == site.binding;
                }
            }
        if (!facts.rolling_bones) ++facts_unknown;
        return source_facts[pipe.id] = std::move(facts);
    }

    // `vbN->stride` of a replaced draw (RuntimeDraw::vertex_stride): the pipeline's binding N, as DX11's
    // input slot N the layer takes it elsewhere too (the blend-index stream at vb2); none bound is 0.
    static bool vertex_stride(const PipeInfo& pipe, unsigned slot, uint32_t& stride, std::string& why)
    {
        if (std::find(pipe.dynamic_states.begin(), pipe.dynamic_states.end(), uint32_t(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)) != pipe.dynamic_states.end()) {
            why = "the pipeline sets its vertex strides at recording"; return false;
        }
        stride = 0;
        for (const auto& b : pipe.bindings) if (b.binding == slot) { stride = b.stride; break; }
        return true;
    }
    // The draw matched `section_index` of the package (its hash, count and first index checked by the
    // device's index): recorded as the Mod's, hidden, or left to the game.
    bool record(CmdBuf* cb, const DrawRec& r, uint32_t section_index);
    // A replaced draw's plan with a scope for its owned GPU work (P2, D-087): recorded without a
    // queue, pending until submit_draw_work. Caller holds mod_mx.
    bool plan_draw(const efmi::frontend::RuntimeDraw& request, CharacterList& output, bool& skip, bool& added, std::string& reason,
                   const VertexInputRecipe* vertex, bool* textures)
    {
        efmi::frontend::OwnedRuntime* gpu = nullptr;
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
        auto began = std::chrono::steady_clock::now();
        auto phase = [&](const char* name) {
            const auto now = std::chrono::steady_clock::now();
            program->note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - began).count())); began = now;
        };
#else
        auto phase = [](const char*) {};
#endif
        // A folder generation's packages share its INI generation and owned runtime: one plan at a time.
        std::unique_lock<std::mutex> shared;
        if (plan_mx) shared = std::unique_lock<std::mutex>(*plan_mx);
        if (presenter && !draw_work_failed && program->gpu() && !program->gpu()->scope_active()) {
            try { program->gpu()->begin_draw(); gpu = program->gpu().get(); } catch (const std::exception&) {}
        }
        phase("phase gpu begin");
        const bool planned = program->plan(request, package, output, skip, added, reason, vertex, textures);
        phase("phase program plan (whole)");
        if (gpu && gpu->scope_active()) { try { gpu->suspend(); } catch (const std::exception&) {} }
        if (gpu && gpu->pending()) draw_work_waiting.store(true, std::memory_order_release);
        phase("phase gpu suspend");
        return planned;
    }
    // Before an application submission on a graphics queue: the owned work replaced draws recorded,
    // on the queue the generation's owned work belongs to (its first submission fixes it).
    void submit_draw_work(VkQueue queue)
    {
        if (!draw_work_waiting.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mod_mx);
        if (!presenter || !program || !program->gpu() || draw_work_failed || !program->gpu()->pending()) {
            draw_work_waiting.store(false, std::memory_order_relaxed); return;
        }
        if (std::find(device->material_queues.begin(), device->material_queues.end(), queue) == device->material_queues.end()) return;
        const VkQueue bound = program->gpu()->queue();
        if (bound && bound != queue) return;
        std::string reason;
        std::unique_lock<std::mutex> shared;
        if (plan_mx) shared = std::unique_lock<std::mutex>(*plan_mx);
        if (!presenter->submit_pending(queue, reason)) {
            draw_work_failed = true; note(reason);
            logf("owned GPU work of replaced draws stopped: %s", reason.c_str());
        }
        draw_work_waiting.store(!draw_work_failed && program->gpu()->pending(), std::memory_order_relaxed);
    }
    // The draw's render targets for `@oN` (D-087), from its pipeline's subpass, and its shader stages
    // for `vs` and `ps` (D-089).
    static void render_targets(const PipeInfo& pipe, efmi::frontend::RuntimeDraw& request)
    {
        if (!pipe.recipe) return;
        request.shader_stages = 0;
        for (const auto& stage : pipe.recipe->stages) request.shader_stages |= uint32_t(stage.stage);
        if (!pipe.recipe->render_pass || pipe.recipe->subpass >= pipe.recipe->render_pass->subpasses.size()) return;
        const auto& sub = pipe.recipe->render_pass->subpasses[pipe.recipe->subpass];
        uint32_t mask = 0;
        for (size_t i = 0; i < sub.colors.size() && i < 8; ++i)
            if (sub.colors[i].attachment != VK_ATTACHMENT_UNUSED) mask |= 1u << i;
        if (sub.has_depth && sub.depth.attachment != VK_ATTACHMENT_UNUSED) mask |= 1u << 8;
        request.targets_known = true; request.render_targets = mask;
        request.render_pass = uint64_t(pipe.recipe->render_pass->id) << 8 | pipe.recipe->subpass;
    }
    // The application's draw made again with the Mod's textures (D-083). Caller holds res_mx and
    // mod_mx; `list` is a texture-only list of the program.
    bool record_textures(CmdBuf* cb, const DrawRec& r, const PipeInfo& pipe, uint32_t section, uint64_t present, const CharacterList& list);
};

bool CharacterGeneration::init_materials()
{
    using namespace efmi;
    if (device->material_queues.empty()) { last_reason = device->material_queue_reason; return false; }
    if (package.material_vendor && (package.material_vendor != properties.vendorID || package.material_device != properties.deviceID)) {
        last_reason = "material dimension compatibility belongs to a different device"; return false;
    }
    if (!prepare_character_materials(package, materials, image_counts, last_reason)) return false;
    {
        std::set<uint32_t> slots;
        for (const auto& draw : package.pixels) for (const auto& pixel : draw) slots.insert(pixel.slot);
        material_slots.assign(slots.begin(), slots.end());
    }
    // The package's own profiles came from paired captures; the register rule must give the same
    // bindings for the slots they place. A difference is reported, the profile stays in use.
    for (uint32_t i = 0; i < package.profiles.size(); ++i) {
        std::vector<CharacterMaterialRule> rules;
        std::string why;
        if (!rule_rules(package.profiles[i].code, rules, why)) { ++rule_mismatches; logf("character material: profile %u not read by the register rule: %s", i, why.c_str()); continue; }
        for (const auto& own : package.profiles[i].rules)
            for (const auto& rule : rules)
                if (rule.slot == own.slot && (rule.kind != own.kind || rule.set != own.set || rule.binding != own.binding)) {
                    ++rule_mismatches;
                    logf("character material: profile %u slot %u is %u.%u by capture, %u.%u by the register rule", i, own.slot,
                         own.set, own.binding, rule.set, rule.binding);
                }
    }
    // The images as the device's residency holds them (D-094): declared here, loaded on first bind.
    if (!textures || !textures->usable()) { last_reason = textures ? textures->reason() : "textures need the device's residency"; return false; }
    for (size_t i = 0; i < package.images.size(); ++i) {
        const auto& path = package.images[i];
        // A file that does not load is the original's null resource: no plan binds it (D-139).
        if (program && program->image_unloadable(path)) { image_ids.push_back(TextureResidency::kNone); continue; }
        std::string why;
        const auto id = textures->declare(path, why, i < package.image_flags.size() ? package.image_flags[i] : 0u);
        if (id == TextureResidency::kNone) { last_reason = why; return false; }
        image_ids.push_back(id);
    }
    // D-092: activating brings the mesh and the first snapshot batches up; the textures come on demand.
    activation_bytes = package.payload_bytes + (32ull << 20);
    return true;
}

bool CharacterGeneration::material_draws(const PipeInfo& pipe, uint32_t section, const DrawRec& r, uint64_t now,
                                      const CharacterList& list,
                                      std::vector<const StaticMaterial*>& plans, std::vector<std::vector<SnapshotImage>>& views,
                                      uint32_t& origin)
{
    static const StaticMaterial inherited{true, false, {}, {}, {}};
    const uint32_t pipeline_id = pipe.id;
    const GraphicsRecipe& recipe = *pipe.recipe;
    const PipelineLayoutInfo& layout = *pipe.layout;
    origin = 0;
    uint32_t profile = kCharacterNone;
    bool no_fragment = true;
    if (package.materials) {
        const auto found = material_profiles.find(pipeline_id);
        if (found != material_profiles.end()) profile = found->second;
        else {
            for (const auto& stage : recipe.stages) if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                no_fragment = false;
                for (uint32_t i = 0; i < package.profiles.size(); ++i)
                    if (package.profiles[i].code == stage.shader->code) { profile = i; break; }
            }
            if (no_fragment) profile = kCharacterNone - 1;
            if (profile == kCharacterNone && layout.complete) {
                // A fragment shader whose layout holds no image for it cannot read a texture: every
                // slot the mod binds is absent there, as for the pass that tests visibility with
                // depth only. Nothing to replace; the mesh is still the mod's.
                bool images = false, known = true;
                for (const auto& set : layout.sets) {
                    if (!set || !set->complete) { known = false; break; }
                    for (const auto& b : set->bindings) {
                        const uint32_t type = b.second.type;
                        if ((b.second.stages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                            (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                             type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                             type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                             type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) images = true;
                    }
                }
                if (known && !images) profile = kCharacterNone - 1;
            }
            if (material_profiles.size() >= 4096) material_profiles.clear();
            material_profiles[pipeline_id] = profile;
        }
    }
    // The table of plans: the package's own profile; for a fragment shader without one the rules
    // the DX11 register rule reads off its module; failing that none, and the application's images
    // stay on the mod's mesh. Keeping the game's mesh there instead would mix the two models on screen.
    const std::vector<StaticMaterial>* table = nullptr;
    bool binds = false;
    for (uint32_t i = 0; i < list.count; ++i) binds |= package.materials && !package.pixels[list.first + i].empty();
    if (binds && profile < package.profiles.size()) {
        table = &materials[profile];
    } else if (binds && profile == kCharacterNone) {
        const uint32_t index = derive(pipe);
        if (index != kCharacterNone) table = &derived_profiles[index]->plans;
        origin = table ? 1 : 2;
    }
    // Decide the entire logical transaction before obtaining any views or issuing a command.
    for (uint32_t i = 0; i < list.count; ++i) {
        const uint32_t index = list.first + i;
        const StaticMaterial* plan = &inherited;
        if (table && !package.pixels[index].empty()) {
            plan = &(*table)[index];
            if (!plan->supported) return refuse(C_MATERIAL, plan->reason);
        }
        plans.push_back(plan);
    }
    // Queue only this transaction's images together; the owned upload queue prepares them in
    // parallel before the draw uses them. Recordings retain their independent GPU owners.
    auto& selected = scratch_image_ids; selected.clear();
    for (const auto* plan : plans) for (uint32_t asset : plan->assets) {
        if (!textures || asset >= image_ids.size()) return refuse(C_IMAGE, "character texture outside the package's images");
        selected.push_back(image_ids[asset]); frame_image(image_ids[asset]);
    }
    auto& bindings = scratch_image_bindings;
    if (!selected.empty()) textures->bind(selected, bindings);
    bool pending = false;
    size_t binding = 0;
    if (views.size() < plans.size()) views.resize(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        const auto* plan = plans[i];
        auto& draw_views = views[i]; draw_views.clear();
        for (size_t asset = 0; asset < plan->assets.size(); ++asset) {
            const auto& bound = bindings[binding++];
            if (bound.failed) return refuse(C_IMAGE, "character texture: " + bound.reason);
            if (!bound.view) { pending = true; continue; }
            draw_views.push_back({bound.view, bound.owner});
        }
    }
    // An image still on its way after the bind's wait (its load refused by the budget, or the wait
    // timed out) closes the package's frame (D-097); logged once, as this shows the game's mesh.
    if (pending) {
        gate_open = false; ++counters[C_IMAGE_PENDING];
        if (!gate_logged) { gate_logged = true; logf("character frame gated: a texture of the draw is not resident after the wait; the game draws this package this frame"); }
        return false;
    }
    return true;
}

bool CharacterGeneration::record(CmdBuf* cb, const DrawRec& r, uint32_t section_index)
{
    if (!enabled.load(std::memory_order_relaxed) || !cache) return false;
    bool gpu_draw = false; // this draw binds the device's merged skeleton (D-117) rather than the CPU master
    CharacterCommand* bone_owner = nullptr; // the recording's owner, which carries the device imports (D-127)
    uint64_t borrowed_sources = 0; // components whose import this recording takes from one not yet submitted (D-127)

    ThreadTrace* const trace = device->trace ? device->trace() : nullptr;
    const uint64_t entered_us = host_time_us();
    StageMark locking(trace, "character draw (generation lock)");
    std::lock_guard<std::mutex> mod_lock(mod_mx);
    ScratchScope scratch_scope{*this};
    StageMark stage(trace, "character draw (plan, variants, recording)");
    uint64_t mark_us = host_time_us();
    stage_us[S_LOCK] += mark_us - entered_us;
    auto lap = [&](Stage s) { const uint64_t now_us = host_time_us(); stage_us[s] += now_us - mark_us; mark_us = now_us; };
    const uint64_t present = frame.load(std::memory_order_relaxed);
    InstanceEntry entry, patched;
    const CharacterList* list = nullptr;
    CharacterList runtime_list;
    Identity* identity = nullptr;
    auto& variants = scratch_variants; variants.clear();
    auto& material_plans = scratch_material_plans; material_plans.clear();
    auto& material_views = scratch_material_views;
    uint32_t material_origin = 0;
    DrawGeometry& original = scratch_original;
    original.first_binding = 0; original.buffers.clear(); original.offsets.clear();
    original.index = VK_NULL_HANDLE; original.index_offset = 0; original.index_type = VK_INDEX_TYPE_UINT32;
    struct LookupsReset {
        DrawLookups& l;
        ~LookupsReset() { l.pipeline = VK_NULL_HANDLE; l.pipe = nullptr; l.registers = nullptr; l.set = VK_NULL_HANDLE; l.set_id = 0;
                          l.contents = nullptr; l.answered[0] = l.answered[1] = 0; }
    } lookups_reset{lookups};
    // The application's own set 3, when it has one bound: the replacement uses that slot, so it is
    // bound again afterwards exactly as the application bound it.
    uint32_t restore_slot = 0;
    VkPipelineLayout restore_layout = VK_NULL_HANDLE;
    VkDescriptorSet restore_set = VK_NULL_HANDLE;
    auto& restore_offsets = scratch_restore_offsets; restore_offsets.clear();
    {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        const CharacterSection* const section = &package.sections[section_index];
        if (!activated.load(std::memory_order_relaxed)) {
            if (!memory_allows_activation()) { ++counters[C_MEMORY]; return false; }
            // The package goes to the GPU now; this draw is skipped, as the Core skips a new object's
            // first frame (D-103): the character is never the game's mesh before the Mod's.
            activate(); ++counters[C_PREPARING]; return true;
        }
        if (!ready.load(std::memory_order_relaxed)) {
            // The worker ended without the package: the game's draw stays, and the activation is tried again
            // at its time. While the mesh and arenas are on their way the draw is skipped, as before.
            if (worker_failed.load(std::memory_order_acquire)) { retry_activation(); ++counters[C_ACTIVATION]; return false; }
            ++counters[C_PREPARING]; return true;
        }
        ++counters[C_MATCHED];
        if (present != gate_frame) begin_gate_frame(present);
        if (section->component == kCharacterNone) {
            // Exact IB-only override: no fabricated identity or bone import.
            // Live conditions still run; a changed rule may only emit an empty skip.
            if (program) {
                efmi::frontend::RuntimeDraw request;
                request.hash = section->hash; request.count = r.count; request.first = r.first;
                request.base = r.vertex_offset; request.instances = r.instances; request.first_instance = r.first_instance;
                request.mod = program->folder_index();
                request.frame = present; request.seconds = ini_seconds();
                request.frame_seconds = frame_seconds.load(std::memory_order_relaxed);
                request.pixel_texture = [this, &r](unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why) {
                    return pixel_texture(r, slot, texture, why); };
                request.texture_roles = [this, &r](int32_t roles[3], std::string& why) { return texture_roles(r, roles, why); };
                const auto found = device->pipes.find(r.pipeline);
                if (found != device->pipes.end()) {
                    render_targets(found->second, request);
                    request.vertex_stride = [&found](unsigned slot, uint32_t& stride, std::string& why) { return vertex_stride(found->second, slot, stride, why); };
                }
                bool skip = false, added = false, textures = false; std::string reason;
                if (!plan_draw(request, runtime_list, skip, added, reason, nullptr, &textures)) return refuse(C_STATE, "INI hide: " + reason);
                if (!skip) {
                    if (!textures) return false;
                    if (added && !refresh_materials()) return false;
                    const auto pipe = device->pipes.find(r.pipeline);
                    if (pipe == device->pipes.end() || !pipe->second.recipe || !pipe->second.layout)
                        return refuse(C_PIPELINE, "texture-only draw pipeline metadata missing");
                    return record_textures(cb, r, pipe->second, static_cast<uint32_t>(section - package.sections.data()), present, runtime_list);
                }
                if (runtime_list.count) return refuse(C_STATE, "INI hide override unexpectedly emits geometry");
            }
            // With the package's mesh left to the game this frame, what it would hide stays as well.
            if (!gate_open) return gated();
            ++counters[C_HIDDEN]; return true;
        }
        // The character's frame is this recording of the command buffer (see the identity expiry).
        if (!cb->character_frame) cb->character_frame = ++device->character_recordings;
        const uint64_t now = cb->character_frame;
        // The mod's LOD variable: a section that assigns it changes it for the sections that do not.
        if (section->lod != kCharacterNone) lod = section->lod;
        const CharacterComponent& component = package.components[section->component];
        if (cb->secondary || !cb->in_rp || cb->dyn_rp_count || cb->m2_extended_bind)
            return refuse(C_STATE, "character draw outside a primary classic render pass or after extended binds");
        if (r.instances != 1 || r.first_instance != 0) return refuse(C_INSTANCES, "character draw is instanced");
        auto pipe = device->pipes.find(r.pipeline);
        if (pipe == device->pipes.end() || !pipe->second.recipe || !pipe->second.layout)
            return refuse(C_PIPELINE, "character draw pipeline metadata missing");
        lookups.pipeline = r.pipeline; lookups.pipe = &pipe->second;

        // The instance entry is at set 2 binding 0 in every pipeline of the game's characters.
        const auto& window = r.sets[2];
        if (window.dyn_count && uint64_t(window.dyn_first) + window.dyn_count > cb->dyn.size())
            return refuse(C_ENTRY, "character dynamic offsets incomplete");
        auto read_range = [&](const BoundRange* range, uint64_t offset, size_t size, uint8_t* output) {
            if (!range || offset > range->size || size > range->size - offset) return false;
            auto buffer = device->bufs.find(range->buffer);
            return buffer != device->bufs.end() && read_buffer_locked(device, buffer->second, range->off + offset, size, output) == SRC_MAPPED;
        };
        BoundRange entry_range;
        const BoundRange* instance = nullptr;
        if (bound_range(window, 2, 0, window.dyn_count ? cb->dyn.data() + window.dyn_first : nullptr, window.dyn_count, entry_range) &&
            (entry_range.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || entry_range.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))
            instance = &entry_range;
        if (!read_range(instance, 0, entry.size(), entry.data())) return refuse(C_ENTRY, "character instance entry is not mapped");
        identity = identify(entry, now, present);
        if (!identity) return refuse(C_IDENTITIES, "character position invalid or all identities in use");
        identity->seen |= 1ull << section->component;
        if (now > identity->update_frame) {
            std::string reason;
            if (!identity->skeleton.begin_update(now, reason)) return refuse(C_IMPORT, reason);
            identity->update_frame = now;
            acquire_ring(*identity); // the frame's merged skeleton on the device (D-117), or the CPU's
        }

        lap(S_IDENTIFY);
        // Import this component's poses into the identity's master, whatever happens to the draw.
        uint32_t offsets[2];
        std::memcpy(offsets, entry.data() + 80, 8);
        const uint8_t dynamic = (offsets[0] >= MergedSkeleton::kStaticRows ? 1 : 0) | (offsets[1] >= MergedSkeleton::kStaticRows ? 2 : 0);
        const BoneSite site = site_of(pipe->second);
        // On the device (D-127) every draw's component is imported in its own recording, whatever happens
        // to the draw; on the CPU the master is imported once per frame.
        gpu_draw = identity->gpu_frame && identity->ring;
        if (gpu_draw) {
            const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
            bone_owner = static_cast<CharacterCommand*>(held.get());
            if (!bone_owner) gpu_draw = false;
        }
        if (site.ok && dynamic && (gpu_draw || identity->skeleton.needs_import(0, section->component, lod))) {
            const bool remapped = lod == 1 && component.has_remap;
            uint32_t last = component.count - 1;
            if (remapped) last = *std::max_element(component.remap_lod1.begin(), component.remap_lod1.end());
            const size_t needed = (size_t(last) + 2) * MergedSkeleton::kBytesPerBone;
            current.resize(needed); previous.resize(needed);
            auto& bone_ranges = scratch_bone_ranges; bone_ranges.clear();
            const auto& bound = r.sets[site.set];
            bool readable = !(bound.dyn_count && uint64_t(bound.dyn_first) + bound.dyn_count > cb->dyn.size());
            if (readable) collect_buffer_ranges_locked(device, bound, bound.dyn_count ? cb->dyn.data() + bound.dyn_first : nullptr, bound.dyn_count, site.set, bone_ranges);
            const BoundRange* bones = nullptr;
            for (const auto& range : bone_ranges)
                if (range.element == 0 && range.binding == site.binding &&
                    (range.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || range.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) bones = &range;
            if (gpu_draw && !(bones && readable && prepare_bone_job(*identity, *bone_owner, section->component, component, remapped, needed, *bones, offsets, dynamic)))
                gpu_draw = false; // this draw imports on the CPU and binds the master
            if (gpu_draw) {
                identity->coverage[section->component] |= dynamic;
                ++identity->generation;
            } else if (identity->skeleton.needs_import(0, section->component, lod)) {
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                const uint64_t bones_read_began = host_time_us();
#endif
                readable = readable && (!(dynamic & 1) || read_range(bones, uint64_t(offsets[0]) * 16, needed, current.data())) &&
                           (!(dynamic & 2) || read_range(bones, uint64_t(offsets[1]) * 16, needed, previous.data()));
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                if (program) program->note_timing("bones read", host_time_us() - bones_read_began);
                const uint64_t bones_import_began = host_time_us();
#endif
                if (!readable) {
                    ++counters[C_BONES];
                } else {
                    std::string reason;
                    static const std::vector<uint16_t> identity_map;
                    const auto result = identity->skeleton.import_component(0, section->component, lod, entry,
                        {current.data(), needed}, {previous.data(), needed}, remapped ? component.remap_lod1 : identity_map, 1.0f, reason);
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                    if (program) program->note_timing("bones import", host_time_us() - bones_import_began);
#endif
                    if (result == BoneImportResult::Imported) {
                        identity->coverage[section->component] |= dynamic;
                        ++identity->generation;
                    } else if (result == BoneImportResult::Rejected) {
                        ++counters[C_IMPORT];
                        note("character bone import: " + reason);
                    }
                }
            }
        }

        lap(S_BONES);
        // As in the core: the object is the mod's once enough of its components were seen at this
        // place and a frame has passed. Until then the game draws it.
        if (!identity->custom) {
            uint32_t bits = 0;
            for (uint64_t value = identity->seen; value; value &= value - 1) ++bits;
            // The original Core's identification_min_components as the Mod sets it (EFMI exports: 3).
            const uint32_t wanted = program && program->identify_components() ? program->identify_components() : kIdentifyingComponents;
            if (bits >= std::min<uint32_t>(wanted, static_cast<uint32_t>(package.components.size())) && now > identity->first_frame)
                identity->custom = true;
            else {
                ++counters[C_IDENTIFYING];
                // As the Core (SpatialIdentification.ini): an object's first frame is skipped, not drawn as
                // the game's, so a character never shows the game's mesh before the Mod's (D-103); from
                // the next frame on an object still unidentified is the game's, as there, except while a
                // new object was never the Mod's yet (kFreshPresents).
                if (present == identity->first_present) return true;
                if (!identity->shown && present - identity->first_present < kFreshPresents) { ++counters[C_HELD]; return true; }
                if (!identity->shown) ++counters[C_ORIGINAL_FIRST];
                return false;
            }
        }
        list = &package.lists[component.lists[lod]];
        if (program) {
            efmi::frontend::RuntimeDraw request;
            request.hash = section->hash; request.count = r.count; request.first = r.first; request.base = r.vertex_offset;
            request.component = section->component; request.lod = lod; request.identity = identity->id; request.frame = now;
            request.mod = program->folder_index();
            request.instance_entry = entry.data(); request.instance_entry_size = entry.size();
            render_targets(pipe->second, request);
            // The size of colour target 0 for the list's rt_width and rt_height, as the original reads
            // them off the bound target (ProcessParamRTSize): none bound (a depth-only pass) answers -1,
            // as there; a bound one its image's size, or the pass's render area when the framebuffer's
            // views are not known (a Mod's `if rt_width != 0` guards every draw).
            if (request.targets_known && !(request.render_targets & 1)) {
                request.target_width = request.target_height = -2; // asked, nothing bound
            } else if (request.targets_known) {
                bool sized = false;
                if (cb->pass.info && cb->pass.info->known && cb->pass.recipe && pipe->second.recipe) {
                    const uint32_t sub = pipe->second.recipe->subpass;
                    if (sub < cb->pass.recipe->subpasses.size() && !cb->pass.recipe->subpasses[sub].colors.empty()) {
                        const uint32_t ci = cb->pass.recipe->subpasses[sub].colors[0].attachment;
                        if (ci < cb->pass.info->views.size() && cb->pass.info->views[ci].known) {
                            request.target_width = float(cb->pass.info->views[ci].image.extent.width);
                            request.target_height = float(cb->pass.info->views[ci].image.extent.height);
                            sized = true;
                        }
                    }
                }
                // The framebuffer's size (its attachments' at least), then the pass's render area.
                if (!sized && cb->pass.info && cb->pass.info->width && cb->pass.info->height) {
                    request.target_width = float(cb->pass.info->width); request.target_height = float(cb->pass.info->height);
                    sized = true;
                }
                if (!sized && cb->pass.area.extent.width && cb->pass.area.extent.height) {
                    request.target_width = float(cb->pass.area.offset.x + int32_t(cb->pass.area.extent.width));
                    request.target_height = float(cb->pass.area.offset.y + int32_t(cb->pass.area.extent.height));
                }
            }
            if (request.target_width == -1 && !target_size_logged) {
                target_size_logged = true;
                logf("character draw without a render target size: targets %s (mask 0x%x), pipeline recipe %s, render pass %s, "
                     "framebuffer %s, area %ux%u", request.targets_known ? "known" : "unknown", request.render_targets,
                     pipe->second.recipe ? "yes" : "no", pipe->second.recipe && pipe->second.recipe->render_pass ? "yes" : "no",
                     cb->pass.info ? "known" : "unknown", cb->pass.area.extent.width, cb->pass.area.extent.height);
            }
            // EFMITTL's lit vertex shader hook (D-113).
            if (translucency && program->efmittl()) { request.lit_vertex = lit_vertex(pipe->second); translucency->listen(); }
            request.seconds = ini_seconds();
            request.frame_seconds = frame_seconds.load(std::memory_order_relaxed);
            request.pixel_texture = [this, &r](unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why) {
                return pixel_texture(r, slot, texture, why); };
            request.texture_roles = [this, &r](int32_t roles[3], std::string& why) { return texture_roles(r, roles, why); };
            request.vertex_stride = [&pipe](unsigned slot, uint32_t& stride, std::string& why) { return vertex_stride(pipe->second, slot, stride, why); };
            const SourceFacts& facts = facts_of(pipe->second, site);
            request.source_vs_config_slot = facts.vs_config_slot;
            request.rolling_bone_source = facts.rolling_bones;
            request.source_srv = [&facts](wchar_t stage, unsigned slot, uint32_t& type, uint32_t& dimension, uint32_t& stride, std::string& why) {
                const wchar_t* names = L"vhdgp";
                const wchar_t* at = stage ? wcschr(names, stage) : nullptr;
                const auto* registers = at ? facts.registers[size_t(at - names)].get() : nullptr;
                if (!registers) { why = "the stage's SPIR-V registers are not known"; return false; }
                efmi::gpu::SourceSrvDeclaration declaration;
                if (!efmi::gpu::source_srv_declaration(*registers, slot, declaration, why)) return false;
                type = declaration.words ? efmi::frontend::kWordBufferDeclaration : declaration.type;
                dimension = declaration.dimension; stride = declaration.stride;
                return true;
            };
            bool skip = false, added = false, textures = false; std::string reason;
            if (!plan_draw(request, runtime_list, skip, added, reason, &pipe->second.recipe->vertex, &textures))
                return refuse(C_STATE, "INI plan: " + reason);
            if (!skip) {
                // The Mod's lists keep the game's draw (its own identification rejected the object, D-086).
                if (!textures) {
                    static std::mutex kept_mx; static std::set<uint32_t> kept_logged;
                    { std::lock_guard<std::mutex> hold(kept_mx); if (kept_logged.insert(section->hash).second) logf("character draw kept the game's: section %08x (count %u)", section->hash, section->match_count); }
                    // A new object the Mod has not drawn yet (its registration with the Core takes a
                    // Present): skipped rather than the game's mesh, for kFreshPresents at most.
                    if (!identity->shown && present - identity->first_present < kFreshPresents) { ++counters[C_HELD]; return true; }
                    if (!identity->shown) ++counters[C_ORIGINAL_FIRST];
                    ++counters[C_KEPT]; return false;
                }
                if (added && !refresh_materials()) return false;
                const bool textured = record_textures(cb, r, pipe->second, static_cast<uint32_t>(section - package.sections.data()), present, runtime_list);
                if (textured) identity->shown = true;
                return textured;
            }
            if (added && !refresh_materials()) return false;
            list = &runtime_list;
        }
        lap(S_PLAN);
        if (!list->count) { if (!gate_open) return gated(); ++counters[C_HIDDEN]; return true; }
        for (uint32_t c = 0; c < package.components.size(); ++c)
            if ((list->required >> c & 1) && (identity->coverage[c] & dynamic) != dynamic) { ++counters[C_INCOMPLETE]; return false; }
        // Under the tracker's lock (D-127): each component the list's vertices read comes from an import
        // this recording carries, from the object's skeleton that a submitted batch imported it into, or
        // from a recording that drew it and is not yet submitted (its bone data valid until that recording
        // completed; the tracker's id tells a reused buffer handle apart). A draw that would read bones no
        // import wrote is refused rather than drawn with them.
        if (gpu_draw)
            for (uint32_t c = 0; c < package.components.size() && c < 64; ++c) {
                if (!(list->required >> c & 1)) continue;
                if (bone_owner->has_bone_job(identity->ring.get(), c)) continue;
                if (identity->ring->submitted.load(std::memory_order_acquire) >> c & 1) continue;
                const BoneJob* source = c < identity->bone_sources.size() && identity->bone_sources[c].ring ? &identity->bone_sources[c] : nullptr;
                if (source && source->recording && source->recording->pending(source->epoch)) {
                    const auto alive = device->bufs.find(source->source);
                    if (alive != device->bufs.end() && alive->second.id == source->source_id) { borrowed_sources |= 1ull << c; continue; }
                }
                return refuse(C_BONES_UNAVAILABLE, "character draw reads bones of a component no device import wrote");
            }
        // The private set is bound right above the pipeline's own sets: slot 3 for most character
        // pipelines, slot 4 for the four-set ones (the game's visibility pass among them).
        const uint32_t own = static_cast<uint32_t>(pipe->second.layout->sets.size());
        if (own >= kMaxSets) {
            // Not a tracked slot: usable only if no pipeline layout of the application reaches it.
            if (device->max_layout_sets > own) return refuse(C_SET3, "character draw: the slot above the pipeline's sets is in use and not tracked");
        } else if (r.sets[own].pushed) {
            return refuse(C_SET3, "character draw with pushed descriptors at the private set's slot");
        } else if (r.sets[own].set) {
            // The tracker keeps a set only while later binds left it valid, so the layout it was
            // bound with agrees with the current one for the lower sets, and binding it again there
            // disturbs nothing. Vulkan keeps that layout alive while this recording lasts.
            const SetBind& app = r.sets[own];
            restore_slot = own;
            auto live = device->pipeline_layouts.find(app.layout_handle);
            if (!app.layout_handle || !app.layout || app.unsupported || live == device->pipeline_layouts.end() ||
                live->second != app.layout || (app.dyn_count && uint64_t(app.dyn_first) + app.dyn_count > cb->dyn.size()))
                return refuse(C_SET3, "character draw with an application set that cannot be bound again");
            restore_layout = app.layout_handle; restore_set = app.set;
            restore_offsets.assign(cb->dyn.begin() + app.dyn_first, cb->dyn.begin() + app.dyn_first + app.dyn_count);
        }

        // Restore exactly the vertex bindings the pipeline can read among the four tracked ones.
        uint32_t slots = 0;
        for (const auto& b : pipe->second.recipe->vertex.bindings) if (b.binding < kMaxVb) slots = std::max(slots, b.binding + 1);
        for (uint32_t slot = 0; slot < 4; ++slot)
            for (uint32_t i = 0; i < list->count; ++i)
                if (package.draws[list->first + i].vb[slot] != kCharacterNone) slots = std::max(slots, slot + 1);
        if (!slots || !r.ib) return refuse(C_BINDINGS, "character draw without tracked vertex or index bindings");
        for (uint32_t slot = 0; slot < slots; ++slot) {
            if (!r.vb[slot].buf) return refuse(C_BINDINGS, "character original vertex bindings incomplete");
            original.buffers.push_back(r.vb[slot].buf); original.offsets.push_back(r.vb[slot].off);
        }
        original.index = r.ib; original.index_offset = r.ib_off;
        original.index_type = r.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        bool pending = false;
        if (!material_draws(pipe->second, static_cast<uint32_t>(section - package.sections.data()), r, present, *list,
                            material_plans, material_views, material_origin)) return false;
        lap(S_MATERIALS);
        // EFMITTL's translucent draws (D-113): captured at the end of the pass when the library's
        // shaders, the layers and the pass's targets are there; otherwise drawn as before.
        capturing.assign(list->count, 0); capture_masks.assign(list->count, SnapshotImage{}); capture_t4.assign(list->count, 0);
        capture_sets = static_cast<uint32_t>(pipe->second.layout->sets.size());
        for (auto& v : scratch_captures) { v.clear(); v.resize(list->count); }
        bool any_translucent = false;
        for (uint32_t i = 0; i < list->count; ++i) any_translucent |= package.draws[list->first + i].translucent;
        if (any_translucent) {
            std::string why;
            const uint32_t own_set = static_cast<uint32_t>(pipe->second.layout->sets.size());
            const auto& ps = cb->pass;
            const RenderSubpass* sub = ps.recipe && pipe->second.subpass < ps.recipe->subpasses.size() ? &ps.recipe->subpasses[pipe->second.subpass] : nullptr;
            const TargetView* target = sub && !sub->colors.empty() && ps.info && sub->colors[0].attachment < ps.info->views.size()
                                           ? &ps.info->views[sub->colors[0].attachment] : nullptr;
            capture_layers.reset();
            const bool ready = translucency && program && program->efmittl() && target && target->known &&
                               translucency->shaders(program->library_folder("efmittl"), own_set, why) &&
                               (capture_layers = translucency->prepare(target->image.extent.width, target->image.extent.height, why)) != nullptr;
            if (!ready && !why.empty()) note(why);
            for (uint32_t i = 0; ready && i < list->count; ++i) {
                const CharacterDraw& d = package.draws[list->first + i];
                if (!d.translucent) continue;
                capturing[i] = 1;
                if (d.mask_image != kCharacterNone) {
                    if (!textures || d.mask_image >= image_ids.size()) return refuse(C_IMAGE, "translucency mask outside the package's images");
                    auto& mask_bindings = scratch_image_bindings;
                    frame_image(image_ids[d.mask_image]);
                    textures->bind({image_ids[d.mask_image]}, mask_bindings);
                    if (mask_bindings.empty() || mask_bindings[0].failed) return refuse(C_IMAGE, "translucency mask: " + (mask_bindings.empty() ? std::string() : mask_bindings[0].reason));
                    if (!mask_bindings[0].view) { gate_open = false; ++counters[C_IMAGE_PENDING]; return false; }
                    capture_masks[i] = {mask_bindings[0].view, mask_bindings[0].owner};
                }
                // The game's screen-space mask (ps-t4) is replaced by the neutral one while captured.
                const FragmentRegisters* registers = nullptr;
                {
                    auto known = fragment_registers.find(pipe->second.id);
                    if (known == fragment_registers.end()) {
                        FragmentRegisters fresh; fresh.readable = true;
                        for (const auto& stage : pipe->second.recipe->stages)
                            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                                std::string ignored; fresh.readable = efmi::gpu::source_registers(stage.shader->code, fresh.list, ignored);
                            }
                        if (fragment_registers.size() >= 8192) fragment_registers.clear();
                        known = fragment_registers.emplace(pipe->second.id, std::move(fresh)).first;
                    }
                    registers = &known->second;
                }
                if (registers->readable)
                    if (const auto* reg = efmi::gpu::find_source_register(registers->list, 't', 4);
                        reg && reg->kind == efmi::gpu::SourceRegister::ImageSrv && reg->count == 1 && reg->dim == 1 &&
                        image_counts.count(uint32_t(material_views[i].size()) + 1)) {
                        bool patched = false;
                        for (const auto& image : material_plans[i]->patch.images) patched |= image.set == reg->set && image.binding == reg->binding;
                        for (const auto& image : material_plans[i]->patch.nulls) patched |= image.set == reg->set && image.binding == reg->binding;
                        if (!patched) { capture_t4[i] = 1; capture_t4_binding = {reg->set, reg->binding}; }
                    }
            }
        }
        struct WaitingVariant { uint32_t draw; VertexInputRecipe vertex; };
        std::vector<WaitingVariant> waiting; // Allocates only for a cold variant.
        variants.resize(list->count);
        // While a discard gate is set (D-133) the draw's fragment stage goes without the character dither,
        // as the application's own draws of the pipeline do through its twin. The other state is prepared
        // with the first, so the key finds it made.
        const bool dithered = g_dither_gated.load(std::memory_order_relaxed) && pipeline_dither(pipe->second.id, *pipe->second.recipe);
        const bool no_dither = dithered && g_dither_off.load(std::memory_order_relaxed);
        auto patch_of = [&](uint32_t i) -> const FragmentPatch& {
            if (!no_dither) return material_plans[i]->patch;
            if (scratch_patches.size() < list->count) scratch_patches.resize(list->count);
            scratch_patches[i] = material_plans[i]->patch; scratch_patches[i].no_dither = true;
            return scratch_patches[i];
        };
        auto keep_variant = [&](uint32_t i, const VariantResult& result) {
            if (!result.variant) return false;
            variants[i] = result.variant;
            if (variant_lookup.size() < 65536)
                variant_lookup.emplace(std::make_tuple(pipe->second.id, list->first + i, material_plans[i], no_dither), result.variant);
            return true;
        };
        for (uint32_t i = 0; i < list->count; ++i) {
            if (capturing[i]) {
                VertexInputRecipe vertex;
                std::string reason;
                if (!package.vertex_input(package.draws[list->first + i], pipe->second.recipe->vertex, vertex, reason))
                    return refuse(C_VERTEX, "character vertex declaration: " + reason);
                const uint32_t own_set = static_cast<uint32_t>(pipe->second.layout->sets.size());
                for (uint32_t k = 0; k < kCaptureKinds; ++k) {
                    const auto recipe = translucency->recipe(pipe->second, CaptureKind(k), own_set, reason);
                    if (!recipe) return refuse(C_VARIANT, "translucent capture: " + reason);
                    FragmentPatch patch;
                    if (k < 2) { patch = material_plans[i]->patch; patch.no_dither = no_dither; if (capture_t4[i]) patch.images.push_back({capture_t4_binding.first, capture_t4_binding.second}); }
                    else patch.replacement_images = 2;
                    const auto result = cache->request(capture_identity(pipe->second.id, CaptureKind(k)), recipe, pipe->second.layout, vertex, patch);
                    if (k < 2 && dithered) cache->prepare(capture_identity(pipe->second.id, CaptureKind(k)), recipe, pipe->second.layout, vertex, other_state(patch));
                    if (result.variant) { scratch_captures[k][i] = result.variant; continue; }
                    if (result.state == VariantState::Queued || result.state == VariantState::Building) {
                        waiting_captures.push_back({i, k, vertex, recipe, std::move(patch)}); continue;
                    }
                    return refuse(C_VARIANT, "translucent capture variant: " + result.reason);
                }
                continue;
            }
            const auto lookup_key = std::make_tuple(pipe->second.id, list->first + i, material_plans[i], no_dither);
            if (const auto known = variant_lookup.find(lookup_key); known != variant_lookup.end()) { variants[i] = known->second; continue; }
            VertexInputRecipe vertex;
            std::string reason;
            if (!package.vertex_input(package.draws[list->first + i], pipe->second.recipe->vertex, vertex, reason))
                return refuse(C_VERTEX, "character vertex declaration: " + reason);
            // Issue all cold requests before waiting so the existing builders can run in parallel.
            const auto result = cache->request(pipe->second.id, pipe->second.recipe, pipe->second.layout, vertex, patch_of(i));
            if (dithered) cache->prepare(pipe->second.id, pipe->second.recipe, pipe->second.layout, vertex, other_state(patch_of(i)));
            if (keep_variant(i, result)) continue;
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { waiting.push_back({i, std::move(vertex)}); continue; }
            return refuse(C_VARIANT, "character variant: " + result.reason);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        for (const auto& request : waiting) {
            const auto remaining = std::max(std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            const auto result = cache->await(pipe->second.id, pipe->second.recipe, pipe->second.layout,
                request.vertex, patch_of(request.draw), remaining);
            if (keep_variant(request.draw, result)) continue;
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { pending = true; continue; }
            return refuse(C_VARIANT, "character variant: " + result.reason);
        }
        for (const auto& request : waiting_captures) {
            const auto remaining = std::max(std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            const auto result = cache->await(capture_identity(pipe->second.id, CaptureKind(request.kind)), request.recipe, pipe->second.layout,
                                             request.vertex, request.patch, remaining);
            if (result.variant) { scratch_captures[request.kind][request.draw] = result.variant; continue; }
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { pending = true; continue; }
            return refuse(C_VARIANT, "translucent capture variant: " + result.reason);
        }
        waiting_captures.clear();
        if (pending) { gate_open = false; frame_variants_pending = true; ++counters[C_PENDING]; return false; }
        // Images and variants were asked for above, so a closed frame still brings them up.
        if (!gate_open) return gated();
        std::string reason;
        if (!identity->skeleton.patch_entry(0, entry, patched, reason)) return refuse(C_IMPORT, reason);
    }

    lap(S_VARIANTS);
    // A recording that holds work of other replacement code keeps it to its end. What that code
    // left in a command buffer from earlier recordings gives way.
    // Every character package that replaces draws of a recording has its own owner in it.
    const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
    if (!held) return refuse(C_EXHAUSTED, "character snapshot pool is at capacity or refilling");
    CharacterCommand* const command_owner = static_cast<CharacterCommand*>(held.get());
    command_owner->used = true;
    const auto mesh_owner = std::static_pointer_cast<const GpuMesh>(command_owner->keep);
    if (!mesh_owner) return refuse(C_EXHAUSTED, "character command owner has no mesh");
    // The object's snapshot this recording's draws read (D-127): copied in the recording's batch.
    uint32_t snapshot_slot = UINT32_MAX;
    if (gpu_draw) {
        snapshot_slot = command_owner->snapshot(identity->ring);
        if (snapshot_slot == UINT32_MAX) return refuse(C_BONE_RING, "character bone snapshots are all taken");
    }
    lap(S_OWNER);
    // Everything is decided; from here nothing can fail after a command was emitted.
    auto& geometry = scratch_geometry; if (geometry.size() < list->count) geometry.resize(list->count);
    auto& items = scratch_items; items.clear();
    scratch_pending_captures.clear(); // nothing of an earlier refused draw goes to the queue
    const std::vector<uint8_t>& master = identity->skeleton.bytes();
    const uint64_t key = identity->id << 32 ^ identity->generation;
    const AppendBones append_bones = [&](DrawSnapshots& arena, const std::vector<SnapshotImage>& views, SnapshotSlot& out) {
        if (!gpu_draw) return arena.append_shared(master.data(), master.size(), key, patched, views, out);
        const auto& ring = identity->ring;
        if (!arena.append_external(ring->buffer(snapshot_slot), ring->offset(snapshot_slot), ring->slot_bytes, patched, views, out)) return false;
        // A component only a recording not yet submitted imported: this recording takes that import, which
        // never replaces one of its own (D-127).
        if (borrowed_sources) {
            uint64_t taken = 0;
            for (uint32_t c = 0; c < 64; ++c)
                if (borrowed_sources >> c & 1) { command_owner->add_bone_job(identity->bone_sources[c]); ++taken; }
            borrowed_sources = 0;
            if (bones_gpu) bones_gpu->note_borrowed(taken);
        }
        return true;
    };
    for (uint32_t i = 0; i < list->count; ++i) {
        const CharacterDraw& d = package.draws[list->first + i];
        SnapshotSlot slot;
        const std::vector<SnapshotImage>* slot_views = &material_views[i];
        if (capturing[i] && capture_t4[i]) {
            scratch_capture_views = material_views[i]; scratch_capture_views.push_back(Translucency::neutral_view(capture_layers));
            if (!scratch_capture_views.back().view) return refuse(C_IMAGE, "translucency layers missing");
            slot_views = &scratch_capture_views;
        }
        const uint32_t count = static_cast<uint32_t>(slot_views->size());
        auto arena = count ? command_owner->material_snapshots.at(count) : command_owner->snapshots;
        const uint32_t slices_before = arena->slices_used();
        const uint64_t append_began = host_time_us();
        if (!append_bones(*arena, *slot_views, slot)) return refuse(C_EXHAUSTED, arena->reason);
        stage_us[S_APPEND] += host_time_us() - append_began;
        if (arena->slices_used() != slices_before) ++counters[C_SNAPSHOT_COPIES];
        DrawGeometry& g = geometry[i];
        g = original;
        for (uint32_t s = 0; s < g.buffers.size(); ++s) {
            if (d.vb[s] == kCharacterOwned) {
                // Checked by vertex_input above: the slot has its owned buffer.
                const auto* o = d.owned_stream(s);
                g.buffers[s] = reinterpret_cast<VkBuffer>(o->buffer); g.offsets[s] = o->offset;
                command_owner->owned.push_back(o->keep); ++counters[C_OWNED];
            } else if (d.vb[s] != kCharacterNone) { g.buffers[s] = mesh_owner->buffer; g.offsets[s] = package.streams[d.vb[s]].offset; }
        }
        // A non-indexed draw (D-142) leaves the application's index binding in place, unused.
        if (d.indexed) {
            const auto& index = package.streams[d.index_stream];
            g.index = mesh_owner->buffer; g.index_offset = index.offset;
            g.index_type = index.stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
        IndexedDraw args; args.count = d.count; args.first = d.first; args.vertex_offset = d.base;
        args.instances = d.instances; args.first_instance = d.first_instance; args.indexed = d.indexed;
        if (capturing[i]) {
            if (!capture(cb, r, capture_sets, d, i, slot, g, args, append_bones, command_owner)) return false;
            continue;
        }
        items.push_back({variants[i].get(), slot.set, slot.offset, &g, args});
    }
    // The captures go to the pass's queue once every draw of the list was decided.
    for (auto& c : scratch_pending_captures) {
        std::string why;
        if (!translucency->queue(cb, std::move(c), why)) { scratch_pending_captures.clear(); return refuse(C_STATE, why); }
        ++counters[C_TRANSLUCENT];
    }
    scratch_pending_captures.clear();
    lap(S_SNAPSHOT);
    command_owner->variants.insert(command_owner->variants.end(), variants.begin(), variants.end());
    if (!emit_variant_draws(draw, cb->handle, items.data(), items.size(), r.bound ? r.bound : r.pipeline, original))
        return refuse(C_VARIANT, "character draw transaction refused");
    if (restore_layout) {
        draw.CmdBindDescriptorSets(cb->handle, VK_PIPELINE_BIND_POINT_GRAPHICS, restore_layout, restore_slot, 1, &restore_set,
                                   static_cast<uint32_t>(restore_offsets.size()), restore_offsets.empty() ? nullptr : restore_offsets.data());
        ++counters[C_SET3_RESTORED];
    }
    lap(S_EMIT);
    ++counters[C_REPLACED];
    if (identity) identity->shown = true;
    if (material_origin) ++counters[material_origin == 1 ? C_DERIVED : C_INHERITED];
    counters[C_ITEMS] += list->count;
    for (const auto* plan : material_plans) {
        if (!plan->patch.images.empty() || !plan->patch.nulls.empty()) ++counters[C_MATERIAL_ITEMS];
        if (plan->compatibility) ++counters[C_COMPAT_ITEMS];
    }
    return true;
}

bool CharacterGeneration::record_textures(CmdBuf* cb, const DrawRec& r, const PipeInfo& pipe, uint32_t section, uint64_t present,
                                          const CharacterList& list)
{
    if (list.count != 1 || !package.draws[list.first].texture_only) return refuse(C_STATE, "texture-only list expected");
    if (cb->secondary || !cb->in_rp || cb->dyn_rp_count || cb->m2_extended_bind)
        return refuse(C_STATE, "texture-only draw outside a primary classic render pass or after extended binds");
    std::vector<const StaticMaterial*> plans;
    std::vector<std::vector<SnapshotImage>> views;
    uint32_t origin = 0;
    if (!material_draws(pipe, section, r, present, list, plans, views, origin)) return false;
    // Nothing the module can take: the application's draw stays as it is.
    if (plans.size() != 1 || (plans[0]->patch.images.empty() && plans[0]->patch.nulls.empty())) return false;
    // The private set above the pipeline's own sets, and an application set there bound again after.
    uint32_t restore_slot = 0;
    VkPipelineLayout restore_layout = VK_NULL_HANDLE;
    VkDescriptorSet restore_set = VK_NULL_HANDLE;
    std::vector<uint32_t> restore_offsets;
    const uint32_t own = static_cast<uint32_t>(pipe.layout->sets.size());
    if (own >= kMaxSets) {
        if (device->max_layout_sets > own) return refuse(C_SET3, "texture-only draw: the slot above the pipeline's sets is in use and not tracked");
    } else if (r.sets[own].pushed) {
        return refuse(C_SET3, "texture-only draw with pushed descriptors at the private set's slot");
    } else if (r.sets[own].set) {
        const SetBind& app = r.sets[own];
        restore_slot = own;
        auto live = device->pipeline_layouts.find(app.layout_handle);
        if (!app.layout_handle || !app.layout || app.unsupported || live == device->pipeline_layouts.end() ||
            live->second != app.layout || (app.dyn_count && uint64_t(app.dyn_first) + app.dyn_count > cb->dyn.size()))
            return refuse(C_SET3, "texture-only draw with an application set that cannot be bound again");
        restore_layout = app.layout_handle; restore_set = app.set;
        restore_offsets.assign(cb->dyn.begin() + app.dyn_first, cb->dyn.begin() + app.dyn_first + app.dyn_count);
    }
    // The application's own geometry: the vertex bindings the pipeline reads among the tracked ones,
    // bound again as they are; higher ones are not touched and stay the application's.
    DrawGeometry original;
    uint32_t slots = 0;
    for (const auto& b : pipe.recipe->vertex.bindings) if (b.binding < kMaxVb) slots = std::max(slots, b.binding + 1);
    if (!r.ib) return refuse(C_BINDINGS, "texture-only draw without a tracked index binding");
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (!r.vb[slot].buf) return refuse(C_BINDINGS, "texture-only draw's vertex bindings incomplete");
        original.buffers.push_back(r.vb[slot].buf); original.offsets.push_back(r.vb[slot].off);
    }
    if (original.buffers.empty()) return refuse(C_BINDINGS, "texture-only draw without vertex bindings");
    original.index = r.ib; original.index_offset = r.ib_off;
    original.index_type = r.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    FragmentPatch patch = plans[0]->patch;
    patch.keep_vertex = true;
    const bool dithered = g_dither_gated.load(std::memory_order_relaxed) && pipeline_dither(pipe.id, *pipe.recipe);
    patch.no_dither = dithered && g_dither_off.load(std::memory_order_relaxed);
    const auto result = cache->await(pipe.id, pipe.recipe, pipe.layout, pipe.recipe->vertex, patch, std::chrono::milliseconds(4000));
    if (dithered) cache->prepare(pipe.id, pipe.recipe, pipe.layout, pipe.recipe->vertex, other_state(patch));
    if (!result.variant) {
        if (result.state == VariantState::Queued || result.state == VariantState::Building) { ++counters[C_PENDING]; return false; }
        return refuse(C_VARIANT, "texture-only variant: " + result.reason);
    }
    // Every character package that replaces draws of a recording has its own owner in it.
    const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
    if (!held) return refuse(C_EXHAUSTED, "character snapshot pool is at capacity or refilling");
    CharacterCommand* const command_owner = static_cast<CharacterCommand*>(held.get());
    command_owner->used = true;
    const uint32_t count = static_cast<uint32_t>(views[0].size());
    auto arena = count ? command_owner->material_snapshots.at(count) : command_owner->snapshots;
    static const InstanceEntry unused{};
    SnapshotSlot slot;
    if (!arena->append_images(unused, views[0], slot)) return refuse(C_EXHAUSTED, arena->reason);
    IndexedDraw args;
    args.count = r.count; args.instances = r.instances; args.first = r.first; args.vertex_offset = r.vertex_offset;
    args.first_instance = r.first_instance;
    const VariantDraw item{result.variant.get(), slot.set, slot.offset, &original, args};
    command_owner->variants.push_back(result.variant);
    if (!emit_variant_draws(draw, cb->handle, &item, 1, r.bound ? r.bound : r.pipeline, original))
        return refuse(C_VARIANT, "texture-only draw transaction refused");
    if (restore_layout) {
        draw.CmdBindDescriptorSets(cb->handle, VK_PIPELINE_BIND_POINT_GRAPHICS, restore_layout, restore_slot, 1, &restore_set,
                                   static_cast<uint32_t>(restore_offsets.size()), restore_offsets.empty() ? nullptr : restore_offsets.data());
        ++counters[C_SET3_RESTORED];
    }
    ++counters[C_TEXTURED];
    if (origin) ++counters[origin == 1 ? C_DERIVED : C_INHERITED];
    ++counters[C_MATERIAL_ITEMS];
    if (plans[0]->compatibility) ++counters[C_COMPAT_ITEMS];
    return true;
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

VkResult generation_submit(CharacterGeneration& backend, VkQueue queue)
{
    ThreadTrace* const trace = backend.device->trace ? backend.device->trace() : nullptr;
    // Owned GPU work of replaced draws runs before the application's work on the queue (D-087).
    if (backend.presenter) { StageMark stage(trace, "character draw work submission (generation lock)"); backend.submit_draw_work(queue); }
    return VK_SUCCESS;
}

std::shared_ptr<CharacterGeneration> generation_create(Device* device, const VkPhysicalDeviceProperties& properties,
                                                   const VkPhysicalDeviceMemoryProperties& memory,
                                                   const std::string& path, bool lazy, TextureResidency* textures,
                                                   std::shared_ptr<VariantCache> variants, Translucency* translucency, BoneMerge* bones,
                                                   CharacterGeneration* predecessor = nullptr)
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

bool generation_record(CharacterGeneration& backend, CmdBuf* cb, const DrawRec& r, uint32_t section)
{
    try { return backend.record(cb, r, section); }
    catch (const std::exception& e) { ++backend.counters[C_STATE]; backend.note(e.what()); return false; }
}

void generation_retire(CharacterGeneration& backend, uint32_t pipeline_id)
{
    std::lock_guard<std::mutex> lock(backend.mod_mx);
    // The fast lookup is another strong owner in addition to the device cache (BUG-041).
    // Recorded commands retain their variants independently through GPU completion.
    for (auto it = backend.variant_lookup.begin(); it != backend.variant_lookup.end();) {
        if (std::get<0>(it->first) == pipeline_id) it = backend.variant_lookup.erase(it);
        else ++it;
    }
    backend.sites.erase(pipeline_id);
    backend.material_profiles.erase(pipeline_id);
    backend.derived_for_pipeline.erase(pipeline_id);
    backend.fragment_registers.erase(pipeline_id);
    backend.source_facts.erase(pipeline_id);
}

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

void generation_enable(CharacterGeneration&, bool);
void generation_status(CharacterGeneration&, JsonWriter&);

// D-131 (docs/folder-generation.md): the Mods folder's one INI generation and the packages carved from
// it, one per character Mod it could take. The generation's Present, saves and F10 are the folder's.
struct FolderGeneration {
    Device* device = nullptr;
    std::vector<std::shared_ptr<CharacterGeneration>> packages; // in the folder's order
    std::vector<std::pair<std::string, std::string>> left_out;   // a Mod's INI and why it has no package
    std::shared_ptr<OwnedPresenter> presenter;
    std::shared_ptr<std::mutex> plan_mx = std::make_shared<std::mutex>();
    CharacterProgram* program = nullptr;        // the shared generation, through one package's view of it
    std::shared_ptr<CharacterGeneration> owner; // that package
    std::atomic<uint64_t> frame{1}, presents{0};
    std::atomic<float> frame_seconds{1.0f / 60};
    uint64_t last_frame_us = 0, save_checked_us = host_time_us();
    std::vector<efmi::frontend::IniRuntime::DiscardGate> gates; // D-133, under plan_mx
    std::future<void> saving;
    bool empty = false; // the folder holds no character Mod: a valid generation of no package
    std::vector<std::pair<std::string, std::string>> left_inputs; // inputs of no character Mod the folder was loaded without, and why
    std::vector<std::string> unmatched;                            // enabled INIs with mesh sections that are no character Mod's
    uint32_t gaps_seen = 0; size_t gaps_shown = 0;                 // LayerGap texts logged and shown, on the presenting thread
    // What does not show, on the screen as in the log (docs/architecture-review-2.1.md, R-11): a folder
    // that did not load, the Mods and inputs left out with their reasons, and INIs whose mesh sections
    // match no draw because they are no EFMI character Mod. At the first load and after every exchange.
    void announce() const
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
        for (const auto& ini : unmatched)
            notice(NoticeLevel::Notice, ("unmatched " + ini).c_str(),
                   ("EFMI Vulkan: " + mod(ini) + ": its mesh sections replace nothing (no [CommandListInitializeMergedSkeleton]: not an EFMI character Mod)").c_str());
        if (program && (!left_out.empty() || !left_inputs.empty()))
            notice(NoticeLevel::Info, "", ("EFMI Vulkan: " + std::to_string(packages.size()) + " Mods loaded, " +
                                           std::to_string(left_out.size() + left_inputs.size()) + " left out").c_str());
    }
    std::string reason, last_note;
    std::mutex note_mx;
    void note(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(note_mx);
        if (text == last_note) return;
        last_note = text; logf("folder generation: %s", text.c_str());
    }
    void save()
    {
        if (saving.valid() && saving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        if (saving.valid()) saving.get();
        auto keep = owner;
        saving = std::async(std::launch::async, [keep] {
            std::string error;
            if (keep && keep->program && !keep->program->save(error)) logf("folder generation settings: %s", error.c_str());
        });
    }
    // Off the presenting thread: every package stops, then the owned runtime; the settings once. Once,
    // whoever asks first (the retirement after an exchange, the device going); the others wait for it.
    std::once_flag stop_once;
    void stop() { std::call_once(stop_once, [this] { stop_now(); }); }
    void stop_now()
    {
        if (saving.valid()) saving.get();
        for (auto& p : packages) if (p) p->stop();
        if (presenter) presenter->stop();
        if (program) { std::string error; if (!program->save(error)) logf("folder generation shutdown settings: %s", error.c_str()); }
    }
};

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
    try { for (const auto& ini : CharacterProgram::unmatched_mesh_inis(source.mods)) folder->unmatched.push_back(ini.u8string()); }
    catch (const std::exception& e) { logf("folder generation: INIs with mesh sections not listed: %s", e.what()); }
    const auto began = host_time_us();
    {
        // Every character Mod gone (removed, disabled): the original's reload gives a configuration
        // without them, and so does this one.
        std::string none;
        if (CharacterProgram::mods_characters(source.mods, none).empty()) {
            folder->empty = true; folder->reason = none.empty() ? "the Mods folder holds no character Mod" : none;
            logf("folder generation: %s", folder->reason.c_str());
            return folder;
        }
    }
    if (!CharacterProgram::discover_folder(source, mods, folder->reason, gpu)) {
        logf("folder generation refused: %s", folder->reason.c_str());
        for (auto& m : mods) if (!m.reason.empty()) folder->left_out.push_back({m.ini.u8string(), m.reason});
        return folder;
    }
    for (auto& m : mods)
        if (m.program && m.program->gpu()) { folder->presenter = std::make_shared<OwnedPresenter>(device, m.program->gpu()); break; }
    // The discard gates as the load left them (D-133); the live generation's reach the draws (BUG-070:
    // 2.0.7 set them from a reload candidate while it loaded).
    for (auto& m : mods)
        if (m.program) { folder->gates = m.program->discard_gates(); break; }
    for (auto& m : mods)
        if (m.program) {
            if (!folder->presenter && !m.program->gpu_reason().empty()) logf("owned GPU work unavailable: %s", m.program->gpu_reason().c_str());
            // What the original overlay would show after a load; the generation carries on (D-067).
            const auto loaded = m.program->status();
            if (loaded.diagnostics) logf("INI loaded with %zu original warning(s)", loaded.diagnostics);
            for (const auto& text : loaded.diagnostic_messages) logf("INI warning: %s", text.c_str());
            break;
        }
    for (auto& m : mods) {
        const auto path = m.ini.u8string();
        if (!m.program) {
            folder->left_out.push_back({path, m.reason});
            logf("character package refused: %s: %s", path.c_str(), m.reason.c_str());
            continue;
        }
        auto package = std::make_shared<CharacterGeneration>(device, properties, memory, path, true, textures, variants, translucency, bones);
        CharacterProgram* view = m.program.get();
        if (!package->init_adopted(std::move(m.program), std::move(m.package), folder->presenter, folder->plan_mx)) {
            folder->left_out.push_back({path, package->last_reason});
            logf("character package refused: %s: %s", path.c_str(), package->last_reason.c_str());
            package->abandon();
            continue;
        }
        if (!folder->program) { folder->program = view; folder->owner = package; }
        folder->packages.push_back(std::move(package));
    }
    for (const auto& item : left_inputs) {
        folder->left_inputs.push_back({item.first.u8string(), item.second});
        logf("folder generation: input left out: %s: %s", item.first.u8string().c_str(), item.second.c_str());
    }
    for (const auto& ini : folder->unmatched) logf("folder generation: mesh sections that match no draw (no character Mod): %s", ini.c_str());
    logf("folder generation: %zu character Mods, %zu left out, loaded in %llu ms", folder->packages.size(), folder->left_out.size(),
         static_cast<unsigned long long>((host_time_us() - began) / 1000));
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
    void retire(std::shared_ptr<CharacterGeneration> old)
    {
        if (old) retiring.push_back(std::async(std::launch::async, [old = std::move(old)] {
            try { old->stop(); } catch (const std::exception& e) { logf("character generation retirement: %s", e.what()); }
        }));
    }
    // Device teardown: everything stops here, waiting for what it must.
    void stop()
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
    void stop_later()
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
    bool revive(std::shared_ptr<CharacterGeneration> package)
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
    void frame(bool had_draws, PresentWork* present, const efmi::frontend::RuntimeInput& input)
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
    void retire_folder(std::shared_ptr<FolderGeneration> old)
    {
        if (old) folder_retiring.push_back(std::async(std::launch::async, [old = std::move(old)] {
            try { old->stop(); }
            catch (const std::exception& e) { logf("folder generation retirement: %s", e.what()); }
        }));
    }
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
    void open_pipeline_cache()
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
    void close_pipeline_cache()
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
    void rebuild_index()
    {
        auto next = std::make_shared<Index>();
        for (uint32_t i = 0; i < controllers.size(); ++i) {
            auto& c = *controllers[i];
            std::shared_lock<std::shared_mutex> lock(c.mutex);
            next->generations.push_back(c.generation);
            const bool active_now = !c.stopped && c.active && c.active->activated.load(std::memory_order_relaxed);
            next->activated.push_back(active_now ? 1 : 0);
            if (c.stopped || !c.active) continue;
            const auto& sections = c.active->package.sections;
            for (uint32_t s = 0; s < sections.size(); ++s) {
                next->by_hash[sections[s].hash].push_back({i, s});
                // A hash-only section (a hidden game mesh) acts for an activated package only (D-084), so
                // only such a package makes every indexed draw hashed (D-107): two lazy packages with one
                // each had every draw of the frame hashed, 147 misses a frame in the seventh session.
                if (sections[s].match_count == kCharacterNone) { if (active_now) next->hash_only = true; }
                else next->counts.insert(sections[s].match_count);
            }
        }
        std::unique_lock<std::shared_mutex> lock(index_mx);
        index = std::move(next);
    }
    bool index_stale()
    {
        std::shared_ptr<const Index> current;
        { std::shared_lock<std::shared_mutex> lock(index_mx); current = index; }
        if (!current || current->generations.size() != controllers.size()) return true;
        for (uint32_t i = 0; i < controllers.size(); ++i) {
            std::shared_lock<std::shared_mutex> lock(controllers[i]->mutex);
            if (controllers[i]->generation != current->generations[i]) return true;
            const auto& c = *controllers[i];
            const bool active_now = !c.stopped && c.active && c.active->activated.load(std::memory_order_relaxed);
            if (active_now != (current->activated[i] != 0)) return true;
        }
        return false;
    }
    ~CharacterBackend()
    {
        if (twins_sweep.valid()) twins_sweep.get();
        for (auto& c : controllers) c->stop();
        if (folder_loading.valid()) { auto candidate = folder_loading.get(); if (candidate) candidate->stop(); }
        if (folder_staged) folder_staged->stop();
        if (folder) folder->stop();
        for (auto& f : folder_retiring) f.get();
        if (textures) textures->stop(); if (variants) variants->stop(); close_pipeline_cache();
    }
};

namespace {
// The character Mods of an EFMI Mods folder (D-078).
constexpr size_t kMaxPackages = 4096; // the folder generation's bound (D-141)
std::vector<std::string> mods_characters(std::string& reason)
{
    std::vector<std::string> found;
    for (const auto& path : CharacterProgram::mods_characters(mods_folder(), reason)) found.push_back(path.u8string());
    if (found.size() > kMaxPackages) { found.resize(kMaxPackages); logf("character Mods: more than 4096 in %s, the first 4096 are used", mods_folder().u8string().c_str()); }
    if (found.empty() && reason.empty()) reason = "no character Mod in " + mods_folder().u8string();
    return found;
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
}

void prepare_twins(CharacterBackend& b);
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
    for (auto& c : b.controllers) c->stop();
    if (b.folder_loading.valid()) { auto candidate = b.folder_loading.get(); if (candidate) candidate->stop(); }
    if (b.folder_staged) { b.folder_staged->stop(); b.folder_staged.reset(); }
    if (b.folder) b.folder->stop();
    for (auto& f : b.folder_retiring) f.get();
    b.folder_retiring.clear();
    if (b.textures) b.textures->stop(); if (b.variants) b.variants->stop(); b.close_pipeline_cache();
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
            if (current->owner && current->program && !current->program->save(reason)) logf("folder generation settings before reload: %s", reason.c_str());
            return folder_create(d, p, m, textures, variants, translucency, bones);
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
// Caller holds res_mx. The index bytes of one draw, hashed once for every package of the device
// (D-097) and then recognized by a short fingerprint; the full hash is repeated now and then because
// mapped pools change unseen. Each recording thread keeps its own table (above).
// The fingerprint is two uncached lines read over the bus (most of a draw's cost in the 1.6.8 session,
// D-099): it is read every draw only for a region whose hash a package acts on, where a stale hash
// would replace the wrong object; for the others every kQuickFrames, a stale hash there delays a
// match by that much at most.
constexpr uint64_t kQuickFrames = 16;
bool index_region_hash(Device* device, const DrawRec& r, uint64_t now, const CharacterBackend::Index& index, uint32_t& hash)
{
    thread_local IndexHashTable table;
    auto ib = device->bufs.find(r.ib);
    if (ib == device->bufs.end() || (r.index_size != 2 && r.index_size != 4)) return false;
    const uint64_t start = uint64_t(r.first) * r.index_size, bytes = uint64_t(r.count) * r.index_size;
    const BufInfo& info = ib->second;
    if (r.ib_off > info.size || start > info.size - r.ib_off || bytes > info.size - r.ib_off - start || bytes > (64u << 20)) return false;
    const uint64_t offset = r.ib_off + start;
    const IndexKey key = {r.ib, offset, static_cast<uint32_t>(bytes)};
    // The region's generation: the newest tracked write overlapping it (D-099), not the buffer's.
    const uint64_t region_gen = info.region_gen(offset, bytes);
    auto found = table.hashes.find(key);
    const bool current = found != table.hashes.end() && found->second.buffer_id == info.id && found->second.gen == region_gen &&
                         now < found->second.next_full;
    // Stable bytes are not proof that a coherent mapped pool cannot be rewritten (BUG-040).
    // Positive matches must still inspect the fingerprint on every draw.
    if (current && now < found->second.next_quick && !index.by_hash.count(found->second.hash)) {
        found->second.seen = now; hash = found->second.hash; ++g_quick_skipped;
        return true;
    }
    uint8_t edge[128];
    const size_t head = static_cast<size_t>(std::min<uint64_t>(bytes, 64)), tail = static_cast<size_t>(std::min<uint64_t>(bytes - head, 64));
    const uint64_t read_began = host_time_us();
    if (read_buffer_locked(device, info, offset, head, edge) != SRC_MAPPED ||
        (tail && read_buffer_locked(device, info, offset + bytes - tail, tail, edge + head) != SRC_MAPPED)) return false;
    g_quick_read_us += host_time_us() - read_began;
    const uint32_t quick = crc32c(0, edge, head + tail);
    ++g_quick_reads;
    if (current && found->second.quick == quick) {
        found->second.seen = now; found->second.next_quick = now + kQuickFrames; hash = found->second.hash;
        return true;
    }
    if (found == table.hashes.end()) ++g_hash_new;
    else if (found->second.buffer_id != info.id || found->second.gen != region_gen) ++g_hash_gen;
    else if (found->second.quick != quick) ++g_hash_quick;
    else ++g_hash_due;
    table.scratch.resize(static_cast<size_t>(bytes));
    if (read_buffer_locked(device, info, offset, bytes, table.scratch.data()) != SRC_MAPPED) return false;
    hash = crc32c(0, table.scratch.data(), table.scratch.size());
    ++table.full_hashes; ++g_hash_full; g_hash_bytes += bytes;
    if (found == table.hashes.end() && table.hashes.size() >= kHashTableEntries) table.trim(now);
    // Regions first seen in one frame are not all hashed again in one later frame.
    table.hashes[key] = {info.id, region_gen, now + kRehashFrames + (IndexKeyHash()(key) >> 7) % kRehashFrames, hash, quick, now, now + kQuickFrames};
    return true;
}

bool character_record(CharacterBackend& b, CmdBuf* cb, const DrawRec& r)
{
    // The first package whose section matches records the draw; the others never see it. The draw's
    // index region is hashed once (D-097) and only the packages with a section of that hash are asked.
    if (!b.enabled.load(std::memory_order_relaxed)) return false;
    struct Clock {
        CharacterBackend& b; uint64_t began;
        ~Clock() { b.record_us += host_time_us() - began; ++b.record_calls; }
    } clock{b, host_time_us()};
    std::shared_ptr<const CharacterBackend::Index> index;
    { std::shared_lock<std::shared_mutex> lock(b.index_mx); index = b.index; }
    if (!index || (!index->hash_only && !index->counts.count(r.count))) return false;
    uint32_t hash = 0;
    {
        std::shared_lock<std::shared_mutex> lock(b.device->res_mx);
        const bool hashed = index_region_hash(b.device, r, b.frames.load(std::memory_order_relaxed), *index, hash);
        b.hash_us += host_time_us() - clock.began; ++b.hash_calls;
        if (!hashed) return false;
    }
    const auto found = index->by_hash.find(hash);
    if (found == index->by_hash.end()) { ++b.hash_misses; return false; }
    for (const auto& candidate : found->second) {
        auto& c = *b.controllers[candidate.package];
        std::shared_lock<std::shared_mutex> lock(c.mutex);
        // A generation exchanged since the index was built sees the draw once the index is rebuilt.
        if (c.stopped || !c.active || c.generation != index->generations[candidate.package]) continue;
        const auto& section = c.active->package.sections[candidate.section];
        if (section.match_count == kCharacterNone) {
            // Hash-only sections (hidden game meshes) act for an activated character only.
            if (!c.active->activated.load(std::memory_order_relaxed)) continue;
        } else if (section.match_count != r.count || (section.match_first != kCharacterNone && section.match_first != r.first)) {
            continue;
        }
        if (section.stride_slot != kCharacterNone) {
            // A branch of a list that chooses the component by `vbN->stride` (LIMIT-041): the original
            // reads the bound vertex buffer's stride, the layer the draw's pipeline binding.
            std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
            const auto pipe = b.device->pipes.find(r.pipeline);
            uint32_t stride = 0;
            std::string why;
            if (pipe == b.device->pipes.end() || !CharacterGeneration::vertex_stride(pipe->second, section.stride_slot, stride, why) ||
                stride != section.stride) continue;
        }
        if (generation_record(*c.active, cb, r, candidate.section)) return true;
    }
    return false;
}
// At the application's graphics pipeline bind (D-133): while a discard gate is set (RabbitFX's anti-
// dither), a pipeline whose fragment stage has the character dither is bound as its twin without it, as
// the original's replaced shader skips the discard on every draw it makes. The twin is a variant of the
// pipeline (its own vertex stage and images, the dither branch steered away) built by the variant cache's
// workers; the first bind waits for it, and binds the application's pipeline only when that wait runs out
// or the twin cannot be made. Recording thread; the gate off costs one load.
VkPipeline character_bind(CharacterBackend& b, CmdBuf* cb, VkPipeline pipeline)
{
    // RabbitFX's state alone decides, as under DX11 its replaced shaders do whatever the character Mods do.
    if (!g_dither_off.load(std::memory_order_relaxed) || !b.variants || !cb) return pipeline;
    uint32_t id = 0;
    std::shared_ptr<const GraphicsRecipe> recipe;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        const auto pipe = b.device->pipes.find(pipeline);
        if (pipe == b.device->pipes.end() || !pipe->second.recipe || !pipe->second.layout) return pipeline;
        id = pipe->second.id; recipe = pipe->second.recipe; layout = pipe->second.layout;
    }
    if (!pipeline_dither(id, *recipe)) return pipeline;
    std::shared_ptr<const PipelineVariant> twin;
    {
        std::shared_lock<std::shared_mutex> lock(b.twins_mx);
        if (b.twins_failed.count(id)) return pipeline;
        const auto found = b.twins.find(id);
        if (found != b.twins.end()) twin = found->second;
    }
    if (!twin) {
        // Prepared when the game created the pipeline (character_created), so normally made by now; a twin
        // still being built is waited for (bounded), as a replaced draw waits for its variant (D-103).
        const auto result = b.variants->await(twin_identity(id), recipe, layout, recipe->vertex, twin_patch(), std::chrono::milliseconds(4000));
        std::unique_lock<std::shared_mutex> lock(b.twins_mx);
        if (result.variant) twin = b.twins[id] = result.variant;
        else {
            if (result.state == VariantState::Failed && b.twins_failed.insert(id).second)
                logf("character dither: no twin for pipeline %u: %s", id, result.reason.c_str());
            return pipeline;
        }
    }
    const auto held = owner_for(cb->owners, &b.twins, [] { return std::make_shared<TwinOwner>(); });
    if (!held) return pipeline;
    auto& owner = static_cast<TwinOwner&>(*held);
    owner.used = true;
    if (std::find(owner.held.begin(), owner.held.end(), twin) == owner.held.end()) owner.held.push_back(twin);
    ++b.twin_binds;
    return twin->pipeline;
}
// At the application's graphics pipeline creation (D-133): when the Mods have a discard gate, the twin of a
// pipeline with the character dither is prepared at once, behind the variants draws wait for, as the
// original replaces the shader when the game creates it; the key then only changes what is bound.
void prepare_twin(CharacterBackend& b, uint32_t id, const std::shared_ptr<const GraphicsRecipe>& recipe,
                  const std::shared_ptr<const PipelineLayoutInfo>& layout)
{
    if (!recipe || !layout || !pipeline_dither(id, *recipe)) return;
    const auto result = b.variants->prepare(twin_identity(id), recipe, layout, recipe->vertex, twin_patch());
    if (result.state == VariantState::Queued) ++b.twins_prepared;
}
void character_created(CharacterBackend& b, VkPipeline pipeline)
{
    if (!g_dither_gated.load(std::memory_order_relaxed) || !b.variants) return;
    uint32_t id = 0;
    std::shared_ptr<const GraphicsRecipe> recipe;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        const auto pipe = b.device->pipes.find(pipeline);
        if (pipe == b.device->pipes.end()) return;
        id = pipe->second.id; recipe = pipe->second.recipe; layout = pipe->second.layout;
    }
    prepare_twin(b, id, recipe, layout);
}
// A backend made while the game already has its pipelines (a module exchange): their twins, as above.
void prepare_twins(CharacterBackend& b)
{
    if (!g_dither_gated.load(std::memory_order_relaxed) || !b.variants) return;
    std::vector<std::tuple<uint32_t, std::shared_ptr<const GraphicsRecipe>, std::shared_ptr<const PipelineLayoutInfo>>> pipes;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        pipes.reserve(b.device->pipes.size());
        for (const auto& pipe : b.device->pipes)
            if (pipe.second.recipe && pipe.second.layout) pipes.emplace_back(pipe.second.id, pipe.second.recipe, pipe.second.layout);
    }
    for (const auto& [id, recipe, layout] : pipes) prepare_twin(b, id, recipe, layout);
}
void character_retire(CharacterBackend& b, uint32_t id)
{
    if (b.variants) {
        b.variants->retire(id);
        for (uint32_t k = 0; k < kCaptureKinds; ++k) b.variants->retire(capture_identity(id, CaptureKind(k)));
        b.variants->retire(twin_identity(id));
    }
    { std::unique_lock<std::shared_mutex> lock(b.twins_mx); b.twins.erase(id); b.twins_failed.erase(id); }
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
        w.key("unmatched"); w.begin_array(); for (const auto& ini : b.folder->unmatched) w.str(ini); w.end_array();
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
        return m;
    }();
    return &module;
}

void generation_enable(CharacterGeneration& backend, bool enabled) { backend.enabled = enabled && bool(backend.cache); }

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

#if defined(EFVK_CHARACTER_CACHE_TESTS)
#include "../../sandbox/character_cache_cases.h"
#endif
