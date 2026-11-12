#include "efmittl_bridge.h"
#include "runtime_host.h"
#include "globals.h"
#include <cmath>

bool HostCompileShaderRegexList(const wchar_t* name, CommandList* pre, CommandList* post);

namespace efmi::frontend {
namespace {
const std::wstring library = L"\\efmittl\\";
CommandListVariable* variable(const wchar_t* name)
{
    const auto found = command_list_globals.find(L"$" + library + name);
    return found == command_list_globals.end() ? nullptr : &found->second;
}
// A count or first index the library's caller set: a whole number a draw can take.
bool index_value(float value, uint32_t& out)
{
    if (!std::isfinite(value) || value < 0 || value >= 4294967296.0f || std::floor(value) != value) return false;
    out = uint32_t(value); return true;
}
}
void EfmittlBridge::compile(Group& group, const wchar_t* name)
{
    // The lists become roots of the generation: optimised and adapted with every other one.
    group.declared = HostCompileShaderRegexList((L"ShaderRegex" + library + name).c_str(), &group.pre, &group.post);
}
void EfmittlBridge::constants(const CommandList& list, Group::Constants& out)
{
    out.whole = true; out.values.clear();
    for (const auto& held : list.commands) {
        const auto* assignment = dynamic_cast<const VariableAssignment*>(DrawPlanAdapter::original(*held));
        float value = 0;
        if (!assignment || !assignment->var || !const_cast<CommandListExpression&>(assignment->expression).static_evaluate(&value)) {
            out.whole = false; out.values.clear(); return;
        }
        out.values.emplace_back(assignment->var, value);
    }
}
std::unique_ptr<EfmittlBridge> EfmittlBridge::install(DrawPlanAdapter& a, std::vector<std::wstring>& boundaries)
{
    const std::wstring capture = L"commandlist" + library + L"capture";
    const auto list = explicitCommandListSections.find(capture);
    const auto mask = customResources.find(L"resource" + library + L"mask");
    if (list == explicitCommandListSections.end() || !explicitCommandListSections.count(L"commandlist" + library + L"draw") ||
        !explicitCommandListSections.count(L"commandlist" + library + L"composite") || mask == customResources.end())
        return nullptr;
    std::unique_ptr<EfmittlBridge> bridge(new EfmittlBridge);
    auto& b = *bridge;
    const std::pair<CommandListVariable**, const wchar_t*> names[] = {
        {&b.lit_seen_, L"lit_seen"}, {&b.copy_after_lit_, L"copy_after_lit"}, {&b.alpha_, L"alpha"},
        {&b.count_, L"count"}, {&b.first_, L"first"}, {&b.channel_, L"mask_channel"}, {&b.invert_, L"mask_invert"}};
    for (const auto& name : names)
        if (!(*name.first = variable(name.second))) return nullptr;
    // The groups the host tells of a draw, by the library's section names. The lit group is the one the
    // capture depends on; the others are the library's later additions and may be absent.
    compile(b.vertex_[size_t(VertexHook::Lit)], L"EFMITTLLitVS");
    if (!b.vertex_[size_t(VertexHook::Lit)].declared) return nullptr;
    compile(b.vertex_[size_t(VertexHook::LitSkin)], L"EFMITTLLitVSSkin");
    compile(b.vertex_[size_t(VertexHook::Outline)], L"EFMITTLOutlineVS");
    compile(b.vertex_[size_t(VertexHook::ShadowPlain)], L"EFMITTLShadowVSPlain");
    compile(b.vertex_[size_t(VertexHook::ShadowFading)], L"EFMITTLShadowVSFading");
    compile(b.clip_first_, L"EFMITTLClipFirst");
    for (unsigned slot = 0; slot < 24; ++slot) compile(b.diffuse_at_[slot], (L"EFMITTLDiffuseAt" + std::to_wstring(slot)).c_str());
    b.adapter_ = &a; b.mask_ = &mask->second;
    boundaries.push_back(capture);
    // The library's CommandListCapture, reached only in the forward lit pass outside its fallback,
    // after CommandListDraw resolved the opacity: the call's draw goes to the renderer's layers.
    a.native(list->second, capture, [&b](CommandListState& state, unsigned phases, std::wstring& why) {
        if (!(phases & 1)) return true;
        uint32_t count = 0, first = 0;
        if (!index_value(b.count_->fval, count) || !index_value(b.first_->fval, first)) { why = L"EFMITTL capture: invalid count or first index"; return false; }
        const float channel = std::isfinite(b.channel_->fval) ? std::fmin(std::fmax(b.channel_->fval, 0.0f), 3.0f) : 0.0f;
        return b.adapter_->translucent(state, count, first, b.alpha_->fval, b.mask_, uint32_t(channel), b.invert_->fval > 0.5f, why);
    });
    // The library's CommandListShadow (D-185), reached from CommandListDraw for a translucent call without o0 outside its
    // fallback. Under DX11, in a pass its shadow vertex groups recognised ($vs_kind 2 or 3) it draws the call with its
    // shadow pixel shader (shadow_a.hlsl or shadow_b.hlsl: a texel kept with a probability equal to the opacity there),
    // and elsewhere the call as it is (`drawindexed = $count, $first, 0`). The renderer draws the former in place.
    const std::wstring shadow = L"commandlist" + library + L"shadow";
    const auto shadow_list = explicitCommandListSections.find(shadow);
    b.vs_kind_ = variable(L"vs_kind");
    if (shadow_list != explicitCommandListSections.end() && b.vs_kind_ && b.vertex_[size_t(VertexHook::ShadowPlain)].declared &&
        b.vertex_[size_t(VertexHook::ShadowFading)].declared) {
        boundaries.push_back(shadow);
        a.native(shadow_list->second, shadow, [&b](CommandListState& state, unsigned phases, std::wstring& why) {
            if (!(phases & 1)) return true;
            uint32_t count = 0, first = 0;
            if (!index_value(b.count_->fval, count) || !index_value(b.first_->fval, first)) { why = L"EFMITTL shadow: invalid count or first index"; return false; }
            if (b.vs_kind_->fval != 2.0f && b.vs_kind_->fval != 3.0f) return b.adapter_->draw_indexed(state, count, first, why);
            const float channel = std::isfinite(b.channel_->fval) ? std::fmin(std::fmax(b.channel_->fval, 0.0f), 3.0f) : 0.0f;
            return b.adapter_->translucent(state, count, first, b.alpha_->fval, b.mask_, uint32_t(channel), b.invert_->fval > 0.5f, why, true);
        });
    }
    return bridge;
}
void EfmittlBridge::prepare()
{
    for (auto& group : vertex_)
        if (group.declared) { constants(group.pre, group.pre_constants); constants(group.post, group.post_constants); }
}
void EfmittlBridge::before_draw(const RuntimeDraw& draw) noexcept
{
    const auto kind = size_t(draw.vertex_hook);
    drawing_ = kind && kind < 6 && vertex_[kind].declared ? &vertex_[kind] : nullptr;
    if (drawing_ && drawing_->pre_constants.whole)
        for (const auto& value : drawing_->pre_constants.values) value.first->fval = value.second;
}
void EfmittlBridge::after_draw() noexcept
{
    if (drawing_ && drawing_->post_constants.whole)
        for (const auto& value : drawing_->post_constants.values) value.first->fval = value.second;
    drawing_ = nullptr;
}
void EfmittlBridge::draw_hooks(const RuntimeDraw& draw, DrawHooks& hooks)
{
    if (drawing_) {
        if (!drawing_->pre_constants.whole) hooks.before.push_back(&drawing_->pre);
        if (!drawing_->post_constants.whole) hooks.after.push_back(&drawing_->post);
    }
    if (!draw.pixel_hooks) return;
    const auto pixel = [&](Group& group) {
        if (!group.declared) return;
        if (!group.pre.commands.empty()) hooks.between.push_back(&group.pre);
        if (!group.post.commands.empty()) hooks.after.push_back(&group.post);
    };
    // A shader's groups run in the order of their section names: ClipFirst ahead of the DiffuseAt ones.
    if (draw.uv_narrow_first) pixel(clip_first_);
    if (draw.uv_colour_slot >= 0 && draw.uv_colour_slot < 24) pixel(diffuse_at_[draw.uv_colour_slot]);
}
void EfmittlBridge::library_hook(LibraryHook hook) noexcept
{
    if (hook == LibraryHook::CopyShader && lit_seen_->fval != 0.0f) copy_after_lit_->fval = 1.0f;
}
}
