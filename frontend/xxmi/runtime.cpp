#include "runtime_host.h"
#include "resource_plan.h"
#include "shader_regex.h"
#include "globals.h"
#include "HackerContext.h"
#include "Override.h"
#include "ResourceHash.h"
#include "engine_host.h"
#include "layer_gap.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <mutex>
#include <cmath>
#include <unordered_set>

void HostParseExcerpt(const wchar_t*, const wchar_t*);
void HostParseFile(const wchar_t*, const wchar_t*);
std::vector<std::wstring> HostReadLines(const wchar_t*);
void HostParseIncludes();
void HostInsertBuiltins();
void HostCompileRemainingLists(const std::vector<std::wstring>&);
void HostCompileEntries(const std::vector<ExplicitCommandListSection*>&, const std::vector<std::wstring>&);
const std::vector<ExplicitCommandListSection*>& RuntimeInputDependencies();
void RuntimeResetInputDependencies();
void HostParseConstants();
void HostEnumerateLists();
void HostParseResources();
void HostParseTextureOverrides();
void HostParseResourceMatching();
void HostParseShaderOverrides();
void HostParseShaderRegex();
void HostCompileRoots(const std::vector<CommandList*>&, const std::vector<std::wstring>&);
void HostResourceRoot(const std::wstring&);
size_t HostDiagnosticCount();
std::string HostDiagnosticsSince(size_t);
std::vector<std::string> HostDiagnosticListSince(size_t);
void RuntimeResetParser();
void RuntimeParseKeys();
void RuntimeEnumerateCustomShaders();
void RuntimeCompilePresent();
void RuntimeCompileConstants(CommandList&,CommandList&);
void RuntimeParseParameterRegister();
void RuntimeUpdateTransitions(HackerDevice*);
void RuntimeValidateSections(const std::vector<std::wstring>&,bool custom_shaders,bool shader_hooks);
void RuntimeShaderRegexGates(std::vector<std::pair<uint32_t, uint32_t>>& gates, CommandList& assignments);
size_t RuntimeInputCount();
bool RuntimeDispatchInput(HackerDevice*, const efmi::frontend::RuntimeInput&);
std::vector<std::vector<int>> RuntimeInputKeys();
void RuntimeWindowSample(const efmi::frontend::RuntimeInput*);
HWND RuntimeWindowHandle();
extern std::vector<CommandList*> pending_post_command_lists;
extern std::vector<std::shared_ptr<CommandList>> dynamically_allocated_command_lists;

// Only this library defines these in production. The standalone old host still
// has its own executable globals and does not link the runtime translation unit.
// G is the entered generation's (engine_globals.h, D-129).
__declspec(thread) Globals* efvk_engine_g = nullptr;
FILE* LogFile = nullptr;
bool gLogDebug = false;
LogVerbosity gLogVerbosity = LogVerbosity::INVALID;

namespace efmi::frontend {
namespace {
// Loads one after another (D-129): what only the parser uses (its section tables, the include
// walk, the key parser's dependencies) is shared. Taken before the generation's own lock, and only
// by a load: no drawing, presenting or saving thread ever waits for it.
std::mutex load_mutex;
// The generations' locks' contention (EngineLockStats): entries, entries that found their
// generation's lock held, and the time those waited until they held it.
std::atomic<uint64_t> engine_entries{0}, engine_waits{0}, engine_wait_us{0};
// What a Present's key lists run with, on the presenting thread.
__declspec(thread) DrawPlanAdapter* input_adapter = nullptr;
__declspec(thread) HackerContext* input_context = nullptr;

std::string utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid UTF-16 text");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
void read_ini(const std::filesystem::path& path, const std::wstring& name)
{
    // The original's own reader (IniHandler.cpp ParseNamespacedIniFile): UTF-8 with a byte order mark
    // consumed, text mode, and nothing of a file after a byte sequence that is not UTF-8 (the judge's
    // encoding case: the lines before it are read, those after it are not; the layer refused the whole
    // file before). An input the generation needs that is not there refuses here; the original warns.
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) throw std::runtime_error("Cannot read INI: " + path.u8string());
    HostParseFile(path.wstring().c_str(), name.empty() ? nullptr : name.c_str());
}
// The installation's d3dx.ini, the original's first file (not namespaced): its own sections, except
// [Include...] (the Core's files and the Mods folder's enabled INIs are the layer's inputs) and
// [Loader] (the game). What a generation takes from it: a command list Mods call by name
// (LIMIT-039), the [Rendering] keys the parser and the compiler read (ini_params, recursive_include,
// track_region_hashes), and a [Constants], [Present] or [Key] the player wrote there.
void read_installation(const std::filesystem::path& file)
{
    std::wstring kept;
    bool skip = false;
    for (const auto& line : HostReadLines(file.wstring().c_str())) {
        const auto first = line.find_first_not_of(L" \t");
        if (first != std::wstring::npos && line[first] == L'[') {
            // The name as the original takes it: up to the first `]`, the spaces around left out.
            auto last = line.find(L']');
            if (last == std::wstring::npos) last = line.size();
            const auto end = line.find_last_not_of(L" \t", last - 1);
            const std::wstring name = end == std::wstring::npos || end < first + 1 ? std::wstring() : line.substr(first + 1, end - first);
            skip = !_wcsnicmp(name.c_str(), L"Include", 7) || !_wcsicmp(name.c_str(), L"Loader");
        }
        if (!skip) kept += line + L'\n';
    }
    HostParseExcerpt(kept.c_str(), nullptr);
}
// A generation's original engine: the objects the original keeps in process globals, which
// engine_globals.h redirects to the generation the calling thread entered (D-129). The state owns
// them for its whole life; a scope only names it as the thread's.
struct EngineState {
    std::unique_ptr<Globals> globals; // first: the other members' destructors may reach it
    CustomResources resources;
    CustomResourcePools pools;
    CustomShaders shaders;
    ShaderRegexGroups regex;
    std::vector<ShaderRegexGroup*> regex_index;
    ExplicitCommandListSections lists;
    CommandListVariables variables;
    std::vector<CommandListVariable*> persistent;
    std::map<std::wstring, float> unknown;
    std::vector<CommandList*> registered, post;
    std::unordered_set<CommandList*> profiled_lists;
    std::unordered_set<CommandListCommand*> profiled_commands;
    std::vector<std::shared_ptr<CommandList>> allocated;
    PresetOverrideMap presets;
    OverrideTransition transition;
    OverrideGlobalSave saved;
    std::vector<InputAction*> input;
    EngineHostState host;
    // The latest host window and cursor sample; the original's window queries read it.
    efmi::frontend::RuntimeInput sample;
    // G->user_config_dirty as every call left it, read without the generation's lock (settings_dirty()).
    std::atomic<bool> dirty{false};
    // host.gap_count as every call left it, read without the generation's lock (gap_count()).
    std::atomic<uint32_t> gaps{0};
    // One thread at a time in this generation, as the original's one context.
    std::mutex mutex;
    EngineState() : globals(std::make_unique<Globals>())
    {
        // IniHandler's values when [System] screen_width and screen_height are absent; a
        // generation reads no d3dx.ini. window_width falls back to them last.
        globals->gFallbackScreenWidth = 1920; globals->gFallbackScreenHeight = 1080;
        InitializeCriticalSection(&globals->mCriticalSection);
        InitializeCriticalSection(&globals->mShaderBindingsLock);
        InitializeCriticalSection(&globals->mResourcesLock);
        static std::once_flag creation_lock;
        std::call_once(creation_lock, [] { InitializeCriticalSection(&resource_creation_mode_lock); });
    }
    ~EngineState()
    {
        for (auto* action : input) delete action;
        DeleteCriticalSection(&globals->mCriticalSection);
        DeleteCriticalSection(&globals->mShaderBindingsLock);
        DeleteCriticalSection(&globals->mResourcesLock);
    }
};
// The generation the calling thread entered.
__declspec(thread) EngineState* engine_current = nullptr;
EngineState& entered()
{
    if (!engine_current) throw std::runtime_error("Original engine state used outside a generation");
    return *engine_current;
}
// A thread entering a generation: its lock, then its state as the thread's original engine. A load
// takes the load mutex first.
struct Scope {
    std::unique_lock<std::mutex> loading;
    std::unique_lock<std::mutex> lock;
    EngineState& state;
    enum Kind { Run, Load };
    Scope(EngineState& s, Kind kind = Run) : state(s)
    {
        if (engine_current) throw std::runtime_error("Nested frontend generation activation");
        if (kind == Load) loading = std::unique_lock<std::mutex>(load_mutex);
        lock = std::unique_lock<std::mutex>(state.mutex, std::try_to_lock);
        engine_entries.fetch_add(1, std::memory_order_relaxed);
        if (!lock.owns_lock()) {
            const auto began = std::chrono::steady_clock::now();
            lock.lock();
            engine_waits.fetch_add(1, std::memory_order_relaxed);
            engine_wait_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count()),
                                     std::memory_order_relaxed);
        }
        engine_current = &state; efvk_engine_g = state.globals.get(); RuntimeWindowSample(&state.sample);
    }
    ~Scope()
    {
        state.dirty.store(G->user_config_dirty, std::memory_order_relaxed);
        state.gaps.store(uint32_t(state.host.gap_count), std::memory_order_relaxed);
        input_adapter = nullptr; input_context = nullptr; RuntimeWindowSample(nullptr);
        clear_command_list_profiling(); engine_current = nullptr; efvk_engine_g = nullptr;
    }
};
void f10(HackerDevice*, void*) { G->gReloadConfigPending = true; }
// LayerGap's sink: the entered generation keeps what the layer did not do, for the host to show.
void keep_gap(const wchar_t* text)
{
    auto* host = EngineHost();
    if (!host) return;
    std::string kept;
    try { kept = utf8(text); } catch (const std::exception&) { kept = "(a text that is not UTF-16)"; }
    host->gaps.emplace_back(++host->gap_count, std::move(kept));
    if (host->gaps.size() > 64) host->gaps.pop_front();
}
const bool gap_sink_installed = (g_layer_gap_sink = keep_gap, true);
}
}

// The accessors engine_globals.h names, with the original's declared types.
CustomResources* efvk_engine_customResources() { return &efmi::frontend::entered().resources; }
CustomResourcePools* efvk_engine_customResourcePools() { return &efmi::frontend::entered().pools; }
CustomShaders* efvk_engine_customShaders() { return &efmi::frontend::entered().shaders; }
ShaderRegexGroups* efvk_engine_shader_regex_groups() { return &efmi::frontend::entered().regex; }
std::vector<ShaderRegexGroup*>* efvk_engine_shader_regex_group_index() { return &efmi::frontend::entered().regex_index; }
ExplicitCommandListSections* efvk_engine_explicitCommandListSections() { return &efmi::frontend::entered().lists; }
CommandListVariables* efvk_engine_command_list_globals() { return &efmi::frontend::entered().variables; }
std::vector<CommandListVariable*>* efvk_engine_persistent_variables() { return &efmi::frontend::entered().persistent; }
std::map<std::wstring, float>* efvk_engine_unknown_variables() { return &efmi::frontend::entered().unknown; }
std::vector<CommandList*>* efvk_engine_registered_command_lists() { return &efmi::frontend::entered().registered; }
std::unordered_set<CommandList*>* efvk_engine_command_lists_profiling() { return &efmi::frontend::entered().profiled_lists; }
std::unordered_set<CommandListCommand*>* efvk_engine_command_lists_cmd_profiling() { return &efmi::frontend::entered().profiled_commands; }
std::vector<std::shared_ptr<CommandList>>* efvk_engine_dynamically_allocated_command_lists() { return &efmi::frontend::entered().allocated; }
std::vector<CommandList*>* efvk_engine_pending_post_command_lists() { return &efmi::frontend::entered().post; }
PresetOverrideMap* efvk_engine_presetOverrides() { return &efmi::frontend::entered().presets; }
OverrideTransition* efvk_engine_CurrentTransition() { return &efmi::frontend::entered().transition; }
OverrideGlobalSave* efvk_engine_OverrideSave() { return &efmi::frontend::entered().saved; }
std::vector<InputAction*>* RuntimeInputActions() { return &efmi::frontend::entered().input; }
EngineHostState* EngineHost() { auto* state = efmi::frontend::engine_current; return state ? &state->host : nullptr; }

namespace efmi::frontend {

struct IniRuntime::Impl {
    const RuntimeDraw* current_draw = nullptr; // during draw(): the host's texture identities
    EngineState state;
    RuntimeConfig config;
    std::filesystem::path settings;
    std::shared_ptr<RuntimeExtension> extension;
    DrawPlanAdapter adapter;
    ModTextures mod_textures; // what the loader makes of the Mods' image files, for the property queries (D-170)
    SourceShaderContext source_shaders;
    SourceShaderRegex source_regex;
    std::unique_ptr<FilePlanCompiler> files;
    HackerDevice device;
    HackerContext context;
    RuntimeStatus stats;
    size_t diagnostics_base = 0; // the host's diagnostic count when this generation's load began
    RuntimeInput last_input;
    std::array<bool, 256> blocked_keys{};
    bool can_save = true;
    // Saves in the order their texts were taken; the file is written without the engine lock.
    std::mutex save_mx;
    // The variable and parameter state the last present() started from, for replay() (BUG-036):
    // the generation's variables, listed once (again when the table grows), and their values. A
    // pair per variable built through the hash table every presentation cost 5% of a profile.
    std::vector<CommandListVariable*> variable_list;
    std::vector<float> pre_values;
    std::vector<DirectX::XMFLOAT4> pre_parameters;
    bool replayable = false;
    // The plans of the last draw, kept for their capacity (D-108).
    DrawHooks draw_hooks; // a library's shader group lists of the draw (D-171)
    std::vector<CommandList*> draw_pre, draw_post;
    DrawPlan draw_plan;
    assets::FilePlan draw_result;
    TextureOverrideMatches draw_matches;
    void snapshot_variables()
    {
        if (variable_list.size() != command_list_globals.size()) {
            variable_list.clear(); variable_list.reserve(command_list_globals.size());
            for (auto& item : command_list_globals) variable_list.push_back(&item.second);
        }
        pre_values.resize(variable_list.size());
        for (size_t i = 0; i < variable_list.size(); ++i) pre_values[i] = variable_list[i]->fval;
    }
    std::vector<std::shared_ptr<const assets::FileResource>> resources;
    // D-133: the IniParams elements ShaderRegex replacements test before a discard they keep, and the
    // sections' unconditional assignments to them (RuntimeShaderRegexGates, at load).
    std::vector<IniRuntime::DiscardGate> gates;
    CommandList gate_assignments;
    // After the Present lists: the assignments run, as when a replaced shader draws, and the gates read.
    void run_gates()
    {
        if (gates.empty()) return;
        CommandListState state; state.mHackerContext = &context; state.mHackerDevice = &device;
        for (const auto& command : gate_assignments.commands) command->run(&state);
        for (auto& gate : gates) {
            const auto& value = G->iniParams[gate.index];
            gate.value = gate.component == 0 ? value.x : gate.component == 1 ? value.y : gate.component == 2 ? value.z : value.w;
        }
    }
    Impl() { device.mHackerContext = &context; context.mHackerDevice = &device; }
    // A replaced draw's colour target size as the host read it off its render pass: the original
    // caches rt_width and rt_height in the state off OMGetRenderTargets at their first use, and a
    // draw's list that reads them (a Mod's GUI placement, a guard on every draw) would otherwise
    // refuse outside the owned GPU domain. The same for the draw's fresh states (the post root).
    void host_draw_state(CommandListState& s) const
    {
        if (current_draw && current_draw->target_width > 0 && current_draw->target_height > 0) {
            s.rt_width = current_draw->target_width; s.rt_height = current_draw->target_height;
        } else if (current_draw && current_draw->target_width == -2) {
            s.rt_width = s.rt_height = -3; // the host's "nothing bound": the queries answer -1 as the original
        }
    }
    CommandListState command_state()
    {
        CommandListState s; s.mHackerContext = &context; s.mHackerDevice = &device;
        host_draw_state(s);
        return s;
    }
    bool execute(CommandList& pre, CommandList& post, assets::FilePlan& result, std::wstring& reason,DrawPlanAdapter::OriginalCall between={})
    {
        auto s = command_state(); DrawPlan plan;
        return adapter.execute_pair(pre, post, s, plan, reason,std::move(between)) && files->compile(plan, result, reason);
    }
};

IniRuntime::IniRuntime() : impl_(std::make_unique<Impl>()) {}
IniRuntime::~IniRuntime()
{
    // Nothing else holds this generation now. Original destructors may write through G
    // (~UpscalingFlipBBCommand clears a flag) and reach the original's containers, so they address
    // this generation's, whose Globals outlive every other member of the state.
    auto* const outer_state = engine_current; auto* const outer_globals = efvk_engine_g;
    engine_current = &impl_->state; efvk_engine_g = impl_->state.globals.get();
    impl_.reset();
    engine_current = outer_state; efvk_engine_g = outer_globals;
}

std::shared_ptr<IniRuntime> IniRuntime::load(const RuntimeConfig& config, std::shared_ptr<RuntimeExtension> extension, std::string& reason)
{
    const char* phase = "input loading";
    try {
        auto runtime = std::shared_ptr<IniRuntime>(new IniRuntime);
        auto& p = *runtime->impl_; p.config = config; p.extension = std::move(extension);
        struct LoadGuard {
            std::shared_ptr<RuntimeExtension> extension;bool complete=false;
            ~LoadGuard(){if(!complete && extension)extension->execution_failed();}
        } load_guard{p.extension};
        if ((config.inputs.empty() && config.mods_directory.empty()) || config.libraries.size() > 16 || !config.resource_root.is_absolute() ||
            !config.settings_directory.is_absolute() || config.settings_name.empty() || config.settings_name.size() > 80 ||
            config.settings_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            throw std::runtime_error("Invalid frontend input/settings location");
        p.settings = config.settings_directory / (config.settings_name + ".ini");
        Scope scope(p.state, Scope::Load); RuntimeResetParser(); RuntimeResetInputDependencies();
        const auto diagnostics = HostDiagnosticCount();
        p.diagnostics_base = diagnostics;
        HostResourceRoot(config.resource_root.wstring());
        if (!config.installation.empty()) {
            std::error_code error;
            const auto d3dx = config.installation / "d3dx.ini";
            if (std::filesystem::is_regular_file(d3dx, error)) read_installation(d3dx);
        }
        if (!config.mods_directory.empty()) {
            auto relative = config.mods_directory.lexically_relative(config.resource_root);
            if (!config.mods_directory.is_absolute() || relative.empty() || *relative.begin() == L".." ||
                !std::filesystem::is_directory(config.mods_directory))
                throw std::runtime_error("Enabled Mods directory must be inside the resource root");
            const auto root = L"[Include]\ninclude_recursive = " + relative.wstring() +
                L"\nexclude_recursive = DISABLED*\nexclude_recursive = desktop.ini\n";
            HostParseExcerpt(root.c_str(), nullptr);
        }
        for (const auto& path : config.inputs) {
            if (!path.is_absolute()) throw std::runtime_error("INI input path must be absolute");
            auto relative = path.lexically_relative(config.resource_root);
            read_ini(path, relative.empty() || *relative.begin() == L".." ? path.filename().wstring() : relative.wstring());
        }
        // A test's hook for the folder's isolation: an input whose path contains this text refuses the load.
        if (const wchar_t* refused = _wgetenv(L"EFMI_VK_TEST_REFUSE_INPUT"); refused && *refused)
            for (const auto& path : config.inputs)
                if (path.wstring().find(refused) != std::wstring::npos)
                    throw std::runtime_error("test: the input " + path.filename().u8string() + " refuses the load");
        for (const auto& path : config.libraries) {
            if (!path.is_absolute()) throw std::runtime_error("Library INI path must be absolute");
            read_ini(path, path.lexically_relative(config.resource_root).wstring());
        }
        // The original inserts its built-in sections (BuiltInCommandListUnbindAllRenderTargets and the
        // scissor custom shaders) after d3dx.ini and before the included files; Mods run them by name.
        HostInsertBuiltins();
        HostParseIncludes();
        // The original loader may declare user_config, but our persistence
        // destination is always the explicit project-owned generation path.
        G->user_config = p.settings.wstring();
        RuntimeValidateSections(p.extension ? p.extension->native_shaders() : std::vector<std::wstring>{},
                                p.extension && p.extension->custom_shader_execution(),config.shader_hooks);
        if (p.extension) {
            const auto declarations = p.extension->declarations();
            if (!declarations.empty()) HostParseExcerpt(declarations.c_str(), L"efvk-native.ini");
            size_t unit = 0;
            for (const auto& text : p.extension->declaration_units()) {
                const auto name = L"efvk-library-" + std::to_wstring(unit++) + L".ini";
                HostParseExcerpt(text.c_str(), name.c_str());
            }
        }
        for (const auto& path : config.native_library_files) {
            // The file's namespace line, then only its [Key...] and [ShaderRegex...] sections.
            std::ifstream in(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
            const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
            std::wstring text(size_t(std::max(n, 0)), L'\0');
            if (n) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
            std::wistringstream lines(text);
            std::wstring line, kept;
            bool section = false, keep = false;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                const auto first = line.find_first_not_of(L" \t");
                if (first != std::wstring::npos && line[first] == L'[') {
                    section = true;
                    keep = !_wcsnicmp(line.c_str() + first, L"[Key", 4) || !_wcsnicmp(line.c_str() + first, L"[ShaderRegex", 12);
                }
                if (keep || (!section && first != std::wstring::npos && !_wcsnicmp(line.c_str() + first, L"namespace", 9))) kept += line + L"\n";
            }
            HostParseExcerpt(kept.c_str(), path.lexically_relative(config.resource_root).wstring().c_str());
        }
        // Original [Constants] parsing consumes declarations and later executes
        // assignments from the generated settings file after default values.
        if (std::filesystem::exists(p.settings)) read_ini(p.settings, L"efvk-user-settings.ini");
        phase = "declaration parsing";
        RuntimeEnumerateCustomShaders(); HostEnumerateLists(); HostParseConstants(); HostParseResources();RuntimeParseParameterRegister();
        if(config.shader_hooks){
            if(config.initial_shaders)p.source_shaders.install(*config.initial_shaders,p.context);
            p.adapter.shader_queries([&p](auto& operand,auto& state,auto& value,auto& why){return p.source_shaders.query(operand,state,value,why);});
        }else if(config.initial_shaders)throw std::runtime_error("Source shader facts require shader hook opt-in");
        p.adapter.fresh_state([&p](CommandListState& s){p.host_draw_state(s);});
        p.adapter.host_values([&p](ParamOverrideType type,float& value){
            const auto& sample=p.state.sample;
            if(type==ParamOverrideType::EFFECTIVE_DPI){value=(std::max)(float(sample.effective_dpi),96.0f);return true;}
            if(type==ParamOverrideType::CURSOR_HOTSPOT_X){value=float(sample.cursor.hotspot_x);return true;}
            if(type==ParamOverrideType::CURSOR_HOTSPOT_Y){value=float(sample.cursor.hotspot_y);return true;}
            return false;
        });
        if (config.texture_identity && !config.shader_hooks) {
            p.adapter.texture_queries([&p](unsigned slot, const DrawPlanAdapter::PixelBinding& binding, CommandListState&,
                                           DrawPlanAdapter::TextureIdentity& id, std::wstring& why) {
                // A resource of the generation: the original keeps no hash for what it creates itself. Its
                // properties are its file's (D-170); a file the loader makes nothing of is the null resource.
                if (!binding.application) {
                    id.bound = binding.resource != nullptr; id.hash = 0;
                    if (binding.resource && id.properties_wanted) {
                        bool loads = false; std::wstring unknown;
                        if (p.mod_textures.describe(*binding.resource, id.properties, loads, unknown)) id.bound = loads;
                    }
                    return true;
                }
                if (!p.current_draw || !p.current_draw->pixel_texture) { why = L"Source texture identity unavailable outside a draw"; return false; }
                RuntimeTexture texture; std::string reason;
                if (!p.current_draw->pixel_texture(slot, texture, reason)) { why.assign(reason.begin(), reason.end()); return false; }
                static_assert(sizeof(texture.description) == sizeof(D3D11_TEXTURE2D_DESC), "the description is D3D11_TEXTURE2D_DESC's fields");
                id.bound = texture.bound; id.hash = texture.hash; id.described = texture.described;
                if (texture.described) memcpy(&id.description, texture.description, sizeof(id.description));
                id.properties.dimension = D3D11_RESOURCE_DIMENSION(texture.dimension);
                id.properties.format_known = texture.format_known; id.properties.format = DXGI_FORMAT(texture.view_format);
                id.properties.width = texture.width; id.properties.height = texture.height;
                id.properties.mips = texture.levels; id.properties.array = texture.layers;
                return true;
            });
        }
        // The matched draw's index region hash, which is what the original's region hash of its own index
        // buffer gives (D-144).
        p.adapter.index_region_hash([&p](uint32_t& hash) { if (!p.current_draw) return false; hash = p.current_draw->hash; return true; });
        // What a draw of the lists can read (draw_state.h): the stages and vertex slots of the matched draw's pipeline.
        p.adapter.pipeline([&p] {
            PipelineFacts facts;
            if (p.current_draw) {
                facts.stages = p.current_draw->shader_stages; facts.vertex_slots = p.current_draw->vertex_slots;
                if (p.current_draw->targets_known) facts.targets = p.current_draw->render_targets;
            }
            return facts;
        });
        auto boundaries = p.extension ? p.extension->install(p.adapter) : std::vector<std::wstring>{};
        if(config.shader_hooks){HostParseShaderOverrides();HostParseShaderRegex();
            for(const auto& entry:G->mShaderOverrideMap)
                if(entry.second.depth_filter!=DepthBufferFilter::NONE || entry.second.model[0])
                    throw std::runtime_error("Legacy shader depth/model substitution requires a source replacement adapter");
        }
        HostParseTextureOverrides();HostParseResourceMatching(); RuntimeCompilePresent(); RuntimeParseKeys();
        std::wstring validation;
        if (p.extension && !p.extension->validate_overrides(validation)) throw std::runtime_error("Native override registry refused: " + utf8(validation));
        RegisterKeyBinding(L"reload_config", L"no_modifiers VK_F10", std::make_shared<InputCallbacks>(f10, nullptr, nullptr), 0, 0, 0, InputDisableScope::ALL);
        CommandList constants,post_constants; RuntimeCompileConstants(constants,post_constants);
        auto roots = registered_command_lists;
        phase = "command parsing";
        HostCompileRoots(roots, boundaries);
        HostCompileEntries(RuntimeInputDependencies(), boundaries);
        RuntimeResetInputDependencies();
        // Directory generations validate all enabled files. Legacy explicit
        // profiles intentionally import only a reachable Core subgraph; those
        // partial inputs do not declare the rest of the Core's dependencies.
        if (!config.mods_directory.empty()) HostCompileRemainingLists(boundaries);
        auto all_roots = registered_command_lists;
        // The optimizer releases its temporary ownership of branch lists.
        // Removed branches then die; only adapt surviving children through
        // their owning IfCommand, never through the old raw registration list.
        std::unordered_set<CommandList*> branch_lists;
        for (const auto& branch : dynamically_allocated_command_lists) branch_lists.insert(branch.get());
        all_roots.erase(std::remove_if(all_roots.begin(), all_roots.end(),
            [&](const auto* root) { return branch_lists.count(const_cast<CommandList*>(root)); }), all_roots.end());
        {
            // Before the parameters are sized: the gate assignments reserve their elements as the original's do.
            std::vector<std::pair<uint32_t, uint32_t>> elements;
            RuntimeShaderRegexGates(elements, p.gate_assignments);
            for (const auto& e : elements)
                if (e.second < 4 && int(e.first) < 65536) {
                    p.gates.push_back({e.first, e.second, 0});
                    G->iniParamsReserved = std::max(G->iniParamsReserved, int(e.first) + 1);
                }
        }
        if (G->iniParamsReserved < 0 || G->iniParamsReserved > 65536)
            throw std::runtime_error("INI parameter reservation exceeds generation budget");
        G->iniParams.assign(G->iniParamsReserved, DirectX::XMFLOAT4(0, 0, 0, 0));
        phase = "original command optimization";
        optimise_command_lists(nullptr);
        if(config.shader_hooks)p.source_regex.initialize(p.adapter);
        // Includes key-owned activation/post lists as well as Present and the
        // original hash matcher lists; do not adapt only named callbacks.
        phase = "Vulkan command adaptation";
        for (auto* root : all_roots) p.adapter.adapt(*root);
        p.adapter.adapt(constants);p.adapter.adapt(post_constants);
        p.adapter.adapt(G->present_command_list); p.adapter.adapt(G->post_present_command_list);
        for (auto& hashed : G->mTextureOverrideMap) for (auto& entry : hashed.second) {
            p.adapter.adapt(entry.command_list); p.adapter.adapt(entry.post_command_list);
        }
        for(auto& entry:G->mShaderOverrideMap){p.adapter.adapt(entry.second.command_list);p.adapter.adapt(entry.second.post_command_list);}
        if(config.shader_hooks && config.initial_shaders)p.source_regex.apply(p.source_regex.prepare(*config.initial_shaders,63,false),p.adapter);
        phase = "file declaration adaptation";
        p.files = std::make_unique<FilePlanCompiler>(customResources);
        auto initial = p.command_state(); DrawPlan unused; std::wstring why;
        if(p.extension && !p.extension->initialize_parameters(initial,why))
            throw std::runtime_error("GPU parameter initialization refused: "+utf8(why));
        // The original runs each command of [Constants] on its own (D-155): one the planner refuses
        // does nothing, with its warning, and the rest run.
        p.adapter.tolerate_refusals(true);
        if (!p.adapter.execute(constants, initial, unused, why) || !unused.items.empty())
            throw std::runtime_error("Constants execution refused: " + utf8(why));
        // Match InitIniParams: defaults and persisted pre values are not dirty;
        // post-Constants assignments can make the user settings dirty again.
        G->user_config_dirty=false;
        auto post_initial=p.command_state();post_initial.post=true;
        if(!p.adapter.execute(post_constants,post_initial,unused,why) || !unused.items.empty())
            throw std::runtime_error("Post-Constants execution refused: "+utf8(why));
        p.adapter.tolerate_refusals(false);
        if (p.extension && !p.extension->prepare(p.adapter, initial, why))
            throw std::runtime_error("Native generation preparation refused: " + utf8(why));
        // Original warnings (a duplicate section, an unrecognized line, a disabled regex group) do
        // not stop the original loader: it keeps what it could parse and reports the rest. The
        // generation does the same and keeps the texts for the status (D-067). What this host
        // cannot execute is refused by the adapters, independently of these warnings.
        p.stats.diagnostics = HostDiagnosticCount() - diagnostics;
        p.stats.diagnostic_messages = HostDiagnosticListSince(diagnostics);
        // Collect every branch's reachable files, not dormant declarations.
        // Original resource copy/alias commands are included by the adapter.
        for (auto* resource : p.adapter.referenced_files()) {
            // In the roles the lists bind it in, as the original's buffer takes the bind flags of the
            // assignments that use it; a file only copied into other resources keeps every role its
            // declaration allows, an index format excluding the vertex one (a typed vertex buffer binds by its
            // stride, so `format = R16_UINT, stride = 6` would otherwise be both).
            const unsigned roles = p.adapter.referenced_roles(resource);
            const bool index_format = resource->override_format == DXGI_FORMAT_R16_UINT || resource->override_format == DXGI_FORMAT_R32_UINT;
            for (auto operation : {PlanOperation::BindVertex, PlanOperation::BindIndex, PlanOperation::BindPixelResource}) {
                const unsigned bit = operation == PlanOperation::BindVertex ? 1u : operation == PlanOperation::BindIndex ? 2u : 4u;
                if (roles ? !(roles & bit) : (operation == PlanOperation::BindVertex && index_format)) continue;
                if (auto file = p.files->binding(resource, operation)) p.resources.push_back(std::move(file));
            }
        }
        p.stats.bindings = RuntimeInputCount();G->constants_run=true;
        // The gates hold from the first draw on, as the original's assignment runs at a replaced shader's
        // first draw (D-133): once now, with the saved values restored, and after every presentation.
        p.run_gates();
        RuntimeResetParser();load_guard.complete=true;return runtime;
    } catch (const std::exception& e) { reason = std::string(phase) + ": " + e.what(); return {}; }
}

void IniRuntime::note_timing(const char* name, uint64_t microseconds) { impl_->adapter.note_timing(name, microseconds); }

EngineLockStats engine_lock_stats()
{
    return {engine_entries.load(std::memory_order_relaxed), engine_waits.load(std::memory_order_relaxed), engine_wait_us.load(std::memory_order_relaxed)};
}

bool IniRuntime::draw(const RuntimeDraw& input, assets::FilePlan& output, std::string& reason)
{
    struct Completion {RuntimeExtension* extension;~Completion(){if(extension)extension->after_draw();}} completion{impl_->extension.get()};
    try {
        if(impl_->config.shader_hooks)throw std::runtime_error("Shader hook generation requires the intercepted call entry");
        if(!std::isfinite(input.frame_seconds) || input.frame_seconds<0)throw std::runtime_error("Original frame duration is invalid");
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
        auto phase_began = std::chrono::steady_clock::now();
        auto phase = [&](const char* name) {
            const auto now = std::chrono::steady_clock::now();
            impl_->adapter.note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - phase_began).count()));
            phase_began = now;
        };
#else
        auto phase = [](const char*) {};
#endif
        auto& p = *impl_; Scope scope(p.state); phase("phase scope");
        struct Current {Impl& p; Current(Impl& owner, const RuntimeDraw& draw):p(owner){p.current_draw=&draw;} ~Current(){p.current_draw=nullptr;}} current(p, input);
        G->frame_no = unsigned(input.frame); G->gTime = input.seconds; G->gFrameTime = input.frame_seconds;
        // DRAW_NUMBER: the Core's identification tells a second identifier call within one draw by it.
        ++p.context.draw_number;
        DrawCallInfo call(DrawCall::DrawIndexedInstanced, 0, input.count, input.instances, uint32_t(input.base), input.first, input.first_instance, nullptr, 0);
        TextureOverrideMatches& matches = p.draw_matches; matches.clear();
        find_texture_override_for_hash(input.hash, &matches, &call); phase("phase find");
        assets::FilePlan& result = p.draw_result;
        result.items.clear(); result.skip_original = false; result.pre_items = SIZE_MAX;
        auto s = p.command_state(); s.call_info = &call; std::wstring why;
        ResourceCopyTarget index_target;
        index_target.type = ResourceCopyTargetType::INDEX_BUFFER;
        s.this_target = &index_target;
        if (p.extension && !p.extension->before_draw(input, s, why)) throw std::runtime_error(utf8(why));
        phase("phase before_draw");
        if(!matches.empty()){
            // A library's shader groups (D-171), in the original's order around the matched lists: the vertex
            // stage's lists ahead of them (the Core's check of `ib` is a later group of the same stage), the
            // pixel stage's after them, all before the application's draw.
            DrawHooks& hooks = p.draw_hooks; hooks.before.clear(); hooks.between.clear(); hooks.after.clear();
            if (p.extension) p.extension->draw_hooks(input, hooks);
            std::vector<CommandList*>& pre = p.draw_pre; std::vector<CommandList*>& post = p.draw_post; pre.clear(); post.clear();
            pre.insert(pre.end(), hooks.before.begin(), hooks.before.end());
            for(auto* match:matches){pre.push_back(&match->command_list);post.push_back(&match->post_command_list);}
            pre.insert(pre.end(), hooks.between.begin(), hooks.between.end());
            DrawPlan& plan = p.draw_plan;
            const auto began = std::chrono::steady_clock::now();
            const bool executed = p.adapter.execute_matches(pre,post,s,plan,why);
            const auto ran = std::chrono::steady_clock::now();
            const bool compiled = executed && p.files->compile(plan,result,why);
            p.stats.draw_execute_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(ran - began).count());
            p.stats.draw_compile_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - ran).count());
            if (!executed || !compiled) throw std::runtime_error("Original draw command refused: " + utf8(why));
            // A group's post list that is more than constants (none in the libraries known): on its own, after
            // the draw's lists.
            for (auto* list : hooks.after) {
                auto after = p.command_state(); after.call_info = &call; after.post = true;
                DrawPlan unused;
                if (!p.adapter.execute(*list, after, unused, why) || !unused.items.empty())
                    throw std::runtime_error("A library's shader group list was refused: " + utf8(why));
            }
            phase("phase execute+compile");
        }
        ++p.stats.draws; std::swap(output, result); return true;
    } catch (const std::exception& e) {
        if(impl_->extension)impl_->extension->execution_failed();
        reason = e.what(); return false;
    }
}

bool IniRuntime::bind_source_shaders(const SourceShaderState& state,std::string& reason){
    reason.clear();try{auto& p=*impl_;Scope scope(p.state);
        if(!p.config.shader_hooks)throw std::runtime_error("Source shader facts require shader hook opt-in");
        auto prepared=p.source_regex.prepare(state,63,false);p.source_shaders.install(state,p.context);p.source_regex.apply(std::move(prepared),p.adapter);return true;
    }catch(const std::exception& e){reason=e.what();return false;}
}
bool IniRuntime::intercept(const RuntimeShaderCall& input,std::string& reason){
    reason.clear();try{auto& p=*impl_;Scope scope(p.state);
        const bool compute=input.call.type==DrawCall::Dispatch || input.call.type==DrawCall::DispatchIndirect;
        struct Completion {RuntimeExtension* extension;bool compute;~Completion(){if(extension){if(compute)extension->after_dispatch();else extension->after_draw();}}} completion{p.extension.get(),compute};
        if(!p.config.shader_hooks)throw std::runtime_error("Intercepted calls require shader hook opt-in");
        if(!std::isfinite(input.seconds) || !std::isfinite(input.frame_seconds) || input.frame_seconds<0)
            throw std::runtime_error("Original shader call time is invalid");
        if(input.call.type<DrawCall::Draw || input.call.type>DrawCall::DispatchIndirect || !input.original)
            throw std::runtime_error("Original shader call boundary is missing or invalid");
        const unsigned shader_mask=compute?32u:31u;
        auto prepared=input.call.hunting_skip?SourceShaderRegex::Preparation{}:p.source_regex.prepare(input.shaders,shader_mask,true);
        p.source_shaders.install(input.shaders,p.context);
        if(prepared)p.source_regex.apply(std::move(prepared),p.adapter);
        G->frame_no=unsigned(input.frame);G->gTime=input.seconds;G->gFrameTime=input.frame_seconds;
        if(compute)++p.context.dispatch_number;else ++p.context.draw_number;
        std::vector<CommandList*> pre,post;
        for(unsigned i=compute?5:0;i<(compute?6:5);++i){const auto& shader=p.source_shaders.stage(i);
            if(input.call.hunting_skip)continue;
            if(G->mShaderOverrideMap.empty())continue;
            if(!shader.known || (shader.bound && !shader.hash_known))throw std::runtime_error("Shader matching requires proven original stage hashes");
            const uint64_t hash=shader.bound?shader.hash:0;
            if(i>0 && i<4 && !hash)continue;
            const auto found=lookup_shaderoverride(hash);if(found==G->mShaderOverrideMap.end())continue;
            pre.push_back(&found->second.command_list);post.push_back(&found->second.post_command_list);
        }
        auto call=input.call;auto state=p.command_state();state.call_info=&call;std::wstring why;
        if(!p.adapter.execute_intercepted(pre,post,state,input.original,why))throw std::runtime_error("Original shader call refused: "+utf8(why));
        ++p.stats.shader_calls;return true;
    }catch(const std::exception& e){if(impl_->extension)impl_->extension->execution_failed();reason=e.what();return false;}
}

void RuntimeRunInputList(CommandList* list, bool post)
{
    if (!input_adapter || !input_context) throw std::runtime_error("Input command outside active generation");
    input_adapter->adapt(*list);
    CommandListState state; state.mHackerContext = input_context; state.mHackerDevice = input_context->mHackerDevice; state.post = post;
    DrawPlan result; std::wstring why;
    if (!input_adapter->execute(*list, state, result, why) || !result.items.empty())
        throw std::runtime_error("Input command requires unavailable render context: " + utf8(why));
}
void RuntimePublishTransitionParameters()
{
    if(!input_adapter || !input_context)throw std::runtime_error("Parameter transition outside active generation");
    CommandListState state;state.mHackerContext=input_context;state.mHackerDevice=input_context->mHackerDevice;
    std::wstring why;
    if(!input_adapter->flush_parameters(state,true,why))throw std::runtime_error("Parameter transition refused: "+utf8(why));
}
bool IniRuntime::replayable() const { return impl_->replayable; }
bool IniRuntime::settings_dirty() const { return impl_->state.dirty.load(std::memory_order_relaxed); }

bool IniRuntime::present(const RuntimeInput& input, uint64_t frame, float seconds, std::string& reason, bool keep_replay_state)
{
    try {
        if(!std::isfinite(input.frame_seconds) || input.frame_seconds<0)throw std::runtime_error("Original frame duration is invalid");
        const auto& w = input.window;
        if (w.client_width < 0 || w.client_height < 0 || w.client_width > 65536 || w.client_height > 65536 ||
            input.resolution_width > 65536 || input.resolution_height > 65536 || !input.resolution_width != !input.resolution_height ||
            input.effective_dpi > 65536 || std::abs(int64_t(input.cursor.hotspot_x)) > 65536 || std::abs(int64_t(input.cursor.hotspot_y)) > 65536)
            throw std::runtime_error("Host window sample is invalid");
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
        auto phase_began = std::chrono::steady_clock::now();
        auto phase = [&](const char* name) {
            const auto now = std::chrono::steady_clock::now();
            impl_->adapter.note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - phase_began).count()));
            phase_began = now;
        };
#else
        auto phase = [](const char*) {};
#endif
        auto& p = *impl_; Scope scope(p.state); phase("present scope"); G->frame_no = unsigned(frame); G->gTime = seconds; G->gFrameTime = input.frame_seconds;
        p.last_input = input; p.state.sample = input;
        G->hWnd = w.present ? RuntimeWindowHandle() : nullptr;
        if (input.resolution_width) { G->mResolutionInfo.width = int(input.resolution_width); G->mResolutionInfo.height = int(input.resolution_height); }
        // HackerSwapChain::RunFrameActions advances the counter before the Present list runs.
        if (input.ticks_us) G->gFPSCounter.Update(input.ticks_us);
        auto sampled = input;
        for (size_t i = 0; i < sampled.keys.size(); ++i) {
            if (!input.foreground || !input.keys[i]) p.blocked_keys[i] = false;
            if (p.blocked_keys[i]) sampled.keys[i] = false;
        }
        input_adapter = &p.adapter; input_context = &p.context;
        // The keys' lists, the transitions and the Present lists run each command on its own, as the
        // original's (D-155): one a key's list cannot act on ended the whole Present for every Mod before.
        struct Tolerant { DrawPlanAdapter& a; Tolerant(DrawPlanAdapter& adapter) : a(adapter) { a.tolerate_refusals(true); } ~Tolerant() { a.tolerate_refusals(false); } } tolerant(p.adapter);
        if (RuntimeDispatchInput(&p.device, sampled)) ++p.stats.key_events;
        CurrentTransition.UpdatePresets(&p.device);RuntimeUpdateTransitions(&p.device);
        phase("present keys+presets");
        // What replay() starts from: the state after the keys, with what the frame's draws set.
        p.replayable = false;
        if (keep_replay_state) { p.snapshot_variables(); p.pre_parameters = G->iniParams; }
        assets::FilePlan ignored; std::wstring why;
        DrawPlanAdapter::OriginalCall between;
        if(p.config.shader_hooks)between=[&](CommandListState&,std::wstring&){++G->frame_no;p.context.ResetCallCounters();return true;};
        // As the original's Present: a command the planner refuses does nothing, with its warning, and the
        // list goes on (D-155). A draw the Present issues outside owned work has nothing to execute it.
        p.adapter.tolerate_refusals(true);
        const bool presented = p.execute(G->present_command_list, G->post_present_command_list, ignored, why, std::move(between));
        p.adapter.tolerate_refusals(false);
        if (!presented) throw std::runtime_error("Present requires unavailable render context: " + utf8(why));
        if (!ignored.items.empty())
            throw std::runtime_error("The Present's draw items have no executor here (" + std::to_string(ignored.items.size()) + ")");
        phase("present execute");
        p.run_gates();
        ++p.stats.presents; p.replayable = keep_replay_state; return true;
    } catch (const std::exception& e) {
        if(impl_->extension)impl_->extension->execution_failed();
        reason = e.what(); return false;
    }
}

bool IniRuntime::replay(const RuntimeInput& input, uint64_t frame, float seconds, std::string& reason)
{
    try {
        if(!std::isfinite(input.frame_seconds) || input.frame_seconds<0)throw std::runtime_error("Original frame duration is invalid");
        const auto& w = input.window;
        if (w.client_width < 0 || w.client_height < 0 || w.client_width > 65536 || w.client_height > 65536 ||
            input.resolution_width > 65536 || input.resolution_height > 65536 || !input.resolution_width != !input.resolution_height ||
            input.effective_dpi > 65536 || std::abs(int64_t(input.cursor.hotspot_x)) > 65536 || std::abs(int64_t(input.cursor.hotspot_y)) > 65536)
            throw std::runtime_error("Host window sample is invalid");
        auto& p = *impl_; Scope scope(p.state);
        if (!p.replayable) throw std::runtime_error("No presentation to replay");
        G->frame_no = unsigned(frame); G->gTime = seconds; G->gFrameTime = input.frame_seconds;
        p.state.sample = input;
        G->hWnd = w.present ? RuntimeWindowHandle() : nullptr;
        if (input.resolution_width) { G->mResolutionInfo.width = int(input.resolution_width); G->mResolutionInfo.height = int(input.resolution_height); }
        // The current state comes back whatever the lists do; the lists start from the state the
        // last present() started from, so they draw what it drew.
        if (p.variable_list.size() != command_list_globals.size()) throw std::runtime_error("Variables changed since the presentation");
        struct Restore {
            Impl& p; std::vector<float> now; std::vector<DirectX::XMFLOAT4> parameters;
            Restore(Impl& impl) : p(impl), parameters(G->iniParams)
            {
                now.resize(p.variable_list.size());
                for (size_t i = 0; i < p.variable_list.size(); ++i) { now[i] = p.variable_list[i]->fval; p.variable_list[i]->fval = p.pre_values[i]; }
                G->iniParams = p.pre_parameters;
            }
            ~Restore()
            {
                for (size_t i = 0; i < p.variable_list.size(); ++i) p.variable_list[i]->fval = now[i];
                G->iniParams = std::move(parameters);
            }
        } restore(p);
        input_adapter = &p.adapter; input_context = &p.context;
        assets::FilePlan ignored; std::wstring why;
        DrawPlanAdapter::OriginalCall between;
        if(p.config.shader_hooks)between=[&](CommandListState&,std::wstring&){++G->frame_no;p.context.ResetCallCounters();return true;};
        p.adapter.tolerate_refusals(true);
        const bool presented = p.execute(G->present_command_list, G->post_present_command_list, ignored, why, std::move(between));
        p.adapter.tolerate_refusals(false);
        if (!presented) throw std::runtime_error("Present replay requires unavailable render context: " + utf8(why));
        if (!ignored.items.empty())
            throw std::runtime_error("The Present's draw items have no executor here (" + std::to_string(ignored.items.size()) + ")");
        ++p.stats.replays; return true;
    } catch (const std::exception& e) {
        if(impl_->extension)impl_->extension->execution_failed();
        reason = e.what(); return false;
    }
}

bool IniRuntime::save(std::string& reason)
{
    try {
        auto& p = *impl_;
        // The file is written outside the engine lock, which every draw and presentation of every
        // generation takes: a flush to disk under it held the game's threads (BUG-058).
        std::lock_guard<std::mutex> saving(p.save_mx);
        std::string bytes;
        {
            Scope scope(p.state);
            if (!p.can_save || !G->user_config_dirty) return true;
            std::ostringstream text; text.imbue(std::locale::classic());
            text << "; EFMI Vulkan persistent original INI variables\n[Constants]\n" << std::setprecision(9);
            for (const auto* value : persistent_variables) {
                if (!std::isfinite(value->fval)) throw std::runtime_error("Non-finite persistent value");
                // In UTF-8 whatever the script, as the original writes d3dx_user.ini (setlocale UTF-8, %ls), and
                // as read_ini reads it back: a namespace outside ASCII (a Mod folder's name) must not stop the
                // save of every Mod's settings (LIMIT-042).
                text << utf8(value->name) << " = " << value->fval << '\n';
            }
            bytes = text.str();
            // Clean from this text on; a change after it marks the variables dirty again.
            G->user_config_dirty = false;
        }
        auto write = [&] {
            std::filesystem::create_directories(p.config.settings_directory);
            const auto temp = p.settings.wstring() + L".tmp";
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create settings temporary file");
            DWORD written = 0;
            const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
            CloseHandle(file);
            if (!ok || !MoveFileExW(temp.c_str(), p.settings.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot atomically publish settings; previous file retained");
        };
        try { write(); }
        catch (...) { Scope scope(p.state); G->user_config_dirty = true; throw; }
        Scope scope(p.state); ++p.stats.saves; return true;
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
RuntimeStatus IniRuntime::status() const
{
    auto& p = *impl_; Scope scope(p.state); auto result = p.stats;
    // The original's warnings as they stand now: the load's, and those a Present or a draw raised since
    // (a command the plan does not take, D-146 and D-155), kept by the host to the last 64.
    result.diagnostics = HostDiagnosticCount() - p.diagnostics_base;
    result.diagnostic_messages = HostDiagnosticListSince(p.diagnostics_base);
    result.commands = p.adapter.command_timing();
    for (const auto& gap : p.state.host.gaps) result.gaps.push_back(gap.second);
    result.dirty = G->user_config_dirty; result.reload_requested = G->gReloadConfigPending;
    for (const auto& item : command_list_globals) result.variables[utf8(item.first)] = item.second.fval;
    return result;
}
uint32_t IniRuntime::gap_count() const { return impl_->state.gaps.load(std::memory_order_relaxed); }
std::vector<std::string> IniRuntime::gaps_since(uint32_t seen) const
{
    auto& p = *impl_; Scope scope(p.state);
    std::vector<std::string> result;
    for (const auto& gap : p.state.host.gaps) if (gap.first > seen) result.push_back(gap.second);
    return result;
}
std::vector<IniRuntime::DiscardGate> IniRuntime::discard_gates() const
{
    auto& p = *impl_; Scope scope(p.state); return p.gates;
}
std::vector<std::shared_ptr<const assets::FileResource>> IniRuntime::resources() const
{
    return impl_->resources;
}
std::vector<std::vector<int>> IniRuntime::key_combos() const
{
    Scope scope(impl_->state); return ::RuntimeInputKeys();
}
void IniRuntime::library_hook(LibraryHook hook)
{
    Scope scope(impl_->state); if (impl_->extension) impl_->extension->library_hook(hook);
}
bool IniRuntime::take_reload_request()
{
    Scope scope(impl_->state); const bool result = G->gReloadConfigPending; G->gReloadConfigPending = false; return result;
}
void IniRuntime::suspend_persistence()
{
    // After a write in progress: the generation that follows saves last.
    std::lock_guard<std::mutex> saving(impl_->save_mx);
    Scope scope(impl_->state); impl_->can_save = false;
}
void IniRuntime::inherit_persistent(const IniRuntime& previous)
{
    std::map<std::wstring, float> values;
    RuntimeInput held;
    { Scope scope(previous.impl_->state); held = previous.impl_->last_input; for (const auto* value : persistent_variables) values.emplace(value->name, value->fval); }
    { Scope scope(impl_->state);
      if (held.foreground) impl_->blocked_keys = held.keys;
      for (auto* value : persistent_variables) {
        const auto found = values.find(value->name);
        if (found != values.end() && value->fval != found->second) { value->fval = found->second; G->user_config_dirty = true; }
    } }
}
}
