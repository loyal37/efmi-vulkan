// The object a matched draw belongs to and its merged skeleton: the range of the instance entry, the
// identities of a package's objects, their skeletons on the device with the imports a recording carries,
// and what a pipeline's vertex shader tells of the bones (CharacterGeneration's part).
#include "character_backend_internal.h"

namespace efvk {
// Caller holds res_mx. The buffer range at `binding`, element 0, of a bound set, by the rules of
// collect_buffer_ranges_locked (the dynamic offset is the one of this element among the set's
// dynamic bindings in binding order; the range lies inside a live buffer), without listing the
// set's other ranges: the instance entry is binding 0 of set 2, read on every matched draw.
bool CharacterGeneration::bound_range(const SetBind& bind, uint32_t slot, uint32_t binding, const uint32_t* dyn, uint32_t dyn_count, BoundRange& out) const
{
    auto dynamic_type = [](uint32_t t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; };
    const DescriptorSetInfo* contents = bind.pushed.get();
    if (!contents && bind.id) {
        const auto set = device->descriptor_sets.find(bind.set);
        if (set != device->descriptor_sets.end() && set->second.id == bind.id) contents = &set->second;
    }
    if (bind.unsupported || !contents || !contents->layout) return false;
    const auto layout = contents->layout->bindings.find(binding);
    if (layout == contents->layout->bindings.end()) return false;
    const uint32_t type = layout->second.type;
    if (!dynamic_type(type) && type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return false;
    uint32_t dynamic = 0;
    for (const auto& kv : contents->layout->bindings) {
        if (kv.first >= binding) break;
        if (dynamic_type(kv.second.type)) {
            const auto count = contents->counts.find(kv.first);
            if (count == contents->counts.end()) return false;
            dynamic += count->second;
        }
    }
    const auto value = contents->values.find({binding, 0});
    if (value == contents->values.end() || !value->second.known) return false;
    const DescriptorElement& e = value->second;
    const auto buffer = device->bufs.find(e.buffer);
    if (!e.buffer_id || buffer == device->bufs.end() || buffer->second.id != e.buffer_id) return false;
    const uint64_t offset = e.offset + (dynamic_type(type) && dynamic < dyn_count ? dyn[dynamic] : 0);
    const uint64_t size = buffer->second.size;
    if (offset > size) return false;
    const uint64_t range = e.range == VK_WHOLE_SIZE ? (e.offset <= size ? size - e.offset : 0) : e.range;
    if (range > size - offset) return false;
    out = BoundRange{};
    out.set = static_cast<uint8_t>(slot); out.binding = binding; out.element = 0; out.type = type;
    out.buffer = e.buffer; out.buffer_id = e.buffer_id; out.off = offset; out.size = range;
    return true;
}

// `now`: the character's frame; `present`: counted presents.
Identity* CharacterGeneration::identify(const InstanceEntry& entry, uint64_t now, uint64_t present)
{
    float position[3];
    std::memcpy(position, entry.data() + 48, sizeof(position));
    int32_t cell[3];
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(position[i]) || std::fabs(position[i]) > 1.0e6f) return nullptr;
        cell[i] = static_cast<int32_t>(std::floor(position[i] / kCellSize));
    }
    const float scaled = std::ceil(kBaseRadius * (frame_seconds.load(std::memory_order_relaxed) * 120.0f));
    const int32_t radius = static_cast<int32_t>(std::min(std::max(scaled, kBaseRadius), 64.0f));
    const uint64_t clock = host_time_us();
    Identity* best = nullptr;
    int32_t best_distance = INT32_MAX;
    Identity* vacant = nullptr;
    Identity* weakest = nullptr;
    Identity* stale = nullptr;
    uint32_t weakest_seen = UINT32_MAX;
    // For the log when a new identity is made: the replaced object nearest to this place.
    int32_t near_distance = INT32_MAX;
    uint64_t near_frames = 0, near_us = 0;
    bool near_expired = false;
    for (auto& identity : identities) {
        int32_t distance = 0;
        for (int i = 0; i < 3; ++i) distance = std::max(distance, std::abs(cell[i] - identity.cell[i]));
        // Recordings of two threads may interleave, so a frame number can be behind the last one.
        const uint64_t gap = now > identity.last_frame ? now - identity.last_frame : identity.last_frame - now;
        const bool unseen = present - identity.last_present > kExpiryFrames;
        bool expired = false;
        if (identity.used && unseen && clock - identity.last_us > g_cfg.m2_identity_expiry_ms * 1000) {
            identity.used = false; expired = true;
            identity.ring.reset(); identity.gpu_frame = false; identity.bone_sources.clear();
            ++counters[C_IDENTITY_EXPIRED];
        }
        if ((identity.used || expired) && identity.custom && distance < near_distance) {
            near_distance = distance; near_frames = gap; near_us = clock - identity.last_us; near_expired = expired;
        }
        if (!identity.used) { if (!vacant) vacant = &identity; continue; }
        // Every component of one object has the same position within a frame, to a cell: the passes
        // of one object differ by that much (D-100; 9 of 42 new identities of a session were this).
        // Across frames the object may have moved, by at most the radius for each frame it was not
        // drawn in.
        const int32_t allowed = gap ? static_cast<int32_t>(std::min<uint64_t>(uint64_t(radius) * gap, 64)) : 1;
        if (distance <= allowed && distance < best_distance) { best = &identity; best_distance = distance; }
        uint32_t bits = 0;
        for (uint64_t value = identity.seen; value; value &= value - 1) ++bits;
        if (!identity.custom && bits < weakest_seen) { weakest = &identity; weakest_seen = bits; }
        if (unseen && (!stale || identity.last_present < stale->last_present)) stale = &identity;
    }
    if (!best) {
        // Farther than the radius in one frame (a dash, a camera cut, 4 to 7 metres in a frame in
        // the game): when exactly one identified object of this package has not been drawn since
        // the last presentation, it is that one, moved (D-100). A second instance appearing in
        // the very frame the first is not drawn is the rarer case; it is told apart a frame later.
        Identity* lone = nullptr;
        uint32_t lone_count = 0;
        for (auto& identity : identities)
            if (identity.used && identity.custom && identity.last_present < present) { lone = &identity; ++lone_count; }
        if (lone_count == 1) { best = lone; best_distance = 0; ++counters[C_IDENTITY_ADOPTED]; }
    }
    if (!best) {
        // Objects that merely share a mesh with the character must not keep it out: when every
        // slot is taken, the unidentified object seen with the fewest components gives way, and
        // failing that the object that has gone longest without a draw.
        if (!vacant) vacant = weakest;
        if (!vacant) vacant = stale;
        if (!vacant) return nullptr;
        ++counters[C_IDENTITY_NEW];
        if (near_distance != INT32_MAX && identity_logs < 200) {
            ++identity_logs;
            logf("character identity %llu at a new place: the nearest replaced object is %d cells away, last drawn %llu frames and %llu ms ago%s (radius %d)",
                 static_cast<unsigned long long>(next_identity), near_distance, static_cast<unsigned long long>(near_frames),
                 static_cast<unsigned long long>(near_us / 1000), near_expired ? ", expired" : "", radius);
        }
        std::string reason;
        if (!vacant->skeleton.configure(package.bones, 1, ranges, reason)) { note(reason); return nullptr; }
        vacant->used = true; vacant->custom = false; vacant->shown = false; vacant->seen = 0; vacant->bone_sources.clear();
        // Another object's skeleton on the device is not this one's (D-127).
        vacant->ring.reset(); vacant->gpu_frame = false;
        vacant->first_frame = now; vacant->first_present = present; vacant->update_frame = 0;
        vacant->id = next_identity++; vacant->generation = 0;
        vacant->coverage.assign(package.components.size(), 0);
        best = vacant;
    }
    std::memcpy(best->cell, cell, sizeof(cell));
    best->last_frame = now;
    best->last_present = present;
    best->last_us = clock;
    return best;
}

// The identity's merged skeleton on the device (D-117, D-127), made on first use; without one the
// recording imports on the CPU as before. An object that imported on the CPU (the bones switched
// there in between) starts its device skeleton again: what it held is older than the master.
void CharacterGeneration::acquire_ring(Identity& identity)
{
    identity.gpu_frame = false;
    if (!bones_gpu || !bones_gpu->usable() || !live_gpu_bones() || bone_ring_failed) {
        if (identity.ring) { identity.ring.reset(); identity.bone_sources.clear(); }
        return;
    }
    if (!identity.ring) {
        std::string why;
        identity.ring = bones_gpu->ring(package.bones, why);
        if (!identity.ring) { bone_ring_failed = true; note("bone ring: " + why); return; }
    }
    identity.gpu_frame = true;
}

bool CharacterGeneration::ensure_remaps()
{
    if (bone_remaps || bone_remaps_failed) return bool(bone_remaps);
    std::vector<std::vector<uint16_t>> tables;
    // Two per component: LOD 0's, then LOD 1's (BUG-093).
    for (const auto& c : package.components) { tables.push_back(c.remap_lod0); tables.push_back(c.has_remap ? c.remap_lod1 : std::vector<uint16_t>{}); }
    std::string why;
    bone_remaps = bones_gpu->remaps(tables, why);
    if (!bone_remaps) { bone_remaps_failed = true; note("bone remaps: " + why); }
    return bool(bone_remaps);
}

// One component's import as a dispatch in front of the draw's recording (D-127), whatever happens to
// the draw, as the Core imports in the draw's list; false when the source cannot be bound (its
// bounds, alignment or remap table), and the CPU import follows for this draw.
bool CharacterGeneration::prepare_bone_job(Identity& identity, CharacterCommand& owner, uint32_t component_index, const CharacterComponent& component,
                                           bool remapped, size_t needed, const BoundRange& bones, const uint32_t offsets[2], uint8_t dynamic, uint32_t lod)
{
    for (uint32_t p = 0; p < 2; ++p)
        if ((dynamic >> p & 1) && (uint64_t(offsets[p]) * 16 > bones.size || needed > bones.size - uint64_t(offsets[p]) * 16)) return false;
    if (bones.off % bones_gpu->alignment()) return false;
    if (remapped && !ensure_remaps()) return false;
    BoneJob& job = scratch_bone_job;
    job.ring = identity.ring; job.remaps = remapped ? bone_remaps : nullptr; job.component = component_index;
    job.table = component_index * 2 + (lod == 1 ? 1 : 0);
    job.source = bones.buffer; job.source_id = bones.buffer_id; job.source_offset = bones.off; job.source_size = bones.size;
    job.rows[0] = offsets[0]; job.rows[1] = offsets[1];
    job.first = component.first; job.count = component.count; job.poses = dynamic; job.remap = remapped;
    job.recording = owner.recording; job.epoch = owner.recording->epoch();
    owner.used = true;
    owner.add_bone_job(job);
    if (identity.bone_sources.size() < package.components.size()) identity.bone_sources.resize(package.components.size());
    identity.bone_sources[component_index] = job;
    ++counters[C_BONE_JOBS];
    return true;
}

BoneSite CharacterGeneration::site_of(const PipeInfo& pipe)
{
    auto found = sites.find(pipe.id);
    if (found != sites.end()) return found->second;
    BoneSite site;
    if (pipe.recipe && pipe.layout) {
        for (const auto& stage : pipe.recipe->stages) {
            if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT || !stage.shader) continue;
            VertexRepoint out;
            if (repoint_vertex(stage.shader->code, static_cast<uint32_t>(pipe.layout->sets.size()), out) && out.bone_set < kMaxSets)
                site = {true, out.bone_set, out.bone_binding};
        }
    }
    if (sites.size() >= 4096) sites.clear();
    sites[pipe.id] = site;
    return site;
}

// Caller holds mod_mx and res_mx. The Core's facts by its own rules on the vertex module's DX11
// registers (D-076), kept only where they name what the renderer itself reads: the window at
// set 2, binding 0 (the instance entry) and the bone buffer the bone site found. Otherwise none,
// and the original Core leaves the mesh to the game.
// The reference stays valid until the next call.
const CharacterGeneration::SourceFacts& CharacterGeneration::facts_of(const PipeInfo& pipe, const BoneSite& site)
{
    const auto known = source_facts.find(pipe.id);
    if (known != source_facts.end()) return known->second;
    if (source_facts.size() >= 4096) source_facts.clear();
    SourceFacts facts;
    if (pipe.recipe)
        for (const auto& stage : pipe.recipe->stages) {
            static const VkShaderStageFlagBits order[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            const auto at = std::find(std::begin(order), std::end(order), stage.stage);
            if (at == std::end(order) || !stage.shader) continue;
            auto registers = std::make_shared<std::vector<efmi::gpu::SourceRegister>>();
            std::string why;
            if (!efmi::gpu::source_registers(stage.shader->code, *registers, why)) continue;
            facts.registers[size_t(at - std::begin(order))] = registers;
            if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
            const auto core = efmi::gpu::core_source_facts(*registers);
            if (core.config && core.config->set == 2 && core.config->binding == 0) {
                facts.vs_config_slot = core.vs_config_slot;
                facts.rolling_bones = core.rolling_bones && site.ok && core.bones->set == site.set && core.bones->binding == site.binding;
            }
        }
    if (!facts.rolling_bones) ++facts_unknown;
    return source_facts[pipe.id] = std::move(facts);
}

} // namespace efvk
