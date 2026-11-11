# EFMI Vulkan - Architecture (the 2.1 tree)

> Rewritten 2026-11-10 from the code (docs/architecture-review-2.1.md, P4-1). What was here before, the
> history of the candidates 0.5 to 2.0, is `.ai-memory/archive/ARCHITECTURE-until-2.1.md`. Verified state,
> versions and what is deployed are in CURRENT_STATE.md, never here.

## What it is

A DLL the launcher injects into the game's Vulkan process. It sits in front of the game's Vulkan calls,
finds the draws of characters that an installed EFMI Mod replaces, runs the Mod's INI through the
original 3DMigoto (XXMI) parser and interpreter, and draws the Mod's meshes and textures in the game's
own render passes. Nothing of the game, the launcher or the Mods is changed on disk.

## Binaries

| Binary | Built from | Role |
| --- | --- | --- |
| `efmi_vk_inject.dll` / `efmi_vk_layer.dll` (one binary, two names) | `layer/src` + the frontend | The host: hooks, tracker, probe pipe, and a built-in copy of the replacement code. The file name selects the inject entry (export redirection of the loader, `bootstrap.cpp`) or the layer entry. |
| `efmi_vk_backend.dll` | `layer/module` + the replacement sources | The replacement code alone, exchangeable while the game runs (`character_module.h`, `EFVK_ABI`, docs/hot-swap.md). |
| `dxcompiler.dll`, `efmi-vkd3d-shader.dll`, `SPIRV-Tools-shared.dll` | deployed beside the layer | The owned GPU runtime's tools: HLSL to SPIR-V, DXBC to SPIR-V, validation. Without them the owned runtime is not made and CustomShader sections do nothing. |
| `server/efmi_vk` | Python | Probe client, command line and MCP server (development only). |

## Source map

| Folder | What |
| --- | --- |
| `layer/src/layer.cpp`, `bootstrap.cpp`, `state.*`, `resources.*`, `resource_hooks.h`, `ipc.cpp` | Hooks, device and command-buffer tracking, descriptor and image metadata, the probe's requests. |
| `layer/src/character_backend.h`, `character_module.h` | The replacement's public interface, and the boundary to a module (`CharacterModule`, `HostApi`). |
| `layer/src/character_backend*.cpp`, `character_backend_internal.h` | The replacement, in parts by concern (the table below); the header holds the types they share. |
| `layer/src/character_program.cpp` | The layer's side of the INI runtime: which INIs are inputs, the folder's load and its isolation (`discover_folder`), the conversion of a plan into the package's draws (`plan`). |
| `layer/src/bone_merge.*`, `merged_skeleton.*`, `draw_snapshots.*`, `pipeline_variant.*`, `variant_cache.*`, `spirv_repoint.*`, `texture_residency.*`, `translucency.*`, `character_present.*`, `notices.*` | Skeleton import on the GPU, per-draw snapshots, private pipeline variants, texture loading and residency, EFMITTL's translucency, the owned Present, the overlay's notices. |
| `frontend/xxmi/ini_module.cpp`, `command_module.cpp` | The original's `IniHandler.cpp` and `CommandList.cpp` (pinned in `F:\vulkan\XXMI-Libs-Package\DirectX11`), included unchanged. |
| `frontend/xxmi/runtime.*`, `runtime_parser.cpp`, `runtime_shim.cpp`, `engine_globals.h` | `IniRuntime`: one generation of the original engine with its own globals (D-129), its load, `draw`, `present`, `save`. |
| `frontend/xxmi/draw_plan.*` | `DrawPlanAdapter`: wraps the original's command objects; decides per command which backend takes it; runs a replaced draw's lists against the draw state. |
| `frontend/xxmi/draw_state.h` | The draw state (D-161): `DrawState` (D3D11's context as values per slot), `SlotValue`, the table of what a draw makes of a slot (`honour_table`), the table of what an operand reads where nothing answers (`operand_table`, D-163). |
| `frontend/xxmi/texture_properties.*` | The original's texture property getters (`->Format`, `->Width`, ...) over a description: the host's of the game's texture, a Mod's file header (D-170). |
| `frontend/xxmi/character_core.cpp`, `efmittl_bridge.cpp`, `rabbit_textures.h` | Native replacements of the EFMI Core's lists and of RabbitFX; EFMITTL's shader groups run as their own lists, chosen by facts the host reads off SPIR-V (`layer/src/shader_hooks.cpp`, D-171). |
| `frontend/xxmi/gpu_*.cpp`, `owned_runtime.*`, `frontend/gpu/*` | The owned GPU runtime: D3D11's resource and state semantics on Vulkan resources the layer owns. |
| `frontend/assets/*` | DDS and WIC decoding, uploads, the file cache. |
| `sandbox/`, `scripts/` | Test programs, the suite (`sandbox/run_all.py`), builds and checks. |

The replacement's parts (`layer/src`, split 2026-11-10, D-160; one file of 4326 lines before):

| File | Concern |
| --- | --- |
| `character_backend_internal.h` | The types the parts share: `CharacterGeneration` (a package), `FolderGeneration`, `CharacterController`, `CharacterBackend`, `CharacterCommand` (the backend's part of a command buffer), `GpuMesh`, `Identity`, the counters and stages. Shared helpers are in the namespace `character_internal`; a class declares its member functions here and each is defined in the part of its concern. |
| `character_backend.cpp` | The backend of a device: the configured folders and packages, device setup, creation and stop, the pipeline cache file, a pipeline retired, a pass ended, the submission (`character_submit`), the presentation (`character_frame`), the module's table. |
| `character_backend_index.cpp` | The device-wide index of sections, the hash of a draw's index region, `character_record` (the draw handed to the package it matches). |
| `character_backend_generation.cpp` | A package's life: load (`init`, `init_adopted`), activation, the worker with the mesh and the snapshot batches, notes, stop, the Present of a generation of its own (`ini_present`, `generation_frame`). |
| `character_backend_mesh.cpp` | A package's mesh on the device (`GpuMesh::create`, `copy`). |
| `character_backend_identity.cpp` | Identity and bones: the instance entry's range, `identify`, the skeleton's ring and import jobs on the device, a pipeline's bone site and Core facts. |
| `character_backend_materials.cpp` | Materials: the frame gate, the texture queries (`pixel_texture`, `texture_roles`), a fragment shader's rules and derived profiles, `material_draws`, what a retired pipeline leaves (`generation_retire`). |
| `character_backend_record.cpp` | Recording: `record()`, `record_textures()`, the translucent captures, the plan with its owned scope (`plan_draw`) and that work's submission. |
| `character_backend_folder.cpp` | The folder generation: `folder_create`, `folder_frame`, the F10 exchange (`folder_frame_and_exchange`), the settings adopted from the Mods' own files. |
| `character_backend_controller.cpp` | One package's generations (`CharacterController`): its frame, its own F10 exchange outside a folder generation, stop and return; the F10 rescan of configured packages. |
| `character_backend_twins.cpp` | The application's pipelines without the character dither (D-133): `character_bind`, `character_created`, `prepare_twins`. |
| `character_backend_status.cpp` | The status: `character_status`, `generation_status`. |

## The INI runtime

- **The original is the parser and the evaluator.** Syntax, expressions, keys, presets, pools,
  namespaces and section order are its code. A generation (`IniRuntime`) owns one set of the original's
  globals; a thread enters it through `Scope` (its mutex), loads run one after another (`load_mutex`).
- **One generation for the Mods folder (D-131).** Inputs: the installation's d3dx.ini without its
  `[Include]` and `[Loader]`, four Core files (`API.ini`, `MergedSkeleton.ini`,
  `SpatialIdentification.ini`, `Helpers.ini`), then every enabled INI of the Mods folder in the
  original's include order. The native libraries' files join by their key and ShaderRegex sections.
- **Three backends behind the adapter.**

| Backend | Used by | Model |
| --- | --- | --- |
| Owned GPU runtime (`GpuResourceRuntime`, `GpuShaderRuntime`) | Present, Constants, key lists, CustomShader, and owned work inside a replaced draw | D3D11's state machine on owned resources; judged against the original (`check_dx11_judge.py`). |
| Plan path (`DrawPlanAdapter::Runtime::run`, then `CharacterProgram::plan`) | every replaced draw | The lists run against a draw state (`draw_state.h`, D-161): every resource assignment sets a slot or a declaration and refuses nothing; a draw is recorded with the state it sees, and one table decides per slot what the layer gives it (honoured, ignored, degraded with a notice, that draw left out, the draw left to the game). The plan's six kinds of items (bind vertex, index, pixel resource; draw; external; translucent) are what each recorded draw needs bound. |
| Native Core (`CharacterCore`) | the EFMI Core's lists a Mod calls | C++ in place of `RegisterMod`, `InitializeMergedSkeleton`, `Component_ReadConfig`, `MergedSkeleton_DetectBoneDataSource`, `SpatialIdentity_IdentifyComponentInstances`, `MergedSkeleton_AttachComponent`, `Component_DrawInstances`. |

- **Failure units (Phases 0 and 1 of the review).**
  - A command, in a Present, Constants or key list and in a replaced draw's lists, at any depth of called
    lists: it does nothing and is said once (`BridgeCommand::run`, `Runtime::skipped`, D-155, D-162).
  - An operand nothing answers: the value of its row in `operand_table()`, said once (D-163).
  - An owned GPU command: one that fails before recording leaves its scope as it was; one that fails after
    recording drops that scope's work, is not run again, and the next scope starts clean
    (`GpuResourceRuntime::run`, `failed()`).
  - A Mod at load: `CharacterCore::prepare` names every Mod it cannot prepare
    (`CharacterDiscovery::refused`); `discover_folder` leaves those out and loads again, leaves out a second
    INI under one namespace, and searches by halves for an input the folder does not load with.
  - The Core: every name the native handlers use is checked at load (`check_interface`); a missing one
    refuses the folder with a statement that names it.
  - A replaced draw as a whole, only for what the host itself cannot do (`refuse_draw`: a native Core
    command's contract, the draw's bone data, a buffer the Mod's mesh needs and the layer cannot bind, the
    planner's budgets; then the file compiler and the conversion): the lists still run to their end, so
    the Mod's variables are the original's, and the draw stays the game's (D-162).
  - An intercepted shader call of the owned domain (`execute_intercepted`) is one transaction, as before.
- **What is said.** The original's warnings are the generation's diagnostics (logged at load). What the
  layer does not do as the original does goes through `LayerGap` (`layer_gap.h`) to `IniRuntime::gap_count`
  and `gaps_since`, which the host shows; per-Mod gaps found at load are `CharacterDiscovery::gaps`.

## What makes a Mod a Mod (the gates)

1. An enabled INI with a section `[CommandListInitializeMergedSkeleton]` is a character Mod
   (`CharacterProgram::mods_characters`) and has a package. Any other INI is an input: its lists and keys
   run, and its mesh sections are matched as the next gate says.
2. The folder loads (above).
3. Discovery (`CharacterCore::discover_sections`, `discover_contract`): the host's sections are the
   registry's TextureOverrides with a constant `match_index_count`, as the original's draw maps hold them
   (D-173). One of the Mod's namespace whose list assigns a constant component and LOD is a part of the
   character; one that assigns none, and one of another INI of the Mod's folder, is a section without a
   component (its lists run at the draws it matches, and the plan says whether the draw is hidden, drawn
   with the Mod's textures, or left). The skeleton comes from the Mod's initializer and its pools. A LOD
   no line assigned has no remap. Not matched yet: the mesh sections of an INI outside every character
   Mod's folder.
4. Registration: `RegisterMod` compares the Mod's required version with the installed Core's `$version`
   (read from the Core's main.ini), as the original's.
5. Each draw: `record()`.

## The draw path (`CharacterGeneration::record`)

`vkCmdDrawIndexed` hook, then the device-wide index of sections (prefiltered by index count, the index
region hashed with CRC-32C; the sections of the hash in two rounds, those with a component first, and a
draw's lists run once: `character_record`, D-173), then per matched draw: activation of the package on its first match,
identification of the object from the instance entry (set 2 binding 0), skeleton import for the
component (GPU dispatches in front of the recording's batch, D-127), `IniRuntime::draw` (the matched
sections' lists into a plan, with an owned scope for owned work), `CharacterProgram::plan` (the plan
into the package's draws, cached by signature), materials (which fragment images are re-pointed, from the
fragment shader's SPIR-V), pipeline variants (the game's pipeline with the vertex buffer bindings moved
and images added, built off the thread), snapshot arena entries, emission into the game's command
buffer, and the game's bindings put back. A refusal anywhere keeps the game's draw and is said once per
Mod and reason.

## The Present path

`vkQueuePresentKHR` hook, `character_frame`: keys, window and cursor sampled once; the folder generation's
Present under its plan lock (`folder_frame`): an owned scope on the presenting queue, the key lists,
transitions and Present lists, submission ordered before the presentation; settings saved off the
thread; then the F10 exchange (`folder_frame_and_exchange`): a successor loads in the background, its
packages are brought up beside the live ones, and every package is exchanged at one presentation.

## Threads and locks

Recording threads (several), the submitting thread, the presenting thread (possibly a frame-generation
library's), and the layer's own: the generation worker per package, variant builders, texture loaders,
retirers, the watchdog, the pipe.

Order where nested: `character_mx` (shared; exclusive at a module exchange), the controller's mutex,
`mod_mx` (per package), `res_mx` (the tracker's, shared in `record()`), `plan_mx` (the folder generation),
the generation's `Scope`, then component mutexes (texture residency, variant cache, bone merge, pool).
Presenting: `present_mx`, `character_mx` shared, `plan_mx`, `Scope`. No inversion is known.
Open: the first-use waits for textures and variants (up to 4 s each) are taken under `res_mx` and
`mod_mx` (review L-04).

## Lifetime

No single mechanism: command owners released at the game's reset or free of a command buffer (arenas,
variants, meshes, twins), per-queue timeline semaphores for bone work, the texture timeline, presenter
tickets, fences for notices and meshes. A package stops once (`stop_once`); a folder generation retires
off the presenting thread. The variant cache has no eviction (review L-08).

## What the layer assumes of this game

The instance entry at set 2 binding 0 with the position at byte 48 and the bone offsets at byte 80; 512
implicit bones; a vertex shader with one storage buffer; a pipeline layout with at least three sets and
one free; indexed draws in a primary command buffer and a classic render pass; host-mapped index pools;
one graphics queue family. A game update that moves one of them is not yet told from a Mod's fault
(review L-07).

## Verification map

`sandbox/run_all.py --only <group>`: `quick` (text, units, evaluation, the execution matrix, the judge),
`ini`, `owned`, `bones`, `replay`, `textures`, `submission`. The original is the judge of the owned
runtime only (`scripts/check_dx11_judge.py`, `sandbox/judge_cases`); replaced draws are checked by plans
over the installed Mods (`check_mod_states.py`, `check_mod_degradation.py`, `check_mod_containment.py`),
by `docs/ini-execution-matrix.json`, by captured-frame replays and by the live scenes
(`sandbox/runtime_test.py`). The matched-draw judge is the review's P3-1.

## Where to read more

docs/architecture-review-2.1.md (findings and plan), docs/ini-semantics.md (how each INI construct is
handled), docs/folder-generation.md, docs/engine-generations.md, docs/f10-operations.md,
docs/draw-gpu-work.md, docs/texture-residency.md, docs/hot-swap.md, docs/plan.md.
