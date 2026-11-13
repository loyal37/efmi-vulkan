#pragma once
#include "graphics_commands.h"
#include "shader_usage.h"
#include <spirv-tools/libspirv.h>
#include <windows.h>
#include <mutex>
#include <stdexcept>

namespace efmi::gpu {
inline void shader_require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline void shader_checked(VkResult r,const char* why){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(why)+": "+std::to_string(r));}
struct ShaderDevice {
    ResourceDevice resource;
    ShaderHost host;
    std::shared_ptr<const uint8_t> domain;
    HMODULE validator=nullptr;
#define VALIDATOR_FUNCS(X) X(spvContextCreate) X(spvContextDestroy) X(spvValidatorOptionsCreate) X(spvValidatorOptionsDestroy) \
    X(spvValidatorOptionsSetScalarBlockLayout) X(spvValidatorOptionsSetUniformBufferStandardLayout) X(spvValidateWithOptions) X(spvDiagnosticDestroy)
#define X(name) decltype(&::name) name=nullptr;
    VALIDATOR_FUNCS(X)
#undef X
    ~ShaderDevice(){if(validator)FreeLibrary(validator);}
    void validate(const HlslProgram& program){
        struct Validation {
            ShaderDevice& d;spv_context context=nullptr;spv_validator_options options=nullptr;spv_diagnostic diagnostic=nullptr;
            ~Validation(){if(diagnostic)d.spvDiagnosticDestroy(diagnostic);if(options)d.spvValidatorOptionsDestroy(options);if(context)d.spvContextDestroy(context);}
        } v{*this};
        v.context=spvContextCreate(program.vulkan_version>=VK_API_VERSION_1_3?SPV_ENV_VULKAN_1_3:
                                  program.vulkan_version>=VK_API_VERSION_1_2?SPV_ENV_VULKAN_1_2:SPV_ENV_VULKAN_1_1);
        v.options=spvValidatorOptionsCreate();shader_require(v.context && v.options,"Cannot create shader validator");
        spvValidatorOptionsSetScalarBlockLayout(v.options,host.scalar_block_layout);
        spvValidatorOptionsSetUniformBufferStandardLayout(v.options,host.uniform_buffer_standard_layout);
        spv_const_binary_t binary{program.spirv.data(),program.spirv.size()};
        if(spvValidateWithOptions(v.context,v.options,&binary,&v.diagnostic)!=SPV_SUCCESS)
            throw std::runtime_error(std::string("Shader is invalid for enabled Vulkan layout features: ")+(v.diagnostic?v.diagnostic->error:"validation failed"));
    }
#define SHADER_FUNCS(X) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateBufferView) X(DestroyBufferView) X(CreateSampler) X(DestroySampler) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdDispatchIndirect) \
    X(CreateGraphicsPipelines) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect)
#define X(name) PFN_vk##name name=nullptr;
    SHADER_FUNCS(X)
#undef X
};
struct ObjectLease {
    std::shared_ptr<assets::AllocationBudget> budget;
    explicit ObjectLease(std::shared_ptr<assets::AllocationBudget> b):budget(std::move(b)){shader_require(budget && budget->acquire(1),"Shader object budget exhausted");}
    explicit ObjectLease(const std::shared_ptr<ShaderDevice>& d):ObjectLease(d->host.objects){}
    ~ObjectLease(){budget->release(1);}
};
struct ShaderPipelineState {
    std::shared_ptr<ShaderDevice> device;
    ObjectLease lease;
    std::vector<ShaderBinding> bindings;
    std::vector<HlslProgram> programs;
    GraphicsDescription graphics;
    bool is_graphics=false;
    std::vector<VkShaderModule> modules;
    VkRenderPass pass=VK_NULL_HANDLE;
    ShaderUsage usage;
    VkShaderModule module=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    std::map<VkDescriptorType,uint32_t> counts;
    // Descriptor sets of this layout: an invocation returns its set once its submission completed,
    // and the next invocation updates and binds it again. Pools of kSetsPerPool sets are made only
    // when none is free (BUG-058: a draw each frame creates no pool and allocates no set).
    static constexpr uint32_t kSetsPerPool=16;
    struct Sets {std::mutex mutex;std::vector<VkDescriptorPool> pools;std::vector<VkDescriptorSet> free;uint32_t left=0;} sets;
    explicit ShaderPipelineState(std::shared_ptr<ShaderDevice> d):device(std::move(d)),lease(device){}
    ~ShaderPipelineState(){const auto& d=*device;const auto dev=d.resource.device;
        for(auto pool:sets.pools)d.DestroyDescriptorPool(dev,pool,nullptr);
        if(pipeline)d.DestroyPipeline(dev,pipeline,nullptr);if(layout)d.DestroyPipelineLayout(dev,layout,nullptr);
        for(auto handle:modules)d.DestroyShaderModule(dev,handle,nullptr);if(pass)d.DestroyRenderPass(dev,pass,nullptr);
        if(descriptors)d.DestroyDescriptorSetLayout(dev,descriptors,nullptr);if(module)d.DestroyShaderModule(dev,module,nullptr);}
};
struct ShaderSampler::Impl {
    std::shared_ptr<ShaderDevice> device;
    ObjectLease lease,sampler_lease;
    VkSampler handle=VK_NULL_HANDLE;
    VkSamplerCreateInfo info{};
    explicit Impl(std::shared_ptr<ShaderDevice> d):device(std::move(d)),lease(device),sampler_lease(device->host.sampler_objects){}
    ~Impl(){if(handle)device->DestroySampler(device->resource.device,handle,nullptr);}
};
struct ShaderCommands::Impl {
    ResourceCommands& commands;
    std::shared_ptr<ShaderDevice> device=std::make_shared<ShaderDevice>();
    explicit Impl(ResourceCommands& c):commands(c){}
};

struct ShaderInvocation {
    std::shared_ptr<ShaderPipelineState> pipeline;
    ObjectLease lease;
    VkDescriptorSet set=VK_NULL_HANDLE; // the pipeline's, returned to it here
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    std::vector<VkBufferView> views;
    std::vector<ShaderDescriptor> owners;
    Buffer arguments;
    GraphicsDraw graphics;
    explicit ShaderInvocation(std::shared_ptr<ShaderPipelineState> p):pipeline(std::move(p)),lease(pipeline->device){}
    ~ShaderInvocation(){const auto& d=*pipeline->device;const auto dev=d.resource.device;
        if(framebuffer)d.DestroyFramebuffer(dev,framebuffer,nullptr);
        if(set){std::lock_guard<std::mutex> lock(pipeline->sets.mutex);pipeline->sets.free.push_back(set);}
        for(auto view:views)d.DestroyBufferView(dev,view,nullptr);}
};
void shader_capabilities(const HlslProgram&,const ShaderDevice&);
}
