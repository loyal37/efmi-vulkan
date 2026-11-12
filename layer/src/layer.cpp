// Entry points and the intercepted Vulkan functions.
//
// Ordinary probe hooks observe and forward. An explicitly configured M2 package can replace
// its one matching component through the separately gated experimental backend.
//
// The hooks get in front of the application in one of two ways:
//   layer  - the Vulkan loader loads the DLL as a layer and chains it in through
//            vkNegotiateLoaderLayerInterfaceVersion and the link info of create calls;
//   inject - the DLL was loaded with LoadLibrary; bootstrap.cpp redirects the exports of
//            vulkan-1.dll to the hooks, which then call the original exports.
#include "state.h"
#include "character_module.h"
#include "m2_backend.h"
#include "character_backend.h"
#include "notices.h"

#include <cstddef>
#include <mutex>

using namespace efvk;

#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion")
#pragma comment(linker, "/EXPORT:efvkGetInstanceProcAddr")
#pragma comment(linker, "/EXPORT:efvkGetDeviceProcAddr")

namespace {

struct InstanceData {
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    uint32_t app_api_version = 0;
};

// Win32 surfaces created through this layer and their windows, for every instance.
std::mutex g_surface_mx;
std::unordered_map<VkSurfaceKHR, HWND> g_surfaces;

std::mutex g_inst_mx;
std::unordered_map<void*, InstanceData*> g_insts;
std::once_flag g_pin_once;

// Inject entry: the addresses the redirected exports of vulkan-1.dll had.
PFN_vkGetInstanceProcAddr g_real_gipa = nullptr;
PFN_vkGetDeviceProcAddr g_real_gdpa = nullptr;
PFN_vkCreateInstance g_real_create_instance = nullptr;
PFN_vkDestroyInstance g_real_destroy_instance = nullptr;
PFN_vkCreateDevice g_real_create_device = nullptr;
PFN_vkCreateWin32SurfaceKHR g_real_create_surface = nullptr;
PFN_vkDestroySurfaceKHR g_real_destroy_surface = nullptr;
Device g_passive_dev; // its dispatch table holds the original exports of the device functions

bool injected()
{
    return g_cfg.entry == Entry::Inject;
}

template <typename T> void* key_of(T handle)
{
    return *reinterpret_cast<void**>(handle);
}

InstanceData* inst_of_key(void* key)
{
    std::lock_guard<std::mutex> lock(g_inst_mx);
    auto it = g_insts.find(key);
    return it == g_insts.end() ? nullptr : it->second;
}

// The pipe thread lives until the process exits, so the module must not be
// unloaded when the loader releases the layer.
void pin_module()
{
    std::call_once(g_pin_once, [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&pin_module), &self);
    });
}

uint8_t index_size_of(VkIndexType t)
{
    switch (t) {
    case VK_INDEX_TYPE_UINT16: return 2;
    case VK_INDEX_TYPE_UINT32: return 4;
    case VK_INDEX_TYPE_UINT8: return 1;
    default: return 0;
    }
}

DrawRec make_rec(const CmdBuf* cb, uint8_t kind, bool compute)
{
    DrawRec r;
    r.kind = kind;
    r.cb_id = cb->id;
    r.tid = cb->tid;
    r.rp = cb->in_rp ? cb->rp_count + cb->dyn_rp_count : 0;
    r.targets = !compute && cb->in_rp ? cb->active_targets : 0;
    if (compute) {
        r.pipeline = cb->comp_pipeline;
        for (uint32_t i = 0; i < kMaxSets; i++)
            r.sets[i] = cb->comp_sets[i];
        return r;
    }
    r.pipeline = cb->gfx_pipeline;
    r.bound = cb->gfx_bound;
    r.ib = cb->ib;
    r.ib_off = cb->ib_off;
    r.index_size = cb->index_size;
    for (uint32_t i = 0; i < kMaxVb; i++)
        r.vb[i] = cb->vb[i];
    for (uint32_t i = 0; i < kMaxSets; i++)
        r.sets[i] = cb->sets[i];
    return r;
}

// ---------------------------------------------------------------------------
// Memory

VKAPI_ATTR VkResult VKAPI_CALL hk_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkDeviceMemory* mem)
{
    count_call(HI_AllocateMemory);
    Device* dev = dev_of(device);
    VkResult r = dev->d.AllocateMemory(device, info, alloc, mem);
    if (r == VK_SUCCESS)
        on_alloc_memory(dev, *mem, info);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_FreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks* alloc)
{
    count_call(HI_FreeMemory);
    Device* dev = dev_of(device);
    if (mem)
        on_free_memory(dev, mem);
    dev->d.FreeMemory(device, mem, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_MapMemory(VkDevice device, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size,
                                            VkMemoryMapFlags flags, void** data)
{
    count_call(HI_MapMemory);
    Device* dev = dev_of(device);
    VkResult r = dev->d.MapMemory(device, mem, off, size, flags, data);
    if (r == VK_SUCCESS)
        on_map_memory(dev, mem, off, size, *data);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_UnmapMemory(VkDevice device, VkDeviceMemory mem)
{
    count_call(HI_UnmapMemory);
    Device* dev = dev_of(device);
    on_unmap_memory(dev, mem);
    dev->d.UnmapMemory(device, mem);
}

VkResult map2(Device* dev, PFN_vkMapMemory2 next, VkDevice device, const VkMemoryMapInfo* info, void** data)
{
    VkResult r = next(device, info, data);
    if (r == VK_SUCCESS)
        on_map_memory(dev, info->memory, info->offset, info->size, *data);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_MapMemory2(VkDevice device, const VkMemoryMapInfo* info, void** data)
{
    count_call(HI_MapMemory2);
    Device* dev = dev_of(device);
    return map2(dev, dev->d.MapMemory2, device, info, data);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_MapMemory2KHR(VkDevice device, const VkMemoryMapInfo* info, void** data)
{
    count_call(HI_MapMemory2KHR);
    Device* dev = dev_of(device);
    return map2(dev, dev->d.MapMemory2KHR, device, info, data);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* info)
{
    count_call(HI_UnmapMemory2);
    Device* dev = dev_of(device);
    on_unmap_memory(dev, info->memory);
    return dev->d.UnmapMemory2(device, info);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_UnmapMemory2KHR(VkDevice device, const VkMemoryUnmapInfo* info)
{
    count_call(HI_UnmapMemory2KHR);
    Device* dev = dev_of(device);
    on_unmap_memory(dev, info->memory);
    return dev->d.UnmapMemory2KHR(device, info);
}

// ---------------------------------------------------------------------------
// Buffers and pipelines

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkBuffer* buf)
{
    count_call(HI_CreateBuffer);
    Device* dev = dev_of(device);
    VkResult r = dev->d.CreateBuffer(device, info, alloc, buf);
    if (r == VK_SUCCESS)
        on_create_buffer(dev, *buf, info);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyBuffer(VkDevice device, VkBuffer buf, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyBuffer);
    Device* dev = dev_of(device);
    if (buf)
        on_destroy_buffer(dev, buf);
    dev->d.DestroyBuffer(device, buf, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_BindBufferMemory(VkDevice device, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off)
{
    count_call(HI_BindBufferMemory);
    Device* dev = dev_of(device);
    VkResult r = dev->d.BindBufferMemory(device, buf, mem, off);
    if (r == VK_SUCCESS)
        on_bind_buffer(dev, buf, mem, off);
    return r;
}

VkResult bind2(Device* dev, PFN_vkBindBufferMemory2 next, VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    VkResult r = next(device, count, infos);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++)
            on_bind_buffer(dev, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    count_call(HI_BindBufferMemory2);
    Device* dev = dev_of(device);
    return bind2(dev, dev->d.BindBufferMemory2, device, count, infos);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    count_call(HI_BindBufferMemory2KHR);
    Device* dev = dev_of(device);
    return bind2(dev, dev->d.BindBufferMemory2KHR, device, count, infos);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                          const VkGraphicsPipelineCreateInfo* infos,
                                                          const VkAllocationCallbacks* alloc, VkPipeline* pipes)
{
    count_call(HI_CreateGraphicsPipelines);
    Device* dev = dev_of(device);
    VkResult r = dev->d.CreateGraphicsPipelines(device, cache, count, infos, alloc, pipes);
    // A batch can return an error after successfully creating some pipelines.
    for (uint32_t i = 0; i < count; i++)
        if (pipes[i]) {
            on_create_graphics_pipeline(dev, pipes[i], &infos[i]);
            m2_created(dev, pipes[i]);
        }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                         const VkComputePipelineCreateInfo* infos,
                                                         const VkAllocationCallbacks* alloc, VkPipeline* pipes)
{
    count_call(HI_CreateComputePipelines);
    Device* dev = dev_of(device);
    VkResult r = dev->d.CreateComputePipelines(device, cache, count, infos, alloc, pipes);
    for (uint32_t i = 0; i < count; i++)
        if (pipes[i])
            on_create_compute_pipeline(dev, pipes[i], &infos[i]);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyPipeline(VkDevice device, VkPipeline pipe, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyPipeline);
    Device* dev = dev_of(device);
    if (pipe) {
        m2_retire(dev, pipe);
        on_destroy_pipeline(dev, pipe);
    }
    dev->d.DestroyPipeline(device, pipe, alloc);
}

// ---------------------------------------------------------------------------
#include "resource_hooks.h"

// Command buffer lifetime

VKAPI_ATTR VkResult VKAPI_CALL hk_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                                         VkCommandBuffer* cbs)
{
    count_call(HI_AllocateCommandBuffers);
    Device* dev = dev_of(device);
    VkResult r = dev->d.AllocateCommandBuffers(device, info, cbs);
    if (r == VK_SUCCESS)
        on_alloc_cbs(dev, info, cbs);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                 const VkCommandBuffer* cbs)
{
    count_call(HI_FreeCommandBuffers);
    Device* dev = dev_of(device);
    on_free_cbs(dev, count, cbs);
    dev->d.FreeCommandBuffers(device, pool, count, cbs);
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyCommandPool);
    Device* dev = dev_of(device);
    if (pool)
        on_destroy_pool(dev, pool);
    dev->d.DestroyCommandPool(device, pool, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    count_call(HI_ResetCommandPool);
    Device* dev = dev_of(device);
    VkResult result = dev->d.ResetCommandPool(device, pool, flags);
    if (result == VK_SUCCESS) on_reset_pool(dev, pool);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info)
{
    count_call(HI_BeginCommandBuffer);
    Device* dev = dev_of(cmd);
    VkResult result = dev->d.BeginCommandBuffer(cmd, info);
    if (result != VK_SUCCESS) return result;
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_RESET);
        cb->reset_recording();
        cb->tid = GetCurrentThreadId();
        cb->threads.push_back(cb->tid);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_EndCommandBuffer(VkCommandBuffer cmd)
{
    count_call(HI_EndCommandBuffer);
    Device* dev = dev_of(cmd);
    return dev->d.EndCommandBuffer(cmd);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_ResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags)
{
    count_call(HI_ResetCommandBuffer);
    Device* dev = dev_of(cmd);
    VkResult result = dev->d.ResetCommandBuffer(cmd, flags);
    if (result != VK_SUCCESS) return result;
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_RESET);
        cb->reset_recording();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Bindings

VKAPI_ATTR void VKAPI_CALL hk_CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline)
{
    count_call(HI_CmdBindPipeline);
    Device* dev = dev_of(cmd);
    VkPipeline bound = pipeline;
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
            // The replacement code may bind the pipeline's twin without the character dither (D-133).
            cb->gfx_pipeline = pipeline;
            bound = m2_bind(dev, cb, pipeline);
            cb->gfx_bound = bound;
        } else if (point == VK_PIPELINE_BIND_POINT_COMPUTE)
            cb->comp_pipeline = pipeline;
    }
    dev->d.CmdBindPipeline(cmd, point, bound);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkIndexType type)
{
    count_call(HI_CmdBindIndexBuffer);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->ib = buf;
        cb->ib_off = off;
        cb->index_size = index_size_of(type);
    }
    dev->d.CmdBindIndexBuffer(cmd, buf, off, type);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindIndexBuffer2(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkDeviceSize size,
                                                  VkIndexType type)
{
    count_call(HI_CmdBindIndexBuffer2);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->m2_extended_bind = true;
        cb->ib = buf;
        cb->ib_off = off;
        cb->index_size = index_size_of(type);
    }
    dev->d.CmdBindIndexBuffer2(cmd, buf, off, size, type);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindIndexBuffer2KHR(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off,
                                                     VkDeviceSize size, VkIndexType type)
{
    count_call(HI_CmdBindIndexBuffer2KHR);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->m2_extended_bind = true;
        cb->ib = buf;
        cb->ib_off = off;
        cb->index_size = index_size_of(type);
    }
    dev->d.CmdBindIndexBuffer2KHR(cmd, buf, off, size, type);
}

void track_vbs(CmdBuf* cb, uint32_t first, uint32_t count, const VkBuffer* bufs, const VkDeviceSize* offs)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = first + i;
        if (slot >= kMaxVb)
            break;
        cb->vb[slot].buf = bufs ? bufs[i] : VK_NULL_HANDLE;
        cb->vb[slot].off = offs ? offs[i] : 0;
    }
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                   const VkBuffer* bufs, const VkDeviceSize* offs)
{
    count_call(HI_CmdBindVertexBuffers);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        track_vbs(cb, first, count, bufs, offs);
    dev->d.CmdBindVertexBuffers(cmd, first, count, bufs, offs);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindVertexBuffers2(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                    const VkBuffer* bufs, const VkDeviceSize* offs,
                                                    const VkDeviceSize* sizes, const VkDeviceSize* strides)
{
    count_call(HI_CmdBindVertexBuffers2);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->m2_extended_bind = true;
        track_vbs(cb, first, count, bufs, offs);
    }
    dev->d.CmdBindVertexBuffers2(cmd, first, count, bufs, offs, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindVertexBuffers2EXT(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                       const VkBuffer* bufs, const VkDeviceSize* offs,
                                                       const VkDeviceSize* sizes, const VkDeviceSize* strides)
{
    count_call(HI_CmdBindVertexBuffers2EXT);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->m2_extended_bind = true;
        track_vbs(cb, first, count, bufs, offs);
    }
    dev->d.CmdBindVertexBuffers2EXT(cmd, first, count, bufs, offs, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                    uint32_t first, uint32_t count, const VkDescriptorSet* sets,
                                                    uint32_t dyn_count, const uint32_t* dyn)
{
    count_call(HI_CmdBindDescriptorSets);
    Device* dev = dev_of(cmd);
    CmdBuf* cb = cb_of(dev, cmd);
    if (cb)
        on_bind_sets(dev, cb, point, layout, first, count, sets, dyn_count, dyn);
    dev->d.CmdBindDescriptorSets(cmd, point, layout, first, count, sets, dyn_count, dyn);
}

// ---------------------------------------------------------------------------
// Draws and dispatches

VKAPI_ATTR void VKAPI_CALL hk_CmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                      uint32_t first_instance)
{
    count_call(HI_CmdDraw);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_DRAW);
        cb->draws.push_back(make_rec(cb, DK_DRAW, false));
        DrawRec& r = cb->draws.back();
        r.count = vertices;
        r.instances = instances;
        r.first = first_vertex;
        r.first_instance = first_instance;
        if (m2_shader_draw(dev, cb, r)) return;
    }
    dev->d.CmdDraw(cmd, vertices, instances, first_vertex, first_instance);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances, uint32_t first_index,
                                             int32_t vertex_offset, uint32_t first_instance)
{
    count_call(HI_CmdDrawIndexed);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_DRAW);
        // Moved in and filled there: a copy would count every bound set's shared layout twice more.
        // Nothing appends to the list while the replacement records this draw.
        cb->draws.push_back(make_rec(cb, DK_INDEXED, false));
        DrawRec& r = cb->draws.back();
        r.count = indices;
        r.instances = instances;
        r.first = first_index;
        r.vertex_offset = vertex_offset;
        r.first_instance = first_instance;
        on_record_indexed(dev, cb);
        if (m2_record(dev, cb, r)) return;
    }
    dev->d.CmdDrawIndexed(cmd, indices, instances, first_index, vertex_offset, first_instance);
}

// True when the draw is to be dropped (a ShaderOverride or hunting, docs/vulkan-native-mods.md).
bool track_indirect(Device* dev, VkCommandBuffer cmd, uint8_t kind, VkBuffer buf, VkDeviceSize off, uint32_t count)
{
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_DRAW);
        cb->draws.push_back(make_rec(cb, kind, kind == DK_DISPATCH_INDIRECT));
        DrawRec& r = cb->draws.back();
        r.count = count;
        r.indirect = buf;
        r.indirect_off = off;
        if (kind == DK_INDIRECT || kind == DK_INDEXED_INDIRECT) return m2_shader_draw(dev, cb, r);
    }
    return false;
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, uint32_t count,
                                              uint32_t stride)
{
    count_call(HI_CmdDrawIndirect);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDIRECT, buf, off, count)) return;
    dev->d.CmdDrawIndirect(cmd, buf, off, count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, uint32_t count,
                                                     uint32_t stride)
{
    count_call(HI_CmdDrawIndexedIndirect);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDEXED_INDIRECT, buf, off, count)) return;
    dev->d.CmdDrawIndexedIndirect(cmd, buf, off, count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndirectCount(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
                                                   VkDeviceSize count_off, uint32_t max_count, uint32_t stride)
{
    count_call(HI_CmdDrawIndirectCount);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDIRECT, buf, off, max_count)) return;
    dev->d.CmdDrawIndirectCount(cmd, buf, off, count_buf, count_off, max_count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndirectCountKHR(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
                                                      VkDeviceSize count_off, uint32_t max_count, uint32_t stride)
{
    count_call(HI_CmdDrawIndirectCountKHR);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDIRECT, buf, off, max_count)) return;
    dev->d.CmdDrawIndirectCountKHR(cmd, buf, off, count_buf, count_off, max_count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off,
                                                          VkBuffer count_buf, VkDeviceSize count_off, uint32_t max_count,
                                                          uint32_t stride)
{
    count_call(HI_CmdDrawIndexedIndirectCount);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDEXED_INDIRECT, buf, off, max_count)) return;
    dev->d.CmdDrawIndexedIndirectCount(cmd, buf, off, count_buf, count_off, max_count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDrawIndexedIndirectCountKHR(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off,
                                                             VkBuffer count_buf, VkDeviceSize count_off,
                                                             uint32_t max_count, uint32_t stride)
{
    count_call(HI_CmdDrawIndexedIndirectCountKHR);
    Device* dev = dev_of(cmd);
    if (track_indirect(dev, cmd, DK_INDEXED_INDIRECT, buf, off, max_count)) return;
    dev->d.CmdDrawIndexedIndirectCountKHR(cmd, buf, off, count_buf, count_off, max_count, stride);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
    count_call(HI_CmdDispatch);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        WorkScope work(dev->work, WK_DRAW);
        cb->draws.push_back(make_rec(cb, DK_DISPATCH, true));
        DrawRec& r = cb->draws.back();
        r.count = x;
        r.instances = y;
        r.first = z;
    }
    dev->d.CmdDispatch(cmd, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off)
{
    count_call(HI_CmdDispatchIndirect);
    Device* dev = dev_of(cmd);
    track_indirect(dev, cmd, DK_DISPATCH_INDIRECT, buf, off, 1);
    dev->d.CmdDispatchIndirect(cmd, buf, off);
}

// ---------------------------------------------------------------------------
// Buffer writes recorded into command buffers

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t count,
                                            const VkBufferCopy* regions)
{
    count_call(HI_CmdCopyBuffer);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        for (uint32_t i = 0; i < count; i++)
            on_copy_buffer(dev, cb, src, dst, regions[i].srcOffset, regions[i].dstOffset, regions[i].size);
    dev->d.CmdCopyBuffer(cmd, src, dst, count, regions);
}

void track_copy2(Device* dev, VkCommandBuffer cmd, const VkCopyBufferInfo2* info)
{
    if (CmdBuf* cb = cb_of(dev, cmd))
        for (uint32_t i = 0; i < info->regionCount; i++)
            on_copy_buffer(dev, cb, info->srcBuffer, info->dstBuffer, info->pRegions[i].srcOffset,
                           info->pRegions[i].dstOffset, info->pRegions[i].size);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBuffer2(VkCommandBuffer cmd, const VkCopyBufferInfo2* info)
{
    count_call(HI_CmdCopyBuffer2);
    Device* dev = dev_of(cmd);
    track_copy2(dev, cmd, info);
    dev->d.CmdCopyBuffer2(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBuffer2KHR(VkCommandBuffer cmd, const VkCopyBufferInfo2* info)
{
    count_call(HI_CmdCopyBuffer2KHR);
    Device* dev = dev_of(cmd);
    track_copy2(dev, cmd, info);
    dev->d.CmdCopyBuffer2KHR(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer src, VkImage dst, VkImageLayout layout,
                                                   uint32_t count, const VkBufferImageCopy* regions)
{
    count_call(HI_CmdCopyBufferToImage);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        for (uint32_t i = 0; i < count; i++)
            on_copy_buffer_to_image(dev, cb, src, dst, regions[i].bufferOffset, regions[i].bufferRowLength,
                                    regions[i].bufferImageHeight, regions[i].imageSubresource, regions[i].imageOffset,
                                    regions[i].imageExtent);
    dev->d.CmdCopyBufferToImage(cmd, src, dst, layout, count, regions);
}

void track_image_copy2(Device* dev, VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info)
{
    if (CmdBuf* cb = cb_of(dev, cmd))
        for (uint32_t i = 0; i < info->regionCount; i++) {
            const VkBufferImageCopy2& r = info->pRegions[i];
            on_copy_buffer_to_image(dev, cb, info->srcBuffer, info->dstImage, r.bufferOffset, r.bufferRowLength,
                                    r.bufferImageHeight, r.imageSubresource, r.imageOffset, r.imageExtent);
        }
}

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBufferToImage2(VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info)
{
    count_call(HI_CmdCopyBufferToImage2);
    Device* dev = dev_of(cmd);
    track_image_copy2(dev, cmd, info);
    dev->d.CmdCopyBufferToImage2(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdCopyBufferToImage2KHR(VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info)
{
    count_call(HI_CmdCopyBufferToImage2KHR);
    Device* dev = dev_of(cmd);
    track_image_copy2(dev, cmd, info);
    dev->d.CmdCopyBufferToImage2KHR(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                                              const void* data)
{
    count_call(HI_CmdUpdateBuffer);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        on_update_buffer(dev, cb, dst, off, size, data);
    dev->d.CmdUpdateBuffer(cmd, dst, off, size, data);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdFillBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                                            uint32_t value)
{
    count_call(HI_CmdFillBuffer);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        on_fill_buffer(dev, cb, dst, off, size, value);
    dev->d.CmdFillBuffer(cmd, dst, off, size, value);
}

// ---------------------------------------------------------------------------
// Render passes

template <typename Info> void rp_begin(Device* dev, VkCommandBuffer cmd, bool dynamic, const Info* info)
{
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->in_rp = true;
        if (dynamic)
            cb->dyn_rp_count++;
        else
            cb->rp_count++;
        on_begin_targets(dev, cb, info);
        on_begin_pass(dev, cb, info);
    }
}

void rp_end(Device* dev, VkCommandBuffer cmd)
{
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        cb->in_rp = false;
        cb->active_targets = 0;
    }
}

// After the application's vkCmdEndRenderPass: the replacement may record work of its own that
// belongs to the pass that ended (D-113), then the instance is forgotten.
void rp_ended(Device* dev, VkCommandBuffer cmd)
{
    if (CmdBuf* cb = cb_of(dev, cmd)) {
        if (cb->pass.render_pass) m2_pass_end(dev, cb);
        cb->pass = CmdBuf::PassInstance{};
    }
}

VKAPI_ATTR void VKAPI_CALL hk_CmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* viewports)
{
    count_call(HI_CmdSetViewport);
    Device* dev = dev_of(cmd);
    if (first == 0 && count && viewports)
        if (CmdBuf* cb = cb_of(dev, cmd)) { cb->viewport = viewports[0]; cb->has_viewport = true; }
    dev->d.CmdSetViewport(cmd, first, count, viewports);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* scissors)
{
    count_call(HI_CmdSetScissor);
    Device* dev = dev_of(cmd);
    if (first == 0 && count && scissors)
        if (CmdBuf* cb = cb_of(dev, cmd)) { cb->scissor = scissors[0]; cb->has_scissor = true; }
    dev->d.CmdSetScissor(cmd, first, count, scissors);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info,
                                                 VkSubpassContents contents)
{
    count_call(HI_CmdBeginRenderPass);
    Device* dev = dev_of(cmd);
    rp_begin(dev, cmd, false, info);
    dev->d.CmdBeginRenderPass(cmd, info, contents);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info,
                                                  const VkSubpassBeginInfo* sub)
{
    count_call(HI_CmdBeginRenderPass2);
    Device* dev = dev_of(cmd);
    rp_begin(dev, cmd, false, info);
    dev->d.CmdBeginRenderPass2(cmd, info, sub);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBeginRenderPass2KHR(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info,
                                                     const VkSubpassBeginInfo* sub)
{
    count_call(HI_CmdBeginRenderPass2KHR);
    Device* dev = dev_of(cmd);
    rp_begin(dev, cmd, false, info);
    dev->d.CmdBeginRenderPass2KHR(cmd, info, sub);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdEndRenderPass(VkCommandBuffer cmd)
{
    count_call(HI_CmdEndRenderPass);
    Device* dev = dev_of(cmd);
    rp_end(dev, cmd);
    dev->d.CmdEndRenderPass(cmd);
    rp_ended(dev, cmd);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo* info)
{
    count_call(HI_CmdEndRenderPass2);
    Device* dev = dev_of(cmd);
    rp_end(dev, cmd);
    dev->d.CmdEndRenderPass2(cmd, info);
    rp_ended(dev, cmd);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdEndRenderPass2KHR(VkCommandBuffer cmd, const VkSubpassEndInfo* info)
{
    count_call(HI_CmdEndRenderPass2KHR);
    Device* dev = dev_of(cmd);
    rp_end(dev, cmd);
    dev->d.CmdEndRenderPass2KHR(cmd, info);
    rp_ended(dev, cmd);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info)
{
    count_call(HI_CmdBeginRendering);
    Device* dev = dev_of(cmd);
    rp_begin(dev, cmd, true, info);
    dev->d.CmdBeginRendering(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo* info)
{
    count_call(HI_CmdBeginRenderingKHR);
    Device* dev = dev_of(cmd);
    rp_begin(dev, cmd, true, info);
    dev->d.CmdBeginRenderingKHR(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdEndRendering(VkCommandBuffer cmd)
{
    count_call(HI_CmdEndRendering);
    Device* dev = dev_of(cmd);
    rp_end(dev, cmd);
    dev->d.CmdEndRendering(cmd);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdEndRenderingKHR(VkCommandBuffer cmd)
{
    count_call(HI_CmdEndRenderingKHR);
    Device* dev = dev_of(cmd);
    rp_end(dev, cmd);
    dev->d.CmdEndRenderingKHR(cmd);
}

VKAPI_ATTR void VKAPI_CALL hk_CmdNextSubpass(VkCommandBuffer cmd, VkSubpassContents contents)
{
    count_call(HI_CmdNextSubpass); Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) on_next_targets(dev, cb);
    dev->d.CmdNextSubpass(cmd, contents);
}
#define NEXT_SUBPASS2(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end) { \
        count_call(HI_##Name); Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) on_next_targets(dev, cb); \
        dev->d.Name(cmd, begin, end); \
    }
NEXT_SUBPASS2(CmdNextSubpass2)
NEXT_SUBPASS2(CmdNextSubpass2KHR)
#undef NEXT_SUBPASS2

VKAPI_ATTR void VKAPI_CALL hk_CmdExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* cbs)
{
    count_call(HI_CmdExecuteCommands);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd))
        on_execute_commands(dev, cb, count, cbs);
    dev->d.CmdExecuteCommands(cmd, count, cbs);
}

// ---------------------------------------------------------------------------
// Submission and frame boundary

// The semaphores a batch of the call signals: a later wait for one of them orders that batch after
// work of a queue (D-127). The set forgets everything when it grows past its cap (handles of
// destroyed semaphores); a wait it does not know keeps the application's stages.
constexpr size_t kQueueSignalledCap = 65536;
void note_queue_signals(Device* dev, uint32_t count, const VkSemaphore* semaphores)
{
    if (!count) return;
    std::lock_guard<std::mutex> lock(dev->queue_signal_mx);
    if (dev->queue_signalled.size() + count > kQueueSignalledCap) dev->queue_signalled.clear();
    dev->queue_signalled.insert(semaphores, semaphores + count);
}
// The stages of the layer's work at the front of a batch (the bone imports and copies, D-127).
constexpr VkPipelineStageFlags kPrefixStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkPipelineStageFlags2 kPrefixStages2 = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
bool chain_has(const void* next, VkStructureType type)
{
    for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext) if (s->sType == type) return true;
    return false;
}
bool protected_submit(const void* next)
{
    for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO && reinterpret_cast<const VkProtectedSubmitInfo*>(s)->protectedSubmit) return true;
    return false;
}
// The character backends' view of a call: per batch the recordings the layer tracks.
template <class Each>
SubmitWork& submit_work(Device* dev, uint32_t count, Each each)
{
    thread_local SubmitWork work;
    work.batches.resize(dev->character_mode ? count : 0); work.signals.clear(); work.owners.clear();
    for (uint32_t i = 0; i < work.batches.size(); i++) {
        auto& batch = work.batches[i]; batch.cbs.clear(); batch.prefix.clear();
        each(i, [&](VkCommandBuffer handle) { if (CmdBuf* cb = cb_of(dev, handle)) batch.cbs.push_back(cb); });
    }
    return work;
}

// The application's call with the layer's work in it (D-127): the command buffers a backend made for a
// batch go at its front, the batch's waits for semaphores a queue signalled cover their stages, and a
// batch of the layer's after the application's signals the backends' timelines. A batch whose command
// buffers carry device masks or protection gets the layer's work in a batch of its own right before it.
VkResult submit_with_work(Device* dev, VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence, const SubmitWork& work)
{
    thread_local std::vector<VkSubmitInfo> infos;
    thread_local std::vector<std::vector<VkCommandBuffer>> lists;
    thread_local std::vector<std::vector<VkPipelineStageFlags>> masks;
    thread_local std::vector<VkSemaphore> semaphores;
    thread_local std::vector<uint64_t> values;
    infos.clear();
    if (lists.size() < count) lists.resize(count);
    if (masks.size() < count) masks.resize(count);
    std::unique_lock<std::mutex> known(dev->queue_signal_mx);
    for (uint32_t i = 0; i < count; ++i) {
        VkSubmitInfo info = submits[i];
        const auto& prefix = work.batches[i].prefix;
        if (!prefix.empty()) {
            if (chain_has(info.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO) || protected_submit(info.pNext)) {
                VkSubmitInfo own{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                own.commandBufferCount = static_cast<uint32_t>(prefix.size()); own.pCommandBuffers = prefix.data();
                infos.push_back(own); ++dev->prefix_alone;
            } else {
                auto& list = lists[i];
                list.assign(prefix.begin(), prefix.end());
                list.insert(list.end(), info.pCommandBuffers, info.pCommandBuffers + info.commandBufferCount);
                info.commandBufferCount = static_cast<uint32_t>(list.size()); info.pCommandBuffers = list.data();
                if (info.waitSemaphoreCount) {
                    auto& mask = masks[i];
                    mask.assign(info.pWaitDstStageMask, info.pWaitDstStageMask + info.waitSemaphoreCount);
                    for (uint32_t k = 0; k < info.waitSemaphoreCount; ++k) {
                        if ((mask[k] & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) || (mask[k] & kPrefixStages) == kPrefixStages) continue;
                        if (!dev->queue_signalled.count(info.pWaitSemaphores[k])) continue;
                        mask[k] |= kPrefixStages; ++dev->widened_waits;
                    }
                    info.pWaitDstStageMask = mask.data();
                }
                ++dev->prefixed_batches;
            }
        }
        infos.push_back(info);
    }
    known.unlock();
    semaphores.clear(); values.clear();
    for (const auto& signal : work.signals) { semaphores.push_back(signal.semaphore); values.push_back(signal.value); }
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.signalSemaphoreValueCount = static_cast<uint32_t>(values.size()); timeline.pSignalSemaphoreValues = values.data();
    if (!semaphores.empty()) {
        VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signal.pNext = &timeline; signal.signalSemaphoreCount = static_cast<uint32_t>(semaphores.size()); signal.pSignalSemaphores = semaphores.data();
        infos.push_back(signal);
    }
    return dev->d.QueueSubmit(queue, static_cast<uint32_t>(infos.size()), infos.data(), fence);
}
VkResult submit2_with_work(Device* dev, VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, const SubmitWork& work,
                           PFN_vkQueueSubmit2 call)
{
    thread_local std::vector<VkSubmitInfo2> infos;
    thread_local std::vector<std::vector<VkCommandBufferSubmitInfo>> lists;
    thread_local std::vector<std::vector<VkSemaphoreSubmitInfo>> waits;
    thread_local std::vector<VkCommandBufferSubmitInfo> alone;
    thread_local std::vector<VkSemaphoreSubmitInfo> signals;
    infos.clear(); alone.clear();
    if (lists.size() < count) lists.resize(count);
    if (waits.size() < count) waits.resize(count);
    size_t alone_total = 0;
    for (uint32_t i = 0; i < count; ++i) alone_total += work.batches[i].prefix.size();
    alone.reserve(alone_total); // pointers into it stay valid
    std::unique_lock<std::mutex> known(dev->queue_signal_mx);
    for (uint32_t i = 0; i < count; ++i) {
        VkSubmitInfo2 info = submits[i];
        const auto& prefix = work.batches[i].prefix;
        if (!prefix.empty()) {
            if ((info.flags & VK_SUBMIT_PROTECTED_BIT) || chain_has(info.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)) {
                const size_t first = alone.size();
                for (VkCommandBuffer cb : prefix) alone.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cb, 0});
                VkSubmitInfo2 own{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                own.commandBufferInfoCount = static_cast<uint32_t>(prefix.size()); own.pCommandBufferInfos = alone.data() + first;
                infos.push_back(own); ++dev->prefix_alone;
            } else {
                auto& list = lists[i];
                list.clear();
                for (VkCommandBuffer cb : prefix) list.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cb, 0});
                list.insert(list.end(), info.pCommandBufferInfos, info.pCommandBufferInfos + info.commandBufferInfoCount);
                info.commandBufferInfoCount = static_cast<uint32_t>(list.size()); info.pCommandBufferInfos = list.data();
                if (info.waitSemaphoreInfoCount) {
                    auto& wait = waits[i];
                    wait.assign(info.pWaitSemaphoreInfos, info.pWaitSemaphoreInfos + info.waitSemaphoreInfoCount);
                    for (auto& w : wait) {
                        if ((w.stageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) || (w.stageMask & kPrefixStages2) == kPrefixStages2) continue;
                        if (!dev->queue_signalled.count(w.semaphore)) continue;
                        w.stageMask |= kPrefixStages2; ++dev->widened_waits;
                    }
                    info.pWaitSemaphoreInfos = wait.data();
                }
                ++dev->prefixed_batches;
            }
        }
        infos.push_back(info);
    }
    known.unlock();
    signals.clear();
    for (const auto& signal : work.signals)
        signals.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signal.semaphore, signal.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
    if (!signals.empty()) {
        VkSubmitInfo2 signal{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        signal.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()); signal.pSignalSemaphoreInfos = signals.data();
        infos.push_back(signal);
    }
    return call(queue, static_cast<uint32_t>(infos.size()), infos.data(), fence);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    count_call(HI_QueueSubmit);
    HookInside inside;
    Device* dev = dev_of(queue);
    SubmitWork& work = submit_work(dev, count, [&](uint32_t i, auto add) {
        for (uint32_t k = 0; k < submits[i].commandBufferCount; ++k) add(submits[i].pCommandBuffers[k]);
    });
    {
        StageMark stage(thread_trace(), "character submission work");
        const auto result = m2_submit(dev, queue, work);
        if (result != VK_SUCCESS) { m2_submitted(dev, queue, work, result); return result; }
    }
    {
        StageMark stage(thread_trace(), "submission tracking");
        for (uint32_t i = 0; i < count; i++) {
            m2_seal(dev, submits[i].commandBufferCount, submits[i].pCommandBuffers);
            on_submit(dev, submits[i].commandBufferCount, submits[i].pCommandBuffers);
            note_queue_signals(dev, submits[i].signalSemaphoreCount, submits[i].pSignalSemaphores);
        }
    }
    StageMark stage(thread_trace(), "driver vkQueueSubmit");
    const VkResult result = work.any_prefix() || !work.signals.empty() ? submit_with_work(dev, queue, count, submits, fence, work)
                                                                       : dev->d.QueueSubmit(queue, count, submits, fence);
    m2_submitted(dev, queue, work, result);
    return result;
}

// The command buffers and signals of a vkQueueSubmit2 call, for tracking.
void track_submit2(Device* dev, uint32_t count, const VkSubmitInfo2* submits)
{
    std::vector<VkCommandBuffer> list;
    std::vector<VkSemaphore> signalled;
    for (uint32_t i = 0; i < count; i++) {
        list.clear(); signalled.clear();
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; k++)
            list.push_back(submits[i].pCommandBufferInfos[k].commandBuffer);
        on_submit(dev, static_cast<uint32_t>(list.size()), list.data());
        m2_seal(dev, static_cast<uint32_t>(list.size()), list.data());
        for (uint32_t k = 0; k < submits[i].signalSemaphoreInfoCount; k++) signalled.push_back(submits[i].pSignalSemaphoreInfos[k].semaphore);
        note_queue_signals(dev, static_cast<uint32_t>(signalled.size()), signalled.data());
    }
}
VkResult queue_submit2(Device* dev, VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, PFN_vkQueueSubmit2 call)
{
    SubmitWork& work = submit_work(dev, count, [&](uint32_t i, auto add) {
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k) add(submits[i].pCommandBufferInfos[k].commandBuffer);
    });
    const auto result = m2_submit(dev, queue, work);
    if (result != VK_SUCCESS) { m2_submitted(dev, queue, work, result); return result; }
    track_submit2(dev, count, submits);
    const VkResult done = work.any_prefix() || !work.signals.empty() ? submit2_with_work(dev, queue, count, submits, fence, work, call)
                                                                     : call(queue, count, submits, fence);
    m2_submitted(dev, queue, work, done);
    return done;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
    count_call(HI_QueueSubmit2);
    HookInside inside;
    Device* dev = dev_of(queue);
    return queue_submit2(dev, queue, count, submits, fence, dev->d.QueueSubmit2);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
    count_call(HI_QueueSubmit2KHR);
    HookInside inside;
    Device* dev = dev_of(queue);
    return queue_submit2(dev, queue, count, submits, fence, dev->d.QueueSubmit2KHR);
}

// The layer's own waits for the GPU (Device::owned_gdpa): one made inside vkQueuePresentKHR is counted and
// logged with what the thread was doing, the first few times (BUG-062).
void note_owned_wait(Device* dev, const char* what)
{
    if (!dev || !in_presentation()) return;
    if (dev->present_waits.fetch_add(1, std::memory_order_relaxed) < 8) {
        ThreadTrace* const t = thread_trace();
        const char* const stage = t ? t->stage.load(std::memory_order_relaxed) : nullptr;
        logf("the layer waited for the GPU inside vkQueuePresentKHR (%s) at: %s", what, stage ? stage : "?");
    }
}
VKAPI_ATTR VkResult VKAPI_CALL owned_WaitForFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout)
{
    Device* dev = dev_of(device);
    note_owned_wait(dev, "vkWaitForFences");
    return dev->d.WaitForFences(device, count, fences, all, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL owned_WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout)
{
    Device* dev = dev_of(device);
    note_owned_wait(dev, "vkWaitSemaphores");
    return (dev->d.WaitSemaphores ? dev->d.WaitSemaphores : dev->d.WaitSemaphoresKHR)(device, info, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL owned_QueueWaitIdle(VkQueue queue)
{
    Device* dev = dev_of(queue);
    note_owned_wait(dev, "vkQueueWaitIdle");
    return dev->d.QueueWaitIdle(queue);
}
VKAPI_ATTR VkResult VKAPI_CALL owned_DeviceWaitIdle(VkDevice device)
{
    Device* dev = dev_of(device);
    note_owned_wait(dev, "vkDeviceWaitIdle");
    return dev->d.DeviceWaitIdle(device);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL owned_gdpa(VkDevice device, const char* name)
{
    Device* dev = dev_of(device);
    if (!dev || !dev->next_gdpa) return nullptr;
    if (!strcmp(name, "vkWaitForFences") && dev->d.WaitForFences) return reinterpret_cast<PFN_vkVoidFunction>(&owned_WaitForFences);
    if ((!strcmp(name, "vkWaitSemaphores") || !strcmp(name, "vkWaitSemaphoresKHR")) && (dev->d.WaitSemaphores || dev->d.WaitSemaphoresKHR))
        return reinterpret_cast<PFN_vkVoidFunction>(&owned_WaitSemaphores);
    if (!strcmp(name, "vkQueueWaitIdle") && dev->d.QueueWaitIdle) return reinterpret_cast<PFN_vkVoidFunction>(&owned_QueueWaitIdle);
    if (!strcmp(name, "vkDeviceWaitIdle") && dev->d.DeviceWaitIdle) return reinterpret_cast<PFN_vkVoidFunction>(&owned_DeviceWaitIdle);
    return dev->next_gdpa(device, name);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
    count_call(HI_QueuePresentKHR);
    struct Presented { HookInside inside; ~Presented() { note_presented(); } } presented;
    Device* dev = dev_of(queue);
    PresentWork present;
    present.queue = queue;
    present.info = info;
    if (info)
        present.waits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    {
        StageMark stage(thread_trace(), "presentation work (frame, character Present)");
        on_present(dev, &present);
    }
    {
        // Over the Mods' Present work, as 3DMigoto draws its notices last (D-137).
        StageMark stage(thread_trace(), "presentation notices");
        notices_present(dev, &present);
    }
    StageMark stage(thread_trace(), "driver vkQueuePresentKHR");
    if (!present.replaced || !info)
        return dev->d.QueuePresentKHR(queue, info);
    // Owned work was drawn into the image: the presentation waits for it, and it waited for the
    // application's semaphores itself.
    VkPresentInfoKHR changed = *info;
    changed.waitSemaphoreCount = static_cast<uint32_t>(present.waits.size());
    changed.pWaitSemaphores = present.waits.data();
    return dev->d.QueuePresentKHR(queue, &changed);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                     const VkAllocationCallbacks* alloc, VkSwapchainKHR* swapchain)
{
    count_call(HI_CreateSwapchainKHR);
    Device* dev = dev_of(device);
    // Owned Present work renders into the image; every swap chain supports color attachment use.
    VkSwapchainCreateInfoKHR changed;
    bool usage_added = false;
    if (info && dev->character_mode && !(info->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        changed = *info;
        changed.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info = &changed;
        usage_added = true;
    }
    const VkResult r = dev->d.CreateSwapchainKHR(device, info, alloc, swapchain);
    if (r != VK_SUCCESS || dev->passive || !info)
        return r;
    SwapchainInfo s;
    s.surface = info->surface;
    s.window = surface_window(info->surface);
    s.format = info->imageFormat;
    s.color_space = info->imageColorSpace;
    s.extent = info->imageExtent;
    s.usage = info->imageUsage;
    s.flags = info->flags;
    s.usage_added = usage_added;
    uint32_t count = 0;
    if (dev->d.GetSwapchainImagesKHR && dev->d.GetSwapchainImagesKHR(device, *swapchain, &count, nullptr) == VK_SUCCESS && count && count <= 16) {
        s.images.resize(count);
        if (dev->d.GetSwapchainImagesKHR(device, *swapchain, &count, s.images.data()) != VK_SUCCESS)
            s.images.clear();
        s.images.resize(s.images.empty() ? 0 : count);
    }
    // Swapchain images have no CreateImage call. Give their views the same stable
    // identities as ordinary attachments before the application creates views.
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D; image.format = info->imageFormat;
    image.extent = {info->imageExtent.width, info->imageExtent.height, 1};
    image.mipLevels = 1; image.arrayLayers = info->imageArrayLayers;
    image.samples = VK_SAMPLE_COUNT_1_BIT; image.usage = info->imageUsage;
    if (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) image.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    for (const auto handle : s.images) on_create_image(dev, handle, &image);
    logf("swapchain created: format=%d color_space=%d %ux%u usage=0x%x flags=0x%x images=%zu window=%s", int(s.format),
         int(s.color_space), s.extent.width, s.extent.height, unsigned(s.usage), unsigned(s.flags), s.images.size(),
         s.window ? "known" : "unknown");
    std::lock_guard<std::mutex> lock(dev->swap_mx);
    static uint32_t generation = 0;
    s.generation = ++generation;
    dev->swapchains[*swapchain] = std::move(s);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroySwapchainKHR);
    Device* dev = dev_of(device);
    if (!dev->passive) {
        std::vector<VkImage> images;
        {
            std::lock_guard<std::mutex> lock(dev->swap_mx);
            const auto found = dev->swapchains.find(swapchain);
            if (found != dev->swapchains.end()) { images = std::move(found->second.images); dev->swapchains.erase(found); }
        }
        notices_swapchain_gone(dev, images);
        for (const auto image : images) on_destroy_image(dev, image);
    }
    dev->d.DestroySwapchainKHR(device, swapchain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images)
{
    count_call(HI_GetSwapchainImagesKHR);
    return dev_of(device)->d.GetSwapchainImagesKHR(device, swapchain, count, images);
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyDevice);
    if (!device)
        return;
    Device* dev = dev_of(device);
    if (!dev)
        return;
    PFN_vkDestroyDevice next = dev->d.DestroyDevice;
    if (!dev->passive) {
        notices_device_gone(dev);
        m2_stop(dev);
        // The application idled the device before destroying it; retired semaphores may still sit in
        // batches of queues it did not idle (a test's second queue), so the device waits once more.
        if (const auto wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dev->next_gdpa(device, "vkDeviceWaitIdle"))) wait_idle(device);
        destroy_retired(dev, true);
        logf("device destroyed after %llu frames", static_cast<unsigned long long>(dev->frame_no));
        dev_destroy(dev);
    }
    next(device, alloc);
}

// ---------------------------------------------------------------------------
// Counted-only functions: one generic hook that counts and forwards

template <typename Fn, Fn DeviceDispatch::*Member, int Index> struct CountedHook;

template <typename R, typename First, typename... Args, R (VKAPI_PTR* DeviceDispatch::*Member)(First, Args...), int Index>
struct CountedHook<R (VKAPI_PTR*)(First, Args...), Member, Index> {
    static VKAPI_ATTR R VKAPI_CALL hook(First first, Args... args)
    {
        Device* dev = dev_of(first);
        count_call(Index);
        HookInside inside;
        return (dev->d.*Member)(first, args...);
    }
};

#define X(n) constexpr PFN_vk##n hk_##n = &CountedHook<PFN_vk##n, &DeviceDispatch::n, HI_##n>::hook;
EFVK_COUNTED_FUNCS(X)
#undef X

// ---------------------------------------------------------------------------
// Hook table

struct HookEntry {
    const char* name;
    PFN_vkVoidFunction hook;
    size_t next_offset;
    bool always; // also installed in passthrough mode
};

#define X(n) {"vk" #n, reinterpret_cast<PFN_vkVoidFunction>(hk_##n), offsetof(DeviceDispatch, n), false},
HookEntry g_hooks[] = {EFVK_DEVICE_FUNCS(X)};
#undef X

bool hook_is_always(const char* name)
{
    return !strcmp(name, "vkDestroyDevice") || !strcmp(name, "vkQueuePresentKHR");
}

const HookEntry* find_hook(const char* name)
{
    for (const HookEntry& h : g_hooks)
        if (!strcmp(h.name, name)) {
            if (g_cfg.mode == Mode::Passthrough && !hook_is_always(name))
                return nullptr;
            return &h;
        }
    return nullptr;
}

PFN_vkVoidFunction& next_slot(DeviceDispatch& d, const HookEntry& h)
{
    return *reinterpret_cast<PFN_vkVoidFunction*>(reinterpret_cast<char*>(&d) + h.next_offset);
}

// ---------------------------------------------------------------------------
// Instance and device creation

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_GetDeviceProcAddr(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_GetInstanceProcAddr(VkInstance instance, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                 VkInstance* instance)
{
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance next;
    if (injected()) {
        gipa = g_real_gipa;
        next = g_real_create_instance;
    } else {
        VkLayerInstanceCreateInfo* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
        while (link &&
               !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
            link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
        if (!link || !link->u.pLayerInfo)
            return VK_ERROR_INITIALIZATION_FAILED;
        gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        link->u.pLayerInfo = link->u.pLayerInfo->pNext;
        next = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    }
    if (!gipa || !next)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(info, alloc, instance);
    if (r != VK_SUCCESS)
        return r;

    pin_module();
    g_attach.instances_created.fetch_add(1);
    InstanceData* inst = new InstanceData();
    inst->handle = *instance;
    inst->gipa = gipa;
    inst->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*instance, "vkDestroyInstance"));
    inst->EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(*instance, "vkEnumerateDeviceExtensionProperties"));
    inst->GetPhysicalDeviceProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(*instance, "vkGetPhysicalDeviceProperties"));
    inst->GetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(*instance, "vkGetPhysicalDeviceMemoryProperties"));
    inst->GetPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(*instance, "vkGetPhysicalDeviceFeatures2"));
    inst->CreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gipa(*instance, "vkCreateWin32SurfaceKHR"));
    inst->DestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(*instance, "vkDestroySurfaceKHR"));
    inst->app_api_version = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    {
        std::lock_guard<std::mutex> lock(g_inst_mx);
        g_insts[key_of(*instance)] = inst;
    }
    const VkApplicationInfo* app = info->pApplicationInfo;
    logf("instance created: app=%s engine=%s api=0x%x", app && app->pApplicationName ? app->pApplicationName : "?",
         app && app->pEngineName ? app->pEngineName : "?", app ? app->apiVersion : 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc)
{
    if (!instance)
        return;
    InstanceData* inst;
    {
        std::lock_guard<std::mutex> lock(g_inst_mx);
        auto it = g_insts.find(key_of(instance));
        if (it == g_insts.end())
            return;
        inst = it->second;
        g_insts.erase(it);
    }
    PFN_vkDestroyInstance next = inst->DestroyInstance;
    delete inst;
    next(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
                                                         const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface)
{
    PFN_vkCreateWin32SurfaceKHR next = nullptr;
    if (injected())
        next = g_real_create_surface;
    else if (InstanceData* inst = inst_of_key(key_of(instance)))
        next = inst->CreateWin32SurfaceKHR;
    if (!next)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = next(instance, info, alloc, surface);
    if (r == VK_SUCCESS && info) {
        std::lock_guard<std::mutex> lock(g_surface_mx);
        g_surfaces[*surface] = info->hwnd;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* alloc)
{
    PFN_vkDestroySurfaceKHR next = nullptr;
    if (injected())
        next = g_real_destroy_surface;
    else if (InstanceData* inst = inst_of_key(key_of(instance)))
        next = inst->DestroySurfaceKHR;
    {
        std::lock_guard<std::mutex> lock(g_surface_mx);
        g_surfaces.erase(surface);
    }
    if (next)
        next(instance, surface, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_EnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char* layer_name,
                                                                     uint32_t* count, VkExtensionProperties* props)
{
    if (layer_name && !strcmp(layer_name, EFVK_LAYER_NAME)) {
        if (count)
            *count = 0;
        return VK_SUCCESS;
    }
    InstanceData* inst = inst_of_key(key_of(phys));
    if (!inst || !inst->EnumerateDeviceExtensionProperties)
        return VK_ERROR_INITIALIZATION_FAILED;
    return inst->EnumerateDeviceExtensionProperties(phys, layer_name, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkDevice* device)
{
    InstanceData* inst = inst_of_key(key_of(phys));
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto link = reinterpret_cast<const VkLayerDeviceCreateInfo*>(p);
            if (link->function == VK_LOADER_DATA_CALLBACK) set_loader_data = link->u.pfnSetDeviceLoaderData;
        }
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDevice next;
    if (injected()) {
        gdpa = g_real_gdpa;
        next = g_real_create_device;
    } else {
        VkLayerDeviceCreateInfo* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
        while (link &&
               !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
            link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
        if (!link || !link->u.pLayerInfo)
            return VK_ERROR_INITIALIZATION_FAILED;
        PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        link->u.pLayerInfo = link->u.pLayerInfo->pNext;
        next = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->handle : VK_NULL_HANDLE, "vkCreateDevice"));
    }
    if (!gdpa || !next)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Character mode: owned GPU work (D-072) needs VK_EXT_robustness2 nullDescriptor for D3D11
    // null views (D-071) and shaderDrawParameters for D3D vertex IDs in compiled shaders. The layer
    // enables what the device supports and the application did not: the extension with that one
    // feature, the core 1.1 feature in the application's own feature structure for the duration
    // of the call, or in a structure of its own. Application requests stay as they are.
    const VkDeviceCreateInfo* requested = info;
    VkDeviceCreateInfo modified = *info;
    // D-103: a queue of the layer's own for uploads; these live until the call below returns.
    std::vector<VkDeviceQueueCreateInfo> queues(info->pQueueCreateInfos, info->pQueueCreateInfos + info->queueCreateInfoCount);
    std::vector<float> upload_priority{1.0f}, bumped_priorities;
    uint32_t upload_family = UINT32_MAX, upload_index = 0;
    VkQueueFlags upload_flags = 0;
    std::vector<const char*> extensions(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    bool null_descriptors = false, null_descriptors_added = false, robustness_requested = false;
    bool draw_parameters = false, draw_parameters_added = false;
    // D-095: timeline semaphores publish the characters' textures to every graphics queue; memory
    // priority and pageable device-local memory let the driver page the layer's textures before the
    // application's. Each is enabled when the device supports it and the application did not.
    bool timeline = false, timeline_added = false, timeline_requested = false;
    bool priority = false, priority_added = false, priority_requested = false;
    bool pageable = false, pageable_added = false, pageable_requested = false;
    VkBool32* app_draw_parameters = nullptr; // the application's field, set only for the call
    VkBool32* app_timeline = nullptr;
    VkBool32* app_priority = nullptr;
    VkBool32* app_pageable = nullptr;
    bool changed = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        robustness_requested |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        timeline_requested |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        priority_requested |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        pageable_requested |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
    }
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = const_cast<VkPhysicalDeviceVulkan12Features*>(reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p));
            timeline = features->timelineSemaphore != VK_FALSE; app_timeline = &features->timelineSemaphore;
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features = const_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(p));
            timeline = features->timelineSemaphore != VK_FALSE; app_timeline = &features->timelineSemaphore;
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT) {
            auto* features = const_cast<VkPhysicalDeviceMemoryPriorityFeaturesEXT*>(reinterpret_cast<const VkPhysicalDeviceMemoryPriorityFeaturesEXT*>(p));
            priority = priority_requested && features->memoryPriority != VK_FALSE; app_priority = &features->memoryPriority;
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT) {
            auto* features = const_cast<VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT*>(reinterpret_cast<const VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT*>(p));
            pageable = pageable_requested && features->pageableDeviceLocalMemory != VK_FALSE; app_pageable = &features->pageableDeviceLocalMemory;
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT && robustness_requested)
            null_descriptors = reinterpret_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(p)->nullDescriptor != VK_FALSE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            auto* features = const_cast<VkPhysicalDeviceVulkan11Features*>(reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(p));
            draw_parameters = features->shaderDrawParameters != VK_FALSE; app_draw_parameters = &features->shaderDrawParameters;
        }
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            auto* features = const_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(reinterpret_cast<const VkPhysicalDeviceShaderDrawParametersFeatures*>(p));
            draw_parameters = features->shaderDrawParameters != VK_FALSE; app_draw_parameters = &features->shaderDrawParameters;
        }
    }
    const uint32_t app_api = inst ? inst->app_api_version : VK_API_VERSION_1_0;
    if (character_configured() && inst && inst->EnumerateDeviceExtensionProperties && inst->GetPhysicalDeviceFeatures2) {
        bool robustness_supported = false, timeline_supported = false, priority_supported = false, pageable_supported = false;
        uint32_t count = 0;
        if (inst->EnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr) == VK_SUCCESS && count && count < 4096) {
            std::vector<VkExtensionProperties> available(count);
            if (inst->EnumerateDeviceExtensionProperties(phys, nullptr, &count, available.data()) == VK_SUCCESS)
                for (uint32_t i = 0; i < count; ++i) {
                    robustness_supported |= !strcmp(available[i].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                    timeline_supported |= !strcmp(available[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
                    priority_supported |= !strcmp(available[i].extensionName, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
                    pageable_supported |= !strcmp(available[i].extensionName, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
                }
        }
        robustness_supported = robustness_supported && !robustness_requested;
        // Timeline semaphores are core since 1.2; a 1.1 application gets the extension.
        const bool timeline_available = app_api >= VK_API_VERSION_1_2 || timeline_supported || timeline_requested;
        VkPhysicalDeviceRobustness2FeaturesEXT robust_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceShaderDrawParametersFeatures draw_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
        VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
        VkBaseOutStructure* query_chain = nullptr;
        auto query = [&](void* node) { auto* s = static_cast<VkBaseOutStructure*>(node); s->pNext = query_chain; query_chain = s; };
        query(&draw_query);
        if (robustness_supported) query(&robust_query);
        if (timeline_available) query(&timeline_query);
        if (priority_supported) query(&priority_query);
        if (pageable_supported) query(&pageable_query);
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = query_chain;
        inst->GetPhysicalDeviceFeatures2(phys, &features);
        void* chain = const_cast<void*>(info->pNext);
        if (robustness_supported && robust_query.nullDescriptor) {
            extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            robust.nullDescriptor = VK_TRUE; robust.pNext = chain; chain = &robust;
            null_descriptors = null_descriptors_added = changed = true;
        }
        if (!draw_parameters && app_api >= VK_API_VERSION_1_1 && draw_query.shaderDrawParameters) {
            if (app_draw_parameters) *app_draw_parameters = VK_TRUE;
            else { draw_parameters_feature.shaderDrawParameters = VK_TRUE; draw_parameters_feature.pNext = chain; chain = &draw_parameters_feature; }
            draw_parameters = draw_parameters_added = changed = true;
        }
        if (!timeline && timeline_available && timeline_query.timelineSemaphore) {
            if (app_timeline) *app_timeline = VK_TRUE;
            else { timeline_feature.timelineSemaphore = VK_TRUE; timeline_feature.pNext = chain; chain = &timeline_feature; }
            if (app_api < VK_API_VERSION_1_2 && !timeline_requested) extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            timeline = timeline_added = changed = true;
        }
        if (g_cfg.m2_pageable && !priority && priority_supported && priority_query.memoryPriority) {
            if (!priority_requested) extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
            if (app_priority) *app_priority = VK_TRUE;
            else { priority_feature.memoryPriority = VK_TRUE; priority_feature.pNext = chain; chain = &priority_feature; }
            priority = priority_added = changed = true;
        }
        if (g_cfg.m2_pageable && !pageable && priority && pageable_supported && pageable_query.pageableDeviceLocalMemory) {
            if (!pageable_requested) extensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
            if (app_pageable) *app_pageable = VK_TRUE;
            else { pageable_feature.pageableDeviceLocalMemory = VK_TRUE; pageable_feature.pNext = chain; chain = &pageable_feature; }
            pageable = pageable_added = changed = true;
        }
        // D-103: one more queue for the layer's uploads, from a transfer-capable family with a queue
        // the application did not request: a dedicated transfer family first, then compute, then
        // graphics. Without one, uploads go through the application's submissions as before.
        if (const auto family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                inst->gipa(inst->handle, "vkGetPhysicalDeviceQueueFamilyProperties"))) {
            uint32_t family_count = 0;
            family_properties(phys, &family_count, nullptr);
            if (family_count && family_count < 64) {
                std::vector<VkQueueFamilyProperties> families(family_count);
                family_properties(phys, &family_count, families.data());
                int best = -1; uint32_t best_family = 0; size_t best_request = SIZE_MAX;
                for (uint32_t f = 0; f < family_count; ++f) {
                    const VkQueueFlags flags = families[f].queueFlags;
                    if (!(flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) continue;
                    uint32_t taken = 0; size_t request = SIZE_MAX; bool special = false;
                    for (size_t i = 0; i < queues.size(); ++i)
                        if (queues[i].queueFamilyIndex == f) { taken += queues[i].queueCount; request = i; special |= queues[i].flags != 0; }
                    if (special || taken >= families[f].queueCount) continue;
                    const int score = (flags & VK_QUEUE_GRAPHICS_BIT) ? 1 : (flags & VK_QUEUE_COMPUTE_BIT) ? 2 : 3;
                    if (score > best) { best = score; best_family = f; best_request = request; }
                }
                if (best >= 0) {
                    if (best_request == SIZE_MAX) {
                        VkDeviceQueueCreateInfo extra{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                        extra.queueFamilyIndex = best_family; extra.queueCount = 1; extra.pQueuePriorities = upload_priority.data();
                        queues.push_back(extra); upload_index = 0;
                    } else {
                        auto& q = queues[best_request];
                        bumped_priorities.assign(q.pQueuePriorities, q.pQueuePriorities + q.queueCount); bumped_priorities.push_back(1.0f);
                        upload_index = q.queueCount; q.queueCount += 1; q.pQueuePriorities = bumped_priorities.data();
                    }
                    upload_family = best_family; upload_flags = families[best_family].queueFlags;
                    modified.queueCreateInfoCount = static_cast<uint32_t>(queues.size()); modified.pQueueCreateInfos = queues.data();
                    changed = true;
                }
            }
        }
        if (changed) {
            modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            modified.ppEnabledExtensionNames = extensions.data();
            modified.pNext = chain;
            info = &modified;
        }
    }
    VkResult r = next(phys, info, alloc, device);
    if (draw_parameters_added && app_draw_parameters) *app_draw_parameters = VK_FALSE;
    if (timeline_added && app_timeline) *app_timeline = VK_FALSE;
    if (priority_added && app_priority) *app_priority = VK_FALSE;
    if (pageable_added && app_pageable) *app_pageable = VK_FALSE;
    if (r != VK_SUCCESS)
        return r;
    info = requested;

    g_attach.devices_created.fetch_add(1);
    Device* dev = dev_create(*device);
    dev->phys = phys;
    dev->next_gdpa = gdpa;
    dev->owned_gdpa = owned_gdpa;
    dev->set_loader_data = set_loader_data;
    dev->null_descriptors = null_descriptors;
    dev->null_descriptors_added = null_descriptors_added;
    dev->draw_parameters = draw_parameters;
    dev->draw_parameters_added = draw_parameters_added;
    dev->timeline_semaphores = timeline; dev->timeline_semaphores_added = timeline_added;
    dev->memory_priority = priority; dev->memory_priority_added = priority_added;
    dev->pageable_memory = pageable; dev->pageable_memory_added = pageable_added;
    if (upload_family != UINT32_MAX) {
        if (const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*device, "vkGetDeviceQueue"))) {
            get_queue(*device, upload_family, upload_index, &dev->upload_queue);
            // A dispatchable object the loader did not hand out gets its dispatch here.
            if (dev->upload_queue && set_loader_data) set_loader_data(*device, dev->upload_queue);
        }
        if (dev->upload_queue) { dev->upload_family = upload_family; dev->upload_flags = upload_flags; }
    }
    if (dev->upload_queue) logf("upload queue: family %u index %u, flags 0x%x (D-103)", upload_family, upload_index, upload_flags);
    else logf("upload queue: none, uploads go through the application's submissions");
    if (inst)
        dev->app_api_version = inst->app_api_version;
    for (const HookEntry& h : g_hooks)
        next_slot(dev->d, h) = gdpa(*device, h.name);
    if (inst && inst->GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props;
        inst->GetPhysicalDeviceProperties(phys, &props);
        memcpy(dev->gpu_name, props.deviceName, sizeof(dev->gpu_name));
        dev->api_version = props.apiVersion;
        dev->vendor_id = props.vendorID;
        dev->device_id = props.deviceID;
        dev->driver_version = props.driverVersion;
        dev->max_bound_descriptor_sets = props.limits.maxBoundDescriptorSets;
        if (inst->GetPhysicalDeviceMemoryProperties) {
            VkPhysicalDeviceMemoryProperties memory;
            inst->GetPhysicalDeviceMemoryProperties(phys, &memory);
            dev->memory_properties = memory;
            if (character_configured()) {
                character_device_setup(dev, info, inst->handle, inst->gipa);
                m2_create(dev, props, memory);
            }
        }
    }
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        dev->extensions.push_back(info->ppEnabledExtensionNames[i]);
    logf("device created on %s, %u extensions enabled, application API 0x%x, null descriptors %s", dev->gpu_name,
         info->enabledExtensionCount, dev->app_api_version,
         null_descriptors_added ? "added" : null_descriptors ? "enabled by the application" : "off");
    if (dev->draw_parameters) logf("shader draw parameters %s", dev->draw_parameters_added ? "added" : "enabled by the application");
    logf("timeline semaphores %s, memory priority %s, pageable device memory %s",
         dev->timeline_semaphores ? (dev->timeline_semaphores_added ? "added" : "enabled by the application") : "off",
         dev->memory_priority ? (dev->memory_priority_added ? "added" : "enabled by the application") : "off",
         dev->pageable_memory ? (dev->pageable_memory_added ? "added" : "enabled by the application") : "off");
    if (g_cfg.mode == Mode::Probe)
        ipc_start();
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_GetDeviceProcAddr(VkDevice device, const char* name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_GetDeviceProcAddr);
    Device* dev = device ? dev_of(device) : nullptr;
    if (!dev || dev->passive)
        return injected() && g_real_gdpa ? g_real_gdpa(device, name) : nullptr;
    if (g_cfg.mode == Mode::Probe)
        note_resolved(name);
    if (const HookEntry* h = find_hook(name)) {
        // Offer a hook only for what the next element of the chain really implements.
        if (next_slot(dev->d, *h))
            return h->hook;
    }
    return dev->next_gdpa(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_GetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_GetInstanceProcAddr);
    if (!strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_CreateInstance);
    if (!strcmp(name, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_DestroyInstance);
    if (!strcmp(name, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_CreateDevice);
    if (!strcmp(name, "vkCreateWin32SurfaceKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_CreateWin32SurfaceKHR);
    if (!strcmp(name, "vkDestroySurfaceKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_DestroySurfaceKHR);
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties") && !injected())
        return reinterpret_cast<PFN_vkVoidFunction>(hk_EnumerateDeviceExtensionProperties);
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(hk_GetDeviceProcAddr);
    if (const HookEntry* h = find_hook(name))
        return h->hook;
    if (injected())
        return g_real_gipa ? g_real_gipa(instance, name) : nullptr;
    if (!instance)
        return nullptr;
    InstanceData* inst = inst_of_key(key_of(instance));
    return inst ? inst->gipa(instance, name) : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Inject entry: which exports of vulkan-1.dll lead to which hook

namespace efvk {

HWND surface_window(VkSurfaceKHR surface)
{
    std::lock_guard<std::mutex> lock(g_surface_mx);
    const auto found = g_surfaces.find(surface);
    return found == g_surfaces.end() ? nullptr : found->second;
}

void collect_export_hooks(std::vector<ExportHook>& out)
{
    out.push_back({"vkGetInstanceProcAddr", reinterpret_cast<void*>(hk_GetInstanceProcAddr),
                   reinterpret_cast<void**>(&g_real_gipa)});
    out.push_back({"vkGetDeviceProcAddr", reinterpret_cast<void*>(hk_GetDeviceProcAddr),
                   reinterpret_cast<void**>(&g_real_gdpa)});
    out.push_back({"vkCreateInstance", reinterpret_cast<void*>(hk_CreateInstance),
                   reinterpret_cast<void**>(&g_real_create_instance)});
    out.push_back({"vkDestroyInstance", reinterpret_cast<void*>(hk_DestroyInstance),
                   reinterpret_cast<void**>(&g_real_destroy_instance)});
    out.push_back({"vkCreateDevice", reinterpret_cast<void*>(hk_CreateDevice),
                   reinterpret_cast<void**>(&g_real_create_device)});
    out.push_back({"vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(hk_CreateWin32SurfaceKHR),
                   reinterpret_cast<void**>(&g_real_create_surface)});
    out.push_back({"vkDestroySurfaceKHR", reinterpret_cast<void*>(hk_DestroySurfaceKHR),
                   reinterpret_cast<void**>(&g_real_destroy_surface)});
    // A device function may be called through its export for a device that was created before
    // the redirection. The original address goes into the dispatch table of the passive device.
    for (const HookEntry& h : g_hooks) {
        if (g_cfg.mode == Mode::Passthrough && !hook_is_always(h.name))
            continue;
        out.push_back({h.name, reinterpret_cast<void*>(h.hook), reinterpret_cast<void**>(&next_slot(g_passive_dev.d, h))});
    }
}

// Called when every original address is known and before the first export is redirected.
void exports_resolved()
{
    dev_set_passive(&g_passive_dev);
}

} // namespace efvk

// ---------------------------------------------------------------------------
// Exports

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL efvkGetInstanceProcAddr(VkInstance instance, const char* name)
{
    return hk_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL efvkGetDeviceProcAddr(VkDevice device, const char* name)
{
    return hk_GetDeviceProcAddr(device, name);
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v)
{
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (injected()) {
        // Hooking twice would record everything twice; the export redirection wins.
        logf("the loader offered to use this DLL as a layer, refused because the entry is inject");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (v->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        v->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    v->pfnGetInstanceProcAddr = hk_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = hk_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        util_init(self);
        if (g_cfg.entry == Entry::Inject) {
            pin_module();
            bootstrap_start();
            if (g_cfg.mode == Mode::Probe)
                ipc_start();
        }
    }
    return TRUE;
}

} // extern "C"
