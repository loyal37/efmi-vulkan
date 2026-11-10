# Architecture review of the 2.1.2 tree (2026-11-10)

The user, 2026-11-10: review the whole project, judge whether the architecture is sound, the INI
interpreter above all (it decides between a Mod that shows and a Mod that does not show at all), and
give a plan to complete it.

Scope: commit c69c1b6 (2.1.2 tree, not deployed). Read: the frontend (`frontend/xxmi`: the hosted
original parser and interpreter, `draw_plan.cpp`, `runtime.cpp`, `runtime_parser.cpp`,
`character_core.cpp`, the owned GPU runtime's failure handling), the layer's plan conversion
(`layer/src/character_program.cpp`), the call sites in `character_backend.cpp` and
`character_present.cpp`, the installed EFMI Core 1.4.9 and the installed Mods' INIs (read only), the
project's own documents (`ini-semantics.md`, `folder-generation.md`, `3dmigoto-flows.md`). Three
separate reviews ran beside it (the layer host, the verification, the original's language and a census
of the installed Mods); what they reported was checked at the cited lines before it was taken over.
Run: `scripts/check_ini_execution_matrix.py` only (section 6). Nothing was built, deployed or loaded
into the game. A finding marked "read" was followed through the code; one marked "inferred" was not
followed to its end.

## 1. Verdict

The decision the project rests on is right: the original's parser and evaluator are compiled unchanged
(`frontend/xxmi/ini_module.cpp`, `command_module.cpp`), so syntax, expressions, keys, presets, pools,
namespaces and the order of sections are the original's by construction. Parsing is not where Mods are
lost.

Mods are lost in what the layer does with the parsed commands. The interpreter is the original's, but
it runs against three different backends with three different failure models, and the one every
character draw goes through is the narrowest:

| Backend | Used by | Model | Failure |
| --- | --- | --- | --- |
| Owned GPU runtime (`gpu_resources.cpp`, `gpu_images.cpp`, `gpu_shaders.cpp`) | Present, Constants, CustomShader | D3D11's state machine on owned Vulkan resources | per command since D-137 to D-155, but one thrown check ends the folder's owned work for good (R-12) |
| Plan path (`draw_plan.cpp`, `character_program.cpp`) | every replaced draw | a whitelist: six plan operations, all or nothing | one unsupported line refuses the draw; the game's mesh stays |
| Native Core (`character_core.cpp`) | the EFMI Core's lists | C++ in place of the Core's INI, bound to its names and to the exporter's shape | the Mod is left out, or never recognised |

So the layer is general across characters, but not across ways of writing a Mod: it takes Mods of
the shape the current EFMI exporter writes against EFMI Core 1.4x. D-137 states the right rule ("refuse
the whole draw is not an outcome"); the structure does not enforce it, so it has been approached one
refusal at a time (D-138 to D-155) and the next Mod written differently finds the next one.

The plan (section 8) changes the structure in four places: the unit a failure is contained to, the
model a replaced draw's lists run against, what makes a Mod a Mod, and what the checks compare with.

## 2. The gates a Mod passes before it shows

In this order; a Mod that fails one does not show (A to D) or shows in part (E).

| Gate | Decided by | A failure shows as | The user is told |
| --- | --- | --- | --- |
| A. The INI is a character Mod | a text search for `[CommandListInitializeMergedSkeleton]` (`character_program.cpp:413`) | no draw of it is ever replaced | no |
| B. The folder loads | `IniRuntime::load`, any exception (`runtime.cpp:324-543`); then every Mod alone, one retry (`character_program.cpp:663-719`) | the Mod left out, or no Mod at all | log and status only |
| C. Discovery makes a contract | `character_core.cpp:682-873` | the Mod left out | log and status only |
| D. Registration | native `RegisterMod`, `required_version <= 1.48` (`character_core.cpp:465-470`) | `$mod_enabled` stays 0 | one overlay warning |
| E. Each draw | host preconditions, the plan (about 140 refusal points), the native contract, the plan conversion, materials and variants | that part keeps the game's look | first three reasons per Mod |

## 3. Findings: the INI interpreter and its backends

Severity is by what the user sees: high = Mods disappear or cannot appear; medium = a part or an
effect is lost; low = hygiene.

### R-01 (high, read). A replaced draw's lists run all or nothing against a whitelist

- The plan has six operations (`draw_plan.h:15`: BindVertex, BindIndex, BindPixelResource,
  DrawIndexed, External, Translucent). A resource assignment is taken only as a reference of a Mod
  resource (or null) into a vertex slot, the index buffer or a pixel texture slot, with special cases
  for the application's textures and its other slots (`draw_plan.cpp:1027-1128`). Operands are
  evaluated only when on a list (`cpu_expression`, `draw_plan.cpp:251-304`: the default is false).
  Everything else throws `Refused` (`draw_plan.cpp:20-24`, the catch-all at `:1232`).
- A throw anywhere ends the whole execution: every matched section's pre and post lists of that draw
  (`execute_impl`, `draw_plan.cpp:1568-1656`), then `IniRuntime::draw` reports "Original draw command
  refused" (`runtime.cpp:594`) and the host keeps the game's draw.
- Only the Present and Constants lists run each command on its own (D-155, `tolerate_refusals`,
  `draw_plan.cpp:1603-1612`); "a replaced draw's lists keep their all-or-nothing plan".
- Counted on c69c1b6 (`refuse(`, `throw std::runtime_error`, `return fail(`): `draw_plan.cpp` 140,
  `character_backend.cpp` 62, `runtime.cpp` 43, `character_core.cpp` 40, `character_program.cpp` 32.
- The original: a command is a D3D11 call on the context; one that cannot act does nothing and the
  list goes on. D3D11's runtime also accepts any binding at any slot whether or not the shader reads
  it, which is what makes the original indifferent to how a Mod is written.

### R-02 (medium, read). A refused plan keeps its variable effects

"Failure preserves output; frontend variable side effects are not rolled back" (`draw_plan.h:88-89`).
The commands before the refusing line ran (`$object_detected = 1`, pool writes, callback
assignments), those after it did not (`$active0 = 1`). The Mod's state after a refused draw is one the
original never reaches; what the Present and the next draws then do follows from it.

### R-03 (high, read). A Mod is a character Mod by a section name

`mods_characters` takes an enabled INI when its text has a section named exactly
`[CommandListInitializeMergedSkeleton]` (`character_program.cpp:405-415`, `has_section` at `:139-153`).
The name is the exporter's template's, not the Core's API (the Mod calls
`CommandList\EFMIv1\InitializeMergedSkeleton` from a list of its own). An INI without that section is a
plain input: its Present and keys run, none of its mesh sections is ever matched, and nothing says so
(`docs/folder-generation.md:22`, `:73-74`).

### R-04 (high, read). The contract is recognised from the exporter's shape

Discovery reads the Mod statically and by name:

- mesh sections are TextureOverrides with a constant `match_index_count` whose list assigns a constant
  to `$\EFMIv1\component_id` and to the Mod's `$lod_level`, directly or inside `if` branches, or under
  `if vbN->stride == K` (`character_core.cpp:154-173`, `:682-739`);
- the skeleton comes from `[CommandList<mod>InitializeMergedSkeleton]`, `$<mod>bones_count`,
  `$<mod>max_instance_count` and the pools `Pool<mod>_MergedSkeleton_Component_VertexGroupOffsets`,
  `..._VertexGroupCounts`, `..._LodRemaps` (`character_core.cpp:814-845`);
- a component with a LOD 0 remap or without a LOD 1 assignment, overlapping components, or no mesh
  section the renderer takes each refuse the whole Mod (`character_core.cpp:743-791`).

The same INI is thus understood twice, once by pattern at load and once by the interpreter per draw,
and the two must agree (R-05). A Mod whose lists compute the component, name these things otherwise, or
come from another exporter version fails the pattern although the interpreter would run it.

### R-05 (medium, read). The live plan must match the static contract

`contract()` (`character_core.cpp:358-366`) refuses the draw unless: one instance, first instance 0,
LOD below 2, the component and bone count of the contract, `$gpu_posed == 1`, `$custom_mesh_scale == 1`.
The host refuses instanced character draws before that ("character draw is instanced",
`character_backend.cpp:1943`). The Core decomposes instanced draws (`CommandListIterateInstances`,
`$instance_count = 8`) and has a path without the merged skeleton
(`CommandListComponent_SetInstanceConfig_VS/PS`); neither exists in the layer.

### R-06 (high, read). The Core version ceiling is a constant below the installed Core

The native `RegisterMod` fails for `required_version > 1.48` ("Mod requires a newer EFMIv1 contract",
`character_core.cpp:465-470`). The installed Core declares `$version = 1.49` (`Core\EFMI\main.ini`); the
installed Mods require 1.41 to 1.48 (census: 5, 4, 1, 1, 19). A Mod exported for 1.49 runs under the
user's DX11 installation and is switched off by the layer: its Present runs `RegisterMod`, the refusal
is skipped with a warning (D-155), `$mod_id` stays -1 and `$mod_enabled` never becomes 1.

### R-07 (high, read). The native Core is bound to the Core's names without a check

- The generation loads four Core files by name (`API.ini`, `MergedSkeleton.ini`,
  `SpatialIdentification.ini`, `Helpers.ini`: `character_program.cpp:244`, `:681-682`), not what
  `main.ini` includes (`ShapeKeys.ini`, `TextPrinter.ini`, `KeyBindings.ini`, `Libraries`). No installed
  Mod calls those by the Core's namespace today (census); one that does gets unrecognised entries.
- Seven Core lists are replaced by C++ (`character_core.cpp:465-542`), found with `.at()`
  (`character_core.cpp:26-27`), as are the Core's variables and resources they read (`:475`, `:519`,
  `:549`, `:559`). A Core release that renames or drops one raises `std::out_of_range` during load: the
  folder refuses, every Mod alone refuses the same way, and no Mod loads, with a map's error text in the
  log.

### R-08 (medium, read). The plan conversion narrows the plan again

`CharacterProgram::plan` (`character_program.cpp:807-1092`) turns plan items into the package's draws
and refuses the draw for:

- a vertex binding at slot 4 or above ("Runtime vertex slot outside profile", `:895`, `:913`); D3D11
  has 32 slots and ignores one the input layout does not read;
- a draw without the exporter's blend-index widening edit at slot 2, or with any layout edit other
  than that and a known element offset edit ("Runtime input-layout edits differ from the validated
  blend-index contract", `:945-964`): a draw that keeps the game's 8-bit indices cannot be drawn;
- an application texture bound at another pixel slot (`:835`, `:874`), an owned buffer outside a vertex
  slot below 4 (`:885`), one blend stream under two index widths (`:995`).

### R-09 (medium, read). A list that draws without `handling = skip` draws nothing

When the plan leaves the application's draw, only pixel bindings of the pre phase are kept (the
texture-only draw of D-083); any buffer binding or draw item returns an empty list
(`character_program.cpp:823-841`), the host counts it as kept and logs once
(`character_backend.cpp:2128-2138`). The original draws the Mod's draws and then the game's.

### R-10 (high, read). One failing input can still take the whole folder

- `IniRuntime::load` is one try block (`runtime.cpp:324-543`); the layer's own stages throw out of it:
  `RuntimeValidateSections` (`runtime_parser.cpp:73-101`), the ShaderOverride model check
  (`runtime.cpp:441-445`), the list graph and adaptation budgets (`ini_module.cpp:134`, `:151`,
  `draw_plan.cpp:1500-1501`), `validate_overrides`, and `prepare`, which stops at the first Mod that
  fails (`character_core.cpp:805-811`, thrown at `runtime.cpp:512-513`).
- The isolation then loads every character Mod alone and leaves out those that fail alone, once
  (`character_program.cpp:663-719`). It gives up, with no Mod loaded, when the folder fails and every
  Mod loads alone (`:718`: an interaction, a plain INI, d3dx.ini, a library), when the second attempt
  fails (`:708`), and when the cause is not a character Mod at all (only those are candidates, `:711`).
  TASKS already records "a plain INI that refuses at load refuses the folder".
- The original: the unit of a parse failure is a line or a section; nothing a Mod contains stops
  another Mod.

### R-11 (high, read). What is left out at start is not shown

A refused folder and a Mod left out at the first load are written to the log and to the status
(`character_backend.cpp:2932-2935`, `:2953-2964`). The overlay's notices come only from a draw's
refusal (`:821`), a Mod's gaps (`:833`), a refused F10 (`:3486`) and the reload time (`:3571`). The case
the user asks about most, a Mod that does not show at all, is the one without a message; so is R-03.

### R-12 (high, read). One failed owned command ends the folder's owned work until F10

- `GpuResourceRuntime::failed_` is set by any exception in a resource command (`gpu_resources.cpp:636`,
  also `:285`, `:713`, `:724`), by `execution_failed()` (`gpu_resources.h:50`) and by `discard()`
  (`gpu_resources.cpp:153`). Nothing ever clears it.
- With it set, `begin` and `finish` throw ("GPU resource generation failed; prepare a new generation",
  `gpu_resources.cpp:140`, `:147`), so every later Present's owned scope fails
  (`character_present.cpp:221`, `:228`), and a replaced draw opens no owned scope
  (`character_backend.cpp:1655-1657`): its lists that need one refuse.
- The latch was made for one Mod's generation (BUG-017). Since D-131 the folder has one owned runtime
  (`character_program.cpp:698`), so one Mod's failing command switches off every Mod's CustomShaders,
  panels, shape-key compute and owned draw work. About 280 `require(` checks stand in the owned
  runtime's files (`gpu_resources.cpp` 75, `graphics_commands.cpp` 65, `shader_commands.cpp` 48,
  `gpu_images.cpp` 41, `gpu_shaders.cpp` 27, `gpu_graphics.cpp` 22).
- A replaced draw that recorded owned work and then refused for any reason sets it as well
  (`runtime.cpp:598-600`, `gpu_resources.h:50`).

### R-13 (medium, read). The per-command rule of D-155 catches one exception type

The tolerant loop catches `Refused` only (`draw_plan.cpp:1604-1605`). `RangeIndexGuard` throws
`std::runtime_error` (`draw_plan.cpp:152`), as does `validate_pool_runtime` called from `check_texture`
(`draw_plan.cpp:698`, `:433-446`) and the original's own code where it throws. Such a command ends the
whole Present ("Frontend raised an exception while planning", `draw_plan.cpp:1648`), for every Mod of
the folder, every frame, and through `execution_failed` sets R-12's latch.

### R-14 (medium, inferred). Without the owned GPU runtime a CustomShader section refuses the load

`RuntimeValidateSections` throws "INI declares an unsupported shader/overlay section" for a
CustomShader section unless the extension executes custom shaders (`runtime_parser.cpp:94-99`), which
only the owned runtime's extension and the offline declarations do (`runtime_host.h:16`). When the
factory gives no runtime on a machine (`gpu_reason`, logged at `character_backend.cpp:2945`), every
input with such a section refuses, where the original skips a shader it cannot compile. Not followed:
which conditions make the factory fail.

### R-15 (known limits, read)

- ShaderOverride and ShaderRegex sections are inert with a warning (D-089, `runtime_parser.cpp:84-93`).
  Installed and enabled today: `Xaihi Venomous Vows 2.1\ps skip.ini` (two pixel shaders with
  `handling = skip`: not skipped under the layer) and `perlica Crimson Rumor 1.7\perlica.ini:704-706`
  (two shaders named by `filter_index`: a condition on them reads 0).
- A TextureOverride acts only inside replaced draws of character Mods; a Mod that only replaces
  textures of an unmodded object, or meshes without the Core, acts on no draw (R-03).
- LOD 2 and 3 stay the game's (the user, 2026-10-28).
- Discovery keys a section by its hash, index count and first index (`character_core.cpp:731-733`);
  `match_first_vertex` (three enabled Mods use it) and the other draw-match keys are honoured by the
  original's matcher at plan time only. No two installed sections differ in those keys alone.

### What the installed Mods use (census, 2026-11-10)

83 INI files: d3dx.ini, 14 of the Core (11 included), 68 in 31 Mod folders (41 files of 20 Mods
enabled). `Mods\DISABELD_TangTang Seaside Breeze 1.4` is misspelt, does not match `DISABLED*` and is
loaded, by the original and by the layer alike. Enabled Mods: 355 TextureOverride sections (351 with
`match_index_count`, all draw-match values plain numbers), 351 Key sections (cycle and hold only, 350
with a `condition`), 62 ShaderRegex and 6 ShaderOverride sections, 27 CustomShader sections; 2250
resource assignments (to vb0-3 720, declarations 761, pools 434, ps-t 130, ib 96, `this` 73, o 27, oD 5,
cs-u 2, vs-t 2), 416 `drawindexed`, 94 `handling = skip`, 20 `draw`, 19 `ElementFormat`, 17 `copy_desc`.
Never used by any installed Mod: presets, `handling = abort`, `drawauto`, the indirect draws,
`draw = from_caller`, `drawindexed = auto`, Key `back`, `wrap`, `delay` and transitions. The installed
set is one exporter's output with hand-written additions; it says little about Mods written another way.

The original's error rule, from its source (IniHandler.cpp:2342, :420-426, :447, CommandList.cpp:6407-6479,
:12457-12466): a line that does not parse is skipped with a warning and the section goes on; an unknown
section is kept and unused; a missing reference is that line's failure; a resource whose file does not
load stays null; the only whole-file skip is a false `condition` in the preamble, and nothing ever
skips another file.

## 4. Findings: the layer host

From the review of `layer/src` and `layer/module`. "read" here means the cited lines were read again
for this document; "reported" means the separate review cites them and they were not read again.

### L-01 (high, read). A failed activation leaves the character invisible

`activate()` sets `activated` once (`character_backend.cpp:1004-1006`). The worker returns when the
mesh cannot be made (`:1062`: a file changed or busy, an allocation), when the first batch cannot
(`:1083`), or on an exception (`:1090`); `ready` then never becomes true, and `record()` answers every
matched draw of the package with "skipped" (`:1898`: `return true`, counter `preparing`). The character
is neither the Mod's nor the game's until the game is restarted, and the notice says "that part keeps
the game's look" (`:820`).

### L-02 (high, read). After a folder refused at start, F10 does nothing

The reload request is the folder program's (`character_backend.cpp:3580-3581`:
`b.folder->program->take_reload_request()`); `folder_frame` returns without a program (`:2989`). A
folder that refused at the first load (R-10) has none, so the user who repairs the Mod and presses F10
gets no reload; the original always reloads. The game has to be restarted.

### L-03 (medium, read). 64 character Mods, the rest dropped with a log line

`kMaxPackages` (`character_backend.cpp:3358`, `:3412`): "no room for ... (64 at most)". D-141 made the
frontend's bound 4096.

### L-04 (medium, read). First-use waits are taken under the tracker's lock

`record()` holds `res_mx` shared and `mod_mx` (`character_backend.cpp:1892`) across the texture wait
(up to 4 s, `texture_residency.cpp:284-286`) and the variant wait (up to 4 s,
`character_backend.cpp:2323-2331`). The waits are D-120's (the original's first-use load also holds the
drawing thread); holding the tracker's lock through them also holds every thread that needs it
exclusively (create, destroy, map, unmap, descriptor updates). Reported with it: mesh-file reads in the
plan, EFMITTL shader preparation and a batch's 64 MiB clear under the same locks; `BoneMerge::prepare`
waiting up to 500 ms inside the game's submit (`bone_merge.cpp:409`).

### L-05 (medium, read). The layer's own failure is returned as the game's

`hk_QueueSubmit` returns the result of the layer's submission work without forwarding the game's
submission (`layer.cpp:1053-1054`, `:1091-1092`); `vkCreateDevice` with the layer's added features is
not tried again without them (`layer.cpp:1667-1673`; each addition is checked against support first).

### L-06 (medium, reported). A package's frame is all or nothing

One image that failed or is over the budget, or one pending variant, closes the package's frame
(`character_backend.cpp:766-789`, `:1842`, `:2343`); a failed image is bound again every frame, so the
Mod stays the game's until F10. Without pageable memory a lazy package never activates on low VRAM
(`:980-1002`), log only. An F10 is refused whole for one failed image or one unready package
(`:3519-3524`, `:3575`), with a notice.

### L-07 (medium, read in part). Facts of this game are constants

The instance entry at set 2 binding 0, the position at byte 48 and the bone offsets at byte 80, 512
implicit bones (`character_backend.cpp:1434`, `:1614`, `:1950-1978`); a vertex shader with exactly one
storage buffer (`spirv_repoint.cpp:38-40`, `:66`); a pipeline layout of at least three sets with one
free; only `vkCmdDrawIndexed` in a primary command buffer and a classic render pass; index pools that
are host-mapped (`== SRC_MAPPED`, `character_backend.cpp:3664`, `:3678`, `:1956`); one graphics queue
family. The DX11 Core finds the same facts by probing (`if vs-cb2->Size / 16 == 4096 ... elif vs-cb1`).
A game update that moves one of them makes every Mod keep the game's look, with per-draw reasons and no
statement that the game changed.

### L-08 (medium, reported). Lifetime has several mechanisms and unbounded tables

Command owners released at the game's reset, per-queue timelines, presenter tickets, fences,
semaphores destroyed "8 presentations later", destructors that wait a second and destroy regardless.
The variant cache has no eviction (entries leave when the game destroys the pipeline; at 4096 new
draws stay the game's, `variant_cache.cpp:75`, `:107`); `TextureResidency::images_` never shrinks.

### L-09 (low, reported). Three load paths and a legacy backend

The .bin package with its sidecar (`character_program.cpp:195-236`), per-Mod generations and the
folder generation all live; the folder path still validates through the legacy binary
(`validate_refreshed`, `character_program.cpp:160-189`); `M2Backend` is dead in a release while every
host entry is named `m2_*`. The module boundary is six functions on paper (`character_module.h:26-40`),
but the module takes the host's locks and reads its tracker maps directly.

No lock order inversion was found (recording: `character_mx` shared, the controller, `mod_mx`,
`res_mx` shared, `plan_mx`, the generation's scope; presenting and submitting are consistent with it).

## 5. Findings: project hygiene

- H-01. `.ai-memory` is 1.2 MB; `CURRENT_STATE.md` is 383 KB and cannot be read in one piece, while
  AGENTS.md makes it the first file of every session. `ARCHITECTURE.md` opens with the 1.1.x to 1.3.x
  candidates and describes 1.0.2 as deployed; nothing describes the 2.1 architecture in one place (it is
  spread over 76 documents and decisions D-001 to D-155).
- H-02. `character_backend.cpp` is 4213 lines (264 KB) holding the generation, the folder, identity,
  materials, bones, recording, F10 and the status.
- H-03. The budgets of `draw_plan.cpp:25-26` (65536 steps, 4096 items, 256 draws per plan) and of the
  plan cache (`character_program.cpp:802`) are not the original's; none is reached by an installed Mod.

## 6. Findings: verification

- V-01 (high, read). The judge never draws. `sandbox/dx11_judge.cpp:44-50` clears and presents; no
  case of `sandbox/judge_cases` has a TextureOverride, a key or a preset; the layer side is
  `resource_gpu.exe --present-ini`. The original is the judge of the owned runtime only. The path
  every character draw takes (R-01, R-05, R-08) has no comparison with the original at all.
- V-02 (high, read). The checks over the installed Mods assert that a plan is made, not what it is:
  `check_mod_states` keeps the boolean of `plan()` (`sandbox/character_test.cpp:490-494`); a plan that
  is accepted and wrong (a buffer, a slot, an order) passes. `ini-semantics.md:42-44` planned a
  comparison "with what the original executes".
- V-03 (high, run). `scripts/check_ini_execution_matrix.py`, a step of the `ini` group, fails on the
  current build: 32 classes, 41 probed uses, 39 failures. `docs/ini-execution-matrix.json` (2026-10-18)
  still expects the refusals D-142 to D-155 lifted ("Planner supports direct indexed draws only" is in
  no source file). The verification lists of those decisions do not name the step, so it has been red
  since 2026-10-28 without being seen; the one table of what each command class does in a replaced
  draw is out of date.
- V-04 (medium, read). The judge step's name matches no `--only` group (`sandbox/run_all.py:118`,
  `:142-149`): `--only ini` and `--only owned` skip it.
- V-05 (medium, read). The offline checks leave out refusals that contain "needs the owned GPU
  domain", "Original buffer window unavailable", "render targets are not known" or "offline "
  (`sandbox/character_test.cpp:61-69`, applied at `:491`). The first was BUG-051's in-game refusal of
  nearly every draw. Texture-hash sections never run in the state check (every slot is bound without a
  hash).
- V-06 (medium, read). Results depend on what is installed (the user's Mods folder, captures and Mod
  copies outside Git). There is no quick group; the suite takes about 30 to 40 minutes.

## 7. What is sound and stays

- The original's parser and evaluator, unchanged, per generation with its own engine state (D-129).
- The original as the judge (D-150), and the test-first rule.
- One generation for the folder (D-131) and the exchange at one Present.
- The merged skeleton and identification as native GPU work: under Vulkan the Core's HLSL cannot read
  the game's buffers by DX11 slot. The coupling to the Core is the problem (R-06, R-07), not the
  native implementation.
- The overlay's notices and the per-Mod gap list (D-137), once they cover the start (R-11).

## 8. Plan

> Progress, 2026-11-10 (sandbox only, nothing run in the game; details in .ai-memory/CURRENT_STATE.md):
> P3-0 done. Phase 0 done (D-156 for the frontend, D-158 for the host) except the game-facts notice of
> L-07. P3-1 done (D-157). P3-2 done as the judge's one-line sweep: 50 lines, 35 as the original's, 12
> recorded differences of the plan path, 3 not judged by its scene; it found two wrong plans, fixed
> (BUG-086, BUG-087). P4-1 done. P4-2 done (D-160): `character_backend.cpp` is eleven files by concern and
> `character_backend_internal.h`; the `character_backend.cpp:NNN` references in this document are lines of
> the reviewed commit c69c1b6. Open: P3-3, P3-4, P4-3, P4-4, and Phases 1 and 2, which need the game.

Each step test first, as the working agreement says: the check that fails before, the change, the
touched area. Phases 0 and 3a come first: they are small, and they make the rest measurable.

### Phase 0: contain a failure to its unit (no change of what is supported)

- P0-1 (R-11, R-03). Notices at the first load: every Mod left out with its reason, a refused folder,
  and every enabled INI with mesh TextureOverrides that is not taken as a character Mod. A line count
  in the overlay ("18 Mods, 1 left out") and a `mods-status.txt` beside the log for reports.
  Check: the live INI scene's left-out Mod has no notice today.
- P0-2 (R-06). `RegisterMod` as the Core's: the installed Core's `$version` (read from `main.ini`)
  against the Mod's `required_version`; the layer's own tested range is a notice, not a refusal.
  Check: a fixture Mod requiring 1.49 beside a Core 1.49.
- P0-3 (R-07). The Core's interface checked at load: every list, variable, pool and resource the
  natives use, looked up without `.at()`, missing ones named in one notice ("EFMI Core x.y: [list] not
  found; this layer knows 1.41 to 1.49"). Check: a Core copy with one list renamed.
- P0-4 (R-12, R-13). The owned runtime's failure belongs to its scope: a failed command discards
  that scope's recorded work, the command (or its root list) is disabled with its warning, and the next
  scope begins clean; the per-command loop catches every exception. Check: the live scene with one
  Mod whose Present throws inside the owned runtime; the other Mod's panel must still draw at the next
  presentation.
- P0-5 (R-10). No exception leaves a Mod's unit at load: `prepare` marks the failing Mod inert and
  goes on; section-level refusals make the section inert with a warning; the fallback bisects the
  inputs (plain INIs included) instead of giving up. Check: a folder that fails only together; a plain
  INI that refuses.
- P0-6 (R-14). Without the owned runtime, CustomShader sections are declared and inert with a warning.
- P0-7 (L-01). A failed activation is a refusal: the package's draws go back to the game's, the
  reason is shown as it is, and the activation is tried again (after an F10, and by itself once the
  cause can have passed: memory, a busy file). Check: the character replacement scene with a mesh file
  made unreadable after the load; today the character disappears.
- P0-8 (L-02). F10 belongs to the host: it reloads the folder whether or not a generation is loaded.
  Check: the live scene started with a folder that refuses, the Mod repaired, F10.
- P0-9 (L-03, L-07). The 64-package bound lifted to the frontend's; and one check of the game's facts
  at the first matched draws (the instance entry, the storage buffer, the layout) that says "this game
  version is not known to the layer" once instead of refusing draw by draw.

### Phase 1: a replaced draw's lists against a state, not a whitelist

- P1-1 (R-01, R-08). A draw state as D3D11's context: per input slot (32), the index buffer, per
  stage the shader resources, constant buffers and samplers, the output targets; each holding one of
  {the application's binding of slot S, a Mod file, an owned resource, nothing}. Every resource
  assignment is a total operation on that state: it cannot refuse. `application_textures`,
  `application_others`, `other_changes`, `vertex_bindings` and `pixel_bindings` become this one
  structure; "the application's binding of slot S" is a value that moves between slots and
  declarations.
- P1-2. A draw command records the state it sees. What Vulkan can honour is decided afterwards, per
  record and per slot, from a table kept in one place: honoured (a Mod buffer or image, the
  application's binding in place, null); ignored because the pipeline does not read it (a vertex slot
  the input layout has not: R-08's slot 4); degraded with a notice (a changed constant buffer or
  texture of another stage: the application's stays); not drawable (changed targets): that draw only.
- P1-3 (R-01, R-02). The lists run to their end. A command the layer cannot honour does nothing, with
  the original's warning once; the draw records made are drawn. No state is left half applied.
- P1-4. Operands the host cannot answer: a table per operand with the value and the notice, decided
  with the judge (P3-1) and the census, instead of a refusal of the list. The table and P1-2's are
  generated into `docs/ini-execution-matrix.json`, which becomes the one statement of what each
  command does (V-03).
- P1-5 (R-08, R-09). The conversion follows: the blend-index edit optional (the pipeline's own 8-bit
  element read as it is), layout edits applied as the original's `ApplyInputLayoutOverrides` gives
  them, the application's texture at another pixel slot bound from the private set, draws without
  `handling = skip` drawn before the game's.

### Phase 2: a Mod by what it does

- P2-1 (R-03, R-15). The device's index of sections comes from the generation's registry: every
  TextureOverride with an index match, whichever INI declares it. The plan of a matched draw says what
  it is: a replaced mesh, a texture-only draw, a hidden draw.
- P2-2. A plain replaced draw: the application's pipeline and descriptor sets, the Mod's buffers and
  textures, no skeleton service. It serves Mods that do not call the Core and the Core's path without
  the merged skeleton (R-05).
- P2-3 (R-04). The character contract from the executed lists: the skeleton from what
  `InitializeMergedSkeleton` and the pools hold after the Mod's own lists ran (as today), the
  sections' components and LODs from the plans as they run, the static reading kept only to prepare
  ahead. A Mod that fails the static reading is prepared at its first draw instead of being left out.
- P2-4 (R-05). Instanced character draws decomposed as the Core's `IterateInstances` does.

### Phase 3: checks that compare with the original where Mods are lost

- P3-0 (V-03, V-04), first of all. The execution matrix regenerated from the current behaviour and
  made green; the judge in the `ini` and `owned` groups; a `quick` group (text, unit, evaluation,
  matrix, judge) for every change.
- P3-1 (V-01). The matched-draw judge: `dx11_judge.cpp` draws an indexed mesh of known bytes with a
  vertex buffer, textures and a pixel shader that fingerprints what was bound; its d3dx.ini has
  `track_region_hashes = 1` and the Core's patternless ShaderRegex with `CheckTextureOverride = ib`.
  The same INI and bytes run through the layer's live scene. Compared: the persistent variables and
  the image. Cases: the matrix's 41 uses, pre and post, several sections on one hash,
  `match_priority`, `handling`, `this`, slot ranges, `draw = from_caller`. Open: whether the pinned
  DLL region-hashes in that program (to try first).
- P3-2 (V-02). "No single line makes a Mod vanish": for a known-good Mod and every command class,
  target kind and option the grammar gives (`check_ini_language.py` enumerates them), the line added to
  a draw list must leave the Mod's other draws planned and drawn. The generalisation of
  `check_mod_degradation.py`'s hand-written variants.
- P3-3. Golden plan digests per section and key state for project-owned Mods and a corpus of public
  Mods' INIs the user provides (INIs suffice for plans): load rate, refusals by reason. The count of
  refusal points reachable from a replaced draw is a number in the status of every round; it only goes
  down.
- P3-4 (V-05). The offline exclusions listed with their in-game counterpart; the state check binds
  hashed textures so texture-hash sections run.

### Phase 4: the project itself

- P4-1 (H-01). `ARCHITECTURE.md` rewritten as the 2.1 architecture on a few pages (the three
  backends, the gates of section 2, the threads and locks); the history moved to an archive;
  `CURRENT_STATE.md` cut to the current round and the open items.
- P4-2 (H-02). `character_backend.cpp` split along its seams (identity and bones, materials and
  variants, recording, the folder and F10, status), without a change of behaviour, between two green
  runs.
- P4-3 (L-04, L-06, L-08). The first-use waits taken without the tracker's lock (the draw's inputs
  copied out first); the frame gate per draw instead of per package, a failed image null as the
  original's instead of bound again every frame; one deferred-destruction queue for what the layer
  owns, and an eviction for the variant cache.
- P4-4 (L-09). The .bin package path, the per-Mod generations and `M2Backend` removed once the folder
  generation has no fallback left to them; the legacy binary round trip replaced by the checks it does.

### Order

P3-0, then P0-1 to P0-9, then P3-1 (the judge has to exist before Phase 1 changes what a replaced draw
does), then P1, P3-2 with it, P2, P4 alongside. Phases 0 and 3-0 need no game session. Phase 1 and 2
end in a deployment and a user-started session each.

## 9. Not verified

- Nothing here was run in the game or in the sandbox, except V-03.
- R-12, R-13, L-01 and L-02 follow from the code; no test reproduces them yet (the checks of P0-4,
  P0-7 and P0-8 would).
- R-14: the conditions under which the owned runtime is unavailable.
- L-04's effect on the whole process, and L-06 and L-08, are the separate review's reading.
- P1-5 and P2-2 are designs; whether the game's pipelines admit them (the 8-bit blend indices, a
  plain replaced draw through the application's sets) needs the captures.
