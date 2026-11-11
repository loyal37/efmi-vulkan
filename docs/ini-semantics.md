# How the layer handles a Mod's INI (D-137)

The user, 2026-10-28, after tangtang Ramune v1.9 drew a broken arm: the INI handling keeps running into
problems, Mods are written in every way, think it through and handle it generally, never per Mod.

## Why problems kept appearing

1. A subset, and refusal outside it. The original's command lists are adapted into a narrow plan
   (vertex, index and pixel-resource bindings, indexed draws; docs/frontend-draw-plans.md): what lies
   outside "fails when reached" and the whole draw keeps the game's mesh. The subset grew from what the
   installed Mods used (the census of docs/ini-language-coverage.md), so a Mod written differently meets
   its edge. On 2026-10-28: about 170 refusal points in the frontend's planning code, 36 in the layer's
   plan conversion, 14 refusal counters in the backend.
2. Failure the other way round. The original contains a failure to its own unit: a resource whose load
   fails stays null and every assignment of it binds nothing (CustomResource::LoadFromFile), a command
   that cannot act does nothing, a warning goes to the overlay, and the draw goes on. The layer refused
   the draw: the user saw a broken model and no message (BUG-076).
3. Checks compared the layer with itself (a Mod alone against the folder generation) in the default
   state, not with the original in every state a player reaches.

## The rule

- The original is the specification, not the installed Mods. Its command language is finite: 28
  command classes in CommandList.h, resource targets of a fixed set of kinds (vertex, index and constant
  buffers, shader resources, unordered access views, render and depth targets, custom resources, swap
  chains, pseudo-resources) per shader stage, with a fixed set of copy options. A Mod's way of writing is
  a combination of these; the combination is executed by the original's own interpreter, which the
  layer runs. Each command is implemented with the D3D11 meaning it has, composable as D3D11 state is.
- Every command ends in one of three ways: implemented; degraded as the original degrades (a missing or
  unloadable file is a null resource; a command that cannot act does nothing; the draw goes on);
  impossible under Vulkan, listed, and degraded visibly. "Refuse the whole draw" is not one of them, nor
  is a budget the original does not have.
- What is degraded is shown, as XXMI shows it: the overlay's warnings at load, at F10 and when they first
  happen, naming the Mod and the section; and the log.
- General: no Mod name, hash or file name in the code. Tests are generated from the language and run over
  every installed Mod in every state its keys reach.

## Order of work

1. Every refusal point classified against the original: where the original degrades, degrade the same way
   and warn. The overlay notices (load, F10, warnings).
2. Checks with the original as the judge: every installed Mod in every state its keys and LODs reach,
   planned and compared with what the original executes; and small INIs per command run under DX11 with
   XXMI and under Vulkan with the layer.
3. The gaps those checks show, closed by implementing the commands' D3D11 meaning for replaced draws, the
   way owned work (Present, CustomShader) already does, not by special cases.

## The overlay's notices (2.1.2)

As 3DMigoto's LogOverlay (Overlay.cpp): notices drawn at the top left of the presented image, a line each
in the level's colour (DIRE red 20 s, WARNING orange-red 10 s, NOTICE orange 5 s, INFO lime green 2 s,
the time from the first showing), on a black backing of 75 %, at most ten, in Arial (3DMigoto's
notifications font) drawn by GDI, so a Mod's name shows in any script. `layer/src/notices.cpp` draws them
with a render pass of its own on the presenting queue after the presentation's other work (the Mods'
Present work), the presentation then waiting for it; three slots, never a wait for the GPU in the
presentation (BUG-062): with all three in use the notices wait a presentation. The notices are kept by
util.cpp (`notice()`, also the module's through HostApi).

- F10: `> Reloaded config in N.NNNs`, as the original after ReloadConfig (IniHandler.cpp), timed from the
  key to the exchange of the folder generation.
- A refused F10 (the layer's; the original always loads what it can): `> Reload failed, the Mods loaded
  before stay: ...` as a warning.
- What the layer cannot do as the original does (a package's refusal reason): once per Mod and reason for
  the process, as a warning naming the Mod. These are the layer's gaps, shown whatever `show_warnings`
  says; the original's own warnings would follow `show_warnings` (EFMI's d3dx.ini: 0), and none is routed
  to the overlay yet.

Checks: owned_present_test (a notice drawn into presented images, LimeGreen text at the top left and
none in the middle, clean synchronization validation), runtime_test (an F10 that reloaded queues the
original's text).

## Every installed Mod in every key state (check_mod_states.py)

The folder generation offline (`efmi_vk_character_test --plan-folder-states`), as the original loads the
folder: two presentations for the default state (EFMI's exports register at a Present), then each key
action's keys as the original parsed them (`IniRuntime::key_combos`: the VK codes of its buttons that are
not inverted) held for a presentation and released, up to eight presses or until the variables repeat a
state. The original's own dispatch decides what a key changes (cycle, toggle, hold, conditions such as
`$active0 == 1`); nothing of the key syntax is read by the check. After each hold and each release every
Mod's sections are planned; a refusal fails the check with the first state it appeared in. The offline
stand-ins' limits (no game pipeline: "offline ...") are not counted.

2026-10-28 on the installed folder: 316 key actions, 2407 states, 18 Mods, every section plans in every
state, 7 s. With BUG-076's fix taken out it reports tangtang Ramune's component 0 at LOD 0. A run_all step.
The same on copies of the user's ten disabled Mods (without their DISABLED_ prefix, with the installed
libraries; 2026-10-28, after D-139): 179 key actions, 2398 states, every section plans.

## Classification of refusal points (2026-10-28)

About 230 refusal texts in the planning code (frontend draw_plan, resource_plan, vertex_plan, material_plan,
draw_transaction, file_draws; the layer's character_program): 41 internal invariants (an adapter not
installed, a cyclic graph), 39 budgets, 56 constructs the plan path does not take, the rest per-value checks.
Worked through by impact: first what drops a whole Mod at load, then what the original degrades, then budgets,
then the gaps (step 3).

- Done: BUG-076 (a missing texture binds nothing), BUG-077 (a missing, short or twice-declared buffer file
  degrades per draw as the original; the cache of distinct draw lists starts again when full instead of
  refusing; check_mod_degradation.py). The every-state check also runs 1200 presentations after the keys
  (a Present that changes state every frame).

## Sections matched by a resource's description (D-138)

A TextureOverride without `hash` but with match_* keys (match_type, match_width, match_height,
match_format, match_byte_width, match_bind_flags, match_usage, match_mips, match_msaa ...) is a
FuzzyMatchResourceDesc of the original (IniHandler.cpp parse_texture_override_fuzzy_match): it matches
every resource whose D3D11 description meets its predicates, `match_usage` DEFAULT unless given, the
types its keys can apply to (update_types_matched), and its draw-context keys. The original looks them
up wherever it looks up a resource's sections (ResourceHash.cpp find_texture_overrides_for_resource:
the whole-resource hash's sections, then these, each filtered by the draw): a CheckTextureOverride of a
pixel slot (EFMI's Core runs one on ps-t0 to ps-t22 in a Mod's draws), `CheckTextureOverride = ib` (the
Core's ShaderRegexEnableTextureOverrides, every vs_4/vs_5 draw of the game: the region hash's sections,
then the index buffer's description's), and when the game creates a resource (HackerDevice
process_texture_override: format, width, height, their multipliers). The original's own resources
(CustomResource, the back buffer) have no handle info and meet none.

The layer, before: any such section refused the whole Mod ("Fuzzy/image overrides require a
source-image identity provider"). Now:

- A section that can match a 2D texture runs at a CheckTextureOverride of a pixel slot, against the
  description of the game's image the layer already makes for its texture hash (D-077: Vulkan format to
  DXGI, usage DEFAULT, bind flags from the image's usage, TEXTURECUBE for cube-compatible images; the
  hash from it matched DX11's on 345 of 347 textures, the two others a cube map and a mip chain the
  game uploads shorter under Vulkan). The original's matcher and order (find_texture_overrides_for_desc
  after the hash's sections) run unchanged in the frontend (`described_resource_matches`). An image
  the layer cannot describe (1D, 3D, a format without a DXGI counterpart) meets none. A Mod's own
  resource meets none, as in the original.
- A section that can match a buffer would also run at the game's draws by their index buffer's
  description. The layer finds the game's draws by their index region's hash (D-097) and has no D3D11
  description of the game's buffers (Unity's Vulkan buffers are not shown to be the DX11 build's): it
  matches none of them. One for 1D or 3D textures likewise.
- A section that changes how the game creates, maps or copies a resource (format, width, height,
  width_multiply, height_multiply, a buffer size with allow_buffer_resize, deny_cpu_read,
  expand_region_copy), matched by hash or description: the layer leaves the game's resources as the game
  made them.

The last two are the layer's gaps: the section is inert, the rest of the Mod loads, and the Mod is told
(CharacterDiscovery::gaps, the original's warning in the log, the first three per Mod as overlay notices
naming the section). No installed Mod has such a section (census 2026-10-28: no match_* key and no
creation-time key in any enabled TextureOverride). Checks: texture_query_test (the original's parse of
a Texture2D and a Buffer section; a described texture meets the Texture2D one after its hash sections,
pre lists before post lists; another description, an undescribed texture and the generation's own
resource meet none) and check_mod_degradation's two variants (a section by a texture's description
loads and plans in every state; one by a buffer's description and a creation-time key load and are
reported), both refused before.

## Image files (D-139)

The original (CustomResource::LoadFromFile) takes a file named .dds with DirectXTK's DDS loader and every
other file with its WIC loader (CreateWICTextureFromFileEx: frame 0, its WIC-to-DXGI table, the sRGB rule
of the container: PNG's sRGB chunk, another's System.Image.ColorSpace 1; one mip). A file either loader
makes nothing of leaves the resource null, with an overlay warning, and every assignment of it binds
nothing; the same for a buffer file it cannot open or read (LoadBufferFromFile).

The layer, before: a replaced draw that bound anything but a .dds refused ("Pixel file binding requires
an unmodified DDS image", the game's mesh drawn); a texture file present but broken refused the draw when
its load failed (C_IMAGE, the game's mesh) and refused an F10 candidate (LIMIT-043); owned work (Present,
CustomShader) took PNG only and refused the Present for any file it could not load. Now:

- `assets::decode_wic` is the PNG decoder made general: any WIC container, frame 0, the same table, the
  container's sRGB rule. Owned work and the replaced draws' loader (ResourceCache, both its own staging
  and the staging arena) take every file not named .dds through it.
- The character program checks at load whether each texture file loads (CharacterProgram image_loads:
  a .dds by its header, another by WIC without decoding the pixels, within the loader's 128 MiB). One
  that does not is the original's null resource: what binds it binds nothing, as for a file not there
  (BUG-076), and the Mod is told (its gaps: log, notice). Such a file is never declared to the residency,
  so it no longer refuses an F10 candidate.
- Owned work: a texture or buffer file the loader makes nothing of is null with the original's warning;
  the Present goes on.

Checks: resource_png_cases (BMP, JPEG, JPEG with forced sRGB and TIFF fixtures, written by WIC's own
encoder, decode to the bytes and format the pinned DirectXTK loader gives on WARP; a broken file fails
there and binds nothing in the owned Present, which goes on), resource_image_ini_cases (a missing PNG and
a truncated DDS bind nothing, the Present goes on; were refusals), asset_gpu wic_tests (a PNG through
both of the loader's paths arrives as decoded; a broken file fails), check_mod_degradation (a bound
texture as a PNG: the Mod's sections refused before, plan now; a broken DDS is reported and binds
nothing), and a vk04 replay of SiModGB's copy with one texture a PNG and one a broken DDS (every matched
draw planned, validation clean). The live INI scene's F10 with its texture file broken was refused
before (the candidate dropped, LIMIT-043); now it goes through as the original's, the pixel reads
the null texture's zero, and an F10 after the file is repaired draws it again; a prepared package's own
images are checked the same way and one that does not load is never declared.

## A texture declaration's keys (D-140)

What the original does with a [Resource] that a replaced draw binds at a pixel slot and that names a file:
LoadFromFile passes the declaration's bind and misc flags and its colour space (the DDS loader's
forceSRGB for `color_space = sRGB`; WIC's FORCE_SRGB or IGNORE_SRGB) and nothing of its description:
width, height, mips, array, msaa, the multipliers and the stride do nothing to a texture DirectXTK makes
from a file. The binding's view takes the declaration's format (CreateCompatibleView: OverrideOutOfBandInfo,
FillOutTex2DDesc): a typed texture's view must have its format, or CreateShaderResourceView fails and the
slot binds nothing; without a format the view is the texture's own. Bind flags without shader_resource
give no view either.

The layer refused every one of these keys ("Pixel file binding requires an unmodified image", the game's
mesh drawn). Now the plan carries the colour space as loader flags, the view format and whether the view
can be made (FileResource); an image is keyed by its file and its loader flags, so a file declared in two
colour spaces is two textures, as in the original; the loaders apply the flags (DDS: the sRGB twin of its
format, DirectXTK's MakeSRGB; WIC: the decoder's flags); the size keys are ignored; and a view format the
loaded texture does not have binds nothing.

Checks: check_mod_degradation's two variants (a bound texture with `color_space = sRGB` and `width = 64`;
one with `format = R32_FLOAT`), 6 sections refused each on the build before, planned now; asset_gpu (one
BC7 file declared plain and sRGB loads as two textures, BC7_UNORM and BC7_SRGB, the blocks as the file
holds them); resource_plan_test (an sRGB declaration was a refusal case; it reaches the loader as its flag).

## The character contract's sizes (D-141)

The original Core has no limit of its own on a character: the Mod sets `$component_count` and
`$bones_count`, and its pools and the merged skeleton are sized by them (MergedSkeleton.ini, the Mod's
[Pool_MergedSkeleton_Component_*] `pool_size = $component_count`). The layer refused a Mod with more than
32 components (a 32-bit mask per stream and per list), 256 mesh sections, 256 mesh files, or (in a
prepared package) 4096 bones. Now a component is a bit of a 64-bit mask (the stream's, the list's, the
identity's components met), and the bounds are the widths of the layer's own indices: 64 components,
65535 bones (16-bit indices after the blend-index edit; the merged skeleton takes 65536), 4096 mesh
sections and files, 4096 Mods in a folder generation. The installed and disabled Mods reach 17 components
and 865 bones. Check: character_test (a package of 40 components loads and its list requires the 40th's
bones; refused on the build before). LOD 2 and 3 sections stay the game's: the user, 2026-10-28, does not
want them taken.

## The draw commands of a replaced draw (D-142)

The original's DrawCommand::run issues DrawIndexed, DrawIndexedInstanced, Draw and DrawInstanced with
their arguments; `draw = from_caller` the matched draw's own call again (an indexed or a non-indexed one,
instanced or not); `draw = auto` Draw(vb0's whole elements from its offset, 0); `drawindexed = auto`
DrawIndexed(the index buffer's whole elements, 0, 0); `drawindexedinstanced = auto` that with the caller's
instance count and first instance; and a count it cannot determine draws nothing. The layer planned
DrawIndexed and (planned, then refused) DrawIndexedInstanced only.

Now the plan carries the instance count and first instance, whether the draw is indexed, and whether
its count is the bound buffer's; the program takes them into the draw (a non-indexed draw without the
index stream, its vertices from `first`; an automatic count from the bound stream's whole elements of
the file, an owned buffer's from its size) and the emitter draws a non-indexed one with vkCmdDraw.
dispatch, the indirect draws and drawauto inside a replaced draw are what remains of DrawCommand (the
first runs the game's last compute shader, which a render pass cannot; the others read their arguments
from GPU buffers).

Checks: check_mod_degradation's four variants (a section's first draw as two instances, `draw = 3, 0`,
`draw = from_caller`, `drawindexed = auto`), each refused on the build before, planned now; a vk04 replay
of SiModGB's copy with three draws made two instances, non-indexed and automatic (validation clean, every
matched draw planned).

## Slot ranges in a replaced draw (D-143)

`ps-t[a:b] = X` binds every slot of the range: to none, to one resource every slot takes, or to the
elements of a pool range of the same size (another size: a warning and nothing); `unless_null` keeps a
slot whose source is none; `Pool[a:b] = ref ps-t[c:d]` fetches the slots into the pool's elements
(SlotRangeCopyOperation::run, RunBind, RunFetch). The original runs a single slot operation per slot for
anything but a plain reference (SlotOp). The plan refused every range. Now it resolves the range with the
original's own code (FrontendSlotRange beside the pinned CommandList.cpp: ResolveRange, pool_element) and
runs one ResourceCopyOperation per slot through the plan, as `ps-tN = ...` goes. A pool element nothing
assigned or wrote, of an untyped pool, binds nothing, as the original's has no resource.

Checks: check_mod_degradation's three range variants (to none, to one texture, from a pool range), refused
on the build before ("Unsupported frontend operation"), planned now.

## CheckTextureOverride of other targets in a replaced draw (D-144)

The original looks up a resource's sections wherever the target is (find_texture_overrides_for_resource,
the region path for an index or vertex buffer). The plan answered pixel slots only and refused any other
target. Now: a Mod's own resource (a declaration, or one the plan bound to the slot) has no handle info and
meets nothing, as in the original; an empty slot is not bound; the matched draw's own index buffer meets
the sections of the region hash the draw was matched by (region_hash_matches: the original's region path
with track_region_hashes, else the hash map the generation matched the draw through);
the game's vertex buffers and the other stages' resources have no DX11 identity the layer knows (their
Vulkan buffers are not shown to be the DX11 build's), so they meet nothing and the original's warning
channel says so once per target.

Checks: check_mod_degradation's two variants (`checktextureoverride = ib` and `= vb0` before a section's
first draw), refused on the build before ("Texture query requires an adapted single pixel slot"),
planned now.

## A view D3D11 does not make, in owned work (D-145)

CreateCompatibleView returns null where D3D11 makes no view: a typed texture under another format, a
typeless one without a format (image_source_reference checks both on WARP); SetResource then binds the
null view and the list goes on. Owned work (Present, CustomShader) refused the whole Present there. Now
the slot binds nothing. Checks: resource_image_ini_cases's three such cases were refusals (the test
asserted it, passing on the build before); they bind nothing now and the Present goes on.

## Commands a replaced draw does not issue (D-146)

`dispatch` runs the game's last compute shader in the original, which a Vulkan render pass cannot hold;
`drawauto` draws stream output, which the layer does not make; the indirect draws read their arguments
from a GPU buffer, which a replaced draw's plan does not take. Each refused the whole draw; now the
command does nothing, the original's warning channel says so once, and the draw's other commands go on.
Check: check_mod_degradation's `dispatch = 1, 1, 1` variant (refused before, planned now).

## store and clear in a replaced draw (D-147)

The original's `store = $v, Resource, i` maps the resource at once (a staging copy and a Map that waits
for the GPU) and the value is there for the list's next command; `clear` clears a resource's view. In a
replaced draw the layer's owned work is recorded while the game records its own command buffer and is
submitted before the game's submission: nothing of it has run when the list goes on, and waiting for the
GPU on the game's recording thread is what froze the game before (BUG-062). So, as the user prefers no
freezes to the exact frame: a store there copies its value into a host buffer with that submission and
takes, at a later execution of the same store, the value of the newest one completed (the value of a
frame or so before); before the first, the variable keeps its value, as the original's does for a null
source. `clear` of the Mod's own resources runs as owned work there, as its copies do. Both reach only
the Mod's resources (the game's are not owned work's). A generation without the owned GPU runtime
(offline) keeps the variable and clears nothing, with the warning once.

Check: live INI rendering's fixture Mod stores `ResourceStoreSource` (data 7) in its sections: refused
on the build before (the whole character stayed the game's), `$stored` is 7 now in every scene.

## Draws past their files (D-148)

D3D11's input assembler reads index 0 past an index buffer and zeros past a vertex buffer. The layer
refused a list whose draw reached past its index file, or whose index file (all of it) reached past a
vertex file. Now a draw keeps its whole triangles inside the index file (the ones past it are one vertex
three times in the original and show nothing; only a triangle across the end differs); a draw's own index
range is bounded by per-64-index block extrema and read exactly from the file only when that bound reaches
past a vertex stream (no false refusal for another draw's indices); a draw that does read past a vertex
file shows vertices of zeros in the original, which the layer leaves out, the list's other draws going on,
logged once.

Checks: check_mod_degradation's variants (a draw's count far past its index file; a position file cut in
half), refused before, planned now; a vk04 replay of SiModGB's copy changed that way (validation clean,
the draw reading past its vertex file left out).

## DDS arrays and cubes in a replaced draw (D-149)

DirectXTK's DDS loader makes an array or a cube texture of such a file. The layer can re-point only a
fragment shader's nonarray 2D images to the Mod's (fragment_images.cpp), and D3D11 leaves undefined what
a 2D slot reads from a view of another dimension (drivers commonly read the first slice). The layer's
loader refused these files (the slot bound nothing since D-139). Now a DDS array or cube (every face; DX10
arrays of either) loads as its first layer, the levels the 2D slot takes; a volume still does not load.
Check: asset_gpu (a two-slice array and a cube, on the worker's own staging and through the arena, load
and read back as their first layer; failing before, 297/297 now).

## Sections matching the game's buffers by description (finding, 2026-10-28)

The DX11 catalogue capture (char01) shows the game's index data in shared pools: five 16 MiB DEFAULT
buffers bound as vertex and index buffers (and one 4 MiB DYNAMIC); a draw's index and vertex offsets are
the same under Vulkan (draw 130: index offset 44128, vertex offsets 13664 and 35424 in both), so Unity's
pooling is the API's. A section matching buffers by description (match_type = Buffer, match_byte_width
...) therefore meets nearly every draw of the game in the original, and the layer would have to run the
INI generation on every draw to follow it. The Vulkan captures do not record buffer sizes, so the pools'
descriptions are not shown equal. These sections stay inert and reported per Mod (D-138); no installed or
disabled Mod has one.

## The original as the judge (D-150)

D-137's step 2: small INIs run under the original itself and under the layer, and what they leave is
compared. scripts/check_dx11_judge.py (a run_all step) extracts the pinned XXMI 3DMigoto (d3d11.dll
1.2.2, the version of the user's EFMI installation, from F:\vulkan\XXMI-PACKAGE-v1.2.2.zip) into
build/dx11-judge/xxmi with a d3dx.ini of its own whose [Loader] names the judge's D3D11 program
(sandbox/dx11_judge.cpp: a hidden window, a swap chain made through the original, frames presented); the
user's installation is never touched. Each case of sandbox/judge_cases runs its commands in [Present]
and puts results in `global persist` variables, which the original saves to d3dx_user.ini (an assignment
must mark them: a store alone does not); the same case runs under the owned GPU runtime
(resource_gpu.exe --present-ini) and its variables are compared.

Cases: expressions; buffers (copy, reference, Region copy, store in and past range, ->size); compute (a
CustomShader dispatch writing a RWBuffer, store; a clear of a buffer without the UAV bind flag); queries
(->width/height/mips/array of a Texture2D declared without `array` and of one with it, bb->width, a
structured buffer's stride, `=== null`).

What it found, both refusals of the layer where the original degrades, both fixed:
- A Texture2D declared without `array` (and other descriptions D3D11 does not create): the original's
  SubstantiateTexture leaves the resource null with a notice, queries give -2 (RESOURCE_NOT_FOUND); the
  layer refused the Present. Now the resource is null and the list goes on (texture and buffer
  substantiation alike).
- A clear of a resource without a view of the cleared kind (a buffer without the UAV bind flag, an image
  without the view's bind flag): the original makes no view and clears nothing; the layer refused. Now
  nothing is cleared, and a view of a kind the bind flags leave out is the original's null view.
resource_image_ini_cases's six cases that asserted those refusals now check the null resource.

## Views D3D11 does not make, and the bind flags of a first use (D-151)

The judge's new cases (images: DDS, WIC PNG with and without alpha, `color_space = sRGB` on both, copies,
clears of a texture without and with render-target and UAV bind flags, a missing file; draw: CustomShader
blend, cull and front, write mask, blend factor, no blend key; flow: if/elif/else, nested conditions,
command lists, IniParams read by a shader, ring pool variables with a negative index, slot ranges of pool
elements; views: typed, structured, raw and reformatted buffer views, a region's view) agree with the
original in 83 more values, after the gaps they found:

- `cs-t[0:1] = ref PoolBuffers[0:1]` with typed buffers in the pool: the original's ViewForSlot makes the
  view with zero stride, offset and size and an UNKNOWN format, which D3D11 refuses for a typed buffer
  (and gives no element for a raw one), so the slots take the null view; only a structured buffer gets one
  element. The layer refused the Present. Now those slots bind nothing.
- CustomResource::Substantiate adds the bind flags of the target a resource is first fetched for
  (GetResource's dst: a reference's destination, a slot range's slots; a copy passes none) unless
  something was assigned to it first. A resource referenced into a pool element took the pool template's
  flags in the original (the image of the flow case binds through the pool there); the layer's load did
  not add them. GpuResourceRuntime::load now takes the destination and adds them once per declaration.
- Every other view D3D11 does not make is the null view rather than a refusal: a single slot reference of
  a resource without the slot's bind flag, a raw view of a buffer without the raw flag, a typeless or
  missing buffer view format, a typed view of a structured buffer, no element in range, an image view
  dimension D3D11 has no view for, a UAV of a multisampled image, and a slot reference of a region outside
  its buffer. A clear through no view clears nothing. A copy of a region outside its buffer stays refused.
- A buffer view's FirstElement can be any element in D3D11 (`->Region(8, 16)` of R32_FLOAT starts at the
  third); a Vulkan texel-buffer view's offset must be a multiple of minTexelBufferOffsetAlignment (16 on
  the test machine) unless texelBufferAlignment is on, which Vulkan 1.3 makes always (the game's version).
  The shader runtime refused. The layer now reads VkPhysicalDeviceTexelBufferAlignmentProperties on a 1.3
  device and aligns as they say for every texel usage of the buffer (a single texel where the device
  allows it); the judge's layer side runs at 1.3, as the game, and fails on any validation message.

Checks: the judge's eight cases (105 values, validation clean at 1.3); resource_gpu's raw-view and
outside-region references bind nothing and the list goes on.

## The application's other bindings put back (D-152)

A replaced draw's plan binds vertex and index buffers and pixel textures; the game's bindings at the
other slots (a stage's t and cb) are its own, in Vulkan terms the layer has no catalogue for (T-030), and
a line that bound one refused the draw. The installed Snowshine corrects its LOD 1 coordinates with
owned work around which it saves the game's vs-t0, nulls it and puts it back (CommandList_CorrectLodCoordinates:
`Resource_CorrectionSavedSkeleton = ref vs-t0`, `vs-t0 = null`, `run = CustomShader...`,
`vs-t0 = ref Resource_CorrectionSavedSkeleton`); under DX11 the null only keeps the shader resource out of
the compute hazard, and the draw that follows reads the game's own vs-t0. The plan now follows that:
a reference of such a slot into a declaration holds a stand-in for the application's binding there (as
application_textures do for pixel slots), and binding the slot records a change unless it is the stand-in
of that very slot, which puts it back. A slot left changed when a draw is planned, when the application's
draw stays after the pre lists, or at the end (the game's later draws read it) refuses as before; owned
work that names a declaration holding such a stand-in refuses as owned work reaching the application's
resources does.

Checks: plan_test (saved, nulled and put back plans the draw; nulled at a draw, moved to another slot,
or left changed at the end refuses).

## A copy of a region past its buffer (D-153)

`ResourceX = copy ResourceY->Region(offset, size)` copies with SpecialCopyBufferRegion: a box from the
offset to the lesser of the end boundary (the size argument) and the offset plus the destination's size,
into the start of a destination made like the source. A box that ends before it starts (an offset at or
past the buffer, an end boundary below the offset) is undefined in D3D11; under the judge the original's
device stops answering after it (every later store, even of the untouched source, reads zero), so it is
no judge case. The layer refused the list. It now copies nothing there and reads no byte past the buffer;
the destination is made as for any copy and reads zero (copy destinations are created zeroed, as D3D11's
are in practice). The defined case is the judge's regions case (the region lands at the start, the copy
keeps the source's size).

Checks: the judge's regions case (5 values); resource_gpu (a region past the buffer and one ending before
its offset copy nothing and the list goes on).

## Freezes (the user's other half, 2026-10-28)

The watchdog's lines in every game log (dist/efmi_vk_layer.log): the presentation stopped inside the
layer's Present work for minutes in 1.9.4 (PID 14500, 285 s) and 1.9.5 (PID 11492, 416 s), BUG-062 and
BUG-063, fixed in 1.9.6 and by D-129's generations with their own engine state; in 2.0.7 (PID 30992,
22 s) Unity's crash handler ran after BUG-067, fixed in 2.0.8. In the 2.0.3 and 2.0.8 sessions, today's
included, no presentation took 1.5 s or more. Most stall lines are threads that did not enter Vulkan for
a while (idle), not freezes. Each freeze the user meets is to be traced from these lines.

## How the files are read (D-154, 2026-10-30)

The parser is the original's, included unchanged (frontend/xxmi/ini_module.cpp); what the layer does
around it is held to the original's code:

- A file is read by the original's own reader (IniHandler.cpp ParseNamespacedIniFile): UTF-8, a byte
  order mark consumed, text mode. A byte sequence that is not UTF-8 ends the stream: the lines before it
  are read, those after it are not. The judge's `encoding` case states it (`$before` 1, `$after` 0; a file
  with a BOM and CRLF line ends reads whole). The layer's own decoder refused such a file whole before.
- The Mods folder is walked as ParseIniFilesRecursive walks it (character_program.cpp enabled_inis): in
  each folder its files whose last four characters are `.ini` as written (a `.INI` is not an INI to the
  original), sorted without regard to case, then its folders so sorted; names starting with DISABLED and
  desktop.ini left out, as EFMI's d3dx.ini excludes them. The character Mods, the libraries, a Mod's
  other INIs and the generation's inputs all come from that one walk; the packages are in that order.
- The installation's d3dx.ini is the original's first file, not namespaced: its sections join every
  generation first, except [Include...] (the layer's inputs stand for it) and [Loader]. So a command list
  it declares is known to Mods (LIMIT-039), [Rendering]'s recursive_include and track_region_hashes are
  EFMI's (1 and 1), and a [Constants], [Present] or [Key] the player wrote there exists.
- No budget on the number of INI files (128 per generation and 64 per Mod folder refused a folder with
  many Mods whole).

Every enabled INI of the folder is an input (D-155), as the original's `include_recursive`: a Mod
that is no character Mod runs its [Present] and keys as owned work, keeps its persistent variables in
mods-folder.ini, and its TextureOverride sections by texture hash act in replaced draws' pixel slot
checks; it acts on no other draw (the layer matches none) and is no package. The native libraries'
files (RabbitFX) still join by their key and ShaderRegex sections only. A copy between declared
resources the plan does not make (`ResourceA = copy ResourceB`, `= copy_desc`, in a [Constants] list or
a draw's) no longer refuses the generation or the draw: the destination stays null, as the original
leaves it when the source is null, the warning names the line once, and the list goes on (the buffer
such a copy should create, which a Mod's compute then fills, is the next item). The Present and
[Constants] lists run as the original runs them, each command on its own: one the planner refuses does
nothing, with its warning once, and the rest run (before, one Mod's `run =
CommandListUnbindAllRenderTargets` in its Present silenced every Mod's Present lines parsed after it);
a replaced draw's lists keep their all-or-nothing plan.

Left (recorded): a folder without any character Mod loads nothing; dormant command lists are compiled
only when reached, so a typo in one is not warned about at load as the original warns; a `condition`
line in a file's preamble is honoured by the parser but not by the character-Mod detection (such a Mod
is left out with a reason, where the original skips it silently).

Checks: runtime_test's Mods folder scene (a Mod in `Zeta` parsed after `fixture` and `other` by its
[Present] order; a `mod.INI` neither a package nor an input, the folder loading without it; a d3dx.ini
beside Mods whose command list the fixture runs without an unrecognised entry; a plain Mod whose
Present runs and whose texture-hash section acts in the fixture's replaced draw; a plain INI with a
`copy_desc` in its post-Constants warned about once and loading otherwise), each failing on the build
before; the judge's `encoding` case.

## A failure stays with its unit (2026-11-10, docs/architecture-review-2.1.md Phase 0)

The original's unit of failure is the line: a command that cannot act does nothing, a warning says so,
and its list, its file and every other Mod go on (IniHandler.cpp:2342, CommandList.cpp:6407-6479). The
layer's units were larger than that in several places; they are now:

- **A command of a Present, Constants or key list**, at every depth of called lists and CustomShaders:
  skipped and said once (`BridgeCommand::run`, `DrawPlanAdapter::Runtime::skipped`), whatever it raised
  (the planner's refusal, or an exception of a guard or of the original's own code, which ended the whole
  Present for every Mod before). The keys' lists and the transitions run under the same rule.
- **An owned GPU command** (`GpuResourceRuntime::run`): one that fails before it recorded any GPU work
  leaves its scope as it was. One that fails after recording part of its work drops that scope's batch
  (`failed()`; the host discards instead of submitting), is not run again in the generation, and the next
  scope starts clean. Before, either failure set a flag nothing cleared: the owned work of the whole
  generation, since D-131 every Mod's panels, CustomShaders and shape-key compute, ended until F10.
  `clear = Resource depth` on a buffer clears nothing, as the original, which asks for a depth-stencil
  view no buffer has (the judge's `failure` case).
- **A CustomShader section without the owned runtime** (its tools not beside the layer, `m2_owned = 0`):
  declared, its runs doing nothing, said once; it refused the whole load before.
- **A Mod at load**: `CharacterCore::prepare` tries every Mod and names those it cannot prepare; the
  folder leaves exactly those out and loads again. A second INI under one namespace is left out with that
  reason (the original keeps the first file's sections and warns about the second's). When every Mod
  loads alone and the folder does not, the input it does not load with is searched for by halves, plain
  INIs first (`CharacterProgram::discover_folder`).
- **The Core**: the names the native handlers bind are checked at load (`check_interface`); a Core
  without one refuses the folder with a statement that names the Core and the missing name.
  `RegisterMod` compares a Mod's required version with the installed Core's `$version` (its main.ini), as
  the original's list does; the layer's constant 1.48 switched off Mods the installed 1.49 runs.
- **A LOD without a remap assignment** has no remap, as the Core's `z0 = @cs-t1 != 0` (BUG-084: the
  installed `lastrite`, which assigns LOD 0 only, was left out whole).

A replaced draw's lists stood or fell together until 2026-11-11; they run each command on its own now
(the next but one section, D-162).

What the layer does not do while a generation runs is said through `LayerGap` (`layer_gap.h`): the
original's warning channel, and `IniRuntime::gap_count` / `gaps_since` for the host.

Checks: `scripts/check_mod_containment.py` (six variants of the smallest installed Mod, each failing
before); resource_gpu's `declined` checks (a declined command leaves the Present running; 17 scopes went
on, 4 were dropped, and the next scope ran); the judge's `failure` case; `check_mod_states.py` on the
installed folder.

## The original as the judge of a replaced draw's lists (D-157, 2026-11-10)

The judge of D-150 runs Present lists only. For the lists of a matched draw the judge's program is the
"game" (sandbox/dx11_judge_scene.h): each frame two indexed quads of known index bytes, drawn with
DrawIndexedInstanced as the game draws its characters; the judge's d3dx.ini has `track_region_hashes = 1`
and a ShaderRegex that runs `CheckTextureOverride = ib` at every draw, as the EFMI Core's
`[ShaderRegexEnableTextureOverrides]`; so the pinned original runs a case's [TextureOverride] sections
by the draws' region hashes. The picture has four places (the game's quads in A and B, a Mod's vertex
files put quads in C and D); the pixel shader returns texel 0 of t0, so a point's colour says which
texture was bound when which mesh was drawn there, and which triangle of the quad a draw covered.

`scripts/check_dx11_judge_draws.py` reads off the original the colour at two points of each place, the
primitives each game draw put through the input assembler (its own and the lists' draws) and the saved
variables; on the layer's side it takes the plan of each draw from the frontend
(`runtime_test.exe --judge-scene`) and works out the picture and the primitives that plan means by
D3D11's rules (an index past its buffer reads 0, a null vertex buffer gives triangles without area, an
empty slot reads zero, the game's draw runs between the pre and the post items unless skipped).

2026-11-10: ten cases as the original's (a replaced mesh, a texture only, a null texture, post commands,
a part of the game's index buffer, a non-indexed draw, `drawindexed = auto`, three sections on one hash
in the order 3 2 1 of match_priority then name, the draw's operands, sections whose draw keys do not
fit). Two known differences, kept as cases that must still differ: `other_stage` (`vs-cb1 = Resource`
left bound: the layer refuses the whole list, the original draws the Mod) and `copy` (`ResourceX = copy
ResourceY` in the list, then `vb0 = ResourceX`: null in a CPU generation, drawn by the original). Not
covered: the conversion of a plan into the package's draws and the backend, and lists calling the Core.

One line at a time (`--sweep`, sandbox/judge_draw_sweep.json): the list of the replaced mesh and, in front
of its draw, one line of each kind a Mod may write (bindings at every kind of slot, copies and references,
conditions on queries, the draw forms, calls, clears). The file records what each line gives against the
original; the step fails when that changes in either direction. 2026-11-10: 50 lines, 35 as the original's;
12 differ in the layer, each with why (a slot of another kind left changed refuses the whole draw, as do
`<slot> = copy Resource`, `unless_null`, one pixel slot bound from another, `ResourceX = ref vb0` and
`o0 = null`; a copy between declared resources stays null); 3 are answered by the character Core's
extension, which this scene's generation has not, and are not judged here.

2026-11-11, with the draw state (the next section): fourteen cases as the original's. The two known
differences agree (`other_stage`: the line is degraded, the game's constant buffer stays, and the case
requires the notice; `copy`: a copy of a file of the Mod holds the file), and two cases are new
(`game_texture_copy`: a copy of the game's texture taken, the slot bound anew, the copy bound back for a
second draw; `post_skip`: `handling = skip` in the post phase leaves the draw). The sweep has 58 lines and
three kinds of record: `same` (49 lines: the plan means the original's picture and primitives and the
layer said nothing), `degraded` (6: the same picture and primitives in this scene, and the layer said a
notice: a constant buffer or a texture of another stage keeps the game's binding; `store`, `clear` and
`dispatch` do nothing in the scene's CPU generation), and `differs`, which names the one measure that may
differ and never a refusal (2 of the layer: `o0 = null` leaves that draw out, so the picture is the
original's and the input assembler's count is not; the game's texture of another pixel slot keeps the
slot's own; 1 the scene does not judge: `vs` as a value, BUG-090). `EFMI_VK_JUDGE_RUNTIME` runs the layer's
side on another build's runtime_test.exe: on the build before, with these records, 4 cases and 23 lines
fail (20 of them refuse the draw or draw another picture).

Two rules of the original the sweep showed, now the plan's:

- The post list of a matched section. The original does not remember what it matched before the draw: the
  list that checks (`CheckTextureOverride = ib`, in both of its phases) looks the index buffer up again in
  its post phase (CheckTextureOverrideCommand::run), and that is the buffer bound by then. A Mod's own
  buffer has no hash and meets no section, so after `ib = ResourceX` the section's `post` lines do not
  run. The plan runs a matched section's post list only when the pre lists bound no index buffer (BUG-086).
- A reference to a pixel slot. `ResourceX = ref ps-tN` holds what the slot holds when the line runs: what
  the lists bound there before, null after their unbind, the game's texture where they bound nothing
  (BUG-087; the plan took the game's texture always).

## A replaced draw's lists against a draw state (D-161, D-162, D-163; docs/architecture-review-2.1.md Phase 1)

Until 2026-11-11 the plan of a replaced draw was a whitelist: a resource assignment was taken only as a
reference of a Mod resource (or null) into a vertex slot, the index buffer or a pixel texture slot, with
special cases for the game's textures and its other slots; any other line, and any operand the host could
not answer, refused the draw, and the game's mesh stayed (the review's R-01).

The original (CommandList.cpp, pinned): a resource assignment reads its source off the D3D11 context or
a custom resource (`ResourceCopyTarget::GetResource`) and sets its destination on the context or the
custom resource (`SetResource`), for every kind of slot of every stage. A null source sets null, unless
`unless_null` keeps the destination. A slot number out of range sets nothing, with a warning. A draw then
reads the context as it stands. D3D11's runtime gives the original three things for this, which the
layer has to provide itself: a context that holds any binding at any slot whether or not the pipeline
reads it; objects with reference counts, so a custom resource can keep what a slot held; and the one
order of a context's calls.

What the layer does now (`frontend/xxmi/draw_state.h`, `draw_plan.cpp`):

- **The state.** `DrawState` is the context of one execution: a value per slot of the input assembler,
  of every stage's shader resources and constant buffers, of the output merger, the unordered access
  views and stream output. A value is the application's binding of slot S of the matched draw, a resource
  of the Mod, a buffer the draw's owned work wrote, nothing, or a resource the plan does not hold
  (IniParams, the back buffer, a region view). A slot the lists did not set holds the application's
  binding of itself. Declarations hold values too. The game's objects are not the layer's to keep: a
  declaration that took the application's binding holds it for the lists of that draw, and in a later draw
  it holds a resource the plan does not know.
- **An assignment** (`Runtime::assign`) is total: it takes the source's value, honours `unless_null`, and
  sets a slot, a declaration or every element of a pool. `copy` into a slot binds what the source shows.
  `copy` into a declaration without keys of its own holds a file of the Mod (nothing writes it) or, for
  this draw's lists, the game's binding (nothing writes that between the line and the draw); any other
  copy is not made in a plan and leaves null with a notice, and with the owned runtime in the draw's scope
  a copy of the Mod's resources is owned work as before (D-087).
- **A draw** is recorded with the state it sees (`Runtime::issue`). Per slot the lists set, the table
  `honour_table()` decides what the layer gives the draw, from the kind of slot, the kind of value and
  what the host knows of the pipeline (its stages, the vertex slots it reads, its pass's targets):

  | Outcome | Slots and values | What happens |
  | --- | --- | --- |
  | honoured | a vertex slot, the index buffer or a pixel texture slot holding a resource of the Mod, null, or the game's own binding in place | the binding stands before the draw in the plan |
  | ignored | a compute slot; a slot of a stage the pipeline has not; a vertex slot its vertex input does not read; null at a target the pass has not | nothing, and nothing is said: a draw does not read it |
  | degraded | a constant buffer or texture of a graphics stage other than a pixel texture; the game's binding of another slot at a pixel slot; a pixel-stage unordered access view; stream output | the game's binding stays for that draw, one notice per command |
  | not drawable | a render or depth target of the pass changed | that draw alone is left out; the application's own draw is hidden |
  | the game's draw | a vertex or index slot holding the game's binding of another slot or a resource the plan does not hold | the Mod's mesh cannot be drawn with it: the matched draw stays the game's, after the lists ran |

  The plan's items are written there: the bindings that differ from what the items before left bound,
  then the draw. The application's own draw, where it stays, is such a record after the pre lists. A list
  that binds and never draws leaves no item.
- **The lists run to their end** (D-162). A command that cannot act does nothing, a notice names it
  once, and its list goes on, in a called list as in the root. What the host itself cannot do (a native
  Core command's contract, the draw's bone data, the planner's budgets, a buffer for the Mod's mesh) leaves
  the draw to the game, but only after the lists ran, so the Mod's variables are what the original leaves
  (R-02: before, a refused draw left them where the throw happened).
- **Operands** (D-163). An operand nothing answers reads its row of `operand_table()`: the value the
  original itself gives for "nothing known" (-1 for a property it cannot read, 0 for a bound resource or
  shader no section names, -1 for `rt_width` without a target), with a notice once. A question about a
  slot is first a question about what the lists bound there.
- **`handling = skip` after the game's draw** does nothing, as in the original.

Both tables are generated into `docs/ini-execution-matrix.json` (`draw_state`, `operands`); the matrix
check fails when the copy is not the source's.

The conversion of a plan into the package's draws (`CharacterProgram::plan`, the review's P1-5) follows in
two places: a vertex slot the pipeline's vertex input does not read never reaches it (the host tells the
frontend the pipeline's slots, `RuntimeDraw::vertex_slots`; R-08's "Runtime vertex slot outside profile");
and the blend-index widening is the Mod's to make: a list without it has its blend stream read by the
pipeline's own BLENDINDICES element, of the width the game declares (`CharacterDraw::pipeline_blend`;
8-bit indices are read as bone indices of the merged skeleton, so such a Mod reaches its first 256 bones).

Not done of Phase 1 (2026-11-11): the game's texture of another pixel slot bound from the draw's own set
(degraded: the game's texture of the slot stays); input-layout edits beyond the blend-index widening and
the offset edits a Mod's lists name (the layer has the pipeline's Vulkan locations, not the game's DX11
semantics; such a draw still stays the game's); draws of a list without `handling = skip` (the Mod's
draws beside the game's: only its textures for the game's draw are taken). A changed constant buffer or
texture of another stage is degraded, not bound.

Checks: plan_test (67 checks; the lists `copyslot`, `unlessnull`, `slottoslot`, `savedvertex`,
`savedpixel`, `copied`, `targets`, `targetback`, `targetkept`, `compute`, `later`, `movedpixel`,
`postskip` are new, and the checks that asserted a whole refusal state the new rule); the Core pool, native
bridge, texture query and resource planner tests; the INI runtime test (502); the judge of the section
before, whose cases and sweep were run on the build before with the new records and failed there.

## What the offline state check leaves out (docs/architecture-review-2.1.md V-05, P3-4)

`scripts/check_mod_states.py` plans every section of every installed Mod without a GPU and without the
game. Its stand-in for the game cannot answer four things; a refusal for one of them is not counted
against the Mod (`offline_limit` in sandbox/character_test.cpp). Each is answered somewhere else, and
only there:

| Left out offline | What answers it in the layer | Where it is checked |
| --- | --- | --- |
| "needs the owned GPU domain": a list that needs owned GPU work in a replaced draw (a render target's size from the owned side, CustomShader work) | the owned GPU runtime of the folder generation | the owned runtime's steps (resource_gpu, EFMITTL owned transparency, live INI rendering); in the game for a Mod's own lists |
| "Original buffer window unavailable": a query into the game's instance or constant data (`Original_VS_CB->...`) | the renderer's instance entry of the draw | the Core's spatial tests on synthetic entries and the captured frame's replay; in the game for every other character |
| "render targets are not known": `oN`, `rt_width` of the replaced draw | the draw's render pass and target size from the host | the INI runtime test with given targets and the replay; in the game otherwise |
| "offline ...": the pixel shader's declarations (`ps->srv_type`), the game's vertex buffer strides other than the one a section was split by | the draw's SPIR-V and its vertex bindings | the judge and the INI runtime test for the rule; in the game for the values |

Also not run offline: sections matched by a texture's hash. Every pixel slot of the stand-in's draws is
bound without a hash, so `CheckTextureOverride = ps-tN` meets no section there; their lists are planned
only where the host names the game's textures (the live scene and the game). The BUG-051 kind of
failure (nearly every draw refused in the game for a reason the offline check leaves out) is what this
table is for: a refusal of the first kind seen in a game log is not covered by a green offline run.

## Status

- BUG-076 (2.1.1 tree): a texture file that is not there binds nothing, in a Mod's own generation and in
  the folder's alike; was a refusal in the folder generation and a bound missing file alone.
