#include "character_core.h"
#include "runtime_host.h"
#include "skeleton_plan.h"
#include "palette_files.h"
#include "rabbit_textures.h"
#include "efmittl_bridge.h"
#include "buffer_queries.h"
#include "layer_gap.h"
#include "globals.h"
#include "Overlay.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>

void HostCompileGraph(const std::wstring&, const std::vector<std::wstring>&);
// Pinned original resource identity encoding in ResourceHash.cpp.
float EncodeFloat30(const uint32_t hash);

namespace efmi::frontend {
namespace {
const std::wstring core_namespace = L"\\efmiv1\\";
std::wstring command(const wchar_t* name) { return L"commandlist" + core_namespace + name; }
std::string narrow(const std::wstring& value)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(n, 0)), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
// The EFMI Core versions whose lists, variables and resources the native handlers were written against
// (the installed Mods require 1.41 to 1.48, the installed Core is 1.49). A newer Core is taken with a
// notice as long as it declares every name the handlers use (check_interface).
constexpr float kKnownCore = 1.49f;
// A name of the Core the native handlers use that its files do not declare: said with the name, where a
// container's lookup error said nothing (a Core release that renames one).
[[noreturn]] void core_missing(const std::wstring& name)
{
    throw std::runtime_error("EFMI Core interface: the Core's files do not declare " + narrow(name) +
                             "; this layer binds EFMI Core 1.41 to 1.49 by its names");
}
CommandListVariable& var(const wchar_t* name)
{
    const auto found = command_list_globals.find(L"$" + core_namespace + name);
    if (found == command_list_globals.end()) core_missing(L"$" + core_namespace + name);
    return found->second;
}
ExplicitCommandListSection& list(const wchar_t* name)
{
    const auto found = explicitCommandListSections.find(command(name));
    if (found == explicitCommandListSections.end()) core_missing(L"[" + command(name) + L"]");
    return found->second;
}
CustomResource& core_resource(const wchar_t* name)
{
    const auto found = customResources.find(L"resource" + core_namespace + name);
    if (found == customResources.end()) core_missing(L"[resource" + core_namespace + name + L"]");
    return found->second;
}
// A name given in UTF-8 (a Mod's namespace from the layer) as the parser spells it. A byte-by-byte
// widening sign-extended every byte outside ASCII (U+FFE6 for 0xE6), so a Mod named in another script
// never found its own sections (D-154).
std::wstring widen(const std::string& value)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), out.data(), n);
    return out;
}
uint32_t integer(float value)
{
    if (!std::isfinite(value) || value < 0 || value > 65536 || std::floor(value) != value) throw std::runtime_error("Invalid native Core integer");
    return uint32_t(value);
}
class CharacterCore : public RuntimeExtension {
    struct FoundSection { efvk::CharacterSection section; uint32_t original = efvk::kCharacterNone; };
    // One character Mod of the generation (D-131): its contract, what discovery found of it and what
    // its preparation made. A generation of one Mod has one, a folder generation one per character Mod.
    struct Mod {
        std::wstring mod; // `\namespace\`
        efvk::CharacterPackage expected;
        // Discovery (D-078): the Mod's mesh sections as the registry holds them, with the original
        // component id and LOD they assign; the contract is completed once the skeleton is known.
        std::shared_ptr<CharacterDiscovery> discovery, facts;
        std::vector<FoundSection> found;
        size_t outside_package = 0;
        std::vector<uint32_t> original_components;
        CustomResource* skeleton = nullptr;
        bool prepared = false;
        std::unordered_set<const CommandList*> roots; // its TextureOverrides' lists (DrawPlanAdapter::current_root)
    };
    std::vector<Mod> mods_;
    // A folder generation (D-131): several Mods, a draw names the one its identity resolved to.
    bool folder_ = false;
    std::unordered_map<const CommandList*, Mod*> root_mods_; // a TextureOverride's list to its Mod
    Mod* preparing_ = nullptr; // the Mod whose initializer prepare() runs
    // The Mod a section belongs to, by its name's namespace (the longest that matches); null for none.
    Mod* mod_of(const std::wstring& section, const wchar_t* kind)
    {
        Mod* best = nullptr; size_t length = 0;
        const size_t prefix = wcslen(kind);
        for (auto& m : mods_)
            if (m.mod.size() > length && section.size() >= prefix + m.mod.size() && !_wcsnicmp(section.c_str(), kind, prefix) &&
                !_wcsnicmp(section.c_str() + prefix, m.mod.c_str(), m.mod.size())) { best = &m; length = m.mod.size(); }
        return best;
    }
    // The Mod whose draw this is: the one the renderer's identity resolved it to.
    Mod* owner()
    {
        if (!folder_) return &mods_[0];
        return current.mod < mods_.size() ? &mods_[current.mod] : nullptr;
    }
    // The Mod whose list calls the Core: in a generation of one Mod that Mod; in a folder generation the
    // Mod whose initializer is prepared, else the one whose TextureOverride's list runs.
    Mod* caller()
    {
        if (!folder_) return &mods_[0];
        if (preparing_) return preparing_;
        const CommandList* root = adapter ? adapter->current_root() : nullptr;
        if (!root) return nullptr;
        const auto found = root_mods_.find(root);
        return found == root_mods_.end() ? nullptr : found->second;
    }
    void add(const std::string& name, const efvk::CharacterPackage& package) { add(widen(name), package, name); }
    void add(const std::wstring& name, const efvk::CharacterPackage& package, const std::string& narrow = {})
    {
        // A declared namespace, or the name the runtime parses an INI without one under (its path
        // relative to the Mod folder, as the original names such a file by its path; in a folder
        // generation the path from the root, with its separators): printable ASCII in lower case,
        // without the brackets of section names.
        // The names are the parser's own (a declared namespace, or the INI's path from the original's
        // root: any script, lower case as it lowers them), for a folder generation and one Mod alike.
        if (name.empty() || name.size() > 260 || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 0x20 || c == L'[' || c == L']'; }))
            throw std::runtime_error("Mod namespace must be printable without brackets");
        (void)narrow;
        Mod m;
        m.mod = L"\\" + name + L"\\";
        m.expected = package;
        // Only contracts are needed here; do not duplicate the mesh payload.
        m.expected.payload.clear(); m.expected.payload.shrink_to_fit();
        mods_.push_back(std::move(m));
    }
    DrawPlanAdapter* adapter = nullptr;
    // The Core's variables this extension reads per draw, found by name once (a string built and
    // hashed per access cost a few microseconds per matched draw); the generation's variable map is
    // stable, its nodes stay where they are.
    std::unordered_map<const wchar_t*, CommandListVariable*> variables_;
    CustomResourcePool* identity_pool_ = nullptr;
    bool identity_pool_known_ = false;
    CustomResource* original_vs_cb_ = nullptr;
    bool original_vs_cb_known_ = false;
    // The Core's lists and resources the native handlers reach per draw, found by name once.
    ExplicitCommandListSection* connect_list_ = nullptr;
    ExplicitCommandListSection* draw_custom_list_ = nullptr;
    CustomResource* output_skeleton_ = nullptr;
    ExplicitCommandListSection& cached_list(ExplicitCommandListSection*& slot, const wchar_t* name)
    {
        if (!slot) slot = &list(name);
        return *slot;
    }
    CommandListVariable& v(const wchar_t* name)
    {
        const auto found = variables_.find(name);
        if (found != variables_.end()) return *found->second;
        auto& value = var(name);
        variables_.emplace(name, &value);
        return value;
    }
    RuntimeDraw current;
    bool attached = false;
    std::unique_ptr<EfmittlBridge> efmittl; // the library's hooks and capture, when it is loaded (D-113)
    bool rabbit_textures = false;
    bool textures = false;                 // the host answers texture queries (D-077)
    std::set<std::wstring> version_told_;  // lines whose Mod was told about its required version
    size_t texture_sections = 0;
    // The constant a section's command list assigns to `name`, at its top level or inside its `if`
    // branches (EFMI exports guard the assignments with `if $mod_enabled && DRAW_TYPE == 4`): one
    // value when every assignment found gives the same constant, none when there is none, one that is
    // not constant, or two that differ. The live plan still runs the branches as the original does.
    static void assignments(const CommandList* list, const std::wstring& name, std::set<uint32_t>& values, bool& unknown, unsigned depth = 0)
    {
        if (!list || depth > 32) { unknown |= depth > 32; return; }
        for (const auto& held : list->commands) {
            // Adapted lists hold the original command inside the planner's bridge.
            const CommandListCommand* command = DrawPlanAdapter::original(*held);
            if (const auto* branch = dynamic_cast<const IfCommand*>(command)) {
                assignments(branch->true_commands_pre.get(), name, values, unknown, depth + 1);
                assignments(branch->false_commands_pre.get(), name, values, unknown, depth + 1);
                continue;
            }
            const auto* assignment = dynamic_cast<const VariableAssignment*>(command);
            if (!assignment || !assignment->var || _wcsicmp(assignment->var->name.c_str(), name.c_str())) continue;
            float value = 0;
            if (const_cast<CommandListExpression&>(assignment->expression).static_evaluate(&value) &&
                std::isfinite(value) && value >= 0 && value < 65536 && std::floor(value) == value)
                values.insert(uint32_t(value));
            else unknown = true;
        }
    }
    // Constant values assigned to `name` anywhere in the mod's command lists (a restore from a saved
    // variable is not a value of its own).
    uint32_t identification_threshold(const Mod& m) const
    {
        const std::wstring name = L"$" + core_namespace + L"identification_min_components";
        const std::wstring prefix = L"commandlist" + m.mod;
        std::set<uint32_t> values;
        for (const auto& section : explicitCommandListSections) {
            if (_wcsnicmp(section.first.c_str(), prefix.c_str(), prefix.size())) continue;
            bool unknown = false;
            assignments(&section.second.command_list, name, values, unknown);
        }
        if (values.size() == 1 && *values.begin() > 0 && *values.begin() <= 16) return *values.begin();
        const auto core = command_list_globals.find(name);
        if (core != command_list_globals.end() && std::isfinite(core->second.fval) && core->second.fval >= 1 && core->second.fval <= 16)
            return uint32_t(core->second.fval);
        return 0;
    }
    // A `handling = skip` anywhere in the list (inside its branches too): the section hides a draw.
    static bool skips(const CommandList* list, unsigned depth = 0)
    {
        if (!list || depth > 32) return false;
        for (const auto& held : list->commands) {
            const CommandListCommand* command = DrawPlanAdapter::original(*held);
            if (dynamic_cast<const SkipCommand*>(command)) return true;
            if (const auto* branch = dynamic_cast<const IfCommand*>(command))
                if (skips(branch->true_commands_pre.get(), depth + 1) || skips(branch->false_commands_pre.get(), depth + 1)) return true;
        }
        return false;
    }
    // Every constant element-offset edit in a list (and its branches) into `out`.
    static void element_edits(const CommandList* list, std::map<std::tuple<uint32_t, std::string, uint32_t>, std::set<uint32_t>>& out,
                              unsigned depth = 0)
    {
        if (!list || depth > 32) return;
        for (const auto& held : list->commands) {
            const CommandListCommand* command = DrawPlanAdapter::original(*held);
            if (const auto* branch = dynamic_cast<const IfCommand*>(command)) {
                element_edits(branch->true_commands_pre.get(), out, depth + 1);
                element_edits(branch->false_commands_pre.get(), out, depth + 1);
                continue;
            }
            const auto* edit = dynamic_cast<const LayoutElementOperation*>(command);
            if (!edit || edit->override.replace.aligned_byte_offset == UINT32_MAX || !edit->dst.member_args[1].expression) continue;
            float index = 0;
            if (!edit->dst.member_args[1].expression->static_evaluate(&index) || !std::isfinite(index) || index < 0 || index > 255) continue;
            out[{edit->override.match.input_slot, edit->override.match.semantic_name, uint32_t(index)}].insert(edit->override.replace.aligned_byte_offset);
        }
    }
    std::map<std::tuple<uint32_t, std::string, uint32_t>, std::set<uint32_t>> element_offsets(const Mod& m) const
    {
        std::map<std::tuple<uint32_t, std::string, uint32_t>, std::set<uint32_t>> out;
        const std::wstring lists = L"commandlist" + m.mod, overrides = L"textureoverride" + m.mod;
        for (const auto& section : explicitCommandListSections)
            if (!_wcsnicmp(section.first.c_str(), lists.c_str(), lists.size())) {
                element_edits(&section.second.command_list, out);
                element_edits(&section.second.post_command_list, out);
            }
        for (const auto& hashed : G->mTextureOverrideMap)
            for (const auto& entry : hashed.second)
                if (!_wcsnicmp(entry.ini_section.c_str(), overrides.c_str(), overrides.size())) {
                    element_edits(&entry.command_list, out);
                    element_edits(&entry.post_command_list, out);
                }
        return out;
    }
    static uint32_t assigned(const CommandList& list, const std::wstring& name)
    {
        std::set<uint32_t> values;
        bool unknown = false;
        assignments(&list, name, values, unknown);
        return !unknown && values.size() == 1 ? *values.begin() : efvk::kCharacterNone;
    }
    // LIMIT-041: an EFMI export may tell a component's LODs apart by the vertex layout, branches
    // `if vbN->stride == K` of its TextureOverride each assigning one component and LOD. The original
    // evaluates the condition per draw from the stride of the bound vertex buffer (GetResourceStride,
    // IAGetVertexBuffers); here each branch is a section of its own, which a draw matches while its
    // pipeline binds slot N with stride K.
    struct StrideChoice { uint32_t slot, stride, component, lod; };
    static bool stride_condition(const CommandListExpression& expression, uint32_t& slot, uint32_t& stride)
    {
        const auto* op = dynamic_cast<const CommandListOperator*>(expression.evaluatable.get());
        if (!op || op->token != L"==") return false;
        auto side = [&](const std::shared_ptr<CommandListEvaluatable>& target, const std::shared_ptr<CommandListEvaluatable>& value) {
            const auto* operand = dynamic_cast<const CommandListOperand*>(target.get());
            float k = 0;
            if (!operand || operand->type != ParamOverrideType::TEXTURE || operand->texture_filter_target.type != ResourceCopyTargetType::VERTEX_BUFFER ||
                operand->texture_filter_target.evaluation_mode != ResourceCopyTargetEvaluationMode::RESOURCE_STRIDE ||
                operand->texture_filter_target.slot_expression || operand->texture_filter_target.slot >= 4)
                return false;
            if (!value || !value->static_evaluate(&k) || !(k >= 1 && k <= 2048) || std::floor(k) != k) return false;
            slot = operand->texture_filter_target.slot; stride = uint32_t(k);
            return true;
        };
        return side(op->lhs, op->rhs) || side(op->rhs, op->lhs);
    }
    // The choices of such a list, in its order; false when the component is assigned outside them or a
    // branch assigns no single constant component.
    static bool stride_choices(const CommandList* list, const std::wstring& component, const std::wstring& lod, std::vector<StrideChoice>& out,
                               unsigned depth = 0)
    {
        if (!list) return true;
        if (depth > 32) return false;
        for (const auto& held : list->commands) {
            const CommandListCommand* command = DrawPlanAdapter::original(*held);
            const auto* branch = dynamic_cast<const IfCommand*>(command);
            if (!branch) {
                const auto* assignment = dynamic_cast<const VariableAssignment*>(command);
                if (assignment && assignment->var && !_wcsicmp(assignment->var->name.c_str(), component.c_str())) return false;
                continue;
            }
            StrideChoice choice{};
            if (stride_condition(branch->expression, choice.slot, choice.stride)) {
                if (!branch->true_commands_pre) return false;
                choice.component = assigned(*branch->true_commands_pre, component);
                choice.lod = assigned(*branch->true_commands_pre, lod);
                if (choice.component == efvk::kCharacterNone) return false;
                out.push_back(choice);
                // An `elif` continues the choices in the branch not taken.
                if (!stride_choices(branch->false_commands_pre.get(), component, lod, out, depth + 1)) return false;
                continue;
            }
            if (!stride_choices(branch->true_commands_pre.get(), component, lod, out, depth + 1) ||
                !stride_choices(branch->false_commands_pre.get(), component, lod, out, depth + 1)) return false;
        }
        return true;
    }
    static bool constant(const FuzzyMatch& f, uint32_t& value)
    {
        if (f.op != FuzzyMatchOp::EQUAL || f.rhs_type1 != FuzzyMatchOperandType::VALUE || f.rhs_type2 != FuzzyMatchOperandType::VALUE ||
            f.mask != UINT32_MAX || f.numerator != 1 || f.denominator != 1) return false;
        value = f.val; return true;
    }
    // The instance entry of the current draw as the original Core's `Original_VS_CB` (D-086).
    BufferQueries instance_queries;
    // The original Core's identification result for instance 0 of the current draw
    // (`$PoolSpatialIdentity_IdentifiedInstances[0]`); null when the pool is absent.
    CommandListVariable* identified_instance(bool assignment)
    {
        if (!identity_pool_known_) {
            const auto found = customResourcePools.find(L"pool" + core_namespace + L"spatialidentity_identifiedinstances");
            identity_pool_ = found == customResourcePools.end() ? nullptr : &found->second;
            identity_pool_known_ = true;
        }
        if (!identity_pool_) return nullptr;
        auto* value = identity_pool_->GetVariable(0, false, false, assignment);
        return value == identity_pool_->variable_template.get() ? nullptr : value;
    }
    std::vector<std::wstring> boundaries;
    bool fail(std::wstring& reason, const wchar_t* message) { reason = message; return false; }
    // `@oN` and `@oD` in a replaced draw's plan (D-087): bound when the draw's subpass has that
    // attachment; the original's identity is a hash of the bound resource, here one of the
    // attachment within its render pass. Other questions about the draw's render targets refuse.
    bool render_target(ResourceCopyTarget& target, CommandListState& state, float& value, std::wstring& why)
    {
        const bool identity = target.evaluation_mode == ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY;
        if ((!identity && target.evaluation_mode != ResourceCopyTargetEvaluationMode::RESOURCE) || target.custom_resource_pool)
            return fail(why, L"Only the presence and identity of a replaced draw's render targets are known");
        if (!current.targets_known) return fail(why, L"The replaced draw's render targets are not known");
        unsigned slot = 8;
        if (target.type == ResourceCopyTargetType::RENDER_TARGET) {
            slot = target.ResolveSlot(&state);
            if (slot >= 8) return fail(why, L"Render target slot out of range");
        }
        // `oN` itself (D-089): -0 for none bound, as the original; a bound game target matches no
        // TextureOverride of a Mod here (a DX11 render target hash has no Vulkan counterpart), 0.
        if (!identity) { value = current.render_targets >> slot & 1 ? 0.0f : -0.0f; return true; }
        if (!(current.render_targets >> slot & 1)) { value = 0; return true; }
        const uint64_t mixed = current.render_pass * 0x9E3779B97F4A7C15ull ^ (uint64_t(slot) + 1) * 0xC2B2AE3D27D4EB4Full;
        value = EncodeFloat30(uint32_t(mixed ^ mixed >> 32) | 1u);
        return true;
    }
    // The Core reads `lod_level` only to pick the component's LOD remap (MergedSkeleton.ini:
    // `LodRemaps[component * levels + lod_level]`), so a draw is the original's when the remap the Core
    // picks is the one the layer imported the bones with, whatever the two LOD numbers. In one variable
    // space for the folder, as in the original's, a Mod that never hands its LOD to the Core (Typhoeus,
    // whose LOD1 parts are components of their own) leaves the value another Mod's draw set, and DX11
    // draws it all the same. LOD0 has no remap by the contract; LOD1's is the component's (none for
    // most); beyond LOD1 the remap is not known here.
    bool lod_agrees(const efvk::CharacterPackage& expected)
    {
        const auto lod = integer(v(L"lod_level").fval);
        return lod == current.lod || ((lod == 0 || lod == 1) && !expected.components[current.component].has_remap);
    }
    bool contract(const Mod& m, std::wstring& reason)
    {
        const auto& expected = m.expected;
        return (current.identity && current.instances == 1 && !current.first_instance && current.component < expected.components.size() && current.lod < 2 &&
            m.original_components.size() == expected.components.size() &&
            integer(v(L"component_id").fval) == m.original_components[current.component] && lod_agrees(expected) &&
            integer(v(L"bones_count").fval) == expected.bones && v(L"gpu_posed").fval == 1 && v(L"custom_mesh_scale").fval == 1) ||
            fail(reason, L"Original Core request differs from the resolved native identity, bone or LOD contract");
    }
public:
    // The version the installed Core declares (`global $version` of its main.ini), 0 when not known.
    float core_version_ = 0;
    void set_facts(std::shared_ptr<CharacterDiscovery> out) { mods_[0].facts = std::move(out); }
    CharacterCore(const std::string& name, std::shared_ptr<CharacterDiscovery> output)
        : CharacterCore(name, efvk::CharacterPackage{}, false) { mods_[0].discovery = std::move(output); }
    CharacterCore(const std::string& name, const efvk::CharacterPackage& package, bool rabbit) : rabbit_textures(rabbit) { add(name, package); }
    // A folder generation (D-131): every character Mod of the Mods folder with its discovery output, in
    // the order the draws name them (RuntimeDraw::mod).
    explicit CharacterCore(const std::vector<std::pair<std::wstring, std::shared_ptr<CharacterDiscovery>>>& mods) : folder_(true)
    {
        if (mods.empty() || mods.size() > 4096) throw std::runtime_error("A folder generation holds one to 4096 character Mods");
        for (const auto& m : mods) {
            if (!m.second) throw std::runtime_error("Character discovery needs its output");
            add(m.first, {}); mods_.back().discovery = m.second;
        }
    }
    std::wstring declarations() override
    {
        // What the Core's main.ini declares for registration (the generation loads the Core's API,
        // skeleton, identification and helper files, not its GUI): `$version` as the installed Core states it.
        wchar_t version[64];
        swprintf_s(version, L"global $version = %g\n", double(core_version_ > 0 ? core_version_ : kKnownCore));
        return std::wstring(L"namespace = EFMIv1\n[Constants]\n") + version + LR"ini(global $required_version = 0
global $object_guid = 0
global $mod_id = -1
[ResourceModName]
[ResourceModAuthor]
[ResourceModDesc]
[ResourceModLink]
[ResourceModLogo]
[CommandListRegisterMod]
)ini";
    }
    std::vector<std::wstring> declaration_units() override
    {
        return {rabbit_textures ? rabbit_texture_declarations() : rabbitfx_library_declarations()};
    }
    std::vector<std::wstring> native_shaders() override
    {
        std::vector<std::wstring> names;
        for (const auto* name : {L"mergedskeletoninitializer", L"_mergedskeleton_bonesimporter_rollingbuffer", L"_mergedskeleton_bonesimporter_constantbuffer",
                                L"_mergedskeleton_instanceconfigoverrider_rollingbuffer", L"_mergedskeleton_instanceconfigoverrider_constantbuffer"})
            names.push_back(L"customshader" + core_namespace + name);
        return names;
    }
    // Every name of the Core the native handlers use, looked up once: a Core release without one is
    // said in one statement at load, not found by a draw.
    void check_interface()
    {
        for (const auto* name : {L"initializemergedskeleton", L"component_readconfig", L"mergedskeleton_detectbonedatasource",
                                 L"spatialidentity_identifycomponentinstances", L"mergedskeleton_attachcomponent", L"component_drawinstances",
                                 L"callback_mergedskeleton_connectcomponent", L"callback_component_drawcustom"}) list(name);
        for (const auto* name : {L"bones_count", L"bones_data_source", L"component_id", L"custom_mesh_scale", L"draw_call_has_modded_instance",
                                 L"draw_call_has_unknown_instance", L"enum_bones_source_vs_t0", L"enum_override_type_custom",
                                 L"enum_override_type_original", L"enum_override_type_skip", L"gpu_posed", L"instance_config_vs_cb_slot",
                                 L"lod_level", L"cfg_ms_max_lod_level_count", L"version", L"required_version", L"mod_id"}) var(name);
        for (const auto* name : {L"outputmergedskeleton_template", L"output_mergedskeleton"}) core_resource(name);
    }
    std::vector<std::wstring> install(DrawPlanAdapter& a) override
    {
        adapter = &a;
        check_interface();
        // `Original_VS_CB->SpatialHash(...)` in a replaced draw's plan: the renderer's instance entry.
        a.plan_resource_queries([this](ResourceCopyTarget& target, CommandListState& state, float& value, std::wstring& why) {
            if (target.type == ResourceCopyTargetType::RENDER_TARGET || target.type == ResourceCopyTargetType::DEPTH_STENCIL_TARGET)
                return render_target(target, state, value, why);
            // `vbN->stride` (an EFMI export telling a component's LODs apart by it): the game's vertex
            // buffer at that slot, from the host, as the original reads it off IAGetVertexBuffers.
            if (target.type == ResourceCopyTargetType::VERTEX_BUFFER && target.evaluation_mode == ResourceCopyTargetEvaluationMode::RESOURCE_STRIDE &&
                !target.custom_resource_pool) {
                uint32_t stride = 0; std::string reason;
                if (!current.vertex_stride || !current.vertex_stride(target.ResolveSlot(&state), stride, reason)) {
                    why = L"The game's vertex buffer stride is not known";
                    if (!reason.empty()) { why += L": "; why.append(reason.begin(), reason.end()); }
                    return false;
                }
                value = float(stride); return true;
            }
            return instance_queries.read(target, state, value, why);
        });
        // `vs`, `ps` in a replaced draw's plan (D-089): a game shader has no DX11 identity here and no
        // ShaderOverride is active, so a bound stage reads 0 and an absent one -0, as the original reads
        // a shader no section names. Its declarations as the original's process_shader_filter reads them
        // (D-124): t registers from the host (RuntimeDraw::source_srv), the masks 0 as the original
        // answers them; constant buffer declarations are not known from SPIR-V and refuse.
        a.plan_shader_queries([this](CommandListOperand& operand, CommandListState& state, float& value, std::wstring& why) {
            const auto& t = operand.shader_target;
            if (!current.shader_stages) return fail(why, L"The replaced draw's shader stages are not known");
            const uint32_t bit = t.shader_type == L'v' ? 0x1u : t.shader_type == L'h' ? 0x2u : t.shader_type == L'd' ? 0x4u :
                                 t.shader_type == L'g' ? 0x8u : t.shader_type == L'p' ? 0x10u : 0u;
            if (!bit) return fail(why, L"A draw has no compute shader");
            // No shader bound: -0 whatever is asked.
            if (!(current.shader_stages & bit)) { value = -0.0f; return true; }
            switch (t.evaluation_mode) {
            case ShaderTargetEvaluationMode::SHADER: case ShaderTargetEvaluationMode::DCL_CB_MASK: case ShaderTargetEvaluationMode::DCL_SRV_MASK:
                value = 0.0f; return true;
            case ShaderTargetEvaluationMode::DCL_SRV_TYPE: case ShaderTargetEvaluationMode::DCL_SRV_STRIDE: case ShaderTargetEvaluationMode::DCL_SRV_DIMENSION: {
                // The slot as the original takes it; its guard keeps it within the 128 registers.
                const unsigned slot = unsigned(operand.shader_target.member_args[0].GetValue(&state));
                uint32_t type = 0, dimension = 0, stride = 0; std::string reason;
                if (!current.source_srv || !current.source_srv(t.shader_type, slot, type, dimension, stride, reason)) {
                    why = L"The declaration of the game's shader resource is not known: ";
                    why.append(reason.begin(), reason.end());
                    return false;
                }
                // A buffer of 32-bit words: structured or raw, the one the adapter is trying (it evaluates
                // a condition with each and goes on when they agree).
                if (type == kWordBufferDeclaration) {
                    const bool raw = adapter->undecided_words(t.shader_type, slot);
                    type = raw ? 3 : 2; dimension = raw ? 11 : 1; stride = raw ? 0 : stride;
                }
                // A slot that is not declared: type 0, and -1 for its stride and dimension, as the original
                // (CommandListOperand's shader target evaluation: "Negative one means SRV is not declared").
                if (!type && t.evaluation_mode != ShaderTargetEvaluationMode::DCL_SRV_TYPE) { value = -1.0f; return true; }
                value = float(t.evaluation_mode == ShaderTargetEvaluationMode::DCL_SRV_TYPE ? type :
                              t.evaluation_mode == ShaderTargetEvaluationMode::DCL_SRV_STRIDE ? stride : dimension);
                return true;
            }
            default: return fail(why, L"The constant buffer declarations of the game's shaders are not known");
            }
        });
        auto native = [&](const wchar_t* name, DrawPlanAdapter::NativeCommand callback) {
            boundaries.push_back(command(name)); a.native(list(name), command(name), std::move(callback));
        };
        native(L"registermod", [this](CommandListState&, unsigned phases, std::wstring&) {
            if (!(phases & 1)) return true;
            // The Core's CommandListRegisterMod (main.ini): `$mod_id = 1`, and -1 when the installed Core is
            // older than the Mod requires (`$version < $required_version`) or the Mod names a version below
            // zero; the Mod then leaves itself off. The comparison is with the installed Core's version, as
            // the original's: a constant of the layer switched off Mods the user's installation runs.
            const float required = v(L"required_version").fval;
            const float installed = var(L"version").fval;
            const std::wstring* line = adapter ? adapter->current_line() : nullptr;
            const bool tell = line && version_told_.size() < 256 && version_told_.insert(*line).second;
            if (!(required <= installed) || required < 0) {
                v(L"mod_id").fval = -1;
                if (tell) LayerGap(L"%ls: the Mod requires EFMI %g and the installed EFMI Core is %g; it stays off, as under the original",
                                   line->c_str(), double(required), double(installed));
                return true;
            }
            if (required > kKnownCore && tell)
                LayerGap(L"%ls: the Mod requires EFMI %g; this layer was checked with EFMI Cores up to %g",
                         line->c_str(), double(required), double(kKnownCore));
            v(L"mod_id").fval = 1; return true;
        });
        native(L"initializemergedskeleton", [this](CommandListState&, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m) return fail(why, L"Native skeleton initialization outside a character Mod");
            auto* target = adapter->referenced_resource(&core_resource(L"outputmergedskeleton_template"));
            if (!target || (m->prepared && target != m->skeleton)) return fail(why, L"Unprepared native skeleton output");
            m->skeleton = target; return true;
        });
        native(L"component_readconfig", [this](CommandListState&, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m) return fail(why, L"Original Core configuration read outside a character Mod");
            // Another Mod's list on this draw reads the configuration as the original does; only the
            // draw's own Mod is held to the renderer's contract.
            if (m == owner() && !contract(*m, why)) return false;
            v(L"instance_config_vs_cb_slot").fval = float(current.source_vs_config_slot);
            return true;
        });
        native(L"mergedskeleton_detectbonedatasource", [this](CommandListState&, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m || !m->prepared || (m == owner() && !contract(*m, why))) return false;
            // Never invent a source slot from Vulkan descriptor coordinates.
            // Unknown input preserves the original Core's no-replacement path.
            v(L"bones_data_source").fval = current.rolling_bone_source && current.source_vs_config_slot != -1
                ? v(L"enum_bones_source_vs_t0").fval : 0;
            return true;
        });
        native(L"spatialidentity_identifycomponentinstances", [this](CommandListState&, unsigned phases, std::wstring& why) {
            // Identity resolution is the renderer's existing recording-clock
            // implementation, already supplied before this original callback: the instance is the
            // Mod's, and the Core's outputs say so.
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m) return fail(why, L"Original Core identification outside a character Mod");
            // The instance is another Mod's (D-131): as the original Core's identification answers that
            // Mod, it has neither a modded nor an unknown instance here.
            if (m != owner()) { v(L"draw_call_has_modded_instance").fval = 0; v(L"draw_call_has_unknown_instance").fval = 0; return true; }
            if (!contract(*m, why)) return false;
            v(L"draw_call_has_modded_instance").fval = 1; v(L"draw_call_has_unknown_instance").fval = 0;
            if (auto* status = identified_instance(true)) status->fval = v(L"enum_override_type_custom").fval;
            return true;
        });
        native(L"mergedskeleton_attachcomponent", [this](CommandListState&, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m || m != owner()) return fail(why, L"Native bone attachment outside the draw's Mod");
            if (!m->prepared || !contract(*m, why)) return false;
            if (!output_skeleton_) output_skeleton_ = &core_resource(L"output_mergedskeleton");
            const auto* output = adapter->referenced_resource(output_skeleton_);
            if (output != m->skeleton) return fail(why, L"Native bone attachment references another skeleton");
            attached = true; return true;
        });
        native(L"component_drawinstances", [this](CommandListState& state, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            Mod* m = caller();
            if (!m) return fail(why, L"Original Core instance draw outside a character Mod");
            // Another Mod's instance (D-131): this Mod draws nothing of it and keeps nothing of the game's.
            if (m != owner()) return true;
            if (!m->prepared || !contract(*m, why)) return false;
            // A Mod that runs the Core's identification itself (D-086) may decide otherwise than the
            // renderer: the original instance is drawn by the application's own draw, a skipped one
            // not at all. No result in this draw leaves the renderer's identification.
            if (const auto* status = identified_instance(false)) {
                if (status->fval == v(L"enum_override_type_original").fval) { adapter->keep_original(); return true; }
                if (status->fval == v(L"enum_override_type_skip").fval) return true;
            }
            attached = false;
            if (!adapter->invoke(cached_list(connect_list_, L"callback_mergedskeleton_connectcomponent"), state, 1, why) || !attached)
                return fail(why, L"Mod did not attach its prepared merged skeleton");
            return adapter->invoke(cached_list(draw_custom_list_, L"callback_component_drawcustom"), state, 1, why);
        });
        if (!rabbit_textures) {
            // RabbitFX (D-082): SetTextures binds the Mod's maps where this draw's fragment shader samples
            // the diffuse, lightmap and normal map; a role the draw has not, an unknown answer or a map
            // the Mod did not set binds nothing, and the game's texture stays.
            const std::wstring resolve = L"commandlist\\rabbitfx\\resolveroles";
            boundaries.push_back(resolve);
            a.native(explicitCommandListSections.at(resolve), resolve, [this](CommandListState&, unsigned phases, std::wstring&) {
                if (!(phases & 1)) return true;
                int32_t roles[3] = {-1, -1, -1};
                std::string reason;
                if (!current.texture_roles || !current.texture_roles(roles, reason)) roles[0] = roles[1] = roles[2] = -1;
                static const wchar_t* const maps[3] = {L"diffuse", L"lightmap", L"normalmap"};
                static const wchar_t* const slots[3] = {L"diffuse_slot", L"lightmap_slot", L"normal_slot"};
                for (int i = 0; i < 3; ++i) {
                    const auto declared = customResources.find(std::wstring(L"resource\\rabbitfx\\") + maps[i]);
                    const bool given = declared != customResources.end() && adapter->referenced_resource(&declared->second);
                    command_list_globals.at(std::wstring(L"$\\rabbitfx\\") + slots[i]).fval =
                        given && roles[i] >= 0 && roles[i] < int32_t(kRabbitRoleSlots) ? float(roles[i]) : -1.0f;
                }
                return true;
            });
        }
        efmittl = EfmittlBridge::install(a, boundaries);
        for (auto& m : mods_) {
            if (m.facts) m.facts->efmittl = efmittl != nullptr;
            if (m.discovery) m.discovery->efmittl = efmittl != nullptr;
        }
        // With the host's texture identities (D-077) the original OverrideTextures runs: its
        // CheckTextureOverride on ps-t0 to ps-t22 finds texture-hash sections by the hash of what is
        // bound. Without them the generation registers mesh overrides only and refuses image-hash
        // sections during preparation, never assigning invented hashes.
        textures = a.has_texture_queries();
        if (!textures)
            native(L"overridetextures", [this](CommandListState&, unsigned phases, std::wstring& why) {
                return !(phases & 1) || std::all_of(mods_.begin(), mods_.end(), [](const Mod& m) { return m.prepared; }) ||
                       fail(why, L"Texture registry is not prepared");
            });
        // Preparation executes the original mod's bone declarations, including
        // its exact LOD1 file references. Runtime connection executes them too.
        for (const auto& m : mods_) {
            // Named when missing (an export of another shape, or a namespace the parser spelled otherwise).
            const std::wstring root = L"commandlist" + m.mod + L"initializemergedskeleton";
            if (!explicitCommandListSections.count(root)) throw std::runtime_error(narrow(L"The Mod declares no [" + root + L"]"));
            HostCompileGraph(root, boundaries);
        }
        return boundaries;
    }
    // What the original does for a section and the layer does not (D-137): the original's warning, and a
    // line for the Mod the section belongs to, which the host shows.
    void gap(const std::wstring& section, const std::wstring& text)
    {
        // Through the original's warning channel and the Mod's own list, which the host shows at load.
        LogOverlayW(LOG_WARNING, const_cast<wchar_t*>(L"[%ls] %ls\n"), section.c_str(), text.c_str());
        Mod* m = folder_ ? mod_of(section, L"textureoverride") : &mods_[0];
        CharacterDiscovery* out = m ? (m->discovery ? m->discovery.get() : m->facts.get()) : nullptr;
        if (out && out->gaps.size() < 16) out->gaps.push_back(narrow(L"[" + section + L"] " + text));
    }
    // Sections matched by a resource's description (match_type, match_width, ...: the original's
    // FuzzyMatchResourceDesc, D-138). One that can match a 2D texture runs where the original's does, at a
    // CheckTextureOverride of a pixel slot, against the host's description of the game's image (the one its
    // texture hash is made of, D-077). One that can match a buffer would also run at the game's draws by
    // their index buffer's description, and one for 1D or 3D textures at theirs: the host describes neither,
    // so these match none of them and the Mod is told. And what the original does with a section when the
    // game creates, maps or copies a resource (format, size, CPU reads, region copies): the layer leaves the
    // game's resources as the game made them.
    bool described_sections(std::wstring& why)
    {
        auto creation = [&](const TextureOverride& o) {
            if (o.format != -1 || o.width != -1 || o.height != -1 || o.width_multiply != 1.0f || o.height_multiply != 1.0f ||
                o.override_byte_width != -1 || o.deny_cpu_read || o.expand_region_copy)
                gap(o.ini_section, L"changes how the game creates, maps or copies the resources it matches; "
                                   L"the Vulkan layer leaves them as the game made them");
        };
        for (const auto& hashed : G->mTextureOverrideMap)
            for (const auto& entry : hashed.second) creation(entry);
        for (const auto& fuzzy : G->mFuzzyTextureOverrides) {
            const auto& section = fuzzy->texture_override->ini_section;
            creation(*fuzzy->texture_override);
            if (fuzzy->matches_buffer)
                gap(section, L"matches buffers by their description, which the Vulkan layer does not give for the game's: it matches none of them");
            if (!fuzzy->matches_tex2d && (fuzzy->matches_tex1d || fuzzy->matches_tex3d))
                gap(section, L"matches 1D or 3D textures by their description; the Vulkan layer describes 2D textures only: it matches none of them");
            if (!fuzzy->matches_tex2d) continue;
            if (!textures) return fail(why, L"Generation contains an unprofiled override; source-image identity is unavailable");
            ++texture_sections;
        }
        return true;
    }
    bool validate_overrides(std::wstring& why) override
    {
        if (!described_sections(why)) return false;
        if (std::any_of(mods_.begin(), mods_.end(), [](const Mod& m) { return m.discovery != nullptr; })) return discover_sections(why);
        auto matches = [](const FuzzyMatch& f, uint32_t value, bool) {
            // None: any value (an optional first index, a hash-only section's index count).
            if (value == efvk::kCharacterNone) return f.op == FuzzyMatchOp::ALWAYS;
            return f.op == FuzzyMatchOp::EQUAL && f.rhs_type1 == FuzzyMatchOperandType::VALUE &&
                f.rhs_type2 == FuzzyMatchOperandType::VALUE && f.val == value && f.mask == UINT32_MAX &&
                f.numerator == 1 && f.denominator == 1;
        };
        std::vector<std::vector<bool>> seen;
        for (const auto& m : mods_) seen.emplace_back(m.expected.sections.size(), false);
        for (const auto& hashed : G->mTextureOverrideMap) {
            for (const auto& entry : hashed.second) {
                const bool mesh = entry.match_index_count.op != FuzzyMatchOp::ALWAYS;
                // In a generation of one Mod every section is checked against it, as before.
                Mod* m = folder_ ? mod_of(entry.ini_section, L"textureoverride") : &mods_[0];
                if (m) {
                    const auto& expected = m->expected;
                    auto& own_seen = seen[size_t(m - mods_.data())];
                    // The entry's expected sections: one, or one per branch of a list that chooses the
                    // component by `vbN->stride` (LIMIT-041).
                    std::vector<size_t> indices;
                    for (size_t index = 0; index < expected.sections.size(); ++index) {
                        const auto& s = expected.sections[index];
                        if (s.hash == hashed.first && matches(entry.match_index_count, s.match_count, false) &&
                            matches(entry.match_first_index, s.match_first, true) && !own_seen[index] &&
                            (indices.empty() || s.stride_slot != efvk::kCharacterNone)) indices.push_back(index);
                        if (indices.size() == 1 && expected.sections[indices[0]].stride_slot == efvk::kCharacterNone) break;
                    }
                    const std::wstring prefix = L"textureoverride" + m->mod;
                    if (!indices.empty() && !_wcsnicmp(entry.ini_section.c_str(), prefix.c_str(), prefix.size())) {
                        for (const size_t index : indices) own_seen[index] = true;
                        continue;
                    }
                    // A mesh section the renderer has no part for (a LOD beyond its scope) stays the
                    // game's; an image-hash section runs against the host's texture identities.
                    if (mesh) { ++m->outside_package; continue; }
                } else if (mesh) continue;
                if (!textures) return fail(why, L"Generation contains an unprofiled override; source-image identity is unavailable");
                ++texture_sections;
            }
        }
        for (const auto& own_seen : seen)
            if (!std::all_of(own_seen.begin(), own_seen.end(), [](bool v) { return v; })) return fail(why, L"Original mesh override registry differs from its profile");
        return true;
    }
    // Discovery: a mesh section of a Mod (a constant index count, an optional constant first index) is
    // part of its contract; the component it assigns decides what it draws, none makes it a candidate
    // to hide the game's mesh, which the live plan decides. Everything else stays the game's or, with
    // texture identities, runs against them, as for a package.
    bool discover_sections(std::wstring& why)
    {
        const std::wstring component_id = L"$" + core_namespace + L"component_id";
        for (const auto& hashed : G->mTextureOverrideMap) {
            for (const auto& entry : hashed.second) {
                const bool mesh = entry.match_index_count.op != FuzzyMatchOp::ALWAYS;
                // A generation of one Mod: its own sections by its prefix, every other one beside them.
                Mod* m = folder_ ? mod_of(entry.ini_section, L"textureoverride") : &mods_[0];
                const bool own = m && !_wcsnicmp(entry.ini_section.c_str(), (L"textureoverride" + m->mod).c_str(), m->mod.size() + 15);
                FoundSection s;
                s.section.hash = hashed.first;
                const bool first_ok = entry.match_first_index.op == FuzzyMatchOp::ALWAYS || constant(entry.match_first_index, s.section.match_first);
                if (own && m->discovery && mesh && constant(entry.match_index_count, s.section.match_count) && s.section.match_count && first_ok) {
                    const std::wstring lod_level = L"$" + m->mod + L"lod_level";
                    s.original = assigned(entry.command_list, component_id);
                    s.section.lod = assigned(entry.command_list, lod_level);
                    std::vector<StrideChoice> choices;
                    std::set<uint32_t> strides;
                    if (s.original == efvk::kCharacterNone && stride_choices(&entry.command_list, component_id, lod_level, choices) && !choices.empty() &&
                        std::all_of(choices.begin(), choices.end(), [&](const StrideChoice& c) { return c.slot == choices[0].slot && strides.insert(c.stride).second; })) {
                        // One section per branch (LIMIT-041).
                        for (const auto& c : choices) {
                            FoundSection branch = s;
                            branch.original = c.component; branch.section.lod = c.lod;
                            branch.section.stride_slot = c.slot; branch.section.stride = c.stride;
                            m->found.push_back(branch);
                        }
                        continue;
                    }
                    m->found.push_back(s);
                    continue;
                }
                // A hash alone that skips the draw hides a game mesh by its index region's hash,
                // whatever its index count (as the original matches it); it is never a component.
                if (own && m->discovery && !mesh && entry.match_first_index.op == FuzzyMatchOp::ALWAYS && skips(&entry.command_list)) {
                    s.section.match_count = efvk::kCharacterNone; s.section.match_first = efvk::kCharacterNone;
                    m->found.push_back(s);
                    continue;
                }
                if (mesh) { if (m) ++m->outside_package; continue; }
                if (!textures) return fail(why, L"Generation contains an unprofiled override; source-image identity is unavailable");
                ++texture_sections;
            }
        }
        for (auto& m : mods_) {
            if (!m.discovery) continue;
            if (m.found.size() > efvk::kCharacterSections) return fail(why, L"More mesh sections than one character contract holds");
            // In one order whatever else the generation holds: the registry is a hash table, whose order
            // changes with the other Mods' sections in it (D-131).
            std::stable_sort(m.found.begin(), m.found.end(), [](const FoundSection& a, const FoundSection& b) {
                return std::tie(a.section.hash, a.section.match_count, a.section.match_first) <
                       std::tie(b.section.hash, b.section.match_count, b.section.match_first);
            });
            m.discovery->declared.clear();
            for (const auto& f : m.found) { auto section = f.section; section.component = f.original; m.discovery->declared.push_back(section); }
        }
        return true;
    }
    // Discovery: the active components in bone order, their LOD remaps, and the sections mapped to
    // them. A section on an inactive component or a LOD beyond 1 stays the game's (the renderer's
    // scope is LOD0 and LOD1, as for a package).
    bool discover_contract(Mod& m, const SkeletonFilePlan& metadata, const core::PaletteDefinition& palette, std::wstring& why)
    {
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < metadata.components.size(); ++i) if (metadata.components[i].count) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return metadata.components[a].first < metadata.components[b].first; });
        if (order.empty() || order.size() > efvk::kCharacterComponents) return fail(why, L"Original skeleton has no or too many active components");
        efvk::CharacterPackage contract;
        contract.bones = metadata.bones;
        std::vector<std::filesystem::path> remaps;
        std::map<uint32_t, uint32_t> dense;
        uint32_t cursor = 0;
        for (uint32_t original : order) {
            const auto& got = metadata.components[original];
            const auto& maps = palette.components[original].lods;
            // Bones between components stay as initialized, as the Core's merged skeleton leaves them.
            if (got.first < cursor) return fail(why, L"Original components overlap");
            cursor = got.first + got.count;
            if (!maps[0].available || !maps[0].indices.empty() || !maps[1].available)
                return fail(why, L"Original component has a LOD0 remap or no LOD1 assignment");
            efvk::CharacterComponent c;
            c.first = got.first; c.count = got.count;
            c.has_remap = !maps[1].indices.empty(); c.remap_lod1 = maps[1].indices;
            const auto& lod1 = got.lods[1];
            if (c.has_remap && (lod1.source != RemapSource::File || lod1.path.empty())) return fail(why, L"Original LOD1 remap without its file");
            remaps.push_back(c.has_remap ? lod1.path : std::filesystem::path{});
            dense.emplace(original, uint32_t(contract.components.size()));
            contract.components.push_back(std::move(c));
        }
        if (cursor > metadata.bones) return fail(why, L"Original components exceed the skeleton");
        size_t left = m.outside_package;
        for (const auto& s : m.found) {
            auto section = s.section;
            if (section.lod != efvk::kCharacterNone && section.lod > 1) { ++left; continue; }
            if (s.original == efvk::kCharacterNone) {
                section.component = efvk::kCharacterNone; section.lod = efvk::kCharacterNone;
            } else {
                const auto it = dense.find(s.original);
                if (it == dense.end()) { ++left; continue; }
                section.component = it->second;
            }
            contract.sections.push_back(section);
        }
        if (contract.sections.empty()) return fail(why, L"The mod declares no mesh section the renderer can take");
        m.expected = contract;
        m.discovery->contract = std::move(contract);
        m.discovery->remap_files = std::move(remaps);
        m.discovery->left_to_game = left;
        return true;
    }
    bool prepare(DrawPlanAdapter& a, CommandListState& state, std::wstring& why) override
    {
        if (efmittl) efmittl->prepare();
        // The lists of each Mod's TextureOverrides: a native tells its caller by the root that runs.
        for (auto& m : mods_) m.roots.clear();
        root_mods_.clear();
        if (folder_)
            for (const auto& hashed : G->mTextureOverrideMap)
                for (const auto& entry : hashed.second)
                    if (Mod* m = mod_of(entry.ini_section, L"textureoverride")) {
                        m->roots.insert(&entry.command_list); m->roots.insert(&entry.post_command_list);
                        root_mods_[&entry.command_list] = m; root_mods_[&entry.post_command_list] = m;
                    }
        struct Preparing { Mod*& slot; ~Preparing() { slot = nullptr; } } preparing{preparing_};
        // Every Mod is tried: one that cannot be prepared is named with its reason (CharacterDiscovery::refused),
        // so the folder leaves out exactly those and loads again, instead of loading every Mod alone to find it.
        std::wstring first;
        for (auto& m : mods_) {
            preparing_ = &m;
            std::wstring reason;
            bool prepared = false;
            try { prepared = prepare_mod(a, m, state, reason); }
            catch (const std::exception& e) { const std::string text = e.what(); reason.assign(text.begin(), text.end()); }
            if (prepared) continue;
            m.prepared = false;
            if (reason.empty()) reason = L"the Mod's skeleton declarations did not run";
            if (const auto out = m.discovery ? m.discovery : m.facts) out->refused = narrow(reason);
            if (first.empty()) first = folder_ ? L"[" + m.mod + L"] " + reason : reason;
        }
        why = first;
        return first.empty();
    }
    bool prepare_mod(DrawPlanAdapter& a, Mod& m, CommandListState& state, std::wstring& why)
    {
        // What the Core's contract needs of the Mod's own declarations, each named when it is missing
        // (an export of another shape refuses with the name, not a map's error).
        const std::wstring init_name = L"commandlist" + m.mod + L"initializemergedskeleton";
        const auto init_found = explicitCommandListSections.find(init_name);
        if (init_found == explicitCommandListSections.end()) return fail(why, (L"The Mod declares no [" + init_name + L"]").c_str());
        auto& init = init_found->second;
        a.adapt(init.command_list); a.adapt(init.post_command_list);
        DrawPlan plan;
        if (!a.execute_pair(init.command_list, init.post_command_list, state, plan, why) || !m.skeleton || !plan.items.empty()) return false;
        const auto pools = L"pool" + m.mod + L"_mergedskeleton_component_";
        SkeletonFilePlan metadata; FilePlanCompiler files(customResources);
        auto variable_of = [&](const wchar_t* name, float& out) {
            const auto found = command_list_globals.find(L"$" + m.mod + name);
            if (found == command_list_globals.end()) return fail(why, (L"The Mod declares no $" + m.mod + name).c_str());
            out = found->second.fval; return true;
        };
        auto pool_of = [&](const wchar_t* name) -> CustomResourcePool* {
            const auto found = customResourcePools.find(pools + name);
            if (found == customResourcePools.end()) { fail(why, (L"The Mod declares no [" + pools + name + L"]").c_str()); return nullptr; }
            return &found->second;
        };
        float bones = 0, instances = 0;
        if (!variable_of(L"bones_count", bones) || !variable_of(L"max_instance_count", instances)) return false;
        auto* offsets = pool_of(L"vertexgroupoffsets"); auto* counts = pool_of(L"vertexgroupcounts"); auto* remaps = pool_of(L"lodremaps");
        if (!offsets || !counts || !remaps) return false;
        if (!read_skeleton_plan(integer(bones), integer(instances), integer(var(L"cfg_ms_max_lod_level_count").fval),
            *offsets, *counts, *remaps, a, files, metadata, why)) return false;
        core::PaletteDefinition palette; std::string error;
        if (!load_palette_files(metadata, palette, error)) { why.assign(error.begin(), error.end()); return false; }
        if (m.discovery && !discover_contract(m, metadata, palette, why)) return false;
        const auto& expected = m.expected;
        if (metadata.bones != expected.bones || metadata.components.size() < expected.components.size())
            return fail(why, L"Original skeleton dimensions changed");
        m.original_components.assign(expected.components.size(), UINT32_MAX);
        for (size_t i = 0; i < metadata.components.size(); ++i) {
            const auto& got = metadata.components[i];
            if (!got.count) continue;
            size_t dense = 0;
            while (dense < expected.components.size() && expected.components[dense].first != got.first) ++dense;
            if (dense == expected.components.size() || m.original_components[dense] != UINT32_MAX)
                return fail(why, L"Unprofiled or overlapping active component");
            const auto& want = expected.components[dense];
            const auto& maps = palette.components[i].lods;
            if (got.first != want.first || got.count != want.count ||
                !maps[0].available || !maps[0].indices.empty() || !maps[1].available ||
                maps[1].indices != want.remap_lod1)
                return fail(why, L"Original bone ranges or LOD1 remaps differ from the renderer profile");
            m.original_components[dense] = uint32_t(i);
        }
        if (std::find(m.original_components.begin(), m.original_components.end(), UINT32_MAX) != m.original_components.end())
            return fail(why, L"Profiled component is absent from original skeleton");
        m.prepared = true;
        if (const auto out = m.discovery ? m.discovery : m.facts) {
            out->identify_components = identification_threshold(m);
            out->element_offsets = element_offsets(m);
        }
        return true;
    }
    bool before_draw(const RuntimeDraw& draw, CommandListState&, std::wstring& why) override
    {
        if (folder_ && draw.mod != UINT32_MAX && draw.mod >= mods_.size()) return fail(why, L"The draw names no Mod of the generation");
        if (draw.source_vs_config_slot != -1 && (draw.source_vs_config_slot < 1 || draw.source_vs_config_slot > 3))
            return fail(why, L"Unsupported original Core instance-config constant slot");
        current = draw;
        instance_queries.clear();
        if (draw.instance_entry && draw.instance_entry_size) {
            if (!original_vs_cb_known_) {
                const auto vs_cb = customResources.find(L"resource" + core_namespace + L"original_vs_cb");
                original_vs_cb_ = vs_cb == customResources.end() ? nullptr : &vs_cb->second;
                original_vs_cb_known_ = true;
            }
            if (original_vs_cb_ && !instance_queries.bind(original_vs_cb_, draw.instance_entry, draw.instance_entry_size, why))
                return false;
        }
        if (auto* status = identified_instance(true)) status->fval = 0;
        if (efmittl) efmittl->before_draw(draw);
        return true;
    }
    void draw_hooks(const RuntimeDraw& draw, DrawHooks& hooks) override { if (efmittl) efmittl->draw_hooks(draw, hooks); }
    void after_draw() noexcept override
    {
        // The host's answers refer to its draw: none of them outlives it.
        current.pixel_texture = nullptr; current.texture_roles = nullptr; current.source_srv = nullptr;
        instance_queries.clear(); if (efmittl) efmittl->after_draw();
    }
    void library_hook(LibraryHook hook) noexcept override { if (efmittl) efmittl->library_hook(hook); }
};
}
std::shared_ptr<RuntimeExtension> character_core(const std::string& mod_namespace, const efvk::CharacterPackage& package, bool rabbit)
{
    return std::make_shared<CharacterCore>(mod_namespace, package, rabbit);
}
std::shared_ptr<RuntimeExtension> character_core(const std::string& mod_namespace, const efvk::CharacterPackage& package,
                                               std::shared_ptr<CharacterDiscovery> facts)
{
    auto core = std::make_shared<CharacterCore>(mod_namespace, package, false);
    core->set_facts(std::move(facts));
    return core;
}
std::shared_ptr<RuntimeExtension> character_discovery(const std::string& mod_namespace, std::shared_ptr<CharacterDiscovery> output)
{
    if (!output) throw std::runtime_error("Character discovery needs its output");
    return std::make_shared<CharacterCore>(mod_namespace, std::move(output));
}
std::shared_ptr<RuntimeExtension> character_folder(const std::vector<std::pair<std::wstring, std::shared_ptr<CharacterDiscovery>>>& mods)
{
    return std::make_shared<CharacterCore>(mods);
}
void character_core_version(RuntimeExtension& core, float version)
{
    if (auto* c = dynamic_cast<CharacterCore*>(&core)) c->core_version_ = std::isfinite(version) && version > 0 ? version : 0;
}
std::vector<std::string> character_core_libraries()
{
    std::vector<std::string> names;
    for (const auto& text : {rabbitfx_library_declarations(), rabbit_texture_declarations()}) {
        // Each unit opens with its `namespace = ...` line.
        const auto line = text.substr(0, text.find(L'\n'));
        const auto equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        std::string name;
        for (wchar_t c : line.substr(equals + 1)) if (c != L' ' && c != L'\t') name += char(towlower(c));
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}
namespace {
class DeclarationsOnly final : public RuntimeExtension {
    std::shared_ptr<RuntimeExtension> inner_;
public:
    explicit DeclarationsOnly(std::shared_ptr<RuntimeExtension> inner) : inner_(std::move(inner)) {}
    std::wstring declarations() override { return inner_->declarations(); }
    std::vector<std::wstring> declaration_units() override { return inner_->declaration_units(); }
    std::vector<std::wstring> native_shaders() override { return inner_->native_shaders(); }
    bool custom_shader_execution() const override { return true; }
    std::vector<std::wstring> install(DrawPlanAdapter& adapter) override { return inner_->install(adapter); }
    bool initialize_parameters(CommandListState& state, std::wstring& why) override { return inner_->initialize_parameters(state, why); }
    bool prepare(DrawPlanAdapter& adapter, CommandListState& state, std::wstring& why) override { return inner_->prepare(adapter, state, why); }
    bool validate_overrides(std::wstring& why) override { return inner_->validate_overrides(why); }
    bool before_draw(const RuntimeDraw& draw, CommandListState& state, std::wstring& why) override { return inner_->before_draw(draw, state, why); }
    void draw_hooks(const RuntimeDraw& draw, DrawHooks& hooks) override { inner_->draw_hooks(draw, hooks); }
    void after_draw() noexcept override { inner_->after_draw(); }
    void library_hook(LibraryHook hook) noexcept override { inner_->library_hook(hook); }
    void after_dispatch() noexcept override { inner_->after_dispatch(); }
    void execution_failed() noexcept override { inner_->execution_failed(); }
};
}
std::shared_ptr<RuntimeExtension> declarations_only(std::shared_ptr<RuntimeExtension> inner)
{
    return std::make_shared<DeclarationsOnly>(std::move(inner));
}
}
