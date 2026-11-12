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
- **Frame analysis** (`analyse_frame`): dumps every draw's bindings and resources of one frame. Not
  part of this step.

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
  the others are DX11 hashes and stay as today (section 5). The legacy keys `depth_filter` and `model`
  of an acting section do nothing, with a notice (they swap in the original shader, which the next
  step's replacement gives a meaning). The generation hands the layer the set of the acting hashes.
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
  replaced keeps the Mod's draws, as `hunting_skip` keeps a list's own draws under the original). The
  other marking modes need a replaced fragment shader (the next step): until then they select and mark
  but draw as is, said once.
- **Mark**: the original's log lines with the 8-digit hash (`>>>> pixel shader marked: pixel shader
  hash = 8a1b2c3d`, then the visited peer modules), a notice on the overlay, and the hash on the
  clipboard when `marking_actions` has `clipboard` (the player's own clipboard, as XXMI). Dumping the
  module (`hlsl`, `asm`) is a later part: the layer would write the SPIR-V and its disassembly into a
  folder of its own, never into the installation.
- **What the player sees**: a notice per key with the selection (`PS 3/40: 8a1b2c3d`) in place of the
  overlay's line. The status has a `hunting` object (mode, marking mode, the visited counts, the
  selection, how many draws were skipped, marks).
- **Not yet**: index and vertex buffers (their region hashes for every draw while hunting), render
  targets, compute shaders (the layer does not hook dispatches yet), frame analysis, the `hunting`
  operand (reads 0 as before).

## 7. What needs the game

Everything above is checked in the sandbox (`sandbox/runtime_test.py`: the sandbox program's own
modules, its five draws on two recording threads). The game adds: how many draws a ShaderOverride
meets per frame and what its lists cost there; hunting over the game's thousand-odd modules (the
visited sets, the order a player cycles through them, the frame time while enabled); the clipboard and
notices while playing; whether any game pipeline has no module hash (inline modules).

## 8. What comes after this step

1. Replacing game shaders: a Mod's SPIR-V (or HLSL compiled to it) in place of a module by its hash,
   through pipeline variants made when the game creates the pipeline (as the anti-dither twins), and
   with it the `pink` and `original` marking modes, `depth_filter` and `model`.
2. Slots of other stages and render targets by (set, binding) for Vulkan-native lists, and the
   bindings and draws of a ShaderOverride's lists at any draw.
3. Hunting of index and vertex buffers, render targets and compute shaders; frame analysis; dumping a
   marked module.
