# EFMI Vulkan - Development log

> A short summary is appended at the end of each round: what was actually done, the scope of verification, open problems and where to pick up next time. No chat and no complete logs.

## 2026-10-11: Review of the v1 plan, v2 plan, workspace bootstrap

The user asked for a feasibility review of the v1 plan (a Chinese document in the EFMI output folder). The review read the local Core INIs, a sample mod, the game directory, the Unity player log and the two reference projects. Main findings: the game runs Vulkan by default; native graphics jobs are enabled; the ACE anti-cheat is present; Core issues a dispatch and a buffer copy for every replaced draw, which is illegal inside a Vulkan render pass; ReShade does not expose `vkMapMemory`; the existing frame dump has no modded draw.

A v2 plan was then written. Further reading of the XXMI source and of the mods showed that the IB hash is a plain CRC32C over the index bytes of the draw, that the two merged-skeleton compute shaders only copy and patch, and that RabbitFX mods cannot be stubbed. The plan chooses an own Vulkan layer, the mod INI plus the EFMIv1 interface as compatibility surface, and two-phase execution with CPU-side data preparation.

The user then asked for: a memory system modelled on `F:\EFMI MCP\.ai-memory`, a Git repository, no Chinese characters in the workspace, a live capture tool like the DX11 one to help development, and to start working. Answers to three questions: Vulkan headers go to `F:\vulkan`; commit locally after every verified change; the plan document is tracked.

Nothing was run in the game in this round.

## 2026-10-11: Headers, region hash check, layer probe, sandbox, MCP server

The Vulkan headers were cloned to `F:\vulkan\Vulkan-Headers` (tag `vulkan-sdk-1.4.321.0`). The LunarG installer was not run; it needs elevation and changes system settings.

`scripts/verify_region_hash.py` confirmed the region hash rule on the existing DX11 dump: 336 of 336 distinct index regions match, none mismatches. The same dump shows index buffers bound at large byte offsets.

The layer was written as a read-only probe: interception of memory, buffer, pipeline, command buffer, draw, copy, render pass, submit and present calls; index buffer shadows fed from staging copies; frame capture with region hashes; a named pipe with the framing of the EFMI MCP probe. The sandbox program draws five indexed meshes from a device-local and a mapped index buffer, optionally recording on several threads into secondary command buffers; its shaders are assembled in source because there is no shader compiler. The Python side has the pipe client, a command-line tool and a stdio MCP server with nine tools, among them `efvk_match_mod`, which answers gate item G3 a.

Verification: `sandbox/run_all.py` passes (unit 36, probe 151 / 153 / 153 with 0 / 2 / 5 threads, modes 11, MCP 35). All of it is sandbox evidence on one NVIDIA GPU without validation layers; the game was not involved and nothing was registered system-wide.

Open and next: descriptor set, shader and image tracking (T-011) for G3 b to d; the M0 inventory (T-007); a DX11 baseline with the sample mod (T-008); G1 waits for the user's decision on account and registration (T-009).

## 2026-10-11: Inject entry

The user asked whether the DLL can be loaded into the game by injection, as with d3d11, instead of registering a layer. The launcher log confirmed that XXMI injects `d3d11.dll, efmi_probe.dll` into `Endfield.exe` at start, and the EFMI MCP probe's `bootstrap.cpp` showed the export redirection it uses on the system `d3d11.dll`.

Added `layer/src/bootstrap.cpp` (loader notification, redirection of the exports of `vulkan-1.dll` through jump stubs), an inject path in the create and proc-addr hooks, the passive device for devices created before attaching, the settings file `efmi_vk.cfg`, the second output `efmi_vk_inject.dll`, attach information in the status, and three sandbox options that load a DLL early, late, or late with the functions resolved again through the exports. Layer version 0.2.0.

Verification: `sandbox/run_all.py` passes (unit 36; probe 156 / 158 / 158 with the layer entry and 158 / 160 with the inject entry; modes 40; MCP 35). The injection in these tests is the sandbox calling `LoadLibrary` itself; no cross-process injection and nothing in the game.

The user also said that anything the project needs installed goes into `F:\vulkan` without asking (D-012).

Next: T-011 (descriptor, shader and image tracking), T-007 (inventory), T-015 (validation layers into `F:\vulkan`); G1 through the launcher waits for the user (T-009).

## 2026-10-11: Preparing G1 with the user

The user added `F:\efmi vulkan\dist\efmi_vk_inject.dll` to the launcher's injected libraries and asked how to remove `-force-d3d11`. `dist\efmi_vk.cfg` with `mode = passthrough` was written at once, because from now on every launch through the EFMI profile injects the DLL. Reading the launcher source (`core/game_launcher.py`, `build_cmd_args`) showed that the argument comes from the configuration value `d3d11_mode_cmd_args`, which has no control in the settings window and no signature; the user was told how to clear it and how to restore it. Nothing was run in the game by this project, and the launcher configuration was not edited by this project.

## 2026-10-11: First launches with the DLL in the game; a crash under Vulkan

The user cleared `d3d11_mode_cmd_args` and reported that the launch failed. Read-only diagnosis from four logs: the launch before the edit (DX11) ran 78 s with the DLL present and exited cleanly; the launch after it (Vulkan) reached device creation through the hooks and crashed 5 s later inside `nvoglv64`, called from `vulkan-1` from `unityplayer`, during swapchain set-up. This project's DLL is not on the stack. XXMI's `d3d11.dll` and the DX11 probe were injected into the same process, and D3D modules were loaded right before the crash.

Recorded as BUG-001 with the cause open. Gains: the launcher's injection is early enough (4.6 to 5.5 s before `vulkan-1.dll`), the engine's creation calls pass through the hooks, and `Player.log` reports `jobified=0` under Vulkan.

The user was asked to repeat the Vulkan launch with only this DLL injected (XXMI DLL injection mode Skip), and, if that crashes as well, with nothing injected. Nothing in the launcher configuration or the game was changed by this project.

## 2026-10-11: Third launch, Vulkan with only this DLL

The user set the launcher's XXMI DLL injection mode to Skip, removed the DX11 probe from the injected libraries and launched. The game passed the swapchain set-up where the previous run crashed and kept running; the user was asked to use the session as the 30-minute G1 run and to report when it ends. BUG-001 is thereby avoided and its cause strongly indicated (the DX11 hooks).

## 2026-10-11: Call counters while the G1 run was going

While the user played the G1 session, layer 0.3.0 was written: a call counter on every intercepted function, about forty counted-only functions generated by one template, the names the application resolves, the enabled extensions, the probe command `api_usage` and the tool `efvk_api_usage`. It was built and tested in `build\next\` (`EFMI_VK_DIST`), since the game held the DLL in `dist\`; `run_all.py` learned to build into that directory. All steps pass. `dist\` is to be rebuilt after the game exits.

## 2026-10-11: Ordinary descriptors, images and retained shaders; G1 early exit clarified

Resumed from CURRENT_STATE/TASKS and a screenshot of the earlier round. The user confirmed that run 3 ended before 30 minutes, after scene changes. No Endfield process was present; the layer log has no device-destruction/frame total. G1 remains incomplete. No game was started or injected during this round. The existing `dist/` DLL/configuration remain 0.2.0 passthrough; the earlier planned promotion to probe is deferred.

Implemented the ordinary-descriptor part of T-011 as layer 0.4.0: layouts and lifetimes, writes/copies, immutable samplers, variable counts, per-set/per-array dynamic offsets, compatibility disturbance, separate graphics/compute binding state, and submit-time snapshots with buffer ranges/visibility and image metadata. Added shader lifetime retention, SPIR-V hashes/export and the `efvk_shader` tool with limited declared-name/binding extraction. Templates, push descriptors and descriptor buffers explicitly invalidate stale knowledge and still need decoding. D-013 records the timing/lifetime decisions.

The sandbox now actually uses two descriptor sets, a dynamic storage array and a sampled texture, copies from a descriptor set it frees, destroys shader modules before drawing, and updates a storage descriptor on the rewrite trigger. New native tests cover lifetime and uncommon state semantics without a Vulkan driver. Python test artifacts now stay in `build/tests/`; compiler scratch stays in component `build/tmp/`. On this machine the `python` app alias does not launch the interpreter, so commands use `D:/Python312/python.exe` explicitly.

Verification this round: `EFMI_VK_DIST=F:/efmi vulkan/build/next` and `D:/Python312/python.exe -B sandbox/run_all.py`, all steps pass. Build/text clean; Python 44/44; native tracker 37/37; layer probe 385/385, 387/387, 387/387; inject probe 387/387, 389/389; modes 40/40; MCP 46/46. Evidence is sandbox/CPU-only; no visual review, validation layers, overhead measurements or real-character verification. `docs/probe-resources.md` records schema, limitations and the exact tested paths.

Next: T-011 template/push decoding and sandbox cases; T-012 overhead measurement; T-007 offline compatibility inventory. G1's complete 30-minute session and all further game loading still need the user's on-the-spot instruction.

## 2026-10-11: Reuse the prior G1 evidence and prepare the probe session

The user questioned the 30-minute requirement and then explicitly instructed the project to use the previous run's data. D-014 and the plan now accept historical run 3 as initial loading evidence, with long-duration stability deferred. A repeated 30-minute passthrough session is no longer a prerequisite. The run remains 0.2.0 passthrough evidence; no duration/frame total or missing probe data was inferred.

After confirming no game or sandbox process was running, preserved the old DLLs, manifest, configuration and historical log in `build/backups/before-probe-20261011-212452-288/`. Rebuilt 0.4.0 into `dist/` and set `dist/efmi_vk.cfg` to probe mode. The layer source did not change. The historical game log remains byte-for-byte unchanged; test logs were confined to `build/`.

Verification this round: full `sandbox/run_all.py` with `D:/Python312/python.exe`, process-local `EFMI_VK_DIST=F:/efmi vulkan/dist` and a native-test log path under the backup directory. Build/text passed; Python 44/44; native tracker 37/37; layer probe 385/385, 387/387, 387/387; inject probe 387/387, 389/389; modes 40/40; MCP 46/46. A separate sandbox used the actual `dist/` settings with no mode environment override, reported 0.4.0 probe, captured five draws, and exited with code 0 (`build/tests/sandbox-xm23wqvx/`). The layer and inject copies have identical bytes (SHA-256 `ab54243ae1db4072169e4587686a7df2c71115c7f4303fe2f116363305e07cf0`).

No game was started or injected, and no launcher or system configuration changed. Loading 0.4.0 in the real game and all G2/G3 capture evidence remain open. The next user-chosen short probe session can collect API usage and a frame with the sample character visible. Long-duration observation is a separate follow-up. Offline development can continue meanwhile.

## 2026-10-11: Descriptor templates and push descriptors, without the game

The user requested development of parts that need no game testing. Implemented the next T-011 increment as layer 0.5.0: retained template definitions and offset/stride decoding; direct and template push descriptors; immutable recording snapshots, per-command-buffer/per-bind-point separation, partial updates and layout disturbance. Added core/KHR and maintenance6 wrappers. Executing secondary command buffers now invalidates primary descriptor bindings while retaining copied draw snapshots. JSON exposes binding kind, descriptor timing and resource-metadata timing separately (D-015). Standard writes, templates and pushes share one decoder; unsupported types/layouts remain explicit unknowns.

The sandbox now has ordinary/template/push/push-template material modes. New modes render using the descriptors, including padded template data, an initial template destroyed before drawing, partial direct pushes and different storage ranges for each draw. Template rewrites use a second template. Added native cases for template lifetime/reuse, arbitrary/zero stride, overlapping entries, ignored immutable-sampler input, partial pushes, independent command buffers/bind points, secondary execution and reset.

Verification: built under `build/next/` and ran all of `sandbox/run_all.py`; all steps passed. Final targeted probe run after test cleanup also passed all 17 configurations. Python 44; native tracker 62; modes 40; MCP 46. Ordinary layer 416 / 418 / 418, inject 418 / 420; templates layer 420 / 422, inject 422 / 424; direct pushes and push templates each layer 427 / 429, inject 429 / 431. Core 1.1 template and KHR push commands have GPU coverage; newer aliases and compute/texel wrappers do not. No validation-layer, visual or overhead result was established.

With no game/sandbox process running, backed up 0.4.0 to `build/backups/before-0.5-20261011-214412-992/` and copied the exact tested package to `dist/`, verifying SHA-256 equality. DLL hash: `c9a8b41db3234ecebce64300d0c1dd34de259b6ebd23827516001df4c89a8005`. A sandbox using the actual `dist/` cfg reported 0.5.0 probe, captured five push-template draws with two recording threads and exited normally (`build/tests/sandbox-f6c4cx5r/`). The historical game log is unchanged. No game, launcher configuration, external installation or system state was changed.

Next offline work: T-012 overhead measurement, T-015 validation layers, T-007 compatibility inventory. T-011 standard paths are implemented; specialized paths and G3 byte-availability evidence remain open. No new game session is needed for the next offline tasks.

## 2026-10-11: Many-draw timing and probe overhead

Added repeated meshes with `--draws`, raw host timing phases, warmup/sample control, rotated three-mode benchmark trials and a many-draw capture/timing regression. Child baselines clear inherited explicit-layer activation; failed sandbox startup cleans up its process. Layer code/binary remain 0.5.0. The sandbox build and full regression against `build/next/` pass: unit 44, native 62, all 17 probe configurations, modes 40, timing 84, MCP 46. Copied the tested sandbox to `dist/`; no DLL/config/log change.

Completed 36 timing runs (500/2000 draws, 0/2 recording threads, no layer/passthrough/probe, three rotated trials). At 2000 draws, the median recording-plus-submit increment is 0.424 ms primary / 0.553 ms secondary. FIFO keeps frame medians around 5.55 ms. This is neither game performance nor evidence of the M2 5% target. `docs/probe-performance.md` preserves method, limits and fingerprints; `build/tests/benchmark-owy1jn92/` holds raw samples. T-012 is complete; continuing T-015 and T-007 without the game.

## 2026-10-11: Validation dependency and sandbox correctness

Built pinned Khronos VVL under F:/vulkan, added core/synchronization validation and negative canaries to the suite, fixed sandbox read-only SPIR-V and failure-path shader cleanup, added upload barriers and per-image presentation semaphores. Full regression passes; final validation passes 76 checks over 24 positive configurations and three canaries. Layer DLL is unchanged. Copied the corrected sandbox to dist/. Repeated 36 performance trials after fixes: 2000-draw probe increment 0.386 ms primary / 0.744 ms secondary. No game was launched.

An initial VVL trial wrote a 36-byte default temp cache outside the project. The harness now confines all its child temp/cache variables to build/tests/; subsequent tests confirm that the old cache stays unchanged. The external file was left untouched. Exact coverage, evidence, hashes and limitations are in docs/validation.md and docs/probe-performance.md. T-015 is complete; T-007 inventory continues.

## 2026-10-11: Reproducible compatibility inventory and frontend assessment

Completed T-007 without the game: a lexical inventory tool, metadata-only snapshot and compatibility contract covering main mod/Core syntax, every direct EFMIv1 reference, resource/pool behavior and the image panel missed by the original plan. Fingerprinted 46 Core and 86 mod files and selected game binaries/content indices. Verified local d3d11.dll equals the official v1.2.2 release bytes; pinned source a7847eae82aaac32175c9a7d199673d5c9323bcd. Engine version is known; the marketing/content release label remains unknown and is not guessed.

D-016 retains hosted XXMI reuse after inspecting parser/state dependencies and the pinned WWMI-DX12 host, with a compiled prototype deferred to M3. Corrected the sample description: configured 13 slots, actual ids 0-11, plus enabled k/1.ini panel. Snapshot check matches every fingerprint/lexical item and the English-only check passes. No source from the reference engines was compiled or copied into the layer, no game was started, and no installed mod/launcher file changed. Remaining M2 backend decisions need actual G2/G3 data.

## 2026-10-11: Preserve first-probe evidence and stop at the game-data boundary

Added scripts/collect_probe.py and server/efmi_vk/evidence.py: raw frame, before/after status/API usage, INI fingerprints/matches, G2 summary and bounded retained pipeline/SPIR-V export. Bundles require explicit PID/target, reject wrong executable labels and existing output directories, retain partial files on interruption, and write a hashed manifest last. Live-query gaps and truncation are explicit. No launcher/inject/process-start behavior exists.

The evidence regression passes 52 checks through loader/inject entries with push templates and secondary recording: raw draws and snapshot timing, region matching, shader retention, all file fingerprints, scope/overwrite refusal and a simulated pipeline lifetime gap. Added it to run_all.py. The first-session guide states which G2/G3 questions a bundle can answer and which still require record-time measurements or a modded DX11 baseline.

Final full regression against `dist/` passed: Python 44, native tracker 62, all 17 probe configurations, modes 40, timing 84, core/synchronization validation 76, MCP 46 and evidence export 52. Evidence: `build/final-offline-regression.txt`. DLL and sandbox hashes match `build/next/`.

The native test's default logger appended one 121-byte startup line to `dist/efmi_vk_layer.log`. Verified that the preceding 2246 bytes exactly matched the saved historical log, preserved the mixed file under `build/backups/native-log-repair-20261011-223459/mixed.log`, and restored the exact original bytes. Fixed native_test.py to use an explicit test log and working directory under build/tests/. The corrected native test passes 62 checks and leaves the restored log's SHA-256 unchanged (BUG-002).

The user's requested stopping point is now reached: current offline prerequisites are implemented and verified; actual game descriptors, thread/submission structure, sample IB regions and shader identities are needed before choosing the next backend work. No game was launched or injected. The historical G1 result remains accepted with the long-duration observation deferred. All changes are committed locally. The closing check found an existing origin at https://github.com/loyal37/efmi-vulkan.git; this round did not configure it or push anything.

## 2026-10-11: Review of the 0.4.0 / 0.5.0 commits before they go into the game

The user asked for a review of the seven commits made by another agent (descriptor, image and shader tracking, templates and pushes, performance measurement, validation layers, compatibility inventory, evidence collector), none of which had been in the game, and for problems to be fixed directly.

Method: read the layer changes in full (`resources.cpp`, `resources.h`, `resource_hooks.h`, the diffs of `state.*`, `layer.cpp`, `ipc.cpp`), the tests, scripts and server changes; rebuilt into `build/next/` and ran the whole suite, which passed with the numbers the memory claimed. The validation layer under `F:/vulkan` exists and the suite uses it.

Findings: no error in what the tracker reports for the sandbox. Four parts scaled with the observed process and were fixed in 0.5.1 (D-017): unbounded SPIR-V retention; full scans of all retained shaders on every module or pipeline destruction and of all descriptor sets on every pool reset; no bound on the descriptor contents of a capture; no tracking level between full and passthrough. Also fixed: vertex input pointers were read for mesh pipelines; the evidence collector failed on a stage without a known module; an inline module was kept even with resource tracking off.

Verification: `sandbox/limits_test.py` (new, 100 checks), native tracker 78 checks (16 new), the rest unchanged; the whole suite passes. One transient linker failure was seen and recorded (ISSUE-002). `dist/` was updated to 0.5.1 from the tested files with the previous package saved under `build/backups/`; the settings file keeps `mode = probe` and now lists the two fallback settings as comments. Nothing was launched or injected.

## 2026-10-12: First probe session in the game

The user started the game in the Vulkan configuration with layer 0.5.1 in probe mode and the sample character on screen. Read through the pipe only: status and call counters twice, one collector bundle with the sample mod's INI, and two more captures with live reads of the instance config window and the bone buffer.

Results: one recording thread, one command buffer with every draw, classic render passes (G2); all 12 LOD0 components of `arcane.ini` match by region hash, read from mapped memory (G3 a); the 65536-byte dynamic uniform window and the bone matrices at the offsets of its entry 5 are where EFMI expects them (G3 b, c, from live reads that were partly stale); material textures in set 1, SPIR-V without names (G3 d); descriptor sets are allocated and written through templates every frame. About 120 frames per second and flat memory with full tracking in this scene.

Recorded: CURRENT_STATE (full numbers and evidence paths), D-018, T-017 (snapshot at submit), LIMIT-008 and LIMIT-014. The evidence is under `captures/`, which Git ignores. Nothing in the launcher, the game or the mods was changed by this project.

Next: T-017, then the same scene again; T-008 (DX11 baseline with the mod) when the user has a DX11 session.

## 2026-10-12: Launcher switched back to DX11 at the user's request

The user asked this project to change the two launcher values itself so that the DX11 baseline can be taken. With neither the launcher nor the game running, `XXMI Launcher Config.json` was copied to `build/backups/` and two lines of the EFMI profile were replaced by text (`d3d11_mode_cmd_args` to `-force-d3d11`, `xxmi_dll_inject_mode` to `DIRECT`); the result parses to exactly the intended configuration. The signed list of injected libraries was left alone, and `dist/efmi_vk.cfg` was set to passthrough for the DX11 session. The working agreement now states under which conditions the agent may change a launcher setting.

Reading the launcher log for this also showed that the DX11 probe was injected in this morning's successful Vulkan session, while the crashed one before it had XXMI's `d3d11.dll` as well: BUG-001 is caused by XXMI's `d3d11.dll` under Vulkan.

## 2026-10-12: DX11 baseline captured and paired with the Vulkan frame

The user started the game under DX11 with the sample mod. One full frame was taken through the DX11 probe of the sibling project (its Python client used as a library with byte code writing off; nothing was written there) and saved under `captures/dx11-baseline`. New: `server/efmi_vk/dx11_pair.py` (pairing on plain dictionaries, 25 unit checks) and `scripts/dx11_baseline.py` (`capture`, `summarize`). `docs/dx11-baseline.md` holds the result; the plan got facts 15 to 19, the decisions D-019, the tasks T-018 and T-019.

Main results: the mod is drawn pass for pass like the game's own components, and the same objects are drawn equally often under Vulkan; the instance entry and the bone offsets have the structure read under Vulkan in the morning; the instance window has a fixed place under Vulkan where DX11 needs a per-shader search; texture slots need a table per fragment shader; the vertex input differs in the blend indices only; under DX11 the pixel shader keeps reading the game's bone buffer and instance window, which one shared Vulkan binding does not give for free.

Not done: the frame with the mods off. The probe refused to press F11 because the game window was minimized and sent nothing; this project did not touch the window. Also found: `python` on the PATH is the Store stub (ISSUE-003). The layer was not changed; unit tests and the text check were run, the full suite was not.

## 2026-10-12: Mods-off frame; the slot table is per Vulkan fragment shader and per pass

After the user restored the game window, the DX11 probe pressed EFMI's F11 through the sibling project's own toggle routine (it reads EFMI's notification from the frame to confirm the state), one frame was saved with the mods off and one after the second press with the mods on again. Pairing that frame showed three things the first frame could not: all 12 original components are drawn equally often under both APIs (45 draws); one DX11 pixel shader corresponds to two Vulkan fragment shaders with different binding numbers, so the table is keyed by the Vulkan fragment shader; and the mod's slot assignments apply in every pass, landing on other roles in some passes, which a faithful port has to reproduce.

`scripts/dx11_baseline.py` now saves the slots each shader declares and pairs only those (DX11 keeps stale bindings), pairs the game's own draws one by one, and has a `shaders` command for frames saved earlier. `docs/dx11-baseline.md` was rewritten around the four frames; the plan has fact 20 and a corrected fact 18. T-008 is done. A heredoc turned a regular expression's word boundary into a backspace character while patching; found by the control character count and repaired from a script file.

## 2026-10-12: DX11 texture content hashes

While the DX11 session was still open, one more pair of F11 presses gave a frame with the mods off in which mip 0 of every file-backed texture was read back and hashed (`capture --textures`, 65 textures, about 83 MB). The mods were confirmed on again afterwards. The frame itself no longer pairs with the Vulkan frame: every component is drawn once more than ten minutes earlier.

## 2026-10-12: Launcher back to its Vulkan values

The user closed the game and the launcher after the DX11 baseline. On that go-ahead the two values of the EFMI profile were set back by text replacement (`d3d11_mode_cmd_args` empty, `xxmi_dll_inject_mode` `SKIP`), after a check that neither process ran and with a copy of the file in `build/backups/`; the result parses to exactly the intended configuration and two lines changed. `dist/efmi_vk.cfg` is `mode = probe` again. Nothing was launched.

## 2026-10-12: Layer 0.6.0, contents at submit with a record-time comparison (sandbox only)

Built T-017. The layer reads what selected draws take from buffers twice: a checksum inside the draw call, the bytes at submit; it keeps the bytes within a budget and reports per range whether the two reads agree. A follow rule turns values found in a uniform range into windows of the draw's storage ranges, which is how the bone rows are reached from the instance entry. The sandbox got late-written uniform data so that the comparison is exercised in both directions, also with secondary command buffers and push descriptors. Client, collector (`--snapshot`, `--follow`), tests and `docs/probe-snapshots.md` follow. The full suite passed against `build/next` and against `dist/`, where 0.6.0 now is.

Two patch scripts sent through a shell here-document lost their backslashes again (a regular expression before, C string escapes this time); the second failed its own match check and changed nothing. Patches with backslashes go through a script file.

## 2026-10-12: Second probe session, layer 0.6.0 in the game; G3 b and c pass for the display scene

The user started the game under Vulkan. The collector took one frame with the contents of the mod's 45 draws and one with all indexed draws and every pipeline. The first frame showed everything equal between record and submit; the second showed two bone windows that differed, both in the shadow pass. Series followed to find out where: with windows of exact sizes the bytes that arrive late turned out to lie behind the palettes, never inside one (component 4: 55 bones always equal, 56 not), and the preset's large window was the reason for the first sighting. Reading the meshes from the game gave the same bone counts the mod's INI states. The order of the palettes in the buffer changed between frames, which first looked like a contradiction between two measurements.

The DX11 pairing was repeated against the new frame with all pipelines; the slot table is now keyed by SPIR-V hash throughout. Plan facts 21 and 22, D-021, T-017 closed. The layer was not changed in this round; no test run was needed.

## 2026-10-12: T-019 decided from the shaders of the two Vulkan sessions

The retained SPIR-V of the character's 26 programs was read for which stage refers to the bone buffer and the instance window. The vertex shaders are uniform in the two things that matter (one storage buffer, window at set 2 binding 0), the fragment shaders are not (20 of 45 draws read the bones). That makes the vertex-only re-pointing both exact and simple, and the shared replacement neither. `spirv.py` got `buffer_variables` and `repoint` with unit tests, `scripts/stage_usage.py` reproduces the table and checks the rewritten modules with `spirv-val` from the validation layer build in `F:\vulkan`.

## 2026-10-12: Layer 0.7.0, content hash of texture uploads and pipeline creation inventory (sandbox only)

Built T-018 after the game was closed. Image uploads to mip 0, layer 0 are hashed at submit and the value travels with the descriptor contents; the pairing tool matches textures by content before it falls back to their look and now lists the fate of every slot the mod sets. The same build counts what graphics pipeline creation uses, as preparation for re-creating pipelines in M2. The sandbox's texture became 4x4 with three uploads, one of them with padded rows, which the first test run hashed correctly.

The first full suite run had two failures, neither in the layer: an MCP test still expected the 1x1 texture, and the harness hit a sharing violation on the sandbox's freshly renamed expected file (ISSUE-004). After both corrections the two tests passed against `build/next` and the full suite passed against `dist/`.

## 2026-10-12: Third probe session, resolved texture slots and reported frame-time spikes

Restored memory and connected to the user's already running 0.7.0 Vulkan process, PID 19264. The user reported intermittent stutter during both loading and steady display. Saved status/API evidence and frame 22563 (captures/third-probe, complete, 172 manifest files verified), all 12 character components and all pipelines. No new game process was launched, no input was sent and no rendering was changed.

Compared the existing mods-off DX11 baseline using its 65 saved texture hashes. All four open entries resolve by content; 19 sample slots are placed, 37 undeclared, none open. Saved a metadata-only profile and the third-session report; preserved earlier comparison files. Corrected the comparison's console report to distinguish a binding conflict from multiple DX11 shader identities sharing one Vulkan shader (zero binding conflicts, two unrelated UI identities). T-018 is complete for this scene. Pipeline creation requirements for M2 were recorded as well.

The cumulative upload-processing timer is about 8.1 seconds over 2.55 GiB; a steady three-second interval adds only 596 microseconds while 360 presents advance. This does not identify the cause of all reported stutter. Backed up the cfg and set image_hash = 0 for the next process, keeping resources on. Added D-023 and T-020. Layer 0.7.1 adds bounded host present intervals, window quantiles and peaks with hash/capture tags; no per-frame allocation or resource lock. This is host timing and not GPU/display timing or a verified performance fix.

Full build/regression against build/next passed: unit 88, native 85, 17 probe configurations (503-518), modes 40, limits 148, timing 94, validation 76, MCP 47, evidence 78 (build/pacing-regression-20261012.txt). Normal sandbox tests explicitly enable upload hashes even if the game's cfg disables them; disabled-path tests still pass. Native tests retain an 80 ms hitch among 8 ms intervals across ring rollover.

After the user confirmed normal exit, verified no target process remained, backed up dist/ to build/backups/before-0.7.1-20261012-122110/ and copied the exact tested package. DLL SHA-256 729d914e1f4a1ac66ae26ee2d421707d898b7426e52c756a5997cec50dfe0b7e. Actual-config inject smoke (build/tests/sandbox-un722puo/) confirms 0.7.1 probe, resources on, upload hashing off, five draws and host timing. The historical game log is unchanged; no launcher setting changed. Next: a user-started hash-off comparison; 0.7.1 has not loaded in the game, and the stutter issue remains open.

## 2026-10-12: Hash-off timing in the display and open world, layer 0.7.1

The user started PID 31124 and requested a spike check after 20 seconds in the display. Status confirms 0.7.1, resources on, hashing off, zero captures. Initial reads have recent maxima 13.076/21.729 ms but the game is background at both endpoints. The user clarified that the static character cannot reveal stutter and the open world stutters severely, then confirmed readiness for foreground world sampling.

After five seconds to return to the game, collected nine status reads over 39.992 seconds, with 400/400 foreground polls. Between reads, 11,681 host present intervals contain 142 over 16 ms, 27 over 33 ms and 12 over 50 ms. Maximum 111.204 ms at frame 69089; final rolling window p50 2.989 ms, p99 24.590 ms. All hashing counters and capture sequence remain zero. A supplementary 20.080-second start/end-only interval has 46 over 33 ms and recent maximum 141.033 ms, but only 78/200 foreground polls; recorded as focus-confounded, not a clean comparison. Manifests and analysis are under captures/pacing-hash-off-20261012/.

Recorded docs/pacing-hash-off-session.md and updated T-020/ISSUE-005. Hash-off does not remove world host spikes; resource tracking, game work, driver and presentation remain unisolated. Settings are unchanged; next useful comparison is resources = 0 in the same world after a user-chosen restart. No capture, input, focus change, launch, stop, build or layer change was performed. Verification is evidence fingerprints and English/diff checks; the unchanged layer did not need a regression rerun.

## 2026-10-12: Prepare the resource-off comparison after measurement

With the preceding evidence saved, backed up dist/efmi_vk.cfg to build/backups/before-resources-off-20261012-124818/ and enabled resources = 0, leaving image_hash = 0. The current game process retains its startup settings; nothing was hot-switched or restarted. Actual-config inject smoke (build/tests/sandbox-a_60l8x8/) confirms 0.7.1 probe, resources and image hashing off, zero tracked images/descriptor sets, five captured draws and host pacing available. No layer change or rebuild. This configuration is ready for a user-started same-world comparison, not yet game-verified.

## 2026-10-12: Resource-off stutter persists; passthrough prepared

The user restarted and reports continuing stutter. PID 33204 started at 12:50:29, layer 0.7.1 probe/inject, resources and image hashing confirmed off. Status-only observations captured no frames. The first 40 seconds were mostly background; the second observation had an early focus transition. Selected its final 30.000261 seconds (12:57:21.745-12:57:51.745), all 300 foreground polls and seven endpoints active: 6,766 intervals, 276/74/26 over 16/33/50 ms, largest retained 105.000 ms at frame 53703. Final window p50 2.887 ms, p99 36.888 ms. Hash counters, tracked images/descriptors/retained shaders and capture sequence remain zero. A first analysis assertion correctly rejected a proposed 35-second range containing one background poll; the recorded analysis uses the clean 30-second subset.

Evidence: captures/pacing-resources-off-20261012/, root manifest covering 26 files; report: docs/pacing-resources-off-session.md. Updated current state, tasks, pacing guide and ISSUE-005. Neither disabled resource tracking nor disabled hashing is the sole explanation; remaining buffer/draw tracking and the broader rendering stack are unisolated. Different workloads across runs prevent treating the count difference as a regression caused by the setting.

After measurement, backed up cfg to build/backups/before-passthrough-20261012-125909/ and changed only mode to passthrough. PID 33204 retains its probe setting until exit; no game restart or launcher edit occurred. Actual-config inject smoke (build/tests/sandbox-m_8zuprp/) passes with 546 frames, normal exit, matching destruction count and no probe pipe. DLL SHA-256 remains 729d914e1f4a1ac66ae26ee2d421707d898b7426e52c756a5997cec50dfe0b7e. No layer source/build change or full regression rerun. Next: a user-started same-world passthrough comparison using the user's smoothness report; passthrough still includes injection/setup/present accounting and is not a no-DLL baseline.

## 2026-10-12: User reports no stutter in passthrough

Verified the new game PID 14804 (13:04:17 start), 0.7.1 passthrough/inject log, Vulkan device creation and absence of a probe pipe. The user reports that stutter is gone. Saved captures/pacing-passthrough-20261012/ with log, configuration and a fingerprinted observation. This is a subjective gameplay comparison, without host/GPU/display timing. It narrows T-020 to probe-only work as a group; keep the game in passthrough while investigating buffer snapshots and repeated copy work offline. No game/launcher/config change in this round of observation.

## 2026-10-12: Layer 0.7.2, buffer-copy savings and opt-in work timings

Removed allocation/zero-fill for unreadable record-time buffer sources and the temporary submit copy, preserving submit priority, record-time fallback, unmap snapshots and disjoint self-copy. Added profile_tracker (default off), six timing categories, byte counts, a fixed 64-event history for work of at least 2 ms, and shared QPC timestamps on present peaks. The scopes exclude forwarded driver calls but include tracker waiting/descheduling; nesting and measurement overhead are explicit. BUG-003 describes the proven waste; ISSUE-005 remains open because the game hot path is not identified.

Built in build/next. First full suite failed only the new category-coverage assertion: ordinary staging remains mapped. Added the existing rewrite trigger to exercise unmap-before-copy; the timing test then passed 126 checks. Final full regression passed (build/tracker-work-regression-final-20261012.txt): unit 88, native 101, 17 probe configurations 503-518, modes 40, limits 148, timing 126, validation 76, MCP 47, evidence 78. Native tests also verify bounded history, concurrent accounting, profiling disabled and a 64 MiB unreadable source with no byte-storage allocation.

Observed that the game had exited, checked twice before deployment, and backed up dist/ to build/backups/before-0.7.2-20261012-132253/. Deployed six exact candidate files; both DLL hashes are f8b30ce440856995f0c7776b1adf4196d541e651b4cd4705f3f9fde6e394fde6. The cfg now prepares probe with resources/hash off and profile_tracker on; default buffer shadows and 16 MiB unmap snapshots remain. Candidate and deployed actual-config smoke pass (build/tests/sandbox-bbni7c7u/, sandbox-_5s9b_x_/), five correct draws, timings available, no environment overrides, game log unchanged. No launcher edit, automatic launch or game-tested performance claim. Next requires a user-started same-world comparison with the tested candidate.

## 2026-10-12: 0.7.2 foreground spikes correlate with unmap copies; deploy 0.7.3

The user reports stuttering again. Confirmed user-started PID 41044, 0.7.2 probe/inject, resources/hash off and profiling on. Initial status was background with stale present history; a foreground wait timed out. After the user's explicit readiness, 13 status reads over 24.030379 seconds have 240/240 foreground polls and all endpoints active. The 5,093 host intervals include 203/51/17 over 16/33/50 ms, largest retained 129.125 ms. No captures or hashing. Unmap work copies 3.46 GiB in 11.625 cumulative wall seconds; three sequential unmaps cover 128.807 ms of the largest peak while a zero-byte copy-record operation waits about 44 ms. Preserve wall-time/nesting/retention limits: 11 of 12 query gaps overflow the 64 slow-event capacity. Evidence has 21 verified fingerprints under captures/pacing-tracker-work-20261012/; docs/pacing-tracker-session.md records scope and exclusions.

Implemented 0.7.3 to accelerate detected PAGE_WRITECOMBINE mappings with CPU-gated SSE4.1 streaming loads, exact ranges and fences. Cached/unknown mappings keep memmove. Mapped policy is classified once, recomputed on remap and cleared on unmap. Snapshots and fallbacks are retained, including unmap-before-copy and submit-time priority; lock ordering is unchanged. Added strategy/mapping metadata. Native tests use actual WC pages with inaccessible neighbours and cover offsets, tails, remap and all copy paths. D-025 and BUG-004 record the decision and remaining limits.

An initial build caught the JSON writer method typo; corrected before the successful full build/regression (build/mapped-read-regression-final-20261012.txt): unit 88, native 118, 17 probe configurations 503-518, modes 40, limits 148, timing 126, Vulkan validation 76, MCP 47, evidence 78. Separate headless Vulkan measurements of the production helper on this GPU give type-2 8 MiB medians 24.023 versus 1.270 ms, cached type 3 unchanged, device-local type 4 still expensive at 150.952 ms. Every byte matches. These are local copy timings, not a game-tested speedup.

Observed that the game had exited and checked again before backup/replacement. Backed up dist/ to build/backups/before-0.7.3-20261012-150100/ and deployed the exact candidate. Both DLLs: 4cb8fdc1c111530cdbc8d76c9c2924c1cf61ec6fa701ba64a18edc5b36548c85. Candidate and deployed actual-config inject smoke pass (build/tests/sandbox-wcxt2ysn/, sandbox-zg_0n_36/): five correct hashes, WC mappings/strategy reported, unchanged game log. Settings stay probe, resources/hash off, profile on, default snapshot/shadow limits. No launcher change or automatic game launch/stop. Next requires a short user-started 0.7.3 world comparison; T-020 / ISSUE-005 remains open and no 30-minute prerequisite applies.

## 2026-10-12: Review of the stutter work; the copied buffers were mesh pools; layer 0.7.4

The user asked for a review of the commits made for the stutter and whether it is solved. The three layer versions were read in full: correct, well tested, and honest about being unverified in the game. The open point was the attribution. The session's own buffer inventory, read by generation counter, shows that the copies at unmap were not staging buffers but the game's 16 MiB index and vertex pools on device-local memory, which share the transfer-source bit and the size limit of the rule written for staging buffers. The benchmark that justified 0.7.3 measured the staging memory type; for the pools' type it shows a factor of 1.5.

0.7.4 narrows the rule to transfer-only buffers, drops shadows that could be stale, counts what it passes over and adds a `set` command for the two unmap settings. Native tests 126, limits 172; the full suite passed against `build/next`. A first run in `dist/` rebuilt the DLL there and failed on the diagnostic settings file (ISSUE-006); the tested files were copied back, the harness now pins its settings, and the suite passed in `dist/` without rebuilding. A here-document with an apostrophe in a comment failed in the shell before it ran; the patch went through a script file.

This is a correction of a rule from the first days of the probe, not of the later commits. Whether the game is smooth with it has to be measured in a session the user starts.

## 2026-10-12: The game is smooth with 0.7.4, and the review before it was wrong about why

The user started the game with 0.7.4 and reported that the stutter seemed gone. The first status read settled the question the review had opened: `passed_over` 0. Nothing with further usage is ever unmapped; the copies are staging buffers, tens of thousands of them, and they now run at 3.6 GB/s because of the streaming loads that 0.7.3 added. Across the user's open-world run 3 of 87,005 present intervals exceeded 33 ms. The mesh-pool story came from a counter that also counts tracked writes and from looking at 251 live buffers of a session that had created 280,000.

Three captures in the open world gave the first data from there: one recording thread as before, about 1,190 draws, instancing up to 675, every hash from the mapping, and the mod's LOD1 sections matching in the shadow pass.

The record was corrected rather than extended: `docs/unmap-snapshots.md` rewritten around the measurement with the wrong reasoning kept as its last part, D-026 withdrawn by D-027, BUG-004 and ISSUE-005 closed with the right cause, T-020 closed, T-021 withdrawn, plan facts 24 and 25. Source 0.7.5 restores the default of the copy at unmap and keeps what was right in 0.7.4; it passed the full suite in `build/next` and waits for the game to exit before it goes to `dist/`.

## 2026-10-12: Resume and deploy 0.7.5 after game exit

Restored current state/tasks and checked the screenshot against the repository. The game was no longer running. Backed up dist/ to build/backups/before-0.7.5-20261012-155227/, copied the exact tested binaries from build/next and corrected the cfg comment to describe the restored default. Settings stay probe, resources/hash off and tracker profiling on. Both DLL SHA-256 values are f331ff7c3a390affbc7adfe617a1e1364f9d5441821b86f2ecdabf4ff39c7475.

Full --no-build suite on the deployed files passes (build/deployment-0.7.5-regression.txt): unit 88, native 126, 17 probe configurations 504-519, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 78. Actual-config inject smoke confirms 0.7.5, the full unmap default and correct draw hashes (build/tests/sandbox-2c8cp1sr/); the historical game log is unchanged. No game or launcher start/stop and no launcher edit. Continue M2 offline; no repeated stutter-duration test is required for this deployment.

## 2026-10-12: M2 pipeline recipes, GPU variant/restoration proof and 0.8.0 deployment

Continued until the next necessary game observation. Existing captures lacked complete depth/blend/render-pass state, so added independent core creation recipes and explicit unsupported reasons. The private variant builder reuses shared SPIR-V/layout metadata, redirects only VS bones/instance to set 3, reproduces compatible original layouts and changes the static vertex declaration. The draw helper restores pipeline/VB/IB without touching the original sets. None of it is invoked by probe draw hooks. D-028 refines D8 without requiring a new enabled game feature; T-022 tracks the next gate.

Added CPU ownership/ignored-pointer/failure tests and a headless GPU fixture. The latter builds on a worker after original creation handles die, uses bone index 257 in an RGBA16_UINT replacement, switches uint16 to uint32 indices, applies both own buffer offsets and a nonzero dynamic uniform offset, and confirms original fragment color/blending by pixel readback. The immediately following app draw has no rebinding and still renders correctly. Both RenderPass 1/2 and none/layer/inject pass with zero validation errors/warnings; three negative canaries prove validation is active. No window is opened. All 21 historical character VS modules also match the Python rewrite exactly and pass spirv-val.

Full build/regression: unit 88, native tracker 126, native recipes 120, variant runner 37 over 9 GPU processes, 17 probe cases 514-529, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 78. Log: build/m2-recipes-regression-20261012.txt. Historical shader report: build/variant-shaders-l0syviji/report.json. No layer build/test failure; a temporary actual-config smoke script initially selected draw.hash instead of draw.ib.hash and was corrected, then passed on candidate and dist/.

Game absence checked twice; backup build/backups/before-0.8.0-20261012-163359/; copied exact tested package to dist/. Both DLL SHA-256 values: 6e95b328e5ef019ad3e6d025cad966f2e3b326b97cd34d2b0270155610d73f3d. Settings now probe, resources on, image hashing and optional tracker profiling off; WC fix and unmap defaults retained. Actual-config smokes verify recipes, five hashes before/after rewrite and unchanged historical game log. No launcher edit, game launch or game injection this round.

Stop for the user-started 0.8.0 observation with the sample character visible, display then world if available: collect full matched pipeline/layout recipes plus --snapshot mod --follow instance, prioritize all matched pipelines with --pipeline-limit 128, and inspect preflight and LOD1 bones. No 30-minute prerequisite. M2 remains partial: real animation, bone remapping, textures, asynchronous cache and in-flight retirement still need implementation/evidence. Details: docs/m2-pipeline-variants.md.

## 2026-10-12: Review of 0.8.0 (recipes, variant builder, deployment): no defect found

Requested by the user ("review Codex's commits"); covers `d79cce9` and `09e3e89`. No source change. The game was not running.

Checked directly, not taken from the commit's own account:

- Rebuilt HEAD into `build/next` and ran the whole suite: all steps pass with the documented counts (unit 88, tracker 126, recipes 120, variants 37, 17 probe configurations 514 to 529, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 78). Output: `build/next/run_all.out`.
- `scripts/verify_variant_shaders.py captures/third-probe --dist build/next`: 21 of 21 (`build/variant-shaders-v8lds7nt/`).
- `dist/`: both DLLs hash to `6e95b328...`, the settings file to the value in `build/deployment-0.8.0.json`. The launcher configuration was last written at 15:28:27, before both deployments, and holds the Vulkan values (`SKIP`, empty arguments).
- Read `pipeline_recipe`, `pipeline_variant`, `spirv_repoint`, the hooks and the changes in `state.cpp`. What runs in a game process is the capture at render pass and pipeline creation and the CPU-only preflight inside the `pipeline` command; `PipelineVariant::build` and `emit_variant_draw` have no caller in the layer. The capture refuses `pNext`, flags and extended dynamic state before it follows a pointer, reads depth and blend state only when the subpass uses such attachments, and static viewport arrays only when they are not dynamic. Nothing was added to the per-draw path.

What the earlier game data says about the next session (a prediction, not a result): the 936 pipeline creations of the third session had flags 0, no `pNext`, two stages and only core dynamic states; the 39 pipelines of the 45 matched draws have three set layouts, no push constants, no immutable samplers and no binding flags. Whether the game's render pass creations carry `pNext` or flags was never recorded, so `render_pass_extension` cannot be excluded beforehand.

One note for the integration, added to T-022: 33 of the 45 matched draws use pipelines that declare five vertex bindings (binding 4 with stride 0, the default tangent buffer), while the tracker keeps four slots (`kMaxVb`).

## 2026-10-12: Current 0.8.0 recipes, corrected LOD labels and bounded selection

User-started PID 17484 loads 0.8.0. Display and later world bundles contain complete supported recipes for 39 and 45 matched pipelines. The user corrected the interpretation: they had switched to LOD1 initially and returned to LOD0 after the agent called captures empty. The later two world bundles are LOD0 state with depth-only LOD1 sections, not the requested distant-LOD1 result. Scene labels and missing evidence are preserved in docs/m2-game-recipes.md.

Added a bounded selection collector and CLI section/color requirements, retaining every attempt, manifest fingerprints, explicit exhaustion/interruption and unknown pipeline state. Sandbox evidence tests pass 109 checks. Source layer and deployed DLL stay 0.8.0. Three 12-attempt full-snapshot LOD1 series were still empty, including varied delays; a no-snapshot raw series contains five graphics intervals out of sixteen. An ordinary full recipe collection without snapshots then succeeds on its first attempt: frame 369782, 872 scene draws, 58 matches, 36 supported recipes, all nine LOD1 sections including color passes. Current-session VS checks pass 21/21/19 modules. All saved bundle fingerprints verified.

Do not treat this as complete LOD1 bone evidence. T-023 adds a capture-window fix before another user test: keep recording armed through empty intervals, return one nonempty interval, preserve timeout semantics. No new game launch, replacement rendering, deployment or launcher edit. Also corrected the temporary 0.8.0 smoke's wrong log filename and withdrew its historical-log preservation claim; other smoke checks remain valid.

## 2026-10-12: 0.8.1 keeps snapshot recording armed through empty intervals; candidate verified

Implemented T-023/D-029 in the server capture state machine, Python client and CLI. The default capture is unchanged. The optional wait discards empty intervals' capture data while preserving normal tracking and record-time reads; the response describes one interval with draws and reports the skipped count. Timeout cancels snapshot work. Old layers are rejected explicitly for the new option.

Native tests exercise a draw recorded across two intervening presents, upload/dispatch in a skipped interval, a later buffer write, correct record/submit comparisons, nonmerged statistics, default empty capture and timeout cleanup. All GPU snapshot configurations use the new request too. Full build/regression passes (build/capture-wait-draws-regression-20261012.txt): unit 88, native tracker 144, native recipes 120, variant runner 37, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113.

Candidate actual-config inject smoke passes (build/capture-wait-actual-config-next.json): 0.8.1 probe/resources on, image hashing/profiling off, five snapshot draws with wait enabled, full recipe and correct hashes before/after rewrite. The actual game-log filename is now required and its hashes are saved; equal in this run only. Both candidate DLLs SHA-256 7e311a9f9f358aa50d90c579abfc1384877a8b391aea8ef886d1e87a90ba50d7. Source/candidate only: PID 17484 and dist/ retain 0.8.0. No automatic game stop, launch, deployment or launcher edit. Next requires normal exit, deployment and a user-started LOD1 snapshot observation; no LOD0 recapture or duration test.

## 2026-10-12, 17:36: Deploy the verified 0.8.1 after user-confirmed normal exit

Checked game absence before backup and again before replacing files. Backup: build/backups/before-0.8.1-20261012-173645/. All eight package files copied exactly from the tested build/next and verified by SHA-256; both DLLs retain 7e311a9f9f358aa50d90c579abfc1384877a8b391aea8ef886d1e87a90ba50d7. Deployment manifest: build/deployment-0.8.1.json. Actual-config inject smoke on dist/ passes, including the new wait request with five snapshotted draws, complete recipe, correct hashes before/after rewrite and unchanged actual game log (build/capture-wait-actual-config-dist.json). No rebuild, launcher edit or automatic game launch. Settings remain probe/resources on, image hashing/profiling off. No 0.8.1 game result yet; ready for the user's actual LOD1 scene to collect the missing bone/instance timing.

## 2026-10-12: Actual LOD1 snapshots with deployed 0.8.1

The user entered LOD1 and authorized collection. Confirmed user-started PID 34644 loaded 0.8.1 with probe/resources on, hashing/profiling off. Both bounded collections select their first attempt after 8/7 empty present intervals: frames 19506/20414 have 818/819 graphics draws and 60 dispatches each. All nine LOD1 sections occur, including 25 color-subpass draws per frame; 58 total matches and 36 complete/preflight-supported recipes each. All 58 instance entries and both actual VS bone windows agree at record/submit in both frames. No snapshot omissions; both sets of 147 file fingerprints and parent manifest references verify (captures/m2-0.8.1-20261012/fingerprint-report.json).

T-023 is verified for this scene and the observed LOD1 color-pass timing gap is closed. Data are sufficient to continue offline CPU bone import/remapping; no further scene change or duration test. No replacement rendering, launcher edit, game restart or deployment occurred. Dist/ remains the loaded 0.8.1. Report and evidence limits: docs/m2-game-recipes.md.

## 2026-10-12: Continue offline with CPU skeleton import, candidate 0.8.2

Read the installed Core rolling-buffer importer, initializer, patcher and update rules, and the sample's eight R16_UINT LOD1 remaps without editing them. Implemented bounded atomic CPU import, per-instance/current/previous layout, finer-LOD priority and byte-preserving entry patches. Native tests pass 42 checks; production C++ matches an independent Core-derived Python reference after all 283 matched draws from five saved frames (74 imports, 209 repeats, 407,808-byte master). No direct DX11 output buffer or live replacement is claimed.

Full build/regression in build/next passes: unit 88, tracker 144, bones 42, recipes 120, variants 37, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113 (build/bone-import-regression-20261012.txt). Source/candidate 0.8.2; dist/ and the game retain 0.8.1. Probe hooks do not call the new helper. Continue offline pipeline cache and GPU resource lifetime work. Details: docs/m2-bone-import.md, T-024, D-030.

## 2026-10-12: Cache and draw-snapshot building blocks, candidate 0.8.3

Implemented a bounded asynchronous variant cache and private per-draw VS snapshots. Requests never wait for driver compilation; retirement cancels queued builds and command owners retain ready pipelines. Snapshot storage checks alignments/ranges, pads the last dynamic uniform window and becomes immutable when sealed. Native cache 21 and allocation/range tests 25 pass. The existing GPU test now uses these production helpers, poisons input arrays, retires/stops the cache before submit, retains the pipeline, resubmits the sealed commands, and still passes pixel/state-restoration checks in both render-pass versions through no layer/layer/inject with clean validation.

Full build/regression passes in build/next (build/variant-cache-regression-20261012.txt): unit 88, tracker 144, bones 42, recipes 120, cache 21, snapshots 25, variant runner 39, 17 probe cases 517-532, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113. Dist/ and the running game remain 0.8.1. Helpers are not called by the draw hooks yet; continue minimal sample/command-recording integration. T-025 and docs/m2-cache-and-snapshots.md record the scope.

## 2026-10-12: First complete hooked component, candidate 0.9.0

Connected explicit sample component-7 assets, the CPU importer, asynchronous variants and command-owned snapshots to the actual indexed draw hook. Every draw imports its own palette; mesh and arenas prepare on a worker. Complete transactions replace and restore bindings; unready/unsupported/unreadable cases keep the original. Submission seals snapshots and successful legal reset/begin/pool reset recycles completed slots without per-frame allocation. Runtime enable/disable affects future recordings only. See D-031 and docs/m2-component-hook.md.

Native mesh rejection/declaration tests pass 30. Four actual-DLL GPU configurations (layer/inject, RenderPass 1/2) pass 45 runner checks with 30 pose updates, original next-draw recovery, all reset forms, unchanged resubmission after disable, deliberately unmapped-bone fallback and remap recovery. Core/sync validation is clean. The first full suite found a missing CRC initialization in the new standalone native test, not the DLL; fixed the test initialization and rebuilt that executable. The full final suite passes (build/m2-integration-regression-final-20261012.txt): unit 88, tracker 144, bones 42, recipes 120, cache 21, snapshots 25, variants 39, M2 45, probe 517-532 across 17 cases, modes 40, limits 174, timing 126, validation 76, MCP 47, evidence 113.

The original component-7 package is accepted by the production decoder (40 vertices, 108 indices, 12 local bones at merged 266). All ten matching draws from five saved display/LOD0/LOD1 frames satisfy its explicit profile. The candidate's actual configuration enables that package; scripts/smoke_m2_config.py verifies readiness, ordinary hashes/snapshots/rewrite, clean validation and normal sandbox exit, with the actual game log unchanged (build/m2-component-actual-config-next.json). No installation edits, launcher changes or game restart. Dist/ and PID 34644 remain 0.8.1. Next requires normal exit for exact-file deployment, then a user-started first component replacement observation. Full-character rendering and M2/M3 acceptance remain open.

## 2026-10-12, 18:58: Deploy verified 0.9.0, then pause as requested

The user confirmed normal game exit and requested a pause after configuration. Verified game absence before backup and before each copy. Backed up all old dist/ files with fingerprints to build/backups/before-0.9.0-20261012-185849/. Copied twelve exact tested files, verifying each against build/candidate-0.9.0.json; both DLLs SHA-256 791e7d163c82562acc8732947d615b5caa38530d42ef67b18e618a02d7259d00. The explicit project-owned component package retains SHA-256 318e8354297fcdfd4b352d0ec031a6b2ba6472335fefe0be2c1b16f3eed52167. Manifest: build/deployment-0.9.0.json; implementation commit 592247d.

The deployed actual-config inject smoke passes with M2 ready/enabled, correct five-draw snapshots/hashes before and after rewrite, clean core/sync validation and normal sandbox exit. The actual game log remains unchanged (build/m2-component-actual-config-dist.json). No rebuild or repeated full suite is needed for identical files. Config enables only the explicit component-7 experiment; image hashing/profiling remain off and WC/full-unmap defaults are retained. No launcher change or game launch. Updated memory and paused; resume development or user-started replacement observation only when the user asks. No real-character result for 0.9.0 is claimed.

## 2026-10-12, 19:25: Review of 0.8.1 to 0.9.0; the patched bone offsets were one matrix too high; 0.9.1 deployed

The user asked for the review to continue; it covers `c1bb6db` to `2d64775`. The game was not running. An independent rebuild of 0.9.0 into `build/review-0.9.0` passed the whole suite with the documented counts before anything was changed.

Reading `merged_skeleton.cpp` against the DX11 baseline showed the difference: the layer patched `(1536, 3033)` where the baseline has `(1533, 3030)`. Confirmed three ways before touching code: the core's `MergedSkeleton.ini` passes `$merged_skeleton_offset - 3` to the overrider; all 23 patched vertex entries of two DX11 frames hold `(1533, 3030, 258)`; the disassembled Vulkan vertex shader of the component 7 draw computes `constant5.x + 3 + 3 * index` and picks the entry by `gl_InstanceIndex`. The tests had shared the reading (native expectation, Python reference, sandbox shader). Made the tests right first: with a game-shaped vertex shader the 0.9.0 layer fails all four GPU configurations and four native checks. Then the one-line fix, version 0.9.1 (BUG-005, D-032, `docs/m2-bone-import.md`).

Full build and suite in `build/next` pass (bones 43, M2 45, the rest as before; `build/review-0.9.0/run_all-0.9.1.out`). Replay of the five saved frames: 283 records, same master hash, every patched entry `(1533, 3030)`. Deployed at 19:15 after checking twice that no game or sandbox process ran: backup `build/backups/before-0.9.1-20261012-191540/`, eleven binaries compared by SHA-256, both DLLs `d2850254ded6477c777fc1f06d5f93c8ce921f74897e2881ca996fb8b7cae417`, settings values untouched. Suite on `dist/` without building and the actual-config smoke pass; the game log is unchanged. No launcher edit, no game start.

Nothing else needed changing. Checked with game data rather than assumed: single-instance requirement against `gl_InstanceIndex`; no `Bind*2`, reset or free of command buffers in the 0.8.1 session and one command buffer per frame for the graphics draws, so the arena limit of eight is not near; the package against `CommandList_Draw_Component7`; the mod's index list for component 7 hashes to the game's `a3fe5c47` when written as 16-bit. Recorded LIMIT-020 (a capture does not mark replaced draws). The pause stands; next is the user-started session of T-026.

## 2026-10-12: Resume, first live replacement execution and general-backend direction

User-started PID 3080 loaded 0.9.1 with component 7 enabled in the display/LOD0 scene. A
4.008-second window adds 806 matches and replacements with stable refusal counters. Later
unsupported grows from 96 to 227; the grouped last reason does not identify the individual
condition. Unreadable/exhausted remain zero. Preserved the live status and a full draw/snapshot
bundle, verified all 176 fingerprints, and confirmed both component-7 draws have supported
recipes and record-equal entry/current/previous snapshots. Full-frame offline replay refuses
the additional component-10 pipeline 437's layout before execution. No passed replay claimed.

Located the tiny eye-region mesh and compared its live original XYZ positions to the mod:
identical, although packed normal/UV bytes differ. Saved an uncommitted mesh-space plot.
The user cannot reasonably accept appearance from this part and requests whole-mod support
using general logic, not separate per-component code. Recorded D-033/T-027. The user confirmed
voluntary exit after the pipe disappeared. No launch/stop, runtime toggle, deployment or
external write occurred. Details: docs/m2-first-replacement.md. Continue offline development;
the earlier pause is superseded, and visual/animation acceptance remains open.

## 2026-10-12: General stream contracts and ordered draw transactions, 0.9.2

Added shared backend inputs for vertex stream stride/element mappings and an ordered list of
resolved replacement draws. The old component-7 declaration/emitter now use the shared paths;
no component-number or sample-hash dispatch was added. Every changed stream element is checked
against an explicit source contract, untouched default streams are retained, and failures leave
the output unchanged. The emitter validates the entire bounded list before issuing anything,
supports an intentional empty skip and restores the application once after all selected ranges.

Full build and suite pass in build/next (build/general-draw-0.9.2-regression.txt): recipes 134,
mesh/declarations 49, other suite counts unchanged. Native fixtures cover six UV/blend layout
combinations on arbitrary bindings; command tracing checks ranges, signed base vertices,
instance arguments and restoration. GPU readback proves two distinct replacement entries,
no partial draw on a later invalid item, unchanged FS data, original next draw and sealed
resubmission under clean core/sync validation in both render-pass versions and layer entries.
Source/build/next are 0.9.2; dist is unchanged at 0.9.1. No game action or full-mod compatibility
claim. Details: docs/general-replacement-operations.md. Continue T-027's frontend, resource,
shared-skeleton and material integration using these data inputs.

## 2026-10-12: Pinned XXMI standalone CPU host feasibility

Compiled/linked unmodified XXMI 1.2.2 parser, expression, command-list and pool sources with
the pinned reference's adapter headers and local CPU shims. A headless executable passes 23
checks covering the D-016 semantic questions; keys use synthetic button state, no hooks.
Initial test expectations incorrectly merged root duplicate sections and read ElementFormat's
semantic index before execution. Corrected those against upstream; engine sources unchanged.
The reproducible full build also corrected a self-including wrapper filename and preserved
the original units' Win32 min/max macro requirements separately from CRC compilation.

scripts/probe_xxmi_host.py verifies dependency commits/clean tracked trees, stages original
files under F:/vulkan, builds all units, records fingerprints and returns failure without
running a stale binary. Final report: build/xxmi-host/report.json, 23 passed, zero failed.
Method/limits/attribution: docs/xxmi-host-prototype.md. The GPU seam explicitly refuses when
called, but reference headers still contain unused no-ops and are not a production adapter.
No real mod/resource/Core runtime, layer change, deployment or game action. D-016's bounded
CPU feasibility checkpoint is complete; continue T-027 resource/shared-character integration.

## 2026-10-12: General command-tree adapter and real sample callback plans

Added frontend/xxmi/draw_plan: original expressions, branches and named calls execute while
GPU operations become ordered bindings/draws/layout requests. The adapter has no component
numbers, sample hashes or filenames. Explicit native boundaries survive optimisation and
retain their phase bits. Invalid/unknown operations, unadapted expression queries, aliases
and budget overflows reject the complete output; original skip/layout state stays unchanged.
Resource declaration ownership and frontend variable rollback remain caller responsibilities.

The full standalone build passes the original 23 CPU checks and 28 planner checks. Added a
read-only actual-INI fixture with the original Windows text-mode CRLF handling and parser
diagnostic rejection. scripts/check_mod_draw_plans.py verifies the tested binary/sources,
executes 12 draw callbacks with 16 selected persistent-scalar scenarios at LOD 0 and 1, and
checks selected ranges against the real vertex/index files. All 384 plans pass: 404 draws,
192 mesh-empty callbacks, 18 unique indexed ranges, 38 bound resource declarations. The INI
retains its known hash. Reports: build/xxmi-host/report.json and mod-plan-report.json.

Only callback selection and file ranges are established. OverrideTextures remains an explicit
unresolved native operation; the full mod's initialization, shared bones, GPU assets/materials,
input/persist/reload and panel are not implemented compatibility. No layer change/deployment,
launcher write or game action. Updated architecture/decision scope and the new method document
docs/frontend-draw-plans.md. Continue T-027 offline with those remaining integrations.

## 2026-10-12: Compressed DDS metadata and actual resource sizes

Added a general bounded 2D DDS decoder returning Vulkan formats and exact mip ranges into
owned file data, without decompression, color conversion or payload copies. Rejects unsupported
containers/formats, unsafe sizes, padding, inconsistent headers and incomplete/extra bytes.
The headless host rebuild passes 23 frontend, 28 planner and 15 asset checks. The sample
verifier additionally runs the native decoder on all ten bound DDS files and independently
compares format/dimensions/ranges with the headers; all pass. No GPU upload is claimed.

Measured selected assets: 38 files, 315,328,648 bytes; four DDS images are 8K with 64 MiB
compressed payloads each. Nine selected textures are DX10 BC7, one legacy BC5U, all with
one mip. An initial inspection of every declaration encountered two absent DDS files, but
the verified callback plans never use them; demand loading must preserve this distinction.
INI/assets unchanged; reports in build/xxmi-host/. No DLL/config/deployment/game action.
Updated docs/resource-assets.md and memory. Continue asynchronous GPU resource preparation,
material descriptors and shared character/Core integration without per-component branches.

## 2026-10-12: Standalone GPU texture upload and exact sample readback

Added owning image upload tickets, separating preparation from serialized caller-queue
submission. Images/views stay private until a completion fence is polled; staging is then
released. Decoded formats/mip blocks remain unchanged. Enabled BC/capability/device-limit
checks and partial creation failures publish no partial image. Outstanding uploads remain
owned even after a fence-status error; cancellation destruction waits, normal polling does not.

scripts/check_image_upload.py --sample builds a headless Vulkan test with child-only core/sync
validation. Synthetic 27 checks pass, including changed CPU source after prepare, all compressed
mips, duplicate-submit refusal, disabled BC/extent rejection and an injected staging allocation
failure. GPU blit distinguishes UNORM 128 from sRGB about 55. All ten actual selected DDS files,
including four 8K textures, upload and read back their entire compressed payload exactly;
all runs have zero validation errors/warnings. Evidence: build/assets-gpu/report.json.
One initial test build hit Win32's small macro; renamed the local test variable before execution.

No layer/DLL/config change, injection, game action or original asset write. This is a tested
standalone uploader, not game scheduling/material rendering. Actual enabled-device/queue
state, resource-manager budgets/ownership, material bindings, shared skeletons and full
Core/frontend integration remain T-027 work. Updated docs/resource-assets.md and memory.

## 2026-10-12: Bounded worker texture cache and retained-generation ownership

Added TextureCache and shared allocation budgets. Declarations deduplicate lexically without
touching files; concurrent demands queue one read, and unused missing declarations stay dormant.
The worker reads/decode/prepares/records then polls/releases staging; only the caller submits
already recorded work on its fixed graphics queue. At most one pending upload per cache.
Actual allocation sizes are charged before GPU allocation, including retained old-generation
images. Source, staging and residency have separate aggregate limits. Failed requests are
sticky until a new generation, and stop drains pending work off the rendering path.

Headless GPU fixture passes 57 checks, including 256 concurrent demands, reload with changed
source bytes, retired-generation lifetime, capacity/path/foreign-handle refusal, all three
budget failures, cancellation and injected submission failure. Instrumented driver seams verify
worker/caller separation. Ten actual textures pass both full compressed readback and the cache;
five sample-cache checks pass. Peak bytes: resident 283,377,664, staging 67,108,864, source
67,109,012. All core/sync validation is clean. Report: build/assets-gpu/report.json; log:
build/texture-cache-check.txt. Builder now fingerprints inputs and refuses malformed reports.

Layer source/build/next remain 0.9.2 and dist/0.9.1 remains untouched. This is standalone texture
preparation/ownership, not whole-mod rendering. Buffer resources, descriptor/material support,
shared character/Core and real frontend generation integration remain. No game action or asset
write. Updated docs/resource-assets.md, architecture, current state and tasks.

## 2026-10-12: Fragment image isolation, pixel proof and dimension audit

Added standalone FS image-binding relocation, retaining all nonbinding words and original
VS/sampler/buffer inputs. Initial actual-shader check refused Depth=Unknown declarations;
the specification and actual modules confirm this is not a depth-image assertion. Added
opaque-value tracing and explicit depth/escape refusals, retaining unrelated shadow sampling.

Headless asset suite now passes 79 checks with clean core/sync validation. GPU material test
uses the same original image in VS/FS: private FS turns red without changing VS displacement,
the next original draw stays blue, a zero 2D texture returns zero, and untouched pixels remain
green. Images survive their cache being retired before drawing. Production layer unchanged.

Historical validation found three mappings whose target image type is 3D. The first chat
description called this a mapping issue; checking both APIs refined that conclusion: original
pairings are matching 1x1x1 3D placeholders. Frame-a/frame-on custom DX11 draws 126/145/162 later
bind mod 2D views in those slots. Bindings alone do not establish sampled behavior. Kept the
slot correspondence, added refusal annotations and LIMIT-021 instead of silently converting
dimensions. The other 16 moves across seven shaders match Python and pass spirv-val; all nine
source modules validate. The verifier fingerprints evidence and audits both raw DX11 captures
through the read-only sibling decoder. Report: build/fragment-images/report.json.

Updated docs/fragment-materials.md, D-034, architecture, tasks/current state and known issues.
Still standalone: descriptors, pipeline/cache integration, full Core/shared character and
dimension semantics remain open. No game, DLL/config deployment or original asset write.

## 2026-10-12: DX11 dimension mismatch is not portable

Located all three historical PS binaries in the read-only EFMI output archives. Their zero-seed
FNV-1 hashes equal capture shader ids. Local disassembly confirms Texture3D declarations and
SampleLevel instructions in the affected slots. verify_fragment_images.py now records and
reproduces this evidence. Microsoft D3D11 section 22.3.12 requires a matching Texture3D resource.

Built standalone headless DX11 observation fixture and runner. Four controls (matching 2D/red,
3D/blue, null 2D and null 3D) pass on both NVIDIA hardware and WARP. Invalid 3D-shader/2D-view
samples at three Z coordinates and the reverse dimension mismatch yield RGBA zero on NVIDIA
and texture color on WARP. PSGetShaderResources retains the requested view on both. Green
clear distinguishes these results from dropped draws. Debug runtime unavailable on both;
recorded as unavailable, not passed. No feature installation, game or injected DLL.

Evidence: build/dx11-dimensions/report.json, with source/exe fingerprints and per-run logs.
Default Vulkan refusal remains; no universal dimension-coercion policy is inferred. Valid
explicit-null INI semantics need their own shader operation, not just a zero 1x1 image that
can behave differently under dimension queries or border sampling. Updated current state,
tasks, LIMIT-021 and docs/fragment-materials.md. No layer/config/deployment or asset write.

## 2026-10-12: General explicit-null image semantics

Added shared bounded fragment analysis and explicit-null requests for float32 sampled 2D/3D
images. Selected sample/fetch/size/mip results become typed zero, opaque accesses disappear,
and other resources remain intact. Unsupported opaque merges/escapes, QueryLod, arrays/MS and
sparse operations refuse atomically. No component identifiers or dimension-mismatch policy.

Headless asset suite passes 92 with zero core/sync diagnostics. Pixel controls use white
sampler borders and size/mip queries: a real or zero 1x1 image returns nonzero, while the null
rewrite returns zero and another blue image remains readable. Existing FS/VS isolation and
original-draw restoration controls still pass. Both synthetic source/output validate.
Historical adapter coverage: nine shaders, 19 mapped bindings pass spirv-val after nulling;
this is not a claim that the actual INI nulls all of them. Existing 16 relocation checks and
three expected 3D/2D dimension refusals pass. Reports: build/assets-gpu/report.json and
build/fragment-images/report.json; log build/fragment-null-check.txt.

Updated docs/fragment-materials.md, architecture, tasks and current state. No layer source,
DLL/deployment/config or asset changes. Continue production material descriptor/pipeline
ownership and shared character/Core offline. The user already confirmed voluntary game exit.

## 2026-10-12: Material variants and retained image snapshots in 0.9.3

Integrated ordered fragment image/null contracts into the shared worker-built pipeline
variant and cache. Keys include material shape, not image handles. New private layouts
retain VS bindings 0/1 and add only required FS images at 2+i. Descriptor budget checks
include original retained layouts. Invalid/missing/overlapping sources refuse explicitly.
DrawSnapshots preallocate image owner slots, validate every image before mutation and
retain owners through sealed recording/resubmission until legal reset or destruction.

Full build/regression passed in build/next: unit 88, tracker 144, bones 43, recipes 134,
cache 28, snapshots 34, variants 63, mesh 49, M2 hook 45, probes 517-532, modes 40,
limits 174, timing 126, validation 76, MCP 47, evidence 113. Log:
build/material-0.9.3-regression.txt. New GPU cases in no-layer/layer/inject and RenderPass
1/2 prove red/green per-draw images, null output, original blue restoration, original FS
buffers, rejection atomicity and retained image/pipeline owners over repeated submission.

This is shared backend integration and sandbox evidence. The existing component hook
requests no materials; the hosted XXMI frontend and TextureCache remain standalone.
Source/build/next are 0.9.3; dist stays 0.9.1. No game/deployment/launcher/config or installed
asset changes. Continue mesh-buffer assets, frontend resolution and shared character/Core
with existing captures; no new user game action is needed yet. Updated material docs,
architecture, D-034, current state and tasks.

## 2026-10-12: Bounded vertex/index asset uploads

Added BufferUpload, an immutable device-local byte ticket with explicit prepare, record,
caller submit and poll phases, actual allocation budgets and retained Ready ownership.
Supports vertex/index usages, exact source lengths, transfer visibility and atomic failure
publication. It does not infer stride/index format, parse INI, create texel/storage views
or perform queue handoffs independently of the application.

Headless asset suite passes 138 with clean core/sync validation. Exact 1/3/6/4101-byte
readbacks, source destruction, immutable/pre-fence state, cancellation, submit failure and
both allocation limits are covered. All 28 actual callback-selected BUF files match their
source fingerprints and full GPU readbacks. Material pixel tests now use uploaded VB/IB.
Historical fragment rewrite/null validation passes with the rebuilt fixture. Evidence:
build/assets-gpu/report.json, build/buffer-upload-check.txt, build/fragment-images/report.json.

Updated resource docs, architecture, current state and tasks. This is standalone asset
work; layer/build/next stay 0.9.3 and dist stays 0.9.1. No installed asset, game, deployment
or configuration change. Continue common buffer/image resource cache scheduling and
frontend/shared character integration offline.

## 2026-10-12: One asynchronous cache for all selected mesh and image assets

Generalized the existing texture scheduler into ResourceCache with typed image/buffer
owners and path-plus-usage keys. Kept TextureCache names as compatibility aliases. One
worker/outstanding upload and caller queue handoff share source/staging/resident budgets
across resource kinds and retained generations. No per-component file loading path.

Headless suite passes 149 with clean core/sync validation. Mixed-kind/usage identity,
128 concurrent mesh demands, deliberate wrong decoder, exact outcomes, changed reload
bytes, foreign handles, owner lifetime and intended worker/caller API execution pass.
The material pixel fixture now loads both mesh buffers and images through one cache,
retires it, then renders with retained resources. All 38 actual bound sample assets
(28 BUF and 10 DDS) also pass five cache/validation checks. Peak bytes: resident
315,327,232, staging 67,108,864, source 67,109,012. Final owner release zeros the ledgers.
Evidence: build/assets-gpu/report.json, sample-resources.txt and
build/resource-cache-check.txt. Historical fragment validation also passes.

Updated resource docs, architecture, tasks/current state. Source/build/next remain
0.9.3, dist remains 0.9.1; this common scheduler is standalone, with no deployment or
game/config/installed asset action. Continue hosted frontend resource/command resolution
and shared character/Core identity/completeness using saved data.

## 2026-10-12: Keep the single-character replacement ahead of memory optimization

The user questioned the measured 301 MiB resource footprint, then explicitly directed
work back to the plan before that discussion: get this character's complete replacement
working, and address performance later. An unverified residency/eviction draft was
saved only in build/drafts/residency-unverified.patch and removed from source. No build,
deployment, installed asset change or game action occurred for that draft. The tested
cache at 6dfce2f remains intact. Continue hosted command/resource resolution, shared
character/Core identity and bone completeness, using component-independent operations.

## 2026-10-12: Connect owned frontend file plans to resource preparation

Added a generation-owned snapshot of original XXMI file declarations and an owned
command/resource representation. It preserves strides, index formats, binding order,
explicit nulls, layouts, draw arguments and unresolved native phases. Unsupported
transformations, dynamic/Core resources and foreign declarations refuse atomically.
The cache handoff freezes metadata, deduplicates uploads and collects all Ready owners
without file/driver work on the request path. Pending/failure keeps prior output intact.

Verified: host 23, command planner 28, resource planner 19 and DDS 15; all 384 actual
callback plans match original declarations, with 404 draws/18 index ranges/38 files.
GPU resource suite 161 passes under clean core/sync validation. New checks cover real
VB/IB/image uploads from file plans, repeated references, pending/null/native states,
caller metadata changes, foreign generations, partial vertex elements and owner
retention. Historical fragment checks also pass. Initial host fixture namespace typo
was corrected before the passing build. Logs build/frontend-resource-{host,mod,gpu}.txt.

No layer change, deployment, game action or installed asset modification. Source and
build/next remain 0.9.3; dist remains 0.9.1. Continue resolving per-draw binding state and
native shared character/Core operations. GPU asset readiness is not Core execution,
runtime draw-range validation or real-character replacement acceptance.


## 2026-10-13: Resolve file bindings separately for every planned draw

Continued the user's original single-character plan after interruption. Added
per-draw vertex/index/material snapshots with Ready owners, copied layouts and
arguments, and explicit file/null/application states. Any unresolved native call
or late invalid file range rejects the complete transaction before command output.
Buffer uploads retain only typed whole-file index extrema for conservative bounds.
This can refuse valid slices; all 404 current sample draws pass.

Verification: headless asset suite 200, all 28 actual BUF full readbacks/extrema,
clean core/sync validation; material pixels consume resolved draw metadata after
cache retirement. Host 23/28/19/15, 384 actual plans and historical fragment checks
pass. Logs build/file-draw-bindings-{gpu,host,mod,fragments}.txt. Memory/docs synced.
This is standalone frontend/assets work, with no layer/deployment/game/config or
installed-asset change. Source/build/next remain 0.9.3 and dist 0.9.1. Continue
shared character/Core identity and bone completeness before the next game test.

## 2026-10-13: Execute original skeleton initialization and read its evaluated pools

Added bounded original CPU pool writes/reads, pool references and dynamic callback
aliases to command mediation. Declaration references snapshot metadata instead of
creating D3D resources. Guard direct indices against nonfinite/out-of-range casts;
reject cyclic/unsupported operations and expiring resource aliases. Variable writes
conservatively invalidate declaration aliases in a mixed pool. CPU effects remain
nontransactional while refused draw output stays unchanged.

The skeleton adapter copies evaluated original offsets/counts/remap references into
owned metadata and validates a complete partition, preserving sparse/empty component
ids. Original sample initialization gives 499 bones, 8 instances, 13 declared slots,
12 active components and 8 exact R16 remap files. LOD2/3 remain unassigned. Native GPU
initialization stays an explicit unresolved event, not a claimed successful no-op.

Host tests pass 23/28/19/15 plus Core pools 11 and skeleton metadata 10. The actual
initialization matches the independent earlier literal reference; all 384 callback
plans/404 draw ranges still pass. Reports build/xxmi-host/{report,core-skeleton-report,
mod-plan-report}.json; logs build/core-skeleton-{host,metadata,mod-plans}.txt.
No layer/deployment/game/config/installed-asset change. Continue original spatial
identification and shared skeleton completeness before whole-character emission.

## 2026-10-13: Run original spatial identification on CPU instance windows

Added explicit resource-expression mediation and a bounded SpatialHash reader
using pinned XXMI quantization. A transitive original command-graph compiler
includes runtime callbacks; the original Core spatial state machine and pools
execute unchanged. Native fixture tests two objects, first-frame delay, custom
classification, movement, insufficient evidence, duplicate callers and expiration.
Missing/truncated/nonfinite/reserved-sentinel windows refuse. Twelve cases pass.
A pointer/member typo caused an initial host build failure and was fixed; the
verified runner now checks executable/source hashes before this fixture.

User narrowed scope to LOD0/LOD1. Recorded that decision and restricted remap
resolution to those levels while retaining the original Core pool stride. No
later-LOD assets or rendering tests are required. Source/build/next stay 0.9.3,
dist stays 0.9.1. No layer/deployment/game/config or installed-asset action.
Next: retained palette identities/completeness, native component/material effects
and whole-character command emission through the common backend.

## 2026-10-13 - Shared palette masters and owned remaps

- Continued LOD0/LOD1 single-character priority. Added a bounded CPU palette bank and generation-prepared R16 remap loading from executed original initialization; no component-specific importer.
- Verified sparse ids, cross-component completeness, separate current/previous poses, LOD priority, update retention, invalid input atomicity, identity capacity/retirement and immutable snapshots (24 native checks). Host metadata/file checks now 15; host 23/planner 28/resources 19/DDS 15/pools 11 unchanged. Original spatial 12 and 384 sample plans pass.
- Historical replay: 283 records, 74 imports, 262 incomplete snapshot refusals, 21 complete byte-identical snapshots. Five isolated frames use separate identities, not assumed animation continuity. Eight native-loaded remaps match the independent source bytes. Evidence build/shared-palettes/report.json and build/xxmi-host/core-skeleton-report.json.
- Reviewed the new sources and changed host/tests. Runtime epoch adapter, native Core/material resolution and layer integration remain next. No layer source change or rebuild required; source/build/next 0.9.3 and dist 0.9.1 unchanged. No deployment, installed-asset write, launcher edit or game action.

## 2026-10-13 - Spatial identity epochs for shared palettes

- Added SpatialPalettes without changing pinned original pool code. It consumes original slot/hash outputs and uses original distance math to validate continuity; repeated slot numbers alone never grant access to old masters. Owner reset/failure obligations are explicit.
- Rebuilt the CPU host. Original Core/palette integration passes 21 checks, including real original FIFO slot replacement; native palette checks 24, original metadata 15, historical records 283 and callback plans 384 still pass. Reports build/xxmi-host/core-spatial-report.json, build/shared-palettes/report.json and build/xxmi-host/mod-plan-report.json.
- Reviewed new sources and fixture integration. No layer source or version change; source/build/next 0.9.3, dist 0.9.1 unchanged. Next is native Core component/material execution and retained draw data integration, then DLL linkage. No installed source changes, deployment or game action.

## 2026-10-13 - Native callback execution and palette ownership through plans

- Added explicit native handlers, bounded original callback invocation and per-draw immutable palette owners through file preparation/resource resolution. Unhandled boundaries still block execution; nested failure cannot be ignored into a successful partial plan.
- Review found that pre execution may retarget the post callback. Fixed late alias resolution and added a case that distinguishes it from a stale post target. Native bridge 12, host 23/planner 28/resources 19/DDS 15/pools 11/metadata 15, original spatial 21, palette 24 and all 283 historical records/384 callback plans pass. GPU assets pass 201 with clean validation; reports build/xxmi-host/report.json, build/assets-gpu/report.json and build/shared-palettes/report.json.
- This is bridge plumbing, not completed native EFMI component/material execution. Continue those operations and layer integration offline. No layer source or installed assets changed; source/build/next 0.9.3 and dist 0.9.1 unchanged. No deployment or game action.


## 2026-10-13 - Original component entry and shared skeleton bridge

- Connected actual original TextureOverride matching, pre/post roots and original Core component callbacks to the shared CPU palette bridge. Only LOD0/LOD1, one resolved rolling-buffer instance and a prepared generation are accepted. Original instance iteration remains intact.
- Initial fixture lookup missed preserved entry-section capitalization; corrected its case-insensitive comparison. A hand-written iteration boundary later exposed an incorrect post-phase iterator state and caused a host stack overflow. Removed that boundary, retained original iteration and added a separate native command-stack budget. Neither failed result was treated as verification.
- Final host 23/planner 28/resources 19/DDS 15/pools 11/metadata 15/native bridge 14 passes. Original spatial 21, shared palettes 24 and 283 historical records pass. Full entry fixture: 566 plans, 370 draws, 283 first-frame skips, 137 mesh-empty callbacks, five isolated identity epochs and four refusal/control cases. Independent draw/entry/palette byte comparison passes; 384 isolated callback plans remain passing.
- OverrideTextures stays unresolved (283 events), registration is an explicit fixture input and synthetic repeat updates are not animation evidence. Report build/core-components/report.json. Next: material/profile resolution and shared backend integration. No layer/DLL/deployment/game or installed-asset change; source/build/next 0.9.3, dist 0.9.1.


## 2026-10-13 - Shader-specific material plans and historical coverage

- Added generic exact-fragment profiles and ordered per-draw material contracts/Ready owners. Unknown slots, aliases, incompatible image types and late readiness errors refuse atomically; source-undeclared slots and explicit nulls stay distinct. No component ids in the adapter.
- Asset GPU suite passes 221 checks with zero validation diagnostics. The rendered private-image fixture uses the new material plan. Historical audit: 21 fragment modules, 370 draws, 353 accepted contracts, six unknown LOD1 mapping refusals and eleven known 2D-for-3D refusals. Combined accepted shader patches validate. Existing fragment checks remain 16 moves and 19 null targets passing.
- Existing captures cannot identify source DX11 slots for LOD1 fragments 0aeef59b, 310c25c3 and a6d7088a. Did not propagate source slots solely from shared Vulkan image identity: slot numbers differ by source shader. Reports build/material-plans/report.json and build/assets-gpu/report.json.
- Continue vertex declarations and remaining frontend/Core/backend integration offline. No layer source/version change, deployment, game action or installed-asset write. Source/build/next 0.9.3 and dist 0.9.1 unchanged.


## 2026-10-13 - Original vertex layout rules through general stream contracts

- Wrapped the pinned original command translation unit to expose its CPU layout evaluator. Added exact-VS semantic profiles and per-draw declarations using original ordered layout rules plus shared stream patches. File strides and element formats are data, including index-only four-byte source streams.
- Native vertex tests pass 18; full original entry replay now checks all 370 vertex declarations alongside arguments, entries and bones. All 17 drawn VS modules have prior exact paired semantics. Existing host, spatial 21, palette 24/283 and callback 384 regressions pass. Reports build/xxmi-host/report.json and build/core-components/report.json.
- An added build fingerprint initially named a nonexistent util.h; the builder stopped before compilation and was corrected to actual headers. Final build/replays pass. Source declarations and installed assets remain unchanged.
- Continue native texture queries and unified resource/variant/snapshot handoff. This remains CPU frontend evidence; no actual game pipeline creation, layer source/version change, deployment or game action. Source/build/next 0.9.3, dist 0.9.1.

## 2026-10-13 - Combined retained frontend GPU transactions

- Joined Ready file draws, resolved vertex/material contracts and compact palette owners to existing asynchronous variants, snapshots and ordered emission. Late preparation/refusal is atomic; inherited advancing application streams refuse without new-index bounds proof. Snapshot append failure may consume unused arena slots but emits nothing.
- Standalone GPU asset suite passes 253 checks with clean core/sync validation. Pixel controls prove separate materials, first/base offsets, 16-bit bone indices, original FS isolation and full original-state restoration. The recording alone retains all replacement owners after producer retirement, survives sealed resubmission, and releases them after completion.
- Historical material audit remains 353 accepted, six unknown LOD1 correspondences and eleven dimension refusals over 370 draws. Text checks pass; no layer source/version change or full-suite rebuild is required for this standalone addition. Source/build/next 0.9.3 and dist 0.9.1 unchanged.
- Next: original texture-query/override mediation and runtime/profile integration. No DLL deployment, game action, launcher/configuration change or installed-asset write.

## 2026-10-13 - Original texture matching and callback mediation

- Added an explicit source-identity provider, per-plan pixel binding state, CheckTextureOverride and filter mediation. Reused original hash/draw-context matching, inclusive range resolution and callback execution. Unknown identities, fuzzy rules, missing this, unadapted callbacks and recursive command graphs refuse atomically; nested this/phase and original filter semantics are preserved.
- The initial fixture wrongly expected plain queries to run both phases at once and assumed a half-open range. Original source shows explicit pre/post selects combined execution and ranges include the endpoint; corrected fixture expectations and used those original paths. Range expression guards were also extended so resource queries in bounds cannot reach D3D scaffolding.
- Final native texture checks 21, original installed Core API synthetic-identity checks 22, spatial 21, shared palette 24/283 and full entry replay 566 plans/370 draws pass. No real identity provider is substituted into historical data; 283 unresolved material calls remain explicit. Host executable cf744cdd2673c2fd54ab809aef135ad9b9455f0590b8e0ff9523d87d7a0cf521.
- Continue source profile/runtime integration offline where evidence permits. No layer/DLL version change, game/deployment/configuration action or installed-asset modification. Source/build/next 0.9.3, dist 0.9.1; only LOD0/LOD1 are in scope.

## 2026-10-13 - Real saved pipeline recipes on the standalone GPU driver

- Added a reproducible generated-case runner joining native full-Core vertex output, accepted material contracts and saved pipeline/layout/shader recipes. Exact metadata roundtrips precede driver calls; reports retain source fingerprints, case/draw mappings and omissions. Generated/game data is not committed.
- First build missed an asset header include path. First GPU pass exposed a test-only missing derived blend attachment count (62 validation errors while driver calls returned success); production capture retained it. Fixed the reconstruction and added a pre-driver count check.
- Final result: 88 created variants covering 353 historical draws, 177 checks, zero validation errors/warnings on RTX 5060 Laptop. Six unknown LOD1 mappings and eleven dimension refusals remain. Asset 253 and full original CPU/material replay pass. The child enables supported independent blending; no game features/settings changed.
- This closes historical driver construction, not character rendering or DLL integration. Source/build/next 0.9.3 and dist 0.9.1 unchanged. Remaining source identity/correspondence and runtime integration are recorded; no deployment or game action.

## 2026-10-13 - Prepare the next source capture without entering the game

- Extended the DX11 capture helper with optional original shader archiving (bytes, SHA-256 index and explicit partial errors) and file-backed texture hashing for inactive bound slots. Existing default capture behavior remains. Archives stay in this project; differing bytes under an existing shader hash name refuse overwrite.
- Eleven offline fake-probe checks pass for disassembly failure, existing declarations, collision/path refusal, default/expanded texture selection and unreadable content. CLI help and English-text checks pass. No live probe, installed resource, launcher/configuration or game process was used.
- Documented the exact next source-data target: actual LOD1 DX11 for three unmapped fragment modules, using the two saved Vulkan LOD1 bundles first. This requires a user-initiated test; no 30-minute run is needed. LIMIT-021 and runtime/source identity/DLL implementation remain distinct unfinished work (LIMIT-022). Existing dist 0.9.1 is unchanged, and whole-character visual acceptance is not claimed.

## 2026-10-13, 12:00: Review of 4b398f8 to a3a7905 (0.9.2, 0.9.3, standalone frontend), implementation and design

Requested by the user, who also asked for a judgement of the design. 28 commits, about 11,000 lines. The game was not running; dist/ still holds the 0.9.1 deployed on 2026-10-12 (hashes equal); the launcher file holds the Vulkan values, its other differences from the last backup are the launcher's own counters. No source change in this review.

Rebuilt and rerun independently, all with the documented counts: full layer suite of HEAD in `build/review-0.9.3` (0.9.3); `scripts/probe_xxmi_host.py` (23, 28, 15, 19, 11, 15, 14, 18, 21); `check_core_components.py` (566 plans, 370 draws, entries, palettes and vertex declarations equal); `check_shared_palettes.py` (74 imports, 21 snapshots); `check_mod_draw_plans.py` (384 plans, 404 draws); `check_material_plans.py` (353 accepted, 6 unknown, 11 dimension); `check_historical_variants.py` (88 pipelines created on the driver, 177 checks, validation clean); `check_image_upload.py` (253, validation clean). The figures of the live 0.9.1 session in `docs/m2-first-replacement.md` equal the saved status files.

Read in full: the layer changes (variant builder with fragment patch, cache key, snapshot images, stream patcher), `fragment_images`, `palette_bank`, `spatial_palettes`, `component_bridge`, `draw_transaction`, `material_plan`, `vertex_plan`, `dds`; the interfaces and structure of `draw_plan`, `resource_cache`, `image_upload`. The corrected bone offset of 0.9.1 carries through: the native apply step patches through `MergedSkeleton::patch_entry`. No defect found.

Design: the build-out is the plan's architecture (hosted original engine, native built-ins only where the core touches the GPU, data-only plans, shared backend without component branches, refusal keeps the original draw) and is judged sound; using the original layout evaluator and spatial identification instead of rewriting them is the right call after BUG-005. Seven points are recorded under T-027, the new LIMIT-023 among them; the first is that 27 offline commits now rest on a rendering path nobody has seen draw a visibly changed mesh in the game.

## 2026-10-13, 12:04: Prepare the visible replacement check

The user asked for the first point of the review. The game was already running (PID 31340, started by the user at 11:57 under Vulkan, 0.9.1 with the unaltered package), so the check needs one restart: the package is read at device creation.

Looked at how component 7 is drawn before choosing the change (saved 0.9.1 frame, pipeline 368): a multiplicative, stencil-masked decal without depth write. A patch that leaves the face would be cut by the stencil, so the change keeps it on the face and makes it large: one strip scaled 3.5 times about its centroid and moved 1 cm forward, the other left alone for comparison in the same frame. Added `scripts/package_visible_check.py`; the result `build/m2-component7-visible.bin` passes the decoder of the deployed 0.9.1. Switched `dist/efmi_vk.cfg` to it after a backup. Layer binaries untouched; no sandbox run while the game is up; no launcher edit; the agent neither stopped nor started the game.

12:08: the user had closed the game; the smoke with the deployed settings and the new package passes (the script gained `--version` for binaries older than the source). The user asked whether everything could be replaced instead. Measured the sample's structure from its files to answer: six components are deleted by the mod, four are self-contained copies of the original meshes, and the costume is on components 1 and 2, which need bones of other components (figures under T-027). So the whole character needs the shared skeleton and the frontend in the DLL; that is development, not a setting.

## 2026-10-13, 12:55: Layer 0.10.0, the whole character from a static package; deployed

The user answered the two-way choice with "do B directly": whole character, built by this agent, meshes and bones first. Codex processes were running on the machine; no commit of theirs arrived during the work.

Measured before designing: the mod deletes six components, four are self-contained copies, the costume is on components 1 and 2 with bones of other components. Read the core's INI for the parts a static package cannot carry (spatial identification, the per-instance draw loop, the first-frame skip, the import bookkeeping) and checked the GPU's memory types: type 4 is device memory the host can map, the kind the game's pools use, so the 32 MB of meshes need no upload queue.

Built: `scripts/package_character.py` (draw lists from the hosted frontend's plans, skeleton from the evaluated core initialization, entry sections and the one layout edit from the INI); `layer/src/character_package` and `character_backend`; shared skeleton copies in `DrawSnapshots`; `needs_import` in `MergedSkeleton`; a pipe option to end a frame for tests that never present. Tests: native decoder (37), GPU hook scene with three components and a neighbour (49 over four runs), shared slices (13). First full run of the new GPU test passed; hardened afterwards for the game (staggered re-hash, an object already met in a frame matches only at the same position, eviction of the weakest unidentified identity, 16 identities, 64 skeleton copies per arena) and reran everything.

Offline against six saved frames: sections, vertex declarations (equal to the semantic reference for all 426 items), a model of the decision rules, vertex slot and layout facts. The texcoord strides of the mod's files equal the game's per component and LOD, so the only declaration change is the widened blend index; no per-shader profile is needed for this mod.

Deployed 12:50 after the full suite in `build/next`; suite on `dist/` and the smoke with the real package pass. No launcher edit, no game start. Not seen in the game yet.

## 2026-10-13, 13:35: The whole character is on screen; flicker at LOD 0 traced to set 3; 0.9.5

The user started the game with build 0.10.0, reported "the model is roughly fine, but LOD 0 flickers now and then" and sent a screenshot showing the mod's outfit on the character in the open world. First real-game result for the whole-character backend.

The agent first believed the game had exited (a process check cut after four lines, see CURRENT_STATE) and analysed saved frames instead: the suspicion that LOD 1 imports of the shadow pass disturb the color passes did not hold, the mismatch exists only in the two shadow passes (LIMIT-025). The user pointed out that the game was still running. The live counters then answered at once: `refused_set3` 82 in 378 frames and nothing else. The per-reason counters of 0.10.0 are what made this a one-read diagnosis.

Fix as D-036 and BUG-006. Test first: the scene with a four-set pipeline before and after the character fails against the deployed build with the same counter, passes with the fix. Renumbered to 0.9.5 when the user set the version rule. Full suite and smoke pass in `build/next`. The user then closed the game; deployed at 13:28 with a backup, hashes compared, smoke on dist/ passes. No launcher edit, no game start or stop by the agent.

## 2026-10-13, 14:28: Switch to DX11 for the user's LOD1 source capture

- Restored current memory: the static whole-character backend and 0.9.5 are now deployed, superseding this chat's earlier component-7-only state. The user explicitly requested the DX11 switch and will start the game, disable the mod and report LOD1 readiness.
- Complete process checks found no game or launcher, including an immediate pre-write check. Backed up the active launcher JSON and dist/efmi_vk.cfg to build/backups/launcher-dx11-20261013-142837/, with hashes and change.json evidence.
- Changed only the launcher's EFMI d3d11_mode_cmd_args from empty to -force-d3d11 and xxmi_dll_inject_mode from SKIP to DIRECT. The existing signed extra libraries already include the DX11 probe; both DLL paths exist. Signed values and all other launcher content are unchanged, verified by whole-JSON comparison and exact-byte readback.
- Changed only the project-owned layer mode from probe to passthrough for this DX11 session. No binary, version, mod/Core, game asset or system setting changed. No game launch, probe connection or capture occurred; no new layer regression is claimed for this configuration-only change.
- Next: capture actual LOD1 after the user's ready message with shader archiving and all-bound static texture hashes, targeting the three missing material correspondences. Keep the later 0.9.5 Vulkan flicker acceptance separate. No LOD2+ or long-duration test.


## 2026-10-13: Original LOD1 source capture and verified material profiles

- User entered DX11 and positioned actual LOD1. Initial capture still had mod meshes; retained as initial-mod-on. At the user's explicit request, probe F11 disabled the mod and restored focus. Frame 35553 has 954 game calls and two overlay calls, no XXMI indexed mesh draws. Archived/verified 212 DXBC shaders and hashed 247 static textures, no failures. Capture released; no installed file write.
- Both saved Vulkan LOD1 bundles reproduce four original-mesh pass sequences, exact target vertex declarations and eight rules for the three missing fragments. Two placed images use unique shape/format, not content. Captured source DXBC proves six undeclared slots. Evidence manifest fingerprints 452 files.
- Added the offline raw-evidence audit and per-fragment manifest resolver; updated material checks/table. Audit and SPIR-V checks pass; native replay 359 accepted/11 dimension refusals/zero unknown out of 370. Asset suite 253 and rebuilt hosted native suites pass. No layer change/deployment or new Vulkan rendering claim.
- Latest user asks to finish the next work after the whole-model replacement. Next: integrate static character textures, explicitly handle the device-specific dimension case, full sandbox regression, then user-started Vulkan acceptance. The general hosted runtime remains separate. See docs/lod1-material-capture.md.

## 2026-10-13, 15:47: Static whole-character materials, 0.9.6 deployment and Vulkan preparation

- Continued the requested whole-character work on Claude's existing mesh/bone backend. Added a bounded static material envelope from hosted per-draw pixel bindings, exact fragment profiles and original DDS paths. Shared ResourceCache, native material contracts, variants and command-owned image snapshots now run inside the layer. Whole-component refusal preserves originals; shared bones and application state restoration remain intact. No per-component material branches, later LODs, keys or full hosted runtime were added.
- Made LIMIT-021's three dimensional mismatches an explicit, device-bound package policy from the existing controlled DX11 hardware evidence. Default material selection remains strict. Historical native production contracts cover all 370 draws, eleven using the named policy; 105 saved pipeline combinations create, 211 checks, zero validation diagnostics. Actual game appearance remains unknown.
- The initial layer-entry material cases crashed in the sandbox because upload command buffers lacked loader dispatch initialization. Added the loader callback to internal allocation/setup; inject and layer cases both pass. Final full rebuilt suite passes, including native character 67, snapshots 52, character-hook 127 and all existing steps. Rebuilt assets 253 and pinned host suites also pass. No game crash or load occurred in this round.
- New package build/character/arcane-materials.bin has 20 profiles and nine DDS files alongside the unchanged mesh/bone payload. Candidate actual-config smoke through both entries uploads each file once, nine Ready, zero failures. Resident image allocation is 216,268,800 bytes in the sandbox; no game VRAM/performance claim.
- User answered the pending Vulkan switch question with an explicit request to switch back. After three stopped-process checks, backed up existing files and launcher JSON to build/backups/before-0.9.6-20261013-154745/. Deployed thirteen verified files, exact candidate-byte comparison passes. Both DLLs SHA-256 21b9be301fb8c6b80a268e4434c352a3a5d9e40e78b21a07f3609052e37dba83.
- Changed only the launcher's EFMI d3d11_mode_cmd_args to empty and xxmi_dll_inject_mode to SKIP. Whole-JSON comparison and byte readback preserve every other value and byte, including signed settings. Project config now selects probe/resources, hashing/profiling off, enabled replacement and arcane-materials.bin. Dist actual-config smoke passes through both entries; historical game log unchanged. Manifest build/deployment-0.9.6.json. Full regression ran on identical candidate bytes, not again on dist.
- Next requires the user-started game: verify loaded version, image readiness, material/refusal/set-restoration counters and whole-outfit appearance/motion at LOD0/LOD1. Include the earlier flicker fix; neither sandbox results nor old game evidence closes this acceptance. No game start/stop, installed mod write or system setting change by the agent. Reviewed source diff and synchronized memory for a local commit.

## 2026-10-13: Original model in 0.9.6 traced to queue setup; verified 0.9.7 fix

- User enters Vulkan and reports the original model. Read current PID 18868 status and log: correct 0.9.6/inject/probe load, then package initialization refuses the one-graphics-queue condition. Backend disabled/unready, zero replacements, zero image requests. Saved read-only evidence in captures/materials-0.9.6-20261013/. No game frame capture or installed file change was needed.
- Added support for multiple enabled queues in one unprotected graphics family. First active queue uploads; once all images are Ready, per-queue semaphore signals/waits publish them before any recorded replacement can execute. No draw/submit fence wait or background submission to an application queue. Queue inventory, selected uploader and publication are observable. Version 0.9.7; same mesh/material package.
- Full rebuilt regression passes (build/material-0.9.7-regression.txt): character-hook 261 checks across eighteen cases, all existing steps unchanged/passing. Added queue 1 only with unused queue 0 and separate upload/render queues, both entries and all three submit APIs. Material pixels, state restoration and resubmission pass under clean core/sync validation.
- New two-queue fixture against both deployed 0.9.6 entries reproduces exactly the game's refusal with zero textures/replacements and clean validation (build/material-queues-0.9.6-control.json). The first control runner expected the final snapshot although the fixture intentionally exits early on refusal; corrected the runner to read its saved active snapshot. This was a control-script path error, not an additional layer defect.
- Actual candidate configuration with nine real DDS files passes through both entries using two enabled queues with only queue 1 active. Nine reads/submissions/Ready, publication true, one queue wait, zero failures, 216,268,800 resident image bytes. Saved build/character-assets-config-next-0.9.7.json. Candidate DLLs SHA-256 3ff4517d29ac23cdd877b45f34f777f2146391488955d91f6eb181b40e02995d.
- User has been asked to exit normally so the DLL can be replaced. At this checkpoint dist and PID 18868 still use 0.9.6; no launcher/system/game/mod changes. Review and memory recorded for a local commit. Next: backup/deploy after exit, actual-config smoke on dist, then a user-started game check. Visual materials and the included flicker fix are still pending.

Deployment follow-up, 16:23: the user confirms normal exit, and repeated complete process checks find neither game nor launcher. Backed up to build/backups/before-0.9.7-20261013-162318/, deployed thirteen files and compared their hashes against the fully tested candidate. Actual-config smoke on dist passes through both entries with two enabled graphics queues and only queue 1 active, nine Ready images and completed publication. Report build/character-assets-config-dist-0.9.7.json; manifest build/deployment-0.9.7.json. The same DLL hash above is deployed; game log and launcher bytes are unchanged. No agent game start or launcher edit. The next step requires the user's new game session, not further offline test repetition.

## 2026-10-13, 17:20: Flicker persists with 0.9.7; traced to the visibility pass; 0.9.8 built, not deployed

- User: Codex added the textures and they seem right, but it still flickers. The game runs (PID 7784, 0.9.7, user-started); everything on it was read-only (status, forty frame captures, pipeline and shader requests).
- Counters: `refused_material` 85 in 393 frames, no other refusal. Captures: all refused character draws on four-set pipelines 399/402/404/406 in render pass 12, depth EQUAL without write, no color, fragment-written storage buffer, no images. These are the draws counted as `refused_set3` under 0.10.0; the 0.9.5 diagnosis was wrong (BUG-006 corrected, BUG-009 and ISSUE-008 added).
- 0.9.8: private set index from the layout (D-039), `Device::max_layout_sets`, restoration at the private slot, no material profile needed for a layout without fragment-stage images. Files: layer/src/pipeline_variant.cpp, character_backend.cpp, resources.cpp, state.h, common.h; sandbox/character_gpu_case.h (character draw on the four-set pipeline).
- Verification, sandbox and offline: new phase green with 0.9.8 and red against the deployed 0.9.7 (`refused_variant`); the four real vertex shaders rewrite to set 4 and pass spirv-val; full suite passes in build/next (run_all-0.9.8.out); actual settings and nine real DDS files through both entries, nine ready (build/character-assets-config-next-0.9.8.json). The first smoke used `scripts/smoke_m2_config.py` and showed five failed images: its sandbox device has no BC compression, the layer is not at fault. The running game has 1,157 three-set, 190 four-set and no larger pipelines, so slot 4 is free there.
- Not done: deployment (the game is running) and any game check of 0.9.8. Remaining: deploy after the user closes the game, asset smoke on dist, user-started session reading the refusal counters and the flicker.

Deployment follow-up, 17:59: the user reports the exit; process checks by full image list find neither game nor launcher. Backup build/backups/before-0.9.8-20261013-175909/, thirteen files copied from build/next with equal hashes, manifest build/deployment-0.9.8.json. Real-texture smoke on dist (actual settings, two graphics queues, queue 1 only) passes through both entries, nine images ready. No launcher edit, no system setting, no agent-started game. Remaining: the user's game session with 0.9.8.

## 2026-10-13: Explained the deployed workflow and XXMI participation

- User asked for the complete current replacement flow and how much XXMI participates. Checked current source/config (0.9.8), scripts/build.py, scripts/probe_xxmi_host.py, scripts/package_character.py, character_materials.py, the native character backend and the relevant backend/host documentation.
- The original pinned XXMI C++ frontend actually executes selected callback logic and Core initialization offline. Static packaging consumes one scenario's draw/resource plans and bone metadata; entry matching and the supported layout edit are also read by the packager. Mesh/remap bytes are embedded, DDS files remain referenced. Verified shader-slot correspondences supplement the plans.
- At game runtime the launcher injects the Vulkan DLL; the project's native backend matches draws, tracks identity/LOD, merges bones, prepares resources/variants, emits replacement or hide operations and restores game bindings. The DLL build includes shared asset/GPU helpers but no XXMI interpreter. Runtime Core identification follows native rules derived from Core; it does not execute the original Core INI on each draw. Dynamic source-texture queries, keys/conditions/persist/reload and full hosted integration remain open.
- Clarified current versus planned support rather than assigning an unsupported percentage. Updated the stale TASKS current-state paragraph to the already documented 0.9.8 deployment. Memory-only change; text/diff checks apply, no build, deployment, launcher edit or new game observation.

## 2026-10-13, 19:00: 0.9.8 in the game; shader variants and identity; 0.9.9 built, not deployed

- Deployed 0.9.8 at 17:59 (previous entry). The user started the game and reported: normal state apparently fine, dash flickers, looking up from below badly wrong (screenshot), later the normal state flickers too. Everything on PID 36720 was read-only: status, 30 frame captures, pipeline and shader requests, 30 captures with instance entries.
- Found: the dithered fade uses fragment shaders the package has no profile for, in every pass; 0.9.8 kept the game's mesh there. Each is a profiled shader plus one noise image with shifted bindings. Standing still nothing is refused.
- 0.9.9: layer/src/material_sibling.h/.cpp (image variables of a fragment module, rule transfer), character_materials (one profile at a time), character_backend (signatures of profiled shaders, derivation, fallback to the application's images, variant cache 2,048, identity expiry by frames and time, radius by gap, stale eviction, counters, log), common.h. Tests: sandbox/character_test.cpp (sibling checks with the game's bound images; --derived mode), character_gpu_case.h (identity gap, four-set draw in material runs, shifted-binding shader), variant_fixture.h, character_test.py. Scripts: flicker_capture.py, flicker_shaders.py, sibling_replay.py, sibling_check.py.
- Verification, sandbox and offline: 25 of 25 dither variants derive one stable rule set from the captured frames and pass the production preparation with plans equal to the sibling's; native 77; scene 277 over eighteen runs with clean validation; red against the deployed 0.9.8 for all three new phases; full suite in build/next (run_all-0.9.9.out).
- Not done: deployment (the game is running), any game check of 0.9.9, the after-images (LIMIT-027), the cause of the normal-state flicker (ISSUE-009).

Deployment follow-up, 19:00: the user reports the exit; process checks by full image list find neither game nor launcher. The 0.9.8 session log is saved with the captures. Backup build/backups/before-0.9.9-20261013-190005/, thirteen files copied from build/next with equal hashes, manifest build/deployment-0.9.9.json. Real-texture smoke on dist (actual settings, two graphics queues, queue 1 only) passes through both entries, nine images ready. No launcher edit, no system setting, no agent-started game. Remaining: the user's game session with 0.9.9.

## 2026-10-13, 19:20: 0.9.9 in the game; the frame boundary; 1.0.0 built, not deployed

- Deployed 0.9.9 at 19:00 (previous entry). The user: looking up is right, the normal state still flickers; and asked for deployment without leaving the game with the agent refreshing it (T-029, not started, needs a decision on the loading rule).
- Read-only on PID 32004: no refusals; derived rules in use; the identity log shows 16 of 19 new identities within the same counted frame as the replaced object's last draw, a few cells away. Cause: frames counted at the present, read at record time (BUG-011, D-041).
- 1.0.0: `CmdBuf::character_frame` (state.h, state.cpp), character_backend.cpp (two clocks; identify takes both), common.h; sandbox/character_gpu_case.h (two recordings without a present, the character a cell further). Red against the deployed 0.9.9, green with 1.0.0; character tests 77 and 277; full suite in build/next (run_all-1.0.0.out).
- Not done: deployment (the game is running), any game check of 1.0.0, T-029, LIMIT-027.

Deployment follow-up, 19:26: the game was closed when checked (full image list: neither game nor launcher). 0.9.9 session log saved with the captures. Backup build/backups/before-1.0.0-20261013-192546/, thirteen files copied from build/next with equal hashes, manifest build/deployment-1.0.0.json, real-texture smoke on dist passes through both entries. No launcher edit, no system setting, no agent-started game. The user decided T-029's rule: the replacement logic becomes a module, and swapping it into the running game is the agent's choice (AGENTS.md updated). Remaining: the user's game session with 1.0.0; T-029.

## 2026-10-13, 20:25: Host and module; exchange of the replacement code (1.0.1 built, not deployed)

- User: write the replacement logic as its own module; whether to hot-swap is the agent's choice. Rule added to AGENTS.md (commit b97368c). 1.0.0 was deployed at 19:25 with the game closed (previous entry).
- New: layer/src/character_module.h, command_owner.h, layer/module/module_main.cpp, scripts/hot_swap.py, docs/hot-swap.md. Changed: state.h (CommandOwner in CmdBuf; backend slot, lock, frame counter and properties in Device), m2_backend.h/.cpp (dispatch through the function table under the shared lock, staging, exchange, probe command), character_backend.cpp/.h (own command type, usable/ready, function table), draw_snapshots.h, layer.cpp (submit hooks), ipc.cpp (`module`), build.py (stamp, module target), probe_client.py (swap_module), sandbox scene and tests.
- Found on the way: the stamp must not hash its own compiler flag (layer and module got different stamps and the good module was refused); a command buffer keeps its owner across recordings, so an owner of the old backend has to give way once reset (otherwise the new backend never replaces in that command buffer again); totals of the scene are counted before the exchange because every backend starts its counters at zero; module copies need a serial in the name within one second.
- Verification, sandbox only: character tests 77 and 357; scripts/hot_swap.py twice against the windowed sandbox with the real mesh package, clean exit; full suite in build/next (run_all-1.0.1.out).
- Not done: deployment of 1.0.1, any exchange in the game, the user's check of the 1.0.0 flicker fix.

Deployment follow-up, 20:25: the game was closed when checked. Backup build/backups/before-1.0.1-20261013-202328/, fifteen files copied from build/next with equal hashes, manifest build/deployment-1.0.1.json, real-texture smoke on dist passes through both entries. The layer log had not changed since the 1.0.0 deployment, so 1.0.0 never ran in the game. No launcher edit, no system setting, no agent-started game, no exchange in the game yet. Remaining: the user's session with 1.0.1 (flicker in the normal state; first exchange).

## 2026-10-13: Review of intervening work and the user's correct-looking model/texture report

- User asks to restore memory, understand the intervening work and recommend what comes next; reports that models and textures now seem fine. Reviewed memory, the four implementation commits from 0.9.8 through 1.0.1, current module documentation and the original milestone plan. No implementation or deployment was requested or performed in this review.
- The latest game log now proves 1.0.1/inject/probe loaded in PID 27816 at 20:24:31, with the package configured and mesh uploaded. Process enumeration and probe discovery are empty at review time, so no live status, capture or exchange was attempted. No exit cause is established. Archived the 93-line latest session and its fingerprint in captures/user-review-1.0.1-20261013/.
- Retained the tentative user visual report separately from coverage not specified (movement, combat, LOD and effects). The log still has 50 nearby identity creations, 38 within zero counted frames; these are not correlated with actions or pixels and do not establish visible flicker. No module events or hot-swap ledger exist. Updated stale task summaries and preserved the open real-game acceptance items.
- Recommended next: short scoped LOD0/LOD1 movement/effect verification plus the first unchanged-code exchange, then any observed identity/after-image fault, then live original INI/Core integration with keys, persist and reload for this character. LOD2+, a duration-only test and multi-mod performance work stay outside the current scope. Documentation-only update; text and diff checks apply, no new layer regression claimed.

## 2026-10-13: Live 1.0.1 stability, first game module exchange, and requested dash check

- User starts the game and reports no apparent flicker; PID 19452 loads 1.0.1/inject/probe. Read-only LOD0 baseline: 21.031 seconds, 1,534 drawing frames, all eight sample points foreground. Matched 94,087 = replaced 49,375 + hidden 44,712, every refusal/identification/pending delta zero, nine Ready images. Six LOD1 frames then capture 58-60 character matches each and 39 supported pipelines; across 101 drawing frames all 5,896 matches are replaced or intentionally hidden with no error delta.
- Outside these clean intervals, clustered identity events and variant refusals still occur. The user confirms dashing. The retained reason is the historical missing vertex bone-buffer condition, but no draw-level correlation exists for those events; no new cause or visible flicker was claimed.
- Exercised the prior standing authorization for the first real module exchange. Verified both existing module files against the 1.0.1 deployment hash and clean implementation sources, then scripts/hot_swap.py --pid 19452 --out dist --no-build --timeout 35. Same code, version and package; no source or deployed-file change. At 20:49:41 the module takes over in 0.8 seconds, generation 1, nine Ready images, no module error. The user confirms normal appearance without a persistent anomaly. Post-exchange counters over 10.016 seconds/710 LOD0 drawing frames remain clean. The ledger records module hash/path, source commit and outcome; counters were compared only within each generation.
- User then reports that dashing also seems fine and asks for detection. Requested a short foreground dash interval and captured 58 frames in 24.156 seconds: 3,766 character matches, 98 supported pipelines, no historical after-image vertex shader. Over 964 drawing frames, refusal, pending and identification deltas stay zero, with 5,040 sibling-derived and 1,235 inherited material replacements. This supports the sampled dash; it does not fix or exercise LIMIT-027's historical no-bone path. Corrected the earlier wording that implied residual after-images were necessarily a current visible fault.
- Evidence: captures/stability-1.0.1-20261013-204259-19452/ with raw statuses, captured original draws, pipelines, module ledger copy and fingerprints. Capture records do not annotate replacement pixels. No source change, rebuild, new regression run, launcher edit or process start/stop; only the authorized unchanged-code module exchange affects the running game. T-029's first functional game exchange is verified. Next is T-027 live INI/Core functionality for this character, with the historical after-image path retained as an uncovered follow-up. Memory/docs updated and checked for a local commit.

## 2026-10-13, 22:33: 1.0.2 original main INI, keys, persistence and F10; HLSL panel compilation

- User requests the next runtime stage: original INI conditions, outfit keys, saved settings and F10. During development asks whether Vulkan can support the panel's HLSL. Continued the runtime task and verified that actual HLSL independently, without editing the installed mod/Core or launcher.
- Linked the pinned original XXMI parser/evaluator/matcher/key dispatcher into the layer and backend module. Added isolated frontend generations, native character Core boundaries, reachable resource catalogue, dynamic draw/material plans, foreground input sampling, atomic project-owned persistence, and asynchronous F10 parsing/upload/adoption. Original evaluated bone partitions and LOD maps must match the native profile. Old sealed commands retain immutable owners and a unique generation token. Latest persistent values survive adoption; old settings writers are suspended and held keys do not retrigger.
- Scope: this character's main arcane.ini and LOD0/LOD1. Thirteen listeners on ten physical key combinations, alternate stocking DDS, original branches and existing profiled BUF refresh. Full source-image override identity, arbitrary CustomShaders/includes/controllers and the auxiliary image panel remain unsupported. Native Core texture boundary accepts only the profiled mesh registry; it does not simulate unknown image hashes. See D-043 and docs/live-ini.md.
- Verification: original runtime 45 checks, including all 21 actual sections and all key listeners; real Vulkan runtime runner 28 checks across both entries and one/two graphics queues (pixels, malformed INI, bad DDS, valid reload, persistence, held F10, retained old commands and restoration). Full sandbox/run_all.py passes in build/next/run_all-1.0.2.out; core/sync validation clean. Corrected two test harness errors during development: the layer helper overwrote the validation search path, and the green pixel expectation needed the first component's original fragment-buffer multiplier. Neither was a runtime defect.
- HLSL answer with evidence: official checksum-verified DXC 1.9.2609 installed only in F:/vulkan/dxc-1.9.2609. Unchanged k/Draw_2D.hlsl compiles in both stages and validates for Vulkan 1.1, with slots 100/101/120 retained; build/panel-hlsl/report.json. No panel rendering claimed. It still needs image/IniParams upload, cursor/window queries, pipeline state and safe backbuffer drawing.
- Deployment: game and sandbox absent at two checks, no exit cause inferred. Prepared build/character/arcane-live.bin plus sidecar; settings destination build/character/settings/arcane_fes.ini. Backup build/backups/before-1.0.2-20261013-223323/. Fifteen deployed files hash-match the verified candidate, manifest build/deployment-1.0.2.json. Candidate and dist actual-config smokes each pass through layer/inject with ten Ready images on two enabled queues, queue 1 submitting; clean validation. ABI 16b90a85431f86b4 stays compatible. No game load, module swap in the game or game pixels tested in this round.
- Next: user-started game, real-character outfit/stocking keys and F10, persistent choice after F10 and later normal restart. No duration-only test or new LOD capture required. T-016 panel drawing remains separate from the successful HLSL compilation.

## 2026-10-13, 22:58: First live INI check and request for generic characters

- User reports working outfit keys and no apparent F10 change. User-started PID 31108 loads 1.0.2/inject/probe. Read-only log/status checks prove two completed F10 exchanges at 22:53:43.879 and 22:53:49.113, generation 2, no reload rejection/error/pending work, ten Ready/published images and none failed. All thirteen saved variables match the latest live state; stocking is 1, outfit switches 0 and dirty false. No pre-F10 snapshot or game restart was collected.
- Evidence: captures/live-ini-1.0.2-20261013-225835-31108/, including raw statuses, this PID's log, project settings, summary and fingerprints. In 182 drawing frames, replacements increase by 6,155 and intentional hides by 6,107. Also 504 variant refusals, 72 identifying and two incomplete draws occur, with the historical missing-buffer reason; no draw-level shader/input correlation or newly reported visual failure. Do not label the interval clean or claim all effects work.
- No input injection, source/DLL/config change, reload request, module swap or process start/stop by the agent. Explained why unchanged inputs and retained settings can make a successful F10 visually inconspicuous. Restart persistence and all-key visual coverage remain open. Documentation-only verification uses text/diff checks; no new build/regression is claimed.
- User then requests extending the present capability to arbitrary characters. Continue with a generic profile/import/runtime audit and implementation; ask for a second existing character Mod while independent work proceeds. Do not infer all-Mod compatibility or reintroduce LOD2+ and performance scope.

## 2026-10-14: Directory language and asset baseline; generic Core inputs (1.0.3)

- User broadens arbitrary-character support to the full enabled Mods directory and all original language/model/texture handling; Yvonne remains a regression input and game collection is deferred. D-044 records the shared architecture. Original installation stays read-only.
- Original Include/namespace/command/resource/key/regex inspection parses 36 installed INIs, 1,330 sections and 8,937 commands without diagnostics. Eight positive/negative directory fixtures pass. Parsing does not execute custom graphics. All 108 DDS files decode and 310 available buffer declarations align; 16 PNGs and 11 absent declared files remain explicit audit findings. Source fingerprints unchanged.
- All nine exported skeleton initializers pass; six isolated callback fixtures pass. Ardelia/Liino/Typhoeus need companion/custom operations, despite full-directory syntax passing. No missing source dependency was edited away. Reports: build/ini-folder/, build/folder-assets/, build/mod-corpus/; scope detailed in docs/mod-folder-runtime.md.
- Shared runtime maps original component ids by evaluated bone ranges and handles exact IB-only hides without fabricated identity/bones. Empty aliases preserve original null semantics. Optional source-API bone/config facts feed a native Core boundary; unknown facts stay unknown, invalid slots refuse and valid requests recover. No source slot is guessed from Vulkan descriptor coordinates.
- Yvonne's generic 462-bone/nine-component mesh package passes the production decoder. Its 14 original entry sections execute against synthetic native inputs, emitting 36 draws and six empty skips, with 23 listeners including F10. Texture-role inspection uses an explicitly limited RabbitFX fixture; production rejects it pending original shader gating and fallback semantics. No material/GPU/new-character acceptance is claimed.
- Build and entire sandbox/run_all.py pass in build/next/run_all-1.0.3-verified.out: runtime 48 plus second main INI, character package 79, character GPU 357, live INI GPU 28, validation 76 and all other steps. Earlier harness corrections: constants before key parsing, engine built-ins, actual file fixtures versus null aliases, separate settings namespace. ABI remains 16b90a85431f86b4; dist stays 1.0.2. No deployment, module exchange or game action.
- Latest user steering explicitly requires coverage from original parser source rather than the 36 installed examples. Continue grammar/command inventory and complete runtime parsing/dependency/execution adapters; pause PNG integration. Generic directory rendering, CustomShader, resource copies, ShaderRegex and source image/shader contracts remain open.

### Standalone PNG checkpoint, paused before integration

- Implemented frontend/assets/png.h/.cpp using Windows WIC, with typed grayscale/RGBA precision, default sRGB metadata, immutable decoded output, per-image bounds and shared source reservations. It is not linked into the layer or resource cache.
- scripts/check_png.py passes 28 CPU cases: seven independent valid pixel fixtures, five malformed/dimension/budget refusals preserving output, and all sixteen installed PNGs compared to Pillow. Report build/png/report.json. Original files unchanged; no GPU upload or panel result.
- User redirects priority to all source-defined INI syntax before integration. Preserve this verified standalone component and continue language/runtime work. No version/deployment change; source/candidate remains 1.0.3.

## 2026-10-14: Source-defined INI corpus and CPU directory generations (1.0.4)

- User explicitly requires reading upstream code and adapting all original syntax,
  beyond the installed examples. Added a source-derived inventory, original-parser
  fixtures and independent production-runtime expression results. Source/executable
  fingerprints prevent stale parser evidence; negative tests require structured
  diagnostics. Recognized deprecated notices are not syntax failures.
- 726 syntax cases cover all ordinary dispatch keywords, 48 operators, named
  operands, target members, resource/pool declarations, stages/ranges, CustomShader
  state, key/preset options, override metadata and regex structure. 65 expression
  results validate every operator plus edge semantics. No exhaustive system/driver
  option or graphics-execution claim is made.
- Production RuntimeConfig.mods_directory imports original Includes and compiles
  dormant explicit lists. Namespace routing, companion updates, settings confinement
  and failed reload isolation pass. Legacy explicit inputs keep reachable Core scope.
  Callback-only key/preset dependencies, preset forward references, CPU IniParams,
  hold restoration and abort are now executed through the original engine.
- Expanded Core graphs reproduced a dangling optimizer branch pointer (BUG-012);
  stable-root adaptation fixes it. Key/preset-only targets could remain empty
  (BUG-013); stable named callee discovery fixes copied cycles too. Native callback
  abort failed a new fixture, then passed after its pre/post loops were corrected.
- Initial cycle test expectations were wrong for original smart selection and
  accumulated callback lists; corrected the fixture, preserving original behavior.
  One late-injection sandbox exited normally after two frames before a probe request;
  no cause established (ISSUE-010). Unmodified modes subsequently passed; no blind
  retry or IPC change. The full original failure log remains available.
- Full rebuilt suite passes in build/next/run_all-1.0.4-final.out: parser 726,
  evaluation 65, runtime 65 plus Yvonne inspection, native callbacks 15, character
  package/GPU 79/357, live INI GPU 28, modes 40, validation 76 and all other steps.
  ABI remains 16b90a85431f86b4. Docs distinguish parser acceptance, CPU execution
  and actual Vulkan graphics support. Diff/text checks completed for local commit.
- Dist remains 1.0.2. No deployment, module swap, capture, game input, process
  start/stop or original-installation writes. Continue offline source-defined pool
  reset/description adapters and general graphics families; PNG integration paused.

## 2026-10-14: Original pool reset, metadata and mixed references (1.0.5)

- Continued the requested source-defined runtime work after committing 1.0.4 as
  073a2a5. PoolCopyOperation already implements metadata copy, proxy/index reset
  and independent element resets without a graphics API; the adapter now executes
  those original methods instead of accepting only whole-pool reference.
- Found BUG-014: any scalar write discarded all cached resource aliases in that
  pool. Reference assignment now mirrors the original is_null field; resolution
  observes element-specific reset state. Unrelated slots and both independent
  halves of mixed elements retain the original semantics. Added original
  reset_per_frame_limits execution for the selected counters.
- Eleven new pool cases first fail against 1.0.4 (preserved in
  build/ini-language/pool-before-1.0.5.json). They then pass with the fix; an
  additional counter test brings the pool suite to 23. Three production-runtime
  cases prove file-plan binding, immutable previous plans after reset and separate
  generations, bringing runtime checks to 68 plus the Yvonne main-INI inspection.
- Full rebuild and sandbox/run_all.py pass in build/next/run_all-1.0.5.out:
  source syntax 726, original expression results 65, character package/GPU 79/357,
  live INI GPU 28, modes 40, Vulkan validation 76 and every remaining step. Shared
  ABI remains 16b90a85431f86b4. Reviewed diff and text checks for local commit.
- Scope remains CPU pool semantics and owned file plans. General resource byte
  copies/clears/readback, expiring/keyed resource ownership, CustomShader, dispatch,
  ShaderRegex and full directory rendering remain open. PNG integration stays
  paused. Dist remains 1.0.2; no game actions, original-installation writes,
  deployment or module exchange occurred.

## 2026-10-14: Original INI GPU buffer command foundation (1.0.6)

- The user approves the resource -> CustomShader -> shader matching -> directory
  graphics order and asks to continue offline until a game check is needed. The
  goal remains active; this checkpoint is one implementation stage, not complete
  original-language Vulkan execution.
- Added ResourceCommands allocation, transfer, clear, readback and fence owners,
  plus GpuResourceRuntime as an opt-in extension of the production original
  parser/evaluator. Original buffer descriptions, store range calculation,
  variable output and buffer-view element calculation are reused directly.
  References, compatible copies/copy_desc, typed/structured/raw clear, immediate
  store feedback, dynamic slots, optimizer batches, pool reset and copy limits
  now have actual GPU byte-result checks. Stage slots remain logical bindings.
- New tests exposed an SRV clear mismatch in the first full run. Preserved
  build/resource-commands/view-before-fix.{json,txt} and
  build/next/run_all-1.0.6.out. Corrected view kind, format/range retention, cached
  views, and the distinction between original SRV/UAV end boundaries and CB
  region lengths. Raw-capable buffers can have typed float views too.
- The targeted fixture has 120 passing checks with zero core/sync validation
  errors or warnings through shutdown. File padding/size metadata, same-list
  branches, two independent generations, rejected candidates, retained owners,
  allocation accounting and failed-scope cleanup are covered. The second full
  suite passes in build/next/run_all-1.0.6-final.out. The frozen-source rebuild and
  full suite also pass in build/next/run_all-1.0.6-verified.out: syntax 726,
  expression results 65, CPU runtime 68, character package/GPU 79/357, live INI
  GPU 28, validation 76 and every remaining step. Diff/text reviewed for commit.
- D-045 and docs/gpu-resource-commands.md describe the explicit private outside-
  render-pass execution domain. It performs no application imports, descriptor
  emission, draw-hook waits, deployment or game operations. Dist stays 1.0.2.
  Next offline work: slot ranges and general buffer regions/properties/options,
  then images and the remaining common shader/directory adapters. PNG stays paused.

## 2026-10-14: Original buffer slot-range GPU execution (1.0.7)

- Continued the active offline goal after be9617f. Original slot-range commands
  now bind/fetch pools, broadcast resources and run per-slot compatible copies.
  Original range expressions resolve once with guarded numeric conversions.
  Plain ref fetch bypasses scalar copy counters/overrides; existing source views,
  fresh structured one-element views and constant-buffer region rules remain
  distinct. Invalid ranges poison the scope and cannot submit.
- Targeted GPU tests pass 155 checks with no core/sync validation messages.
  The initial negative-index expectation followed a misleading source comment:
  unsigned size_t modulo maps -1 to element zero for a three-element pool on
  this build. The test now checks the original result. The added CB fixture
  obtains its source through slots to satisfy original parse-time flag inference.
- Frozen-source full rebuild/regression passes in build/next/run_all-1.0.7.out:
  GPU resources 155, source syntax 726, evaluation 65, CPU runtime 68, character
  package/GPU 79/357, live INI GPU 28, validation 76 and every remaining step.
  ABI stays 16b90a85431f86b4. Reviewed diff and text before local commit.
- Dist remains 1.0.2; no game actions, original source/installation writes,
  deployment or module exchange. Next: buffer regions/options/properties, then
  images and the shared shader/directory stages. PNG integration stays paused;
  the active goal is not complete and further offline work needs no user input.

## 2026-10-14: Owned GPU copy pools and original buffer regions (1.0.8)

- After 5087006, the next GPU fixture reproduced BUG-015 against 1.0.7: ref then
  copy reused a borrowed allocation and overwrote the source. Preserved
  build/resource-commands/copy-alias-before-fix.{json,txt}. Separate owned pools
  now retain each original buffer description; copy_desc restores its previous
  allocation and bytes across size changes without mutating a source alias.
- Adapted source CB windows, CB rounding, view-less structure conversion, region
  bounds/stride truncation, raw views and no_view_cache. Slot reads use only the
  metadata returned by the source API. Unwritten cached bytes remain unchanged;
  tests make no promise about new uninitialized tails. Invalid ranges/overrides
  poison the scope. Indexed copies use the bound format width instead of a
  conflicting declaration stride. New source_stride starts as the original zero.
- Targeted GPU suite: 243 passing checks, zero core/sync validation messages.
  Full frozen-source rebuild/regression passes in build/next/run_all-1.0.8.out:
  parser 726, evaluation 65, CPU runtime 68, character package/GPU 79/357,
  live INI GPU 28, validation 76 and all remaining steps. Reviewed diff, report
  source hashes and English text before local commit. ABI remains unchanged.
- Continue offline with resource properties, including target-specific view
  metadata, null/cache and source_stride behavior, before images and shaders.
  Dist remains 1.0.2. No game action, deployment, module swap or writes to the
  original installation. PNG integration stays paused; the goal remains active.

## 2026-10-14: Buffer properties and source view/null metadata (1.0.9)

- Continued after 73efd18. A new GPU fixture reproduces BUG-016 against 1.0.8:
  plain pool fetch preserves unknown custom format, but the adapter inferred the
  stored view's typed format. Preserved metadata-before-fix.{json,txt} under
  build/resource-commands. Use target-specific view facts as the source does.
- Added owned resource identity, stride/source_stride, size, format, flags,
  offset and extent metadata queries. Original declaration overrides/sentinels
  remain distinct; pure Constants queries do not allocate. Draw offsets reuse
  original VB/IB helpers with RuntimeDraw context. this preserves its property
  and region arguments. Source hash/spatial properties remain unadapted.
- Null now retains cached resource/view owners, source_stride survives null and
  direct range fetch, and format/stride-only aliases remain unassigned. The GPU
  fixtures check these independently plus actual this-region bytes, declaration
  queries, context refusals, poisoned submissions and complete owner cleanup.
- Full frozen-source rebuild/regression passes in build/next/run_all-1.0.9.out:
  GPU resources 303 with zero core/sync validation messages, source parser 726,
  evaluation 65, CPU runtime 68, character package/GPU 79/357, live INI GPU 28,
  validation 76 and all remaining steps. Reviewed diff, exact report source
  hashes and English text before local commit; ABI stays 16b90a85431f86b4.
- Next offline increment is whole-pool assignment/remaining buffer copy options,
  then images and the shader/directory stages. Source clear does not implement a
  range loop, so do not invent one from keyword similarity. Next version 1.1.0.
  Dist stays 1.0.2, PNG integration paused, no game action or deployment. The
  requested complete directory/GPU-language goal remains active and incomplete.

## 2026-10-14: Whole-pool resource assignments and buffer modifiers (1.1.0)

- Restored memory after fc230c3, answered the user's next-step question and
  continued the active offline resource -> shader -> directory goal. Implemented
  original bare/wildcard whole-pool assignments using a retained source and a
  local destination target. Repeated commands, proxy roots, per-element counters,
  source-stride metadata reads, mixed resets and resolved null remain distinct.
- mono and set_viewport do no additional buffer work upstream. resolve_msaa
  still creates/reuses a destination but does not copy buffer bytes, even with
  copy also specified. Tests read actual scalar/range/whole-pool GPU results and
  initialized cache contents; no assertion depends on uninitialized allocations.
- The first cache expectation incorrectly treated equal byte widths as equal
  descriptions; the different source stride correctly selects another owned
  cache. Corrected the fixture to verify both retained allocations and their
  independent byte contents. No upstream or installed mod source was changed.
- Targeted suite passes 402 checks, zero core/sync validation messages through
  shutdown. Full frozen-source rebuild/regression passes in
  build/next/run_all-1.1.0.out, including original parser 726, evaluation 65,
  CPU runtime 68, character package/GPU 79/357, live INI GPU 28, validation 76,
  modes 40, MCP 47 and every remaining step. ABI is unchanged.
- Next offline audit: keyed/expiring pool ownership, retained resources after
  reassignment/expiry and original spatial frame duration. IniRuntime currently
  does not supply gFrameTime. Images, GPU parameters and shader/directory work
  remain open. Dist stays 1.0.2; PNG stays paused. No game input, deployment,
  module exchange or original-installation write. The active goal continues.

## 2026-10-14: Keyed/expiring GPU pools and CPU failure propagation (1.1.1)

- Continued offline after 8109fe5. Original FIFO/spatial pools now address owned
  buffers, including missing-key templates, optional read allocation, mixed
  elements, physical ranges, whole-pool writes, expiry and read refresh. Tests
  distinguish null pool entries from earlier retained bindings and mutable copy
  allocation reuse from retained historical contents.
- Present and draw accept a host frame duration and expose original frame_time.
  The existing character backend forwards its measured duration bounded to
  1-250 ms. Spatial radius uses original ceil scaling; invalid clocks and
  overflowing conversion refuse. The source engine still owns lookup/mapping.
- The added CPU-failure test fails before the fix: IniRuntime returns failure
  after prior GPU recording, but finish still submits that work. Saved
  build/resource-commands/cpu-scope-before-fix.{json,txt}. BUG-017 is fixed by
  notifying runtime extensions on failed Present/draw calls; active GPU scopes
  become failed even when a CPU expression caused the error. Both entry paths
  and allocation cleanup are covered. Already-submitted store work is not undone.
- Targeted and full-suite GPU checks: 485 passing with zero core/sync validation
  messages through shutdown. Frozen-source full rebuild/regression passes in
  build/next/run_all-1.1.1.out: parser 726, evaluation 65, CPU runtime 68,
  character package/GPU 79/357, live INI GPU 28, validation 76, modes 40,
  MCP 47, evidence 113 and all other steps. ABI remains 16b90a85431f86b4.
- Next is general mutable images/resource commands, GPU IniParams and the shared
  shader/directory stages. Remaining buffer properties/conversions are still
  explicit gaps. Source hash helpers have target-specific offsets and original
  cached-data behavior; audit them before adding GPU queries. Existing immutable
  2D DDS upload is not array/volume/mutable-image execution. PNG remains paused.
  No deployment, game action, module swap or original-installation write; dist
  remains 1.0.2. The active goal continues and next work needs no user input.

## 2026-10-14: Owned mutable image execution foundation (1.1.2)

- The user asked what comes next. Restored the current memory/clean a09b073
  checkpoint and continued the existing active offline-development goal. Added
  general owned 1D/2D/3D images, arrays, cubes, mip chains and actual image views.
  Descriptions use real device limits and enabled features; allocation honors
  dedicated requirements and shared resident/staging budgets. Core format facts
  are generated from the pinned Vulkan registry and freshness-checked.
- Added pitched buffer-image transfer, compatible image region copy, typed
  color/depth/stencil clear and color MSAA resolve. Per-batch layout state is
  separate from committed owners; only successful submit publishes GENERAL.
  Discard/rejected submission and fence/view lifetime have pixel/byte tests.
  Extension/multiplanar formats, sparse/imported images, cross-dimensional/block
  copies, volume slice views and depth/stencil resolve remain explicit gaps.
- Targeted tests and the full rebuilt suite pass: 563 GPU resource checks with
  zero core/sync validation messages through shutdown. The full frozen-source
  report is build/next/run_all-1.1.2.out: parser 726, evaluation 65, runtime 68,
  character package/GPU 79/357, live INI GPU 28, validation 76, modes 40,
  MCP 47, evidence 113 and all remaining steps. Final source hashes match the
  resource report. ABI remains 16b90a85431f86b4.
- These are direct owned-executor image results. Original INI image declarations,
  views/copy pools and clear/store/property behavior still need adaptation. The
  source audit in docs/gpu-resource-commands.md records relevant original helper
  order and view semantics, including the 3D depth override assigning Height.
  Typeless aliases require clear conversion by the DXGI view format, not simply
  by the Vulkan allocation format. Reuse the original CPU helper rules.
- Next is that INI adapter, then GPU IniParams, CustomShader, shared shader
  matching and complete directory graphics. No game input needed; the active
  goal remains incomplete. PNG paused, LOD0/LOD1 only. Candidate build/next is
  1.1.2; dist stays 1.0.2. No deployment, module exchange, game/launcher action,
  system registration or original installation/Mod writes.

## 2026-10-14: Restored commit context and explained current texture matching

- Read CURRENT_STATE/TASKS first, then recent commits, dist configuration, live INI documentation and production material code. Source/candidate is 1.1.2, deployed host is 1.0.2. Relevant material policy originates in b90f9e9 (guarded sibling rules); abebbaf adds live INI image choices. Later owned image primitives do not imply a deployed material-policy change.
- Verified exact-byte fragment profiles, per-section/pass sibling lookup, shared image-id precedence, unique shape fallback, complete sibling coverage, two-extra limit, fewest-extra selection and ambiguity rejection. Pass keys include attachment formats/samples, subpass, depth write and compare. Shape records view type/format and allocation extent; it is not a content hash or proof that pixels match.
- Without a verified/derived correspondence, retain application images on the mod geometry and retry later. Explicit absent/null and device-bound ZeroFile rules remain distinct. Runtime upload hashing is off and original image-hash override sections are not accepted by the deployed character registry. Prepared-contract/resource/pipeline failures can still refuse a draw; the unknown-profile fallback alone does not keep the original mesh.
- Explanation and memory updates only. No layer/source implementation edit, new build, game access, module exchange, deployment or launcher change. Existing unrelated GPU-image adapter edits remain untouched. Text/diff checks cover this documentation change.

## 2026-10-14: Original INI image execution (1.1.3)

- Restored CURRENT_STATE/TASKS, active offline goal and in-progress image adapter.
  Preserved the intervening ba636c5 material-matching documentation. The user asked
  what comes next; explained the remaining image/parameter/shader/directory order
  and continued the already authorized work without requesting game input.
- Connected original texture declarations and source CPU view/description helpers
  to owned Vulkan images. Added bounded legacy/DX10 DDS metadata and subresource
  loading using pinned DirectXTK format/footprint helpers, including arrays, cubes,
  volumes, BC formats and packed depth/stencil plane extraction. Kept original
  file/declaration metadata precedence and the source's empty Texture3D bug.
- Extended logical slots, references, copies, per-description pools, null/ranges,
  counters, properties and color resolve to images. Clear operations use selected
  DXGI view formats; reusable budgeted scratch images preserve integer channel
  bits and handle sRGB/float aliases. Native headless system D3D11 WARP checks
  establish UNKNOWN-format and typed/typeless view-family behavior. Production
  creates no D3D11 device and still requires an explicit owned queue scope.
- Tests caught BUG-018: stencil-only clear passed an invalid unused depth value
  to Vulkan. Saved the failing validation output, fixed low-level sanitization,
  and verified separate source depth clamping and original flag priority. A final
  source review caught validation of ignored 1D fields; added a failing original
  INI fixture, saved its report and restricted guards to the consumed overload.
  The earlier full run was deliberately interrupted for that fix and is recorded
  as incomplete in build/next/run_all-1.1.3-interrupted-review.out.
- Final targeted and frozen-source full rebuilt regressions pass in
  build/next/run_all-1.1.3.out: resource 1682, original parser 726, evaluation 65,
  CPU runtime 68, character package/GPU 79/357, live INI GPU 28, validation 76,
  modes 40, MCP 47, evidence 113 and all remaining steps. Resource core/sync
  validation has zero messages through shutdown; source hashes match the report.
  ABI remains 16b90a85431f86b4. Text check covers 283 project files.
- Source/candidate is 1.1.3; dist remains 1.0.2. Remaining image formats/conversions,
  buffer queries, GPU IniParams, CustomShader, shader matching and full directory
  graphics remain open. Recorded the original parameter texture/flush/dirty rules
  as the next adapter audit. No deployment, module swap, game/launcher action,
  original-installation write or system registration. PNG paused, LOD0/LOD1 only.
  The active goal remains incomplete and the next work needs no user input.

## 2026-10-14 - Integer RTV and color/depth image conversions (1.1.4)

- Resumed from the project memory and answered the user's next-step question;
  continued the active offline resource/runtime goal. No game input was needed.
- Added original integer RTV float conversion. Headless WARP comparisons cover
  19 integer formats and two typeless view cases over nine input sets. Both
  WARP and hardware truncate tested fractions; finite overflow can differ.
  The implementation deliberately follows WARP's saturation and records that
  boundary as LIMIT-029, without undefined float-to-integer casts. Initial native
  observations are saved in build/resource-commands/dx11-clear-*.txt.
- Connected single-sample R16/R32 color/depth family copies through a reusable
  resident buffer. Tests check native copy semantics, every mip/layer, partial
  rectangles and offsets for all eight compatible color formats, untouched
  texels, repeated copies, allocation budgets and fence-retained retirement.
  Instrumented execution records zero CPU maps and implicit submissions for
  repeated transfers; only the explicit finish submits their batch.
- Enforced actual enabled unrestricted-depth extension for opaque color-to-D32
  transfers. copy_desc, reverse D32 copies and both R16 directions work without
  it. A separate headless device enables the extension for arbitrary float-bit
  round trips. Source review found BUG-019: host float-depth data lacked a range
  check. Added copied-texel validation with pitch padding ignored and corrected
  the ordinary packed DDS fixture to contain valid depth. This was offline
  validation work, not a deployed-game observation.
- Targeted and final frozen-source full rebuild/regression pass in
  build/next/run_all-1.1.4.out: resource 3285, parser 726, evaluation 65, CPU runtime
  68, character package/GPU 79/357, live INI GPU 28, validation 76, modes 40,
  MCP 47, evidence 113 and all remaining steps. Resource core/sync validation
  has zero messages through shutdown; all 52 recorded source hashes match.
  ABI stays 16b90a85431f86b4. Text check covers 284 project files.
- Candidate/source is 1.1.4, dist stays 1.0.2. No deployment, module exchange,
  game/launcher action, original-installation write or system registration.
  Next is GPU IniParams publication; the audit records that the existing prepare
  callback runs after Constants, so zero initialization needs a separate earlier
  boundary. Further resource formats/options, CustomShader, shared shader
  matching and complete directory graphics stay open. PNG paused, LOD0/LOD1 only.
  The active goal is incomplete; next version 1.1.5 needs no game input.

## 2026-10-14: Original GPU parameter publication, 1.1.5

- Finished the previously unverified parameter boundary interfaces and added
  GpuResourceRuntime's persistent float4 Texture1D/SRV. Zero publication precedes
  Constants; original dirty comparisons, root ends, injected draws/dispatch and
  forced key transitions determine later uploads. Special target reads do not
  flush, and explicit logical slot unbinding survives updates.
- Corrected Constants post execution and root abort leakage (BUG-020), keeping
  nested abort and source pre/post-run semantics. Initial load failure now
  poisons an active GPU scope just like Present/draw failure. Source dimensions,
  budgets, discarded commands and retained submission/view owners are tested.
- Headless GPU tests read actual boundary values, signed-zero/NaN behavior,
  hold restoration and equal-value transitions. A native DXC compute consumer
  reads parameter views and resized arrays. This does not implement general
  CustomShader or change the application's shader bindings.
- Frozen-source full build/regression: build/next/run_all-1.1.5.out, all steps
  pass. Resources 3509, parser 726, evaluator 65, CPU runtime 68, character
  package/GPU 79/357, live INI GPU 28, validation 76, modes 40, MCP 47, evidence
  113. Resource core/sync validation is clean through shutdown; all 57 source
  hashes match. Text check: 288 files clean. ABI remains 16b90a85431f86b4.
- Source/candidate 1.1.5; dist 1.0.2 unchanged. No game/deployment/system action.
  User reiterated continuing each offline part without stopping until a game
  test is necessary. Continue with CustomShader; source include, nested phase,
  state restoration and counter rules are audited in docs/gpu-resource-commands.md.
  Installed SPIRV-Reflect SDK 1.4.321.0 under F:/vulkan, commit
  e55086b044225f9b511ae44dbf9b079d3625943f, for upcoming reflection work; not yet
  linked. Remaining resource formats, shader matching and directory graphics
  stay open. PNG paused, LOD0/LOD1 only. Next version: 1.1.6.

## 2026-10-14: Original HLSL compiler foundation, 1.1.6

- Added a read-only bounded source/include compiler preserving original macros,
  main/model-5 validation and include policies. Native DXBC reflection selects
  source slots; DXC and pinned SPIRV-Reflect supply remapped SPIR-V and metadata.
  Counter descriptors and stage/register-class ranges remain independent.
- Fixed standalone D3DPreprocess's empty-include rejection with a whitespace-only
  preprocessing representation. Native D3DCompile comparison proves the source
  accepts the empty file; recorded dependency bytes are unchanged.
- Targeted and full-suite compiler checks: 116, 33 independently valid modules.
  Actual Vulkan compute output matches native DX11 WARP and 65 float4 inputs.
  All rebuilt regression steps pass: build/next/run_all-1.1.6.out; resource checks
  3544, zero core/sync messages through shutdown, character package/GPU 79/357,
  live INI GPU 28, parser726/evaluator65/runtime68, validation76/modes40/MCP47/
  evidence113. ABI unchanged. Compiler evidence includes dependency hashes.
- Source/candidate1.1.6, dist1.0.2. No deployment, game, launcher or system action.
  General INI CustomShader execution is next, not claimed by these tests.
  Remaining flag/device limitations are LIMIT-030; architecture choice D-046.
  Continue offline immediately per user, next1.1.7; PNG paused, LOD0/LOD1 only.

## 2026-10-14: Original CustomShader owned compute,1.1.7

- Added opt-in original CustomShader parsing/execution, direct compute, nested/
  null stages, original counters and state restoration across abort/failure.
  Dormant source failure rejects a candidate; retained old generations survive.
  Native diagnostic NUL normalization preserves later DXC errors.
- Owned Vulkan pipelines/descriptors/views/samplers cover structured/raw/typed
  buffers, image arrays/storage and low-level UAV counters. Recorded dispatches
  retain distinct bindings; fences retain retired owners. Production layout
  validation gates packed float3 storage. Native WARP/Vulkan bytes agree.
- Reproduced/fixed dispatch resource-query guards (BUG-021); interrupted an
  earlier full run for the correction. Final frozen-source rebuilt regression:
  build/next/run_all-1.1.7.out, all steps pass. Resources3633, compiler117/33 valid
  modules, parser726/evaluator65/runtime68, character79/357, liveINI28,
  validation76/modes40/MCP47/evidence113. Core/sync clean through shutdown;
  66 resource and6 compiler source hashes match. ABI unchanged, text301 clean.
  D-047/LIMIT-031 record owned execution boundaries.
- Source/candidate1.1.7; dist1.0.2. No deployment/game/launcher/system action.
  Continue immediately with compute gaps, then graphics/matching/directory
  integration. No game input needed; next1.1.8, PNG paused, LOD0/LOD1 only.


## 2026-10-14: Shared memory, sampling and indirect compute,1.1.8

- Added bounded workgroup size/provenance analysis, precise constant-array sample
  pairs, indirect GPU execution/original INI adaptation, native null samplers,
  explicit host slots and shared sampler quotas. Native DX11 comparisons cover
  shared aggregates/barriers/atomics and interpolation/clamp/negative LOD.
- Corrected test-only Windows small macro collision and region spelling. Original
  indirect first-comma parsing requires a resource alias for multi-argument
  regions. Original owned UAV flags explicitly lack append/counter support.
- Full frozen-source rebuilt regression:build/next/run_all-1.1.8.out, all pass.
  Resources3720, compiler117/33 modules, parser726/evaluator65/runtime68,
  character79/357/liveINI28, validation76/modes40/MCP47/evidence113. Core/sync
  clean;69 resource and6 compiler source hashes match; ABI16b90a85431f86b4.
- Independent native audit during the suite found BUG-022 binding hazards,
  batch publication and OM restore gaps. Source/evidence:build/source-state-probe/.
  Invalid duplicate-UAV calls affect later native observations; isolate them or
  run last. An older coexistence assertion is wrong; passing the suite is not
  full source equivalence. Fix immediately in1.1.9, then graphics/integration.
- Source1.1.8; dist1.0.2 unchanged. No game/deployment/launcher/system action.
  Continue offline without stopping; no game input needed, PNG paused, LOD0/LOD1.


## 2026-10-14: Native source binding semantics, candidate 1.1.9

Continued offline without user input, as requested. Reproduced BUG-022 before
changing production code: new native buffer reference failed at 3728 checks.
Added source subresource conflicts, atomic range publication, complete attachment
validation and source OM save/restore effects. Pixel UAV assignment replaces its
whole set while compute assignment preserves other nonconflicting registers.
Native WARP also verifies larger depth targets, disjoint image mip/layer reads,
null UAV register effects, and high pixel UAV loss during complete OM binding.
Invalid native calls use isolated devices. Empty CustomShader calls are optimized
away upstream; the restore test now contains an actual command.

Old fixtures explicitly unbind writers before subsequent SRV use; all prior byte
checks remain. Fixed the incorrect compute UAV/SRV coexistence assertion. Full
frozen-source build and regression passes in build/next/run_all-1.1.9.out:
resources 4006 with zero core/sync messages through shutdown, compiler 117/33 valid
modules, parser 726/evaluator 65/runtime 68, character 79/357, live INI 28,
validation 76, modes 40, MCP 47, evidence 113, and every other step. All 71 resource
and 6 compiler source hashes match. Dist stays 1.0.2; no game/deployment action.

Continue immediately with graphics semantic linking, source state and draws,
then matching and complete enabled-directory integration. Next 1.2.0. Alphabetic
IO sorting alone is not stage linking; DX11 position-w and vertex/instance offsets
need native output comparisons. No game input is needed. PNG remains paused;
LOD0/LOD1 only.

## 2026-10-14: Original graphics signatures and Vulkan linking,1.2.0

Continued offline as requested. Added FXC signature retention and a bounded,
immutable Vulkan stage linker. Tests cover all graphics stages, arrays/patch IO,
native register contracts and enabled-maintenance4 width rules. Corrected an
initial invalid native pixel fixture (omitted position register); saved evidence
in build/graphics-link-native-invalid/. Added the original register guard instead
of silently treating semantic matches as permission to change native behavior.

Actual case-sensitive DXC ordering produces wrong Vulkan pixels before linking;
after linking all16 pixels match original DX11 WARP byte for byte. Full rebuilt
frozen-source suite passes in build/next/run_all-1.2.0.out: resources4065,
compiler172/58 valid modules and every other step, zero core/sync diagnostics.
All74 resource and9 compiler source hashes match. Text310 files clean before this
memory update. This proves isolated linker pixels, not original INI drawing.

Continue with original graphics state/draws and native coordinates/IDs, then
matching and enabled-directory integration. Next1.2.1; no game input needed,
dist1.0.2 unchanged, PNG paused, LOD0/LOD1 only. No deployment/game action.

## 2026-10-14: Source graphics coordinates and state foundation,1.2.1

Continued after1.2.0 without game input. Native references reproduced three
compiler convention failures: pixel-w reciprocal, base vertex and base instance.
Saved each before-fix failure under build/graphics-coordinate-before-fix/ and
fixed DXC options (BUG-023). Direct/indexed pixel output now agrees, including
negative vertex bases, nonzero first index, multiple instances and every tested
cull/front-face combination. Test devices explicitly enable shaderDrawParameters;
the upcoming production graphics executor must enforce actual feature enablement.

Added SourceGraphicsState and original bitmask-merge bridge. Unmodified original
merge methods run against actual WARP state creation and agree on effective
fields/factors/masks. Corrected a test that compared undefined structure padding;
no runtime field discrepancy remained. The bridge is not yet graphics execution.

Complete frozen-source rebuild/regression:build/next/run_all-1.2.1.out, all pass.
Resources4559, compiler172/58 valid modules; all existing steps and clean core/sync
through shutdown.76 resource and9 compiler hashes match, text312 files clean.
Source1.2.1, dist1.0.2 unchanged. No game/deployment/launcher/system action.

Independent ignored native sampler probe confirms comparison-filter state used by
ordinary SampleLevel still returns texture values. Evidence:build/source-state-
probe/samplers.cpp, samplers.txt. This is diagnostic, not integrated regression.
Next1.2.2: common graphics descriptor/pipeline ownership, original state/stages
and drawing, then matching/directory integration. No game input needed; continue.
PNG paused, LOD0/LOD1 only.

## 2026-10-14, 20:11: Restored the launcher to DX11 at the user's request

- Read current state/tasks and checked the full relevant process list. Neither Endfield nor the XXMI launcher was running; the helper repeated this check before backup and immediately before the write.
- Active configuration: D:/XXMI-Launcher-Portable-v2.1.6/XXMI-Launcher-Portable-v2.1.6/XXMI Launcher Config.json. Changed only Importers.EFMI.Importer.d3d11_mode_cmd_args from empty to -force-d3d11 and xxmi_dll_inject_mode from SKIP to DIRECT. JSON comparison and exact readback prove every other value unchanged; value-span replacement preserves all other bytes and signed settings.
- Exact original copy and manifest: build/backups/launcher-dx11-20261014-201131/. Before SHA-256 632b40e4d380331e50da2ec6c5dc3d692091b67a746dd37cb2b5177ae70c31a6; after e0914e439bd70c2b6392fedaef4ae1c33836d8e7a01d0cc5d01467bc3e1e7dc6.
- No game/launcher start or stop, layer/package deployment, original Mod/Core edit or system setting change. The user may start DX11; no capture or game verification is implied. Existing unrelated graphics implementation edits remain untouched. Text/diff checks apply to this memory-only source change.


## 2026-10-14: Owned graphics executor, candidate 1.2.2

Continued offline. Shared compute descriptor ownership now also backs VS/PS
pipelines and private graphics passes. Direct/indexed/indirect, vertex inputs,
blend/depth and signed offsets match system DX11 WARP references. Owners survive
submission until fence completion; discard and failure release all budgets.
Initial new fixtures used the wrong original stage macro and color aspect for
depth readback; corrected to VERTEX_SHADER and explicit depth aspect. No production
behavior was inferred from those fixture failures.

Complete frozen-source rebuilt suite: build/next/run_all-1.2.2.out, all steps pass;
resources 4749, compiler 172/58 valid, zero core/sync errors/warnings through
shutdown, all 80/9 source hashes match, text 316 clean before memory update.
Owned API evidence is separate from original INI execution and game evidence.
No deployment/game/system action; dist 1.0.2 unchanged. External launcher-state
commit c9000a7 is preserved independently. Next 1.2.3 connects source INI graphics
stages/state/draws and continues offline; no game input is needed.


## 2026-10-14: Original owned INI graphics, candidate1.2.3

Connected original shader stage inheritance, source state overrides/merge and
all direct/indexed/instanced, indirect and automatic owned draw forms. Direct
from-caller uses original intercepted metadata. Native references verify argument
mapping, null VS/PS, depth writes, source blending, scissors, sampler slot scope,
nested restore and GPU discard after a failed root. State keys compare fields;
ignored native fields canonicalize before later merges. Diagnostic state-defaults
probe is now represented by regression cases. Development errors were confined
to unsigned enum-list initialization and fixture clear/depth-view syntax; fixed
before validation, without loosening production view checks.

Full rebuilt frozen-source suite: build/next/run_all-1.2.3.out exit0, all steps
pass. Resources5342, compiler172/58 valid modules, all existing checks, zero core/
sync messages through shutdown.82 resource and9 compiler source hashes match;
text318 files clean. No game/deployment/launcher/system action; dist1.0.2 unchanged.
This is original owned INI graphics, not full-folder or application rendering.

Continue immediately with all installed enabled HLSL declarations/compilation,
then input-layout and remaining execution cases. No game input is needed. Source
comparison instructions, other graphics stages/raster paths, null resources and
unmeasured native state validity remain explicit work (LIMIT-032). PNG paused.


## 2026-10-14: Complete enabled source HLSL audit, candidate1.2.4

Added original-parser shader metadata and a standalone inspector using the
production FXC/DXC path. Every enabled installed source is included, with bounded
per-program compilation and exact dependency snapshots in build/. Original module
fallback and include behavior are preserved. Corrected development fixture
assumptions about built-in shader sections and numeric boolean normalization;
no parser behavior was changed. Added directory/evidence checks to run_all.

Corpus:34 INIs,30 shader sections,38 stage references/37 distinct programs,
47 null and95 inherited stages.37/37 compile and validate. TextPrinter g/p/c and
chest_deform40 c need the relaxed scalar/standard-layout profile. All installed
INI and HLSL dependency bytes unchanged. These are conditional compiler results,
not actual device execution. Full frozen-source rebuild/regression:
build/next/run_all-1.2.4.out exit0, all pass; resources5342, compiler172/58 valid,
directory language9/evidence2, core/sync clean.82/9/12 source hashes match;
text321 files clean. Source1.2.4, dist1.0.2 unchanged; no game/deployment action.

While testing, inspected the actual TextPrinter geometry stage: point input,
triangle-strip output,256 maximum vertices,1 invocation; source t114 stride72.
A separate native/Vulkan diagnostic confirms unclipped fragment z/w=(2,1) for
both APIs (build/source-state-probe/clip-coordinates.cpp/.txt). No text rendering
claim yet. Continue immediately with geometry/Core text execution, then remaining
layouts/state/resources and application/directory integration. No game input.


## 2026-10-14: Geometry and original Core text, candidate1.2.5

Connected owned geometry pipelines and original stage bindings, with actual
feature, topology, interface-component and output-limit gates. Native point/line/
triangle input tests preserve source draw IDs and unclipped z/w; original font
and TextPrinter v/g/p/c run through owned INI execution. Three text cases cover
wrapping/DPI/alignment and multiple64-character chunks. Corrected development
reference expectations for source first-vertex IDs and alpha ONE/ZERO default;
no production semantics were changed for those fixture errors. Native floating
position/blend differences peak at5.96046448e-08; exact chunk integers remain
required. The explicit1e-6 float tolerance is not a bit-exact pixel claim.

Full rebuilt frozen-source suite:build/next/run_all-1.2.5.out exit0, all steps
pass; resources5644, compiler172/58 valid, corpus37/37, core/sync clean, source
hashes84/9/12 match. Installed sources unchanged; no deployment/game/system action,
dist1.0.2 unchanged. This is controlled original HLSL execution, not full Core INI
or game rendering. Preserve the separate launcher DX11 state.

During regression, independent native input-layout probes confirmed stored
creation-register mapping, APPEND alignment, unused-required signature elements,
slot/class/divisor rules and valid large aligned offsets. Read-only source audit
shows layout edits survive nested CustomShader calls and restore at intercepted
AfterDraw, not Present/key roots. Evidence:docs/input-layout-audit.md and
build/source-state-probe/input-layout*.cpp/.txt. Continue immediately with owned
input-layout execution1.2.6; no game input needed. PNG paused, LOD0/LOD1 only.


## 2026-10-14: Original owned input layouts, candidate1.2.6

Implemented immutable source creation signatures and element descriptions,
register-based correspondence to the current VS, original offset/format edits,
native creation failure fallback and intercepted AfterDraw restore. The original
edit helper runs before draw/dispatch parameter flush, including count zero;
CustomShader and Present completion do not restore it. Pipeline keys include
attributes/streams. Native WARP tests validate formats0..115, signature switches,
APPEND integer fetch, component defaults and instance steps0/1 with nonzero bases.
The step0 diagnostic exposed the need to preserve StartInstanceLocation even
when the Vulkan stride is0; fixed before regression. The pinned enum-based
realignment quirk is deliberately preserved and tested.

Full frozen-source rebuild/regression:build/next/run_all-1.2.6.out exit0, all
steps pass. Resources6093, compiler172/58, corpus37/37, core/sync clean through
shutdown; source hashes88/9/12 match. Installed sources unchanged, no game or
deployment, dist1.0.2 unchanged. This is owned execution, not application import.
Native and physical capability probes remain in build/source-state-probe/.
Both GPUs support KHR divisor/zero/nonzero-base features; neither supports packed
16-bit BGR vertex formats. Continue offline with remaining fetch/type/divisor
semantics and matching/directory/application integration. No game input needed.


## 2026-10-14: Enabled owned instance divisors, candidate1.2.7

Implemented actual KHR/core divisor features and property gates, including zero,
physical maximum and nonzero first instances. Pipeline keys include divisors;
instance fetch bounds are checked for indexed direct draws too. Native WARP
matches original INI direct/indexed/indirect/indexed-indirect draws for0/2/3/max.
Missing enablement, zero support, maxima and unknown indirect bases refuse before
GPU recording; no hidden readback. The direct step0 fallback remains intact.

Full frozen-source rebuilt suite:build/next/run_all-1.2.7.out exit0/all steps;
resources6653, compiler172/58, corpus37/37, core/sync clean, source hashes89/9/12
match, installed inputs unchanged, text329 clean. No game/deployment/system action,
dist1.0.2 unchanged. Preserve separately recorded launcher DX11 state.

Independent native diagnostic input-layout-reinterpret.cpp/.txt confirms raw
input bit reinterpretation, format-based absent-component defaults and BGRX alpha1
for float/int/uint signatures. Continue1.2.8 with explicit SPIR-V input lowering;
packed16 BGR and null/OOB remain separate fetch work. No game input needed.


## 2026-10-14: Native vertex input reinterpretation, candidate1.2.8

Added bounded SPIR-V input type/load lowering, alpha1 BGRX fixup, source-format
component defaults and cache identity. Preserve original DXBC/signatures/source
modules. Validate both original and transformed executable before Vulkan calls.
Native WARP exact integer pixels pass66 source INI cases across float/int/uint
and widths1..4, with extra scalar pointer/alias forms and indirect/cache cases.
Development fixture fixes split differently typed auto declarations and supplied
explicit image layer/mip/sample dimensions; no production fallback was added.

Full frozen-source rebuilt suite:build/next/run_all-1.2.8.out exit0/all steps;
resources8962, compiler172/58, corpus37/37, core/sync clean, hashes92/9/12 match;
installed inputs unchanged. Text333 clean after documentation. No deployment,
game or system action; dist1.0.2 and the separate launcher DX11 state unchanged.

Packed16 diagnostics reveal optional native hardware support: RTX5060 refuses
formats85/86/115; WARP accepts but returns only defaults across65536 words. Both
R16_UNORM controls are exact. Retain unsupported execution instead of cloning
those WARP defaults. docs/input-layout-audit.md records scope/evidence.

Independent matched-order.cpp/.txt reproduces BUG-024: direct hash entry1324,
equivalent pre/post texture checking1234. Source audit confirms all pre lists
precede post lists, with shared phase abort and independent root transient state.
Continue1.2.9 immediately with that correction; no game input needed.


## 2026-10-14: Matched command phase correction, candidate1.2.9

Corrected BUG-024 with one shared pre/post transaction, source match ordering,
phase abort and fresh post-root state. CPU tests cover priority/filtering, source
checktextureoverride equivalence, nested combined abort, queries, no matches and
cross-match budgets. GPU/native pixel tests cover pending versus applied layout
lifetime, restoration and later-match failure without partial submission.
Fixture correction: unprefixed handling=abort defaults to both phases in the
original parser. Pre-only tests now say pre handling; both forms are verified.

Full frozen-source rebuilt suite:build/next/run_all-1.2.9.out exit0/all steps;
runtime118, resources9020, compiler172/58, corpus37/37, core/sync clean; hashes
93/9/12 match and installed inputs unchanged. Text336 clean after documentation.
No game, deployment or system action. Dist1.0.2 and the separately authorized
launcher DX11 configuration are unchanged.

Read-only source audit records stage-root ordering, original call placement,
legacy depth-filter semantics and tracked shader queries through CustomShader.
Also records unsafe/unimplemented original cb_mask/srv_mask evaluation. Continue
1.3.0 immediately with owned source shader hooks/queries, then remaining directory
and application integration. No game input is required at this boundary.


## 2026-10-15: Original source shader hooks, candidate1.3.0

Implemented opt-in original ShaderOverride parsing, bounded independent stage
roots around owned graphics/compute, native tracked shader queries and unchanged
DXBC declarations. Preserve temporary CustomShader identity, original hash0 and
duplicate rules, skip/abort, fresh root state, Present counters and distinct
AfterDraw/AfterDispatch layout lifetime. Failed roots cannot submit recorded GPU
work. Unknown source facts and unsupported legacy substitution explicitly refuse.

Full rebuilt frozen-source regression:build/next/run_all-1.3.0.out exit0/all steps;
runtime200, resource9139, compiler172/58, corpus37/37, core/sync clean. Exact
source hashes96/9/12 match and installed sources are unchanged; text341 clean.
Native order-sensitive pixel references and compute output123 verify actual
pre/original/post placement on owned resources. No game/deployment/system action;
dist1.0.2 and the separately authorized launcher DX11 configuration unchanged.

Correction to the preceding audit: cb_mask/srv_mask return0 in this pinned
source; MemberArg's absent expression returns0 safely. The earlier unsafe-access
claim was withdrawn after inspecting that implementation. Native query tests
preserve the actual zero behavior. Initial shader-query fixture also now uses
an ordinary assignment after literal global initialization, as source grammar
requires; production parser was not changed to accept the invalid fixture.

Next1.3.1: general resource matching. Source command lookup appends hash then
fuzzy matches, unlike the separate creation helper. Keep original draw-context
and region rules, all checked targets and pool semantics. Audit recorded in
docs/resource-matching-audit.md. Continue without needing game input.


## 2026-10-15: General original resource matching, candidate1.3.1

Reused unchanged ResourceHash.cpp descriptor/draw predicates through a small
module boundary. Parsed original region policy and draw maps; preserved region
prefiltering, known-zero fallback, native hash/fuzzy order and signed filter zero.
General checks now handle resource targets, stage/IA/UAV slots, dynamic/range
targets and wrapped pool elements. Owned source identities are weak allocation
records: references share them and fresh byte copies remain untracked.

Native WARP comparisons agree for80 buffer/1D/2D/3D identity/draw combinations.
CPU tests cover buffer regions, unknown required facts, this and index guards.
GPU cases exercise stage slots, IA/UAV, pools, alias/copy identity and this writes.
Source nuance found during review: this resolves its checked buffer but still
uses whole-resource matching, not IA region matching. Both are now covered.
Fixture correction: explicit pre/post checktextureoverride each combines both
target phases, so the ordinary phase test now uses one unprefixed command.
The native matcher fixture also received its required explicit Draw call type.

Full rebuilt frozen-source suite:build/next/run_all-1.3.1.out exit0/all steps;
runtime311, resources9242, compiler172/58, corpus37/37, clean core/sync through
shutdown. Source hashes99/9/12 match; installed inputs unchanged, text345 clean.
No game/deployment/system action. Dist1.0.2 and separate launcher DX11 state stay
unchanged. Continue1.3.2 with original ShaderRegex generation ownership, model
facts and bounded/idempotent command linkage. Game input is not needed yet.


## 2026-10-15: Original ShaderRegex links, candidate1.3.2

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

The first GPU fixture expected one region lookup per draw. Original unprefixed
CheckTextureOverride executes in both shader phases, each matching independently;
corrected the expectation to two without changing production behavior. All
native-pixel and command-count assertions remain. CPU tests additionally call
native repeated link/unlink directly and alternate retained frontend generations.

For the next offline translation assessment, downloaded official vkd3d2.1 source
only into F:/vulkan/vkd3d-2.1. Archive SHA256:
7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a.
Source: https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz .
It is not built, linked, deployed or executed. Original XXMI assembly helpers
remain the intended text/patch authority. No change outside project-owned paths.


## 2026-10-15: Original ShaderRegex assembly, candidate1.3.3

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

Initial fixture correction: .Pattern1/.PatternZ are invalid in the pinned
parser even though the internal group has a pattern map. Reject diagnostics
before extension preparation; use sorted valid groups to test sequential edits.
The first exploratory invalid graph produced misleading allocation failures;
no production workaround was added. Corrected the earlier audit inference.

Built independent vkd3d2.1 compiler in F:/vulkan/vkd3d-2.1-build from unchanged
release source. Portable GCC/winflexbison/SPIRV-Headers tools are installed only
under F:/vulkan with source URLs and archive SHA256 records. The build needed
complete SPIRV-Headers and the upstream debug macros left enabled. Library load
reports2.1; DLL SHA256976a527eb6f2529e05103c308a69ba74c6625664a4b92ed3e8ff81f6b3c17f49.
Not linked to this candidate or deployed; next record a reproducible project
build recipe and verify actual translation. All test/code sources stayed frozen
through the full1.3.3 regression; dependency work was independent.


## 2026-10-15: Exact DXBC translation, candidate1.3.4

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

The first failing compile revealed wrapper double-free: native SPIR-V failure
already releases its temporary output without nulling the pointer. Output/scan
ownership is now conditional on success. Repeated unsupported-double failures
verify intact prior programs and clean cleanup. No upstream source edits.

The original assembler emits compact l(1,1,1,1), not the spaced literal in the
initial synthetic patch fixture. Corrected the fixture against actual native
text, leaving production assembly behavior unchanged. Runtime test stdout is
unbuffered so fatal failures retain diagnostic evidence.

Dependency build is reproducible in scripts/build_bytecode_compiler.py, with
pinned archive/tree checks and tool/command/DLL hashes in F:/vulkan. Initial
portable cc1 path was corrected to its libexec directory. No system install or
registry/environment mutation. Full-suite source files stayed frozen; only docs
and separate build/float-controls probes changed while it ran.


## 2026-10-15: Float32 lowering, candidate1.3.5

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

The first Fract boundary test returned1 for tiny negative normal inputs instead
of native0x3f7fffff. An ordered clamp fixed it. A later Intel case preserved a
negative NaN instead of the selected positive canonical NaN because the module
had not declared SignedZeroInfNanPreserve. Adding the required mode (plus RTE)
fixed exact output, and negative host-property tests reject missing enablement.
This is not a relaxation of reference comparisons. All implementation/test
sources stayed frozen through the full run and corpus follow-up.

## 2026-10-15: Exact bytecode buffer views, candidate1.3.6

Added bounded native declaration contracts, original INI stride/class checks,
typed UAV texel conversion and actual per-binding format capability checks.
Native WARP compares complete buffer output including untouched padding for
12/16-byte structures, raw views and RGBA8 read/write with nonzero regions.
Missing features/queries and incompatible source stride refuse before submission.

Initial targeted tests exposed three adapter assumptions: CBVs also report
native BUFFER resource type; array stride may live in reflected type metadata;
native typed views ignore StructureByteStride unless STRUCTURED is set. Fixed
against native evidence. The rejection test now respects poisoned resource
generations instead of trying to recover the same failed generation.

Full rebuilt frozen-source suite: build/next/run_all-1.3.6.out, exit1 with exactly
the two INPUT-001 installed corpus failures. All code/GPU checks pass:
runtime475/resources10784, compiler172/58; core/sync clean through shutdown.
Original duplicate sections produce recoverable native warnings; hosted tests
currently treat those as failures. Follow-up project-only normalized corpus:
55/55 programs from70 declarations, build/corpus-1.3.6/hlsl/report.json.
Source hashes113/9/12 match; all88 installed sources remain unchanged. No game,
deployment, launcher or system actions; dist1.0.2 unchanged. Continue offline
with transactional ShaderRegex publication and exact original source identity.

## 2026-10-17: Review of Codex's work since 1.0.2 for the question of supporting all mods (Claude)

- The user asked how to extend support from one mod to all mods, then told Claude to read Codex's commits before concluding: the first answer (T-030 note of 2026-10-13, 23:34) contradicted 1.0.2's live keys and D-044. Read CURRENT_STATE, TASKS, D-043 to D-066, docs/live-ini.md, docs/mod-folder-runtime.md, docs/shader-matching-audit.md, docs/compatibility-inventory.md and commits up to ab958a3 (candidate 1.3.6).
- Added a review note to T-030: the shader catalogue with application provenance is the unbuilt keystone; the game's assets are in an encrypted VFS and not a route; proposed an offline structural-matching experiment on the arcane DXBC/SPIR-V pairs already on disk. No code, build, deployment, launcher or game action. Codex's uncommitted working-tree changes of 2026-10-15 were left untouched; a remote origin exists and nothing was pushed.

## 2026-10-18: INI handed to Claude; original warnings recoverable (candidate 1.3.8, branch claude/ini)

- The user handed the INI part to Claude. Created worktree `.worktrees/ini` (branch `claude/ini` from 66213ba, `.worktrees/` in .git/info/exclude) to keep clear of Codex's uncommitted 2026-10-15 changes in the main tree.
- Baseline in the worktree: four failed steps. Two were INPUT-001 (strict warning policy), one cascaded from it (the folder HLSL tool is built by the failed step), one was the panel check's fixed path to a Mod the user has removed.
- D-067 implemented: frontend/xxmi/runtime.cpp, runtime.h, runtime_shim.cpp (warnings recorded, no rejection), layer/src/character_backend.cpp (status and log), sandbox/xxmi_host/shim.cpp and folder_parse.cpp (inspection lists warnings, fails on dire only), scripts/check_ini_language.py, sandbox/ini_folder_test.py, sandbox/xxmi_runtime.cpp, runtime_shader_cases.h, runtime_regex_cases.h, resource_gpu.cpp, character_gpu_case.h, runtime_test.py; scripts/check_panel_hlsl.py finds an installed panel source. Version 1.3.8.
- Results: source language 726/726, directory fixtures 9/9, installed folder parses with 11 warnings and no dire message, installed HLSL corpus compiles, runtime 458 checks, live INI rendering 32/32, GPU resources 10786. Full suite: full rebuilt suite in the worktree passes every step (build/next/run_all-1.3.8.out): source language 726/726, directory fixtures, installed folder syntax and HLSL corpus on the original installed files, runtime 458, GPU resources 10786, character 357, live INI 32/32, Vulkan validation 76; the worktree baseline before the change failed four steps (run_all-baseline.out).
- Not done: merge into main; no deployment, game, launcher or system action.

Follow-up, same day: installed operation census. The inspection host prints an `operation_census` (class plus target kinds, stages, copy options and draw form, per section); scripts/ini_operation_census.py attributes it to Mods and writes build/ini-operations/report.json; new suite step. 58 inputs, 16,423 commands, 57 variants, 21 owners. Reading of the adapters: the character content runs in the production renderer; the image panel (`o0 = bb`, clears, non-indexed draw, CustomShader graphics, PNG, cursor) in about 20 Mods is refused in the game; CustomShader compute in three Mods runs only in the owned domain; no texture-hash replacement occurs. Proposed next: the panel, pending the user's word on PNG.

## 2026-10-18: INI execution matrix (candidate 1.3.9, branch claude/ini)

- `runtime_test.exe --probe-command <ini>` runs one fixture through a CPU generation and prints stage, reason, plan items, skip, warnings and variables.
- `docs/ini-execution-matrix.json` and `scripts/check_ini_execution_matrix.py` (new suite step after INI evaluation): 32 classes, 41 probed uses, cited owned evidence verified, installed census counts printed beside. A broken copy fails on a missing class, a wrong probe expectation, stale evidence and a decision that contradicts its probe.
- Found and fixed BUG-025 (teardown crash with the upscaling flip, frontend/xxmi/runtime.cpp). D-068: upscaling flip original, 3DMigoto overlay no-op (frontend/xxmi/draw_plan.cpp). docs/ini-language-coverage.md points at the matrix. Version 1.3.9.
- Results: full rebuilt suite in the worktree passes all 29 steps (build/next/run_all-1.3.9.out): INI execution matrix 32 classes and 41 probes, source language 726/726, evaluation 65/65, runtime 458, GPU resources 10786 with no validation message, character 357, live INI 32/32. Sandbox only; no game, deployment or launcher action.
- Not done: merge into main; no deployment, game, launcher or system action.

## 2026-10-18: merge into main; image panel step 1 (candidate 1.4.0, branch claude/ini)

- The user: "resume, and merge into main" (resume PNG and the image panel; merge claude/ini).
- Merge: Codex's uncommitted main-tree work (19 modified and 2 new files, untouched since 2026-10-15 14:23) was stored verbatim as commit a188300 on branch codex/wip-2026-10-15 (checked file by file), backed up as a patch in the session scratchpad, the main tree cleaned, main fast-forwarded to claude/ini (447b3aa), and the work reapplied uncommitted. Only the version line conflicted; main's 1.3.9 was kept, so Codex's work needs the next free version when it is committed. Codex's added and removed lines in the five overlapping files are unchanged.
- Panel inspection: `k/1.ini` (read-only) needs character marks, Ctrl+K and LBUTTON, window/resolution/cursor operands, persistent variables, writes to the main INI namespace, two PNGs and a CustomShader with alpha blending drawing four vertices into `o0 = set_viewport bb`. The layer intercepts only vkQueuePresentKHR among the presentation calls.
- Step 1 (D-069): frontend/xxmi/runtime.h, runtime.cpp, runtime_shim.cpp, draw_plan.cpp; sandbox/runtime_window_cases.h; docs/live-ini.md. Version 1.4.0.
- Results: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.0.out): runtime 471 checks including 13 window-operand checks, execution matrix 32 classes and 41 probes, GPU resources 10786, character 357, live INI 32/32. Sandbox only.
- PNG reading so far: the pinned DirectXTK WIC loader agrees with frontend/assets/png.cpp on PNG formats and on sRGB detection (sRGB chunk only); the decoder lacks the declaration's color_space flags (FORCE_SRGB, IGNORE_SRGB), and gpu_images.cpp still refuses non-DDS files. force_detect_color_space is not set in the user's d3dx.ini.
- Not done: steps 2 to 4; no deployment, game, launcher or system action.

## 2026-10-18: image panel step 2, PNG file images (candidate 1.4.1, branch claude/ini)

- Read the pinned CustomResource::LoadFromFile, GetWICFlags and DirectXTK WICTextureLoader.cpp: non-DDS files take the WIC loader with maxsize 0 and no context; PNG sRGB comes from the sRGB chunk only; FORCE_SRGB and IGNORE_SRGB come from `color_space`.
- frontend/assets/png.h and png.cpp (loader flags), frontend/xxmi/gpu_images.cpp (WIC path), scripts/build_xxmi_runtime.py (decoder in the runtime objects), sandbox/resource_png_cases.h and resource_gpu.cpp (13 native comparisons, BMP refusal), resource_image_ini_cases.h (a missing PNG now refuses as unreadable), scripts/check_resource_commands.py (copies the installed panel PNGs and checks them unchanged), docs/gpu-resource-commands.md. Version 1.4.1.
- Results: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.1.out): GPU resources 11027 checks with no validation message (13 PNG native comparisons), PNG decoder check 12/12, runtime 471. Sandbox only.
- Not done: steps 3 and 4 of the panel; no deployment, game, launcher or system action.

## 2026-10-18: image panel step 3a, host back buffer (candidate 1.4.2, branch claude/ini)

- frontend/xxmi/gpu_resources.h and .cpp (backbuffer(), swap chain targets in get()), sandbox/resource_backbuffer_cases.h, resource_gpu.cpp, scripts/check_resource_commands.py. Version 1.4.2.
- Read the original target table: bb, r_bb, f_bb are source_only; a defensive copy guard written first was unreachable and removed.
- Read CustomShader::compile and load_cached_shader: a `<name>.<model>.<flags>.bin` with the HLSL's exact FILETIME replaces compilation. All 19 enabled panel Mods ship such caches with matching times; one disabled Mod has none.
- Results: full rebuilt suite (build/next/run_all-1.4.2.out): 28 of 29 steps pass, GPU resources 11056 checks with no validation message; the installed HLSL step failed only because two installed INIs changed during the run and passes when rerun with the installed syntax step.
- Environment during the run: Endfield.exe was running under DX11, and `Mods/Si Lunabelle 1.3/si.ini` and `Si_BossGuard.ini` were rewritten at 19:57:03 while the suite ran; the installed HLSL step then reported its inputs changed. Rerunning the installed syntax step and the HLSL step afterwards passes. No game, launcher or installed file was touched by this work.
- Not done: 3b and step 4; no deployment, game, launcher or system action.

## 2026-10-18: image panel step 3b, the installed panel in the owned domain (candidate 1.4.3, branch claude/ini)

- D-070 frontend/xxmi/gpu_shaders.h/.cpp (cache rule, enable_cached_bytecode). D-071 frontend/gpu/shader_commands.h/.cpp and gpu_shaders.cpp (null descriptors). sandbox/resource_gpu.cpp (device option for robustness2 nullDescriptor), resource_compute_cases.h (host flag), resource_panel_cases.h (end-to-end panel), scripts/check_resource_commands.py (copies the installed panel with times, stub INI, mark hash; checks originals unchanged). docs/custom-shaders.md. Version 1.4.3.
- Findings on the way: the first panel mark section has no match_first_index; hardware float32 mode needs denormal flushing the sandbox device lacks (Software used); the null t101 slot needed D-071; the hardware D3D11 adapter and the owned Vulkan device are both the NVIDIA RTX 5060 Laptop GPU (checked by name).
- Results: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.3.out): GPU resources 11137 checks with no validation message, including the installed panel (2 of 360,000 pixels differ from native hardware D3D11 on the same NVIDIA GPU), execution matrix 32 classes and 41 probes. Sandbox only.
- Not done: step 4 (layer, swap chain, character generation); no deployment, game, launcher or system action.

## 2026-10-19: INI completion phase 1 foundations (candidate 1.4.4, branch claude/ini)

- The user asked whether the INI was fully adapted (it is not: parsing yes, sandbox execution mostly, the game only character content) and chose to complete the INI first. Phase plan recorded in TASKS T-031.
- P1-1 (D-072): frontend/xxmi/owned_generation.h/.cpp, draw_plan.h/.cpp (owned_scope gate, reference mirroring), gpu_shaders.h/.cpp (exclude_shaders), gpu_resources.h (scope_active), scripts/build_xxmi_runtime.py, sandbox/resource_owned_generation_cases.h. A first test version listed an unreferenced command list that explicit-input generations do not compile; it now loads as a directory generation.
- P1-2: frontend/gpu/resource_commands.h/.cpp and resource_images.cpp (import_image, external layouts, SubmitSync), gpu_resources.h/.cpp (finish with semaphores), sandbox/resource_backbuffer_cases.h (imported_backbuffer). Version 1.4.4.
- Results: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.4.out): GPU resources 11175 checks with no validation message (mixed generation and imported back buffer included), runtime 471, execution matrix 41 probes. Sandbox only.
- Not done: P1-3 (layer), P2, phases 2 to 4; no deployment, game, launcher or system action.

## 2026-10-19: P1-3a presentation facts in the layer host (candidate 1.4.5, branch claude/ini)

- layer/src/state.h (SwapchainInfo, device presentation and null-descriptor fields, surface_window), layer/src/layer.cpp (surface hooks in both entries, swap chain hooks, application API version, robustness2 nullDescriptor in character mode), layer/src/state.cpp (status), sandbox/probe_test.py (checks). Version 1.4.5.
- Results: full rebuilt suite passes all 29 steps (build/next/run_all-1.4.5.out), probe end to end in all modes, live INI 32/32 and character 357/357 with the null-descriptor extension added. Sandbox only.
- Design notes for P1-3b: the sandbox swap chain requests color attachment usage only, so the readback test needs an opt-in readable swap chain in the sandbox program; in character mode the layer will add color attachment usage if an application lacks it (always supported by the specification); bb's shader-resource bind flag follows the sampled usage. ShaderCommands needs the SPIR-V validator DLL, so DXC, vkd3d-shader and SPIRV-Tools are to be deployed next to the layer.

## 2026-10-19: P1-3b original Present work on the swap chain (candidate 1.4.6, branch claude/ini)

- frontend/xxmi/owned_runtime.h/.cpp (facade), gpu_resources.h/.cpp (deferred scope, suspend), layer/src/character_present.h/.cpp (OwnedPresenter), character_program.h/.cpp (mixed generation, Mod folder INIs, DDS-only package images, deferred load scope), character_backend.cpp (presenter, frame with presentation, first refusal logged), character_module.h (PresentWork, frame signature), m2_backend.h/.cpp, state.h/.cpp, layer.cpp (presentation waits replaced, color attachment usage, shaderDrawParameters and nullDescriptor in character mode), scripts/build.py (tools beside the layer; presenter in the test builds), sandbox/src/main.cpp (--readable-swapchain), sandbox/owned_present_test.py, sandbox/run_all.py, docs/owned-present.md. Version 1.4.6.
- Problems on the way: layer translation units cannot include the original engine's headers (hence the facade); the tracker test build needed the presenter; the fixture's settings folder sits inside its Mod folder and had to be excluded from the INI scan; the first windowed run refused because the compiled shader needed draw parameters (D-073).
- Results: full rebuilt suite passes all 30 steps (build/next/run_all-1.4.6.out), including owned Present on a swap chain 20/20 in both entries with clean core and synchronization validation, live INI 32/32 and character 357/357 with the mixed generation. Sandbox only.
- Not done: loading into the game (the user's decision), P2, phases 2 to 4.

## 2026-10-19: phase 3, remaining operands and buffer hashes (candidate 1.4.7, branch claude/ini)

- frontend/xxmi/draw_plan.h/.cpp (host_values, render_target_size, HostValueQuery, RenderTargetQuery, window heuristic through o0, fps and ->region accepted), runtime.h/.cpp (ticks_us, effective_dpi, cursor hotspot; FPS counter at present; host values installed), gpu_resources.h/.cpp (render target size, hash queries), layer/src/character_present.cpp (DPI and hotspot sample), character_backend.cpp (ticks), sandbox/runtime_window_cases.h (host values), sandbox/resource_operand_cases.h (new), sandbox/resource_gpu.cpp (hash property cases; the two negative cases that expected hash refusals now use a vertex buffer outside a draw), docs/live-ini.md. Version 1.4.7.
- Problems on the way: two existing negative tests used ->hashregion as the example of an unsupported query; a Texture2D declared without array and msaa fails creation in the original (zeroed description), so the fixture declares them.
- Results: full rebuilt suite passes all 30 steps (build/next/run_all-1.4.7.out): original INI runtime 483 (host values 12 new), GPU resources 11189 with clean validation (owned operand and hash property cases new), live INI 32/32, owned Present 20/20, character 357/357. Sandbox only.
- P2 not started: its installed users read game resources. Reported to the user, who chose game data collection for the catalogue; the collection plan is proposed (TASKS T-031).
- Not done: loading into the game (the user's decision), P2, phases 2 and 4.

## 2026-10-19: shader catalogue builder and collection protocol (branch claude/ini)

- The user answered the P2 question with "collect game data". scripts/shader_catalogue.py (new) merges dx11_baseline.py summaries into the catalogue; sandbox/unit_test.py covers merging, XXMI exclusion, ambiguity, conflicts and the check against a verified table (12 checks). docs/shader-catalogue.md: method, trial result, collection protocol, open points.
- Trial on existing captures (outputs in the main tree's build/catalogue-trial, not committed): 76 programs from 710 draw pairs, consistent with docs/arcane-texture-slots.json, one constant buffer conflict by size.
- Checked read-only: the launcher's EFMI profile is DX11 with the EFMI MCP probe as its injected library; the game and launcher were not running. No setting changed, nothing deployed.

## 2026-10-19: catalogue collection session A under DX11

- The user started the game under DX11 (PID 12024, EFMI MCP probe 0.1.9) and showed 16 characters in the character display with mods off; 16 frames captured into captures/catalogue-20261019/dx11 with shader declarations, DXBC archive and texture content hashes. Verified: every archive entry present and fingerprinted, names equal XXMI's fnv_64_buf, no unreadable texture; manifest.json written. 247 distinct shaders, 509 distinct texture contents. The probe's held capture was released. No launcher setting changed; our layer was not loaded.

## 2026-10-19: catalogue collection session B under Vulkan, launcher back to DX11, D-075

- At the user's request: launcher EFMI profile switched to Vulkan (17:04, backup build/backups/launcher-vulkan-20261019-170405), dist/efmi_vk.cfg set to probe only with image hashes and no character package after a sandbox check found BUG-026 (a refused package crashes the process, 1.0.2 and 1.4.7). A full sandbox capture with the deployed layer and this configuration passed before the game was started.
- The user started the game under Vulkan (PID 17836) and showed the 16 characters again; 16 bundles captured, matched one to one with the DX11 frames, paired and built into a first catalogue (CURRENT_STATE). Helper scripts for this session lived in the scratchpad (capture step with empty-frame retry, scene matching, supplement of omitted pipelines).
- At the user's request the launcher was switched back to DX11 (17:35, backup build/backups/launcher-dx11-20261019-173504); the injected library is the user's to set back.
- The user stated that the work is general, not per character: D-075, AGENTS.md, docs/shader-catalogue.md (role of the catalogue as ground truth), auto memory.

## 2026-10-19: DX11 registers from SPIR-V (D-076)

- Explored the 16-scene catalogue: names are stripped on both sides; interfaces differ (DX11 builds declare more and lay out constant buffers differently), but SPIR-V declaration order agreed with DX11 register order in all 675 adjacent pairs, and ranking shader resource views with buffers first predicts every observed t register (1187 of 1187 over both catalogues and both stages). FXC alone numbers by plain declaration order (own fixture), so buffers-first is the game's build convention.
- server/efmi_vk/source_slots.py, scripts/check_source_slots.py (suite step), docs/source-slot-truth.json, docs/source-slots.md; unit tests with a DXC-compiled fixture (10 new, 110/110).

## 2026-10-19: material rules from the register rule in the layer (candidate 1.4.8)

- frontend/gpu/source_registers.h/.cpp (new), layer/src/character_backend.cpp (derive by the rule, profiles checked against it, sibling machinery removed), scripts/build.py (source_registers in the material sources; material_sibling gone), sandbox/character_test.cpp (register tests instead of sibling tests; --derived prints registers), sandbox/character_test.py and runtime_fixture.py (fixture slot 1), sandbox/character_gpu_case.h, docs/character-materials.md, character-backend.md, hot-swap.md. Removed: layer/src/material_sibling.h/.cpp, scripts/sibling_check.py, sibling_replay.py. Version 1.4.8.
- Results: full rebuilt suite 31/31 steps (build/next/run_all-1.4.8.out). The dithered variant from the 0.9.8 flicker captures gets the same bindings by the rule as by the old sibling search.

## 2026-10-19: BUG-026 fixed (candidate 1.4.9)

- layer/src/character_backend.cpp (abandon after a refused init; arena counts for rules from modules), sandbox/runtime_test.py (refused package with a missing stream file in both entries). Version 1.4.9.
- Results: live INI 38/38; full rebuilt suite 31/31 steps (build/next/run_all-1.4.9.out). The user asked not to end the turn while waiting; waits now block inside the turn.

## 2026-10-19: several packages at once, packages without captures (candidate 1.5.0)

- scripts/check_mod_corpus.py (implicit namespace, planner failures recorded), scripts/package_directory.py (new), scripts/package_character.py (--no-profiles, empty lists for components without entries, LOD beyond 1 left out), scripts/character_materials.py (no table), layer/src/character_package.cpp (no profile allowed), layer/src/character_backend.cpp (CharacterController per package, directory packages, lazy activation, reload of lazy packages, status), layer/src/m2_backend.cpp (character mode for a directory), sandbox (fixture salt and names, --lazy-packages scene prelude, two-package test, envelope without profiles), docs/several-packages.md. Version 1.5.0.
- Results: corpus 21/22 skeletons, 14/22 callbacks; 13 packages built; full rebuilt suite 31/31 steps (build/next/run_all-1.5.0.out).

## 2026-10-19: 13 real packages in the sandbox; texture identity (D-077)

- Sandbox with build/characters (13 live packages): all refused at INI load (11 texture-hash overrides need source-image identity, 2 shader sections). Measured 2 s and 364 MB, normal exit.
- Texture hash rule checked offline on the catalogue captures (docs/texture-identity.md, D-077).
- Implemented in candidate 1.5.1: tracker data hash (layer/src/resources.cpp/.h), layer/src/texture_identity.cpp/.h (new), per-draw texture answers in layer/src/character_backend.cpp, frontend texture queries (frontend/xxmi/runtime.h/.cpp, draw_plan.cpp/.h coexistence with the resource matcher, character_core.cpp), sandbox fixture texture-hash overrides and probe_test DX11 hash check, docs/texture-identity.md and docs/probe-resources.md.
- First suite run failed two steps because the capture's content object gained a key (probe end to end, budgets): moved `dx11_hash` beside `content`, and no hash when image content tracking is off. Rerun: full rebuilt suite 31/31 steps (build/next/run_all-1.5.1.out); live INI 53/53.
- 13-package smoke on 1.5.1: 10 configure, 3 refused (perlica, xaihi: shader sections; zhuangfangyi: mesh outside the stream profile); RabbitFX references unresolved in five (LIMIT-004). Sandbox only.

## 2026-10-19: meshes of other INIs, inert shader sections (candidate 1.5.2)

- layer/src/character_program.cpp/.h and character_backend.cpp (meshes outside the stream profile stay the game's, logged), frontend/xxmi/runtime_parser.cpp (inert ShaderOverride accepted without shader hooks), sandbox/runtime_test.py (outside mesh, inert and acting shader sections), sandbox/xxmi_runtime.cpp and resource_gpu.cpp (unexecutable candidates now use handling = skip; inert form loads), docs/several-packages.md. Version 1.5.2.
- Results: 12 of 13 directory packages load in the sandbox (xaihi refused); live INI 61/61; full suite 30/30 test steps on the rebuilt layer (build/next/run_all-1.5.2.out; the final run used --no-build after a test-only fix).
- Found: six of the twelve reference RabbitFX, a ShaderRegex library in another Mod folder; its patterns pick the game's main character shader by its constant buffer layout and replace the first, second and third texture samples (diffuse, lightmap, normal) with t70 to t72. Planning refuses seven Mods for original diagnostics, mostly the same RabbitFX references inside callback lists.

## 2026-10-19: RabbitFX assessment

- scripts/rabbitfx_assessment.py and docs/rabbitfx-assessment.md: RabbitFX's roles from the already patched DX11 shaders of session A, paired with the Vulkan fragment modules; measured candidate rules (diffuse first sample 79/83, all three roles 52/83). No layer change.
- Process slip: a `pip download` of the third-party regex package went to the scratchpad without asking first; it was deleted unused and the assessment uses Python's own re module with the PCRE syntax converted.
- Waiting for the user: the RabbitFX option and a game session.

## 2026-10-19: character Mods straight from the Mods folder (candidate 1.5.3)

- User: "first do the directory generation, read Mods directly"; then "do not go into the game unless unavoidable, first do everything that can be done".
- frontend/xxmi/character_core.h/.cpp (CharacterDiscovery, discovery mode), layer/src/character_program.h/.cpp (load/catalogue split, discover, declared_namespace, lazy bone streams), character_package.h/.cpp (adopt_bones, shared bone check), character_backend.cpp (Mods enumeration, INI generations, growing material slots), common.h/util.cpp/layer.cpp/m2_backend.cpp (m2_mods, m2_core, m2_settings, character_configured), sandbox/runtime_fixture.py (namespace), runtime_test.py (directory generation), harness.py (pinned m2_mods off), docs/directory-generation.md, D-078. Version 1.5.3.
- Results: installed Mods folder in the sandbox: 19 of 22 load; live INI 71/71; full rebuilt suite 31/31 steps (build/next/run_all-1.5.3.out).
- Found while checking: no installed INI uses a texture-hash TextureOverride; D-077, texture-identity.md and several-packages.md corrected.

## 2026-10-20: Core source facts and the installed Mods' offline plans (candidate 1.5.4)

- sandbox/character_test.cpp (--plan-mods dry run, --core-facts, unit checks), scripts/build.py (the character test links the program and the original runtime), frontend/xxmi/character_core.cpp (assignments inside if branches), frontend/gpu/source_registers.h/.cpp (block sizes, strides, core_source_facts), layer/src/character_backend.cpp (per-pipeline facts, vertex input to the plan), character_program.h/.cpp (bone element from the pipeline), server/efmi_vk/source_slots.py (stride, core_facts), scripts/check_core_facts.py, docs/core-facts-truth.json, scripts/check_mods_dry_run.py, sandbox/run_all.py (two steps), sandbox/unit_test.py, docs/core-facts.md, D-079, BUG-027. Version 1.5.4.
- Found offline: BUG-027 (the live path never replaced a real Mod); fixed by D-079. The dry run of the installed Mods went from 0 to 137 drawn component sections.
- A suite run was cut off by the session pause; rerun.

## 2026-10-20: 32-bit blend indices, namespaces (candidate 1.5.5)

- layer/src/character_package.h/.cpp (CharacterDraw::blend, bone_index_bytes, 32-bit adoption), character_program.cpp (DXGI 3 accepted, refusal names the edits, implicit namespace and plain settings names), frontend/xxmi/character_core.cpp (namespace check), sandbox/character_test.cpp (4 checks), sandbox/runtime_test.py (implicit namespace), scripts/check_core_facts.py (truth written with LF), docs. Version 1.5.5.
- Dry run of the installed Mods: 21 of 22 load without the owned runtime (snowshine needs it), 144 component sections draw, 2 refused for known limits.


## 2026-10-20: captured frames replayed through the layer (candidate 1.5.6)

- User direction in force: no game session unless unavoidable; do everything offline first.
- scripts/replay_capture.py (new), sandbox/replay_gpu.cpp (new), sandbox/run_all.py (replay step), scripts/check_mods_dry_run.py (thresholds), frontend/xxmi/character_core.h/.cpp (identification threshold, facts output), draw_plan.h/.cpp (DrawPlanAdapter::original), layer/src/character_program.h/.cpp, character_backend.cpp (threshold, image budget, dimension policy, status), common.h/util.cpp (m2_dimension_zero), sandbox/character_test.cpp, harness.py (pinned off), docs/capture-replay.md, D-080, BUG-028, LIMIT-034. Version 1.5.6.
- Debugging path worth keeping: an empty G-buffer in the replay was the replay's own (identity bones leave the Z-up mesh lying outside the view); --probe-vertices proved the replayed pass draws, identity everywhere in the bone buffer proved the Mod's draws do, and the upright stand-in fixed it. The forward pass needed the G-buffer's depth (EQUAL tests) and a clear value chosen from any reversed-depth pipeline of the pass.
- Results: 13 of 16 bundles show the Mod in every pass, 0 validation messages in 32 runs; full rebuilt suite (build/next/run_all-1.5.6.out).

## 2026-10-20: images published in batches (candidate 1.5.7)

- frontend/assets/resource_cache.h/.cpp (prioritize), layer/src/character_backend.cpp (batch publication, per-image gating, status), sandbox/asset_cache_test.cpp, sandbox/character_test.py, scripts/replay_capture.py (--mods), docs/character-materials.md, D-081, BUG-029. Version 1.5.7.
- Measured first: cold DDS reads run at about 940 MiB/s, so a Mod's ~600 MiB is not the bottleneck; the serial one-image pipeline and the all-images publication are.
- Results: see CURRENT_STATE; the broken-image replay used a copy of arcane under build/broken-mods (the installed Mod untouched).

## 2026-10-20: RabbitFX texture roles (candidate 1.5.8)

- User: continue offline; RabbitFX option 2 (texture classification). Also asked whether characters were adapted one by one: answered no; every change is a shared mechanism, captures only check rules.
- server/efmi_vk/texture_roles.py, frontend/gpu/texture_roles.h/.cpp, scripts/check_texture_roles.py, docs/texture-roles-truth.json, frontend/xxmi/rabbit_textures.h (library), character_core.cpp (ResolveRoles native), runtime.h (texture_roles), layer/src/character_backend.cpp (role queries, bound_image, BUG-030, derivation logs), frontend/gpu/material_plan.cpp (BUG-030), sandbox/character_test.cpp (--texture-roles, 8 checks), asset_material_plan_test.cpp (4 checks), scripts/replay_capture.py and replay_gpu.cpp (recorded texture sizes, --small-images), build.py, run_all.py, docs/rabbitfx-roles.md, D-082, BUG-030, LIMIT-004. Version 1.5.8.
- Pitfalls met: an elif chain of 64 branches made the dry run hang (nested else-if walked in both phases); flat if blocks over t0..t31 instead. The mifu shirt went black in the replay: BUG-030.

## 2026-10-20: texture-only draws (candidate 1.5.9)

- Sections that keep the game's mesh and only change its textures now redraw the game's draw with the Mod's textures through a vertex-preserving variant (D-083). Files and evidence in CURRENT_STATE.

## 2026-10-20: hash-only hide sections (candidate 1.6.0)

- A Mod's sections with a hash alone that skip the draw now hide game meshes by the region hash at any count (D-084). The replay selected draws by (hash, count) and missed them at first; it now also selects by hash alone. The test copy of arcane under build/hash-only-mods was removed after the run.

## 2026-10-20: application textures and element offsets (candidate 1.6.1)

- User: continue offline, do every item. Checked the game's SPIR-V for input semantics (none) and the game folder for a source of DX11 shader identity (assets in an encrypted VFS: not a route).
- ember's `ResourceX = ref ps-tN` and chengqianyu's ElementOffset edits now plan (D-085); the dry run of the installed Mods refuses no section.

## 2026-10-20: a Mod's own instance identification (candidate 1.6.2)

- Si Lunabelle's BossGuard INI runs the Core's identification internals itself; the plan now answers the spatial query from the instance entry, DRAW_NUMBER counts planned draws, and the native draw call honours the Core's result (D-086). vk04 replays with the guard; a copy whose guard rejects every object keeps the game's draws. New suite step for vk04.

## 2026-10-20: GPU work inside replaced draws (candidate 1.6.3)

- P2: replaced draws run their CustomShaders and copies into Mod resources as owned work recorded during the plan and submitted before the application's submission; owned buffers bind as vertex streams (D-087). Typhoeus' body and clothes are the Mod's in the vk06 replay; its physics stays off (LIMIT-035).
- The first P2 replay crashed in NVIDIA's shader compiler; a crash stack printer in the replay program and a standalone pipeline tool isolated RoundingModeRTE with a division (BUG-031, D-088).

## 2026-10-20: sections acting on game shaders load inert (candidate 1.6.4)

- ShaderOverride/ShaderRegex sections no longer refuse a Mod folder; they are inert and named in the warnings, `vs`/`ps` read as named by none, `oN` reads the subpass (D-089). Yvonne and Xaihi load in the replays.

## 2026-10-20: game test of 1.6.4 prepared

- User: set the shader-keyed features aside and test what is implemented in the game. Deployed 1.6.4 to dist (backup of 1.0.2 first), directory-mode cfg, and at the user's request switched the EFMI launcher profile to Vulkan (backup, two values, exact readback). The injected library is the user's step in the launcher UI.

## 2026-10-20: first game session of 1.6.4 froze; 1.6.5

- The game froze about a second after Si's package activated (probe frame 1535). Status while frozen: our locks free, owned Present failing since the game moved its presentation to its other graphics queue, uploads bound to the first queue. No driver timeout. Cause not proven (BUG-032).
- 1.6.5 (D-090): owned work follows the presentation's queue, uploads and publications on any graphics queue, `m2_owned = 0` switch, stall watchdog in the log.

## 2026-10-20: second game session froze; 1.6.6

- The user switched through characters; the session froze after eight package activations in 17 seconds. The watchdog showed the submit and present threads inside the driver; the status showed about 3 GiB of our images and meshes, late meshes outside device memory: device memory oversubscribed (BUG-033). The user also saw characters flicker between original and Mod, and Mods missing in the open world: one package per recording.
- 1.6.6: activation waits for device memory to spare (D-092), several packages share a recording (D-093), watchdog stages.

## 2026-10-20: texture residency (candidate 1.6.7)

- Third game session: no freeze; only three characters, the rest waiting for device memory; stutter at zero free memory. The user: DX11 handles twenty Mods, this does not; study 3DMigoto, use Vulkan's features, optimize to the limit.
- Studied CustomResource::Substantiate (load on first use) and the device's features (timeline, memory priority, pageable, sparse, ReBAR). Built the device-wide residency (D-094) with timeline publication and the enabled features (D-095); Si replays fully under a 200 MiB cap with reduced loads.

## 2026-10-20: fourth game session (1.6.7) and 1.6.8

- The user ran 1.6.7: characters turned black now and then, performance still poor, and F10 froze the game (BUG-034). Read: texture thrash from the two-frame eviction rule and the gap between passes of one object; 24 us of layer work per draw from per-package hashing; 26 GB of shadow copies; the F10 freeze with both threads inside the layer's stages.
- 1.6.8: idle-only eviction with paced demotion, a frame gate per package, one index region hash per draw (D-097); the presenting thread never waits for the GPU, controllers held shared during Present work, retired semaphores destroyed later (D-098). Si replay (vk04) with a 200 MiB cap at 360 frames: 11 images resident (2 reduced), 184 MiB, 0 evictions (1.6.7 evicted and reloaded throughout), 1 demotion, 579 budget waits while the set came up, 63 gated frames (2353 draws) before the forward pass was fully covered, 9376 replaced draws, validation clean (`--check` passed); character replacement 357 checks, live INI rendering 76 (the broken-DDS reload candidate refused as before); full suite build/next/run_all-1.6.8.out.

## 2026-10-21: fifth game session (1.6.8) and 1.6.9 through module exchanges

- 1.6.8 deployed at 08:01; the user started the game at 08:06. Reports: two to three seconds to show after a character switch, LOD1 reverts, performance still short. Baseline measured by switching the replacement off through the probe: 21 ms against 41 ms.
- Eight module exchanges (`build/hot-swaps.json`), each measured through the probe: reserve and idle threshold, hash table and fingerprint policy, owner pooling, cached arenas, tools beside the layer, stage and command timing. Frames 43 to 24.5 ms in the same scene (D-099, BUG-035, LIMIT-037, docs/recording-performance.md). The layer's write ranges and the whole are 1.6.9; dist is deployed when the game is next closed.
- Tooling: the Bash tool's sandbox cannot open the probe's named pipe (error 3), PowerShell can; the MCP server keeps the pipe while its session lives, released by asking it for a pid that does not exist.

## 2026-10-21: sixth game session continued; 1.7.0 unfinished at the usage limit

- Module exchanges verified D-100 (identities) and D-101 (material rules) in the game. The user rejected a grey placeholder for textures on their way and set the bar: 3DMigoto's experience exactly, no frame-rate loss. Rewrote resource readiness around the layer's own upload queue with draws waiting (D-103), one variant cache per device with a disk pipeline cache, and the Core's first-frame skip. The layer builds; character_test 35/36 (a validation message in the split-queues cases), runtime_test fails around F10 reloads. Work left uncommitted in the worktree; handoff in CURRENT_STATE.md.

## 2026-10-21: 1.7.0 finished (D-100, D-101, D-103)

- At the user's request (13:05) the launcher's EFMI profile was switched back to DX11 (the two values only, backup and manifest in build/backups/launcher-dx11-20261021-130515); the injected library stays the user's to set back.
- Resumed from the handoff. The live INI scene accepted the broken-DDS reload candidate because the D-103 loader retry matched every failure reason containing "budget", the DDS decoder's size refusal among them, and retried the file until the test wrote the good one back (1432 file reads). The retry now covers only the aggregate budgets (a BudgetBusy exception), behind the rest of the queue and bounded to ten seconds. The split-queues validation message (the texture timeline destroyed with a queue wait on it) is gone with the timeline retired and vkDeviceWaitIdle before the final destruction.
- The full suite passed on the final source (build/next/run_all-1.7.0.out: 35 steps; character replacement 357/357, live INI rendering 76/76 with the broken-DDS candidate refused, pipeline variants 63/63, Vulkan validation 76/76, both replays clean). Committed as 1.7.0; main fast-forwarded; dist deployed at 13:40 with the game and launcher closed (the 1.6.9 files and the sixth session's log in build/backups/before-1.7.0-*).

## 2026-10-22: launcher back to Vulkan

- At the user's request (10:30) the launcher's EFMI profile was switched back to Vulkan (the two values only, backup and manifest in build/backups/launcher-vulkan-20261022-103027); the injected library is the user's to set to dist/efmi_vk_inject.dll before the next Vulkan session with 1.7.0.

## 2026-10-22: seventh game session (1.7.0) and 1.7.1

- The user started 1.7.0 (PID 20568, 10:32). Reports in order: a Mod's key panel flickers in fullscreen (BUG-036, found through the presentation counts: four presentations per rendered frame, the Present work only on rendered ones; fixed by the replay D-104, exchanged into the game and confirmed); a Mod folder swapped for another not taken by F10 (BUG-037, the folder rescan, exchanged); the frame rate (measured: 91 fps with the replacement against 120 without; 3.9 ms of recording per rendered frame, 2.0 ms of it the INI plan, and 1.7 ms of Present work per presentation, cut to 0.5 ms by sampling the input once); some keys and the cloth variants of Typhoeus wrong (BUG-038: the 32-bit blend-index layout of its clothes pipeline, found by replaying vk06 with persistent variables; fixed and exchanged); the new Yvonne Mod without its default clothes (BUG-039: the old Mod's settings file inherited; settings per Mod folder, D-105). Si's package was refused all session because its INI (edited by the user that morning, BossMod) queries game shader declarations (LIMIT-036).
- The full suite passed on the final source (build/next/run_all-1.7.1.out: 35 steps; character replacement 357/357, live INI rendering 76/76, pipeline variants 63/63, Vulkan validation 76/76, both replays clean); the vk06 replays with the control and the variant settings drew Typhoeus' clothes and stockings. Committed as 1.7.1; main fast-forwarded; dist deployed at 12:29 (the game closed).
- Next: the INI plan's 2 ms per rendered frame (command dispatch decided once at adaptation), bones on the GPU, the owned-work access to the game's bindings for Typhoeus' physics (LIMIT-035).

## 2026-10-22: the recording path cut offline (1.7.2, D-106)

- The user: aim for 120 fps. Two profiling builds added; the perlica replay profiled by command class and by phase; six cuts to the planner's frame around the commands (kinds decided once, native commands unwrapped, pointer caches in the Core extension, a per-generation variant lookup, plan items by resource object, fingerprint backoff). perlica replay (vk14, 120 frames), per matched draw: list execution 33.2 to 14.5 us, the whole plan 41.5 to 22.4 us; a package's Present 76 to 36 us per presentation; the 22 packages' Present lists 0.48 ms per presentation (with the Core's GUI text of the first minute).
- The full suite passed on the final source (build/next/run_all-1.7.2.out: 34 of 35 steps at the first run, the character replacement step failing two texture-timing checks of one scene (DDS uploaded once, publication reaching the queue) that passed on their own right after and in every earlier run: a timing flake of the upload queue to watch, not of this change; character replacement 357/357 on the rerun, live INI rendering 76/76, pipeline variants 63/63, Vulkan validation 76/76). Committed as 1.7.2; main fast-forwarded; dist deployed at 14:16 (the game closed). In-game numbers to be read in the next session.
- D-107 (whole-frame hashing only for an activated package with hash-only sections) released as 1.7.3: suite 35 steps (build/next/run_all-1.7.3.out), deployed at 15:35 with the game closed. Bone-stage phase timers added to the profiling builds (bones read, bones import).

## 2026-10-22: a sampling profiler and 1.7.4 (D-108)

- Wrote scripts/sample_profile.py (no external tools: ctypes, dbghelp, the PDBs) and profiled the perlica replay without validation after the first minute. Found and cut: the D-104 snapshot taken for every package, allocations in the record path (a sixth of it), repeated pixel slot lookups, a command pool and fence per GPU submission. Interleaved A/B against 1.7.3 on the perlica replay without validation, three rounds each, medians per frame (the machine under another game's load, so the ratios count): recording 1977 to 1621 us (0.82), the INI plan 1224 to 972 us (0.79), the 22 packages' INI Present 806 to 567 us (0.70), Typhoeus' Present GPU submission 107 to 64 us (0.60), the replay's frame interval 0.94.
- The full suite passed (build/next/run_all-1.7.4.out, 35 steps). Committed as 1.7.4; main fast-forwarded; dist deployed at 17:46 with the game closed.

## 2026-10-22: review of Claude's performance commits (Codex)

- User requested review of the performance optimization, not another implementation or game test. Reviewed committed b5c12c3 and predecessors through 1.7.2, excluding and preserving the main tree's uncommitted 1.3.7 work. No runtime code, layer build, deployed file, launcher or process changed.
- Recomputed the saved six-run replay A/B: recording -18.0%, INI plan -20.6%, INI Present -29.6%, owned submission CPU -39.7%, host interval -6.5%. Concurrent game load and differing frame times limit the interval conclusion; no 1.7.4 game acceptance against 120 fps. Inspected the existing 35-step all-passed suite log without rerunning it.
- Three open findings, each checked in native CPU probes: BUG-040 stable positive index hashes remain stale for 15 frames after simulated coherent mapped reuse; BUG-041 all 256 retired variant stand-ins remain owned by the new lookup; BUG-042 all three evicted image stand-ins remain owned by persistent material scratch. Exact committed index/retirement functions and committed ownership types compared at 56d2634, 27da667 and b5c12c3. These are isolated source/ownership results, not GPU-memory measurements or game reproductions.
- Four-second first-use waits remain by D-103; the saved development replay includes a 158410 us texture wait (479066 us total). Bone imports and Present lists remain CPU work. Findings, fix directions and acceptance scope: docs/performance-review-1.7.4.md. Generated probes, results, A/B calculations and evidence fingerprints: build/review-performance-1.7.4/. Memory updated; review documents verified by the text check and diff review before the local commit.

## 2026-10-23: launcher to DX11

- At the user's request (10:00) the launcher's EFMI profile was switched to DX11 (the two values only, backup and manifest in build/backups/launcher-dx11-20261023-100015); the injected library, still dist/efmi_vk_inject.dll, is the user's to set back to the EFMI MCP probe for a DX11 start.

## 2026-10-24: 1.7.5 review fixes verified offline

- User requested all three fixes, continued performance optimization and complete offline tests before the game boundary. Created build/performance-worktree on codex/performance-1.7.5 from fd5592b to preserve main's older uncommitted 1.3.7 work.
- BUG-040 restores per-draw positive fingerprints; BUG-041 retires fast variant lookup owners; BUG-042 clears scratch owners on every exit and retains capacity. The native tracker fixture now calls production paths directly: 14 new checks, 158/158 total.
- Full build into build/performance-next; all 35 sandbox/run_all.py steps passed (build/performance-run-all-1.7.5.out), including both recorded-frame replays, character 357/357, live INI 76/76 and Vulkan validation 76/76. An initial build wrapper failed printing localized compiler output; rerunning with Python UTF-8 completed. No source/compiler error was suppressed.
- Kept build/performance-baseline-1.7.5 for the next A/B. No deployment, real-game load or launcher change. Next: batch first-use resource preparation and bone staging optimization, then remeasure and rerun all offline tests.

## 2026-10-24: 1.7.6 optimization, offline acceptance and deployment

- Continued autonomously after 491107f. D-110 batches selected image/variant requests before bounded waits, uses resource completion notification and retirement/shutdown notification, and reuses SSE2 two-pose bone staging. BUG-043 removes an Image vector reference across an unlocked wait. No per-character runtime data or changed resource budgets.
- Full build and all 35 offline steps passed: native 158, bones 123, variant cache 34, character 361, live INI 76, Vulkan validation 76, GPU INI resources 11,189 and both frame replays. Standalone resource/cache GPU checks: 266, zero validation errors/warnings. Logs: build/performance-build-1.7.6.log, build/performance-run-all-1.7.6.out, build/performance-resource-wait-1.7.6.log.
- Seven CPU importer A/B rounds against corrected 1.7.5: 86.0-89.1% lower medians; identical checksums. This measures cached CPU memory. Three replay pairs each for vk03/vk04 (2600 frames, 300 warmup): recording -1.5%/-2.3%, bones -50.7%/-36.7%, initial texture waits -7.6%/-17.0%, same final target bytes, replacement coverage and residency ledgers; no waits after warmup. Host p50 unchanged on the FIFO replay. Reports under build/performance-worktree/build/bone-bench/ and build/performance-worktree/build/performance-ab/20261024-152518/.
- An additional three-pair vk03 comparison against deployed 1.7.4 records the correctness cost: 412.30 to 425.09 us/frame (+12.79 us, +3.1%), fingerprint reads 3.58 to 29 per frame. First-use image waits 241.03 to 125.35 ms; host p50 5531 to 5537 us. No claim of lower overall steady CPU than 1.7.4 or game FPS acceptance. Report: build/performance-worktree/build/performance-ab/20261024-152920/; analysis in docs/performance-followup-1.7.6.md.
- Deployed 18 tested binaries/manifest to dist after two checks that game/launcher/test processes were closed; every file hash matches. Backup: build/backups/before-1.7.6-20261024-153223/. Configuration unchanged; launcher read-only verification still DX11/DIRECT. Actual-config sandbox injection loaded 1.7.6 and 23 packages with replacement/profiling enabled, exited normally (build/performance-smoke-1.7.6.json). No game start/load or launcher edit.
- Replayed five historical saved game inputs (283 records, 74 imports, 209 repeats) through the deployed bone importer and independent reference: every entry/master equal and output matches the earlier verified hash (build/performance-bone-replay-1.7.6/report.json). No new capture or real-character rendering is claimed.
- Main's 21 older changed/untracked files are retained byte for byte (build/performance-main-wip-before.json). Stop at the requested boundary: real-game timing and transitions, dashes, F10 and long-session resource ownership require a user-started Vulkan session. The 120 fps target remains open.

## 2026-10-24: EFMITTL owned execution and attachment probe, candidate 1.7.7

- User requested inspection/adaptation of the installed EFMITTL folder. Inspected its unchanged development 0.5.2 INI, seven shader programs/caches and existing consumers. Full game support is absent: external library sharing, active game shader hooks and ordered capture/composition against borrowed application state remain (LIMIT-035/036/038). D-111 records this boundary; no per-character mapping was introduced.
- BUG-044: the unchanged cached alpha pixel shader reproduced rejection of SPIR-V DerivativeControl. Accepted that core capability with existing stage/instruction validation intact; diagnostics now identify the refused capability/stage.
- Added scripts/check_efmittl.py and an owned headless GPU fixture. Cached DXBC and source HLSL paths each pass 24 cases/356 checks, with zero Vulkan errors/warnings through shutdown. The fixture confirms eight cached stage translations versus zero, checks all pixels/depth against independent references, public state restoration, repeat frames and final resource ownership. Covers layers/overflow, alpha/masks/mips, scene depth, plain/fading shadows and nonzero instances, missing-hook fallback/recovery, resizing and neutral screen-space mask fill. All 14 installed source/cache files retain their hashes. Report: build/performance-worktree/build/efmittl-gpu/report.json.
- Added capture-only framebuffer/attachment/subpass tracking, imageless/dynamic descriptions, secondary inheritance/rebasing and stable swapchain image IDs. Snapshots retain metadata only. MCP get_draw resolves the context and marks old/missing contexts unknown. Native tracker 178/178; Python units after the reader change 117/117; MCP 50/50.
- Built 1.7.7 in build/efmittl-next. First build exposed a fixture enum-to-flags narrowing conversion, corrected explicitly. Full 36-step suite completed: 35 pass, probe step fails only its old image-count assertion (now includes the three swapchain images). Updated the assertion to use the reported chain size and reran that step: all 17 configurations pass. No full-suite result was relabelled; retain build/efmittl-run-all-1.7.7.out and build/efmittl-probe-1.7.7.out. Resource GPU 11189, both historical frame replays, character 361, live INI 76, validation 76 and evidence 113 pass.
- Work is isolated on codex/efmittl-support in build/performance-worktree because the main tree's 21 older WIP files include an overlapping shader executor edit. Every old file hash remains identical (build/efmittl-main-wip-check.json). Deployed 1.7.6's 18 files still match build/performance-next and the configuration SHA-256 is unchanged (240d9deb70be415add04cbaf97271a5f81467259da3232227f66e473700f5dc8).
- At the initial verification checkpoint the game was present and no deployment had occurred. Existing frames lack scene target aliasing/depth-copy ordering; obtain those facts before the shared game pass implementation. Candidate does not yet make EFMITTL transparent in the game. docs/efmittl-compatibility.md.
- Implementation committed as 776e22e. At final preparation the game and launcher had closed without agent action. Verified closure twice, backed up the 1.7.6 files/cfg/log to build/backups/before-1.7.7-20261024-213603/, and deployed all 18 tested candidate files. Exact hashes match; cfg remains 240d9deb70be415add04cbaf97271a5f81467259da3232227f66e473700f5dc8. Manifest: build/efmittl-candidate-1.7.7.json.
- Actual-config sandbox injection loaded 1.7.7 with 23 packages and replacement/profiling enabled, captured known attachment data and exited normally: build/efmittl-smoke-1.7.7.json and build/efmittl-smoke-capture-1.7.7.json. This is a sandbox capture only. Launcher read-only check still DX11/DIRECT. No game start/stop/load, module exchange, launcher edit, installed Mod/Core write or system-wide configuration change. Next requires an on-the-spot launcher-switch request and user-started Vulkan capture; game integration remains open.

## 2026-10-24: EFMITTL consumers drawn again (1.7.8)

- User: focus the review on the EFMITTL adaptation, then "finish the unfinished yourself". The two WIP snapshot branches are archived as bundles and patches (build/backups/wip-branches-20261024/) and deleted; the merge of codex/efmittl-support was replayed as a linear history with an identical tree (c485787, 188e17d, 192da15) and main pushed to origin, at the user's question why two branches showed.
- Found BUG-045: the parts Mods draw through EFMITTL were not drawn on Vulkan at all (the library was never in a character generation). D-112: libraries called by namespace are loaded into the consumer's generation; plans answer declaration operands. Replay vk01 (Yvonne, 120 frames), 1.7.7 against 1.7.8: covered texels shadow 162226 to 173968, G-buffer 85342 to 92233, forward 44179 to 71365, no refused plan (575 refusals before the declaration answer); vk02 (TangTang) replays with no refused plan.
- Full suite 38/38 steps passed (build/run_all-1.7.8.out): original INI runtime 488, character 361, live INI 76, Vulkan validation 76, GPU resources 11189, EFMITTL owned 356 per path, probe 17 configurations, MCP 50; both earlier replays and the new vk01 library step pass.
- Deployed the 18 tested 1.7.8 files at 22:46 after two closed-process checks (the game and the launcher closed); hashes match, cfg unchanged; 1.7.7 files in build/backups/before-1.7.8-20261024-224635/ (build/candidate-1.7.8.json). Actual-config sandbox injection: 1.7.8, 22 packages, the library loaded by its 3 consumers (Yvonne, TangTang, ardelia), no refusal, normal exit, clean validation, game log unchanged (build/smoke-1.7.8.json). No game load; the launcher stays on DX11.

## 2026-10-25: EFMITTL translucency in the renderer (1.7.9)

- Continued the user's "finish the unfinished yourself" after 1.7.8. Offline facts first (paired catalogue): the Vulkan lit vertex shaders keep DX11's output signature less pruned outputs, the shadow ones output no UV, the forward pass is one subpass with unchanged layouts and the copy shader into R32F follows it. D-113: library policy, layer facts and GPU work; docs/efmittl-transparency.md.
- Built: pass end callback (ABI), viewport/scissor and pass instance tracking, the bridge (hook facts, copy hook, native capture), translucent plan items, capture variants, device layers, deferred recording and composite, replay support for the copy pass. Replay vk01 (Yvonne, 120 frames, --copy-passes): 456 captures, 114 composites, the library's $fallback staying 0, Vulkan validation 0 errors (4 performance notes); against 1.7.8's opaque fallback 30265 forward-target pixels differ, the sleeves (alpha 0.2) most, the dress (0.7) less (build/replay-1.7.9/). vk02 (TangTang, translucent variant on): 114 captures, 114 composites. Hook rules on the paired catalogue: the 7 Vulkan vertex shaders pairing with 1479b2b5 lit, 57 others not; copy rule names 4b4220a1 alone. Sandbox only.
- Full suite 39/39 steps passed (build/run_all-1.7.9.out), with the vk01 step checking translucency and the new hook rule step.
- Deployed the 18 tested 1.7.9 files at 2026-10-25 11:21 after two closed-process checks; hashes match, cfg unchanged; 1.7.8 in build/backups/before-1.7.9-20261025-112124/ (build/candidate-1.7.9.json). The injected DLL and the module changed ABI together. Actual-config sandbox injection: 1.7.9, 22 packages, the library loaded by its 3 consumers, normal exit, clean validation (build/smoke-1.7.9.json). No game load; the launcher stays on DX11.
- 2026-10-25 15:50: at the user's request the launcher's EFMI profile switched to Vulkan (only the two values; backup build/backups/launcher-vulkan-20261025-155010/). The injected library still names the EFMI MCP probe; the user sets it to dist/efmi_vk_inject.dll.

## 2026-10-25: the first Vulkan game session of 1.7.9 and 1.8.0

- User-started session PID 19920 with 1.7.9: translucency working; characters reverting in the open world, one to two seconds when switching back. Status snapshots build/game-1.7.9-status-1.json and -2.json: free memory 188 MiB then 0, 214832 budget waits, 317 evictions, Yvonne and TangTang with no image resident (BUG-047).
- The user asked whether every character and every Mod is supported now: no; RabbitFX glow/FX/hue, game-shader sections other than EFMITTL, owned work reading the game's bindings remain (LIMIT-004, LIMIT-036, LIMIT-035).
- The user installed a Si Mod with a full-width colon in its folder name and pressed F10: the game ended in dist/dxcompiler.dll (16:08:39); started again, it ended at 16:09:37. Reproduced offline with shader_inspect (any character outside ASCII in the path) and with the actual-config sandbox injection (BUG-046). "So it is still not general" (user): the fix is general (fxc as the reference for what runs, aliases for DXC, a contained fault, the original's skip of a failed CustomShader), D-116.
- 1.8.0 built in the worktree .worktrees/fix (detached at 380ac53) with D-114 to D-116; full suite passed; actual-config sandbox smoke: 1.7.9 faults, 1.8.0 runs (build/smoke-1.8.0-candidate.json, build/smoke-1.8.0-api13.json in the worktree). The new Si Mod is refused at its skeleton (BUG-048, next).
- Deployed 1.8.0 at 17:27 with the game and launcher closed (build/candidate-1.8.0.json); post-deployment smoke build/smoke-1.8.0.json.
- 1.8.1 (BUG-048): skeleton components with bones between them and a 1024-image package budget; the new Si Mod configures in the actual-config sandbox at --api 1.3.
- Deployed 1.8.1 at 18:38 with the game and launcher closed (build/candidate-1.8.1.json); post-deployment smoke build/smoke-1.8.1.json. No game load by the agent in this round.

## 2026-10-25 (evening): the 1.8.1 session, the performance analysis and 1.8.2

- User-started session with 1.8.1 (PID 44092): "it stutters badly, and the Si Mod does not show". Status snapshot build/game-1.8.1-status-1.json: translucency working; the Si package refused at its arenas (BUG-049); 5 ms of layer work per frame, bones 17-33 us per matched draw from write-combined memory; the game CPU-bound (4 cores, GPU 68%). An 8 s on/off A/B was inconclusive (the scene changed). The user: think about performance as a whole, not per Mod.
- Analysis written for the user (steady cost, hitches, memory, the game's own cost); offline measurements: footprint 565 MiB for 22 packages (ISSUE-011), replay stage costs (bones 1 us offline against 17-33 in the game: the bus). Decision D-117: the bone import on the GPU as the Core does; D-118: budgets from the Mod's declarations.
- Built 1.8.2 in the worktree: bone_merge.cpp and the compute shader, append_external, ring slots held by recordings, one submission per frame before the game's, probe `set gpu_bones`, arena_slices (BUG-049), the crash logger. The layer-entry tests crashed in the validation layer until vkSetDeviceLoaderData was called on the bone command buffers (the crash logger named the module). Replays vk03, vk04, vk01: pictures identical with GPU and CPU bones; character 361/361; live INI 76/76.
- Full suite 38 steps passed; vk01 six times clean. Deployed 1.8.2 at 20:57 with the game and launcher closed (build/candidate-1.8.2.json); post-deployment smoke build/smoke-1.8.2.json. No game load by the agent.
- Session with 1.8.2 (PID 19260): deadlocked on the GPU at 21:01 (BUG-050, the cross-queue wait against D-091); the user ended the process. Also found: the Si Mod's draws all refused for rt_width (BUG-051). Both fixed for 1.8.3.
- 1.8.3 built, suite passed, deployed at 21:26 with the game closed (build/candidate-1.8.3.json, smoke build/smoke-1.8.3.json). Main's uncommitted WIP touches frontend/xxmi/runtime.cpp, which 1.8.3 changes too: merged by the WIP-protect procedure (patch saved under build/backups/).
- Session with 1.8.3 (PID 35560): the GPU hung two minutes in and the driver crashed (BUG-052). The user: think it through before changing; the sandbox tests tested nothing; follow 3DMigoto; the first frame must be the Mod and a loaded Mod must never show the original. dist/efmi_vk.cfg set to `m2_gpu_bones = 0` so the game plays on the CPU import. 1.8.4 built: per-recording dispatches on the recording's queue (D-117 amended), the two-queue replay and the GPU/CPU picture check in the suite (D-119), waiting instead of gating (D-120).
- 1.8.4 deployed at 22:35 (build/candidate-1.8.4.json, smoke build/smoke-1.8.4.json); GPU bones off in the configuration until switched on in a session through the probe.
- Session with 1.8.4 (PID 26184, user-started): GPU bones watched 60 s and measured with the interleaved A/B (13.8 against 16.6 ms mean frame); two module exchanges for Si's rt_width (the second, with the -3 marker, at 22:53); a third exchange with the reason logging was not made because the user had closed the game (normal exit, Player.log). The user: continue the restructuring (D-121); the play configuration without the probe waits until the rest is done; keep the Si Mod moving at the same time. Offline: BUG-053 reproduced and fixed, D-122, 1.8.5 built.
- 2026-10-26: 1.8.5 suite 41/41, deployed 09:42 (b117cfb), cfg `m2_gpu_bones = 1`, smoke clean. Prepared for 1.8.6: the probe's sampler (`profile`, scripts/profile_game.py), per-thread hook counters with cache-line slots, draw records moved into the list instead of copied.
- 2026-10-26: 1.8.6 suite 41/41, deployed 10:06 (2d00ea7), smoke clean. Main fast-forwarded and pushed after each version (1.8.5 with the WIP-protect procedure: runtime.cpp's WIP conflict resolved by keeping both, sizes match the backup build/backups/wip-before-1.8.5-20261026-094343/).
- 2026-10-26: profiled the Si NightveilTrancer replay (outside sampler, phase and command timing builds): about 28 us per matched draw, 18 of them the original interpreter. 1.8.7 (layouts interned, texture roles kept, entry looked up directly): replay CPU -5.7% against 1.8.6, suite 41/41, deployed 11:26 (de43c4d), smoke clean. Next: the in-game profile in the next session, then D-121 step 4 by its figures (the interpreter's results kept while their inputs hold) and step 3.
- 2026-10-26: 1.8.8 (engine lock contention counted), suite 41/41, deployed 11:50 (1ac78cf), smoke clean.
- 2026-10-26: the 22-Mod replay showed the engine lock contended by the generations' save workers; 1.8.9 saves only when dirty (0 waits), suite 41/41, deployed 12:26 (3731999). The Core adds no Present work per generation (only a disabled debugger branch); the per-generation Present floor (about 3 us) is the runtime's own call overhead.
- 2026-10-26 13:07-13:32, user-started sessions with 1.8.9 (PID 45144, then 36252 after the user restarted; both ended normally, Player.log): the Si Mod drew without legs with GPU bones (CPU fine), the character screen missed Mods (memory gate) and showed the game's model first. Module exchanges (build/hot-swaps.json): 13:15:56 and 13:21:42 into 45144 (no memory gate; plus the hold), 13:31:31 into 36252 (the same), a fourth at 13:32:09 not taken (the probe pipe closed as the game was closing); they were quick fixes; the user: no temporary changes, fix properly and test. 1.9.0 then built offline with a failing check for each problem first.
- 2026-10-26 afternoon, offline: 1.9.0's suite failed only on the offline plans: the user had installed the Tangtang Mod, whose lists ask `vs->srv_stride(0)`, bind `vb3 = vb0` and give the blend edit twice (BUG-057). The DXBC of the paired catalogue showed which t declarations SPIR-V decides (1555 of 1555 answers equal; word buffers and slots beyond the SPIR-V's registers not decided; D-124). Each step with a check failing first (TangTang replay with only the Mod: 3420, then 2736 refusals, then none; the planner host test; the runtime fixture). Suite 45/45.
- 2026-10-26: 1.9.0 deployed 16:03 (3bdf01b), smoke clean (22 packages, Tangtang among them).
- 2026-10-26 16:16-16:37, user-started session with 1.9.0 (PID 20392, DLSS Frame Generation on): Si, Tangtang, Yvonne and arcane replaced, bones complete, no game mesh before the Mod. The user reported the Ctrl+K panels stuttering; status polling (2 s) and profiles showed the per-frame settings saves under the engine lock (BUG-059) and texture first-use waits; at 16:36:48 the game froze (BUG-058, deadlock between Streamline's present thread in vkAllocateDescriptorSets and the game's submission), the user ended it. No module exchange in this session. The layer also logged a first-chance access violation inside EndfieldBase.dll at 16:16:43 before Vulkan loaded; the game continued (its own handled exception). 1.9.1 built offline with failing tests first.
- 2026-10-26 evening, offline: 1.9.1 (BUG-058, BUG-059, D-125), suite 45/45. The user asked for a README update (done) and for a shader cache like 3DMigoto's; assessed: the Mods' shipped DXBC caches and the variant pipeline cache exist, missing are a disk cache of the layer's own HLSL/DXBC translations, a pipeline cache for the owned runtime's pipelines and an in-memory cache of SPIR-V facts. Next version.
- 2026-10-26: 1.9.1 deployed 17:33 (7c97e62), smoke clean.
- 2026-10-26 evening, offline: the shader cache (D-126, 1.9.2). Load-time shader costs measured first through new per-package counters; the cache and its tests (compiler cases, owned Present second start) failing first, then passing; 22 Mods load in 1.66 s instead of 2.41 s with a warm file system cache. The runtime's forced port header turns GetModuleFileNameW into a shim that names the 3DMigoto folder for its own DLL; the cache calls the real function.
- 2026-10-26: 1.9.2 deployed 18:27 (d3f6f87), smokes clean.
- 2026-10-26 18:33-18:5x, user-started session with 1.9.2 (PID 13144, DLSS Frame Generation on): the shader cache read 33 stages and compiled a new Mod's 12; Typhoeus' wings on the ground with GPU bones (BUG-060), fine on the CPU (probe switch at 18:45:23, diagnosis); Frame Generation off and on, then an F10 exchange at 18:47:33 with 20 packages brought up twice and the game slow (BUG-061). A first-chance access violation in the probe pipe thread at 18:41:53 (ntdll under ACE-Base64.dll, handled; not investigated further).
- 2026-10-26 evening, offline: the user asked to think the designs through and solve the problems at the root; docs/design-review-1.9.md (causes A to E and the order of work). Step 1: the rolling-bones replay reproduced the in-game collapse on 1.9.2; D-127 implemented (1.9.3), the replay identical with GPU and CPU bones.
- 2026-10-26 evening: 1.9.3 committed (535488b), main fast-forwarded and pushed. Step 2: BUG-061's textures shared by file identity (1.9.4); the check failed on 1.9.3's binaries (kept in build/dist-1.9.3 of the worktree) and passed on 1.9.4.
- 2026-10-26 20:30-20:5x, user-started session with 1.9.4 (PID 14500): wings fixed in the game; the user added a non-merged-skeleton Mod (not loaded, by design so far) and replaced Typhoeus' Mod; the F10 at 20:46:26 froze the game (BUG-062): the removal stopped the package on the presenting thread and waited for its Present work. The user: regressions as short as possible; first look at how 3DMigoto solves a bug, then build the Vulkan fix on it (AGENTS.md, memory). 1.9.5 fixes it the original's way (D-128).
- 2026-10-26 21:32-21:3x, user-started session with 1.9.5 (PID 11492): the same F10 froze again. The user: it may be that Mod. Found BUG-063 (an exponential walk of if bodies at load, under the engine lock); 1.9.6 fixes it the original's way (each list once). BUG-062 was a real hazard but not the cause of either freeze.
- 2026-10-26 22:0x, user-started session with 1.9.6 (PID 8552): the F10 freeze gone (the user: seems fine now); SiModGB drew nothing (BUG-064). 1.9.7 takes its declarations and texture queries as the original does (IASetIndexBuffer by format, FillInMissingInfo's vertex stride, FindTextureOverrides in plans).
- 2026-10-26 late, offline: 1.9.7 deployed 23:09 (b84ac88), smoke clean. Then the engine lock refactor the user asked for after SiModGB: D-129 (1.9.8), each generation's original engine state its own; the concurrency test failing on 1.9.7 first; the whole suite and an offline A/B (no cost).
- 2026-10-27, offline: 1.9.8 deployed 23:45 (the day before). The texture load path (review step 3): measured against the original's DirectXTK load on D3D11 first, then D-130 (1.9.9): one 64 MiB map 43 to 20 ms (original 28), startup texture waits in the vk03 replay 120 to 46 ms.
- 2026-10-27: 1.9.9 deployed 09:49 (eb32082), smoke clean. Step 4 examined: synchronization validation on the two-queue replay of the Present-compute Mod shows only the harness's own hazards; no change without a failing check.
- 2026-10-27: the user: finish all offline work first and keep main clean. main's foreign uncommitted work preserved on branch wip/source-shader-regex-gpu (b9ed970) and a patch, main cleaned, fast-forwarded and pushed. D-131 step 1 (one generation for the Mods folder, frontend): equivalence check failing first, then passing for 19 of 20 Mods; LIMIT-041 found.
- 2026-10-27: D-131 step 2 (2.0.0): the layer on one folder generation; vk03 A/B (switch off/on) pictures identical, presentation work 221 to 146 us per frame; isolation and the empty folder covered by the live INI and owned Present tests.
- 2026-10-27: 2.0.0 whole suite 49/49, committed (5695edf), main fast-forwarded and pushed, deployed 12:14 with the game closed; smoke: 20 character Mods in the folder generation, 0 left out, loaded in 2.4 s, validation clean.
- 2026-10-27: D-131 step 5 (2.0.1): payloads released once validated, read again from the files. First version read with one thread: upload 16 ms, one frame more of skipped draws in the vk03 A/B; fixed by reading stream groups in parallel (4.5 ms). Private memory 712 to 298 MiB in the actual-config sandbox.
- 2026-10-27: LIMIT-041 lifted in 2.0.2 (D-132): a section per `vbN->stride` branch, matched by the draw's pipeline binding; dry run with no refused section; the scene's device-side check verified by disabling it once (fails) and restoring it.
- 2026-10-27: 2.0.2 deployed 13:56 (979feef) with the game closed; smoke clean (20 Mods, 0 left out, private 306 MiB).
- 2026-10-27: 2.0.3: F10 keeps unchanged meshes (BUG-061's mesh part). The scene check failed on 2.0.2; the first implementation still uploaded because a discovered package's stream order followed the parse's resource order, which differs between parses (streams now in file order), and because a package that is not lazy activates its successor inside init on the loading thread (generation_create now gets the live generation).
- 2026-10-27: 2.0.3 deployed 14:43 (f6d0724) with the game closed; smoke clean (20 Mods, 0 left out, 2.0 s, private 308 MiB).
- 2026-10-27 15:19-15:4x: user-started game session with 2.0.3 (PID 25552), probe status only, no module exchange. SiModGB replaced; 117 fps mean; Typhoeus refused about half its draws (BUG-065). The user, after closing the game: fix offline; DX11 has no such problem, so the design was at fault (the layer demanded the Core's LOD number equal its own instead of what the Core does with it). 2.0.4 fixes it; also found the settings adoption naming (Mods' panel INIs) and RabbitFX's anti-dither being inert.
- 2026-10-27: 2.0.4 deployed 16:06 (229a801), smoke clean. 2.0.5: BUG-066 (panel settings adopted under the folder's names; scene checks failed on 2.0.4 first). Deployed 16:16 (81a422f), settings backed up first; the smoke migrated the real settings file.
- 2026-10-27: D-133 (RabbitFX's anti-dither) designed from the original and the captures; step 1 (the SPIR-V rule and the patch) done: 38 dithered captured modules, all patched and valid. Step 2: the gate from ShaderRegex replacements in a loaded library (scene 96/96); found that RabbitFX itself is the layer's native interface (D-082), which lacks the key, the persistent toggle and the gate; now the installed file's key and ShaderRegex sections join the interface and `$censor` persists (scene 95/95, vk03 reports 172.w).
- 2026-10-27: the user: finish the offline work. D-133 steps 3 and 4 (2.0.6): the patch in replaced draws' variants and twin pipelines bound for the application's draws; the scene with a dithering application shader passes with `$censor` 1 and fails with 0. First version failed three scene checks (gate read only at the first presentation, twins requested without waiting, twins tied to the replacement switch); fixed after the original's behaviour. Whole suite passed; deployed 18:32 (52b16c2), smoke clean.
- 2026-10-27: LIMIT-042 fixed in 2.0.7 (settings named outside ASCII saved as UTF-8, as the original; the original's unknown-settings keeping is not needed: the user's d3dx.ini has clear_unknown_settings = 1).
- 2026-10-27 evening: game session on 2.0.7 (user-started). Found: anti-dither removed the field's alpha tests (BUG-071), its key slow (BUG-072), F10 after Mods were replaced ended the game (BUG-067, from the Unity crash dump: system_error errc 3 from a double join). The user: consider every operation around F10 (docs/f10-operations.md, D-134; BUG-068 to BUG-070). All fixed in 2.0.8 test first; character toggle keys felt slow too: open, to measure in the game. 2.0.8 deployed 20:48 (d532cb3), smoke clean.
- 2026-10-27 night: 2.0.8 in the game (user-started): two F10 with Mods added and removed, anti-dither on, no stall; the user: basically no problems left. Release 2.0.9 prepared (D-135: GPLv3, no probe, playable files only); merging the DLLs into one considered and declined. Published as v2.0.9 (GitHub release, English notes); the user will download and test it.
- 2026-10-27 night: the user unpacked v2.0.9 into EFMI\Mods: no Mod (BUG-073); moved into EFMI it worked. 2.1.0: the EFMI folder found from the release's folder, its own folder never Mod content, foreign handled faults not called crashes; release check extended (failing on 2.0.9).
- 2026-10-28: an AMD player's 2.0.9 log: the layer loaded, 0 of the loader's exports found, no Mod. The user asked for a review of the whole project. 2.1.1: the loader behind a proxy vulkan-1.dll (BUG-074, the AMD log line reproduced in the sandbox with a generated forwarding proxy), character meshes copied into device memory where the host's window is small (BUG-075, simulated window), LIMIT-045 recorded; check_release no longer depends on a removed Mod. Sandbox and release check passed; not run in the game.
- 2026-10-28 afternoon: tangtang Ramune v1.9 broken in the game (BUG-076: a texture file the Mod does not ship refused the draw; fixed as the original, a null binding). The user: the INI handling keeps failing, think it through, general; then: do all of it. D-137 and docs/ini-semantics.md: the original as specification and judge, degrade as it degrades, show what is degraded.
- 2026-10-28: 2.1.2, the overlay's notices as 3DMigoto's (F10 reload time, refused reloads, the layer's refusals once per Mod), drawn after the Present work; sandbox checks failing before, passing.
- 2026-10-28: every installed Mod in every key state (check_mod_states.py), BUG-077 (broken buffer files degrade per draw), the draw-list cache starts again when full; the freezes in the logs traced to BUG-062/063 (1.9.4, 1.9.5) and BUG-067 (2.0.7), none of 1.5 s in 2.0.x since (docs/ini-semantics.md).
- 2026-10-28: D-138, sections matched by a resource's description: texture ones run against the layer's description of the game's image, the rest inert and reported per Mod (CharacterDiscovery::gaps).
- 2026-10-28: D-139, image files load as the original's LoadFromFile (WIC for every file not named .dds; a file that does not load is null in owned work and replaced draws).
- 2026-10-28: D-140, a texture declaration's colour space, format and size keys as the original takes them; the disabled Mods' copies plan in every state.
- 2026-10-28: the user: LOD 2 and 3 not to be done, every other gap to be done. D-141, the contract's sizes as index widths (64 components).
- 2026-10-28: D-142, a replaced draw's direct draws (instanced, non-indexed, from_caller, auto).
- 2026-10-28: D-143, slot ranges in a replaced draw.
- 2026-10-28: D-144, CheckTextureOverride of other targets in a replaced draw; D-145, a view D3D11 does not make binds nothing in owned work; D-146, dispatch, drawauto and indirect draws in a replaced draw do nothing.
- 2026-10-28: D-147, store in a replaced draw a frame late (no wait on the game's thread), clear there as owned work.
- 2026-10-28: the user: do everything without stopping, test after. D-148, draws past their files.
- 2026-10-28: D-149, DDS arrays and cubes as their first layer; buffer-description sections stay inert (the game's buffers are shared pools).
- 2026-10-28: D-150, the original as the judge (XXMI's d3d11.dll in a D3D11 program); its two findings fixed.
- 2026-10-28: D-151, the judge's image, draw and flow cases; views D3D11 does not make are null views; first-use bind flags.
- 2026-10-28: D-152, the application's other bindings saved and put back in a replaced draw (Snowshine LOD 1).
- 2026-10-29: D-153, a copy of a region past its buffer copies nothing (undefined in D3D11).
- 2026-10-30: the user asked for a review of the INI parsing. D-154: files read by the original's reader (judge case `encoding`: the original reads up to a byte sequence that is not UTF-8), the Mods folder walked by the original's rules (case-insensitive order, exact `.ini`), d3dx.ini's own sections first (LIMIT-039 lifted), the 128/64 INI budgets gone, a Mod alone named as the folder names it, the bridge's UTF-8 widening fixed. Failing first in the live INI scene and the judge; the whole folder had refused on a `mod.INI`. Found pre-existing: BUG-078 (vk01 translucency, HEAD fails identically) and the new Mod's copy_desc refusal (reported truthfully now).
- 2026-10-30: the user: every INI under Mods takes part, errors handled the original's way. D-155: every enabled INI an input of the folder generation (LIMIT-046 lifted); an unsupported declaration reference leaves the resource null with a warning once instead of refusing. Failing first in the live INI scene (a plain Mod, a bad INI).
- 2026-10-29 22:24: at the user's request the launcher's EFMI profile switched back to DX11 (only the two values; backup build/backups/launcher-dx11-20261029-222441/). The injected library still names dist/efmi_vk_inject.dll; the user sets it to the EFMI MCP probe before a DX11 start.
- 2026-10-27: cause D measured offline (vk03, phase-timing build and normal build): 16.6 us per matched draw, the plan 10.5 (interpreter about 5.2); the other stages already cached. Recorded in docs/design-review-1.9.md; nothing changed. The remaining items wait on the game or on a failing check.
- 2026-11-10: the user asked for a review of the whole project, the INI interpreter above all, and a plan. Read c69c1b6 (frontend, plan conversion, backend call sites, the installed Core and Mods read only) with three separate reviews beside it (layer host, verification, the original's language and a census), their claims checked at the cited lines. Result in docs/architecture-review-2.1.md: findings R-01 to R-15, L-01 to L-09, V-01 to V-06 and a plan in phases; BUG-079 to BUG-083 and LIMIT-047 recorded. Run: the INI execution matrix check, red with 39 failures (expectations of 2026-10-18). No code changed, nothing built, deployed or loaded into the game.
- 2026-11-10 evening: the user: push, then do the parts without the game. Review pushed (f443e3a). P3-0 committed (23f5702). Phase 0, frontend group, in progress on engine-work (a commit marked work in progress, not merged): containment check 6 of 6 and the installed folder's key-state check pass; BUG-084 (lastrite left out) fixed; BUG-085 found (the owned runtime's test step red since D-155). Stopped at the session's usage limit before the owned runtime's tests were rewritten and before any layer build.
- 2026-11-10 night: the parts of the review's plan without the game, finished as far as recorded in CURRENT_STATE: Phase 0 in the frontend and the host (D-156, D-158; BUG-080 to BUG-085), the matched-draw judge and its one-line sweep (D-157; 50 lines, 35 as the original's, 12 recorded differences, 3 not judged), two differences the sweep found fixed from the original's source (BUG-086 post lists after a replaced index buffer, BUG-087 a saved pixel slot). Sandbox only; nothing deployed or loaded into the game.
- 2026-11-10 night: the whole suite found the EFMITTL installed that day refusing its draws on every build (BUG-088). D-159: a slot below t60 without a SPIR-V register is not declared, and a word buffer's two DX11 answers are both tried in a condition. The library's next query (the game's texture format and size at a slot) is open, so the replay with a library call stays red. Plan digests per Mod (P3-3) and the list of offline exclusions (P3-4) added.
- 2026-11-10 night to 2026-11-11: P4-2 of the review's plan, in a session of its own. character_backend.cpp (4326 lines, one translation unit) moved by a script into eleven translation units by concern and character_backend_internal.h (D-160); scripts/build.py lists the parts. Build and whole suite before and after: 54 passed and the one red step of BUG-088 in both, step for step equal; plan digests of 19 Mods as accepted; the recording path's timers on the vk03 replay unchanged inside one build's spread. BUG-089 recorded from reading (a twin's bind drops the packages' unused owners with the anti-dither on; not reproduced), left for a session of its own. Sandbox only; nothing deployed or loaded into the game.
- 2026-11-11: Phase 1 of the review in a worktree of its own (branch claude/phase1-draw-state-ab9740, not merged). D-161 the draw state (frontend/xxmi/draw_state.h: every resource assignment total, a draw recorded with the state it sees, one table per slot), D-162 a command that cannot act does nothing and the lists run to their end (refuse and refuse_draw), D-163 operands nothing answers read one table; both tables generated into docs/ini-execution-matrix.json. P1-5 in part (unread vertex slots ignored, the blend-index edit optional). Verified on the CPU side only: host probe, INI runtime 502, matrix, the judge's 14 cases, the sweep 58 lines with none refusing; the build before fails 4 cases and 23 lines with the new records. BUG-090 recorded (vs as a value). The session stopped at its usage limit before the layer build was read, before the plan digests were compared and before any GPU step or the whole suite: CURRENT_STATE lists what to do first. Work-in-progress commit on the branch; nothing deployed or loaded into the game.
- 2026-11-11 later: Phase 1 resumed and finished as far as it goes offline. The layer builds; the owned runtime's tests state the new rule (resource_gpu 11288); the globals check knows the draw state's tables; unit checks for the optional blend-index edit. The plan digests' changes were confirmed per section against the build before (check_mod_states.py --explain, EFMI_VK_PLAN_OUTCOMES): refused to planned for the offline stand-in's two reasons, and toggled parts now off where their keys act. Whole suite 54 passed, 1 failed (BUG-088's step), as the baseline. Merged into main. The user changed the Mods folder during the day (EFMITTL 0.5.5, Zhuangfangyi; four Mods the replays use were removed, the project's copies serve). Nothing deployed or loaded into the game.
- 2026-11-11: BUG-088 in a session of its own (branch claude/efmittl-compat-cc233f), beside the Phase 1 session. The replay with a library call led through three more causes, each read off the original first and given a failing check: the textures' properties (D-170: judge case of 34 values, six sweep lines, a catalogue check of 2239 same-texture slots), the library's shader groups (D-171: the groups' own lists, chosen by SPIR-V rules checked against their patterns on the catalogue's DX11 listings), the libraries a Mod calls in the folder generation (D-172). The installed Yvonne no longer calls the translucent draw; the suite step replays ardelia now. Sandbox only.
- 2026-11-11 evening: 2.1.3 at the user's word (build it, to play on Vulkan next). Found while preparing it: the main checkout's dist held 2.0.8 (2026-10-27), not 2.1.2, and the user's start of that morning loaded the release 2.0.9 from the launcher's folder, so 2.1.3 is the first build in the game with everything from 2.1.0 on. Work tree build checked (quick group, the original INI runtime, the status' version through the inject entry); the 2.0.8 files kept in build/dist-2.0.8-20261027 of the main checkout.
- 2026-11-11 evening, later: the main checkout's dist built from 6e923f8 (18:58, game closed) and checked in the sandbox with its own configuration and the installed Mods (2.1.3, 19 packages, none left out; 2.0.8 on the same folder: 18, last rite left out) and with the suite steps that load it. The step "original INI runtime" was red on the main checkout for a Mod the user had removed (an October package file switched an acceptance of it on): the script and the test program ask for the Mod's INI now (c44e9b6). Not loaded into the game.
- 2026-11-11 19:24: the user started the game through the EFMI Vulkan Launcher and 2.1.3 loaded (the first game session of everything from 2.1.0 on). The agent read the log and two status samples: 19 Mods in the folder, Si, Yvonne, Zhuangfangyi and arcane replaced, EFMITTL 0.55 capturing translucency without fallback, no refusal, no degraded-line notice, no crash line, an F10 reloading in 3.5 s; 46.8 us per matched draw. Nothing seen on screen by the agent; no A/B.
- 2026-11-11, after the first 2.1.3 session's readings: the user on the screen: the characters and the translucent parts all normal (Si Lunabelle, Yvonne, Zhuangfangyi and arcane were the ones replaced, by the status). The other installed Mods, the first frame of an appearance and a frame-rate comparison stay open.
- 2026-11-11 19:32 to 19:39, the same game session: the user changed Mods while playing (four F10 in all, each exchange complete). Read from the status and log: ardelia replaced with translucency capturing, the new last rite 2.3 replaced, no crash line. Three notices of a part keeping the game's look: Zhuangfangyi with LIMIT-027's reason and the bones guard right after it, and lastrite's missing catalogue image while a second Mod of the same character and namespace stood beside it (BUG-091, gone when the old folder was removed).
- 2026-11-11 night: BUG-092, the user's report from the 2.1.3 session: the notices flicker while refreshing. The status had it (busy 1290 of 5501 presentations with a notice): three overlay slots against a frame generator's presentations. The original draws at every Present with the immediate context; the layer makes a slot when none is free and releases the text images when no notice is shown. Failing first with a new sandbox mode that presents five images behind a slow frame (24 of 48 without the notice before, 0 of 42 now). 2.1.4 built and checked in the work tree; not in dist while the game runs.
- 2026-11-11 20:16: 2.1.4 built into the main checkout's dist after the user had closed the game; loaded there in the sandbox with dist's configuration (19 packages, validation clean) and the owned-present step on it (80 checks). Not loaded into the game yet.
- 2026-11-11 late evening: the user, asked what comes next and given the open small items: those are not the point. Taken as the review's Phase 2, the part of its plan not begun. While the code was read the user asked why only a character's INI matches (every INI should) and said that loading should not tell characters from other Mods at all; both confirmed as the direction (R-03, R-04). Read from the original how a draw is looked up (the Core's check of `ib`, the draw maps by `match_index_count`, the region hash): whichever INI declares a section. A census of the installed folder found last rite's `Texture\hair.ini` (installed that day), whose two hair sections never ran, and 80 panel marks that ran only with a character's draw. P2-1's first part done test first (D-173): the sections of every INI of a character Mod's folder, two rounds of candidates, the lists once, a section without a component before the package is on the device. The offline planner needed the same candidate rule (it planned the marks with a first index the character section takes: eight refusals of the stand-in, not of the layer). Plan outcomes of the old and the new build compared by hash and count: 277 the same, 11 new, none changed. Sandbox only; nothing deployed, nothing in the game.
- 2026-11-12 morning: P2-1 second part (D-174), test first: an INI outside every character Mod's folder has a package without a skeleton, a Mods folder needs no character Mod, and the folder's generation is its own program. The first try refused the new packages (their sections came without the no-component mark); the session ended in the rebuild and was picked up from the half-built tree. Checked against the 2.1.4 binaries for the failing-first evidence and for the plan outcomes (263 the same, 11 added, none changed). Found on the way: the user's Yvonne Mod, changed after midnight with a tool that lets LOD components share bone spans, is left out by the old and the new build alike (BUG-093); it turns two steps red on this folder. The own-mesh draw (P2-2) is written and set aside untested, BUG-093 first. Sandbox only; nothing deployed, nothing in the game.
- 2026-11-12 morning, BUG-093 (D-175): the user's Yvonne loads again. Read from the Core's MergedSkeleton.ini that a component is an offset and a count with a remap per LOD and that nothing keeps components apart. Test first with a fixture written as the user's tool writes a Mod (a fourth component on component 0's bones, a LOD 0 remap): 2 of 10 before, 10 of 10 after, bones on the device and on the CPU. The package counts needed bones in spans now, either LOD has a remap, and the device's tables are two per component. Three unit expectations pinned the old rule (bone, package, the host's skeleton metadata) and were turned; one new expectation of mine was wrong about the test package and was corrected. The scene check of textures reaching Ready needs a package that is not lazy, so the CPU path is exercised by the setting, not by the split queues. Sandbox only; the game's dist is still 2.1.4, which leaves Yvonne out.
- 2026-11-12 morning, P2-2 (D-176): a section without a component draws a mesh of its own under the game's vertex stage and bones, as the original's lists draw with the game's shaders and bindings. Written the evening before and set aside for BUG-093; put back, with a switch in the scene for what it expects at the unmatched draw. Test first: 3 of 5 on the binaries before (the plan refused), 5 of 5 after, at the first run. The touched steps all passed, the plan digests as accepted. Sandbox only; the game's dist is still 2.1.4.
- 2026-11-12 10:44: 2.1.5 (D-173 to D-176) built into the main checkout's dist after the whole suite passed on the work tree (57 steps); the game was not running. Loaded there in the sandbox with dist's configuration: 19 packages, Yvonne among them, validation clean; live INI rendering 154 on that dist. The 2.1.4 binaries kept in build/dist-2.1.4-20261111. Not loaded into the game yet. Open of Phase 2: a texture of another package's folder bound in a draw (BUG-091 may be it), the skeleton and sections read by the exporter's names at load (the rest of P2-3), instanced character draws (P2-4).
- 2026-11-12 afternoon, P2-3 (D-177): the user, of Phase 2's open items: the point is the contract read by the exporter's names. Read the Core's interface: a Mod is known to it by the lists it runs, the Core's variables it assigns and the pools and resource it references into the Core's inputs. Test first with the fixture renamed throughout and its component handed over in a list the section runs: 1 of 5 before, 5 of 5 after. The first attempt read the Mod's lists before they were compiled (the initializer's call was not found); reading the sections' lines fixed that. Reading the sections again at preparation lost the stride branches of the user's avywenna 1.7, because the adapter had wrapped the operands; the comparison of plans against the deployed 2.1.5 caught it, and the operands are now unwrapped as commands are. Then 319 sections as before. A build was stopped by the user mid-way and restarted. Sandbox only; not in dist (the game was running).
- 2026-11-12 afternoon: the user said to build and then finish everything offline. 2.1.6 (2.1.5 with D-177) built into the main checkout's dist while the game was closed (22 packages, validation clean). Then D-178 (BUG-091: a package's images are every image of the folder; reproduced with the exact message first) and BUG-089 (reproduced with a new counter, one snapshot batch per recording with RabbitFX's gate set; the twins' bind keeps the packages' owners). The census of instanced draws in the 16 captured frames: no character draw is ever instanced (P2-4 has no case in this game yet). The installed Mods folder changed again during the round (22 Mods, several new versions).
- 2026-11-12 evening, still offline: D-179 (R-09, draws beside the game's without skip), D-180 (P2-4, instanced component draws decomposed as the Core's IterateInstances), D-181 (L-08, the variant cache's eviction) and BUG-090 (`vs` reads the Core's ShaderRegex value), each from the original's code first and with a check that failed on the build before. The touched areas, 35 steps, passed on the work tree. Not in dist. Then D-182 (P2-3's last part, a component computed at the draw), D-183 (L-07, one notice when the game's character draws are not what the layer knows) and D-184 (P3-4, the texture-hash sections in the state check), each with its check first; the whole suite on the work tree (one step, pipeline recipes, stalled once at its time limit in code this round did not touch and passed three reruns); 2.1.7.
- 2026-11-12 19:39: 2.1.7 built into the main checkout's dist with the game closed; the 2.1.6 binaries kept in build/dist-2.1.6-20261112. Loaded there in the sandbox with dist's configuration: 22 packages, none left out, the notices only Xaihi's two inert ShaderOverride lines, `game_shape` told false, validation clean. Not loaded into the game.
- 2026-11-12 20:10 to 21:30: the user played 2.1.7 (PID 7584). Read from the probe's status and captures: everything loaded, five characters replaced, no new notice. The user's two findings: translucent clothes cast full shadows (BUG-094, the library's shadow groups unrecognised; fixed as D-185 in the sandbox, failing checks first) and Zhuangfangyi's dash shows the game's textures (LIMIT-027: the dash effect vertex shaders read bones from a uniform block, which the original Core supports and the layer refuses; found by scanning the probe's pipelines for refused ones and saving their SPIR-V, since captures with frame generation on kept landing on generated frames). The user restated the two routes (compatibility, and the Vulkan loader for this game, decided 2026-11-10); route 2's next step started in a session of its own.
- 2026-11-12 21:24: 2.1.8 (2.1.7 and D-185) built into the main checkout's dist with the game closed; loaded in the sandbox with dist's configuration (22 packages, validation clean). The dash fix runs in a session of its own (branch dash-cb-bones). Not loaded into the game.
- 2026-11-12 night, LIMIT-027 (D-186), a session of its own on branch claude/dash-cb-bones-e6d724: the dash after-images keep the game's mesh because their vertex shaders read the bones from a uniform block of 4096 vectors. Read first the original Core (MergedSkeleton.ini DetectBoneDataSource cb4/cb3, the ConstantBuffer importer and config overrider, ResourceConfigWithBones) and 3DMigoto's cb_size (the DXBC's declared size), then the three captured shaders (row 0: flags, current and previous rows; bone i at offset + 4 + 3 * i). Test first: the live scene `constant_bones` (2 of 6 on the build before, 6 of 6 after, bones on the device and on the CPU) and recipe_test on the fixture and the captured modules (170 of 170). The block becomes a read-only storage block at the private set's binding 0; the bones are imported from it on the CPU and the draw binds [config rows][merged poses]. The touched areas, 33 steps, passed. Sandbox only; not merged, not in dist, nothing loaded into the game.
- 2026-11-12 night: route 2's first step in a session of its own (branch claude/vulkan-loader-d7c127, started by the EFMITTL session). Read the original's ShaderOverride and hunting first; docs/vulkan-native-mods.md. D-187: ShaderOverride by the SPIR-V module's CRC-32C (below 2^32; DX11 hashes stay inert), at every draw of the pipelines with the module, in the original's stage order. D-188: hunting of vertex and fragment modules with the installation's [Hunting] keys. The user asked during the step to keep the DX11 route compatible: plan digests of the 22 installed Mods equal to the build before's in 7922 states. Failing first: two new live scenes (25 checks, all failed before). Touched areas passed (CURRENT_STATE). Sandbox only; not merged, not deployed, nothing in the game. Next: LIMIT-048 and TASKS (shader replacement).
- 2026-11-13, progress audit requested by the user: reviewed the current state, tasks, plan, architecture, issues, original HackerContext draw ordering/restoration, source and worktree status, binary version strings and retained test evidence. At inspection main/engine-work/local origin/main were 5206e0d (source 2.1.9), main dist was 2.1.8, and the merged worktree binaries still identified as 2.1.8 before the version-only rebuild; latest recorded real-game evidence was 2.1.7. D-189 existed as uncommitted side-worktree changes (its retained log: 33 steps passed), not part of main. Reran only `sandbox/runtime_test.py spirv_shader_overrides hunting_keys constant_bones` in .worktrees/fix: 31/31 passed. Reconciled current merge/deployment statements in CURRENT_STATE and TASKS and listed the remaining acceptance boundaries. Records only; text check passed (533 files), and the diff and whitespace check were clean before the local commit. No rebuild, full suite, game launch/attachment, module exchange, deployment, launcher edit or remote operation.
- 2026-11-13: route 2 resumed at the main session's request after it merged step 1 and the dash fix (ae4db05). D-189, the rest of hunting (index and vertex buffers, render targets, the `hunting` value, dumps, frame analysis's log), read from Hunting.cpp, ResourceHash.cpp, HackerContext.cpp, CommandList.cpp and FrameAnalysis.cpp first. Failing first 0 of 11 on ae4db05's build, 11 of 11 now; plan digests 0 changed against ae4db05's build. Shader replacement drafted next (3DMigoto's ShaderFixes; there is no `vs =`/`ps =` in a ShaderOverride).
- 2026-11-13, D-189 review for integration: read the original hunting, buffer-region and shader-reload paths. Fixed the timestamp collision that overwrote an earlier frame-analysis log; the original refuses an existing directory. New regression 11/12 before, 12/12 after; all three hunting/ShaderOverride scenes 37/37. Layer/backend built in this worktree (Python UTF-8 retry after a console-encoding failure; existing signedness warnings only), quick group six steps, live INI rendering 221/221, modes 50/50 and Vulkan validation 76/76 passed. Earlier broader run-A.out: 33 steps passed. Source review and text/whitespace checks precede the commit. No game, deployment or external configuration changes. Next: merge D-189, then implement D-190 game shader replacement.
