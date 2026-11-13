// Metadata for shader modules, image resources and descriptor bindings.
// Ordinary sets resolve at submit; pushes at record. Retained immutable samplers defer destruction.
#include "state.h"
#include "json.h"
#include "texture_identity.h"

#include <algorithm>
#include <cstddef>

namespace efvk {

template <typename T> static const T* find_next(const void* next, VkStructureType type)
{
    for (auto p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
        if (p->sType == type)
            return reinterpret_cast<const T*>(p);
    return nullptr;
}

template <typename Map, typename Handle> static uint32_t resource_id(const Map& map, Handle handle)
{
    auto it = map.find(handle);
    return it == map.end() ? 0 : it->second.id;
}

// Nothing is recorded for the passive device, nor when resource tracking is switched off.
static bool skip(const Device* dev)
{
    return dev->passive || !g_cfg.track_resources;
}

static std::shared_ptr<const ShaderInfo> make_shader_locked(Device* dev, const VkShaderModuleCreateInfo* info)
{
    auto s = std::make_shared<ShaderInfo>();
    s->id = dev->next_resource_id++;
    const size_t words = info->pCode ? info->codeSize / 4 : 0;
    const uint64_t bytes = static_cast<uint64_t>(words) * 4;
    s->size = static_cast<uint32_t>(bytes);
    s->hash = crc32c(0, info->pCode, static_cast<size_t>(bytes));
    // An application can have tens of thousands of modules. Keep their code only up to the budget;
    // hash and size are kept for all of them.
    const uint64_t kept = dev->shader_bytes->load(std::memory_order_relaxed);
    if (bytes && bytes <= g_cfg.shader_budget && kept <= g_cfg.shader_budget - bytes) {
        s->code.assign(info->pCode, info->pCode + words);
        s->retained_total = dev->shader_bytes;
        dev->shader_bytes->fetch_add(bytes, std::memory_order_relaxed);
    } else if (bytes) {
        dev->shaders_not_retained++;
    }
    dev->shader_by_id[s->id] = s;
    return s;
}

std::vector<uint32_t> shader_ids_of(const PipeInfo& pipe)
{
    std::vector<uint32_t> ids;
    for (const ShaderStage& s : pipe.shaders)
        if (s.shader) ids.push_back(s.shader->id);
    return ids;
}

// Drops the id entries of shaders that nothing refers to any more. Looking only at the given
// ids keeps the destruction of a module or pipeline independent of how many shaders exist.
void forget_shaders_locked(Device* dev, const std::vector<uint32_t>& ids)
{
    for (uint32_t id : ids) {
        auto it = dev->shader_by_id.find(id);
        if (it != dev->shader_by_id.end() && it->second.expired()) dev->shader_by_id.erase(it);
    }
}

void on_create_shader(Device* dev, VkShaderModule h, const VkShaderModuleCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto& slot = dev->shaders[h];
    if (slot) {
        // The handle was reused without a destroy being seen. Let go of the old module first, so
        // that its bytes are back in the budget before the new one is measured against it.
        std::vector<uint32_t> replaced{slot->id};
        slot.reset();
        forget_shaders_locked(dev, replaced);
    }
    slot = make_shader_locked(dev, info);
}

void on_destroy_shader(Device* dev, VkShaderModule h)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto it = dev->shaders.find(h);
    if (it == dev->shaders.end()) return;
    std::vector<uint32_t> ids;
    if (it->second) ids.push_back(it->second->id);
    dev->shaders.erase(it);
    forget_shaders_locked(dev, ids);
}

void track_stages_locked(Device* dev, PipeInfo& pipe, uint32_t count, const VkPipelineShaderStageCreateInfo* stages)
{
    for (uint32_t i = 0; i < count; i++) {
        const auto& in = stages[i];
        ShaderStage s;
        s.stage = in.stage;
        s.entry = in.pName ? in.pName : "";
        auto it = dev->shaders.find(in.module);
        if (it != dev->shaders.end()) s.shader = it->second;
        else if (!g_cfg.track_resources) {
            // Reduced tracking keeps the stage and entry point, not the module passed inline.
        } else if (auto ci = find_next<VkShaderModuleCreateInfo>(in.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO))
            s.shader = make_shader_locked(dev, ci);
        pipe.shaders.push_back(std::move(s));
    }
}

void on_create_image(Device* dev, VkImage h, const VkImageCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    ImageInfo image;
    image.id = dev->next_resource_id++;
    image.type = info->imageType;
    image.format = info->format;
    image.extent = info->extent;
    image.levels = info->mipLevels;
    image.layers = info->arrayLayers;
    image.samples = info->samples;
    image.usage = info->usage;
    image.flags = info->flags;
    dev->images[h] = image;
}

void on_create_image_view(Device* dev, VkImageView h, const VkImageViewCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    ImageViewInfo view;
    view.id = dev->next_resource_id++;
    view.image = info->image;
    view.image_id = resource_id(dev->images, info->image);
    view.type = info->viewType;
    view.format = info->format;
    view.components = info->components;
    view.range = info->subresourceRange;
    dev->image_views[h] = view;
}

void on_create_buffer_view(Device* dev, VkBufferView h, const VkBufferViewCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    BufferViewInfo view;
    view.id = dev->next_resource_id++;
    view.buffer = info->buffer;
    view.buffer_id = resource_id(dev->bufs, info->buffer);
    view.format = info->format;
    view.offset = info->offset;
    view.range = info->range;
    dev->buffer_views[h] = view;
}

void on_create_sampler(Device* dev, VkSampler h, const VkSamplerCreateInfo* info, const VkAllocationCallbacks* alloc)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    SamplerInfo s;
    s.id = dev->next_resource_id++;
    s.mag = info->magFilter;
    s.min = info->minFilter;
    s.mipmap = info->mipmapMode;
    s.address[0] = info->addressModeU;
    s.address[1] = info->addressModeV;
    s.address[2] = info->addressModeW;
    // A custom allocator's user data may expire when the application destroys the object.
    // Such samplers remain tracked but cannot enter an asynchronously retained variant.
    if (!alloc && dev->d.DestroySampler) {
        s.lifetime = std::make_shared<SamplerLifetime>();
        s.lifetime->device = dev->handle; s.lifetime->handle = h; s.lifetime->destroy = dev->d.DestroySampler;
    }
    dev->samplers[h] = s;
}

bool on_destroy_sampler(Device* dev, VkSampler h)
{
    if (skip(dev)) return false;
    std::shared_ptr<SamplerLifetime> owner;
    {
        std::unique_lock<std::shared_mutex> lock(dev->res_mx);
        const auto it = dev->samplers.find(h);
        if (it == dev->samplers.end()) return false;
        owner = it->second.lifetime;
        if (owner) owner->requested = true;
        dev->samplers.erase(it);
    }
    return bool(owner); // The last owner, possibly this one, performs the actual driver call.
}

#define DESTROY_RESOURCE(fn, Handle, map) \
    void fn(Device* dev, Handle h) { \
        if (skip(dev)) return; \
        std::unique_lock<std::shared_mutex> lock(dev->res_mx); \
        dev->map.erase(h); \
    }
DESTROY_RESOURCE(on_destroy_image, VkImage, images)
DESTROY_RESOURCE(on_destroy_image_view, VkImageView, image_views)
DESTROY_RESOURCE(on_destroy_buffer_view, VkBufferView, buffer_views)
DESTROY_RESOURCE(on_destroy_pipeline_layout, VkPipelineLayout, pipeline_layouts)
DESTROY_RESOURCE(on_destroy_descriptor_template, VkDescriptorUpdateTemplate, descriptor_templates)
#undef DESTROY_RESOURCE

static bool is_dynamic(uint32_t type)
{
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static bool is_buffer(uint32_t type)
{
    return is_dynamic(type) || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

// Layout contents interned once at creation: equal contents get one id for the process.
static std::mutex g_intern_mx;
static std::unordered_map<std::string, uint64_t> g_interned;
template <class T> static void put_key(std::string& key, const T& value) { key.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
static uint64_t intern(const std::string& key)
{
    std::lock_guard<std::mutex> lock(g_intern_mx);
    return g_interned.emplace(key, uint64_t(g_interned.size() + 1)).first->second;
}

bool on_destroy_set_layout(Device* dev, VkDescriptorSetLayout h)
{
    if (skip(dev)) return false;
    std::shared_ptr<const DescriptorLayoutInfo> held;
    {
        std::unique_lock<std::shared_mutex> lock(dev->res_mx);
        const auto it = dev->set_layouts.find(h);
        if (it == dev->set_layouts.end()) return false;
        held = it->second;
        if (held->lifetime) held->lifetime->requested = true;
        dev->set_layouts.erase(it);
    }
    return bool(held->lifetime);
}

void on_create_set_layout(Device* dev, VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo* info, const VkAllocationCallbacks* alloc)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto layout = std::make_shared<DescriptorLayoutInfo>();
    layout->id = dev->next_resource_id++;
    layout->flags = info->flags;
    if (!alloc && dev->d.DestroyDescriptorSetLayout) {
        layout->lifetime = std::make_shared<SetLayoutLifetime>();
        layout->lifetime->device = dev->handle; layout->lifetime->handle = h;
        layout->lifetime->destroy = dev->d.DestroyDescriptorSetLayout;
    }
    auto flags = find_next<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
        info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            layout->complete = false;
    for (uint32_t i = 0; i < info->bindingCount; i++) {
        const auto& in = info->pBindings[i];
        DescriptorBinding b;
        b.type = in.descriptorType;
        b.count = in.descriptorCount;
        b.stages = in.stageFlags;
        if (flags && i < flags->bindingCount) b.flags = flags->pBindingFlags[i];
        if (in.pImmutableSamplers &&
            (b.type == VK_DESCRIPTOR_TYPE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
        {
            b.immutable_samplers.assign(in.pImmutableSamplers, in.pImmutableSamplers + b.count);
            for (auto sampler : b.immutable_samplers) {
                const auto found = dev->samplers.find(sampler);
                b.immutable_owners.push_back(found == dev->samplers.end() ? nullptr : found->second.lifetime);
            }
        }
        layout->bindings[in.binding] = std::move(b);
    }
    if (layout->complete) {
        std::string key = "S";
        put_key(key, layout->flags);
        for (const auto& kv : layout->bindings) {
            const auto& b = kv.second;
            put_key(key, kv.first); put_key(key, b.type); put_key(key, b.count); put_key(key, b.stages); put_key(key, b.flags);
            put_key(key, uint32_t(b.immutable_samplers.size()));
            for (VkSampler sampler : b.immutable_samplers) put_key(key, sampler);
        }
        layout->canonical = intern(key);
    }
    dev->set_layouts[h] = std::move(layout);
}

void on_create_pipeline_layout(Device* dev, VkPipelineLayout h, const VkPipelineLayoutCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto layout = std::make_shared<PipelineLayoutInfo>();
    layout->id = dev->next_resource_id++;
    layout->flags = info->flags;
    layout->complete = info->pNext == nullptr;
    dev->max_layout_sets = std::max(dev->max_layout_sets, info->setLayoutCount);
    for (uint32_t i = 0; i < info->setLayoutCount; i++) {
        auto it = dev->set_layouts.find(info->pSetLayouts[i]);
        layout->sets.push_back(it == dev->set_layouts.end() ? nullptr : it->second);
    }
    if (info->pushConstantRangeCount)
        layout->pushes.assign(info->pPushConstantRanges, info->pPushConstantRanges + info->pushConstantRangeCount);
    std::sort(layout->pushes.begin(), layout->pushes.end(), [](const auto& a, const auto& b) {
        if (a.stageFlags != b.stageFlags) return a.stageFlags < b.stageFlags;
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.size < b.size;
    });
    std::string key = "P";
    put_key(key, layout->flags); put_key(key, uint32_t(layout->pushes.size()));
    for (const auto& push : layout->pushes) { put_key(key, push.stageFlags); put_key(key, push.offset); put_key(key, push.size); }
    bool known = layout->complete;
    for (const auto& set : layout->sets) {
        known = known && set && set->canonical;
        if (known) put_key(key, set->canonical);
        layout->prefix.push_back(known ? intern(key) : 0);
    }
    dev->pipeline_layouts[h] = std::move(layout);
}

static bool same_set_layout(const DescriptorLayoutInfo& a, const DescriptorLayoutInfo& b)
{
    return &a == &b || (a.canonical && a.canonical == b.canonical);
}

static bool compatible(const std::shared_ptr<const PipelineLayoutInfo>& a,
                       const std::shared_ptr<const PipelineLayoutInfo>& b, uint32_t slot)
{
    if (!a || !b || slot >= a->sets.size() || slot >= b->sets.size()) return false;
    if (a == b) return true;
    return slot < a->prefix.size() && slot < b->prefix.size() && a->prefix[slot] && a->prefix[slot] == b->prefix[slot];
}

void on_allocate_sets(Device* dev, const VkDescriptorSetAllocateInfo* info, const VkDescriptorSet* sets)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto variable = find_next<VkDescriptorSetVariableDescriptorCountAllocateInfo>(
        info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);
    for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
        DescriptorSetInfo set;
        set.id = dev->next_resource_id++;
        set.pool = info->descriptorPool;
        auto lit = dev->set_layouts.find(info->pSetLayouts[i]);
        if (lit != dev->set_layouts.end()) {
            set.layout = lit->second;
            for (const auto& kv : set.layout->bindings) {
                const auto& b = kv.second;
                uint32_t count = b.count;
                if (b.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
                    count = variable && i < variable->descriptorSetCount ? variable->pDescriptorCounts[i] : 0;
                set.counts[kv.first] = count;
                // Immutable samplers are initialized by allocation, without a write.
                for (uint32_t k = 0; k < count && k < b.immutable_samplers.size(); k++) {
                    DescriptorElement e;
                    e.type = b.type;
                    e.known = b.type == VK_DESCRIPTOR_TYPE_SAMPLER;
                    e.sampler = b.immutable_samplers[k];
                    e.sampler_id = resource_id(dev->samplers, e.sampler);
                    set.values[{kv.first, k}] = e;
                }
            }
        }
        // A handle that is still known belongs to a set whose release was not seen.
        auto old = dev->descriptor_sets.find(sets[i]);
        if (old != dev->descriptor_sets.end() && old->second.pool != set.pool) {
            auto owner = dev->pool_sets.find(old->second.pool);
            if (owner != dev->pool_sets.end()) owner->second.erase(sets[i]);
        }
        dev->pool_sets[set.pool].insert(sets[i]);
        dev->descriptor_sets[sets[i]] = std::move(set);
    }
}

void on_free_sets(Device* dev, uint32_t count, const VkDescriptorSet* sets)
{
    if (skip(dev) || !sets) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    for (uint32_t i = 0; i < count; i++) {
        auto it = dev->descriptor_sets.find(sets[i]);
        if (it == dev->descriptor_sets.end()) continue;
        auto owner = dev->pool_sets.find(it->second.pool);
        if (owner != dev->pool_sets.end()) owner->second.erase(sets[i]);
        dev->descriptor_sets.erase(it);
    }
}

void on_clear_descriptor_pool(Device* dev, VkDescriptorPool pool)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto owner = dev->pool_sets.find(pool);
    if (owner == dev->pool_sets.end()) return;
    for (VkDescriptorSet h : owner->second) {
        auto it = dev->descriptor_sets.find(h);
        if (it != dev->descriptor_sets.end() && it->second.pool == pool) dev->descriptor_sets.erase(it);
    }
    dev->pool_sets.erase(owner);
}

// A write/copy can span consecutive bindings (including zero-sized ones).
static bool advance_element(const DescriptorSetInfo& set, uint32_t& binding, uint32_t& element)
{
    auto it = set.counts.lower_bound(binding);
    while (it != set.counts.end()) {
        if (element < it->second) { binding = it->first; return true; }
        element -= it->second;
        ++it;
    }
    return false;
}

static void set_element(Device* dev, DescriptorSetInfo& set, uint32_t binding, uint32_t element, DescriptorElement value)
{
    const auto& b = set.layout->bindings.at(binding);
    if (element < b.immutable_samplers.size()) {
        value.sampler = b.immutable_samplers[element];
        value.sampler_id = resource_id(dev->samplers, value.sampler);
        if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) value.known = true;
    }
    set.values[{binding, element}] = value;
}

static size_t descriptor_data_size(uint32_t type)
{
    if (is_buffer(type)) return sizeof(VkDescriptorBufferInfo);
    if (type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
        return sizeof(VkBufferView);
    if (type <= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
        return sizeof(VkDescriptorImageInfo);
    return 0;
}

// The application controls template alignment and stride. Read only fields that
// Vulkan consumes, using memcpy rather than dereferencing an unaligned structure.
static DescriptorElement decode_element(Device* dev, const DescriptorBinding& binding, uint32_t type, const uint8_t* data)
{
    DescriptorElement e;
    e.type = type;
    if (!descriptor_data_size(type)) return e;
    const bool immutable = !binding.immutable_samplers.empty();
    if (type == VK_DESCRIPTOR_TYPE_SAMPLER && immutable) { e.known = true; return e; }
    if (!data) return e;
    e.known = true;
    if (is_buffer(type)) {
        VkDescriptorBufferInfo b;
        memcpy(&b, data, sizeof(b));
        e.buffer = b.buffer; e.offset = b.offset; e.range = b.range;
        e.buffer_id = resource_id(dev->bufs, e.buffer);
    } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
        memcpy(&e.buffer_view, data, sizeof(e.buffer_view));
        e.buffer_view_id = resource_id(dev->buffer_views, e.buffer_view);
    } else {
        if ((type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && !immutable) {
            memcpy(&e.sampler, data + offsetof(VkDescriptorImageInfo, sampler), sizeof(e.sampler));
            e.sampler_id = resource_id(dev->samplers, e.sampler);
        }
        if (type != VK_DESCRIPTOR_TYPE_SAMPLER) {
            memcpy(&e.image_view, data + offsetof(VkDescriptorImageInfo, imageView), sizeof(e.image_view));
            memcpy(&e.image_layout, data + offsetof(VkDescriptorImageInfo, imageLayout), sizeof(e.image_layout));
            e.image_view_id = resource_id(dev->image_views, e.image_view);
        }
    }
    return e;
}

static void apply_elements_locked(Device* dev, DescriptorSetInfo& set, uint32_t binding, uint32_t element,
                                  uint32_t count, uint32_t type, const void* data, size_t offset, size_t stride)
{
    if (!set.layout) return;
    size_t size = descriptor_data_size(type);
    for (uint32_t i = 0; i < count && advance_element(set, binding, element); i++, element++) {
        const auto& b = set.layout->bindings.at(binding);
        const uint8_t* source = nullptr;
        // Unknown descriptor types and immutable sampler-only writes consume no data.
        if (size && data && !(type == VK_DESCRIPTOR_TYPE_SAMPLER && !b.immutable_samplers.empty()) &&
            offset <= SIZE_MAX - size && (!i || stride <= (SIZE_MAX - size - offset) / i))
            source = static_cast<const uint8_t*>(data) + offset + static_cast<size_t>(i) * stride;
        set_element(dev, set, binding, element, decode_element(dev, b, type, source));
    }
    set.generation++;
}

static void apply_write_locked(Device* dev, DescriptorSetInfo& set, const VkWriteDescriptorSet& w)
{
    const void* data = nullptr;
    if (is_buffer(w.descriptorType)) data = w.pBufferInfo;
    else if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
        data = w.pTexelBufferView;
    else if (descriptor_data_size(w.descriptorType)) data = w.pImageInfo;
    apply_elements_locked(dev, set, w.dstBinding, w.dstArrayElement, w.descriptorCount,
                          w.descriptorType, data, 0, descriptor_data_size(w.descriptorType));
}

void on_update_sets(Device* dev, uint32_t write_count, const VkWriteDescriptorSet* writes,
                    uint32_t copy_count, const VkCopyDescriptorSet* copies)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    for (uint32_t n = 0; n < write_count; n++) {
        const auto& w = writes[n];
        auto it = dev->descriptor_sets.find(w.dstSet);
        if (it == dev->descriptor_sets.end() || !it->second.layout) continue;
        apply_write_locked(dev, it->second, w);
    }
    // Vulkan applies all writes first, then copies in array order.
    for (uint32_t n = 0; n < copy_count; n++) {
        const auto& c = copies[n];
        auto src = dev->descriptor_sets.find(c.srcSet);
        auto dst = dev->descriptor_sets.find(c.dstSet);
        if (dst == dev->descriptor_sets.end() || !dst->second.layout) continue;
        uint32_t sb = c.srcBinding, se = c.srcArrayElement, db = c.dstBinding, de = c.dstArrayElement;
        for (uint32_t i = 0; i < c.descriptorCount && advance_element(dst->second, db, de); i++, se++, de++) {
            DescriptorElement e;
            e.type = dst->second.layout->bindings.at(db).type;
            if (src != dev->descriptor_sets.end() && advance_element(src->second, sb, se)) {
                auto v = src->second.values.find({sb, se});
                if (v != src->second.values.end()) e = v->second;
            }
            set_element(dev, dst->second, db, de, e);
        }
        dst->second.generation++;
    }
}

void on_create_descriptor_template(Device* dev, VkDescriptorUpdateTemplate h, const VkDescriptorUpdateTemplateCreateInfo* info)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    DescriptorTemplateInfo t;
    t.type = info->templateType;
    t.complete = info->pNext == nullptr;
    if (info->descriptorUpdateEntryCount)
        t.entries.assign(info->pDescriptorUpdateEntries, info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
    if (t.type == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
        auto it = dev->set_layouts.find(info->descriptorSetLayout);
        if (it != dev->set_layouts.end()) t.layout = it->second;
    } else if (t.type == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
        t.point = info->pipelineBindPoint; t.set = info->set;
        auto it = dev->pipeline_layouts.find(info->pipelineLayout);
        if (it != dev->pipeline_layouts.end()) t.pipeline_layout = it->second;
    }
    dev->descriptor_templates[h] = std::move(t);
}

static void apply_template_locked(Device* dev, DescriptorSetInfo& set, const DescriptorTemplateInfo& t, const void* data)
{
    for (const auto& e : t.entries)
        apply_elements_locked(dev, set, e.dstBinding, e.dstArrayElement, e.descriptorCount,
                              e.descriptorType, data, e.offset, e.stride);
}

void on_update_template(Device* dev, VkDescriptorSet h, VkDescriptorUpdateTemplate update, const void* data)
{
    if (skip(dev)) return;
    std::unique_lock<std::shared_mutex> lock(dev->res_mx);
    auto s = dev->descriptor_sets.find(h);
    if (s == dev->descriptor_sets.end()) return;
    auto t = dev->descriptor_templates.find(update);
    if (t == dev->descriptor_templates.end() || !t->second.complete || !t->second.layout || !s->second.layout ||
        t->second.type != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
        !same_set_layout(*t->second.layout, *s->second.layout)) {
        s->second.values.clear();
        s->second.unsupported = "unknown_descriptor_template";
        s->second.generation++;
        return;
    }
    apply_template_locked(dev, s->second, t->second, data);
}

static void disturb_sets(SetBind* bound, const std::shared_ptr<const PipelineLayoutInfo>& layout, uint32_t slot)
{
    bool disturb_higher = slot < kMaxSets && bound[slot].layout && !compatible(bound[slot].layout, layout, slot);
    for (uint32_t k = 0; k < kMaxSets; k++) {
        if ((k < slot && bound[k].layout && !compatible(bound[k].layout, layout, k)) || (k > slot && disturb_higher)) {
            bound[k] = SetBind();
            bound[k].layout = layout;
            bound[k].unsupported = "disturbed_pipeline_layout";
        }
    }
}

static std::shared_ptr<DescriptorSetInfo> prepare_push_locked(CmdBuf* cb, VkPipelineBindPoint point,
    const std::shared_ptr<const PipelineLayoutInfo>& layout, uint32_t slot)
{
    auto* bound = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? cb->sets : cb->comp_sets;
    if (!layout || slot >= layout->sets.size() || !layout->sets[slot] ||
        !(layout->sets[slot]->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)) {
        on_unsupported_descriptor_bind(cb, point, "unknown_push_layout");
        return nullptr;
    }
    disturb_sets(bound, layout, slot);
    if (slot >= kMaxSets) return nullptr;
    auto previous = bound[slot].pushed;
    auto pushed = previous && same_set_layout(*previous->layout, *layout->sets[slot])
        ? std::make_shared<DescriptorSetInfo>(*previous) : std::make_shared<DescriptorSetInfo>();
    pushed->layout = layout->sets[slot];
    if (!previous || !same_set_layout(*previous->layout, *pushed->layout))
        for (const auto& b : pushed->layout->bindings) pushed->counts[b.first] = b.second.count;
    // In contrast to allocated sets, even immutable samplers are undefined until pushed.
    bound[slot] = SetBind();
    bound[slot].layout = layout;
    bound[slot].pushed = pushed;
    return pushed;
}

void on_push_descriptors(Device* dev, CmdBuf* cb, VkPipelineBindPoint point, VkPipelineLayout handle,
                         uint32_t slot, uint32_t count, const VkWriteDescriptorSet* writes)
{
    if (skip(dev) || (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE)) return;
    std::shared_lock<std::shared_mutex> lock(dev->res_mx);
    auto it = dev->pipeline_layouts.find(handle);
    auto pushed = prepare_push_locked(cb, point, it == dev->pipeline_layouts.end() ? nullptr : it->second, slot);
    if (pushed) for (uint32_t i = 0; i < count; i++) apply_write_locked(dev, *pushed, writes[i]);
}

void on_push_template(Device* dev, CmdBuf* cb, VkDescriptorUpdateTemplate update, VkPipelineLayout handle,
                       uint32_t slot, const void* data)
{
    if (skip(dev)) return;
    std::shared_lock<std::shared_mutex> lock(dev->res_mx);
    auto t = dev->descriptor_templates.find(update);
    if (t == dev->descriptor_templates.end() || t->second.type != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
        on_unsupported_descriptor_bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, "unknown_push_template");
        on_unsupported_descriptor_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, "unknown_push_template");
        return;
    }
    const auto& info = t->second;
    if (info.point != VK_PIPELINE_BIND_POINT_GRAPHICS && info.point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    auto lit = dev->pipeline_layouts.find(handle);
    if (!info.complete || info.set != slot || lit == dev->pipeline_layouts.end() || !compatible(info.pipeline_layout, lit->second, slot)) {
        on_unsupported_descriptor_bind(cb, info.point, "unknown_push_template_layout");
        return;
    }
    auto pushed = prepare_push_locked(cb, info.point, lit->second, slot);
    if (pushed) apply_template_locked(dev, *pushed, info, data);
}

void on_bind_sets(Device* dev, CmdBuf* cb, VkPipelineBindPoint point, VkPipelineLayout handle,
                  uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dyn_count, const uint32_t* dyn)
{
    if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    auto* bound = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? cb->sets : cb->comp_sets;
    if (!g_cfg.track_resources) {
        // No layouts are known, so the dynamic offsets cannot be split between the sets of the
        // call. Keep the handles, and all offsets of the call with its first set.
        uint32_t first_offset = static_cast<uint32_t>(cb->dyn.size());
        if (dyn_count) cb->dyn.insert(cb->dyn.end(), dyn, dyn + dyn_count);
        for (uint32_t i = 0; i < count && first + i < kMaxSets; i++) {
            auto& s = bound[first + i];
            s = SetBind();
            s.set = sets[i];
            s.dyn_first = first_offset;
            s.dyn_count = i == 0 ? dyn_count : 0;
            s.unsupported = "resource_tracking_off";
        }
        return;
    }
    std::shared_lock<std::shared_mutex> lock(dev->res_mx);
    auto lit = dev->pipeline_layouts.find(handle);
    auto layout = lit == dev->pipeline_layouts.end() ? nullptr : lit->second;
    uint32_t base = static_cast<uint32_t>(cb->dyn.size()), cursor = 0;
    if (dyn_count) cb->dyn.insert(cb->dyn.end(), dyn, dyn + dyn_count);
    bool offsets_known = true;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = first + i;
        // Disturb lower sets and, when replacing an incompatible binding, higher sets.
        disturb_sets(bound, layout, slot);
        auto sit = dev->descriptor_sets.find(sets[i]);
        uint32_t n = 0;
        if (sit == dev->descriptor_sets.end() || !sit->second.layout) offsets_known = false;
        else for (const auto& b : sit->second.layout->bindings)
            if (is_dynamic(b.second.type)) n += sit->second.counts.at(b.first);
        if (cursor > dyn_count || n > dyn_count - cursor) offsets_known = false;
        if (slot < kMaxSets) {
            auto& s = bound[slot];
            s = SetBind();
            s.set = sets[i];
            s.id = sit == dev->descriptor_sets.end() ? 0 : sit->second.id;
            s.layout = layout;
            s.layout_handle = handle;
            if (offsets_known) { s.dyn_first = base + cursor; s.dyn_count = n; }
            else s.unsupported = "unknown_dynamic_offset_layout";
        }
        cursor += n;
    }
}

void on_unsupported_descriptor_bind(CmdBuf* cb, VkPipelineBindPoint point, const char* reason)
{
    // Conservative: an untracked binding may also disturb other sets.
    if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    auto* bound = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? cb->sets : cb->comp_sets;
    for (uint32_t i = 0; i < kMaxSets; i++) { bound[i] = SetBind(); bound[i].unsupported = reason; }
}

static void write_buffer_range(JsonWriter& w, Device* dev, VkBuffer handle, uint32_t id,
                               uint64_t offset, uint64_t range, uint32_t dynamic)
{
    auto it = dev->bufs.find(handle);
    bool alive = id && it != dev->bufs.end() && it->second.id == id;
    w.kn("buf", id);
    w.kb("resource_known", alive);
    w.kn("offset", offset);
    w.kn("dynamic_offset", dynamic);
    uint64_t effective = offset + dynamic;
    w.kn("effective_offset", effective);
    w.kb("whole_size", range == VK_WHOLE_SIZE);
    if (range == VK_WHOLE_SIZE) range = alive && offset <= it->second.size ? it->second.size - offset : 0;
    w.kn("range", range);
    const char* src = "none";
    if (alive && effective >= offset && effective <= it->second.size && range <= it->second.size - effective) {
        const auto& b = it->second;
        auto m = dev->mems.find(b.mem);
        if (m != dev->mems.end() && m->second.mapped) {
            uint64_t at = b.mem_off + effective;
            if (at >= m->second.map_off && at - m->second.map_off <= m->second.map_size &&
                range <= m->second.map_size - (at - m->second.map_off)) src = "mapped";
        }
        if (!strcmp(src, "none") && b.known.covers(effective, range)) src = "shadow";
    }
    w.kv("src", src);
}

static void write_element(JsonWriter& w, Device* dev, const DescriptorElement& e, uint32_t dynamic)
{
    w.kb("known", e.known);
    if (!e.known) return;
    if (is_buffer(e.type)) write_buffer_range(w, dev, e.buffer, e.buffer_id, e.offset, e.range, dynamic);
    if (e.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || e.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
        w.kn("buffer_view", e.buffer_view_id);
        auto it = dev->buffer_views.find(e.buffer_view);
        bool alive = e.buffer_view_id && it != dev->buffer_views.end() && it->second.id == e.buffer_view_id;
        w.kb("view_known", alive);
        if (alive) {
            const auto& v = it->second;
            w.kn("format", v.format);
            write_buffer_range(w, dev, v.buffer, v.buffer_id, v.offset, v.range, 0);
        }
    }
    if (e.type == VK_DESCRIPTOR_TYPE_SAMPLER || e.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        w.kn("sampler", e.sampler_id);
        auto s = dev->samplers.find(e.sampler);
        w.kb("sampler_known", e.sampler_id && s != dev->samplers.end() && s->second.id == e.sampler_id);
    }
    if (e.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || e.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        e.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || e.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        w.kn("image_view", e.image_view_id);
        w.kn("image_layout", e.image_layout);
        auto v = dev->image_views.find(e.image_view);
        bool alive = e.image_view_id && v != dev->image_views.end() && v->second.id == e.image_view_id;
        w.kb("view_known", alive);
        if (alive) {
            const auto& view = v->second;
            w.kn("view_type", view.type);
            w.kn("format", view.format);
            w.key("subresource"); w.begin_object();
            w.kn("aspect", view.range.aspectMask);
            w.kn("base_mip", view.range.baseMipLevel); w.kn("levels", view.range.levelCount);
            w.kn("base_layer", view.range.baseArrayLayer); w.kn("layers", view.range.layerCount);
            w.end_object();
            w.kn("image", view.image_id);
            auto img = dev->images.find(view.image);
            bool image_known = view.image_id && img != dev->images.end() && img->second.id == view.image_id;
            w.kb("image_known", image_known);
            if (image_known) {
                const auto& image = img->second;
                w.key("extent"); w.begin_array();
                w.num(image.extent.width); w.num(image.extent.height); w.num(image.extent.depth); w.end_array();
                w.kn("image_format", image.format); w.kn("image_type", image.type);
                w.kn("mip_levels", image.levels); w.kn("array_layers", image.layers);
                w.kn("samples", image.samples); w.kn("usage", image.usage);
                w.key("content"); w.begin_object();
                w.kv("state", image_content_name(image.content));
                if (image.content == IMG_HASHED) { w.key("crc"); w.hex32(image.content_crc); }
                w.kn("bytes", image.content_bytes); w.kn("uploads", image.content_uploads);
                w.end_object();
                uint32_t original = 0;
                if (dx11_texture_hash(image, original)) { w.key("dx11_hash"); w.hex32(original); } // D-077
            }
        }
    }
}

void write_descriptor_set_locked(JsonWriter& w, Device* dev, const SetBind& bind, const std::vector<uint32_t>& dyn,
                                 VkPipeline pipeline, uint32_t slot)
{
    w.begin_object();
    w.kv("snapshot", bind.pushed ? "record" : "submit");
    w.kv("resource_snapshot", "submit");
    w.kv("kind", bind.pushed ? "push" : "set");
    w.kn("id", bind.id);
    if (pipeline) {
        auto p = dev->pipes.find(pipeline);
        w.kb("pipeline_compatible", p != dev->pipes.end() && compatible(bind.layout, p->second.layout, slot));
    }
    auto it = dev->descriptor_sets.find(bind.set);
    const DescriptorSetInfo* contents = bind.pushed.get();
    if (!contents && bind.id && it != dev->descriptor_sets.end() && it->second.id == bind.id) contents = &it->second;
    bool tracked = !bind.unsupported && contents && contents->layout;
    w.kb("tracked", tracked);
    if (!tracked) {
        w.kv("reason", bind.unsupported ? bind.unsupported : (bind.set ? "unknown_or_freed_set" : "unbound"));
        w.end_object();
        return;
    }
    const auto& set = *contents;
    w.kn("layout", set.layout->id);
    w.kn("generation", set.generation);
    if (set.unsupported) w.kv("unsupported_update", set.unsupported);
    w.key("bindings"); w.begin_array();
    uint32_t dynamic = 0, emitted = 0;
    constexpr uint32_t limit = 256;
    for (const auto& kv : set.layout->bindings) {
        uint32_t binding = kv.first;
        const auto& b = kv.second;
        uint32_t count = set.counts.at(binding);
        w.begin_object();
        w.kn("binding", binding); w.kn("type", b.type); w.kn("count", count);
        w.kn("stages", b.stages); w.kn("flags", b.flags);
        w.kb("immutable_sampler", !b.immutable_samplers.empty());
        w.key("elements"); w.begin_array();
        uint32_t defined = 0, shown = 0;
        auto v = set.values.lower_bound({binding, 0});
        for (; v != set.values.end() && v->first.first == binding; ++v) {
            if (v->second.known) defined++;
            if (emitted >= limit) continue;
            uint32_t element = v->first.second;
            uint32_t offset = is_dynamic(b.type) && dynamic + element < dyn.size() ? dyn[dynamic + element] : 0;
            w.begin_object(); w.kn("element", element);
            write_element(w, dev, v->second, offset);
            w.end_object(); emitted++; shown++;
        }
        w.end_array();
        w.kn("known_elements", defined);
        w.kn("unknown_elements", count - std::min(count, defined));
        w.kn("shown_elements", shown);
        w.end_object();
        if (is_dynamic(b.type)) dynamic += count;
    }
    w.end_array();
    w.kb("truncated", set.values.size() > emitted);
    w.kn("element_limit", limit);
    w.end_object();
}

void collect_buffer_ranges_locked(Device* dev, const SetBind& bind, const uint32_t* dyn, uint32_t dyn_count, uint32_t slot,
                                  std::vector<BoundRange>& out)
{
    auto it = dev->descriptor_sets.find(bind.set);
    const DescriptorSetInfo* contents = bind.pushed.get();
    if (!contents && bind.id && it != dev->descriptor_sets.end() && it->second.id == bind.id) contents = &it->second;
    if (bind.unsupported || !contents || !contents->layout) return;
    const auto& set = *contents;
    uint32_t dynamic = 0;
    for (const auto& kv : set.layout->bindings) {
        uint32_t binding = kv.first;
        const auto& b = kv.second;
        if (is_buffer(b.type)) {
            auto v = set.values.lower_bound({binding, 0});
            for (; v != set.values.end() && v->first.first == binding; ++v) {
                const DescriptorElement& e = v->second;
                if (!e.known) continue;
                auto buf = dev->bufs.find(e.buffer);
                if (!e.buffer_id || buf == dev->bufs.end() || buf->second.id != e.buffer_id) continue;
                uint32_t element = v->first.second;
                uint64_t offset = e.offset + (is_dynamic(b.type) && dynamic + element < dyn_count ? dyn[dynamic + element] : 0);
                uint64_t size = buf->second.size;
                if (offset > size) continue;
                uint64_t range = e.range == VK_WHOLE_SIZE ? (e.offset <= size ? size - e.offset : 0) : e.range;
                if (range > size - offset) continue;
                BoundRange r;
                r.set = static_cast<uint8_t>(slot);
                r.binding = binding;
                r.element = element;
                r.type = b.type;
                r.buffer = e.buffer;
                r.buffer_id = e.buffer_id;
                r.off = offset;
                r.size = range;
                out.push_back(r);
            }
        }
        if (is_dynamic(b.type)) dynamic += set.counts.at(binding);
    }
}

bool format_block(uint32_t format, uint32_t& bw, uint32_t& bh, uint32_t& bytes)
{
    bw = bh = 1;
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK) {
        bw = bh = 4;
        bool small = format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || format == VK_FORMAT_BC4_UNORM_BLOCK ||
                     format == VK_FORMAT_BC4_SNORM_BLOCK;
        bytes = small ? 8 : 16;
        return true;
    }
    struct Span { uint32_t first, last, bytes; };
    static const Span spans[] = {
        {VK_FORMAT_R4G4_UNORM_PACK8, VK_FORMAT_R4G4_UNORM_PACK8, 1},
        {VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB, 1},
        {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SRGB, 2},
        {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_B8G8R8_SRGB, 3},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_SINT_PACK32, 4},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SFLOAT, 2},
        {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SFLOAT, 4},
        {VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SFLOAT, 6},
        {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
        {VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT, 4},
        {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SFLOAT, 8},
        {VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4},
        {VK_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, 2},
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT, 4},
    };
    for (const Span& s : spans)
        if (format >= s.first && format <= s.last) {
            bytes = s.bytes;
            return true;
        }
    return false;
}

const char* image_content_name(uint8_t state)
{
    switch (state) {
    case IMG_HASHED: return "hashed";
    case IMG_PARTIAL: return "partial";
    case IMG_UNREADABLE: return "unreadable";
    case IMG_UNSUPPORTED: return "unsupported_format";
    case IMG_TOO_LARGE: return "too_large";
    default: return "none";
    }
}

void apply_image_writes(Device* dev, CmdBuf* cb)
{
    if (cb->image_writes.empty())
        return;
    struct Result {
        VkImage image;
        uint8_t state;
        uint32_t crc;
        uint64_t bytes;
        uint32_t dx11 = 0; // the original's data hash (D-077)
    };
    std::vector<Result> results;
    results.reserve(cb->image_writes.size());
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    uint64_t hashed_bytes = 0;
    {
        // Reading and hashing needs the lock shared only; the results are stored afterwards.
        std::shared_lock<std::shared_mutex> lock(dev->res_mx);
        std::vector<uint8_t> tmp;
        for (const PendingImageWrite& w : cb->image_writes) {
            auto it = dev->images.find(w.dst);
            if (it == dev->images.end())
                continue;
            const ImageInfo& img = it->second;
            Result r = {w.dst, IMG_PARTIAL, 0, 0, 0};
            // texture_hash_version 0: width * height * array size bytes from the start of the first
            // subresource when that is not longer than it, else the whole subresource.
            const uint64_t original_length = uint64_t(img.extent.width) * img.extent.height * std::max(img.layers, 1u);
            uint32_t bw, bh, bb;
            bool whole = w.ox == 0 && w.oy == 0 && w.oz == 0 && w.w == img.extent.width && w.h == img.extent.height &&
                         w.d == img.extent.depth;
            if (!format_block(img.format, bw, bh, bb)) {
                r.state = IMG_UNSUPPORTED;
            } else if (whole) {
                uint64_t row_texels = w.row_length ? w.row_length : w.w;
                uint64_t slice_texels = w.image_height ? w.image_height : w.h;
                uint64_t row_bytes = (static_cast<uint64_t>(w.w) + bw - 1) / bw * bb; // of one block row, without padding
                uint64_t pitch = (row_texels + bw - 1) / bw * bb;                     // from one block row to the next
                uint64_t rows = (static_cast<uint64_t>(w.h) + bh - 1) / bh;
                uint64_t slice_rows = (slice_texels + bh - 1) / bh;                   // block rows from one slice to the next
                r.bytes = row_bytes * rows * w.d;
                auto bit = dev->bufs.find(w.src);
                if (r.bytes > g_cfg.image_hash_limit) {
                    r.state = IMG_TOO_LARGE;
                } else if (bit == dev->bufs.end()) {
                    r.state = IMG_UNREADABLE;
                } else if (pitch == row_bytes && slice_rows == rows) {
                    tmp.resize(static_cast<size_t>(r.bytes));
                    if (read_buffer_locked(dev, bit->second, w.src_off, r.bytes, tmp.data()) != SRC_NONE) {
                        r.crc = crc32c(0, tmp.data(), tmp.size());
                        r.dx11 = original_length < r.bytes ? crc32c(0, tmp.data(), static_cast<size_t>(original_length)) : r.crc;
                        r.state = IMG_HASHED;
                    } else {
                        r.state = IMG_UNREADABLE;
                    }
                } else {
                    // Padded rows or slices: hash the rows one after the other, as if they were packed.
                    tmp.resize(static_cast<size_t>(row_bytes));
                    r.state = IMG_HASHED;
                    uint64_t prefix = original_length < r.bytes ? original_length : 0;
                    for (uint64_t z = 0; z < w.d && r.state == IMG_HASHED; z++)
                        for (uint64_t y = 0; y < rows; y++) {
                            uint64_t off = w.src_off + (z * slice_rows + y) * pitch;
                            if (read_buffer_locked(dev, bit->second, off, row_bytes, tmp.data()) == SRC_NONE) {
                                r.state = IMG_UNREADABLE;
                                break;
                            }
                            r.crc = crc32c(r.crc, tmp.data(), tmp.size());
                            if (prefix) {
                                const uint64_t take = std::min<uint64_t>(prefix, tmp.size());
                                r.dx11 = crc32c(r.dx11, tmp.data(), static_cast<size_t>(take));
                                prefix -= take;
                            }
                        }
                    if (r.state == IMG_HASHED && !(original_length < r.bytes)) r.dx11 = r.crc;
                }
                if (r.state == IMG_HASHED)
                    hashed_bytes += r.bytes;
            }
            results.push_back(r);
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(dev->res_mx);
        for (const Result& r : results) {
            auto it = dev->images.find(r.image);
            if (it == dev->images.end())
                continue;
            it->second.content = r.state;
            it->second.content_crc = r.crc;
            it->second.dx11_data_crc = r.dx11;
            it->second.content_bytes = r.bytes;
            it->second.content_uploads++;
            dev->image_uploads[r.state].fetch_add(1, std::memory_order_relaxed);
        }
    }
    QueryPerformanceCounter(&t1);
    dev->image_hash_bytes.fetch_add(hashed_bytes, std::memory_order_relaxed);
    dev->image_hash_us.fetch_add(static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart),
                                 std::memory_order_relaxed);
}

void write_stages(JsonWriter& w, const PipeInfo& p)
{
    w.kn("layout", p.layout ? p.layout->id : 0);
    w.key("shaders"); w.begin_array();
    for (const auto& s : p.shaders) {
        w.begin_object(); w.kn("stage", s.stage); w.kv("entry", s.entry);
        w.kb("known", s.shader != nullptr);
        if (s.shader) {
            w.kn("id", s.shader->id); w.kn("size", s.shader->size);
            w.key("hash"); w.hex32(s.shader->hash);
            w.kb("code_retained", !s.shader->code.empty());
        }
        w.end_object();
    }
    w.end_array();
}

std::string probe_shader(uint32_t id, bool with_code, std::vector<uint8_t>& payload)
{
    DeviceRef ref;
    std::shared_ptr<const ShaderInfo> shader;
    if (ref.dev) {
        std::shared_lock<std::shared_mutex> lock(ref.dev->res_mx);
        auto it = ref.dev->shader_by_id.find(id);
        if (it != ref.dev->shader_by_id.end()) shader = it->second.lock();
    }
    JsonWriter w;
    w.begin_object();
    if (!shader) {
        w.kb("ok", false); w.kv("error", "unknown shader id (module and all referencing pipelines may have been destroyed)");
    } else if (with_code && shader->code.empty() && shader->size) {
        w.kb("ok", false);
        w.kv("error", "the SPIR-V of this shader was not kept: the shader budget was exhausted when it was created "
                      "(setting shader_budget); hash and size are still reported by the pipeline");
    } else if (with_code && shader->code.size() * 4 > (64ull << 20)) {
        w.kb("ok", false); w.kv("error", "shader exceeds the 64 MiB payload limit");
    } else {
        w.kb("ok", true); w.key("result"); w.begin_object();
        w.kn("id", shader->id); w.kn("size", shader->size);
        w.key("hash"); w.hex32(shader->hash); w.kv("hash_algorithm", "crc32c");
        w.kb("code_retained", !shader->code.empty());
        w.end_object();
        if (with_code) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(shader->code.data());
            payload.assign(bytes, bytes + shader->code.size() * 4);
        }
    }
    w.end_object();
    return w.out;
}

} // namespace efvk
