#pragma once
#include "draw_plan.h"
#include "runtime.h"
#include "runtime_host.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace efmi::frontend {
// EFMITTL on Vulkan (D-113, D-171, docs/efmittl-transparency.md). The library's INI keeps its policy. Its
// shader groups (ShaderRegex) match DX11 listings, which a Vulkan draw has none of: the host says which
// group a draw's shaders stand for (RuntimeDraw::vertex_hook and the pixel facts), and this runs that
// group's own command lists, as the library wrote them, where the original runs them. Its capture is a
// native boundary whose translucent draws the renderer records into its layers. Installed only into a
// generation that loaded the library with the interface named here.
class EfmittlBridge {
public:
    // Null when the generation has no EFMITTL library or not the lists, variables, resource and lit group
    // this bridge knows; the library then keeps its own fallback (opaque parts).
    static std::unique_ptr<EfmittlBridge> install(DrawPlanAdapter&, std::vector<std::wstring>& boundaries);
    // Once the generation's lists are optimised and adapted: which groups' lists only assign constants.
    void prepare();
    // The vertex group of the draw: its pre list when that only assigns constants (`$vs_kind = 1` and the
    // per-draw marks the library clears), and after the draw its post list likewise (`$vs_kind = 0`).
    void before_draw(const RuntimeDraw&) noexcept;
    void after_draw() noexcept;
    // The lists that are more than constants: the vertex group's, and the pixel groups' (ClipFirst, then
    // the DiffuseAt of the slot, in the order of their section names as the original runs them).
    void draw_hooks(const RuntimeDraw&, DrawHooks&);
    // The copy shader group's pre list: `if $lit_seen` then `$copy_after_lit = 1`; its composite is the
    // renderer's (D-113).
    void library_hook(LibraryHook) noexcept;
private:
    EfmittlBridge() = default;
    // One group: the command lists of its main section, parsed by the original as it parses a group's.
    struct Group {
        bool declared = false;
        CommandList pre, post;
        // Set by prepare(): the list is nothing but assignments of constants to variables, applied directly.
        struct Constants { bool whole = false; std::vector<std::pair<CommandListVariable*, float>> values; } pre_constants, post_constants;
    };
    static void compile(Group&, const wchar_t* name);
    static void constants(const CommandList&, Group::Constants&);
    DrawPlanAdapter* adapter_ = nullptr;
    CustomResource* mask_ = nullptr;
    CommandListVariable *lit_seen_ = nullptr, *copy_after_lit_ = nullptr, *alpha_ = nullptr,
                        *count_ = nullptr, *first_ = nullptr, *channel_ = nullptr, *invert_ = nullptr;
    Group vertex_[4];       // by VertexHook; the first is not used
    Group clip_first_, diffuse_at_[24];
    Group* drawing_ = nullptr; // the vertex group of the draw between before_draw and after_draw
};
}
