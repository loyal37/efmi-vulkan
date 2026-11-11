// A matched draw recorded as the Mod's (CharacterGeneration's part): its lists' plan with the scope of
// their owned work, its variants, snapshots and emission, the translucent captures, the application's
// draw made again with the Mod's textures, and the owned work submitted before the application's.
#include "character_backend_internal.h"

namespace efvk {
namespace {
// This backend's owner in a recording, as owner_for, except that another backend's owner stays while
// that backend keeps using the recording: every reset clears `used`, so dropping the others' unused
// owners at the first draw destroyed their arenas and had their workers make new ones for every
// recording (D-099, arcane 142 us per draw in the 1.6.8 session). One goes after kIdleRecordings
// resets without use (its generation was exchanged), or when it is not this code's.
constexpr uint32_t kIdleRecordings = 8;
template <class Make>
std::shared_ptr<CommandOwner> character_owner_for(std::vector<std::shared_ptr<CommandOwner>>& owners, const void* backend, Make make)
{
    std::shared_ptr<CommandOwner> mine;
    for (auto it = owners.begin(); it != owners.end();) {
        if (*it && (*it)->backend == backend) { mine = *it; ++it; continue; }
        const auto* other = *it ? dynamic_cast<const CharacterCommand*>(it->get()) : nullptr;
        if (!*it || (other && !other->used && other->idle_recordings >= kIdleRecordings)) it = owners.erase(it);
        else ++it;
    }
    if (!mine && (mine = make())) owners.push_back(mine);
    return mine;
}
} // namespace

// One captured call (D-113): its opacity set and parameter layer, the game's sets and dynamic
// state of the draw (`own`: the pipeline's sets below the private one), queued after the list.
// Caller holds the mod lock.
bool CharacterGeneration::capture(CmdBuf* cb, const DrawRec& r, uint32_t own, const CharacterDraw& d, uint32_t i, const SnapshotSlot& colour,
                                  const DrawGeometry& geometry, const IndexedDraw& args, const AppendBones& append_bones, CharacterCommand* owner)
{
    uint32_t layer = 0;
    if (!translucency->reserve(cb, layer)) return refuse(C_STATE, "more translucent calls in one pass than the layers' parameters hold");
    std::vector<SnapshotImage> views{capture_masks[i].view ? capture_masks[i] : Translucency::neutral_view(capture_layers),
                                     Translucency::params_view(capture_layers, layer)};
    if (!views[0].view || !views[1].view) return refuse(C_IMAGE, "translucency layers missing");
    auto arena = owner->material_snapshots.find(2);
    if (arena == owner->material_snapshots.end()) return refuse(C_EXHAUSTED, "no two-image arena for the translucent opacity");
    TranslucentCapture c;
    if (!append_bones(*arena->second, views, c.slots[1])) return refuse(C_EXHAUSTED, arena->second->reason);
    c.slots[0] = colour;
    for (uint32_t k = 0; k < kCaptureKinds; ++k) { c.variants[k] = scratch_captures[k][i]; owner->variants.push_back(c.variants[k]); }
    c.geometry = geometry; c.draw = args;
    for (uint32_t s = 0; s < own && s < kMaxSets; ++s) {
        const SetBind& b = r.sets[s];
        TranslucentCapture::Bound bound;
        if (b.set && !b.pushed && !b.unsupported) {
            if (uint64_t(b.dyn_first) + b.dyn_count > cb->dyn.size()) return refuse(C_ENTRY, "translucent capture: dynamic offsets incomplete");
            bound.layout = b.layout_handle; bound.set = b.set;
            bound.dyn.assign(cb->dyn.begin() + b.dyn_first, cb->dyn.begin() + b.dyn_first + b.dyn_count);
        } else if (b.pushed || b.unsupported) return refuse(C_SET3, "translucent capture: a set that cannot be bound again");
        c.sets.push_back(std::move(bound));
    }
    const VkRect2D area = cb->pass.area;
    c.viewport = cb->has_viewport ? cb->viewport : VkViewport{float(area.offset.x), float(area.offset.y), float(area.extent.width), float(area.extent.height), 0, 1};
    c.scissor = cb->has_scissor ? cb->scissor : area;
    c.params[0] = d.alpha; c.params[1] = d.mask_image != kCharacterNone ? 1.0f : 0.0f;
    c.params[2] = float(d.mask_channel); c.params[3] = d.mask_invert ? 1.0f : 0.0f;
    c.params_layer = layer; c.layers = capture_layers;
    scratch_pending_captures.push_back(std::move(c));
    return true;
}

// Which of the library's shader groups the pipeline's vertex and pixel shaders stand for (shader_hooks.h).
const CharacterGeneration::LibraryHooks& CharacterGeneration::library_hooks(const PipeInfo& pipe)
{
    const auto found = library_hook_facts.find(pipe.id);
    if (found != library_hook_facts.end()) return found->second;
    LibraryHooks hooks;
    if (pipe.recipe)
        for (const auto& stage : pipe.recipe->stages) {
            if (!stage.shader) continue;
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) hooks.vertex = character_vertex_hook(stage.shader->code);
            else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                const PixelHook pixel = character_pixel_hook(stage.shader->code);
                hooks.pixel = pixel.known; hooks.colour_slot = pixel.colour_slot; hooks.narrow_first = pixel.narrow_first;
            }
        }
    if (library_hook_facts.size() >= 8192) library_hook_facts.clear();
    return library_hook_facts.emplace(pipe.id, hooks).first->second;
}

// `vbN->stride` of a replaced draw (RuntimeDraw::vertex_stride): the pipeline's binding N, as DX11's
// input slot N the layer takes it elsewhere too (the blend-index stream at vb2); none bound is 0.
bool CharacterGeneration::vertex_stride(const PipeInfo& pipe, unsigned slot, uint32_t& stride, std::string& why)
{
    if (std::find(pipe.dynamic_states.begin(), pipe.dynamic_states.end(), uint32_t(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)) != pipe.dynamic_states.end()) {
        why = "the pipeline sets its vertex strides at recording"; return false;
    }
    stride = 0;
    for (const auto& b : pipe.bindings) if (b.binding == slot) { stride = b.stride; break; }
    return true;
}

// A replaced draw's plan with a scope for its owned GPU work (P2, D-087): recorded without a
// queue, pending until submit_draw_work. Caller holds mod_mx.
bool CharacterGeneration::plan_draw(const efmi::frontend::RuntimeDraw& request, CharacterList& output, bool& skip, bool& added, std::string& reason,
                                    const VertexInputRecipe* vertex, bool* textures)
{
    efmi::frontend::OwnedRuntime* gpu = nullptr;
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
    auto began = std::chrono::steady_clock::now();
    auto phase = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        program->note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - began).count())); began = now;
    };
#else
    auto phase = [](const char*) {};
#endif
    // A folder generation's packages share its INI generation and owned runtime: one plan at a time.
    std::unique_lock<std::mutex> shared;
    if (plan_mx) shared = std::unique_lock<std::mutex>(*plan_mx);
    if (presenter && !draw_work_failed && program->gpu() && !program->gpu()->scope_active()) {
        try { program->gpu()->begin_draw(); gpu = program->gpu().get(); } catch (const std::exception&) {}
    }
    phase("phase gpu begin");
    const bool planned = program->plan(request, package, output, skip, added, reason, vertex, textures);
    phase("phase program plan (whole)");
    if (gpu && gpu->scope_active()) { try { gpu->suspend(); } catch (const std::exception&) {} }
    if (gpu && gpu->pending()) draw_work_waiting.store(true, std::memory_order_release);
    phase("phase gpu suspend");
    return planned;
}

// Before an application submission on a graphics queue: the owned work replaced draws recorded,
// on the queue the generation's owned work belongs to (its first submission fixes it).
void CharacterGeneration::submit_draw_work(VkQueue queue)
{
    if (!draw_work_waiting.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mod_mx);
    if (!presenter || !program || !program->gpu() || draw_work_failed || !program->gpu()->pending()) {
        draw_work_waiting.store(false, std::memory_order_relaxed); return;
    }
    if (std::find(device->material_queues.begin(), device->material_queues.end(), queue) == device->material_queues.end()) return;
    const VkQueue bound = program->gpu()->queue();
    if (bound && bound != queue) return;
    std::string reason;
    std::unique_lock<std::mutex> shared;
    if (plan_mx) shared = std::unique_lock<std::mutex>(*plan_mx);
    if (!presenter->submit_pending(queue, reason)) {
        draw_work_failed = true; note(reason);
        logf("owned GPU work of replaced draws stopped: %s", reason.c_str());
    }
    draw_work_waiting.store(!draw_work_failed && program->gpu()->pending(), std::memory_order_relaxed);
}

// The draw's render targets for `@oN` (D-087), from its pipeline's subpass, and its shader stages
// for `vs` and `ps` (D-089).
void CharacterGeneration::render_targets(const PipeInfo& pipe, efmi::frontend::RuntimeDraw& request)
{
    // The vertex slots the pipeline reads (D-161): a buffer the lists bind at another slot is nothing to the
    // draw. Not known where the vertex input is set at recording, or where the pipeline states none.
    if (!pipe.bindings.empty() &&
        std::find(pipe.dynamic_states.begin(), pipe.dynamic_states.end(), uint32_t(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) == pipe.dynamic_states.end()) {
        request.vertex_slots = 0;
        for (const auto& b : pipe.bindings) if (b.binding < 32) request.vertex_slots |= 1u << b.binding;
    }
    if (!pipe.recipe) return;
    request.shader_stages = 0;
    for (const auto& stage : pipe.recipe->stages) request.shader_stages |= uint32_t(stage.stage);
    if (!pipe.recipe->render_pass || pipe.recipe->subpass >= pipe.recipe->render_pass->subpasses.size()) return;
    const auto& sub = pipe.recipe->render_pass->subpasses[pipe.recipe->subpass];
    uint32_t mask = 0;
    for (size_t i = 0; i < sub.colors.size() && i < 8; ++i)
        if (sub.colors[i].attachment != VK_ATTACHMENT_UNUSED) mask |= 1u << i;
    if (sub.has_depth && sub.depth.attachment != VK_ATTACHMENT_UNUSED) mask |= 1u << 8;
    request.targets_known = true; request.render_targets = mask;
    request.render_pass = uint64_t(pipe.recipe->render_pass->id) << 8 | pipe.recipe->subpass;
}

bool CharacterGeneration::record(CmdBuf* cb, const DrawRec& r, uint32_t section_index)
{
    if (!enabled.load(std::memory_order_relaxed) || !cache) return false;
    bool gpu_draw = false; // this draw binds the device's merged skeleton (D-117) rather than the CPU master
    CharacterCommand* bone_owner = nullptr; // the recording's owner, which carries the device imports (D-127)
    uint64_t borrowed_sources = 0; // components whose import this recording takes from one not yet submitted (D-127)

    ThreadTrace* const trace = device->trace ? device->trace() : nullptr;
    const uint64_t entered_us = host_time_us();
    StageMark locking(trace, "character draw (generation lock)");
    std::lock_guard<std::mutex> mod_lock(mod_mx);
    ScratchScope scratch_scope{*this};
    StageMark stage(trace, "character draw (plan, variants, recording)");
    uint64_t mark_us = host_time_us();
    stage_us[S_LOCK] += mark_us - entered_us;
    auto lap = [&](Stage s) { const uint64_t now_us = host_time_us(); stage_us[s] += now_us - mark_us; mark_us = now_us; };
    const uint64_t present = frame.load(std::memory_order_relaxed);
    InstanceEntry entry, patched;
    const CharacterList* list = nullptr;
    CharacterList runtime_list;
    Identity* identity = nullptr;
    auto& variants = scratch_variants; variants.clear();
    auto& material_plans = scratch_material_plans; material_plans.clear();
    auto& material_views = scratch_material_views;
    uint32_t material_origin = 0;
    DrawGeometry& original = scratch_original;
    original.first_binding = 0; original.buffers.clear(); original.offsets.clear();
    original.index = VK_NULL_HANDLE; original.index_offset = 0; original.index_type = VK_INDEX_TYPE_UINT32;
    struct LookupsReset {
        DrawLookups& l;
        ~LookupsReset() { l.pipeline = VK_NULL_HANDLE; l.pipe = nullptr; l.registers = nullptr; l.set = VK_NULL_HANDLE; l.set_id = 0;
                          l.contents = nullptr; l.answered[0] = l.answered[1] = 0; }
    } lookups_reset{lookups};
    // The application's own set 3, when it has one bound: the replacement uses that slot, so it is
    // bound again afterwards exactly as the application bound it.
    uint32_t restore_slot = 0;
    VkPipelineLayout restore_layout = VK_NULL_HANDLE;
    VkDescriptorSet restore_set = VK_NULL_HANDLE;
    auto& restore_offsets = scratch_restore_offsets; restore_offsets.clear();
    {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        const CharacterSection* const section = &package.sections[section_index];
        if (!activated.load(std::memory_order_relaxed)) {
            if (!memory_allows_activation()) { ++counters[C_MEMORY]; return false; }
            // The package goes to the GPU now; this draw is skipped, as the Core skips a new object's
            // first frame (D-103): the character is never the game's mesh before the Mod's.
            activate(); ++counters[C_PREPARING]; return true;
        }
        if (!ready.load(std::memory_order_relaxed)) {
            // The worker ended without the package: the game's draw stays, and the activation is tried again
            // at its time. While the mesh and arenas are on their way the draw is skipped, as before.
            if (worker_failed.load(std::memory_order_acquire)) { retry_activation(); ++counters[C_ACTIVATION]; return false; }
            ++counters[C_PREPARING]; return true;
        }
        ++counters[C_MATCHED];
        if (present != gate_frame) begin_gate_frame(present);
        if (section->component == kCharacterNone) {
            // Exact IB-only override: no fabricated identity or bone import.
            // Live conditions still run; a changed rule may only emit an empty skip.
            if (program) {
                efmi::frontend::RuntimeDraw request;
                request.hash = section->hash; request.count = r.count; request.first = r.first;
                request.base = r.vertex_offset; request.instances = r.instances; request.first_instance = r.first_instance;
                request.mod = program->folder_index();
                request.frame = present; request.seconds = ini_seconds();
                request.frame_seconds = frame_seconds.load(std::memory_order_relaxed);
                request.pixel_texture = [this, &r](unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why) {
                    return pixel_texture(r, slot, texture, why); };
                request.texture_roles = [this, &r](int32_t roles[3], std::string& why) { return texture_roles(r, roles, why); };
                const auto found = device->pipes.find(r.pipeline);
                if (found != device->pipes.end()) {
                    render_targets(found->second, request);
                    request.vertex_stride = [&found](unsigned slot, uint32_t& stride, std::string& why) { return vertex_stride(found->second, slot, stride, why); };
                }
                bool skip = false, added = false, textures = false; std::string reason;
                if (!plan_draw(request, runtime_list, skip, added, reason, nullptr, &textures)) return refuse(C_STATE, "INI hide: " + reason);
                if (!skip) {
                    if (!textures) return false;
                    if (added && !refresh_materials()) return false;
                    const auto pipe = device->pipes.find(r.pipeline);
                    if (pipe == device->pipes.end() || !pipe->second.recipe || !pipe->second.layout)
                        return refuse(C_PIPELINE, "texture-only draw pipeline metadata missing");
                    return record_textures(cb, r, pipe->second, static_cast<uint32_t>(section - package.sections.data()), present, runtime_list);
                }
                if (runtime_list.count) return refuse(C_STATE, "INI hide override unexpectedly emits geometry");
            }
            // With the package's mesh left to the game this frame, what it would hide stays as well.
            if (!gate_open) return gated();
            ++counters[C_HIDDEN]; return true;
        }
        // The character's frame is this recording of the command buffer (see the identity expiry).
        if (!cb->character_frame) cb->character_frame = ++device->character_recordings;
        const uint64_t now = cb->character_frame;
        // The mod's LOD variable: a section that assigns it changes it for the sections that do not.
        if (section->lod != kCharacterNone) lod = section->lod;
        const CharacterComponent& component = package.components[section->component];
        if (cb->secondary || !cb->in_rp || cb->dyn_rp_count || cb->m2_extended_bind)
            return refuse(C_STATE, "character draw outside a primary classic render pass or after extended binds");
        if (r.instances != 1 || r.first_instance != 0) return refuse(C_INSTANCES, "character draw is instanced");
        auto pipe = device->pipes.find(r.pipeline);
        if (pipe == device->pipes.end() || !pipe->second.recipe || !pipe->second.layout)
            return refuse(C_PIPELINE, "character draw pipeline metadata missing");
        lookups.pipeline = r.pipeline; lookups.pipe = &pipe->second;

        // The instance entry is at set 2 binding 0 in every pipeline of the game's characters.
        const auto& window = r.sets[2];
        if (window.dyn_count && uint64_t(window.dyn_first) + window.dyn_count > cb->dyn.size())
            return refuse(C_ENTRY, "character dynamic offsets incomplete");
        auto read_range = [&](const BoundRange* range, uint64_t offset, size_t size, uint8_t* output) {
            if (!range || offset > range->size || size > range->size - offset) return false;
            auto buffer = device->bufs.find(range->buffer);
            return buffer != device->bufs.end() && read_buffer_locked(device, buffer->second, range->off + offset, size, output) == SRC_MAPPED;
        };
        BoundRange entry_range;
        const BoundRange* instance = nullptr;
        if (bound_range(window, 2, 0, window.dyn_count ? cb->dyn.data() + window.dyn_first : nullptr, window.dyn_count, entry_range) &&
            (entry_range.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || entry_range.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))
            instance = &entry_range;
        if (!read_range(instance, 0, entry.size(), entry.data())) return refuse(C_ENTRY, "character instance entry is not mapped");
        identity = identify(entry, now, present);
        if (!identity) return refuse(C_IDENTITIES, "character position invalid or all identities in use");
        identity->seen |= 1ull << section->component;
        if (now > identity->update_frame) {
            std::string reason;
            if (!identity->skeleton.begin_update(now, reason)) return refuse(C_IMPORT, reason);
            identity->update_frame = now;
            acquire_ring(*identity); // the frame's merged skeleton on the device (D-117), or the CPU's
        }

        lap(S_IDENTIFY);
        // Import this component's poses into the identity's master, whatever happens to the draw.
        uint32_t offsets[2];
        std::memcpy(offsets, entry.data() + 80, 8);
        const uint8_t dynamic = (offsets[0] >= MergedSkeleton::kStaticRows ? 1 : 0) | (offsets[1] >= MergedSkeleton::kStaticRows ? 2 : 0);
        const BoneSite site = site_of(pipe->second);
        // On the device (D-127) every draw's component is imported in its own recording, whatever happens
        // to the draw; on the CPU the master is imported once per frame.
        gpu_draw = identity->gpu_frame && identity->ring;
        if (gpu_draw) {
            const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
            bone_owner = static_cast<CharacterCommand*>(held.get());
            if (!bone_owner) gpu_draw = false;
        }
        if (site.ok && dynamic && (gpu_draw || identity->skeleton.needs_import(0, section->component, lod))) {
            const bool remapped = lod == 1 && component.has_remap;
            uint32_t last = component.count - 1;
            if (remapped) last = *std::max_element(component.remap_lod1.begin(), component.remap_lod1.end());
            const size_t needed = (size_t(last) + 2) * MergedSkeleton::kBytesPerBone;
            current.resize(needed); previous.resize(needed);
            auto& bone_ranges = scratch_bone_ranges; bone_ranges.clear();
            const auto& bound = r.sets[site.set];
            bool readable = !(bound.dyn_count && uint64_t(bound.dyn_first) + bound.dyn_count > cb->dyn.size());
            if (readable) collect_buffer_ranges_locked(device, bound, bound.dyn_count ? cb->dyn.data() + bound.dyn_first : nullptr, bound.dyn_count, site.set, bone_ranges);
            const BoundRange* bones = nullptr;
            for (const auto& range : bone_ranges)
                if (range.element == 0 && range.binding == site.binding &&
                    (range.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || range.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) bones = &range;
            if (gpu_draw && !(bones && readable && prepare_bone_job(*identity, *bone_owner, section->component, component, remapped, needed, *bones, offsets, dynamic)))
                gpu_draw = false; // this draw imports on the CPU and binds the master
            if (gpu_draw) {
                identity->coverage[section->component] |= dynamic;
                ++identity->generation;
            } else if (identity->skeleton.needs_import(0, section->component, lod)) {
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                const uint64_t bones_read_began = host_time_us();
#endif
                readable = readable && (!(dynamic & 1) || read_range(bones, uint64_t(offsets[0]) * 16, needed, current.data())) &&
                           (!(dynamic & 2) || read_range(bones, uint64_t(offsets[1]) * 16, needed, previous.data()));
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                if (program) program->note_timing("bones read", host_time_us() - bones_read_began);
                const uint64_t bones_import_began = host_time_us();
#endif
                if (!readable) {
                    ++counters[C_BONES];
                } else {
                    std::string reason;
                    static const std::vector<uint16_t> identity_map;
                    const auto result = identity->skeleton.import_component(0, section->component, lod, entry,
                        {current.data(), needed}, {previous.data(), needed}, remapped ? component.remap_lod1 : identity_map, 1.0f, reason);
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
                    if (program) program->note_timing("bones import", host_time_us() - bones_import_began);
#endif
                    if (result == BoneImportResult::Imported) {
                        identity->coverage[section->component] |= dynamic;
                        ++identity->generation;
                    } else if (result == BoneImportResult::Rejected) {
                        ++counters[C_IMPORT];
                        note("character bone import: " + reason);
                    }
                }
            }
        }

        lap(S_BONES);
        // As in the core: the object is the mod's once enough of its components were seen at this
        // place and a frame has passed. Until then the game draws it.
        if (!identity->custom) {
            uint32_t bits = 0;
            for (uint64_t value = identity->seen; value; value &= value - 1) ++bits;
            // The original Core's identification_min_components as the Mod sets it (EFMI exports: 3).
            const uint32_t wanted = program && program->identify_components() ? program->identify_components() : kIdentifyingComponents;
            if (bits >= std::min<uint32_t>(wanted, static_cast<uint32_t>(package.components.size())) && now > identity->first_frame)
                identity->custom = true;
            else {
                ++counters[C_IDENTIFYING];
                // As the Core (SpatialIdentification.ini): an object's first frame is skipped, not drawn as
                // the game's, so a character never shows the game's mesh before the Mod's (D-103); from
                // the next frame on an object still unidentified is the game's, as there, except while a
                // new object was never the Mod's yet (kFreshPresents).
                if (present == identity->first_present) return true;
                if (!identity->shown && present - identity->first_present < kFreshPresents) { ++counters[C_HELD]; return true; }
                if (!identity->shown) ++counters[C_ORIGINAL_FIRST];
                return false;
            }
        }
        list = &package.lists[component.lists[lod]];
        if (program) {
            efmi::frontend::RuntimeDraw request;
            request.hash = section->hash; request.count = r.count; request.first = r.first; request.base = r.vertex_offset;
            request.component = section->component; request.lod = lod; request.identity = identity->id; request.frame = now;
            request.mod = program->folder_index();
            request.instance_entry = entry.data(); request.instance_entry_size = entry.size();
            render_targets(pipe->second, request);
            // The size of colour target 0 for the list's rt_width and rt_height, as the original reads
            // them off the bound target (ProcessParamRTSize): none bound (a depth-only pass) answers -1,
            // as there; a bound one its image's size, or the pass's render area when the framebuffer's
            // views are not known (a Mod's `if rt_width != 0` guards every draw).
            if (request.targets_known && !(request.render_targets & 1)) {
                request.target_width = request.target_height = -2; // asked, nothing bound
            } else if (request.targets_known) {
                bool sized = false;
                if (cb->pass.info && cb->pass.info->known && cb->pass.recipe && pipe->second.recipe) {
                    const uint32_t sub = pipe->second.recipe->subpass;
                    if (sub < cb->pass.recipe->subpasses.size() && !cb->pass.recipe->subpasses[sub].colors.empty()) {
                        const uint32_t ci = cb->pass.recipe->subpasses[sub].colors[0].attachment;
                        if (ci < cb->pass.info->views.size() && cb->pass.info->views[ci].known) {
                            request.target_width = float(cb->pass.info->views[ci].image.extent.width);
                            request.target_height = float(cb->pass.info->views[ci].image.extent.height);
                            sized = true;
                        }
                    }
                }
                // The framebuffer's size (its attachments' at least), then the pass's render area.
                if (!sized && cb->pass.info && cb->pass.info->width && cb->pass.info->height) {
                    request.target_width = float(cb->pass.info->width); request.target_height = float(cb->pass.info->height);
                    sized = true;
                }
                if (!sized && cb->pass.area.extent.width && cb->pass.area.extent.height) {
                    request.target_width = float(cb->pass.area.offset.x + int32_t(cb->pass.area.extent.width));
                    request.target_height = float(cb->pass.area.offset.y + int32_t(cb->pass.area.extent.height));
                }
            }
            if (request.target_width == -1 && !target_size_logged) {
                target_size_logged = true;
                logf("character draw without a render target size: targets %s (mask 0x%x), pipeline recipe %s, render pass %s, "
                     "framebuffer %s, area %ux%u", request.targets_known ? "known" : "unknown", request.render_targets,
                     pipe->second.recipe ? "yes" : "no", pipe->second.recipe && pipe->second.recipe->render_pass ? "yes" : "no",
                     cb->pass.info ? "known" : "unknown", cb->pass.area.extent.width, cb->pass.area.extent.height);
            }
            // EFMITTL's shader groups (D-113, D-171).
            if (translucency && program->efmittl()) {
                const LibraryHooks& hooks = library_hooks(pipe->second);
                request.vertex_hook = efmi::frontend::VertexHook(hooks.vertex);
                request.pixel_hooks = hooks.pixel; request.uv_colour_slot = hooks.colour_slot; request.uv_narrow_first = hooks.narrow_first;
                translucency->listen();
            }
            request.seconds = ini_seconds();
            request.frame_seconds = frame_seconds.load(std::memory_order_relaxed);
            request.pixel_texture = [this, &r](unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why) {
                return pixel_texture(r, slot, texture, why); };
            request.texture_roles = [this, &r](int32_t roles[3], std::string& why) { return texture_roles(r, roles, why); };
            request.vertex_stride = [&pipe](unsigned slot, uint32_t& stride, std::string& why) { return vertex_stride(pipe->second, slot, stride, why); };
            const SourceFacts& facts = facts_of(pipe->second, site);
            request.source_vs_config_slot = facts.vs_config_slot;
            request.rolling_bone_source = facts.rolling_bones;
            request.source_srv = [&facts](wchar_t stage, unsigned slot, uint32_t& type, uint32_t& dimension, uint32_t& stride, std::string& why) {
                const wchar_t* names = L"vhdgp";
                const wchar_t* at = stage ? wcschr(names, stage) : nullptr;
                const auto* registers = at ? facts.registers[size_t(at - names)].get() : nullptr;
                if (!registers) { why = "the stage's SPIR-V registers are not known"; return false; }
                efmi::gpu::SourceSrvDeclaration declaration;
                if (!efmi::gpu::source_srv_declaration(*registers, slot, declaration, why)) return false;
                type = declaration.words ? efmi::frontend::kWordBufferDeclaration : declaration.type;
                dimension = declaration.dimension; stride = declaration.stride;
                return true;
            };
            bool skip = false, added = false, textures = false; std::string reason;
            if (!plan_draw(request, runtime_list, skip, added, reason, &pipe->second.recipe->vertex, &textures))
                return refuse(C_STATE, "INI plan: " + reason);
            if (!skip) {
                // The Mod's lists keep the game's draw (its own identification rejected the object, D-086).
                if (!textures) {
                    static std::mutex kept_mx; static std::set<uint32_t> kept_logged;
                    { std::lock_guard<std::mutex> hold(kept_mx); if (kept_logged.insert(section->hash).second) logf("character draw kept the game's: section %08x (count %u)", section->hash, section->match_count); }
                    // A new object the Mod has not drawn yet (its registration with the Core takes a
                    // Present): skipped rather than the game's mesh, for kFreshPresents at most.
                    if (!identity->shown && present - identity->first_present < kFreshPresents) { ++counters[C_HELD]; return true; }
                    if (!identity->shown) ++counters[C_ORIGINAL_FIRST];
                    ++counters[C_KEPT]; return false;
                }
                if (added && !refresh_materials()) return false;
                const bool textured = record_textures(cb, r, pipe->second, static_cast<uint32_t>(section - package.sections.data()), present, runtime_list);
                if (textured) identity->shown = true;
                return textured;
            }
            if (added && !refresh_materials()) return false;
            list = &runtime_list;
        }
        lap(S_PLAN);
        if (!list->count) { if (!gate_open) return gated(); ++counters[C_HIDDEN]; return true; }
        for (uint32_t c = 0; c < package.components.size(); ++c)
            if ((list->required >> c & 1) && (identity->coverage[c] & dynamic) != dynamic) { ++counters[C_INCOMPLETE]; return false; }
        // Under the tracker's lock (D-127): each component the list's vertices read comes from an import
        // this recording carries, from the object's skeleton that a submitted batch imported it into, or
        // from a recording that drew it and is not yet submitted (its bone data valid until that recording
        // completed; the tracker's id tells a reused buffer handle apart). A draw that would read bones no
        // import wrote is refused rather than drawn with them.
        if (gpu_draw)
            for (uint32_t c = 0; c < package.components.size() && c < 64; ++c) {
                if (!(list->required >> c & 1)) continue;
                if (bone_owner->has_bone_job(identity->ring.get(), c)) continue;
                if (identity->ring->submitted.load(std::memory_order_acquire) >> c & 1) continue;
                const BoneJob* source = c < identity->bone_sources.size() && identity->bone_sources[c].ring ? &identity->bone_sources[c] : nullptr;
                if (source && source->recording && source->recording->pending(source->epoch)) {
                    const auto alive = device->bufs.find(source->source);
                    if (alive != device->bufs.end() && alive->second.id == source->source_id) { borrowed_sources |= 1ull << c; continue; }
                }
                return refuse(C_BONES_UNAVAILABLE, "character draw reads bones of a component no device import wrote");
            }
        // The private set is bound right above the pipeline's own sets: slot 3 for most character
        // pipelines, slot 4 for the four-set ones (the game's visibility pass among them).
        const uint32_t own = static_cast<uint32_t>(pipe->second.layout->sets.size());
        if (own >= kMaxSets) {
            // Not a tracked slot: usable only if no pipeline layout of the application reaches it.
            if (device->max_layout_sets > own) return refuse(C_SET3, "character draw: the slot above the pipeline's sets is in use and not tracked");
        } else if (r.sets[own].pushed) {
            return refuse(C_SET3, "character draw with pushed descriptors at the private set's slot");
        } else if (r.sets[own].set) {
            // The tracker keeps a set only while later binds left it valid, so the layout it was
            // bound with agrees with the current one for the lower sets, and binding it again there
            // disturbs nothing. Vulkan keeps that layout alive while this recording lasts.
            const SetBind& app = r.sets[own];
            restore_slot = own;
            auto live = device->pipeline_layouts.find(app.layout_handle);
            if (!app.layout_handle || !app.layout || app.unsupported || live == device->pipeline_layouts.end() ||
                live->second != app.layout || (app.dyn_count && uint64_t(app.dyn_first) + app.dyn_count > cb->dyn.size()))
                return refuse(C_SET3, "character draw with an application set that cannot be bound again");
            restore_layout = app.layout_handle; restore_set = app.set;
            restore_offsets.assign(cb->dyn.begin() + app.dyn_first, cb->dyn.begin() + app.dyn_first + app.dyn_count);
        }

        // Restore exactly the vertex bindings the pipeline can read among the four tracked ones.
        uint32_t slots = 0;
        for (const auto& b : pipe->second.recipe->vertex.bindings) if (b.binding < kMaxVb) slots = std::max(slots, b.binding + 1);
        for (uint32_t slot = 0; slot < 4; ++slot)
            for (uint32_t i = 0; i < list->count; ++i)
                if (package.draws[list->first + i].vb[slot] != kCharacterNone) slots = std::max(slots, slot + 1);
        if (!slots || !r.ib) return refuse(C_BINDINGS, "character draw without tracked vertex or index bindings");
        for (uint32_t slot = 0; slot < slots; ++slot) {
            if (!r.vb[slot].buf) return refuse(C_BINDINGS, "character original vertex bindings incomplete");
            original.buffers.push_back(r.vb[slot].buf); original.offsets.push_back(r.vb[slot].off);
        }
        original.index = r.ib; original.index_offset = r.ib_off;
        original.index_type = r.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        bool pending = false;
        if (!material_draws(pipe->second, static_cast<uint32_t>(section - package.sections.data()), r, present, *list,
                            material_plans, material_views, material_origin)) return false;
        lap(S_MATERIALS);
        // EFMITTL's translucent draws (D-113): captured at the end of the pass when the library's
        // shaders, the layers and the pass's targets are there; otherwise drawn as before.
        capturing.assign(list->count, 0); capture_masks.assign(list->count, SnapshotImage{}); capture_t4.assign(list->count, 0);
        capture_sets = static_cast<uint32_t>(pipe->second.layout->sets.size());
        for (auto& v : scratch_captures) { v.clear(); v.resize(list->count); }
        bool any_translucent = false;
        for (uint32_t i = 0; i < list->count; ++i) any_translucent |= package.draws[list->first + i].translucent;
        if (any_translucent) {
            std::string why;
            const uint32_t own_set = static_cast<uint32_t>(pipe->second.layout->sets.size());
            const auto& ps = cb->pass;
            const RenderSubpass* sub = ps.recipe && pipe->second.subpass < ps.recipe->subpasses.size() ? &ps.recipe->subpasses[pipe->second.subpass] : nullptr;
            const TargetView* target = sub && !sub->colors.empty() && ps.info && sub->colors[0].attachment < ps.info->views.size()
                                           ? &ps.info->views[sub->colors[0].attachment] : nullptr;
            capture_layers.reset();
            const bool ready = translucency && program && program->efmittl() && target && target->known &&
                               translucency->shaders(program->library_folder("efmittl"), own_set, why) &&
                               (capture_layers = translucency->prepare(target->image.extent.width, target->image.extent.height, why)) != nullptr;
            if (!ready && !why.empty()) note(why);
            for (uint32_t i = 0; ready && i < list->count; ++i) {
                const CharacterDraw& d = package.draws[list->first + i];
                if (!d.translucent) continue;
                capturing[i] = 1;
                if (d.mask_image != kCharacterNone) {
                    if (!textures || d.mask_image >= image_ids.size()) return refuse(C_IMAGE, "translucency mask outside the package's images");
                    auto& mask_bindings = scratch_image_bindings;
                    frame_image(image_ids[d.mask_image]);
                    textures->bind({image_ids[d.mask_image]}, mask_bindings);
                    if (mask_bindings.empty() || mask_bindings[0].failed) return refuse(C_IMAGE, "translucency mask: " + (mask_bindings.empty() ? std::string() : mask_bindings[0].reason));
                    if (!mask_bindings[0].view) { gate_open = false; ++counters[C_IMAGE_PENDING]; return false; }
                    capture_masks[i] = {mask_bindings[0].view, mask_bindings[0].owner};
                }
                // The game's screen-space mask (ps-t4) is replaced by the neutral one while captured.
                const FragmentRegisters* registers = nullptr;
                {
                    auto known = fragment_registers.find(pipe->second.id);
                    if (known == fragment_registers.end()) {
                        FragmentRegisters fresh; fresh.readable = true;
                        for (const auto& stage : pipe->second.recipe->stages)
                            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                                std::string ignored; fresh.readable = efmi::gpu::source_registers(stage.shader->code, fresh.list, ignored);
                            }
                        if (fragment_registers.size() >= 8192) fragment_registers.clear();
                        known = fragment_registers.emplace(pipe->second.id, std::move(fresh)).first;
                    }
                    registers = &known->second;
                }
                if (registers->readable)
                    if (const auto* reg = efmi::gpu::find_source_register(registers->list, 't', 4);
                        reg && reg->kind == efmi::gpu::SourceRegister::ImageSrv && reg->count == 1 && reg->dim == 1 &&
                        image_counts.count(uint32_t(material_views[i].size()) + 1)) {
                        bool patched = false;
                        for (const auto& image : material_plans[i]->patch.images) patched |= image.set == reg->set && image.binding == reg->binding;
                        for (const auto& image : material_plans[i]->patch.nulls) patched |= image.set == reg->set && image.binding == reg->binding;
                        if (!patched) { capture_t4[i] = 1; capture_t4_binding = {reg->set, reg->binding}; }
                    }
            }
        }
        struct WaitingVariant { uint32_t draw; VertexInputRecipe vertex; };
        std::vector<WaitingVariant> waiting; // Allocates only for a cold variant.
        variants.resize(list->count);
        // While a discard gate is set (D-133) the draw's fragment stage goes without the character dither,
        // as the application's own draws of the pipeline do through its twin. The other state is prepared
        // with the first, so the key finds it made.
        const bool dithered = g_dither_gated.load(std::memory_order_relaxed) && pipeline_dither(pipe->second.id, *pipe->second.recipe);
        const bool no_dither = dithered && g_dither_off.load(std::memory_order_relaxed);
        auto patch_of = [&](uint32_t i) -> const FragmentPatch& {
            if (!no_dither) return material_plans[i]->patch;
            if (scratch_patches.size() < list->count) scratch_patches.resize(list->count);
            scratch_patches[i] = material_plans[i]->patch; scratch_patches[i].no_dither = true;
            return scratch_patches[i];
        };
        auto keep_variant = [&](uint32_t i, const VariantResult& result) {
            if (!result.variant) return false;
            variants[i] = result.variant;
            if (variant_lookup.size() < 65536)
                variant_lookup.emplace(std::make_tuple(pipe->second.id, list->first + i, material_plans[i], no_dither), result.variant);
            return true;
        };
        for (uint32_t i = 0; i < list->count; ++i) {
            if (capturing[i]) {
                VertexInputRecipe vertex;
                std::string reason;
                if (!package.vertex_input(package.draws[list->first + i], pipe->second.recipe->vertex, vertex, reason))
                    return refuse(C_VERTEX, "character vertex declaration: " + reason);
                const uint32_t own_set = static_cast<uint32_t>(pipe->second.layout->sets.size());
                for (uint32_t k = 0; k < kCaptureKinds; ++k) {
                    const auto recipe = translucency->recipe(pipe->second, CaptureKind(k), own_set, reason);
                    if (!recipe) return refuse(C_VARIANT, "translucent capture: " + reason);
                    FragmentPatch patch;
                    if (k < 2) { patch = material_plans[i]->patch; patch.no_dither = no_dither; if (capture_t4[i]) patch.images.push_back({capture_t4_binding.first, capture_t4_binding.second}); }
                    else patch.replacement_images = 2;
                    const auto result = cache->request(capture_identity(pipe->second.id, CaptureKind(k)), recipe, pipe->second.layout, vertex, patch);
                    if (k < 2 && dithered) cache->prepare(capture_identity(pipe->second.id, CaptureKind(k)), recipe, pipe->second.layout, vertex, other_state(patch));
                    if (result.variant) { scratch_captures[k][i] = result.variant; continue; }
                    if (result.state == VariantState::Queued || result.state == VariantState::Building) {
                        waiting_captures.push_back({i, k, vertex, recipe, std::move(patch)}); continue;
                    }
                    return refuse(C_VARIANT, "translucent capture variant: " + result.reason);
                }
                continue;
            }
            const auto lookup_key = std::make_tuple(pipe->second.id, list->first + i, material_plans[i], no_dither);
            if (const auto known = variant_lookup.find(lookup_key); known != variant_lookup.end()) { variants[i] = known->second; continue; }
            VertexInputRecipe vertex;
            std::string reason;
            if (!package.vertex_input(package.draws[list->first + i], pipe->second.recipe->vertex, vertex, reason))
                return refuse(C_VERTEX, "character vertex declaration: " + reason);
            // Issue all cold requests before waiting so the existing builders can run in parallel.
            const auto result = cache->request(pipe->second.id, pipe->second.recipe, pipe->second.layout, vertex, patch_of(i));
            if (dithered) cache->prepare(pipe->second.id, pipe->second.recipe, pipe->second.layout, vertex, other_state(patch_of(i)));
            if (keep_variant(i, result)) continue;
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { waiting.push_back({i, std::move(vertex)}); continue; }
            return refuse(C_VARIANT, "character variant: " + result.reason);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        for (const auto& request : waiting) {
            const auto remaining = std::max(std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            const auto result = cache->await(pipe->second.id, pipe->second.recipe, pipe->second.layout,
                request.vertex, patch_of(request.draw), remaining);
            if (keep_variant(request.draw, result)) continue;
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { pending = true; continue; }
            return refuse(C_VARIANT, "character variant: " + result.reason);
        }
        for (const auto& request : waiting_captures) {
            const auto remaining = std::max(std::chrono::milliseconds(0),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            const auto result = cache->await(capture_identity(pipe->second.id, CaptureKind(request.kind)), request.recipe, pipe->second.layout,
                                             request.vertex, request.patch, remaining);
            if (result.variant) { scratch_captures[request.kind][request.draw] = result.variant; continue; }
            if (result.state == VariantState::Queued || result.state == VariantState::Building) { pending = true; continue; }
            return refuse(C_VARIANT, "translucent capture variant: " + result.reason);
        }
        waiting_captures.clear();
        if (pending) { gate_open = false; frame_variants_pending = true; ++counters[C_PENDING]; return false; }
        // Images and variants were asked for above, so a closed frame still brings them up.
        if (!gate_open) return gated();
        std::string reason;
        if (!identity->skeleton.patch_entry(0, entry, patched, reason)) return refuse(C_IMPORT, reason);
    }

    lap(S_VARIANTS);
    // A recording that holds work of other replacement code keeps it to its end. What that code
    // left in a command buffer from earlier recordings gives way.
    // Every character package that replaces draws of a recording has its own owner in it.
    const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
    if (!held) return refuse(C_EXHAUSTED, "character snapshot pool is at capacity or refilling");
    CharacterCommand* const command_owner = static_cast<CharacterCommand*>(held.get());
    command_owner->used = true;
    const auto mesh_owner = std::static_pointer_cast<const GpuMesh>(command_owner->keep);
    if (!mesh_owner) return refuse(C_EXHAUSTED, "character command owner has no mesh");
    // The object's snapshot this recording's draws read (D-127): copied in the recording's batch.
    uint32_t snapshot_slot = UINT32_MAX;
    if (gpu_draw) {
        snapshot_slot = command_owner->snapshot(identity->ring);
        if (snapshot_slot == UINT32_MAX) return refuse(C_BONE_RING, "character bone snapshots are all taken");
    }
    lap(S_OWNER);
    // Everything is decided; from here nothing can fail after a command was emitted.
    auto& geometry = scratch_geometry; if (geometry.size() < list->count) geometry.resize(list->count);
    auto& items = scratch_items; items.clear();
    scratch_pending_captures.clear(); // nothing of an earlier refused draw goes to the queue
    const std::vector<uint8_t>& master = identity->skeleton.bytes();
    const uint64_t key = identity->id << 32 ^ identity->generation;
    const AppendBones append_bones = [&](DrawSnapshots& arena, const std::vector<SnapshotImage>& views, SnapshotSlot& out) {
        if (!gpu_draw) return arena.append_shared(master.data(), master.size(), key, patched, views, out);
        const auto& ring = identity->ring;
        if (!arena.append_external(ring->buffer(snapshot_slot), ring->offset(snapshot_slot), ring->slot_bytes, patched, views, out)) return false;
        // A component only a recording not yet submitted imported: this recording takes that import, which
        // never replaces one of its own (D-127).
        if (borrowed_sources) {
            uint64_t taken = 0;
            for (uint32_t c = 0; c < 64; ++c)
                if (borrowed_sources >> c & 1) { command_owner->add_bone_job(identity->bone_sources[c]); ++taken; }
            borrowed_sources = 0;
            if (bones_gpu) bones_gpu->note_borrowed(taken);
        }
        return true;
    };
    for (uint32_t i = 0; i < list->count; ++i) {
        const CharacterDraw& d = package.draws[list->first + i];
        SnapshotSlot slot;
        const std::vector<SnapshotImage>* slot_views = &material_views[i];
        if (capturing[i] && capture_t4[i]) {
            scratch_capture_views = material_views[i]; scratch_capture_views.push_back(Translucency::neutral_view(capture_layers));
            if (!scratch_capture_views.back().view) return refuse(C_IMAGE, "translucency layers missing");
            slot_views = &scratch_capture_views;
        }
        const uint32_t count = static_cast<uint32_t>(slot_views->size());
        auto arena = count ? command_owner->material_snapshots.at(count) : command_owner->snapshots;
        const uint32_t slices_before = arena->slices_used();
        const uint64_t append_began = host_time_us();
        if (!append_bones(*arena, *slot_views, slot)) return refuse(C_EXHAUSTED, arena->reason);
        stage_us[S_APPEND] += host_time_us() - append_began;
        if (arena->slices_used() != slices_before) ++counters[C_SNAPSHOT_COPIES];
        DrawGeometry& g = geometry[i];
        g = original;
        for (uint32_t s = 0; s < g.buffers.size(); ++s) {
            if (d.vb[s] == kCharacterOwned) {
                // Checked by vertex_input above: the slot has its owned buffer.
                const auto* o = d.owned_stream(s);
                g.buffers[s] = reinterpret_cast<VkBuffer>(o->buffer); g.offsets[s] = o->offset;
                command_owner->owned.push_back(o->keep); ++counters[C_OWNED];
            } else if (d.vb[s] != kCharacterNone) { g.buffers[s] = mesh_owner->buffer; g.offsets[s] = package.streams[d.vb[s]].offset; }
        }
        // A non-indexed draw (D-142) leaves the application's index binding in place, unused.
        if (d.indexed) {
            const auto& index = package.streams[d.index_stream];
            g.index = mesh_owner->buffer; g.index_offset = index.offset;
            g.index_type = index.stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
        IndexedDraw args; args.count = d.count; args.first = d.first; args.vertex_offset = d.base;
        args.instances = d.instances; args.first_instance = d.first_instance; args.indexed = d.indexed;
        if (capturing[i]) {
            if (!capture(cb, r, capture_sets, d, i, slot, g, args, append_bones, command_owner)) return false;
            continue;
        }
        items.push_back({variants[i].get(), slot.set, slot.offset, &g, args});
    }
    // The captures go to the pass's queue once every draw of the list was decided.
    for (auto& c : scratch_pending_captures) {
        std::string why;
        if (!translucency->queue(cb, std::move(c), why)) { scratch_pending_captures.clear(); return refuse(C_STATE, why); }
        ++counters[C_TRANSLUCENT];
    }
    scratch_pending_captures.clear();
    lap(S_SNAPSHOT);
    command_owner->variants.insert(command_owner->variants.end(), variants.begin(), variants.end());
    if (!emit_variant_draws(draw, cb->handle, items.data(), items.size(), r.bound ? r.bound : r.pipeline, original))
        return refuse(C_VARIANT, "character draw transaction refused");
    if (restore_layout) {
        draw.CmdBindDescriptorSets(cb->handle, VK_PIPELINE_BIND_POINT_GRAPHICS, restore_layout, restore_slot, 1, &restore_set,
                                   static_cast<uint32_t>(restore_offsets.size()), restore_offsets.empty() ? nullptr : restore_offsets.data());
        ++counters[C_SET3_RESTORED];
    }
    lap(S_EMIT);
    ++counters[C_REPLACED];
    if (identity) identity->shown = true;
    if (material_origin) ++counters[material_origin == 1 ? C_DERIVED : C_INHERITED];
    counters[C_ITEMS] += list->count;
    for (const auto* plan : material_plans) {
        if (!plan->patch.images.empty() || !plan->patch.nulls.empty()) ++counters[C_MATERIAL_ITEMS];
        if (plan->compatibility) ++counters[C_COMPAT_ITEMS];
    }
    return true;
}

bool CharacterGeneration::record_textures(CmdBuf* cb, const DrawRec& r, const PipeInfo& pipe, uint32_t section, uint64_t present,
                                          const CharacterList& list)
{
    if (list.count != 1 || !package.draws[list.first].texture_only) return refuse(C_STATE, "texture-only list expected");
    if (cb->secondary || !cb->in_rp || cb->dyn_rp_count || cb->m2_extended_bind)
        return refuse(C_STATE, "texture-only draw outside a primary classic render pass or after extended binds");
    std::vector<const StaticMaterial*> plans;
    std::vector<std::vector<SnapshotImage>> views;
    uint32_t origin = 0;
    if (!material_draws(pipe, section, r, present, list, plans, views, origin)) return false;
    // Nothing the module can take: the application's draw stays as it is.
    if (plans.size() != 1 || (plans[0]->patch.images.empty() && plans[0]->patch.nulls.empty())) return false;
    // The private set above the pipeline's own sets, and an application set there bound again after.
    uint32_t restore_slot = 0;
    VkPipelineLayout restore_layout = VK_NULL_HANDLE;
    VkDescriptorSet restore_set = VK_NULL_HANDLE;
    std::vector<uint32_t> restore_offsets;
    const uint32_t own = static_cast<uint32_t>(pipe.layout->sets.size());
    if (own >= kMaxSets) {
        if (device->max_layout_sets > own) return refuse(C_SET3, "texture-only draw: the slot above the pipeline's sets is in use and not tracked");
    } else if (r.sets[own].pushed) {
        return refuse(C_SET3, "texture-only draw with pushed descriptors at the private set's slot");
    } else if (r.sets[own].set) {
        const SetBind& app = r.sets[own];
        restore_slot = own;
        auto live = device->pipeline_layouts.find(app.layout_handle);
        if (!app.layout_handle || !app.layout || app.unsupported || live == device->pipeline_layouts.end() ||
            live->second != app.layout || (app.dyn_count && uint64_t(app.dyn_first) + app.dyn_count > cb->dyn.size()))
            return refuse(C_SET3, "texture-only draw with an application set that cannot be bound again");
        restore_layout = app.layout_handle; restore_set = app.set;
        restore_offsets.assign(cb->dyn.begin() + app.dyn_first, cb->dyn.begin() + app.dyn_first + app.dyn_count);
    }
    // The application's own geometry: the vertex bindings the pipeline reads among the tracked ones,
    // bound again as they are; higher ones are not touched and stay the application's.
    DrawGeometry original;
    uint32_t slots = 0;
    for (const auto& b : pipe.recipe->vertex.bindings) if (b.binding < kMaxVb) slots = std::max(slots, b.binding + 1);
    if (!r.ib) return refuse(C_BINDINGS, "texture-only draw without a tracked index binding");
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (!r.vb[slot].buf) return refuse(C_BINDINGS, "texture-only draw's vertex bindings incomplete");
        original.buffers.push_back(r.vb[slot].buf); original.offsets.push_back(r.vb[slot].off);
    }
    if (original.buffers.empty()) return refuse(C_BINDINGS, "texture-only draw without vertex bindings");
    original.index = r.ib; original.index_offset = r.ib_off;
    original.index_type = r.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    FragmentPatch patch = plans[0]->patch;
    patch.keep_vertex = true;
    const bool dithered = g_dither_gated.load(std::memory_order_relaxed) && pipeline_dither(pipe.id, *pipe.recipe);
    patch.no_dither = dithered && g_dither_off.load(std::memory_order_relaxed);
    const auto result = cache->await(pipe.id, pipe.recipe, pipe.layout, pipe.recipe->vertex, patch, std::chrono::milliseconds(4000));
    if (dithered) cache->prepare(pipe.id, pipe.recipe, pipe.layout, pipe.recipe->vertex, other_state(patch));
    if (!result.variant) {
        if (result.state == VariantState::Queued || result.state == VariantState::Building) { ++counters[C_PENDING]; return false; }
        return refuse(C_VARIANT, "texture-only variant: " + result.reason);
    }
    // Every character package that replaces draws of a recording has its own owner in it.
    const auto held = character_owner_for(cb->owners, generation_key.get(), [this] { return std::shared_ptr<CommandOwner>(command()); });
    if (!held) return refuse(C_EXHAUSTED, "character snapshot pool is at capacity or refilling");
    CharacterCommand* const command_owner = static_cast<CharacterCommand*>(held.get());
    command_owner->used = true;
    const uint32_t count = static_cast<uint32_t>(views[0].size());
    auto arena = count ? command_owner->material_snapshots.at(count) : command_owner->snapshots;
    static const InstanceEntry unused{};
    SnapshotSlot slot;
    if (!arena->append_images(unused, views[0], slot)) return refuse(C_EXHAUSTED, arena->reason);
    IndexedDraw args;
    args.count = r.count; args.instances = r.instances; args.first = r.first; args.vertex_offset = r.vertex_offset;
    args.first_instance = r.first_instance;
    const VariantDraw item{result.variant.get(), slot.set, slot.offset, &original, args};
    command_owner->variants.push_back(result.variant);
    if (!emit_variant_draws(draw, cb->handle, &item, 1, r.bound ? r.bound : r.pipeline, original))
        return refuse(C_VARIANT, "texture-only draw transaction refused");
    if (restore_layout) {
        draw.CmdBindDescriptorSets(cb->handle, VK_PIPELINE_BIND_POINT_GRAPHICS, restore_layout, restore_slot, 1, &restore_set,
                                   static_cast<uint32_t>(restore_offsets.size()), restore_offsets.empty() ? nullptr : restore_offsets.data());
        ++counters[C_SET3_RESTORED];
    }
    ++counters[C_TEXTURED];
    if (origin) ++counters[origin == 1 ? C_DERIVED : C_INHERITED];
    ++counters[C_MATERIAL_ITEMS];
    if (plans[0]->compatibility) ++counters[C_COMPAT_ITEMS];
    return true;
}

VkResult generation_submit(CharacterGeneration& backend, VkQueue queue)
{
    ThreadTrace* const trace = backend.device->trace ? backend.device->trace() : nullptr;
    // Owned GPU work of replaced draws runs before the application's work on the queue (D-087).
    if (backend.presenter) { StageMark stage(trace, "character draw work submission (generation lock)"); backend.submit_draw_work(queue); }
    return VK_SUCCESS;
}

bool generation_record(CharacterGeneration& backend, CmdBuf* cb, const DrawRec& r, uint32_t section)
{
    try { return backend.record(cb, r, section); }
    catch (const std::exception& e) { ++backend.counters[C_STATE]; backend.note(e.what()); return false; }
}

} // namespace efvk
