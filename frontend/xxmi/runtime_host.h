#pragma once
#include "runtime.h"
#include "draw_plan.h"
#include "shader_state.h"

namespace efmi::frontend {
// Lists of a library's shader groups that the original runs around a draw's matched lists (D-171), in its
// order: `before` ahead of them (the vertex stage's lists come first), `between` after them and ahead of the
// application's draw (the pixel stage's), `after` once the draw is done (post lists).
struct DrawHooks { std::vector<CommandList*> before, between, after; };
// Extensions mediate named native Core boundaries, never reinterpret INI text.
// Execution methods run with the generation's original globals installed.
// Cleanup/failure notifications may also run after that scope has unwound.
class RuntimeExtension {
public:
    virtual ~RuntimeExtension() = default;
    virtual std::wstring declarations() { return {}; }
    virtual std::vector<std::wstring> declaration_units() { return {}; }
    virtual std::vector<std::wstring> native_shaders() { return {}; }
    virtual bool custom_shader_execution() const { return false; }
    virtual std::vector<std::wstring> install(DrawPlanAdapter&) = 0;
    // Called with zeroed original parameter storage, before either Constants
    // phase. GPU extensions require their host's explicit queue scope here.
    virtual bool initialize_parameters(CommandListState&,std::wstring&) { return true; }
    virtual bool prepare(DrawPlanAdapter&, CommandListState&, std::wstring&) { return true; }
    virtual bool validate_overrides(std::wstring&) { return true; }
    virtual bool before_draw(const RuntimeDraw&, CommandListState&, std::wstring&) { return true; }
    // After before_draw: the hook lists of this draw, adapted lists the extension registered at install.
    virtual void draw_hooks(const RuntimeDraw&, DrawHooks&) {}
    // After all matched pre/post lists, on either success or failure. Present
    // and key roots do not have the original HackerContext::AfterDraw lifetime.
    virtual void after_draw() noexcept {}
    // A shader hook event of the game's frame (D-113), outside any draw.
    virtual void library_hook(LibraryHook) noexcept {}
    // Native AfterDispatch does not restore an input layout edited by injected
    // commands; its lifetime continues until the corresponding outer draw end.
    virtual void after_dispatch() noexcept {}
    // A CPU command or expression can fail after an extension recorded GPU
    // work. Notify it even when the failure did not occur inside its callback.
    virtual void execution_failed() noexcept {}
};
void RuntimeRunInputList(CommandList*, bool post);
struct RuntimeShaderCall {
    DrawCallInfo call;
    SourceShaderState shaders;
    uint64_t frame=0;
    float seconds=0,frame_seconds=0;
    // Records the original operation in the host's active resource transaction.
    // Called only when not skipped. Must not submit or re-enter IniRuntime.
    DrawPlanAdapter::OriginalCall original;
};
void RuntimePublishTransitionParameters();
struct RuntimeShaderDeclaration {
    CustomShader* shader=nullptr;
    std::wstring name,root,namespace_path;
    std::array<std::wstring,6> sources; // v,h,d,g,p,c; empty is inherited.
    int recursive_include=0;
};
// Uses original section keys, enums, state parsers and command-list parser.
// The callback replaces only the original source-API shader compiler boundary.
void RuntimeParseCustomShaders(const std::function<void(const RuntimeShaderDeclaration&)>&);
}
