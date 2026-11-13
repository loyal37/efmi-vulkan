// Offline tests of tracker lifetimes and descriptor semantics. No Vulkan driver.
#include "../layer/src/state.h"
#include "../layer/src/json.h"
#include "src/spirv.h"

#include <cstdio>
#include <thread>

using namespace efvk;
namespace efvk { void character_cache_cases(void (*check)(bool, const char*)); }

static int passed = 0, failed = 0;
static void check(bool ok, const char* label)
{
    if (ok) passed++;
    else { failed++; printf("FAIL: %s\n", label); }
}
template <typename T> static T h(uintptr_t value) { return reinterpret_cast<T>(value); }

static void layout(Device& d, VkDescriptorSetLayout handle, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                   const void* next = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0)
{
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = next;
    info.flags = flags;
    info.bindingCount = static_cast<uint32_t>(bindings.size()); info.pBindings = bindings.data();
    on_create_set_layout(&d, handle, &info);
}

static void pipeline_layout(Device& d, VkPipelineLayout handle, const std::vector<VkDescriptorSetLayout>& sets,
                            bool push = false)
{
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size()); info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = push ? 1 : 0; info.pPushConstantRanges = &range;
    on_create_pipeline_layout(&d, handle, &info);
}

static void allocate(Device& d, VkDescriptorPool pool, VkDescriptorSetLayout sl, VkDescriptorSet set, const void* next = nullptr)
{
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.pNext = next; info.descriptorPool = pool; info.descriptorSetCount = 1; info.pSetLayouts = &sl;
    on_allocate_sets(&d, &info, &set);
}

static std::string snapshot(Device& d, VkDescriptorSet set)
{
    SetBind b;
    b.set = set; b.id = d.descriptor_sets.at(set).id;
    JsonWriter w;
    write_descriptor_set_locked(w, &d, b, {});
    return w.out;
}

static void descriptor_writes()
{
    Device d;
    auto pool = h<VkDescriptorPool>(1);
    auto a = h<VkDescriptorSet>(2), b = h<VkDescriptorSet>(3);
    auto la = h<VkDescriptorSetLayout>(4), lb = h<VkDescriptorSetLayout>(5);
    auto sampler_a = h<VkSampler>(6), sampler_b = h<VkSampler>(7);
    auto buffer = h<VkBuffer>(8);
    VkSamplerCreateInfo si = {};
    on_create_sampler(&d, sampler_a, &si); on_create_sampler(&d, sampler_b, &si);
    VkBufferCreateInfo bi = {};
    bi.size = 4096; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    on_create_buffer(&d, buffer, &bi);
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_a},
    };
    layout(d, la, bindings);
    bindings[3].pImmutableSamplers = &sampler_b;
    layout(d, lb, bindings);
    allocate(d, pool, la, a); allocate(d, pool, lb, b);
    VkDescriptorBufferInfo infos[] = {{buffer, 64, 16}, {buffer, 128, 16}, {buffer, 192, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet write = {};
    write.dstSet = a; write.dstBinding = 3; write.dstArrayElement = 1; write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = infos;
    VkCopyDescriptorSet copy = {};
    copy.srcSet = a; copy.srcBinding = 3; copy.srcArrayElement = 1;
    copy.dstSet = b; copy.dstBinding = 3; copy.descriptorCount = 3;
    on_update_sets(&d, 1, &write, 1, &copy);
    auto& av = d.descriptor_sets.at(a).values;
    auto& bv = d.descriptor_sets.at(b).values;
    check(av.at({3, 1}).offset == 64 && av.at({5, 0}).offset == 128 && av.at({5, 1}).offset == 192,
          "write spans array boundary and skips a zero-sized binding");
    check(bv.at({3, 0}).offset == 64 && bv.at({3, 1}).offset == 128 && bv.at({5, 0}).offset == 192,
          "copies see writes from the same call and span destination bindings");
    check(bv.at({5, 0}).range == VK_WHOLE_SIZE, "whole-size descriptor survives a copy");
    check(bv.at({8, 0}).known && bv.at({8, 0}).sampler == sampler_b, "immutable sampler initialized at allocation");
    copy.srcBinding = copy.dstBinding = 8; copy.srcArrayElement = 0; copy.descriptorCount = 1;
    on_update_sets(&d, 0, nullptr, 1, &copy);
    check(bv.at({8, 0}).sampler == sampler_b, "copy cannot replace destination immutable sampler");
    write.dstSet = b; write.dstBinding = 5; write.dstArrayElement = 1; write.descriptorCount = 1;
    on_update_sets(&d, 1, &write, 0, nullptr);
    check(bv.at({5, 1}).known, "destination is defined before the undefined copy");
    copy.srcBinding = 3; copy.dstBinding = 5; copy.dstArrayElement = 1;
    on_update_sets(&d, 0, nullptr, 1, &copy);
    check(!bv.at({5, 1}).known, "copying an undefined descriptor clears previous knowledge");
    std::string before = snapshot(d, b);
    check(before.find("\"whole_size\":true,\"range\":3904") != std::string::npos,
          "whole size resolves against the tracked buffer size");
    uint32_t old_buffer_id = bv.at({3, 0}).buffer_id;
    on_destroy_buffer(&d, buffer); on_create_buffer(&d, buffer, &bi);
    check(d.bufs.at(buffer).id != old_buffer_id, "buffer ids change on handle reuse");
    check(snapshot(d, b).find("\"resource_known\":false") != std::string::npos, "reused buffer is not attributed to an old descriptor");
    on_update_template(&d, b, h<VkDescriptorUpdateTemplate>(99), nullptr);
    check(bv.empty() && snapshot(d, b).find("unknown_descriptor_template") != std::string::npos,
          "unknown template updates clear stale ordinary descriptors and identify the gap");
    check(before.find("unknown_descriptor_template") == std::string::npos, "previous capture is immutable after an update");
    on_free_sets(&d, 1, &a);
    check(d.descriptor_sets.count(a) == 0 && d.descriptor_sets.count(b) == 1, "free removes only the requested set");
    SetBind old;
    old.set = b; old.id = d.descriptor_sets.at(b).id;
    on_clear_descriptor_pool(&d, pool);
    check(d.descriptor_sets.empty(), "pool reset/destroy removes every owned set");
    allocate(d, pool, lb, b);
    JsonWriter stale;
    write_descriptor_set_locked(stale, &d, old, {});
    check(stale.out.find("unknown_or_freed_set") != std::string::npos, "set handle reuse cannot alter an old recording");

    VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags = {};
    flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags.bindingCount = 1; flags.pBindingFlags = &flag;
    layout(d, la, {{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10000, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}, &flags);
    uint32_t count = 3;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable = {};
    variable.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variable.descriptorSetCount = 1; variable.pDescriptorCounts = &count;
    allocate(d, pool, la, a, &variable);
    check(d.descriptor_sets.at(a).counts.at(7) == 3, "variable count comes from allocation");
    check(d.descriptor_sets.at(a).values.empty(), "unused bindless elements allocate no values");
    on_destroy_set_layout(&d, la);
    check(snapshot(d, a).find("\"count\":3") != std::string::npos, "set retains its layout after layout handle destruction");

    layout(d, la, {{0, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 16, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    allocate(d, pool, la, a);
    write = {};
    write.dstSet = a; write.descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK; write.descriptorCount = 16;
    on_update_sets(&d, 1, &write, 0, nullptr);
    check(snapshot(d, a).find("\"unknown_elements\":16") != std::string::npos,
          "unsupported descriptor types remain unknown without dereferencing ignored pointers");
}

static void descriptor_binding()
{
    Device d;
    auto a = h<VkDescriptorSetLayout>(1), b = h<VkDescriptorSetLayout>(2), twin = h<VkDescriptorSetLayout>(3);
    auto set_a = h<VkDescriptorSet>(4), set_b = h<VkDescriptorSet>(5);
    auto pool = h<VkDescriptorPool>(6);
    auto pl = h<VkPipelineLayout>(7), same = h<VkPipelineLayout>(8), changed = h<VkPipelineLayout>(9);
    layout(d, a, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    layout(d, b, bindings); layout(d, twin, bindings);
    pipeline_layout(d, pl, {a, b}); pipeline_layout(d, same, {a, twin}); pipeline_layout(d, changed, {a, b}, true);
    allocate(d, pool, a, set_a); allocate(d, pool, b, set_b);
    CmdBuf cb;
    VkDescriptorSet sets[] = {set_a, set_b};
    uint32_t dyn[] = {10, 20, 30, 40};
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 4, dyn);
    check(cb.sets[0].dyn_count == 1 && cb.sets[1].dyn_count == 3, "dynamic offsets split by set");
    check(cb.dyn[cb.sets[1].dyn_first] == 20 && cb.dyn[cb.sets[1].dyn_first + 2] == 40,
          "second set starts after first set's dynamic elements");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, sets, 1, dyn + 1);
    check(cb.dyn[cb.comp_sets[0].dyn_first] == 20 && cb.dyn[cb.sets[0].dyn_first] == 10,
          "compute and graphics descriptor bindings are independent");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, same, 1, 1, sets + 1, 3, dyn + 1);
    check(cb.sets[0].id != 0, "equivalent layout with a different handle preserves lower set");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, changed, 1, 1, sets + 1, 3, dyn + 1);
    check(cb.sets[0].id == 0 && cb.sets[0].unsupported, "incompatible push ranges disturb lower set");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, sets, 1, dyn);
    check(cb.sets[1].id == 0 && cb.sets[1].unsupported, "incompatible lower rebind disturbs higher set");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 4, dyn);
    on_destroy_set_layout(&d, a); on_destroy_set_layout(&d, b); on_destroy_pipeline_layout(&d, pl);
    check(cb.sets[0].layout && cb.sets[0].layout->sets[1]->bindings.size() == 2,
          "recording keeps pipeline and descriptor layout definitions alive");
    on_unsupported_descriptor_bind(&cb, VK_PIPELINE_BIND_POINT_GRAPHICS, "push_descriptors");
    check(cb.sets[0].set == VK_NULL_HANDLE && cb.comp_sets[0].id != 0,
          "unsupported graphics binding clears stale sets but preserves compute");
    cb.reset_recording();
    check(!cb.comp_sets[0].set && !cb.sets[0].layout && cb.dyn.empty(), "reset releases both bind points and dynamic data");
}

static void descriptor_templates()
{
    Device d;
    auto sl = h<VkDescriptorSetLayout>(1);
    auto set = h<VkDescriptorSet>(2);
    auto update = h<VkDescriptorUpdateTemplate>(3);
    auto buffer = h<VkBuffer>(4);
    auto sampler = h<VkSampler>(5);
    auto view = h<VkImageView>(6);
    auto texel = h<VkBufferView>(7);
    VkBufferCreateInfo bi = {}; bi.size = 4096;
    on_create_buffer(&d, buffer, &bi);
    VkSamplerCreateInfo si = {}; on_create_sampler(&d, sampler, &si);
    layout(d, sl, {
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler},
        {8, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    });
    allocate(d, h<VkDescriptorPool>(8), sl, set);
    std::vector<uint8_t> data(160, 0);
    VkDescriptorBufferInfo buffers[] = {{buffer, 64, 16}, {buffer, 128, VK_WHOLE_SIZE}};
    memcpy(data.data() + 1, &buffers[0], sizeof(buffers[0]));
    memcpy(data.data() + 41, &buffers[1], sizeof(buffers[1]));
    VkDescriptorImageInfo image = {h<VkSampler>(999), view, VK_IMAGE_LAYOUT_GENERAL};
    memcpy(data.data() + 89, &image, sizeof(image));
    memcpy(data.data() + 130, &texel, sizeof(texel));
    VkDescriptorUpdateTemplateEntry entries[] = {
        {1, 1, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 40},
        {7, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 89, 0},
        {8, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 130, 0},
        {9, 0, 4, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, SIZE_MAX, SIZE_MAX},
    };
    VkDescriptorUpdateTemplateCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
    ci.descriptorSetLayout = sl; ci.descriptorUpdateEntryCount = 4; ci.pDescriptorUpdateEntries = entries;
    on_create_descriptor_template(&d, update, &ci);
    entries[0].offset = 1000;
    on_destroy_set_layout(&d, sl);
    on_update_template(&d, set, update, data.data());
    auto& values = d.descriptor_sets.at(set).values;
    check(values.at({1, 1}).offset == 64 && values.at({2, 0}).offset == 128,
          "template copies its entries and decodes padded unaligned data across bindings");
    check(values.at({2, 0}).range == VK_WHOLE_SIZE && values.at({1, 1}).buffer_id == d.bufs.at(buffer).id,
          "template retains full buffer ranges and allocation ids");
    check(values.at({7, 0}).sampler == sampler && values.at({7, 0}).image_view == view &&
          values.at({7, 0}).image_layout == VK_IMAGE_LAYOUT_GENERAL,
          "template image fields and immutable sampler survive layout destruction");
    check(values.at({8, 0}).buffer_view == texel, "template decodes texel-buffer handles");
    check(!values.at({9, 0}).known && !values.at({9, 3}).known,
          "unsupported template types never dereference their opaque payload");
    std::string before = snapshot(d, set);
    on_destroy_descriptor_template(&d, update);
    check(d.descriptor_templates.empty() && snapshot(d, set) == before,
          "destroying a template does not erase already updated descriptors");

    // Reusing the handle must replace the old decoder, including its retained layout.
    layout(d, sl, {{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    allocate(d, h<VkDescriptorPool>(8), sl, set);
    entries[0] = {1, 0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0};
    entries[1] = {1, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 41, 0};
    ci.descriptorUpdateEntryCount = 2;
    on_create_descriptor_template(&d, update, &ci);
    on_update_template(&d, set, update, data.data());
    const auto& updated = d.descriptor_sets.at(set).values;
    check(updated.at({1, 0}).offset == 64 && updated.at({1, 1}).offset == 128,
          "zero template stride repeats data and overlapping entries apply in order");
    check(updated.size() == 2, "reused template handle has no old image or texel entries");
    auto other = h<VkDescriptorSetLayout>(10);
    layout(d, other, {{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    allocate(d, h<VkDescriptorPool>(8), other, set);
    on_update_template(&d, set, update, reinterpret_cast<const void*>(1));
    check(d.descriptor_sets.at(set).values.empty() && d.descriptor_sets.at(set).unsupported,
          "incompatible template layout reports unknown without reading data");
}

static void pushed_descriptors()
{
    Device d;
    auto ordinary = h<VkDescriptorSetLayout>(1), pushed = h<VkDescriptorSetLayout>(2);
    auto pl = h<VkPipelineLayout>(3), twin = h<VkPipelineLayout>(4), incompatible = h<VkPipelineLayout>(5);
    auto set = h<VkDescriptorSet>(6);
    auto buffer = h<VkBuffer>(7);
    auto sampler = h<VkSampler>(8);
    VkBufferCreateInfo bi = {}; bi.size = 4096; on_create_buffer(&d, buffer, &bi);
    VkSamplerCreateInfo si = {}; on_create_sampler(&d, sampler, &si);
    layout(d, ordinary, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    layout(d, pushed, {
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler},
    }, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    pipeline_layout(d, pl, {ordinary, pushed}); pipeline_layout(d, twin, {ordinary, pushed});
    pipeline_layout(d, incompatible, {ordinary, pushed}, true);
    allocate(d, h<VkDescriptorPool>(9), ordinary, set);
    CmdBuf cb;
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
    VkDescriptorBufferInfo data = {buffer, 64, 16};
    VkWriteDescriptorSet write = {};
    write.dstSet = h<VkDescriptorSet>(999); // ignored for pushes
    write.dstBinding = 2; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &data;
    on_push_descriptors(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &write);
    check(cb.sets[0].set == set && cb.sets[1].pushed->values.at({2, 0}).offset == 64,
          "pushing a higher set preserves compatible ordinary bindings and ignores dstSet");
    check(cb.sets[1].pushed->values.count({5, 0}) == 0, "immutable push sampler is undefined until explicitly pushed");
    SetBind first_draw = cb.sets[1];
    data.offset = 128; write.dstArrayElement = 1;
    on_push_descriptors(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, twin, 1, 1, &write);
    check(cb.sets[1].pushed->values.at({2, 0}).offset == 64 && cb.sets[1].pushed->values.at({2, 1}).offset == 128,
          "partial push with a compatible layout retains previous array elements");
    check(first_draw.pushed->values.count({2, 1}) == 0 && first_draw.pushed->values.at({2, 0}).offset == 64,
          "later push does not overwrite the values retained by an earlier draw");
    VkWriteDescriptorSet sampler_write = {};
    sampler_write.dstBinding = 5; sampler_write.descriptorCount = 1; sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    on_push_descriptors(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &sampler_write);
    check(cb.sets[1].pushed->values.at({5, 0}).sampler == sampler && cb.sets[1].pushed->values.at({5, 0}).known,
          "immutable sampler push reads the layout without touching ignored image data");
    JsonWriter json;
    write_descriptor_set_locked(json, &d, cb.sets[1], {});
    check(json.out.find("\"snapshot\":\"record\"") != std::string::npos &&
          json.out.find("\"resource_snapshot\":\"submit\"") != std::string::npos &&
          json.out.find("\"kind\":\"push\"") != std::string::npos,
          "push JSON distinguishes record-time values from submit-time resource metadata");
    auto update = h<VkDescriptorUpdateTemplate>(10);
    VkDescriptorUpdateTemplateEntry entry = {2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, sizeof(data)};
    VkDescriptorUpdateTemplateCreateInfo ci = {};
    ci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    ci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; ci.pipelineLayout = pl; ci.set = 1;
    ci.descriptorUpdateEntryCount = 1; ci.pDescriptorUpdateEntries = &entry;
    on_create_descriptor_template(&d, update, &ci);
    data.offset = 256;
    on_push_template(&d, &cb, update, twin, 1, &data);
    check(cb.comp_sets[1].pushed->values.at({2, 0}).offset == 256 && cb.sets[1].pushed->values.at({2, 0}).offset == 64,
          "push template uses its creation bind point without disturbing graphics");
    CmdBuf other_cb;
    data.offset = 512;
    on_push_template(&d, &other_cb, update, pl, 1, &data);
    check(cb.comp_sets[1].pushed->values.at({2, 0}).offset == 256,
          "different command buffers never share mutable push contents");
    on_push_template(&d, &other_cb, update, pl, 0, reinterpret_cast<const void*>(1));
    check(!other_cb.comp_sets[1].pushed && other_cb.comp_sets[1].unsupported,
          "wrong push template set clears the affected bind point without reading data");
    on_destroy_descriptor_template(&d, update);
    check(cb.comp_sets[1].pushed->values.at({2, 0}).offset == 256,
          "push contents survive template destruction");
    on_bind_sets(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, incompatible, 0, 1, &set, 0, nullptr);
    check(!cb.sets[1].pushed && cb.comp_sets[1].pushed, "incompatible lower bind disturbs graphics push state only");
    write.dstArrayElement = 1;
    on_push_descriptors(&d, &cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &write);
    check(cb.sets[1].pushed->values.count({2, 0}) == 0 && cb.sets[1].pushed->values.count({5, 0}) == 0,
          "a push after disturbance begins with undefined descriptors");

    auto child_handle = h<VkCommandBuffer>(11);
    auto child = std::make_unique<CmdBuf>();
    DrawRec draw; draw.sets[1] = first_draw; child->draws.push_back(draw);
    d.cbs[child_handle] = std::move(child);
    on_execute_commands(&d, &cb, 1, &child_handle);
    d.cbs.at(child_handle)->reset_recording();
    check(cb.draws[0].sets[1].pushed->values.at({2, 0}).offset == 64,
          "flattened secondary draws retain their own immutable push values");
    check(!cb.sets[1].pushed && !cb.comp_sets[1].pushed, "executing secondaries invalidates primary descriptor bindings");
    on_destroy_pipeline_layout(&d, pl); on_destroy_set_layout(&d, pushed);
    check(cb.draws[0].sets[1].pushed->layout->bindings.size() == 2, "recorded push retains its layout definition");
    cb.reset_recording();
    check(cb.draws.empty() && !cb.sets[1].pushed && !cb.comp_sets[1].pushed, "reset drops push draws and both bind points");
}

static void shaders_and_views()
{
    // Only the dispatch key is inspected; no function is called on this fake device.
    void* dispatch = nullptr;
    Device* d = dev_create(reinterpret_cast<VkDevice>(&dispatch));
    auto module = h<VkShaderModule>(1), module2 = h<VkShaderModule>(2);
    auto pipe = h<VkPipeline>(3);
    auto code = spv::vertex_shader();
    VkShaderModuleCreateInfo sm = {};
    sm.codeSize = code.size() * 4; sm.pCode = code.data();
    on_create_shader(d, module, &sm);
    uint32_t shader_id = d->shaders.at(module)->id;
    VkPipelineShaderStageCreateInfo stage = {};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = module; stage.pName = "main";
    VkGraphicsPipelineCreateInfo info = {};
    info.stageCount = 1; info.pStages = &stage;
    on_create_graphics_pipeline(d, pipe, &info);
    on_destroy_shader(d, module);
    std::vector<uint8_t> payload;
    check(probe_shader(shader_id, true, payload).find("\"ok\":true") != std::string::npos &&
          payload.size() == code.size() * 4 && !memcmp(payload.data(), code.data(), payload.size()),
          "pipeline retains exact SPIR-V bytes after module destruction");
    on_create_shader(d, module, &sm);
    check(d->shaders.at(module)->id != shader_id && d->pipes.at(pipe).shaders[0].shader->id == shader_id,
          "shader module handle reuse does not change old pipeline stages");
    on_destroy_pipeline(d, pipe);
    check(d->shader_by_id.count(shader_id) == 0, "last pipeline destruction reclaims old shader bytes and id");
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    stage.module = VK_NULL_HANDLE; stage.pNext = &sm;
    on_create_graphics_pipeline(d, pipe, &info);
    check(d->pipes.at(pipe).shaders[0].shader != nullptr, "inline pipeline SPIR-V is retained");
    on_destroy_pipeline(d, pipe); on_destroy_shader(d, module); on_destroy_shader(d, module2);
    check(d->shader_by_id.empty(), "all unreferenced shader metadata is reclaimed");

    auto buffer = h<VkBuffer>(11);
    auto view = h<VkBufferView>(12);
    VkBufferCreateInfo bi = {}; bi.size = 1024; bi.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    on_create_buffer(d, buffer, &bi);
    VkBufferViewCreateInfo bv = {}; bv.buffer = buffer; bv.format = VK_FORMAT_R32_SFLOAT; bv.offset = 64; bv.range = 128;
    on_create_buffer_view(d, view, &bv);
    check(d->buffer_views.at(view).buffer_id == d->bufs.at(buffer).id && d->buffer_views.at(view).range == 128,
          "texel view keeps buffer identity and range");
    on_destroy_buffer_view(d, view);
    check(d->buffer_views.empty(), "buffer view destruction is tracked");
    auto image = h<VkImage>(13);
    auto image_view = h<VkImageView>(14);
    VkImageCreateInfo im = {}; im.extent = {64, 32, 1}; im.format = VK_FORMAT_R8G8B8A8_UNORM; im.mipLevels = 3; im.arrayLayers = 2;
    on_create_image(d, image, &im);
    VkImageViewCreateInfo iv = {}; iv.image = image; iv.format = im.format; iv.subresourceRange = {1, 1, 2, 0, 2};
    on_create_image_view(d, image_view, &iv);
    check(d->image_views.at(image_view).image_id == d->images.at(image).id && d->image_views.at(image_view).range.baseMipLevel == 1,
          "image view keeps image identity and subresource range");
    on_destroy_image(d, image); on_destroy_image_view(d, image_view);
    check(d->images.empty() && d->image_views.empty(), "image and view destruction are tracked");
    dev_destroy(d);
}

// What keeps the tracker's work and memory bounded in a process with many pools and shaders.
static void pools_and_budgets()
{
    Device d;
    auto pool_a = h<VkDescriptorPool>(1), pool_b = h<VkDescriptorPool>(2);
    auto sl = h<VkDescriptorSetLayout>(3);
    layout(d, sl, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    auto s1 = h<VkDescriptorSet>(10), s2 = h<VkDescriptorSet>(11), s3 = h<VkDescriptorSet>(12);
    allocate(d, pool_a, sl, s1); allocate(d, pool_a, sl, s2); allocate(d, pool_b, sl, s3);
    check(d.pool_sets.at(pool_a).size() == 2 && d.pool_sets.at(pool_b).size() == 1, "sets are indexed by their pool");
    on_free_sets(&d, 1, &s1);
    check(d.pool_sets.at(pool_a).size() == 1 && d.descriptor_sets.count(s1) == 0, "a freed set leaves the index of its pool");
    // The handle of a set whose release was not seen comes back from another pool.
    allocate(d, pool_b, sl, s2);
    check(d.descriptor_sets.at(s2).pool == pool_b && d.pool_sets.at(pool_a).count(s2) == 0,
          "a reused handle moves to its new pool");
    on_clear_descriptor_pool(&d, pool_a);
    check(d.descriptor_sets.count(s2) == 1 && d.descriptor_sets.count(s3) == 1 && d.pool_sets.count(pool_a) == 0,
          "resetting the old pool keeps the reused handle");
    on_clear_descriptor_pool(&d, pool_b);
    check(d.descriptor_sets.empty() && d.pool_sets.empty(), "resetting the new pool removes its sets");
    on_clear_descriptor_pool(&d, h<VkDescriptorPool>(99));
    on_free_sets(&d, 0, nullptr);
    check(d.descriptor_sets.empty(), "an unknown pool and an empty free are harmless");

    auto code = spv::vertex_shader();
    VkShaderModuleCreateInfo sm = {};
    sm.codeSize = code.size() * 4; sm.pCode = code.data();
    const uint64_t one = code.size() * 4;
    const uint64_t saved = g_cfg.shader_budget;
    g_cfg.shader_budget = one + one / 2; // room for one module
    auto m1 = h<VkShaderModule>(20), m2 = h<VkShaderModule>(21), m3 = h<VkShaderModule>(22);
    on_create_shader(&d, m1, &sm); on_create_shader(&d, m2, &sm);
    check(!d.shaders.at(m1)->code.empty() && d.shaders.at(m2)->code.empty() && d.shaders.at(m2)->size == one &&
          d.shaders.at(m2)->hash == d.shaders.at(m1)->hash, "over the budget the code is dropped, hash and size stay");
    check(d.shader_bytes->load() == one && d.shaders_not_retained == 1, "kept bytes and dropped modules are counted");
    std::vector<uint8_t> payload;
    check(probe_shader(d.shaders.at(m2)->id, true, payload).find("\"ok\":true") == std::string::npos && payload.empty(),
          "a module without code is not served as an empty shader");
    on_destroy_shader(&d, m1);
    check(d.shader_bytes->load() == 0, "destroying a module gives its bytes back");
    on_create_shader(&d, m3, &sm);
    check(!d.shaders.at(m3)->code.empty(), "the freed budget is used again");
    const uint32_t old_id = d.shaders.at(m3)->id;
    on_create_shader(&d, m3, &sm);
    check(d.shader_by_id.count(old_id) == 0 && d.shader_bytes->load() == one && !d.shaders.at(m3)->code.empty(),
          "a reused module handle releases the old entry before the new one is measured");
    // A pipeline keeps a destroyed module alive; only the last reference releases it.
    VkPipelineShaderStageCreateInfo stage = {};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = m3; stage.pName = "main";
    VkGraphicsPipelineCreateInfo info = {};
    info.stageCount = 1; info.pStages = &stage;
    auto pipe = h<VkPipeline>(30);
    on_create_graphics_pipeline(&d, pipe, &info);
    const uint32_t kept_id = d.shaders.at(m3)->id;
    on_destroy_shader(&d, m3);
    check(d.shader_by_id.count(kept_id) == 1 && d.shader_bytes->load() == one, "a pipeline keeps the code of a destroyed module");
    on_create_graphics_pipeline(&d, pipe, &info); // handle reused; the module is gone, so no shader this time
    check(d.shader_by_id.count(kept_id) == 0 && d.shader_bytes->load() == 0, "replacing the pipeline releases the old shader");
    on_destroy_pipeline(&d, pipe); on_destroy_shader(&d, m2);
    check(d.shader_by_id.empty() && d.shaders.empty() && d.shader_bytes->load() == 0, "every shader entry and byte is reclaimed");
    g_cfg.shader_budget = saved;

    // Vertex input and input assembly pointers of a mesh pipeline are ignored by Vulkan and may be garbage.
    VkPipelineShaderStageCreateInfo mesh = {};
    mesh.stage = VK_SHADER_STAGE_MESH_BIT_EXT; mesh.pName = "main";
    VkGraphicsPipelineCreateInfo mi = {};
    mi.stageCount = 1; mi.pStages = &mesh;
    mi.pVertexInputState = reinterpret_cast<const VkPipelineVertexInputStateCreateInfo*>(static_cast<uintptr_t>(8));
    mi.pInputAssemblyState = reinterpret_cast<const VkPipelineInputAssemblyStateCreateInfo*>(static_cast<uintptr_t>(8));
    on_create_graphics_pipeline(&d, h<VkPipeline>(31), &mi);
    check(d.pipes.at(h<VkPipeline>(31)).bindings.empty() && d.pipes.at(h<VkPipeline>(31)).topology == 0,
          "a mesh pipeline does not read its vertex input pointers");
}

static void present_pacing()
{
    PresentPacing p;
    uint64_t t = 1000000;
    p.observe(t, 0, 100, false);
    check(p.intervals == 0 && p.size == 0, "first present has no earlier interval");
    p.observe(t += 8000, 1, 110, false);
    p.observe(t += 8000, 2, 110, false);
    p.observe(t += 80000, 3, 60110, true);
    p.observe(t += 8000, 4, 60112, false);
    check(p.intervals == 4 && p.over_16ms == 1 && p.over_33ms == 1 && p.over_50ms == 1,
          "one hitch remains visible among fast present intervals");
    check(p.peak.us == 80000 && p.peak.frame == 3 && p.peak.image_hash_us == 60000 && p.peak.capture && p.captures == 1 &&
          p.peak.end_us == t - 8000,
          "peak preserves its frame, upload cost and capture marker");
    for (size_t i = 0; i < PresentPacing::capacity + 7; ++i)
        p.observe(t += 8000, i + 5, 60112, false);
    check(p.size == PresentPacing::capacity && p.intervals == PresentPacing::capacity + 11 && p.peak.us == 80000,
          "bounded window rolls while lifetime hitch evidence stays");
    bool all_fast = true;
    for (const auto& s : p.samples) all_fast &= s.us == 8000 && !s.capture;
    check(all_fast, "old hitch leaves the recent window");
    p.observe(t += 8000, 9999, 0, false);
    check(p.samples[(p.next + p.capacity - 1) % p.capacity].image_hash_us == 0,
          "a reset upload counter does not underflow");
    const auto n = p.intervals;
    p.observe(t, 10000, 0, false);
    p.observe(t - 1, 10001, 0, false);
    check(p.intervals == n, "non-increasing timestamps do not invent intervals");
}

static void tracker_work()
{
    TrackerWork work;
    work.record(WK_UNMAP, 1000, 81000, 16ull << 20);
    work.record(WK_UNMAP, 82000, 82010, 32);
    auto s = work.snapshot();
    check(s.totals[WK_UNMAP].calls == 2 && s.totals[WK_UNMAP].us == 80010 && s.totals[WK_UNMAP].max_us == 80000 &&
          s.totals[WK_UNMAP].bytes == (16ull << 20) + 32, "work counters retain a large copy among cheap calls");
    check(s.size == 1 && s.slow_count == 1 && s.events[0].start_us == 1000 && s.events[0].us == 80000,
          "only slow work enters the timestamped history");
    for (uint64_t i = 0; i < 70; ++i) work.record(WK_SUBMIT, 100000 + i * 10000, 102000 + i * 10000, i);
    s = work.snapshot();
    check(s.size == 64 && s.slow_count == 71 && s.events[0].start_us == 160000 && s.events[63].bytes == 69,
          "slow work history is bounded and ordered across rollover");
    check(s.totals[WK_UNMAP].max_us == 80000, "lifetime work peak survives history rollover");
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([&work] {
        for (int j = 0; j < 1000; ++j) work.record(WK_DRAW, 1, 2, 3);
    });
    for (auto& writer : writers) writer.join();
    s = work.snapshot();
    check(s.totals[WK_DRAW].calls == 4000 && s.totals[WK_DRAW].us == 4000 && s.totals[WK_DRAW].bytes == 12000,
          "concurrent recording threads do not lose work counts");
    const auto enabled = g_cfg.profile_tracker;
    g_cfg.profile_tracker = false;
    { WorkScope scope(work, WK_DRAW); scope.bytes = 200; }
    check(work.snapshot().totals[WK_DRAW].calls == 4000, "disabled profiling records no work");
    g_cfg.profile_tracker = enabled;
}

static void buffer_copy_paths()
{
    const auto config = g_cfg;
    g_cfg.shadow_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    g_cfg.unmap_snapshot_limit = 0;
    g_cfg.profile_tracker = true;
    Device d;
    const auto source = h<VkBuffer>(201), target = h<VkBuffer>(202);
    const auto memory = h<VkDeviceMemory>(203);
    const auto cmd = h<VkCommandBuffer>(204);
    VkMemoryAllocateInfo alloc = {}; alloc.allocationSize = 64;
    on_alloc_memory(&d, memory, &alloc);
    VkBufferCreateInfo info = {}; info.size = 64; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    on_create_buffer(&d, source, &info); on_bind_buffer(&d, source, memory, 0);
    info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    on_create_buffer(&d, target, &info);
    VkCommandBufferAllocateInfo ci = {}; ci.commandBufferCount = 1;
    on_alloc_cbs(&d, &ci, &cmd);
    auto* cb = d.cbs.at(cmd).get();
    std::vector<uint8_t> mapped(64, 7), result(16);
    on_map_memory(&d, memory, 0, mapped.size(), mapped.data());
    on_copy_buffer(&d, cb, source, target, 0, 0, 16);
    check(cb->writes[0].have_data && cb->writes[0].data == std::vector<uint8_t>(16, 7),
          "record-time fallback preserves the readable source range");
    std::fill(mapped.begin(), mapped.end(), 9);
    on_submit(&d, 1, &cmd);
    check(read_buffer_locked(&d, d.bufs.at(target), 0, 16, result.data()) == SRC_SHADOW &&
          result == std::vector<uint8_t>(16, 9), "submit-time bytes take priority over the earlier copy");
    auto totals = d.work.snapshot().totals;
    check(totals[WK_COPY_RECORD].bytes == 16 && totals[WK_BUFFER_SUBMIT].bytes == 16,
          "one retained record copy and one direct submit copy are accounted");
    cb->reset_recording();
    on_copy_buffer(&d, cb, source, target, 0, 16, 16);
    on_unmap_memory(&d, memory);
    check(d.bufs.at(source).shadow.empty(), "unmap snapshot limit zero keeps no staging copy");
    on_submit(&d, 1, &cmd);
    check(read_buffer_locked(&d, d.bufs.at(target), 16, 16, result.data()) == SRC_SHADOW &&
          result == std::vector<uint8_t>(16, 9), "record-time fallback still works after an unsnapshotted unmap");
    cb->reset_recording();
    g_cfg.unmap_snapshot_limit = 64;
    on_map_memory(&d, memory, 0, mapped.size(), mapped.data());
    std::fill(mapped.begin(), mapped.end(), 11);
    on_unmap_memory(&d, memory);
    on_copy_buffer(&d, cb, source, target, 0, 32, 16);
    on_submit(&d, 1, &cmd);
    check(read_buffer_locked(&d, d.bufs.at(target), 32, 16, result.data()) == SRC_SHADOW &&
          result == std::vector<uint8_t>(16, 11), "an unmap snapshot remains readable when recording happens later");
    check(d.work.snapshot().totals[WK_UNMAP].bytes == 64, "unmap work reports the full staging snapshot bytes");
    cb->reset_recording();
    on_copy_buffer(&d, cb, target, target, 32, 0, 16);
    on_submit(&d, 1, &cmd);
    check(read_buffer_locked(&d, d.bufs.at(target), 0, 16, result.data()) == SRC_SHADOW &&
          result == std::vector<uint8_t>(16, 11), "a disjoint self-copy uses the existing shadow without invalidating its source");
    cb->reset_recording();
    on_destroy_buffer(&d, source); on_destroy_buffer(&d, target);
    info.size = 64ull << 20;
    on_create_buffer(&d, source, &info); on_create_buffer(&d, target, &info);
    on_copy_buffer(&d, cb, source, target, 0, 0, info.size);
    check(!cb->writes[0].have_data && cb->writes[0].data.capacity() == 0,
          "an unreadable 64 MiB source allocates no fallback byte storage");
    on_submit(&d, 1, &cmd);
    check(d.bufs.at(target).shadow.empty() && d.bufs.at(target).unknown_writes == 1,
          "an unreadable submitted copy stays unknown without allocating its destination shadow");
    g_cfg = config;
}

static void mapped_read_paths()
{
    SYSTEM_INFO system = {}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT,
                                                         PAGE_READWRITE | PAGE_WRITECOMBINE));
    check(allocation != nullptr, "allocate real write-combined pages for mapped-read coverage");
    if (!allocation) return;
    DWORD old = 0;
    const bool guarded = VirtualProtect(allocation, page, PAGE_NOACCESS, &old) &&
                         VirtualProtect(allocation + page * 2, page, PAGE_NOACCESS, &old);
    check(guarded, "surround the source page with inaccessible pages");
    if (!guarded) { VirtualFree(allocation, 0, MEM_RELEASE); return; }
    auto* src = allocation + page;
    std::vector<uint8_t> expected(page), output(page + 128), want(page + 128);
    for (size_t i = 0; i < page; ++i) expected[i] = static_cast<uint8_t>(i * 13 + 5);
    memcpy(src, expected.data(), page);
    check(mapped_range_is_write_combined(src, page), "OS policy identifies the entire WC mapping");
    check(!mapped_range_is_write_combined(expected.data(), expected.size()), "ordinary heap is not marked WC");
    check(!mapped_range_is_write_combined(src + page - 1, 2) &&
          !mapped_range_is_write_combined(nullptr, 0), "mixed/inaccessible and empty ranges fall back");

    bool exact = true;
    for (size_t offset = 0; offset < 64; ++offset) {
        for (size_t size : {size_t(0), size_t(1), size_t(15), size_t(16), size_t(17), size_t(63),
                            size_t(64), size_t(65), size_t(127), size_t(128), size_t(129), size_t(257)}) {
            std::fill(output.begin(), output.end(), 0xa5);
            want = output;
            const size_t dst_offset = (offset * 7) % 64 + 1;
            memcpy(want.data() + dst_offset, expected.data() + offset, size);
            copy_buffer_bytes(output.data() + dst_offset, src + offset, size, true);
            exact = exact && output == want;
        }
    }
    check(exact, "WC copy handles all source alignments and unaligned destinations without extra writes");
    exact = true;
    for (size_t size = 0; size <= 513; ++size) {
        copy_buffer_bytes(output.data(), src + page - size, size, true);
        exact = exact && std::equal(output.begin(), output.begin() + size, expected.end() - size);
    }
    check(exact, "WC tails end exactly at an inaccessible page without over-reading");
    copy_buffer_bytes(output.data(), src, page, false);
    check(std::equal(expected.begin(), expected.end(), output.begin()), "ordinary fallback preserves WC contents");
    want = expected;
    memmove(want.data() + 3, want.data(), 200);
    copy_buffer_bytes(expected.data() + 3, expected.data(), 200, false);
    check(expected == want, "cached-shadow fallback retains overlapping memmove semantics");

    const auto config = g_cfg;
    g_cfg.shadow_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    g_cfg.unmap_snapshot_limit = 1024;
    g_cfg.profile_tracker = true;
    Device d;
    const auto source = h<VkBuffer>(301), target = h<VkBuffer>(302);
    const auto memory = h<VkDeviceMemory>(303);
    const auto cmd = h<VkCommandBuffer>(304);
    VkMemoryAllocateInfo alloc = {}; alloc.allocationSize = 1024;
    on_alloc_memory(&d, memory, &alloc);
    VkBufferCreateInfo info = {}; info.size = 257; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    on_create_buffer(&d, source, &info); on_bind_buffer(&d, source, memory, 131);
    info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    on_create_buffer(&d, target, &info);
    VkCommandBufferAllocateInfo ci = {}; ci.commandBufferCount = 1;
    on_alloc_cbs(&d, &ci, &cmd);
    auto* cb = d.cbs.at(cmd).get();
    on_map_memory(&d, memory, 128, 512, src + 32);
    check(d.mems.at(memory).write_combined, "map records page policy with a nonzero memory offset");
    std::vector<uint8_t> first(257, 19), second(257, 23), result(257);
    memcpy(src + 35, first.data(), first.size());
    on_copy_buffer(&d, cb, source, target, 0, 0, 257);
    check(cb->writes[0].data == first, "record fallback copies an unaligned WC source exactly");
    memcpy(src + 35, second.data(), second.size());
    on_submit(&d, 1, &cmd);
    check(d.bufs.at(target).shadow == second, "submit reads newer WC bytes before the record fallback");
    check(read_buffer_locked(&d, d.bufs.at(source), 0, result.size(), result.data()) == SRC_MAPPED &&
          result == second, "direct probe reads use the same bounded mapped-copy path");
    on_unmap_memory(&d, memory);
    check(!d.mems.at(memory).write_combined && !d.mems.at(memory).mapped && d.bufs.at(source).shadow == second,
          "unmap retains every source byte and clears the mapped policy");
    cb->reset_recording();
    on_copy_buffer(&d, cb, source, target, 0, 0, 257);
    on_submit(&d, 1, &cmd);
    check(d.bufs.at(target).shadow == second && d.work.snapshot().totals[WK_UNMAP].bytes == 257,
          "later buffer copies retain the full WC unmap snapshot");
    std::vector<uint8_t> cached(1024, 29);
    on_map_memory(&d, memory, 0, VK_WHOLE_SIZE, cached.data());
    check(!d.mems.at(memory).write_combined, "a remap recomputes policy instead of reusing the WC flag");
    on_unmap_memory(&d, memory);
    check(d.bufs.at(source).shadow == std::vector<uint8_t>(257, 29), "ordinary remapped snapshots remain exact");
    g_cfg = config;
    VirtualFree(allocation, 0, MEM_RELEASE);
}

// What is copied when memory is unmapped: staging buffers, not buffers that only happen to have the
// transfer-source bit, such as the index and vertex pools of a game.
static void unmap_selection()
{
    const auto config = g_cfg;
    g_cfg.shadow_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    g_cfg.unmap_snapshot_limit = 64;
    g_cfg.unmap_snapshot_all = false;
    Device d;
    const auto pool = h<VkBuffer>(401), staging = h<VkBuffer>(402), scratch = h<VkBuffer>(403), half = h<VkBuffer>(404);
    const auto memory = h<VkDeviceMemory>(405);
    const auto cmd = h<VkCommandBuffer>(406);
    VkMemoryAllocateInfo alloc = {}; alloc.allocationSize = 128;
    on_alloc_memory(&d, memory, &alloc);
    VkBufferCreateInfo info = {}; info.size = 32;
    info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    on_create_buffer(&d, pool, &info); on_bind_buffer(&d, pool, memory, 0);
    info.size = 16; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    on_create_buffer(&d, staging, &info); on_bind_buffer(&d, staging, memory, 32);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    on_create_buffer(&d, scratch, &info); on_bind_buffer(&d, scratch, memory, 48);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    on_create_buffer(&d, half, &info); on_bind_buffer(&d, half, memory, 88); // the mapping below ends inside it
    VkCommandBufferAllocateInfo ci = {}; ci.commandBufferCount = 1;
    on_alloc_cbs(&d, &ci, &cmd);
    auto* cb = d.cbs.at(cmd).get();
    std::vector<uint8_t> mapped(96), result(32);
    for (size_t i = 0; i < mapped.size(); ++i) mapped[i] = static_cast<uint8_t>(i + 1);
    on_map_memory(&d, memory, 0, mapped.size(), mapped.data());
    on_copy_buffer(&d, cb, staging, pool, 0, 0, 16);
    on_submit(&d, 1, &cmd);
    check(d.bufs.at(pool).known.total() == 16, "a tracked copy into a mapped pool is shadowed");
    on_unmap_memory(&d, memory);
    check(d.bufs.at(staging).known.total() == 16 && d.bufs.at(scratch).known.total() == 16,
          "buffers that are transfer buffers and nothing else are copied at unmap");
    check(d.bufs.at(pool).known.total() == 0 && read_buffer_locked(&d, d.bufs.at(pool), 0, 16, result.data()) == SRC_NONE,
          "a pool with the transfer-source bit is not copied and its older shadow is no longer served");
    check(d.bufs.at(half).shadow.empty(), "a buffer the mapping covers only in part is not copied");
    check(d.unmap_taken == 2 && d.unmap_taken_bytes == 32 && d.unmap_passed == 1 && d.unmap_passed_bytes == 32 &&
          d.unmap_invalidated == 1, "unmap counters: two copies, one pool passed over, one shadow dropped");

    g_cfg.unmap_snapshot_all = true;
    on_map_memory(&d, memory, 0, mapped.size(), mapped.data());
    mapped[5] = 99;
    on_unmap_memory(&d, memory);
    check(read_buffer_locked(&d, d.bufs.at(pool), 0, 32, result.data()) == SRC_SHADOW && result[5] == 99 && result[31] == 32,
          "with unmap_snapshot_all every whole transfer source is copied, as before 0.7.4");
    check(d.unmap_taken == 5 && d.unmap_passed == 1, "the pool counts as copied then, not as passed over");

    g_cfg.unmap_snapshot_limit = 0;
    on_map_memory(&d, memory, 0, mapped.size(), mapped.data());
    on_unmap_memory(&d, memory);
    check(d.bufs.at(pool).known.total() == 0 && d.bufs.at(staging).known.total() == 0 && d.unmap_taken == 5,
          "a limit of zero copies nothing and still serves no stale shadow");
    g_cfg = config;
}

static bool wait_capture_armed(Device* dev)
{
    const ULONGLONG deadline = GetTickCount64() + 1000;
    do {
        {
            std::lock_guard<std::mutex> lock(dev->frame_mx);
            if (dev->cap_state == 1) return true;
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

static void capture_empty_intervals()
{
    // Registered fake device so the real probe request/wait/serialization path is exercised.
    // The test supplies presents and submissions directly; it calls no Vulkan driver.
    void* dispatch_key = &dispatch_key;
    Device* dev = dev_create(reinterpret_cast<VkDevice>(&dispatch_key));
    Device& d = *dev;
    const auto buffer = h<VkBuffer>(301), target = h<VkBuffer>(302);
    const auto memory = h<VkDeviceMemory>(303);
    const auto cmd = h<VkCommandBuffer>(304), upload = h<VkCommandBuffer>(305);
    const auto sl = h<VkDescriptorSetLayout>(306);
    const auto pl = h<VkPipelineLayout>(307);
    const auto set = h<VkDescriptorSet>(308);
    VkMemoryAllocateInfo ai = {}; ai.allocationSize = 64;
    on_alloc_memory(dev, memory, &ai);
    VkBufferCreateInfo bi = {}; bi.size = 64;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    on_create_buffer(dev, buffer, &bi); on_bind_buffer(dev, buffer, memory, 0);
    on_create_buffer(dev, target, &bi);
    std::vector<uint8_t> mapped(64, 17);
    on_map_memory(dev, memory, 0, mapped.size(), mapped.data());
    layout(d, sl, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    pipeline_layout(d, pl, {sl});
    allocate(d, h<VkDescriptorPool>(309), sl, set);
    VkDescriptorBufferInfo buf = {buffer, 0, 64};
    VkWriteDescriptorSet write = {}; write.dstSet = set; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &buf;
    on_update_sets(dev, 1, &write, 0, nullptr);
    VkCommandBuffer cmds[] = {cmd, upload};
    VkCommandBufferAllocateInfo ci = {}; ci.commandBufferCount = 2;
    on_alloc_cbs(dev, &ci, cmds);
    CmdBuf* cb = d.cbs.at(cmd).get();
    CmdBuf* other = d.cbs.at(upload).get();
    on_bind_sets(dev, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);

    auto req = std::make_shared<SnapRequest>(); req->all = true; req->max_range = 64;
    req->follow = true; req->follow_count = 0; req->follow_entry = 16;
    std::string answer;
    std::thread reader([&] { answer = probe_capture(2000, true, 1 << 20, req, true); });
    bool armed = wait_capture_armed(dev);
    check(armed, "nonempty capture request armed");
    if (!armed) { reader.join(); dev_destroy(dev); return; }
    on_present(dev);
    check(d.snap_recording.load() && std::atomic_load(&d.snap_live) == req,
          "snapshot reads start at the first boundary");
    DrawRec draw; draw.kind = DK_INDEXED; draw.count = 3; draw.instances = 1;
    draw.ib = buffer; draw.index_size = 2; draw.sets[0] = cb->sets[0];
    cb->draws.push_back(draw); on_record_indexed(dev, cb);
    check(cb->rec_snaps.size() == 1 && cb->rec_snaps[0].ranges.size() == 2,
          "first draw records uniform and entry before an intervening present");
    on_copy_buffer(dev, other, buffer, target, 0, 0, 16);
    DrawRec dispatch; dispatch.kind = DK_DISPATCH; other->draws.push_back(dispatch);
    on_submit(dev, 1, &upload);
    on_present(dev);
    check(d.cap_state == 2 && d.cap_seq == 0 && d.snap_recording.load(),
          "upload and dispatch interval does not end nonempty capture");
    check(d.cap_building->draws.empty() && d.cap_building->descriptor_bytes == 0,
          "skipped interval contents and descriptor budget are discarded");
    check(d.bufs.at(target).known.covers(0, 16), "tracked writes in skipped intervals still take effect");
    mapped[0] = 29;
    cb->draws.push_back(draw); on_record_indexed(dev, cb);
    on_present(dev);
    check(d.cap_building->skipped_empty_intervals == 2 && cb->rec_snaps.size() == 2,
          "record-time reads stay armed across several empty intervals");
    on_submit(dev, 1, &cmd);
    on_present(dev);
    reader.join();
    auto cap = d.cap_last;
    check(answer.find("\"ok\":true") != std::string::npos && cap && cap->draws.size() == 2,
          "nonempty request returns the graphics submission");
    check(cap && cap->wait_draws && cap->skipped_empty_intervals == 2 &&
          answer.find("\"skipped_empty_intervals\":2") != std::string::npos,
          "nonempty selection metadata is serialized");
    check(cap && cap->stats.submits == 1 && cap->stats.draws == 2 && cap->stats.dispatches == 0,
          "returned statistics cover only the selected interval");
    check(cap && cap->snap_draws == 2 && cap->draws[0].snaps.size() == 2 &&
          cap->draws[0].snaps[0].rec == REC_DIFFERENT && cap->draws[1].snaps[0].rec == REC_EQUAL,
          "record/submit comparisons survive empty intervals and still notice later writes");
    check(d.cap_state == 0 && !d.capture_targets.load() && !d.snap_recording.load() && !std::atomic_load(&d.snap_live),
          "successful wait stops record-time snapshot work");

    std::thread ordinary([&] { answer = probe_capture(2000, true, 0); });
    armed = wait_capture_armed(dev);
    check(armed, "ordinary capture can follow the nonempty capture");
    if (armed) { on_present(dev); on_present(dev); }
    ordinary.join();
    check(answer.find("\"ok\":true") != std::string::npos && d.cap_last->draws.empty() &&
          !d.cap_last->wait_draws && d.cap_last->skipped_empty_intervals == 0,
          "default mode still returns an empty interval without waiting");
    const uint64_t completed = d.cap_seq;
    std::thread timeout([&] { answer = probe_capture(100, true, 1024, req, true); });
    armed = wait_capture_armed(dev);
    check(armed, "timeout case armed");
    if (armed) { on_present(dev); on_present(dev); on_present(dev); }
    timeout.join();
    check(answer.find("timeout: no interval containing draws") != std::string::npos,
          "empty presents cannot turn timeout into capture success");
    check(d.cap_state == 0 && !d.cap_building && !d.capture_targets.load() && !d.snap_recording.load() &&
          !std::atomic_load(&d.snap_live) && d.cap_seq == completed,
          "timeout clears active snapshot work and does not publish a partial capture");
    on_present(dev);
    check(d.cap_seq == completed, "later presents cannot publish a timed-out request");
    dev_destroy(dev);
}

#include "render_target_cases.h"

static unsigned sampler_destroys = 0;
static unsigned layout_destroys = 0;
static VKAPI_ATTR void VKAPI_CALL destroy_retained_sampler(VkDevice, VkSampler, const VkAllocationCallbacks*) { ++sampler_destroys; }
static VKAPI_ATTR void VKAPI_CALL destroy_retained_layout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {
    check(sampler_destroys == 0, "retained layout is destroyed before its immutable sampler"); ++layout_destroys;
}
static void immutable_sampler_lifetime()
{
    Device d; d.d.DestroySampler = destroy_retained_sampler;
    d.d.DestroyDescriptorSetLayout = destroy_retained_layout;
    const auto sampler = h<VkSampler>(301); const auto sl = h<VkDescriptorSetLayout>(302);
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    on_create_sampler(&d, sampler, &ci);
    layout(d, sl, {{0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler}});
    auto held = d.set_layouts.at(sl);
    check(on_destroy_sampler(&d, sampler) && sampler_destroys == 0,
          "application sampler destruction waits for immutable layout owners");
    check(on_destroy_set_layout(&d, sl) && layout_destroys == 0, "application layout destruction waits for queued metadata owners");
    check(sampler_destroys == 0 && held->bindings.at(0).immutable_owners.at(0)->handle == sampler,
          "queued layout metadata retains the exact immutable sampler handle");
    held.reset();
    check(sampler_destroys == 1 && layout_destroys == 1, "last retained owner destroys the layout and sampler once");
    VkAllocationCallbacks allocator{};
    on_create_sampler(&d, sampler, &ci, &allocator);
    check(!on_destroy_sampler(&d, sampler) && sampler_destroys == 1,
          "custom allocator sampler destruction remains synchronous in the hook");
    on_create_sampler(&d, sampler, &ci);
    d.samplers.clear();
    check(sampler_destroys == 1, "metadata alone never destroys an application-owned sampler");
}

int main()
{
    util_init(GetModuleHandleW(nullptr));
    descriptor_writes();
    descriptor_binding();
    descriptor_templates();
    pushed_descriptors();
    shaders_and_views();
    pools_and_budgets();
    present_pacing();
    tracker_work();
    buffer_copy_paths();
    mapped_read_paths();
    unmap_selection();
    character_cache_cases(check);
    capture_empty_intervals();
    render_target_cases();
    immutable_sampler_lifetime();
    printf("tracker_test: %d/%d checks passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
