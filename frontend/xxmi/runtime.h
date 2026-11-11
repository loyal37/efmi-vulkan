#pragma once
#include "../assets/file_plan.h"
#include <tuple>
#include <array>
#include <functional>
#include <map>

namespace efmi::frontend {
class RuntimeExtension;
struct SourceShaderState;
struct RuntimeShaderCall;
struct RuntimeConfig {
    std::filesystem::path resource_root;
    std::vector<std::filesystem::path> inputs;
    // The caller supplies a project-owned directory. Never writes next to a mod.
    std::filesystem::path settings_directory;
    std::string settings_name;
    // Optional enabled Mods tree; the original Include implementation owns
    // recursion, ordering, namespaces, conditions and exclusions.
    std::filesystem::path mods_directory;
    // Opt in only when the host supplies original tracked shader facts and an
    // executable pre/original/post call boundary. Existing character plans do not.
    bool shader_hooks=false;
    std::shared_ptr<const SourceShaderState> initial_shaders;
    // The host answers texture queries of draws (RuntimeDraw::pixel_texture): texture-hash
    // overrides, CheckTextureOverride and Core's OverrideTextures run against its answers (D-077).
    bool texture_identity=false;
    // Libraries the inputs call by namespace, from outside the resource root (D-112). Parsed after
    // the inputs, each under its path relative to the root (`..\EFMITTL\translucency.ini`), so the
    // files it names resolve against its own folder as the original resolves them.
    std::vector<std::filesystem::path> libraries;
    // Installed files of libraries the runtime provides natively (RabbitFX, D-082): their key and
    // ShaderRegex sections join after the native interface (the key toggles the interface's variables,
    // the ShaderRegex sections give the discard gates, D-133); every other section is the interface's.
    std::vector<std::filesystem::path> native_library_files;
    // The installation (the folder of the original's d3dx.ini): its own sections join the generation
    // first, as the original parses that file first; empty for none.
    std::filesystem::path installation;
};
struct RuntimeInput {
    bool foreground = false;
    std::array<bool, 256> keys{};
    // Original frame_time and spatial-pool radius scale, supplied by the host.
    // Zero preserves the original startup value; do not infer a frame rate.
    float frame_seconds = 0;
    // The presentation window and cursor, sampled with the keys. The original reads them with
    // GetClientRect, GetCursorInfo and ScreenToClient on its hWnd; a generation answers those
    // calls from this sample. Without a window the original has no hWnd: window_width falls
    // back to the resolution and then to 1920x1080, and cursor_window_* are screen positions.
    struct Window {
        bool present = false;
        int32_t client_left = 0, client_top = 0; // Client origin in screen coordinates.
        int32_t client_width = 0, client_height = 0;
    } window;
    struct Cursor {
        bool showing = false;
        int32_t screen_x = 0, screen_y = 0;
        int32_t hotspot_x = 0, hotspot_y = 0; // GetIconInfo on the cursor; zero without one.
    } cursor;
    // The swap chain size, which the original keeps in mResolutionInfo (res_width/res_height).
    // Zero keeps the previous value; a generation starts with the original's unset -1.
    uint32_t resolution_width = 0, resolution_height = 0;
    // GetDpiForMonitor's effective DPI for the window's monitor (the primary one without a
    // window), read per-monitor aware as the original does. The original reports at least 96;
    // zero, without a sample, reports that.
    uint32_t effective_dpi = 0;
    // The host's steady clock at this presentation in microseconds (the original's
    // GetSystemTicks), which advances the original FPS counter. Zero leaves it unchanged.
    uint64_t ticks_us = 0;
};
// A texture at a DX11 pixel slot of a draw as the original sees it (D-077, D-138): its resource hash and
// the D3D11_TEXTURE2D_DESC its match_* sections compare, field by field (Width, Height, MipLevels,
// ArraySize, Format, SampleDesc.Count, SampleDesc.Quality, Usage, BindFlags, CPUAccessFlags, MiscFlags);
// `described` false when the host cannot tell, and no such section matches it.
struct RuntimeTexture {
    bool bound = false;
    uint32_t hash = 0;
    bool described = false;
    uint32_t description[11] = {};
    // What `ps-tN->Format`, `->Width`, `->Height`, `->Mips` and `->Array` read of it (D-170), as the original's
    // getters take them off the resource behind the slot's view. `dimension`: the resource as
    // D3D11_RESOURCE_DIMENSION (1 a buffer, 2 a 1D, 3 a 2D, 4 a 3D texture), 0 when the host cannot tell and
    // those queries refuse. `view_format`: the DXGI_FORMAT of the DX11 view of the same resource, valid with
    // `format_known` (0 with it: a view without a format, a structured buffer's). The size, mip levels and
    // array size are the resource's own, not the view's.
    uint32_t dimension = 0;
    bool format_known = false;
    uint32_t view_format = 0;
    uint32_t width = 0, height = 0, levels = 0, layers = 0;
};
constexpr uint32_t kWordBufferDeclaration = 0xFFFFFFFFu;
// The vertex shader groups of the EFMITTL library a draw's vertex shader stands for (D-113, D-171): the lit
// pass of cloth, the lit pass of skin, an outline. The shadow groups are never met (the Vulkan shadow vertex
// shaders output no UV).
enum class VertexHook : uint32_t { None = 0, Lit = 1, LitSkin = 2, Outline = 3 };
struct RuntimeDraw {
    uint32_t hash = 0, count = 0, first = 0, instances = 1, first_instance = 0;
    int32_t base = 0;
    uint64_t frame = 0;
    float seconds = 0;
    // Supplied only after the renderer has resolved this single native identity
    // and imported the component's source bones. No original-resource queries.
    uint32_t component = UINT32_MAX, lod = UINT32_MAX;
    uint64_t identity = 0;
    // A folder generation (D-131): the Mod the identity resolved the draw to, by its index among the
    // generation's character Mods; UINT32_MAX in a generation of one Mod.
    uint32_t mod = UINT32_MAX;
    // Optional source-API facts from a verified shader contract. A Vulkan
    // binding number alone does not establish the original DX11 constant slot.
    bool rolling_bone_source = false;
    int32_t source_vs_config_slot = -1;
    float frame_seconds = 0;
    // With RuntimeConfig::texture_identity: the application's texture at a DX11 pixel slot of this
    // draw, as the original's resource hash and description (`bound` false for an empty slot). False refuses.
    std::function<bool(unsigned slot, RuntimeTexture& texture, std::string& reason)> pixel_texture;
    // RabbitFX's texture roles in this draw (D-082): the DX11 t registers of the diffuse map, the
    // lightmap and the normal map its fragment shader samples, -1 for a role it has not. False:
    // not known, and the RabbitFX library binds nothing.
    std::function<bool(int32_t roles[3], std::string& reason)> texture_roles;
    // The DX11 declaration of t register `slot` of the draw's shader at `stage` (L'v', L'h', L'd',
    // L'g', L'p') as the original reads it off DXBC, for `vs->srv_type(n)`, `srv_stride` and
    // `srv_dimension` (D-124): the original's ShaderResourceType, D3D_SRV_DIMENSION and stride.
    // False: not known, and the query refuses.
    // `type` kWordBufferDeclaration: a buffer of 32-bit words, which DX11 declares structured (type 2,
    // dimension 1, the given stride) or raw (type 3, dimension 11, stride 0); the SPIR-V does not tell.
    // A condition is then evaluated with both and goes on when they agree (D-159).
    std::function<bool(wchar_t stage, unsigned slot, uint32_t& type, uint32_t& dimension, uint32_t& stride, std::string& reason)> source_srv;
    // `vbN->stride` in a replaced draw's plan: the stride of the game's vertex buffer at DX11 input slot N
    // (the Vulkan binding of that number), as the original's IAGetVertexBuffers gives it; 0 when none is
    // bound. False with the reason when it is not known (a dynamic stride).
    std::function<bool(unsigned slot, uint32_t& stride, std::string& reason)> vertex_stride;
    // The renderer's instance entry of this single-instance draw (the 64 floats per instance the
    // original Core reads from `Original_VS_CB`, its translation at floats 12 to 14), valid during the
    // call; a Mod that runs the Core's spatial identification itself reads it (D-086). Null: none.
    const uint8_t* instance_entry = nullptr;
    size_t instance_entry_size = 0;
    // The draw's render targets as its pipeline's subpass names them (D-087): bit i for a color
    // attachment at location i, which the fragment shader's output i (DX11 oN) writes, bit 8 for a
    // depth attachment. `render_pass` tells the attachments of different render passes apart.
    bool targets_known = false;
    uint32_t render_targets = 0;
    uint64_t render_pass = 0;
    // The shader stages of the draw's pipeline, as Vulkan stage bits (vertex 1, geometry 8,
    // fragment 16) (D-089); 0 when not known.
    uint32_t shader_stages = 0;
    // Bit N: the pipeline's vertex input reads binding N, DX11's input slot N (D-161: a buffer the lists
    // bind at a slot the pipeline does not read is nothing to the draw, as under D3D11). Every bit when
    // the host does not know the pipeline's vertex input.
    uint32_t vertex_slots = UINT32_MAX;
    // What a library's shader groups (ShaderRegex) match of this draw under DX11, read off the host's Vulkan
    // pipeline (D-113, D-171): the group of the game's vertex shader, and of its pixel shader the facts the
    // library's pixel groups decide by. `pixel_hooks` false: the host did not read the pixel shader, and no
    // pixel group runs.
    VertexHook vertex_hook = VertexHook::None;
    bool pixel_hooks = false;
    // The DX11 t register of the first 2D texture below t24 whose colour (three or four channels) the pixel
    // shader samples at an interpolated input, -1 for none; and whether a sample of one or two channels at
    // such an input comes ahead of every other one (the hair's alpha clip in an outline).
    int32_t uv_colour_slot = -1;
    bool uv_narrow_first = false;
    // The size of the draw's colour target 0 as the host knows it (the original's rt_width and
    // rt_height, read off the bound render target); -2 when none is bound (the queries answer -1);
    // -1 when the host does not know it.
    float target_width = -1, target_height = -1;
};
// The events of the game's frame a library's shader hooks see under DX11 and the host reports
// (D-113): the game's copy shader drew with a single colour target.
enum class LibraryHook : uint32_t { CopyShader = 1 };
struct RuntimeStatus {
    uint64_t draws = 0, presents = 0, replays = 0, key_events = 0, saves = 0, shader_calls=0;
    // Microseconds the matched command lists took to run and their plans to compile, over all draws,
    // and the calls and microseconds per command class.
    uint64_t draw_execute_us = 0, draw_compile_us = 0;
    std::vector<std::tuple<std::string, uint64_t, uint64_t>> commands;
    size_t bindings = 0;
    // Original warnings while this generation was parsed. The original engine reports them and
    // continues with what it could parse; so does this runtime. At most the latest 64 texts.
    size_t diagnostics = 0;
    std::vector<std::string> diagnostic_messages;
    // What the layer did not do as the original does, in the order it met it (the latest 64).
    std::vector<std::string> gaps;
    bool dirty = false, reload_requested = false;
    std::map<std::string, float> variables;
};

// The process-wide engine lock every generation's calls take (the original engine addresses its
// globals): calls, calls that found it held, and the microseconds those waited.
struct EngineLockStats { uint64_t entries = 0, waits = 0, wait_us = 0; };
EngineLockStats engine_lock_stats();

// Owns an isolated generation of the original XXMI frontend. Calls are serialized
// across generations because the original engine addresses its globals directly.
// Files/resources in emitted plans are owned, not pointers into this generation.
class IniRuntime {
public:
    static std::shared_ptr<IniRuntime> load(const RuntimeConfig&, std::shared_ptr<RuntimeExtension>, std::string& reason);
    ~IniRuntime();
    IniRuntime(const IniRuntime&) = delete;
    IniRuntime& operator=(const IniRuntime&) = delete;
    bool draw(const RuntimeDraw&, assets::FilePlan&, std::string& reason);
    bool bind_source_shaders(const SourceShaderState&,std::string& reason);
    bool intercept(const RuntimeShaderCall&,std::string& reason);
    // Input is supplied by the host. Tests never send input to other processes.
    // `keep_replay_state`: keep what replay() needs (every variable's value before the lists, D-104);
    // the host asks for it only where it will replay, the copy of every variable costing per package.
    bool present(const RuntimeInput&, uint64_t frame, float seconds, std::string& reason, bool keep_replay_state = true);
    bool replayable() const;
    // The last present() again for another presentation of the same frame (frame generation,
    // a frame presented again): the Present lists run from the variable state present() started
    // with, recording their GPU work again, and every variable and parameter is restored after.
    // No key is dispatched and no transition advances. False with the reason, or without a
    // presentation to replay (BUG-036).
    bool replay(const RuntimeInput&, uint64_t frame, float seconds, std::string& reason);
    bool save(std::string& reason);
    // Whether the persistent variables changed since the last save, as the last call left them; read
    // without the engine lock, so the host asks for a save only when there is one to make.
    bool settings_dirty() const;
    bool take_reload_request();
    // A shader hook event of the game's frame (D-113), for the libraries the generation loaded.
    void library_hook(LibraryHook);
    void inherit_persistent(const IniRuntime& previous);
    // Retired workers must not overwrite a newer generation's settings file.
    void suspend_persistence();
    RuntimeStatus status() const;
    // D-133: the IniParams elements ShaderRegex replacements test before a discard they keep (RabbitFX:
    // `ld ... l(172, 0), t120` and `if_z ini.w` around `${discard}`), each with its value after the
    // sections' unconditional assignments to it, which present() runs as the original runs them
    // whenever a replaced shader draws. Empty when no replacement gates a discard.
    struct DiscardGate { uint32_t index = 0, component = 0; float value = 0; };
    std::vector<DiscardGate> discard_gates() const;
    // What the layer did not do as the original does since the generation was made (D-137): how many
    // such texts there were, read without the generation's lock, and those after the first `seen`.
    uint32_t gap_count() const;
    std::vector<std::string> gaps_since(uint32_t seen) const;
    void note_timing(const char* name, uint64_t microseconds);
    std::vector<std::shared_ptr<const assets::FileResource>> resources() const;
    // The virtual keys each of the generation's key actions needs held, in the original's order (a
    // controller's buttons left out), for checks that press them as a player does (D-137).
    std::vector<std::vector<int>> key_combos() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    IniRuntime();
};
}
