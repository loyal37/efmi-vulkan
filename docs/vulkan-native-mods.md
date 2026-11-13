# Vulkan-native Mods, first step: ShaderOverride by SPIR-V hash, and hunting

Route 2 of the project (the user, 2026-11-10, restated 2026-11-12): a version of 3DMigoto for this
game's Vulkan renderer, for Mods written against Vulkan itself, beside route 1 (installed DX11-written
Mods unchanged). Its order: the review's Phase 1 (a draw's lists run against a draw state) and Phase 2
(any Mod replaces meshes), both done; then this step, ShaderOverride by SPIR-V hash and hunting; then
replacing game shaders; then slots of other stages (`vs-cb`, `vs-t`, render targets). This document
is about this step only; the last section lists what comes after it.

## 1. What 3DMigoto does (read from `F:\vulkan\XXMI-Libs-Package\DirectX11`)

### ShaderOverride

- **The hash.** A shader is named by a 64-bit hash of its bytecode, computed when the game creates it
  (`CreateVertexShader` and the others in HackerDevice.cpp) and printed as 16 hex digits. A section
  reads it with `GetIniHash` (IniHandler.cpp): `sscanf("%16llx%n")`, the whole value or nothing, so
  `hash = 1234abcd` is the 64-bit number 0x1234abcd.
- **Parsing** (`ParseShaderOverrideSections`): every `[ShaderOverride*]` section goes into
  `G->mShaderOverrideMap[hash]`. Sections with one hash share one entry: the later section's commands
  are appended to the same `command_list` and `post_command_list`, `filter_index` is read again from
  each section (default `FLT_MAX`), and `allow_duplicate_hash` decides only whether the duplicate is
  warned about (all must say `true`, or one `overrule`). Keys of the section itself: `hash`,
  `allow_duplicate_hash`, `depth_filter` and `model` (legacy), `disable_scissor` (becomes a `run` of a
  built-in list), `filter_index`; every other line is a command (`handling = skip`, assignments,
  `run`, `checktextureoverride`, bindings, draws), `pre` by default, `post` with the prefix.
- **ShaderRegex links** (ShaderRegex.cpp `link_command_lists_and_filter_index`): a ShaderRegex group
  that matches a shader appends a `run = linked command list` to that hash's entry, after the
  section's own commands, and sets the entry's `filter_index` only when no section set one. The EFMI
  Core's `[ShaderRegexEnableTextureOverrides]` matches every vertex shader with
  `if $enable_mods: if DRAW_TYPE % 2 == 0: CheckTextureOverride = ib`: that is how TextureOverride
  sections run at all.
- **At a draw** (HackerContext.cpp `BeforeDraw`, `AfterDraw`; every `Draw*` call goes through them):
  hunting first (below), then for the bound vertex, hull, domain, geometry and pixel shader in that
  order: look the hash up, remember the entry's post list, run its pre list (`ProcessShaderOverride`:
  the legacy `depth_filter` decides whether the original shader is swapped back in). The draw goes to
  D3D11 unless `call_info.skip` was set (`handling = skip`, by any list of any stage). `AfterDraw`
  runs the post lists in the same stage order. The vertex stage's entry holds the Core's link, so the
  order of one draw is: the Mod's vertex ShaderOverride commands, the TextureOverride sections of the
  index buffer, the hull, domain and geometry entries, the pixel entry; the same order for the post
  lists.
- **`vs`, `ps` as values** (CommandList.cpp `process_shader_filter`): no shader bound reads -0; a
  bound shader without an entry 0; an entry with `filter_index` that value; an entry without one 1.
  This is how a TextureOverride tells passes apart (`if ps == 203`, the outline pass a ShaderOverride
  marked).
- **`checktextureoverride = ps-t0`** (any slot): runs the TextureOverride sections of the hash of the
  resource bound there, in the same draw. The layer has this already for the texture-hash sections.

What D3D11 does for 3DMigoto here: a draw is one call on one context; a list's bindings stay on that
context until changed; not issuing a draw has no further consequence.

### Hunting (Hunting.cpp, HackerContext.cpp, HackerDXGI.cpp, Overlay.cpp)

- **Settings**: `[Hunting]` of d3dx.ini. `hunting = 0` registers no hunting key (XXMI then keeps it
  "soft disabled", 2, for its ShaderRegex), `1` enabled, `2` soft disabled: the keys are registered and
  `toggle_hunting` switches between 1 and 2. `marking_mode` (`skip`, `original`, `pink`, `mono`; key
  `next_marking_mode`), `marking_actions` (`clipboard`, `hlsl`, `asm`, `regex`, `mono_snapshot`,
  `snapshot_if_pink`; default clipboard and hlsl), `repeat_rate`, and per kind a `next_`, `previous_`
  and `mark_` key: pixel, vertex, compute, geometry, domain, hull shader, index buffer, vertex buffer
  (and its slot), render target; `done_hunting`. The installed d3dx.ini has `hunting = 2`,
  `marking_mode = skip`, `marking_actions = clipboard hlsl asm regex`, numpad keys (toggle on 0, pixel
  shaders 1-3, vertex shaders 4-6, index buffers 7-9, `+` done) and `analyse_frame` on Shift+F11.
- **What is visited**: while hunting is enabled, every `*SetShader` inserts the shader's hash into the
  stage's set (`std::set`, so in ascending hash order); `BeforeDraw` registers the draw's index and
  vertex buffer region hashes (with `track_region_hashes`). The sets are cleared after 60 seconds
  without a hunting key (`TimeoutHuntingBuffers` in the Present hook) and by `done_hunting`.
- **Selection** (`HuntNext`, `HuntPrev`): the next or previous element of the set after the selected
  one, wrapping; the overlay shows `VS:3/15 PS:7/40 ...` (position from 1, set size).
- **At a draw**: if the draw's index buffer, any stage's shader, a vertex buffer or a render target is
  the selected one, the draw is "marked": `marking_mode = skip` sets `call_info.skip` and
  `hunting_skip` (the latter keeps a `draw = from_caller` of a list from drawing it again), `pink`
  swaps in a pink pixel shader; the ShaderOverride lists still run. The marked shader's peers (index
  and vertex buffers drawn with it) are collected for the mark's log.
- **Mark** (`MarkPixelShader` and the others): logs `>>>> pixel shader marked: pixel shader hash = %016I64x`
  and the visited peers, copies the hash to the clipboard (`HashToClipboard`, "copied to clipboard"
  on the overlay), dumps the shader's HLSL or assembly to ShaderFixes, takes screenshots, as
  `marking_actions` says.
- **Index and vertex buffers** (BeforeDraw with `track_region_hashes`, ResourceHash.cpp): the index
  buffer's hash is the region hash of the draw's indices (the TextureOverride hash); a vertex buffer's is
  the CRC-32C of its region from `offset + stride * FirstVertex`, `stride * VertexCount` bytes long, or for
  an indexed draw `(3 * IndexCount + 10) / 20 + 3` vertices from the base vertex (the EFMI fork's
  estimate). `next_vertexbuffer_slot` restricts the vertex buffers to one slot (-1, every slot, first) and
  starts their set again. Render targets are the bound views' resources; their mark logs the resource's
  hash and description (`LogRenderTarget`).
- **`hunting` as a value** (CommandList.cpp): `G->hunting`. XXMI never leaves it at 0 (ParseHuntingSection
  sets 2 for `hunting = 0`), so a list reads 2 while hunting is soft disabled and 1 while enabled; the
  optimiser folds it to 0 only when hunting is disabled outright.
- **Frame analysis** (`AnalyseFrame`, FrameAnalysis.cpp): only while hunting is enabled; the next frame
  is logged into `FrameAnalysis-%Y-%m-%d-%H%M%S` beside the DLL (`log.txt`: per draw, numbered with six
  digits, the calls that set its state with the shaders' hashes, then the draw call), and with
  `analyse_options` the bound resources are dumped as files; a second press aborts; the overlay says
  "Frame analysis saved to ...".

## 2. What Vulkan has of it, and what not

- **Has.** Every module's bytes at `vkCreateShaderModule` (the layer keeps them: `ShaderInfo`, the
  probe's `shader` request); every pipeline's stages and their modules at creation (`PipeInfo::shaders`);
  every draw with the pipeline bound for it (`DrawRec::pipeline`), on whichever thread records it,
  primary or secondary command buffer; not forwarding a draw is legal and changes no other state.
- **Not as in DX11.** Stages are not bound one at a time: a pipeline is a fixed set of modules, and a
  module is in many pipelines; one module's bytes may be created as several `VkShaderModule` objects
  (equal bytes, equal hash). There is no DX11 hash of a game shader (D-089: SPIR-V compiled
  separately, nothing derives one from the other). A shader cannot be switched at a draw without
  another pipeline (the next step, shader replacement, needs pipeline variants as the anti-dither
  twins of D-133 are). Bindings are by (set, binding), not by DX11 slot (a later step).
- **Not in this game.** Shader objects (`VK_EXT_shader_object`) and modules given inline to the
  pipeline (`VkShaderModuleCreateInfo` chained into a stage) were not seen; such a pipeline has no
  module hash here and no ShaderOverride acts on it.

## 3. How a Vulkan-native Mod names a shader

- **The hash is the SPIR-V module's CRC-32C**: of the bytes the application gives
  `vkCreateShaderModule` (`codeSize` of them), printed as 8 lowercase hex digits. It is exactly the
  `hash` the layer already shows: the probe's `shader` and `pipeline` requests (`hash_algorithm:
  crc32c`), the MCP tools `efvk_shader` and `efvk_pipeline`, the shader file names of capture bundles,
  the shader catalogue and slot tables (docs/shader-catalogue.md, keyed by vertex and fragment shader
  hash), the sandbox program's `expected.json`. A Mod writes it as 3DMigoto writes a shader hash:
  `[ShaderOverrideOutline]` `hash = 8a1b2c3d`.
- **Why not a 64-bit hash as 3DMigoto's.** The original's parser reads either width into one 64-bit
  value. A 32-bit value is below 2^32, and a 3DMigoto shader hash (64-bit FNV-1a) is below 2^32 with a
  chance of 2^-32: the value alone tells a Vulkan section from a DX11 one at load, which keeps the
  compatibility route's notice exact (section 5), and the hash every existing tool prints stays the
  one a Mod writes. The cost is the chance of two of the game's modules sharing a CRC-32C: about
  1300 modules were alive at once in the 2.1.3 game session, so a pair among them is about 2 in 10000.
  The layer checks it: a module whose hash an earlier module of other bytes already had is a
  collision, logged and counted in the status (`shader_hash_collisions`), and a ShaderOverride on that
  hash acts on both, as a 3DMigoto texture hash shared by two textures does.

## 4. A ShaderOverride on a SPIR-V hash

- **Loading.** The folder generation parses the ShaderOverride sections whose hash is below 2^32 with
  the original's parser and its rules (merging, `allow_duplicate_hash`, `filter_index`, warnings);
  the others are DX11 hashes and stay as today (section 5). D-191 implements `depth_filter` for native
  replacements: `depth_active` requires a depth attachment in the captured subpass, `depth_inactive`
  requires none. A mismatch uses the original stage; the section's command lists still run. `model`
  remains an offline HLSL compiler option, with a notice for the unused INI key.
- **At every draw** (indexed, non-indexed, indirect) of every pipeline whose stage modules include such
  a hash: the entries of the vertex, tessellation control and evaluation (hull, domain), geometry and
  fragment (pixel) module run as the original's, in its order and with its draw call info (`DRAW_TYPE`
  of the Vulkan call: `DrawIndexedInstanced`, `DrawInstanced`, or their indirect forms):
  1. the vertex entry's pre list;
  2. for a draw a Mod section matched (the device index, D-097, D-173): the library's vertex groups and
     the TextureOverride sections, as now;
  3. the hull, domain and geometry entries' pre lists, then the pixel entry's;
  4. the library's pixel groups, as now;
  5. the game's draw unless a list set `handling = skip`; for a matched draw, the Mod's draws as now;
  6. the post lists in the same order.
- **`vs`, `ps`** read `process_shader_filter`'s values with the module's entry: its `filter_index`, 1
  without one, 0 for a bound module without an entry, -0 for a stage the pipeline has not. A vertex
  module without an entry keeps BUG-090's value (the Core's patternless ShaderRegex).
- **A draw no Mod section matched** runs only the ShaderOverride lists, on the folder's generation,
  under its plan lock as a matched draw's. What its lists can do in this step: variables, conditions,
  keys' state, `run` of command lists, `handling = skip`, `vs` and `ps`. A binding or a draw of the
  lists at such a draw is not done: the game's draw stays as it is (also with `handling = skip`, as
  D-162 leaves a draw the host cannot draw to the game), said once per section. The game's textures at
  such a draw (`checktextureoverride = ps-tN`, texture queries) are not answered there yet: the layer
  answers them from a package's material state, which a draw no Mod section matched has not. Both
  need route 2's later steps (slots by (set, binding)).
- **Cost.** With no acting hash in the generation a draw pays one atomic read. With some: per draw the
  pipeline's module hashes (kept per pipeline), and the lists only where one matches.

## 5. Compatibility with the DX11 route

The installed Mods are DX11-written and are route 1's; this step must not change what any of them
does (the user, 2026-11-12: consider the compatibility with the existing DX11 route). The rules:

- **A DX11-hash section stays exactly as today.** A section whose hash is 2^32 or more is a DX11 hash
  (written for 3DMigoto under DX11: Xaihi's `ps skip.ini`, perlica's hair sections). It is not parsed
  into the generation (as today: its lists are not compiled, so no new warning can come from them),
  and the layer says once at load what it says today ("acts on game shaders by their DX11 hash, which
  the Vulkan layer cannot see: inert"); one that only declares (`hash`, `filter_index`,
  `allow_duplicate_hash`) is said nowhere. Only sections with a SPIR-V hash are parsed, with the
  original's parser and its rules among themselves.
- **Nothing else changes for a folder without a SPIR-V-hash section**, which is the installed folder
  today: no list runs at a draw that did not run before, `vs` and `ps` read what they read (0, BUG-090's
  value for `vs`), the order of a matched draw's lists is the same, and a draw pays one atomic read.
  Checked by the plan digests of every installed Mod in every key state (`scripts/check_mod_states.py`:
  the build before and this one on the same folder) and by the notices of the installed folder in the
  sandbox (Xaihi's two lines and nothing new).
- **Both in one Mod, or one Mod for each.** A DX11 Mod's TextureOverride that tells passes by a
  ShaderOverride's `filter_index` (`if ps == 203`, its outline section by DX11 hash) reads 0 under
  Vulkan today. A section with the Vulkan module's hash and the same `filter_index`, in the Mod or in an
  INI of its own beside it, makes the same condition true at the same draws: the bridge from a DX11 Mod
  to its Vulkan port is a few lines, not a rewrite. A DX11-hash and a SPIR-V-hash section in one Mod
  never meet, so neither changes what the other does.
- **Hunting keys.** The installed d3dx.ini registers its numpad keys under XXMI's DX11 (`hunting = 2`);
  under the layer they now do what they do there (toggle on numpad 0; the player under DX11 has the
  same keys). With hunting soft disabled, as installed, no draw is skipped and nothing is visited until
  the toggle; `hunting = 0` in d3dx.ini switches them off for both routes.
- **The compatibility route's own uses of shaders are untouched**: EFMITTL's groups and RabbitFX's gates
  (D-171, D-133) stay native and keep their places in a matched draw's order (section 4, steps 2 and 4).

## 6. Hunting in the layer

- **Settings and keys** are the original's: the installation's d3dx.ini (its own sections join the
  folder generation first, D-154), its `[Hunting]` section, its key strings through the original's
  `RegisterIniKeyBinding` and input dispatcher, so modifiers, `NO_` prefixes and `repeat_rate` mean
  what they mean under XXMI. `hunting = 0` registers nothing. A key's action is queued by the
  generation and carried out by the layer after the presentation it was dispatched in. They are the host's keys, not a Mod's: the checks that press every Mod key
  (`scripts/check_mod_states.py`) leave them out.
- **State** is the layer's, for the process (an F10 reloads `hunting`, `marking_mode`,
  `marking_actions` from the file and keeps the selection, as the original's globals do): the mode
  (1 enabled, 2 soft disabled), the visited sets, the selection per kind, the time of the last key.
- **Visited**: while enabled, every draw's vertex and fragment module hashes (ascending order, as
  `std::set`); cleared after 60 seconds without a hunting key and by `done_hunting`.
- **Selection and skip**: `next_`/`previous_` vertex and pixel shader as `HuntNext`/`HuntPrev`; with
  `marking_mode = skip` every draw whose pipeline has the selected module is not forwarded (a draw a Mod
  replaced keeps the Mod's draws, as `hunting_skip` keeps a list's own draws under the original).
  D-192 adds `original` (restore only the selected VS/PS stage, including its native replacement and
  anti-dither patch) and `pink` (the selected draw's pixel output is float4(1,0,1,1) at location 0).
  Pink supports shader, buffer and target selections, including ordinary Mod mesh variants; a custom
  shader pass such as a translucency capture keeps its own shader. The host restores the previous
  pipeline after each direct/indirect draw, and recording owners retain every temporary twin.
  An incompatible pink output interface leaves the original shader and reports why. `mono` has no
  stereo separation to change in this renderer and is reported as such.
- **Mark**: the original's log lines with the 8-digit hash (`>>>> pixel shader marked: pixel shader
  hash = 8a1b2c3d`, `>>>> Index buffer marked: index buffer hash = ...`, `>>>> Vertex buffer marked:
  ...`, `>>>> Render target marked: ...` with the image's format and size, then the visited peer modules),
  a notice on the overlay, and the hash on the clipboard when `marking_actions` has `clipboard` (the
  player's own clipboard, as XXMI). With `hlsl`, `asm` or `regex` (the original's dump actions) a marked
  module's SPIR-V is written as the game created it, `<hash>-vs.spv` or `<hash>-ps.spv`, into
  `ShaderFixes` in the layer's settings folder (never the installation's, which is the DX11 route's): the
  file to start a replacement from (section 7).
- **Index and vertex buffers, render targets** (D-189): while enabled every indexed draw's index region
  hash, its vertex buffers' region hashes by the original's rule (above; both read from the application's
  mapping or the layer's shadow copy, as the original reads its own copy of a buffer) and its colour
  targets' images (known where the draw
  is recorded inside its render pass instance; draws in secondary command buffers have none) are
  visited; `next_`/`previous_`/`mark_` index buffer, vertex buffer (and `next_`/`previous_vertexbuffer_slot`)
  and render target select and mark as the shaders do, and a draw with a selected one is skipped.
- **`hunting` as a value** (D-189): the generation's `G->hunting` is the layer's mode (2 where no key is
  registered or while soft disabled, 1 enabled), set before the optimiser and at every presentation.
  No installed Mod reads it.
- **Frame analysis, the log** (D-189): `analyse_frame` while enabled records the draws between the next
  two presentations into `FrameAnalysis-<time>/log.txt` in the layer's settings folder: per draw its
  number, command buffer and pipeline, `VSSetShader hash=...` and the other stages, the index region
  hash, the vertex buffers' region hashes, the colour targets, then the call as the original names it
  (`DrawIndexedInstanced(...)`, `DrawInstanced(...)`, the indirect forms); a notice says where. Dumping
  the resources (`analyse_options` dump_rt, dump_tex, dump_cb, dump_vb, dump_ib) needs reading the game's
  images and buffers back after the frame: not yet.
- **What the player sees**: a notice per key with the selection (`PS 3/40: 8a1b2c3d`, `IB 2/5: ...`,
  `RT 1/1: image 7`) in place of the overlay's line. The status has a `hunting` object (mode, marking
  mode, the visited counts and selections of the five kinds, the vertex buffer slot, draws skipped,
  marks, dumps, frame analyses).
- **Not yet**: compute hunting, dumping resources in a frame analysis, the overlay's steady line.

## 7. What needs the game

Everything above is checked in the sandbox (`sandbox/runtime_test.py`: the sandbox program's own
modules, its five draws on two recording threads). The game adds: how many draws a ShaderOverride
meets per frame and what its lists cost there; hunting over the game's thousand-odd modules (the
visited sets, the order a player cycles through them, the frame time while enabled); the clipboard and
notices while playing; whether any game pipeline has no module hash (inline modules).

## 8. What comes after this step

1. Replacing game shaders: D-190 implements the SPIR-V files and an offline HLSL compiler (section 9).
   D-191 adds `depth_filter`; D-192 adds `pink` and `original` marking;
   the compiler's explicit `--model` chooses the native HLSL target, not an INI `model` directive.
2. Slots of other stages and render targets by (set, binding) for Vulkan-native lists, and the
   bindings and draws of a ShaderOverride's lists at any draw.
3. Hunting of compute shaders (dispatches hooked); frame analysis's resource dumps. (Index and vertex
   buffers, render targets, the `hunting` value, dumping a marked module and frame analysis's log: D-189.)

## 9. Native game shader replacement (D-190)

The layer reads `<hash>-<stage>_replace.spv` from `ShaderFixes` inside its settings folder. `hash` is
the original eight-digit CRC-32C from hunting; `stage` is `vs`, `hs`, `ds`, `gs` or `ps`. Hunting's
`<hash>-ps.spv` dumps do not act until explicitly made replacement files. The DX11 installation's
ShaderFixes folder is not read or changed. A native shader keeps the game's entry point, descriptor
set/binding interface, push constants, and stage inputs/outputs. It can stop using inputs/descriptors,
but cannot add resources or widen declared access. Vertex and other producer stages must keep outputs
needed by the next stage, including built-ins. Changed resource bindings belong to the next route-2 step.

An HLSL source can be compiled offline, using the tools installed in `F:/vulkan`:

```powershell
python scripts/compile_game_shader.py shader.hlsl --hash 12345678 --stage ps --output-dir dist/settings/ShaderFixes
```

Replace the example hash with the hunted module's hash. Use explicit `[[vk::binding(binding, set)]]`
and `[[vk::location(location)]]` declarations; this compiler does not apply the compatibility route's
DX11 register shifts. `--entry` defaults to `main`, `--model` to `6_0`, and `--target` to `vulkan1.1`.
The compiler validates its temporary SPIR-V before replacing the destination atomically; compilation
or validation failure leaves the last good file intact. This command only creates a file. F10 loads
it into an already running layer; real-game loading still requires the project's authorization rules.

Files load with the folder generation, initially and on F10. The folder worker validates SPIR-V for
the application's Vulkan version and the enabled features supported by the owned shader runtime. A
known pipeline's interface is checked there too; new pipelines are checked on the variant worker.
An invalid reload keeps the last good module and reports why; deletion restores the game shader.
Unchanged code keeps the same revision and cached variants. An interface mismatch first encountered
on a new pipeline keeps that pipeline's original stage. A refused native stage does not suppress a
character Mod's mesh. ShaderOverride matching and hunting always use the original hash.

A shader can satisfy Vulkan's interface but remove a pattern the compatibility renderer needs for
its bone or image rewrite. If such a replacement prevents a character variant from being built,
that variant retries with the game's original stages and records the reason. This preserves the Mod
mesh; it does not claim that every authored shader can compose with the compatibility rewrites.

Variants are prepared from owned graphics recipes when game pipelines are created or the files reload.
A shader-only variant keeps the application's full layout without adding a private set. Character
variants use the same published shader set before their ordinary vertex/image rewrites. Queued builds
retain a revision snapshot, and recorded command buffers retain their actual pipelines through
completion, including an old revision after F10. Native twins obey the backend's enable switch.

Descriptor layouts and immutable samplers retain the original handles until the application has
requested destruction and all metadata/variant/recording owners are gone. Using the original layout
also avoids a false incompatibility from the pinned validation layer's immutable-sampler pointer
comparison. The original 3DMigoto saves sampler references with `PSGetSamplers`, restores them with
`PSSetSamplers` and then releases them; Vulkan requires this lifetime to be explicit. Native shader
replacement adds no resource upload or write dependency: the replacement must preserve the game's
resource access contract; existing queue order/barriers and the character path's owned-work
synchronization still apply. See the Vulkan specification's
[pipeline layout compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility).

The status's `shader_fixes` reports loaded files, revision, bind count and file-validation warnings.
Per-pipeline interface/build refusals appear in the overlay and log. A file count alone is not proof
that a replacement was drawn. Current boundaries: graphics pipelines captured by the existing recipe
tracker; a single matching entry point; supported core capabilities and fully tracked layouts. Layout
binding flags, unsupported extension state, and immutable samplers/layouts created with custom host
allocators remain refused. An empty Mods folder supports initial ShaderFixes loading and F10 via the
existing host input fallback (verified in D-192). Hunting keys still require a loaded INI generation.
Compute shader replacement and native arbitrary resource/draw lists remain pending. D-191 publishes
depth filters with the INI/shader generation; a filter-only F10 changes the revision and invalidates
cached variants. Depth presence means an attachment, independently of the pipeline's depth-test switch.

Verification lives in `sandbox/native_shader_test.py`; `sandbox/run_all.py --no-build --only native-shaders`
adds tracker lifetime, pipeline/cache, live INI and validation checks. These are sandbox results, not
real-game acceptance.

`sandbox/hunting_shader_test.py` checks VS/PS original selection, pink shader/index/target selections,
clearing, a Mod's own mesh region, non-indexed and indexed-indirect draws, and the inject entry. A
render-target selection identifies one swapchain image, so another presented image need not be pink;
that case checks selected pipeline binds and validation, while the shader/index cases verify pixels.

The routine pipeline step runs deterministic fixtures. Historical captured-shader rewrites are a
separate `sandbox/run_all.py --no-build --only shader-corpus` step, also in the full suite and the
bones group; it retains the capture checks without scanning the entire capture tree for every native
pipeline change. This separation avoids the old 30-second cold-read timeout.
