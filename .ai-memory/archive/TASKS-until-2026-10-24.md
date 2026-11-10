# EFMI Vulkan - Tasks, checkpoints until 2026-10-24 (archived 2026-11-10)

> Last updated: 2026-10-24 (review fixes and offline performance follow-up). Earlier checkpoints below are historical.

Milestones and gates refer to `docs/plan.md`.

## Current implementation and follow-up

User request: fix all three findings, continue optimizing and finish offline tests before a game test is needed. Review: docs/performance-review-1.7.4.md. Release 1.7.6 is in build/performance-next; the separate worktree preserves the main tree's older uncommitted 1.3.7 work. Deployed to dist with the game closed; no game load.

- Implemented BUG-040 / P1: validate positive-match fingerprints every draw.
- Implemented BUG-041 / P2: erase retired pipeline identities from the fast lookup.
- Implemented BUG-042 / P2: release scratch owners on all exits, retaining capacity.
- Verified: native production-path checks 158/158 and all 35 baseline regression steps. Log: build/performance-run-all-1.7.5.out.
- Implemented in 1.7.6: batch first-use texture/variant preparation, completion notification and reused SSE2 bone staging. Complete 35-step suite passed, plus 266 standalone GPU resource/cache checks.
- Completed: three interleaved A/B pairs in each of vk03/vk04 against corrected 1.7.5, plus vk03 against deployed 1.7.4; same final targets/coverage/residency. Deployed tested 1.7.6 with backup; actual-config sandbox smoke passed. Historical bone replay: 283/283 record results byte-exact.
- Stop at game acceptance: launcher remains DX11/DIRECT. A launcher switch needs the user's request on the spot; only the user starts the game. Validate real frame intervals and mapped-read cost before choosing the next performance work. The 120 fps target remains unverified.
- Game acceptance remains open: compare the same user-started game scene against 120 fps, test first appearance, switching, dashes, reload and long-session resource ownership. Offline results cannot establish these.

## Earlier Codex checkpoint (2026-10-15, historical)

Candidate1.3.6 verifies exact bytecode buffer contracts and typed UAV format
conversion in owned INI execution. Full rebuilt frozen-source run in
build/next/run_all-1.3.6.out passes all code/GPU steps: runtime475/resources10784,
compiler172/58, clean core/sync. Only the two installed corpus checks fail under
INPUT-001. Native duplicate warnings are recoverable; hosted checks currently
reject any warning. Normalized project-only copy:55/55 programs from70 source
declarations, hashes113/9/12 match, all88 installed sources unchanged.
Dist1.0.2 unchanged; no game/deployment/launcher/system actions.

Next: transactional native ShaderRegex executable publication with exact source
association, followed by source warning-policy audit and remaining original
runtime/application integration. Keep working offline; no game input needed.
PNG paused, LOD0/LOD1 only. See D-066 and docs/shader-matching-audit.md.

Earlier verified checkpoint:

Float32 bytecode lowering now preserves signed denormals in arithmetic while
leaving data moves intact. Software handling runs on NVIDIA, hardware handling
on Intel. Both explicitly require NaN/infinity/signed-zero preservation and RTE;
Fract retains the native result below1. Native WARP comparisons cover eight
boundary/vector cases plus compute, graphics and descriptor execution.

Full rebuilt frozen-source run:build/next/run_all-1.3.5.out. All code/GPU steps
pass (runtime475, resources10650, compiler172/58; core/sync clean). Two installed
corpus steps fail because fangyi ul.ini contains identical duplicate resource
sections (INPUT-001); do not label the full run green. A source-only project
copy removes those two blocks:70 declarations,55 distinct HLSL programs all
compile/validate. Evidence:build/corpus-1.3.5/manifest.json and hlsl/report.json.
Source hashes110/9/12 match; all88 copied installed inputs remain unchanged.
Text358 clean. No game, deployment, launcher or system action; dist1.0.2 unchanged.

Continue offline with exact native buffer-view contracts and regex executable
publication. Do not equate scalar SPIR-V storage stride with the original
structured stride. Source identity, command publication and executable lifetime
must stay associated atomically. No game input needed; keep going. PNG paused,
LOD0/LOD1 only. See D-065, LIMIT-033 and docs/bytecode-compiler.md.

Earlier verified checkpoint:

Exact original/reassembled DXBC now translates to SPIR-V through unchanged
vkd3d2.1. Native scanning supplies original register/signature contracts even
without RDEF. Owned compute and graphics tests compare changed bytecode, shared
memory/atomics, typed/raw/structured buffers, image reads, counters and nonzero
vertex/instance IDs with native results. Runtime regex patch publication remains
separate; current1.3.3 links still refuse bytecode-changing plans.

Full rebuilt frozen-source suite:build/next/run_all-1.3.4.out exit0/all steps;
runtime475, resources9908, compiler172/58, corpus37/37, clean core/sync through
shutdown. Source hashes108/9/12 match; installed inputs unchanged; text356 clean.
Strict DXBC translation tests explicitly use Intel float controls. Existing INI
resource tests remain on NVIDIA. No game/deployment/system action; dist1.0.2 and
separate launcher DX11 state stay unchanged.

Continue1.3.5 offline with source-correct float32 lowering for devices without
explicit denormal control, then regex executable publication and view adaptation.
The local NVIDIA default mode matches basic arithmetic but differs from native
WARP for six subnormal sign/absolute-value rows. Intel strict mode matches all.
Do not silently drop the requested mode. See LIMIT-033 and docs/bytecode-compiler
.md; build/float-controls contains the isolated probes. Game input is not needed;
keep going. PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

Original ShaderRegex text matching, capture replacement, temporary registers
and declaration insertion now run through unchanged native disassembly, regex
and signature-aware assembly. Immutable original DXBC is validated and retained
per generation. Isolated native preparation preserves explicit command roots,
local storage and filter state; incomplete executable patches cannot publish.
Pattern-only rules can drive owned Vulkan execution with exact native pixels.

Full rebuilt frozen-source suite:build/next/run_all-1.3.3.out exit0/all steps;
runtime475, resources9360, compiler172/58, corpus37/37, clean core/sync through
shutdown. Source hashes104/9/12 match; installed inputs unchanged; text351 clean.
Native WARP executes round-tripped and original-regex-patched bytecode with
expected outputs1/7/9. This does not yet execute changed bytecode on Vulkan.
No game/deployment/system action; dist1.0.2 and separate launcher DX11 state stay
unchanged.

Continue1.3.4 offline with bounded DXBC-to-SPIR-V translation, exact source
association and native/GPU comparisons before publishing patched executables.
The native parser accepts only .Pattern, not numbered/multiple pattern sections;
the earlier multi-pattern partial-failure inference is withdrawn. Valid sorted
groups remain tested. Native Rendering getters now initialize the three assembly
flags before parsing/hashing. No game input needed; continue. PNG paused,
LOD0/LOD1 only. See docs/shader-matching-audit.md.

Earlier verified checkpoint:

Original ShaderRegex parsing and patternless command linkage now run through
unchanged native source. Bounded links retain shared identity and native type,
so duplicate suppression, unlink/filter restoration and generation isolation
remain intact. Proven native shader models are required; relevant assembly
patterns refuse before execution until their source/translation path is ready.

The unchanged installed Core VS regex drives owned indexed replacement, native
key disable/re-enable and non-indexed bypass. Pixels agree exactly with native
DX11. Full rebuilt frozen-source suite:build/next/run_all-1.3.2.out exit0/all steps;
runtime407, resources9301, compiler172/58, corpus37/37, clean core/sync through
shutdown. Source hashes102/9/12 match; installed inputs unchanged; text349 clean.
No game/deployment/system action; dist1.0.2 and separate launcher DX11 state stay
unchanged.

Continue1.3.3 offline with original assembly disassembly, regex matching and
substitution/reassembly. Preserve original sorted/partial-patch behavior and
keep executable translation distinct from matching. The original Core link is
not application shader provenance. Game input is not needed yet; keep going.
PNG paused, LOD0/LOD1 only. See docs/shader-matching-audit.md.

Earlier verified checkpoint:

General CheckTextureOverride and resource filters now use original descriptor,
draw-info and buffer-region rules for source targets and ranges. Hash matches
precede fuzzy matches, including exact matches. Weak allocation identities
preserve references without assigning a copied allocation its source's hash.
Unknown facts refuse when required; no implicit readback is introduced. Dynamic
this, pool wrap/read semantics and finite/non-finite slot handling are tested.

Full rebuilt frozen-source regression:build/next/run_all-1.3.1.out exit0/all steps;
runtime311, resources9242, compiler172/58, corpus37/37, core/sync clean. Source
hashes99/9/12 match; installed inputs unchanged; text345 clean. Native WARP
reference comparisons cover80 resource/identity/draw combinations. No game,
deployment or system action; dist1.0.2 and separate launcher DX11 state unchanged.

Continue1.3.2 offline with original ShaderRegex parsing and patternless command
linkage, including Core's VS entry to CheckTextureOverride=ib. Preserve proven
shader_model identity, explicit-filter precedence, repeated-bind idempotence and
generation ownership. Assembly-changing patterns, legacy substitutions and
application placement remain separate. See both matching audit documents.
No game input needed; continue. PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

Original ShaderOverride parsing now drives independent VS/HS/DS/GS/PS and CS
roots around an owned original call. Tracked source shader identity survives
nested CustomShader execution; original declaration queries use the unchanged
DXBC reader. Atomic fact updates, native hash0 rules, duplicate/abort/skip rules,
fresh root state and distinct AfterDraw/AfterDispatch layout lifetimes are tested.
Present models the owned frame/counter boundary between pre and post roots.

Full rebuilt frozen-source suite:build/next/run_all-1.3.0.out exit0/all steps;
runtime200, resources9139, compiler172/58, corpus37/37, core/sync clean through
shutdown. Source hashes96/9/12 match; installed inputs unchanged. Text341 clean
including the new resource matching audit. No game, deployment or system action;
dist1.0.2 and the separate launcher DX11 state remain unchanged.

Continue1.3.1 offline with general resource matching/checktextureoverride for
IB/VB, all source slots, ranges and pools. Reuse original descriptor/draw-info
predicates and preserve hash-then-fuzzy order, region prefilter/fallback and this
rebinding. Audit:docs/resource-matching-audit.md. ShaderRegex, legacy substitution,
application import and placement remain separate. No game input needed; keep
working. PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.2.9 fixes matched pre/post order, phase state and shared budgets (BUG-024).
Full rebuilt suite passes:build/next/run_all-1.2.9.out, runtime118/resources9020,
compiler172/58, corpus37/37, clean core/sync; source hashes93/9/12 match.
Installed inputs/dist1.0.2 unchanged; no game/deployment/system action.
Continue1.3.0 with original ShaderOverride stage scheduling and tracked shader
queries. Preserve source root, skip, duplicate and identity semantics; owned
pre/original/post execution precedes application integration. Source audit in
docs/shader-matching-audit.md. No game input needed; continue without stopping.
PNG paused, LOD0/LOD1 only. Separate launcher DX11 setting remains unchanged.

Earlier verified checkpoint:

1.2.8 implements original vertex input bit reinterpretation and BGRX alpha1,
including aliased/scalar loads, source defaults and cache identity. Native exact
pixels pass66 original INI cases. Full rebuilt regression passes in
build/next/run_all-1.2.8.out:resources8962, compiler172/58, corpus37/37, core/sync
clean; hashes92/9/12 match. Installed inputs/dist1.0.2 unchanged; no game action.
Next1.2.9: fix reproduced BUG-024 multi-match phase order and shared budgets/state,
then source matching/directory/application integration. Optional packed16 BGR has
no reliable native pixel oracle here; keep the explicit boundary (audit recorded).
No game input needed; continue. PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:


1.2.7 executes original instance divisors with actual enabled KHR/core features,
zero/max/nonzero-base properties and pipeline/buffer validation. Native DX11
pixels agree for steps0/2/3/max and all four direct/indexed/indirect draw forms.
Full rebuilt suite passes:build/next/run_all-1.2.7.out, resources6653, compiler172/
58, corpus37/37, clean core/sync, hashes89/9/12 match; installed inputs unchanged.
Next1.2.8: source vertex bit reinterpretation/BGRX alpha, then remaining fetch
cases and source matching/directory/application integration. No game input
needed; keep going. Dist1.0.2 unchanged; PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:


1.2.6 adds owned input layouts with original creation-register mapping, source
edit helper/timing, nested persistence, intercepted-draw restore and instance
steps0/1. Native WARP creation/pixel references pass. Full rebuilt suite:
build/next/run_all-1.2.6.out, resources6093, compiler172/58, corpus37/37, core/sync
clean; source hashes88/9/12 match. Dist1.0.2 and installed inputs unchanged.
Continue1.2.7 with remaining vertex fetch/type/divisor cases, then source shader
matching, directory scheduling and application placement. Existing boundaries:
null/OOB fetch, native format reinterpretation, packed16 BGR, divisors>1 and
indirect step0. Do not confuse owned execution with application layout import.
No game input needed; keep going. PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.2.5 executes owned original VS/GS/PS geometry and unchanged Core TextPrinter
HLSL/font. Native DX11 chunk fields match exactly; float positions/pixels differ
by at most5.96e-08. Full rebuilt suite passes in build/next/run_all-1.2.5.out:
resources5644, compiler172/58, corpus37/37, core/sync clean; source hashes84/9/12
match and installed sources unchanged. This is controlled owned shader execution,
not complete Core/application rendering. Next1.2.6: original input layouts/edits
and creation-register/lifetime semantics (docs/input-layout-audit.md), then
remaining state/resources and matching/directory/application integration.
No game input needed; continue. Dist1.0.2, PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.2.4 audits all original enabled Mods/Core HLSL declarations.34 INI inputs,
30 shader sections,37 distinct programs all compile/validate; four use the
conditional scalar/standard-layout validation profile. Sources unchanged; this
is compiler evidence, not directory rendering. Full rebuilt suite passes in
build/next/run_all-1.2.4.out: resources5342, compiler172/58, source hashes82/9/12
match and core/sync clean. Next1.2.5: owned geometry execution and the actual Core
text shader, then remaining vertex layouts, state/resources and matching/directory
integration. Continue without game input. Dist1.0.2, PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:


1.2.3 executes original owned VS/PS INI graphics, source state/sampler overrides,
direct/indirect/automatic/from-caller draws and nested restore. Native DX11 pixel
references and failed-root discard pass. Full rebuilt suite:
build/next/run_all-1.2.3.out, resources5342, compiler172/58 valid, clean core/sync,
source hashes82/9 match. Next1.2.4 audits every enabled Mods/Core HLSL declaration
through original parser metadata and compiler; then input layouts, remaining
shader/state cases and matching/directory integration. LIMIT-032 records execution
boundaries. No game input needed; keep going. Dist1.0.2, PNG paused, LOD0/LOD1.

Earlier verified checkpoint:


1.2.2 completes owned graphics pipelines/draws, immutable descriptors and fence
retention, with native DX11 pixels and clean full regression. Evidence:
build/next/run_all-1.2.2.out; resources 4749, compiler 172/58 valid, source hashes
80/9 match. Original INI graphics calls still need integration. Next 1.2.3:
source stages/state/layouts/draws and sampler rules, then remaining capabilities,
shader matching and enabled-directory integration. No game input needed; continue.
Dist 1.0.2 unchanged; PNG paused, LOD0/LOD1 only. Keep the separate launcher DX11
state recorded below; no launcher or game action was part of this increment.

Earlier verified checkpoint:


1.2.1 fixes native pixel-w and draw-ID conventions (BUG-023) and exposes original
fixed-function description merging. Native DX11/Vulkan output agrees for direct/
indexed offsets, negative bases, multiple instances and culling. Full rebuilt
suite passes in build/next/run_all-1.2.1.out: resources4559, compiler172/58 valid
modules, core/sync clean, all76 resource and9 compiler hashes match. State bridge
is CPU-only; INI graphics drawing still needs integration. Next1.2.2: common
retained graphics descriptors/pipelines, original state/stages/draws, then matching
and enabled-directory integration. Gate actual shaderDrawParameters; adapt source
comparison samplers according to executable sample operations. No game input
needed. Dist1.0.2 unchanged, PNG paused, LOD0/LOD1 only; continue immediately.

Launcher configuration restored to DX11 at the user's request on 2026-10-14 20:11. The two authorized values are -force-d3d11 and DIRECT; backup and exact-change proof are in build/backups/launcher-dx11-20261014-201131/. No layer/package change or new game result. Offline candidate work stays separate from the user-started DX11 session; do not assume the launcher still selects Vulkan.

Earlier verified checkpoint:


1.2.0 adds bounded original-signature validation and Vulkan graphics IO linking.
Native DX11 and linked Vulkan pixel output agree byte for byte; raw DXC locations
reproduce incorrect output. Complete rebuilt regression passes in
build/next/run_all-1.2.0.out: resources4065, compiler172/58 valid modules, clean
core/sync; all74 resource and9 compiler hashes match. Isolated pixel/link evidence
is not original INI graphics execution. Continue immediately with source graphics
state/draws, coordinate/ID comparisons, then matching and enabled-directory
integration. No game input needed. Next1.2.1, dist1.0.2 unchanged, PNG paused,
LOD0/LOD1 only. No game/deployment action.

Earlier verified checkpoint:


1.1.9 fixes BUG-022 with native source binding hazards, atomic slot ranges,
attachment compatibility, complete pixel UAV replacement and OM restore effects.
Full rebuilt regression: build/next/run_all-1.1.9.out, all steps pass; resources
4006, compiler 117/33 valid modules, core/sync clean. Source hashes match.
Continue immediately with graphics semantic linking/state/draws, then shader
matching and enabled-directory integration. No game input needed. Next 1.2.0;
dist 1.0.2 unchanged, PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.1.8 adds shared memory, exact constant sample pairs, GPU indirect compute and
owned source sampler defaults/quotas. Complete rebuilt regression passes:
build/next/run_all-1.1.8.out, resources3720 and all other steps, core/sync clean.
Immediate next1.1.9: BUG-022 source binding conflicts, batched slot publication
and OM restore effects, with native WARP references. Correct the older logical
coexistence test; isolate invalid native calls. Then graphics/matching/directory
integration. Continue immediately; no game input needed. Dist1.0.2, PNG paused,
LOD0/LOD1 only. Details:CURRENT_STATE.md and docs/custom-shaders.md.

Earlier verified checkpoint:

1.1.7 executes original CustomShader direct compute with owned Vulkan pipelines,
immutable descriptors and fence-retained owners. Original counters/nested phases,
null stages and state restoration pass. Production validation uses enabled layout
features. BUG-021 fixes resource queries in all draw/dispatch argument guards.
Full rebuilt suite passes: build/next/run_all-1.1.7.out, resources3633,
compiler117/33 valid modules, all other steps and clean core/sync validation.
Dist remains1.0.2; no game/deployment action.

Continue offline immediately: workgroup memory and precise sampler/image usage,
source sampler/counter state, indirect dispatch and remaining compute cases;
then graphics state/draws, shader matching and directory integration. No game
input is needed. LIMIT-031 and docs/custom-shaders.md describe the boundaries.
Next1.1.8; PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.1.6 supplies original HLSL preprocessing/model-5 validation, DXC compilation
and source-reflected Vulkan descriptor mapping. All six stages and source
edge cases pass 116 compiler checks; 33 modules validate. Actual Vulkan compute
output matches native DX11 WARP and input bytes. Full rebuilt regression passes
in build/next/run_all-1.1.6.out, resources 3544 with clean validation, every other
step passing. This is the compiler foundation, not general CustomShader
execution. Source flag/device differences are explicit in LIMIT-030 and
docs/hlsl-compiler.md. Dist remains 1.0.2; no game or deployment action.

Next: owned shader descriptor/pipeline execution and original CustomShader
calls, then graphics state, shader matching and complete directory integration.
Continue without stopping at this checkpoint. No game input needed. Normalize
native diagnostic trailing NULs in the next compiler review. Next version1.1.7.
PNG paused, LOD0/LOD1 only.

Earlier verified checkpoint:

1.1.5 implements owned GPU IniParams publication: zero initialization, original
dirty comparisons, draw/dispatch/root boundaries and forced key transitions.
Constants post and root-phase abort isolation are corrected (BUG-020). Tests
read real GPU values and execute a native HLSL Texture1D consumer. Scope failure,
discard, reload sizing and fence-retained lifetime pass. Complete rebuilt
regression: build/next/run_all-1.1.5.out, resources 3509 with clean core/sync
validation and all remaining steps passing. ABI is unchanged; dist is 1.0.2.

Next: CustomShader source loading, compilation, descriptors and draw/dispatch
execution; then shared shader matching and complete enabled-directory graphics.
Preserve source include rules, state save/restore, nested pre/post behavior and
frame counters (audit: docs/gpu-resource-commands.md). SPIRV-Reflect is installed
under F:/vulkan for the next implementation, not linked yet. Continue offline
without stopping between verified increments, as the user reiterated. No game
input is needed now. PNG stays paused; LOD0/LOD1 only. Next version is 1.1.6.

Earlier verified checkpoint:

1.1.4 adds integer RTV float conversion and single-sample R16/R32 color/depth
bit copies to the owned original-runtime adapter. Native WARP pixel/copy
references, actual Vulkan bytes, arrays/mips/partial regions, scratch reuse,
budget failure and fence-retained lifetime pass 3285 checks with zero core/sync
validation messages. BUG-019's depth payload range check is fixed. LIMIT-029
records the deliberate WARP overflow policy and differing hardware observations.
Full frozen-source rebuild/regression passes in build/next/run_all-1.1.4.out;
all 52 resource source hashes match. Dist stays 1.0.2; no deployment or game action.

Next: GPU IniParams, then CustomShader, shared shader matching and complete
directory graphics. Parameters must preserve original zero initialization,
float dirty comparison and draw/list-boundary publication; see
docs/gpu-resource-commands.md. Unmapped image formats, multisampled color/depth
copies, other misc/sample options and buffer hash/properties remain resource
follow-ups. Opaque color-to-D32 copies need actual enabled unrestricted depth;
application resource import and inline command placement remain separate work.
Slots/viewport still hold logical state, not emitted shader/attachment bindings.
PNG remains paused, LOD0/LOD1 only, no game input needed. The active goal remains
incomplete. Next version is 1.1.5.

Earlier verified checkpoint:

1.1.2 supplies owned mutable 1D/2D/3D/array/cube images, views, pitched transfers,
image copies, color/depth/stencil clears and color MSAA resolve. Capability/feature
checks, shared budgets and fence-retained owners are explicit. Discard and failed
submit do not publish pending image layouts. These are direct GPU primitives;
original INI image semantics are not connected yet. The frozen-source full
rebuild/regression passes in build/next/run_all-1.1.2.out, with 563 GPU resource
checks and zero core/sync validation messages. Dist stays 1.0.2.

Next: original INI texture declarations, view selection, image copy/cache and
clear/property semantics; then GPU IniParams, CustomShader, shared shader matching
and complete directory graphics. Follow the pinned view helpers and source audit
in docs/gpu-resource-commands.md, especially typeless/view-format clear behavior.
Remaining buffer properties/conversions remain open. PNG is paused, LOD0/LOD1
only, no game input needed. The active goal remains incomplete; next is 1.1.3.

Material matching boundary reviewed on 2026-10-14: live INI selects DDS/null bindings; verified exact-fragment profiles or guarded sibling derivation determine Vulkan placement. Runtime content hashing and general source-image hash overrides remain outside the deployed character adapter. No new shader-matching implementation, deployment or game test was performed for this explanation.

Earlier verified checkpoint:

1.1.1 executes owned FIFO/spatial/expiring buffer pools, preserves original
slot reuse and expiry behavior, and supplies frame_time to original Present/draw
contexts. BUG-017 is reproduced and fixed: CPU-side command failure now also
poisons a pending GPU scope. Targeted suite: 485 passing checks, clean validation.
Full rebuild/regression passes in build/next/run_all-1.1.1.out. Dist stays 1.0.2;
the active complete-directory goal remains incomplete and needs no game input.

Next: general mutable image resources/commands, GPU IniParams, CustomShader,
shared shader matching, then complete directory graphics. Keep remaining buffer
hash/properties/conversions and source-API description/view cases tracked; the
existing immutable 2D DDS uploader is not a general mutable image executor.
Application import/inline placement remain separate architecture work. PNG is
paused. Next version is 1.1.2.

Earlier verified checkpoint:

1.1.0 implements bare/wildcard whole-pool resource assignments and the remaining
named buffer copy modifiers. Sources are retained across pool writes, with
original per-element counters, metadata reads, proxies and caches. Buffer
resolve_msaa creates/reuses the destination without copying bytes. Targeted GPU
suite: 402 passing checks, clean validation. Full rebuild/regression passes in
build/next/run_all-1.1.0.out. Deployment remains 1.0.2; the active goal is not
complete and this checkpoint needs no game input.

Next: keyed/expiring pool ownership and remaining resource properties/conversions,
then mutable images, GPU IniParams, CustomShader, shader matching and directory
graphics. Original slot reassignment does not automatically clear the old
resource; expiry/reset and reference lifetime must be tested independently.
Spatial pools also need original frame duration supplied by the host runtime.
PNG stays paused. Next version is 1.1.1.

Earlier verified checkpoint:

1.0.9 adds owned buffer property queries and original target-specific metadata,
null view caching and source_stride retention. Targeted GPU suite: 303 passing
checks, clean validation. Full rebuild/regression passes in
build/next/run_all-1.0.9.out. Further resource/image/shader/directory work remains
offline; the active goal is not complete and no game input is needed now.
Next: whole-pool resource assignments and remaining buffer copy flags, then
image operations and GPU parameters/shaders. Preserve one resolved source for a
whole-pool operation. Next version is 1.1.0; deployment stays 1.0.2.

Earlier verified checkpoint:

1.0.8 fixes copy-pool alias ownership (BUG-015) and adds original buffer region
copies, CB windows/rounding, vertex-to-structured conversion, raw views and
no_view_cache. Targeted GPU checks: 243 passing, no validation messages. Complete
rebuilt regression passes in build/next/run_all-1.0.8.out. Continue offline with remaining properties/options,
images, shaders and directory graphics; the active goal remains incomplete.
Property fixtures must distinguish slot views from custom/pool metadata and
check original null/cache/source-stride behavior before the next adapter change.

Earlier verified checkpoint:

1.0.7 adds original buffer slot-range bind/fetch/broadcast/copy execution. The
155-check headless GPU fixture passes with clean validation; the complete rebuilt
suite passes in build/next/run_all-1.0.7.out. Source-specific null, view,
constant-buffer region, counter and negative ring-index behavior are covered.
Next offline work remains buffer regions/options/properties, images, then the
shader and directory stages below. No game input or deployment is needed now.

Earlier verified checkpoint:

1.0.6 now connects original INI commands to an opt-in owned-buffer Vulkan backend.
The 120-check targeted GPU test passes with clean core/sync validation, including
real store feedback, aliases/copies, source view semantics, file padding, pool
resets, generation lifetime and failure cleanup. A full suite passes in
build/next/run_all-1.0.6-final.out. The frozen-source rebuild/regression also passes
in build/next/run_all-1.0.6-verified.out; this is the final candidate checkpoint.
The first full run failed only the newly added SRV-view clear fixture; its fix
now uses explicit view metadata and the original buffer-view element helper.
Continue the user-authorized order offline: finish general resources, then
CustomShader, shared shader matching and complete directory graphics. The goal
is not complete and no game action is needed for the next increments. Details:
docs/gpu-resource-commands.md. Dist stays 1.0.2; PNG integration stays paused.

Earlier verified checkpoint:

Current work is T-030's source-defined language/runtime implementation. Source
and candidate are now 1.0.5 with a passing full rebuilt regression;
dist remains 1.0.2. Pool copy_desc/reset and mixed-element references now pass
23 adapter checks. The latest passing full report is build/next/run_all-1.0.5.out.
726 original-parser checks and 65 expression checks pass. Production CPU directory
generations now resolve Includes, namespaces and callback-only dependencies;
68 runtime checks cover existing profiles and the added language behavior.
General graphics operations and shader sections still need Vulkan adapters.
PNG integration is paused under the latest user steering. The user has deferred
new game collection; continue offline and do not require capture per character.
See CURRENT_STATE.md and docs/ini-language-coverage.md. The following 1.0.2
deployment evidence remains valid historically.

1.0.2 is deployed and links the original main-INI runtime into the character backend:
conditions, thirteen original listeners, alternate stocking DDS, atomic project
settings and staged F10. Native Core bone/identity boundaries are explicit and
validated, not replaced by a new INI interpreter. CPU checks 45, Vulkan runtime
runner checks 28 and the full regression pass. Candidate/deployed actual-config
smokes upload ten images cleanly through both entries on two enabled queues.
Backup build/backups/before-1.0.2-20261013-223323/; deployment manifest
build/deployment-1.0.2.json. User-started PID 31108 confirms real-character key
changes, two completed F10 exchanges and ten Ready images; thirteen project-saved
choices match live variables. Restart persistence and all-key visual coverage
remain open. Evidence: captures/live-ini-1.0.2-20261013-225835-31108/.
The user now requests extension to arbitrary characters; audit generic import,
profiles and runtime boundaries, then validate a second character. Details:
docs/live-ini.md and CURRENT_STATE.md.
The original panel's HLSL compiles/validates; its drawing adapter remains T-016.

Earlier 1.0.1 checkpoint:

T-001 to T-008, T-012, T-014, T-015, T-017, T-020 and T-023 are complete within their recorded scope. T-011's standard descriptor/resource paths are implemented. T-018 is complete for the display scene. The stutter fix remains 0.7.3's streaming loads; T-021 is withdrawn. Source/build/next/dist remain 1.0.1. User-started PID 19452 passes sampled LOD0/LOD1 checks and a 58-frame requested dash interval without new refusal, pending work or identification; the user reports apparently normal drawing/dashing. The first unchanged-code module exchange succeeds in 0.8 seconds and the user confirms normal appearance (T-029). Next: T-027 live INI/Core, keys, persist and reload for this character; its interpreter is not yet linked into the deployed backend. LIMIT-027's historical no-bone after-images were not exercised in the dash sample and remain an uncovered follow-up, not a newly reproduced visual defect.

## Tasks and follow-ups

### T-031: INI language and runtime (Claude from 2026-10-18; branch claude/ini)

Handed to Claude by the user on 2026-10-18. Working in `.worktrees/ini` (branch `claude/ini`) until the branch can be merged with the main tree, where Codex's uncommitted 2026-10-15 work sits.

Done: D-067, original warnings are recoverable; INPUT-001 resolved (candidate 1.3.8).

Done 2026-10-18: installed operation census (scripts/ini_operation_census.py, suite step; docs/ini-language-coverage.md). Installed Mods use 16 of the 32 original command classes in 57 variants; the character content runs in the production renderer; the image panel `k/1.ini` shipped in about 20 Mods is the construct that cannot run in the game; three Mods use CustomShader compute; no Mod replaces a texture by its hash.

Done 2026-10-18 (candidate 1.3.9): execution matrix per original command class (docs/ini-execution-matrix.json, scripts/check_ini_execution_matrix.py, suite step), BUG-025 teardown crash fixed, D-068 upscaling flip and overlay accepted.

User decision 2026-10-19 ("first do 1, complete the INI"): complete INI execution before the panel-only route. Phases, by dependency:
- Phase 1, owned GPU execution in the game. P1 (done in the sandbox in 1.4.6, docs/owned-present.md; not loaded into the game): Present, key and Constants lists run owned GPU commands in the character generation (a composite of the character extension and GpuShaderRuntime; GPU commands only inside a host scope, so replaced draws keep the plan path), executed in a private submission before vkQueuePresentKHR, bb = the imported swap chain image (PRESENT_SRC in and out, semaphores), window sample and swap chain size from intercepted surfaces and swap chains. This also brings the image panel of about 20 Mods into the game. P2: GPU work inside matched draws (clears, copies, store, CustomShader in TextureOverride lists; typhoeus) recorded into private command buffers submitted before the application's submission, and owned buffers bindable by replaced draws.
- Phase 2 step (1.5.0, docs/several-packages.md): every installed character Mod packaged without captures where isolated planning allows (13 of 22), several packages at once with lazy GPU residency. Next: release of unused packages, one engine for the whole directory instead of per-Mod generations, the Mods isolated planning refuses.
- RabbitFX roles (1.5.8, D-082, BUG-030, docs/rabbitfx-roles.md): done in the sandbox for Mod meshes. Texture-only replacement of game draws: done in 1.5.9 (D-083, docs/texture-only-draws.md). Hash-only hide sections: done in 1.6.0 (D-084). ember's application texture references and chengqianyu's element offsets: done in 1.6.1 (D-085). Si's own identification: done in 1.6.2 (D-086). Typhoeus P2: done in 1.6.3 (D-087; physics off, LIMIT-035; NVIDIA division fault D-088). Yvonne/Xaihi: load with their shader sections inert in 1.6.4 (D-089, LIMIT-036); acting on game shaders needs DX11 shader identity in Vulkan, which needs the user's decision on a game-wide shader table. All five offline items of 2026-10-20 done as far as they can be without a game session or that decision. User 2026-10-20: set shader-keyed features aside and test the implemented ones in the game. First session froze (BUG-032); 1.6.5 adds queue following, uploads on any queue, `m2_owned` and a stall watchdog. Second session froze too (BUG-033: device memory oversubscribed after many activations; also one package per recording). 1.6.6: activation needs memory to spare (D-092), shared recordings (D-093). Third session with 1.6.6 (no freeze, three characters, stutter at zero free memory) led to D-094/D-095 in 1.6.7: textures on first bind, eviction under a device budget, reduction under pressure, timeline publication, memory priority. Fourth session with 1.6.7 (texture thrash, black characters, F10 freeze; BUG-034) led to D-097/D-098 in 1.6.8. Fifth session with 1.6.8 (BUG-035) led to D-099 in 1.6.9 through eight module exchanges in the running game: 24.5 ms against a 20.9 ms baseline. 1.6.9 deployed 09:42; the sixth session led to 1.7.0 (identities D-100, material rules and the pipeline cache D-101, the layer's upload queue with waiting draws and one variant cache per device D-103), deployed at 13:40. Then offline through the replay: the INI dispatch (command kinds decided once at adaptation; about a hundred commands per matched draw), bones on the GPU (variant vertex shader reading the game's bone buffer through a remap table), one variant cache per device plus a VkPipelineCache on disk (character switches wait for variants), the Present lists of 22 packages (1.6 ms per frame), the tracker's per-draw bookkeeping. Next game session with 1.6.8, `profile_tracker = 1` and `unmap_snapshot_limit = 0`: watch `textures` (evictions should stay near zero while characters stay on screen), `gated_frames`, `present_work` and `draw_record` per draw, and whether F10 holds; `m2_owned = 0` remains the fallback. Still open from the session: the maxima of `submit_tracking` (56 ms) and `command_reset` (61 ms), one owned submission per frame per package with GPU Present work (a command pool and fence created per submission). Then: BC7 transcoding of uncompressed textures into a project-owned cache, sparse residency for the mip tail, a transfer queue for uploads, a device-level draw count index (one lookup per draw instead of one per package), skipping texture-hash queries for generations without texture-hash sections.
- Capture replay (1.5.6, D-080, BUG-028, docs/capture-replay.md): 13 of 16 catalogue characters replaced in every pass in the sandbox replay; a suite step. Since 1.6.4 all 16 load and replace (Si D-086, Typhoeus D-087, Yvonne and Xaihi D-089); `exhausted` counts at a replay's start are the snapshot pool filling (the same in 1.6.2 and 1.6.3 for vk03). Next offline: P2 GPU work inside matched draws (Typhoeus' chest deformation), the Core's spatial identification run for Mods that call it themselves (LIMIT-034, Si), faster first appearance (done in 1.5.7: batch publication and load priority; still open: reading the next file while one uploads), ElementOffset edits, xaihi/yvonne shader sections (DX11 shader identity).
- Core source facts (1.5.4, D-079, BUG-027, docs/core-facts.md): done offline; the installed Mods' dry run is a suite step. Done in 1.5.5: the R32 blend-index edit, namespaces with spaces and implicit ones. Next offline: ElementOffset edits (DX11 semantics of Vulkan attributes), hash-only index hide sections (cost of hashing every draw region), a dry run that includes the owned runtime (Mods' other INIs, CustomShader), then one engine for the whole folder.
- Directory generation step (1.5.3, D-078, docs/directory-generation.md): character Mods read from the Mods folder, packages made in the layer; 19 of 22 load in the sandbox. Next, offline: hash-only index hide sections (`this = null` without an index count), the R32G32B32A32_UINT blend-index edit (chengqianyu), implicit namespaces (last rite), then one engine for the whole folder (libraries in other folders, shared keys), releasing unseen characters, reading meshes only on activation.
- Texture identity (1.5.1, D-077, docs/texture-identity.md): done in the sandbox. 1.5.2: meshes of other INIs stay the game's, inert ShaderOverride sections load; 12 of 13 directory packages load. Next: RabbitFX texture roles (LIMIT-004; six of the twelve; assessed in docs/rabbitfx-assessment.md, option waiting for the user), xaihi's shader skip (needs DX11 shader identity), the nine Mods planning refuses (seven by original diagnostics, mostly RabbitFX references inside callback lists; gilberta DDS sRGB; last rite without namespace; chengqianyu's R32 edit), the uncompressed-format prefix rule against game data, first texture-hash matches in the game.
- Phase 2, the directory generation in the game: all enabled Mods in one generation, routing of matched objects to their Mod, on-demand residency instead of offline character packages.
- Phase 3, remaining language families (item 2 below, plus rt_width, cursor images, DPI and the [System] settings). Done in 1.4.7 for everything that does not read game resources: fps, effective_dpi, cursor hotspots, ->region expressions, and in the owned domain rt_width/rt_height and buffer hashes (D-074, docs/live-ini.md). Left: rt_width in replaced draws, scissor_*, cursor images, ->lastframe on pools, buffer hashes on game data, fuzzy texture matching, the [System] fallback size; their installed users (Si Lunabelle's guard, Yvonne's translucency capture) read game resources, so they belong with phase 4.
- P2 analysis (2026-10-19, waiting for the user): the installed users of GPU work inside matched draws (typhoeus' chest physics reading the game's vs-t0 and vs-cb regions, Si Lunabelle's guard reading the game slot vs-t119) read game bindings, which need the shader catalogue, and must run inside the game's render pass in draw order. What P2 can do without the catalogue (owned work on Mod-owned resources at draw time, submitted before the application's submission, owned buffers bindable by replaced draws) would only run Typhoeus' deformation from a rest state, with no visible change. User decision 2026-10-19: collect game data for the catalogue first.
- Catalogue data collection: done on 2026-10-19 (16 characters in both APIs; CURRENT_STATE). Next, in this order (D-075): (1) derive the DX11-to-Vulkan correspondence from the shaders' own interfaces and measure it against the 16-scene ground truth, so characters never captured are covered (done for t registers, D-076, docs/source-slots.md; the layer derives the rules of every unprofiled fragment shader from it since 1.4.8); (2) per-pass alignment in the pairing for objects drawn a different number of times; (3) constant buffers by static content; (4) BUG-026; (5) the correspondence as source facts in the runtime (phase 4, game-binding part of P2). The protocol as proposed: session A under DX11 with the current launcher configuration (XXMI and the EFMI MCP DX11 probe): per character the character display with mods off (F11), scripts/dx11_baseline.py capture with shader declarations, DXBC archive and texture hashes. Session B under Vulkan: the user replaces the injected library in the launcher UI (signed) with dist/efmi_vk_inject.dll and asks for the two Vulkan values; layer 1.0.2 unchanged, dist/efmi_vk.cfg with replacement off and image hashing on; the same scenes captured, all pipelines and SPIR-V exported. Same window size, quality settings and characters in both; optionally LOD1 at distance. Offline: the pairing already covers every object when summarize runs without --mod; scripts/shader_catalogue.py (2026-10-19) merges scenes into the catalogue (docs/shader-catalogue.md). Next after collection: constant buffers by static content instead of size, structural matching tested on the collected ground truth, then the catalogue as the runtime's source facts for game bindings (phase 4 and the game-binding part of P2).
- Phase 4, families acting on the game's own shaders and textures (game-slot checktextureoverride as in gilberta and arclight, texture-hash overrides, ShaderOverride and ShaderRegex/RabbitFX), which need the shader catalogue of T-030 and therefore game data collection, deferred by the user.

Backlog, proposed order:
1. The image panel in the game, resumed with PNG by the user on 2026-10-18. Steps: (1) window, resolution and cursor operands - done in 1.4.0; (2) PNG files as owned images with the pinned WIC loader semantics - done in 1.4.1; (3) owned `o0 = set_viewport bb` at Present - host back buffer done in 1.4.2 (3a); 3b: CustomShader bytecode cache rule (D-070), null views (D-071) and the installed panel end to end - done in 1.4.3, matching native DX11 on the same GPU; (4) layer: swap chain and surface interception, window sample, panel INI in the character generation, drawing on the swap chain image before present. Deployment and loading in the game only on the user's word.
1b. Extend the execution matrix below class level: copy options, target kinds per destination, resource properties, image formats, so that each census variant maps to a row.
2. Families that are still parse-only: remaining copy conversions and options, fuzzy texture matcher metadata (refused today), system and driver settings. Region and spatial hash properties: done in the owned domain in 1.4.7.
3. PNG/WIC resources: resumed by the user on 2026-10-18 as part of item 1.
4. A directory generation in the production renderer (several Mods, routing of matched objects, on-demand residency), together with the shared shader catalogue of T-030.
5. Merge claude/ini into main at checkpoints (first merge 2026-10-18 at 447b3aa). Each merge sets Codex's uncommitted main-tree work aside and restores it (branch codex/wip-2026-10-15 holds it).

### T-030: Generic directory runtime and assets (active; user-directed architecture)

User decisions on 2026-10-13/14 supersede the preliminary ideas below: the input
is the enabled Mods directory and its original Core/library/companion dependencies.
INI syntax, models, skeletons and textures use shared implementations. No per-Mod
syntax whitelist, independent component code or mandatory paired character
capture. Use the pinned original language engine in production (already linked
for the scoped 1.0.2 runtime), not a second miniature expression interpreter.
Yvonne is an additional regression fixture; the user deferred game collection.

First offline checkpoint: all installed INIs parse; eight language fixtures pass;
all nine bone initializers succeed; declared DDS/buffer assets are audited. Six
isolated callback fixtures pass, with remaining dependency/renderer boundaries
explicitly recorded. 1.0.3 candidate build and full regression pass in
build/next/run_all-1.0.3-verified.out. The second original main INI runs with
synthetic native Core inputs; no second-character shader mapping or GPU claim.
The latest user correction prioritizes every source-defined INI construct and
its Vulkan adapter, not sample-driven acceptance. Audit upstream parser dispatch,
section metadata, expressions and resource directives; expand tests from that
inventory and connect directory dependencies to production generations. Pause
PNG integration until this language/runtime work is addressed.
The standalone PNG decoder is verified in 28 CPU cases and committed separately;
it is not linked into the runtime and does not close the PNG rendering gap.
1.0.4 connects original Include loading to CPU directory generations and parses
all dormant explicit lists in that mode. Key/preset run targets are compiled,
IniParams/presets/abort execute on the CPU, and stable-root adaptation repairs
optimizer branch lifetime. Source-derived parser/evaluator fixtures are now in
run_all.py; docs/ini-language-coverage.md records the remaining families.
1.0.5 follows with original pool metadata/proxy/index/element resets, independent
mixed-element resource state and original per-frame counter reset. The old broad
reference invalidation fails the added fixtures; 23 pool adapter checks and three
new production file-plan/isolation checks now pass. GPU resource copy/clear and
expiring/keyed resource pools remain separate work.
Acceptance still needs a complete graphics directory generation, native
registration/routing of multiple objects, common mesh/image ownership and shader
contracts, general CustomShader/copy/dispatch and ShaderRegex adapters, then actual
supported Mods in Vulkan. Parsing alone is not rendering acceptance.

Review note (Claude, 2026-10-17, after reading 1.0.2 to candidate 1.3.6): the
architecture above is sound and the live runtime is proven in the game. Its
unbuilt keystone is the shared shader catalogue with application shader
provenance (D-044): fixed ps-tN slots, texture-hash overrides, ShaderOverride
by game shader hash and ShaderRegex/RabbitFX all act on the game's shaders, so
each needs, per Vulkan pipeline of a matched draw, the DX11 shader identity and
bytes, its register-to-descriptor map and a constant-buffer layout check. The
owned DX11 execution cannot be connected to game draws without it. Constraints
set by the user: no per-character paired captures, game collection deferred.
The game's assets sit in an encrypted custom VFS (StreamingAssets/VFS, 63 GB);
breaking that is not a route for this project. Proposed offline experiment with
data already on disk: DXBC of arcane's shaders (captures/dx11-lod1-20261013/
*.dxbc), their SPIR-V (Vulkan captures) and the 20 known pairings as ground
truth. If structure alone (vkd3d translation of the DXBC compared with the
game's SPIR-V: interface signature, sampled-image use order, constant buffer
sizes) recovers the pairings and register maps, the catalogue can later be
filled from two ordinary sessions (one DX11 shader dump, one Vulkan module
collection) without paired captures. Not started; a proposal.

Historical survey and preliminary ideas (not the current implementation plan):

Survey of the nine character mods installed on this machine (pattern counts over the INI files, read-only, approximate): all nine use the merged skeleton, LOD1 sections and the same one vertex-layout edit; each has 12 to 22 key sections and 27 to 40 variables. Textures: fixed `ps-t` slots only in two (arcane, ardelia); overrides by texture hash (`this =`) in six (liino, tangtang, xaihi, yvonne, laevatain, perlica), four of them with fixed slots as well; six reference RabbitFX (liino, tangtang, typhoeus, yvonne, laevatain, perlica; LIMIT-004), which patches DX11 shader assembly by regular expressions per shader family.

What is already general: finding the mod's components (the INI's hash is the CRC-32C the layer computes, plan fact 5), merged bones, identity, LOD, the layout edit, feature variants of shaders (sibling rules), building a package from INI and core. What is tied to the one mod: the table from source slots to Vulkan bindings per fragment shader (20 profiles from paired DX11 and Vulkan captures); one fixed state of the variables; one package per process; no RabbitFX.

Ideas put to the user, in the order proposed:
1. Move the knowledge from the mod to the game: a table per fragment shader of the game (which binding is which source slot), shared by every mod, and replacement by the identity of the original texture as the main mechanism, which is what overrides by texture hash already mean. Two cheap experiments decide the route: whether 3DMigoto's texture hash can be recomputed from what the layer sees (offline, from captures/dx11-baseline, where DX11 resource hashes and content hashes are both recorded), and how many fragment shaders of a second character are already among the 20 profiles and their siblings (needs frames with another character on screen).
2. Variables and keys at run time: the packager compiles the mod's command lists into a table of conditions per component instead of one scenario, a small interpreter in the module evaluates it, the hosted original engine stays the reference for differential tests.
3. Several mods at once: a table of packages, textures loaded on demand within a budget, the mods folder scanned.
4. RabbitFX as its own work: first only what it does for textures (the mod's texture instead of the original one, which falls under 1), effects later as SPIR-V rewrites per shader family.
Also noted: profiles keyed by exact shader bytes all expire with a game update; identity of textures does not.

### T-029: Deployment without leaving the game (implemented; first exchange verified in the game)

State 2026-10-13, live verification: the unchanged verified 1.0.1 module takes over PID 19452 at 20:49:41, generation 1 after 0.8 seconds, nine images Ready and no module error. The user confirms normal appearance with no persistent anomaly. A post-exchange 710-frame interval is clean, followed by the clean requested dash interval. Evidence: build/hot-swaps.json and captures/stability-1.0.1-20261013-204259-19452/. Use scripts/hot_swap.py for subsequent compatible backend changes. Not claimed: repeated-exchange lifetime/performance acceptance or a seamless transition on every exchange. Variant warming remains an optional follow-up.

Decision of the user, 2026-10-13: write the replacement logic as a module of its own; whether to swap it into the running game is the agent's choice, for instance to verify a change of the replacement logic right away. Recorded in AGENTS.md. Point 4 below is thereby settled.

The user asked for a way to deploy while the game runs and for the agent to refresh the deployed state itself. A loaded DLL cannot be replaced, and the tracker's state (every object the game created since start-up) cannot be rebuilt by a second copy of the layer. The original proposal below is now implemented; its permission question was settled by the user's standing authorization recorded above:
1. Host and module. The injected DLL keeps the hooks and the tracker and is not replaced during a session. The character backend with what it owns (variants, snapshots, skeleton, package, materials) is built a second time as a module DLL behind the ten functions of `character_backend.h`; the built-in copy stays the default, so nothing changes without a module.
2. A probe command loads a module from a file copied to a unique name, checks an interface stamp made from the shared headers (a module built against other tracker structures is refused and needs a restart), creates its backend, and swaps the active one between draws. The old backend stops replacing and is released when its last command buffer is; old modules stay mapped until the process ends.
3. Sandbox proof before any game use: swap in the middle of a scene under validation, with draws before and after replaced and nothing leaked; refusal of a mismatched module.
4. Rule to settle with the user first (AGENTS.md says the layer is not loaded into the real game without the user saying so on the spot): a swap loads new code into the running game. Either each swap is announced and approved, or the user gives a standing permission and it is written into AGENTS.md.
Limits known in advance: changes to the tracker or to shared structures still need a restart; a crash in a new module is a crash of the game.

### T-028: Whole character from a static package (1.0.1 sampled LOD0/LOD1 and dash checks pass)

Latest evidence: captures/stability-1.0.1-20261013-204259-19452/. Before exchange, a 1,534-frame LOD0 interval and 101-frame LOD1 interval have no refusal/identification/pending. After exchange, 710 LOD0 drawing frames remain clean. A requested dash interval has 58 captured frames with 3,766 character matches on 98 supported pipelines, no new runtime refusal/identification, and active sibling/inherited material replacements. The user reports no apparent flicker, normal appearance after exchange and apparently correct dashing. Historical no-bone after-image shaders were absent; preserve LIMIT-027 as uncovered rather than requiring repair of an unobserved present defect. The earlier PID 27816 appearance-only review remains in captures/user-review-1.0.1-20261013/.

Requested by the user on 2026-10-13: replace everything, not one strip; the agent builds it. Step 1, meshes and shared bones with the game's textures, is built, verified in the sandbox and against saved frames, and deployed (`docs/character-backend.md`, D-035). The backend takes its draw lists from the hosted frontend's plans and its bone logic from `MergedSkeleton`; it adds identity, identification, coverage and hiding natively, following the core's INI.

Game session of 2026-10-13 (build 0.10.0 = 0.9.4): the character is drawn in the mod's outfit; flicker at LOD 0 from `refused_set3`, 82 in 378 frames. 0.9.5 binds the application's set 3 again instead of refusing and was deployed at 13:28 after the user closed the game; confirm in a new session (`refused_set3` 0, `restored_set3` growing, no flicker). See CURRENT_STATE.

Next, after the actual DX11 capture of 2026-10-13:
1. Complete: original LOD1 source capture, eight rules for the three missing fragments, both historical Vulkan frames agree. Audit and native material replay pass (359 accepted, eleven dimension refusals, no unknown profile). See docs/lod1-material-capture.md. No further LOD1 source capture is needed for these profiles.
2. Complete offline and deployed in 0.9.6: static hosted pixel-binding plans, 20 exact fragment profiles, nine DDS files, shared ResourceCache, native material contracts and command-owned image snapshots. LIMIT-021 uses an explicit policy bound to the measured device. Full suite passes; native contracts cover 370 historical draws with no omissions, 105 pipelines create, actual configuration uploads all nine images through both entries. This does not complete hosted runtime integration. See docs/character-materials.md and D-037.
3. Actual 0.9.6 game result: PID 18868 loads the right DLL but refuses initialization at the one-graphics-queue requirement, with zero replacements/images. 0.9.7 supports multiple queues in one graphics family and publishes images through one-time semaphore waits. Full rebuilt suite passes, character hooks 261 across eighteen cases, actual nine DDS files pass with only queue 1 used, and both old entries reproduce the refusal. User exited normally; 0.9.7 deployed at 16:23 and actual-config smoke passes on dist. Required next: a new user-started Vulkan check of loaded version, queue/publication/image state, replacement/refusal/restoration counters, whole-outfit textures, motion, LOD1 and flicker. Launcher remains Vulkan. A capture still does not mark replaced draws (LIMIT-020).
4. 0.9.7 game result (user, 2026-10-13): textures seem right, flicker at LOD 0 persists. Cause traced to the game's visibility pass on four-set pipelines (BUG-009); 0.9.8 takes the private set's index from the layout and passes sandbox, real-shader and full-suite checks. Deployed at 17:59 after the user's exit, smoke on dist passes. Open: a user-started check of counters and flicker.
5. 0.9.8 game result (user, 2026-10-13): dithered fade and dash show the original model mixed with the mod's; the normal state seems to flicker now and then. 0.9.9 derives rules for unprofiled fragment shaders from a profiled sibling, keeps the mod's mesh in any case, and lets an identity survive gaps (BUG-010, D-040). Deployed at 19:00 after the user's exit, smoke on dist passes. Open: check dither, dash and the normal state in the game with the new counters and log lines.
6. 0.9.9 game result (user, 2026-10-13): the view from below is right; the normal state still flickers. The 0.9.9 log shows the cause: frames were counted at the present (BUG-011). 1.0.0 counts the character's frame by the recording of its command buffer. Deployed at 19:25, smoke on dist passes. Open: the user's judgement and the identity lines of the log.
7. Latest 1.0.1 live results above are positive for ordinary LOD0/LOD1 and the requested dash sample. The historically unsupported after-image shaders did not occur in that sample (LIMIT-027); collect them when reproduced rather than assuming every dash still fails.
8. Generalizations the sample did not need: draws with several instances, a second mod at once, a four-set pipeline with material images in a sandbox scene.

### T-027: Data-driven character replacement and frontend integration (in progress)

Current boundary: 1.0.2 links the original main-INI engine and implements live
conditions/keys/persist/F10 with the existing native renderer. Offline and sandbox
verification is recorded above. The user confirms outfit keys in the game;
read-only PID 31108 evidence proves two F10 exchanges and saved choices matching
live state. Across-restart persistence and exhaustive key/effect acceptance remain
open; the short status interval has missing-buffer variant refusals (LIMIT-027).
The latest request is to generalize beyond this character without per-component
code; second-character selection is pending while the generic boundaries are
audited. General
source-image identity queries and image-hash override registries, arbitrary
CustomShader/Include adapters, and the auxiliary panel remain outside this
accepted profile. T-028's material correspondences still supply the renderer's
shader profile. The review and checkpoints below are historical.

Review, 2026-10-13 (second agent, at the user's request; implementation and design). No defect found in 0.9.2/0.9.3 or the standalone frontend; every suite was rebuilt and rerun independently with the documented counts. The architecture follows plan D2/D4 and section 4 and is judged sound. Points to settle before the frontend is linked into the DLL, in this order:

Structure of the sample, measured on 2026-10-13 from `arcane.ini` and its blend buffers (which bones carry a nonzero weight), as the basis for staging a whole-character replacement: components 3, 5, 8, 9, 10 and 11 have no mesh in the mod (the original draw is to be skipped; their bones still feed the merged skeleton). Components 0 (20,507 vertices, 75,987 indices), 4 (1,860 / 9,576), 6 (90 / 732) and 7 (40 / 108) use only their own bones and have one draw each with the original's index count. The costume itself hangs on component 1 (324,044 vertices, about 1.4 million indices in 13 draw commands, 27 of its 49 bones belong to other components) and component 2 (37,953 vertices, 208,692 indices, 42 of 73 bones from other components): these two cannot be drawn without the shared skeleton, and the deployed 0.9.1 path (one package, one self-contained component, fixed strides 16/8/12/16) takes none of the others either. A first whole-character milestone that needs no new DX11 evidence is meshes and bones with the game's own textures (shape and animation visible, colors wrong); the material work (LIMIT-021, LIMIT-022, source texture identity) can follow it.

0. In preparation since 2026-10-13, 12:04: point 1 below. Package `build/m2-component7-visible.bin`, settings file switched, waiting for the user to restart the game; details and what to look for are at the top of CURRENT_STATE.md.
1. A visible check in the game comes first. No replaced draw has been seen to render correctly yet: component 7 has the original positions. The existing 0.9.1 path with a deliberately altered copy of the package (project-owned bytes, for example displaced positions) would confirm skinning, entry patch, variant and restoration on a real character in one short session.
2. LIMIT-023: four-set pipelines; private set index from the layout; skip and bone import must not depend on the variant preflight; one counter per refusal condition.
3. LIMIT-021 needs a decision, not a permanent refusal: eleven of 370 historical draws stay original otherwise. On this machine's DX11 driver the mismatched binding read zero in the headless test, which is what explicit null reproduces; that is a named policy for the user to accept, not a default.
4. Cost on the recording thread is unmeasured. The whole historical replay (566 plans, process start, parsing and 38 MB of file IO included) takes 0.105 s, an upper bound of 0.19 ms per plan and far too loose for a budget of about 58 matched draws per frame. Measure plans per second without IO in the host. Each matched draw also copies the complete 72,480-byte palette once for the snapshot and once per emitted draw into the arena, about 10 MB per frame for one character; the master changes only at imports, so slices can be shared per master generation.
5. Profiles (vertex semantics, texture slots) and source texture identities come only from paired DX11 and Vulkan captures. That is right given stripped SPIR-V and a non-monotonic slot order (fact: `ps-t13` to binding 4, `ps-t14` to binding 3), but it puts a DX11 session by the user in front of every new shader. Key profiles by shader bytes so that characters share them, and collect DX11 sources in bulk.
6. The DDS reader is stricter than the loader XXMI uses (refuses a wrong linear size, trailing bytes, several legacy pixel formats). Safe, but existing mods are the target; follow the acceptance rules of the original loader.
7. `.ai-memory/CURRENT_STATE.md` has grown to 521 lines and 110 KB of checkpoint history; a resume now reads about 160 KB before it starts. Move finished checkpoints to the session log.

Former data dependency closed on 2026-10-13: actual original LOD1 DX11 declarations/bytecode establish fragments 0aeef59b, 310c25c3 and a6d7088a against both saved Vulkan LOD1 bundles. Source-identity provider, runtime/generation and full hosted DLL integration are separate. Dist is now the static whole-character material backend. No LOD2+ work or long-duration test requirement.

Historical driver checkpoint (2026-10-13): all 88 accepted whole-character pipeline/layout/vertex/material combinations from saved LOD0/LOD1 data create under clean validation in a separate GPU process (177 checks, 353 covered draws). The replay verifies serialized state exactly before creation and records omitted unknown/dimension cases. This closes driver-construction coverage for accepted historical contracts, not texture identity, live resources or character appearance. Runtime/profile and DLL integration remain open; no deployment/game action. See docs/fragment-materials.md.

Original texture-query checkpoint (2026-10-13): generic native mediation now executes original hash/draw matching and callbacks, inclusive ranges, dynamic slots, filter indices and nested this bindings. Providers must prove source-API identity; unknown/fuzzy/unadapted requests refuse. Original Core OverrideTextures is enabled automatically when a provider is registered before bridge installation. Native checks 21, actual Core synthetic-identity checks 22, spatial 21, palettes 24/283 and historical 370-draw replay pass. Still required: actual runtime source identities/profiles and DLL integration. Historical material calls remain unresolved rather than assuming no matches. No game/deployment change; LOD0/LOD1 only.

Combined GPU handoff checkpoint (2026-10-13): Ready files, resolved vertex/material contracts and immutable palettes now form one retained transaction through shared VariantCache/DrawSnapshots/emission. Standalone GPU assets pass 253 checks with clean validation, including distinct per-draw mesh/material/entry state, original restoration and sealed resubmission after producer retirement. Historical material outcomes remain 353/6/11 accepted/unknown/dimension-refused. No DLL or game change; native texture queries and runtime/profile integration remain next. LOD0/LOD1 only. See docs/frontend-draw-plans.md.

Native command bridge checkpoint (2026-10-13): named native handlers can invoke adapted original callbacks within the same bounded transaction; nested failures remain fatal and post callbacks resolve aliases after pre execution. Each indexed item retains its optional immutable palette through owned file plans and GPU-resource resolution. Host native bridge checks 12 and asset GPU checks 201 pass with clean validation; original spatial 21, palette 24/283 historical records, and 384 callback plans remain passing. This is the execution/ownership bridge, not the complete EFMI Core implementation or DLL integration.

Spatial palette epochs checkpoint (2026-10-13): SpatialPalettes now consumes original Core slot/hash results, keeps continuity only within the original time/proximity rules, retires expired/reused slots and invalidates on incomplete execution. It assigns no spatial slots itself. Original Core plus palette tests pass 21, including actual full-pool FIFO reuse; retained snapshots survive retirement. The caller must observe every identification and invalidate after a pool reset or failed original execution. Historical palette replay and 384 callback plans remain passing. Native Core/material execution and DLL integration remain open; no new game data or deployment.

Shared palette checkpoint (2026-10-13): PaletteBank owns bounded per-identity masters for LOD0/LOD1, with original component ids, per-pose completeness, better-LOD priority and immutable compact snapshots. The original initialization file loader owns all eight R16 remaps. Native palette checks 24 and metadata checks 15 pass; 283 historical records yield 74 imports, 21 complete snapshots equal to an independent reference, and 262 expected incomplete refusals. Each saved frame uses a separate identity, so no continuous tracking is inferred. Original spatial checks 12 and all 384 callback plans still pass. Caller-supplied identity epochs, native Core/material execution and DLL integration remain open. No layer/deployment/game change; report build/shared-palettes/report.json.

Spatial Core checkpoint (2026-10-13): original SpatialIdentification.ini and Helpers.ini now execute through mediated queries against borrowed CPU instance windows. Only SpatialHash buffer access is adapted; quantization uses pinned XXMI code and its original pool/state machine handles identification. Twelve headless cases pass for two instances, movement, new/expired objects, duplicate callers and invalid windows. Source/skeleton/callback regressions remain separate. This does not yet assign retained palette epochs or emit character draws. LOD0/LOD1 only; shared palette completeness and native Core/material/DLL integration remain next.

User scope, 2026-10-13: support LOD0 and LOD1 only. Do not develop or test LOD2 or later. Preserve the original Core pool stride when reading its declarations, but do not resolve/load later-LOD assets or make later LODs a completion gate.

Core initialization checkpoint (2026-10-13): the hosted planner now executes original scalar pools, pool references and runtime callback aliases, with guarded indices and owned declaration references. read_skeleton_plan copies evaluated pool metadata without parsing INI text. Original sample initialization yields 499 bones, 8 instances, 13 declared slots/12 active components, and 8 file remaps; all agree with the older independent reference. Explicit identity and unassigned LODs stay distinct. Host tests: 23/28/19/15 plus 11 Core-pool and 10 skeleton metadata checks; 384 callback plans still pass. GPU initialization remains an unresolved native event. Shared spatial identity/completeness and Core/native material/DLL integration remain open. See docs/core-skeleton.md. Source/build/next 0.9.3 and dist 0.9.1 unchanged.

Per-draw checkpoint (2026-10-13): FileDraws captures ordered bindings, null/application distinctions, layouts and arguments with retained Ready owners. It refuses unresolved native calls and any late file-bound failure atomically. Typed upload extrema prove whole-file bounds for all 404 actual draws; 200 GPU checks plus 28 real BUF readbacks pass with clean validation. Host 23/28/19/15 and 384 plans pass. Next: shared character/Core identity and bone completeness, native material resolution and original pipeline/declaration validation, then DLL integration. No layer or deployment change.

Current priority (user clarification): continue the pre-VRAM-discussion plan and get this character's complete replacement working first. Keep shared component-independent operations, but defer multi-mod residency/eviction and performance tuning. The unverified optimization draft is outside source; the verified cache remains unchanged. Next: hosted frontend resource/command resolution, then shared character/Core identity and bone completeness.

Owned-resource checkpoint: FilePlanCompiler snapshots XXMI file declarations and preserves binding order/nulls, native boundaries, indexed arguments and layout edits. prepare/request_file_plan connects them to ResourceCache with complete typed Ready owner collection and unchanged output on pending/refusal. Host 23, planner 28, resource planner 19, DDS 15 and asset/GPU 161 pass, clean validation; all 384 actual callback plans match original file binding metadata and bounds. See docs/frontend-draw-plans.md. Still open: per-draw binding state, native Core/material callbacks, runtime range adapter, shared identity/bone completeness and DLL integration. No layer/deployment change.

Latest resource checkpoint: common ResourceCache supports images and vertex/index buffers under one worker, one pending upload, one caller queue and shared generation budgets. 149 headless checks and five actual-cache checks pass for all 38 bound sample resources; peaks resident 315,327,232, staging 67,108,864, source 67,109,012 bytes, validation clean. Material pixels use cached mesh/images after cache retirement. No layer version change. Continue hosted resource/command resolution and shared character/Core completeness; resource scheduling itself is no longer texture-only. See docs/resource-assets.md.

Mesh upload checkpoint: BufferUpload prepares exact vertex/index bytes on device-local memory, records transfer-to-vertex/index barriers, submits on the caller queue and publishes after polled completion. Actual allocation budgets survive retained owners. Asset/GPU checks 138 pass, plus all 28 selected actual BUF readbacks under clean validation. Material fixture uses uploaded VB/IB. Standalone only; next unify file/resource scheduling for images and buffers, then resolve frontend/Core transactions. See docs/resource-assets.md.

Latest checkpoint 0.9.3: image relocation/null contracts now integrate into shared PipelineVariant/VariantCache and DrawSnapshots. Full layer build/regression passes; cache 28, snapshots 34, variants 63. GPU cases cover per-draw red/green images, null output, original blue restoration and owner retirement/resubmission under clean validation. Descriptor budgets count retained original layouts too. The component hook still has no material requests, and standalone TextureCache/XXMI host are not yet integrated. Continue general mesh-buffer loading, resource/command resolution and shared character/Core. No deployment; docs/fragment-materials.md.

Explicit-null operation complete within the standalone supported image types: typed zero sampling/fetch/query results, selected opaque loads removed, other image reads retained. Asset/GPU fixture 92 passes, validation clean; all nine historical fragment modules/19 mapped bindings validate after null adaptation. Pixel controls show why a zero image alone is insufficient. This is not automatic dimension-mismatch handling or production integration. Continue descriptor/pipeline ownership and shared character/Core. See docs/fragment-materials.md.

Dimension investigation: matching archived DX11 shader hashes confirm 3D declarations. Independent DX11 pixel controls pass on NVIDIA/WARP; invalid 2D/3D bindings yield different results (zero/color), so no portable coercion rule is justified. LIMIT-021 default refusal remains. Next: valid explicit-null shader semantics, descriptor/pipeline integration and shared character/Core; an observed-device compatibility option, if implemented, is explicit data rather than component branches. See docs/fragment-materials.md.

Latest material checkpoint: standalone fragment-image relocation and offscreen pixel test pass; full headless asset fixture now 79 checks, validation clean. Sixteen historical 2D moves across seven shaders pass spirv-val and exact reference comparison. Three original-resource mappings describe 3D placeholders while DX11 mod-on draws bind 2D DDS views; they remain valid correspondences but are refused for replacement pending source/API semantics (LIMIT-021). No partial acceptance of such a callback. See docs/fragment-materials.md. Continue descriptor/pipeline integration, shared character state and this offline compatibility investigation.

Latest resource checkpoint: TextureCache provides bounded on-demand file/GPU ownership and immutable Ready owners across generation retirement. Worker handles reads/preparation/recording/fence polling; caller alone submits one prepared upload at a time. Shared actual-allocation budgets bound source/staging/residency. Headless checks 57, sample cache checks five and all ten DDS payload readbacks pass under clean core/sync validation. Actual sample image residency is 283,377,664 bytes. No mesh-buffer manager, descriptors or DLL integration yet. See docs/resource-assets.md.

Full original entry/Core checkpoint (2026-10-13): a generic CPU bridge now prepares evaluated skeleton metadata and mediates config reads, source selection and palette application while keeping original component matching, spatial decisions, instance iteration and callbacks. Pre/post roots form one bounded transaction. Five historical bundles (283 records, repeated for two synthetic updates) produce 566 plans, 283 original first-frame skips, 137 intentional mesh-empty callbacks and 370 draws; all arguments, patched entries and complete palettes match an independent reference. Four invalid-profile/changed-metadata/recovery/disabled-mod controls pass. Native bridge checks 14, original spatial 21 and palette 24/283 remain passing. OverrideTextures is still unresolved (283 calls); no material/GPU character execution or DLL linkage is claimed. Continue material/profile resolution and backend integration. Source/build/next 0.9.3 and dist 0.9.1 unchanged. Report build/core-components/report.json.

Material contract checkpoint (2026-10-13): exact-fragment MaterialProfile separates unknown, source-undeclared and mapped slots. Per-draw contracts preserve explicit nulls and Ready image ownership; unsupported late requests refuse the entire transaction. Standalone GPU assets pass 221 checks with clean validation, including profile-selected replacement pixels. Historical material audit covers 370 draws across 21 fragment shaders: 353 accepted contracts, six expected unknown LOD1 correspondence refusals and eleven expected 2D-for-3D refusals. All accepted combined bytecode patches validate. This is not complete Core texture-override handling, GPU creation of game pipelines or character rendering. Missing LOD1 fragments are 0aeef59b, 310c25c3 and a6d7088a; identical bound images across passes cannot establish a DX slot mapping. Continue vertex declaration and remaining frontend/backend integration offline. Reports build/material-plans/report.json and build/assets-gpu/report.json.

Vertex declaration checkpoint (2026-10-13): VertexProfile retains exact VS code and observed semantic correspondence, calls the pinned original XXMI input-layout evaluator, then constructs shared stream patches from per-draw file strides. Default application streams remain intact; unsupported semantic relinking, inherited-stream edits, rates, aliases and late invalid declarations refuse atomically. All 18 native cases pass, including 12-to-16 and 4-to-8 byte blend streams. Full original entry replay now compares every vertex declaration for all 370 emitted draws as well as arguments/entries/palettes. All 17 actually drawn VS modules have prior paired semantics. Host, spatial 21, palette 24/283 and 384 callback plans pass. No GPU game-pipeline creation, whole-character hook, deployment or game result; native texture queries and combined backend handoff remain open. Source/build/next 0.9.3, dist 0.9.1.

User direction: whole character support, not handwritten adapters per component. Reuse the pipeline cache, VS isolation, bone importer, command-owned storage and restoration. Introduce component/LOD/resource/draw data as input to shared backend operations; integrate the hosted XXMI frontend under D-016 rather than treating an asset packer as an INI engine. Cross-component skeleton completeness, material descriptors, multi-instance identity, conditionals/keys/reload and the image panel need explicit coverage. All existing captures can be used offline. The next user game check should show a substantial visual change, not ask the user to inspect the tiny component-7 mesh. See D-033.

0.9.2 checkpoint: shared stream-contract editor and ordered draw transactions are implemented and used by the old component hook. Full suite passes in build/next; native recipes 134, mesh/declarations 49. GPU readback covers two resolved draws, atomic rejection of a later invalid item, original-state restoration and sealed resubmission. No component branches or INI execution; no deployment. See docs/general-replacement-operations.md. Continue frontend/resource/shared-skeleton/material integration.

Frontend feasibility checkpoint: pinned XXMI 1.2.2 compiles/links and passes 23 CPU host checks via scripts/probe_xxmi_host.py. Original parser/evaluator/pools execute; synthetic key edges exercise both duplicate listeners. Root versus included duplicate sections and runtime ElementFormat index resolution are verified. See docs/xxmi-host-prototype.md. This is a standalone headless executable, not DLL integration or installed-mod/Core/resource/persist/reload compatibility. Resource/state adapters and shared character execution remain open.

Frontend callback checkpoint: general command-tree mediation passes 28 native checks and executes all 12 real sample draw callbacks in 384 plans across 16 selected scalar scenarios and both LOD values. All 18 unique draw ranges fit the actual vertex/index files; 38 bound resource declarations. The original expressions/control flow are reused. Unknown GPU queries/commands reject, and native Core calls remain unresolved explicit operations. No per-component code, resource upload or DLL integration. See docs/frontend-draw-plans.md. Continue shared character bones, GPU resources/materials and full Core/input/persist/reload integration; no new game input is required for these offline tasks.

Asset checkpoint: generic bounded 2D DDS metadata decoding passes 15 native checks and all ten bound sample textures, preserving BC7 linear/sRGB and BC5U payloads/mip ranges. The 38 bound files total about 301 MiB; whole-character resource budgets/preparation cannot reuse the tiny component package limits. Two missing declarations are unused in tested callbacks. See docs/resource-assets.md. Async GPU loading/upload, device compatibility, descriptor ownership and readiness remain work; no game result is claimed.

GPU resource checkpoint: separate ImageUpload implements preparation, serialized caller-queue submission and nonwaiting fence publication. Headless GPU checks pass 27; all ten actual selected DDS payloads round-trip exactly with clean core/sync validation, including 8K files. Synthetic blit verifies sRGB versus UNORM. It remains standalone, with no game feature/queue observation or scheduler/descriptor binding. Next: bounded resource-manager ownership and integration with actual device/queue state, material descriptors and shared character/Core execution. Details: docs/resource-assets.md.

### T-026: One self-contained component through the draw hook (sandbox complete; game execution observed)

Latest: PID 3080, 0.9.1, display/LOD0, +806 matched/+806 replaced in the stable 4.008-second sample. Later unsupported reaches 227; its grouped reason is unresolved. Both component-7 draws in saved frame 51,570 have supported recipes and complete record-equal snapshots. The eye-region mesh has original XYZ positions; user observation cannot accept its tiny unchanged appearance. User voluntarily exited. No LOD1 replacement/animation/visual acceptance is claimed. Details: docs/m2-first-replacement.md. The following deployment notes are historical.

0.9.0 implements an explicit package for sample component 7, mapped per-draw current/previous import, asynchronous variants, command-owned immutable snapshots, successful-reset recycling and original draw restoration/fallback. Native package checks 30, actual-hook runner 45 and the full suite pass. The candidate's actual configuration loads the original asset package in a sandbox with clean validation and normal exit. The ten matching draws in five saved frames satisfy the profile; this does not prove game driver creation or visual correctness. Details and current configuration: docs/m2-component-hook.md, D-031.

Deployment is complete: backup build/backups/before-0.9.0-20261012-185849/, twelve candidate files hash-verified, deployed actual-config smoke clean (build/deployment-0.9.0.json). Paused at the user's request. After an explicit resume, the next checkpoint is user-started Vulkan observation in display/LOD0 and actual LOD1. Check loaded version, ready/matched/replaced/refusal counters, retained original rendering and animation; live enabled=0/1 affects future recordings. Do not claim full M2: other components, cross-component masters/LOD remaps, texture overrides, several character instances and the image panel remain open. No launcher edit or automatic game start.

Review, 2026-10-12: 0.9.0 would have skinned the replaced component with each next bone's matrix (BUG-005). 0.9.1 corrects the patched offsets and is deployed (build/deployment-0.9.1.json, backup build/backups/before-0.9.1-20261012-191540/). The sandbox vertex shader now addresses bones as the game's does. At the game checkpoint read `status.replacement`: `replaced` should grow while `unsupported`, `unreadable` and `exhausted` stay at zero or are explained by `last_reason`. A capture does not mark replaced draws (LIMIT-020). The mod's index list for component 7 equals the game's, so a correct replacement should look like the original.

### T-025: Asynchronous variant cache and private draw snapshots (sandbox helpers complete)

The 0.8.3 helper checkpoint adds a bounded worker cache with generation/vertex-declaration keys, cancellation and command-owned ready variants, plus immutable-after-seal per-draw bone/entry storage with full uniform-range padding. Native cache 21, snapshots 25, GPU variant runner 39 and the full suite pass. The real GPU test retires the cache before submit, retains its variant and resubmits sealed commands. Details: docs/m2-cache-and-snapshots.md. Candidate 0.9.0 now supplies actual command-hook ownership/sealing and completed-recording recycling for T-026. Game rendering remains unverified. Dist/ stays 0.8.1.

### T-024: CPU rolling-buffer bone import and instance patch (offline helper complete)

Source/candidate 0.8.2 implements the Core-derived CPU path with checked ranges, per-instance/current/previous separation, duplicate LOD remaps, finer-LOD priority and atomic failure. Native 42 and full regression pass. Offline replay of 283 draws across five saved frames matches an independent Python reference after every entry/master update. See docs/m2-bone-import.md and D-030. No captured DX11 compute-output buffer was available, so direct original-output equality and real-character animation are not claimed. GPU snapshots, command-buffer lifetime, pipeline caching and hooking remain T-022/M2 work. Dist/ stays 0.8.1.

Corrected in 0.9.1 (BUG-005, D-032): the patched offsets are one matrix below the first merged bone, `(1533, 3030)` for the sample, as the core's INI, the DX11 baseline and the game's vertex shader agree. The reference of the replay follows; native checks are 43, the replay of 283 draws passes with every entry at that pair and an unchanged master.

### T-022: Owned pipeline variants and state restoration (M2; recipes and observed LOD1 bone timing captured)

0.8.0 adds complete core graphics/render-pass recipes with independent lifetimes, a C++ VS-only rewriter, private pipeline/layout construction and transactional direct indexed draw/restoration. Native ownership/error tests, real GPU pixel readback and core/sync validation pass. All 21 historical VS rewrites equal the Python reference and pass spirv-val. New `pipeline.recipe`, `layout_recipe` and `variant_preflight` make gaps explicit. Evidence and supported scope: `docs/m2-pipeline-variants.md`. The builder uses static vertex input (D-028), not a newly enabled game feature.

The user-started 0.8.0 session supplies complete supported recipes for all matched pipelines: display 39, world LOD0 45, actual LOD1 36. The 0.8.1 actual LOD1 session adds two full snapshot frames: all 58 matched entries and both VS bone windows agree at record/submit, including all nine LOD1 sections in color subpasses. This closes the observed LOD1 timing gap (T-023). See docs/m2-game-recipes.md. Candidate 0.9.0 connects the builder/emitter to the optional component-7 hook (T-026). Full sample assets/remaps/texture patches and real-character acceptance remain open; no complete M2 result exists.

Review note (2026-10-12, from `captures/third-probe`): 33 of the 45 matched draws use pipelines that declare five vertex bindings, (0,16) (1,12|20|8) (2,12|4) (3,16) (4,0); binding 4 with stride 0 is the game's default tangent buffer, which the mod does not replace. The tracker keeps four slots (`kMaxVb = 4`), so a capture does not show what is bound at slot 4. The replacement vertex declaration therefore has to carry binding 4 over from the recipe unchanged, and the replaced draw rebinds and restores bindings 0 to 3 only. Raising `kMaxVb` is needed only if a later mod replaces that slot.

### T-023: Capture an interval containing graphics while keeping snapshot recording armed (done in 0.8.1; actual LOD1 verified)

The user confirmed normal LOD1 display/movement, but snapshot captures repeatedly returned empty intervals while no-snapshot frames contain graphics. 0.8.1 implements the opt-in server wait (D-029): skip empty present intervals without merging data or stopping record-time reads until a drawn interval or timeout; report skipped intervals. Native 144, evidence 113 and the full suite pass, as do candidate/deployed actual-config smokes. User-started PID 34644 now supplies two actual LOD1 frames on the first collection attempt, after 8/7 skipped intervals, with all 58 relevant entries/current/previous bone windows equal at record/submit. Evidence: captures/m2-0.8.1-20261012/ and docs/m2-game-recipes.md. No attribution to anti-cheat, frame generation or LOD rendering failure is established. The MCP wrapper's new-option exposure is a later tooling follow-up; client/CLI are available.

### T-007: M0 inventory (done; static assessment)

`docs/compatibility-inventory.md` and `docs/compatibility-inputs.json` cover every lexical construct/command and direct EFMIv1 item used by the main mod and Core, plus the previously missed image panel. Pinned the exact release DLL and source commit; saved Core/mod tree and game binary/content-index fingerprints. Engine version is known; the game marketing label is not inferred. D-016 retains the hosted XXMI engine approach, with explicit adapter boundaries and a later compiled prototype. Reproducible snapshot check passes. No runtime compatibility is claimed.

### T-008: DX11 baseline capture with the sample mod (done on 2026-10-12)

State of the machine for this session: at the user's request the launcher's EFMI profile was set back to DX11 by this project (`d3d11_mode_cmd_args` = `-force-d3d11`, `xxmi_dll_inject_mode` = `DIRECT`; copy of the file before the change in `build/backups/launcher-config-before-dx11-20261012-100111.json`). The injected libraries are still `efmi_probe.dll` and `efmi_vk_inject.dll`; that value is signed and was not touched, so `dist/efmi_vk.cfg` was set to `mode = passthrough` for the DX11 session. Reverted at 10:43 the same day, after the user had closed the game and the launcher and asked for it: both launcher values back to empty and `SKIP` (copy before the change: `build/backups/launcher-config-before-vulkan-20261012-104327.json`), `dist/efmi_vk.cfg` back to `mode = probe`.

Done on 2026-10-12 in the user's DX11 session (PID 9264): one full frame with the mod on screen and constant buffer contents, `captures/dx11-baseline/frame-a.*`, taken with `scripts/dx11_baseline.py` through the DX11 probe of the sibling project, and paired with `captures/first-probe`. Results and tables are in `docs/dx11-baseline.md`; facts 15 to 19 of the plan and D-019 follow from it. G3 d is answered in kind (a table per fragment shader), G3 b and c are confirmed in structure from the DX11 side (instance entry, constant 5, bone buffer), G3 e in detail (only the blend index attribute and its stride differ, plus 32-bit indices).

The frame with the mods off followed the same morning, once the user had restored the game window: the DX11 probe pressed EFMI's F11, the frame after it (`frame-off`, EFMI printed `Mods Display: Disabled`) was saved, and a second press switched the mods on again (`frame-on`, `Mods Display: Enabled`). With it all 12 original components pair draw for draw with the Vulkan frame (45 draws each side), and each slot the mod's draw sections set was looked up per pass: 15 placed, 4 one of two (T-018), 37 not declared by the shader of that pass. The table is keyed by the Vulkan fragment shader (111 entries for 36 fragment shaders over the four frames, no contradiction). The mod's own toggle keys only select index ranges and were not exercised.

### T-009: Gate G1 in the real game (initial loading accepted; long-duration observation deferred)

Route: the inject entry through the launcher, no registry (D-011). Historical G1 runs used passthrough. Later capture sessions used probe (D-014); `dist/efmi_vk.cfg` now prepares a 0.7.2 diagnostic probe comparison for T-020.

Done on 2026-10-11 (details in CURRENT_STATE): one DX11 launch with the DLL present, 78 s, clean exit; one Vulkan launch that crashed in the NVIDIA driver about 5 s after device creation, with XXMI's `d3d11.dll` and the DX11 probe also injected (BUG-001). Timing of the injection is confirmed to be early enough.

Run 3 (20:38, only this DLL injected) passed the previous crash point. The user reports exiting before 30 minutes, with scene changes, and explicitly instructs the project to use that run's data. No game process remains; the layer log contains no device-destruction/frame-count record. This is accepted initial loading evidence, with long-duration stability still unverified. Do not require a repeat 30-minute run as a prerequisite. `dist/` now holds sandbox-verified 0.5.1 probe, ready for a user-initiated session; this version has not loaded in the real game.

What the user set in the launcher for run 3: the injection mode of the XXMI DLL to Skip (the Chinese label of that option is the one translated from "Skip"), leave only `F:\efmi vulkan\dist\efmi_vk_inject.dll` in the injected libraries, keep `d3d11_mode_cmd_args` empty, start. If the game still crashes: start it from the official launcher with nothing injected, to see whether Vulkan works by itself today. After each run read `dist\efmi_vk_layer.log`, `Player.log` and the launcher log.

To restore normal EFMI afterwards: `d3d11_mode_cmd_args` back to `-force-d3d11`, injection mode back to the first option (Direct), `efmi_probe.dll` back in the list if wanted.

The two launcher settings are independent (the user asked on 2026-10-11 whether the JSON value can go back once the injection mode is Skip; it cannot for a Vulkan session). `d3d11_mode_cmd_args` selects the game's renderer: with `-force-d3d11` the game runs Direct3D 11 and this DLL sees only the engine's probing instance. The injection mode decides whether XXMI's `d3d11.dll` is put into the process. A Vulkan session needs the value empty and the mode Skip; a normal EFMI session needs `-force-d3d11` and the mode Direct, preferably without this DLL in the injected libraries.

Where the argument lives: `d3d11_mode_cmd_args` under `Importers.EFMI.Importer` in `XXMI Launcher Config.json`; no control in the settings window, not a signed field (launcher source `core/game_launcher.py`, `build_cmd_args`).

### T-017: Snapshot at submit of the buffer ranges a matched draw uses (done on 2026-10-12: layer 0.6.0, sandbox and game)

Result in the game (CURRENT_STATE, `docs/probe-snapshots.md`, facts 21 and 22, D-021): the instance entry and a component's own palette are complete when its draw is recorded and final at submit; the bone count comes from the mod. Not done: MCP tools for the snapshot; other scenes.

Done: request, record-time checksum, submit-time bytes, comparison states, budget, `snapshot` command, client, collector options, sandbox scenario and tests (`docs/probe-snapshots.md`, CURRENT_STATE). Open: run it in the game on the scene of the first session (`docs/first-probe-session.md` has the command) and read, for the 45 draws of the 12 components, the entry item's record state (is the instance entry in place when the draw is recorded), the follow items' record state (are the bone rows), and the bytes themselves against the DX11 values (constant 3, constant 5). Then decide D3. Not done: MCP tools for the snapshot.

Original task text:

The first probe session read the instance config and the bones live, after the frame, and most reads were already overwritten (CURRENT_STATE). The layer should keep, for the draws of a captured frame whose region hash is in a list given with the request, the bytes of each uniform buffer range of at most 64 KiB as they are at submit, and for the instance config layout the bone rows at the two offsets of entry 5. With that, G3 b and c can be checked for all 12 components in one frame and across passes, and the same code path is what M2's CPU bone import reads. Also measure whether the bytes are already in place when the draw is recorded (compare a record-time and a submit-time read). Sandbox test with a mapped buffer that is rewritten every frame. The DX11 baseline fixes what to keep: the 256 bytes of the instance entry at the dynamic offset of set 2 binding 0 (16 constants; constant 5 holds the two bone offsets in 16-byte elements) and the bone rows from there.

### T-018: Tell textures apart by content (done for the display scene on 2026-10-12)

The third Vulkan session with 0.7.0 resolves all four former ambiguities by content: 19 placed mod slots, 37 undeclared, none open. Pipeline inventory is captured: 936 created graphics pipelines; the character uses 39 objects, 26 shader pairs, three sets and no specialization/push constants. Exact mappings and evidence: docs/third-probe-session.md and docs/arcane-texture-slots.json. Upload processing accumulated about 8.1 seconds; hashing is disabled for the next session. Intermittent stutter is tracked separately as T-020, not considered solved. Reproducing XXMI's texture resource hash remains T-016 work.

Original task text:

Two textures of the same size, mip count and format cannot be paired between a DX11 and a Vulkan frame, and a slot table built from looks alone stays ambiguous there. Four such entries exist for the sample mod: component 0 main pass `ps-t16` and `ps-t17` (set 1 bindings 1 and 3), component 0 render pass 24 `ps-t16`, component 4 main pass `ps-t17`. The DX11 half exists since 2026-10-12: `captures/dx11-baseline/frame-off-tex.textures.json` has CRC-32C and SHA-256 of mip 0 (rows without padding) of the 65 file-backed textures the scene reads with the mods off, keyed by DX11 resource hash, all distinct. For the open entries: component 0 light map `9a6c6973` is `7309a055` and normal map `733efa0d` is `12076a3f`; in render pass 24 `ps-t16` `908140d5` is `91bc27d4` and `ps-t13` `151c9982` is `8fcf3b42`; component 4 `ps-t17` `0e43c0bf` is `7ebbf40f` and `ps-t12` `9750205d` is `db2f1ff8`. What is missing is the Vulkan half. The layer sees every upload from process start: hash the bytes of mip 0 when they are copied from a buffer into an image (`vkCmdCopyBufferToImage`, source mapped or shadowed), keep the hash with the image and report it with the descriptor contents. On the DX11 side the sibling probe can read mip 0 of a bound texture; `dx11_baseline.py` then keys on the content hash. Check whether XXMI's own texture hash can be reproduced from the same bytes: the image panel mod (T-016) matches by texture hash.

### T-019: Keep the fragment stage on the game's data in a replaced draw (D-022; GPU sandbox proof done, real character open)

Decision: option (a). The vertex module of the pipeline variant is re-pointed to a set of the layer's own (set 3 for all 45 draws of the sample character); the fragment module is untouched. Evidence and the rule for finding the two variables: fact 23, `scripts/stage_usage.py`, `docs/dx11-baseline.md`. First GPU check now passes in 0.8.0's synthetic sandbox: both stages read the buffers, VS uses the replacement and FS keeps the original, verified by pixel readback. C++ also reproduces all 21 historical Python rewrites exactly. Real-character rendering remains open in M2/T-022.

Original question:

Under DX11 the core replaces the bone buffer and the instance window for the vertex shader only; the pixel shader keeps the game's and reads them (fact 19). Under Vulkan one binding serves both stages. Options: (a) in the pipeline variant, re-point the vertex module's two variables to bindings of an extra set the layer owns (two decorations each; the set layouts the game binds stay compatible because sets 0 to 2 are unchanged); (b) replace the shared bindings and show from the listings and on screen that what the fragment shader reads there (two translation terms of the component's first bone when a flag is set) stays right or does not matter. Decide with the SPIR-V of the paired fragment shaders in `captures/first-probe`.

### T-020: Diagnose intermittent frame-time spikes (closed on 2026-10-12: slow reads of write-combined staging memory at unmap, fixed by the streaming loads of 0.7.3)

Result in the game with 0.7.4 (CURRENT_STATE, `docs/unmap-snapshots.md`): 3 of 87,005 present intervals over 33 ms across the user's open-world run, copies at unmap at 3.6 GB/s instead of 0.33, and the user reports the stutter gone. The paragraph below this one, from the review before that session, names the mesh pools as the cause; that was wrong (`passed_over` 0 of 144,103) and is kept only as record. Not done and not required: a foreground-only interval sample of 0.7.4; removing the copies altogether.

Review text, superseded:

Review on 2026-10-12 (CURRENT_STATE, `docs/unmap-snapshots.md`, D-026): the bytes copied at unmap were 16 MiB index and vertex pools on memory type 4, which carry the transfer-source bit; the staging buffers the copy was meant for account for 7 MiB of the session. 0.7.3 speeds up memory type 2 and hardly type 4. 0.7.4 copies transfer-only buffers and drops shadows that may be stale. Open: the user-started comparison on the same route with the settings of the last sessions; what to read is listed in `docs/unmap-snapshots.md`. The task closes when the counters show the pools passed over, the tracker timings no longer accumulate seconds in the unmap path, and the user finds the movement smooth; if spikes remain, the tracker timings of that session say where to look next.

Earlier record:

The user reports stutter during loading and steady display in the third session. Average 120 presents/s does not resolve the report. T-018's timer establishes heavy earlier upload work but only 596 microseconds in a later three-second interval. Cause of steady-scene spikes is unknown.

Done: fully sandbox-verified 0.7.1 diagnostics, actual-config smoke and real-game status-only observation (PID 31124). The user reports severe open-world stutter; the static display cannot judge it. A 39.992-second foreground interval contains 142 intervals over 16 ms, 27 over 33 ms, 12 over 50 ms, maximum 111.204 ms. Final rolling window p50 2.989 ms, p99 24.590 ms. Image-hash time and capture sequence remain zero. A later quiet interval changes focus and cannot serve as a clean comparison.

Resource-off comparison, new user-started PID 33204: settings confirmed off, zero tracked images/descriptors/retained shaders, hashing and captures zero. The selected 30.000261-second interval has 300/300 foreground polls, 74 intervals over 33 ms, 26 over 50 ms and a retained peak of 105.000 ms. Excluded earlier background/focus-transition segments. Workload differs across sessions; do not infer that disabling resources worsens performance. See docs/pacing-resources-off-session.md.

Passthrough comparison: user-started PID 14804 at 13:04:17, log confirms 0.7.1 passthrough/inject and Vulkan device creation; no pipe. The user reports no stutter. This implicates probe-only work as a group, without a timing trace or a proven individual cause.

0.7.2 removed allocation for unreadable buffer-copy fallbacks and the submit-time intermediate copy, retaining data semantics, and added default-off profile_tracker counters (D-024). Real-game PID 41044 still stutters. A 24.030379-second foreground sample has 5,093 intervals, 17 over 50 ms, retained max 129.125 ms; unmap copies 3.46 GiB in 11.625 cumulative wall seconds. Three consecutive 43-44 ms snapshots span nearly all of the largest peak; copy recording concurrently waits with zero copied bytes. See docs/pacing-tracker-session.md for retention limits and evidence.

0.7.3 uses streaming loads only for detected WC mappings on capable CPUs, preserving full snapshots and fallback semantics (D-025). Full build/regression passes, native 118, and actual-config smoke passes before/after deployment. Local Vulkan type-2 8 MiB copy improves 24.023 to 1.270 ms; this is not a game result. Next: user-started same-world comparison after verifying loaded 0.7.3. Settings stay probe, resources/hash off, profile on, default snapshots/shadows. No 30-minute prerequisite; T-020 remains open until a fresh result. Source defaults still keep profiling off.

### T-021: Index identity when the game's mesh pools are not readable (withdrawn on 2026-10-12: the premise was wrong)

The pools are not unmapped: no pool was ever inside an unmapped range in the 0.7.4 session, and all 1,154 to 1,185 direct indexed draws of three open-world frames have their hash from the mapping. The text below rested on the misread generation counters. What remains true and belongs to M2: index bytes of a pool are read from write-combined device-local memory, so the product should hash a mesh once and remember it rather than read it every frame.

Withdrawn text:

The game keeps index and vertex data in 16 MiB pools on device-local host-visible memory and maps and unmaps them while it streams (2,612 generations on 22 pools in 22 minutes). A draw whose pool is unmapped when the layer wants its index bytes cannot be hashed from the mapping, and copying pools at unmap is what stalled the game. Reading megabytes from that memory every frame is not an option for the product either.

Measure in the next open-world session, with 0.7.4: of the indexed draws of a few captures taken while moving, how many have hash source `none`; how long a pool stays unmapped. Then choose: hash mesh data when it arrives through tracked buffer copies from staging (the 0.7.2 session recorded 2 MiB of such copies in 24 s, read from cheap memory) and key a cache on buffer, offset and count; or rely on the mapping where it is always present at draw time. Do not consider keeping the application's mapping alive behind its back without a decision of its own.

### T-010: Gates G2 and G3 in the real game (display scene and a first open-world scene answered on 2026-10-12)

Open world, three frames with 0.7.4 (`captures/open-world-0.7.4/`, fact 24): G2 unchanged (one thread, two command buffers); G3 a holds, with LOD1 sections matching in the shadow pass; the character's draws are direct with one instance. Not looked at in the open world: instance entries and bones (resources were off), texture slots of the LOD1 and shadow programs. Still unobserved: combat, a second instance of the character.

State after the third session and the DX11 baseline: G2 answered; G3 a, b, c, e and f pass for the display scene; G3 d now has all 19 applicable sample slot assignments placed, including the four former ambiguities. None of this is known for the open world, combat, several instances of a character, LOD1 or scenes with indirect draws. Smoothness remains T-020 and rendering remains M2.

Result of the first session (details and evidence paths in CURRENT_STATE): G2 answered for the display scene (one thread, one command buffer with all draws, classic render passes); G3 a passes (12 of 12 LOD0 components); G3 f passes for the scene; G3 b and c strongly indicated, to be made rigorous by T-017; G3 d needs T-008, and name-based mapping is ruled out because the SPIR-V is stripped; G3 e as expected. Still to observe in later sessions: open world and combat, several instances, LOD1, scenes where indirect draws occur.

Preparation that was used: `scripts/collect_probe.py` and `docs/first-probe-session.md` preserve raw frames, API usage, matching evidence and bounded shader/pipeline exports in a labelled, hashed bundle. Sandbox evidence-export regression passes 52 checks; it cannot substitute for a game session. The next required step is actual data, not further duration-only testing.

Start by collecting `efvk_api_usage` to identify the engine's actual paths. G2 is answered by `efvk_status` and `efvk_capture_frame` (threads, command buffers, secondary buffers, submits, render pass kinds). G3 item a is answered by `efvk_match_mod` on `arcane fes 1.2`, with the corresponding character visible. Items b, c and d need T-011. Historical passthrough logs contain none of these captures; D-014 removes the repeated-duration prerequisite but does not supply missing frame data.

### T-011: Track descriptor sets, shader modules and images (standard paths implemented; extended coverage open, M1)

**Done in 0.4.0, sandbox evidence only**: ordinary `vkUpdateDescriptorSets` writes/copies, layouts and pool/set lifetimes; dynamic offset attribution (LIMIT-005 fixed), graphics/compute state separation, immutable samplers, descriptor arrays and variable counts; image/view/sampler and texel-buffer metadata; retained pipeline SPIR-V and `efvk_shader` export with declared names/bindings. The sandbox actually uses two sets, dynamic storage buffers and a sampled texture. Full suite passes; detailed evidence and limits: `docs/probe-resources.md`.

**Added in 0.5.0, sandbox evidence only**: template creation/destruction, copied layout/entry definitions, offset/stride decoding, direct and template pushes with immutable per-recording contents, partial updates and layout disturbance. Push state is separate for each command buffer and bind point; secondary execution clears primary descriptor bindings. Native tracker 62 checks; 17 GPU probe configurations cover ordinary sets, templates, pushes and push templates through both entries with primary/secondary recording. Full suite passes; exact scope is in `docs/probe-resources.md`.

**Remaining**: descriptor buffers, specialized descriptor types, dynamic inline pipeline layouts, swapchain image metadata and full SPIR-V reflection. Newer aliases and compute/texel push wrappers need GPU coverage. Prioritize extended paths using actual engine API usage when available. Buffer visibility is still measured at submit, even when push values are recorded earlier; G3 b/c require separate record-time byte-availability evidence.

### T-012: Measure the overhead of the probe (done in the sandbox)

Implemented `--draws N`, warmup/sample control and host phase timings. `sandbox/benchmark.py` completed 36 runs at 500/2000 draws and 0/2 recording threads through the inject entry, comparing no layer/passthrough/probe. At 2000 draws, probe adds 0.386/0.744 ms to median recording plus submission. FIFO whole-frame time cannot measure CPU overhead by itself. Method and limits: `docs/probe-performance.md`. Timing regression: 84 checks. Game overhead and the M2 5% target remain unverified.

### T-015: Validation layers into `F:\vulkan` (done in the sandbox)

Built pinned Khronos VVL from source under `F:/vulkan`, with child-only activation. Fixed sandbox SPIR-V, shader cleanup and synchronization; 24 positive core/sync configurations plus three negative canaries pass 76 checks. Added to `run_all.py`; no registry registration or SDK installer. Initial default-cache incident and final cache confinement: `docs/validation.md`. Real-game validation is not claimed.

### T-013: Read-back from the GPU when a buffer has no CPU-visible source (open; only if G3 shows the need)

For buffers that are written by copies whose source was never visible to the CPU, or by compute. Appends a copy into a layer-owned buffer at submit time and reads it when the fence signals.

### T-016: Include the sample image panel in full-folder acceptance (M3 follow-up)

1.0.2 preparation: the unchanged panel HLSL compiles with DXC 1.9.2609 and both
stages validate as Vulkan 1.1 SPIR-V. Inputs/outputs agree and bindings 100/101/120
remain explicit. Report build/panel-hlsl/report.json and docs/live-ini.md. No
panel draw or input/present integration is implemented; k/1.ini stays outside the
runtime input list and passing it currently refuses explicitly.

The inventory found enabled `k/1.ini` and `Draw_2D.hlsl`, with two images, cursor input and the main toggle variables. Character-only rendering cannot establish full-folder compatibility. Its HLSL pipeline and input/present behavior must be implemented or explicitly reported unsupported. This follows the G2/G3-backed M2 backend; it does not justify editing or disabling the installed mod.

## Done

- T-001 Workspace bootstrap (2026-10-11): English plan, memory system, working agreement, text check, Git.
- T-002 Vulkan headers and build (2026-10-11): `scripts/fetch_vulkan_headers.ps1`, `scripts/build.py`.
- T-003 Region hash rule against the DX11 dump (2026-10-11): 336 of 336 distinct regions match.
- T-004 Layer skeleton and sandbox (2026-10-11).
- T-005 Tracking, frame capture and probe (2026-10-11): `sandbox/probe_test.py`.
- T-006 MCP server (2026-10-11): nine tools, `.mcp.json`, `sandbox/mcp_test.py`.
- T-014 Inject entry (2026-10-11): `layer/src/bootstrap.cpp`, `efmi_vk_inject.dll`, `efmi_vk.cfg`, passive device; probe and modes tests in both entries.
