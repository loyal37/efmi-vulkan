# EFMI Vulkan - Architecture

Original ShaderRegex generations now own native groups, models and immutable
bounded DXBC. Native matching/assembly runs against isolated override-map nodes;
only fully prepared pattern-only links publish. Original root/local-variable
storage stays stable. DxbcCompiler (candidate1.3.4) translates exact bytecode
through pinned vkd3d2.1, derives registers/signatures by scanning instructions and
reflects the emitted SPIR-V into the existing owned executor. It is separate from
HlslCompiler, which recompiles CustomShader source with FXC/DXC. Patched regex
runtime publication remains a later explicit integration boundary.

Translated float32 execution selects hardware denormal control or bounded
integer-bit lowering of arithmetic operands/results, preserving pure data moves.
Both require explicit signed-zero/infinity/NaN preservation and nearest-even
rounding with actual device enablement/properties. Fractional results retain the
native range below1. Candidate1.3.5 tests software denormals on NVIDIA and hardware
denormals on Intel; neither implies game enablement or application provenance.
See docs/bytecode-compiler.md and docs/shader-matching-audit.md.

Compute follow-up1.1.8 adds bounded executable workgroup/provenance analysis,
indirect argument-buffer retention/ordering and original source sampler slots.
Null samplers use native defaults; explicit snapshots and shared sampler quotas
are host inputs. No implicit CPU count feedback or application import. Native
audit found BUG-022 binding/batch/OM side effects; fix before graphics integration.

GpuShaderRuntime (1.1.7) composes original CustomShader parsing/command objects,
GpuResourceRuntime, HlslCompiler and ShaderCommands. It explicitly opts into
general declarations; the deployed character extension does not. All sources
compile before generation acceptance; supported compute pipelines are lazy.
Nested phases share original state/budgets; RAII restores compute/OM/viewport.
Each dispatch owns immutable descriptors and retained resources through its
ResourceSubmission fence. Production SPIR-V validation uses actual block-layout
enablement. Execution shares the explicit serialized outside-render-pass queue
domain, without implicit submits/waits. Graphics/application import/placement
remain open. See docs/custom-shaders.md and D-047.

HlslCompiler (1.1.6) supplies original preprocessing/model-5 validation followed
by DXC SPIR-V compilation. FXC reflection owns the source register mapping;
SPIRV-Reflect supplies Vulkan descriptor/IO metadata and counters. Stages and
register classes occupy separate bindings. Dependencies remain read-only and
bounded; compilation never changes process CWD or writes caches beside Mods.
Native GPU reference tests cover a parameter consumer. General graphics shader
execution, pipeline state and application integration are still separate work.
Details: docs/hlsl-compiler.md.

Current development: the pinned original parser/evaluator owns enabled-directory
generations, dependency graphs, CPU variables/keys/presets/pools and command
objects. The optional GpuResourceRuntime extension connects those original
commands to ResourceCommands, an owned Vulkan execution domain for mutable
buffers/images, transfers, clears, buffer store feedback and logical source slots. Copy pools
own per-description allocations independently of aliases; fence tickets retain
resources through submission. Whole-pool assignments retain the source resource
before changing destinations, while using original per-element metadata/counters
and copy pools. Details: docs/gpu-resource-commands.md and D-045.
Original FIFO/spatial/expiry rules own pool mapping and null state; bindings and
fence tickets retain GPU allocations separately. Runtime inputs supply original
frame_time, and the source spatial radius is guarded before its integer cast.
Present/draw failure notifies extensions even for CPU-side errors, so pending
GPU work from a failed list cannot be submitted.
The owned executor also supplies image allocation/view/transfer/clear/resolve
primitives (1.1.2). Actual image capabilities and enabled logical-device features
are explicit host inputs. Format footprints come from the pinned Vulkan registry.
Pending image layouts live in the submission, with GENERAL published only after
successful submit; discard cannot change an owner's last submitted state. Views
and fence tickets retain image allocations independently. The 1.1.3 image adapter
connects original declarations, DDS decoding, views, copies, clears, properties
and color resolves. It reuses original CPU description/view helpers, preserves
DXGI family rules and adapts view-specific clear formats using owned scratch
images. Native WARP view checks and Vulkan pixel readback stay in the sandbox.
The 1.1.4 conversion paths add native-reference integer RTV conversion and
single-sample R16/R32 color/depth bit transfers through reusable resident buffers.
Scratch owners share budgets and submission lifetime. Opaque bits entering D32
require actual logical-device unrestricted-depth enablement; CPU float-depth
uploads otherwise validate copied texels before recording. These checks do not
change the application's device features or introduce an implicit GPU wait.
The 1.1.5 parameter adapter owns a float4 Texture1D/SRV, publishes zero before
Constants and preserves original dirty/root/draw/transition update boundaries.
Its source special target returns the published image without forcing an update.
Root pre/post abort is isolated; nested phases still share state. An optional
draw callback is a shader host boundary, not a general shader implementation.
The native test consumer verifies actual HLSL reads separately from this adapter.
This requires an explicit outside-render-pass scope on a host-serialized queue.
It does not replace the character hook's file-plan renderer, import application
resources, or emit shader descriptors/draws. Remaining image formats/conversions, shader adapters and complete
directory graphics remain development work. Use CURRENT_STATE.md for verified
source, candidate and deployed versions rather than historical paragraphs below.

Deployed 1.0.2 integration: the pinned original XXMI runtime is linked into the layer and
exchangeable backend. A `.runtime.json` sidecar opts a character profile into
live main-INI execution. `IniRuntime` owns isolated parser/variable/pool/key
generations; `CharacterProgram` translates owned evaluated plans into the existing
renderer; native Core boundaries retain the verified identity, bone import and
draw path. Original input conditions and duplicate listeners, persistent values,
and staged F10 resource generations run inside that backend. Without a sidecar,
the static package behavior stays available. Details and explicit unsupported
boundaries: `docs/live-ini.md`, D-043. The image panel HLSL compiles and validates
with DXC, but panel rendering and full-folder acceptance remain T-016. Use
CURRENT_STATE.md for the verified/deployed/game-loaded checkpoint.

The following are historical integration checkpoints.

Layer 0.10.0: `character_package` and `character_backend` replace a whole character from a static package, with one merged skeleton per character instance and the game's own material; see `docs/character-backend.md`. `m2_package` selects it or the single-component backend by the file's magic.

Earlier integration checkpoint: the frontend described next was built at 0.9.3 and is not in the DLL.
FilePlanCompiler copies original XXMI file declarations into owned per-operation
contracts; frontend/assets/file_plan declares/demands the shared ResourceCache and
collects immutable typed GPU owners atomically. Explicit nulls and native pre/post
boundaries remain distinct. Core/material operations and shared character state still
need resolution before execution; this frontend is not in the DLL. See
docs/frontend-draw-plans.md. Multi-mod residency tuning is deferred until this
character's replacement works, per the user's priority clarification.

The target product architecture is in `docs/plan.md`, section 4. Source/build/next are 0.9.2; dist remains 0.9.1. The optional component-7 hook emitted replacements in a user-started display session; appearance/animation are not accepted. 0.9.2 adds shared stream contracts and ordered draw transactions (`docs/general-replacement-operations.md`) with full sandbox verification, not deployment. Protocol 1, server 0.1.0. Resource inspection is in `docs/probe-resources.md`, snapshots/waiting in `docs/probe-snapshots.md`, owned variants in `docs/m2-pipeline-variants.md`, and the hooked M2 path in `docs/m2-component-hook.md`. D-016/D-033 retain frontend-driven component data; no complete INI engine or whole-mod runtime exists yet.

## Data path of the development probe

`MergedSkeleton` (0.8.2) is an offline-tested CPU master with
bounded layout, per-component/instance update stamps and atomic rolling-buffer imports.
It patches only constant 5's two dynamic offsets and owns no GPU resources.
See `docs/m2-bone-import.md`.

`VariantCache` (0.8.3) is a bounded worker; `DrawSnapshots` owns private,
aligned per-draw bone/instance copies and immutable sealing. Both now run in the validated
GPU sandbox; see `docs/m2-cache-and-snapshots.md`.

`M2Mesh`/`M2Backend` (0.9.0) load an explicit bounded component package and prepare its mesh
and snapshot arenas off the draw thread. The opt-in indexed hook matches original mapped
indices, requests a ready variant, reads both mapped poses, imports a self-contained palette
and emits a private VS draw while preserving FS and restoring original bindings. `M2Command`
retains mesh, variants and per-draw snapshots with the application's recording. Submit seals
them; successful legal begin/reset/pool reset recycles completed slots, and free/pool destruction
releases them. Disabled, unready or unsupported conditions keep the original draw. Both workers
join before device teardown. IPC `m2` controls future recordings; `status.replacement` reports
readiness and outcomes. No completion is inferred from presents.

`agent -> Python MCP stdio (or CLI) -> named pipe -> efmi_vk_layer.dll inside the Vulkan process -> tracked state / mapped memory / shadow copies -> result`

The DLL sits in front of the application's Vulkan calls. Without an explicit enabled M2
package it forwards every call unchanged; the ordinary probe only observes.

## Two entries, one binary

The build writes the same binary twice: `efmi_vk_layer.dll` and `efmi_vk_inject.dll`. The file name selects the entry (`util.cpp`; the setting `entry` overrides it).

- **Layer entry** (`efmi_vk_layer.dll`, named by the manifest `efmi_vk_layer.json`): the Vulkan loader loads the DLL, negotiates through `vkNegotiateLoaderLayerInterfaceVersion` and passes link info in the create calls; the hooks call the next element of the loader's chain.
- **Inject entry** (`efmi_vk_inject.dll`): something loads the DLL with `LoadLibrary`, the way a mod launcher injects libraries into a game. `DllMain` pins the module, starts `bootstrap.cpp` and, in probe mode, the pipe. `bootstrap.cpp` registers a Windows loader notification (`LdrRegisterDllNotification`) and, as soon as `vulkan-1.dll` is mapped (or at once when it already is), redirects its export table entries for every hooked function to 16-byte jump stubs allocated within 2 GB above the module. The file on disk is not touched. The hooks then call the addresses the exports had. `vkCreateInstance` and `vkCreateDevice` have no link info in this entry; they call the original exports, and `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` hand out hooks or fall through to the originals.

In the inject entry a hook can be reached, through a redirected export, for a device that was created before the DLL attached. `dev_of` then returns the **passive device**: a static `Device` whose dispatch table holds the original export addresses and for which every tracker function returns at once. Calls are passed on untouched and counted in `passive_calls`. The order in `patch_loader` is: learn every original address, make the passive device known, then redirect.

If the loader offers to use an inject-entry DLL as a layer as well, negotiation is refused, so nothing is hooked twice.

The technique mirrors `bootstrap.cpp` of the EFMI MCP probe, which redirects two exports of the system `d3d11.dll` and is verified in the real game under DX11.

## Layer modules

| File | Responsibility |
| --- | --- |
| `layer/src/layer.cpp` | `vkNegotiateLoaderLayerInterfaceVersion`, instance and device creation (walks the loader's link info, fills the dispatch table), `GetInstanceProcAddr` / `GetDeviceProcAddr`, every hook. A hook forwards the call and, in probe mode, feeds the tracker. Pins the module so the pipe thread survives the loader unloading the layer |
| `layer/src/bootstrap.cpp` | Inject entry: loader notification, export table redirection of `vulkan-1.dll`, jump stubs |
| `layer/src/state.h` / `state.cpp` | Device registry; tracking of memory, buffers, pipelines, command buffers; application of buffer writes to shadows at submit time; frame assembly and statistics; the capture state machine; the probe requests and their JSON |
| `layer/src/resources.h` / `resources.cpp` | Descriptor layouts, sets, templates and immutable push contents; pipeline layout compatibility; images/views/samplers; shader bytecode retention and inspection |
| `layer/src/pipeline_recipe.h` / `pipeline_recipe.cpp` | Owned classic render-pass and graphics pipeline state; creation-call pointer views; explicit unsupported reasons and lossless export |
| `layer/src/spirv_repoint.h` / `spirv_repoint.cpp` | Narrow C++ implementation of D-022; move exactly two VS buffer bindings, reject ambiguity/collisions |
| `layer/src/pipeline_variant.h` / `pipeline_variant.cpp` | CPU eligibility, private compatible pipeline construction and direct indexed-draw restoration; only the sandbox invokes drawing |
| `layer/src/resource_hooks.h` | Resource lifetime/update hooks included inside `layer.cpp`; unimplemented descriptor paths invalidate stale tracked knowledge |
| `layer/src/ipc.cpp` | Named pipe server thread and request dispatch |
| `layer/src/util.cpp` / `common.h` | Configuration from environment variables, log file, CRC-32C (SSE4.2 with a table fallback) |
| `layer/src/json.h` | JSON writer and a reader for flat request objects |

### Hooks and dispatch

`EFVK_DEVICE_FUNCS` in `state.h` generates dispatch slots, hooks and counters. `EFVK_TRACKED_FUNCS` feed state or explicitly invalidate unsupported descriptor paths, including template lifetime and push commands. `EFVK_COUNTED_FUNCS` only count/forward pool creation, descriptor-buffer binding, push constants, render passes/framebuffers, image uploads and additional draw/dispatch forms. `CountedHook` generates these wrappers. Every hook increments its counter; `vkGetDeviceProcAddr` records requested names. Promoted/extension aliases have separate next-pointers. Device proc lookup offers hooks only when the next chain element implements them. Passthrough hooks only device destruction and present beyond the instance/device creation entry points.

A device is found from a dispatchable handle through its dispatch key (the first pointer of the handle); a single-device fast path avoids the registry lock.

### Tracked state

- **Memory** (`MemInfo`): size, type index, the current mapping (pointer, offset, size), the buffers bound to it.
- **Buffers** (`BufInfo`): id, size, usage, memory binding, a shadow byte array with an interval set of known bytes, a generation counter, a count of unknown writes.
- **Pipelines** (`PipeInfo`): id, stages, topology, vertex input, retained pipeline layout, and stage entry points with shared references to `ShaderInfo` SPIR-V. Destroying a module drops its live-handle entry; pipeline references keep its bytecode alive. Weak id lookup entries are pruned on destruction.
- **Descriptor layouts, sets and templates**: retained immutable layout definitions, pool ownership, allocation ids, variable counts and sparse element values. Template entries are copied at creation and decoded by offset/stride using the ordinary-write decoder. Destruction removes the decoder without erasing previous updates. Resource ids guard against handle reuse; unknown template/layout paths discard stale values.
- **Images, image/buffer views and samplers**: creation metadata and identities, removed at destruction. Image bytes and swapchain image creation are not tracked.
- **Command buffers** (`CmdBuf`): pipelines, IB, VB slots 0 to 3, and separate graphics/compute descriptor slots 0 to 3. Set binds retain layout definitions and dynamic ranges; layout compatibility can disturb previous bindings. Push updates create immutable `DescriptorSetInfo` contents, retained by each subsequent `DrawRec`; partial writes copy compatible previous values. Ordinary descriptor contents and resource metadata resolve at submit. Records also retain thread/CB ids, writes and render-pass counters. Executing secondary buffers copies records in execution order, rebases dynamic offsets and invalidates primary descriptor bindings. Reset releases push state and recorded draws. Additional shared references and push copies increase overhead; T-012 measured synthetic overhead; see docs/probe-performance.md.

Locks: `res_mx` (shared mutex) guards memory, buffers and pipelines; `cb_mx` guards only the map of command buffers, not their content (one command buffer is recorded by one thread at a time, and read by the submitting thread after recording ended); `frame_mx` guards the frame being assembled and the capture state. Order when nested: `frame_mx` then `res_mx`. The pipe thread holds the registry lock shared while it serves one request, except while it waits for a capture.

### Where buffer bytes come from

`read_buffer_locked` answers from the application's own mapping when the buffer's memory is mapped and the range lies inside the mapping (`mapped`), otherwise from the shadow when the range is known (`shadow`), otherwise not at all (`none`).

The shadow of a buffer is fed at submit time, in recording order, from the pending writes of the submitted command buffers: `vkCmdCopyBuffer(2)` (bytes read from the source at submit time, falling back to what was read when the copy was recorded), `vkCmdUpdateBuffer`, `vkCmdFillBuffer`. Only buffers whose usage intersects `shadow_usage` (default: index buffers) get a shadow. When memory is unmapped, every `TRANSFER_SRC` buffer in it of at most 16 MiB is snapshotted into its own shadow first, so a copy recorded after the unmap still finds its source. A write whose bytes cannot be obtained removes the range from the known set and is counted as an unknown write.

Unmap, free and destroy take `res_mx` exclusively before the call goes down, so a reader never touches a mapping that is going away.

### Frames and capture

`vkQueueSubmit(2)` adds the submitted command buffers to the current frame's statistics. `vkQueuePresentKHR` closes the frame. A capture request arms the state machine; the frame that starts at the next present is recorded in full and published at the present after it. For each draw of a captured frame the handles are resolved to ids and, for a direct indexed draw, the CRC-32C of the index range (`bind offset + index size * first index`, `index size * index count` bytes) is computed.

During an armed capture, descriptor contents are serialized into immutable JSON under `res_mx` at submit time. Ordinary set values resolve then; pushes use the immutable values retained during recording. `snapshot` distinguishes these timings, while `resource_snapshot: submit` describes resource identities/visibility and image metadata in both cases. A recorded buffer handle/range is not evidence of record-time byte readability or later GPU consumption. Unknown/truncated states are explicit. Slots remain limited to 0 through 3 and each set emits at most 256 stored elements per draw.

## Probe protocol

Pipe `\\.\pipe\efmi-vk-<PID>`, one client at a time, started when the first device is created in probe mode. The security descriptor gives system and administrators full control and the interactive user read and write, so that a client without elevation can talk to an elevated process. A message in either direction is `[u32 header_len][u32 payload_len][header JSON][payload]`. Requests are flat JSON objects with `cmd`; replies are `{"ok":true,"result":...}` or `{"ok":false,"error":"..."}`.

| `cmd` | Arguments | Result |
| --- | --- | --- |
| `status` | | Version, protocol, pid, exe, mode, entry, `attach` (was `vulkan-1.dll` found, was it already loaded, exports redirected, instances and devices seen at creation, passive calls), device, frame counter, statistics of the last frame, maxima, counts |
| `capture` | `timeout_ms`, `draws`, `descriptor_budget`; `snapshot`, `snapshot_all`, `snapshot_hashes`, `snapshot_max_range`, `snapshot_budget`, `follow` and `follow_*` | Statistics of one complete frame and, unless `draws` is 0, every draw. With `snapshot`, the selected draws list what they read from buffers at submit, compared with a read at record time |
| `snapshot` | `seq`, `id` | The kept bytes of one range of the latest capture as payload |
| `set` | `unmap_snapshot_all`, `unmap_snapshot_limit` (both optional) | Changes these two settings in the running process, under the resource lock; returns their values. The limit is clamped to 256 MiB |
| `buffers` | `usage`, `min_size`, `limit` | Tracked buffers |
| `read_buffer` | `id`, `offset`, `size` | Source and generation in the header, the bytes as payload (at most 64 MiB) |
| `hash` | `id`, `offset`, `size` | CRC-32C of the range |
| `pipeline` | `id` | Vertex input, topology, layout id, stages and retained shader ids/hashes/entry points |
| `shader` | `id`, `code` | SPIR-V size and CRC-32C; binary payload if `code` is nonzero (at most 64 MiB) |
| `api_usage` | | Call count of every intercepted function that was called, the names the application resolved, the enabled device extensions |

## Settings of the DLL

Each setting is read from an environment variable and, when that is not set, from `efmi_vk.cfg` next to the DLL (`key = value` lines, `#` starts a comment). The file exists because a process started by a launcher cannot easily be given environment variables.

| Variable | Key in the file | Meaning |
| --- | --- | --- |
| `EFMI_VK_MODE` | `mode` | `probe` (default) or `passthrough` |
| `EFMI_VK_ENTRY` | `entry` | `layer` or `inject`; default from the file name |
| `EFMI_VK_LOG` | `log` | Log file; default `efmi_vk_layer.log` next to the DLL |
| `EFMI_VK_SHADOW_USAGE` | `shadow_usage` | Hex `VkBufferUsageFlags` mask of buffers that get a shadow |
| `EFMI_VK_UNMAP_SNAPSHOT_LIMIT` | `unmap_snapshot_limit` | Bytes; 0 disables the snapshot at unmap. Can be changed in a running process with `set` |
| `EFMI_VK_UNMAP_SNAPSHOT_ALL` | `unmap_snapshot_all` | Default on: every whole transfer source within the limit is copied at unmap. `0` or `off` restricts the copy to transfer-only (staging) buffers; that was the default of 0.7.4 only. Can be changed with `set` |
| `EFMI_VK_RESOURCES` | `resources` | `0` or `off`: reduced tracking. Draws, buffers, hashes and call counters stay; descriptor, image and shader metadata are not kept |
| `EFMI_VK_SHADER_BUDGET` | `shader_budget` | Bytes of SPIR-V kept in total (default 512 MiB); past it a module keeps hash and size only |

Bounds that keep the probe's cost independent of the size of the observed process (0.5.1): the SPIR-V budget above, with the kept total given back when the last reference to a module dies; module and pipeline destruction look only at the shader ids involved (`forget_shaders_locked`), not at every retained shader; descriptor sets are indexed by pool (`Device::pool_sets`), so a pool reset touches only its own sets; a capture keeps descriptor contents only up to the `descriptor_budget` of the request (default 64 MiB of JSON, 0 for none) and marks the rest with the reason `capture_descriptor_budget`. The status reports `resources`, `shader_budget`, `counts.retained_shader_bytes` and `counts.shaders_not_retained`; a capture reports `descriptor_budget`, `descriptor_bytes` and `descriptors_omitted`.

In the inject entry the pipe is started when the DLL is loaded, not at device creation, so that the status can be read even when the DLL came too late to see a device.

## Server modules

| File | Responsibility |
| --- | --- |
| `server/efmi_vk/probe_client.py` | Finds probe pipes, sends requests, raises `ProbeError` |
| `server/efmi_vk/hashes.py` | CRC-32C in Python and the index region rule |
| `server/efmi_vk/mods.py` | Reads the TextureOverride sections of mod INIs (hash, `match_first_index`, `match_index_count`) and matches them against captured draws |
| `server/efmi_vk/tools.py` | `Session` (pipe connection, last capture) and eleven tools; `ToolError` |
| `server/efmi_vk/spirv.py` | Limited declared-name and direct set/binding metadata; `buffer_variables` (uniform and storage buffer variables with the references to each) and `repoint` (move variables to another set and binding by rewriting two decoration literals each), the reference for the pipeline variant of D-022; not full reflection |
| `scripts/stage_usage.py` | For a bundle taken with a mod: which stage of each program reads the bone buffer and the instance window; with `--repoint`, rewrites every vertex shader as the variant would and checks it with `spirv-val` |
| `server/efmi_vk/evidence.py`, `scripts/collect_probe.py` | Explicit-target raw frame/counter/match export, bounded retained shaders, file fingerprints and query-gap reporting; with `--snapshot`, the kept buffer contents as `snapshots.bin` and `snapshots.json` |
| `server/efmi_vk/dx11_pair.py`, `scripts/dx11_baseline.py` | A DX11 frame taken through the sibling project's probe (its client is used as a library, read only) and its pairing with a Vulkan bundle: objects by region hash and by the mod's index ranges, draws by order of use, resources by shape, size, mip count and format. Writes `<label>.summary.json`; results in `docs/dx11-baseline.md` |
| `server/efmi_vk/mcp_stdio.py` | JSON-RPC 2.0 over stdio: `initialize`, `ping`, `tools/list`, `tools/call` |
| `server/run_server.py`, `server/efvk.py` | Entry points: MCP server, command line |

## Sandbox

`sandbox/src/main.cpp` loads Vulkan at run time and opens a 640 x 360 secondary-screen window without focus. It draws five indexed meshes: three from device-local 16-bit IB A uploaded through staging, two from mapped 32-bit IB B at byte offset 16. Each draw binds set 0 (dynamic UBO) and set 1 (dynamic storage array, immutable sampler, sampled 1x1 image), alternating multi-set and separate calls. Descriptors are copied from a set that is freed; shader modules are destroyed before drawing. The trigger rewrites six indices through an unmapped staging buffer and updates one storage descriptor. `expected.json` contains independent CRC-32C fingerprints of the index regions and SPIR-V. `--threads N` records on N secondary-buffer threads; the three inject options still exercise early, late and late-with-export-resolution loading.

`--descriptors template` uses padded application data and destroys the initial template before drawing; a second template performs the rewrite. `push` and `push-template` replace material set 1 with non-dynamic storage descriptors whose base offsets vary per draw, using KHR push commands. Direct pushes separate buffer and image writes to exercise incremental updates. These modes are tested with primary/secondary command buffers through both entries. The standard suite requires push-descriptor support and does not silently skip those cases.

`sandbox/src/spirv.h` assembles the vertex and fragment shader word by word, because there is no shader compiler on the machine.

## Build and verification entry points

- `scripts/build.py [layer] [sandbox] [--out DIR]`: finds `vcvars64.bat` and the Vulkan headers, compiles with `cl`, writes the layer manifest.
- `scripts/fetch_vulkan_headers.ps1`: clones the pinned header tag into `F:\vulkan\Vulkan-Headers`.
- `sandbox/run_all.py [--no-build]`: build, `scripts/check_text.py`, `unit_test.py` (CRC vectors, INI parser, override matching, MCP message handling), `probe_test.py` (sandbox with the layer entry and 0, 2 and 5 recording threads, and with the inject entry and 0 and 2 threads: statistics, every draw, hashes before and after the rewrite, buffer read-back, pipeline, reconnect, attach information), `modes_test.py` (no layer: no pipe; passthrough in both entries: no pipe, frame count agrees; late injection: the sandbox is unaffected and no device is tracked; late injection with the functions resolved again through the exports: calls go through the passive device; settings taken from `efmi_vk.cfg`), `mcp_test.py` (the MCP server as a child process against the sandbox, every tool).
- `sandbox/harness.py`: `Sandbox` context manager with the `entry` argument; `layer_env` enables the layer for a child process through `VK_LAYER_PATH` and `VK_INSTANCE_LAYERS`; `inject_env` sets no loader variable and lets the sandbox load the DLL itself. Both switch other implicit layers off with `VK_LOADER_LAYERS_DISABLE=~implicit~`.
- `sandbox/tracker_test.cpp` / `native_test.py`: offline C++ tests of descriptor semantics and resource lifetimes; compiled alongside the layer and included in `run_all.py`. They make no Vulkan calls. All Python test scratch artifacts stay inside `build/tests/`; compiler scratch stays in component `build/tmp/` directories.
- `scripts/verify_region_hash.py <dump>`: region hash rule against an XXMI FrameAnalysis dump.

`dist/`, the `build/` directories, `captures/` and logs are ignored by Git. A build on disk does not mean a running process has loaded it.

## Offline readiness

Many-draw mode cycles the five meshes and descriptor ranges. QPC records host wait/record/submit/present phases with warmup exclusion. Core/synchronization validation and negative canaries are part of the suite. VVL and its dependencies are built under F:/vulkan; child caches stay in build/tests. See docs/validation.md.

scripts/inventory_compat.py preserves lexical section order/duplicates and emits fingerprints and line references. It does not evaluate INIs. D-016 retains a hosted XXMI frontend behind explicit state/resource/input adapters, with native Core-VK operations. The standalone CPU prototype now compiles/links pinned 1.2.2 and passes 23 checks (docs/xxmi-host-prototype.md); it is not linked into the layer. Reference D3D-shaped headers remain compile scaffolding, not the production resource adapter. Resource/Core execution, persist/reload and full mod compatibility remain open.

frontend/xxmi/draw_plan mediates parsed backend commands while retaining original expression/branch/named-call execution. It emits ordered resource/draw operations and owned per-draw layout requests; native Core boundaries retain phase bits as unresolved operations. It validates supported expression operands before evaluation, never forwards arbitrary commands to the reference graphics context, and rejects incomplete output atomically. Resource pointers borrow from the frontend generation and variable effects are not transactional. This standalone adapter passes 28 native checks plus 384 actual sample callback plans/file-range checks; it is not in the layer. See docs/frontend-draw-plans.md.

frontend/assets/dds decodes bounded single-layer 2D metadata into VkFormat and mip byte ranges without transforming/copying payloads. Fifteen native checks and all ten actually selected sample DDS files pass; this is not a GPU uploader. Selected assets total about 301 MiB, including four 8K textures. Prepare resources outside the draw path and load on actual use; unused declarations may name absent files. Queue/fence/device-format checks and resource lifetimes remain to be connected (docs/resource-assets.md).

frontend/assets/image_upload is an owning GPU ticket: off-draw image/staging preparation and command recording, submit under caller queue serialization, and nonwaiting completion polling before public image/view handles exist. Staging ends at upload completion; image ownership extends through consuming commands. TextureCache adds a single worker, dormant/deduplicated declarations, sticky demand failures and one upload at a time. Actual-allocation ledgers are shared across generations; old retained GPU owners stay charged. Worker never submits; caller never reads/allocates/records/polls. Headless checks pass 57 plus five actual-cache checks and all ten DDS payload readbacks under clean core/sync validation. No layer hook, cross-queue synchronization, implicit feature enablement or game-overhead claim (docs/resource-assets.md).

Private material-image prototype (D-034): frontend/gpu/fragment_images rewrites only explicitly requested FS image set/binding literals. Opaque-use tracing rejects depth comparison/escapes while allowing the game's Depth=Unknown declarations for proven noncomparison paths. The standalone GPU fixture keeps VS image reads and original next draws intact after replacing FS color. Actual descriptor/pipeline integration is still absent. Original-resource correspondence and replacement type compatibility are separate; LIMIT-021 identifies three 3D targets for sample 2D files. See docs/fragment-materials.md.

The same analysis implements explicit null sampled 2D/3D images by replacing supported numeric image results with typed zero and removing opaque accesses. It does not substitute a dummy texture, infer null from dimension mismatches, or modify other images. Mixed opaque selections/escapes and unsupported queries refuse. Pixel and historical bytecode evidence are in docs/fragment-materials.md.

0.9.3 integrates these material contracts into PipelineVariant and VariantCache. Ordered source image bindings map to private set 3, bindings 2+i; explicit nulls consume no descriptor. The cache identity includes this contract separately from vertex declarations, never live image handles. Private layouts preserve original sets/push ranges and check added sampled-image limits against retained original layouts. DrawSnapshots use the same private layout builder, preallocate image owner slots and retain each Ready view owner through sealing/resubmission. Recycling requires a legal completed command reset, as before. The M2 component hook supplies an empty material contract; hosted frontend/asset-cache production integration remains open.

BufferUpload is the standalone immutable vertex/index counterpart of ImageUpload: bounded exact bytes, device-local destination, host-coherent staging, worker-side preparation/recording, caller-serialized single-queue submission and fence-gated publication. Actual allocation ledgers remain charged through Ready ownership. Stride, format, range checks and declaration interpretation belong to the frontend; this uploader does no per-component transformation. Common file scheduling and layer/frontend integration are still open.

ResourceCache now owns the common file scheduler for both upload ticket types. Keys include lexical absolute path and buffer usage (zero selects image decode); each Ready snapshot contains the typed owner for its kind. One worker and one outstanding upload share actual source/staging/resident budgets across all assets and retained generations. texture_cache.h preserves old texture type aliases. Headless material rendering combines cached mesh and images after cache retirement; hosted frontend/layer integration remains open.

## Contents kept by a capture (0.6.0)

`SnapRequest` travels with the armed capture. From the present that starts the captured frame until the one that ends it, `Device::snap_recording` is set and `on_record_indexed` (called by the `vkCmdDrawIndexed` hook) hashes the draw's index range, and for a selected draw reads its uniform ranges, the entry and the follow windows, keeping a checksum per range in `CmdBuf::rec_snaps`; secondaries hand them to the primary in `on_execute_commands`. At submit `capture_draws` reads the same ranges again through the same function (`read_snapshot_locked`), stores the bytes once per distinct (buffer, offset, size) in `Capture::blobs` up to the request's budget and sets the comparison state of each item. `collect_buffer_ranges_locked` in `resources.cpp` resolves a bound set to buffer ranges with the dynamic offsets applied. Outside such a frame the draw path costs one relaxed atomic load.

## Image content and pipeline creation (0.7.0)

Follow-up in 0.7.1: state.h owns a fixed PresentPacing ring (pacing.h). on_present adds QPC host-boundary intervals under frame_mx, with upload-timer deltas and capture markers. probe_status copies it, then computes quantiles and formats host_pacing outside that lock. See docs/frame-pacing.md. It neither reads GPU timestamps nor measures actual displayed frames.

0.7.2: work.h supplies the shared QPC clock and opt-in TrackerWork counters plus a fixed 64-event slow-operation ring per device. WorkScope wraps buffer unmap, copy recording, shadow submit, submit assembly, command reset and draw/dispatch recording, excluding forwarded driver calls. profile_tracker defaults off. status.tracker_work is formatted outside its history lock, and host_pacing peaks include host_end_us for correlation. Nested/concurrent wall-time spans are not additive or proof of a presentation stall. Buffer copies now borrow CPU-readable ranges under res_mx, allocating record-time fallbacks only for readable sources and copying directly into destination shadows at submit. See docs/tracker-work.md.

0.7.3: MemInfo stores a WC page-policy flag for its current mapping, classified once by VirtualQuery over the complete range. util.cpp::copy_buffer_bytes selects SSE4.1 streaming reads only for flagged mappings and requests of at least 64 bytes; cached/unknown mappings and CPU shadows retain memmove. The helper fences streaming reads and never rounds outside the requested bytes. It is used by unmap snapshots, record fallback, submit writes and read_buffer_locked. Remap reclassifies and unmap clears the flag. Snapshot limits, visibility semantics and resource-lock ordering are unchanged. Status exposes mapped_read; buffer metadata adds mapped_write_combined. Local and game evidence boundaries are recorded in docs/pacing-tracker-session.md (D-025).

`vkCmdCopyBufferToImage` and its two `2` forms are tracked functions now. A copy to mip 0, layer 0 is noted in `CmdBuf::image_writes` (secondaries hand theirs to the primary) and handled in `on_submit` by `apply_image_writes` (`resources.cpp`): with `res_mx` held shared, the source bytes of a copy that covers the whole level are read through `read_buffer_locked` and hashed row by row without padding (`format_block` gives the texel block of a format); then `res_mx` is taken exclusively for the short time it takes to store state, CRC, size and upload count in `ImageInfo`. `write_element` reports them as `content` of a sampled image. Counters per outcome, bytes and microseconds are atomics of the device. Settings: `image_hash` / `EFMI_VK_IMAGE_HASH`, `image_hash_limit` / `EFMI_VK_IMAGE_HASH_LIMIT`.

`note_pipeline_creation_locked` (`state.cpp`) counts into `Device::pipeline_uses` what each graphics pipeline creation uses and keeps flags, subpass, dynamic states and the presence of specialization in `PipeInfo`. It reads only state that has to be valid; depth-stencil and colour blend state are counted by presence. `api_usage` reports the counters, `pipeline` the per-pipeline part together with the ids of the set layouts and the push constant ranges of the layout.

## What is copied at unmap (0.7.4)

`on_unmap_memory` walks the buffers of the memory object. A buffer wholly inside the mapped range, at most `unmap_snapshot_limit` bytes, with the transfer-source bit is copied into its shadow (0.7.5; with `unmap_snapshot_all` off only if it is a transfer buffer and nothing else, which was the default of 0.7.4). Every other buffer the mapping touches has its shadow knowledge dropped, because the application could have written it unseen. Counters `unmap_taken`, `unmap_passed`, `unmap_invalidated` and their bytes are atomics of the device and appear as `counts.unmap`. History, including the wrong premise of 0.7.4: `docs/unmap-snapshots.md`, D-027.

FileDraws resolves the ordered file plan into per-draw VB/IB/PS ownership, layout and argument snapshots. Application-inherited, explicit-null and file bindings remain distinct. File IB ranges plus signed bases are checked against typed whole-file extrema captured at upload preparation; all sample draws pass, but valid subranges can be conservatively refused. Unresolved native boundaries and any late invalid draw reject the whole output. No commands are emitted, and the layer must still validate the application pipeline, input rates and untouched bindings. See docs/frontend-draw-plans.md.

The hosted command mediator executes original scalar pool/reference/callback operations and snapshots custom resource references without D3D objects. It refuses expiring resource aliases, unknown GPU queries and cyclic references; variable writes conservatively discard resource aliases in a mixed pool. The skeleton metadata adapter reads evaluated original pools, owns remap paths, preserves empty component ids and distinguishes unassigned LODs from explicit identity. Native GPU initialization, spatial identity and shared-master lifetime remain separate adapters. See docs/core-skeleton.md.

Original spatial identification stays in the hosted INI/state machine. The explicit resource-query adapter reads SpatialHash from borrowed tracked CPU windows and uses pinned original coordinate packing; pool matching/expiration are original code. No query falls through to D3D scaffolding. Reserved hash sentinels and invalid inputs refuse. Palette identity epochs and completeness still require their retained state adapter. Scope is LOD0/LOD1 only; keep Core pool stride as data without resolving later-LOD assets.

PaletteBank owns complete CPU masters by explicit identity epochs, independent of reusable original spatial slots. Prepared PaletteDefinitions own exact LOD0/LOD1 remaps from evaluated original INI pools. Complete per-pose snapshots compact one identity to slot zero and remain immutable after master updates or retirement. Missing component data refuses publication. Epoch assignment, native Core integration and GPU snapshot handoff remain open; see docs/core-skeleton.md.

SpatialPalettes couples PaletteBank to observed original Core spatial results. It retains tokens only across observed original proximity/expiration continuity and retires masters on expiry, distant slot reuse or explicit invalidation. It never selects an original pool slot. Its owner must observe every successful identification and invalidate after failed execution or pool reset; see docs/core-skeleton.md.

DrawPlanAdapter now supports explicit native handlers and bounded nested original callback invocation with sticky transaction failure. Optional immutable PaletteSnapshot owners travel through indexed PlanItem, FilePlanItem and resolved FileDraw without inheriting across draws. Missing native handlers remain External and block file-draw execution. EFMI Core builtin implementation and DLL linkage are still separate integration work.


CoreComponentBridge connects one prepared original skeleton to bounded CPU
rolling-buffer inputs. It keeps original texture-override matching, spatial
state, iteration and callbacks; native operations supply evaluated config,
skeleton attachment/source selection and owned complete palettes. A post-original
spatial observer imports providers even during first-frame skips. Plans retain
one compact immutable palette per draw; no component-specific rendering path.
Pre/post roots execute atomically with a separate native stack budget. Material
OverrideTextures, complete profile resolution and DLL emission remain open.
Historical two-update replay is synthetic CPU evidence, not game animation.


frontend/gpu/material_plan compiles explicit logical pixel-slot correspondence
against exact retained fragment code, independent of file readiness. It separates
unknown/absent/image slots and emits ordered FragmentPatch plus retained
SnapshotImage owners for each FileDraw. Entire transactions refuse on a late
unsupported mapping or missing Ready owner. The caller proves source API
correspondence; shared image identity across passes is not that proof. No implicit
dimension-mismatch-to-null rule is implemented. DLL integration remains open.


VertexProfile connects owned FileDraw layout/stride metadata to shared
patch_vertex_streams using exact-shader semantic correspondence and the original
XXMI layout evaluator exposed by command_module.cpp. Original source files stay
untouched. Correspondence preparation and CPU declaration resolution are separate
from GPU readiness/variant creation. Default application bindings survive;
unsupported semantic changes, rates or aliased edits refuse the transaction.

frontend/gpu/draw_transaction joins resolved frontend owners to the existing
shared variant cache and emitter. Complete Ready buffer bounds, vertex strides,
per-draw material owners and compact palettes are required before publication.
Inherited advancing vertex streams currently refuse because new indices have no
proved bound for those application buffers; stride-zero defaults remain intact.
Variant demand never waits. Caller-allocated snapshot arenas stay immutable after
sealing, while RecordedTransaction retains mesh, images, palettes, variants and
arenas through GPU completion and legal resubmission. Pending/refused preparation
publishes nothing. A late append failure can consume unused arena slots but emits
no commands and preserves the previous recording. Application objects and queue/
command synchronization remain the caller's responsibility; no live hook uses
this frontend bridge yet.

DrawPlanAdapter mediates CheckTextureOverride and scalar pixel filters against
an explicit source-API identity provider. The original resource hash matcher,
draw filters, range resolver and command executor are reused. The adapter tracks
per-plan file/null pixel bindings so later queries observe previous replacements;
unknown source identities and descriptor fuzzy matching refuse. It preserves
negative-zero unbound filter results, selected filter priority, phase ordering
and nested this restoration. All returned original callback lists must already
belong to the same adapted generation. Register the provider before installing
CoreComponentBridge; its instance boundaries() then includes or excludes the
unresolved texture boundary consistently. Current historical fixtures supply no
source identity provider and continue to record that unresolved work explicitly.

## Host and module (1.0.1)

The injected DLL is the host: hooks, tracker, probe pipe, captures, and a built-in copy of the replacement code. The replacement code (character backend, package, materials, sibling rules, merged skeleton, pipeline variants and cache, draw snapshots, texture upload) is also built alone as `efmi_vk_backend.dll`. The host calls it through `CharacterModule` (layer/src/character_module.h) under `Device::character_mx` and can take another copy of it while the process runs: staged, served by the submit hook, promoted at a frame's end, the old one destroyed off the draw path. `EFVK_ABI` stamps the shared headers; see docs/hot-swap.md and D-042.
