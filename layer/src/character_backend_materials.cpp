// A package's materials (CharacterGeneration's part): the frame gate, the texture queries of a draw's
// lists, the rules and plans of a fragment shader, the images a draw binds, and what a retired pipeline
// leaves behind.
#include "character_backend_internal.h"

namespace efvk {
namespace {
// Fragment shaders without a profile in the package whose rules are derived from the module.
constexpr size_t kDerivedProfiles = 256;
} // namespace

void CharacterGeneration::begin_gate_frame(uint64_t present)
{
    gate_frame = present;
    last_frame_images.swap(frame_images); frame_images.clear();
    last_variants_pending = frame_variants_pending; frame_variants_pending = false;
    gate_open = !last_variants_pending;
    if (gate_open && textures && !textures->ensure(last_frame_images)) {
        // A texture of the frame before is not resident (evicted, demoted, or its load refused):
        // the recording waits for it, as 3DMigoto loads a Mod's texture when it is first bound,
        // rather than drawing the game's mesh meanwhile (the user's bar: a loaded Mod never shows
        // the original again). The frame goes to the game only when the wait failed.
        auto& bindings = scratch_image_bindings;
        textures->bind(last_frame_images, bindings);
        gate_open = textures->ensure(last_frame_images);
        if (!gate_open && !gate_logged) {
            gate_logged = true;
            std::string why;
            for (const auto& b : bindings) if (!b.view) { why = b.failed ? b.reason : "still loading after the wait"; break; }
            logf("character frame gated: textures of %zu images not resident (%s); the game draws this package this frame",
                 last_frame_images.size(), why.c_str());
        }
    }
    if (!gate_open) ++counters[C_GATED_FRAMES];
}

bool CharacterGeneration::refresh_materials()
{
    variant_lookup.clear();
    // A live plan added a draw (a key, a texture section): every profile gets its plan. The rules
    // cover every slot (D-101), so nothing is derived again and the variants of the earlier draws
    // keep their patches.
    {
        std::set<uint32_t> slots(material_slots.begin(), material_slots.end());
        for (const auto& draw : package.pixels) for (const auto& pixel : draw) slots.insert(pixel.slot);
        material_slots.assign(slots.begin(), slots.end());
    }
    std::set<uint32_t> counts;
    std::string reason;
    if (!prepare_character_materials(package, materials, counts, reason)) return refuse(C_MATERIAL, reason);
    for (auto& derived : derived_profiles) {
        CharacterMaterialProfile profile{derived->code, derived->rules};
        if (!prepare_character_profile(package, profile, derived->plans, counts, reason)) return refuse(C_MATERIAL, reason);
    }
    for (auto count : counts) if (!image_counts.count(count)) return refuse(C_MATERIAL, "runtime material lacks a prepared arena");
    return true;
}

// Material rules of a fragment module for the slots the package binds, by the DX11 register rule
// (D-076): a slot whose t register the module does not declare is absent, as an unread slot is
// under DX11; a single image is bound there. False for a module the rule cannot read or a slot on
// something else than one image (left to the application's images, never guessed).
bool CharacterGeneration::rule_rules(const std::vector<uint32_t>& code, std::vector<CharacterMaterialRule>& rules, std::string& reason) const
{
    std::vector<efmi::gpu::SourceRegister> registers;
    if (!efmi::gpu::source_registers(code, registers, reason)) return false;
    rules.clear();
    // Every DX11 pixel slot, whatever the Mod binds today (D-101): rules that depended on the slots
    // bound so far were derived again whenever a plan bound a new one, and the variants compiled
    // again with them, the character flickering to the game's mesh each time (2-3 times after a
    // switch in the character menu).
    for (uint32_t slot = 0; slot < 128; ++slot) {
        const auto* r = efmi::gpu::find_source_register(registers, 't', slot);
        CharacterMaterialRule rule;
        rule.slot = slot;
        // A slot that holds no single image in this shader (nothing, a buffer, an array) takes no
        // file image here, as one the shader does not declare, and the rest of the slots still do.
        if (!r || r->kind != efmi::gpu::SourceRegister::ImageSrv || r->count != 1) {
            rule.kind = CharacterMaterialRule::Absent; rules.push_back(rule); continue;
        }
        // The device-bound compatibility policy of the package covers a 2D file at a 3D image.
        rule.kind = package.material_vendor && r->dim == 2 ? CharacterMaterialRule::ZeroFile : CharacterMaterialRule::Image;
        rule.set = r->set; rule.binding = r->binding;
        rules.push_back(rule);
    }
    return true;
}

// Caller holds mod_mx and res_mx shared. The application's texture at DX11 pixel slot `slot` of the
// draw, by its original resource hash (D-077) and description (D-138): the slot's binding comes from
// the fragment module's registers (D-076), the image from the tracked descriptor. A slot the module
// does not declare reads nothing here; a resource whose hash cannot be known answers a bound slot
// without a hash, so no hash section matches it and the draw is still replaced (counted in
// texture_unknown, reason noted); one the layer cannot describe meets no match_* section.
bool CharacterGeneration::pixel_texture(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string& why)
{
    // Inside record(): one draw's bindings do not change while it is planned, so a slot's answer
    // holds for the rest of the draw (D-108).
    const bool cacheable = lookups.pipeline == r.pipeline && lookups.pipe && slot < 128;
    if (cacheable && (lookups.answered[slot >> 6] >> (slot & 63) & 1)) {
        ++texture_queries; texture = lookups.answers[slot]; return true;
    }
    const bool answered = pixel_texture_uncached(r, slot, texture, why);
    if (cacheable && answered) { lookups.answered[slot >> 6] |= 1ull << (slot & 63); lookups.answers[slot] = texture; }
    return answered;
}

bool CharacterGeneration::pixel_texture_uncached(const DrawRec& r, unsigned slot, efmi::frontend::RuntimeTexture& texture, std::string&)
{
    ++texture_queries;
    texture = {};
    bool& bound = texture.bound;
    uint32_t& hash = texture.hash;
    const auto unknown = [&](const char* why) { bound = true; hash = 0; ++texture_unknown; texture_reason = why; return true; };
    const PipeInfo* info = lookups.pipeline == r.pipeline ? lookups.pipe : nullptr;
    if (!info) {
        const auto pipe = device->pipes.find(r.pipeline);
        if (pipe != device->pipes.end()) info = &pipe->second;
    }
    if (!info || !info->recipe) return unknown("unknown pipeline");
    const FragmentRegisters* registers_known = lookups.pipe == info ? static_cast<const FragmentRegisters*>(lookups.registers) : nullptr;
    auto known = fragment_registers.end();
    if (!registers_known) known = fragment_registers.find(info->id);
    if (!registers_known && known == fragment_registers.end()) {
        if (fragment_registers.size() >= 8192) fragment_registers.clear();
        FragmentRegisters registers;
        registers.readable = true; // a pipeline without a fragment stage reads no texture
        for (const auto& stage : info->recipe->stages)
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                std::string why;
                registers.readable = efmi::gpu::source_registers(stage.shader->code, registers.list, why);
            }
        known = fragment_registers.emplace(info->id, std::move(registers)).first;
    }
    if (!registers_known) {
        registers_known = &known->second;
        if (lookups.pipe == info) lookups.registers = registers_known;
    }
    if (!registers_known->readable) return unknown("the fragment module is not read by the register rule");
    const auto* reg = efmi::gpu::find_source_register(registers_known->list, 't', slot);
    if (!reg) return true;
    if (reg->kind != efmi::gpu::SourceRegister::ImageSrv || reg->count != 1 || reg->set >= kMaxSets)
        return unknown("a pixel slot is not a single image");
    const char* why = nullptr;
    bool empty = false;
    const ImageInfo* image = bound_image(r, reg->set, reg->binding, empty, why);
    if (empty) return true; // a null view: an empty slot
    if (!image) return unknown(why);
    texture.described = dx11_texture_desc(*image, texture.description);
    if (!dx11_texture_hash(*image, hash)) return unknown("an image without a known original hash");
    bound = true;
    return true;
}

// Caller holds res_mx. The image the draw binds at element 0 of (set, binding): null with `empty`
// for a null view, null with `why` when the layer cannot tell.
const ImageInfo* CharacterGeneration::bound_image(const DrawRec& r, uint32_t set_index, uint32_t binding, bool& empty, const char*& why)
{
    empty = false;
    if (set_index >= kMaxSets) { why = "a set outside the tracked slots"; return nullptr; }
    const SetBind& bind = r.sets[set_index];
    const DescriptorSetInfo* contents = bind.pushed.get();
    if (!contents && bind.id) {
        if (lookups.pipe && lookups.set == bind.set && lookups.set_id == bind.id) contents = lookups.contents;
        else {
            const auto set = device->descriptor_sets.find(bind.set);
            if (set != device->descriptor_sets.end() && set->second.id == bind.id) contents = &set->second;
            if (lookups.pipe) { lookups.set = bind.set; lookups.set_id = bind.id; lookups.contents = contents; }
        }
    }
    if (bind.unsupported || !contents) { why = "the descriptor set of a pixel slot is not tracked"; return nullptr; }
    const auto value = contents->values.find({binding, 0});
    if (value == contents->values.end() || !value->second.known) { why = "a pixel slot's descriptor is unknown"; return nullptr; }
    if (!value->second.image_view) { empty = true; return nullptr; }
    const auto view = device->image_views.find(value->second.image_view);
    if (view == device->image_views.end() || view->second.id != value->second.image_view_id) { why = "unknown image view"; return nullptr; }
    const auto image = device->images.find(view->second.image);
    if (image == device->images.end() || image->second.id != view->second.image_id) { why = "unknown image"; return nullptr; }
    return &image->second;
}

// Caller holds res_mx and mod_mx. RabbitFX's roles in this draw (D-082): the fragment module's
// samples and the images the draw binds at them. False when the module is not read.
bool CharacterGeneration::texture_roles(const DrawRec& r, int32_t roles[3], std::string& reason)
{
    roles[0] = roles[1] = roles[2] = -1;
    const auto pipe = device->pipes.find(r.pipeline);
    if (pipe == device->pipes.end() || !pipe->second.recipe) { reason = "unknown pipeline"; return false; }
    auto known = fragment_samples.find(pipe->second.id);
    if (known == fragment_samples.end()) {
        if (fragment_samples.size() >= 8192) fragment_samples.clear();
        FragmentSamples samples;
        samples.readable = true; // a pipeline without a fragment stage samples nothing
        for (const auto& stage : pipe->second.recipe->stages)
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                std::string why;
                samples.readable = efmi::gpu::role_samples(stage.shader->code, samples.list, why);
            }
        for (const auto& sample : samples.list) if (sample.set < kMaxSets) samples.sets |= 1u << sample.set;
        known = fragment_samples.emplace(pipe->second.id, std::move(samples)).first;
    }
    if (!known->second.readable) { reason = "the fragment module is not read"; return false; }
    RoleKey key;
    key.pipe = pipe->second.id;
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        if (!(known->second.sets >> i & 1)) continue;
        const SetBind& bind = r.sets[i];
        if (bind.pushed) {
            key.identity[i] = uint64_t(reinterpret_cast<uintptr_t>(bind.pushed.get())); key.generation[i] = bind.pushed->generation;
        } else if (bind.id && !bind.unsupported) {
            key.identity[i] = uint64_t(bind.id) << 1 | 1;
            const auto set = device->descriptor_sets.find(bind.set);
            if (set != device->descriptor_sets.end() && set->second.id == bind.id) key.generation[i] = set->second.generation + 1;
        }
    }
    ++role_queries;
    if (const auto cached = role_cache.find(key); cached != role_cache.end()) {
        std::copy(cached->second.roles, cached->second.roles + 3, roles);
        if (roles[0] >= 0) ++role_draws;
        return true;
    }
    const auto found = efmi::gpu::texture_roles(known->second.list, [&](const efmi::gpu::RoleSample& s) {
        efmi::gpu::RoleTexture texture;
        const char* why = nullptr;
        bool empty = false;
        if (const ImageInfo* image = bound_image(r, s.set, s.binding, empty, why)) {
            texture.known = true; texture.format = image->format;
            texture.width = image->extent.width; texture.height = image->extent.height; texture.levels = image->levels;
        }
        return texture;
    });
    roles[0] = found.diffuse; roles[1] = found.lightmap; roles[2] = found.normal;
    if (role_cache.size() >= 4096) role_cache.clear();
    std::copy(roles, roles + 3, role_cache[key].roles);
    if (found.diffuse >= 0) ++role_draws;
    return true;
}

// Caller holds res_mx. The derived profile of a fragment shader the package has no profile for,
// or none: then the mesh is the mod's and the images stay the application's.
uint32_t CharacterGeneration::derive(const PipeInfo& pipe)
{
    const auto known = derived_for_pipeline.find(pipe.id);
    if (known != derived_for_pipeline.end()) return known->second;
    if (derived_for_pipeline.size() >= 8192) derived_for_pipeline.clear();
    uint32_t& result = derived_for_pipeline[pipe.id];
    result = kCharacterNone;
    const std::vector<uint32_t>* code = nullptr;
    for (const auto& stage : pipe.recipe->stages)
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) code = &stage.shader->code;
    if (!code) return result;
    const uint32_t crc = crc32c(0, code->data(), code->size() * 4);
    for (uint32_t i = 0; i < derived_profiles.size(); ++i)
        if (derived_profiles[i]->crc == crc && derived_profiles[i]->code == *code) return result = i;
    std::string reason;
    auto created = std::make_unique<DerivedProfile>();
    if (!rule_rules(*code, created->rules, reason)) {
        char name[16];
        snprintf(name, sizeof(name), "%08x", crc);
        return refuse_profile(std::string("character material: fragment ") + name + " keeps the application's images: " + reason, result);
    }
    if (derived_profiles.size() >= kDerivedProfiles) return refuse_profile("character material: too many derived profiles", result);
    created->crc = crc; created->code = *code; created->slots = material_slots;
    CharacterMaterialProfile source;
    source.code = created->code; source.rules = created->rules;
    std::set<uint32_t> counts;
    if (!prepare_character_profile(package, source, created->plans, counts, reason)) {
        return refuse_profile("character material: pipeline " + std::to_string(pipe.id) + " derived profile refused: " + reason, result);
    }
    // The arenas of a command are made for the image counts known at setup.
    for (uint32_t count : counts)
        if (!image_counts.count(count))
            return refuse_profile("character material: derived profile of pipeline " + std::to_string(pipe.id) + " needs " +
                                  std::to_string(count) + " images, no arena has that count", result);
    size_t images = 0;
    for (const auto& rule : created->rules) if (rule.kind != CharacterMaterialRule::Absent) ++images;
    logf("character material: fragment %08x (pipeline %u) takes %zu image rules from the DX11 register rule", crc, pipe.id, images);
    derived_profiles.push_back(std::move(created));
    return result = static_cast<uint32_t>(derived_profiles.size() - 1);
}

bool CharacterGeneration::init_materials()
{
    using namespace efmi;
    if (device->material_queues.empty()) { last_reason = device->material_queue_reason; return false; }
    if (package.material_vendor && (package.material_vendor != properties.vendorID || package.material_device != properties.deviceID)) {
        last_reason = "material dimension compatibility belongs to a different device"; return false;
    }
    if (!prepare_character_materials(package, materials, image_counts, last_reason)) return false;
    {
        std::set<uint32_t> slots;
        for (const auto& draw : package.pixels) for (const auto& pixel : draw) slots.insert(pixel.slot);
        material_slots.assign(slots.begin(), slots.end());
    }
    // The package's own profiles came from paired captures; the register rule must give the same
    // bindings for the slots they place. A difference is reported, the profile stays in use.
    for (uint32_t i = 0; i < package.profiles.size(); ++i) {
        std::vector<CharacterMaterialRule> rules;
        std::string why;
        if (!rule_rules(package.profiles[i].code, rules, why)) { ++rule_mismatches; logf("character material: profile %u not read by the register rule: %s", i, why.c_str()); continue; }
        for (const auto& own : package.profiles[i].rules)
            for (const auto& rule : rules)
                if (rule.slot == own.slot && (rule.kind != own.kind || rule.set != own.set || rule.binding != own.binding)) {
                    ++rule_mismatches;
                    logf("character material: profile %u slot %u is %u.%u by capture, %u.%u by the register rule", i, own.slot,
                         own.set, own.binding, rule.set, rule.binding);
                }
    }
    // The images as the device's residency holds them (D-094): declared here, loaded on first bind.
    if (!textures || !textures->usable()) { last_reason = textures ? textures->reason() : "textures need the device's residency"; return false; }
    for (size_t i = 0; i < package.images.size(); ++i) {
        const auto& path = package.images[i];
        // A file that does not load is the original's null resource: no plan binds it (D-139).
        if (program && program->image_unloadable(path)) { image_ids.push_back(TextureResidency::kNone); continue; }
        std::string why;
        const auto id = textures->declare(path, why, i < package.image_flags.size() ? package.image_flags[i] : 0u);
        if (id == TextureResidency::kNone) { last_reason = why; return false; }
        image_ids.push_back(id);
    }
    // D-092: activating brings the mesh and the first snapshot batches up; the textures come on demand.
    activation_bytes = package.payload_bytes + (32ull << 20);
    return true;
}

bool CharacterGeneration::material_draws(const PipeInfo& pipe, uint32_t section, const DrawRec& r, uint64_t now,
                                      const CharacterList& list,
                                      std::vector<const StaticMaterial*>& plans, std::vector<std::vector<SnapshotImage>>& views,
                                      uint32_t& origin)
{
    static const StaticMaterial inherited{true, false, {}, {}, {}};
    const uint32_t pipeline_id = pipe.id;
    const GraphicsRecipe& recipe = *pipe.recipe;
    const PipelineLayoutInfo& layout = *pipe.layout;
    origin = 0;
    uint32_t profile = kCharacterNone;
    bool no_fragment = true;
    if (package.materials) {
        const auto found = material_profiles.find(pipeline_id);
        if (found != material_profiles.end()) profile = found->second;
        else {
            for (const auto& stage : recipe.stages) if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) {
                no_fragment = false;
                for (uint32_t i = 0; i < package.profiles.size(); ++i)
                    if (package.profiles[i].code == stage.shader->code) { profile = i; break; }
            }
            if (no_fragment) profile = kCharacterNone - 1;
            if (profile == kCharacterNone && layout.complete) {
                // A fragment shader whose layout holds no image for it cannot read a texture: every
                // slot the mod binds is absent there, as for the pass that tests visibility with
                // depth only. Nothing to replace; the mesh is still the mod's.
                bool images = false, known = true;
                for (const auto& set : layout.sets) {
                    if (!set || !set->complete) { known = false; break; }
                    for (const auto& b : set->bindings) {
                        const uint32_t type = b.second.type;
                        if ((b.second.stages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                            (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                             type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                             type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                             type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) images = true;
                    }
                }
                if (known && !images) profile = kCharacterNone - 1;
            }
            if (material_profiles.size() >= 4096) material_profiles.clear();
            material_profiles[pipeline_id] = profile;
        }
    }
    // The table of plans: the package's own profile; for a fragment shader without one the rules
    // the DX11 register rule reads off its module; failing that none, and the application's images
    // stay on the mod's mesh. Keeping the game's mesh there instead would mix the two models on screen.
    const std::vector<StaticMaterial>* table = nullptr;
    bool binds = false;
    for (uint32_t i = 0; i < list.count; ++i) binds |= package.materials && !package.pixels[list.first + i].empty();
    if (binds && profile < package.profiles.size()) {
        table = &materials[profile];
    } else if (binds && profile == kCharacterNone) {
        const uint32_t index = derive(pipe);
        if (index != kCharacterNone) table = &derived_profiles[index]->plans;
        origin = table ? 1 : 2;
    }
    // Decide the entire logical transaction before obtaining any views or issuing a command.
    for (uint32_t i = 0; i < list.count; ++i) {
        const uint32_t index = list.first + i;
        const StaticMaterial* plan = &inherited;
        if (table && !package.pixels[index].empty()) {
            plan = &(*table)[index];
            if (!plan->supported) return refuse(C_MATERIAL, plan->reason);
        }
        plans.push_back(plan);
    }
    // Queue only this transaction's images together; the owned upload queue prepares them in
    // parallel before the draw uses them. Recordings retain their independent GPU owners.
    auto& selected = scratch_image_ids; selected.clear();
    for (const auto* plan : plans) for (uint32_t asset : plan->assets) {
        if (!textures || asset >= image_ids.size()) return refuse(C_IMAGE, "character texture outside the package's images");
        selected.push_back(image_ids[asset]); frame_image(image_ids[asset]);
    }
    auto& bindings = scratch_image_bindings;
    if (!selected.empty()) textures->bind(selected, bindings);
    bool pending = false;
    size_t binding = 0;
    if (views.size() < plans.size()) views.resize(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        const auto* plan = plans[i];
        auto& draw_views = views[i]; draw_views.clear();
        for (size_t asset = 0; asset < plan->assets.size(); ++asset) {
            const auto& bound = bindings[binding++];
            if (bound.failed) return refuse(C_IMAGE, "character texture: " + bound.reason);
            if (!bound.view) { pending = true; continue; }
            draw_views.push_back({bound.view, bound.owner});
        }
    }
    // An image still on its way after the bind's wait (its load refused by the budget, or the wait
    // timed out) closes the package's frame (D-097); logged once, as this shows the game's mesh.
    if (pending) {
        gate_open = false; ++counters[C_IMAGE_PENDING];
        if (!gate_logged) { gate_logged = true; logf("character frame gated: a texture of the draw is not resident after the wait; the game draws this package this frame"); }
        return false;
    }
    return true;
}

void generation_retire(CharacterGeneration& backend, uint32_t pipeline_id)
{
    std::lock_guard<std::mutex> lock(backend.mod_mx);
    // The fast lookup is another strong owner in addition to the device cache (BUG-041).
    // Recorded commands retain their variants independently through GPU completion.
    for (auto it = backend.variant_lookup.begin(); it != backend.variant_lookup.end();) {
        if (std::get<0>(it->first) == pipeline_id) it = backend.variant_lookup.erase(it);
        else ++it;
    }
    backend.sites.erase(pipeline_id);
    backend.material_profiles.erase(pipeline_id);
    backend.derived_for_pipeline.erase(pipeline_id);
    backend.fragment_registers.erase(pipeline_id);
    backend.source_facts.erase(pipeline_id);
}

} // namespace efvk
