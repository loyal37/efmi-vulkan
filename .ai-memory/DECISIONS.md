# EFMI Vulkan - Technical decisions

Design decisions D1 to D11 of the product itself are in `docs/plan.md`, section 3. This file records the decisions about how the project is run, and later changes to the design with their reasons.

## D-001: Changes stay inside the project; EFMI, the game and EFMI MCP are read-only

- Date: 2026-10-11.
- Decision: writes are limited to `F:\efmi vulkan` and to `F:\vulkan` (Vulkan headers / SDK). The EFMI/XXMI installation, the launcher configuration, existing mods, the game installation and `F:\EFMI MCP` are only read.
- Reason: same boundary the user set for the sibling project; this project needs real data from those places but no change to them.

## D-002: Memory split by seven responsibilities

- Date: 2026-10-11.
- Decision: overview, architecture, current state, decisions, issues, tasks and session log are seven Markdown files in `.ai-memory/`; the root `AGENTS.md` is the entry point for resuming and maintaining them, and `CLAUDE.md` imports it.
- Reason: the user asked for a memory system modelled on `F:\EFMI MCP\.ai-memory`. Only the structure was taken from there; no content of that project was copied.

## D-003: English only, no Chinese characters in the workspace

- Date: 2026-10-11 (user).
- Decision: file names and contents are English; `scripts/check_text.py` fails the regression suite when it finds CJK characters. The plan written in Chinese earlier the same day was translated to `docs/plan.md` and the Chinese file removed.
- Note: the conversation with the user stays in Chinese.

## D-004: Commit locally after every verified change; no remote

- Date: 2026-10-11 (user, stated twice).
- Decision: each change that passed its verification is committed unprompted. No remote is configured and nothing is pushed until the user names a repository. `docs/plan.md` is tracked (the user chose this explicitly; it differs from the EFMI MCP project, where the plan stays local).

## D-005: Vulkan headers live in `F:\vulkan`, outside the repository

- Date: 2026-10-11 (user: install to `F:\vulkan`).
- Decision: `scripts/fetch_vulkan_headers.ps1` makes a shallow clone of `KhronosGroup/Vulkan-Headers` at tag `vulkan-sdk-1.4.321.0` into `F:\vulkan\Vulkan-Headers`. Build scripts look for headers in this order: `%VULKAN_SDK%\Include`, `F:\vulkan\Include`, `F:\vulkan\Vulkan-Headers\include`.
- Not done: the LunarG SDK installer was not run. It needs elevation, accepts licence terms and changes system environment variables and the registry, which is the user's to do. If the user installs the SDK into `F:\vulkan`, the build picks it up without a change.
- Consequence: there is no `glslc` and no validation layer on this machine. The sandbox builds its two shaders with a small SPIR-V builder in its own source.

## D-006: A live probe for development, modelled on EFMI MCP

- Date: 2026-10-11 (user asked for a tool like the DX11 one to help development).
- Decision: the read-only diagnostics of the layer are served over a named pipe `\\.\pipe\efmi-vk-<PID>` with the same framing as the EFMI MCP probe (`[u32 header_len][u32 payload_len][header JSON][payload]`). `server/efmi_vk` holds the client, a CLI and a stdio MCP server; `.mcp.json` registers the server for this workspace.
- Reason: gates G2 and G3 are questions about live data (threads, submits, index bytes, bindings). Answering them through tools an agent can call is faster and more repeatable than reading log files, and the sandbox gives the same tools a target that needs no game.
- Boundary: the probe is read-only. It does not change what the application draws.

## D-007: The sandbox is the regression target; the real game is a separate layer of evidence

- Date: 2026-10-11.
- Decision: every probe feature gets a sandbox test with known expected values (the sandbox writes `expected.json`). Results from the sandbox are never reported as results from the game.
- Reason: the game cannot be used for routine testing (anti-cheat, account risk, user's decision per run).

## D-008: One Python build script instead of batch files

- Date: 2026-10-11.
- Decision: `scripts/build.py` finds `vcvars64.bat` (through `VCVARS` or `vswhere`) and the Vulkan headers, runs `cl` for the layer and the sandbox and writes the layer manifest. The regression suite calls it.
- Reason: one place for the header search order of D-005; the tests can build into another directory with `--out` while a process still holds the DLL in `dist/`.

## D-009: Two modes of the layer; draw records are always kept in probe mode

- Date: 2026-10-11.
- Decision: `EFMI_VK_MODE=passthrough` hooks only device destruction and present and starts no pipe; it is the mode for gate G1. In probe mode every command buffer keeps the records of its draws all the time, not only while a capture is armed.
- Reason: a capture then never depends on when a command buffer was begun, which matters for engines that record ahead of the frame boundary. The original record cost was about 200 bytes and one vector push per draw; 0.4.0 adds descriptor allocation ids and shared layout references, so that old estimate no longer describes the current record. Heavy-scene overhead remains unmeasured (T-012).

## D-010: Shadows are fed at submit time and only for index buffers by default

- Date: 2026-10-11.
- Decision: buffer writes recorded into a command buffer are applied to the shadow when that command buffer is submitted, reading the staging source at that moment. Only buffers with index usage get a shadow unless `EFMI_VK_SHADOW_USAGE` widens the mask.
- Reason: what the staging buffer holds at submit time is what the GPU copies; a shadow for every buffer of a game would cost too much memory. The DX11 dump shows large shared index buffers bound at byte offsets (plan fact 14), so the shadow is per buffer with an interval set of known bytes, and hashes are taken over sub-ranges.
- Open: whether the game's uploads are visible to the CPU at all is gate G3; LIMIT-007 lists what is not covered.

## D-011: The game gets the DLL by injection with export redirection; the layer entry is the fallback

- Date: 2026-10-11 (the user asked whether loading can work by injection, as with d3d11).
- Decision: a second entry was added to the same binary. Loaded with `LoadLibrary`, the DLL redirects the export table entries of `vulkan-1.dll` in memory and calls the original addresses from its hooks. `efmi_vk_inject.dll` selects this entry by its file name, `efmi_vk_layer.dll` the layer entry. For the game the plan is the launcher's extra-library injection, which already brings the user's DX11 probe into `Endfield.exe`.
- Reason: no registry entry and no system-wide change; independent of elevation (LIMIT-012); the same launch flow the user already runs; the redirection technique is the one the EFMI MCP probe uses on `d3d11.dll` in the real game.
- Cost: the DLL has to be in the process before the engine resolves its Vulkan functions, which is not known for the game (LIMIT-013). A device created earlier is served by the passive device and not tracked.
- Settings: `efmi_vk.cfg` next to the DLL, because the launcher cannot hand environment variables to the game; an environment variable still wins, which the tests rely on.

## D-012: What the project needs is installed into `F:\vulkan` without asking

- Date: 2026-10-11 (user).
- Decision: dependencies such as headers, SDKs and tools go into `F:\vulkan` directly. This replaces asking before each download.
- Boundary kept: nothing an installation would write outside that folder (registry, system environment variables, system directories). If an installer cannot be confined to the folder, that is reported instead of run.

## D-013: Descriptor snapshots are explicitly at submit; bytecode follows pipeline lifetime

- Date: 2026-10-11.
- Decision: the first T-011 increment tracks ordinary descriptor writes/copies and snapshots contents when a captured command buffer is submitted. Dynamic offsets are taken at bind time and split by layout. Shader bytes have shared ownership between their live module and pipelines; ids do not rely on handles remaining unique.
- Reason: descriptor sets are references, not inline copies of their contents. Querying them later from the pipe could silently return another frame's data; retaining immutable capture JSON avoids that. Module destruction after pipeline creation is normal and must not remove shader diagnostics prematurely.
- Limits: submit-time visibility does not establish G3's record-time premise. In 0.4.0, templates, pushes and descriptor buffers discarded stale tracked knowledge; D-015 subsequently adds template/push decoding. Descriptor buffers remain unsupported. A small declared-binding SPIR-V reader is not full reflection. Details: `docs/probe-resources.md`.
- Historical deployment choice, superseded by D-014: because run 3 ended before 30 minutes, this implementation round built/tested 0.4.0 under `build/next/` and kept `dist/` at 0.2.0 passthrough.

## D-014: Reuse run 3 as initial loading evidence; defer the long-duration observation

- Date: 2026-10-11 (user).
- Decision: the user confirmed exiting run 3 before 30 minutes after scene changes, then instructed the project to use the previous run's data. Accept it as initial G1 loading evidence and continue development and short probe preparation without requiring another 30-minute passthrough session. The original duration was a project observation target, not a technical requirement.
- Evidence boundary: the historical run used 0.2.0 passthrough. Loading and device creation were logged; scene changes and the early exit were reported by the user. Exact duration, total frames, long-duration stability, probe-mode game behavior and real-character capture data remain unverified. Do not relabel historical results as a new test.
- Deployment: after confirming no game process remained, preserved the old DLLs/settings/log under `build/backups/`, rebuilt and regression-tested 0.4.0 in `dist/`, and set `dist/efmi_vk.cfg` to probe. A sandbox using that actual configuration captured a frame successfully.
- Boundary kept: this decision prepares the next session; it does not authorize the agent to start or inject the game. Each real-game launch still follows the user's on-the-spot instruction. Launcher settings remain read-only, and blocked loading still stops the project.

## D-015: Push contents are retained at recording time; resource visibility stays at submit

- Date: 2026-10-11 (implementation of the next T-011 increment).
- Decision: template definitions own their copied entries and retained layout metadata. Ordinary template updates use the same decoder as writes. Each push update creates immutable contents retained by subsequent draw records, separate for each command buffer and bind point. Partial writes preserve compatible elements; layout disturbance and reset invalidate them. Immutable sampler-only push bindings start undefined.
- Reason: looking up a push bank's final state at submit would give earlier draws the last update's descriptors. Recording immutable values prevents this and survives secondary-buffer flattening. A push template's bind point comes from its creation metadata, not from the currently bound graphics pipeline.
- Diagnostic boundary: `snapshot` is `record` for pushes and `submit` for ordinary sets; `resource_snapshot` is always `submit`. Retaining a buffer handle/range is not evidence that its bytes were readable at record time. This does not satisfy G3 b/c by itself.
- Scope: standard buffer/image/texel/sampler descriptors; unknown templates and unsupported layouts discard stale state. Descriptor buffers and specialized types remain unimplemented. GPU and CPU coverage are recorded separately in `docs/probe-resources.md`.

## D-016: Retain XXMI hosting with explicit adapter boundaries; correct sample scope

- Date: 2026-10-11, M0 static dependency assessment.
- Decision: keep plan D4's hosted XXMI frontend for M3. The pinned v1.2.2 parser/expression sources share D3D-shaped state, globals and resource code; they are not an isolated portable library. WWMI-DX12 shows a concrete host arrangement but uses an older baseline and a substantial adapter. Define state/query/resource/input boundaries and test new Pool/Region/ElementFormat semantics in a later compiled prototype. A reduced interpreter stays a fallback if that prototype fails.
- Evidence: local d3d11.dll exactly matches the official v1.2.2 archive and source tag a7847eae82aaac32175c9a7d199673d5c9323bcd. Reference host commit 113f42cc2dc5702dbba624ee16abac96d68ed3b5. No extraction, compilation or runtime equivalence of a host is claimed. Details and all input hashes: docs/compatibility-inventory.md and its JSON companion.
- Scope correction: the enabled mod folder contains arcane.ini and k/1.ini, an HLSL image panel. The main file configures 13 slots but defines component ids 0-11. Full-folder acceptance includes the panel; do not silently disable it or count character-only rendering as complete compatibility. Existing mod files remain read-only.
- Update, 2026-10-12: the bounded native host feasibility prototype compiles, links and passes 23 original-implementation CPU checks. Keep the hosted approach; no reduced interpreter is needed for this checkpoint. It is standalone, with reference headers as scaffolding and no layer/graphics/mod integration. Exact tested semantics and remaining adapter work: docs/xxmi-host-prototype.md.

## D-017: Whatever the probe keeps is bounded, and there is a tracking level below full

- Date: 2026-10-11, review of 0.4.0 / 0.5.0 before the first probe session in the game.
- Decision: SPIR-V is kept only up to `shader_budget` (default 512 MiB); a capture keeps descriptor contents only up to the request's `descriptor_budget` (default 64 MiB); destruction and pool reset do work proportional to what is destroyed, not to what exists; `resources = 0` is a tracking level between full probe and passthrough.
- Reason: the sandbox has two shader modules, a handful of descriptor sets and five draws. The game has a 206 MB pipeline cache, and its counts are unknown until the first session. Unbounded retention and full-table scans under the exclusive resource lock were the parts most likely to hurt the game, and a session that goes badly should be able to fall back without losing the draw data.
- Not decided: the defaults are guesses to be revised with the first real numbers. A budget reached in the game is reported (`shaders_not_retained`, `descriptors_omitted`), not hidden.
- Kept as it was: the per-update cost of descriptor tracking (exclusive lock and map updates per written element). Its size in the game is unknown; `efvk_api_usage` shows the update rate.

## D-018: What the first game data settles in the plan

- Date: 2026-10-12, after the first probe session (CURRENT_STATE).
- D5, threading: the engine records on one thread in the observed scene and `Player.log` says `jobified=0`. The mod lock stays in the design, but bring-up does not need a single-threaded mode forced from outside. To be re-checked in the open world.
- D6, hashes: index data is read from the application's own mapping; the shadow path was not needed for any draw. Shadows stay for the case of device-local index buffers, which this scene does not have.
- D7, texture slots: mapping by resource name is ruled out, the SPIR-V carries no names. The mapping has to come from binding order and a per-shader profile, checked against the DX11 baseline.
- D3, two-phase execution: its premise looks right. Instance config and bones are in mapped memory and all per-frame data arrives without buffer copies. It is not proven until T-017 shows the bytes at submit and at record time.
- D8, vertex input: confirmed as needed; the game's blend stream has stride 12 with 8-bit indices, the mod's has stride 16 with 16-bit indices.
- Compute: 47 dispatches per frame, all outside render passes and none needed by the character's vertex shader for bones. The merged skeleton does not depend on them.

## D-019: What the DX11 baseline settles in the plan

- Date: 2026-10-12, after pairing a DX11 frame of the sample mod with the Vulkan frame (`docs/dx11-baseline.md`, facts 15 to 19).
- D7, texture slots: a mod's `ps-tN` is translated through a table from (Vulkan fragment shader, DX11 slot) to (set, binding), per pass and including bindings of the global set, because the mod assigns its slots in every pass and under DX11 they land on whatever that pass has there (fact 20). Replacing by role ("the diffuse map") is rejected: it would not show what DX11 shows. The table is data produced by `scripts/dx11_baseline.py` from paired frames, not something the layer derives at run time: binding order is not register order and there are no names. Where two textures look alike the entry stays open until content identity exists (T-018). A draw whose fragment shader has no table entry is left unreplaced and reported.
- Instance window: taken from set 2 binding 0 at the draw's dynamic offset, 256 bytes; found by its place in the layout. The DX11 core's per-shader search for the slot has no counterpart.
- Bone buffer: identified as a buffer (the one storage buffer of 8,413,184 bytes the character pipelines bind in set 0), not by a binding number.
- D3, CPU work: the two compute steps of the core are a copy of bone rows per component and frame and a patched copy of 16 constants per matched draw. Both are done on the CPU before the draw is replaced, provided T-017 shows the bytes are there in time.
- D8, vertex input: the variant changes the stride of the blend binding and the format of its unsigned-integer attribute and binds 32-bit indices; attributes are identified by binding and format, not by semantic.
- Not decided: how the fragment stage keeps the game's bone buffer and instance window (T-019).

## D-020: How a capture keeps buffer contents

- Date: 2026-10-12, with layer 0.6.0 (T-017).
- Bytes are kept as they are at submit, not at record: submit is the last moment the layer sees before the GPU may read, and one read per distinct range serves all draws of the command buffer. Record time contributes a checksum per range only; that answers whether the data was complete then at four bytes per range instead of a second copy.
- The request names what to follow (set, binding, byte offset, count, stride, window size) instead of the layer knowing the game's instance entry. The game's rule lives in `scripts/collect_probe.py` as a preset. A later Core-VK will read the same places through its own code; the probe stays generic.
- The first bytes of the followed range are a separate item (`entry`): the game's range is 64 KiB with the entries of many draws, and a comparison of the whole range would call a draw's data incomplete because of its neighbours.
- Selection at record time is by region hash, computed there a second time while a snapshot capture is in its frame. Rejected: reading every draw at record time (cost in every frame of the capture for draws nobody asked about) and selecting at submit only (no record-time evidence).
- Bounded like everything else the probe keeps (D-017): budget in bytes, size limit per uniform range, clamps on every requested size; past the budget a range is still checksummed and compared.

## D-021: What the second probe session settles in the plan

- Date: 2026-10-12, after layer 0.6.0 ran in the game (facts 21 and 22).
- D3, bone import: done by the CPU from the application's own mapped memory. No GPU read-back (T-013 stays closed for bones), no compute pass. The premise of the two-phase execution is measured, not assumed.
- When to read: both moments are valid in the observed scene. The instance entry and the bones of a component may be read when its matched draw is recorded; reading at submit is always at least as good and remains the fallback if another scene shows late data. Which of the two M2 uses is an implementation choice; M2 starts with the read at record time because it needs no deferred work, and keeps the comparison of 0.6.0 as a check.
- How much to read: exactly `VertexGroupCounts[component]` bones of three rows from the entry's offset, as the DX11 core does. Never a fixed window: bytes behind a palette belong to something written later.
- What not to use: the order or distance of palettes in the bone buffer, which changes per frame.
- Scope: one scene with one standing character. The decision is revisited if a later session shows `different` inside a palette or an entry.

## D-022: The pipeline variant gives the vertex stage a set of its own

- Date: 2026-10-12 (T-019, fact 23).
- The vertex module of a replaced draw's pipeline is rewritten so that its storage buffer and its instance window are bound at bindings 0 and 1 of the first set the pipeline does not use (set 3 for the sample character). The layer binds one set of its own there: the merged skeleton as storage buffer and its patched instance entries as dynamic uniform buffer, one 256-byte slot per replaced draw with 64 KiB of room behind the last slot, because the shader's block is 4096 constants long. The fragment module is not touched and reads the game's bone buffer and window, as it does under DX11.
- The variant's pipeline layout keeps the game's set layouts and push constant ranges and adds one set layout at the end, so the sets the game has bound stay valid for it.
- The two variables are found by structure: the only storage buffer of the vertex module, and its uniform buffer at set 2 binding 0. A vertex module with no or several storage buffers, or without that uniform buffer, gets no variant; its draw is left to the game and reported.
- Rejected: replacing the shared bindings. It needs a copy of the global set (about 50 bindings) for every pass that draws the character, and 20 of the 45 draws have a fragment shader that would then read the merged skeleton at a patched offset instead of the component's own first bone.
- Cloning a set and overriding bindings is still needed for textures (set 1, and now and then a binding of the global set, fact 20). That mechanism is separate from this decision.
- Evidence is offline: four words per module, `spirv-val` on 21 real vertex shaders. Creating and drawing with such a pipeline is the first thing M2 checks.

## D-023: Disable upload hashing after the required identity capture; measure spikes separately

- Date: 2026-10-12, third Vulkan probe session, user-reported intermittent stutter during loading and steady display.
- T-018 now has its real content evidence: all four open sample slots resolve. Set image_hash = 0 in the prepared dist/ configuration for the next process; the existing process retains its original setting. Preserve full resource tracking for the first comparison.
- The measured 8.1 seconds is cumulative time inside upload processing, including reads and locks. A steady 3-second interval advances 360 presents with only 596 microseconds added there. Do not claim that average 120/s disproves stutter or that hashing caused every reported spike.
- Add bounded host present-interval diagnostics in 0.7.1 (T-020), including recent percentiles/peaks, lifetime threshold counts and capture/hash tags. This is host timing, not GPU/display timing. No performance fix is considered game-verified until a new user-started comparison session.

## D-024: Preserve buffer evidence while measuring probe-only work

- Date: 2026-10-12, after the user reports smooth movement in confirmed 0.7.1 passthrough and stutter in resource-off probe.
- Remove proven unnecessary copy work without discarding readable fallback data: test source visibility before allocating record-time storage, and copy directly into the target shadow at submit. Keep unmap snapshots and submit-time priority. The game's individual stall source remains unproven.
- Add opt-in profile_tracker measurements in 0.7.2, default off, with cumulative category counters and a fixed 64-event slow history on the present diagnostic's QPC clock. Exclude forwarded driver calls. Account for profiling overhead, nesting and descheduling; never equate the sum to frame time.
- Keep the smooth passthrough run as evidence. Any diagnostic probe candidate requires a user-started game comparison after sandbox verification. Disabling buffer snapshots is a separate capability tradeoff, not a silently applied fix.

## D-025: Accelerate detected WC reads while preserving snapshot evidence

- Date: 2026-10-12, after 0.7.2 foreground world timing correlates the 129 ms peak with three expensive unmaps and a zero-byte copy-record wait. The global resource lock makes these snapshot reads relevant to unrelated recording work.
- Keep the 16 MiB unmap limit, index shadow mask, record fallback and submit priority. Do not lower data coverage to make the performance comparison appear successful.
- Classify the complete mapped range once using VirtualQuery. Use SSE4.1 streaming loads only for committed PAGE_WRITECOMBINE mappings on capable CPUs, with aligned loads, exact prefix/tail handling and memory fences. Recompute on remap, clear on unmap; unknown/mixed/cached ranges use memmove. This changes the copy implementation, not Vulkan synchronization or lock ordering.
- Apply the helper to every mapped-read path, including unmap, record fallback, submit and direct probe reads; CPU shadows still use memmove. Expose the available strategy and current per-buffer WC classification for verification.
- Local headless Vulkan measurements validate the mechanism (type 2: 24.023 to 1.270 ms for 8 MiB), not real-game smoothness. Type-4 device-local reads remain expensive. Keep profile_tracker on for the next comparable game run and ISSUE-005 open; source profiling defaults stay off. Full evidence: docs/pacing-tracker-session.md.

## D-026: At unmap, copy staging buffers only (WITHDRAWN by D-027: the premise was wrong)

- Date: 2026-10-12, after reviewing 0.7.1 to 0.7.3 against the buffer inventory of the 0.7.2 game session.
- The copy at unmap exists for staging buffers. Its rule (transfer-source bit, at most 16 MiB) also matched the game's mesh pools, and they were where the bytes went: 2,612 generations on 22 pools against 87 on 76 staging buffers. The rule is now: transfer source, optionally transfer destination, no other usage, wholly inside the mapped range, within the limit.
- This lowers coverage on purpose, which D-024 and D-025 declined to do. The reason is different from theirs: they would not trade evidence for a better-looking measurement; here the copy of a pool was never the feature's purpose, its one use (a hash while the pool is unmapped) has not occurred in any game bundle, and no layer that has to run during normal play could afford it. What is lost is reported per draw (`hash: null`, source `none`) and counted (`counts.unmap`), not hidden.
- A buffer that is not copied has its shadow knowledge dropped at unmap. Serving bytes that the application may have overwritten through the mapping is worse than serving none.
- 0.7.3's streaming loads stay: they are correct and help the reads that remain. They are not counted as the fix; their own benchmark shows 1.5 times on the memory type the pools use.
- `unmap_snapshot_all` and the `set` command exist so that the old behaviour can be brought back in a running process for a comparison. Switching it on in the game brings the stalls back and is done only with the user's agreement.
- Not decided: how draws get their index identity when a pool is unmapped (T-021).

## D-027: Withdraw D-026; the copy at unmap covers every whole transfer source again

- Date: 2026-10-12, after the first game session with 0.7.4.
- D-026 rested on the claim that the bytes copied at unmap were the game's mesh pools. The counters 0.7.4 added show that no pool is ever unmapped (`passed_over` 0 of 144,103 buffers) and that the volume is staging memory, as BUG-004 and D-025 had said. The claim came from reading `generation` as a count of unmap copies, which it is not, and from an inventory that cannot show short-lived buffers.
- The default goes back to what D-024 and D-025 decided: no lowering of coverage without cause. `unmap_snapshot_all` stays as a setting, default on.
- Kept from 0.7.4 because they are right on their own: dropping the shadow knowledge of a buffer that was mapped and not copied; `counts.unmap`; the `set` command; pinned settings in the test harness.
- The stutter fix is 0.7.3's (D-025) and is now confirmed in the game.
- For the working method: an attribution that changes the layer or the record needs a direct count first. Here the count was one small counter away, and was added in the same change that acted on the unverified reading.

## D-028: Own creation recipes; first M2 variant uses a static vertex declaration

- Date: 2026-10-12, after resuming M2 following the verified stutter correction.
- Retain complete supported graphics/render-pass state, including relevant fixed-function values, and reject unknown extensions explicitly. No asynchronous work may borrow application creation arrays or shader/render-pass handles. Layout metadata and SPIR-V have shared ownership. A shader pair alone is not a pipeline identity.
- The initial variant rebuilds the vertex declaration for the replacement stride/formats. This refines plan D8: dynamic vertex input is not required for the first backend, because the application may not have enabled that feature. The eventual cache key includes the original pipeline identity and replacement vertex declaration. Asynchronous creation and retaining the original draw until ready remain requirements; a worker build is tested, but the live game's request/cache/retirement system is still open.
- Keep D-022's VS-only private set, compatible original sets and push ranges, and untouched fragment module. Unsupported immutable samplers/layout flags never become guessed equivalents. A failed construction or unready transaction leaves the original draw available.
- Sandbox GPU readback is the first check: 16-bit bone index 257, changed stride/indices, two replacement buffers, nonzero dynamic offsets, original fragment data, blending and the next original draw after restoration. This passed under core/sync validation. Historical shaders are validated separately, never described as a new game result.
- The next game observation captures complete pipeline/layout recipes and current bone/instance data before integrating the real sample. Old creation counters do not supply the missing values. 0.8.0 stays in probe mode and never invokes replacement drawing in the game.

## D-029: Select a nonempty present interval without merging frames or losing record-time reads

- Date: 2026-10-12, after the user corrected the LOD labels and actual LOD1 recipe capture succeeded only without snapshots.
- An empty captured present interval is not evidence that the scene or character stopped drawing. User-reported scene state and mesh-section/pass coverage remain separate evidence. Depth-only LOD1 matches cannot substitute for LOD1 color coverage.
- Layer 0.8.1 adds opt-in wait_draws. Keep the same snapshot request armed across intervals without graphics submissions, discard those intervals' captured contents/budgets and return one interval with draw commands. Preserve their ordinary tracked buffer writes. Frame counters, pacing and the default one-interval capture are unchanged.
- Record-time comparisons belong to the actual draw recording, which may precede a skipped present; they are not fabricated from submit-time data. The existing timeout clears pending work. Report skipped_empty_intervals, and advertise a feature so older layers cannot silently ignore the option.
- Selection is bounded and only a diagnostic request. It does not alter gameplay, present synchronization, driver submissions or replacement drawing. No frame-generation producer or game fault is inferred from the observed empty intervals.
- Native and GPU sandbox checks plus the full suite pass. Real LOD1 color-pass snapshots remain open until a user-started run with the deployed candidate; do not hot-replace a loaded DLL or claim the fix verified in the game in advance.

## D-030: Preserve Core bone import semantics with atomic CPU updates

- Date: 2026-10-12, after the actual LOD1 snapshots supplied the remaining timing evidence.
- Copy source bone remap[i] after the leading source matrix; keep 512 implicit identities and separate current/previous slices per instance. Multiply all matrix terms by mesh scale. Patch only nonstatic x/y offsets of constant 5; preserve flags and every other entry byte.
- Import once per logical update/component/instance, allowing a lower LOD number to replace an earlier import. The caller owns update serials; empty present intervals are not a sufficient animation boundary. Preserve the master between updates as Core does.
- Validate both poses and remap ranges before mutating data or stamps. Invalid input leaves the old master and original drawing available. Keep the GPU snapshot separate and immutable for its actual lifetime; this CPU helper does not choose that lifetime.
- The five saved game frames agree byte for byte with an independent Core-derived Python reference over 283 draws. That is offline evidence, not a captured DX11 compute-output comparison or GPU rendering. New sandbox-native cases establish rejected-input and multi-instance behavior separately.

## D-031: First hooked M2 draw is one self-contained component with recording-owned storage

- Date: 2026-10-12, after actual LOD1 capture and verification of the importer/cache/snapshot helpers.
- Use sample component 7 (40 vertices, 108 indices, bones 266-277 of 499) because every weighted vertex depends only on its own palette. Copy original BUF bytes into an explicit, bounded, fingerprinted package; do not call this INI execution or full mod compatibility. The installed sample remains read-only.
- Import both poses independently for each draw. For this first component, neither an empty present interval nor cross-component ordering defines an animation update. Only complete mapped data and a ready compatible variant authorize skipping the original draw.
- Each command recording owns mesh, ready variants and per-draw snapshots. Seal at submit, preserve across resubmission and recycle only after a successful legal begin/reset/pool reset. Keep arenas with completed command buffers to avoid allocation every frame. Vulkan's application completion requirements govern reuse; the layer does not infer completion from frame count or elapsed time.
- Preserve original fragment inputs and application bindings; restore pipeline/VB/IB after emitting. Pending, invalid, unsupported and capacity cases forward the original. The optional runtime switch affects future recordings only. Default package/enable settings keep ordinary probe behavior.
- Sandbox hook rendering, package checks and saved game-data inspection are distinct from the next user-started game replacement checkpoint. Full-character geometry, texture overrides, several instances and the image panel remain open.

## D-032: The patched entry names the matrix before the merged skeleton; test shaders address bones as the game does

- Date: 2026-10-12, review of 0.8.1 to 0.9.0 at the user's request. Corrects one sentence of D-030 ("Patch only nonstatic x/y offsets of constant 5"): which bytes are patched was right, the value was not.
- The offsets written into constant 5 are `1536 + 6 * bones * instance - 3` and that plus `3 * bones`. Evidence from three sides, none derived from the others: the core's INI (`$merged_skeleton_offset - 3`), the DX11 baseline (`(1533, 3030)` in all 23 patched entries of two frames) and the game's Vulkan vertex shader (`offset + 3 + 3 * index`). See BUG-005 and `docs/m2-bone-import.md`.
- A rule for the tests, since three of them agreed with the code and with each other while all were wrong: a sandbox shader that stands in for a game shader reproduces the game's addressing, read from the game's SPIR-V, and a reference for a core rule is checked against a value measured under DX11 where one exists. Agreement between an implementation and a reference written from the same reading is not evidence for the reading.
- The core's INI files are part of its logic: values such as this one are computed there and passed through `IniParams`. Reading the HLSL alone is not enough when a rule of the core is reimplemented.
- 0.9.1 was deployed with the settings 0.9.0 had (component 7 enabled), because the user had that configuration set up before pausing; the first Vulkan start will draw the replacement. That start remains the user's decision.

## D-033: General replacement operations, component differences supplied as data

- User scope amendment, 2026-10-13: only LOD0 and LOD1 are required. Do not develop/test LOD2 or later. The original Core pool indexing stride remains data; later-LOD assets are not resolved or loaded.

- Date: 2026-10-12, after the user resumed and observed the component-7 experiment.
- The user requests whole-character support and explicitly questions the cost of individually adapting every component. Component 7 was a backend bring-up fixture, not the product architecture. Do not expand it into component-number branches or separate handwritten replacement functions.
- Keep the shared Vulkan pipeline/cache, bone import, command lifetime and state-restoration operations. Component hashes, resource declarations, draw ranges, LOD remaps and material bindings are data supplied by the frontend. New supported components must exercise the same operations without backend source edits.
- D-016 still governs INI execution: host the pinned XXMI frontend and replace its backend operations through explicit adapters. A static package/compiler is a backend test input, not proof of INI conditionals, keys, persist, pools, reload or panel compatibility.
- Offline update, 2026-10-12: use command-tree mediation to preserve original CPU evaluation while emitting typed backend requests. Register native Core boundaries before optimisation and retain them as unresolved operations; do not use silent placeholder no-ops. The first adapter passes 28 native checks and executes the sample's 12 draw callbacks without component branches. It does not implement full Core, GPU resources or a production host generation. Details: docs/frontend-draw-plans.md.
- Full-character draws can use bones imported from other components. An independent per-draw master with only the triggering component's palette is insufficient. Establish shared-instance/update identity and completeness before making such draws active; do not render missing bones as zero or mix character instances.
- Preserve real evidence boundaries. This session proves live replacement execution for a small mesh whose original XYZ positions are identical; it does not visually accept M2. The next suitable visual checkpoint should use a substantial character change. Continue offline work using the existing display/LOD0/LOD1 captures.

## D-034: Isolate material images by shader stage and check replacement types

- Date: 2026-10-12, during offline whole-character work following D-033.
- A PS image replacement must not modify the VS's resources just because both stages use the same original Vulkan binding. Relocate only requested FS image declarations into private bindings; retain samplers, original buffers and unrelated shader code. The existing VS skeleton/entry isolation remains independent.
- Image type/dimension, shader usage, actual enabled device features, resource readiness and retained ownership are required in addition to an original-resource slot correspondence. Do not silently overwrite original descriptor sets, guess mappings for unknown shaders, or treat an incompatible operation as a successful no-op.
- The bounded standalone helper and pixel fixture prove the 2D operation, not production pipeline integration. Three 3D original placeholders subsequently receive 2D mod assets under the historical DX11 path; keep the original correspondences but refuse that replacement until its source semantics are established (LIMIT-021).
- Offline 0.9.3 extends this into the shared variant/cache/snapshot backend. Keep material binding order and null masks in the variant key, actual image owners in immutable per-recording snapshots, and source slot resolution in the frontend. Explicit nulls use shader results, not dummy textures. Descriptor budgets include original retained layouts. Headless validation/readback verifies these operations; no whole-character hook or game acceptance is implied.

## D-035: Put the whole character on screen from a static package before linking the frontend

- Date: 2026-10-13, at the user's request after the review of 0.9.2 and 0.9.3 ("replace everything", "do B directly").
- A package built offline from the hosted frontend's plans replaces the live INI engine for now. The plans come from the original XXMI engine, so the draw lists are not a second reading of the INI; what is lost is everything that changes at run time (keys, conditions, reload). This is the "backend test input" D-033 allows, used to reach a visible whole-character result early, as the review recommended, not a replacement for D-016.
- The mod-level logic the package cannot carry is written natively after the core's INI, and only that: identity by position, identification after four components and one frame, import once per frame and component with the lower LOD winning, the mod's LOD variable, hiding. Two deliberate deviations, both towards the game's own picture: the first frame of a new object is drawn by the game instead of skipped, and a draw whose bones are not all imported yet keeps the original instead of using zero matrices.
- Requirements are data, not rules of thumb: which components a draw list needs is computed from the bone indices of its vertices, so a component the game does not draw only blocks the lists that use its bones.
- Textures are step 2. Step 1 is expected to look wrong in color and right in shape and motion; that is its purpose.
- Skeleton copies are shared between draws that read the same bytes, and meshes live in device memory the host can write. Both follow from the review (per-draw copies, system-memory vertex data for 385,000 vertices in five passes).
- One backend per process, chosen by the package's magic; the single-component path of 0.9.1 stays as it is.

## D-036: Borrow slot 3 and give it back; version numbers count in single digits

- Date: 2026-10-13, after the first whole-character session.
- The private set stays at slot 3. When the application has a set bound there, the replacement is recorded and the application's set is bound again with the layout handle and dynamic offsets it was bound with. This rests on two facts: the tracker drops a set as soon as a later bind disturbs it, so a set it still shows is valid and its layout agrees with the current one for the lower sets; and a layout used in a recording outlives the recording. Refusing was safe and wrong for this game. Moving the private set above the application's sets was considered and dropped: the layout at slot 3 would have to match whatever the application bound there.
- A stream of the mod that is shorter than the game's is bound as it is, with the elements where the shader expects them, as the source API does; the mesh buffer is padded so the last vertex reads inside it.
- Version numbers (user, 2026-10-13): three single digits, add one at the right, carry at ten. Recorded in AGENTS.md. The build released as 0.10.0 keeps that name in what was written before; it is 0.9.4 in the sequence.

## D-037: Extend the static character package with exact material contracts

- Date: 2026-10-13, continuing the user's request to finish the next step after the whole-model replacement. Scope stays one character, LOD0/LOD1; the user explicitly authorizes switching back to Vulkan after offline verification.
- Keep the original hosted scenario as data. Store each draw's logical image/null state and exact fragment profiles in a bounded material envelope. Reuse ResourceCache, MaterialProfile, variant compilation and command-owned snapshots. No component-specific branches or new INI interpreter; full hosted keys, conditions, reload and source-identity queries remain T-027.
- Match profiles by exact fragment bytes and retain the entire original component when any material item is unknown, pending or failed. Precompute profile/draw contracts; image bindings keep owners for the command's lifetime, with shared bone slices and sealed resubmission semantics.
- LIMIT-021 is handled by an explicit package option named dx11-invalid-2d-to-3d-zero, based on the existing controlled hardware experiment, for its three annotated slots only. The package records the evidence fingerprint and the measured vendor/device; other devices refuse. Default material selection still refuses dimensional mismatches. This is device-specific compatibility, never a statement of portable D3D11 semantics or proven game pixels.
- Workers prepare uploads; only the application's graphics-queue hook submits them, using exactly one enabled unprotected graphics queue. Device features and loader dispatch initialization are taken from the actual creation chain.
- Full rebuilt regression, historical native contracts/pipeline construction and actual-file/config smoke precede deployment. The user starts the real game. The 0.9.5 flicker fix and all 0.9.6 material appearance stay unverified there until that session.

## D-038: Publish immutable character images to every enabled graphics queue

- Date: 2026-10-13, after 0.9.6 refused graphics-queue setup in the user's real Vulkan session (BUG-008).
- Support all queues of one unprotected graphics family rather than assuming that only one queue is created or that queue index 0 is active. Keep one uploader, selected from the application's actual submission hooks, and retain ResourceCache's single-queue submission contract. Other families/protected arrangements remain explicit refusals.
- After every static image is Ready, enqueue one semaphore signal per graphics queue on the uploader. Only then permit command recording to retain those images. Before forwarding each consumer queue's first application submission, enqueue its one-time wait at ALL_COMMANDS. This supplies cross-queue visibility instead of assuming application synchronization covers private uploads.
- Semaphore/fence allocation belongs to setup, workers never submit to application queues, and no fence or idle wait is added to draw/submission. Shutdown drains outstanding publication objects. Submission errors propagate through legacy, core Submit2 and KHR Submit2 hooks.
- New regression must reproduce the old refusal and prove queue-1-only and separate-upload/render behavior through both entries and all submit APIs. Actual-file/config smoke includes multiple enabled queues. Offline success still does not establish game appearance or the earlier flicker fix.

## D-039: The private set sits right above the pipeline's own sets

- Date: 2026-10-13, after the flicker survived 0.9.5 and 0.9.7.
- The private set's index is the number of sets of the pipeline's layout: 3 for the ordinary character pipelines, 4 for the four-set pipelines of the visibility pass. The variant's layout is the application's sets unchanged plus the private one, so binding the private set disturbs none of the application's and the fragment stage keeps reading its own.
- This replaces the constant 3 of D-022 and D-036. D-036 dropped "above the application's sets" for a three-set pipeline with a stale set 3; that case keeps its answer (use slot 3, bind the application's set again). What is new is that the slot follows the layout.
- The tracker follows four slots. A private set at slot 4 or above cannot be given back, so it is used only while no pipeline layout of the application has ever had that many sets (`Device::max_layout_sets`); otherwise the draw is refused. Widening the tracker was not needed: the game has no layout with more than four sets.
- With a material package a fragment shader without a profile is accepted when its complete layout has no image descriptor for the fragment stage. Refusing there kept the original mesh in a pass that has no texture to get wrong.

## D-040: Unprofiled fragment shaders take a sibling's rules; the mesh is replaced regardless; identities survive gaps

- Date: 2026-10-13, after the 0.9.8 game session.
- D-037 tied material rules to exact fragment bytes and kept the original component for any other shader. In the game that keeps the original mesh in whole states of the character (dithered fade), which is worse than any texture error. Reversed for the mesh: a texture question never keeps the game's mesh.
- Rules for an unknown shader are derived at run time from a profiled shader of the same section and kind of pass, through the images the application has bound: a rule moves to the binding that holds the same image. Chosen over more captured profiles (every state of the game would need its own capture under both APIs) and over replacing by image identity alone (a profiled shader may leave a texture alone that another replaces; the sibling decides). The derived shader then looks like its sibling, whatever the source API would do with the mod's fixed slots in the variant.
- Guard rails: same kind of pass (attachment formats, depth write and compare), every image of the sibling present, at most two images more, one best candidate, rule moves only through certain matches, preparation by the production code, image counts the arenas exist for. Anything else: application's images, retried later.
- An identity is given up after the core's two frames and, in addition, half a second without a draw. A counted frame under Vulkan is a present after which something was drawn, which with frame generation is not a frame of the game.
- The next version number after 0.9.9 is 1.0.0 (AGENTS.md rule); it does not mean a release.

## D-041: The character's frame is a command-buffer recording

- Date: 2026-10-13, after the identity lines of 0.9.9.
- The frame of the mod's logic (same place within a frame, one import per component, "a frame has passed") is the recording of the command buffer that holds the character's draws. Basis: all matched draws of a frame are in one command buffer and one submit in every captured frame, and a recording begins before each frame.
- The present stays as a second clock for periodic work and expiry. It is not a frame boundary at record time under Vulkan: recording runs ahead of presenting.
- Two command buffers with character draws in one frame would count as two frames. The rules tolerate that (place within the radius, a second import), and recordings that interleave are handled by taking the distance between frame numbers without sign.
- Version 1.0.0 follows 0.9.9 by the counting rule of AGENTS.md; it marks no release.

## D-042: The replacement code is a module; the tracker is the host

- Date: 2026-10-13, on the user's request and decision (AGENTS.md: swapping the module into the running game is the agent's choice).
- The line is drawn where state ends: hooks and tracker hold what cannot be rebuilt and stay in the injected DLL; the character backend and everything it owns can start again at any time and is the module. The built-in copy stays the default, so the layer works without any module file and the tests of both paths are the same tests.
- Structures cross as they are, guarded by a stamp over the shared headers and compiler flags. Chosen over a C interface with copied data: the backend reads large parts of the tracker on every draw. The price is that a change to a shared header cannot be swapped; the stamp turns that into a refusal instead of a crash.
- A used module is never unloaded and an old backend is destroyed off the draw path, after an exclusive lock has shown that no call is inside it. Chosen over reference counting on the draw path and over leaking old backends (their textures alone are some 200 MB).
- The new backend is staged and takes over when ready, not at once: otherwise every exchange would show the game's model for as long as textures upload.
- Every exchange in the game is recorded in build/hot-swaps.json; it is a load of new code into the real game.

## D-043: Original main INI in the backend, with native Core contracts and staged F10

- Date: 2026-10-13, user requests live INI conditions, outfit keys, persistence and F10. Same character and LOD0/LOD1 scope; no installed mod, Core or launcher edits.
- Link the pinned original XXMI frontend into both the layer and backend module. Isolate its globals, command/resource maps, pools, original key actions and transitions by generation under a module-local engine lock. Reuse the original expression/parser/matcher/dispatcher; do not make a second INI interpreter.
- A sidecar opts the existing mesh/material profile into live commands and refreshed file assets. Keep the native identity and bone backend established by D-033/D-035/D-041, with explicit Core registration, skeleton initialization/attachment and draw-instance adapters. Verify original evaluated bone ranges and both LOD maps against the profile; there is no per-component code. Accept the profiled mesh override registry only. Source-image hash overrides and arbitrary CustomShaders require their own adapters, rather than invented identities or skipped commands.
- Prepare every reachable DDS branch, including the alternate stocking, but not dormant file declarations. Existing profiled mesh BUF files are revalidated on reload; new stream/semantic structures need profile preparation. Bounded runtime plans preserve order and explicit null material bindings.
- Foreground-process keyboard sampling feeds original listeners. Persist only original persist variables, atomically and off the draw path, in project-owned settings. The original installation and DX11 saved state are read-only and are not used as a save destination.
- F10 parses/prepares a new complete generation while the active one draws, stages queue publication, carries the latest persistent state and suppresses held keys until release. Suspend the old settings writer before adoption. Old sealed commands retain immutable GPU owners and a unique generation token. Failed candidates retain the active generation. Identity/variant warmup may briefly show the game's character; do not promise a visually seamless exchange.
- The user's HLSL question is answered with actual compilation: use DXC to preserve the original panel language and compile it to SPIR-V. Both stages of the unmodified panel validate for Vulkan 1.1. This does not supply its missing Present/backbuffer pipeline, image/IniParams upload or cursor adapters. Keep that work explicitly in T-016.

## D-044: The directory and original language define compatibility

- Date: 2026-10-13/14, explicit user corrections while requesting arbitrary-character support. Yvonne is a second regression fixture; new game collection is deferred. The scope includes every enabled Mod INI and its Core/library/companion dependencies, with shared model, skeleton and texture handling. LOD0/LOD1 remains the requested rendering scope.
- Use the pinned original parser, evaluator, matcher and input system. Existing Mods supply test cases, never a whitelist of accepted syntax. Source parsing and declarations are API-independent; Vulkan adapters must implement each executed graphics operation or refuse it explicitly. A successful parse is not a successful draw.
- Make directory discovery and dependency loading the frontend entry point. Preserve original include ordering, namespaces, disabled names, cross-file references and command priority. Do not choose one character at a time as the production configuration model. Reuse resources and load/upload on demand; a full directory scan does not mean all files become resident.
- Model components, bone ranges, LOD maps, vertex/index buffers and texture choices come from original evaluated declarations and commands. No handwritten per-character/component branches. Keep game shader correspondence in a shared shader catalogue; per-character paired captures are not a normal installation requirement. Unknown shader semantics remain an explicit adapter gap rather than a guessed binding.
- Verify the installed directory offline: original syntax and references, declared file storage/decoding, model draw bounds and bone initialization, then sandbox graphics and generation lifetime. Keep the remaining PNG, CustomShader, ShaderRegex, resource-copy and complete-directory routing work visible. Do not edit original INIs or omit companion files to manufacture compatibility.
- Source audit follow-up, 2026-10-14: inventory the pinned parser dispatch, metadata and operand/operator tables; make missing/new constructs fail coverage checks. Validate parsing and actual execution separately with independent expected values. Retain original key-cycle semantics even when surprising. PNG integration is paused until the requested language/runtime work advances; no all-language claim follows from installed-folder or syntax-only success.

## D-045: Execute original GPU resource commands in an explicit owned domain

- Date: 2026-10-14, user-directed implementation order: general GPU resources,
  then CustomShader, shared shader matching, and complete directory graphics.
  Continue offline until game evidence is actually necessary.
- Keep the pinned original parser/evaluator. The command adapter dispatches
  GPU operations to a generation-owned extension; it does not build another
  INI interpreter. Initial buffers use original description and view-element
  helpers, logical source slots and explicit allocation/view metadata.
- Start with owned resources and private primary command buffers, requiring an
  explicit outside-render-pass scope and host serialization of one queue.
  This is not permission to split game render passes or submit concurrently
  with game calls. Application import and arbitrary inline placement remain
  architecture work; do not sneak waits into the existing character draw hook.
- Original store can affect the same command list's next condition. Read actual
  GPU bytes and wait only in an explicit supported scope; do not silently return
  an older asynchronous value. Regular submissions retain owners through fences.
  Share budget ledgers across reload generations, while isolating mutable data.
- Failed scopes cannot be submitted. Discard makes the candidate unusable;
  original CPU assignments and already-submitted feedback are not transactional.
  A working prior generation retains its own state. A logical source binding is
  not a Vulkan descriptor bind, and buffer execution is not shader execution.
- Image foundation follow-up, 2026-10-14 (1.1.2): require actual physical-device
  image limits and enabled logical-device features in the owned graphics-queue
  domain. Use registry-derived core format footprints, explicit subresources,
  real views and fence-retained allocations. Track pending layouts separately
  from submitted owners; only successful submission publishes GENERAL. This
  does not connect original INI texture semantics, expand the DDS loader or
  authorize application image import. PNG integration stays paused.
- Image-command follow-up (1.1.3): call original texture override/view helpers,
  preserve source metadata and copy ownership, and reuse pinned DDS format and
  footprint decoding. DXGI view compatibility remains stricter than Vulkan's
  compatible-format classes. Clear through the selected view's format, using
  budgeted scratch images when needed; do not convert integer UAV bit patterns
  through floats. Logical RTV/DSV/viewport state does not authorize application
  render-target binding. Integer RTV conversion and other format gaps stay
  explicit. Native D3D11 reference devices exist only in the headless test.
- Conversion follow-up (1.1.4): integer RTV clear conversion follows measured
  native WARP truncation and saturation, with hardware overflow differences
  recorded in LIMIT-029. R16/R32 color/depth bit copies use reusable resident GPU
  scratch without readback. Opaque color-to-D32 transfers require actual enabled
  VK_EXT_depth_range_unrestricted, not support alone. Host float-depth uploads
  validate copied values when that extension is absent (BUG-019). This does not
  authorize changes to an application's enabled extensions or its render passes.
- Parameter follow-up (1.1.5): retain the original float4 Texture1D contract,
  zero-before-Constants initialization, dirty float comparisons, root/draw flush
  boundaries and forced transition publication. iniparams reads the published
  allocation without flushing. Add an explicit initialization hook instead of
  moving existing native Core prepare. Shader consumers use retained views;
  logical stage slots alone do not change application descriptors. A draw host
  callback is optional and absent execution refuses. Keep owned scopes explicit
  through generation preparation and poison them on load/draw failure.

## D-046: Original reflection owns HLSL register identity

- Date: 2026-10-14, compiler foundation 1.1.6.
- Preserve original preprocessing, stage macros, model-5 source validation and
  include rules before DXC compilation. Derive slot numbers from original FXC
  reflection rather than assuming DXC automatic assignment agrees. Reserve
  independent Vulkan binding ranges for stages/classes/counters.
- Keep exact read-only source dependency snapshots and an unchanged old program
  after failed compilation. No caches in installed Mod directories or global
  compiler environment/CWD changes. SPIRV-Reflect is a pinned external source
  dependency under F:/vulkan; retain metadata and strip reflection-only Google
  executable annotations after reading it.
- Independent SPIR-V validation and native DX11/Vulkan output comparisons are
  separate evidence from INI shader execution. Unsupported legacy flags and
  target/device requirements remain explicit. Do not label syntax coverage or
  successful compilation as complete shader/Mod compatibility.

## D-047: CustomShader uses explicit owned domains and retained descriptors

- Date:2026-10-14, owned compute1.1.7. Reuse original keys/state parsers, command
  lists, phases and counters; replace compiler/GPU boundaries. Opt-in preserves
  the character path. All declared sources compile before candidate acceptance;
  failure rejects the candidate instead of original log-and-remove behavior.
- Immutable dispatch descriptors retain owners through ResourceSubmission fences.
  Use the explicit serialized owned resource domain outside render passes and
  shared budgets. This does not authorize application imports or hook waits.
- Validate SPIR-V using actual scalar/uniform layout enablement and a pinned
  validator DLL at an explicit path. Successful DXC compilation alone does not
  prove device compatibility. Unsupported execution cases refuse explicitly.


## D-048: Compute uses executable facts and native source defaults

- Date:2026-10-14, compute follow-up1.1.8. Bound executable storage/provenance
  analysis; refuse unknown layouts/origins. Validate actual sampler/image pairs,
  including array elements; unrelated fetches do not inherit filter restrictions.
- Indirect counts stay GPU-resident and must obey device limits. No hidden CPU
  feedback/wait or semantic-changing count rewrite; fences retain arguments.
- Empty owned source sampler slots use native D3D11 defaults, verified with WARP.
  Explicit host snapshots are separate from INI syntax/application import.
  Sampler quotas are shared within actual host-reserved physical headroom.
- Original owned INI UAV creation does not support append/counter flags. Importing
  application counter views remains separate. Source binding hazards and batch/OM
  effects are required; BUG-022 must be fixed before graphics integration.


## D-049: Source binding state follows native API side effects

- Date: 2026-10-14, binding fix 1.1.9. Logical Vulkan-owned slots represent
  D3D11 state, including read/write hazards and complete OM behavior. Buffer
  ranges are one source subresource; image mip/layer ranges remain distinct.
- Materialize an entire source slot range before publication. Native invalid
  output batches preserve prior bindings; do not publish part of a range.
- Validate semantics using actual system D3D11 WARP calls, with fresh devices for
  invalid calls. Preserve pixel/compute UAV differences and source OM save scope.
  Native queries and owned Vulkan validation are separate from game evidence.
- Correct historical fixtures that assumed invalid simultaneous bindings, while
  keeping their original data assertions. No game or deployment was needed.

## D-050: Validate native signatures before Vulkan IO relocation

- Date:2026-10-14, graphics linker1.2.0. Preserve original FXC register/component
  signatures and SPIR-V IDs. Native D3D11 shares adjacent stage registers; semantic
  identity does not repair an invalid native signature pair. Check that contract
  before relocating Vulkan input locations using case-insensitive source names.
- Bound module/metadata/dependency payload before copying. Keep caller programs
  immutable. Patch/per-vertex dimensions are distinct; wider vectors require the
  logical device's actual maintenance4 enablement, never mere physical support.
- Verify actual pixels against original DXBC on system D3D11 WARP. Compiler,
  independent linker host, original INI draw execution and game rendering are
  separate evidence. An invalid development fixture was corrected before release.

## D-051: Source graphics conventions are measured before draw integration

- Date:2026-10-14, candidate1.2.1. Native SM5 pixel w and vertex/instance IDs differ
  from raw Vulkan built-ins. Use the pinned DXC conversions and actual enabled
  draw-parameter capabilities; do not assume zero offsets or unit clip w.
- Negative Vulkan viewport height preserves source pixel centers, culling and
  front-face input in native pixel references. Original state masks are evaluated
  through the pinned helper. Compare semantic fields rather than struct padding.
- CPU state descriptions, isolated native/Vulkan pixels and production original
  INI drawing remain separate evidence. Continue connecting the owned executor.


## D-052: Graphics shares retained owned resource execution

- Date: 2026-10-14, candidate 1.2.2. Compute and graphics share immutable descriptor
  validation and fence-owned objects. The host explicitly owns the queue domain;
  private passes do not imply permission to interrupt application rendering.
- Validate actual enabled features, formats, state and buffer ranges before
  recording. GPU-produced indices/counts remain an explicit valid-range contract;
  do not introduce hidden waits or CPU readbacks for draw validation.
- Native DX11 pixel references validate graphics API behavior. Original INI stage/
  state conversion and application integration require their own evidence.


## D-053: Original owned draws use effective source state

- Date:2026-10-14, candidate1.2.3. Keep original lists, counters, arguments and
  nested phases; translate effective state at the GPU boundary. Normalize native
  ignored fields before later mask merges, based on actual state creation.
- Original comparison-filter state does not turn ordinary Sample into a depth
  comparison. PS sampler overrides touch slot0 only. Actual Dref instructions
  need their own adapter and refuse until it exists.
- Null PS preserves color while depth/stencil can still execute. Native pixel
  references and failed-root discard establish owned behavior; application import
  and complete-directory execution require separate proof.


## D-054: Validate the complete enabled source corpus with original metadata

- Date:2026-10-14, candidate1.2.4. The pinned parser supplies shader declarations,
  flags, namespace paths and include policy. Do not introduce another INI parser
  or reduce the inventory to selected characters. Preserve built-ins/null/inherit.
- Compile read-only installed sources through the production compiler, retain
  exact include snapshots and independently validate SPIR-V. Record required
  layout options rather than treating relaxed validation as device enablement.
- Full-directory compilation, owned shader execution and application/game
  rendering remain separate milestones. Continue execution work after the audit.


## D-055: Validate owned geometry with unchanged Core HLSL and native output

- Date:2026-10-14, candidate1.2.5. Use actual enabled geometry/layout/raster
  features, primitive modes and bounded interface/output limits. Geometry-stage
  storage belongs to vertexPipelineStoresAndAtomics; stage resource slots remain
  distinct. Last-stage position is required before rasterization.
- Reuse the original installed TextPrinter source and font as read-only inputs.
  Compare actual compute chunk records and rendered glyphs to native WARP; permit
  only bounded floating arithmetic/blend rounding (1e-6, observed5.96e-08), with
  exact integer chunk fields and nonempty foreground/background checks.
- Original HLSL execution in controlled owned fixtures is distinct from complete
  Core INI/window/backbuffer integration and real-game rendering. Continue offline.


## D-056: Preserve source layout identity, edit timing and outer draw lifetime

- Date:2026-10-14, candidate1.2.6. Retain the original input creation signature;
  native registers, not current semantic names alone, determine fetched values.
  Use the pinned element-edit helper and its actual behavior without repairs.
- Native invalid creation preserves the current layout. Unsupported Vulkan fetch
  or device limits refuse explicitly; do not disguise them as source no-ops.
- Apply edits before parameter flush/argument evaluation, including zero draws
  and dispatches. Keep changes through nested CustomShader/Present calls; restore
  at intercepted draw completion, including failure. Shader and GPU resource
  lifetimes remain independent from the immutable CPU layout description.


## D-057: Instance divisors require enabled features and actual base properties

- Date:2026-10-14, candidate1.2.7. Represent instance rate in the graphics
  pipeline and cache identity; query/enable KHR/core features in the owned host.
- Preserve source first-instance addition after division, including divisor0.
  Unknown GPU indirect bases require supportsNonZeroFirstInstance. Do not add
  hidden CPU waits/readbacks. Physical support alone is not logical enablement.
- Retain direct zero-stride fallback, and keep application device feature import
  separate from owned sandbox evidence. Native pixels verify all four draw forms.


## D-058: Preserve fetched vertex bits before shader interpretation

- Date:2026-10-14, candidate1.2.8. The input assembler format determines numeric
  conversion and absent-component defaults. Retype Vulkan inputs to that class,
  then bitcast loads to the original shader type. Do not use numeric casts.
- BGRX supplies float alpha1 before any reinterpretation. Include fixups in the
  cache key even when BGRA/BGRX share the same Vulkan format. Preserve source
  signatures and validate unmodified/lowered modules independently.
- Optional packed16 BGR remains explicit: local DX11 hardware refuses it and
  WARP's successful creation yields only defaults. Creation success alone is
  not evidence for the intended decoded vertex values. Continue other offline
  integration without claiming these formats or application input import.


## D-059: Match phases share one transaction; stage roots retain source boundaries

- Date:2026-10-14, candidate1.2.9. Preserve all-pre then all-post match ordering.
  Abort and transient state are shared within a matched phase, while independent
  root invocations reset transient state. Keep applied GPU state and transaction
  budgets across the complete call. Do not publish a partial failed plan.
- Source ShaderOverride stage scheduling needs independent roots and explicit
  original-call placement. Tracked shader identity differs from temporary custom
  executables. Import proven source facts; do not equate SPIR-V and DXBC hashes.
- Continue the owned implementation offline before application integration.


## D-060: Source shader facts and owned executables have separate lifetimes

- Date:2026-10-15, candidate1.3.0. Preserve the original tracked shader identity
  across CustomShader stage swaps; native query evaluation consumes proven hash
  and DXBC declaration facts. Validate complete snapshots before publication.
- Execute independent stage roots around one explicit original-operation callback
  in the same bounded transaction. No callback may submit or re-enter the runtime.
  A failure prevents submission of earlier recorded work. AfterDispatch does not
  acquire the original AfterDraw input-layout restoration behavior.
- Keep legacy shader substitution, ShaderRegex and application resource/state
  import explicit. Source declaration names alone do not imply an implementation:
  cb_mask/srv_mask return0 in the pinned evaluator, confirmed after correcting
  the initial audit. Owned pre/original/post pixels do not prove game placement.


## D-061: Resource filters use source command matching and explicit identity

- Date:2026-10-15, candidate1.3.1. Reuse original descriptor/draw predicates and
  region helpers. Command matching appends hash then fuzzy results; the source
  creation helper's early return is not equivalent. Keep this query's original
  target type when selecting region versus whole-resource matching.
- Import proven tracking/hash facts separately from GPU objects. Keep weak
  allocation identity through aliases; do not infer original hashes from Vulkan
  handles or newly copied bytes. The source policy owner updates those facts.
- Preserve original dynamic target re-evaluation and pool range reads. Bound
  non-finite casts and total execution, but retain finite invalid-range no-ops
  and unbound slot sentinels. No hidden GPU readback to discover missing hashes.


## D-062: Native regex links retain identity; assembly needs source evidence

- Date:2026-10-15, candidate1.3.2. Compile unchanged original ShaderRegex.cpp;
  retain RunLinkedCommandList type and shared identity inside a bounded wrapper.
  Repeated linking and native unlink/filter restoration must remain valid.
- Keep regex groups/index/hash and source model cache generation-local. Import
  proven native model facts with explicit source hashes. A Vulkan stage/model
  guess cannot justify matching or a silent nonmatch. Native disk caches remain
  disabled so the frontend cannot write installation-relative files.
- Relevant patterned groups currently refuse. Next reuse original disassembly,
  matching/substitution and signature-aware assembly before executable translation.
  Core's patternless GPU result does not establish RabbitFX or game compatibility.


## D-063: Native assembly matching precedes executable publication

- Date:2026-10-15, candidate1.3.3. Use unchanged Flugan disassembly and
  signature-aware assembly around the original regex implementation. Keep
  immutable bounded original DXBC and reject conflicts for a cached identity.
- Prepare against isolated native map nodes so failed translation cannot leave
  linked commands or invalidate live local-variable pointers. Pattern-only
  plans publish; bytecode-changing plans remain analysis-only until a compiler
  and exact executable association are available. Source query facts remain
  original metadata, independently of a patched executable.
- Preserve actual source grammar: only .Pattern is supported by this pinned
  parser. Internal data structures are not evidence for numbered INI syntax.
  Invoke original Rendering getters before regex hashing/disassembly, matching
  the initialization normally performed by the full native loader.


## D-064: Exact bytecode translation keeps device execution requirements

- Date:2026-10-15, candidate1.3.4. Use unchanged vkd3d-shader2.1 for exact DXBC,
  distinct from recompiling original HLSL. Scan instruction/signature facts;
  optional RDEF and application SPIR-V hashes are not authoritative substitutes.
- Only successful native outputs transfer ownership. A failed native compiler
  may free partial output without clearing its pointer. Reject external dump/
  config overrides and retain atomic caller-program publication.
- Preserve source float32 denormal semantics. Strict modules need actual enabled
  float controls and matching properties. The local NVIDIA default is measurably
  different for sign/absolute-value operations, despite matching basic arithmetic.
  Implement and verify software lowering before using this translation path there.
- Keep compiled bytecode, owned GPU execution, runtime regex publication and
  application/game integration distinct. Continue offline without user input.


## D-065: Float32 compatibility separates denormals from other device modes

- Date:2026-10-15, candidate1.3.5. Use bounded scalar/vector integer-bit flushing
  when explicit float32 denormal control is unavailable. Preserve data moves,
  result identity and arithmetic NoContraction decorations. Unsupported shapes
  refuse atomically; never silently drop the source's mode.
- Both paths require enabled SignedZeroInfNanPreserve32 and RoundingModeRTE32.
  An Intel reference test exposed NaN self-comparison optimization when only
  denormal control was declared. Software denormals do not replace these modes.
- Bound GLSL Fract results to the native range below1 without suppressing NaNs.
  Keep exact native WARP comparisons for both local GPUs and retain failing
  evidence separately from the corrected implementation. These tests are owned
  execution, not a real-game validation or whole-language conformance proof.

## D-066: Original buffer views are independent of executable representation

- Date:2026-10-15, candidate1.3.6. Supplement native scanner facts with bounded
  original SM4.x/5.0 buffer declarations. Retain raw/typed/structured class and
  source stride even when translated storage is a scalar word array. Cross-check
  registers/raw flags, and validate the original INI view class/stride at use.
- Typed buffer UAV programs use texel views for native format conversion. Check
  actual logical read/write feature enablement, enabled format queries and
  per-format capabilities using traced binding accesses. Known formats still
  require exact agreement. Capability failures cannot submit partial GPU work.
- Keep owned view adaptation separate from application import and UAV counters.
  A failed resource execution poisons that generation; preserving previous GPU
  output does not imply recovery within the same failed generation.

## D-067: Original INI warnings are recoverable; they never reject a generation

- Date: 2026-10-18. Claude takes over the INI language and runtime work at the user's request ("ini this part is handed to you").
- Basis: the pinned IniHandler, CommandList and ShaderRegex code report problems through LogOverlay and keep what they could parse: duplicate sections keep the first copy, unrecognized lines and broken expressions are skipped, duplicate ShaderOverride hashes are merged with a conflict warning, a numbered Pattern section disables its group. No warning or dire message stops the original loader or an F10 reload. D-044 makes the original language the authority.
- The hosted runtime and the folder inspection rejected any warning. That kept INPUT-001's installed Mod and, in a directory generation, every Mod beside it from loading at all; tests had encoded the deviation (malformed F10 refused, duplicate hash refused, numbered pattern refused, dormant typo refused).
- Now: a generation loads with the original's recovery; warnings are counted, the latest 64 texts kept, logged at load and reported in the status. The inspection completes, lists them and fails only on dire messages. Fixtures still require a reported warning for every invalid construct, so the rule against silent acceptance holds.
- Unchanged: explicit adapter refusals (constructs or operations without a Vulkan implementation, including D-047's compile failures) still refuse the candidate and F10 keeps the active generation for them. Offline package builders stay strict.

## D-068: The execution matrix is the authority for INI execution; overlay and upscaling flip accepted

- Date: 2026-10-18 (candidate 1.3.9, branch claude/ini).
- `docs/ini-execution-matrix.json` replaces the hand-kept execution table of docs/ini-language-coverage.md. Every original command class has an entry; CPU decisions are probed and owned GPU decisions cite sandbox checks, both enforced by a suite step. New classes in a future pinned source fail the suite until decided.
- `special = upscaling_switch_bb` runs as original code: it only clears the generation's bb_is_upscaling_bb flag, whose one original reader is the DX11 `bb` lookup that a hosted generation never runs. If owned backbuffer placement is built, it must honour the flag.
- `special = draw_3dmigoto_overlay` is a no-op: the original draws the 3DMigoto overlay only when the swap chain has one, and a hosted generation has none. This is the original behaviour in that situation, not a rendering feature left out; the rule that missing rendering features refuse stands.

## D-069: Original window and cursor queries read a host sample

- Date: 2026-10-18 (candidate 1.4.0, branch claude/ini), first step of the image panel.
- The original reads window size, cursor position and visibility through GetClientRect, GetCursorInfo and ScreenToClient on G->hWnd and keeps the swap chain size in mResolutionInfo. A hosted generation must not query a real window from inside the game or a test, and its results must be reproducible, so the host supplies one sample per present (RuntimeInput window, cursor, resolution) and the original functions answer from it. G->hWnd is a token that only these functions recognise; nothing passes it to Win32.
- Original fallbacks are kept: no window means no hWnd (window positions are screen positions, window_width falls back to the resolution and then the fallback screen size); a resolution of zero keeps the earlier value, starting from the original's unset -1.
- The window heuristic's render-target step would query the reference D3D context, which a hosted generation does not have. It continues as the original does with no render target bound; RT_WIDTH itself stays refused.
- The fallback screen size is IniHandler's default 1920x1080 because a generation reads no d3dx.ini. The user's d3dx.ini sets 2560x1600; the difference is reached only when neither a window nor a swap chain size is known.

## D-070: Owned CustomShaders use the original shader cache

- Date: 2026-10-18 (candidate 1.4.3, branch claude/ini).
- The pinned CustomShader::compile loads `<file without extension>.<model>.<flags in lowercase hex>.bin` instead of compiling when its last-write time equals the HLSL's exactly, and writes such caches only with cache_shaders. All 19 enabled Mods with an image panel ship `Draw_2D.vs_5_0.8000.bin` and `.ps_` with matching times, so the panel that runs under DX11 is that DXBC, not a recompile of the HLSL beside it.
- GpuShaderRuntime applies the same rule. With enable_cached_bytecode the cache runs through the exact DXBC translation (vkd3d-shader, docs/bytecode-compiler.md); without the opt-in a valid cache refuses, rather than silently running different code from the HLSL. No cache is ever written beside an installed Mod.
- Codex's uncommitted work adds a separate bytecode opt-in for source shaders (ShaderRegex) to the same class; the two can be unified when that work is committed.

## D-071: D3D11 null views become Vulkan null descriptors

- Date: 2026-10-18 (candidate 1.4.3, branch claude/ini).
- D3D11 reads a null shader resource, constant buffer or UAV as zero, reports zero dimensions and drops writes. The image panel relies on it (t101 is null unless minimised). VK_EXT_robustness2 nullDescriptor gives the same behaviour for a descriptor written with a null handle.
- An unbound CustomShader slot is written as a null descriptor when the host reports nullDescriptor enabled on the device (ShaderHost::null_descriptors); otherwise it refuses as before. Only that feature is used; robust buffer and image access stay disabled, so in-bounds behaviour is unchanged.
- In the game this needs the feature on the application's device (step 4): enabling it at device creation or refusing the panel are the options.

## D-072: A generation in the game combines the character extension with owned GPU execution behind a host scope

- Date: 2026-10-19 (candidate 1.4.4, branch claude/ini), phase 1 of completing the INI in the game.
- Replaced character draws must stay inside the application's render pass with the game's pipeline variants (the file plan); original GPU commands (clears, copies, store, CustomShader, non-indexed draws) need the owned outside-render-pass domain. One generation keeps the INI's variables shared, which the image panel needs (it toggles the main INI's outfit variables).
- The owned GPU callbacks therefore apply only while the host has an owned scope open (Present, Constants, key and reload lists). Replaced draws open none and behave exactly as before; a CustomShader inside one refuses until phase 1 step P2 records such work into private command buffers before the application's submission.
- Declaration references executed by the GPU side are mirrored into the plan; references made by replaced draws are not yet visible to the GPU side (recorded limitation). The native extension's preparation runs with the gate closed, and its natively executed CustomShaders are not compiled.

## D-073: In character mode the layer enables what owned GPU work needs on the application's objects

- Date: 2026-10-19 (candidate 1.4.6, branch claude/ini).
- Owned Present work runs on the application's device and swap chain. It needs D3D11 null views (robustness2 nullDescriptor, D-071), vertex IDs with D3D semantics in HLSL-compiled shaders (shaderDrawParameters, core in Vulkan 1.1) and a swap chain image usable as a color attachment.
- Only in character mode, only when supported and not already requested: the layer adds the robustness2 extension with nullDescriptor alone; sets shaderDrawParameters in the application's own Vulkan 1.1 or draw-parameter feature structure for the duration of vkCreateDevice and restores it afterwards, or adds a structure of its own; adds color attachment usage to a swap chain lacking it (every swap chain supports it). Application requests are never changed otherwise.
- The status reports each addition, so a session shows exactly what was enabled. None of this affects the application's own rendering; it is not an anti-cheat measure in any direction.

## D-074: Operands the original answers with Win32 or the device state

- Date: 2026-10-19 (candidate 1.4.7, branch claude/ini), phase 3.
- fps reads the original FPSCounter in the generation's globals; the host supplies the steady clock at each presentation (ticks_us, the original's GetSystemTicks) and the counter advances before the Present list, as in RunFrameActions. Zero leaves it.
- effective_dpi and cursor_hotspot_x/_y are answered by a host-value adapter from the host's sample; the original's Win32 calls (GetDpiForMonitor with a temporary per-monitor awareness, GetIconInfo) run in the layer at the presentation, not inside a generation. Without a sample the original's floor 96 and a zero hotspot remain. This supersedes the "stay refused" part of D-069 for these operands.
- rt_width/rt_height are answered only in the owned domain, from o0 with the original's view rule and state caching; the window heuristic uses the same answer. In replaced draws the game's render target is not known, so they still refuse there (D-069's note stays true outside the owned domain).
- ->hashregion/->spatialhash on owned resources return what the original returns for resources without its CPU data cache: -2 for null, -3 for images, UNKNOWN (-1) for buffers. Hashes of game data stay refused until game bindings are known; Core's spatial identification keeps its bounded adapter.

## D-075: General mechanisms only; captures are ground truth, not per-character data

- Date: 2026-10-19, user ("you must remember, what you are making is general, not adapting characters one by one"), said right after the 16-character catalogue collection. Recorded in AGENTS.md. It restates D-044.
- Every installed Mod and every character, including characters never captured, must work through shared mechanisms. No per-character package, profile, table, branch or capture is a requirement of the product.
- Captures (the 2026-10-19 catalogue collection, the arcane sample) are ground truth: they build and check general rules. The DX11-to-Vulkan correspondence is to be derived from the shaders themselves (DXBC declarations and their vkd3d translation against the game's SPIR-V interface); the catalogue measures how well it works. A rule that only works for captured shaders is not the solution.
- The arcane vertical slice (one package, 20 profiles from paired captures) is history and a regression fixture, not a model for further Mods.

## D-076: DX11 registers come from the game's SPIR-V by declaration order

- Date: 2026-10-19, after the 16-character catalogue (D-075).
- Both of the game's compilers keep the HLSL declaration order (FXC numbers registers, DXC emits variables in it); the DX11 build places read-only buffers ahead of textures. The t register of a resource is therefore its rank among the SPIR-V module's shader resource views, buffers first, each group in declaration order. Checked on 1187 observed slots of 258 shaders, pixel and vertex stage, no exception.
- The runtime derives slot correspondence from the Vulkan module it already sees, for any shader. Captured per-shader profiles (the arcane 20) become test data. Constant buffers follow the declaration order of uniform blocks (size-checked); samplers and UAVs follow it unverified.
- If a future shader breaks the rule, the refusal is per shader and the check names it; the rule is not to be patched per character.

## D-077: DX11 texture hashes are computed from the game's Vulkan uploads

- Date: 2026-10-19, after all 13 directory packages were refused for texture-hash overrides. Correction the same day: a scan of every installed INI finds no texture-hash TextureOverride; the refusals came from mesh sections outside the package (accepted since 1.5.1) and from three hash-only `this = null` index sections, which the registry check counts as texture sections. The mechanism has no installed user yet.
- The original texture hash is crc32c of D3D11_TEXTURE2D_DESC seeded with the data hash of the first subresource (texture_hash_version 0 length rule). For the game's asset textures the description fields the Vulkan side lacks are constant: usage 0, bind flags shader-resource, CPU access 0, misc 0 (TEXTURECUBE for cube maps). The layer computes the hash from the latest complete upload of mip 0, slice 0 (the captures cannot tell it from the first, which the original uses with track_texture_updates = 0) and the image's own description; an image never uploaded from a buffer gets the hash with seed 0, as a texture created without data.
- Checked: 506 of 506 DX11 shader-resource textures by the formula; 345 of 347 Vulkan images in BC/R8 formats give the DX11 hash of the same content (two explained). Uncompressed formats use the first W*H bytes, to be checked against data.
- No per-character or per-texture table is involved. Where the hash cannot be known (partial or unreadable upload, unmapped format, a slot on a buffer or array, an untracked descriptor) the answer is a bound slot without a hash: no texture override matches, the draw is still replaced, and the case is counted (texture_unknown). Implemented in 1.5.1 (docs/texture-identity.md).

## D-078: Character Mods are read from the Mods folder; packages are made in the layer

- Date: 2026-10-19, user: "first do the directory generation, read Mods directly" and "do not go into the game unless unavoidable; first do everything that can be done".
- `m2_mods` names the EFMI Mods folder. Every enabled INI declaring `[CommandListInitializeMergedSkeleton]` becomes a lazy generation; its package is made in-process from what the original engine declares in that generation: mesh sections from the override registry (constant matches and the constants their command lists assign to component and LOD), bones/components/remaps from the original Core initializer, every declared mesh file as a stream, every DDS file as an image, bone streams and material slots found by the live plans. The result passes the package decoder.
- Offline packages (`m2_package`) stay for the tests and older configurations; no runtime path depends on them for a Mods folder. Settings go to a project-owned folder (`m2_settings`); nothing is written into the Mods folder.
- Still one generation per character INI; one engine for the whole folder is the next step (D-075 generality: cross-folder libraries, shared keys).

## D-079: The Core's per-draw source facts come from the vertex module by the Core's own rules

- Date: 2026-10-19, after BUG-027.
- The original Core decides from the bound vertex resources whether a draw may be replaced: the instance-config cb slot (first of vs-cb2, vs-cb1, vs-cb3 with 4096 vectors) and the rolling bone source (vs-t0 a 16-byte structured buffer, with such a window). The layer applies the same rules to the registers D-076 gives the vertex module (declared block sizes, buffer element strides) and keeps them only where they name what the renderer itself reads (window at set 2, binding 0; bones at the bone site). Otherwise unknown, and the Core leaves the mesh to the game.
- Checked on the 57 catalogue vertex shaders (docs/core-facts-truth.json, suite step): the replacement decision agrees for all; for the 49 with rolling bones slot and window agree. C++ and Python agree on all 57 modules. No per-shader table.
- Bone streams under the blend-index edit take the element offset from the draw's pipeline (R8G8B8A8_UINT in binding 2, R16G16B16A16_UNORM weights); the packager's stride rule only where the pipeline has no such element.


## D-080: A Mod's identification threshold, the image budget and the dimension policy follow the Mod and the device

- Date: 2026-10-20, after the capture replay (docs/capture-replay.md) found BUG-028.
- Identification: an object becomes the Mod's once as many of its components were seen at one place as the Mod's own `$\EFMIv1\identification_min_components` asks, read as a constant assignment in the Mod's command lists (EFMI exports set 3 or 2 in DrawInstances) and through command-list adapters to the original command; without one, the Core's global value (default 4). Never a per-character number.
- Resident image budget per character: a quarter of the largest device-local heap, clamped to 512 MiB .. 2 GiB (was a fixed 512 MiB; arcane needs 576 MiB).
- The DX11 dimension policy for a 2D file at a 3D shader image (reads zero on the measured NVIDIA device) is configuration, `m2_dimension_zero = vendor:device`, applied to every package of a Mods folder when the device matches; offline packages keep their own. The replay and the deployment take it from the DX11 dimension evidence; another device gets no policy and such material slots keep the game's image.

## D-081: A character's images are published in batches as they become Ready

- Date: 2026-10-20 (candidate 1.5.7), from the capture replay's first-appearance times and from reading the publication rule of 0.9.7.
- The uploader queue publishes, at each of its submissions, the images that became Ready since the last batch: one signal per graphics queue per batch, at most 16 batches, the last only once every image is Ready or failed. Consumer queues wait once per batch before application work. Views are retained only after their image's batch was enqueued.
- A draw that needs an image not yet published waits for it (the game draws) and moves it to the front of the load queue; a draw that needs a failed image is refused, the character's other draws still replace. Before, one failed image kept the whole character the game's, and unused variants delayed everything.
- `images_published` keeps meaning "all images" for F10 adoption and the offline package's readiness.

## D-082: RabbitFX texture roles by classifying the game's textures (user, 2026-10-20)

- The user chose option 2 of docs/rabbitfx-assessment.md: classify the game textures a character fragment shader samples (format, size and the like, as WWMI-DX12's rabbitfx module does) to find the diffuse, lightmap and normal-map roles, instead of the diffuse-only first-sample rule or leaving RabbitFX Mods on the game's textures.
- The classification must be general (no per-shader or per-character table, D-075) and is checked against the RabbitFX ground truth of the catalogue (the patched DX11 shaders) before any Mod relies on it.
- Same day, the user chose to continue offline before a game session.
- Implemented in 1.5.8 (docs/rabbitfx-roles.md): diffuse = first sRGB asset texture sampled; lightmap = a linear BC texture of its size right after it; normal map = the first BC5 after those, else a sample decoded as a normal (x2 then a square root in its data flow) within 100 instructions, else the next linear BC texture. Agrees with RabbitFX on 462/474/457 of 480 catalogue draws (diffuse/lightmap/normal), all three on 117 of 128 patterns; the 11 others are RabbitFX's own misreadings, named in scripts/check_texture_roles.py. Where RabbitFX takes a lightmap for the normal map the rule follows the shader. Glow, FX and hue need RabbitFX's rewrite and are not reproduced.

## D-083: A section that only changes a game draw's textures redraws that draw with them

- Date: 2026-10-20 (candidate 1.5.9), the next step after D-082 (13 sections of six installed Mods keep the game's mesh and change its textures, mostly through RabbitFX).
- A plan that leaves the application's draw and binds textures in its pre phase, and nothing else, is a texture-only draw: the layer draws the application's draw again in its place with the application's geometry, arguments and vertex stage, through a variant that only re-points fragment images to the private set, with the Mod's images by the same material rules. Post-phase commands are not part of it (DrawPlan/FilePlan pre_items).
- Geometry changes of a game draw without a draw of the Mod (vertex/index bindings alone) stay the game's. docs/texture-only-draws.md.

## D-084: A Mod's hash-only hide sections match by the index region's hash at any count

- Date: 2026-10-20 (candidate 1.6.0).
- A section of the Mod with a hash and neither index count nor first index, whose list skips the draw, is a hide section for any count, as the original matches it; one without a skip stays a possible texture-hash section (D-077). Only an activated character's draws are hashed for them; the hashes are cached per region. docs/directory-generation.md.

## D-085: Element offset edits take the game's offset from the Mod's own restore

- Date: 2026-10-20 (candidate 1.6.1). The game's SPIR-V has no input semantics (no UserSemantic, no names), so an ElementOffset edit cannot be mapped to a Vulkan attribute by name. The constant offsets a Mod's lists give one (slot, semantic, index) are collected; with exactly two, the value the draw's edit does not set is the game's, and the attribute of that binding at that offset is moved. No capture or table; a Mod without the set-and-restore idiom stays refused.
- Same version: `ResourceX = ref ps-tN` keeps the application's texture of the matched draw; bound back at its own slot it is the application's binding.

## D-086: A Mod's own identification runs over the Core's internals; the native draw honours its result

- Date: 2026-10-20 (candidate 1.6.2, LIMIT-034). Si Lunabelle's optional guard replaces the Core's identification call with its own list over the Core's spatial pools and output lists. The renderer still identifies by place first (D-041); within an identified draw's plan, `Original_VS_CB->SpatialHash` is answered on the CPU from the renderer's instance entry (the same 64 floats, the same translation), DRAW_NUMBER counts planned draws, and the Core's lists run as written.
- The native `Component_DrawInstances` reads `$PoolSpatialIdentity_IdentifiedInstances[0]`, reset before each draw: original keeps the application's draw for the whole plan (sticky against a later `handling = skip`, as the original Core's explicit draw of the original instance is not undone by it), skip draws nothing, custom or no result draws the Mod. The native identification call writes the custom result itself.
- General: any Mod that drives the Core's identification itself, by the Core's own lists; no per-character rule. A guard's own reads of game bindings beyond the instance entry still refuse.

## D-087: GPU work inside a replaced draw runs as owned work before the application's submission

- Date: 2026-10-20 (candidate 1.6.3, P2). A replaced draw's plan runs with a Draw scope of the owned runtime (recorded without a queue). The draw's own bindings and draws keep the plan path; CustomShaders, copies into the Mod's resources, references to owned content and everything inside a CustomShader are owned work. Owned work never reads or binds the application's bindings (refuses), and store refuses (no feedback mid-recording).
- A vertex binding of owned content binds the owned buffer (PlanItem/FilePlanItem/CharacterDraw `owned`), kept alive by the recording. The work is submitted on the graphics queue before the application's next submission there; barriers at both ends of every owned operation order it against the frames around it. Within one submission an owned resource holds its last content for all draws (Typhoeus writes the same each pass).
- Render target presence and identity (`oN`, `@oN`, `@oD`) come from the draw pipeline's subpass (location N is DX11 oN). A refusal in a Draw scope fails the GPU side only once owned work was recorded in it.
- Physics that reads the game's bindings (Typhoeus' camera pass) stays off: the native Core leaves `merged_skeleton_connected` unset, so the Mod's own gate finds no camera pass.

## D-088: On NVIDIA, a translated module with a division loses RoundingModeRTE

- Date: 2026-10-20 (candidate 1.6.3, BUG-031). NVIDIA driver 591.86 (RTX 5060 Laptop GPU) ends the process inside its shader compiler for any module with `RoundingModeRTE 32` and an OpFDiv, OpFRem or OpFMod (a three-instruction compute shader shows it; FAdd, FSub, FMul are fine, and so is the module without the mode). `frontend/gpu/driver_workarounds.h` drops that execution mode for such modules on vendor 0x10de at both module creation sites; NVIDIA's 32-bit float arithmetic rounds to nearest even without it. Other modules and vendors are unchanged.

## D-089: Sections acting on game shaders stay inert in a Vulkan character generation

- Date: 2026-10-20 (candidate 1.6.4). User: do every item offline, Yvonne/Xaihi included. A DX11 shader hash cannot be derived for a Vulkan pipeline (SPIR-V compiled separately, game assets encrypted, no interface rule), and a capture-derived DX11-to-Vulkan shader table would be a table the runtime depends on (D-075), which needs the user's decision.
- Replaces the earlier rule "reject the whole candidate instead of silently dropping active features" for ShaderOverride and ShaderRegex: they load inert and are named in the generation's warnings, so the Mod's meshes and textures apply. `vs`/`ps` in a replaced draw read "named by no ShaderOverride" (0 bound, -0 absent, from the pipeline's stages); `oN` reads -0 unbound and 0 bound (no Mod TextureOverride matches a game render target here). Declaration queries still refuse.

## D-090: Owned work follows the presentation's queue; uploads on any graphics queue; stall watchdog

- Date: 2026-10-20 (candidate 1.6.5, BUG-032). The game presents on either of its two graphics queues. An owned runtime bound to another queue than the presentation's waits for every earlier owned submission (CPU fence wait, a rare event) and moves to the new queue; the presenter counts queue_changes. Draw work still goes only to the bound queue.
- Image uploads and their publications go to whichever graphics queue the application submits on: a Prepared upload has no predecessor in flight, and an image is published only after its upload completed, so no queue order between them is needed.
- `m2_owned = 0` (EFMI_VK_M2_OWNED) turns owned GPU execution off: CPU generations, the Mod folders' main INIs only (as the offline dry run).
- Stall watchdog in the layer: every hook records the thread's last entry; hooks that can wait (submission, presentation, vkWaitForFences, vkAcquireNextImage(2)KHR, vkQueueWaitIdle, vkDeviceWaitIdle, vkWaitSemaphores) also mark the thread as inside. With no presentation returned for five seconds the log lists threads inside such a hook and threads whose last hook is older than two seconds, every ten seconds while it lasts. Never in passthrough mode.

## D-091: A consumer queue waits only for a publication that completed on the GPU

- Date: 2026-10-20 (candidate 1.6.5, BUG-032). Until 1.6.4 the layer enqueued, before an application submission on a graphics queue, a wait for each image publication signalled by its own submission on the upload queue. If the application lets work on that other queue wait for a signal it submits later on this one (allowed with timeline semaphores), the inserted wait holds this queue while the other waits for it: a deadlock of both queues. The game uses two graphics queues, and the session froze about a second after Si's images were published.
- Now a batch publishes its images only when its signal submission's fence has completed (checked with vkGetFenceStatus at each application submission); consumer queues enqueue waits only for completed batches, which are already satisfied and cannot block. A recording retains an image only after its batch completed, and that recording's submission comes after the wait for it on its queue. Images become usable a frame or two later than before. D-040's semaphore dependency stays.

## D-092: A character package activates only with device memory to spare

- Date: 2026-10-20 (candidate 1.6.6, BUG-033). Each package used to check only its own image budget (a quarter of the heap), so switching through characters piled images, meshes and snapshot batches onto the game's own memory until the device was oversubscribed. A lazy package now activates on its first match only when the largest device-local heap's budget minus its use (VK_EXT_memory_budget, a physical-device query) covers what it takes (its DDS sizes up to the image budget, the mesh, 128 MiB of snapshot batches) plus a reserve (`m2_reserve_mb`, default the larger of 1 GiB and an eighth of the heap). Otherwise its character stays the game's; the check repeats about once a second (counter waiting_memory, a log line every 30 seconds). Without the extension nothing changes.
- Not yet: releasing the memory of packages whose characters are no longer drawn.

## D-093: Several character packages share one recording

- Date: 2026-10-20 (candidate 1.6.6, BUG-033). A command buffer keeps one owner per replacement backend (`CmdBuf::owners`, `owner_for`); an owner another backend left unused gives way as before. The open world draws several characters in one command buffer; before, only the first package replaced its draws and the others kept the game's, and packages took command buffers from one another, emptying their snapshot pools.
- The stall watchdog also prints what a thread does inside the layer (StageMark: character submission work, image queue lock, consumer waits, character Present, character draw, driver vkQueueSubmit or vkQueuePresentKHR).

## D-094: Textures are loaded on first bind and evicted under a budget

- Date: 2026-10-20 (candidate 1.6.7, BUG-033). User: study 3DMigoto's loading, use Vulkan's own means, performance to the limit. 3DMigoto substantiates a resource on its first use and the DX11 runtime pages unused textures; the layer loaded every DDS of a package at activation and freed nothing, so three characters filled the card.
- One `TextureResidency` per device: images declared without IO under the generation's tag; loaded on first bind (the draw stays the game's meanwhile); a target from the cap (`m2_texture_budget_mb`, default heap/2) and the device's free memory beyond a reserve (VK_EXT_memory_budget, `m2_reserve_mb`); over it the least recently bound images not bound in the last two frames are released (recordings keep their GPU objects until they complete); under pressure the largest resident full-size texture is demoted and loads without its top mip level (a quarter), one per frame, and demands made under pressure load reduced (`m2_texture_reduce_mb`, default 16). A reload candidate reads its files again, preloads what the live generation shows and is refused when a file fails. Snapshot arenas in host memory. `m2_texture_preload` for tests. docs/texture-residency.md.

## D-095: Timeline publication; memory priority and pageable device memory enabled by the layer

- Date: 2026-10-20 (candidate 1.6.7). Every upload signals one timeline semaphore with its serial; before an application submission on a graphics queue the residency submits one upload, reads the counter and enqueues an already satisfied wait for it on queues that have not waited for that value: cross-queue visibility without per-package batches, binary semaphores or fences, and no wait that can hold a queue (D-091).
- At device creation the layer enables, when supported and not enabled by the application: timeline semaphores (core 1.2, else VK_KHR_timeline_semaphore), VK_EXT_memory_priority (the layer's textures at 0.25) and VK_EXT_pageable_device_local_memory (`m2_pageable = 0` leaves both off), as it does nullDescriptor and shaderDrawParameters. This GPU (RTX 5060 Laptop, 591.86) supports all three, sparse residency and ReBAR.

## D-096: One index of the packages' draw counts per device

- Date: 2026-10-20 (candidate 1.6.7). Every draw asked every package in turn (22 shared locks and 22 hash lookups per draw, thousands of draws per frame). The device backend keeps one index from index count to the packages with such a section, and the packages with hash-only sections; a draw looks up its count once and asks only those. Rebuilt after a frame in which a controller's generation changed (F10), so one frame after an exchange may miss a count only the new generation has.

## D-097: Idle-only eviction, a frame gate per package, one index region hash per draw

- Date: 2026-10-20 (candidate 1.6.8, BUG-034). The 1.6.7 session evicted images bound two frames earlier (574 evictions against 591 loads), so the forward pass of an object lost its texture while its other passes were already the Mod's: the depth test failed and the character turned black. Every activated package with hash-only sections also hashed each draw's index region itself (24 us per draw).
- Eviction only of images no frame bound for 120 frames, least recently bound first, down to the target; what the frames keep binding stays, and a working set over the target shrinks by one demotion (reduced load) every 30 frames. A load goes ahead beyond the target within the cap and half the reserve; beyond those the images not bound for eight frames go first, then the image waits. The device's free memory is sampled twice a second and corrected by what was loaded since.
- Frame gate: a package replaces draws in a frame only while every image and pipeline variant its draws needed in the frame before is in place (the residency's `ensure` demands what is missing), and the first missing one closes the frame for the package's remaining draws, hides included. Whole frames go to the game's mesh, never parts of one.
- The device index is by hash: a draw's index region is hashed once (a fingerprint table per recording thread) and only the packages with a section of that hash are asked; counts and first indices are checked there; hash-only sections act for activated packages only, as before. docs/texture-residency.md.

## D-098: The presenting thread never waits for the GPU; a presenter's semaphores outlive it

- Date: 2026-10-20 (candidate 1.6.8, BUG-034). At F10 the presenting thread and the submitting thread froze inside the layer's stages; the presenting thread held a controller exclusively during the generation's Present work, waited for fences when the presentation changed queues, and a retired presenter destroyed its `done` semaphores at once, though a presentation queued in the same frame could still wait on one (the retire runs on another thread as soon as the exchange is decided).
- A controller is held shared during its generation's Present work and exclusively only for the exchange; `follow` rebinds the owned work to the new queue only once the earlier submissions completed on their own, skipping the owned Present meanwhile (`queue_waits`); a presenter's semaphores go to the device's retired list and are destroyed on the presenting thread eight presentations later (all at device destruction). The timeline publication (D-095) was checked: uploads complete one at a time, so its signals are monotonic.

## D-099: The recording path measured in the game and cut (1.6.9)

- Date: 2026-10-21 (fifth game session, 1.6.8, BUG-035). The replacement halved the frame rate of the open world (41 ms against 21 ms with it switched off through the probe). Measured through eight exchanges of the module in the running game, each verified by the next status reading. docs/recording-performance.md.
- Texture reserve default 512 MiB (a sixteenth of the heap; 1 GiB and an eighth before) and eviction only after 600 frames idle (120 before): with 889 MiB free the 1 GiB reserve had put the target at its 64 MiB floor and evicted everything two seconds idle, reloading characters on every LOD change and menu switch.
- Index hashing: the per-thread table holds 65536 regions and is trimmed of idle entries, never emptied; the edge fingerprint (two uncached reads over the bus, most of a draw's cost) is read every draw only for a region whose hash a package acts on, every 16 frames for the others; regions are hashed again every 1024 frames; a buffer remembers its last 32 writes by range and a region's generation is the newest write overlapping it (layer).
- A recording keeps another package's command owner until 8 resets found it unused: dropping it at the first draw of every recording destroyed its arenas and had its worker make new ones (142 us per draw).
- Snapshot bone copies reused by key in any order, arenas in cached host memory; a backend disabled through the probe hashes nothing; the owned GPU tools are found beside the layer when the module lives elsewhere; stage timing per package and recording timing per device in the status, per-command timing behind EFMI_COMMAND_TIMING.
- Result in the same scene: 24.5 ms mean against the 20.9 ms baseline; plan (INI, about a hundred commands per matched draw) and bones are what remains, to be worked on offline through the replay.

## D-100: Identities survive dashes, cuts and brief occlusion

- Date: 2026-10-21 (sixth game session, 1.6.9 to 1.7.0). Characters at close range reverted to the game's mesh for a moment: 42 identities created in ten minutes, each costing identification frames and bone imports. Nine were the same object's passes a cell apart within one frame; about twenty were identities expired after 0.5 s unseen; the rest were moves of 4-7 metres in one frame.
- Within one recording an object's draws may differ by one cell; an identity expires after 10 s (and 4 presentations) without a draw (`m2_identity_expiry_ms`, the tests use 500 ms); when no identity is within the radius and exactly one identified object of the package has not been drawn since the last presentation, it is adopted as moved (`identities_adopted`). Verified in the running game by module exchange: 8 identities in three minutes, 4 of them first ones. docs/recording-performance.md.

## D-101: Material rules for every slot; a pipeline cache on disk; parallel variant builders

- Date: 2026-10-21 (sixth game session). In the character menu a switched character showed the game's mesh, became the Mod after two seconds and flickered back two or three times: derived material rules were made for the slots bound so far, a plan binding a new slot derived them again and the variants compiled again with new patches, the frame gate closing each time.
- A fragment shader's rules cover every DX11 pixel slot (one image or absent), derived once per shader; `refresh_materials` only adds plans for new draws. Variants are created through a device-wide VkPipelineCache kept in the settings folder between sessions (the variant passes it through a creation thunk; the recipe's signature is one host and module share), and each variant cache builds on two to four threads. The rules change was exchanged into the running game and verified by the log (no repeated derivation); the cache and the builders take effect at the next start.

## D-102: A placeholder image for textures on their way (withdrawn)

- Date: 2026-10-21. Tried for the menu's flicker and withdrawn the same hour at the user's word: grey parts are no better than the game's mesh; the requirement is the original's behaviour (the first frame is the Mod, nothing flickers). Nothing of it remains in the code.

## D-103: Resources ready before the draw: the layer's upload queue, waiting draws, one variant cache

- Date: 2026-10-21 (sixth game session). The user: the first frame of a switched character must be the Mod and nothing may flicker. The original loads a resource synchronously when a draw first uses it.
- The layer requests a queue of its own at device creation (a transfer-capable family with a queue the application did not take); images and buffers are shared between the two families; the residency's three loaders submit uploads there and publish through the device timeline; a loader finding an aggregate budget held by the others goes again later (ten seconds at most), every other failure is final at once (a broken texture file still refuses a reload candidate). A draw waits (bounded, 4 s) for a texture on its way and for a pipeline variant still building; the frame gate covers failures and timeouts; an object's first frame is skipped as the Core does, not drawn as the game's, and so are a package's draws while it goes to the GPU (`preparing`). One VariantCache per device, shared by every package, built on 2-4 threads through the disk pipeline cache. The texture timeline semaphore is retired, not destroyed, when the residency stops. Needs the game restarted; verified by the suite before deployment. docs/recording-performance.md, docs/texture-residency.md.

## D-104: A presentation without a draw replays the last Present's drawing

- Date: 2026-10-22 (seventh game session, BUG-036). With frame generation the game presents generated frames between the rendered ones, nothing drawn in their intervals; what a Mod draws at presentation time must be in every presented image.
- The Present lists run once per rendered frame, as the original's run once per frame, the INI's frame time and frame counter with them. A presentation without a draw, when the last Present drew into or copied into the back buffer, runs the Present lists again from the variable state that Present started from and restores every variable and parameter afterwards; no key is dispatched, no transition advances. The GPU resource runtime notes whether a scope targeted the back buffer; a Present that did not (Typhoeus' per-frame work) is not replayed. `replay()` in the INI runtime, `last_drew_backbuffer()` in the presenter. The keys, window and cursor are sampled once per presentation for every package (they cost per package before: 1.7 ms to 0.5 ms per presentation with 22 packages).

## D-105: Persistent INI variables are kept per Mod folder

- Date: 2026-10-22 (BUG-039). Two Mods of one character declare the same namespace; the toggles the one persisted must not come up in the other when it takes its place. The settings file is named by the namespace and the Mod's folder, a departure from the original's one `d3dx_user.ini` by namespace, chosen for the user's expectation (a new Mod shows what its INI shows by default).

## D-106: The recording path cut offline against the 120 fps bar

- Date: 2026-10-22. The user's bar is the frame rate without the replacement (120 fps against 91 with it). Measured in the game and offline through two profiling builds (`EFMI_VK_COMMAND_TIMING`, `EFMI_VK_PHASE_TIMING`, scripts/build.py and build_xxmi_runtime.py) with the perlica replay: the INI commands themselves took a fifth of a matched draw's list execution, the planner's frame around them the rest.
- Command kinds decided once at adaptation (bits per bridge command, `DrawPlanAdapter::Runtime::classify`); native commands with CPU arguments left unwrapped; the Core extension's variables, lists and resources found once per generation; a per-generation variant lookup; plan items' streams and images by resource object; the fingerprint of a stable region read every 16 frames after 120 matches. docs/recording-performance.md has the numbers. Not changed: what the lists do; the original's semantics of every command.

## D-107: Whole-frame hashing only for an activated package with hash-only sections

- Date: 2026-10-22. A hash-only section (a hidden game mesh without an index count, D-084) acts for an activated package only, yet one such section anywhere (chengqianyu, last rite, one each) made every indexed draw of the frame hashed: 177 hash calls a frame with 147 misses in the seventh session, about 0.5 ms. The index keeps whether each package was activated and is rebuilt when that changes; only an activated package's hash-only sections make every draw hashed. `fingerprint_read_microseconds` in the status times the edge fingerprint reads. Verified by the suite (build/next/run_all-1.7.3a.out, 35 steps); the game's numbers come with the next session.

## D-108: A sampling profiler, and the allocations and repeated lookups it found

- Date: 2026-10-22. Offline work against the 120 fps bar continued with a profiler of the project's own (`scripts/sample_profile.py`: suspend, context, dbghelp stack walk and PDB names; `--dump`, `--delay`), the replay without the validation layer (`EFMI_VK_REPLAY_NO_VALIDATION=1`) and the profiling runtime built with debug information.
- Changed: the D-104 replay snapshot only where the last Present drew into the back buffer, as a flat copy over a variable list made once; the per-draw containers of the planner, the runtime, the file plan compiler, the program and `record()` kept for their capacity; a sorted-vector slot map in the plan; an external name copied only for a kept item; a draw's pipeline, registers, descriptor set and pixel slot answers found once per draw; command pools and fences of completed GPU resource submissions reused (`SubmissionRecycler`, ResetCommandPool and ResetFences in the resource dispatch). Semantics unchanged; docs/recording-performance.md.

## D-109: Correct ownership and invalidation before performance acceptance

- Date: 2026-10-24 (1.7.5, BUG-040 through BUG-042). Revoke only D-106's stable-positive fingerprint backoff: coherent mapped memory can change without a tracked write. Positive matches use the existing per-draw edge fingerprint again; unmatched regions retain bounded backoff and full hashes retain their existing schedule.
- Capacity reuse never extends GPU resource ownership past the logical draw. A scope guard releases scratch image/variant owners, while command snapshots independently retain resources through completion. Retiring a pipeline also removes the fast lookup's owners. The native test includes production paths under a test-only macro, absent from the deployed build.

## D-110: Batch first-use work and reuse validated bone staging

- Date: 2026-10-24 (1.7.6). A logical draw queues its selected textures before waiting so the existing loaders can overlap work under the unchanged budgets. ResourceRequest notifies terminal/released states; one common four-second deadline covers the texture batch. No future timeline value or incomplete image becomes a binding. A separate common deadline covers the draw list's requested pipeline variants, with retirement/shutdown waking waiters promptly. Ready paths reuse storage and release scratch owners at draw exit. draw_waits now counts batches; draw_wait_images records bindings, including duplicates.
- MergedSkeleton configures one largest-component two-pose staging buffer, then uses SSE2 on exact float4 rows. Multiplication still runs for scale 1; scalar rounding, signed zero and FTZ/DAZ semantics are checked byte for byte. Both poses and all stamps remain unchanged on rejection. No per-character data or capture dependency is added, and GPU bone remapping is not claimed.
- Acceptance uses corrected 1.7.5 as baseline, separate first-use and post-warmup counters, paired replay runs, final pixels/coverage/residency checks, and the complete offline suite. Cached CPU importer speed is not a game frame-rate claim. Real mapped reads, character switches, reloads, dashes and long-session ownership still need the user's game session.

## D-111: Validate the original transparency library and observe its game boundary

- Date: 2026-10-24 (candidate 1.7.7). The user requested EFMITTL compatibility. Exercise its unchanged INI and both cached DXBC/source HLSL on owned GPU targets, with independent pixel/depth expectations; do not replace the library with plain unsorted alpha blending or claim that shader compilation establishes support.
- Captures retain bounded record-time framebuffer/attachment/subpass facts only while capture is armed. Metadata ownership survives handle reuse without retaining application Vulkan objects. Unknown data stays unknown; no character-specific mapping is created.
- Actual integration requires shared library frame state, general Vulkan shader/pass facts and ordered capture/composite work against the application's targets. Historical captures lack target aliasing and cannot prove the composite boundary. Keep game shader hooks inert until their executable boundary is implemented and tested; obtain that evidence in a user-started session with the new probe. docs/efmittl-compatibility.md.

## D-112: A generation loads the libraries its INIs call by namespace

- Date: 2026-10-24 (1.7.8). The user: finish the unfinished EFMITTL adaptation. The original loads the whole Mods folder into one space; a character generation holds one Mod, so it takes the libraries it calls: every enabled INI of the Mods folder outside every character Mod's folder whose declared namespace an input references (`CommandList\NS\`, `$\NS\`, `Resource\NS\`) and no input or the extension (RabbitFX) declares, then those they call. The folder is read only when a reference names an undeclared namespace. A library with `[TextureOverride...]` sections is left out (it would act once per consumer); the log names each library loaded or left out. General: no library or character is named in the code.
- A library is parsed after the inputs under its path relative to the Mod folder (RuntimeConfig::libraries), so the files it names resolve against its own folder as the original's namespace path does.
- In a replaced draw's plan, or in a generation without owned GPU work, a declaration operand in resource or identity mode is answered from the plan's references: -0 / 0 when it holds nothing, 0 / its identity when it holds a resource of the generation (the original keeps no hash for what it made itself); an application texture held by a declaration refuses as before.
- The library's frame state is per generation. Its game hooks stay inert (LIMIT-036), so EFMITTL's FrameReset selects its own fallback (opaque parts) after the first forward frame; transparency in the game remains LIMIT-038.

## D-113: EFMITTL translucency: the library's policy, the layer's facts and GPU work

- Date: 2026-10-25 (1.7.9). The user: finish the unfinished EFMITTL adaptation. The library's INI keeps its policy unchanged (which pass draws, captures or leaves a part out, its opaque fallback). Its four DX11 ShaderRegex hooks cannot be evaluated on SPIR-V (outputs pruned per pipeline), so the layer supplies their meaning from the game's SPIR-V: the lit vertex shader by its output interface (a subset of 1479b2b5's signature at locations 0-8 with the UV and the instance kept), the copy shader by its structure (one explicit-LOD sample stored to the only output), the shadow hooks never (the Vulkan shadow vertex shaders output no UV; the library's rule keeps the full shadow). Both rules are checked against the paired catalogue (scripts/check_shader_hooks.py), not tabled.
- CommandListCapture is a native boundary (efmittl_bridge.cpp) emitting translucent draws; the renderer records the library's capture (game shaders into RGBA16F + D32 layers, opacity with the library's alpha.hlsl) at the end of the forward pass and composites with its composite.hlsl into that pass's colour target over its depth. The layers are the device's, one composite per forward pass for every consumer, as the library's single frame state. Like RabbitFX's roles (D-082) the bridge knows one library's interface by name; a library without it keeps its fallback.
- The composite runs at the end of the capturing pass rather than after the next copy draw: for the forward pass that is the same point (its next pass is the copy) and the layouts are the pass's own, with no barrier tracking of the game.

## D-114: A working-set floor for textures under pageable memory; eviction for free memory

- Date: 2026-10-25 (1.8.0, BUG-047). The user: characters must not revert to the game's model, and a switched character must show at once (the parity bar). With pageable device memory a load within the floor (the reserve's size, 512 MiB on this machine, at most the cap) goes ahead whatever the free memory: the system pages what is used least, our textures first by their 0.25 priority, as DX11's residency does for 3DMigoto. Without pageable memory the floor stays 64 MiB.
- The budget target never drops below the floor, so idle eviction (600 frames) starts only above it: party members out of view keep their textures.
- Beyond the floor a load short of free memory first evicts what no recent frame bound (8 frames), least recently bound first, until it fits; before, eviction under the free-memory limit was bounded by the cap and freed nothing. Released bytes count as free at once (`released_since_sample_`).
- Large textures still load without their top level while free memory is under twice the reserve; no further reduction (a reduced image has no way back to full size without a reload, and a reload closes the draw's gate).

## D-115: EFMITTL layers made per layer when first drawn into, released when idle

- Date: 2026-10-25 (1.8.0, BUG-047). A layer set of a target size makes the small parts (parameters, neutral mask, passes, pool, two 1x1 spare images) at once and each of its four layers (both faces, RGBA16F and D32, about 100 MiB at 2560x1600) when a pass first draws into it; the composite's set holds the spare images for the layers not made (the library's composite reads only the layers in use), keyed by the scene depth and the layers made. A set no pass used for 5 s is released (Translucency::frame from character_frame); recordings still using it keep it. Status `translucency.layer_sets`, `layers`, `layer_bytes`.

## D-116: CustomShader sources DXC refuses run as the original compiler's bytecode; one that compiles nowhere is skipped

- Date: 2026-10-25 (1.8.0, BUG-046). The user asked whether every character and every Mod works now; a Mod whose folder name is outside ASCII and whose shader DXC refuses ended the game. 3DMigoto runs what fxc compiles, so fxc is the reference: when DXC refuses a source that fxc took, the generation translates fxc's bytecode (the translation cached `.bin` files already use) and counts it (`reference_programs`). DXC sees the preprocessed text with its `#line` files as ASCII aliases, its diagnostics renamed back; a structured exception inside DXC refuses that compile and every later one in the process.
- As upstream ParseCustomShaderSections: a CustomShader that does not compile is skipped with "Error compiling custom shader" (an original warning), its command list left empty; its runs do nothing and the rest of the Mod loads. Before, the exception refused the whole generation.

## D-117: The merged skeleton is imported on the GPU, as the Core does

- Date: 2026-10-25 (1.8.2). The user: think about performance as a whole, not Mod by Mod. In the 1.8.1 session the layer's steady cost was about 5 ms per frame on the game's recording threads, 50-90 us per matched draw, of which 17-33 us was "bones": the CPU reading each component's bone matrices (two poses, about 25-60 KB) out of the game's write-combined mapped buffer with streaming loads, and the master then copied into every recording's arena. Offline, where the same data is in cached memory, the read costs 1 us: the bus is the cost, and no CPU-side change removes it. 3DMigoto never reads bones on the CPU: the Core's MergedSkeleton_BoneDataImporter compute shaders copy them on the GPU.
- The layer now does the same (bone_merge.cpp, layer/shaders/bone_merge.hlsl compiled to SPIR-V by the build). Per identity a ring of eight merged skeletons lives in device memory (the implicit identity matrices, then two poses); each frame takes a slot no recording still reads and no submission still writes. A drawn component becomes a job (the game's bone buffer as bound at the draw, the entry's two row offsets, the component's range, its LOD remap table); the jobs of all packages are dispatched in one submission of ours on the application's queue right before its own submission (character_submit), with a barrier to the vertex shaders, a timeline semaphore for completion, and a wait on the other graphics queue when the dispatches ran there. The draws bind the ring slot directly (DrawSnapshots::append_external): nothing of the skeleton is copied per recording any more.
- What stays as it was: identification, the entry patch, the frame gate, the LOD1 remap, "a lower LOD replaces" (a later job of the same component in the same frame replaces the earlier one), bones between components (zero). The CPU import remains the fallback (no ring slot, an unaligned or short source range, `m2_gpu_bones = 0`; the probe's `set gpu_bones=0/1` switches at run time for an interleaved A/B). The non-finite check of the CPU import has no GPU counterpart, as in the Core.
- Verified offline: the replays vk03, vk04 (Si Lunabelle, remaps) and vk01 (Yvonne, the translucent capture path) render identical pictures with the import on the GPU and on the CPU; validation clean; one submission per frame. The gain itself shows only in the game (the write-combined read is absent offline): to be measured with the interleaved A/B in the next session.
- Amended the same evening (1.8.3, BUG-050): the first version made the draws' queue wait for the dispatches' queue when the game had submitted the frame's draws on the other graphics queue; with the game's own semaphores between its queues the GPU deadlocked within two minutes. D-091 stands for the bone merge too: a queue never waits for the other; a completed batch on the other queue gets an already satisfied wait, a running one is dispatched again on this queue (`bones.redispatches`), the writes being identical.
- Amended again (1.8.4, BUG-052; the user: think it through, and follow 3DMigoto): the 1.8.3 session hung the GPU and the driver crashed within two minutes. The dispatches of a frame had been batched and signalled one timeline from whichever queue took them, so two queues signalled the same timeline out of order (undefined). The design now follows the Core's own ordering as closely as Vulkan allows (a compute dispatch cannot sit inside the game's render pass, so it cannot go between draws as under DX11): the dispatches belong to the recording whose draws read them (CharacterCommand::bone_jobs, one per component, the later import of a component replacing the earlier), and at the application's submission of those command buffers they are dispatched on that queue right before them (the module's submit entry takes the recordings). Nothing crosses the queues: no wait, no shared semaphore; each queue has a timeline of its own, read only for the slots' reuse. A recording never submitted drops its jobs at its reset.
- Found on the way: a layer that allocates command buffers must give them the loader's dispatch pointer (vkSetDeviceLoaderData); without it the validation layer below faulted in the layer entry while the inject entry passed. The layer now logs any hardware fault in the process with module+offset and a short stack (util.cpp install_crash_log, at most eight), symbolized offline with the build's PDBs.

## D-118: Fixed budgets give way to sizes taken from the Mod

- Date: 2026-10-25 (1.8.1, 1.8.2; the user, 2026-10-25: no adjustments per Mod). Three constants refused the new Si Mod in turn: 64 images per package (it declares 83), skeleton components as a partition (it leaves bones between them), 64 bone copies in each of up to nine arenas (its 1299 bones made every batch exceed 64 MiB). Each was a sanity constant standing in for a size the Mod declares. Rule: size from the declaration (images: 1024 as a sanity cap only; arena slices from the bone count within the batch budget, at least eight; components as declared), and where a budget is real, degrade (fewer slices) rather than refuse the package. The remaining fixed budgets are listed in LIMIT-040 to be gone through the same way.

## D-119: The replay models the game's use of its two graphics queues

- Date: 2026-10-25 (1.8.4). The user: the sandbox tests tested nothing (two sessions failed where every offline check had passed). The replay and the sandbox ran one queue with a wait for idle after every frame; the game alternates two graphics queues, submits a frame's command buffers across them with semaphores between, and keeps frames in flight. Both failures (BUG-050, BUG-052) lived there. `replay_capture.py --two-queues` now replays a frame's passes in two command buffers, the first on queue A and the second on queue B (waiting for the first through a semaphore and for the previous frame's second half on the other queue through a chain), the queues swapping every frame, three frames in flight on fences, the presentation on queue B; `--bones-identical` replays the enabled run again with the import on the CPU and, with `--check`, fails unless the pictures match and the GPU run's `bones` status shows jobs, no failure and every submission completed. The suite runs vk03 this way. What the game does that this still does not: its exact semaphore graph and present engine; a session remains the last check.

## D-120: A matched draw waits for its textures; it never shows the game's mesh once the Mod is loaded

- Date: 2026-10-25 (1.8.4). The user: the first frame must be the Mod, a loaded Mod never turns back into the original (the bar of 2026-10-21 restated). The frame gate of D-097 drew the game's mesh for a whole frame whenever a texture of the frame before was not resident or a variant was still building; the 1.7.9 session showed 1709 gated frames for one Mod. Now the gate first waits for those textures (TextureResidency::bind, bounded as the draws' waits are, 4 s) as 3DMigoto loads a Mod's texture at its first bind, and gates only when the wait failed, logged once with the reason. Demotion under memory pressure leaves images bound within the last two seconds alone (reloading an image on screen reduced showed the game's mesh meanwhile). The Core's identification of a new object (its components seen, a frame passed) stays as the original has it.

## D-121: The design follows 3DMigoto's flows

- Date: 2026-10-25. The user: read 3DMigoto's source in full and design on it; performance is the main work. docs/3dmigoto-flows.md records its flows (the draw: a shader override lookup, a count prefilter, a cached region hash, commands executed as D3D calls; resources created and loaded at first use and never unloaded, WDDM paging; one parse and one variable space for all Mods with one Present list; the Core's skinning as dispatches before the draw; diagnostics off in play) against the layer's. Order of work: resources as the original (first-bind load with a wait, no eviction of used textures under pageable memory, no texture gate); the GPU skinning verified and measured in a session; one generation for the whole Mods folder; per-draw work proportional to what changed (the translated draw list, materials and variants kept per section and input state); a play configuration without the probe's diagnostics. Each step measured in the game with the interleaved A/B.

## D-122: Textures as the original holds them (D-121 step 1)

- Date: 2026-10-25 (1.8.5). 3DMigoto loads a resource at its first use and never unloads it; WDDM pages what the frames do not use. With VK_EXT_pageable_device_local_memory (and the layer's low memory priority) the residency now does the same up to its cap: the floor is the whole cap, so below it nothing is evicted for being idle, nothing is demoted and nothing loads reduced; the first bind waits for the load (D-120). Beyond the cap (half the device-local heap by default, at most 4 GiB) the least recently bound image not bound in the last eight frames goes, as before. Without pageable memory the policy of D-097 and D-114 stays.

## D-123: A new object never shows the game's mesh before the Mod's

- Date: 2026-10-26 (1.9.0). The user's bar (2026-10-21, restated in the character screen): the first thing seen is the Mod. The Core's identification (frames after an object's first) and an EFMI Mod's registration with the Core's API (a Present after detection, while the game records frames ahead) kept the game's mesh for a few frames at every new appearance; under DX11 the registration happens once per session and the identification in the frame the object appears. Until one draw of an object was the Mod's, a matched draw that would be the game's mesh is skipped, for at most 30 presents (half a second at 60); past that a Mod that keeps the game's mesh on purpose shows it. Counted as `held_before_shown`, and `original_before_shown` past the hold; the replay's --mod-first check fails on either `kept_original` or `original_before_shown`.
- D-122 amended: the activation gate (D-092) no longer waits for free device memory when the device memory is pageable (BUG-056).

## D-124: A game shader's t declarations come from its SPIR-V

- Date: 2026-10-26 (1.9.0). A newly installed Mod (Tangtang) guards its Core call with `if vs->srv_stride(0) == 16` (the Core's own test for the rolling bone buffer); D-089 refused every declaration query, so every matched draw kept the game's mesh. The original reads these off the DXBC declarations; by the register rule (D-076) the same register's declaration comes from SPIR-V: images typed with their view dimension, texel buffers typed buffers, read-only storage buffers structured with their array stride. Not decided, and refused: a buffer of 32-bit unsigned words (raw or structured), a slot without a SPIR-V register (the DX11 pixel shaders declare t60 and up besides). Checked on the paired catalogue's DXBC: 1555 of 1555 answers equal; 5 registers only the SPIR-V build has read as declared (docs/source-slots.md).
- `srv_mask` and `cb_mask` read 0 as in the original (its evaluation has no case for them); without a shader every query reads -0. `cb_type` and `cb_size` still refuse (FXC declares the highest vector used, not the block).
- A draw's stages are known from its pipeline whatever its render pass (RuntimeDraw::shader_stages, 0 unknown). The dry run takes the game's common character draw: a vertex and a pixel shader, the common vertex registers.
- Amends D-089 ("Declaration queries still refuse").

## D-125: The owned runtime makes no driver allocation in a frame that repeats the last one

- Date: 2026-10-26 (1.9.1, BUG-058). The owned runtime runs on the presentation's thread (with frame generation another thread than the game's) inside the engine lock and the generation's lock, which the game's own draws and submissions need. A driver allocation there can wait for the GPU (a full card, pageable memory) while the GPU waits for the game: a deadlock, and a stall each frame short of one. So what a frame repeats is reused, not made again: descriptor sets per pipeline layout and upload buffers return when the submission that used them completed (the submission's fence wait already gated their owners); only what a frame makes for the first time is allocated. The presenter counts allocations and pools; the owned Present test holds them flat over a panel's frames.
- An application submission takes a generation's lock only when that generation's replaced draws left owned work to submit.

## D-126: The layer keeps its shader translations on disk, as 3DMigoto keeps its compiled CustomShaders

- Date: 2026-10-26 (1.9.2), the user's request. Every generation compiled its CustomShaders at each load (device creation, F10): 0.77 s of a 2.4 s load of 22 Mods. Now each translated stage (DXC from HLSL, vkd3d-shader from a shipped .bin) is kept in `settings/shader-cache` under a SHA-256 key of everything that decides it (tools and this build by path, size and write time; path, stage, flags, Vulkan version and source bytes, or bytecode bytes, stage and float mode); an HLSL entry also holds its includes' bytes and is stale when one changed. Misses compile as before; writing never fails a load; entries of other builds are removed when a process first opens the folder; `shader_cache = 0` turns it off. The owned runtime's pipelines go through the device's variant pipeline cache. docs/shader-cache.md.
- The compiler class itself still keeps nothing on disk (hlsl_compiler.h); the cache is a class beside it, writing only to the project's folder, never next to a Mod.

## D-127: The layer's GPU work rides in the game's batches; one persistent merged skeleton per object

- Date: 2026-10-26 (1.9.3, BUG-060). The user: the designs still have many problems; think through how to solve them properly (docs/design-review-1.9.md). Under DX11 the original issues its work on the context of the draw that needs it, so it runs in the game's order. The layer's bone dispatches went in submissions of their own in front of the application's, which do not wait for what the application's batch waits for; waits of our own deadlocked (BUG-050, BUG-052), so D-117 kept per-frame ring slots, and BUG-054 then re-read the game's bone buffer at later submissions, outside the time the game keeps it valid.
- Now: a command buffer of ours goes at the front of the application's batch that carries the recordings needing it (the host's submit hooks, both vkQueueSubmit and vkQueueSubmit2), the batch's waits on semaphores a queue signalled get our stages added (compute and transfer), and a batch of ours after the application's in the same call signals the merge's timeline for that queue (reuse of our command buffers and snapshots only). No wait or semaphore of ours between queues: D-091 stands. A batch with device masks or protection gets our work in a batch of its own right before it (`prefix_alone`).
- Bones: per object one persistent merged skeleton (slot 0 of its ring, the Core's ResourceMergedSkeletonDataRW), written only by imports; an import belongs to the recording whose draw drew the component, whatever happens to the draw, and runs in that recording's batch. Each recording's draws read their own snapshot, copied in the batch after its imports. A component read but not imported comes from the persistent skeleton; when no submitted batch ever imported it, the recording takes the import of a recording still recorded and not submitted (`borrowed`); otherwise the draw is refused (`bones_unavailable`), never drawn with bones no import wrote. Snapshot slots grow in chunks of eight up to 64 per object.
- Status: `bones.copies`, `borrowed`, `failed_calls`; `replacement.submission_work` (`prefixed_batches`, `widened_waits`, `prefix_alone`). BUG-054's `components_missing` is gone (a snapshot is the whole skeleton).

## D-128: Nothing waits for the GPU inside vkQueuePresentKHR; what a reload drops is destroyed later

- Date: 2026-10-26 (1.9.5, BUG-062). The user: when there is a bug, first look at how 3DMigoto solves it, then solve it on that basis with Vulkan. 3DMigoto's F10 is `ReloadConfig` (IniHandler.cpp), run from Present: it parses the new configuration and clears the old, whose CustomResources only `Release()` their D3D11 objects; the D3D11 runtime destroys an object once the GPU is done with it. Present never waits for the GPU.
- The layer's form: the presenting thread only drops references and takes packages out; a generation stops (and waits for its GPU work) on a thread of its own: an exchanged generation through `retire` as before, a package whose Mod went through `CharacterController::stop_later` (it ran `stop` on the presenting thread, whose fence wait held the Present work that could complete only after the presentation went: the game froze). Only device teardown stops synchronously.
- Enforced: every device function the layer's own code takes comes from `Device::owned_gdpa`, whose GPU waits (vkWaitForFences, vkWaitSemaphores, vkQueueWaitIdle, vkDeviceWaitIdle) count one made on a thread inside vkQueuePresentKHR (`replacement.present_gpu_waits`, logged with the thread's stage). owned_present_test requires 0 through an F10 that removes a Mod with Present work.


## D-129: Each generation's original engine state is its own; a load never holds what the game's threads need

- Date: 2026-10-26 (1.9.8, BUG-063's open part; docs/engine-generations.md). The original keeps one configuration in process globals and reloads it in Present under `G->mCriticalSection`: the game stops for the reload. The layer loaded in the background but kept one process-wide engine lock that every draw, Present and load of every generation took, so a load still stopped the game, for as long as anything inside it took (BUG-063: forever).
- The original's configuration globals (`G`, `customResources`, `explicitCommandListSections`, `command_list_globals`, the key actions and the rest of the former swap set) are redirected by a forced include (`frontend/xxmi/engine_globals.h`) to the generation the calling thread entered: the pinned sources' declarations and definitions become accessor declarations; the generation's `EngineState` owns the objects; `G` and the runtime's scope-local pointers are `__declspec(thread)` (only trivially destructible values: the module is exchanged while the game's threads live on).
- A scope takes only its generation's mutex; a load also the load mutex first, which serializes what only the parser uses and which no drawing, presenting or saving thread takes. The original assembler's tables (filled through operator[]) take an assembler mutex; the resource root and the warnings are kept per generation.
- Enforced: `scripts/check_engine_globals.py` (suite step "INI runtime globals classified") lists the runtime objects' writable data symbols and fails on one that no rule classifies. Tests: runtime_concurrency_cases (a load held in its scope, a draw held in its scope, two generations driven from two threads while a third loads and retires).

## D-130: Texture loads read the file straight into a staging arena the loaders keep

- Date: 2026-10-27 (1.9.9, cause C of docs/design-review-1.9.md). The original substantiates a texture at its first use with DirectXTK's CreateDDSTextureFromFileEx: one read of the file into an uninitialized buffer, CreateTexture2D with that as initial data, the driver's copy. Measured on this machine on the installed Mods' ten largest maps (scripts/bench_texture_load.py, file cache warm): 28 ms per 64 MiB map, 297 ms for the ten one after another. The layer's loader took 43 ms for one map: the file into a zero-filled vector, a new host-visible staging allocation per load (pages the system zeroes), a second copy into it, a command pool and fence made and freed per load, fence polling in 1 ms sleeps.
- With the layer's upload queue, an image's load reads the DDS header, takes a slice of a staging arena (one host-visible buffer, 256 MiB, made at a busy period's first load and released after five seconds without one) and reads the payload straight into it; the worker records the copy into its own command buffer, submits with its own fence and waits on it. Buffers and devices without the upload queue keep the former path.
- Measured: one map 20 ms, the ten together 99 ms; in the vk03 replay the draws' texture waits at startup 120 to 46 ms (median of three interleaved pairs), the longest startup frame 207 to 127 ms, pictures identical.
- Enforced: the suite step "texture first-use loads against the original" (bench_texture_load.py --check) fails when one map alone or all of them take longer than the original's loads; the asset tests read every level of arena loads back (including one without its top level) and check one arena per busy period, one command pool per worker and the arena's release.

## D-185: EFMITTL's translucent parts cast a shadow by their opacity (BUG-094)

- Date: 2026-11-12. The user in the game (2.1.7): Zhuangfangyi's see-through shirt and sarong look as under DX11 but the shadows on the skin under them are strange. The library (translucency.ini 0.5.6, CommandListDraw): a translucent call without o0 runs CommandListShadow; in a pass its shadow vertex groups recognise ($vs_kind 2 or 3: the plain and the fading shadow vertex shaders of skinned characters) it draws the call with shadow_a.hlsl or shadow_b.hlsl, which keep a shadow-map texel with a probability equal to the opacity there (the mask at the part's UV, the interleaved gradient noise of opacity.hlsli; shadow_b also keeps the game's own fade test); anywhere else it draws the call as it is. On Vulkan the shadow groups were never recognised (`$vs_kind` 0), so every translucent part cast its full shadow.
- Decision:
  - Hook facts (`layer/src/shader_hooks.cpp`, `shadow_vertex`): a vertex module with the skinned character's inputs (float3, float2, float3, float4, float4, int4 at locations 0, 1, 2, 4, 5, 6), the instance index, a storage buffer and uniform blocks is the plain shadow vertex shader when it outputs the position alone, the fading one when it adds the UV (float2 at location 0). On the paired catalogue: d29292e6 pairs with f11c7e1d (the plain group), c8fa3abe with bbc56f5c (the fading group); 1425b014, the pair of d657fd46 (two UV sets, no group), has other inputs and outputs. `VertexHook` 4 and 5 run the library's two groups.
  - The bridge (`frontend/xxmi/efmittl_bridge.cpp`): CommandListShadow is a native boundary. With `$vs_kind` 2 or 3 the call becomes a translucent plan item marked `shadow`; otherwise `drawindexed = $count, $first, 0` as the list does (`DrawPlanAdapter::draw_indexed`).
  - The renderer (`character_backend_record.cpp`, `translucency.cpp`): such a draw is drawn in place in the shadow pass with a variant of the game's pipeline whose fragment stage is the layer's own main over the library's opacity.hlsli (its mask read and noise; the opacity, channel and inversion as specialization constants, since no parameter image can be written inside the pass), compiled once with and once without a mask; the mask is the draw's one private image. The plain shadow vertex shader has no UV output, so for a masked call it is given one (`add_uv_output`): the UV input times the material block's transform (set 1 binding 0, the vec4 at byte 160, which the fading shader's UV output and DX11's `mad o0.xy, v1.xyxx, cb3[10].xyxx, cb3[10].zwzz` use). Where the library's shaders are not there, the call is drawn as before (the library's own rule: the full shadow). The status counts `translucent_shadows`.
- Checks failing first: `scripts/check_shader_hooks.py` with the library's shadow groups (d29292e6 and c8fa3abe given group 0, their DX11 partners match the plain and fading groups); the replay of vk11 (ardelia) with `--expect-shadows` (`translucent_shadows` 0). Now: both pass; vk11 226 and vk08 (Zhuangfangyi, masked calls on the plain shader) 232 translucent shadows, validation as before (0 errors, the 4 known performance notes). vk08's shadow map against 2.1.7's: 95589 covered texels to 93612, 14644 texels changed.
- Left: shadow_b's own fade test of the game (the fading shader's per-instance fade near the camera) is not kept: the layer's fragment stage takes the opacity alone. Not seen in the game yet.

## D-184: The state check runs the texture-hash sections (review V-05, P3-4)

- Date: 2026-11-12. The original runs a TextureOverride matched by a texture's hash where a list asks for the override of a slot (`CheckTextureOverride = ps-tN`; the EFMI Core's CommandListOverrideTextures asks t0 to t22, and Mods ask single slots) and the resource there has the section's hash; its list usually puts the Mod's texture in `this`.
- Before: the state check (`scripts/check_mod_states.py`, `plan_folder_states` in sandbox/character_test.cpp) answered every pixel slot as bound without a hash, so those sections never ran offline; what their lists do was checked only in the live scene and the game.
- Decision: `IniRuntime::texture_hashes()` lists the hashes of the sections without an index match whose lists put a resource in `this`. After every other state (so that nothing their lists change enters a digest) every section of every Mod is planned once more per such hash, with that hash at every pixel slot; a refusal is reported as the others are. The same section is planned again without the hash, and the plans whose bindings differ are counted: evidence that the sections ran. The digests are unchanged.
- On the installed folder: 5 such hashes, 1595 plans, 900 of them binding otherwise than without the hash, no refusal.
- Left: a hash at every slot at once is not a game's draw (one texture at one slot); a section that checks two textures together is not combined.

## D-183: The layer says once when the game's character draws are not what it knows (review L-07, P0-9's second part)

- Date: 2026-11-12. The DX11 Core finds the facts of the game it runs in by probing (`if vs-cb2->Size / 16 == 4096 ... elif vs-cb1`) and replaces nothing where none fits; 3DMigoto itself says nothing then. The layer reads them from fixed places: the instance entry at set 2 binding 0 in mapped memory, a pipeline the tracker knows, a primary command buffer in a classic render pass, the game's vertex and index bindings tracked, a vertex input and a vertex shader its variants can take.
- Before: a game update that moved one of them made every Mod keep the game's look, with a refusal per draw and Mod and no statement that the game itself changed.
- Decision (`layer/src/character_backend_record.cpp`, `character_backend_internal.h`, `character_backend_status.cpp`): the refusals for those facts go through `refuse_shape`, which counts them for the process (`GameShape`); a character draw that is replaced or drawn beside counts as fitting. When 240 such refusals came and no character draw of any Mod fitted, one notice (key `game-shape`): the game's character draws are not what the layer knows (the first such reason), the game version is probably newer than the layer, every Mod keeps the game's look. The replacement status has `game_shape` (fitting, unfit, told, first_unfit). Nothing else changes: the draws stay the game's as before.
- Check failing first: the live scene `unknown_game` (`EFMI_VK_TEST_UNKNOWN_GAME`: the application unmaps the instance entries' memory before its character draws): 2 of 3 on the build before (every frame the game's, nothing said), 3 of 3 now.

## D-182: A component computed at the draw is the one the lists handed the Core (review P2-3, last part)

- Date: 2026-11-12. The original Core reads `$\EFMIv1\component_id` and `$\EFMIv1\lod_level` when the Mod's list calls it (Component_ReadConfig, DrawInstances): whatever the lists computed by then is the draw's component. Nothing is read ahead of the draw.
- Before: a section's component was the one constant its lists hand the Core (D-177). A section whose lists compute it (`$\EFMIv1\component_id = $part + 1`, or two branches with different constants) was a section without a component; at its draws the Core's call failed the contract ("Original Core request differs ...") and the part stayed the game's.
- Decision (`frontend/xxmi/character_core.cpp`, `layer/src/character_backend_record.cpp`): such a section is marked `computed` (`CharacterSection::computed`, not in the file). The host must know a draw's component before the lists run (its bones are imported first), so a draw of the section is made for the component the lists handed the Core at its last draw (`CharacterGeneration::handed`). At the Core's call the contract is checked as before; when the draw cannot go as it was made, the lists' component and LOD are handed over (`RuntimeDraw::handed_component`), the next draw is made for them, and this one is held, as the Core holds an object's first frame. The first draw of such a section goes the way of a section without a component, whose lists run and hand the component over. The lists run once per draw, as the original's. Its instanced draws are decomposed as D-180's. The status counts `components_handed`.
- Check failing first: the live scene `computed_component` (the fixture with every section's component computed, `$part_base + N`): 1 of 3 on the build before (the character the game's), 3 of 3 now (the whole character scene as with constants).
- Left: a change of the handed component costs one held draw of that part, where the original has none. No installed Mod computes a component (their plans are as accepted).

## D-181: The variant cache lets go of the finished variant least recently asked for (review L-08, part of P4-3)

- Date: 2026-11-12. The original has no bound of this kind: D3D11 creates the shader and state objects a draw needs, and a resource the context still uses lives until the GPU is done with it (reference counts, deferred destruction).
- Before: the variant cache had no eviction. Entries left only when the game destroyed the pipeline; at the capacity (4096) every new variant was refused ("variant cache capacity reached") and the draws that needed one stayed the game's for the rest of the session.
- Decision (`layer/src/variant_cache.*`): each job carries the cache's clock of the last request that asked for it. When a new variant does not fit, the finished jobs (ready or failed, never queued or building) least recently asked for go, an eighth of the capacity at a time (`evict`); they are released after the lock. A variant a command buffer or a draw lookup still holds lives on through its shared pointer, as D3D11's reference counts keep an object; one asked for again is built again. The status counts `evictions`.
- Check failing first: `sandbox/cache_test.cpp` (pipeline recipes and variants): a cache of two with two finished variants; a third was refused before (40 of 41), and is built now with the one asked for again kept and the held one alive (41 of 41).
- Left of P4-3: `TextureResidency::images_` never shrinks; one deferred-destruction queue for what the layer owns; the first-use waits under the tracker's lock (L-04); the failed image bound again (L-06).

## D-180: An instanced draw of a section with a component is decomposed as the Core's IterateInstances (review R-05, P2-4)

- Date: 2026-11-12. The original Core: a component's draw goes through `CommandListComponent_DrawInstances` and `CommandListIterateInstances` (Core API.ini and Helpers.ini). Per instance up to `$instance_count` it reads that instance's config at `$draw_call_instance_id * $cfg_instance_config_byte_size` (256 bytes), identifies the object, and draws the Mod's mesh for an instance that is one of the Mod's objects, with the instance's config moved into place; any other instance is drawn alone as the game's (`drawindexedinstanced = INDEX_COUNT, 1, FIRST_INDEX, FIRST_VERTEX, FIRST_INSTANCE + id`).
- Before: the host refused every instanced character draw ("character draw is instanced"), so the whole draw stayed the game's. No character draw in the captures is instanced (16 frames), and every instanced draw there has first instance 0.
- Decision (`layer/src/character_backend_record.cpp`): `generation_record` sends an instanced draw of a section with a component to `record_instances`, which runs `record()` once per instance with that instance's index; `record()` reads the instance's entry that many entries after the first. An instance `record()` does not take is drawn alone with the game's state, which `record()` puts back after its own draws (`vkCmdDrawIndexed(count, 1, first, vertex_offset, k)`). A section without a component runs its lists once for the whole draw, as the original's do. Limits, where the application's instanced draw stays: a first instance other than 0 (the place of the entries is open), more than 64 instances. The status counts `instanced_draws`.
- Check failing first: the live scene's `instanced_draw` (`sandbox/runtime_test.py`, `character_gpu_case.h` mode 3): one draw of two instances whose second entry is the Mod's character at another place; before, the instance was not drawn as the Mod's (1 of 3), now 3 of 3.
- Not verified: no instanced character draw has been seen in the game; the case is the Core's design, not a captured draw.

## D-179: A list that draws without `handling = skip` draws beside the game's draw (review R-09, part of P1-5)

- Date: 2026-11-12. The original (HackerContext.cpp, `DrawIndexed`): `BeforeDraw` runs the pre lists, whose draw commands go to the context at once; `if (!c.call_info.skip)` the game's draw follows; then `AfterDraw` and the post lists. A matched section without `handling = skip` that draws therefore draws the Mod's mesh and then the game's.
- Before: such a plan was read as a texture-only draw (D-083); its draw items were dropped, so the Mod's mesh was never drawn, and a section without a component logged "INI hide override unexpectedly emits geometry".
- Decision (`layer/src/character_program.cpp` plan, `character_backend_record.cpp`): a plan with draw items and no skip is handed to the caller as a hiding plan is. With a component the draw is recorded as a replaced one and then given back to the game (`record()` returns false; it puts the game's pipeline, buffers and set 3 back after its draws, so the game's draw follows with its own state); without one, `record_own_mesh` (D-176) draws it and the game's draw follows. While the package comes up the game's draw stays alone. The status counts `drawn_beside` apart from `replaced`.
- Check failing first: the live scene's `own_mesh` with the section's skip removed (`own_mesh_case(beside=True)`): the Mod's triangle and the game's mesh both on screen; before, only the game's. A section with a component that draws without skip goes the replaced draw's way in `record()` and has no scene of its own.
- Left: the pixel bindings the pre phase makes for the game's own draw after the Mod's draws (the original's context keeps them for the game's draw); the game's draw goes with its own textures. No installed Mod has a drawing section without skip.

## D-178: Every package catalogues every image of the folder generation (BUG-091)

- Date: 2026-11-12. The user: finish everything that needs no game.
- The original keeps one registry of resources: any section a draw matches may bind any declared texture, wherever its file lies (a section of another Mod's INI for the same draw, a shared texture in another folder).
- The layer before catalogued a package's images by its folders (`CharacterProgram::owns`): a plan that bound another folder's texture was refused, "INI image not in prepared resource catalogue" (BUG-091's notice, with two Mods of one character in the folder).
- Decision: a package's image catalogue holds every image the folder generation declares; its mesh files stay its own (a package's mesh is one upload). The residency holds an image once by its file and loads it at its first bind, so an entry costs a declaration; whether a file loads is read once per file for the process (`image_loads`). The runtime catalogue's budget is 65536 (`kCharacterRuntimeImages`). The offline planner reports a Mod's own images (`CharacterProgram::owned`) for the comparison of a Mod alone and in the folder, and the whole catalogue as `catalogue_images`.
- Left: a mesh file of another package's folder bound in a draw is still refused (one upload per package).

## D-177: A character is what its lists hand to the Core, not what the exporter names it (review R-04, P2-3)

- Date: 2026-11-12. The user, on the open items of Phase 2: the point is that the skeleton contract was still read by the exporter's names at load, so a Mod written otherwise is left out.
- The original Core knows a Mod only through its own interface: the lists a Mod runs (`CommandList\EFMIv1\InitializeMergedSkeleton`, `...\Component_DrawInstances` and the others), the Core's variables a Mod assigns before it runs them (`$\EFMIv1\component_id`, `lod_level`, `bones_count`, `instance_count`), the pools a Mod references into the Core's inputs (`Pool\EFMIv1\Input_MergedSkeleton_Component_VertexGroupOffsets = ref ...`, `..._VertexGroupCounts`, `..._LodRemaps`) and the resource it references into `Resource\EFMIv1\OutputMergedSkeleton_Template`. What the Mod calls its own lists, variables and pools, and in which of its lists it assigns them, is nothing to the Core.
- The layer before read the contract by the names the exporter gives: an INI was a character Mod by a section `[CommandListInitializeMergedSkeleton]`; the skeleton by `$<mod>\bones_count`, `$<mod>\max_instance_count` and the pools `Pool<mod>_MergedSkeleton_Component_...`; a section's component by a constant assigned to `$\EFMIv1\component_id` in the section's own list, its LOD by the Mod's `$lod_level`.
- Decision (`frontend/xxmi/character_core.cpp`, `layer/src/character_program.cpp`):
  - A character Mod is an INI whose lists run the Core's InitializeMergedSkeleton (`calls_skeleton_initializer`). The lists that declare its skeleton are those of its lists with that call (`Mod::initializers`, read off the sections' lines with `HostSectionRuns` before anything is compiled); they are run at preparation as before.
  - The bone and instance counts are the numbers its lists assign to the Core's `bones_count` and `instance_count` (`core_number`, with its variables as its [Constants] leave them); its pools are those its lists reference into the Core's three input pools (`core_pool_source`). The exporter's names stay a fallback where a list hands nothing over, and what is then missing is said with both.
  - A section's component and LOD are the constants its lists hand to `$\EFMIv1\component_id` and `$\EFMIv1\lod_level`, directly or through a variable of the Mod's they assign there (`feeder_of`, `handed_through`, `constant_to`), in the section's own list or a list of the Mod's it runs (`each_command` follows runs, not into the Core's lists). Where the lists never hand the Core a LOD, the Mod's `$lod_level` as before.
  - The sections are read twice: when the overrides are parsed, and again at preparation, when every list is compiled and a run can be followed. The adapter wraps operands it answers by then; `DrawPlanAdapter::original(const CommandListEvaluatable*)` gives the original operand, as `original(command)` does for commands (a first try without it lost the stride branches of the user's avywenna 1.7: the comparison of plans caught it).
- Left: the identification threshold and the element offset edits are still read from constants in the Mod's lists by name of the Core's variable (they were never bound to the exporter's names); the static reading itself stays (P2-3's last part: a section first read at its first draw where the static reading finds nothing).

## D-176: A section without a component draws a mesh of its own under the game's vertex stage (review Phase 2, P2-2)

- Date: 2026-11-12.
- The original: a TextureOverride's lists run on the game's draw state. `handling = skip` drops the game's draw; `vbN = Resource`, `ib = Resource` and the texture assignments bind the Mod's resources on the context, and `drawindexed` is a DrawIndexed there: with the game's shaders, its constant buffers and whatever else the game bound, the bone buffer its vertex shader reads among it. No Core list is called; the bones are the game's own of that draw, so the Mod's blend stream names the bones of the game's part. D3D11 takes the Mod's buffers under the game's input layout by their strides.
- The layer before: only a section with a component drew a mesh, through the merged skeleton (identity, import, the vertex stage re-pointed to the private set). A section without one could hide the draw or give it textures (D-083); a plan with a draw was refused, "INI hide override unexpectedly emits geometry", and the game's draw stayed.
- Decision: `CharacterGeneration::record_own_mesh`. The game's pipeline with its vertex stage kept (a variant with `keep_vertex` and `own_geometry`: one for its vertex input alone is a variant now), the vertex input the Mod's streams need (`CharacterPackage::vertex_input`: their strides, the list's blend-index and offset edits), the Mod's images through the private set as in a texture-only draw. The game's sets stay bound, so the vertices read the game's bones of this draw. `CharacterProgram::plan` checks no blend stream of a draw without a component against a merged skeleton and requires no bones of it (`own_bones`); such plans are kept apart from a component's by their signature.
- Counter `own_mesh`. It serves an INI beside a character Mod and one in a package without a skeleton alike (D-173, D-174).
- Limits: the vertex slots the pipeline reads among the four tracked; a translucent draw of the EFMITTL library in such a list is drawn in the pass, not captured; an owned GPU buffer bound as a vertex stream is taken as in a component's draw.

## D-175: Components may share bones, and either LOD may have a remap (BUG-093; review R-04, a part of P2-3)

- Date: 2026-11-12. The user's Yvonne Mod, changed that night by a tool that puts the LOD 1 parts on the LOD 0 part's bones, was left out whole: "Skeleton components overlap".
- The original (the Core's MergedSkeleton.ini): `CommandListMergedSkeleton_AttachComponent` reads the component's offset and count from the Mod's pools; the importer (`CustomShader_MergedSkeleton_BonesImporter_RollingBuffer` and `_ConstantBuffer`) binds `PoolInput_MergedSkeleton_Component_LodRemaps[$component_id * levels + $lod_level]`, uses it when it is not null (`z0 = @cs-t1 != 0`) and writes `count` bones at the skeleton's offset plus the component's. No component is compared with another, and a remap is one entry per component and LOD. Two components on the same bones are thus filled by whichever is drawn, which is what the tool relies on: the game draws an object's LOD 0 part or its LOD 1 part, each through its own remap onto the same merged bones.
- The layer before: components had to lie apart, in order, at five places (the skeleton plan read from the pools, the palette, the contract, the package format, the CPU skeleton); a LOD 0 remap refused the Mod; and the bones a draw needs were counted in components.
- Decision:
  - Components are ranges in the order of their first bones and may share bones. A package has `spans`: the bones some component imports, cut at every component's end, each with the components it lies in (`CharacterPackage::make_spans`). With components apart a span is a component, and every mask is as it was. A stream's bones (`CharacterStream::spans`) and a list's needs (`CharacterList::required`) are masks of spans; at most 64.
  - A draw's bones are there when every span it needs was imported by some component the span lies in: on the CPU by that component's coverage, on the device by a job of this recording, by one a submitted batch carried, or by one taken from a recording not yet submitted.
  - A component has a remap per LOD (`remap_lod0` beside `remap_lod1`, `CharacterComponent::remap(lod)`). The package format's sixth component word, zero until now, is the LOD 0 remap's stream plus one. The device's remap tables are two per component, and a bone job names its table (`BoneJob::table`).
  - `discover_contract` refuses neither; `prepare_mod` matches the contract's components by first bone and count, two of the same in the pools' order. `lod_agrees` takes LOD 0 and 1 as the same only for a component without a remap at either.
- Not changed: LOD 2 and 3 stay the game's; the rest of R-04 (the skeleton and the sections read by the exporter's names at load). A remap pool element no line assigned was no remap before as well, as the original takes it (`load_palette_files`).

## D-174: A package without a skeleton, and a Mods folder without a character Mod (review Phase 2, P2-1, second part)

- Date: 2026-11-12 (the user, 2026-11-11: loading should not tell characters from other Mods at all).
- The original has no notion of a character Mod: every enabled INI is parsed into one registry (ParseIniFilesRecursive), and a draw is looked up in it (D-173). A character Mod is an INI whose lists call the Core's merged-skeleton lists.
- The layer before: a package, the unit that carries a Mod's sections, mesh files and texture catalogue on the device, was made per INI with `[CommandListInitializeMergedSkeleton]`. An INI outside every such INI's folder had its mesh sections matched at no draw (a notice said so since P0-1), and a Mods folder without a character Mod loaded nothing: no Present, no key, no panel.
- Decision:
  - The frontend reports the registry's mesh sections that no character Mod carries, each with the folder of its INI (`BesideSection`, `character_folder(mods, rest)`); `mods` may be empty.
  - `CharacterProgram::discover_folder` makes a package per such folder (`FolderMod::plain`): no bones, no components, sections without a component, the mesh files and images under the folder that are no other package's (`foreign_`), the libraries its INIs call. The package format holds no such package, so it is validated on its own (`validate_refreshed`: its streams and sections as `decode` checks them).
  - The folder's generation is a program of its own (`FolderSource::generation`, `FolderGeneration::generation`), no longer one package's view of it: a folder whose INIs have no mesh section presents, saves and reloads. A folder is empty only without any enabled INI.
  - The notice "its mesh sections replace nothing", the status's `unmatched` and `CharacterProgram::unmatched_mesh_inis` are gone: there is no such INI any more. A package's status has `components` (0: without a skeleton).
  - A character INI disabled by its file's name leaves its folder's other INIs enabled, as under the original; their mesh sections then have a package of their own until the character INI is back.
- What stays a character's: the skeleton service (identity, bone import, the merged skeleton's private set) goes to sections with a component, and which INIs have a skeleton is still read at load from the exporter's shape (R-04, P2-3). A section without a component that draws a mesh of its own is still refused (P2-2).

## D-173: The host's sections come from the registry, whichever INI declares them (review Phase 2, P2-1, first part)

- Date: 2026-11-11 (the user: the review's Phase 2 is the point, not the small bugs; and: why does only a character's INI match, should not every INI?). Found in the installed Mods: `last rite Ethereal Prayer 2.3\Texture\hair.ini` gives the hair's two draws (hashes ce03a49a and 2ba325f7, no section of `last rite.ini`) the Mod's diffuse texture; under the layer neither section ever ran. 80 further sections of 21 panel INIs (`k\1.ini`, `$active = 1`) share their hash with a character section and ran only with that section's draw.
- The original (XXMI with `track_region_hashes = 1`, the EFMI configuration): the Core's `[ShaderRegexEnableTextureOverrides]` runs `CheckTextureOverride = ib` at every indexed draw of a vertex shader of model 4 or 5 while `$enable_mods`. ResourceCopyTarget::FindTextureOverrides (CommandList.cpp:10304) takes the candidates from `G->mTextureOverrideDrawIndexMap[IndexCount]`, which BuildTextureOverrideDrawMaps (IniHandler.cpp:4427) fills with every TextureOverride of the registry whose `match_index_count` is `= N`; it hashes the draw's index region only when there are candidates, keeps those of that hash whose other draw keys match (matches_draw_info), and CheckTextureOverrideCommand::run runs each one's list. Which INI declares a section plays no part; a section without `match_index_count` is no candidate for an index buffer under region hashes. D3D11 then takes whatever the lists bind.
- The layer before: the device's index held the sections of character packages, and a package's sections were those of the character INI's namespace (CharacterCore::discover_sections). Another INI's section ran only where its hash was also a character section's (the frontend's matcher, the original's own, runs every section of a hash once the host hands a draw over).
- Decision, first part: a TextureOverride with a constant `match_index_count` (and a constant or no `match_first_index`) that no character Mod's namespace owns joins the package of the Mod whose folder holds its INI (the folder of the INI that declares the Mod's initializer; the parser keeps each section's INI path, HostSectionFolder over get_namespaced_section_path), as a section without a component. A package's files were already those under its folder (CharacterProgram::owns), so the section's textures are in its catalogue. One with the draw keys of a section the Mod already has is not added: the lists of every section a draw matches run with that draw.
- A draw's sections are tried in two rounds (character_record): those with a component first, and only a draw none of them matches goes to the sections without one. The lists then run once: a package whose plan ran and left the draw to the game ends the search (t_draw_plans). Before, a draw a component section left to the game went on to a section without a component of the same hash, whose plan ran the character's lists again without an identity.
- A section without a component runs its lists whatever the package's state (the original's run at every draw the section matches): only a plan that makes the draw the Mod's (hidden, or drawn with its textures) needs the package on the device and brings it up. Before, the first such draw activated the package and was skipped, so a section that only sets a variable would have hidden the game's draw for the frames of the activation.
- Left for the next parts: INIs outside every character Mod's folder (a package without a skeleton, and a Mods folder without any character Mod), geometry in a section without a component (P2-2), a texture of another package's folder bound in a draw (the catalogue is per package).

## D-172: The libraries a Mod calls in the folder generation, whatever their hash sections and the runtime

- Date: 2026-11-11 (BUG-088's fourth cause). The EFMITTL of 2026-11-10 has a TextureOverride section (`[TextureOverrideEFMITTLNormalSlot_36d7856a]`, a filter index on the game's default normal map). CharacterProgram::libraries leaves out a library with such a section, a rule made for a generation of one Mod: there a library loaded into every calling Mod's generation would act once per generation, where the original acts once. The folder generation applied the same rule when it recorded which libraries each Mod calls, so no Mod called EFMITTL any more; the renderer compiles the library's shaders from the called library's folder (Translucency::shaders, `library_folder("efmittl")`), got an empty folder, and every capture failed ("HLSL source and working directory must be absolute").
- The original has one namespace space: every enabled INI is parsed once and a library's sections act once, which the folder generation reproduces since D-155 (the library is one of its inputs). In that generation the rule has nothing to prevent.
- Decision: in the folder generation (CharacterProgram::libraries with `one_generation`) a library's hash sections do not keep a Mod from calling it. The lookup also no longer depends on the owned GPU runtime (it was gated on it since D-131 step 1, when the folder's inputs were still per Mod): which libraries a Mod calls is a fact of its text. A generation of one Mod keeps the rule.
- Checked: scripts/check_folder_generation.py requires that every library a Mod calls when alone is recorded for it in the folder generation (the planner prints `libraries`); failing for the 15 installed Mods that call EFMITTL before, passing after.

## D-171: A library's shader groups run as the library wrote them, chosen by facts the host reads off SPIR-V (amends D-113)

- Date: 2026-11-11 (BUG-088). D-113 gave EFMITTL's one lit vertex group its meaning natively: `$vs_kind = 1` before a draw whose vertex shader has the lit interface, 0 after. The library of 2026-11-10 has more groups and more work in them: a lit group for skin, an outline group (`$vs_kind = 4`), and every vertex group clears its per-draw marks (`$slot_pending`, `$slot_hook`, `$slot_clip`); pixel groups, ClipFirst and one DiffuseAt per slot t8 to t23, write the diffuse slot the shader samples for the key the Mod's first SetTextures call left (`$PoolRegexDiffuse[$slot_pkey] = N + 32 * $slot_clip`). With only `$vs_kind` given, the library saw skin parts as no lit pass and fell back to drawing them opaque.
- The original (ShaderRegex.cpp, IniHandler.cpp's parse_shader_regex_section_main, HackerContext's shader hooks): a group whose pattern matches a shader's listing gets the group's command lists, which run around every draw with that shader: the vertex stage's pre lists ahead of the pixel stage's, groups of a shader in the order of their section names; post lists after the draw. The Core's own check of `ib` is a later group of the vertex stage, so the library's vertex lists run ahead of a Mod's TextureOverride lists and its pixel lists after them, both before the draw.
- Decision: the library's groups keep their lists. The bridge (efmittl_bridge.cpp) compiles each known group's main section with the original's parser (HostCompileShaderRegexList, ShaderRegexIniKeys), and per draw the runtime runs them in the original's places: the vertex group's lists ahead of the matched lists and after the draw, the pixel groups' after the matched lists (DrawHooks). Lists that only assign constants (all of the vertex groups' today) are applied directly, as they cost nothing to run that way.
- What Vulkan does not give: the DX11 listing the patterns match. Which group a draw's shaders stand for is read off the pipeline's SPIR-V by rules checked against the groups' own patterns on the paired catalogue's DX11 listings (scripts/check_shader_hooks.py, run against the installed library):
  - lit (D-113's rule) and lit skin: the outputs of the DX11 signature at locations 0 to 8 and 0 to 9;
  - outline: the ramp the library finds as `4.583663` in the listing, which FXC folded from 114.59157 and 0.04; the Vulkan modules keep the first, in every outline vertex shader of the catalogue and no other (12 of 12);
  - DiffuseAt and ClipFirst: the first sample of a 2D texture below t24 at an interpolated input, its channels as the module reads them, in the module's order except that a sample goes after every discard before its first use that does not depend on it, as FXC lists it (the hair's alpha clip). 64 vertex shaders and every fragment pair agree; two fragment pairs differ, both in draws whose vertex shader is of no group, where the pixel groups' conditions do nothing (recorded with why).
- The shadow groups stay unmet as in D-113, and the copy group keeps its native hook (LibraryHook::CopyShader).
- Verified in the sandbox: the runtime test's library cases (12 checks: the lists' order, a group per vertex kind, ClipFirst before DiffuseAt, a slot without a group); the hook check on the catalogue.

## D-170: A texture's properties in a replaced draw's plan: the original's getters over the host's description and the Mod's file

- Date: 2026-11-11 (BUG-088's third cause). The EFMITTL installed on 2026-11-10 reads `ps-t[N]->Format`, `->Width`, `->Height` and `->Mips` of the game's texture at every declared 2D slot below t24 to tell the diffuse, light and normal maps apart; twenty of the installed Mods call that list. The plan path answered no property of any resource, and one unanswered query refuses the draw.
- The original (CommandList.cpp: process_texture_filter, ResourceCopyTarget::GetResourceFormat, GetResourceWidth, GetResourceHeight, GetResourceArray, GetResourceMips, GetResource): the target is resolved to the resource bound now (PSGetShaderResources for a pixel slot, the declaration's own for a custom resource). No resource reads -2 (an empty slot, a slot index out of range, a declaration that holds nothing or was set to null). The format is the bound view's, and the texture's own where the view has none; without either, -1. Width, height and mip levels come from the resource's description, not the view's (a buffer reads -4, a 1D texture's height 1); the array size likewise, and -4 for a 3D texture. A declaration answers its own `format`, `width`, `height`, `array` and `mips` keys before any resource is looked at.
- What D3D11 does for the original and the layer does itself: the bound state. The plan keeps what the lists left at each pixel slot (`pixel_held`: the application's texture of that or another slot, a resource of the generation, nothing) and what a declaration holds; the query resolves its target through that, then asks the texture provider for the description (DrawPlanAdapter::TextureIdentity::properties). The semantics are one header, frontend/xxmi/texture_properties.h; draw_plan.cpp only resolves the target and calls it.
- The game's texture: the host fills RuntimeTexture (dimension, view format, width, height, levels, layers) from the Vulkan image and view the draw binds at the register's binding (D-076). The view format maps to the DXGI format of the DX11 view of the same resource (texture_identity.cpp, dxgi_view_format): a colour view has the texture's own format on both sides; a depth texture is typeless under DX11 and its view names the depth bits (D16_UNORM is R16_UNORM, D32_SFLOAT_S8_UINT is R32_FLOAT_X8X24_TYPELESS). Other depth formats are not mapped: the catalogue has none at a pixel slot, and an unmapped format refuses the query rather than guess.
- Ground truth, the paired catalogue (scripts/check_texture_properties.py, a suite step): over the 953 paired draws, 2239 slots hold the same texture on both sides (equal content) and all five answers are equal on every one; of the 3595 other slots of the same draws (render targets, placeholders, two sessions) the format is equal on 3581, with the size, levels and layers equal as well; the 14 others are two effect shaders whose paired draws are not the same pass (listed in the script). 921 slots hold buffers on both sides.
- A Mod's own texture: the description DirectXTK's loader gives the file (frontend/xxmi/texture_properties.cpp reads the header with the pinned loader's structures; another container through the WIC decoder), with the declaration's colour space. The original as the judge (sandbox/judge_draw_cases/properties.ini, 34 values) showed what no reading of the source had: a file declared with another `format` still reads its own format at a slot (28 for an R8G8B8A8_UNORM file declared sRGB) and through a declaration that holds it by reference; only the declaration itself answers its key (29).
- Not known, and refused with the reason: a buffer view's format where the SPIR-V does not tell it (a buffer of words, a typed buffer), an image the layer does not track, a resource of the Mod that is not a file (owned work answers those in its own scope), other stages' slots. A slot the pixel shader has no register for reads as empty; under DX11 a texture left bound by an earlier draw would be read there, which no list can rely on.
- The offline planner's stand-in does not describe the game's textures; a list that asks is refused there with the reason, as the stand-in's other limits.
- With D-163 (Phase 1, merged first) an operand nothing answers reads -1 and the list goes on instead of refusing the draw. That does not make these answers optional: with -1 for every format and size the library's scan finds no slot and binds the Mods' textures nowhere. Since the rebase the query resolves its target through the draw state (D-161: `named()`, `context`, `held()`), ahead of `from_state`, whose `of_declaration` would answer a Mod's file at a slot with its declaration's format.
- Verified in the sandbox only: the judge's case and six one-line sweep cases as the original's, the catalogue check, live INI rendering (a list reads 28, 64, 64, 1, 1 of the scene's texture through the layer and -2 of a slot without a register), and the replay with a library call, whose 220 refused plans are gone.
## D-163: An operand nothing answers reads a value from one table, with a notice (docs/architecture-review-2.1.md P1-4)

- Date: 2026-11-11. Before: an operand the host could not answer for a replaced draw (`rt_width` without a known target, `vs->cb_size(n)`, a property of the game's buffer, a texture the host cannot identify) refused the command, and with it every list of the draw; in a Present list it left the assignment undone (D-155).
- The original (CommandList.cpp, CommandListOperand::evaluate and what it calls): every operand has a value. A slot with nothing bound reads -0 as a filter value and 0 as an identity; a property the original cannot read is `ResourcePropertyResult::UNKNOWN` (-1), of nothing bound `RESOURCE_NOT_FOUND` (-2); a register a shader does not declare reads type 0 and -1 for its size, stride and dimension; `rt_width` stays -1 while no 2D target is bound at o0. D3D11's context answers all of these for it.
- Decision: `operand_table()` in `frontend/xxmi/draw_state.h`, one row per kind of operand with the value the expression goes on with and what the value means under the original. Every query evaluatable of the adapter (`NativeResourceQuery`, `NativeShaderQuery`, `RenderTargetQuery`, `HostValueQuery`, the new `UnansweredOperand` for the scissor rectangle) returns its row's value where nothing answers, and `QueryService::gap` says so once per operand through LayerGap, with the line. No operand refuses a list. The guards of a slot number and of a range bound return a value the original's own check rejects (below every slot; not a number), so the original's warning and no-op follow where its cast would be undefined.
- A question about a slot is first one about what the lists bound there (D-161): nothing bound, a resource of the Mod (its declaration's stride, format, byte width or file size, as the original reads a custom resource), or the game's binding of another slot (asked of the host for that slot).
- Applies wherever the adapter evaluates, the Present and Constants lists included: a query nothing answers gives the table's value there too (it left the variable as it was). An intercepted shader call of the owned domain keeps its transaction: there such an operand ends the call (`QueryService::strict`).
- The table is generated into `docs/ini-execution-matrix.json` (`operands`); scripts/check_ini_execution_matrix.py fails when the copy is not the source's.
- Verified in the sandbox: the INI runtime test (an unknown target reads -1 with a notice and the list runs; shader declaration queries without facts read 0 and -1), plan_test, the judge's sweep (`if vb0->stride == 8`, `if ib->size > 0` as the original's). Not run in the game.

## D-162: In a replaced draw's lists a command that cannot act does nothing; what the host cannot draw leaves the draw to the game after the lists ran (docs/architecture-review-2.1.md P1-3, R-01, R-02)

- Date: 2026-11-11. Before: a throw anywhere in a replaced draw's lists ended every list of that draw and the game's mesh stayed (R-01); the commands before the throw had run, those after it had not, so the Mod's variables were in a state the original never reaches (R-02).
- The original: the unit of failure is the line. A command is a call on the context; one that cannot act does nothing, a warning says so, and its list goes on (CommandList.cpp:6407-6479; `_RunCommandList` stops only at its recursion limit, with a warning).
- Decision, two kinds of failure, named in `frontend/xxmi/draw_plan.cpp`:
  - `refuse`: the command's own. The bridge holds it at the command, in a called list as in the root (`BridgeCommand::run`, the root loop of `execute_impl`, each operation of a batch): the command does nothing, `Runtime::skipped` says so once, the list runs to its end and the draws it records are drawn. D-155 did this for the Present, Constants and key lists; a replaced draw's lists now run the same way.
  - `refuse_draw`: the host's. There is no line of the original this corresponds to: a native command (the Core's list in C++) whose contract with the renderer does not hold, the draw's bone data missing, a buffer at a vertex or index slot that the layer cannot bind for the Mod's geometry, the planner's step and stack budgets. The reason is kept (`Runtime::whole`), the lists still run to their end, and the execution then returns that reason: the draw stays the game's. In a list that draws nothing it is one more skipped command, as before.
- What stays whole: a plan the file compiler cannot make buffers of (a vertex or index buffer that is no file buffer), the conversion's refusals in the layer, the native Core's contract (Phase 2 widens it). An intercepted shader call of the owned domain is one transaction as before (`Runtime::strict`).
- A command of the original's own that the bridge does not hold (an `if` or an assignment whose operands the CPU evaluates, D-106) and that raises ends the list it stands in, at the nearest bridged command; with D-163 its operands do not raise, a pool index outside the integers still does.
- `handling = skip` after the application's draw does nothing, as in the original (the context reads the flag between its pre and post lists); the plan took it in the post phase too.
- Counted: `refuse(` in draw_plan.cpp was 138 lines on 144abc0, each of which could refuse a whole draw. Now 101 `refuse(` lines, each a command that does nothing, and 7 `refuse_draw(` lines, the sites that leave a draw to the game.
- Verified in the sandbox: plan_test (67 checks), the Core pool, native bridge and texture query tests, the INI runtime test (502), the judge's cases and sweep. Not run in the game.

## D-161: A replaced draw's lists run against a draw state; what a draw makes of it is one table (docs/architecture-review-2.1.md P1-1, P1-2, R-01, R-08)

- Date: 2026-11-11. Before: the plan was a whitelist. A resource assignment was taken only as a reference of a Mod resource (or null) into a vertex slot, the index buffer or a pixel texture slot, with special cases kept in six tables of the adapter (`pixel_bindings`, `pixel_held`, `vertex_bindings`, `index_binding`, `application_textures`, `application_others` with `other_changes`); everything else refused the draw.
- The original (ResourceCopyOperation::run, CopyResourceToResource, ResourceCopyTarget::GetResource and SetResource): a source is read off the D3D11 context or a custom resource, a destination is set on the context or the custom resource, for every kind of slot of every stage; a null source sets null unless `unless_null`; a slot number out of range sets nothing with a warning. Nothing depends on whether the pipeline reads the slot. D3D11's runtime gives the original three things for this: a context that holds any binding at any slot, reference-counted objects that a custom resource can keep, and one context's order of calls.
- Decision: `frontend/xxmi/draw_state.h`.
  - `DrawState`: the context of one execution, one value per slot (32 vertex slots, the index buffer, 128 shader resources and 14 constant buffers per stage, 8 render targets and the depth target, 64 unordered access views for the pixel and the compute stage, 4 stream outputs), sparse: the lists of a draw set a handful.
  - `SlotValue`: the application's binding of slot S of the matched draw, a resource of the Mod, a buffer the draw's owned work wrote, nothing, or a resource the plan does not hold (IniParams, the back buffer, a region view). A slot the lists did not set holds the application's binding of itself. The value moves: `ResourceX = ref vs-t0`, `ps-t3 = ResourceX`, `vb3 = vb0`. A declaration holds a value too (`Runtime::references`); the application's binding it took means that of the draw it was taken in, and in a later draw it is a resource the plan does not hold (the original holds the object; the layer holds no game object beyond the draw).
  - Every assignment is `Runtime::assign`: source value, `unless_null`, destination (a slot, a declaration, every element of a pool, or nothing for a target the original does not set). It refuses nothing. `copy` into a slot binds what the source shows; `copy` into a declaration without keys of its own holds a file of the Mod or, for the lists of this draw, the game's binding; any other copy the plan does not make leaves null with the notice (D-155), and with the owned runtime in a draw's scope a copy of the Mod's resources is owned work as before (D-087).
  - A draw is recorded with the state it sees (`Runtime::issue`): per slot the lists set, `honour()` reads the table `honour_table()` (class of slot, class of value, the pipeline's stages, vertex slots and targets from the host): honoured (the binding stands before the draw in the plan); ignored (a compute slot, a stage or a vertex slot the pipeline has not, a target the pass has not); degraded (a graphics stage's constant buffer or texture other than a pixel texture, the game's binding of another slot: the game's binding stays, one notice per command); not drawable (a changed target: that draw alone is left out, the application's own draw is hidden); the game's draw (a vertex or index slot holding what the layer cannot bind: `refuse_draw`).
  - The plan's items are written where a draw is recorded: the bindings that differ from what the items before left bound, then the draw. The application's draw, where it stays, is such a record after the pre lists. A list that binds and never draws leaves no item. `PlanItem::application_slot` on a vertex or index binding means the game's buffer back in its place.
- The host tells the frontend what the pipeline reads: `RuntimeDraw::vertex_slots` (new), `shader_stages`, `render_targets` (`DrawPlanAdapter::pipeline`).
- A resource of the Mod that is no image the layer makes (a buffer, a target of owned work) at a pixel slot keeps the game's texture there with a notice, in the file compiler (`FilePlanCompiler::compile`), where the declaration's kind is known; it refused the draw.
- Queries read the state (D-163); `DrawPlanAdapter::held` gives a host what the lists bound at a slot.
- The table is generated into `docs/ini-execution-matrix.json` (`draw_state`).
- Verified in the sandbox: plan_test, the judge's 14 cases (two were recorded differences, two are new) and its sweep, against the build before with the new records. Not run in the game.

## D-160: character_backend.cpp in parts by concern, its shared types in a named namespace (docs/architecture-review-2.1.md P4-2)

- Date: 2026-11-10. H-02: one translation unit of 4326 lines held the generation, the folder, identity, materials, bones, recording, F10 and the status.
- Decision: eleven translation units by concern and one internal header, `layer/src/character_backend_internal.h` (the list is in that header and in ARCHITECTURE.md). The code was moved, not rewritten: a script took line ranges of the original, every non-blank line of it is in exactly one part, and the only new text is the glue (includes, namespaces, declarations, the qualified names of member functions now defined outside their class; 268 lines, listed by comparing the parts with the original). `character_backend.h`, `character_module.h` and the stamp of the shared headers are unchanged.
- What a split forces, and what follows from it:
  - What the one file kept in its unnamed namespace and more than one part uses is in the named namespace `efvk::character_internal`, used through a using-directive as an unnamed namespace is. A type is then the same type in every part, and a function or variable exists once. What one part alone uses stays in that part's unnamed namespace.
  - The run-time type name of those types changed with it. The compiler puts a hash of the source file's path into an unnamed namespace's name (seen in the objects: one value for every build of this checkout, another for the main checkout's). `character_owner_for` finds another copy's `CharacterCommand` by `dynamic_cast` to drop an owner unused for eight recordings; between two copies of the code in one process (the built-in one and an exchanged module) that cast met the other copy's owner only when both were built from the same source path, and meets it now wherever they were built. Within one copy nothing changes.
  - Member functions of `CharacterGeneration`, `FolderGeneration`, `CharacterController`, `CharacterBackend` and `GpuMesh` that were defined inside their class are defined in the part of their concern. The class keeps its data in the same order, the declarations, and the members of a line or two. A function defined in another part is no longer inlined into its caller.
  - Function statics, the thread-local hash tables and the globals exist once as before, each defined in one part. One default argument (`generation_create`'s predecessor) moved from the definition to the declaration.
- Not changed: no logic, no name, no message, no order of data members.
- Checked (sandbox): the whole suite before and after, step for step equal (54 passed, the one red step BUG-088's in both); plan digests of the 19 installed Mods as accepted; the recording path's own timers on the vk03 replay, old and new binaries interleaved, inside one build's spread (medians 50.0 and 50.6 us per matched draw under the validation layer). The cast between two copies of the code was not exercised.

## D-159: A slot the draw's shader has no register for is not declared, below t60 (amends D-124)

- Date: 2026-11-10. Found by the suite: the replay with a library call failed (no translucency capture, 226 plans refused: "no t register in the SPIR-V"). Not this round's change: the binaries of 2026-10-27 in the main checkout fail the same step. The cause is the EFMITTL the user installed on 2026-11-10 13:15, whose lists ask `ps->srv_type(n)` and `srv_dimension(n)` for every slot of a range (8 to 21, and a loop below t24); D-124 refused a query on a slot without a SPIR-V register, and one refused query refuses the whole draw.
- The original (CommandList.cpp, the shader target's evaluation): a slot the DXBC does not declare reads type NONE (0); `srv_stride` and `srv_dimension` read -1.
- Ground truth, the paired catalogue (180 shader pairs): of 9094 slots below t60 that a SPIR-V has no register for, the DX11 build declares 3 (one pixel t3 in the pairs of two shaders); from t60 up the DX11 pixel shaders declare 551 registers the SPIR-V lacks (explicit register assignments).
- Rule: without a register, a slot below t60 (kSourceExplicitRegisters) is answered as not declared, as the original answers it; from t60 up the query still refuses.
- A buffer of 32-bit words (D-124's other undecided case: DX11 declares it structured with stride 4 or raw, 74 and 72 times in the catalogue, the SPIR-V the same) is one of two answers. An `if` whose condition asks it is evaluated with every combination of the registers it asks (up to four) and goes on when the condition is the same, as it then is under the original whichever the DX11 build declares (BridgeCommand::checked, DrawPlanAdapter::undecided_words); a condition that differs, or any other command that asks, refuses with the line. EFMITTL's `ps->srv_type(n) == 1 && ps->srv_dimension(n) == 4` is false for both.
- A general rule of the game's two shader builds, checked on every pair by scripts/check_source_declarations.py, which fails when more than the 3 recorded slots differ.
- Effect on the Core's own test: `vs->srv_stride(0) == 16` on a vertex shader without t0 reads -1 and takes the other branch, as under the original, where it refused the draw before.
- Verified in the sandbox only: the unit checks of efmi_vk_character_test (a slot below t60, t60 and up, a word buffer), scripts/check_source_declarations.py on the paired catalogue (1555 answers equal, 9091 of 9094 slots without a register not declared by DX11 either), and the replay with a library call, whose refusals moved from the declaration queries to the next thing the new library asks (BUG-088, open).

## D-158: The host says what does not show and tries again (docs/architecture-review-2.1.md P0-1, P0-7, P0-8, P0-9)

- Date: 2026-11-10. The review's host findings (L-01, L-02, L-03, R-11): a Mod left out at the first load was in the log and the status only, a failed activation left a character invisible until the game restarted, F10 did nothing when the first load had refused, 64 packages.
- What a folder generation leaves out is announced when it is installed, at the first load as after F10 (`FolderGeneration::announce`): every Mod not loaded with its reason, every input left out, and every INI outside a character Mod whose mesh sections (a TextureOverride matched by index count or first index) match no draw in this layer (`CharacterProgram::unmatched_mesh_inis`, status `unmatched`). What the frontend does not do while the generation runs (LayerGap) is shown as it comes: three notices a generation, then one line for the rest.
- A package's activation that ends without being ready is a refusal of that package for now, not a state: its draws go to the game, the counter `activation_failed` says so, and the activation starts again after a pause that grows with the number of failures (1 s, 2 s ... 32 s). A matched draw starts the retry (it joins the worker that has returned and starts one); the work itself is the worker's, as at the first activation.
- F10 is the host's own key while the folder has no program; the folder's program takes it otherwise, as before.
- The package bound is 4096. It was a fixed 64 from the time of one package per character.
- Test hooks, in test executables only: EFMI_VK_TEST_ACTIVATION_FAILS, EFMI_VK_TEST_REFUSE_INPUT.
- Verified in the sandbox (live INI rendering, 118 checks; the four new ones failed on the build before). Not run in the game.

## D-157: The original as the judge of replaced draws (docs/architecture-review-2.1.md P3-1)

- Date: 2026-11-10. The judge of D-150 compares Present lists; no draw ran there, so the path every character draw takes had no comparison with the original (review V-01). Now the judge's program is the "game" (sandbox/dx11_judge_scene.h): two indexed quads a frame of known index bytes, DrawIndexedInstanced as the game draws; the judge's d3dx.ini has `track_region_hashes = 1` and a ShaderRegex that checks `ib` at every draw, as the EFMI Core's; the original runs the case's [TextureOverride] sections by region hash. Read off the original: eight points of the picture (two per place, one in each triangle), the primitives each game draw put through the input assembler (D3D11_QUERY_PIPELINE_STATISTICS), the saved variables.
- The layer's side is the frontend's plan (`runtime_test.exe --judge-scene`): scripts/check_dx11_judge_draws.py works out from each draw's plan (bindings, draws, skip, pre and post) the picture and the primitives it means, by D3D11's rules for the scene's files, and compares. It does not cover what CharacterProgram::plan and the backend make of a plan, nor lists that call the EFMI Core.
- Cases (sandbox/judge_draw_cases): replace, texture_only, no_texture, post, half, nonindexed, auto, order (match_priority, then the name: 321), operands (draw_type 4, index_count 6 ...), unmatched: all as the original's. Known differences, run and required to still differ (KNOWN in the script): `other_stage` (a binding at another stage's slot left changed refuses the whole list; the original draws) and `copy` (a copy between the Mod's resources in a draw's list stays null without the owned runtime; the original draws): the two faces of review R-01 the first run showed.
- A step of run_all, in the `ini`, `owned` and `quick` groups.
- One line at a time (`--sweep`, a step of its own, P3-2): a working replaced mesh and each line of sandbox/judge_draw_sweep.json in front of its draw. The file is the record: `expect`, and for a difference its `class` (`layer`, or `scene` when the layer's side of the scene lacks what answers the line in a character generation) and `finding`. The step fails when a line that agreed differs and when a recorded difference agrees, so the count can only be changed on purpose. 2026-11-10: 50 lines, 35 as the original's, 12 differ in the layer, 3 not judged. The sweep's first run found BUG-086 and BUG-087.

## D-156: A failure stays with its unit (docs/architecture-review-2.1.md Phase 0, docs/ini-semantics.md)

- Date: 2026-11-10, the user after the review: do the parts that need no game session. The original's unit of failure is the line; the layer's units were the Present, the generation's owned GPU work, the folder. Now:
- A command of a Present, Constants or key list is on its own at every depth of called lists (BridgeCommand::run), whatever it raises. An owned GPU command that fails before it recorded work (ResourceCommands::recorded) leaves its scope as it was; one that fails after drops that scope's batch, is not run again, and the next scope starts clean (GpuResourceRuntime::failed; the flag was permanent and, since D-131, every Mod's). A CustomShader section without the owned runtime is inert and said, not a refused load.
- At load: `prepare` names every Mod it cannot prepare and the folder leaves out exactly those; a second INI under one namespace is left out (the original keeps the first file's sections); an input the folder does not load with, when every Mod loads alone, is found by halving (plain INIs first). The Core's names the native handlers bind are checked and named when missing; RegisterMod compares with the installed Core's `$version`, as its own list does (the constant 1.48 was below the installed 1.49). A LOD no line assigned has no remap, as the Core's `@cs-t1 != 0` (BUG-084).
- What the layer does not do while a generation runs goes through LayerGap to the host (IniRuntime::gap_count, gaps_since). The overlay shows the first three of a generation and one line for the rest; an acting ShaderOverride is said there, ShaderRegex sections (the libraries', provided natively) stay the original's warning.
- Not changed: a replaced draw's lists still stand or fall together (Phase 1).

## D-155: Every enabled INI of the Mods folder is an input of the folder generation (docs/ini-semantics.md)

- Date: 2026-10-30, the user: the parsing must not stop at character Mods; every INI under Mods takes part as in the original, and a parse error is handled the original's way. The original's `include_recursive = Mods` parses every enabled INI into one space; a section it cannot use is an overlay warning and the rest loads.
- The folder generation's inputs are now every enabled INI in the original's include order (enabled_inis), except the native libraries' files (their key and ShaderRegex sections join through native_library_files, the rest is the interface's) and anything under the folder of a character Mod the isolation left out. The character Mods are still those with `[CommandListInitializeMergedSkeleton]`; `libraries()` only decides which library folders a Mod's package owns files of (its notes say "library called"). A Mod that is no character Mod runs its [Present] and keys as owned work, keeps its persistent variables in mods-folder.ini, and its TextureOverride sections by texture hash act in replaced draws' pixel slot checks (D-077); it acts on no other draw (the layer matches none) and it is no package. LIMIT-046 lifted.
- A copy between declared resources the plan does not make (`ResourceA = copy ResourceB`, `= copy_desc`; in a [Constants] list or a draw's) refused the whole generation, or the whole draw, before. Now the destination stays null, as the original leaves it when the source is null, the warning names the line once, and the list goes on; every other kind of source keeps refusing. A file-backed buffer so copied is the next item (the original creates one with the description and no data, which the Mod's compute then fills: the user's `\u6c64\u6c64` Mod and Typhoeus).
- The original runs each command of its Present and [Constants] lists on its own: one that cannot act does nothing and the list goes on. The planner ended the whole execution at the first refused command, so one Mod's line (`run = CommandListUnbindAllRenderTargets` in a Present: `o0 = null` has no binding destination in the plan) silenced every Mod's Present lines parsed after it. Root lists now run tolerant (DrawPlanAdapter::tolerate_refusals): the refused command does nothing, the original's warning names it once, the rest run; a replaced draw's lists keep their all-or-nothing plan. A Present whose draw items nothing executes says so ("no executor"), which the offline dry runs take as their own limit; the dry runs likewise leave out refusals of services the stand-in lacks (the owned GPU runtime, instance windows, render targets), which the layer has in the game. The status's INI warnings (`diagnostics`, `diagnostic_messages`) are live from then on: the load's and those a Present or a draw raised since (the host keeps the last 64), where they were the load's snapshot.
- Not done: a folder without any character Mod loads nothing (the original would run the plain INIs' Present); a plain INI that still refuses at load refuses the folder with its reason (no isolation for them; no such refusal is known).

## D-154: INI files read by the original's reader, the Mods folder walked by its rules, d3dx.ini's own sections first (docs/ini-semantics.md)

- Date: 2026-10-30, the user's review of the INI parsing. The parser itself is the original's (IniHandler.cpp included unchanged); what the layer did around it diverged in four places, each checked against the original's code and, where a file's bytes decide, against the original as the judge.
- Files: the original reads an INI through ParseNamespacedIniFile (wifstream, codecvt_utf8 with the byte order mark consumed, text mode); a byte sequence that is not UTF-8 ends the stream, so the lines before it are read and those after it are not (judge case `encoding`: `$before` 1, `$after` 0; a BOM and CRLF are read whole). The layer's read_ini decoded the file itself with MB_ERR_INVALID_CHARS and refused the whole file ("INI is not valid UTF-8"), so a Mod saved in a legacy code page was left out of the folder where the original runs what it read. Now read_ini is the original's reader (HostParseFile); its 32 MiB budget is gone.
- The folder: the original's ParseIniFilesRecursive sorts each folder's names without regard to case (WStringInsensitiveLess) and takes a file as an INI when its last four characters are `.ini` as written (a `.INI` is not one). The layer's five walks (character Mods, native libraries, called libraries, a Mod's other INIs, the folder's include order) used an ordinal sort and a case-insensitive extension: a Mod in `Zeta` was parsed before one in `fixture` (its [Present] lines first), and a `mod.INI` was a character Mod to the layer but never an input, so the whole folder refused with no Mod left out (it loaded alone). One walk now (enabled_inis) with the original's rules; the packages come in that order too.
- The installation's d3dx.ini: the original parses it first, not namespaced; the layer never read it, so its [Rendering] keys the parser and the compiler take (recursive_include = 1 in EFMI's, track_region_hashes = 1, ini_params) were defaults, a command list it declares was an unrecognised entry to Mods (LIMIT-039), and a [Constants], [Present] or [Key] written there did not exist. Now its sections except [Include...] (the Core's files and the folder's enabled INIs are the layer's inputs) and [Loader] join first (RuntimeConfig::installation: the Mods folder's parent, else the Core's grandparent).
- Budgets the original does not have, gone: 128 INI inputs per generation and 64 per Mod folder (a player with many Mods crossed them with the whole folder refused).
- A Mod alone (the folder's isolation retry, `m2_folder_generation = 0`, the dry runs) is named as the folder names it: an INI without a namespace line by its path from the Mods folder's parent, in any script (the original's implicit namespace); the generation's resource root is that parent. Before, a Mod alone was named by its file name and had to be ASCII, so a Mod in a folder named in another script (the user's `\u6c64\u6c64`, installed 2026-10-29) could never be tried alone: when the folder refused for any reason, it was left out with "its file name is not ASCII" instead of the real reason. The native Core bridge takes any printable name without brackets for one Mod as for the folder. Found behind it: the bridge widened a Mod's UTF-8 name byte by byte (0xE6 to U+FFE6), so a Mod named in another script never found its own `[CommandListInitializeMergedSkeleton]`; the bridge's lookups of a Mod's declarations (that list, `$bones_count`, `$max_instance_count`, the three skeleton pools) name what is missing instead of a map's error.
- Left as they are, recorded: enabled INIs that are no character Mod's, no library a Mod calls and no native library are not loaded (LIMIT-046); dormant command lists are compiled only when reached (the original parses every one at load and warns on a typo there); a `condition` line in a file's preamble is honoured by the parser but not by the character-Mod detection.

## D-153: A copy of a region past its buffer copies nothing (docs/ini-semantics.md)

- Date: 2026-10-29. Undefined in D3D11 (the original's device stops answering under the judge), refused by the layer; now nothing is copied, the destination is made and reads zero, and the list goes on. Copy destinations are created zeroed.

## D-152: The application's other bindings saved and put back leave a replaced draw as the application bound it (docs/ini-semantics.md)

- Date: 2026-10-28. Snowshine's LOD 1 correction saves, nulls and restores vs-t0 around owned work; the plan refused the first line. Stand-ins for the application's bindings at non-pixel slots (as for pixel slots) let the plan see the restore; any slot left changed at a draw or at the end still refuses (the game's bindings in Vulkan terms remain T-030).

## D-151: Views D3D11 does not make are null views; a first use adds its destination's bind flags (docs/ini-semantics.md)

- Date: 2026-10-28, found by the judge (D-150). The original's CreateCompatibleView returns null where D3D11 refuses a view, and its callers bind, clear or copy through nothing; the layer refused those Presents. CustomResource::Substantiate adds the bind flags of the target it is first fetched for (references and slot ranges); the layer's load did not. A texel-buffer view starts at a single texel on Vulkan 1.3 (texelBufferAlignment properties), as a D3D11 FirstElement does.

## D-150: The original as the judge: XXMI's own d3d11.dll in a D3D11 program of the project's (docs/ini-semantics.md)

- Date: 2026-10-28, D-137 step 2. Cases run in [Present] under the pinned XXMI 3DMigoto (extracted from F:\vulkan into build/, a d3dx.ini of the judge's own naming the judge program; the installation untouched) and under the owned GPU runtime; their persistent variables are compared. A run_all step.
- Its first findings, fixed: a description D3D11 does not create is a null resource (was a refusal), and a clear without a view of its kind clears nothing (was a refusal).

## D-149: DDS arrays and cubes load as their first layer for a replaced draw's 2D slot (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. The layer re-points only nonarray 2D fragment images; D3D11 leaves a 2D slot reading another view dimension undefined (drivers read the first slice). A DDS array or cube loads as its first layer (levels); volumes still do not. Buffer-description sections stay inert: the game's buffers are shared 16 MiB pools (DX11 capture), so in the original they would meet nearly every draw.

## D-148: Draws past their files as the input assembler reads them (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. A draw keeps its whole triangles inside the index file (index 0 past it draws nothing in the original); its own index range decides the vertex bounds (block extrema, the file read exactly only when needed); a draw reading past a vertex file (zeros in the original) is left out, the list goes on.

## D-147: store in a replaced draw reads a frame late; clear runs as owned work (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. The original's store waits for the GPU; the layer's owned work in a replaced draw runs after the game's recording, and a wait on the game's thread froze it before (BUG-062). A store there takes the value its copy read at an earlier execution, once that submission completed (ResourceCommands::feedback); before the first the variable stays. clear of the Mod's resources is owned work. Exact timing would need a wait on the game's thread: the user's priority is no freezes; to be revisited if a Mod needs the same frame.

## D-146: dispatch, drawauto and indirect draws in a replaced draw do nothing, with a warning (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. They refused the whole draw. A compute dispatch cannot run in the game's render pass and the layer has no stream output; the indirect draws' GPU arguments are not taken by the plan (no Mod uses them).

## D-145: A view D3D11 does not make binds nothing in owned work (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. As CreateCompatibleView's null view: a typed texture under another format, a typeless one without a format, bind nothing; the Present goes on.

## D-144: CheckTextureOverride of other targets in a replaced draw (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. As find_texture_overrides_for_resource: a Mod's resource meets nothing; the matched draw's own index buffer meets its region hash's sections through the original's region path; the game's other buffers and other stages' resources have no known DX11 identity and meet nothing, with the original's warning.

## D-143: Slot ranges in a replaced draw as the original's single slot operations (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. A slot range refused the replaced draw.
- The range resolves with the original's code (FrontendSlotRange); each slot runs as a ResourceCopyOperation through the plan; an untyped pool element nothing assigned binds nothing.

## D-142: A replaced draw issues the original's direct draws (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. The plan took DrawIndexed only and the program one instance only.
- As DrawCommand::run: DrawIndexedInstanced, Draw, DrawInstanced, from_caller (the matched draw's call) and the three `auto` counts (the bound buffer's whole elements; none, nothing drawn) are planned and drawn (vkCmdDraw for the non-indexed ones).

## D-141: The character contract's sizes are the widths of the layer's indices, not budgets (docs/ini-semantics.md)

- Date: 2026-10-28. The user: LOD 2 and 3 sections are not to be done; everything else of D-137's gaps is.
- The original Core sizes its pools by the Mod's counts. The layer's component masks are 64-bit (stream, list, identity), and the limits are 64 components, 65535 bones, 4096 mesh sections and files, 4096 Mods in a folder.

## D-140: A texture declaration's colour space, format and size keys as the original takes them (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. Any of these keys on a texture a replaced draw bound refused the draw.
- As LoadFromFile and CreateCompatibleView: the colour space is a loader flag (DDS forceSRGB, WIC FORCE/IGNORE_SRGB) and part of the image's identity; the size keys do nothing to a file texture; the declaration's format is the view's, and a typed texture without that format, or a declaration whose bind flags leave out shader_resource, binds nothing.

## D-139: Image files load as the original loads them (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. A replaced draw bound DDS files only; a broken texture refused its draws and an F10; owned work took PNG only and refused its Present for a file it could not load.
- As CustomResource::LoadFromFile: a .dds by the DDS loader, every other file by WIC (frame 0, the pinned DirectXTK table and sRGB rule; assets::decode_wic), in owned work and in replaced draws. A file the loader makes nothing of is the original's null resource: owned work warns and goes on; the character program finds it at load (image_loads) and binds nothing for it, the Mod told.

## D-138: Sections matched by a resource's description, as the original matches them (docs/ini-semantics.md)

- Date: 2026-10-28, D-137's gaps. A TextureOverride with match_* keys refused the whole Mod.
- As the original (find_texture_overrides_for_resource: hash sections, then the description's, filtered by the draw), a section that can match a 2D texture runs at every CheckTextureOverride of a pixel slot against the D3D11_TEXTURE2D_DESC the layer makes of the game's image for its texture hash (D-077), with the original's matcher unchanged; an image the layer cannot describe and a Mod's own resource meet none.
- What the layer cannot do as the original: a section matching the game's buffers by description (the draws are found by their index region's hash; no D3D11 description of the game's buffers is established), one for 1D or 3D textures, and keys that change the game's resources when they are created, mapped or copied. Each such section is inert, the Mod loads, and the Mod is told (CharacterDiscovery::gaps: the log, the first three as overlay notices).

## D-137: The original is the INI's specification, including how it fails (docs/ini-semantics.md)

- Date: 2026-10-28. The user: the INI handling keeps meeting problems; Mods are written every way; think it through, general, never per Mod; then: do all of it.
- Each of the original's commands is implemented with its D3D11 meaning, or degraded as the original degrades (null resource, command without effect, draw goes on), or listed as impossible under Vulkan and degraded visibly. No refusal of a whole draw for a unit's failure, no budget the original lacks. Degradations are shown as XXMI shows warnings (overlay) and logged. Checks take the original as the judge over every installed Mod in every state its keys reach, and per command against DX11 with XXMI.
- Order: classify every refusal point and degrade where the original does, with the overlay notices; the checks; the gaps closed by implementing the commands for replaced draws as owned work already does.

## D-136: The loader is the module that holds the code; meshes in device memory by copy without resizable BAR

- Date: 2026-10-28 (2.1.1, BUG-074, BUG-075), from the review the user asked for.
- As 3DMigoto takes the original d3d11.dll from the system folder whatever else carries that name (D3D11Wrapper.cpp: original_d3d11.dll, else GetSystemDirectory), the inject entry patches only a module whose vkGetInstanceProcAddr export is code, follows a proxy's forwarders to the module it waits for, and logs every candidate. One loader is patched (one set of originals); a second is logged.
- As D3D11 fills a DEFAULT buffer from initial data, a character's mesh goes into device memory: mapped where the host sees the largest device-local heap, copied on the layer's upload queue otherwise (the worker waits for the copy before publishing), host memory only without an upload queue. Tests simulate the small window with EFMI_VK_TEST_SMALL_BAR in their child process.

## D-135: The playable release, GPLv3, without the probe (docs/release.md)

- Date: 2026-10-27 (2.0.9). The user: publish a release under GPLv3 (the layer compiles 3DMigoto's GPLv3 engine); its text tells players how to install and use it; nothing for development goes in; players hold a playable version.
- `scripts/build.py release`: the inject DLL built with `EFVK_NO_PROBE` and without ipc.cpp (the probe pipe's access list let any interactive user's process drive the elevated game, module exchange included), the three GPU tools it loads (not dxil.dll), a configuration with `m2_mods = ../Mods`, README, LICENSE, THIRD_PARTY_NOTICES.md, licence texts; zipped with its SHA-256. Symbols stay in layer/build/release. A relative configured folder is now taken from the DLL's folder, not the game's working directory.
- `scripts/check_release.py`: the archive's files and digest, no pipe name in the binary, the release injected into the sandbox with the installed Mods (judged from its log: every character Mod loaded, no fault, no probe pipe, validation clean).
- One DLL was considered and declined: DXC exists only as Microsoft's DLL, vkd3d-shader is a MinGW build, loading from memory is never done; SPIRV-Tools could be linked statically.

## D-134: Every F10 operation against the original (docs/f10-operations.md)

- Date: 2026-10-27 (2.0.8). The user, after BUG-067: consider everything a player may do around F10. Contract, from ReloadConfig: each press reloads (BUG-069); what the new configuration no longer has is released once (BUG-067); a Mod gone and back is the same Mod (BUG-068); what the draws see comes from the live generation only (BUG-070). Each row of the document names its check; the Mods-folder scene ends with the player's operations (disable the drawn Mod, enable it again, two F10 in a row, RabbitFX off and on).

## D-133: RabbitFX's anti-dither from SPIR-V facts and the library's own gate

- Date: 2026-10-27 (docs/rabbitfx-dither.md). The user: RabbitFX's anti-dither has no effect, adapt it. The original: RabbitFX's ShaderRegex replacements wrap the character shaders' dither discard (interleaved gradient noise against the instance fade in cb2) in `if_z IniParams[172].w`, its sections assign `w172 = $censor` (`\` key). Under Vulkan the sections are inert (LIMIT-036).
- Design: the dither kill found in SPIR-V by data flow (the noise constant 52.9829 and the instance window at set 2 binding 0; 38 of 263 captured fragment modules, noise-only and cutout kills left alone); a patched module whose dither branches never kill; the gate is the IniParams element the replacement text tests, assigned by the sections' own lists, evaluated at each presentation; applied to every draw as the original's replaced shader is: twin pipelines for the game's draws while the gate is on, the patch in replaced draws' variants. Steps test first, in the document's order.
- Amended in 2.0.8 (BUG-071, BUG-072): the rule needs the camera fade (the larger of two instance window values) in the kill's condition, as RabbitFX's patterns need `max r, cb.x, cb.x`; twins and the other state of replaced draws are prepared ahead, behind requested variants.
- Done in 2.0.6. Amended while building it: the gate is read once after the load too (RabbitFX's assignment runs at a replaced shader's first draw, so the first frame already sees it); the first bind of a dithered pipeline waits for its twin, bounded (the original's patched shader exists from the game's shader creation); the twins follow RabbitFX's gate alone, not the layer's replacement switch (DX11's patched shaders do not depend on the character Mods). RabbitFX itself is the layer's native interface (D-082): the installed file's [Key...] and [ShaderRegex...] sections join it and `$censor` persists.

## D-132: A section per `vbN->stride` branch, matched by the draw's pipeline binding

- Date: 2026-10-27 (LIMIT-041). An EFMI export may tell a component's LODs apart by the vertex layout: one TextureOverride whose `if vbN->stride == K` / `elif` branches each assign one component and LOD (avywenna's face, 22a60235: stride 12 component 0 LOD 0, stride 8 component 3 LOD 1). The original evaluates the condition per draw from the bound vertex buffer's stride (`GetResourceStride`, `IAGetVertexBuffers`) and the Core identifies the draw with the component the branch assigned.
- The layer resolves a draw's component before it plans (identity, bones). Discovery therefore makes a section per branch (`CharacterSection::stride_slot`, `stride`; not in the package file): the condition is an equality of a vertex-buffer stride operand and a constant, every branch assigns one constant component, no assignment of the component lies outside the branches, one slot for all, strides distinct. The device index matches such a section only to a draw whose pipeline binds that slot with that stride (a pipeline with dynamic strides matches none); the plan then answers `vbN->stride` from the same pipeline, so the branch the list takes is the section's.
- Checks failing before: the installed Mods' dry run refusing nothing for the stride (scripts/check_mods_dry_run.py, the stride no longer a known limit); a fixture Mod whose first section branches on `vb1->stride` plans a section per branch offline (runtime test); the Mods-folder scene with the non-matching branch first logs no contract refusal (the same scene with the device check disabled fails).

## D-131: One generation for the Mods folder, packages per character Mod

- Date: 2026-10-27 (docs/folder-generation.md; docs/3dmigoto-flows.md section 3). The original parses d3dx.ini, the Core and every enabled INI of the Mods folder into one space and runs every matching TextureOverride of every Mod; its Core keeps no per-Mod identity (`RegisterMod` sets `$mod_id` to 1 or -1), a Mod points the Core at its own resources before each call. The layer had a generation per character Mod: the Core parsed once per Mod, a Present list per Mod, F10 loading them one after another.
- One generation for the folder; per character Mod the device-side package as before. The native Core bridge serves several Mods: per Mod its contract, discovery and prepared skeleton; a native call answers the Mod whose TextureOverride's list runs (DrawPlanAdapter::current_root) and holds only the draw's own Mod (RuntimeDraw::mod, which the renderer's identity resolved) to the contract; another Mod's list on the same draw finds no instance of its own and draws nothing, as the original Core's identification answers it.
- A Mod the generation cannot take drops only itself: when the folder refuses, the Mods that refuse in a generation of their own are left out with their reasons and the rest load again.
- Step 1 (2026-10-27, offline): `CharacterProgram::discover_folder`; inputs in the original's include order (per folder its INIs by name, then its folders), implicit namespaces as the original names them (the INI's path from the Mods folder's parent). Discovered sections come in one order (hash, count, first index) whatever else the registry holds. Check: scripts/check_folder_generation.py (suite step) runs the dry run per Mod and for the folder and requires every Mod to load, discover and plan the same; on the installed folder 19 of 20 load in both (Typhoeus refuses alone offline and is left out of the folder with its reason).
- Step 2 (2.0.0): the layer on it: one folder generation with m2_mods (switch `m2_folder_generation`, default on), the INI Present once per presentation, the packages' plans under one lock (the original's one context), one settings file (`mods-folder.ini`, the Mods' own files adopted once), F10 exchanging every package at one presentation. Measured on vk03 (20 Mods): presentation work 221 to 146 us per frame, recording +2.4%, pictures identical.
- Found on the way: a plan's `vbN->stride` is now answered from the draw's pipeline (RuntimeDraw::vertex_stride); a CPU generation without owned reads answers resource queries from the host's CPU answers (the dry run had refused every one); LIMIT-041.
- Step 5 (2.0.1, ISSUE-011): the original keeps no host copy of a buffer made from a file (LoadBufferFromFile reads it, CreateBuffer takes it, the bytes are freed; the resource is substantiated at first use). A package releases its payload once validated and catalogued (`release_payload`): per stream its file, offset and the CRC32C of the validated bytes. The upload at activation (`read_payload`) and a stream a live plan adopts for bones (`read_stream`) read the files again and refuse bytes that are not the validated ones. The upload reads with up to four threads, each stream group into its own part of the mapped memory through a 1 MiB bounce buffer (one thread took 16 ms for a 23.7 MB character and skipped its draws one frame longer at startup than the held payload; four take 4.5 ms). Private memory, actual configuration with the installed Mods folder: 712 to 298 MiB.
