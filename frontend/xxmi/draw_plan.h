#pragma once
#include "CommandList.h"
#include "draw_state.h"
#include "texture_properties.h"
#include <tuple>
#include <cstdint>
#include <functional>
namespace efmi::core {struct PaletteSnapshot;}

namespace efmi::frontend {
struct SourceResourceMatch;

// Guard original pool indexing before its numeric casts or lookup. Call with
// the owning generation's globals installed; the original engine owns mapping.
void validate_pool_runtime(CustomResourcePool&);

enum class PlanOperation { BindVertex, BindIndex, BindPixelResource, DrawIndexed, External, Translucent };

// Resource pointers borrow declarations from a retained frontend generation.
// This CPU plan has no GPU readiness, resource lifetime or pipeline guarantee.
// The binding items are what the draws of the plan saw in the draw state (draw_state.h), written where
// a draw is recorded: before a draw item stand the bindings that differ from what the items before it
// left bound, and only those the layer honours (honour()). A list that binds and never draws leaves no item.
struct PlanItem {
    PlanOperation operation = PlanOperation::External;
    unsigned slot = 0;
    CustomResource* resource = nullptr; // An explicit null binding when absent.
    unsigned index_count = 0, first_index = 0, instance_count = 1, first_instance = 0;
    int vertex_offset = 0;
    // A DrawIndexed's (D-142): false for the original's Draw and DrawInstanced (index_count is then the
    // vertex count, first_index the first vertex); `automatic` for draw, drawindexed and
    // drawindexedinstanced = auto, whose count the bound buffers give (get_*_count_from_current_*).
    bool indexed = true, automatic = false;
    std::vector<InputLayoutElementOverride> layout;
    std::wstring external;
    unsigned phase_mask = 0; // External callback: bit 0 pre, bit 1 post.
    // A binding of the application's own binding of the matched draw; resource is null then. -1: none.
    // BindPixelResource: its texture of this DX11 pixel slot (a slot put back, or `ps-t1 = ref ps-t0`).
    // BindVertex, BindIndex: always the slot itself, the application's buffer back in its place.
    int application_slot = -1;
    // BindVertex of a buffer the owned GPU work of a replaced draw wrote (P2, D-087): a Vulkan
    // handle on the application's device, bound as it is; resource is null then. `keep` holds the
    // allocation while the item lives.
    using OwnedBuffer = OwnedPlanBuffer;
    std::shared_ptr<const OwnedBuffer> owned;
    std::shared_ptr<const core::PaletteSnapshot> palette; // Optional owned native Core draw data.
    // Translucent (D-113): a DrawIndexed of the current bindings that a library captures into a
    // translucency layer rather than drawing it in the pass, with this opacity and, when `resource`
    // holds one, this opacity mask (channel 0-3 for R, G, B, A; inverted: white is transparent).
    float alpha = 1;
    uint32_t mask_channel = 0;
    bool mask_invert = false;
};

struct DrawPlan {
    bool skip_original = false;
    std::vector<PlanItem> items;
    // Items the pre phase made: those before the original draw; the rest are post commands.
    size_t pre_items = SIZE_MAX;
};

// Mediate backend commands; retain the original XXMI expression, branch and
// named-call implementation. Adapt after upstream flow-control optimisation.
// An external entry stays an explicit unresolved operation, never a no-op.
class DrawPlanAdapter {
public:
    struct Runtime;
    DrawPlanAdapter();
    ~DrawPlanAdapter();
    DrawPlanAdapter(const DrawPlanAdapter&) = delete;
    DrawPlanAdapter& operator=(const DrawPlanAdapter&) = delete;

    // Register native boundaries before upstream optimisation; otherwise an
    // empty native declaration would be optimised out as a no-op.
    void external(ExplicitCommandListSection& section, const std::wstring& name);
    using NativeCommand=std::function<bool(CommandListState&,unsigned,std::wstring&)>;
    void native(ExplicitCommandListSection&,const std::wstring& name,NativeCommand);
    // Keep original list execution, then observe its evaluated CPU state.
    void after(ExplicitCommandListSection&,NativeCommand);
    // Invoke an adapted callback from a native command inside the same bounded
    // transaction. A refused nested call makes that transaction fail even when
    // its caller accidentally ignores the returned false.
    bool invoke(ExplicitCommandListSection&,CommandListState&,unsigned phases,std::wstring& reason);
    using PaletteSource=std::function<bool(CommandListState&,std::shared_ptr<const core::PaletteSnapshot>&,std::wstring&)>;
    void palette_source(PaletteSource);
    void adapt(CommandList& root);
    // Calls and microseconds per command class since the adapter was made (D-099).
    std::vector<std::tuple<std::string, uint64_t, uint64_t>> command_timing() const;
    // A phase of the host's own, counted with the command classes (EFMI_COMMAND_TIMING builds).
    void note_timing(const char* name, uint64_t microseconds);
    // Preserve original RunLinkedCommandList pointer identity/type for native
    // ShaderRegex duplicate suppression and unlinking, while bounding execution.
    std::shared_ptr<RunLinkedCommandList> linked(CommandList&,const std::wstring& ini_line);
    // Serial execution only. Failure preserves output; frontend variable side
    // effects are not rolled back. Callers use a fresh CommandListState.
    bool execute(CommandList& root, CommandListState& state, DrawPlan& output,
                 std::wstring& reason);
    using OriginalCall=std::function<bool(CommandListState&,std::wstring&)>;
    bool execute_pair(CommandList& pre,CommandList& post,CommandListState&,DrawPlan&,std::wstring& reason,OriginalCall between={});
    // The original runs each command on its own: one that cannot act does nothing and its list goes on. So
    // does every execution here: a command the planner refuses is skipped with a notice once (D-155 for
    // the Present, Constants and key lists; D-162 for a replaced draw's). What differs is a failure of the
    // host itself (a native command's contract, a budget): with this set, in a list that draws nothing,
    // it is one more skipped command; without it the lists still run to their end and the execution
    // then reports that reason, so the draw stays the game's.
    void tolerate_refusals(bool);
    // What the host knows of the pipeline of the draw the lists run for (its stages, the vertex slots it
    // reads): a slot the pipeline does not read is nothing to a draw (honour()). Install before adaptation.
    void pipeline(std::function<PipelineFacts()>);
    // During an execution: what the draw state holds at a pipeline slot (or at `this`), as the lists set
    // it so far, and which slot that is. False for a target that is no slot of the context. A host that
    // answers a question about a slot reads what is bound there with this (D-161).
    bool held(ResourceCopyTarget&, CommandListState&, SlotValue&, SlotId&) const;
    // What a declaration holds now, as the plan's references say.
    SlotValue held(CustomResource* declaration) const;
    // Matches of one resource share a phase. Keep every pre list ahead of every
    // post list; abort and budgets belong to the phase/transaction, not a match.
    // The post lists run where the original's second check would meet the sections
    // again: not after the pre lists bound an index buffer of their own.
    bool execute_matches(const std::vector<CommandList*>& pre,const std::vector<CommandList*>& post,CommandListState&,DrawPlan&,std::wstring&);
    // Each shader stage is an independent root. One transaction retains budgets
    // and GPU state across every pre root, the original call and every post root.
    // Deferred file plans are refused; the host must actually record their work.
    bool execute_intercepted(const std::vector<CommandList*>& pre,const std::vector<CommandList*>& post,
                             CommandListState&,OriginalCall,std::wstring&);
    // CPU declaration references are snapshots, not D3D resources. The frontend
    // generation must outlive these pointers. Pool resources are supported only
    // for nonexpiring direct-index pools until runtime identity is connected.
    CustomResource* referenced_resource(CustomResource* declaration) const;
    bool has_reference(const CustomResource* declaration) const;
    using ResourceQuery = std::function<bool(ResourceCopyTarget&,CommandListState&,float&,std::wstring&)>;
    // Install before adaptation. Every mediated query must explicitly succeed;
    // refusal never falls through to the reference D3D graphics scaffolding.
    void resource_queries(ResourceQuery);
    // Queries the host answers on the CPU where the owned GPU callbacks are absent (a replaced
    // draw's plan): a false answer refuses as before. Install before adaptation.
    void plan_resource_queries(ResourceQuery);
    // During a plan: the application's draw runs after all, whatever `handling = skip` the matched
    // lists execute, as a native command decides when the original Core draws the original instance
    // (its explicit draw with the application's bindings).
    void keep_original();
    // During an execution: the matched root whose commands run (a TextureOverride's list, or a
    // shader hook's), else null. A folder generation's natives tell the calling Mod by it (D-131).
    const CommandList* current_root() const;
    // During a native command: the INI line that runs it (its section names the calling Mod), else null.
    const std::wstring* current_line() const;
    // During a plan, from a native command: a translucent draw of the current bindings (D-113), as a
    // DrawIndexed of `count` indices from `first` would be planned, with its opacity and mask (null:
    // none). False refuses with `why`.
    bool translucent(CommandListState&, uint32_t count, uint32_t first, float alpha, CustomResource* mask,
                     uint32_t channel, bool invert, std::wstring& why);
    using ShaderQuery=std::function<bool(CommandListOperand&,CommandListState&,float&,std::wstring&)>;
    void shader_queries(ShaderQuery);
    // Shader operands (`vs`, `ps`) the host answers on the CPU where no owned shader facts apply (a
    // replaced draw's plan, D-089). A false answer refuses. Install before adaptation.
    void plan_shader_queries(ShaderQuery);
    // A declaration the SPIR-V leaves between two DX11 answers (a buffer of 32-bit words: structured or
    // raw): true when the running evaluation takes the second for this register. The adapter evaluates an
    // `if` with every combination and refuses only when they do not agree; any other command that asks
    // is refused.
    bool undecided_words(wchar_t stage,unsigned slot);
    // Operands the original answers with Win32 calls outside the window sample (effective_dpi,
    // cursor_hotspot_x/y): the host answers them from its own sample. Install before adaptation.
    using HostValue=std::function<bool(ParamOverrideType,float&)>;
    void host_values(HostValue);
    // rt_width/rt_height: the size of the 2D render target view bound to o0; bound is false when
    // there is none (the original then leaves its -1). Install before adaptation.
    using RenderTargetSize=std::function<bool(CommandListState&,bool& bound,float& width,float& height,std::wstring&)>;
    void render_target_size(RenderTargetSize);
    // What the host gives every fresh state of a draw's execution as the original's context would
    // answer it (the post root's, RunCommandListComplete; an intercepted call's roots): the draw's
    // render target size. Install before adaptation.
    using FreshState=std::function<void(CommandListState&)>;
    void fresh_state(FreshState);
    using ResourceCommand=std::function<bool(CommandListCommand&,CommandListState&,std::wstring&)>;
    // An opt-in execution backend owns resource state, GPU synchronization and
    // feedback. Without one, the existing immutable file-plan path is retained.
    // Install before adaptation; backend refusal never falls through to D3D.
    void resource_commands(ResourceCommand);
    using ParameterFlush=std::function<bool(CommandListState&,bool force,std::wstring&)>;
    // Root pre/post completion and every injected draw/dispatch publish dirty
    // parameters. Transitions can force publication outside a command root.
    void parameter_flush(ParameterFlush);
    using DrawPrepare=std::function<bool(CommandListState&,std::wstring&)>;
    // Original layout edits occur before parameter publication and argument
    // evaluation, including zero draws and compute dispatch. Hunting skips it.
    void draw_prepare(DrawPrepare);
    bool flush_parameters(CommandListState&,bool force,std::wstring&);
    using ShaderCommand=std::function<bool(RunCustomShaderCommand&,CommandListState&,std::wstring&)>;
    void shader_commands(ShaderCommand);
    // Both nested phases share the caller's abort/parameter state; no root flush.
    bool invoke_shader(CustomShader&,CommandListState&,std::wstring&);
    // What the matcher is asked about: the application's texture of the pixel slot the query names (the
    // slot it was bound at by the game, also where the lists moved it to another), or a resource of the Mod.
    struct PixelBinding {
        bool application=true;
        CustomResource* resource=nullptr; // Null with application=false means an explicit unbind.
    };
    // With `described`, the description the original's match_* sections compare (D-138). `properties`: what
    // `ps-tN->Format`, `->Width`, `->Height`, `->Mips` and `->Array` read of it (D-170, texture_properties.h);
    // the provider fills it for the application's texture, and for a resource of the generation when the
    // caller set `properties_wanted` (its file's header is read then, and `bound` is whether it loads).
    struct TextureIdentity {bool bound=false;uint32_t hash=0;bool described=false;D3D11_TEXTURE2D_DESC description{};
                            bool properties_wanted=false;TextureProperties properties;};
    using TextureQuery=std::function<bool(unsigned,const PixelBinding&,CommandListState&,TextureIdentity&,std::wstring&)>;
    // The provider proves the source-API resource hash (including the original
    // hash policy), not merely a Vulkan object identity or arbitrary byte hash.
    // Unknown data refuses. Hash and descriptor (match_*) matching use original XXMI
    // rules; a texture without a description meets no descriptor section. Install before adaptation. With a
    // source resource matcher as well, the matcher answers inside owned scopes and
    // this query outside them (the host's draws, D-077).
    void texture_queries(TextureQuery);
    bool has_texture_queries() const;
    // The index region hash the matched draw was found by (D-144): what the original's region hash of the
    // draw's own index buffer is. False outside a draw.
    void index_region_hash(std::function<bool(uint32_t&)>);
    // The original command an adapted list holds in place of `command` (itself when not adapted).
    static const CommandListCommand* original(const CommandListCommand& command);
    // The original operand an adapted expression holds in place of `node` (a host query around it), itself when
    // it is not wrapped; null where the adapter put no operand of the original's (an unanswered one).
    static const CommandListEvaluatable* original(const CommandListEvaluatable* node);
    using ResourceMatchQuery=std::function<bool(ResourceCopyTarget&,CommandListState&,SourceResourceMatch&,std::wstring&)>;
    // General source targets, including IB/VB, stage slots, resources and pools.
    // Mutually exclusive with the legacy pixel-only file-plan provider.
    void resource_matches(ResourceMatchQuery);
    // A generation that also renders replaced draws through the file plan installs a gate:
    // while it returns false, every owned GPU callback above (resource commands and queries,
    // render target size, matches, parameter publication, draw preparation, CustomShader; not
    // host values) is treated as absent, so
    // replaced draws keep the plan path. Declaration references made while it is open are
    // mirrored into the plan's references. Install before adaptation.
    void owned_scope(std::function<bool()>);
    // The host's scope for GPU work inside a replaced draw (P2, D-087). While it returns true the
    // draw's own bindings and draws keep the plan path; CustomShaders, copies into the Mod's
    // resources and everything inside a CustomShader run as owned work, which never reads or binds
    // the application's resources; a vertex binding of a resource that work wrote binds the owned
    // buffer (owned_bindings). Queries on the application's bindings take the plan's answers, those
    // on the Mod's resources the owned ones. Install before adaptation, beside owned_scope.
    void draw_scope(std::function<bool()>);
    using OwnedBinding=std::function<bool(ResourceCopyTarget&,CommandListState&,PlanItem::OwnedBuffer&,std::wstring&)>;
    void owned_bindings(OwnedBinding);
    // File declarations referenced anywhere in the adapted command graph,
    // including both branches. Unused INI declarations need not exist on disk.
    std::vector<CustomResource*> referenced_files() const;
    // How the lists bind a referenced file (bit 0 a vertex slot, bit 1 the index buffer, bit 2 a shader
    // slot); 0 when only copies into other resources reach it. The original's buffer takes the bind
    // flags of the assignments that use it.
    unsigned referenced_roles(CustomResource*) const;
private:
    bool execute_impl(const std::vector<CommandList*>& pre,const std::vector<CommandList*>* post,CommandListState&,DrawPlan&,std::wstring&,OriginalCall between={},
                      bool matched=false);
    void adapt_tree(CommandList& root, size_t depth);
    std::shared_ptr<Runtime> runtime_;
};

} // namespace efmi::frontend
