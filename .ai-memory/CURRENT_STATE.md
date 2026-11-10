# EFMI Vulkan - Current development state

> Branches, 2026-10-24: main only. codex/efmittl-support was merged and its merge replayed as a linear history (c485787, 188e17d, 192da15, tree identical to the merge) and pushed to origin; the WIP snapshot branches codex/wip-2026-10-15 and codex/wip-on-main are archived as bundles and patches in build/backups/wip-branches-20261024/ and deleted. Earlier: codex/efmittl-support is merged into main, so main's code is the deployed candidate 1.7.7. claude/ini and codex/performance-1.7.5 were already in main and are deleted; their worktrees removed (the ini worktree's superseded uncommitted 1.7.5 attempt is kept as a patch in build/backups/branch-cleanup-20261024-214325/). Main's 21 older WIP files stay uncommitted in the main tree, as before.

> Active request: 2026-10-24, inspect and adapt the installed EFMITTL plugin. Implementation 776e22e is on codex/efmittl-support in build/performance-worktree; main's 21 older WIP files are preserved. Candidate 1.7.7 was deployed at 21:36 after the game and launcher had closed; the actual-config sandbox smoke passed. No game load, launcher change or module exchange was performed.

> Launcher, 2026-10-29 22:24: the EFMI profile switched back to DX11 at the user's request (d3d11_mode_cmd_args = -force-d3d11, xxmi_dll_inject_mode = DIRECT; game and launcher closed, checked twice; only the two value spans changed, every other JSON value unchanged, exact readback; backup and manifest in build/backups/launcher-dx11-20261029-222441/). The injected library (signed) is still F:\efmi vulkan\dist\efmi_vk_inject.dll; the user sets it to F:\EFMI MCP\dist\efmi_probe.dll in the launcher UI before a DX11 start (a DX11 start with efmi_vk_inject.dll injected crashed on 2026-10-11). Before that, 2026-10-25 15:50: switched to Vulkan (build/backups/launcher-vulkan-20261025-155010/).

## Architecture review (2026-11-10, docs/architecture-review-2.1.md)

- The user asked for a review of the whole project, the INI interpreter above all, and a plan. Review of c69c1b6 by reading; nothing built, deployed or loaded into the game; one check run. No code changed.
- Verdict: the hosted original parser and evaluator are right and are not where Mods are lost. Mods are lost in the three backends the interpreter runs against: the plan path of replaced draws is a whitelist run all or nothing (R-01), a Mod is a character Mod by a section name and by the exporter's shape (R-03, R-04), the native Core is bound to the Core's names and to version 1.48 while the installed Core is 1.49 (R-06, R-07), one failing input can still take the folder (R-10), what is left out at start has no notice (R-11), and one failed owned command ends the folder's owned GPU work until F10 (R-12). Layer host: a failed activation leaves a character invisible (L-01), F10 does nothing after a folder refused at start (L-02), 64 packages (L-03).
- Verification: the judge never draws, so replaced draws have no comparison with the original (V-01); the installed-Mod checks assert that a plan exists, not what it is (V-02).
- Run (sandbox, CPU only): `scripts/check_ini_execution_matrix.py` on the build of 2026-10-30: 32 classes, 41 probed uses, 39 failures. `docs/ini-execution-matrix.json` still expects the refusals D-142 to D-155 lifted; the step is in the `ini` group and has been red since 2026-10-28 (BUG-079).
- Plan in the document's section 8, waiting for the user's choice: P3-0 (matrix and judge groups) and Phase 0 (containment, P0-1 to P0-9) first, then the matched-draw judge (P3-1), then the draw state in place of the whitelist (Phase 1) and Mods by what they do (Phase 2).
- Not verified: everything marked "read" or "reported" there was not reproduced by a test.

## The review's plan, the parts without the game (2026-11-10, docs/architecture-review-2.1.md section 8)

- The user, 2026-11-10: push, then do the parts that need no game session.
- P3-0 done: `docs/ini-execution-matrix.json` states the current behaviour of the 11 stale uses (new decision `degraded`, with its warning required), `scripts/check_ini_execution_matrix.py` 0 failures (39 before); the judge in the `ini` and `owned` groups; `sandbox/run_all.py --only quick` (text 492 files, unit 117/117, evaluation 65/65, matrix, judge): all passed, 56 s.

## Every enabled INI an input (2026-10-30, D-155, docs/ini-semantics.md)

- The user: the parsing must take every INI under Mods as the original does, and handle a parse error the original's way. Now the folder generation's inputs are every enabled INI (except the native libraries' files and the folders of left-out character Mods): a plain Mod's Present and keys run, its settings persist, its texture-hash sections act in replaced draws; LIMIT-046 lifted. A declaration reference the layer does not take (`copy_desc`) leaves the destination null with the original-style warning once instead of refusing the generation or the draw.
- Failing first: the live INI scene's `plain` Mod (Present and a texture-hash section on the fixture's draw) and `bad` INI (a `copy_desc` in its post-Constants): 112/115 on the build before.
- Found on the way: the fixture's `run = CommandListUnbindAllRenderTargets` in its Present (`o0 = null`, no binding destination in the plan) ended the whole Present at that command every frame, so every Mod's Present lines parsed after it never ran (the plain Mod's `$presents` stayed 0, the settings were never saved). The Present and [Constants] lists now run each command on their own as the original does: a refused one does nothing with its warning once (DrawPlanAdapter::tolerate_refusals); a replaced draw's lists keep their all-or-nothing plan. With the owned GPU runtime a `copy_desc` of a file-backed buffer is made as owned work (the live scene's `bad` INI loads and presents without a warning), so the user's `\u6c64\u6c64` Mod, refused for that line only in the offline dry run, may already load in the game; offline the plan leaves such a copy null with a warning. The dry runs leave out refusals of services their stand-in lacks (the owned GPU domain, instance windows, render targets: Si Lunabelle and the new Mod offline) and a Present whose draws have no executor.
- Verification (sandbox): text, unit 117, original frontend host (Core pools 23/23 with a copy that leaves its destination null), INI runtime, the installed Mods' offline plans (20 of 20 loading; the folder generation, every key state, broken files), the Mods-folder and texture-filter replays, the judge, character replacement 377/377, live INI, owned Present 66/66. Red before as after: the library-call replay (BUG-078). Not deployed.

## INI parsing review (2026-10-30, D-154, docs/ini-semantics.md "How the files are read")

- The user asked for a review of the INI parsing. The parser is the original's; the layer around it diverged in: how a file is read (its own UTF-8 decoder refused a file with a byte sequence that is not UTF-8 whole, where the original reads up to that line: judge case `encoding`, `$before` 1, `$after` 0), how the Mods folder is walked (ordinal sort and a case-insensitive `.ini` against the original's case-insensitive sort and exact `.ini`; a `mod.INI` made the whole folder refuse), the installation's d3dx.ini never read (its [Rendering] keys, a command list it declares: LIMIT-039, a [Constants]/[Present]/[Key] written there), budgets of 128 and 64 INI files, and a Mod alone named by its ASCII file name instead of the original's implicit namespace (the user's new Mod `\u6c64\u6c64`, 2026-10-29, could not be tried alone). Behind the last: the native Core bridge widened a Mod's UTF-8 name byte by byte, so a Mod named in another script never found its own skeleton list; its lookups of a Mod's declarations now name what is missing.
- Failing first: runtime_test's Mods folder scene (a `Zeta` Mod's [Present] order, a `mod.INI`, a d3dx.ini beside Mods with a list the fixture runs, a character Mod in a folder named with a full-width colon without a namespace line): the whole folder refused on the build before; the judge's `encoding` case (4 values as the original's).
- The new Mod's real reason, shown now: `[Constants] resource... = copy_desc ...` in its post-Constants ("Unsupported declaration reference"), the construct that left Typhoeus out (D-131 step 1); the every-key-state check reports it NOT LOADED until copy_desc declaration references are taken (TASKS).
- Found on the way, pre-existing: BUG-078 (the vk01 library-call replay captures no translucency; HEAD built in a worktree of its own fails identically; translucency.ini changed 2026-10-29 22:52). The runtime test read the status JSON (UTF-8) with the console code page, so a package named in another script never compared equal; it reads UTF-8 now.
- Verification (sandbox): text, unit 117, original frontend host, INI runtime 499, the installed Mods' offline plans (dry run 19 of 20 loading, the folder generation, broken files), the Mods-folder and texture-filter replays, the judge (ten cases, `encoding` among them), character replacement 377/377, live INI 111/111, owned Present 66/66. Red, both before this change: the every-key-state check on the new Mod's copy_desc, and the library-call replay (BUG-078). Not deployed, not run in the game.

## 2.1.2: BUG-076 and the overlay's notices (2026-10-28, D-137)

- BUG-076 (tangtang Ramune v1.9's broken arm): a texture file the Mod does not ship binds nothing, as in the original; check_folder_generation failing before, passing.
- The overlay's notices (docs/ini-semantics.md): `> Reloaded config in N.NNNs` after F10 as the original, a refused F10, and the layer's refusals once per Mod and reason, drawn top left as 3DMigoto's (GDI Arial, level colours and times, black backing), after the Present work on the presenting queue, no GPU wait in the presentation. Failing first: owned_present_test (no notice drawn) and runtime_test (no reload notice) on the build before; now 66/66 and 104/104; text, unit, native tracker, M2 hook, character replacement, probe end to end, modes, Vulkan validation passed. Readback: green text on the dark backing at the top left (640x360 sandbox, 14 px).
- Every installed Mod in every key state (scripts/check_mod_states.py, a run_all step): the original's parsed keys pressed as a player does, every section planned after each hold and release; 316 key actions, 2407 states, 18 Mods, no refusal; reports BUG-076 with its fix taken out. The INI runtime test links character_program.cpp without util.cpp: its own logf (BUG-076's log line had broken that step; caught now).
- BUG-077 (D-137's classification): a missing, short or twice-declared buffer file degrades per draw as the original, no longer drops the Mod; check_mod_degradation.py (a copy of the smallest installed character Mod broken three ways, then every key state) failing before, passing; offline plans, every captured-frame replay, character replacement, live INI, native tracker, INI runtime passed.
- D-138 (D-137's gaps): a TextureOverride with match_* keys refused the whole Mod; now one that can match a 2D texture runs at a CheckTextureOverride of a pixel slot against the D3D11_TEXTURE2D_DESC the layer makes of the game's image (texture_identity.cpp `dx11_texture_desc`, the one its hash is made of), by the original's matcher in its order (hash sections, then these). A buffer's by description, a 1D or 3D texture's, and keys that change the game's resources at creation, map or copy are inert, the rest of the Mod loads, and each is reported for its Mod (CharacterDiscovery::gaps: log, the first three as notices; check_mod_states prints them). Failing first: check_mod_degradation's two new variants (the Mod refused on the build before), passing after; texture_query_test 25/25 (a described texture meets the section after its hash sections, pre before post; another description, an undescribed texture and the generation's own resource meet none); run_all's touched areas passed (text, unit, original frontend host, INI runtime 499, the four offline plan steps, Mods-folder and texture-filter replays, character replacement 377/377, live INI 104/104, probe end to end).
- D-139: image files as the original's LoadFromFile: a .dds by the DDS loader, every other file by WIC (assets::decode_wic, the PNG decoder made general: frame 0, the pinned DirectXTK table, the container's sRGB rule), in owned work and in replaced draws (ResourceCache's own staging and the staging arena). A file the loader makes nothing of is null: owned work warns and goes on; the character program finds it at load (image_loads) and binds nothing for it, the Mod told, so it no longer refuses a draw or an F10 candidate (LIMIT-043 narrowed to GPU-side load failures). Failing first: check_mod_degradation's PNG variant (6 sections refused, "Pixel file binding requires an unmodified DDS image") and broken-DDS variant (not reported), both passing now. resource_png_cases: BMP, JPEG, JPEG forced sRGB and TIFF fixtures equal the pinned DirectXTK loader on WARP, a broken file binds nothing; resource_image_ini_cases: a missing PNG and a truncated DDS bind nothing (were refusals); asset_gpu 282/282 with a PNG through both loader paths read back as decoded; a vk04 replay of SiModGB's copy with a PNG and a broken DDS texture: every matched draw planned, validation clean.
- D-140: a texture declaration's keys as the original takes them: `color_space` a loader flag (DDS forceSRGB, WIC FORCE/IGNORE_SRGB) and part of the image's identity (two colour spaces, two textures), the size keys ignored for a file texture, the declared `format` the view's (a typed texture without it, or bind flags without shader_resource, binds nothing). Failing first: check_mod_degradation's colour-space-and-size and wrong-format variants (6 sections refused each), planned now; asset_gpu 287/287 (one BC7 file plain and sRGB: two textures, BC7_UNORM and BC7_SRGB, blocks unchanged). The user's ten disabled Mods, copied without the prefix: every section plans in every state (179 key actions, 2398 states).
- D-141: the character contract's sizes as index widths, not budgets: component masks 64-bit (stream, list, identity), 64 components, 65535 bones, 4096 mesh sections and files, 4096 Mods in a folder generation (the original Core sizes its pools by the Mod's counts). Failing first: character_test's 40-component package (refused by the 32-component limit on the build before). The user, 2026-10-28: LOD 2 and 3 sections are not to be done.
- D-142: a replaced draw issues the original's direct draws: DrawIndexedInstanced (instances and first instance kept), Draw and DrawInstanced (vkCmdDraw, no index stream), `draw = from_caller` (the matched draw's call), and the three `auto` counts from the bound stream's whole elements. Failing first: check_mod_degradation's four draw variants (refused on the build before), planned now; a vk04 replay of SiModGB's copy with three draws changed that way.
- D-143: slot ranges in a replaced draw (`ps-t[a:b] = null`, `= ResourceX`, `= ref Pool[a:b]`, fetches into a pool) as the original's single slot operations, the range resolved by the original's code; an untyped pool element nothing assigned binds nothing. Failing first: check_mod_degradation's three range variants ("Unsupported frontend operation" before), planned now.
- D-144: CheckTextureOverride of a target other than a pixel slot in a replaced draw: a Mod's resource meets nothing, the matched draw's index buffer meets its region hash's sections through the original's region path, the game's other buffers and stages meet nothing with the original's warning. Failing first: check_mod_degradation's `checktextureoverride = ib` and `= vb0` variants (refused before), planned now.
- D-145: in owned work a view D3D11 does not make (typed texture under another format, typeless without one) binds nothing and the Present goes on (was a refusal; the test asserted it on the build before; WARP shows D3D11 makes no such view).
- D-146: dispatch, drawauto and the indirect draws in a replaced draw do nothing with the original's warning, the draw goes on (refused the draw before; check_mod_degradation's dispatch variant).
- D-147: store in a replaced draw takes the value of an earlier execution's read once its submission completed (no wait on the game's thread; before the first, the variable stays); clear of the Mod's resources runs as owned work there. Failing first: live INI rendering with a store in the fixture's sections (the character stayed the game's on the build before); `$stored` reads 7 now.
- D-148: draws past their files: the index range is clamped to whole triangles in the file, the vertex bounds come from the draw's own index range (block extrema, exact file read when needed), a draw reading past a vertex file is left out with the list going on. Failing first: check_mod_degradation's two variants (refused before); a vk04 replay with them (validation clean).
- D-149: a DDS array or cube loads as its first layer for a replaced draw's 2D slot (asset_gpu failing before, 297/297 now); volumes still do not. Finding: the game's index data are shared 16 MiB pools in DX11 (and the same offsets under Vulkan), so buffer-description sections, which would meet nearly every draw, stay inert (D-138).
- D-150: the original as the judge: scripts/check_dx11_judge.py runs sandbox/judge_cases under the pinned XXMI 3DMigoto (d3d11.dll 1.2.2 extracted from F:\vulkan into build/dx11-judge, a d3dx.ini of its own; sandbox/dx11_judge.cpp) and under the owned GPU runtime (resource_gpu.exe --present-ini), comparing their persistent variables; 4 cases, 21 values agree. It found two refusals where the original degrades, fixed: a description D3D11 does not create is a null resource (queries -2), a clear without a view of its kind clears nothing.
- D-151: the judge's cases cover image files (DDS, WIC, sRGB), copies, clears, CustomShader draws with their own blend and rasterizer states, conditions, command lists, IniParams, pool variables, slot ranges of pool elements and buffer views: eight cases, 105 values as the original's, the layer side at Vulkan 1.3 with the validation layer clean. What they found, fixed: a slot range of typed buffers binds nothing in the original (a view D3D11 refuses), where the layer refused; every view D3D11 does not make is now a null view; a resource's first reference adds its destination's bind flags (CustomResource::Substantiate); a texel-buffer view at an element the 16-byte minimum does not align (a Region) uses Vulkan 1.3's single-texel alignment.
- D-152: a replaced draw's plan follows the application's bindings at non-pixel slots saved into a declaration, nulled and put back (Snowshine's LOD 1 coordinate correction around its owned work), instead of refusing the first such line; a slot left changed at a draw or at the end still refuses.
- D-153: a copy of a buffer region past its buffer (undefined in D3D11; the original's device stops answering under the judge) copies nothing and the list goes on; copy destinations are created zeroed. The judge has nine cases, 110 values.
- Not deployed, not run in the game.

## 2.1.1: the loader behind a proxy DLL, character meshes without resizable BAR (2026-10-28, BUG-074, BUG-075)

- The user asked for a review of the whole project after an AMD player's 2.0.9 log (the layer loaded, "no free memory near vulkan-1.dll for 0 stubs; nothing redirected", nothing after: no Mod). Reviewed for what breaks on other machines: injection and the loader (BUG-074), device creation (every added extension, feature and the upload queue already checked against support), memory types and heaps (BUG-075; the snapshot arenas, bone rings and translucency targets choose correctly), the layer's own shaders (bone_merge.hlsl, no subgroup operations), paths outside ASCII (wide or u8path everywhere, BUG-046 and LIMIT-042 earlier), compiler loading (full paths), formats (LIMIT-045), device teardown, the release check.
- BUG-074: the first module named vulkan-1.dll was taken for the loader. 2.1.1 patches only a module that exports vkGetInstanceProcAddr as code; a proxy's forwarders name the loader that is waited for (by full path, or a renamed copy by its file name); every module of that name is described in the log (path, exported names, where vkGetInstanceProcAddr forwards); a second real loader is logged and left alone. Test (modes): the sandbox copied next to a generated proxy vulkan-1.dll (every export of the system loader forwarded by full path; and one forwarding to a copy named vulkan-1-real.dll). Failing on 2.1.0 with the AMD log's exact line ("0 stubs; nothing redirected", no device), 50/50 on 2.1.1.
- BUG-075: character meshes went into host-visible device memory whatever its heap. This machine has resizable BAR (that type on the 7.71 GiB heap); without it the window is a heap of about 256 MiB the game uses too (four characters were 110 MB here). 2.1.1 maps device memory only when it is on the largest device-local heap (resizable BAR, integrated GPUs), otherwise copies the mesh into device memory on the layer's upload queue (staging in system memory, fence, then published), as D3D11 fills a DEFAULT buffer from its initial data (CustomResource::SubstantiateBuffer); host memory without an upload queue. Status `mesh_memory` (mapped, copied, host). Memory type choices skip protected, lazily allocated and AMD device-coherent types. Test (character replacement): the split-queue runs with `EFMI_VK_TEST_SMALL_BAR=1` (the test's child process: host-visible device memory taken for a heap of its own of 256 MiB) must copy, the others map; failing on the build before (16 checks), 377/377 with pixels and sync validation; the vk03 Mods-folder replay with the simulation passed (a 23.7 MB mesh copied in 8.1 ms, mapped 5.3-5.9 ms).
- check_release.py chose its layout Mod by a fixed name the user has since removed; it now takes the smallest character Mod the installed folder loads and every enabled folder that is not one.
- Evidence (sandbox): modes 50/50, character replacement 377/377, text, unit, probe end to end, Vulkan validation, the vk03 Mods-folder replay (small window simulated): all passed. Release check on EFMI-Vulkan-2.1.1.zip (SHA-256 dba416f31195a16396690b41b8fa8257ad4e29053ea09c9297d46c93f72bd391): passed (installed Mods 18 character Mods, none left out; both unpack layouts). Not run in the game; not on an AMD GPU (none here). 2.1.0 was never published; 2.1.1 supersedes it.

## 2.1.0: the release finds the EFMI folder from where it is unpacked (2026-10-27, BUG-073)

- The user downloaded v2.0.9 and unpacked it into EFMI\Mods: no Mod loaded (`../Mods` named EFMI\Mods\Mods). Moved to the EFMI folder it worked in the game (the user, 2026-10-27): the first game run of the release build.
- 2.1.0: `m2_mods = auto` (the release's): the first folder upward from the DLL's that holds Mods and Core\EFMI; every scan of the Mods folder leaves out the layer's own folder (its settings file is an INI); a first-chance fault outside the layer's folder is logged as `exception:` with a note, not as `crash:` (the game raises and handles one at every start; the 2.0.9 log showed it as a crash). The sandbox's `--handled-fault` raises one after its device exists; symbolize_crash.py reads both kinds of lines.
- Evidence (sandbox): check_release.py extended with the unpack layouts (an EFMI folder of its own with the installed Core, one character Mod and the libraries; the release in EFMI and in EFMI\Mods; an INI planted in its settings; the handled fault): failing on the 2.0.9 archive (the Mods folder layout found no Mod, the fault logged as a crash), passing on 2.1.0 (and the installed Mods: 18 character Mods, none left out); run_all's touched areas (20 steps: text, unit, both offline plan steps, every captured-frame replay, character replacement, live INI, probe end to end, modes, validation): all passed. Archive EFMI-Vulkan-2.1.0.zip, SHA-256 0e50731e675fe6ca3cae55e063fd907cf4673af284fbcacba703ee214f078ec8.

## 2.0.9: the playable release (2026-10-27, D-135)

- The user, in the 2.0.8 game session: basically no problems left; publish a release (GPLv3, install and use only, nothing for development).
- 2.0.9: `scripts/build.py release` (no probe compiled in), relative configured folders from the DLL's folder (the release's `m2_mods = ../Mods`), LICENSE (GPLv3), THIRD_PARTY_NOTICES.md, release/ (configuration and README), scripts/check_release.py, docs/release.md.
- Evidence (sandbox): the Mods-folder scene with the Mods folder named relative to the layer's folder (failing on 2.0.8: no Mod found; 25/25 now); the release check (files, digest, no pipe name, injected with the installed Mods: 18 character Mods, none left out, no fault, no pipe, validation clean, Core shaders compiled without dxil.dll); run_all's touched areas (text, unit, Mods-folder replay, character replacement, live INI, owned Present, probe end to end, modes): all passed. The release itself not yet run in the game; the user will download and test it.
- Published 2026-10-27: tag v2.0.9 on c7cf4f5, https://github.com/loyal37/efmi-vulkan/releases/tag/v2.0.9, asset EFMI-Vulkan-2.0.9.zip (12,891,347 bytes, SHA-256 06e614c5f32fc022b246ccfc00c683683cc207d1703ee44206c06b7b757a8dc6, downloaded back and compared). Notes in English (the user: everything published in English). The user's own dist stays the 2.0.8 development build with the probe.

## 2.0.8: F10 around Mods added and removed, RabbitFX's anti-dither narrowed and ahead of its key (2026-10-27)

- Game session of 2.0.7 (PID 30992, user-started, 2026-10-27 19:0x-19:25): with the anti-dither on, the field's grass and bushes drew as solid quads (BUG-071); the `\` key took a second or two (BUG-072); an F10 after the user replaced a Typhoeus Mod and deleted Si SorceressV2 ended the game (BUG-067; Unity crash report: our std::system_error, errc 3, rethrown in folder_frame_and_exchange from the old generation's retirement). The user then asked for every operation around F10 to be considered: docs/f10-operations.md; BUG-068, BUG-069, BUG-070 found that way.
- 2.0.8: a package and a folder generation stop once, whoever asks first (std::call_once), and a background stop's error is logged, never rethrown into the game; a Mod enabled again takes its slot back and a stopped package's controller holds nothing; an F10 pressed while a reload loads reloads again after it; the dither gate is set from the live generation only; the dither rule needs the camera fade (`max` of two instance window values, RabbitFX's `max r, cb.x, cb.x`): the field's LOD cross-fade with its alpha test is no longer taken; the twins of dithered pipelines are prepared when the game creates them (the variant cache's `prepare`, built behind requested variants), the other state of replaced draws with the first.
- Evidence (sandbox): recipe test (the foliage-shaped fixture and the field frame's eleven alpha-tested modules, failing on 2.0.7; 45 of 626 captured modules dithered, all patched and valid), cache test (prepared variants build after requested ones, a request moves one ahead), the Mods-folder scene (25/25 three times; failing on 2.0.7: the disabled drawn Mod's F10 ended the process with 0xC0000409, the Mod enabled again took a third slot, the F10 pressed during a load was lost), and the touched areas of run_all (23 steps: text, unit, globals, every captured-frame replay, recipes and variants, M2 hook, character replacement, live INI, owned Present, probe end to end, modes, budgets, many-draw timing, validation): all passed. Not yet seen in the game.
- Deployed 2026-10-27 20:48 (d532cb3) with the game and the launcher closed; settings backed up to build/backups/settings-before-2.0.8-*; actual-config smoke: 2.0.8, 17 Mods (the user removed the new Typhoeus Mod after the crash), none refused, validation clean, game log unchanged, 298 MB private.
- Open: character Mods' toggle keys felt slow too (the user); the 60 s recording showed every key event and no texture or variant wait. Their sections have `condition = $active0 == 1`, set by the character's draw and cleared in [Present]'s post list; a press whose first presentation finds it 0 is lost, as in the original, where the frame's draws always precede its Present. To measure in the game before any change.

## 2.0.7: settings named outside ASCII are saved (2026-10-27, LIMIT-042)

- A persistent variable whose namespace is outside ASCII (an INI without a namespace in a Mod folder named with a full-width colon, as Mod folders are) stopped the save of every Mod's settings in the folder generation; it is now written as UTF-8, as the original does. Scene test failing on 2.0.6, 97/97 now.
- Deployed 2026-10-27 18:51 (5017280) with the game and the launcher closed; settings backed up to build/backups/settings-before-2.0.7-*; actual-config smoke: 20 Mods, none refused, validation clean, game log unchanged.

## 2.0.6: RabbitFX's anti-dither (2026-10-27, D-133)

- The user: RabbitFX's anti-dither had no effect under Vulkan. It now works as RabbitFX's ShaderRegex does under DX11: the `\` key toggles the persistent `$\rabbitfx\censor`; while it is set, every draw of a pipeline whose fragment stage has the character dither (interleaved gradient noise against the instance window; 38 of 473 captured modules) is drawn without it: the application's draws through a twin pipeline bound in place of the game's, replaced draws through their variants.
- Status: `folder_generation.discard_gates` (RabbitFX: index 172, component 3), `dither_off`, `dither_twins`, `dither_twins_failed`, `dither_twin_binds`.
- Evidence (sandbox): recipe tests (rule and patch on fixtures and the 38 captured modules, all valid SPIR-V), the Mods-folder scene with a dithering application shader and `$censor` 1 (96/96; with `$censor` 0 its pictures fail), the vk03 replay with the installed folder (gate 172.w found, value 0). Whole suite passed (build/run_all-2.0.6.out, the replays rerun after their build lists gained the new source: build/run_all-2.0.6b.out). Not yet seen in the game.
- Deployed 2026-10-27 18:32 (52b16c2) with the game and the launcher closed; settings backed up to build/backups/settings-before-2.0.6-*; actual-config smoke: 20 Mods, none refused, validation clean, game log unchanged, private 308 MiB.

## 2.0.5: panel settings adopted under the folder's names (2026-10-27, BUG-066)

- Settings of a Mod's INI without a namespace (`k\1.ini` panels) are adopted under the folder generation's names; a folder file adopted by 2.0.0 to 2.0.3 is adopted again once. Verification: the failing scene checks (93/93 now), the vk03 replay, owned Present.
- Deployed 2026-10-27 16:16 (81a422f) with the game and the launcher closed; 2.0.4 was deployed at 16:06 (229a801). The settings folder was copied to build/backups/settings-before-2.0.5-* first. The actual-config smoke (20 Mods, none refused, validation clean, game log unchanged) migrated dist/settings/mods-folder.ini: no old panel names left, 85 lines under the folder's names, no settings warning.

## 2.0.4: a LOD the Core holds from another Mod (2026-10-27, BUG-065)

- From the 2.0.3 game session: Typhoeus' draws refused in about half; the folder's one `$\EFMIv1\lod_level` held another Mod's LOD. The contract now compares the remap the Core picks, not the LOD number (docs: KNOWN_ISSUES BUG-065).
- Same session, sandbox-independent facts: frame pacing over the last 2048 frames mean 8.55 ms (117 fps), p50 8.34 ms, p99 10.8 ms, one 345 ms frame with a 289 ms first texture load; SiModGB replaced 554,457 of 554,548 matched draws.
- Verification: the failing test, offline plans and folder generation unchanged (143 drawn sections, 19 of 19 alike), ten captured-frame replays, character replacement, live INI 91/91.

## 2.0.3: F10 keeps unchanged meshes on the device (2026-10-27, BUG-061's open part)

- A successor generation whose package streams are the same bytes at the same places as the live one's keeps the live mesh (`keep_mesh`, `CharacterPackage::same_payload`: files, offsets, validated checksums), as images are kept by their files since 1.9.4. The original releases every resource on F10 and reads its files again at first use; the layer reads them once for the validation and no more. A discovered package's mesh files are laid out in the order of their files (the generation's resource order differed between parses, so the same files made different layouts).
- Status: `record.mesh_uploads`, `record.meshes_kept` (device-wide).
- Deployed 2026-10-27 14:43 (f6d0724) with the game and the launcher closed; backup build/backups/before-2.0.3-20261027-144345, manifest build/candidate-2.0.3.json, cfg unchanged. Actual-config sandbox at --api 1.3: 20 character Mods, 0 left out, loaded in 2016 ms, private memory 308 MiB, validation clean, game log unchanged (build/smoke-2.0.3.json in the worktree). Not yet in the game.
- Check failing on 2.0.2 (all four configurations): the live INI scene's F10 with unchanged mesh files uploads no mesh again and keeps the live one. Touched areas passed (text, unit, offline plans, folder generation, ten captured-frame replays, character replacement, live INI, owned Present).

## 2.0.2: a component chosen by `vbN->stride` (2026-10-27, D-132, LIMIT-041)

- Discovery makes a section per `if vbN->stride == K` branch of a TextureOverride; the device index matches it only to a draw whose pipeline binds that stride; the plan answers the stride from the same pipeline.
- Evidence (offline and sandbox): installed Mods' dry run with no refused section (avywenna's face planned as two sections, 2 draws and 2 images each; drawn sections 141 to 143); fixture Mod with a stride-branching section planned per branch; live INI scene 87/87 with the fixture's first section branching on `vb1->stride` (the non-matching branch first), and 86/87 with the device-side check disabled. Touched areas passed (text, unit, offline plans, folder generation, vk03 replay, character replacement, live INI).
- Deployed 2026-10-27 13:56 (979feef, with 2.0.1's payload release) with the game and the launcher closed; backup build/backups/before-2.0.2-20261027-135629, manifest build/candidate-2.0.2.json, cfg unchanged. Actual-config sandbox at --api 1.3: 20 character Mods in the folder generation, 0 left out, loaded in 2251 ms, private memory 306 MiB, validation clean, game log unchanged (build/smoke-2.0.2.json in the worktree). Not yet in the game.

## 2.0.1: mesh payloads not held in host memory (2026-10-27, D-131 step 5, ISSUE-011)

- As the original frees a buffer file's bytes once the resource holds them, a package releases its payload once validated; each stream keeps its file, offset and checksum. The activation upload reads the files again with up to four threads (bounce buffer, checksum), and so does a stream a live plan adopts for bones; changed bytes refuse with the file's name. Status: `package_bytes`, `payload_held_bytes` (0), `mesh_upload_microseconds`.
- Checks failing before: the live INI scene's packages and Mods folder report `payload_held_bytes` 0 (2.0.0 has no such field and holds the payload); character package unit tests (a loaded package and an envelope read back from their files, a changed file refused, a missing file refused).
- Evidence (sandbox): actual configuration with the installed Mods folder, 20 character Mods: private memory 712 to 298 MiB. vk03 A/B against the deployed 2.0.0, three interleaved pairs: final targets, steady replacement and residency identical; mesh upload 4.5 ms for 23.7 MB; skipped draws at startup 112 to 87; steady costs within the run-to-run spread. Touched areas passed (unit, offline plans, ten captured-frame replays, character replacement 109 package checks, live INI, owned Present).

## 2.0.0: one generation for the Mods folder (2026-10-27, D-131)

- docs/folder-generation.md. Step 1, offline and in the frontend: the native Core bridge serves several Mods in one generation (per Mod its contract, discovery and skeleton; a native answers the Mod whose TextureOverride runs and holds only the draw's own Mod to the contract); `CharacterProgram::discover_folder` loads the Mods folder in the original's include order and gives each character Mod its package; a Mod the folder cannot take is left out with its own reason.
- Check failing before (no folder generation): scripts/check_folder_generation.py, now a suite step: every character Mod loads, discovers and plans the same alone and in the folder (19 of 20 on the installed folder; Typhoeus refuses alone offline and is left out of the folder).
- Changed for the per-Mod generations too: discovered sections in one order (hash, count, first index); a plan's `vbN->stride` answered from the draw's pipeline; a generation without owned reads answers resource queries from the host's CPU answers. Found: LIMIT-041 (avywenna's stride-selected component keeps the game's mesh).
- Verification: run_all --only ini, replay, character replacement, live INI: passed.
- Step 2 (the layer): with m2_mods the layer makes one folder generation (`folder_create`): a package per character Mod carved from it, the INI Present once per presentation under the folder's lock (`folder_frame`), plans of every package under that lock, one settings file `mods-folder.ini` (the Mods' own files adopted the first time), F10 loading a successor that replaces every package at one presentation (a Mod added becomes a package, one gone stops; a refused successor counts as refused in every package's status). `m2_folder_generation = 0` keeps a generation per Mod. A folder without character Mods is a valid empty generation. Namespaces as the parser names them (wide, any script: a namespaceless INI in a folder named with a full-width colon).
- Evidence (sandbox and replays): vk03 with the 20 installed Mods, same build with the switch off and on, three interleaved pairs: final targets, replacement and residency identical; presentation work 221 to 146 us per frame; recording 410 to 420 us per frame (plan 199 to 208); the INI Present ran 300 times in 300 presentations. Live INI scene: a Mod the layer cannot take (an export without TextureOverride sections) is left out alone with its reason, the other two run the whole scene (keys, F10 reloads, exchanges, pixels). Owned Present: F10 with the only Mod gone exchanges to an empty generation without a GPU wait in Present.
- Whole suite 49/49 (build/run_all-2.0.0.out in the worktree). Commit 5695edf; main fast-forwarded and pushed.
- Deployed 2026-10-27 12:14 (5695edf) with the game and the launcher closed; backup build/backups/before-2.0.0-20261027-121448, manifest build/candidate-2.0.0.json, cfg unchanged. Actual-config sandbox at --api 1.3: the folder generation with 20 character Mods, 0 left out, loaded in 2381 ms; validation clean, game log unchanged (build/smoke-2.0.0.json in the worktree). Not yet in the game.

## 1.9.9: texture loads at least as fast as the original's (2026-10-27, D-130, cause C)

- An image's load reads the file straight into a staging arena the loaders keep, with each worker's own command buffer and fence (docs/design-review-1.9.md cause C). Benchmark against the original's DirectXTK load on D3D11, the installed Mods' ten largest maps (661 MiB, file cache warm): one 64 MiB map 43 ms on 1.9.8, 20 ms on 1.9.9, 28 ms for the original; the ten together 245 ms, 99 ms, and 297 ms for the original one after another (build/texture-bench/report.json in the worktree).
- Replay vk03 (22 Mods), three interleaved pairs against the deployed 1.9.8: startup texture waits 120 to 46 ms, longest startup frame 207 to 127 ms; pictures, replacement and residency identical; per-frame cost unchanged (build/performance-ab in the worktree).
- Verification: asset tests 274 checks (new: every level of arena loads read back, one arena per busy period, one pool per worker, the arena released when idle), validation clean; run_all --only textures and every captured-frame step, and the new step "texture first-use loads against the original": passed.
- Deployed 2026-10-27 09:49 (eb32082) with the game and the launcher closed; backup build/backups/before-1.9.9-20261027-094917, manifest build/candidate-1.9.9.json, cfg unchanged. Actual-config sandbox at --api 1.3: 20 packages configured, none refused, validation clean.
- Not yet in the game. To see: switching characters and a Mod's options with `textures.draw_wait_microseconds` and `load_max_microseconds` lower than 1.9.8's.

## 1.9.8: each generation's original engine state is its own (2026-10-26, D-129, BUG-063's open part)

- The original's configuration globals are redirected by a forced include (frontend/xxmi/engine_globals.h) to the generation the calling thread entered; a scope takes only its generation's lock; a load also the load mutex, which no drawing, presenting or saving thread takes; the assembler's tables under their own mutex; the resource root and warnings per generation (docs/engine-generations.md).
- Test that fails on 1.9.7: runtime_concurrency_cases ("a running generation presents and draws while another generation's load holds its scope"); on 1.9.8 the runtime's 499 checks pass. New suite step "INI runtime globals classified" (scripts/check_engine_globals.py): 911 writable globals in 52 objects, all classified (constant 820, load 11, lock 14, once 22, present 2, statistics 42).
- Verification: the whole suite (49 steps) passed (build/run_all-1.9.8.out in the worktree), the change being in the runtime core every area runs through. Offline A/B against the deployed 1.9.7, replay vk03 (22 Mods), three interleaved pairs of 2600 frames: recording work 463 to 456 us per frame, plan 222 to 219, host frame p50 5555 to 5560 us; final targets, replacement and residency identical (build/performance-ab/20261026-234325).
- Deployed 23:45 (604d147) with the game and the launcher closed; backup build/backups/before-1.9.8-20261026-234556, manifest build/candidate-1.9.8.json, cfg unchanged. Actual-config sandbox at --api 1.3: 20 packages configured, none refused, validation clean, the same exit as 1.9.6 and 1.9.7 (build/smoke-1.9.8.json in the worktree).
- Not yet in the game. To see: an F10 or a newly added Mod while playing without a stall for the load's length; SiModGB (1.9.7).

## 1.9.7: SiModGB's declarations and texture queries as the original takes them (2026-10-26, BUG-064)

- Session with 1.9.6 (PID 8552, 22:0x, user-started): the F10 freeze is gone (the user: seems fine now); SiModGB drew nothing (index files with `stride = 6`, typed vertex files, `ps-tN == filter_index` in plans refused).
- 1.9.7: index files by their format, vertex files by their stride or their format's size (the original's FillInMissingInfo), file roles from the assignments that bind them, pixel-slot texture queries in plans from the host's texture matcher.
- Verification: run_all --only frontend host (all host checks), offline plans (SiModGB 26 of 26 sections plan), Mods folder, library call, texture filter (new: vk04 with only SiModGB, no refusal), character replacement, live INI: passed.
- Deployed 23:09 (b84ac88) with the game and the launcher closed; backup build/backups/before-1.9.7-20261026-230904, manifest build/candidate-1.9.7.json, cfg unchanged. Actual-config sandbox at --api 1.3: 20 packages configured, none refused, validation clean (build/smoke-1.9.7.json in the worktree). Not yet in the game.
- main not fast-forwarded: the main checkout holds uncommitted work of another session in draw_plan.cpp/h and runtime.cpp, which this commit also changes; the commits are on branch `engine-work` (worktree .worktrees/fix).
- Next: the engine lock refactor (BUG-063's open part), design first.

## 1.9.6: a long else-if chain loads in bounded time (2026-10-26, BUG-063)

- Session with 1.9.5 (PID 11492, 21:32-21:3x, user-started): the same F10 froze again at 21:35:51; `present_gpu_waits` stayed 0 (1.9.5's guard held). Sampled through the probe in the frozen game: the presenting thread waiting for the engine lock in an F10 exchange, the render thread for a controller lock; the engine lock held by the new Typhoeus Mod's load, which never finished. Offline the sandbox with the actual configuration hung at device creation with that Mod installed; a stack dump of the sandbox (scratch tool, our own test process only) showed `HostCompileRoots` in a deep recursion: `compile_graph` walked every body of every `if` without remembering lists, 2^n steps for the panel's else-if chain (the original shares one nested `if` between pre and post bodies).
- 1.9.6: every command list visited once, as the original walks its lists flat. Test that fails on 1.9.5: owned_present_test's 40-option else-if chain in a second INI's [Present] (the device never came up); 6/6 on 1.9.6. Actual-config sandbox with the installed Mods folder: 20 packages configured, the new Typhoeus Mod among them, none refused, validation clean (build/smoke-1.9.6.json).
- Verification: run_all --only frontend host, live INI, owned Present: all passed (80 and 54 checks).
- Open: background loads still hold the global engine lock for a whole INI load (BUG-063's open part).
- Deployed 21:59 (b13d595) with the game and the launcher closed; backup build/backups/before-1.9.6-20261026-215940, manifest build/candidate-1.9.6.json, cfg unchanged.

## 1.9.5: nothing waits for the GPU in Present (2026-10-26, BUG-062, D-128)

- Session with 1.9.4 (PID 14500, 20:30-20:5x, user-started): Typhoeus' wings stayed in place with GPU bones (BUG-060 confirmed fixed in the game; `bones` 1835 submissions, 0 failures, `submission_work.prefixed_batches` 1836, `prefix_alone` 0); an F10 at 20:39:35 shared every texture (`reload candidate demands 0 of 21 images`). The user replaced Typhoeus' Mod and pressed F10 at 20:46:26: the game froze (BUG-062). The new Mod named \u6d01\u5c14\u914d\u5854 (Gilberta) is not an EFMI merged-skeleton Mod (per-IB replacement, cross-IB bones by DX11 vertex shader hashes): the layer does not load it; the user: no cross-IB support for now, F10 first.
- 1.9.5 (D-128, as 3DMigoto's reload, which only releases and never waits for the GPU in Present): a package whose Mod went stops on a thread of its own (`stop_later`); the layer's own GPU waits count one made inside vkQueuePresentKHR (`replacement.present_gpu_waits`). Test that fails on 1.9.4 (with the counting and the sandbox's test keys): owned_present_test's F10 with the Mod gone (1 wait in Present, logged at "presentation work (frame, character Present)", the place the game froze); on 1.9.5 0 in both entries.
- Verification: the touched areas (run_all --only owned Present, live INI, character replacement, two queues): all passed (48, 80, 361 checks, replay). The whole suite not run (the user: keep regressions short).
- Deployed 21:31 (877da1c) with the game and the launcher closed; backup build/backups/before-1.9.5-20261026-213107, manifest build/candidate-1.9.5.json, cfg unchanged.

## 1.9.4: one image per texture file across generations (2026-10-26, BUG-061)

- An image is the device's, keyed by its file's path, size and write time; generations count their declarations; an F10 shares every file that did not change (no second read, no second copy), a changed file is a new image, a failed load is tried again by a later generation. The submission hook no longer looks recording up per command buffer outside character mode.
- Test that fails on 1.9.3 (all four entry and queue configurations): the live INI scene's F10 with unchanged texture files reads none again and declares no second image; on 1.9.4 the scene's reloads read 4 files instead of 8.
- Not done (BUG-061's open part): bringing up only the packages drawn lately (tried and withdrawn), the meshes per generation.
- Deployed 20:29 (7a6a68b, with 1.9.3's bone change) with the game and the launcher closed; backup build/backups/before-1.9.4-20261026-202937, manifest build/candidate-1.9.4.json, cfg unchanged (`m2_gpu_bones = 1`). No actual-config smoke this time (the user asked to test in the game at once). The shader cache is rebuilt on the first start (new build identity).
- Verification: the failing-first check and the live INI rendering step (80/80, all four configurations); the whole suite was started and stopped at the user's request before deployment (the user tests in the game instead); 1.9.3's suite had passed the rest. Not yet in the game.

## 1.9.3: one persistent merged skeleton per object, our GPU work inside the game's batches (2026-10-26, BUG-060, D-127)

- Session with 1.9.2 (PID 13144, 18:33-18:5x, user-started, DLSS Frame Generation on): Typhoeus' wings on the ground in some frames with GPU bones (BUG-060; the probe's `gpu_bones=0` at 18:45:23 was diagnosis only, the wings then stayed); after Frame Generation off and on an F10 exchange at 18:47:33 brought 20 packages up twice (BUG-061, open); stutter while switching from texture first loads. The user: the designs still have many problems, solve them at the root. docs/design-review-1.9.md names the causes and the order of work.
- 1.9.3 (D-127): per object one persistent merged skeleton, written only by imports run in the batch of the recording that drew the component; per recording a snapshot copied after its batch's imports; our command buffer at the front of the application's batch, its waits on queue-signalled semaphores widened to our stages, our timeline signalled after the application's batches in the same call (vkQueueSubmit and vkQueueSubmit2). Status: `bones.copies`, `borrowed`, `failed_calls`, `replacement.submission_work`.
- Test that fails on 1.9.2: replay vk06 with only the Typhoeus Mod, `--split-passes --rolling-bones --bones-identical --check` (pictures differ, the lower body and the hands' parts drawn down to the image's bottom as in the game); on 1.9.3 identical, validation clean, 112 of 112 batches prefixed and completed on two queues. `--animate-bones` alone: 1.9.2 differed by a pose one frame newer, 1.9.3 identical. New suite step "captured frame with the game's bone regions reused".
- Verification: full suite 46 steps (build/run_all-1.9.3.out in the worktree): 45 passed at once, the TangTang declaration step failed only because the user had removed that Mod; replay_capture's --only-mod now keeps using the project's copy of a Mod no longer installed, and the step passed on 1.9.3 (2736 draws replaced, as before). Not yet in the game.

## 1.9.2: shader cache (2026-10-26, D-126)

- The user asked for a shader cache like 3DMigoto's. Measured first (sandbox, actual configuration, 22 Mods): the CustomShaders cost 8 DXC compiles (0.59 s) and 62 translations of shipped caches (0.18 s) at every load; the status now reports this per package (`owned_present.shaders`).
- 1.9.2: translated stages kept in `settings/shader-cache` (D-126), the owned runtime's pipelines through the device's pipeline cache. Load of the 22 Mods with a warm file system cache: 2.40-2.43 s without, 1.65-1.67 s with the cache (35 entries, 7.4 MiB; the first load already shares one entry among the identical panels). Tests that fail without it: the owned Present test's second start (2 stages compiled again), and the compiler test's cache cases.
- Verification: full suite 45/45 (compiler test 186 checks, owned Present 38/38). Deployed 18:27 (d3f6f87), game and launcher closed, backup build/backups/before-1.9.2-20261026-182725, cfg unchanged. Two actual-config sandbox smokes clean (22 packages, no refusal, validation or fault; build/smoke-1.9.2.json); they filled dist/settings/shader-cache with 35 entries of the deployed build, which the game's next start reads. Not yet in the game (neither 1.9.1 nor 1.9.2).

## 1.9.1: no driver allocation per frame in owned work, settings saved as the original (2026-10-26, BUG-058, BUG-059, D-125)

- Session with 1.9.0 (PID 20392, 16:16-16:37, user-started, DLSS Frame Generation on). Data captured in the real game before the freeze: Si NightveilTrancer 361633 of 382408 matches replaced, Tangtang 155970, Yvonne, arcane; `bones.components_missing` 0; `held_before_shown` 47, `original_before_shown` 0, `waiting_memory` 0. The user found the Mods' Ctrl+K key panels stuttering when opened and while switching options; then the game froze (BUG-058). Measured: the panel's per-frame settings saves under the engine lock (BUG-059), texture first-use waits up to 263 ms for 64 MiB 8192x8192 BC7 maps (reading one alone takes 30-70 ms), and single frames of 0.6-0.9 s while switching that neither explains (not yet attributed; the profile windows missed them before the freeze).
- 1.9.1: descriptor sets and upload buffers reused (D-125), the application's submission takes a generation's lock only for its waiting draw work, settings saved every 60 s and at exit with the file written outside the engine lock. Tests that fail on 1.9.0: the owned Present test's allocation counters, runtime_test's save cadence (457 saves before).
- Verification: full suite 45/45 (a first run failed the color/depth transfer check, which wants no staging charged while a frame converts: kept upload buffers are now off the staging budget while idle and charged again when reused). Deployed 17:33 (7c97e62) with the game and the launcher closed; backup build/backups/before-1.9.1-20261026-173327, cfg unchanged. Actual-config sandbox smoke (--api 1.3): 22 packages configured, none refused, no validation error, no fault (build/smoke-1.9.1.json). Not yet in the game.
- README rewritten for the current state (the user, 2026-10-26: it was out of date).

## 1.9.0: bones across recordings, no wait for memory, the Mod first (2026-10-26, BUG-054, BUG-055, BUG-056, D-123)

- Session with 1.8.9 (PIDs 45144 and 36252, user-started; two module exchanges in each, untested quick fixes, which the user rejected: fix properly and test first). Findings: Si NightveilTrancer replaced from its first matches (BUG-053 fixed in the game) but without its legs with the bones on the GPU (BUG-054); Mods missing in the character screen and others turning from the game's model into the Mod (BUG-056, D-123); the probe's bone switch not reaching a swapped module (BUG-055). Profile of the game's render thread: busy 73%, a layer frame in 62% of that; the engine lock 5 us per frame (not a bottleneck); textures as designed (0 evictions); about 176 indexed draws per frame through the replacement, 6.3 us each.
- 1.9.0: each fix with a check that fails on 1.8.9 first: two-queue replay `components_missing` (113 before), the character test's bone switch after exchanges, `--reserve-mb 1000000` (nothing replaced before), `--mod-first` on vk03 (12 game meshes before). New suite steps for the last two and for the Si Mod's bones on two queues. The replay harness signals its presentation semaphore per swap chain image (the validation layer's reuse check had flagged the frame-slot one).
- The newly installed Tangtang Mod (BUG-057): declaration queries from SPIR-V (D-124, checked against the paired catalogue's DXBC), `vb3 = vb0` by reference, a blend edit given twice. In the TangTang replay with only this Mod every matched draw is replaced (2736); the dry run plans all its sections.
- Verification: full suite 45/45 (run_all, all steps passed, with the new steps for the Si Mod's bones on two queues, a full card, the TangTang replay with only the Tangtang Mod and the DXBC comparison of the declarations). Deployed 16:03 (3bdf01b) with the game and the launcher closed; backup build/backups/before-1.9.0-20261026-160326, manifest build/candidate-1.9.0.json, cfg unchanged. Actual-config sandbox smoke (--api 1.3, installed Mods): 22 packages configured, none refused, no validation error, no fault (build/smoke-1.9.0.json). Not yet in the game.

## 1.8.9: settings saved only when dirty (2026-10-26)

- The 22-Mod replay (vk03) showed the engine lock contended even with one recording thread: 1338 waits, 43 ms over 900 frames, because every Present woke each generation's worker to save, and save() took the lock to find nothing dirty. The worker is now woken only when the persistent variables changed (noted under the lock the calls hold anyway): 0 waits. Replay CPU per frame against 1.8.8, six pairs: 2781 to 2699 us. Present of all 22 generations: 226 us per frame (docs/recording-performance.md).
- Full suite 41/41 (build/run_all-1.8.9.out in the worktree). Deployed the 18 files (3731999) on 2026-10-26 12:26 after two closed-process checks; cfg unchanged; 1.8.8 in build/backups/before-1.8.9-20261026-122640/ (build/candidate-1.8.9.json). Smoke at --api 1.3: 1.8.9, 22 packages, none refused, no validation message (build/smoke-1.8.9.json). Not yet seen in the game.

## 1.8.8: the engine lock's contention counted (2026-10-26)

- Every INI call of every generation (a matched draw, a Present) takes one process-wide engine lock, because the original engine addresses its globals; the game records on several threads, so matched draws may wait for each other there, about 20 us each, which neither the sandbox nor the replay (one recording thread) shows. The replacement status now has `engine_lock` (entries, waits, wait_us). Whether to let generations run concurrently (the original's globals redirected per thread around the pinned sources) depends on what a session shows. scripts/profile_game.py also names the waits the layer makes itself.
- Full suite 41/41 (build/run_all-1.8.8.out in the worktree). Deployed the 18 files (1ac78cf) on 2026-10-26 11:50 after two closed-process checks; cfg unchanged; 1.8.7 in build/backups/before-1.8.8-20261026-115037/ (build/candidate-1.8.8.json). Smoke at --api 1.3: 1.8.8, 22 packages, none refused, no validation message (build/smoke-1.8.8.json). Not yet seen in the game.

## 1.8.7: layouts interned, texture roles kept, the entry looked up directly (2026-10-26, D-121 step 4)

- Found with the outside sampler on the replay of the Si NightveilTrancer Mod (docs/recording-performance.md): every descriptor-set bind compared pipeline layouts field by field; RabbitFX's texture roles were classified at every matched draw (most of its allocations); the instance entry was found by listing every range of set 2. 1.8.7 interns layouts at creation (one comparison per check), keeps the roles per pipeline and its sets' identity and write generation, and looks binding 0 up directly.
- Replay process CPU per frame against the deployed 1.8.6, interleaved, six pairs: 2766 to 2609 us (-5.7%, every pair faster; the driver included). Steady per matched draw before the change: about 28 us, 22 of them the plan, 18 the original interpreter.
- Verification: replay vk04 with the Mod `--check --no-refusal` passed; full suite 41/41 (build/run_all-1.8.7.out in the worktree). Deployed the 18 files (de43c4d) on 2026-10-26 11:26 after two closed-process checks; cfg unchanged; 1.8.6 in build/backups/before-1.8.7-20261026-112646/ (build/candidate-1.8.7.json). Smoke at --api 1.3: 1.8.7, 22 packages, none refused, the Si NightveilTrancer Mod configured, no validation message (build/smoke-1.8.7.json). Not yet seen in the game.

## 1.8.6: the probe's sampler; less shared state written per hook (2026-10-26, D-121 step 4)

- In the 1.8.4 session the layer's work around each of the game's draws came to about 7 us on average, matched or not, and the counters did not say where. 1.8.6 adds the probe's sampler (`profile`, layer/src/sampler.cpp, scripts/profile_game.py, docs/profiling.md) to find that out in the game.
- Two costs every recording thread paid at every hook are gone: the hook call counters were one global atomic per hook (now each thread counts in its own slot and the probe sums them), and the threads' watchdog slots shared cache lines (now one slot per line). The draw hooks moved their record into the command buffer's list instead of copying it (two atomic counts less per bound set on control blocks all threads share).
- Verification: probe end-to-end 17/17 configurations with the sampler checks; a 3 s sandbox profile symbolized; full suite 41/41 (build/run_all-1.8.6.out in the worktree). Deployed the 18 files (2d00ea7) on 2026-10-26 10:06 after two closed-process checks; hashes match; cfg unchanged (`m2_gpu_bones = 1`); 1.8.5 in build/backups/before-1.8.6-20261026-100612/ (build/candidate-1.8.6.json). Smoke at --api 1.3: 1.8.6, 22 packages, none refused, the Si NightveilTrancer Mod configured, no validation message (build/smoke-1.8.6.json). Not yet seen in the game.

## 1.8.5: the Si NightveilTrancer Mod draws; textures as the original (2026-10-25, BUG-053, D-122)

- BUG-053 found offline after the session: the post phase's fresh state lacked the host's render target answer, one refusal ended the generation's GPU side, and the Mod's compute-written positions had no vertex usage. Fixed; the original's built-in sections inserted; each distinct refusal reason logged once per generation. Replay vk04 with the Mod alone: 4444 of 4568 draws replaced, none refused, Present clean, validation clean.
- D-122 (D-121 step 1): with pageable device memory the texture working set is the whole cap; no idle eviction, demotion or reduced load below it.
- Offline tooling: the dry run takes wide path arguments and lists the sections of a Mod whose declarations need the GPU (`declarations_only`); the replay's Mod copy has a plain folder name; a suite step replays the Mod with `--no-refusal`.
- Full suite 41/41 (build/run_all-1.8.5.out in the worktree; the first run caught a wrong first form of the fix, see BUG-053). Deployed the 18 files (b117cfb) on 2026-10-26 09:42 after two closed-process checks; hashes match; 1.8.4 in build/backups/before-1.8.5-20261026-094247/ (build/candidate-1.8.5.json). Then dist/efmi_vk.cfg set back to `m2_gpu_bones = 1` (the 1.8.4 session ran the GPU import about ten minutes with every submission completed and measured -2.8 ms per frame; the probe's `set gpu_bones=0` switches back in a session). Post-deployment smoke at --api 1.3: 1.8.5, 22 packages configured, none refused, the Si NightveilTrancer Mod configured, texture floor = cap (3949 MiB), no validation message (build/smoke-1.8.5.json). Not yet seen in the game.

## 1.8.4 in the game: GPU skinning measured (2026-10-25, PID 26184)

- User-started session with 1.8.4 (CPU bones by the configuration). The bones were switched to the GPU through the probe and watched for 60 s: 3915 submissions, all completed, no failure, no slot wait (build/game-1.8.4-gpu-bones-watch.json). Interleaved A/B in one scene (scripts/ab_game.py --setting gpu_bones, 10 rounds of 3 s, medians; 1044 against 1047 draws per present): mean frame 13.8 ms on the GPU against 16.6 ms on the CPU (-2.8 ms, 17%), layer recording work 4.96 against 7.60 ms per present, frames over 16 ms 18.5 against 97 per window (build/game-1.8.4-ab-gpu-bones.json). The GPU stays on in this session. The first in-game measurement of a performance change that is not confounded by the scene (ISSUE-012).
- The Si Mod was still refused at every draw for rt_width: a depth-only pass (no colour target) left the query unanswered, and the -2 marker re-asked the owned domain. The module now answers -1 for "none bound" (marker -3) and a bound target's size from the framebuffer or the pass's render area; exchanged into the running game.

## 1.8.4: bone dispatches per recording, the two-queue replay, waiting instead of the game's mesh (2026-10-25, D-117 amended, D-119, D-120, BUG-052)

- The 1.8.3 session hung the GPU within two minutes (BUG-052: one timeline signalled from two queues). The deployed configuration was switched to `m2_gpu_bones = 0` (dist/efmi_vk.cfg) so the user can play on the CPU import meanwhile.
- 1.8.4: the dispatches belong to the recording and go to its queue at its submission (the module's submit entry takes the command buffers; per-queue timelines; nothing crosses queues); the replay's two-queue mode and the GPU/CPU picture check in the suite (D-119); the frame gate waits for textures instead of drawing the game's mesh, demotion spares images on screen (D-120); the probe's `set gpu_bones` reaches the module (live_gpu_bones); the crash logger keeps the first eight distinct fault addresses; the stall watchdog names the submission sub-step.
- Found by the new two-queue replay at once: with three frames in flight the snapshot pool ran out (36 draws refused to the game's mesh in 120 frames); a recording now makes its batch where it is needed when none is prepared (as 3DMigoto creates a resource at first use), up to 32 per package (`batches_made_at_draw`). Two-queue replay vk03: no exhaustion, pictures identical with the bones on the GPU and the CPU, 232 of 232 submissions completed on two queues, validation clean. Full suite 39/39 (build/run_all-1.8.4b.out in the worktree).
- Deployed the 18 files (733b363) at 22:35 after two closed-process checks; hashes match; 1.8.3 in build/backups/before-1.8.4-20261025-223508/ (build/candidate-1.8.4.json). dist/efmi_vk.cfg carries `m2_gpu_bones = 0` (added with the game closed after the 1.8.3 crash): a session starts on the CPU import and switches through the probe (`set gpu_bones=1`) while `bones` is watched. Post-deployment smoke at --api 1.3: 1.8.4, 22 packages configured, none refused, no validation message (build/smoke-1.8.4.json).
- docs/3dmigoto-flows.md: the original's flows read from its source (the draw, resources, one space for all Mods, skinning, threads, the play path) and the order of work that follows (D-121).

## 1.8.3: no cross-queue wait for the bone dispatches; rt_width in draw lists (2026-10-25, BUG-050, BUG-051)

- The 1.8.2 session deadlocked on the GPU after two minutes (BUG-050): the bone merge's cross-queue wait against the game's own queue semaphores, the rule of D-091 broken. 1.8.3 never waits across queues (an already satisfied wait, or the same jobs dispatched again on the draws' queue). The Si Mod's draws were all refused for `rt_width` in their lists (BUG-051); the draw request now carries the render pass's colour target size and the INI runtime starts the draw's state with it.
- Full suite 38 steps passed (build/run_all-1.8.3.out in the worktree), character 361/361. Deployed the 18 files (375bc2d) at 21:26 after two closed-process checks; hashes match, cfg unchanged; 1.8.2 in build/backups/before-1.8.3-20261025-212636/ (build/candidate-1.8.3.json). Post-deployment smoke at --api 1.3: 1.8.3, 22 packages configured, none refused, no validation message (build/smoke-1.8.3.json). Game check open: no stall over a session, `bones.redispatches`/`queue_waits`, the Si Mod drawing, the interleaved A/B.

## 1.8.2: the merged skeleton imported on the GPU; arenas sized from the skeleton (2026-10-25, D-117, D-118, BUG-049)

- The 1.8.1 session: EFMITTL translucency worked (24044 captures, 6004 composites), the new Si Mod loaded but never drew (BUG-049), and the user found the game stuttering badly. Per-stage figures of the session (build/game-1.8.1-status-1.json): 50-90 us per matched draw, bones 17-33 us (write-combined reads), plan 15-37 us, the rest about 20 us; about 5 ms of layer work per frame; the game itself CPU-bound under Vulkan (ISSUE-012).
- 1.8.2 moves the bone import to compute dispatches before the frame's draws (D-117: bone_merge.cpp, layer/shaders/bone_merge.hlsl, rings per identity, one submission per frame, the draws bind the ring slot; `m2_gpu_bones`, probe `set gpu_bones`), sizes the arenas' bone copies from the skeleton (D-118), and logs hardware faults with module+offset (util.cpp). Offline: vk03, vk04 (--only-mod Si Lunabelle) and vk01 render identical pictures with the import on the GPU and on the CPU (scratch ab_bones.py, build/replay/*); character 361/361, live INI 76/76; validation clean. The gain shows only in the game: next session, the interleaved A/B (ab_interleaved.py --setting gpu_bones, and m2) in one scene.
- Layer-entry tests crashed inside the validation layer until the bone command buffers got the loader's dispatch pointer (vkSetDeviceLoaderData); the crash logger found it.
- Full suite passed (build/run_all-1.8.2b.out in the worktree); the vk01 step six times without the sporadic validation-layer crash of one earlier run (ISSUE-013). Deployed the 18 files (3e1188f) at 20:57 after two closed-process checks; hashes match, cfg unchanged; 1.8.1 in build/backups/before-1.8.2-20261025-205745/ (build/candidate-1.8.2.json). Post-deployment smoke at --api 1.3: 1.8.2, 22 packages configured, none refused, no validation message, game log unchanged (build/smoke-1.8.2.json). The injected DLL changed with the module (common.h is in the ABI stamp): the game must restart; no module exchange into a running 1.8.1.

## 1.8.1: skeleton components with bones between them (2026-10-25, BUG-048)

- The new Si Mod's 24 components leave bones between them, as the Core allows; the layer required a partition in five places and refused the package. 1.8.1 takes disjoint ranges in order, gaps keep their initial value, a gap bone belongs to no component; the per-package image budget is 1024 (the Mod's INIs declare 83 DDS images). Actual-config sandbox at --api 1.3: all 22 packages configure, the new Si Mod included. Tests: bone 127, character package 94, skeleton metadata 15.
- Full suite 39/39 steps passed (build/run_all-1.8.1.out in the worktree). The shared header stamp is unchanged from 1.8.0 (0xb963e9d2a0e919ba), so 1.8.1's module would exchange into a running 1.8.0. Deployed the 18 files (f4daed7) at 18:38 after two closed-process checks; hashes match, cfg unchanged; 1.8.0 in build/backups/before-1.8.1-20261025-183837/ (build/candidate-1.8.1.json). Post-deployment smoke at --api 1.3: 1.8.1, 22 packages configured, none refused, no validation message, game log unchanged (build/smoke-1.8.1.json).

## First Vulkan game session of 1.7.9 and 1.8.0 (2026-10-25, BUG-046, BUG-047, D-114 to D-116)

- Game session (PID 19920, user-started, 1.7.9 injected by the launcher): EFMITTL translucency worked in the game (27119 captures, 6701 composites, no error). The user saw characters revert to the game's model in the open world and a one to two second wait when switching back (BUG-047: textures waiting for free memory with the device at its budget, idle eviction of party members out of view, 400 MiB EFMITTL layer sets kept). Then the user installed a Si Mod whose folder name holds a full-width colon and pressed F10: the game ended inside DXC; started again, it ended 19 s after start (BUG-046). Windows reported the commit limit during the F10 reload (ISSUE-011).
- 1.8.0: DXC sees ASCII file aliases and a DXC fault is contained; a CustomShader DXC refuses runs as fxc's bytecode through the translation; a CustomShader that compiles nowhere is skipped with the original's warning (D-116). Texture working-set floor under pageable memory (512 MiB here), eviction for free memory, released bytes counted (D-114). EFMITTL layers made per layer when first drawn into, sets released after 5 s idle, `layer_bytes` in status (D-115).
- Verified offline: installed Mods' HLSL 75/75 compiled and validated, 4 through the bytecode translation (the Si shape-key shaders); GPU resources 11201 checks with a new case (a Mod folder outside ASCII, a Length the translation runs, a shader that compiles nowhere skipped), 0 validation messages; full suite all steps passed (build/run_all-1.8.0.out with the build, build/run_all-1.8.0-rerun.out after the test-only changes). The vk04 step now replays a project copy of Si Lunabelle (`--only-mod`): the user disabled that Mod.
- Actual-config sandbox injection with the installed Mods folder: the deployed 1.7.9 ends with 0xC0000005 right after the first package, as the game did; 1.8.0 runs (22 packages, no DXC fault, no validation message, floor 512 MiB). At the game's Vulkan 1.3 (`efmi_vk_sandbox --api 1.3`, new) the new Si Mod's shaders pass the capability gate; its package is still refused: "Skeleton components overlap or leave a gap" (BUG-048: its 24 components leave bones between them, which the Core allows). In the game it shows the original model until that is lifted.
- Deployed the 18 tested 1.8.0 files (17f7db6) at 17:27 after two closed-process checks; hashes match, cfg unchanged; 1.7.9 in build/backups/before-1.8.0-20261025-172754/ (build/candidate-1.8.0.json). Post-deployment actual-config smoke at --api 1.3: 1.8.0, 22 packages, no DXC fault, no validation message, game log unchanged, the new Si Mod refused at its skeleton (build/smoke-1.8.0.json). Main merged by fast-forward with its 19 WIP files replayed line for line (backup build/backups/wip-before-1.8.0-20261025-172715/).
- Not seen in the game yet: the floor and the layer release (`textures.floor_bytes`, `budget_waits`, `evictions`, gated frames, `translucency.layer_bytes`).

## EFMITTL translucency: 1.7.9 (D-113)

- The parts Mods draw through EFMITTL are translucent: the library's INI decides, the layer supplies its hook facts from SPIR-V (lit vertex shader by output interface, copy shader by structure; shadows stay full), records its capture at the end of the forward pass into device layers and composites with the library's own shaders. docs/efmittl-transparency.md.
- Replay vk01 (Yvonne, 120 frames, --copy-passes): 456 captures, 114 composites, the library's $fallback staying 0, Vulkan validation 0 errors (4 performance notes); against 1.7.8's opaque fallback 30265 forward-target pixels differ, the sleeves (alpha 0.2) most, the dress (0.7) less (build/replay-1.7.9/). vk02 (TangTang, translucent variant on): 114 captures, 114 composites. Hook rules on the paired catalogue: the 7 Vulkan vertex shaders pairing with 1479b2b5 lit, 57 others not; copy rule names 4b4220a1 alone. Sandbox only.
- Full suite 39/39 steps passed (build/run_all-1.7.9.out), with the vk01 step checking translucency and the new hook rule step.
- Deployed the 18 tested 1.7.9 files at 2026-10-25 11:21 after two closed-process checks; hashes match, cfg unchanged; 1.7.8 in build/backups/before-1.7.9-20261025-112124/ (build/candidate-1.7.9.json). The injected DLL and the module changed ABI together. Actual-config sandbox injection: 1.7.9, 22 packages, the library loaded by its 3 consumers, normal exit, clean validation (build/smoke-1.7.9.json). No game load; the launcher stays on DX11.
- Not yet seen in the game: the real copy pass, the game's targets and frames in flight, the cost.

## EFMITTL consumers drawn: 1.7.8 (D-112, BUG-045)

- Up to 1.7.7 every part a Mod draws through EFMITTL was missing on Vulkan: the library was in no character generation and its calls were unrecognised entries. A generation now loads the libraries its INIs call by namespace (CharacterProgram::libraries, RuntimeConfig::libraries) and a replaced draw's plan answers `ResourceX !== null` from its references (DrawPlanAdapter, QueryService::declared). The library's own fallback draws the parts opaque from the second frame of a generation; transparency stays LIMIT-038.
- Replay vk01 (Yvonne, 120 frames), 1.7.7 against 1.7.8: covered texels shadow 162226 to 173968, G-buffer 85342 to 92233, forward 44179 to 71365, no refused plan (575 refusals before the declaration answer); vk02 (TangTang) replays with no refused plan.
- Full suite 38/38 steps passed (build/run_all-1.7.8.out): original INI runtime 488, character 361, live INI 76, Vulkan validation 76, GPU resources 11189, EFMITTL owned 356 per path, probe 17 configurations, MCP 50; both earlier replays and the new vk01 library step pass.
- Deployed the 18 tested 1.7.8 files at 22:46 after two closed-process checks (the game and the launcher closed); hashes match, cfg unchanged; 1.7.7 files in build/backups/before-1.7.8-20261024-224635/ (build/candidate-1.7.8.json). Actual-config sandbox injection: 1.7.8, 22 packages, the library loaded by its 3 consumers (Yvonne, TangTang, ardelia), no refusal, normal exit, clean validation, game log unchanged (build/smoke-1.7.8.json). No game load; the launcher stays on DX11.
- New suite step: "captured frame with a library call" (vk01 with --check --no-refusal). Original INI runtime test runtime_library_cases (488 checks).

## EFMITTL candidate: 1.7.7

- Full game support is still absent (D-111, LIMIT-038). The character loader does not share this external library across packages, game ShaderRegex hooks remain inert, and borrowed game programs/targets with ordered pass-local capture/composition are not connected. Do not treat the owned fixture as real-character acceptance.
- Fixed BUG-044: accept core SPIR-V DerivativeControl in the GPU executor. The unchanged plugin's supplied alpha DXBC cache reproduced the refusal; source-only compilation did not reveal it.
- New GPU fixture executes unchanged EFMITTL 0.5.2 INI in separate cached-DXBC and source-HLSL runs: 24 cases and 356 checks each, zero validation errors/warnings. Eight cached shader-stage translations per fixture versus zero on the source path are checked. Pixels/depth cover transparency, four layers/overflow, scene depth, all mask channels/inversion, bilinear wrapping/mips, plain/fading shadows including a nonzero instance, fallback/recovery, resize and neutral screen-space mask fill. All 14 installed inputs remain unchanged.
- The probe now captures framebuffer/attachment/subpass identities, imageless/dynamic targets and secondary inheritance, bounded and armed only during capture. MCP get_draw resolves target_info and handles older captures explicitly. Native tracker: 178/178; updated Python units: 117/117; MCP: 50/50. Historical game captures lack the attachment data needed to prove the game's compositing boundary.
- Built in build/efmittl-next. The 36-step suite completed with 35 passing; the only failure was the old assertion that the image count excludes swapchain images. Updated it to include the reported chain image count; all 17 affected probe configurations then passed. No unresolved failure and no claim of a second all-green monolithic run. Both historical replays, resource GPU 11189, character 361, live INI 76 and Vulkan validation 76 pass. Logs: build/efmittl-run-all-1.7.7.out and build/efmittl-probe-1.7.7.out in the main project.
- Deployed all 18 tested 1.7.7 files, with identical hashes and the unchanged configuration, after two closed-process checks. Previous 1.7.6 files/config/log: build/backups/before-1.7.7-20261024-213603/. Actual-config sandbox injection: version 1.7.7, 23 packages, replacement/profiling enabled, one known attachment context, normal exit (build/efmittl-smoke-1.7.7.json). Candidate/deployment hashes: build/efmittl-candidate-1.7.7.json.
- Stop at the next evidence boundary: a user-started Vulkan session must establish targets/pass ordering before integration. Launcher read-only verification still shows DX11/DIRECT; only the user can authorize its next switch on the spot. Main's overlapping older WIP remains untouched. No real-game load or new game capture. Details: docs/efmittl-compatibility.md.

> Implementation update: 2026-10-24, codex/performance-1.7.5, release 1.7.6 (offline verified; game acceptance pending). User requested the three review fixes, further optimization and complete offline testing before the next game test. The main tree's 21 pre-existing changed/untracked files are preserved; the clean implementation is in build/performance-worktree.

## Previous release: 1.7.6

- 1.7.5 (491107f) fixes BUG-040/041/042: positive index fingerprints checked every draw, pipeline lookup owners retired, draw scratch owners cleared on every exit while retaining capacity. Its 35-step suite passed; build/performance-baseline-1.7.5 is the corrected A/B baseline.
- 1.7.6 batches only selected textures and pipeline variants before waiting, wakes resource waits on completion and variant waits on retirement/shutdown, uses one bounded deadline per batch, and reuses/vectorizes two-pose bone staging (D-110). BUG-043 removes an Image vector reference across an unlocked wait. Ready-only image publication, budgets and draw atomicity remain.
- Complete build in build/performance-next. All 35 offline steps passed: native 158/158, bones 123, cache concurrency 34, character 361/361 including cold materials, live INI 76/76, Vulkan validation 76/76, both capture replays clean. Standalone resource/cache GPU tests: 266, zero validation errors/warnings. Logs: build/performance-build-1.7.6.log, build/performance-run-all-1.7.6.out, build/performance-resource-wait-1.7.6.log.
- Seven interleaved CPU importer rounds against 491107f: median time reduced 86.0-89.1% for 16/64/256/1024-bone identity/remapped imports; all checksums agree. This is cached CPU memory only. Evidence: build/performance-worktree/build/bone-bench/report.json. Three paired replays per scene: recording CPU -1.5%/-2.3%, bone stage -50.7%/-36.7%, initial texture waits -7.6%/-17.0%; final pixels, steady replacement coverage and residency ledgers agree. Host p50 is essentially unchanged (FIFO replay). Against deployed 1.7.4 in vk03, recording is +12.79 us/frame (+3.1%) after restoring hash validation; no lower overall steady CPU/FPS claim. Detailed tables and report paths: docs/performance-followup-1.7.6.md.
- Deployed 1.7.6 with the game/launcher closed (checked twice); 18 files match tested candidate hashes. Backup: build/backups/before-1.7.6-20261024-153223/. Actual-config sandbox injection smoke passes: 23 packages, version 1.7.6, replacement/profiling on, normal exit. Five historical bone frames also pass 283-record byte-exact reference replay (build/performance-bone-replay-1.7.6/report.json). No game load or launcher edit. Configuration unchanged; launcher read back as DX11/DIRECT. Stop here for a user-requested switch and user-started Vulkan session to validate real frame pacing, transitions, reload and ownership. No new capture or real-character verification this round.

> Historical review update: 2026-10-22, main. User asked for an audit of Claude's performance commits, not implementation or a game test. Reviewed b5c12c3 (1.7.4) and preceding performance changes. Runtime/deployment unchanged.

## Performance review of Claude's 1.7.4

CPU savings are supported by the saved three-pair A/B: recording -18.0%, INI
plan -20.6%, INI Present -29.6%. Acceptance against 120 fps remains open; the
offline host-interval median improves 6.5% under uncontrolled concurrent game
load, without a current game comparison or tail-latency acceptance.

Three findings open at that review (now fixed offline): BUG-040 (P1, stable positive index hashes stay stale after
coherent mapped reuse), BUG-041 (P2, the new fast lookup owns retired pipeline
variants), BUG-042 (P2, persistent material scratch owns textures after eviction
and command completion). Native source-extracted/model ownership probes compare
56d2634, 27da667 and b5c12c3: stale hash for 15 frames, 256/256 retired variants
retained, and 3/3 evicted image stand-ins retained respectively. These are CPU
unit reproductions, not measured GPU memory or real-character defects.

See docs/performance-review-1.7.4.md and build/review-performance-1.7.4/. Existing
35-step 1.7.4 suite log inspected, not rerun. Four-second draw-path resource
waits and CPU bone imports remain; the saved development replay has one 158 ms
texture wait. No layer/source fix, build of the layer, deployment, process,
launcher or system change this round. Existing uncommitted 1.3.7 work preserved.
Next implementation should fix and cover the three findings before performance
acceptance. The current user request is review only.

## Earlier state recorded by Claude

> Last updated: 2026-10-22 17:46, branch claude/ini: 1.7.4, deployed to dist at 17:46 with the game closed: the allocations and repeated lookups a sampling profiler found in the recording and the Present (D-108; scripts/sample_profile.py), on top of 1.7.3 (D-107) and 1.7.2 (D-106). To be measured in the next game session against 120 fps. Earlier: 1.7.3, deployed to dist at 15:35 with the game closed: whole-frame index hashing only for an activated package with hash-only sections (D-107), on top of 1.7.2's recording-path cuts (D-106). To be measured in the next game session against 120 fps. Earlier: 1.7.2, deployed to dist at 14:16 with the game closed: the recording path cut offline against the 120 fps bar (D-106: command kinds decided once, native commands unwrapped, pointer caches, a variant lookup, items by resource object, fingerprint backoff; list execution per matched draw 33 to about 15 us in the perlica replay), to be measured in the next game session. 1.7.1: a presentation without a draw (frame generation) replays the last Present's drawing (D-104, BUG-036); F10 rescans the Mods folder (BUG-037); blend-index and weight elements of any width (BUG-038, Typhoeus' clothes); persistent variables per Mod folder (D-105, BUG-039); the INI input sampled once per presentation. All verified in the seventh game session by module exchange and by the suite. 1.7.0: identities survive dashes, cuts and brief occlusion (D-100); material rules for every pixel slot, a VkPipelineCache on disk, 2-4 variant builders (D-101); the layer's own upload queue with draws waiting for their textures and variants, one variant cache per device, the Core's first-frame skip (D-103; a grey placeholder D-102 tried and withdrawn). D-100 and D-101 verified in the sixth game session by module exchange; the rest by the suite, to be watched in the next session (`draw_waits`, `variant_waits`, `gated_frames`). 1.6.9: the recording path measured in the fifth game session and cut through eight module exchanges (reserve 512 MiB, idle 600 frames, fingerprint policy, owner pooling, cached arenas, write ranges per buffer; D-099, BUG-035, LIMIT-037, docs/recording-performance.md): 24.5 ms against a 20.9 ms baseline in the open world, 41 ms before. 1.6.8: after the fourth game session (1.6.7: texture thrash, black characters, F10 freeze, BUG-034): idle-only eviction with paced demotion, a frame gate per package, one index region hash per draw (D-097); the presenting thread never waits for the GPU, controllers held shared during Present work, retired semaphores destroyed later, the frame statistics lock released for the Present work (D-098). 1.6.7: textures loaded on first bind and evicted under a device budget, reduced under pressure, published through a timeline semaphore; memory priority and pageable device memory enabled (D-094, D-095). 1.6.6: second game session froze after eight activations (BUG-033, device memory); packages activate only with memory to spare (D-092), several packages share one recording (D-093), watchdog stages. 1.6.5: first game session of 1.6.4 froze (BUG-032); owned work follows the presentation queue, uploads on any graphics queue, `m2_owned` switch, stall watchdog (D-090); consumer queues wait only for completed publications (D-091, the likely freeze). 1.6.4: sections acting on game shaders load inert (D-089, LIMIT-036; Yvonne and Xaihi load). 1.6.3: GPU work inside replaced draws (P2, D-087; Typhoeus' body and clothes now the Mod's) and the NVIDIA division workaround (D-088, BUG-031). 1.6.2: a Mod's own instance identification over the Core's internals (D-086, LIMIT-034 resolved; Si's guard replays). 1.6.1: application texture references and element offset edits (D-085); the dry run has no refused section. User 2026-10-20: continue offline and do every remaining item. 1.6.0: a Mod's hash-only hide sections (D-084). 1.5.9: texture-only redraw of game draws whose sections only change textures (D-083). 1.5.8: RabbitFX texture roles by classifying the draw's textures (D-082, user's choice; docs/rabbitfx-roles.md), BUG-030 (one odd slot refused a shader's whole material profile). 1.5.7: images published in batches as they become Ready (D-081, BUG-029: one failed image blocked the whole character; unused variants delayed first appearance). 1.5.6: captured frames replayed through the layer with the Mods folder (scripts/replay_capture.py, docs/capture-replay.md): 13 of 16 catalogue characters show the Mod's mesh in every pass their components draw in, 0 validation messages; found and fixed BUG-028 (identification threshold, image budget, dimension policy; D-080); new limit LIMIT-034 (Si's own identification). Earlier, 1.5.5: the 32-bit blend-index edit per draw, namespaces with spaces and INIs without a namespace line. Installed Mods' offline dry run: 21 of 22 load without the owned runtime, 144 component sections draw, 2 refused (an ElementOffset edit, GPU work in a draw). 1.5.4: BUG-027 fixed by D-079 (Core source facts from the vertex module). 1.5.3: Mods read straight from the Mods folder (D-078). User direction 2026-10-19: no game session unless unavoidable. RabbitFX option still open. Not loaded into the game. main follows claude/ini; Codex's uncommitted work stays restored in the main tree. dist: layer 1.0.2 with the probe-only configuration of session B. Launcher on DX11; injected library still dist/efmi_vk_inject.dll.

Deployed 1.6.8 (750f27d) to dist on 2026-10-21 at 08:01 with game and launcher closed (the 1.6.7 files, cfg and the frozen session's log in build/backups/before-1.6.8-20261021-080158; the status read before the freeze in build/backups/session-1.6.7-status-before-freeze.json); the cfg adds `unmap_snapshot_limit = 0` and keeps profile_tracker = 1. Not yet loaded into the game: the user starts it with the account of their choice through the launcher's injection. Earlier: Deployed 1.6.7 (03ed601) to dist at 22:41 with game and launcher closed (1.6.6 files, its session log and status in build/backups/before-1.6.7-*); cfg now with profile_tracker = 1 to measure the layer's CPU cost. Earlier: deployed 1.6.6 (2e33b31) to dist at 20:54 with game and launcher closed (1.6.5 files and its session log in build/backups/before-1.6.6-*). Earlier: deployed 1.6.5 (1803b47) to dist at 19:55 with game and launcher closed; 1.6.4 files, cfg and the frozen session's log in build/backups/before-1.6.5-*; same cfg (owned execution on). Game session of 1.6.4, 2026-10-20 19:10-19:18: the user injected dist/efmi_vk_inject.dll and started the game; it froze at probe frame 1535 shortly after Si's package activated (BUG-032); the user ended it. Launcher still in the Vulkan configuration. Before it: game test of 1.6.4 prepared, 2026-10-20 19:08 (user: test what is implemented in the game; shader-keyed features set aside): dist rebuilt from 2397ef8 (layer 1.6.4, owned GPU tools beside it; the 1.0.2 files and cfg backed up in build/backups/before-1.6.4-*), dist/efmi_vk.cfg in directory mode (m2_mods and m2_core of the installed EFMI, m2_dimension_zero 4318:11545, image_hash 1, m2_enabled 1). At the user's request on the spot the EFMI profile was switched to Vulkan (d3d11_mode_cmd_args empty, xxmi_dll_inject_mode SKIP; game and launcher closed, only the two value spans, exact readback; build/backups/launcher-vulkan-20261020-190826/). The injected library (signed) is still F:\EFMI MCP\dist\efmi_probe.dll; the user sets it to F:\efmi vulkan\dist\efmi_vk_inject.dll in the launcher UI and starts the game with the account of their choice. Not yet loaded into the game.

Launcher state, 2026-10-23 10:00: at the user's request, the EFMI profile switched to DX11: d3d11_mode_cmd_args = -force-d3d11, xxmi_dll_inject_mode = DIRECT; game and launcher closed, checked twice before writing; only the two value spans changed, every other JSON value unchanged, exact readback. Backup and manifest: build/backups/launcher-dx11-20261023-100015/. The injected library (signed) is still F:\efmi vulkan\dist\efmi_vk_inject.dll, which the user sets back to F:\EFMI MCP\dist\efmi_probe.dll in the launcher UI before a DX11 start (a DX11 start with efmi_vk_inject.dll injected crashed on 2026-10-11). Before that, 2026-10-22 10:30: at the user's request ("switch back to Vulkan first"), the EFMI profile switched back to Vulkan: d3d11_mode_cmd_args empty, xxmi_dll_inject_mode = SKIP; game and launcher closed, checked twice before writing; only the two value spans changed, every other JSON value unchanged, exact readback. Backup and manifest: build/backups/launcher-vulkan-20261022-103027/. The injected library (signed) is F:\EFMI MCP\dist\efmi_probe.dll, set by the user for the DX11 session; a Vulkan session with 1.7.0 needs the user to set it to F:\efmi vulkan\dist\efmi_vk_inject.dll in the launcher UI. Before that, 2026-10-21 13:05: at the user's request ("switch XXMI back to DX11 first"), the EFMI profile switched back to DX11: d3d11_mode_cmd_args = -force-d3d11, xxmi_dll_inject_mode = DIRECT; game and launcher closed, checked twice before writing; only the two value spans changed, every other JSON value unchanged, exact readback. Backup and manifest: build/backups/launcher-dx11-20261021-130515/. The injected library (signed) is still dist/efmi_vk_inject.dll, which is the user's to set back in the launcher UI before a DX11 start (a DX11 start with it injected crashed on 2026-10-11); the profile had been on Vulkan since 2026-10-20 19:08 (launcher-vulkan-20261020-190826) for the game sessions. Before that, 2026-10-19 17:35: at the user's explicit request, switched the EFMI profile back to DX11 after catalogue session B: d3d11_mode_cmd_args = -force-d3d11, xxmi_dll_inject_mode = DIRECT; game and launcher closed, checked again before writing; only the two value spans changed, exact readback. Backup and manifest: build/backups/launcher-dx11-20261019-173504/. The injected library (signed) was still dist/efmi_vk_inject.dll at that time; the user sets it back to F:\EFMI MCP\dist\efmi_probe.dll in the launcher UI (a DX11 start with efmi_vk_inject.dll injected crashed on 2026-10-11). dist/efmi_vk.cfg keeps the probe-only configuration without a character package (BUG-026); the earlier one is in build/backups/before-catalogue-b-20261019-170419/.



## 1.7.4: what a sampling profiler found (2026-10-22, D-108)

- scripts/sample_profile.py (`--image replay_gpu.exe --nth 2 --delay 70 --seconds 60 --dump out.json`, started from PowerShell while the replay runs; the Bash sandbox cannot open other processes reliably), the replay with `EFMI_VK_REPLAY_NO_VALIDATION=1`, a profiling build (`EFMI_VK_PHASE_TIMING=1 scripts/build.py --out build/timing2`, the runtime with /Z7). Keep the profiled build's PDB in place: another build overwrites layer/build/*.pdb and the names go wrong.
- Changed in runtime.cpp (snapshot on demand, `replayable()`, kept draw plan, file plan and matches), draw_plan.cpp (kept candidate, external name on keep, `note_timing` slots), resource_plan.cpp/.h (kept result), character_program.cpp/.h (`PixelSlots`, kept containers, `file_plan_`), character_backend.cpp (scratch containers of `record()`, `DrawLookups` with slot answers), resource_commands.cpp/.h (`SubmissionRecycler`).
- Interleaved A/B against 1.7.3 on the perlica replay without validation, three rounds each, medians per frame (the machine under another game's load, so the ratios count): recording 1977 to 1621 us (0.82), the INI plan 1224 to 972 us (0.79), the 22 packages' INI Present 806 to 567 us (0.70), Typhoeus' Present GPU submission 107 to 64 us (0.60), the replay's frame interval 0.94.
- The full suite passed (build/next/run_all-1.7.4.out, 35 steps). Deployed 1.7.4 at 17:46 (the 1.7.3 files in build/backups/before-1.7.4-*). Next session: the frame interval against 120 fps, `stage_us` per package, `record.hash_microseconds`, `fingerprint_read_microseconds`.

## 1.7.3: the activated-only hashing rule (2026-10-22, D-107, deployed at 15:35)

- layer/src/character_backend.cpp: hash-only sections make every indexed draw hashed only while their package is activated (the index's `activated` flags, rebuilt on a change); `fingerprint_read_microseconds` in `record`. Suite passed (build/next/run_all-1.7.3a.out, 35 steps). Released as 1.7.3 (suite build/next/run_all-1.7.3.out, 35 steps) and deployed to dist at 15:35 with the game and launcher closed (the 1.7.2 files in build/backups/before-1.7.3-*). Next session: measure against 120 fps.
- Next offline: the bones on the GPU (merged_skeleton.cpp; 0.4 ms a frame of write-combined reads), the Present lists per package. The character replacement step has a timing flake in one scene's two texture checks that passes on a rerun.

## The recording path cut offline (2026-10-22, 1.7.2, D-106)

- Profiling builds: `EFMI_VK_COMMAND_TIMING=1 scripts/build.py --out build/timing` (every command class, exclusive) and `EFMI_VK_PHASE_TIMING=1` (phases around the plan and the Present); read through the replay's status.json `ini.commands`. draw_plan.cpp (`classify`, `as<>`, the unwrapped natives in `adapt_tree`, `note_timing`), character_core.cpp (`v()`, cached pool, lists and resources), character_backend.cpp (`variant_lookup`, `kStableReads`), character_program.cpp (`stream_index`, `image_index`, hashed `lists_`).
- perlica replay (vk14, 120 frames), per matched draw: list execution 33.2 to 14.5 us, the whole plan 41.5 to 22.4 us; a package's Present 76 to 36 us per presentation; the 22 packages' Present lists 0.48 ms per presentation (with the Core's GUI text of the first minute).
- The full suite passed on the final source (build/next/run_all-1.7.2.out: 34 of 35 steps at the first run, the character replacement step failing two texture-timing checks of one scene (DDS uploaded once, publication reaching the queue) that passed on their own right after and in every earlier run: a timing flake of the upload queue to watch, not of this change; character replacement 357/357 on the rerun, live INI rendering 76/76, pipeline variants 63/63, Vulkan validation 76/76). Deployed 1.7.2 to dist at 14:16 with the game and launcher closed (the 1.7.1 files in build/backups/before-1.7.2-*). Next session: the frame interval against 120 fps, `stage_us` per package (plan, bones, variants), `record.hash_microseconds`.
- Next offline: bones on the GPU (a remap table, no write-combined reads), the Present lists per package, the index hashing of unmatched draws.

## The seventh game session and 1.7.1 (2026-10-22, D-104, D-105, BUG-036 to BUG-039)

- 1.7.0 in the game (PID 20568, 10:32-): the key panel flickered in fullscreen (four to ten presentations per rendered frame, frame generation; the Present work ran on rendered frames only) → D-104 in character_backend.cpp/character_present.cpp/runtime.cpp (`replay`, `backbuffer_targeted`, `last_drew_backbuffer`, `presents` per generation), exchanged and confirmed steady. F10 did not take a swapped Mod folder → `character_rescan` on an F10 (BUG-037). Typhoeus' clothes variants hollow/spiky/invisible → BUG-038 in character_package.h/.cpp and character_program.cpp (`blend_index_format`, `blend_weight_bytes`, float weights in `stream_bones`, the live edit replacing any-width BLENDINDICES in `vertex_input`), reproduced by `scripts/replay_capture.py --settings` with vk06. The new Yvonne Mod inherited the old one's settings → D-105 (settings file `<namespace>-<folder>.ini`). The keys, window and cursor sampled once per presentation (`character_frame`).
- Frame rate measured in the session: 91 fps with the replacement, 120 without; per rendered frame 3.9 ms of recording on the rendering thread (INI plan 2.0 ms = 67 us per matched draw, bones 0.4, index hashing 0.65, variants 0.16, snapshot and emit 0.3) and 1.7 ms of Present work per presentation before the input sampling change, about 0.5 ms after.
- The full suite passed on the final source (build/next/run_all-1.7.1.out: 35 steps; character replacement 357/357, live INI rendering 76/76, pipeline variants 63/63, Vulkan validation 76/76, both replays clean); the vk06 replays with the control and the variant settings drew Typhoeus' clothes and stockings. Deployed 1.7.1 to dist at 12:29 with the game and launcher closed (the 1.7.0 files, cfg and the seventh session's log in build/backups/before-1.7.1-*).
- Next: the INI plan cost offline through the replay (command kinds decided once, about a hundred commands per matched draw); bones on the GPU; owned work reading the game's bindings (LIMIT-035, Typhoeus' physics); Si's INI queries of game shader declarations (LIMIT-036) stay inert.

## The sixth game session and 1.7.0 (2026-10-21, D-100, D-101, D-103)

- 1.6.9 in the game (PID 20688, 09:45-): 17.5 ms frames (p95 19.5, none over 33 ms in ten minutes), recording 5.2 ms per frame, 21 textures resident at full size, 0 evictions. The user: characters at close range still revert for a moment; a character switched in the menu shows the game's mesh for two seconds, then flickers two or three times; the bar is 3DMigoto's experience exactly with no frame-rate loss (memory efmi-vulkan-parity-goal).
- D-100 (identities: a cell of tolerance within a recording, 10 s expiry `m2_identity_expiry_ms`, the lone undrawn identity adopted) and D-101 (material rules for every slot, derived once) exchanged into the running game and verified by the counters and the log: 8 identities in three minutes instead of 42 in ten, `incomplete` 19392 to 5, no rule derived twice. A grey placeholder (D-102) tried and withdrawn at the user's word.
- D-103 in the layer (needs a restart): layer.cpp requests an upload queue of its own at device creation (Device::upload_queue/upload_family/upload_mx); image_upload/buffer_upload share the objects between the families; resource_cache.cpp runs three loaders that submit on that queue and publish through the device timeline, retrying only an aggregate budget held by the others (BudgetBusy, ten seconds at most); texture_residency.cpp bind() waits up to 4 s for a texture on its way (`draw_waits`), the timeline retired on stop; variant_cache await() with 2-4 builders and the disk VkPipelineCache (pipeline_variant.cpp thunk); character_backend.cpp: one VariantCache per device, first-frame skip as the Core's SpatialIdentification, `preparing`, counters `identities_adopted`, `gated`, `variant_waits`, status `pipeline_cache`, `upload_queue`.
- The full suite passed on the final source (build/next/run_all-1.7.0.out: 35 steps; character replacement 357/357, live INI rendering 76/76 with the broken-DDS candidate refused, pipeline variants 63/63, Vulkan validation 76/76, both replays clean). Deployed 1.7.0 to dist at 13:40 with the game and launcher closed (the 1.6.9 files, cfg and the sixth session's log in build/backups/before-1.7.0-*). Next session: watch `textures.draw_waits`/`draw_wait_max_microseconds`, `record.variant_waits`, `gated_frames`, `identities_created`, the frame interval, and whether a switched character is the Mod from its first frame.
- Next offline (through the replay's `stage_us`): the INI dispatch (command kinds decided once), bones on the GPU, the Present lists' cost, the tracker's per-draw bookkeeping.

## The fifth game session and 1.6.9 (2026-10-21, D-099)

- The user ran 1.6.8 (PID 31884, 08:06-): slow character switches, LOD1 reverts, half the baseline frame rate. Eight module exchanges in the running game fixed the texture reserve and idle threshold, the hash table and fingerprint reads, the owner churn of arenas, the tools lookup, and added stage timing; the layer gained write ranges per buffer (region generations). Measurements and the result (24.5 ms against 20.9 ms baseline) in docs/recording-performance.md.
- Verified: each exchange by the probe's status in the game; the whole by the suite (build/next/run_all-1.6.9.out). Deployed 1.6.9 (356633d, build/next copied) to dist on 2026-10-21 at 09:42 with the game and launcher closed; the 1.6.8 files, cfg and the session's log in build/backups/before-1.6.9-20261021-094241 (the log also as session-1.6.8-*.log); the cfg comment names 1.6.9, the values are unchanged. Not yet loaded: the user starts the game with the account of their choice through the launcher's injection.
- Next (offline, through the replay's `stage_us`): the INI dispatch (command kinds decided once), bones (GPU-side remap), one variant cache per device with a VkPipelineCache on disk, the Present lists' cost.

## After the fourth game session (candidate 1.6.8, 2026-10-21, D-097, D-098)

- Session 4 with 1.6.7 (22:46-22:52): 17 packages activated; textures thrashed (591 loads, 574 evictions, 13519 budget waits against a 356 MiB target), characters turned black now and then, frames 23.8 ms mean / 56 ms p95, the layer's draw work 24 us per draw (per-package hashing), 26 GB of probe shadow copies; F10 froze the game with both threads inside the layer's stages (BUG-034).
- 1.6.8: layer/src/texture_residency.h/.cpp (idle-only eviction, paced demotion, loads beyond the target within the cap and half the reserve, `ensure`), character_backend.cpp (frame gate per package, device index by hash with a per-thread fingerprint table, controller held shared during Present work, `gated`/`gated_frames` counters), character_present.h/.cpp (non-blocking `follow`, semaphores retired to the device), state.h/.cpp and layer.cpp (retired semaphores destroyed on the presenting thread, `present_mx` so `frame_mx` is held around the statistics only, `present_work` timing category), work.h.
- Evidence: Si replay (vk04) with a 200 MiB cap at 360 frames: 11 images resident (2 reduced), 184 MiB, 0 evictions (1.6.7 evicted and reloaded throughout), 1 demotion, 579 budget waits while the set came up, 63 gated frames (2353 draws) before the forward pass was fully covered, 9376 replaced draws, validation clean (`--check` passed); character replacement 357 checks, live INI rendering 76 (the broken-DDS reload candidate refused as before); full suite build/next/run_all-1.6.8.out. Sandbox only; to be verified in the game with `unmap_snapshot_limit = 0` in the configuration.

## Texture residency (candidate 1.6.7, 2026-10-20, D-094, D-095)

- User, after the third game session (three characters only, stutter): the design is not acceptable as it stands; DX11 runs twenty Mods; study 3DMigoto's loading, use Vulkan's own features, performance to the limit.
- layer/src/texture_residency.h/.cpp (new, device-wide: declare under a tag, bind on the draw path, budget target from VK_EXT_memory_budget, LRU eviction, demotion to reduced loads, timeline publication, status), character_backend.cpp (per-package image machinery removed; image_ids; preload_matching and refusal of a reload candidate with a failed file; activation gate counts mesh and batches only), frontend/assets/image_upload.h/.cpp (skip_levels, memory priority, timeline signal), buffer_upload (timeline signal), resource_cache.h/.cpp (tagged declarations, serial, release, submit with timeline), layer/src/layer.cpp and state.h (timeline, memory priority, pageable features), draw_snapshots.cpp (arenas in host memory), common.h/util.cpp (m2_texture_budget_mb, m2_texture_reduce_mb, m2_texture_preload, m2_pageable), build.py (module and test sources), tests (character_test, runtime_test, check_character_assets, variant_gpu: publication expectations through `textures`, preload in tests).
- Also D-096: a device-level index of draw counts to packages (character_backend.cpp, CharacterBackend::Index), one lookup per draw instead of one per package; sandbox/snapshot_test.cpp expects arenas in host memory.
- Evidence: Si replay with a 200 MiB cap: forward pass fully covered, 11 resident (2 reduced), 184 MiB, validation clean; Typhoeus replay 12 of 20 images loaded (229 MiB instead of 770), 9301 replaced at 240 frames; character replacement 357, live INI 76 (broken-DDS candidate refused); full suite build/next/run_all-1.6.7b.out. Sandbox only.

## Device memory and shared recordings after the second game session (candidate 1.6.6, 2026-10-20, D-092, D-093, BUG-033)

- layer/src/character_backend.cpp (memory_allows_activation, activation_bytes, counter waiting_memory, owner_for at both recording sites, StageMark in submit, Present and draw), state.h/.cpp (Device memory budget query, ThreadTrace stage, StageMark, CmdBuf::owners), command_owner.h (owner_for), m2_backend.cpp, layer.cpp (stages of submission and presentation), common.h/util.cpp (m2_reserve_mb).
- Evidence: replay of vk03 with m2_reserve_mb = 100000 leaves the package waiting ("needs 726 MiB ... 5845 MiB free"); full suite passed (35 steps, build/next/run_all-1.6.6.out). Sandbox only.

## Queues and the stall watchdog after the first game session (candidate 1.6.5, 2026-10-20, D-090, BUG-032)

- layer/src/state.h/.cpp (ThreadTrace, HookInside, watchdog thread, waits counted), layer.cpp (inside marks on submission and presentation), common.h/util.cpp (m2_owned), character_present.h/.cpp (follow the presentation's queue, queue_changes), character_backend.cpp (uploads and publications on any graphics queue; a consumer queue waits only for a completed publication, D-091), frontend/gpu/resource_commands.h, frontend/xxmi/gpu_resources.h, owned_runtime.h/.cpp (rebind), frontend/assets/resource_cache.cpp (no fixed submission queue).
- Evidence: full suite build/next/run_all-1.6.5b.out (the split-queue test now expects a queue to wait for a batch once it completed, D-091; the upload queue index names the queue of the last upload). Sandbox only; the fixes are not yet verified in the game.

## Sections acting on game shaders (candidate 1.6.4, 2026-10-20, D-089)

- frontend/xxmi/runtime_parser.cpp (ShaderOverride/ShaderRegex inert with a warning), draw_plan.h/.cpp (plan_shader_queries), runtime.h (shader_stages), character_core.cpp (`vs`/`ps`, `oN`), layer/src/character_backend.cpp (pipeline stages), sandbox/runtime_test.py (the acting section loads and is named), sandbox/xxmi_runtime.cpp and resource_gpu.cpp (the refusal examples are now an unexecutable CustomShader; an acting ShaderOverride loads inert and is reported).
- Also: `null` in a plan reads as no resource (-0, identity 0); `o0 === null` refused every plan before.
- Evidence: replays vk01 (Yvonne) 8071 replaced, vk16 (Xaihi) 10072 replaced, both without refusal or validation message at 360 frames; the inert sections are named in the log. Full suite passed (35 steps, build/next/run_all-1.6.4b.out). Sandbox only.

## GPU work inside replaced draws (candidate 1.6.3, 2026-10-20, D-087, D-088)

- frontend/xxmi/draw_plan.h/.cpp (draw_scope, owned_bindings, owned contents, owned_only, routing inside CustomShaders, queries by side), gpu_resources.h/.cpp and gpu_parameters.cpp (ExecutionSite::Draw, owned_vertex, store refusal, failure only after recorded work), owned_generation.cpp, owned_runtime.h/.cpp (begin_draw, pending, queue), frontend/assets/file_plan.h, resource_plan.cpp, file_draws.cpp, frontend/gpu/resource_commands.h (pending), driver_workarounds.h (new), shader_commands.cpp and graphics_commands.cpp (module words), frontend/xxmi/runtime.h (render targets in RuntimeDraw), character_core.cpp (`@oN`), layer/src/character_backend.cpp (plan_draw, submit_draw_work, render targets, owned vertex streams, counter owned_streams), character_present.h/.cpp (submit_pending, draw_work_submitted), character_package.h/.cpp, character_program.cpp; sandbox/replay_gpu.cpp (fatal exception stack), scripts/replay_capture.py (dbghelp).
- Evidence: vk06 replay 360 frames: 14130 replaced (10300 before), no refused draw, owned_streams 19354, draw work submitted 359 times, validation clean; the G-buffer shows the Mod's whole body. Driver fault isolated with a standalone pipeline tool (scratchpad). Full suite: build/next/run_all-1.6.3.out. Sandbox only. docs/draw-gpu-work.md.

## A Mod's own instance identification (candidate 1.6.2, 2026-10-20, D-086)

- frontend/xxmi/runtime.h (RuntimeDraw::instance_entry), runtime.cpp (DRAW_NUMBER per planned draw), draw_plan.h/.cpp (plan_resource_queries, sticky keep_original), character_core.cpp (instance entry as Original_VS_CB through BufferQueries, native identification writes the custom result, native Component_DrawInstances honours original/skip), layer/src/character_backend.cpp (entry in the request, counter kept_original), scripts/build_xxmi_runtime.py (buffer_queries.cpp in the module), sandbox/run_all.py (vk04 replay step).
- Evidence: vk04 replay with Si_BossGuard.ini: 10334 replaced at 360 frames, --check passes at 120 and 360; the guard's pool and the Core's spatial pools filled; a project-owned copy whose guard rejects every object keeps the game's draws (kept_original 4998, replaced 0). Full suite: build/next/run_all-1.6.2.out. Sandbox only.

## Application textures and element offsets (candidate 1.6.1, 2026-10-20, D-085)

- User: "continue offline, do every item" (ember, chengqianyu, Si, Typhoeus P2, Yvonne/Xaihi ShaderOverride).
- frontend/xxmi/draw_plan.h/.cpp (application_textures stand-ins, PlanItem::application_slot, fetch batches of pixel references), frontend/assets/file_plan.h, file_draws.cpp, frontend/xxmi/resource_plan.cpp, frontend/xxmi/character_core.h/.cpp (element_offsets in discovery and facts), layer/src/character_package.h/.cpp (CharacterDraw::offsets in vertex_input), character_program.h/.cpp (offset edits, application slots), sandbox/character_test.cpp (2 checks).
- Evidence: dry run 146 drawn, 0 refused (ember 79b78a0f and chengqianyu fc19ffe3 plan); character package 93; full suite rebuilt and passed (35 steps, build/next/run_all-1.6.1b.out). Sandbox only.

## Hash-only hide sections (candidate 1.6.0, 2026-10-20, D-084)

- frontend/xxmi/character_core.cpp (discovery of hash-only sections that skip; registry check treats a none count as any), layer/src/character_backend.cpp (hash_only sections, matched for activated characters), sandbox/character_test.cpp (dry run count), scripts/replay_capture.py (selection by hash alone). Five installed sections (xaihi 3, chengqianyu 1, last rite 1) now plan as hidden in the dry run (76 to 81).
- Evidence: replay of arcane with a copy whose 9e31e918 section lost its count: matched 10439 and hidden 6119, as with the original Mod; full rebuilt suite build/next/run_all-1.6.0.out. Sandbox only.

## Texture-only draws (candidate 1.5.9, 2026-10-20, D-083)

- frontend/xxmi/draw_plan.h/.cpp and frontend/assets/file_plan.h, resource_plan.cpp (pre_items), layer/src/character_package.h (CharacterDraw::texture_only), character_program.h/.cpp (texture-only lists), pipeline_variant.h/.cpp and variant_cache.cpp (FragmentPatch::keep_vertex), draw_snapshots.h/.cpp (append_images), character_backend.cpp (record_textures, counter textured), sandbox/variant_gpu.cpp (6 checks), character_test.cpp and check_mods_dry_run.py (texture-only in the dry run). docs/texture-only-draws.md.
- Evidence: pipeline variant scenes pass with the texture-only pixel check; dry run 4 fixed-slot sections texture-only; replay of Liino, rossiy, arclight, zhuangfangyi: 1694/2365/298/1184 texture-only draws, 0 validation messages, every pass still shows the Mod; full rebuilt suite build/next/run_all-1.5.9.out. Sandbox only.

## RabbitFX texture roles (candidate 1.5.8, 2026-10-20, D-082, BUG-030)

- User chose option 2 (classification by textures) and to continue offline. Rule: diffuse = first sRGB asset texture sampled; lightmap = linear BC texture of its size right after; normal = first BC5 after, else a sample decoded as a normal (x2, square root in its data flow) within 100 instructions, else the next linear BC. server/efmi_vk/texture_roles.py and frontend/gpu/texture_roles.cpp (C++ equals Python on all 83 catalogue modules); scripts/check_texture_roles.py with docs/texture-roles-truth.json (suite step): agrees with RabbitFX on 462/474/457 of 480 draws, all three roles on 117 of 128 patterns, the 11 others are RabbitFX's misreadings (named).
- Runtime: generations carry a native RabbitFX namespace (frontend/xxmi/rabbit_textures.h): SetTextures runs CommandListResolveRoles (native, asks RuntimeDraw::texture_roles) and flat `if $diffuse_slot == N` blocks binding the Mod's maps at t0..t31; glow/FX/hue not reproduced. Backend answers per draw from cached fragment samples and the bound images (status texture_role_queries/draws).
- BUG-030 fixed: a slot that is a buffer/array or an unpatchable image in one shader no longer refuses that shader's whole profile (rule_rules, MaterialProfile::prepare); only a draw binding it is refused.
- Replay: textures keep the recorded size/mips (--small-images for the old 4x4), derivation failures are logged. 16 bundles, 360 frames: 0 validation messages, 13 characters replaced in every pass, roles asked in Liino, Typhoeus, mifu, rossiy, perlica; material_inherited 0 everywhere. Not yet: RabbitFX/texture changes in sections that keep the game's mesh (texture-only replacement of game draws).
- Evidence: texture roles check, character package 91, asset/material 261, full rebuilt suite build/next/run_all-1.5.8.out. Sandbox only.

## Images published in batches (candidate 1.5.7, 2026-10-20, D-081, BUG-029)

- layer/src/character_backend.cpp: per-image publication flags; up to 16 batches, each with one semaphore per graphics queue and fences made at setup; consumer queues wait once per batch; material draws retain only published views and prioritize what they wait for; status image_publications, images_published_count. frontend/assets/resource_cache: prioritize(). sandbox/asset_cache_test.cpp (reorder), sandbox/character_test.py (waits = batches x queues). scripts/replay_capture.py --mods (a project-owned Mods copy).
- Evidence: resource cache 257 checks; character replacement 357; replay of arcane with one truncated diffuse map in build/broken-mods: 8 of 9 images published in 8 batches, 2352 replaced, 1192 forward draws refused for the broken image; full rebuilt suite build/next/run_all-1.5.7.out. Sandbox only.

## Captured frames replayed through the layer (candidate 1.5.6, 2026-10-20, D-080, BUG-028)

- scripts/replay_capture.py and sandbox/replay_gpu.cpp rebuild the draws an installed Mod's sections match from a catalogue bundle (game pipelines, SPIR-V, descriptors, uniform snapshots) and run them for N frames on a real swap chain in a child process with the layer and the Mods folder, validation on. Stand-ins for what bundles lack: forged index regions with the same CRC-32C, zero vertex and storage buffers, upright bones at the instance entries' rows (character meshes are stored Z-up), grey images, one depth per format shared across passes. Diagnoses: --rp, --open-tests, --probe-vertices. docs/capture-replay.md.
- Found (BUG-028, fixed by D-080): identification waited for 4 components while EFMI exports set 3 or 2 (and adapted lists hid the assignment); a fixed 512 MiB image budget below a Mod's ~600 MiB; no dimension policy in directory mode. Now: the Mod's own identification_min_components (dry run: 2 for 3 Mods, 3 for 8, 4 for 10), a quarter of the largest device-local heap clamped to 512 MiB..2 GiB, `m2_dimension_zero = vendor:device`.
- Replay of all 16 bundles: 13 characters replaced in every pass with a component draw (prepass, G-buffer, forward with the Mod's textures); Yvonne and Xaihi refused (acting ShaderOverride), Si refused at its draws (LIMIT-034: its BossGuard INI runs the Core's spatial identification itself; resolved in 1.6.2, D-086). Typhoeus' chest compute deformation stays the game's (P2). A Mod's ~600 MiB of DDS takes seconds from a cold disk before its materials publish; the game's draw stays meanwhile.
- The G-buffer fragment shaders read DX11 t0/t1 only, so Mods' textures at t16..t19 reach the forward pass only, as under DX11.
- Suite step "captured frame replayed with the Mods folder" (arcane, 300 frames, --check). check_mods_dry_run.py asserts every loaded Mod states an identification threshold.
- Evidence: build/replay/<bundle>/summary.json, build/replay/characters.png, full rebuilt suite build/next/run_all-1.5.6.out. Sandbox only; not loaded into the game.


## 32-bit blend indices, namespaces (candidate 1.5.5, 2026-10-20)

- A live draw under `vb2->ElementFormat(BLENDINDICES, 0) = R32G32B32A32_UINT` (DXGI 3) is accepted: CharacterDraw carries the draw's blend format, vertex_input widens binding 2's R8G8B8A8_UINT element to it, and the stream's bone indices are adopted as 32-bit (CharacterStream::bone_index_bytes; same checks as for 16-bit). chengqianyu's component 2 plans now.
- CharacterCore accepts any lower-case printable ASCII namespace without separators (last rite declares `namespace = last rite`); discovery reads an INI without a namespace line under its file name, the settings file gets a plain name.
- A refused layout edit now names the edits (chengqianyu's LOD1 section: ElementOffset of NORMAL and TEXCOORD6, not supported).
- Evidence: character package unit checks 83 (32-bit widening, 32-bit bone adoption), runtime_test implicit namespace 3 checks, Mods dry run 21 load / 144 drawn / 2 refused (known limits), full rebuilt suite (build/next/run_all-1.5.5.out). Offline only.

## Core source facts, offline plans of the installed Mods (candidate 1.5.4, 2026-10-20, D-079, BUG-027)

- `efmi_vk_character_test --plan-mods <Mods> <Core> <settings>` runs every character INI through discovery and the live plan of each section (no GPU). Its first run showed every component section "not replaced": EFMI exports draw only under `if $\EFMIv1\bones_data_source != 0 && $\EFMIv1\instance_config_vs_cb_slot != -1`, and the backend never supplied those facts (BUG-027). The sandbox fixture has no such guard; the live path had never met a real Mod in the game.
- D-079: frontend/gpu/source_registers (block sizes, buffer strides, core_source_facts) and server/efmi_vk/source_slots.py (core_facts) apply the Core's rules (first of vs-cb2/cb1/cb3 with 4096 vectors; vs-t0 a 16-byte structured buffer) to the D-076 registers; the backend keeps them per pipeline only where they name the window at set 2, binding 0 and the bone site. scripts/check_core_facts.py with docs/core-facts-truth.json (suite step): the replacement decision agrees for all 57 catalogue vertex shaders, slot and window for the 49 with rolling bones; C++ and Python agree on all 57 modules.
- Also from the dry run: component and LOD assignments sit inside `if` branches (discovery reads them there); bone streams take the element offset from the draw's pipeline (Typhoeus' 24-byte blend stream), the packager's rule only without one.
- scripts/check_mods_dry_run.py (suite step): 20 of 22 load without the owned runtime; 137 component sections draw (110 with the Mod's images), 23 hide, 70 hide sections apply; refused only for known limits (chengqianyu's R32 blend edit, one ember section with GPU work in a draw).
- Evidence: character package unit checks 79, unit tests 114, full rebuilt suite (build/next/run_all-1.5.4.out). Sandbox and offline only.

## Character Mods straight from the Mods folder (candidate 1.5.3, 2026-10-19, D-078)

- Config `m2_mods` (EFMI_VK_M2_MODS), optional `m2_core`, `m2_settings`. Every enabled INI with `[CommandListInitializeMergedSkeleton]` is a lazy generation; `CharacterCore` in discovery mode reports the contract (override registry sections with constant matches and the component/LOD constants their command lists assign; bones, components and LOD1 remaps from the original Core initializer) and `CharacterProgram::discover` makes the package: every declared mesh file a stream, remap files remap streams, DDS files the image catalogue, the blend-index edit; then the package decoder. Bone streams are adopted when a live plan binds them under the blend-index edit (CharacterPackage::adopt_bones, the packager's rule); material slots follow the live plans (refresh_materials re-derives rules for a grown slot set).
- Sandbox, installed Mods folder (no draw matches): 22 found, 19 load in 3.7 s, 519 MB working set; refused xaihi and yvonne (acting ShaderOverride sections), last rite (no namespace). chengqianyu loads but its R32 blend edit should make its plans refuse (contract reading, not exercised). Nine reference RabbitFX.
- Correction to 1.5.1: no installed INI has a texture-hash TextureOverride; the 1.5.1 gain came from accepting mesh sections outside the package; texture identity only lets three hash-only `this = null` index sections pass (D-077 updated).
- Evidence: runtime_test directory generation 10 checks (fixture Mod, another Mod, a DISABLED copy: keys, F10, exchanges, pixels as with the package; the other never on the GPU; settings in the configured folder; nothing written into Mods); live INI 71/71; full rebuilt suite 31/31 steps (build/next/run_all-1.5.3.out). Sandbox only.

## RabbitFX assessment (2026-10-19, docs/rabbitfx-assessment.md)

The archived DX11 pixel shaders of session A are already patched by RabbitFX (t70 to t74 in their code), which gives its roles as ground truth: 91 of 163 patched; diffuse registers t11 to t21. scripts/rabbitfx_assessment.py pairs them with the Vulkan fragment modules (83 pairs): diffuse is the first SPIR-V image sample in 79, lightmap always the second when present (34), normal third (36), second (16) or first (3). SPIR-V block sizes are full where DX11 declares used ranges, so RabbitFX's shader selection is not reproducible from them. A candidate rule gets diffuse 79/83, lightmap 75/83, normal 54/83, all three 52/83. Options for the user: diffuse-only by the first sample, texture classification by format (WWMI-DX12's approach), or keeping/refusing RabbitFX Mods until verified.

## Meshes of other INIs, inert shader sections (candidate 1.5.2, 2026-10-19)

- layer/src/character_program.cpp: a mesh file the generation's INIs reference that is not in the package's stream profile no longer refuses the package (zhuangfangyi's second INI `fangyi ul.ini` draws its own meshes at objects the package has no part for). Those sections are not planned, the game draws them; a replaced draw binding such a file still refuses in plan(). Logged as "N referenced mesh file(s) outside the package".
- frontend/xxmi/runtime_parser.cpp: without shader hooks, a ShaderOverride with only hash, filter_index and allow_duplicate_hash is accepted (perlica's hair sections): it runs no command, and nothing can read its filter_index without shader facts (a ps/vs operand is no CPU expression). One with any other key (handling = skip, commands) still refuses the package (xaihi's `ps skip.ini`). The three frontend tests that used a bare ShaderOverride as the unexecutable candidate now use one with handling = skip; a new check loads the inert form.
- 13-package smoke: 12 load; xaihi refused. docs/several-packages.md.
- Evidence: live INI 61/61 (new: another INI's mesh outside the package, inert and acting ShaderOverride in another INI of the Mod folder; the outside-mesh check failed on 1.5.1 with zhuangfangyi's refusal), original INI runtime 484; full suite: rebuilt run passed every step but the original INI runtime (a third bare-ShaderOverride test), then after that test fix all 30 test steps passed with --no-build on the same layer build (build/next/run_all-1.5.2.out). Sandbox only.

## Texture identity in the layer (candidate 1.5.1, 2026-10-19, D-077)

- The tracker computes, at each complete upload of mip 0, slice 0, the original's data hash beside its own content CRC (the first W * H * layers bytes when shorter than the level, rows without padding); layer/src/texture_identity.cpp builds the D3D11_TEXTURE2D_DESC from the Vulkan image (DXGI format table, bind flags from the usage, TEXTURECUBE for cube-compatible images) and hashes it with that seed, or seed 0 for an image never uploaded from a buffer. No hash with image content tracking off, for 1D/3D images, unmapped formats or partial/unreadable uploads. Captures carry `dx11_hash` beside `content`.
- Per replaced draw the INI plan asks for the texture at a DX11 pixel slot: the fragment module's registers (D-076, cached per pipeline) give set and binding, the draw's tracked descriptor the image. Unknown cases answer a bound slot without a hash (no override matches; the draw is still replaced); status texture_queries, texture_unknown, texture_unknown_reason per package.
- Frontend: RuntimeConfig::texture_identity and RuntimeDraw::pixel_texture; DrawPlanAdapter texture queries and the GPU runtime's resource matcher now coexist (the matcher answers inside owned scopes). CharacterCore lets the original OverrideTextures run when the host answers, accepts texture-hash sections, and leaves mesh sections the package has no part for (LOD beyond the packager's scope) to the game.
- 13-package sandbox smoke (build/characters, no draw of the sandbox matches): 10 configure (akekuri, arcane, arclight, avywenna, estella, laevatain, mifu, rossiy, si_lunabelle, tangtang), 3 refused: perlica and xaihi (ShaderOverride/ShaderRegex sections), zhuangfangyi ("Referenced INI mesh is outside the prepared stream profile", masked before). Five load with original warnings for RabbitFX references (the library is a shader-regex Mod in its own folder, LIMIT-004): their RabbitFX textures do not bind.
- Evidence: live INI 53/53 (Core's OverrideTextures finds the scene's texture by its description-only hash; the same description over other data does not match; in all four configurations), probe end to end with the image's DX11 hash from a padded upload checked against an independent Python computation, full rebuilt suite 31/31 steps (build/next/run_all-1.5.1.out). Sandbox only; not seen in the game.

## 13 real packages in the sandbox; DX11 texture hashes from Vulkan uploads (2026-10-19, D-077)

Loading all 13 directory packages into the sandbox program (no draw of it matches a character): 2 s, 364 MB, exits normally, but every package is refused at INI load: 11 with "Native override registry refused: Generation contains an unprofiled override; source-image identity is unavailable" (texture-hash TextureOverrides), 2 with "INI declares an unsupported shader/overlay section" (ShaderOverride/ShaderRegex, phase 4). So texture identity is the next general mechanism. D-077, docs/texture-identity.md: the original hash is crc32c of the 44-byte D3D11_TEXTURE2D_DESC seeded with the data hash of the first subresource; checked on 506 of 506 DX11 shader-resource textures, and from Vulkan image records (format mapped to DXGI, usage 0, bind 8, misc 0, the layer's mip 0 CRC) on 345 of 347 BC/R8 textures with a DX11 counterpart (the two others explained: cube map, mip count). Uncompressed formats need the first W*H bytes (rule known, not yet checked). Next: compute the hash in the layer at upload and give the character program a texture identity provider, so texture-hash overrides and Core's OverrideTextures run.

## Several packages at once, packages without captures (candidate 1.5.0, 2026-10-19)

- scripts/check_mod_corpus.py plans every installed character INI (implicit namespace = file name; a planner that stops is recorded, not fatal): 21 of 22 skeletons, 14 of 22 callback fixtures. scripts/package_directory.py (new) builds each planned Mod's package with static pixel bindings and no fragment profiles (package_character.py --no-profiles; components without an entry section get empty lists; entry sections beyond LOD1 are left out), its live sidecar, and checks it with the production decoder: 13 of 22 packaged (build/characters/directory.json). The decoder accepts a material envelope without profiles (D-076).
- m2_package may name a directory: every *.bin with a .runtime.json sidecar is a package; the module's backend holds one controller per package, draws go to the first whose section matches, Present work runs package by package. GPU residency is lazy for a directory: mesh, arenas and images go up on a package's first match (all 13 at once would be about 2.7 GB of images). Status: the first package on the GPU at the top, every package under "packages".
- Not done: releasing packages, one engine for the whole directory, the eight Mods planning cannot isolate, chengqianyu's R32 blend-index edit.
- Evidence: live INI 45/45 including two packages in a directory (the other character's package sorts first, never activates, uploads nothing; the scene's keys, reloads, module exchanges and pixels as with one package); character package 74, character replacement 357/357; full rebuilt suite 31/31 steps (build/next/run_all-1.5.0.out). Sandbox only.

## BUG-026 fixed (candidate 1.4.9, 2026-10-19)

A refused character init now abandons the backend: the INI program and its presenter are dropped, so no presentation reaches a half-made program; the refusal is logged as before. Arenas: a package with a live INI program gets every image count of the budget (0 to 8), a static one the most slots one draw binds, since rules from a module (D-076) can bind any of them. sandbox/runtime_test.py (live INI step) runs the sandbox program in both entries with a fixture package whose stream file was removed: it runs and exits normally, logs the refusal and reports the backend neither ready nor enabled (6 new checks, 38/38). Full rebuilt suite passes all 31 steps (build/next/run_all-1.4.9.out). The deployed 1.0.2 still has the defect; dist keeps the configuration without a package.

## Material rules from the register rule in the layer (candidate 1.4.8, 2026-10-19)

frontend/gpu/source_registers.h/.cpp is the C++ form of D-076 (t registers buffers first then images, b, s, u in declaration order; arrays count their elements; malformed or unknown modules refused). The character backend derives the rules of every fragment shader without a package profile from its module: for each slot the package binds, no register there is absent (as an unread slot under DX11), one image there is bound (ZeroFile under the package's device-bound 3D policy), anything else or an unreadable module keeps the application's images. Derived profiles are cached per pipeline. At setup every package profile is checked against the rule (status material_rule_mismatches, log line). The sibling search (signatures, pass kinds, material_sibling.cpp, sibling_replay.py, sibling_check.py) is removed. Fixture: its module has the read-only bone block at t0 and the image at t1, so the fixture package and the live INI fixture bind slot 1; the variant with its image at binding 8 shows the mod's texture through the rule; the four-set pipeline's shader declares no image and gets every slot absent (two derived profiles). On the game's dithered variant f1ded4ae the rule gives t15 (1,8), t16 (1,4), t17 (1,2), the textures the sibling search found, and the profile 09e0be38 its captured (1,6), (1,3), (1,1).

Evidence: full rebuilt suite passes all 31 steps (build/next/run_all-1.4.8.out): character replacement 357/357, live INI 32/32, owned Present 20/20, DX11 registers from SPIR-V 1187. Sandbox only.

## DX11 registers from the game's SPIR-V (2026-10-19, D-076)

The general correspondence the catalogue was collected for: the DX11 t register of a game shader's resource is its rank among the SPIR-V module's shader resource views, read-only buffers first, then images, each in declaration order (both compilers keep the HLSL declaration order; the DX11 build puts buffers first). server/efmi_vk/source_slots.py implements it; docs/source-slot-truth.json (258 shaders, metadata only) and scripts/check_source_slots.py (suite step) check it: all 1187 observed t registers of both stages come out as observed, 0 wrong; constant buffers by declaration order agree with 1121 of 1126 size-paired windows. Unit tests 110/110 (DXC-compiled fixture). docs/source-slots.md. This needs no DX11 data and no capture per character. Next: use it in the layer instead of captured fragment profiles.

## Catalogue collection session B, Vulkan (2026-10-19, done)

User-started Vulkan session (PID 17836), layer 1.0.2 from dist injected by the launcher, probe mode, image content hashes on, no character package (BUG-026), nothing replaced. The user showed the same 16 characters in the same order at 2560x1600; scripts/collect_probe.py captured each (captures/catalogue-20261019/vulkan/vk01 to vk16, snapshots of all draws). One empty frame during a screen change was discarded and captured again; two frames used more than 128 pipelines, whose remainder (18 and 4) was queried while the objects were alive and recorded in the bundle's supplement.json with the manifest updated. Every Vulkan frame matches its DX11 frame by index region hashes. Paired summaries per scene (captures/catalogue-20261019/dx11/charNN.summary.json) and a catalogue build/catalogue/catalogue-20261019.json: 953 draw pairs, 116 programs, 113 of 163 drawn DX11 pixel and 33 of 54 vertex shaders, 800 texture slots (1891 observations by content, 1507 by look), no texture conflict, two constant buffer conflicts by size (vs-cb3), consistent with docs/arcane-texture-slots.json. Objects drawn once more in the DX11 frame than in the Vulkan frame stay unpaired (char10: 15 of 25; three further Vulkan frames of it gave the same counts). Character names are still pending from the user.

## Catalogue collection session A, DX11 (2026-10-19, done)

User-started DX11 session (PID 12024, XXMI with the EFMI MCP DX11 probe 0.1.9 as the launcher's injected library, resolution 2560x1600); our layer not loaded. The user showed 16 characters in the character display with mods off (F11); one frame each through scripts/dx11_baseline.py capture --textures --archive-shaders into captures/catalogue-20261019/dx11 (labels char01 to char16, 291 MB, not committed). Every frame has game draws only. 247 distinct shaders archived as DXBC, all named by XXMI's fnv_64_buf of their bytes; 509 distinct texture contents hashed, none unreadable; manifest.json fingerprints 2108 files. From the second character on, each character adds 1 to 18 vertex/pixel shaders, all used only by its own meshes (character-specific materials); 217 distinct vertex/pixel shaders in total. The probe's capture was released afterwards. Character names are pending from the user (pairing with the Vulkan frames does not need them: objects pair by index region hash). Next: session B under Vulkan (protocol in docs/shader-catalogue.md), same resolution and quality.

## Shader catalogue: builder and collection protocol (2026-10-19, after 1.4.7)

The user chose game data collection for the shared shader catalogue. scripts/shader_catalogue.py merges scene summaries of dx11_baseline.py summarize (run without --mod, it pairs every object both frames draw) into a catalogue keyed by Vulkan program, with every DX11 slot of every stage, where it was found, how often and by which evidence; conflicts are reported, --check compares with docs/arcane-texture-slots.json. Trial on data already on disk (arcane display and the two LOD1 frames): 3 scenes, 710 draw pairs, 76 programs, consistent with the verified table; textures mostly by look where content hashes were missing, constant buffers only by size (one conflict). docs/shader-catalogue.md has the collection protocol: session A under DX11 with the current launcher configuration, session B under Vulkan with layer 1.0.2 as deployed (dist/efmi_vk.cfg: replacement off, image hashing on; the user replaces the injected library in the launcher UI and asks for the two Vulkan values). Waiting for the user: account, characters, times. Unit tests 100/100 (12 new for the catalogue). No game session yet.

## Phase 3: remaining operands and buffer hashes (candidate 1.4.7, 2026-10-19)

A probe of every named operand in a draw and in [Present] showed hunting, frame_analysis, sli and stereo_* already constant 0 (the original optimiser, hunting disabled). Added (D-074):
- fps: the generation's original FPS counter advances at each presentation from the host's steady clock (RuntimeInput::ticks_us; the layer passes host_time_us).
- effective_dpi, cursor_hotspot_x/_y: DrawPlanAdapter::host_values answers them from the host sample (RuntimeInput::effective_dpi, Cursor::hotspot_x/_y; original floor 96). OwnedPresenter::sample computes them as the original does (GetDpiForMonitor per-monitor aware on the window's monitor; GetIconInfo, kept per cursor handle).
- Resource->region(...) as an expression: the original returns 0 without the resource; accepted.
- Owned domain: rt_width/rt_height through DrawPlanAdapter::render_target_size (o0's Texture2D for 2D/2DMS views, original caching in the state, the window heuristic falls back to it); ->hashregion/->spatialhash keep the original sentinels and are UNKNOWN on owned buffers, which never carry the original's data cache; vertex or index buffers outside a draw refuse.
Still refused, all with installed users that read game resources only: rt_width in replaced draws and CPU generations, scissor_*, cursor images, ->lastframe on pools, buffer hashes outside the owned domain (Core spatial identification keeps its adapter).

Evidence: full rebuilt suite passes all 30 steps (build/next/run_all-1.4.7.out): original INI runtime 483 (host values 12 new), GPU resources 11189 with clean validation (owned operand and hash property cases new), live INI 32/32, owned Present 20/20, character 357/357. Sandbox only.

## P1-3b: original Present work on the swap chain (candidate 1.4.6, 2026-10-19)

The character generation in the game becomes a mixed generation when the owned GPU tools are deployed beside the layer (DXC with dxil, efmi-vkd3d-shader, SPIRV-Tools-shared, copied by scripts/build.py): CharacterProgram combines the character extension with the owned runtime through frontend/xxmi/owned_runtime.h (a facade keeping the original engine's headers out of the layer), loads the Mod folder's other enabled INIs with it (DISABLED names and the settings folder excluded), keeps non-DDS images out of the character package, and loads inside a deferred scope that is suspended afterwards. layer/src/character_present.cpp (OwnedPresenter) samples the window, cursor and swap chain size, imports the presented image as bb (PRESENT_SRC in and out), runs the Present lists in a scope on the presenting queue and submits with the application's wait semaphores, signalling a per-image semaphore that the presentation waits for instead. The module's frame entry gained the presentation (PresentWork, an ABI change). The first refusal is logged; it leaves the GPU side failed while replaced draws continue.

D-073: in character mode the layer enables on the application's objects what this work needs and the application did not: robustness2 nullDescriptor (D-071), shaderDrawParameters (HLSL vertex IDs need the base vertex; the application's own feature structure is set for the call and restored, or a structure of the layer's is added), color attachment usage on swap chains. Found by the first windowed run: without draw parameters the compiled panel shader refused.

sandbox/owned_present_test.py (new suite step) runs efmi_vk_sandbox.exe with a real swap chain in both entries, the synthetic character fixture and a second INI drawing an opaque quarter from its Present list: the test-only copy of the 20th owned presentation shows the quarter and the application's picture elsewhere; core and synchronization validation are clean. Not verified in the game.

Evidence: full rebuilt suite passes all 30 steps (build/next/run_all-1.4.6.out), including owned Present on a swap chain 20/20 in both entries with clean core and synchronization validation, live INI 32/32 and character 357/357 with the mixed generation. Sandbox only.

## P1-3a: presentation facts in the layer host (candidate 1.4.5, 2026-10-19)

- Win32 surfaces are hooked in both entries (loader layer through vkGetInstanceProcAddr, injection through the vulkan-1.dll exports) and their windows recorded; vkCreateSwapchainKHR moved from counted to tracked, with vkDestroySwapchainKHR and vkGetSwapchainImagesKHR: each device keeps its swap chains' surface window, format, color space, extent, usage, flags and images (state.h SwapchainInfo, an ABI change).
- The application's API version is kept per instance and device.
- Character mode only: when the physical device supports VK_EXT_robustness2 nullDescriptor and the application did not ask for the extension, the layer adds it with that one feature (D-071); an application request is used as it is. dev->extensions keeps the application's own list. On the NVIDIA RTX 5060 the live INI and character tests run with it added and clean validation.
- Status JSON device: application_api, null_descriptors, null_descriptors_added, swapchains (format, color space, size, usage, flags, image count, whether the window is known). A game session will report the game's swap chain format this way.
- Verification: probe_test in all entry, thread and descriptor modes checks the recorded swap chain and window; live INI 32/32 and character 357/357 in character mode with the extension added.

Evidence: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.5.out), probe end to end in all modes, live INI 32/32 and character 357/357 with the null-descriptor extension added. Sandbox only.

## Phase 1 foundations: mixed generation and imported images (candidate 1.4.4, 2026-10-19)

The user chose "first do 1, complete the INI": owned GPU execution in the game, then the directory generation, the remaining families, and last the families that need the shader catalogue (TASKS T-031). Phase 1, step P1 (Present, key and Constants lists in the game) needs three pieces; two are done:

- P1-1, D-072: frontend/xxmi/owned_generation.h/.cpp combines a native extension (Core boundaries and the replaced-draw file plan) with GpuShaderRuntime. DrawPlanAdapter::owned_scope installs a gate: while the host has no owned GPU scope open, every owned callback (resource commands and queries, matches, parameter publication, draw preparation, CustomShader) is treated as absent, so replaced draws keep the plan path unchanged and a CustomShader inside one refuses with its reason. Declaration references the GPU side executes are mirrored into the plan's references. The native extension's preparation runs with the gate closed; its native CustomShaders are not compiled (GpuShaderRuntime::exclude_shaders). sandbox/resource_owned_generation_cases.h checks all of it with a Present draw into a host back buffer that matches native DX11 bytes.
- P1-2: gpu::ResourceCommands::import_image takes a caller-owned 2D image (a swap chain image) valid in a given layout before and after every submission that uses it, never destroyed here; submit() and GpuResourceRuntime::finish() take wait and signal semaphores. sandbox imported_backbuffer: an image cleared and left in SHADER_READ_ONLY by raw Vulkan is imported, drawn into, handed back and waited on through a semaphore; contents outside the draw survive; validation including synchronization is clean.
- Next, P1-3 in the layer: swap chain and surface tracking (state.h, an ABI change: the injected DLL is replaced only while the game is closed), nullDescriptor at device creation when supported and not enabled, a module present entry that records and submits the generation's Present with the app's wait semaphores and returns its own, the window sample, the Mod folder's other INIs (the panel) in the character generation, load-time Constants GPU work recorded on the loader thread and submitted at the first present. Verification in efmi_vk_sandbox.exe (real swap chain) with the synthetic character fixture, status counters, clean validation and a test-only readback of the presented image.

Evidence: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.4.out): GPU resources 11175 checks with no validation message (mixed generation and imported back buffer included), runtime 471, execution matrix 41 probes. Sandbox only.

## Image panel, step 3b: the installed panel in the owned domain (candidate 1.4.3, 2026-10-18)

D-070: owned CustomShader compilation follows the original cache rule. A `<file>.<model>.<flags hex>.bin` whose last-write time equals the HLSL's exactly replaces compilation (CustomShader::compile, load_cached_shader); with GpuShaderRuntime::enable_cached_bytecode it runs through the exact DXBC translation, without it such a cache refuses. All 19 enabled panel Mods ship both caches with matching times. D-071: an unbound CustomShader slot becomes a null descriptor when the device enables VK_EXT_robustness2 nullDescriptor (ShaderHost::null_descriptors), giving D3D11's zero reads, zero dimensions and dropped writes; otherwise it still refuses. The sandbox device enables nullDescriptor only.

sandbox/resource_panel_cases.h runs the installed panel of the first enabled Mod that ships it (akekuri sol paraiso 1.6 today; files copied with their times, a stub for the main INI variables it reads): character mark, Ctrl+K, 24 frames of fade-in, window sample, both PNGs, the shipped DXBC, into a host back buffer. The reference replays the final draw on native D3D11 with the same DXBC, WIC-loaded PNGs and the owned final IniParams. On the NVIDIA GeForce RTX 5060 Laptop GPU used by both APIs the owned result differs from native hardware D3D11 in 2 of 360,000 pixels (largest 5); WARP itself differs from that driver in 30,979 pixels (largest 2), so WARP is not an exact reference for this transcendental-heavy shader. The check requires the same adapter and at most 0.01% differing pixels within 16 levels. The Software float32 mode is used because the sandbox device lacks 32-bit denormal flushing. docs/custom-shaders.md.

Step 4 (the layer): intercept vkCreateSwapchainKHR/vkGetSwapchainImagesKHR and the surface's window, supply the window sample and swap chain size, import the swap chain image as the back buffer (PRESENT_SRC in and out, contents kept, possibly a UNORM view of an sRGB image), host an owned GPU generation for the panel next to the character generation (same variables), draw before vkQueuePresentKHR with semaphores, and decide the bytecode library path and nullDescriptor on the game's device. Deployment and any load into the game are the user's call.

Evidence: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.3.out): GPU resources 11137 checks with no validation message, including the installed panel (2 of 360,000 pixels differ from native hardware D3D11 on the same NVIDIA GPU), execution matrix 32 classes and 41 probes. Sandbox only.

## Image panel, step 3a: host back buffer (candidate 1.4.2, 2026-10-18)

GpuResourceRuntime::backbuffer(image, format, bind flags) is the host's current back buffer, which bb, r_bb and f_bb return (without upscaling the original hands out the swap chain's current buffer for all three; GetBuffer returns the resource alone, so no view and no out-of-band format). They are source-only targets in the original parser: an assignment to them is a parse warning and never executes, so no write path exists. sandbox/resource_backbuffer_cases.h draws a blended triangle-strip CustomShader into `o0 = set_viewport bb` on an R32G32B32A32 back buffer with initial contents and matches native DX11 bytes; queries, the restored empty render target, a missing back buffer (null sentinel -2) and the two parse warnings are checked. A native-reference mistake was corrected by the source: IniHandler's `blend =` sets the color factors only, alpha keeps ONE, ZERO, ADD.

Found for step 3b: all 19 enabled Mods with a panel ship `Draw_2D.vs_5_0.8000.bin` and `.ps_` caches with the HLSL's modification time (one disabled Mod has none), and the original CustomShader::compile loads such a cache instead of compiling (exact FILETIME equality; it writes caches only with cache_shaders). The panel that runs in DX11 is therefore the shipped DXBC. Planned D-070: owned CustomShader compilation uses that cache through the exact DXBC translation, never writing a cache. Codex's uncommitted work adds a separate bytecode compiler opt-in for source shaders in the same class.

Evidence: full rebuilt suite (build/next/run_all-1.4.2.out): 28 of 29 steps pass, GPU resources 11056 checks with no validation message; the installed HLSL step failed only because two installed INIs changed during the run and passes when rerun with the installed syntax step.

## Image panel, step 2: PNG file images (candidate 1.4.1, 2026-10-18)

The owned image adapter takes the original's WIC path for every non-DDS file: CreateWICTextureFromFileEx semantics (one mip, no generated chain, declaration bind and misc flags, GetWICFlags from `color_space`), with frontend/assets/png.cpp as the PNG decoder; it gained the FORCE_SRGB and IGNORE_SRGB loader flags. Other WIC containers and unreadable files refuse with a reason. sandbox/resource_png_cases.h loads 13 PNGs both through the pinned DirectXTK loader on WARP and through an original INI into an owned Vulkan image; format, size, mips, misc flags and all bytes agree, including the panel's installed `k.png` and `panel_icon.png` (copied by check_resource_commands.py, originals checked unchanged). An earlier assumption was corrected by the native result: WIC's PNG decoder yields 32bppBGRA for 8-bit RGBA, so the original texture is B8G8R8A8. The character renderer in the game still refuses PNG bindings; that changes with step 4. docs/gpu-resource-commands.md.

Evidence: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.1.out): GPU resources 11027 checks with no validation message (13 PNG native comparisons), PNG decoder check 12/12, runtime 471. Sandbox only.

## Image panel, step 1: window, resolution and cursor operands (candidate 1.4.0, 2026-10-18)

The user resumed PNG and asked for the image panel in the game ("resume, and merge into main"). The panel (`k/1.ini`, about 20 installed Mods) needs, in order: (1) the original window, resolution and cursor operands; (2) PNG files as owned images with the original WIC loader semantics; (3) owned drawing into the backbuffer (`o0 = set_viewport bb`) at Present, compared with native DX11 pixels; (4) the layer drawing it on the game's swap chain, with the window sample, the swap chain size and the panel's INI in the character generation. Loading in the game stays the user's call.

Step 1 is done: `RuntimeInput` carries a window sample (client rectangle and screen origin, cursor screen position and visibility, swap chain size) per present; the original GetClientRect, GetCursorInfo and ScreenToClient calls read it through a token hWnd that never reaches a real window API; the bridge accepts `res_*`, `window_*`, `cursor_showing`, `cursor_screen_*`, `cursor_window_*`, `cursor_x`/`_y`, and keeps the render-target fallback of the window heuristic away from the reference context (D-069). The layer does not supply the sample yet (step 4). docs/live-ini.md describes it.

Evidence: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.0.out): runtime 471 checks including 13 window-operand checks, execution matrix 32 classes and 41 probes, GPU resources 10786, character 357, live INI 32/32. Sandbox only.

## INI execution matrix (candidate 1.3.9, branch claude/ini, 2026-10-18)

`docs/ini-execution-matrix.json` states for each of the 32 original command classes its origin and, per use, how it executes in a CPU generation (how the character renderer in the game hosts Mod commands) and in the owned GPU domain of the sandbox. `scripts/check_ini_execution_matrix.py` (suite step "INI execution matrix") fails when a source class has no entry, when the origin disagrees with the pinned source, when a CPU decision disagrees with its probe or when cited owned evidence is gone. Probes are INI fixtures run through the new `runtime_test.exe --probe-command` (load, present, one matching draw, present). 41 probed uses agree; a deliberately broken copy of the matrix fails on all four injected faults. docs/ini-language-coverage.md now points at the matrix; its stale hand-kept table is gone.

Found by the probes: `special = upscaling_switch_bb` crashed the host when a generation was retired (BUG-025: the original destructor writes through G, which is null outside a scope). Fixed in IniRuntime's destructor; the same fixture segfaults on a build without the fix and completes with it. No installed Mod uses the command. D-068: the upscaling flip now runs as original code and `special = draw_3dmigoto_overlay` is a no-op (no 3DMigoto overlay exists in a hosted generation); both refused a draw before.

Evidence: full rebuilt suite in the worktree passes all 29 steps (build/next/run_all-1.3.9.out): INI execution matrix 32 classes and 41 probes, source language 726/726, evaluation 65/65, runtime 458, GPU resources 10786 with no validation message, character 357, live INI 32/32. Sandbox only; no game, deployment or launcher action.

## INI ownership and recoverable warnings (candidate 1.3.8, branch claude/ini, 2026-10-18)

The user handed the INI part to Claude ("ini this part is handed to you, continue to perfect it"). Codex's uncommitted changes of 2026-10-15 in the main tree (ShaderRegex executable publication, gpu_source_shaders, resource_regex_execution; layer version 1.3.7) overlap the runtime files, so Claude works in a git worktree, `.worktrees/ini` on branch `claude/ini` from 66213ba, and leaves the main tree untouched. The branch is not merged; merging needs Codex's pending work committed or set aside (the user's call). The version 1.3.7 is left to that pending work; the branch uses 1.3.8.

D-067: original warnings never reject a generation. The original loader reports duplicate sections, unrecognized lines, broken expressions, duplicate ShaderOverride hashes and numbered Pattern sections through its overlay and continues with what it could parse; the hosted runtime and folder inspection used to reject the whole configuration for any of them. Now a generation loads like the original, keeps the count and latest 64 texts (`RuntimeStatus.diagnostics`), logs them at load and reports them as `status.replacement.ini.diagnostics` and `diagnostic_messages`. The inspection completes and fails only on dire messages. Adapter refusals (constructs without a Vulkan implementation, compile failures of D-047) still refuse a candidate, and F10 keeps the active generation for them. Seven tests that encoded the old rule now assert the original behaviour and keep a refused-candidate case each.

Installed inputs changed since 2026-10-15 (read-only observation): about 22 enabled Mods, 58 INI inputs; `arcane fes 1.2` is gone (now `arcane xoxo 1.3` and archives), Yvonne is 1.8, fangyi `zhuangfangyi lilith vice 2.0`. The installed folder parses with 11 original warnings and no dire message (INPUT-001's duplicates, gilberta's duplicate section, entries outside a section and an unresolved RabbitFX resource reference). The panel check now takes the first enabled installed `k/draw_2d.hlsl` instead of the removed path. The deployed 1.0.2 live package (build/character/arcane-live.bin sidecar) names the removed arcane fes 1.2 main INI; a Vulkan session would not load that character's INI. Not verified in a game; the launcher has been on DX11 since 2026-10-14.

Evidence: full rebuilt suite in the worktree passes every step (build/next/run_all-1.3.8.out): source language 726/726, directory fixtures, installed folder syntax and HLSL corpus on the original installed files, runtime 458, GPU resources 10786, character 357, live INI 32/32, Vulkan validation 76; the worktree baseline before the change failed four steps (run_all-baseline.out)

## Exact bytecode buffer views (candidate1.3.6)

Native raw/structured/typed declarations now retain their original view contract,
separate from executable scalar storage stride. Typed UAV programs use texel
views for format conversion; unknown-format access checks actual enabled device
features and per-format capabilities. Native WARP and owned Vulkan outputs agree
for 12/16-byte structures, nonzero regions, raw views and RGBA8 conversions.

Full rebuilt frozen-source run: build/next/run_all-1.3.6.out. All code/GPU steps
pass (runtime475, resources10784, compiler172/58; core/sync clean). The two
installed corpus steps still fail under INPUT-001. Original source emits
recoverable duplicate-section warnings, retaining the first section; the hosted
check treats warnings as failures. This distinction needs a source-policy audit.
The unchanged project-only normalized copy compiles/validates55 programs from70
declarations: build/corpus-1.3.6/hlsl/report.json. Source hashes113/9/12 match;
all88 installed source hashes remain unchanged. No game, deployment, launcher or
system action. Dist1.0.2 remains unchanged.

Continue offline with transactional ShaderRegex executable publication, original
source association and generation lifetimes. Source facts/command links must not
publish on translation failure. Then continue the original-language/runtime
integration work; no game input needed. PNG paused, LOD0/LOD1 only. See D-066.

## Float32 lowering (candidate1.3.5)

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

## Exact DXBC translation (verified candidate1.3.4)

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

## Original ShaderRegex assembly (verified candidate1.3.3)

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

## Original ShaderRegex linkage (verified candidate1.3.2)

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

## General original resource matching (verified candidate1.3.1)

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

## Original shader hooks (verified candidate1.3.0)

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

## Matched command phases (verified candidate1.2.9)

BUG-024 is fixed: direct hash matches execute all pre lists before all post
lists in one bounded transaction. Abort and cached queries are shared within a
phase; the independent post root receives fresh transient state. Applied GPU
bindings/layout survive; unconsumed layout edits do not. Failure preserves the
caller plan and prevents submission of earlier recorded GPU work.

Full rebuilt frozen-source suite:build/next/run_all-1.2.9.out exit0/all steps;
runtime118, resources9020, compiler172/58, corpus37/37, core/sync clean through
shutdown. Source hashes93/9/12 match; installed inputs unchanged. No game,
deployment or system action; dist1.0.2 and the separate launcher DX11 state stay
unchanged. Text336 clean after the new shader audit document.

Continue1.3.0 offline with original ShaderOverride parsing, bounded stage roots,
source shader queries and pre/original/post placement on owned resources.
Source audit:docs/shader-matching-audit.md. Temporary CustomShader executables
do not change the original tracked shader queried by vs/ps/cs. Proven source
hashes and declaration metadata are required; do not substitute SPIR-V hashes.
Legacy substitutions, ShaderRegex patching and actual application placement
remain separate. No game input needed; keep going. PNG paused, LOD0/LOD1 only.

## Native vertex input bits (verified candidate1.2.8)

Owned source layouts now preserve float/int/uint input register reinterpretation
by retyping executable inputs to the actual fetched scalar class and bitcasting
loads back to the original shader type. Missing components use fetched-format
defaults. BGRX uses BGRA hardware fetch with alpha1 inserted before the bitcast.
Immutable source signatures/executables remain intact; conversion flags enter
pipeline identity. Original and lowered SPIR-V validate before GPU creation.
Numeric types and pointers are reused/reordered without duplicate declarations.

Native WARP bit-exact pixel references cover66 original INI combinations, input
widths1..4, normalization/sign extension, missing components, BGRA/BGRX cache
switches and indirect draws. Additional executable cases force pointer aliases
and scalar access chains with constant and computed indices. Full rebuilt frozen
suite:build/next/run_all-1.2.8.out exit0/all steps; resources8962, compiler172/58,
corpus37/37, core/sync clean, source hashes92/9/12 match. Installed inputs and
dist1.0.2 unchanged. No game/deployment/system action. Text333 clean after docs.

The packed16 native audit found device-dependent support: RTX5060 DX11 rejects
formats85/86/115 (IA flag0, E_INVALIDARG); WARP advertises/creates them but returns
constant(0,0,0,1) over all65536 words. R16_UNORM controls pass every value on both.
Keep packed16 explicitly unsupported; do not turn that WARP result into a decoder.
See docs/input-layout-audit.md. Null/OOB and complex input shapes remain separate.

Next1.2.9 fixes BUG-024: direct multi-match hash entry executes1324 while equivalent
source pre/post texture checking executes1234. Group pre lists before post lists,
preserve within-phase state/budgets and reset transient state at the independent
post root. Evidence:docs/matched-command-order.md and matched-order diagnostic.
Then continue source shader matching/directory/application integration. No game
input needed; keep going. PNG paused, LOD0/LOD1 only.

## Owned instance divisors (verified candidate1.2.7)

Original instance steps0/2/3 and the physical maximum now use the actually enabled
KHR/core divisor path. Native DX11 WARP pixel comparisons cover direct, indexed,
indirect and indexed-indirect draws with nonzero first instances. The source base
is added after division, including divisor0. Pipeline identity includes divisors;
direct instance bounds are checked even for indexed draws. Feature/zero/max/base
properties refuse before Vulkan calls. Indirect bases require property support
without hidden CPU readbacks. The direct zero-stride fallback remains available.

Full frozen-source rebuilt regression:build/next/run_all-1.2.7.out exit0, all
steps pass; resources6653, compiler172/58, corpus37/37, core/sync clean through
shutdown. Source hashes89/9/12 match; installed inputs unchanged; text329 clean.
No game, deployment or system action; dist1.0.2 unchanged. This is owned sandbox
execution, not application feature enablement or game rendering.

Continue1.2.8 with typed vertex input lowering and BGRX alpha semantics. Native
read-only WARP diagnostic input-layout-reinterpret.cpp/.txt confirms raw bits,
format-based missing-component defaults and BGRX alpha1 across float/int/uint
signatures. That diagnostic is not yet a Vulkan execution result. Packed16 BGR,
null/OOB fetch, source matching and directory/application integration remain.
No game input needed; continue. PNG paused, LOD0/LOD1 only.

## Owned input layouts and original edits (verified candidate1.2.6)

SourceInputLayout retains immutable element descriptions and the native creation
signature, including required-but-unused semantics. Vulkan input locations follow
the current VS's native registers and the layout's original register mapping.
Creation validity/APPEND alignment, slot rates and source formats are compared to
system DX11 WARP. Native-valid large offsets remain distinct from Vulkan limits.

Original element-offset/format edits run before parameter flush and argument
execution, including zero draws and dispatches. The pinned helper is unchanged,
including its enum-based realignment boundary. Failed native creation keeps the
old layout; unsupported execution refuses instead of faking creation failure.
Edits survive nested CustomShader and Present calls. Intercepted draw completion
restores the original layout on success/failure. Effective layout attributes and
stream rates participate in the pipeline key. Zero instance step adds the native
StartInstanceLocation byte base before using a zero Vulkan binding stride.

Native pixel tests cover switched VS/creation signatures, instance steps0/1 with
nonzero bases, packed R8/R16 APPEND input, component defaults, nested edits,
invalid-creation fallback, original draw restore and failed-root GPU discard.
Full frozen-source rebuilt regression:build/next/run_all-1.2.6.out exit0, all
steps pass; resources6093, compiler172/58, corpus37/37, clean core/sync validation,
source hashes88/9/12 match. Installed inputs unchanged. No game/deployment action;
dist1.0.2 remains unchanged. This is owned execution, not application layout import.

Continue offline with remaining input fetch/divisor/type cases and original
shader matching/directory integration. Native and physical probes show both GPUs
support KHR instance divisors, but neither exposes packed16 BGR vertex formats;
those require lowering, not a blind VkFormat substitution. Type reinterpretation,
null/OOB inputs, divisors above1 and indirect zero-divisor reads remain explicit
boundaries. No game input needed. PNG paused, LOD0/LOD1 only.

## Owned geometry and original Core text (verified candidate1.2.5)

Original CustomShader now executes VS/GS/PS on owned resources. Point, line and
triangle assembly feed triangle-strip geometry output; actual geometryShader,
depthClamp, scalar-layout and draw-parameter enablement are checked. Geometry
primitive compatibility, invocation/output counts and component limits refuse
before Vulkan pipeline creation. The last raster stage must provide position.
Descriptors remain stage-specific and submission-retained. Tessellation, geometry
point/line output, adjacency and standalone point raster rules remain explicit.

The unchanged installed TextPrinter.hlsl and LiberationSans-Bold.dds now run
through original INI resource/compute/graphics commands in the headless sandbox.
Three cases cover ordinary text, newline/wrapping/DPI/centering, and70-character
right/bottom alignment across the64-character geometry chunk boundary. Native
DX11 WARP agrees on exact chunk start/count fields; maximum floating position
and RGBA differences are5.96046448e-08 (test tolerance1e-6). Visible glyph/background
assertions prevent an empty-image false pass. Native default alpha blending is
ONE/ZERO when only the RGB blend key is set. Scalar stride72 requires enablement.
This is the original HLSL/font with controlled owned fixtures, not the complete
Core INI, application backbuffer/window integration or a game rendering result.

Full frozen-source rebuild/regression:build/next/run_all-1.2.5.out exit0, all
steps pass. Resources5644, compiler172/58 valid, full directory37/37 programs,
zero core/sync errors/warnings through shutdown;84 resource,9 compiler,12 corpus
source hashes match. Installed shader/font and corpus sources unchanged.
Dist1.0.2 unchanged; no game/deployment/launcher/system action.

Continue1.2.6 with owned input layouts and original layout-edit lifetime, then
remaining shader/state/resource and matching/directory/application integration.
Independent native probes confirm layouts retain their creation-signature
register map, even after switching VS; APPEND alignment and native failure rules
are recorded in docs/input-layout-audit.md. These probes are not Vulkan layout
execution yet. No game input needed; keep going. PNG paused, LOD0/LOD1 only.

## Full enabled-directory HLSL audit (verified candidate1.2.4)

The original parser now exports CustomShader stage declarations, namespace paths,
compile flags and effective recursive-include policy. The inspector compiles every
nonempty/non-null source stage with the production HlslCompiler, retaining DXBC,
SPIR-V, exact dependency snapshots and diagnostics under build/hlsl-folder/.
Namespace-first/module-fallback resolution is tested. Original built-in shader
sections remain in the inventory; source boolean expression normalization maps
recursive_include=-1 to1 in this pinned revision, and the test preserves that.

Current read-only corpus:34 INI inputs,30 shader sections,38 active declarations,
37 distinct programs,47 explicit null stages and95 inherited stages. All37 pass
native SM5 checking, Vulkan1.2 compilation and independent SPIR-V validation.
Four modules require the scalar/standard-layout validation profile: TextPrinter
geometry/pixel/compute and chest_deform40 compute. These profiles are conditional
requirements, not proof those features are enabled in an application device.
All INI sources and successful HLSL dependencies remain byte-identical. No GPU
execution, complete-directory scheduling, PNG support or game result is implied.

Full frozen-source rebuild/regression:build/next/run_all-1.2.4.out exit0, all steps
pass, including the new corpus/metadata/evidence tests. Resources5342, compiler
172/58 valid modules, directory language9/evidence2; zero core/sync messages.
All82 resource,9 compiler and12 corpus source hashes match; text321 files clean.
Dist1.0.2 unchanged. No game/deployment/launcher/system action.

Next1.2.5: owned geometry-stage execution and the actual Core text shader path,
then remaining vertex layouts, state/resource cases and source shader/directory
integration. No game input needed; continue. PNG paused, LOD0/LOD1 only.
An isolated diagnostic at build/source-state-probe/clip-coordinates.cpp/.txt
confirms native DepthClipEnable=false and Vulkan depth clamping both deliver
fragment z/w=(2,1) before depth testing. This is a diagnostic pixel case, not
TextPrinter verification. Its original shader packs texture coordinates into Z/W.

## Original owned INI graphics (verified candidate 1.2.3)

Original CustomShader now overrides/inherits/restores VS/PS and source graphics
state in the owned executor. Direct, indexed, instanced, indirect, automatic
counts and direct from-caller commands use original argument and buffer rules.
Native state creation canonicalization is applied to inactive depth/stencil/blend
fields; invalid active stencil creation falls back to the native null state.
Null VS does no work; null PS preserves color while accepted depth still writes.
Semantic state keys bound pipeline reuse; descriptors and targets remain retained.

Native DX11 WARP pixel references cover all these draw paths, nested restoration,
constant blending, inherited scissors, depth acceptance, and original point/linear
sampler overrides on PS slot0 without changing slot3. Ordinary sampling ignores
the source comparison-filter bit; actual comparison instructions explicitly refuse.
Root failure restores CPU state and prevents submission of partial graphics work.

Full frozen-source rebuild/regression: build/next/run_all-1.2.3.out, exit0, all
steps pass. Resources5342, compiler172/58 valid modules, core/sync clean through
shutdown; all82 resource and9 compiler source hashes match. Text318 files clean.
This is original INI execution on owned sandbox images, not application resources
or real characters. Dist1.0.2 unchanged; no deployment/game/launcher/system action.
Development fixtures were corrected for original clear syntax and typed depth-view
rules; production constraints were preserved.

Continue1.2.4 with all enabled Mods/Core HLSL declarations from the original parser
and compiler diagnostics for the complete corpus. Then input layouts, remaining
shader/state/resource cases, matching and application/directory integration.
Current frontend still explicitly refuses vertex layouts, geometry/tessellation,
point/line raster rules, depth bias, multisampling, comparison sampling, null
resource emulation and attachment-free graphics. Source state validity beyond the
measured cases remains a compatibility boundary, not a full native fallback proof.
No game input needed; continue without stopping. PNG paused; LOD0/LOD1 only.

## Owned graphics execution (verified candidate 1.2.2)

Production ShaderCommands now records owned VS/PS graphics work with shared
immutable descriptors, private render passes and submission-retained pipeline,
framebuffer, vertex/index/argument and target owners. Direct, indexed, instanced
and indirect draws share the resource domain and actual enabled feature checks.
State/attachment/layout/range validation occurs before recording. No hidden CPU
readback or queue wait is added; GPU indices/counts remain a caller-valid contract.

Native DX11 WARP and Vulkan pixel references agree for distinct stage bindings,
textures, vertex input, blend constants, depth tests, signed indexed bases and
nonzero draw offsets. Tests cover discarded/submitted lifetime and quota rollback.
Complete frozen-source rebuild/regression: build/next/run_all-1.2.2.out, all steps
pass; resources 4749, compiler 172/58 valid modules, zero core/sync messages through
shutdown. All 80 resource and 9 compiler source hashes match; text 316 files clean
before this memory update. This is owned graphics API execution, not original INI
graphics calls, application pass placement or game rendering. Dist 1.0.2 unchanged.

Next 1.2.3: connect original CustomShader stages/state and Draw commands, including
inherited layouts, automatic counts, source samplers and state restoration. Then
finish remaining shader semantics, matching and enabled-directory integration.
Current owned graphics supports VS/PS and triangle/line topologies; geometry,
tessellation, dual-source output lowering and additional attachment/interface
cases remain explicit work. No game input needed; continue without stopping.
PNG paused; LOD0/LOD1 only. No deployment, game or system action by this work.

## Source coordinates and state descriptions (verified candidate1.2.1)

BUG-023 is fixed: the compiler uses DXC's DX-position-w and nonzero base vertex/
instance options. Before-fix native comparisons expose pixel w=2 versus Vulkan
0.5, and source IDs excluding offsets while Vulkan included them. Native DX11
WARP and Vulkan now agree byte for byte for pixel centers/depth/w, all cull/front
face combinations, direct start offsets, indexed signed base vertices and multiple
instances. Draw-parameter shader capabilities require actual device enablement;
the headless reference explicitly enables shaderDrawParameters. Production
graphics pipeline creation must enforce that feature before using these modules.

SourceGraphicsState exposes original default/replace/bitmask-merge descriptions,
using the pinned masked-copy helper. Native original merge methods create actual
WARP states and agree on effective fields, blend factors, sample masks and stencil
references. PS sampler override touches slot0 only. This is a CPU description
bridge, not yet production INI graphics execution. Tests compare semantic fields,
not unspecified D3D structure padding.

Full frozen-source rebuild/regression:build/next/run_all-1.2.1.out, all pass;
resources4559, compiler172/58 valid modules, all existing suites. Core/sync clean
through shutdown;76 resource and9 compiler hashes match; text312 files clean.
No deployment/game/launcher/system action. Dist1.0.2 remains unchanged.

Next1.2.2: reuse owned descriptor lifetimes for graphics pipelines/draws, connect
original stages and fixed-function state, then matching/directory integration.
Independent native sampler probe build/source-state-probe/samplers.cpp/.txt shows
comparison filters used by ordinary SampleLevel return ordinary texture values:
point (4,1,4), linear (2.5,1,4). The sampler adapter must use actual comparison
instructions, not the filter comparison bit alone. Probe is diagnostic, not yet
integrated regression. No game input needed; continue without checkpoint stops.
PNG paused, LOD0/LOD1 only. See docs/custom-shaders.md and D-051.


## Original graphics linking (verified candidate 1.2.0)

HLSL compilation retains original DXBC register/component signatures and stable
SPIR-V interface identities. The bounded linker validates native adjacent-stage
contracts, then relocates Vulkan locations by original case-insensitive semantic
and index. Vertex/hull/domain/geometry/pixel chains, patch/per-vertex IO and fixed
arrays are covered. Wider output vectors require actual enabled maintenance4;
unsupported packed/block/specialized interfaces refuse. Inputs remain immutable.

A native DX11 WARP and headless Vulkan pixel reference exposes case-dependent
DXC location ordering: raw Vulkan returns (207,311,2,3), while linked Vulkan and
original DX11 both return (702,1103,7,11) for all 16 pixels. An earlier fixture
omitted a native position register and was itself incompatible. It was corrected
and the native signature guard added; semantics alone cannot repair invalid DXBC.
Evidence of that development correction is in build/graphics-link-native-invalid/.

Complete frozen-source rebuild/regression: build/next/run_all-1.2.0.out, all steps
pass. Resources 4065, compiler 172 checks/58 valid modules, parser 726/evaluator65/
runtime68, character79/357, liveINI28, validation76/modes40/MCP47/evidence113.
Core/sync clean through shutdown; all 74 resource and 9 compiler hashes match.
This is compiler/linker and isolated pixels, not original INI graphics execution.

Continue immediately with original fixed-function state and owned drawing,
including native coordinate, position-w and vertex/instance offset references;
then shared shader matching and enabled-directory integration. No game input
needed. Next1.2.1, dist1.0.2 unchanged, PNG paused, LOD0/LOD1 only. No deployment,
game, launcher or system action. See docs/custom-shaders.md and D-050.


## Source binding semantics (verified candidate 1.1.9)

Owned bindings now reproduce native D3D11 input/output conflicts. Buffer byte
ranges still conflict; image mip/layer ranges can be independent. All six shader
stages and IA bindings participate. Ranges materialize every element before one
publication, and duplicate output batches preserve the complete prior state.
RTV/DSV assignment validates the complete attachment set and clears pixel UAVs.
A depth target can be larger than color targets. Pixel UAV assignments replace
the entire pixel UAV set and remove RTV registers at/after the first UAV slot;
compute UAV assignments preserve other nonconflicting registers.

CustomShader OM restoration applies native input/compute-output hazards. The
pinned helper saves only pixel UAV slots 0-7; complete restoration discards high
slots. Empty CustomShader calls can be removed by the original optimizer.
Older resource fixtures now explicitly unbind writers before later reads; their
byte assertions remain intact. The old UAV+SRV coexistence assertion is corrected.

BUG-022 was reproduced before the fix at 3728 checks. New native WARP references
use fresh devices for invalid calls, which can contaminate later observations.
Complete frozen-source rebuild/regression: build/next/run_all-1.1.9.out, all steps
pass. Resources 4006, compiler 117/33 valid modules, parser 726/evaluator 65/runtime
68, character 79/357, live INI 28, validation 76/modes 40/MCP 47/evidence 113.
Core/sync clean through shutdown; 71 resource and 6 compiler hashes match.
This is owned binding and sandbox evidence, with no deployment or game action.

Next version 1.2.0: graphics semantic linking, original state and draw execution,
then shader matching and enabled-directory integration. Alphabetic DXC IO order
alone is insufficient when stage signatures differ. Preserve source semantics
and verify coordinate/position/vertex/instance behavior against native DX11.
Current compiler lacks the DXC position-w option. Vector-width differences need
actual maintenance4 enablement or rewriting; do not silently assume it. Original
sampler overrides affect PS s0, use comparison filters, and save all 16 PS samplers.
No game input is needed. Continue without stopping between verified increments.
Dist 1.0.2, PNG paused, LOD0/LOD1 only. See docs/custom-shaders.md and D-049.

## Compute follow-up (verified candidate1.1.8)

Bounded executable analysis measures fixed workgroup storage and resolves
sampled image/sampler pairs, including constant array elements. Actual shared
structures/barriers/atomics match DX11. Unrelated integer fetches coexist with
linear sampling. Unknown provenance and special shared layouts remain explicit.
Indirect compute reads GPU-produced arguments with barriers and fence ownership,
without CPU feedback. Original byte offsets, null sources and resource queries
are tested. GPU counts must obey device limits. Default source samplers match
native D3D11 interpolation/clamp/signed LOD; explicit host snapshots and shared
reserved sampler quotas work. Do not invent source append/counter views: original
owned INI creation lacks those flags. Application counter imports remain open.

Full frozen-source rebuild/regression:build/next/run_all-1.1.8.out, all steps pass.
Resources3720, compiler117/33 modules, parser726/evaluator65/runtime68,
character79/357, liveINI28, validation76/modes40/MCP47/evidence113. Core/sync clean
through shutdown;69 resource and6 compiler hashes match; ABI unchanged.

Independent native WARP probes confirmed BUG-022: owned slots omit source
read/write conflicts. Buffer ranges still conflict; texture mips/layers can be
independent. Source slot ranges publish as one batch, and OM restoration has
binding side effects. An older logical-only scope fixture incorrectly expects
UAV+SRV coexistence. Evidence:build/source-state-probe/hazards.cpp and hazards.txt.
Invalid duplicate-UAV batches can affect later native hazard observations even
after ClearState; isolate invalid cases or run them last. Existing distinct-
resource compute/native output comparisons remain valid, but full-suite success
is not proof of source binding equivalence. Fix immediately in1.1.9.

Then continue graphics stages/state/draws, shader matching and directory
integration. No game input needed; dist1.0.2 unchanged, no deployment/game action.
PNG paused, LOD0/LOD1 only. See docs/custom-shaders.md and D-048.

## Original CustomShader compute (verified candidate1.1.7)

GpuShaderRuntime opts into original CustomShader parsing, compiles every declared
stage and executes direct compute through owned Vulkan pipelines and immutable
descriptors. Nested pre/post, original frame/reset limits, null/inherited compute,
abort and viewport/OM restoration are tested; ordinary slots persist. Failed
candidates preserve a retained old generation. ShaderCommands supports buffers,
typed views, image arrays, storage images, samplers and separate UAV counters at
its explicit low-level boundary. Fences retain every dispatch owner.

Production SPIR-V validation uses actual enabled block-layout features. Packed
float3 storage refuses without scalar layout and runs on an enabled headless
device. Native DX11 WARP/Vulkan parameter-consumer bytes agree. Native diagnostic
trailing NULs are removed so warnings cannot hide later DXC errors. BUG-021 fixes
resource-query argument guards across every original draw/dispatch form.

The final frozen-source rebuilt suite passes: build/next/run_all-1.1.7.out.
Resources3633, compiler117/33 valid modules, parser726/evaluator65/runtime68,
character79/357, liveINI28, validation76/modes40/MCP47/evidence113 and all other
steps pass. Core/sync clean through shutdown;66 resource and6 compiler source
hashes match. ABI16b90a85431f86b4 unchanged; text301 files clean. An earlier
interrupted run is not complete verification evidence.

Next: workgroup memory and precise sampler/image usage, source sampler/counter
state, indirect execution and remaining device cases; then graphics state/draws,
shader matching and directory integration. Continue immediately without game
input. This is owned offline compute, not complete Mod graphics or application
import. See docs/custom-shaders.md, LIMIT-031 and D-047. Next1.1.8;
dist1.0.2 unchanged, no deployment/game action. PNG paused, LOD0/LOD1 only.

## Original HLSL compiler (verified candidate 1.1.6)

HlslCompiler preserves original main/stage macros, model-5 validation and
include policies, then compiles expanded source to SPIR-V with DXC. Original
DXBC reflection supplies source slots; Vulkan binding ranges separate stages,
resource classes and UAV counters. SPIRV-Reflect is now linked from its pinned
external checkout. Exact dependencies, capabilities, descriptor/IO metadata and
original DXBC are retained. Source files and installed Mods remain read-only.

All six stages, flag cases, arrays, automatic slots, raw/typed resources,
include modes/fallback/cycles and failed-candidate preservation pass 116 checks;
33 generated modules pass spirv-val. The headless resource fixture compares
actual Vulkan and native D3D11 WARP output for an unchanged-style parameter
consumer: all bytes match the 65-float4 input. Native D3DCompile accepts empty
includes while standalone D3DPreprocess fails; a whitespace-only preprocessing
workaround preserves original empty dependency bytes and is tested.

The full rebuilt suite passes in build/next/run_all-1.1.6.out: resources 3544,
compiler 116, parser 726, evaluator 65, CPU runtime 68, character package/GPU
79/357, live INI GPU 28, validation 76, modes 40, MCP 47, evidence 113 and every
other step. Resource core/sync validation remains clean through shutdown.
This proves the compiler and the tested native GPU consumer, not general INI
CustomShader execution or game rendering. ABI remains 16b90a85431f86b4.

Continue immediately with owned shader descriptors/pipelines and original
CustomShader execution, then graphics, shader matching and directory integration.
No game input is needed. Details and remaining flag/device differences are in
docs/hlsl-compiler.md and LIMIT-030. The next review should also normalize
trailing NULs in native diagnostics so a warning cannot hide a later DXC error.
Next version 1.1.7; dist remains 1.0.2. PNG paused, LOD0/LOD1 only.

Material matching clarification (2026-10-14): deployed 1.0.2 executes the original main INI to choose logical pixel bindings. Exact fragment bytes select the verified source-slot/Vulkan-binding profile. Unprofiled fragments derive a profile from the same override section and pass kind, matching shared image ids first and otherwise unique view-type/format/extent signatures. Every sibling image must be represented, at most two target images may be extra, and equal-best candidates with different rules refuse derivation. Unknown correspondence keeps game images on the mod mesh and retries later; it does not by itself revert geometry. image_hash remains 0; original source-image hash override execution is not enabled in this character registry. This explanation inspected commits/code only, with no new game observation. Candidate image-command work does not replace the deployed character material policy.

## Original GPU parameters (verified candidate 1.1.5)

GpuResourceRuntime now owns the original float4 Texture1D/SRV, publishes zeros
before Constants and flushes at injected draw/dispatch and root pre/post ends.
Original dirty comparisons preserve signed-zero and NaN behavior. Key/preset
transitions force an upload before deferred post callbacks, including equal
values. iniparams reads the published image without flushing; references and
copies preserve their distinct behavior. Initial logical stage slots honor
Rendering.ini_params and explicit unbinding survives subsequent updates.

BUG-020 fixes omitted Constants post execution and abort leaking across root
phases. Nested abort still stops its own phase, including explicitly combined
pre/post nested calls. Post-Constants persistent writes remain dirty. Failed
load/draw scopes cannot submit partially recorded parameter updates. The host
must open its serialized queue domain before loading nonempty parameters; no
implicit queue submission, application descriptor binding or game wait is added.

The headless suite checks actual GPU values, key/hold restoration, zero-count
draws, dispatch boundaries, hunting skip, budgets, discard and retired owners.
A native DXC HLSL consumer reads Texture1D snapshots and a resized 65-element
vector. It is not the general CustomShader adapter. Frozen-source full regression
passes in build/next/run_all-1.1.5.out: resources 3509, parser 726, evaluation 65,
CPU runtime 68, character package/GPU 79/357, live INI GPU 28, validation 76,
modes 40, MCP 47, evidence 113 and all other steps. Resource validation stays
clean through device shutdown. ABI remains 16b90a85431f86b4.

Continue immediately with CustomShader source loading/compiler/descriptor and
execution adapters, then shared shader matching and complete directory graphics.
The user reaffirmed on 2026-10-14: finish each offline part and continue without
stopping until game testing is actually required. No game input is needed now.
Next source audit is in docs/gpu-resource-commands.md. SPIRV-Reflect SDK tag
1.4.321.0 was installed at F:/vulkan/SPIRV-Reflect-1.4.321.0, commit
e55086b044225f9b511ae44dbf9b079d3625943f, for the next shader work; it is not linked
in 1.1.5. Remaining image formats/conversions, resource hashes/properties,
application import and inline placement remain tracked. PNG stays paused.
Next version is 1.1.6; dist stays 1.0.2.

## Integer RTV and color/depth conversion (verified candidate 1.1.4)

Integer RTV clears now convert original floats with measured native WARP
truncation, NaN-to-zero and channel saturation. Tests compare actual pixels for
19 integer formats plus two typeless allocation/view cases across nine input
sets. Original int/hex options keep their distinct RTV/UAV semantics. Hardware
observations agree on fractions but differ for finite values outside the 32-bit
range; deterministic WARP saturation is the selected policy (LIMIT-029), not a
claim of cross-vendor identical overflow. Details: docs/gpu-resource-commands.md.

Single-sample R16/R32 color/depth family copies use a reusable shared-budget
resident buffer without CPU mapping, staging, implicit submission or waits.
Original INI copies preserve every mip/layer; lower-level tests also check
selected rectangles, independent offsets and untouched regions across all eight
compatible color formats. Native WARP copies provide a source-API reference.
Copy cache reuse, extension refusal, scope poisoning, budget exhaustion and
fence-retained image/scratch lifetime are covered.

Opaque color-to-D32 copies require actual enabled VK_EXT_depth_range_unrestricted.
Without it, copy_desc, D32-to-color and both R16 directions still work. A separate
headless device enables the extension for arbitrary float-bit round trips.
BUG-019 is fixed: host float-depth uploads validate copied values in [0,1] when
the extension is absent, ignoring pitch padding. The earlier packed DDS fixture
now contains valid depths; clean core/sync messages alone never proved that data
constraint. The low-level device-buffer-to-depth primitive retains an explicit
caller range obligation.

The final frozen-source rebuild/regression passes in build/next/run_all-1.1.4.out:
resource suite 3285, parser 726, evaluation 65, CPU runtime 68, character package/
GPU 79/357, live INI GPU 28, validation 76, modes 40, MCP 47, evidence 113 and every
remaining step. Resource tests have zero core/sync warnings/errors through device
shutdown; all 52 recorded source hashes match. ABI remains 16b90a85431f86b4.
Candidate stays in build/next; the current diff and memory form this checkpoint.

Next: GPU IniParams publication, preserving original zero initialization, dirty
float comparison and draw/list-boundary flushing; then CustomShader, shared shader
matching and complete enabled-directory graphics. Remaining format mappings
(alpha-only/BGRX/video), multisampled color/depth copies, source misc/sample
options and buffer hash/properties remain tracked resource follow-ups. Logical
bindings/viewport still do not emit shader or attachment commands. PNG stays
paused, no game input is needed, and the active goal remains incomplete.
Next version is 1.1.5. Dist remains 1.0.2.

## Original INI image commands (verified candidate 1.1.3)

GpuResourceRuntime now connects original texture declarations, bounded legacy/
DX10 DDS loading, SRV/UAV/RTV/DSV views, references, owned copy caches, ranges,
whole-pool assignments, clears, properties and color MSAA resolve to the owned
Vulkan executor. It calls original description/view helpers and reuses pinned
DirectXTK format/footprint decoding. Shader descriptors and draws remain separate.

Tests read actual pixels/bytes across 1D/2D/3D images, arrays, cubes, mip chains,
BC1/BC7 and separate DDS depth/stencil planes. They distinguish reference from
copy, restore earlier per-description cache bytes after alias/null changes,
preserve upper mips and exercise pool counters and retained-view lifetime.
Integer UAV clears preserve low channel bits and float NaN payloads; sRGB clears
use the view format through reusable budgeted scratch images. Native headless
WARP tests verify UNKNOWN-format inheritance and DXGI view-family restrictions.

BUG-018 is reproduced and fixed: stencil-only clears no longer pass an invalid
unused depth value to Vulkan. Explicit depth clears use D3D11 clamping and the
original depth/stencil flag priority. Review also found overvalidation of unused
dimension fields; a 1D fixture fails before the fix and now preserves the source's
ignored height/depth/sample options. Evidence is under build/resource-commands:
stencil-before-fix and unused-image-fields-before-fix, each with JSON/text logs.

The final frozen-source rebuild/regression passes in build/next/run_all-1.1.3.out:
resource suite 1682, parser 726, evaluation 65, CPU runtime 68, character package/
GPU 79/357, live INI GPU 28, validation 76, modes 40, MCP 47, evidence 113 and every
remaining step. Resource tests have zero core/sync warnings/errors through
shutdown, and final source hashes match. ABI remains 16b90a85431f86b4.
An earlier full run was intentionally interrupted for the unused-field fix;
run_all-1.1.3-interrupted-review.out is not a completed regression result.

Remaining image work includes integer RTV float conversion, unmapped DXGI
formats, R16/R32 color/depth family copies and source sample/misc options.
Logical bindings/viewport do not emit Vulkan shader or attachment state. Original
store is a buffer operation; no invented image-store semantics are claimed.
The source's empty Texture3D depth-to-Height bug is preserved/refused; valid volume
DDS and existing-volume copies work. Invalid source-API descriptions currently
refuse execution instead of reproducing a driver's log-and-continue behavior.

Next: remaining resource format/conversion cases and GPU IniParams publication,
then CustomShader, shared shader matching and complete enabled-directory graphics.
The source audit for IniParams in docs/gpu-resource-commands.md records its float4
Texture1D representation, zero initialization and draw/list-boundary flush order,
including signed-zero dirty tracking. Remaining buffer hash/properties stay open.
PNG is paused; no game input is needed. The active goal remains incomplete.
Next version is 1.1.4. The candidate is in build/next; dist stays 1.0.2.

## Owned mutable image foundation (verified candidate 1.1.2)

ResourceCommands now owns optimal-tiling 1D/2D/3D images, arrays, cubes, mip
chains and real image views. Pitched buffer-image transfers, compatible image
copies, color/depth/stencil clears and color MSAA resolve execute on an explicit
owned graphics queue. Actual capabilities and enabled device features gate
creation; dedicated allocation requirements and shared budgets are honored.
Core format facts for 184 formats come from the pinned Vulkan registry, checked
against the generated snapshot. This is not support for every format on a GPU.

Pending layouts stay with the submission. Successful submission publishes
GENERAL; discard or failed submit leaves the last submitted state intact.
Views/fence tickets retain image allocations after other owners retire. Tests
check actual bytes/pixels, pitches, untouched regions, arrays/mips/volume slices,
compressed edge blocks, depth/stencil independence, MSAA, negative descriptions,
budget limits, rejected submit and owner cleanup. No shader use of the views is
claimed. Original INI image execution is still the next adapter step.

Targeted and frozen-source full-suite resource checks: 563 passing, zero core/
sync validation messages through shutdown. Full rebuild/regression passes in
build/next/run_all-1.1.2.out: parser 726, evaluation 65, CPU runtime 68,
character package/GPU 79/357, live INI GPU 28, validation 76, modes 40,
MCP 47, evidence 113 and all other steps. Source hashes match the final report;
ABI stays 16b90a85431f86b4. The candidate is in build/next, not deployed.

Next: connect original texture declarations, view selection, image copies/cache,
clears and properties to GpuResourceRuntime. Source audit notes are in
docs/gpu-resource-commands.md, including zero-description defaults, the original
3D depth-to-Height override, typeless copy descriptions and view-specific clear
semantics. Do not clear a typeless alias using the allocation format when the
original uses a different DXGI view format. Reuse original CPU view helpers.
Then GPU IniParams, CustomShader, shared shader matching and directory graphics.
Remaining buffer hash/properties/conversions remain tracked. PNG is paused;
no game input is needed. The active goal is incomplete; next version is 1.1.3.

## Original keyed/expiring GPU pools (verified candidate 1.1.1)

Owned buffer pools now use original FIFO/spatial lookup, slot reassignment,
expiry and optional read refresh/reset. Reassigning a key does not automatically
clear the slot's resource. Expiry can null a pool entry while earlier bindings
retain its allocation. Reusing a mutable copy-cache allocation changes the bytes
seen by earlier bindings, matching the source; retained ownership is not a
historical-content snapshot. Physical ranges/whole-pool operations also work.

The host supplies original frame_time through Present and draw inputs. Spatial
radius follows the source's ceil scaling with that duration. The character host
forwards its existing measured duration (bounded to 1-250 ms); frontend default
zero remains the original startup value. Invalid duration/radius refuses before
numeric casts. No game clock or behavior has been verified in this round.

A new test exposed BUG-017: a CPU expression could fail after GPU recording yet
leave the pending scope submittable. Saved cpu-scope-before-fix.{json,txt} in
build/resource-commands. IniRuntime now notifies its extension on failed
Present/draw execution, including failures outside GPU callbacks. Failed active
scopes cannot submit and discard releases pending owners; prior CPU changes and
already-submitted store feedback are still not transactional.

Targeted suite: 485 checks, zero core/sync validation messages through shutdown.
Full frozen-source rebuild/regression passes in build/next/run_all-1.1.1.out:
parser 726, evaluation 65, CPU runtime 68, character package/GPU 79/357,
live INI GPU 28, validation 76 and all remaining steps. ABI stays 16b90a85431f86b4.

Next: general mutable images and their resource commands, then GPU IniParams,
CustomShader, shared shader matching and complete directory graphics. Remaining
buffer hash/properties/conversions and source-API invalid-description cases stay
open alongside image work. Existing DdsImage/ImageUpload only support immutable
2D single-layer assets; do not mistake them for a mutable array/volume backend.
Continue the explicit owned outside-render-pass domain before application import
or inline command placement. PNG is paused; no game input is needed. The active
goal remains incomplete; next version is 1.1.2. Dist is unchanged at 1.0.2.

## Original whole-pool GPU assignments (verified candidate 1.1.0)

Bare and wildcard whole-pool resource assignments now execute through the owned
buffer backend. The source allocation/view is retained before any destination
element changes. Each destination keeps its own copy limit, metadata overrides
and per-description allocation pool. Original proxy, mixed-element reset and
resolved-null behavior remain active; the original command is not mutated.
Source stride metadata is still queried per destination, as upstream does, even
when a type switch changes its dynamic index. The GPU source remains fixed.

All named copy modifiers now have their original buffer behavior, including
mono and set_viewport doing no additional buffer work, and resolve_msaa creating
or reusing a destination without copying bytes. These also pass through ranges
and whole-pool assignments. This does not implement image resolves or viewports.

Targeted GPU suite: 402 passing checks and zero core/sync validation messages
through shutdown. The frozen-source full rebuild/regression passes in
build/next/run_all-1.1.0.out: parser 726, evaluation 65, CPU runtime 68, character
package/GPU 79/357, live INI GPU 28, validation 76 and every remaining step.
ABI remains 16b90a85431f86b4. No deployment or original-installation writes.

Next offline work: keyed/expiring buffer pool lifetime, remaining resource
properties/conversions, mutable images, GPU IniParams, then CustomShader,
shared shader matching and complete directory graphics. Spatial pools need an
explicit original frame duration: IniRuntime currently leaves gFrameTime at its
zero default. Audit original slot reuse/reset semantics before removing the
pool restrictions. PNG remains paused; no game input is needed now. The active
goal remains incomplete. Next version is 1.1.1.

## Original buffer properties and target metadata (verified candidate 1.0.9)

The owned GPU extension now evaluates identity, stride/source_stride, size,
format, flags, offset and extent metadata with original precedence/sentinels.
Pure declaration overrides work during Constants without allocation or a GPU
scope. VB/IB offsets use the original helpers with a real RuntimeDraw context;
using those draw offsets from Present refuses. this-target property and region
arguments survive target resolution. Region/spatial hashes remain explicit gaps.

Source audit and a new GPU test reproduce incorrect inferred format after plain
pool view fetch against 1.0.8; preserved in
build/resource-commands/metadata-before-fix.{json,txt}. FillInMissingInfo now
applies view facts only for the original stage target types. Null retains its
cached owner/view, source_stride survives null and direct range fetch, and a
format/stride-only declaration remains unassigned. These source behaviors have
separate GPU/view/property fixtures; an invalid context poisons the scope.

Targeted suite: 303 checks passing, zero core/sync validation messages. Full
frozen-source rebuild/regression passes in build/next/run_all-1.0.9.out: parser
726, evaluation 65, CPU runtime 68, character package/GPU 79/357, live INI GPU 28,
validation 76 and all remaining steps. ABI remains 16b90a85431f86b4.
Further offline work: remaining buffer options and ownership cases, images,
GPU IniParams, CustomShader, shader matching and complete directory graphics.
PNG integration stays paused; no deployment/game/original-installation action.
Next buffer increment: whole-pool resource assignment and remaining copy flags
under their original buffer semantics. Whole-pool copy must resolve its source
once, retaining it through every destination assignment. Check upstream before
adding range behavior to other commands; ClearViewCommand itself has no range
loop. The next version after 1.0.9 is 1.1.0, per the user's digit-counter rule.

## Owned copy pools and original buffer regions (verified candidate 1.0.8)

Source audit found BUG-015: a resource assigned by reference was reused as a
writable copy destination, modifying its source; the single-description cache
also lost old contents after a size change. A GPU byte-result test first fails
against 1.0.7 (build/resource-commands/copy-alias-before-fix.{json,txt}). Copy
pools now own allocations independently of current aliases, keyed by the full
buffer description. Returning via copy_desc preserves the earlier allocation.

Buffer region copies, CB windows/rounding, complete-structure conversion, raw
view selection and no_view_cache execute with original source semantics.
Slot reads preserve only metadata actually returned by that source API; IB
copies use the bound index format's stride, and CB copies use its visible window.
Invalid ranges/overrides refuse before their copy; unwritten cached tails stay
untouched. A new allocation's unwritten tail is deliberately not asserted.

Targeted headless tests pass 243 checks with clean core/sync validation, including
copy alias isolation, repeated descriptions, known bytes, cache/view ownership,
retained generations and failed-scope cleanup. Full rebuilt regression passes in
build/next/run_all-1.0.8.out: source parser 726, evaluation 65, CPU runtime 68,
character package/GPU 79/357, live INI GPU 28, validation 76 and all other steps.
Further offline work: remaining properties/options, image operations, GPU
IniParams, CustomShader, shader matching and complete directory graphics. PNG
integration remains paused. No game, deployment or original-installation action.

Next source audit: property getters use target-specific GetViewInfo, so metadata
queried from a slot and from a plain pool-fetched view can differ. Add execution
fixtures before extending queries. Also check format-only unassigned declarations,
source_stride preservation on null/range fetch and original cached-null behavior.

## Original INI GPU slot ranges (verified candidate 1.0.7)

The original SlotRangeCopyOperation now executes buffer pool ranges, broadcasts,
dynamic bounds, null/unless_null and per-slot compatible copies through the owned
GPU extension. Plain range fetch retains its original direct assignment behavior:
no single-copy counter or destination override. Compatible source views survive;
fresh structured plain-range views expose one element, matching the source helper.
Constant-buffer broadcasts bind whole resources; unless_null retains an existing
region. Bounds are checked before original float-to-int casts.

The targeted headless GPU fixture passes 155 checks with zero core/sync validation
errors or warnings. It also covers byte results, range/slot count errors, invalid
numeric bounds, poisoned-scope refusal and generation/cache cleanup. The negative
index test initially followed an upstream comment, which differs from the code:
size_t arithmetic selects element zero for -1 in a three-element pool on this
64-bit build. The fixture now preserves the actual original result. Full rebuilt
regression passes in build/next/run_all-1.0.7.out: GPU resources 155, source syntax
726, evaluation 65, CPU runtime 68, character package/GPU 79/357, live INI GPU 28,
Vulkan validation 76 and all remaining steps. ABI remains 16b90a85431f86b4.

Fresh typed/raw plain-range views without an existing compatible view remain an
explicit refusal. General buffer copy regions/options/properties, images, GPU
IniParams, shaders and directory graphics remain offline work. The active goal
continues in the resource -> CustomShader -> matching -> directory order. No game
input, deployment, module exchange or original-installation writes; PNG paused.

## Original INI owned GPU buffers (verified candidate 1.0.6)

User directs continued offline implementation in this order: common resource
commands, CustomShader, shader matching/overrides, then enabled-directory GPU
execution. Stop for game input only when the offline work requires it. The goal
remains active; this buffer checkpoint does not complete the requested scope.

The opt-in GpuResourceRuntime extension uses the production original INI engine
to execute owned mutable Buffer/StructuredBuffer/ByteAddressBuffer declarations,
references, compatible copies/copy_desc, 32-bit typed and raw/structured clears,
GPU store feedback, dynamic slots and optimized bind/fetch batches. Original
pool reset/copy counters remain active. Slots are source-API logical bindings,
not emitted Vulkan descriptors. The old character renderer is unchanged.

ResourceCommands records private outside-render-pass command buffers on one
explicit host-serialized queue. Shared allocation budgets and fence-owned
resources survive generation retirement. Store waits explicitly for real GPU
readback and may drive the next original INI branch; no draw-hook wait or
application resource import has been added. Failed scopes cannot submit and
discarded generations cannot resume.

Targeted build/resource-commands/report.json: 120 checks pass, zero core/sync
validation errors or warnings through device shutdown. Checks cover byte results,
same-list branches, file padding, copy/alias isolation, regions, view semantics,
pool resets, per-frame limits, bad candidates, retained owners and failure cleanup.
The first full suite found the new SRV-clear fixture failing, with every other
step passing. Preserve build/next/run_all-1.0.6.out and
build/resource-commands/view-before-fix.{json,txt}; the adapter now retains view
kind/format/range and calls the original buffer-view element helper. In that
helper, size metadata is an end boundary, not a length after the offset; tests
preserve this behavior separately from CB region lengths. The following full
suite passes in build/next/run_all-1.0.6-final.out. Because final corrections were
made during that build, the final checkpoint is the passing frozen-source full
rebuild/regression in build/next/run_all-1.0.6-verified.out: GPU resources 120,
original syntax 726, evaluation 65, CPU runtime 68, character package/GPU 79/357,
live INI GPU 28, Vulkan validation 76 and all remaining steps.

Read docs/gpu-resource-commands.md. Still open: image commands/conversions,
general region copies/options/properties, slot ranges, keyed/expiring pools,
GPU IniParams, application scheduling/import, CustomShader, ShaderOverride/Regex
and complete directory drawing/reload. PNG integration remains paused. No game,
launcher, original Mod/Core, deployment or module exchange was touched.

## Original pool reset and reference semantics (verified candidate 1.0.5)

Continued source audit found that a scalar assignment discarded every resource
reference in the same pool, including unrelated slots. The adapter now mirrors
the original resource null state for assignment and observes it for references;
the original pool engine controls element switching and mixed-variable/resource
resets. CPU pool copy_desc, whole-pool proxy/index reset, independent resource
and variable resets, and reset_per_frame_limits execute through original methods.
No resource byte-copy or GPU clear/dispatch support is implied.

Twelve new adapter checks pass, bringing pool coverage to 23 checks. Eleven of
these first failed against 1.0.4, preserved in
build/ini-language/pool-before-1.0.5.json. Three additional production runtime
fixtures check owned file plans, per-generation isolation and null bindings.
The complete rebuild/regression passes in build/next/run_all-1.0.5.out: pool
adapter 23, production runtime 68 plus Yvonne inspection, source parser 726,
independent evaluation 65, character package/GPU 79/357, live INI GPU 28,
Vulkan validation 76 and every other step. ABI remains 16b90a85431f86b4.
Direct resource pools remain nonexpiring ring/static references; broader GPU
resource lifetime and full directory graphics execution remain open.

## Source-defined language and production CPU generations (1.0.4)

The user's latest request is to inspect upstream source and adapt every original
INI construct. PNG integration stays paused. Read docs/ini-language-coverage.md
for the grammar inventory and the separate execution boundaries; do not equate
the full-folder inspector with the production renderer.

- scripts/ini_language.py derives commands, operators, operands, target members,
  section metadata, formats, enums and setting reads from the pinned C++ sources.
  scripts/check_ini_language.py checks 726 parser cases, including negative
  diagnostics and every ordinary command keyword, all 48 operators and all
  source-declared shader/resource members. It checks source/executable freshness
  and distinguishes recognized deprecated notices from syntax errors.
- scripts/check_ini_evaluation.py runs all 48 operators through the production
  original engine with independent expected values: 65 checks including signed
  zero, precedence, negative rounding, parameters, time and draw context. These
  are CPU results; system/driver options and all argument combinations are not
  exhaustively validated.
- RuntimeConfig.mods_directory loads the enabled directory through original
  Includes, namespaces and exclusions. Directory generations parse dormant
  command lists too; bad candidates retain the previous generation. Two routed
  namespaces, companion reload and explicit project-only settings pass CPU tests.
  Legacy explicit-input profiles retain their scoped reachable Core graph.
- Key/preset-only callbacks now enter dependency compilation, including copied
  cycle callbacks. Presets are enumerated before Constants. Isolated CPU
  IniParams, original hold restoration, presets and abort handling are connected.
  Original cycle smart-start and accumulated run-list behavior are preserved.
- Expanded dependency loading exposed dangling optimizer-owned branch pointers;
  adaptation now follows only stable roots and surviving children. Native
  callback abort is covered by a red-then-green source-semantics fixture. Failed
  loads report the phase and bounded original diagnostics.

Reports: build/ini-language/report.json, build/ini-evaluation/report.json,
build/xxmi-runtime/runtime.txt and build/next/run_all-1.0.4-final.out. Final full
rebuild/regression passes after the native-abort correction: source parser 726,
evaluation 65, runtime 65 plus Yvonne inspection, native callbacks 15, character
package/GPU 79/357, live INI GPU 28, modes 40, validation 76 and every other step.
Shared ABI remains 16b90a85431f86b4. Pool reset/description follow-up is recorded
in 1.0.5 above; the general graphics families below remain open.

Non-native CustomShader, ShaderOverride and ShaderRegex sections still refuse
in the production character runtime. General copies/clear/store/readback,
non-indexed/indirect draw and dispatch, GPU IniParams, shader contracts and
complete directory rendering remain implementation work. The inspector accepts
their original syntax; that is not a working Vulkan adapter. No new real-game
result, source asset modification, launcher change, deployment or module swap.

## Earlier directory-wide language and asset checkpoint (1.0.3)

The user selected Yvonne Ensnared Heart 1.6 as a second regression input, then
explicitly deferred game collection and corrected the goal: load the enabled
Mods folder, support the original language generally, and apply the same generic
approach to models, bones and textures. Existing Mods are test inputs, not syntax
whitelists. Mandatory per-character captures and handcrafted component adapters
are not the intended architecture. D-044 and docs/mod-folder-runtime.md record
this; the former T-030 ideas-only wording is superseded.

Read-only offline evidence:
- build/ini-folder/report.json: original Include/namespace/command/resource/key
  parsing across 36 INIs, 1,330 sections, 655 resources, 77 pools, 175 keys and
  8,937 commands, no diagnostics. Includes Core, panels, companion files and
  RabbitFX; graphics execution is not claimed. Eight positive/negative directory
  fixtures pass and are included in sandbox/run_all.py.
- build/folder-assets/report.json: 445 file declarations, 443 unique paths;
  108 DDS files decode, 310 available buffer declarations align. Sixteen PNGs
  still need a Vulkan image loader; eleven declared files are absent. These
  findings do not establish reachability or visual defects. All sources unchanged.
- build/mod-corpus/report.json: all nine original exported skeleton initializers
  succeed. Six isolated callback fixtures succeed; Ardelia, Liino and Typhoeus
  need companion command lists/custom operations. Full-folder parsing already
  resolves their syntax, so isolated failures are not bad source INIs.
- Yvonne's generic mesh package is built at build/characters/yvonne/mesh.bin:
  nine components, 462 bones, fourteen IB sections including five independent
  hides, 72 default-scenario LOD draw records. No material mapping/game claim.
- The original Yvonne main INI now executes all fourteen entries with synthetic
  native inputs: 36 draws, six empty skips, 23 listeners including F10. The
  missing bone-source query boundary is adapted through explicit source-API
  facts; absent facts remain unknown and invalid slots refuse. The current game
  renderer has not acquired the extra source-slot facts for this character.

Source changes remove dense original-component numbering assumptions, preserve
IB-only suppression without a fake skeleton component. Texture-role planning
is an offline fixture only; production refuses it until the original shader
gating and fallback semantics are implemented. The deployed runtime is still one main
INI/profile. Complete directory generation/routing, dependency execution, shared
shader contracts, general custom shaders/copies/PNG and ShaderRegex remain open.
The initial host inspection had wrong constant/key ordering and omitted built-ins;
both were corrected. Empty-resource fixtures now distinguish file declarations
from unassigned null aliases. No source INI was changed to make tests pass.

The complete build and sandbox/run_all.py pass in
build/next/run_all-1.0.3-verified.out with EFMI_VK_DIST set to build/next. Original
runtime has 48 fixture checks plus the second main-INI inspection; package checks
79, character GPU runner 357, live INI GPU 28, validation 76 and all remaining
steps pass. The first regression failure was an independent fixture reusing
settings from another namespace; separate settings fixed it. Shared ABI remains
16b90a85431f86b4. No original installation/launcher/game writes.

Latest user steering: all original INI syntax must be adapted from upstream
source, not inferred from the installed Mod sample. Prioritize a source-defined
grammar/command inventory, complete shared parsing, runtime dependency loading
and execution adapters. PNG integration is paused while this work proceeds.

Before that steering, a standalone WIC PNG decoder was completed and checked:
build/png/report.json has 28 passing cases (12 synthetic/error/budget cases and
16 installed images compared byte-for-byte to Pillow). It preserves typed gray,
16-bit RGBA, alpha and default sRGB metadata, with shared source reservations.
It is not linked into ImageUpload, ResourceCache or the layer. No GPU upload or
panel rendering is claimed; keep its integration paused.

## 1.0.2: main INI, outfit keys, persistence and F10 (first game check)

User requests original INI runtime conditions, outfit keys, settings save and F10.
They also ask whether Vulkan can support the panel's HLSL. Implemented the scoped
original-engine runtime in frontend/xxmi/runtime*, native character Core adapters,
renderer resource catalogue and staged F10 generations; details in docs/live-ini.md
and D-043. The existing native bone/identity renderer remains, with contracts
validated against original evaluated pools. This is not arbitrary DX11 emulation.

- Original main INI tests: 45 checks including all 21 LOD0/LOD1 entry sections,
  all 13 original key listeners, conditional mesh ranges, alternate stocking DDS,
  generation isolation, foreground/hold release, atomic persistence and held F10.
- Actual hook/GPU fixture: 28 runner checks across both entries and one/two queues,
  including pixels after Ctrl+5, bad-INI and bad-DDS rollback, valid F10, retained
  old sealed commands, application restoration and no held-key retrigger. Clean
  core/synchronization validation through shutdown. Full suite passes in
  build/next/run_all-1.0.2.out, including the unchanged static character's 357
  checks, native packages 77, recipes 134, snapshots 52 and the probe/MCP suites.
- Prepared build/character/arcane-live.bin and its runtime sidecar from the old
  verified material profile. Live execution rereads the original main INI and
  reachable files; ten DDS images include the alternate stocking. Settings go to
  build/character/settings/arcane_fes.ini. Original mod/Core/DX11 settings stay
  read-only. Candidate/deployed `m2_package` now selects this live profile and
  `m2_enabled = 1`; hashing/profiling remain disabled as before.
- HLSL: installed checksum-verified official DXC 1.9.2609 only under F:/vulkan.
  Both stages of unmodified k/Draw_2D.hlsl compile and pass spirv-val for Vulkan
  1.1; bindings 100, 101 and 120 are explicit. Report build/panel-hlsl/report.json.
  No panel pipeline, cursor/IniParams adapter, backbuffer rendering or image pixels
  are claimed; k/1.ini is not among the runtime inputs (T-016).

Deployment: two stopped-process checks, backup
build/backups/before-1.0.2-20261013-223323/, fifteen hash-matched deployed files,
manifest build/deployment-1.0.2.json. Candidate and deployed actual-config smokes
both load through layer/inject on two enabled graphics queues using queue 1:
ten images Ready, zero failed, all published, clean validation. Reports:
build/character-assets-config-next-1.0.2.json and
build/character-assets-config-dist-1.0.2.json. Shared ABI remains
16b90a85431f86b4. No launcher setting, original mod/Core, agent-started game or
live module exchange changed in this implementation round.

Live follow-up: user-started PID 31108 loaded 1.0.2/inject/probe at 22:50:45.
The user reports working outfit keys and cannot see a change from F10. The log
records completed INI generations 1 and 2 at 22:53:43.879 and 22:53:49.113.
Two read-only statuses at 22:58:35/38 confirm two reloads, zero rejected/pending,
no reload error, ten Ready/published images and no failed images. All thirteen
saved variables match the latest status (stocking 1, outfit switches 0); dirty is
false. Evidence: captures/live-ini-1.0.2-20261013-225835-31108/.

Replacement continues after reload: 182 drawing frames add 6,155 replacements
and 6,107 intentional hides. This is not a clean stability interval: 504 variant
refusals, 72 identifying and two incomplete draws also occur. The retained
reason is the historical missing vertex buffer; no effect/shader correlation or
new visible defect is established. See LIMIT-027. No pre-F10 snapshot exists,
so choice preservation across these exchanges is not independently measured;
restart persistence and visual coverage of every key remain open. Unchanged
files and retained settings can naturally produce the same appearance on F10.

New user request: extend the current capability to arbitrary characters. Audit
profile/import and runtime assumptions, remove character-specific restrictions,
and validate a second character selected by the user. Do not claim arbitrary mod
compatibility from the existing character's result. Original sources remain
read-only; LOD2+ and multi-mod performance optimization remain outside scope.
T-016 panel drawing is still separate from successful HLSL compilation.

## 1.0.1: stable sampled drawing, first real module exchange, and clean dash interval

The user started another session (PID 19452, process creation 20:37:04) and reported that it seemed not to flicker. Live status confirms 1.0.1/inject/probe, an enabled/ready character backend and nine Ready images, none failed. Evidence is archived with fingerprints in captures/stability-1.0.1-20261013-204259-19452/.

- LOD0 baseline: 21.031 seconds, game foreground at all eight sample points, 1,534 drawing frames. Matched 94,087 = replaced 49,375 + intentionally hidden 44,712. Every refusal, pending, unreadable, incomplete and identification delta is zero; one identity remains stable.
- LOD1 follow-up: six captured original-draw frames, 58-60 character matches each, 39 pipelines with supported preflight. Across 101 drawing frames the counters add 5,896 matched = 2,950 replaced + 2,946 hidden, no refusal/identification/pending. Captures do not annotate replacement pixels; the live counters and user report are separate evidence.
- At 20:49:41, under the standing module authorization, scripts/hot_swap.py --no-build --out dist exchanges the already regression-verified 1.0.1 backend in that same process. Candidate and dist module hashes match deployment-1.0.1.json, e965c0e45ccf5ec6a3260016e65f1cdd5d0b1c4d91f8d14f0398f48b36804a39. Generation 0 to 1 in 0.8 seconds; nine images Ready, no module error. The user confirms normal appearance with no persistent anomaly. Ledger: build/hot-swaps.json; loaded copy: build/modules/efmi_vk_backend-20261013-204941-e965c0e4.dll. No source, deployed DLL/config, launcher or package was changed.
- Post-exchange interval: 10.016 seconds, 710 drawing frames, LOD0 and foreground at all three samples. Matched 43,527 = replaced 22,842 + hidden 20,685; every refusal/pending/identification delta remains zero. Counters reset on exchange; these deltas do not cross generations.
- The user confirms having dashed and then asks whether the apparently correct dash can be checked. A requested 24.156-second dash interval captures 58 frames, 3,766 matched character draws and 98 pipelines, all with supported preflight. Across 964 drawing frames there are about 62,120 matches, 5,040 sibling-derived and 1,235 inherited material replacements, and zero new refusal, pending or identification. Nine images remain Ready. Both historical after-image vertex shaders (8d5133a1, 2cf51393) are absent from these captured draws. This is a positive check of the sampled dash, not proof that LIMIT-027's different path was repaired.

Outside the clean intervals there were variant refusals and clustered identity events; the retained reason is the known missing vertex bone-buffer condition. They were not captured at draw level or precisely correlated with input, so do not label them as ongoing normal-state flicker or assign them to a new cause. No more game restart, duration-only run or repeated bone capture is required by these results. T-029's first functional game exchange is verified; repeated-exchange lifetime/performance and transient visual behavior beyond the user's report are not claimed. Continue T-027's original INI/Core runtime, keys, persist and reload for this character; keep the historical after-image path as an uncovered follow-up rather than declaring a present visual defect.

## Earlier post-session review: 1.0.1 loaded; user reports correct-looking models and textures

The user requested a review of the intervening work and the next steps. Commits 536878b, b90f9e9, 3b1e25e and ff4bd12 supply, respectively, four-set visibility-pass replacement, sibling material rules for fade/dither variants, recording-based character frame boundaries, and the exchangeable replacement module. The last deployment commit is 3f5c805. No source, deployed binary, configuration or running backend was changed during this review.

The latest game log records 1.0.1/inject/probe in PID 27816, two graphics queues in family 0, the configured 21-section package and the mesh upload. The user says models and textures seem fine; no particular scene, LOD, animation or effect coverage was specified. This is a user visual observation plus a recorded game load, not a complete acceptance test. Current process enumeration and probe discovery are empty, so no live status or frame was collected. The log does not record the exit reason; do not infer a crash.

Archived the latest session in captures/user-review-1.0.1-20261013/ with a fingerprinted manifest. It contains 50 new-identity-near-replaced-object lines, 38 saying zero frames, up to 20:30:37. These events are not correlated with user actions or pixels and do not prove visible flicker. The earlier expectation of no zero-frame identity lines has not been met; BUG-011's full real-game acceptance remains open. There are no module exchange log lines or build/hot-swaps.json ledger.

Recommended order: a short LOD0/LOD1 movement, dash, close-camera and combat check with counter deltas; one unchanged-code module exchange to verify T-029 in the game; investigate any observed identity or after-image issue (LIMIT-027); then T-027's live original INI/Core execution, keys, persist and reload for this character. This is the proposed next work, not a new source change or an exchange performed in this review. No duration-only test, LOD2+ or multi-mod optimization is required.

Current architecture boundary, established by the earlier architecture review and unchanged in 1.0.1: the pinned original XXMI engine executes selected mod callbacks and Core initialization in the standalone CPU host. The packager consumes those plans for one fixed scenario, embeds mesh/remap data and records DDS paths plus verified shader-slot profiles. The deployed DLL does not link the XXMI interpreter: native character tracking, merged skeletons, resources, pipeline variants and draw restoration execute the static package. Original Core texture queries, live INI conditions/keys/persist/reload and full hosted runtime integration remain T-027; standalone tests do not mean those are active in the game. The launcher provides injection.

## Earlier deployment checkpoint: 1.0.1 host and exchangeable module (2026-10-13, 20:25)

At this deployment checkpoint only "builds" and "loads in the sandbox" were established. The post-session review above supersedes the game-load status; an exchange in the game is still outstanding.

The user's decision of 2026-10-13: the replacement logic as a module of its own; swapping it into the running game is the agent's choice (AGENTS.md, D-042, T-029). Built as layer 1.0.1:
- `layer/src/character_module.h` is the boundary: `CharacterModule` (the backend's functions) and `HostApi` (log, CRC, the tracker's two buffer readers). The layer DLL keeps the code built in and uses it until told otherwise; `efmi_vk_backend.dll` (scripts/build.py, target `module`; source list MODULE_SOURCES plus layer/module/module_main.cpp) is the same code alone.
- `CommandOwner` (command_owner.h) is what a command buffer keeps of a backend; the tracker resets and seals it through virtual calls. `Device` holds the backend in use, a staged one, their function tables, `character_mx`, the frame counter of the character and the device properties for a later backend.
- Probe command `module` (ProbeClient.swap_module): load, stamp check, backend made and staged, served by the submit hook until ready, takes over at the end of a frame under the exclusive lock, old backend stopped on its own thread. A used module is never unloaded.
- The stamp `EFVK_ABI`: hash over state.h, character_module.h, command_owner.h, json.h and what they include from layer/src (ten headers now) plus compiler flags, version text left out. A module with another stamp is refused. A change to those headers, the hooks or the tracker needs the layer replaced with the game closed; scripts/hot_swap.py says so (exit 3) before building.
- `scripts/hot_swap.py --note "..."`: builds the module, copies it to build/modules/ under a name of its own, exchanges it in the one Endfield.exe process, waits, appends a record to build/hot-swaps.json.

Evidence: character scene, sixteen full runs through both entries, two exchanges each while frames are drawn plus a refused mismatched module, then disable, enable and pool reset on the module's backend, validation clean also at device destruction: 357 checks. scripts/hot_swap.py against the windowed sandbox with the candidate's real settings and the real mesh package: two exchanges of 0.2 s, clean exit. The headless texture program with the actual material package and its nine textures on two queues: the module's backend is staged, uploads all nine again and takes over 0.27 s later, validation clean (build/hot-swap-real-package-1.0.1.json). Full suite: build/next/run_all-1.0.1.out, all steps pass. Real-texture smoke with two queues passes (build/character-assets-config-next-1.0.1.json). Candidate layer DLLs SHA-256 f2a50b062f75de7a793abd8f0785a70a9dfe1a0f715d86ef909dba649bcd46d1, module e965c0e45ccf5ec6a3260016e65f1cdd5d0b1c4d91f8d14f0398f48b36804a39.

Two test faults met on the way, neither in the layer: the texture phase of the two-queue runs exchanged once before the scene counted its own exchanges from zero (the scene now counts from where it starts); and the scene began drawing before the backend's pool was filled when the machine was busy right after the build (`exhausted` 10; the scene now waits for `ready`, as the game is long past that when a character appears).

What an exchange looks like on screen: the new backend has no identity and no variants, so the character is the game's for the frames of identification and variant building.

Deployed at 20:23 with the game closed (full image list: neither game nor launcher): backup build/backups/before-1.0.1-20261013-202328/, fifteen files copied from build/next with equal hashes (the thirteen of before plus efmi_vk_backend.dll and its mismatched test copy), manifest build/deployment-1.0.1.json, real-texture smoke on dist with two graphics queues passes through both entries (build/character-assets-config-dist-1.0.1.json); dist files, layer log and launcher configuration unchanged by the smoke. The layer log was the same as at the 1.0.0 deployment: 1.0.0 was never loaded in the game.

Open, in the next user-started session: (1) the normal state while moving, which is the check of the frame-boundary fix: `identities_created` near the number of real jumps, no identity line with "0 frames" in the log, the user's eyes; (2) one exchange with unchanged code (`python scripts/hot_swap.py --note "first exchange in the game"`) as the test of the mechanism in the game: generation 1, nine images ready, nothing refused afterwards, and what the user sees during it; (3) from then on changes of the replacement logic by exchange. Then LIMIT-027 (after-images).

## 0.9.9 in the game: variants right; the normal-state flicker is the frame boundary; 1.0.0 built (2026-10-13, 19:20)

"Loads in the real game" and "data captured in the real game" apply to 0.9.9. 1.0.0 "builds" and passes "in the sandbox"; it has not been in the game.

User-started session with 0.9.9, PID 32004. The user: looking up is right now; the normal state still flickers. Read-only on that process, evidence in captures/flicker-0.9.9-20261013/ (three status pairs, the layer log):
- No refusal of any kind; `material_derived` 8,397, `material_inherited` 2,387; 26 fragment shaders took a sibling's rules (17 with one image more, 9 with the same images). This is the game result for BUG-010: fixed, by the user's eyes for the view from below.
- 19 new identities next to a replaced object in about 50 seconds: 16 within the same counted frame as the object's last draw (4 to 8 ms after it, 1 to 3 cells away), 3 one frame later 36 to 40 cells away. ISSUE-009 is answered: the backend's frame was the present, read at record time, and the game records its next frame before the previous present; the moved character was a new object for a frame and the game drew it (BUG-011).

1.0.0: the character's frame is the recording of its command buffer (`CmdBuf::character_frame`, cleared at begin and pool reset). Exact-place rule, one import per component and "a frame has passed" use it; counted presents pace periodic work and, with half a second, the expiry. Evidence: a sandbox phase with two recordings and no present between them, the character a cell further, passes with 1.0.0 and fails against the deployed 0.9.9; character package 77, character replacement 277; full suite build/next/run_all-1.0.0.out. All steps pass. Real-texture smoke with two queues on build/next passes through both entries (build/character-assets-config-next-1.0.0.json). Candidate DLLs SHA-256 4d25aa9d9369b6c3dec4109b0e8e8efd65237632702be7f3c12c399e417a1adc.

Deployed at 19:25 after the user closed the game: process check by full image list found neither game nor launcher; the 0.9.9 session's layer log is kept in captures/flicker-0.9.9-20261013/; backup build/backups/before-1.0.0-20261013-192546/; thirteen files copied from build/next with equal hashes; manifest build/deployment-1.0.0.json; real-texture smoke on dist with two graphics queues passes through both entries (build/character-assets-config-dist-1.0.0.json); dist files, layer log and launcher configuration unchanged by the smoke. The full suite ran on the identical bytes in build/next.

Open, in this order: in the next session `identities_created` should stay near the number of real jumps and the log should show no "0 frames" line; the user's judgement of the normal state. Then T-029 (deployment without leaving the game) and LIMIT-027 (after-images).

## 0.9.8 in the game: dither and dash break the picture; 0.9.9 built (2026-10-13, 19:00)

"Loads in the real game" and "data captured in the real game" apply to 0.9.8. 0.9.9 "builds" and passes "in the sandbox"; it has not been in the game.

User-started session with 0.9.8, PID 36720, nine images ready. The user: the normal state seemed fine at first; the dash flickers; looking up from below is "a big problem" (screenshot: the character dithered, original and mod model mixed); later, the normal state seems to flicker too. Read-only on that process, evidence in captures/flicker-0.9.8-20261013/:
- Dither frames (camera close, dash): every component the mod draws is on a fragment shader without a profile, in every pass. 0.9.8 refused these under `refused_material` (828 in five seconds with dashes) and drew the game's mesh while hidden components stayed hidden. Each such shader is the profiled one of the same component and pass plus one noise image, with set 1's bindings moved.
- Standing, twelve seconds: no refusal at all, 61 to 64 matched draws per frame, all at one position. So the normal-state flicker is not a refused draw. Session totals after about 23 minutes: `identifying` 2,653 (each counted draw is the game's model on screen), `refused_variant` 954, `incomplete` 4.
- Also without profile: a four-set overlay shader on all components, after-image shaders whose vertex shader has no bone buffer (cannot be replaced, LIMIT-027), and a shader for component 7 present in one stretch only.
- The visibility-pass fix of 0.9.8 has no direct confirmation: the user's first impression was that the normal state no longer flickers, the later one that it still does.

0.9.9 (D-040): rules for a shader without a profile from its profiled sibling through the bound images; without a sibling the application's images on the mod's mesh, never the game's mesh; variant cache 2,048; identity given up after two counted frames and half a second, radius growing with the gap, stale identity evicted when all slots are taken; counters `material_derived`, `material_inherited`, `identities_created`, `identities_expired`, status `material_derived_profiles`, log lines for derived rules and for new identities near a replaced object.

Evidence for 0.9.9, none of it from the game: scripts/sibling_replay.py over the 30 captured frames gives one stable sibling and rule set for all 25 dither variants on three-set pipelines (one extra image each); scripts/sibling_check.py puts those 25 real modules through the production preparation, all accepted with plans equal to the sibling's; native checks 77; the scene through the real hooks 277 checks over eighteen runs, validation clean; the deployed 0.9.8 fails the three new phases. Full suite: build/next/run_all-0.9.9.out. All steps pass. Real-texture smoke with two queues on build/next passes through both entries (build/character-assets-config-next-0.9.9.json). Candidate DLLs SHA-256 a31c2bf3e7795450bf66a7b544412ea89aa3fb66c3e44ffae6853cf1e9bc6ab4.

Deployed at 19:00 after the user reported the exit: process checks by full image list found neither game nor launcher; the 0.9.8 session's layer log is kept in captures/flicker-0.9.8-20261013/; backup build/backups/before-0.9.9-20261013-190005/; thirteen files copied from build/next with equal hashes; manifest build/deployment-0.9.9.json. Real-texture smoke on dist with the actual settings and two graphics queues (queue 1 only) passes through both entries, nine images ready (build/character-assets-config-dist-0.9.9.json). dist files, layer log and launcher configuration are unchanged by the smoke; the launcher was only read (inject mode SKIP, no DX11 arguments). The full suite ran on the identical bytes in build/next and was not repeated on dist.

Open, in a user-started session: dither and dash (expect `refused_material` 0, `material_derived` growing, the log naming the derived rules), and the normal state: if it still flickers, read `identities_created` and the identity lines of dist/efmi_vk_layer.log first; if identities are stable, capture frames with entries while it flickers (scratch tool capture_positions.py of this session, or scripts/collect_probe.py --snapshot mod --follow instance).

## 0.9.7 in the game: textures right, flicker persists; cause found in the visibility pass; 0.9.8 built (2026-10-13, 17:20)

"Loads in the real game" and "data captured in the real game" apply to 0.9.7 only. 0.9.8 "builds" and passes "in the sandbox"; it has not been in the game.

User-started Vulkan session, PID 7784, layer 0.9.7 with the material package. The user: textures seem right, the flicker is still there. Read-only on that process:
- Status: nine images ready, none failed; counters over 393 frames `refused_material` 85, every other refusal 0.
- Forty captured frames (captures/flicker-0.9.7-20261013/): all refused character draws are on pipelines 399/402/404/406 in render pass 12: no color attachment, depth format 130 tested EQUAL and not written, four-set layouts, bone buffer at set 3 binding 2, a fragment-written storage buffer at set 3 binding 3, no images. Of the 1,430 pipelines of that process 1,157 have three sets, 190 four, none more.
- These are the draws 0.10.0 counted as `refused_set3`. The 0.9.5 change (bind the application's set 3 again) was a real correction of a wrong refusal but not the cause of the flicker; BUG-006 is corrected accordingly and the cause is BUG-009.

Reading: the pass tells the game which parts are visible (its only output is that storage buffer). The original mesh fails the equal-depth test against the mod's depth, the game takes the part for hidden and leaves it out for some frames, and the mod's draws go with it. The refused draws are measured; that the flicker follows from them is an inference until a session with 0.9.8.

0.9.8: the private set's index is the pipeline's set count (variant preparation, builder, fragment images, restoration of an application set at that slot); a slot above the tracked four is used only while `Device::max_layout_sets` shows no application layout that large; a fragment shader whose layout has no fragment-stage image needs no material profile. Evidence: the new scene phase (character draw on the four-set pipeline) passes with 0.9.8 and fails against the deployed 0.9.7 with `refused_variant`; the real shaders of the four pipelines rewrite to set 4 and pass spirv-val; full suite in build/next passes (build/next/run_all-0.9.8.out); actual settings plus the nine real DDS files pass through both entries (build/character-assets-config-next-0.9.8.json). Candidate DLLs SHA-256 19ca13424e73e622af3a4a4703bcf709309731a4cfb2ecc58c681898c9b1f461. See docs/character-backend.md, last section.

Note on tools: `scripts/smoke_m2_config.py` reports `images_failed` 5 for a material package because its sandbox device does not enable BC compression; that is the script's device, not the layer. For a material package use `scripts/check_character_assets.py --actual-config`.

Deployed at 17:59 after the user reported the exit: three complete process checks found neither game nor launcher; backup build/backups/before-0.9.8-20261013-175909/ (thirteen files and the layer log); thirteen files copied from build/next, each hash equal to the tested candidate; manifest build/deployment-0.9.8.json. Real-texture smoke on dist with the actual settings and the game's queue setup (two graphics queues, only queue 1 submitting) passes through both entries: nine images ready, none failed, publication complete (build/character-assets-config-dist-0.9.8.json). dist files, layer log and launcher configuration are unchanged by the smoke. The launcher file differs from its state at the 0.9.7 deployment because the user ran the launcher in between; it was only read, and the two named values are as before (inject mode SKIP, no DX11 arguments). The 0.9.7 session's layer log is kept in captures/flicker-0.9.7-20261013/. The full suite ran on the identical bytes in build/next and was not repeated on dist.

Next, in a user-started session: loaded version 0.9.8, nine images ready, `refused_material`, `refused_variant` and `refused_set3` 0 with `replaced` growing, `last_reason` empty, and the user's judgement of the flicker. If it still flickers, capture frames while it does and compare which components the game draws in the frames before and after.

## 0.9.6 refused in the game; 0.9.7 deployed after normal exit

The user reports the original model after entering. Current-process evidence, not a historical inference: PID 18868 loaded 0.9.6/inject/probe at 15:56; log says "character package refused: materials require one unprotected graphics queue". Status is configured/enabled/ready false, replaced zero, image_count zero. This fails before mesh upload or texture setup. Read-only status/log and fingerprints saved in captures/materials-0.9.6-20261013/. The old status does not expose the requested queue inventory; do not invent an exact game count.

Fixed the overly strict queue requirement in 0.9.7 (BUG-008, D-038): all enabled queues in one unprotected graphics family are known, the first active one uploads, and one-time semaphore signals/waits publish the immutable images to every queue before it can execute a replacement. No render-path fence/idle wait and no worker submission to the application's queue. Status and logs now expose queue inventory/publication. The existing package and installed files are unchanged. Multiple graphics families/protected queues still refuse; the chosen uploader must keep submitting during preparation.

Full rebuilt suite passes (build/material-0.9.7-regression.txt), including character hooks 261 checks across eighteen cases, both entries and all three submit APIs. New cases cover queue 1 only with queue 0 unused, and uploads on queue 0 followed by material drawing on queue 1. Both old 0.9.6 entries reproduce the game refusal on the new two-queue fixture (build/material-queues-0.9.6-control.json). Actual candidate settings plus nine real DDS files pass on two queues with only queue 1 active; all nine Ready, publication complete, zero failures (build/character-assets-config-next-0.9.7.json). Both candidate DLLs SHA-256 3ff4517d29ac23cdd877b45f34f777f2146391488955d91f6eb181b40e02995d.

The user reported normal exit; complete process checks confirmed the game and launcher absent, including an immediate pre-copy check. Deployed thirteen verified files at 16:23, backup build/backups/before-0.9.7-20261013-162318/, manifest build/deployment-0.9.7.json. All hashes match the tested candidate. Actual-config two-queue smoke passes on dist through both entries (build/character-assets-config-dist-0.9.7.json), same nine Ready images and successful publication. Game log and launcher bytes are unchanged. No launcher edit, system setting change or agent-started game. The full suite ran on identical candidate bytes and was not repeated on dist.

The user starts the next game session; confirm 0.9.7, queue inventory, images_ready = 9, images_published true and replacement/material/refusal counters before visual acceptance. image_queue_waits counts queues actually used, so an enabled but unused queue need not have waited. The uploaded package and settings remain the same apart from the config's version comment.

## Earlier checkpoint: 0.9.6 static materials deployed

Completed the requested next step after Claude's whole-model mesh/bone backend. The material envelope carries the hosted scenario's per-draw file/null bindings and exact shader profiles. Shared ResourceCache uploads files, the existing variant helper isolates only FS images, and command-owned image arenas share bones and remain immutable across resubmission. Unknown/pending/failed materials retain the entire original component. No component-specific adapter, runtime keys, full Core texture-identity execution, LOD2+ or multi-mod optimization was added. See docs/character-materials.md and D-037.

Package build/character/arcane-materials.bin: 32,988,541 bytes, SHA-256 5bcae691949fcc0a8f3ccabf3719a349de1e17a316030521ad68c3d705e10218; unchanged mesh payload, 499 bones, 12 components, 21 sections, 28 draws, 20 exact fragment profiles and nine DDS files. LIMIT-021 has an explicit dx11-invalid-2d-to-3d-zero policy for this measured NVIDIA device only; default selection still refuses mismatches. Hardware evidence is not game pixel proof.

Full rebuilt suite passes in build/next (build/material-0.9.6-regression-final.txt): native character 67, shared snapshots 52, character-hook 127 across both DLL entries and render-pass versions, all existing steps. Initial layer-only material tests exposed missing loader initialization of internal upload command buffers; fixed before this final run (BUG-007). Rebuilt asset suite 253 and pinned host suites pass. Native production contracts cover all 370 historical draws, eleven with the named policy; all 105 pipeline combinations create, 211 checks and validation zero/zero (build/character-variants/report.json). These are sandbox/offline results.

Deployed thirteen files at 15:47 after three complete stopped-process checks. Both DLLs SHA-256 21b9be301fb8c6b80a268e4434c352a3a5d9e40e78b21a07f3609052e37dba83; backup build/backups/before-0.9.6-20261013-154745/, manifest build/deployment-0.9.6.json. Candidate and dist bytes match. Actual-config smoke passes through both entries on both locations: nine images Ready, nine file reads/submissions, zero failures, 216,268,800 resident image bytes. Game log unchanged. Full suite was not repeated on identical deployed bytes.

The user explicitly answered to switch back to Vulkan. In the active nested launcher JSON only Importers.EFMI.Importer.d3d11_mode_cmd_args (-force-d3d11 to empty) and xxmi_dll_inject_mode (DIRECT to SKIP) changed; all other bytes and signed values are preserved. The original JSON is in the same backup. Project settings: probe, resources on, image_hash/profile_tracker off, m2_enabled = 1, new material package. The agent did not start or stop the game.

Next checkpoint: user starts Vulkan and shows the character at LOD0, then LOD1 in the world. Confirm loaded 0.9.6 and images_ready = 9, images_failed = 0; sample replacement/material/refusal/restored_set3 counters after pending settles, and ask about the whole outfit, animation and flicker. Capture records still represent original draws (LIMIT-020). Do not substitute historical meshes, sandbox pixels or an empty capture for this round's visual acceptance. No repeated bone capture, tiny-part inspection, 30-minute duration or LOD2+ is required.

## Earlier checkpoint: LOD1 material evidence complete (2026-10-13)

The user-positioned LOD1 session is PID 33852, DX11. The first capture still had mod mesh draws and is retained as initial-mod-on. The user explicitly asked the agent to disable the mod; the probe sent F11 with observed key events and restored focus. Original frame 35553 contains 954 game calls, only two XXMI overlay calls and no XXMI indexed meshes. All 212 DXBC archives and 247 bound static texture readbacks succeeded; 452 files are fingerprinted in captures/dx11-lod1-20261013/manifest.json. Probe capture released; mod remains disabled. No installed file was edited.

Both historical Vulkan LOD1 frames reproduce the same complete pass sequences and three source profiles: 310c25c3 maps ps-t17 to (1,6) and ps-t18 to (1,3), uniquely by shape/format, not content; a6d7088a and 0aeef59b do not declare ps-t15/16/17 in the paired original DXBC. Adopted eight rules in docs/arcane-texture-slots.json. Evidence audit passes; native material replay now accepts 359/370 draws, with eleven existing dimension refusals and no unknown profile. Fragment relocation/null checks and rebuilt asset/host suites pass. See docs/lod1-material-capture.md for scope and evidence.

The latest user instruction is to finish the next work after Claude's whole-model replacement. Continue offline texture integration using the existing static character backend and shared GPU helpers. Keep LIMIT-021 device-specific compatibility explicit. The dynamic hosted frontend and runtime source-identity queries are separate. Dist/DLL versions and launcher values are unchanged; 0.9.5 flicker acceptance still awaits a user-started Vulkan session.

## DX11 LOD1 source capture prepared (2026-10-13, 14:28)

The user explicitly requested switching the launcher to DX11 and will enter the game, disable the mod and position the character at LOD1 before reporting readiness. The game and launcher were absent in complete process checks, including one immediately before the write. The active file is D:/XXMI-Launcher-Portable-v2.1.6/XXMI-Launcher-Portable-v2.1.6/XXMI Launcher Config.json. Only Importers.EFMI.Importer.d3d11_mode_cmd_args (empty to -force-d3d11) and xxmi_dll_inject_mode (SKIP to DIRECT) changed. The signed values, extra library list and custom launch remain byte-preserved. The already configured DX11 probe and Vulkan inject DLL both exist.

Backups of that JSON and the project-owned layer configuration, plus the exact changes and before/after SHA-256 values, are in build/backups/launcher-dx11-20261013-142837/. Whole-JSON comparison proves only the two authorized launcher values changed. dist/efmi_vk.cfg changes only mode = probe to mode = passthrough for this DX11 session; the package, enable flag, DLLs and installed mods are untouched. Exact-byte readback passed. This is configuration verification, not a new build, game load or capture.

On the user's ready message, verify the running DX11 probe, then use scripts/dx11_baseline.py with --archive-shaders and --all-bound-textures in a fresh capture folder. The targets remain LOD1 fragments 0aeef59b, 310c25c3 and a6d7088a, compared first with the saved Vulkan LOD1 bundles. No 30-minute run or LOD2+ data is needed. The separate 0.9.5 Vulkan flicker acceptance is pending; a later switch back requires the user's request and the usual stopped-process checks.

## First whole-character session in the game; flicker traced to set 3; 0.9.5 deployed (2026-10-13, 13:35)

Loads in the real game, replaces in the real game. The user started Vulkan; Endfield PID 41888 loaded build 0.10.0 at 12:57 (log: character package configured, 21 sections, mesh uploaded to device memory). The user reports the model "roughly fine" and sent a screenshot of the character in the mod's outfit in the open world, and reports that LOD 0 flickers now and then. The agent neither started nor stopped the game. Appearance beyond that report, animation quality and LOD 1 were not judged.

Live counters, read twice four seconds apart (378 frames): matched 23,212, replaced 12,099, hidden 11,030, `refused_set3` 82, every other refusal 0, no `incomplete`, `pending` or `exhausted`. Totals at 13:05: matched 2,665,586, replaced 1,359,667, items 3,442,115, hidden 1,278,664, identifying 3,033, pending 2,414, `refused_set3` 20,782, `refused_vertex` 1,025; one identity; `last_reason` "character draw with an application set 3 bound". So about one character draw in 150 fell back to the game's own mesh for a frame: the flicker. It is also the unexplained refusal of the 0.9.1 session (227 of 57,751).

Present intervals with the replacement active, last 2,048: mean 10.5 ms, p50 10.2, p95 13.0, p99 15.9, max 23.7; 19 over 16 ms, none over 33 ms. No like-for-like figure without the replacement was taken in this session.

Fix, 0.9.5. A set the tracker still shows at slot 3 is valid by Vulkan's rules, and the layout it was bound with agrees with the current one for sets 0 to 2. The tracker now keeps that layout's handle (`SetBind::layout_handle`); the backend records its replacement and then binds the application's set 3 again with that layout and its dynamic offsets (`restored_set3` counts it). Pushed descriptors at set 3 still refuse. The sandbox scene now does what the game does: a four-set pipeline draws, the character's draws follow with sets 0 to 2 bound, the four-set pipeline draws again without binding set 3. Against the deployed 0.10.0 that scene fails with `refused_set3` 10; with 0.9.5 it passes under core and synchronization validation, which would flag a set that was not put back.

Also in 0.9.5: a replaced stream may have an element up to 64 bytes past the file's stride, as the source API reads it (the LOD 1 texture coordinate file on a base mesh; this was `refused_vertex`), with 256 bytes of padding behind the mesh; the version rule.

Verified: full suite in `build/next` (character package 40, character replacement 53, everything else as before; `build/next/run_all-0.9.5.out`); smoke with the candidate settings and the real package (`build/character-actual-config-next-0.9.5.json`); both candidate DLLs `2c0ae7981238625d48c4c0931ee01be5593a88aef323dc2018ae152ab2718363`. Sandbox results; 0.9.5 has not been in the game.

Checked offline and not the cause (LIMIT-025): in LOD 0 world frames the two shadow passes read bones that differ from the frame's final state for two components, because the first shadow pass imports LOD 1 poses. The passes that make the picture read consistent bones.

Deployed at 13:28 after the user closed the game (process checked by name): thirteen files copied from build/next and compared by SHA-256, manifest build/deployment-0.9.5.json, smoke on dist/ with the real settings passes (build/character-actual-config-dist-0.9.5.json). The full suite was not run again on dist/; it ran on the same bytes in build/next. Next: the user starts again; read `refused_set3` (expected 0) and `restored_set3`, and ask about the flicker.

A slip to remember: the first process check of this round cut its output after four lines, missed Endfield behind three Codex processes, and the user was told the game had exited. It had not. Check for the one process by name, unshortened.

## 0.10.0: whole-character backend deployed; waits for a user-started session (2026-10-13, 12:55)

The user asked for everything to be replaced rather than one strip, and for the agent to build it ("do B directly"): first meshes and shared bones with the game's textures, textures afterwards. Built on the same day; details in `docs/character-backend.md`.

What it is. `scripts/package_character.py` turns one scenario of the hosted frontend's plans (what the original XXMI engine binds and draws per component, `build/xxmi-host/mod-plans.json`), the evaluated skeleton (`core-skeleton-plan.json`) and the mod's mesh files into one package: `build/character/arcane.bin`, 31,953,454 bytes, SHA-256 `f29c3cfceaa907e1b29dde090afd59cef32022f221f5435559bcc15350626e2e`, 499 bones, 12 components, 21 sections, 28 draws. In the layer, `character_package` checks it completely at load and `character_backend` runs it: section by index hash, object identity by the entry's translation, the core's identification rule (four components, one frame), import of every matched component into the identity's merged skeleton with LOD 1 remaps, one transaction per replaced draw through the shared variant cache and `emit_variant_draws`, empty lists hide the draw, every refusal keeps the original and has its own counter. `DrawSnapshots` gained shared skeleton copies; the mesh goes into device memory the host can map. `m2_package` selects the backend by the file's magic; the single-component path is unchanged.

Verified in the sandbox: full suite in `build/next` and on `dist/` without building, all steps pass; new are 37 native decoder checks, 49 checks over four GPU runs through the real hooks (three components, shared bones proven by pixels, hidden component, LOD remap, a neighbour that shares a mesh, disable and enable, clean validation, no refusal) and 13 for shared snapshot slices. Logs `build/next/run_all-0.10.0.out`, `build/deployment-0.10.0-regression.txt`.

Checked against saved game frames, offline: all 329 matched draws of six frames find the section the Python matcher found; all 426 replaced draw items get the vertex declaration the semantic reference gives; a model of the rules yields 24 to 32 replaced and 21 to 29 hidden draws per frame, 56 to 80 arena entries, no refusal; one translation per frame for all matched draws; four vertex slots bound and three-set pipelines throughout, except component 10's depth-only draw on four sets, which the mod hides (`build/character/arcane.check.json`).

Deployed at 12:50, game not running: backup `build/backups/before-0.10.0-20261013-125058/`, thirteen files copied from `build/next` and compared, both DLLs `13478e7588582b95d4c76da14f59d357b014fda249dd0687f6fa5cf4c09362c7`, manifest `build/deployment-0.10.0.json`. Settings: probe, resources on, image hashing and profiling off, `m2_package = F:/efmi vulkan/build/character/arcane.bin`, `m2_enabled = 1`. Smoke with these settings: backend ready, 31,950,446 bytes in device memory, validation clean, game log unchanged (`build/character-actual-config-dist.json`). No launcher edit, no game start.

Not verified, and the point of the next session: whether the game's driver creates the variants, what the character looks like, whether it animates correctly, what it costs. Expected on screen with a correct replacement: the mod's outfit in shape and motion, textured wrongly (the game's textures on the mod's coordinates), and six original components gone. In the first second or two parts appear one by one while variants compile. Read `status.replacement.counters`: `replaced`, `items` and `hidden` should grow, the `refused_*`, `unreadable_*`, `exhausted` and `incomplete` counters should stay at zero or be explained; `pending` stops growing once the variants exist. `{"cmd":"m2","enabled":0}` returns to the game's draws.

The visible check of component 7 prepared at 12:04 is superseded; its package and settings backup remain in `build/`.

## Visible replacement check prepared; waits for a restart of the game by the user (2026-10-13, 12:04)

The user chose the first recommendation of the review: see a replaced draw render in the game before anything else. Layer and binaries are unchanged (dist/ 0.9.1, both DLLs `d2850254...`). Only the package changes: `scripts/package_visible_check.py` derives `build/m2-component7-visible.bin` (SHA-256 `6d395edbe94b5401fd3f3e676dc7026a2864d07cbf02b17f990f0c40540fac4f`) from the reviewed package. The 20 vertices of the strip with x < 0 are scaled 3.5 times about that strip's centroid and moved 1 cm towards -y; the strip grows from 3.5 x 1.5 cm to 12.4 x 5.1 cm. The other strip, texture coordinates, blend data, indices and the match profile are byte-identical. The decoder of the deployed 0.9.1 (`dist/efmi_vk_mesh_test.exe --inspect`) accepts it.

`dist/efmi_vk.cfg` now names that package (backup `build/backups/efmi_vk.cfg-before-visible-check-20261013-120426`; `m2_enabled = 1` as before). The settings file is read when the DLL loads: Endfield PID 31340, started by the user at 11:57 under Vulkan, keeps the old package until it exits. No sandbox run was made with the new settings while the game was running. The user closed the game shortly after; at 12:08 the smoke with the deployed settings passed (`scripts/smoke_m2_config.py --dist dist --version 0.9.1`: 0.9.1, backend configured, enabled and ready with the new package, validation clean, normal exit, game log unchanged; `build/visible-check-actual-config-dist.json`). No launcher edit, no game start or stop by the agent.

What the draw is, from the saved 0.9.1 frame (pipeline 368, draws 86 and 87): depth test greater-or-equal without depth write, stencil test equal under mask 0x14, back-face culling, and multiplicative blending (source factor zero, destination factor source color). The strips darken what is under them, and only on pixels the stencil admits. Expected with a correct replacement: around one eye a much larger darkened patch that stays on the face, follows the head and deforms with the eyelid; the other eye as before. Wrong skinning would show as a patch that is missing, detached, stretched or jittering. `{"cmd":"m2","enabled":0}` on the pipe returns to the original draws for a direct comparison. If nothing differs at all, the result is inconclusive (texture content or stencil may hide the patch), not a pass.

After the check: restore the settings file from the backup, or set the package the next step needs.

The user then asked whether everything can be replaced instead of one strip. Not with what is deployed; what the sample needs per component is now listed under T-027 (measured from the mod's files, read-only).

## 0.9.3 material pipeline and command ownership (sandbox only)

Reviewed on 2026-10-13, 12:00 (second agent; implementation and design, see the session log and T-027): every suite rebuilt and rerun independently with the documented counts, no defect found, architecture judged sound. Open points recorded under T-027; new LIMIT-023 (a matched draw on a four-set pipeline, which the variant design cannot take). The review's first recommendation is a visible replacement check in the game before the frontend is linked into the DLL.

User scope, 2026-10-13: support LOD0 and LOD1 only. Do not develop or test LOD2 or later. Preserve the original Core pool stride when reading its declarations, but do not resolve/load later-LOD assets or make later LODs a completion gate.

User priority clarification: follow the plan from before the VRAM discussion. Finish this character's complete replacement through the general hosted frontend, resources, shared bones and draw backend first; multi-mod residency/performance optimization comes afterward. The unverified residency/eviction detour was removed from source and retained only as build/drafts/residency-unverified.patch. The verified ResourceCache from 6dfce2f is unchanged. Continue offline until actual game evidence is required.

The shared backend now accepts ordered FS image relocations and explicit-null requests alongside VS bone/entry isolation. Worker-built variants recreate compatible private descriptors and key on both vertex and material contracts. Limits include original descriptors as well as added images. DrawSnapshots capture per-draw views and retain resource owners until legal command reuse/destruction; they reject partial images before mutation and remain sealed across resubmission. The existing component-7 hook requests no material changes.

Full build/suite in build/next passes: unit 88, tracker 144, bones 43, recipes 134, cache 28, snapshots 34, variants 63, mesh 49, M2 hook 45, probes 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47 and evidence 113. Log: build/material-0.9.3-regression.txt. New GPU cases under no layer/layer/inject and RenderPass 1/2 verify red and green per-draw images, a null variant, original blue restoration, original FS buffer isolation, rejected transaction pixels and retired image/pipeline owners across resubmission. This uses shared backend code in headless fixtures, not the real-game hook emitting whole-character materials.

Next: native Core/material resolution and shared character identity/bone completeness. The hosted frontend now converts declarations into owned file plans and hands them to the standalone ResourceCache; it is not linked into the DLL. LIMIT-021 keeps incompatible 2D-for-3D replacements refused. No deployment, launcher/config edit, installed-asset write or game start. See docs/fragment-materials.md.

## General frontend draw plans (offline only)

Next evidence checkpoint (2026-10-13): completing the material profile requires source DX11 data for actual LOD1 fragments 0aeef59b, 310c25c3 and a6d7088a. Existing saved frames do not establish those three mappings; do not guess from image reuse. The DX11 capture helper now optionally archives original DXBC plus SHA-256/index/error records and includes inactive bound file-backed 2D textures. Eleven fake-probe checks pass; no game connection occurred. Use a new session folder and capture the user-positioned actual LOD1 with the mod disabled, after the user initiates/authorizes the DX11 session. Existing Vulkan LOD1 bundles are the first comparison targets. LIMIT-021 and source-identity/runtime/DLL integration remain separate unresolved work. This is an evidence boundary, not a claim that the entire frontend or character replacement is finished.

Historical driver checkpoint (2026-10-13): scripts/check_historical_variants.py combines the native full-Core vertex outputs and accepted material contracts with complete saved game pipeline/layout/shader recipes. Exact recipe/layout/source-and-mod-vertex/material/dynamic roundtrips precede creation. All 88 distinct bundle/pipeline/contracts covering 353 draws create on the RTX 5060 Laptop GPU, with 177 checks and zero validation diagnostics. Six unknown LOD1 correspondence and eleven dimension-mismatch draws remain explicit omissions. The fixture enables supported independent blending in its child device; this is driver construction, not actual character drawing, runtime texture identity resolution or DLL integration. Asset 253 and historical CPU/material replays pass. Reports build/historical-variants/report.json and runtime.txt. Layer/source versions and dist remain unchanged.

Original texture-query checkpoint (2026-10-13): DrawPlanAdapter mediates pixel CheckTextureOverride and filter queries through an explicit source-API texture identity provider. It uses original hash/draw-context matching, original range resolution and original callback execution, preserves pre/post ordering, tracks changed file/null bindings and restores nested this/phase state. Unknown identities, unadapted targets, unsupported fuzzy rules and recursion refuse. Registering the provider before CoreComponentBridge.install enables the original OverrideTextures graph automatically; absent providers retain unresolved events. Native texture checks 21 and actual Core API synthetic-identity checks 22 pass. Existing host, spatial 21, palette 24/283 and full historical 370-draw declaration/palette replay pass. Historical captures still lack complete source texture identities, so their 283 unresolved material calls are deliberately retained. Runtime identity/profile preparation and DLL integration remain open. Source/build/next 0.9.3 and dist 0.9.1 unchanged.

Combined GPU handoff checkpoint (2026-10-13): DrawTransaction composes Ready file bindings, resolved vertex/material declarations and compact palette owners into the shared asynchronous variant cache, command snapshots and ordered emitter. Late validation/pending states preserve complete output. Advancing inherited streams refuse without bounds proof; stride-zero application defaults remain supported. The standalone asset suite passes 253 checks with zero core/sync validation diagnostics. New pixel cases verify per-draw red/green images, explicit null, 16-bit bone indices, distinct first/base values, original FS buffers, original-state restoration and sealed resubmission after all producer caches retire. Historical material audit remains 353 accepted, six unknown LOD1 correspondences and eleven dimension refusals across 370 draws. No layer source/version change, DLL integration, deployment or game action. Native texture dispatch and full runtime/profile integration remain open.

Vertex declaration checkpoint (2026-10-13): VertexProfile retains exact VS code and observed semantic correspondence, calls the pinned original XXMI input-layout evaluator, then constructs shared stream patches from per-draw file strides. Default application streams remain intact; unsupported semantic relinking, inherited-stream edits, rates, aliases and late invalid declarations refuse atomically. All 18 native cases pass, including 12-to-16 and 4-to-8 byte blend streams. Full original entry replay now compares every vertex declaration for all 370 emitted draws as well as arguments/entries/palettes. All 17 actually drawn VS modules have prior paired semantics. Host, spatial 21, palette 24/283 and 384 callback plans pass. No GPU game-pipeline creation, whole-character hook, deployment or game result; native texture queries and combined backend handoff remain open. Source/build/next 0.9.3, dist 0.9.1.

Material contract checkpoint (2026-10-13): exact-fragment MaterialProfile separates unknown, source-undeclared and mapped slots. Per-draw contracts preserve explicit nulls and Ready image ownership; unsupported late requests refuse the entire transaction. Standalone GPU assets pass 221 checks with clean validation, including profile-selected replacement pixels. Historical material audit covers 370 draws across 21 fragment shaders: 353 accepted contracts, six expected unknown LOD1 correspondence refusals and eleven expected 2D-for-3D refusals. All accepted combined bytecode patches validate. This is not complete Core texture-override handling, GPU creation of game pipelines or character rendering. Missing LOD1 fragments are 0aeef59b, 310c25c3 and a6d7088a; identical bound images across passes cannot establish a DX slot mapping. Continue vertex declaration and remaining frontend/backend integration offline. Reports build/material-plans/report.json and build/assets-gpu/report.json.

Full original entry/Core checkpoint (2026-10-13): a generic CPU bridge now prepares evaluated skeleton metadata and mediates config reads, source selection and palette application while keeping original component matching, spatial decisions, instance iteration and callbacks. Pre/post roots form one bounded transaction. Five historical bundles (283 records, repeated for two synthetic updates) produce 566 plans, 283 original first-frame skips, 137 intentional mesh-empty callbacks and 370 draws; all arguments, patched entries and complete palettes match an independent reference. Four invalid-profile/changed-metadata/recovery/disabled-mod controls pass. Native bridge checks 14, original spatial 21 and palette 24/283 remain passing. OverrideTextures is still unresolved (283 calls); no material/GPU character execution or DLL linkage is claimed. Continue material/profile resolution and backend integration. Source/build/next 0.9.3 and dist 0.9.1 unchanged. Report build/core-components/report.json.

Native command bridge checkpoint (2026-10-13): named native handlers can invoke adapted original callbacks within the same bounded transaction; nested failures remain fatal and post callbacks resolve aliases after pre execution. Each indexed item retains its optional immutable palette through owned file plans and GPU-resource resolution. Host native bridge checks 12 and asset GPU checks 201 pass with clean validation; original spatial 21, palette 24/283 historical records, and 384 callback plans remain passing. This is the execution/ownership bridge, not the complete EFMI Core implementation or DLL integration.

Spatial palette epochs checkpoint (2026-10-13): SpatialPalettes now consumes original Core slot/hash results, keeps continuity only within the original time/proximity rules, retires expired/reused slots and invalidates on incomplete execution. It assigns no spatial slots itself. Original Core plus palette tests pass 21, including actual full-pool FIFO reuse; retained snapshots survive retirement. The caller must observe every identification and invalidate after a pool reset or failed original execution. Historical palette replay and 384 callback plans remain passing. Native Core/material execution and DLL integration remain open; no new game data or deployment.

Shared palette checkpoint (2026-10-13): PaletteBank owns bounded per-identity masters for LOD0/LOD1, with original component ids, per-pose completeness, better-LOD priority and immutable compact snapshots. The original initialization file loader owns all eight R16 remaps. Native palette checks 24 and metadata checks 15 pass; 283 historical records yield 74 imports, 21 complete snapshots equal to an independent reference, and 262 expected incomplete refusals. Each saved frame uses a separate identity, so no continuous tracking is inferred. Original spatial checks 12 and all 384 callback plans still pass. Caller-supplied identity epochs, native Core/material execution and DLL integration remain open. No layer/deployment/game change; report build/shared-palettes/report.json.

Spatial Core checkpoint (2026-10-13): original SpatialIdentification.ini and Helpers.ini now execute through mediated queries against borrowed CPU instance windows. Only SpatialHash buffer access is adapted; quantization uses pinned XXMI code and its original pool/state machine handles identification. Twelve headless cases pass for two instances, movement, new/expired objects, duplicate callers and invalid windows. Source/skeleton/callback regressions remain separate. This does not yet assign retained palette epochs or emit character draws. LOD0/LOD1 only; shared palette completeness and native Core/material/DLL integration remain next.

Core initialization checkpoint (2026-10-13): the hosted planner now executes original scalar pools, pool references and runtime callback aliases, with guarded indices and owned declaration references. read_skeleton_plan copies evaluated pool metadata without parsing INI text. Original sample initialization yields 499 bones, 8 instances, 13 declared slots/12 active components, and 8 file remaps; all agree with the older independent reference. Explicit identity and unassigned LODs stay distinct. Host tests: 23/28/19/15 plus 11 Core-pool and 10 skeleton metadata checks; 384 callback plans still pass. GPU initialization remains an unresolved native event. Shared spatial identity/completeness and Core/native material/DLL integration remain open. See docs/core-skeleton.md. Source/build/next 0.9.3 and dist 0.9.1 unchanged.

Per-draw checkpoint (2026-10-13): ordered file operations now resolve to separate VB/IB/material/layout/argument snapshots, retaining Ready owners and distinguishing inherited application bindings from explicit nulls. Late index/base/vertex failure refuses the complete transaction. Buffer uploads retain only typed whole-file index extrema for conservative bounds; all 404 actual sample draws fit this rule. GPU assets pass 200 with clean validation, all 28 actual BUF readbacks/extrema pass, and historical fragment checks pass. Host remains 23/28/19/15; sample plans remain 384. Logs build/file-draw-bindings-{gpu,host,mod,fragments}.txt. Native boundaries still refuse until resolved; original pipeline/input-rate validation and shared Core remain open. Standalone only; DLL/build/next 0.9.3 and dist 0.9.1 unchanged.

Owned-resource checkpoint: original XXMI declarations now become immutable file plans with explicit byte strides, index formats, ordered bindings/nulls, layout edits and native boundaries. The common cache handoff publishes typed owners only after every selected file is Ready; unknown resources/transformations refuse instead of guessing. Native host 23, planner 28, resource planner 19 and DDS 15 pass. All 384 actual callback plans resolve paths/usages/strides/formats consistently with source declarations. Headless GPU assets pass 161, validation clean; historical fragment checks pass. Logs build/frontend-resource-{host,mod,gpu}.txt. Native Core remains unresolved and no whole-character draw is emitted by this frontend yet. Continue per-draw state/shared character Core integration, not memory optimization. Source/build/next 0.9.3 and dist 0.9.1 unchanged.

Unified resource checkpoint: ResourceCache now serves images and VB/IB through one worker/outstanding upload/caller queue handoff, sharing budgets across kinds and generations. TextureCache names remain aliases. Headless fixture 149 passes plus five actual-cache checks for all 38 selected files (28 BUF, 10 DDS), with clean validation. Peak resident/staging/source bytes: 315,327,232 / 67,108,864 / 67,109,012. The material renderer loads mesh and images through one cache and retires it before drawing; retained owners render correctly. Report build/assets-gpu/report.json; log build/resource-cache-check.txt. Resource loading remains standalone; next resolve hosted frontend resource/command declarations and shared character/Core. Layer/build/next 0.9.3 and dist 0.9.1 unchanged.

Mesh-buffer checkpoint: immutable device-local vertex/index uploads now preserve exact bytes and expose handles only after polled completion, with actual resident/staging budgets. Headless asset fixture passes 138; all 28 callback-selected BUF files pass full upload/readback, unchanged source hashes, and the material pixel fixture renders with uploaded VB/IB. Core/sync validation is clean. Report build/assets-gpu/report.json; log build/buffer-upload-check.txt. This is standalone, no layer/source version or deployment change. Next: common buffer/image resource scheduling, then frontend and shared character/Core integration.

Explicit-null checkpoint: shared fragment analysis now zeros supported image samples/fetches and size/mip queries and removes selected opaque loads, preserving unrelated resources. It refuses mixed opaque selection/escapes and unsupported QueryLod rather than guessing. Headless asset suite passes 92 with clean core/sync validation; pixels distinguish absent resources from a zero 1x1 texture under white-border sampling and queries. Nine historical shaders/19 bindings also validate as adapter coverage, not a null policy for LIMIT-021. Source/build/next 0.9.2 and dist/0.9.1 unchanged. Next: production material descriptors/pipeline ownership and shared character/Core. See docs/fragment-materials.md.

Dimension follow-up: the three saved DX11 PS binaries were located and matched by zero-seed FNV-1 to capture hashes; all three declare/sample Texture3D. Their later 2D bindings violate the D3D11 contract. A separate headless pixel test passes four controls on NVIDIA and WARP but gives zero for mismatches on NVIDIA and texture color on WARP; API getters retain the view in both. Debug runtime unavailable, no validation claim. Keep default refusal; explicit-null handling and any named compatibility policy are separate work. Reports: build/dx11-dimensions/report.json and updated build/fragment-images/report.json. Still no game/layer/config change.

Fragment-material checkpoint: explicit FS image relocation preserves VS code, samplers and buffers, and traces opaque uses before accepting Depth=Unknown image types. Headless uploader/cache/material suite passes 79; pixel proof covers red private FS/unchanged blue VS source, original next draw, explicit zero 2D fallback and retired cache ownership under clean validation. Historical SPIR-V: 16 moves across seven shaders validate and match the Python reference. Three of the 19 original-resource correspondences are 3D placeholders; DX11 mod-on captures later bind 2D assets there. They are annotated/refused, not removed as bad original pairings. See docs/fragment-materials.md and LIMIT-021. Production descriptors/variant integration and source dimension semantics remain open; no layer change or deployment.

Texture-cache checkpoint: bounded asynchronous demand loading now deduplicates declarations, leaves unused missing files dormant, prepares/records on a worker, submits only on the caller's fixed queue and publishes after worker fence polling. Shared allocation ledgers retain charges through old-generation GPU owners. Headless checks pass 57 plus five actual-cache checks and ten full DDS payload readbacks, with clean core/sync validation. The sample's ten cached images peak at 283,377,664 resident bytes, 67,108,864 staging bytes and 67,109,012 source bytes. See docs/resource-assets.md and build/assets-gpu/report.json. This remains standalone; buffer resources, material descriptors, shared character/Core and layer integration are open.

GPU asset checkpoint: a separate ImageUpload ticket now prepares/upload-transitions DDS images, withholds image/view handles until a completion fence is polled, and releases staging after completion. Native headless GPU checks pass 27; all ten actual selected textures pass complete compressed-byte upload/readback with zero core/sync validation diagnostics (build/assets-gpu/report.json). GPU blit tests distinguish linear/sRGB interpretation. It is not linked into the layer and has no upload scheduler, descriptor binding or game overhead result. Caller must serialize submission on its graphics queue and retain Ready images through all uses; no cross-queue ownership or game feature enablement is implemented. See docs/resource-assets.md.

Asset decoding checkpoint: frontend/assets/dds.cpp preserves compressed bytes, color-space formats and mip ranges without copying payloads. All 15 native decoder checks pass alongside host 23/planner 28; all ten selected actual DDS files pass independent header/range comparison. Bound asset total is 315,328,648 bytes (four 8K/64 MiB textures). Two missing DDS declarations are unreferenced in the tested callbacks, so demand loading must distinguish declaration from use. See docs/resource-assets.md. The later GPU ticket above extends this; material binding and layer integration are still open. Layer source/build/next and dist versions are unchanged.

The original XXMI command trees now produce ordered resource-binding/draw/layout requests through frontend/xxmi/draw_plan.cpp. No component id or sample asset name occurs in this adapter. Native Core boundaries stay explicit unresolved operations; unsupported commands/queries, invalid arguments and budgets reject the whole plan. Native checks pass 28 in addition to the 23 host checks. The original caller skip flag/layout are not changed. Resource declarations are borrowed from a retained frontend generation; frontend variable effects are not rolled back.

The actual sample INI (unchanged SHA-256 697d0c98378b83d3aef592a54536470bff4410993f475c1514ed701c4b6f71bd) executes all 12 draw callbacks in 16 selected scalar scenarios at both LOD values: 384 plans, 404 draw items, 192 mesh-empty runs, 18 unique index ranges and 38 bound resource declarations. Every selected range fits its actual index/vertex files. Reports: build/xxmi-host/report.json and mod-plan-report.json. Details: docs/frontend-draw-plans.md. Core initialization/shared bones, resource GPU ownership, material descriptors, real keys/persist/reload and the panel remain open. No layer rebuild was required for this standalone frontend-only change; no deployment or game action.

## Hosted XXMI frontend feasibility (standalone CPU only)

The pinned 1.2.2 sources now compile/link in a headless host and pass 23 native CPU checks: expressions/control flow/post phase, pools/expiration/aliases, callback references, duplicate-section distinctions, Region arguments, ElementFormat execution and duplicate synthetic key listeners. Reproducible builder rejects changed dependency revisions/trees, fingerprints inputs, and never executes a stale binary after failure. Report: build/xxmi-host/report.json; method and limits: docs/xxmi-host-prototype.md.

This closes D-016's bounded host feasibility checkpoint, not full INI/Core compatibility. The reference D3D-shaped headers are compile scaffolding, with unused graphics no-ops; missing local GPU adapters explicitly refuse. No graphics device, real key input, installed-mod execution or layer integration occurred. No DLL/deployment/config change; full 0.9.2 regression above remains applicable. Continue general resource/shared-skeleton/material integration offline.

## 0.9.2 shared declaration editing and draw transactions (sandbox only)

Added data-driven stream contracts (original/replacement stride and element offset/format) and ordered multi-draw transactions. The legacy M2 package delegates to these operations; its restrictive eligibility and configuration are unchanged. Contracts preserve shader locations and untouched streams, and reject incomplete/ambiguous mappings atomically. Transactions preflight all resolved items before emitting anything, allow an intentional empty skip, and restore the application's bindings once after all draws. No component identifiers are used by either operation.

Full build/suite passes in build/next: unit 88, tracker 144, bones 43, recipes 134, cache 21, snapshots 25, mesh/declarations 49, variants 39, M2 hook 45, probes 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. Log: build/general-draw-0.9.2-regression.txt. The GPU fixture renders two private ranges and the original next draw, rejects an entire partly ready transaction with pixel proof, and resubmits sealed storage under clean core/sync validation. Details: docs/general-replacement-operations.md. No deployment or new game request; frontend/shared character skeleton/material integration remains open.

## 0.9.1 first game replacement and direction for general mod support

User-started Endfield PID 3080 loaded 0.9.1 at 19:22:00 with the component-7 package enabled. In a 4.008-second status window, matched and replaced both increase by 806, with pending 22 and unsupported 96 unchanged, and unreadable/exhausted zero. Later counters are matched 57,751, replaced 57,502, pending 22 and unsupported 227. The grouped last refusal names primary/classic/single-instance/free-set-3/extended-binding constraints but does not identify which one failed; do not claim the whole session had no new refusals. The user confirmed voluntary exit. No runtime switch, deployment, launcher edit or automatic game start occurred.

Saved display frame 51,570 contains 46 matched draws; component 7 is draw 86/87 on supported pipeline 368. Both instance entries and both bone windows compare equal at record/submit. The 176 bundle file fingerprints verify. Pipeline 437 for component 10 draw 38 lacks the current three-set layout precondition; full-frame bone replay refused before execution, not passed. Evidence and limits: docs/m2-first-replacement.md. Captures retain the application's original draw state (LIMIT-020), so counters are execution evidence, not pixel proof.

Mesh-space inspection locates component 7 in two small strips near the eyes. Its XYZ positions equal the live original exactly; packed vertex/UV bytes do not all match. The user cannot reasonably judge this tiny part. Stop requesting microscopic visual acceptance; use a substantial character replacement at the next appropriate game checkpoint. D-033 keeps component differences in data and preserves the hosted XXMI frontend direction of D-016. Shared skeleton assembly, resource/declaration handling, textures, INI execution, toggles and panel remain work, not implemented compatibility.

The sections below are historical checkpoints; the earlier pause and no-game statements no longer describe the current state.

## Review of 0.8.1 to 0.9.0: one defect, corrected in 0.9.1 and deployed (2026-10-12, 19:25)

Requested by the user ("continue the review"); covers `c1bb6db` to `2d64775`. The game was not running.

The defect: `MergedSkeleton::patch_entry` wrote `(1536, 3033)` into constant 5 of the patched entry for the sample; the right pair is `(1533, 3030)`. The game's vertex shaders read bone `i` at `offset + 3 + 3 * i`. Evidence, each independent of the others: `MergedSkeleton.ini` passes `$merged_skeleton_offset - 3`; the DX11 baseline has `(1533, 3030, 258)` in all 23 patched vertex entries of `frame-a` and of `frame-on`; the disassembled Vulkan vertex shader of the component 7 draw (shader 111231) adds 3 before `3 * index`. With 0.9.0 the replaced component would have been skinned with each next bone's matrix. The suite did not show it because the native test, the Python reference and the sandbox vertex shader shared the reading.

Changed: `patch_entry` (one subtraction), the native expectations plus a check of the DX11 pair, the reference in `scripts/verify_bone_import.py`, the M2 sandbox vertex shader (now `offset + 3 + 3 * index`, original mesh on bone 0), `scripts/smoke_m2_config.py` (takes the expected version from the source), version 0.9.1. With the corrected fixture the 0.9.0 layer fails all four GPU configurations (`build/review-0.9.0/`, 5 of 13 checks); 0.9.1 passes.

Verified: full build and suite in `build/next` (unit 88, tracker 144, bones 43, recipes 120, cache 21, snapshots 25, variants 39, mesh 30, M2 45, 17 probe cases 517 to 532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113; `build/review-0.9.0/run_all-0.9.1.out`). Replay of the five saved game frames: 283 records, master hash unchanged, every patched entry `(1533, 3030)` (`build/bone-replay-yuopgr1q/report.json`). These are sandbox and offline results.

Deployed at 19:15: backup `build/backups/before-0.9.1-20261012-191540/`, eleven tested binaries copied from `build/next` and compared by SHA-256, both DLLs `d2850254ded6477c777fc1f06d5f93c8ce921f74897e2881ca996fb8b7cae417`; the settings file keeps its values, only its first comment line names 0.9.1. Manifest `build/deployment-0.9.1.json`. Suite on `dist/` without building passes (`build/deployment-0.9.1-regression.txt`); smoke with the real settings: 0.9.1, backend configured, enabled and ready, validation clean, game log unchanged (`build/m2-component-actual-config-dist-0.9.1.json`). No launcher edit, no game start.

The rest of the review found no further defect. Read in full: `m2_backend`, `variant_cache`, `draw_snapshots`, `merged_skeleton`, `m2_mesh`, the hook and tracker changes. Checked against game data: the entry is chosen by `gl_InstanceIndex`, so requiring one instance with first instance 0 and reading the entry at the start of the window is right; the 0.8.1 session used no `Bind*2` call, no command buffer reset or free, 23 command buffer allocations, and its graphics draws sit in one command buffer per frame (ids 1 to 3 seen), well inside the limit of eight arenas; the package matches `CommandList_Draw_Component7` of the mod (four buffers, `drawindexed = 108,0,0`); binding 4 is carried over as T-022 asked. The independent rebuild of 0.9.0 passed with Codex's counts before any change. One limit recorded: a capture does not mark replaced draws (LIMIT-020).

What the next session has to watch, besides appearance: `status.replacement` should show `replaced` growing with `unsupported`, `unreadable` and `exhausted` at zero or explained by `last_reason`. Only component 7 is replaced, with the game's own textures; the mod's index list for it equals the game's (written as 16-bit it hashes to `a3fe5c47`) and the INI calls the mesh a copy, so a correct replacement should look like the original and a wrong one shows as a broken part; the vertex positions were not compared. `{"cmd":"m2","enabled":0}` on the pipe returns to the original draws for later recordings.

## 0.9.0 deployed and verified; paused at the user's request

The user reported normal game exit and then requested a pause after configuration. Verified no Endfield or sandbox process before backup and again before copying. Backed up all old dist/ files with verified fingerprints to build/backups/before-0.9.0-20261012-185849/. Copied the twelve exact candidate files, including cfg, and checked each against build/candidate-0.9.0.json. Both DLLs SHA-256: 791e7d163c82562acc8732947d615b5caa38530d42ef67b18e618a02d7259d00. The separately referenced project-owned component package is unchanged, SHA-256 318e8354297fcdfd4b352d0ec031a6b2ba6472335fefe0be2c1b16f3eed52167. Manifest: build/deployment-0.9.0.json; source commit: 592247d.

Deployed actual-config inject smoke passes with backend configured/enabled/ready, correct ordinary capture/hash/rewrite, normal sandbox exit and clean core/sync validation. The actual game log hash is unchanged during the smoke. Report: build/m2-component-actual-config-dist.json. The earlier complete regression applies to these identical binaries; no rebuild or repeated full suite. Settings: probe/resources on, image hashing/profiling off, explicit component-7 package enabled, full unmap/WC defaults retained. No launcher edit or game start.

Work is paused. On an explicit resume, the next game checkpoint is a user-started display/LOD0 observation, then actual LOD1: check loaded version, ready/replaced/refusal counters and appearance/animation. Only component 7 is enabled, not the full mod. Do not begin collection or further development while the pause stands. Sections below record earlier checkpoints.

## 0.9.0 first hooked component ready for deployment after game exit

Implemented an opt-in, bounded component package and hooked replacement. It checks the exact index region and supported state, requests a variant asynchronously, imports both poses from mapped source bytes, appends private VS data and emits/restores one indexed draw. Pending/invalid/unsupported requests keep the original. Completed command buffers reuse their arena after a successful legal begin/reset/pool reset; sealed recordings survive resubmission unchanged. Workers and recorded owners release before device destruction. See docs/m2-component-hook.md, T-026 and D-031.

Native mesh checks pass 30; actual-hook runner 45 covers both entries and RenderPass 1/2 with 30 pose updates, original next-draw restoration, reset/pool reuse, live switching, sealed resubmission, unmapped-bone fallback and remap recovery. GPU core/sync validation is clean. Full build/regression passes: unit 88, tracker 144, bones 42, recipes 120, cache 21, snapshots 25, variants 39, M2 45, 17 probes 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. Logs: build/m2-integration-regression-20261012.txt (initial test-only CRC initialization failure), build/m2-integration-regression-final-20261012.txt (corrected native executable and all steps passed).

The unchanged sample component-7 bytes are in build/m2-component7.bin with source/INI fingerprints. Production decoder accepts the package: 40 vertices, 108 indices, bones 266-277 of 499. All ten matching draws across five saved display/LOD0/LOD1 frames meet its draw/declaration conditions (build/m2-component7-capture-check.json). Candidate actual-config smoke passes under validation with normal sandbox exit, backend ready, correct ordinary hashes/capture/rewrite and unchanged game log (build/m2-component-actual-config-next.json).

Candidate cfg: probe, resources on, image hashing/profiling off, m2_package = F:/efmi vulkan/build/m2-component7.bin, m2_enabled = 1; full unmap/WC defaults retained. Default source behavior remains no replacement without explicit package/enable settings. No game restart, launcher edit or deployment yet. Real-character appearance/animation, actual game variant creation and overhead remain unverified; only component 7 is implemented, not the full mod or its textures/image panel. The next run should begin in display/LOD0, then actual LOD1, with counters and an optional live off/on comparison. No duration-only or repeated bone capture is needed. Entries below are historical checkpoints.

## 0.8.3 asynchronous variants and private snapshots verified in the sandbox

VariantCache bounds/deduplicates owned requests, builds outside the drawing thread, retains failed results, cancels retired jobs and joins on shutdown. Recorded command owners retain ready pipelines. DrawSnapshots preallocates aligned per-draw bone/entry slices with full uniform-range padding; seal disallows further writes/recreation. Native cache 21 and snapshot 25 checks pass. The six positive headless GPU configurations use both production helpers, poison input arrays, retire/stop the cache before drawing via retained ownership, and resubmit immutable commands; pixels/restoration and core/sync validation pass. Three negative canaries remain active.

Full build/regression passes (build/variant-cache-regression-20261012.txt): unit 88, tracker 144, bones 42, recipes 120, cache 21, snapshots 25, variant runner 39, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. Scope: docs/m2-cache-and-snapshots.md. No live hook integration or game replacement yet; dist/ untouched. Continue implementing the minimal self-contained sample component and command-recording resource ownership.

## 0.8.2 CPU bone import verified offline

Added a bounded, atomic rolling-buffer importer and 256-byte instance patcher, preserving Core's leading source matrix, 512 implicit identities, current/previous slices, remap direction, mesh scale and finer-LOD update priority. Probe hooks do not invoke it. Native tests pass 42 checks. Independent Python replay over the saved display, two LOD0-world and two actual LOD1 frames matches production C++ after every one of 283 records (74 imports, 209 repeats, 407,808-byte master). This is not comparison with a captured DX11 compute-output buffer or a rendered character. Details: docs/m2-bone-import.md and D-030.

Full build/regression in build/next passes (build/bone-import-regression-20261012.txt): unit 88, tracker 144, bones 42, recipes 120, variants 37, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. No deployment or new game observation is required for this helper. Continue the M2 backend.

## 0.8.1 actual LOD1 capture succeeds (2026-10-12)

User-started Endfield PID 34644 loaded 0.8.1 probe/inject with resources on, image hashing/profiling off. The user explicitly kept LOD1. Both collections select their first attempt after skipping 8/7 empty present intervals: frames 19506/20414, 818/819 graphics draws plus 60 dispatches each, 35 render passes. All nine LOD1 sections are present, with 47 LOD1 draws (25 color-subpass draws) plus 11 unchanged base-section draws. All 36 matched recipes are complete/preflight-supported. In each frame all 58 entries and both actual VS 12 KiB bone windows agree at record and submit; no missing relevant snapshots. All 147 bundle file fingerprints per frame and the series manifest fingerprints verify.

Evidence: captures/m2-0.8.1-20261012/lod1-a/attempt-01 and lod1-b/attempt-01; report: docs/m2-game-recipes.md. T-023 is closed for this observed scene; exact present producer remains unproven. This supplies LOD1 color-pass record-time availability, not replacement or animation proof. No further game action is needed for offline bone import/remapping. Keep building in build/next while the game holds dist/0.8.1. All earlier entries below are historical checkpoints.

## 0.8.1 deployed after normal exit (2026-10-12, 17:36)

The user reported normal exit; game absence was checked before backup and again before copying. Backed up old dist/ files to build/backups/before-0.8.1-20261012-173645/. Copied the eight exact tested package files and verified every SHA-256 against build/next; both DLLs are 7e311a9f9f358aa50d90c579abfc1384877a8b391aea8ef886d1e87a90ba50d7. Manifest: build/deployment-0.8.1.json. Deployed actual-config inject smoke passes (build/capture-wait-actual-config-dist.json), including the new snapshot wait, five correct hashes before/after rewrite, full recipe and unchanged actual game log. No rebuild in dist/ and no repeated full suite was needed for byte-identical files.

Settings remain probe, resources on, image hashing/profiling off; full unmap/WC defaults retained. No launcher change or game launch. Next requires the user to enter the actual LOD1 state again, then collect with --wait-draws --snapshot mod --follow instance --require-section '*_LOD1' --require-color --attempts 8 --pipeline-limit 128. The recipes and scene corrections from this round are already saved. No LOD0 recapture or duration-only test; LOD1 color-pass bone timing and game effectiveness of the capture fix are still open.

## 0.8.1 capture-window fix: candidate verification before the deployment above

T-023/D-029: capture(wait_draws=True) retains record-time snapshot work across empty present intervals, returns one interval containing draw commands, and reports how many empty intervals were skipped. It does not merge interval contents/statistics, change normal tracking or enable replacements. Default capture remains unchanged; original timeout stops pending snapshot work. Older layers are explicitly refused for the new option. CLI supports --wait-draws; the MCP wrapper does not yet expose it.

Full build/regression passes in build/next: unit 88, native tracker 144, native recipes 120, variant runner 37, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. Log: build/capture-wait-draws-regression-20261012.txt. Native tests reproduce empty intervals during recording, including upload/dispatch and a later buffer write; GPU tests cover snapshots through both entries/all descriptor modes. Actual-config inject smoke passes (build/capture-wait-actual-config-next.json), with the real game-log file's hash now recorded and unchanged during that smoke. Candidate DLL hash for both entries: 7e311a9f9f358aa50d90c579abfc1384877a8b391aea8ef886d1e87a90ba50d7.

Next: after observing normal game exit, back up dist/, deploy the exact tested package with the same probe/resources/hash/profiling settings, verify it, then ask for a user-started LOD1 observation. Use --wait-draws --snapshot mod --follow instance --require-section '*_LOD1' --require-color --attempts 8 --pipeline-limit 128. No repeated LOD0 capture or duration test is needed. Game LOD1 bone timing is still unverified; sandbox success does not close it.

## Current 0.8.0 game evidence and corrected LOD labels (2026-10-12)

The user initially entered LOD1, then returned to LOD0 after the agent misinterpreted empty captures. Preserve that correction: display is LOD0; world-rendered-1/2 are the user's LOD0 state, with seven LOD1 sections only in depth passes. They do not establish distant LOD1 color coverage. After explicitly returning to LOD1, lod1-recipes/attempt-01 captures all nine LOD1 sections including color passes. Details and evidence limits: docs/m2-game-recipes.md.

Display: 45 matched draws, 39 complete/supported recipes. LOD0 world: 61 draws, 45 recipes; all 61 entries and both VS bone windows equal at record/submit in two frames, including 14 LOD1 depth draws. Actual LOD1 world: 58 matched draws, 36 complete/supported recipes, but no buffer snapshots. All matched exports exist; unrelated omitted world pipelines are explicit. Current-session VS rewrite/spirv-val checks pass 21/21/19 modules for those scenes.

T-023: 0.8.0 ends capture at the next present even if that interval contains no scene draws. A 40-status LOD1 sample has 30 empty intervals, while total draws increase; no-snapshot captures obtain graphics, but 36 full-snapshot attempts yield none. The exact present producer is unproven. The bounded collector preserves attempts and distinguishes depth-only matches from color-subpass coverage (109 sandbox evidence checks at that checkpoint). The opt-in wait is now implemented and verified as the 0.8.1 candidate above. No game restart or DLL replacement has occurred.

Correction: the 0.8.0 temporary actual-config smoke's historical-log check used the wrong filename and compared missing-file values. It did not prove the log unchanged; the recipe/settings/hash checks remain valid. The script is corrected for future runs. See docs/m2-game-recipes.md.

## M2 owned pipeline variants and restoration: sandbox verified, 0.8.0 deployed (2026-10-12, 16:34)

Continued independent development after restoring memory and deploying 0.7.5. Added full owned classic RenderPass 1/2 and graphics pipeline recipes, C++ VS-only buffer re-pointing, a private pipeline/layout builder and a direct indexed-draw helper that restores pipeline/vertex/index state. The sandbox now proves replacement geometry with RGBA16_UINT bone index 257, uint32 indices and nonzero instance offsets while FS reads the original buffers; the next original draw works without application rebinding. Core/sync validation is clean in both render-pass versions through no layer/layer/inject. Private creation objects are rebuilt from owned metadata on a worker after the source shader/render-pass handles die. Details and limits: docs/m2-pipeline-variants.md, D-028, T-022.

The layer's draw hooks still only observe. No variant request queue/cache, in-flight replacement resources, sample asset/bone import, texture patching or real-character rendering exists. New pipeline exports include recipe, layout_recipe and CPU-only variant_preflight; complete/preflight is not a driver build result. Unknown extensions/flags/layouts fail explicitly. The initial variant uses static vertex input and requires no new game device feature.

Full build and regression in build/next passes: unit 88, native tracker 126, recipe native 120, variant runner 37 (9 headless GPU processes), 17 probe cases 514-529, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 78. Log: build/m2-recipes-regression-20261012.txt. All 21 historical character VS rewrites match the Python reference exactly and pass spirv-val (build/variant-shaders-l0syviji/report.json). These are sandbox and historical offline results, not this round's game evidence.

Game absence checked twice before replacing dist/ with the exact tested package. Backup: build/backups/before-0.8.0-20261012-163359/. Both DLLs SHA-256 6e95b328e5ef019ad3e6d025cad966f2e3b326b97cd34d2b0270155610d73f3d. Actual-config inject smoke passes on candidate and dist/; full pipeline recipe, five correct hashes before/after rewrite, historical game log unchanged (build/m2-actual-config-next.json, build/m2-actual-config-dist.json). dist/efmi_vk.cfg: probe, resources=1, image_hash=0, profile_tracker=0; unmap defaults and WC fix retained. No game/launcher launch or launcher edit.

Next: user starts Vulkan with the sample character visible, preferably display then world. Confirm loaded 0.8.0/settings, collect with --pipeline-limit 128 --snapshot mod --follow instance, inspect every matched pipeline's recipe/preflight and LOD1 bone/instance evidence. Historical captures have no full depth/blend/render-pass recipes and cannot answer this. Stop here for that observation; no duration-only or 30-minute run is needed. M2 is not complete.

Reviewed on 2026-10-12, 16:47 (second agent, at the user's request): independent rebuild and full suite pass with the same counts, the 21 shader rewrites verify again, the deployed hashes and the launcher's Vulkan values are as stated, and reading the recipe, variant and re-pointing code found no defect. Only the capture at creation time and the CPU-only preflight run in a game process. Note for the integration in T-022: most character pipelines declare a fifth vertex binding that the tracker does not keep.

## Deployment of the verified 0.7.5 package (2026-10-12, 15:52)

The user requested continuation after restoring memory. Endfield was no longer running. Backed up dist/ to build/backups/before-0.7.5-20261012-155227/ and copied the tested build/next binaries without rebuilding. Both DLLs have SHA-256 f331ff7c3a390affbc7adfe617a1e1364f9d5441821b86f2ecdabf4ff39c7475. The cfg comment now states the restored full transfer-source default; settings remain probe, resources/hash off and profiling on.

Full suite on dist/ with --no-build passes: unit 88, native 126, 17 probe configurations 504-519, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 78 (build/deployment-0.7.5-regression.txt). Actual-config inject smoke passes, including unmap_snapshot_all=true and five hashes before/after rewrite (build/tests/sandbox-2c8cp1sr/). The game log is unchanged. No game/launcher launch or launcher change. M2 development can continue offline; the 0.7.5 deployment itself does not require another duration-only game test.

## Game session with 0.7.4: stutter gone, the review's attribution refuted, first open-world frames (2026-10-12, from 15:28; started by the user)

Loads in the real game and data captured in the real game. PID 27188 under Vulkan; the log confirms 0.7.4 probe/inject at 15:28:07 with resources and image hashing off, tracker timings on, and the copy at unmap restricted to staging buffers. The user reports that the stutter seems gone and then ran the open-world route. Full account: `docs/unmap-snapshots.md`; evidence in `captures/pacing-0.7.4-20261012/` and `captures/open-world-0.7.4/` (not committed).

- Between two status reads 87,005 present intervals apart (about 12 minutes, the run included): 24 over 16 ms, 3 over 33 ms, 2 over 50 ms. The 0.7.2 foreground sample had 203 / 51 / 17 in 5,093. Last 2,048 intervals at the second read: p50 8.4 ms, p99 10.6 ms, max 14.5 ms. The two are not the same conditions (the 0.7.4 span includes background time; two attempts to catch a foreground window before the run failed), so no foreground-only figure for 0.7.4 exists.
- Copy at unmap in that span: 52,639 buffers, 27.9 GB, 7.7 s, that is 3.6 GB/s against 0.33 GB/s over the 0.7.2 session. Same volume, eleven times faster: the streaming loads of 0.7.3.
- `passed_over` is 0 of 144,103: the staging-only rule of 0.7.4 skipped nothing. The mesh pools are mapped the whole time. The section below, written before this session, attributed the copies to the pools; that was wrong. It read the `generation` counter as a count of copies at unmap although tracked buffer copies increment it too, and it drew conclusions from an inventory of 251 live buffers about a session that had created 280,000.
- Open world, three captures: about 1,190 draws in 35 render passes, one recording thread, two command buffers, four submits; 428 instanced draws with up to 675 instances; one indexed indirect draw; hash source `mapped` for every direct indexed draw (1,154 to 1,185), none without a hash; 19 of the mod's 21 sections match, the 7 LOD1 ones in render pass 5 with two draws each. A capture takes 208 ms round trip.

Source 0.7.5 restores the default to every whole transfer source; stale-shadow invalidation, counts.unmap, set and pinned test settings stay (D-027). It passed the full suite in build/next and was later deployed after game exit, as recorded above. The 0.7.4 measurements show the two unmap rules behaved identically in that session; 0.7.5 itself has not been loaded into the game.

T-020 is closed on this evidence; T-021 is withdrawn. Open: nothing about the stutter. The copies still move about 28 GB in twelve minutes that nothing reads in this game; removing them is not attempted.

## Review of 0.7.1 to 0.7.3 and layer 0.7.4 (2026-10-12; its attribution to the mesh pools was WRONG, see the section above)

Asked by the user to review the three layer versions made for the stutter and to say whether it is solved. It is not known to be solved: 0.7.3 never ran in the game, and the review found that it does not reach the main cost.

- The code of 0.7.1 (present intervals), 0.7.2 (copy savings, tracker timings) and 0.7.3 (streaming loads for write-combined mappings) was read in full and is correct; nothing in it was changed. The diagnosis that copies at unmap under the exclusive resource lock overlap the largest intervals stands.
- New from the same evidence (`captures/pacing-tracker-work-20261012/buffers.json`): 22 pool buffers of 16 MiB (some 8 MiB) with usage index + vertex + storage + transfer, on memory type 4, carry 2,612 generations, 1,491 / 573 / 511 on three of them; the 76 staging buffers on memory type 2 carry 87 generations and 7 MiB in all. The old rule copied every buffer with the transfer-source bit up to 16 MiB, so the gigabytes were whole mesh pools copied each time the game unmapped them while streaming. The display scene does not stream, which is why the first three sessions were smooth.
- 0.7.3's own measurement: type 2 improves from 24.0 to 1.3 ms per 8 MiB, type 4 from 226 to 151 ms. The pools are type 4. Whether its helper is faster on the game's pool pages is unknown; it cannot be relied on.
- 0.7.4: at unmap only transfer-only buffers inside the mapped range and within the limit are copied; any other buffer the mapping touches loses its shadow knowledge (it may have been written unseen; this also closes a case where a stale shadow could be served). `unmap_snapshot_all` restores the old rule; `set` changes both unmap settings in a running process; `counts.unmap` reports copied, passed over and dropped. Description: `docs/unmap-snapshots.md`; D-026.
- Given up: a hash for a draw whose pool is unmapped at the moment of hashing (it is now `null` with source `none`). All 578 hashes of the three game bundles came from the mapping, but in the display scene. The open world is unmeasured (T-021).

Evidence, sandbox only: the full suite against `build/next` (unit 88, native tracker 126, 17 probe configurations of 504 to 519 checks, modes 40, limits 172, timing 126, validation 76, MCP 47, evidence 78), the same suite against the copied files in `dist/` without rebuilding, and a smoke run in `dist/` with the settings file as the game reads it (0.7.4, probe, inject, resources off, profiling on, staging-only copies, five correct hashes before and after the rewrite, `set` works; `build/tests/sandbox-ep3rst7t/`). `dist/` holds 0.7.4, both DLLs SHA-256 `d51f072b9af7eeda527536d4b6a6b1aabda5004cef1d06df9595508a15350535`; the files it replaced are in `build/backups/before-0.7.4-20261012/`. `dist/efmi_vk.cfg` keeps the diagnostic settings of the last sessions (probe, resources and image hash off, profile_tracker on), with the unmap settings at their new defaults. No game process has loaded 0.7.4; the game log is unchanged.

Next requires the user: the same open-world route under Vulkan. Order of work in that session is in `docs/unmap-snapshots.md`: counters, tracker timings, present intervals against the 0.7.2 sample, a few captures for the hash sources, and only with the user's agreement a short switch back to the old rule.

## Layer 0.7.3: mapped-read fix after measured 0.7.2 stalls (confirmed in the later 0.7.4 session)

User-started PID 41044 (14:22:01) loaded 0.7.2 probe/inject with resources/hash off and profiling on; the user reports renewed stutter. Initial background data is not a valid pacing comparison. After explicit readiness, the 14:44:14.810-14:44:38.840 foreground sample has 240/240 active polls, 5,093 intervals, 203/51/17 over 16/33/50 ms and a retained 129.125 ms peak. No captures or image-hash work. Unmap tracking copies 3.46 GiB and accumulates 11.625 seconds of wall time; the peak overlaps three 43-44 ms unmaps, with a zero-byte copy-record call waiting about 44 ms. res_mx is exclusive during the snapshot and shared during copy recording. This strongly identifies a contributor, not every spike's cause. Histories roll over; missing overlap is not exculpatory. Evidence: captures/pacing-tracker-work-20261012/, 21 fingerprints verified; report: docs/pacing-tracker-session.md.

0.7.3 checks the mapped range's OS page policy once at map time. On SSE4.1 CPUs, PAGE_WRITECOMBINE reads use bounded streaming loads with fences; cached/unknown ranges retain memmove. Unmap snapshots, record fallback, submit priority and buffer visibility are preserved. Added status.mapped_read and buffers[].mapped_write_combined. In a separate headless Vulkan process on this GPU, an 8 MiB type-2 upload copy falls from 24.023 to 1.270 ms; cached type 3 is unchanged, type 4 remains expensive (226.258 to 150.952 ms). All bytes match. These are local copy timings, not a game speedup. D-025 / BUG-004.

Full build/regression passes: unit 88, native 118, 17 probe configurations 503-518, modes 40, limits 148, timing 126, validation 76, MCP 47, evidence 78. First build caught a JSON writer method typo; corrected before the successful suite. Log: build/mapped-read-regression-final-20261012.txt. Native tests cover WC page boundaries/alignment, cached fallback, map offsets/remap, submit priority and complete unmap snapshots.

Observed game exit and checked again before backup/replacement. Deployed the exact candidate from build/candidate-0.7.3-20261012/; prior files are in build/backups/before-0.7.3-20261012-150100/. Both DLL SHA-256: 4cb8fdc1c111530cdbc8d76c9c2924c1cf61ec6fa701ba64a18edc5b36548c85. Actual-config inject smoke passes on candidate and dist/ (build/tests/sandbox-wcxt2ysn/, sandbox-zg_0n_36/), five correct hashes and WC mappings reported. Game log unchanged; no launcher edit or automatic game launch/stop. Diagnostic settings stay probe, resources/hash off, profile on; 16 MiB snapshots and shadow_usage 0x40 retained.

Next requires the user: start through the same Vulkan launcher setup and move along the same open-world route. Confirm loaded 0.7.3 before status-only sampling. A short clean foreground comparison is sufficient; no repeated 30-minute run. T-020 / ISSUE-005 is not closed, and 0.7.3 has no real-game result yet.

## Layer 0.7.2: earlier buffer-copy savings and optional tracker timings

Two confirmed unnecessary costs are removed without changing readable fallback behavior: record-time copies first check source visibility before allocating byte storage; submit writes borrow a readable source under res_mx and copy straight into the destination shadow instead of using a temporary vector. Submit-time contents still take priority; unmap snapshots and record-time fallback remain. An unreadable 64 MiB native-test source now allocates no fallback/destination byte storage. This is not yet a game stutter fix.

Optional profile_tracker = 1 adds timings for buffer unmap, buffer copy recording, buffer submit, submit assembly, command reset and draw/dispatch recording. Default off; clocks/counters can perturb timing when on. A bounded 64-event history retains operations of at least 2 ms, with timestamps on the same QPC clock as present peaks' new host_end_us. Excludes forwarded driver calls, includes tracker lock wait/descheduling; nested/concurrent totals must not be added. Details: docs/tracker-work.md, D-024.

Built into build/next and ran the full suite. The first run failed only a new timing assertion because the ordinary sandbox never unmaps its staging buffer; exercising its existing rewrite path fixed the scenario. Final full run passes: unit 88, native 101, 17 probe configurations 503-518 each, modes 40, limits 148, timing 126, validation 76, MCP 47, evidence 78. Logs: build/tracker-work-regression-20261012.txt and build/tracker-work-regression-final-20261012.txt.

The game had exited by deployment; checked twice before replacing files. Backed up dist/ to build/backups/before-0.7.2-20261012-132253/, then copied the exact candidate package. DLL SHA-256 for both entries: f8b30ce440856995f0c7776b1adf4196d541e651b4cd4705f3f9fde6e394fde6. Candidate actual-config smoke (build/tests/sandbox-bbni7c7u/) and deployed actual-config smoke (build/tests/sandbox-_5s9b_x_/) both confirm 0.7.2 probe/inject, resources/hash off, profiling on, five correct draws and unmap measurements. No settings environment overrides; historical game log unchanged. Unmap snapshot limit 16 MiB and shadow_usage 0x40 remain enabled. No launcher change or automatic game launch.

The requested 0.7.2 comparison has now completed (section above): stutter persists and large unmap snapshots overlap the largest peaks. Its deployment/configuration details here are historical. Passthrough remains the observed smooth fallback, requiring a process restart.

## Passthrough comparison: user reports smooth movement (2026-10-12, 13:04 launch)

User-started PID 14804 began at 13:04:17; the log confirms 0.7.1 passthrough/inject at 13:04:19.597, resources and hashing off, Vulkan device created, seven exports redirected. No probe pipe exists. The user reports the stutter is gone. Evidence: captures/pacing-passthrough-20261012/; follow-up in docs/pacing-resources-off-session.md. This is a subjective game result, without a host/GPU/display timing trace. It implicates probe-only work as a group; it does not identify one routine or prove all Vulkan workloads smooth.

Offline follow-up is now built and verified as 0.7.2 above. This passthrough result remains the control; the individual source of the probe-mode stalls is still unconfirmed.

## Resources off: spikes persist; passthrough prepared (2026-10-12, 12:57)

The user restarted the game and reports continuing stutter. New PID 33204 started at 12:50:29 and confirms 0.7.1 probe/inject, resources off, hashing off. All tracked images/descriptors/retained shaders, hash counters and capture sequence remain zero. Buffer shadowing, draw records, call counters and frame assembly still run.

The first observation was mostly background; the second had an early focus transition. Kept only its final 30.000261 seconds (12:57:21.745-12:57:51.745), with 300/300 foreground polls and all seven endpoints foreground: 6,766 intervals, 276/74/26 over 16/33/50 ms, largest retained 105.000 ms at frame 53703. Final window p50 2.887 ms, p99 36.888 ms. Host timings are not GPU/display times; differing world workloads prevent attributing the larger counts to the settings change. Evidence: captures/pacing-resources-off-20261012/; report: docs/pacing-resources-off-session.md. T-020 / ISSUE-005 stays open.

After measurement, backed up the cfg to build/backups/before-passthrough-20261012-125909/ and changed only mode to passthrough. PID 33204 still uses probe until it exits; nothing was hot-switched or restarted. Actual-config inject smoke passes with no pipe, 546 frames and normal exit (build/tests/sandbox-m_8zuprp/). DLL unchanged at 729d914e1f4a1ac66ae26ee2d421707d898b7426e52c756a5997cec50dfe0b7e. Next: a short user-started same-world movement comparison in passthrough. It removes remaining draw/buffer tracking and probe service, but keeps injection/setup/present accounting; it is not a no-DLL baseline. No interval data can be queried without the pipe.

## Hash-off comparison: open-world spikes confirmed (2026-10-12, 12:38-12:39)

User-started PID 31124 loaded 0.7.1 in probe/inject mode; resources on, image hashing off. Initial display readings were taken while the game was background, and the user said its mostly static character cannot reveal the problem. Switched to user-confirmed open-world movement: nine status reads over 39.992 seconds, 400/400 foreground polls, no captures, 11,681 host present intervals. Counts over 16/33/50 ms: 142/27/12. Largest interval 111.204 ms at frame 69089; final window p50 2.989 ms and p99 24.590 ms. Hash work and capture count remain zero. Evidence: `captures/pacing-hash-off-20261012/`; account: `docs/pacing-hash-off-session.md`.

Spikes remain with hashing off; resource/descriptor tracking has not been isolated from game/driver/presentation work. A supplementary 20-second start/end-only observation has a focus confound (78/200 foreground polls) and is not a clean comparison. No layer/config/launcher change or game launch/stop occurred during measurement. No stutter fix is claimed.

After preserving the observations, prepared `dist/efmi_vk.cfg` with resources = 0 and image_hash = 0 for the next user-started process. Previous cfg: `build/backups/before-resources-off-20261012-124818/`. The running PID 31124 retains resources on; no hot switch or restart occurred. Actual-config inject sandbox passes with resources/images/descriptors off, draw capture and pacing available (`build/tests/sandbox-a_60l8x8/`). Next: compare the same foreground open-world movement after restart; the resource-off configuration is not yet game-verified.

## Third probe: texture identities resolved; stutter remains open (2026-10-12)

User-started Vulkan PID 19264 loaded 0.7.0 at 12:05. Captured frame 22563 in `captures/third-probe/`: all 12 LOD0 components, 45 matched draws, all 114 pipelines exported, no query or descriptor gaps. Verified every one of the manifest's 172 file fingerprints. Pairing with the saved DX11 baseline gives 19 placed and 37 undeclared mod slot assignments, none open; all four former ambiguities resolve by content. Metadata profile: `docs/arcane-texture-slots.json`; full evidence and pipeline creation requirements: `docs/third-probe-session.md`.

The user describes intermittent drops during loading and steady display. Upload processing accumulated about 8.1 seconds for 2.55 GiB; a later 3.000533-second sample advances 360 presents with only 596 microseconds added to that timer. This establishes costly loading work, not the cause of every spike. 0.7.0 lacks per-present timing. T-020 remains open; do not claim a game-tested performance fix.

After collection, set `dist/efmi_vk.cfg` to `image_hash = 0` for the next process (old cfg: `build/backups/before-image-hash-off-20261012-121047/`). The user then confirmed normal game exit. Confirmed no target process remained, backed up the old package/log/settings to `build/backups/before-0.7.1-20261012-122110/`, and copied the exact tested 0.7.1 package to dist/. DLL SHA-256: `729d914e1f4a1ac66ae26ee2d421707d898b7426e52c756a5997cec50dfe0b7e`. Launcher settings were not touched.

0.7.1 adds a fixed 2048-entry host present-interval ring, recent p50/p95/p99 and peaks, lifetime threshold counts, and hash/capture tags (`docs/frame-pacing.md`). No per-frame allocation or resource lock; sorting occurs on the probe thread outside the frame lock. It measures host boundaries, not GPU/display latency. Full build/regression against build/next passes: unit 88, native 85, all 17 probe configurations (503-518), modes 40, limits 148, timing 94, validation 76, MCP 47, evidence 78 (`build/pacing-regression-20261012.txt`). A separate dist/ inject smoke test reads the actual cfg without mode/hash/resource environment overrides: 0.7.1 probe, resources on, image hashing off, five captured draws and host timing available (`build/tests/sandbox-un722puo/`). Game log unchanged.

The planned hash-off comparison and subsequent resource-off comparison have since completed (sections above); both retain open-world spikes. No repeated 30-minute run. M2 now has the sample texture table and pipeline requirements, but no replacement rendering has been implemented.

## DX11 baseline of the sample mod (2026-10-12, 10:09; game under DX11, started by the user)

Data captured in the real game under DX11, PID 9264 (`Forcing GfxDevice: Direct3D 11` in `Player.log`), with XXMI's `d3d11.dll`, the DX11 probe 0.1.6 of the sibling project and this project's DLL in passthrough (it redirected the exports of `vulkan-1.dll` and saw one instance of another component; nothing else). One full frame, 30344, with constant buffer contents: `captures/dx11-baseline/frame-a.*` (not committed). Taken and summarised with the new `scripts/dx11_baseline.py`, which uses the sibling project's client as a library without writing there, and paired with `captures/first-probe` by `server/efmi_vk/dx11_pair.py`. Full account: `docs/dx11-baseline.md`. A second frame of the same session (`frame-b`, 150191), taken with the script's own `capture` command, has the same XXMI figures (113 calls, 12 and 45 compute runs).

- 346 calls, 113 by XXMI: 56 indexed draws of six mod sections and 57 dispatches of the core's two compute shaders. 18 of the 21 objects both frames contain pair draw for draw (41 draws); component 0 is drawn 7 times under both APIs.
- Instance entry: 16 constants; constant 3 the translation (-300, 300, -300), constant 5 (bone offset, previous bone offset, 258, 1.0f). The game's values (504633, 502851) become (1533, 3030) in the 256-byte copy the mod draw uses. Same structure as read under Vulkan. The second compute step ran 45 times; the Vulkan frame has 45 draws of the 12 original components.
- Under Vulkan the instance window is set 2 binding 0 in 38 of 38 paired draws; under DX11 it is `vs-cb1`, `vs-cb2`, `vs-cb3` or `vs-cb6` depending on the shader. The bone buffer (8,413,184 bytes on both sides) is a storage buffer of set 0 at binding 7, 9, 18 or 19.
- Texture slots: no common order, no names on either side, and the table belongs to the Vulkan fragment shader: one DX11 pixel shader is drawn with two Vulkan fragment shaders that place the same slot at different bindings. Component 1 main pass: `ps-t15` diffuse is set 1 binding 6, `ps-t16` light map binding 2, `ps-t17` normal map binding 5, `ps-t18` binding 3.
- Vertex input: identical except the blend indices (mod RGBA16 uint in a stride of 16, game RGBA8 uint in a stride of 12); the mod's indices are 32-bit.
- Under DX11 only the vertex stage gets the merged skeleton and the patched entry; the pixel shader keeps the game's and reads them. One Vulkan binding serves both stages (T-019).

Mods off and on again (10:29, after the user restored the game window): the DX11 probe pressed EFMI's F11 once, `frame-off` (170064) was saved with EFMI's `Mods Display: Disabled` in it, a second press gave `frame-on` (170102) with `Mods Display: Enabled`. The mods are on as before. No other key was sent.

- With the mods off the game draws the 12 components 7, 4, 4, 5, 4, 2, 3, 2, 2, 4, 4, 4 times; the Vulkan frame has the same numbers. All 45 draws pair; the instance window is set 2 binding 0 in 56 of 56 paired draws.
- The pairing now uses only the slots a shader declares (the declarations are read from the running game and saved as `<label>.shaders.json`), since DX11 leaves earlier bindings in place.
- Slot table over the four frames: 111 entries for 36 Vulkan fragment shaders, no contradiction. Of the slots the mod's draw sections set, 15 are placed, 4 are one of two (T-018), 37 are not declared by the shader of that pass.
- The mod assigns its slots in every pass. For component 0 this puts its diffuse map over a 1x1 volume texture in one pass and its normal map into the diffuse slot of render pass 24; that is how it looks under DX11, so the translation has to be by slot and pass (fact 20).

A third pair of F11 presses at 10:40 gave `frame-off-tex` (249580, mods off; the mods were confirmed on again afterwards) with content hashes of mip 0 of the 65 file-backed textures of the scene (`--textures`; DX11 half of T-018). In that frame every component is drawn once more than at 10:29, so it pairs with nothing of the mod and is used for the hashes only: pairing by draw count needs the same state of the scene.

Scope: one scene, one character, the mod's own key states as found, two sessions (window sizes differ, so screen-sized textures do not pair).

Checks of this round: `sandbox/unit_test.py` 75 of 75 (31 for the pairing), `scripts/check_text.py` clean. The layer was not changed and not rebuilt; the full suite was not run.

## Texture content hashes and pipeline creation inventory: layer 0.7.0 in `dist/` (2026-10-12; sandbox only)

T-018 is built. The layer hashes mip 0, layer 0 of an image when a buffer copy covers the whole level, at submit, rows without padding, and reports state, CRC-32C, size and upload count with every sampled image of a descriptor; the status counts uploads per outcome, bytes and time. `scripts/dx11_baseline.py summarize` pairs textures by content first (`--textures` names the DX11 hashes) and lists where each slot the mod sets lands (`mod_slots`: placed, open, no counterpart, undeclared). With the existing Vulkan bundles, which carry no contents yet, the result is unchanged: 15 placed, 4 open, 37 undeclared.

For M2 the same build counts what the game's graphics pipeline creation uses (`api_usage.pipeline_creation`) and reports per pipeline its flags, subpass, dynamic states, specialization, set layout ids and push constant ranges. Description: `docs/probe-resources.md`, section 0.7.0; data path: ARCHITECTURE.

The sandbox uploads its texture three times (a 2x2 part, the whole 4x4 level packed, the whole level with padded rows); the value reported is the CRC of the last one as the sandbox computes it itself.

Evidence, sandbox only: the full `sandbox/run_all.py` against `build/next` had two failures outside the layer (an expectation of `mcp_test.py` that still said 1x1, and a start-up race of the harness, ISSUE-004); both tests passed there after the corrections, and the full suite then passed against `dist/` after the build there: unit 88, native tracker 78, 17 probe configurations (503 to 518 checks each), modes 40, limits 148, timing 84, validation 76, MCP 47, evidence 78; all passed. `dist/efmi_vk_layer.dll` and `dist/efmi_vk_inject.dll` are 0.7.0, SHA-256 `8c5657ea2f81ba9ed89714448577fb7f1dcb6824360fbc57dd8dff45fe9cb1ca`; `dist/efmi_vk.cfg` is `mode = probe`. No game process has loaded 0.7.0.

What a game session has to show: that content hashes of the DX11 side and of the Vulkan side meet at all (the DX11 values are of mip 0 as read back; the Vulkan values of mip 0 as uploaded), what the hashing costs while a scene loads (22,150 image uploads were counted in the second session), and what pipeline creation uses. The command sequence is in `docs/first-probe-session.md`, third session.

## Vertex-stage bindings for the pipeline variant decided (2026-10-12; offline, from game data)

T-019 is decided as D-022. `scripts/stage_usage.py` on `captures/second-probe-all` and on `captures/first-probe` gives the same result: 45 draws of the 12 components, 26 programs, 21 vertex shaders; every vertex shader has exactly one storage buffer and it is the bone buffer; all read the instance window at set 2 binding 0; 20 draws have a fragment shader that reads the bone buffer, 29 one that reads the window; three sets are bound. The variant therefore re-points the vertex module's two variables to bindings 0 and 1 of set 3 and leaves the fragment module alone. `server/efmi_vk/spirv.py` has the reference implementation (`buffer_variables`, `repoint`); applied to the 21 real vertex shaders it changes four words each and `spirv-val` accepts every result. Offline evidence only: no pipeline was created from a rewritten module. Unit tests 84 of 84, text check clean; the layer was not changed.

## Second probe session in the real game: G3 b and c (2026-10-12, 11:10 to at least 11:35; started by the user)

Loads in the real game and data captured in the real game; nothing drawn, nothing verified on a character. PID 36420, Vulkan (no forced DX11 in `Player.log`), layer 0.6.0 by the launcher's injection (`efmi_vk_layer 0.6.0 loaded into Endfield.exe, mode=probe, entry=inject`), XXMI's `d3d11.dll` not injected. 120 frames per second, working set 11.8 GB and flat, `shaders_not_retained` 0, `unknown_writes` 0. Bundles `captures/second-probe` (45 draws of the 12 components with contents) and `captures/second-probe-all` (all 210 indexed draws with contents, all 117 pipelines); about 1,100 further captures for series. Full account: last section of `docs/probe-snapshots.md`.

- G3 b, instance entry: equal at record and at submit in 3,882 of 3,882 comparisons; same structure as under DX11 (constant 3 translation, constant 5 the two bone offsets, 258, 1.0f).
- G3 c, bones: final at submit (240 of 240 windows equal a read 1 to 3 ms later); a component's own palette is complete when its first draw is recorded (component 4: 55 bones equal in 492 of 492 windows), the bytes behind a palette are not (one bone more differs in 21 of 60 frames, always in the shadow pass, always the current offset).
- The bone count per component is in the mod (`VertexGroupCounts`: 45, 86, 42, 15, 54, 16, 8, 12, 80, 117, 23, 1; 499 in all) and equals what the game's meshes use, 12 of 12.
- The palettes are adjacent in a ring buffer, about 1782 elements per frame, in an order that changes between frames.
- All other uniform ranges of sets 0 and 2: 29,967 comparisons, none different. Selection by hash at record time never failed.
- The DX11 pairing was repeated against this session's frame with every pipeline: 36 fragment shaders by SPIR-V hash, 118 entries, none contradictory, same result for the mod's slots.

D-021 follows: D3 holds for this scene without a GPU read-back. Open: other scenes (open world, combat, several instances, LOD1), T-018.

## Contents at submit and the record-time comparison: layer 0.6.0 in `dist/` (2026-10-12; sandbox, then the game)

T-017 is implemented. A capture can be asked to keep, for the draws whose region hash is listed (or for all indexed draws), every uniform range of their sets as it is at `vkQueueSubmit`, the first bytes of one chosen range as the draw's own entry, and windows of the draw's storage ranges located by 32-bit values read from that entry. Each item also carries a comparison with a read taken inside `vkCmdDrawIndexed`: `equal`, `different`, `other_range`, `unreadable` or `not_recorded`. Kept bytes are bounded by a budget (32 MiB by default) and fetched with the new `snapshot` command. Description: `docs/probe-snapshots.md`; data path: ARCHITECTURE.

For the game the rule is set 2 binding 0, values at byte 80, 16 bytes per element (`scripts/collect_probe.py --snapshot mod --follow instance`): the instance entry, the two bone offsets of its constant 5 and 12,288 bytes of the bone buffer at each. The command for the session is in `docs/first-probe-session.md`.

The sandbox now gives each uniform slot 96 bytes with a frame number written before recording, a second one written for odd slots after recording and before submit, and two element numbers into its storage ranges, so that both comparison outcomes occur in every frame.

Evidence, sandbox only: full `sandbox/run_all.py` against `build/next` and again against `dist/` after the build there: unit 75, native tracker 78, 17 probe configurations (485 to 500 checks each, with secondary command buffers and all four descriptor modes), modes 40, limits 132, timing 84, validation 76, MCP 46, evidence 78; all passed. `dist/efmi_vk_layer.dll` and `dist/efmi_vk_inject.dll` are 0.6.0, SHA-256 `dfbe6fb440d261b99ff649acfe92e822188fd4d41c9f3d108ef49197437dd5fa`; `dist/efmi_vk.cfg` is `mode = probe`. The game loaded it at 11:10 the same day (section above).

Not covered: non-indexed and indirect draws, texel buffers, push constants; bytes written between submit and execution; what a compute pass produces. The MCP tools do not expose the snapshot yet (the client and the collector do).

## Machine back in its Vulkan configuration (2026-10-12 10:43; game and launcher closed by the user)

After the DX11 session the user closed the game and the launcher and had this project switch back: `d3d11_mode_cmd_args` from `-force-d3d11` to empty and `xxmi_dll_inject_mode` from `DIRECT` to `SKIP`, two lines, copy of the file before the change in `build/backups/launcher-config-before-vulkan-20261012-104327.json`; the signed library list was not touched. `dist/efmi_vk.cfg` is `mode = probe` again. `dist/` held layer 0.5.1 at that point; 0.6.0 replaced it later the same day (section above). The next launch through the launcher is a Vulkan session with the probe, which stays the user's decision.

## Machine set up for the DX11 baseline (2026-10-12 10:01; historical, reverted at 10:43)

At the user's request this project switched the launcher's EFMI profile back to DX11: `d3d11_mode_cmd_args` from empty to `-force-d3d11` and `xxmi_dll_inject_mode` from `SKIP` to `DIRECT`, two lines, with the launcher and the game not running and a copy of the file saved in `build/backups/`. `dist/efmi_vk.cfg` was `mode = passthrough` for that session, because the launcher injects this DLL under DX11 as well. The launcher log also narrowed BUG-001: XXMI's `d3d11.dll` under Vulkan is the cause, not the DX11 probe.

## First probe session in the real game (2026-10-12, 09:47 to at least 09:55; started by the user)

Launcher configuration of run 3 (Vulkan, XXMI DLL injection mode Skip, only `efmi_vk_inject.dll` injected). `Endfield.exe` PID 8920, layer 0.5.1 in probe mode with full resource tracking. The character of `arcane fes 1.2` was on screen in what looks like a character display scene (objects placed around (-300, 300, -300)). Everything below was read through the probe pipe; nothing was written to the game. Evidence, local and ignored by Git: `captures/first-look/` (status and call counters twice, 5 s apart), `captures/first-probe/` (collector bundle of frame 30630: 84 files, `frame.json` 4.7 MB, 74 shaders, manifest `complete_with_gaps` because 68 pipelines were over the limit of 48), `captures/g3bc/g3bc.json` (two more captures with live buffer reads).

**The probe in the game.** The DLL arrived 4.8 s before `vulkan-1.dll`; 93 of 127 exports redirected; 5 instance creations and 1 device creation seen; no passive call. About 120 frames per second over a 5 s interval with full tracking (possibly the game's cap; no figure without the probe was taken). Process working set 11.5 to 11.6 GB, private 8.4 GB, flat over 7 minutes; the passthrough run of 2026-10-11 had 11.2 GB after 6 minutes. Probe holdings: 22.9 MB of SPIR-V for 1106 shaders (budget 512 MiB, nothing dropped), 2888 live descriptor sets, 6472 images, 22 MB of shadow. No crash, dialog or disconnect was reported by the user in these minutes.

**G2, recording and submission (answered for this scene).** One recording thread. Per frame: 2 command buffers, 4 submits, 28 classic render passes, no secondary command buffers, no dynamic rendering; 238 to 244 draws (215 to 221 indexed, 23 non-indexed, none indirect) and 47 or 48 compute dispatches, all outside render passes. All draws and dispatches of the captured frame are in one command buffer and one submit. Maxima since start (loading included): 3 threads, 8 command buffers, 30 submits. `Player.log` says `jobified=0`. Entry points in use: `vkQueueSubmit`, `vkCmdBeginRenderPass`, `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`; `vkCmdDrawIndexedIndirect` and `vkCmdDispatchIndirect` have been called earlier but not in this scene.

**Descriptor path.** Per frame about 190 `vkAllocateDescriptorSets`, 188 `vkUpdateDescriptorSetWithTemplate`, 2 `vkResetDescriptorPool`, 281 `vkCmdBindDescriptorSets`: sets are allocated and written through core 1.1 templates every frame and released by pool reset. No ordinary `vkUpdateDescriptorSets` and no push descriptors in the interval, although `VK_KHR_push_descriptor` is enabled. All sets of the captured frame were tracked; 1.7 MB of descriptor contents, nothing omitted.

**G3 a, index hashes (passes).** `arcane.ini` has 21 overrides with a hash. All 12 LOD0 entry points (components 0 to 11) match draws of the frame by region hash, first index and index count; the 9 that do not match are the LOD1 entries, which are not drawn with the character close. Every one of the 215 indexed draws was hashed from the application's own mapping (`src: mapped`); no shadow was needed and `unknown_writes` is 0. Geometry lives in 16 MiB pool buffers (usage vertex, index, storage, transfer) in host-visible memory that stays mapped; 5 of them serve the frame, all with 16-bit indices and non-zero bind offsets, as under DX11. Each component is drawn in up to four passes (per pass: 10, 11, 17 and 7 character draws in render passes 5, 8, 21 and 24).

**G3 b, instance config (strongly indicated).** Every character draw binds, as set 2 binding 0, a dynamic uniform buffer of exactly 65536 bytes (4096 constants, as EFMI checks under DX11) in a mapped scratch buffer that alternates between two buffers from frame to frame. A live read right after a capture gave, for 8 of 12 components, floats 12 to 14 = (-300, 300, -300) and entry 5 = (bone offset, second bone offset, 258, 1.0f).

**G3 c, bones (strongly indicated).** The vertex shaders of the character use a storage buffer of 8413184 bytes (buffer 37, mapped) besides the instance config. At `entry5.x * 16` and `entry5.y * 16` bytes it holds rows of four floats that form 3x4 matrices: an orthonormal 3x3 part and a translation of about (-300, 301.4, -300), the same for both offsets up to the fourth decimal. This is the layout EFMI's bone importer reads (`src_offsets.x + 3 + bone * 3`), with x and y as two skeletons (current and previous frame).

Why b and c are not "passed": the reads were taken after the frame, from buffers the engine rewrites every frame. In the first capture 4 of 12 components already read as zeros or other data, in the second capture 11 of 12. The values that were read are consistent with each other, but only a snapshot taken by the layer at submit can show the bytes a draw really used, and nothing yet measures whether they are in place when the draw is recorded. Indirect support: all per-frame data reaches the GPU through mapped memory (no buffer copies and no unknown writes in a steady frame).

**G3 d, textures (open, with one decision forced).** In the main colour pass the material textures of the character are in set 1 (for component 1: six textures, BC7, BC7 sRGB and BC5 at up to 2048 x 2048 plus two small RGBA8 lookups); set 0 holds 31 pass-level textures. The depth-only pass has none in set 1, the pass at render pass 8 has two. None of the 74 exported shaders contains a single `OpName`: the game's SPIR-V is stripped, so mapping by resource name (first choice of plan decision D7) is not possible. The mapping to DX11 registers t12 to t18 needs the modded DX11 baseline (T-008).

**G3 e, vertex streams (as expected, to be compared with DX11).** Four bindings with strides 16, 12, 12, 16 in one pool buffer, binding 3 at the same offset as binding 0. Attributes: position R32G32B32 float plus one float in binding 0; R32G32 float and R8G8B8A8 snorm in binding 1; R16G16B16A16 unorm (weights) and R8G8B8A8 uint (indices) in binding 2. The mod's blend stream has stride 16 with 16-bit indices, so a pipeline variant with other vertex input is needed, as plan decision D8 assumes.

**G3 f, draw shape (passes for this scene).** Direct `vkCmdDrawIndexed` with instance count 1 and first instance 0 for every character draw.

What this session does not establish: long-duration stability; behaviour in the open world or in combat, with several instances, LOD1, or indirect draws; any overhead figure; anything about rendering a mod.

## Review before the first probe session; layer 0.5.1 in `dist/` (2026-10-11; sandbox only)

The 0.4.0 / 0.5.0 work of the sections below was reviewed before any of it goes into the game (session log, D-017). The suite passed as recorded and nothing the tracker reports for the sandbox was found wrong. What was changed is how the probe behaves in a process much larger than the sandbox:

- SPIR-V is kept up to `shader_budget` (512 MiB by default), not without bound; hash and size stay for every module.
- Destroying a module or pipeline, and resetting a descriptor pool, no longer scan every retained shader or every descriptor set.
- A capture keeps at most `descriptor_budget` (64 MiB by default) of descriptor contents and counts what it left out.
- `resources = 0` is a tracking level between full probe and passthrough.
- Mesh pipelines, a collector crash on an unknown shader stage and inline modules under reduced tracking were corrected.

Verification: whole suite against `build/next/`: Python 44, native tracker 78, the 17 probe configurations unchanged, modes 40, limits 100 (new, `sandbox/limits_test.py`), timing 84, validation 76, MCP 46, evidence 52. The tested files were copied to `dist/` (DLL SHA-256 `ce676f264ae4345535fda649766aa23f34a5241a6a9f90123d88ae3a4d009ca8`); the 0.5.0 package is in `build/backups/before-0.5.1-20261011-230051/`. A sandbox run against `dist/` with the settings file as it is (inject entry, push templates, two threads) reported 0.5.1 in probe mode and captured five draws with matching hashes; the historical game log is byte-identical before and after.

Not established: anything about 0.5.1 in the game. The cost of full tracking there is unknown (LIMIT-014); `docs/first-probe-session.md` says what to read first and how to fall back. The numbers of the sections below that mention 0.5.0 describe that version.

## First-probe exporter ready; game-data boundary reached (2026-10-11)

`scripts/collect_probe.py` now saves raw frames, API counters/status before and after, INI fingerprints/matches, G2 summaries and bounded retained shader/pipeline exports. Explicit PID/target labels prevent treating a sandbox capture as game evidence; output cannot overwrite an earlier bundle. Missing post-capture objects and pipeline limits are explicit gaps; a manifest with file SHA-256 values is written last. The collector never launches or injects anything.

Sandbox verification: 52 checks through both entries, push templates and secondary buffers, including failed-query retention, evidence hashes, wrong-scope and overwrite refusal. All current M0/M1 offline prerequisites (T-007/T-012/T-015 and capture preparation) are complete. The next backend decision needs real G2/G3 data; see `docs/first-probe-session.md`. Dist remains 0.5.0 probe with the corrected sandbox; real-game loading of this version and real-character capture remain unverified. Historical run 3 remains accepted initial G1 evidence; no repeated 30-minute run is required.

Final regression against `dist/` passes: Python 44, native tracker 62, all 17 probe configurations, modes 40, timing 84, core/synchronization validation 76, MCP 46 and evidence export 52 (`build/final-offline-regression.txt`). Native tests now write only to their own `build/tests/` log. A test startup line appended to the historical game log was identified and preserved separately, and the exact original bytes were restored; the corrected native run passes 62 checks without changing that log (BUG-002). DLL and sandbox hashes match the tested `build/next/` copies.

## Compatibility inventory complete (2026-10-11; static evidence)

T-007 is complete: docs/compatibility-inventory.md plus the metadata-only compatibility-inputs.json pin 46 Core files, 86 mod files, game binary/content indices and the matching XXMI source. The local 1.2.2.0 DLL exactly matches the official v1.2.2 archive; source commit a7847eae82aaac32175c9a7d199673d5c9323bcd. Game engine version is 2021.3.34.0; no marketing/content version label was recovered, so exact selected-file hashes identify the build. The reproducible inventory check matches all saved metadata.

Corrections: arcane.ini configures 13 slots but has entry points for ids 0-11, and the folder also loads k/1.ini, an interactive HLSL image panel. Complete folder compatibility must include it; it was missing from the original inventory. D-016 retains XXMI engine hosting behind an explicit adapter after static dependency analysis; no compiled host prototype is claimed. Remaining backend decisions require real G2/G3 observations, not more duration-only loading tests.

## Vulkan validation (2026-10-11; sandbox only)

T-015 is complete. Built Khronos VVL 1.4.321 under `F:/vulkan`; the regression suite now requires it. Core/synchronization validation passes 24 positive configurations and three negative canaries: 76 checks. Fixed the sandbox's read-only SPIR-V decoration and shader cleanup; added upload barriers and per-image presentation semaphores. Layer DLL stays 0.5.0, unchanged.

Full suite passes (unit 44, native 62, 17 probe configurations, modes 40, timing 84, MCP 46); final targeted validation passes 76/76. The corrected sandbox is in `dist/`. Repeated 36 benchmark runs: at 2000 draws the median probe increment is 0.386 ms primary / 0.744 ms secondary. These supersede the earlier numbers below. Details: `docs/validation.md`, `docs/probe-performance.md`. An initial VVL run wrote a 36-byte default system temp cache; subsequent child caches are confined to project test directories and that file remains unchanged. No registry/launcher/game change. Next: T-007 inventory, then the real G2/G3 data boundary.

## Many-draw performance measurement (2026-10-11; sandbox only)

T-012 is complete. The sandbox accepts `--draws`, records raw host timings with configurable warmup/sample counts, and `sandbox/benchmark.py` compares no layer, passthrough and probe through the inject entry with rotated trial order. The layer binary is unchanged at 0.5.0. Timing output separates FIFO waiting from recording/submitting. At 2000 draws the median probe increment is 0.424 ms (primary) / 0.553 ms (two secondary recording threads); this does not establish game overhead or the M2 5% target. Full method, limitations and fingerprints: `docs/probe-performance.md`; raw evidence: `build/tests/benchmark-owy1jn92/report.json` (36 runs).

The 37-draw timing/capture regression passes 84 checks. The full suite against `build/next/` passes (Python 44, native 62, existing 17 probe configurations, modes 40, timing 84, MCP 46). Test baselines now clear inherited layer activation; failed startup also cleans up its process. The tested sandbox is copied to `dist/`; the DLL, cfg and historical game log are unchanged. Next: T-015 validation and T-007 inventory.

## Templates and push descriptors (2026-10-11; sandbox only)

The user requested continued development without entering the game. T-011 now decodes descriptor update templates and direct/template push descriptors. Template definitions retain their layouts and copy their entries; offsets/strides, arrays, buffer/image/texel descriptors and immutable samplers share the ordinary-write decoder. Core/KHR and maintenance6 entry points feed the tracker. Unknown templates and unsupported layouts still discard stale knowledge.

Push contents are immutable per recording update, retained by each draw and separate across command buffers and graphics/compute bind points. Partial writes retain compatible elements; disturbance/reset discards them. Immutable samplers must be explicitly pushed. Secondary draws retain their push contents; executing secondaries invalidates primary descriptor bindings. JSON distinguishes `snapshot: record` for pushes from `snapshot: submit` for ordinary sets. Resource metadata and buffer visibility remain submit-time observations (D-015).

- **Built/verified**: 0.5.0 under `build/next/`; full `sandbox/run_all.py` passed. Python 44, native tracker 62, modes 40, MCP 46. Final probe tests pass 17 configurations: ordinary layer 416 / 418 / 418, inject 418 / 420; templates layer 420 / 422, inject 422 / 424; direct pushes and push templates each layer 427 / 429, inject 429 / 431. New paths run with 0 and 2 recording threads through both entries.
- **Prepared in `dist/`**: copied the exact tested DLLs, manifest and sandbox/native executables and checked SHA-256 equality. Old 0.4.0 package/settings/log: `build/backups/before-0.5-20261011-214412-992/`. DLL SHA-256: `c9a8b41db3234ecebce64300d0c1dd34de259b6ebd23827516001df4c89a8005`.
- **Configuration smoke test**: `dist/` inject entry, actual cfg with no mode override, push templates and 2 threads; reported 0.5.0 probe, captured five draws, exited 0 (`build/tests/sandbox-f6c4cx5r/`). The historical game log is unchanged. No game process was running before replacement; no game was launched or injected.
- **Limits**: GPU coverage is core 1.1 templates plus KHR push commands on the existing NVIDIA GPU. Newer aliases and compute/texel pushes have tracker coverage only where described in `docs/probe-resources.md`; no wrapper/GPU coverage is claimed for them. Descriptor buffers, specialized descriptor types, dynamic inline pipeline layouts and full reflection remain open. No validation-layer, visual or overhead result was established.

Next offline work: T-012 overhead measurement, T-015 validation layers, T-007 compatibility inventory. Real-game capture remains a separate user-chosen session; it is not required to continue these tasks.

## Prior game evidence accepted; 0.4.0 probe prepared (historical, 2026-10-11)

The user instructed the project to use the previous run's data. Do not require another 30-minute passthrough run before continuing development or preparing a short probe capture. Run 3 remains historical evidence of loading and scene changes; its exact duration and frame total are unknown. Its passthrough log contains no captured draws or descriptor data.

- **Prepared**: rebuilt layer 0.4.0, both entries and the sandbox in `dist/`, with `mode = probe`. No game process was present before replacing the DLL. The old DLLs, manifest, settings and game log are preserved in `build/backups/before-probe-20261011-212452-288/`.
- **Verified this round, sandbox only**: full `sandbox/run_all.py` against `dist/`; Python 44, native tracker 37, layer probe 385 / 387 / 387, inject probe 387 / 389, modes 40, MCP 46. All passed. A separate inject sandbox read the actual `dist/efmi_vk.cfg` without a mode environment override, reported 0.4.0, captured five draws and exited normally (`build/tests/sandbox-xm23wqvx/`).
- **Evidence preserved**: the historical `dist/efmi_vk_layer.log` is byte-for-byte unchanged. Test logs stayed under `build/`. The two DLL entries have identical bytes; SHA-256 `ab54243ae1db4072169e4587686a7df2c71115c7f4303fe2f116363305e07cf0`.
- **Next game step**: when the user chooses to launch a short probe session, use run 3's launcher configuration, collect API usage and a frame for G2/G3 a with the sample character visible. No game was started this round, and 0.4.0 has not yet loaded in the real game. Long-duration stability is a separate follow-up, not a prerequisite.

## Ordinary descriptors, shader modules and images (2026-10-11; sandbox only)

T-011's ordinary descriptor path is implemented. Draws contain submit-time descriptor contents, dynamic offsets separated by set/binding/array element, buffer ids/ranges/visibility, and image/view/sampler identities, formats and dimensions. Writes, copies, immutable samplers, variable counts, layout disturbance, set/pool lifetimes and handle reuse are tracked. Graphics and compute binding state is separate.

Pipelines retain stage entry points and SPIR-V after module destruction. `efvk_shader` reads or exports the bytes and extracts declared names and direct set/binding decorations. This is limited metadata, not full reflection. Bytecode is reclaimed after its last module/pipeline reference disappears.

- **Built**: layer, sandbox and native tracker test in `build/next/` (0.4.0).
- **Verified during implementation**: full `sandbox/run_all.py`, using `D:/Python312/python.exe` and process-local `EFMI_VK_DIST=F:/efmi vulkan/build/next`; Python 44, native tracker 37, probe 385 / 387 / 387 (layer) and 387 / 389 (inject), modes 40, MCP 46. All passed. Test artifacts now stay under `build/tests/`.
- **Not established**: game loading of 0.4.0, game captures, real-character data, G3 record-time CPU readability, validation-layer cleanliness, visual correctness or overhead.
- **Follow-up**: templates and push descriptors were subsequently implemented in 0.5.0 above. Descriptor buffers, swapchain image metadata and full reflection remain limited; see `docs/probe-resources.md` and LIMIT-006.
- **Deployment follow-up**: the initial implementation left `dist/` at 0.2.0 passthrough. D-014 supersedes that hold: 0.4.0 is now prepared in `dist/` with probe mode, as recorded above. A new game run still requires the user's on-the-spot instruction.

## Call counters, initially staged in `build/next/` (2026-10-11; sandbox only)

Layer 0.3.0 counts every intercepted device function and adds about forty functions that are only counted (descriptor paths, image and shader creation, uploads to images, multi-draw and others), the probe command `api_usage` and the tool `efvk_api_usage`. Purpose: the first probe-mode session in the game then shows which descriptor update path, which submit, copy, map and render pass variants and which draw forms the engine really uses, which decides what T-011 has to track and which untested hooks (LIMIT-011) matter.

- Built into `build\next\` because the running game (run 3) holds `dist\efmi_vk_inject.dll`. `sandbox/run_all.py` with `EFMI_VK_DIST=build\next` passes: unit 36; probe 168 / 170 / 170 (layer entry) and 170 / 172 (inject entry); modes 40; MCP 39.
- **Follow-up completed under D-014**: the user accepts historical run 3 as initial loading evidence. The counters are included in 0.4.0, now rebuilt and verified in `dist/` with probe mode configured. No real-game counter data has been collected yet.

## First runs in the real game (2026-10-11 20:27 and 20:29; started by the user through the launcher)

The user added `efmi_vk_inject.dll` to the launcher's injected libraries and launched twice. The DLL was in passthrough mode (`dist\efmi_vk.cfg`). Evidence: `dist\efmi_vk_layer.log`, the launcher log, `Player.log` / `Player-prev.log`, `F:\EFMI MCP\dist\efmi_probe.log`.

**Run 1, 20:27, DX11** (`cmd_args='-force-d3d11'`, PID 30628; injected: XXMI `d3d11.dll`, `efmi_probe.dll`, `efmi_vk_inject.dll`):

- The DLL loaded, registered the loader notification, and `vulkan-1.dll` was mapped 5.5 s later; 7 of 7 exports redirected. One instance creation was seen (`app=vkFoo`, the engine's probing instance; no device, as expected under DX11).
- The game ran for about 78 s and exited normally (`Player-prev.log` ends with the usual memory statistics, no crash).

**Run 2, 20:29, Vulkan** (`cmd_args=''`, PID 24040; same three libraries injected):

- The DLL loaded 4.6 s before `vulkan-1.dll` was mapped; 7 of 7 exports redirected. Instance creations seen: `vkFoo`, three without application info (API 1.1), then `app=Endfield engine=Unity` (API 1.3). Device creation seen at 20:29:37 on the NVIDIA GPU with 19 extensions.
- About 5 s later the game crashed. Stack from `Player.log`: `unityplayer` -> `vulkan-1` -> `nvoglv64` (four frames inside the NVIDIA Vulkan driver). `efmi_vk_inject.dll` is not on the stack. The last line before the crash is the engine's swapchain attempt (`attempt: 0 useWideColorColorspace: 0 hdrProbe: 0)`). The modules loaded immediately before are D3D ones (`nvwgf2umx.dll`, `d3d11on12.dll`, `dcomp.dll`, `D3DSCache.dll`), which fits the NVIDIA driver setting up DXGI-based presentation for the swapchain.
- In the same process XXMI's `d3d11.dll` (3Dmigoto, which hooks D3D11 / DXGI creation) was loaded, and the DX11 probe had redirected `D3D11CreateDevice` of the system `d3d11.dll` at 20:29:28.

What this establishes:

- **Timing of the launcher's injection is early enough.** Twice the DLL was in the process several seconds before `vulkan-1.dll`. The engine's instance and device creation went through the hooks. This answers the timing and resolution questions of LIMIT-013.
- **The DLL loads and redirects in the real game under DX11 without a visible effect** (one run, 78 s; not the 30 minutes G1 asks for).
- **`jobified=0` under Vulkan as well**: `Player.log` of run 2 says `GfxDevice: creating device client; threaded=1; jobified=0`. A first answer to G2: this run did not use graphics jobs, despite `boot.config`. To be confirmed by counting recording threads once a frame is captured.
- Implicit layers the game listed under Vulkan: `VK_LAYER_NV_optimus` (disabled by the engine), `VK_LAYER_NV_present`, EOS overlay, Steam overlay, Steam Fossilize, Rockstar Social Club.

What it does not establish: the cause of the crash. Suspect, unproven: the D3D11 hooks of XXMI and of the DX11 probe inside a process whose Vulkan driver creates D3D / DXGI objects for presentation. Not excluded: this DLL (absent from the stack, and it hooks only creation and present in passthrough mode), or the game's Vulkan path failing by itself on the current game and driver versions. G1 is therefore neither passed nor failed.

**Run 3, 20:38, Vulkan, only this DLL injected** (`cmd_args=''`, PID 1660; the launcher log lists `efmi_vk_inject.dll` alone; XXMI DLL injection mode Skip, the DX11 probe removed from the list):

- The DLL loaded 4.1 s before `vulkan-1.dll`; 7 of 7 exports redirected; the same sequence of instance creations as in run 2; device creation seen at 20:38:29.
- The game went past the swapchain attempts where run 2 crashed (three `attempt: 0 ...` lines in `Player.log`, no crash) and was still running normally 50 s later, loading assets, when the logs were read. `Player.log` again says `jobified=0`.

Reading of runs 2 and 3: the same Vulkan launch crashes with XXMI's `d3d11.dll` and the DX11 probe injected and does not crash without them. That points at the DX11 hooks, not at this DLL and not at the game's Vulkan path. It is one run each, so "strongly indicated", not proven; which of the two DX11 libraries is responsible is unknown and does not matter for this project. The configuration for every Vulkan session from now on is run 3's.

**Run 3 outcome, reported by the user**: exited before 30 minutes, with scene changes. No `Endfield` process was present when checked. The layer log ends at device creation and has no destruction/frame-count record; `Player.log` has shutdown memory statistics and a JobTempAlloc allocation warning at its end. Exact duration and total frames cannot be inferred from the layer log. The user subsequently accepted this as sufficient initial loading evidence to continue (D-014); it does not establish long-duration stability.

## Inject entry (2026-10-11; sandbox only, game not involved)

The user asked whether the DLL can get into the game by injection, as with d3d11, instead of a registry entry. It can; the entry was added (D-011, architecture in `ARCHITECTURE.md`).

- `dist/efmi_vk_inject.dll` is the same binary as `efmi_vk_layer.dll`; the file name selects the entry. `dist/efmi_vk.cfg` configures a process that cannot be given environment variables.
- `sandbox/run_all.py` passes: unit 36; probe end to end 156 / 158 / 158 with the layer entry (0, 2, 5 threads) and 158 / 160 with the inject entry (0, 2 threads); modes 40; MCP 35. The inject runs establish: with the DLL loaded before Vulkan, everything the layer entry reports is reported identically (draws, hashes, read-back), `vulkan-1.dll` was found after the DLL, at least 40 exports were redirected, no passive call. Loaded after the device exists, the sandbox runs on unaffected, no device is tracked and a capture is refused. Loaded late with the device functions resolved again through the exports, more than 100 calls go through the passive device and the sandbox still runs to its end. Passthrough works in both entries with agreeing frame counts.
- Not established: injection from another process; anything in the game (LIMIT-013).

## Probe, sandbox and MCP server (2026-10-11; sandbox only, game not involved)

- **Built**: `dist/efmi_vk_layer.dll` with manifest, `dist/efmi_vk_sandbox.exe`, both with MSVC from `D:\Microsoft` against the headers in `F:\vulkan\Vulkan-Headers` (tag `vulkan-sdk-1.4.321.0`, commit `2cd90f9d`).
- **Loads in the sandbox**: the loader picks the layer up through `VK_LAYER_PATH` / `VK_INSTANCE_LAYERS` set for the child process only. Device: NVIDIA GeForce RTX 5060 Laptop GPU.
- **Captures in the sandbox**: `sandbox/run_all.py` passed in full when this was written (the current counts are in the section above). What the checks establish:
  - every draw of a frame is reported in order with index buffer id, bind offset, index size, first index, count, instances, dynamic offset, pipeline, recording thread and command buffer;
  - the region hash of each draw equals the value the sandbox computed with an independent CRC-32C, for a buffer read from the application's mapping and for a device-local buffer read from the shadow;
  - a rewrite through a staging buffer that is unmapped before the copy is recorded changes exactly the hash of the rewritten mesh, with no unknown write;
  - draws recorded on several threads into secondary command buffers arrive in execution order with the right thread and command buffer ids;
  - without the layer, and in passthrough mode, no pipe appears; in passthrough mode the layer's frame count equals the sandbox's.
- **Not established**: the rendered picture of the sandbox was not looked at; only that every Vulkan call succeeded on the driver. There is no validation layer on the machine (LIMIT-002). Overhead was not measured.

## Region hash rule checked against real DX11 data (2026-10-11; offline)

`scripts/verify_region_hash.py` on `FrameAnalysis-2026-10-07-123937`: 688 indexed draws in the log, 711 IB files, 336 distinct regions; for all 336 the CRC-32C over `[bind offset + index size * first index, index size * count)` of the dumped buffer equals the region hash in the file name; no mismatch. 23 files had no draw information in the log and were skipped. The dump also shows that the game binds shared index buffers at large byte offsets under DX11 (plan fact 14).

## Workspace (2026-10-11)

`docs/plan.md` is the English v2 plan; `.ai-memory/` and `AGENTS.md` are in place; `scripts/check_text.py` enforces the no-Chinese rule; the Git repository is on branch `main`. The final check found an existing `origin` at `https://github.com/loyal37/efmi-vulkan.git`, superseding the earlier no-remote observation. This round did not configure it or push anything; changes were committed locally. `.mcp.json` registers the MCP server `efmi-vk` for this workspace (a session picks it up when it starts).

## Next

State on 2026-10-12 after the first probe session and the DX11 baseline:

1. T-017 is done, G3 b and c pass for the display scene (D-021). The launcher's EFMI profile and `dist/efmi_vk.cfg` stay at their Vulkan values.
2. T-018, game part: a Vulkan session with 0.7.0 on the display scene (third session in `docs/first-probe-session.md`), when the user starts it.
3. M2 (minimal replacement of one component) with the vertex-stage set of D-022.

Later sessions: open world, combat, several instances, LOD1, indirect draws. M3 must account for the image panel (T-016).

Historical run 3 remains accepted initial G1 loading evidence. A repeated 30-minute passthrough run is not a prerequisite. No real-game launch or injection was performed in this development round.
