// Included inside layer.cpp's hook namespace. Calls are forwarded unchanged.

#define RESOURCE_HOOKS(Name, Handle, Info, create_fn, destroy_fn) \
    VKAPI_ATTR VkResult VKAPI_CALL hk_Create##Name(VkDevice device, const Info* info, \
                                                   const VkAllocationCallbacks* alloc, Handle* handle) { \
        count_call(HI_Create##Name); \
        Device* dev = dev_of(device); \
        VkResult result = dev->d.Create##Name(device, info, alloc, handle); \
        if (result == VK_SUCCESS) create_fn(dev, *handle, info); \
        return result; \
    } \
    VKAPI_ATTR void VKAPI_CALL hk_Destroy##Name(VkDevice device, Handle handle, const VkAllocationCallbacks* alloc) { \
        count_call(HI_Destroy##Name); \
        Device* dev = dev_of(device); \
        if (handle) destroy_fn(dev, handle); \
        dev->d.Destroy##Name(device, handle, alloc); \
    }
RESOURCE_HOOKS(ShaderModule, VkShaderModule, VkShaderModuleCreateInfo, on_create_shader, on_destroy_shader)
RESOURCE_HOOKS(Image, VkImage, VkImageCreateInfo, on_create_image, on_destroy_image)
RESOURCE_HOOKS(ImageView, VkImageView, VkImageViewCreateInfo, on_create_image_view, on_destroy_image_view)
RESOURCE_HOOKS(BufferView, VkBufferView, VkBufferViewCreateInfo, on_create_buffer_view, on_destroy_buffer_view)
RESOURCE_HOOKS(PipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo, on_create_pipeline_layout, on_destroy_pipeline_layout)
RESOURCE_HOOKS(RenderPass, VkRenderPass, VkRenderPassCreateInfo, on_create_render_pass, on_destroy_render_pass)
RESOURCE_HOOKS(Framebuffer, VkFramebuffer, VkFramebufferCreateInfo, on_create_framebuffer, on_destroy_framebuffer)
RESOURCE_HOOKS(DescriptorUpdateTemplate, VkDescriptorUpdateTemplate, VkDescriptorUpdateTemplateCreateInfo,
               on_create_descriptor_template, on_destroy_descriptor_template)
RESOURCE_HOOKS(DescriptorUpdateTemplateKHR, VkDescriptorUpdateTemplate, VkDescriptorUpdateTemplateCreateInfo,
               on_create_descriptor_template, on_destroy_descriptor_template)
#undef RESOURCE_HOOKS

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkSampler* sampler)
{
    count_call(HI_CreateSampler);
    Device* dev = dev_of(device);
    const auto result = dev->d.CreateSampler(device, info, alloc, sampler);
    if (result == VK_SUCCESS) on_create_sampler(dev, *sampler, info, alloc);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hk_DestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroySampler);
    Device* dev = dev_of(device);
    if (!sampler || !on_destroy_sampler(dev, sampler)) dev->d.DestroySampler(device, sampler, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                                                           const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* layout)
{
    count_call(HI_CreateDescriptorSetLayout);
    Device* dev = dev_of(device);
    const auto result = dev->d.CreateDescriptorSetLayout(device, info, alloc, layout);
    if (result == VK_SUCCESS) on_create_set_layout(dev, *layout, info, alloc);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hk_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyDescriptorSetLayout);
    Device* dev = dev_of(device);
    if (!layout || !on_destroy_set_layout(dev, layout)) dev->d.DestroyDescriptorSetLayout(device, layout, alloc);
}

#define RENDER_PASS2_HOOK(Name) \
    VKAPI_ATTR VkResult VKAPI_CALL hk_##Name(VkDevice device, const VkRenderPassCreateInfo2* info, \
                                            const VkAllocationCallbacks* alloc, VkRenderPass* pass) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(device); \
        VkResult result = dev->d.Name(device, info, alloc, pass); \
        if (result == VK_SUCCESS) on_create_render_pass2(dev, *pass, info); \
        return result; \
    }
RENDER_PASS2_HOOK(CreateRenderPass2)
RENDER_PASS2_HOOK(CreateRenderPass2KHR)
#undef RENDER_PASS2_HOOK

VKAPI_ATTR VkResult VKAPI_CALL hk_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info,
                                                       VkDescriptorSet* sets)
{
    count_call(HI_AllocateDescriptorSets);
    Device* dev = dev_of(device);
    VkResult r = dev->d.AllocateDescriptorSets(device, info, sets);
    if (r == VK_SUCCESS) on_allocate_sets(dev, info, sets);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count,
                                                   const VkDescriptorSet* sets)
{
    count_call(HI_FreeDescriptorSets);
    Device* dev = dev_of(device);
    VkResult r = dev->d.FreeDescriptorSets(device, pool, count, sets);
    if (r == VK_SUCCESS) on_free_sets(dev, count, sets);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* alloc)
{
    count_call(HI_DestroyDescriptorPool);
    Device* dev = dev_of(device);
    on_clear_descriptor_pool(dev, pool);
    dev->d.DestroyDescriptorPool(device, pool, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
{
    count_call(HI_ResetDescriptorPool);
    Device* dev = dev_of(device);
    VkResult r = dev->d.ResetDescriptorPool(device, pool, flags);
    if (r == VK_SUCCESS) on_clear_descriptor_pool(dev, pool);
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_UpdateDescriptorSets(VkDevice device, uint32_t write_count, const VkWriteDescriptorSet* writes,
                                                  uint32_t copy_count, const VkCopyDescriptorSet* copies)
{
    count_call(HI_UpdateDescriptorSets);
    Device* dev = dev_of(device);
    on_update_sets(dev, write_count, writes, copy_count, copies);
    dev->d.UpdateDescriptorSets(device, write_count, writes, copy_count, copies);
}

#define UPDATE_TEMPLATE_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate update, const void* data) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(device); \
        on_update_template(dev, set, update, data); \
        dev->d.Name(device, set, update, data); \
    }
UPDATE_TEMPLATE_HOOK(UpdateDescriptorSetWithTemplate)
UPDATE_TEMPLATE_HOOK(UpdateDescriptorSetWithTemplateKHR)
#undef UPDATE_TEMPLATE_HOOK

#define BIND_SETS2_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, const VkBindDescriptorSetsInfo* info) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) { \
            if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) \
                on_bind_sets(dev, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, \
                             info->descriptorSetCount, info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets); \
            if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) \
                on_bind_sets(dev, cb, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, \
                             info->descriptorSetCount, info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets); \
        } \
        dev->d.Name(cmd, info); \
    }
BIND_SETS2_HOOK(CmdBindDescriptorSets2)
BIND_SETS2_HOOK(CmdBindDescriptorSets2KHR)
#undef BIND_SETS2_HOOK

#define PUSH_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, \
                                        uint32_t set, uint32_t count, const VkWriteDescriptorSet* writes) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) on_push_descriptors(dev, cb, point, layout, set, count, writes); \
        dev->d.Name(cmd, point, layout, set, count, writes); \
    }
PUSH_HOOK(CmdPushDescriptorSet)
PUSH_HOOK(CmdPushDescriptorSetKHR)
#undef PUSH_HOOK

#define PUSH_TEMPLATE_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, VkDescriptorUpdateTemplate update, \
                                        VkPipelineLayout layout, uint32_t set, const void* data) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) on_push_template(dev, cb, update, layout, set, data); \
        dev->d.Name(cmd, update, layout, set, data); \
    }
PUSH_TEMPLATE_HOOK(CmdPushDescriptorSetWithTemplate)
PUSH_TEMPLATE_HOOK(CmdPushDescriptorSetWithTemplateKHR)
#undef PUSH_TEMPLATE_HOOK

#define PUSH2_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, const VkPushDescriptorSetInfo* info) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) { \
            if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) \
                on_push_descriptors(dev, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set, \
                                     info->descriptorWriteCount, info->pDescriptorWrites); \
            if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) \
                on_push_descriptors(dev, cb, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set, \
                                     info->descriptorWriteCount, info->pDescriptorWrites); \
        } \
        dev->d.Name(cmd, info); \
    }
PUSH2_HOOK(CmdPushDescriptorSet2)
PUSH2_HOOK(CmdPushDescriptorSet2KHR)
#undef PUSH2_HOOK

#define PUSH_TEMPLATE2_HOOK(Name) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, const VkPushDescriptorSetWithTemplateInfo* info) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) \
            on_push_template(dev, cb, info->descriptorUpdateTemplate, info->layout, info->set, info->pData); \
        dev->d.Name(cmd, info); \
    }
PUSH_TEMPLATE2_HOOK(CmdPushDescriptorSetWithTemplate2)
PUSH_TEMPLATE2_HOOK(CmdPushDescriptorSetWithTemplate2KHR)
#undef PUSH_TEMPLATE2_HOOK

VKAPI_ATTR void VKAPI_CALL hk_CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer cmd, VkPipelineBindPoint point,
    VkPipelineLayout layout, uint32_t first, uint32_t count, const uint32_t* indices, const VkDeviceSize* offsets)
{
    count_call(HI_CmdSetDescriptorBufferOffsetsEXT);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) on_unsupported_descriptor_bind(cb, point, "descriptor_buffers");
    dev->d.CmdSetDescriptorBufferOffsetsEXT(cmd, point, layout, first, count, indices, offsets);
}

// Descriptor buffers are still unsupported. Do not leave stale descriptors visible.
#define UNSUPPORTED_INFO_HOOK(Name, Info, reason) \
    VKAPI_ATTR void VKAPI_CALL hk_##Name(VkCommandBuffer cmd, const Info* info) { \
        count_call(HI_##Name); \
        Device* dev = dev_of(cmd); \
        if (CmdBuf* cb = cb_of(dev, cmd)) { \
            on_unsupported_descriptor_bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, reason); \
            on_unsupported_descriptor_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, reason); \
        } \
        dev->d.Name(cmd, info); \
    }
UNSUPPORTED_INFO_HOOK(CmdSetDescriptorBufferOffsets2EXT, VkSetDescriptorBufferOffsetsInfoEXT, "descriptor_buffers")
UNSUPPORTED_INFO_HOOK(CmdBindDescriptorBufferEmbeddedSamplers2EXT, VkBindDescriptorBufferEmbeddedSamplersInfoEXT, "descriptor_buffers")
#undef UNSUPPORTED_INFO_HOOK

VKAPI_ATTR void VKAPI_CALL hk_CmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer cmd,
    VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set)
{
    count_call(HI_CmdBindDescriptorBufferEmbeddedSamplersEXT);
    Device* dev = dev_of(cmd);
    if (CmdBuf* cb = cb_of(dev, cmd)) on_unsupported_descriptor_bind(cb, point, "descriptor_buffers");
    dev->d.CmdBindDescriptorBufferEmbeddedSamplersEXT(cmd, point, layout, set);
}
