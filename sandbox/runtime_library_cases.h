// A library the Mod calls by namespace from outside its folder (D-112): parsed after the Mod's INI
// under its path relative to the Mod's folder, so the files it names resolve against its own folder,
// and a declaration it holds answered in a draw's plan as the original answers it.
static void runtime_library_cases(const std::filesystem::path& folder)
{
    using namespace efmi::frontend;
    const auto mods = std::filesystem::absolute(folder / "library-case" / "Mods");
    const auto mod = mods / "mod", library = mods / "lib";
    std::filesystem::create_directories(mod);
    std::filesystem::create_directories(library / "data");
    write(library / "data" / "own.buf", "\x01\x00\x02\x00");
    write(mod / "texture.buf", "\x03\x00\x04\x00");
    write(library / "lib.ini", R"ini(namespace = lib_case
[Constants]
global $masked = -1
global $own = -1
global $cleared = -1
global $count = 0
[ResourceMask]
[ResourceOwn]
type = Buffer
format = R16_UINT
filename = data\own.buf
[CommandListProbe]
if ResourceMask !== null
    $masked = 1
else
    $masked = 0
endif
if ResourceOwn !== null
    $own = 1
endif
ResourceMask = null
if ResourceMask === null
    $cleared = 1
endif
$count = 0
)ini");
    write(mod / "mod.ini", R"ini(namespace = mod_case
[ResourceTexture]
type = Buffer
format = R16_UINT
filename = texture.buf
[TextureOverrideDraw]
hash = 12345678
Resource\lib_case\Mask = ref ResourceTexture
$\lib_case\count = 30
run = CommandList\lib_case\Probe
[TextureOverrideBare]
hash = 87654321
ib = Resource\lib_case\Own
$\lib_case\count = 30
run = CommandList\lib_case\Probe
)ini");
    std::filesystem::remove(folder / "library-case" / "settings" / "library.ini");
    RuntimeConfig config{mod, {mod / "mod.ini"}, folder / "library-case" / "settings", "library"};
    config.libraries = {library / "lib.ini"};
    std::string reason;
    auto runtime = IniRuntime::load(config, {}, reason);
    if (!runtime) throw std::runtime_error(reason);
    check(runtime->status().diagnostics == 0, "A library called by namespace parses with the Mod without original warnings");
    bool own_file = false;
    std::string seen;
    for (const auto& file : runtime->resources()) {
        std::error_code error;
        own_file |= std::filesystem::equivalent(file->path, library / "data" / "own.buf", error);
        seen += " " + file->path.u8string();
    }
    if (!own_file) throw std::runtime_error("A library's file resolves against the library's own folder, not the Mod's:" + seen);
    auto value = [&](const char* name) { return runtime->status().variables.at(std::string("$\\lib_case\\") + name); };
    RuntimeDraw draw; draw.hash = 0x12345678; draw.count = 30; draw.frame = 1;
    efmi::assets::FilePlan plan;
    if (!runtime->draw(draw, plan, reason)) throw std::runtime_error("A draw calling the library's list plans: " + reason);
    check(value("masked") == 1 && value("own") == 1 && value("cleared") == 1 && value("count") == 0,
          "In a draw's plan a declaration holding a resource is not null, one set to null is (the original's -0)");
    draw.hash = 0x87654321; draw.frame = 2;
    if (!runtime->draw(draw, plan, reason)) throw std::runtime_error("A draw without the assignment plans: " + reason);
    check(value("masked") == 0, "A declaration never assigned is null in the plan");
}

// A library's shader groups (D-171): the host tells which group a draw's shaders stand for, and the bridge
// runs that group's own lists where the original runs them: the vertex group's ahead of the Mod's lists and
// its post list after the draw, the pixel groups' after the Mod's lists. The library here has the interface
// the bridge knows and groups of the installed library's shape; a list of its own records what a Mod's
// call sees.
namespace {
class LibraryBridgeOnly final : public efmi::frontend::RuntimeExtension {
public:
    std::unique_ptr<efmi::frontend::EfmittlBridge> bridge;
    std::vector<std::wstring> install(efmi::frontend::DrawPlanAdapter& adapter) override
    {
        std::vector<std::wstring> boundaries;
        bridge = efmi::frontend::EfmittlBridge::install(adapter, boundaries);
        return boundaries;
    }
    bool prepare(efmi::frontend::DrawPlanAdapter&, CommandListState&, std::wstring&) override { if (bridge) bridge->prepare(); return true; }
    bool before_draw(const efmi::frontend::RuntimeDraw& draw, CommandListState&, std::wstring&) override { if (bridge) bridge->before_draw(draw); return true; }
    void draw_hooks(const efmi::frontend::RuntimeDraw& draw, efmi::frontend::DrawHooks& hooks) override { if (bridge) bridge->draw_hooks(draw, hooks); }
    void after_draw() noexcept override { if (bridge) bridge->after_draw(); }
};
}
static void runtime_library_hook_cases(const std::filesystem::path& folder)
{
    using namespace efmi::frontend;
    const auto mods = std::filesystem::absolute(folder / "library-hooks" / "Mods");
    const auto mod = mods / "mod", library = mods / "EFMITTL";
    std::filesystem::create_directories(mod);
    std::filesystem::create_directories(library);
    write(library / "translucency.ini", R"ini(namespace = EFMITTL
[Constants]
global $vs_kind = 0
global $lit_seen = 0
global $copy_after_lit = 0
global $alpha = -1
global $count = 0
global $first = 0
global $mask_channel = 0
global $mask_invert = 0
global $slot_hook = 7
global $slot_pending = 0
global $slot_pkey = 0
global $slot_clip = 0
global $seen_kind = -1
global $seen_hook = -1
global $answer = -2
[ResourceMask]
[PoolRegexDiffuse]
pool_size = 16
pool_index_type = fifo
pool_variable_default_value = -1
[CommandListCapture]
[CommandListDraw]
[CommandListComposite]
[CommandListMark]
$seen_kind = $vs_kind
$seen_hook = $slot_hook
$slot_hook = 1
$slot_pkey = 3
$slot_pending = 1
[CommandListRead]
$answer = $PoolRegexDiffuse[3]
[ShaderRegexEFMITTLLitVS]
shader_model = vs_5_0
$vs_kind = 1
$slot_pending = 0
$slot_hook = 0
$slot_clip = 0
post $vs_kind = 0
[ShaderRegexEFMITTLLitVS.Pattern]
^vs_5_0\n
[ShaderRegexEFMITTLLitVSSkin]
shader_model = vs_5_0
$vs_kind = 1
$slot_pending = 0
$slot_hook = 0
$slot_clip = 0
post $vs_kind = 0
[ShaderRegexEFMITTLLitVSSkin.Pattern]
^vs_5_0\n
[ShaderRegexEFMITTLOutlineVS]
shader_model = vs_5_0
$vs_kind = 4
$slot_pending = 0
$slot_hook = 0
$slot_clip = 0
post $vs_kind = 0
[ShaderRegexEFMITTLOutlineVS.Pattern]
^vs_5_0\n
[ShaderRegexEFMITTLClipFirst]
shader_model = ps_5_0
if $slot_pending && $vs_kind == 4
    $slot_clip = 1
endif
[ShaderRegexEFMITTLClipFirst.Pattern]
^ps_5_0\n
[ShaderRegexEFMITTLDiffuseAt17]
shader_model = ps_5_0
if $slot_pending && ($vs_kind == 1 || $vs_kind == 4)
    $PoolRegexDiffuse[$slot_pkey] = 17 + 32 * $slot_clip
    $slot_pending = 0
endif
[ShaderRegexEFMITTLDiffuseAt17.Pattern]
^ps_5_0\n
)ini");
    write(mod / "mod.ini", R"ini(namespace = hook_case
[TextureOverrideMark]
hash = 11111111
run = CommandList\EFMITTL\Mark
[TextureOverrideRead]
hash = 22222222
run = CommandList\EFMITTL\Read
)ini");
    std::filesystem::remove(folder / "library-hooks" / "settings" / "hooks.ini");
    RuntimeConfig config{mod, {mod / "mod.ini"}, folder / "library-hooks" / "settings", "hooks"};
    config.libraries = {library / "translucency.ini"};
    auto extension = std::make_shared<LibraryBridgeOnly>();
    std::string reason;
    auto runtime = IniRuntime::load(config, extension, reason);
    if (!runtime) throw std::runtime_error("A library with shader groups loads: " + reason);
    check(extension->bridge != nullptr, "The bridge takes a library that has the interface and the lit group it knows");
    auto value = [&](const char* name) { return runtime->status().variables.at(std::string("$\\efmittl\\") + name); };
    uint64_t frame = 0;
    auto draw = [&](uint32_t hash, VertexHook vertex, bool pixel, int32_t slot, bool narrow) {
        RuntimeDraw request; request.hash = hash; request.count = 30; request.frame = ++frame;
        request.vertex_hook = vertex; request.pixel_hooks = pixel; request.uv_colour_slot = slot; request.uv_narrow_first = narrow;
        efmi::assets::FilePlan plan;
        if (!runtime->draw(request, plan, reason)) throw std::runtime_error("A draw with a library's shader groups plans: " + reason);
    };
    const uint32_t mark = 0x11111111, read = 0x22222222;
    draw(mark, VertexHook::Lit, true, 17, false);
    check(value("seen_kind") == 1 && value("seen_hook") == 0, "The lit group's list runs ahead of the Mod's: its kind is set and its per-draw mark cleared");
    check(value("vs_kind") == 0, "The lit group's post list runs after the draw");
    check(value("slot_pending") == 0, "The pixel group of the slot runs after the Mod's lists and takes the pending answer");
    draw(read, VertexHook::None, false, -1, false);
    check(value("answer") == 17, "The DiffuseAt group wrote its slot for the key the Mod's call left");
    draw(mark, VertexHook::None, true, 17, false);
    check(value("seen_kind") == 0 && value("seen_hook") == 1, "A draw whose vertex shader is no group's runs no vertex list: the kind stays 0, the mark is not cleared");
    check(value("slot_pending") == 1, "The pixel group's own condition keeps it from answering outside a lit or outline pass");
    draw(mark, VertexHook::Outline, true, 17, true);
    check(value("seen_kind") == 4, "The outline group's list sets its kind");
    draw(read, VertexHook::None, false, -1, false);
    check(value("answer") == 49, "ClipFirst runs ahead of DiffuseAt, in the order of their names: the outline's answer carries the clip mark");
    draw(mark, VertexHook::LitSkin, false, 17, false);
    check(value("seen_kind") == 1 && value("slot_pending") == 1, "The lit group of skin sets the lit kind; without the host's pixel facts no pixel group runs");
    draw(mark, VertexHook::Lit, true, 5, false);
    check(value("slot_pending") == 1, "A colour slot the library has no group for answers nothing");
    draw(mark, VertexHook::Lit, true, 17, true);
    draw(read, VertexHook::None, false, -1, false);
    check(value("answer") == 17, "In the lit pass ClipFirst's own condition leaves the mark unset");
}
