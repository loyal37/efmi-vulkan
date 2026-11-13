#include "shader_commands_internal.h"
#include "native_shader.h"
#include "image_formats.h"
#include "shader_usage.h"
#include "driver_workarounds.h"
#include <spirv_reflect.h>
#include <spirv-tools/libspirv.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>

namespace efmi::gpu {
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void checked(VkResult result,const char* why){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(why)+": "+std::to_string(result));}
const ImageFormatInfo& format_info(VkFormat format){require(format>VK_FORMAT_UNDEFINED && uint32_t(format)<185,"Shader view format requires an extension adapter");return image_formats[uint32_t(format)];}
VkImageViewType view_type(const ShaderBinding& b){
    switch(b.image_dimension){
    case SpvDim1D:return b.image_array?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D;
    case SpvDim2D:return b.image_array?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    case SpvDim3D:require(!b.image_array,"Shader declares an unsupported array of volume images");return VK_IMAGE_VIEW_TYPE_3D;
    case SpvDimCube:return b.image_array?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE;
    default:throw std::runtime_error("Shader image dimension requires another adapter");
    }
}
}
void shader_capabilities(const HlslProgram& program,const ShaderDevice& device){
    const auto& f=device.resource.enabled_features;
    for(auto cap:program.capabilities){bool supported=false;
        switch(cap){
        case SpvCapabilityMatrix:case SpvCapabilityShader:case SpvCapabilitySampled1D:case SpvCapabilityImage1D:
        case SpvCapabilitySampledBuffer:case SpvCapabilityImageBuffer:case SpvCapabilityImageQuery:
        // DerivativeControl is core Vulkan 1.0; the validator still enforces stage/instruction rules.
        // Cached SM5 ddx/ddy commonly translate to the coarse/fine derivative instructions.
        case SpvCapabilityDerivativeControl:
        case SpvCapabilityImageMSArray:case SpvCapabilityAtomicStorage:supported=true;break;
        case SpvCapabilityDrawParameters:supported=device.host.shader_draw_parameters;break;
        case SpvCapabilityDenormPreserve:case SpvCapabilityDenormFlushToZero:
        case SpvCapabilitySignedZeroInfNanPreserve:case SpvCapabilityRoundingModeRTE:supported=device.host.float_controls;break;
        case SpvCapabilityGeometry:supported=f.geometryShader;break;
        case SpvCapabilityTessellation:supported=f.tessellationShader;break;
        case SpvCapabilityFloat64:supported=f.shaderFloat64;break;
        case SpvCapabilityInt64:supported=f.shaderInt64;break;
        case SpvCapabilityInt16:supported=f.shaderInt16;break;
        case SpvCapabilityImageGatherExtended:supported=f.shaderImageGatherExtended;break;
        case SpvCapabilityStorageImageMultisample:supported=f.shaderStorageImageMultisample;break;
        case SpvCapabilityStorageImageExtendedFormats:supported=f.shaderStorageImageExtendedFormats;break;
        case SpvCapabilityStorageImageReadWithoutFormat:supported=f.shaderStorageImageReadWithoutFormat;break;
        case SpvCapabilityStorageImageWriteWithoutFormat:supported=f.shaderStorageImageWriteWithoutFormat;break;
        case SpvCapabilityUniformBufferArrayDynamicIndexing:supported=f.shaderUniformBufferArrayDynamicIndexing;break;
        case SpvCapabilitySampledImageArrayDynamicIndexing:supported=f.shaderSampledImageArrayDynamicIndexing;break;
        case SpvCapabilityStorageBufferArrayDynamicIndexing:supported=f.shaderStorageBufferArrayDynamicIndexing;break;
        case SpvCapabilityStorageImageArrayDynamicIndexing:supported=f.shaderStorageImageArrayDynamicIndexing;break;
        case SpvCapabilityImageCubeArray:case SpvCapabilitySampledCubeArray:supported=f.imageCubeArray;break;
        default:break;
        }
        if(!supported)throw std::runtime_error("Shader capability " + std::to_string(cap) +
            " is not enabled or needs another execution adapter (stage " + std::to_string(program.stage) + ")");
    }
    for(size_t at=5;at<program.spirv.size();){const auto word=program.spirv[at],count=word>>16,op=word&65535;
        require(count && count<=program.spirv.size()-at,"Invalid float-control instruction boundary");
        if(op==SpvOpExecutionMode && count>=3){const auto mode=program.spirv[at+2];
            if(mode==SpvExecutionModeDenormPreserve || mode==SpvExecutionModeDenormFlushToZero ||
               mode==SpvExecutionModeSignedZeroInfNanPreserve || mode==SpvExecutionModeRoundingModeRTE){
                require(count==4 && program.spirv[at+3]==32 && device.host.float_controls,
                    "Shader arithmetic mode requires enabled 32-bit float controls");
                const auto& p=device.host.float_properties;
                require(mode==SpvExecutionModeDenormPreserve?p.shaderDenormPreserveFloat32:
                    mode==SpvExecutionModeDenormFlushToZero?p.shaderDenormFlushToZeroFloat32:
                    mode==SpvExecutionModeSignedZeroInfNanPreserve?p.shaderSignedZeroInfNanPreserveFloat32:
                    p.shaderRoundingModeRTEFloat32,"Shader arithmetic mode is unsupported by this device");}}
        at+=count;
    }
}
ComputePipeline::ComputePipeline(std::shared_ptr<ShaderPipelineState> impl):impl_(std::move(impl)){}
ComputePipeline::~ComputePipeline()=default;
const HlslProgram& ComputePipeline::program() const {return impl_->programs.front();}
ShaderSampler::ShaderSampler(std::shared_ptr<Impl> impl):impl_(std::move(impl)){}
ShaderSampler::~ShaderSampler()=default;
bool validate_native_shader(const std::vector<uint32_t>& words, VkShaderStageFlagBits stage,
                            const ShaderHost& host, const VkPhysicalDeviceFeatures& features, uint32_t api, std::string& reason) {
    try {
        ShaderDevice d;
        d.host = host;
        d.resource.enabled_features = features;
        require(host.validator.is_absolute(), "Native validator path must be absolute");
        d.validator = LoadLibraryExW(host.validator.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(d.validator != nullptr, "Cannot load native SPIR-V validator");
#define X(name) d.name = reinterpret_cast<decltype(d.name)>(GetProcAddress(d.validator, #name)); require(d.name != nullptr, "Missing validator entry " #name);
        VALIDATOR_FUNCS(X)
#undef X
        HlslProgram program;
        program.spirv = words; program.stage = stage; program.vulkan_version = api;
        d.validate(program);
        for (size_t at = 5; at < words.size(); at += words[at] >> 16)
            if ((words[at] & 65535) == SpvOpCapability) program.capabilities.push_back(words[at + 1]);
        shader_capabilities(program, d);
        SpvReflectShaderModule reflected{};
        require(spvReflectCreateShaderModule(words.size() * 4, words.data(), &reflected) == SPV_REFLECT_RESULT_SUCCESS,
                "Cannot reflect native shader");
        const bool correct_stage = reflected.entry_point_count == 1 && uint32_t(reflected.shader_stage) == uint32_t(stage);
        spvReflectDestroyShaderModule(&reflected);
        require(correct_stage, "Native shader must have one entry point of the named stage");
        reason.clear();
        return true;
    } catch (const std::exception& e) { reason = e.what(); return false; }
}

bool native_shader_interface(const std::vector<uint32_t>& original, const std::vector<uint32_t>& replacement,
                             VkShaderStageFlagBits stage, const std::string& entry, std::string& reason) {
    struct Reflection {
        SpvReflectShaderModule module{};
        bool valid = false;
        ~Reflection() { if (valid) spvReflectDestroyShaderModule(&module); }
        void read(const std::vector<uint32_t>& code) {
            require(spvReflectCreateShaderModule(code.size() * 4, code.data(), &module) == SPV_REFLECT_RESULT_SUCCESS,
                    "Cannot reflect native shader interface");
            valid = true;
        }
    } before, after;
    try {
        before.read(original); after.read(replacement);
        const auto& a = before.module; const auto& b = after.module;
        require(a.entry_point_count == 1 && b.entry_point_count == 1 && b.entry_point_name && entry == b.entry_point_name &&
                uint32_t(b.shader_stage) == uint32_t(stage), "Native shader entry point or stage differs");
        for (uint32_t i = 0; i < b.descriptor_binding_count; ++i) {
            const auto& next = b.descriptor_bindings[i];
            const SpvReflectDescriptorBinding* old = nullptr;
            for (uint32_t j = 0; j < a.descriptor_binding_count; ++j)
                if (a.descriptor_bindings[j].set == next.set && a.descriptor_bindings[j].binding == next.binding) old = &a.descriptor_bindings[j];
            require(old && old->descriptor_type == next.descriptor_type && old->count == next.count && old->block.size >= next.block.size,
                    "Native shader changes a descriptor interface");
            require(!next.accessed || old->accessed, "Native shader accesses a previously unused descriptor");
            require(old->resource_type == next.resource_type && old->input_attachment_index == next.input_attachment_index &&
                    old->image.dim == next.image.dim && old->image.depth == next.image.depth && old->image.arrayed == next.image.arrayed &&
                    old->image.ms == next.image.ms && old->image.sampled == next.image.sampled && old->image.image_format == next.image.image_format,
                    "Native shader changes an image type or resource access class");
            if (old->type_description && next.type_description &&
                (old->type_description->type_flags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_IMAGE)) {
                const auto& x = *old->type_description; const auto& y = *next.type_description;
                constexpr uint32_t scalar = SPV_REFLECT_TYPE_FLAG_BOOL | SPV_REFLECT_TYPE_FLAG_INT | SPV_REFLECT_TYPE_FLAG_FLOAT;
                require((x.type_flags & scalar) == (y.type_flags & scalar) &&
                        x.traits.numeric.scalar.width == y.traits.numeric.scalar.width &&
                        x.traits.numeric.scalar.signedness == y.traits.numeric.scalar.signedness,
                        "Native shader changes an image sampled scalar type");
            }
            constexpr uint32_t access = SPV_REFLECT_DECORATION_NON_WRITABLE | SPV_REFLECT_DECORATION_NON_READABLE;
            require((next.decoration_flags & old->decoration_flags & access) == (old->decoration_flags & access),
                    "Native shader expands descriptor read/write access");
            const auto block_access = [&](const auto& self, const SpvReflectBlockVariable& x, const SpvReflectBlockVariable& y) -> bool {
                if ((x.decoration_flags & y.decoration_flags & access) != (x.decoration_flags & access)) return false;
                for (uint32_t n = 0; n < y.member_count; ++n) {
                    const SpvReflectBlockVariable* member = nullptr;
                    for (uint32_t m = 0; m < x.member_count; ++m) if (x.members[m].offset == y.members[n].offset) member = &x.members[m];
                    if (!member || !self(self, *member, y.members[n])) return false;
                }
                return true;
            };
            require(block_access(block_access, old->block, next.block), "Native shader expands storage block access");
        }
        for (uint32_t i = 0; i < b.push_constant_block_count; ++i) {
            const auto& next = b.push_constant_blocks[i];
            bool covered = false;
            for (uint32_t j = 0; j < a.push_constant_block_count; ++j) {
                const auto& old = a.push_constant_blocks[j];
                covered |= next.offset >= old.offset && uint64_t(next.offset) + next.size <= uint64_t(old.offset) + old.size;
            }
            require(covered, "Native shader expands push constants");
        }
        const auto same_variable = [](const auto& self, const SpvReflectInterfaceVariable& x, const SpvReflectInterfaceVariable& y) -> bool {
            if (x.built_in != y.built_in || (x.built_in < 0 && (x.location != y.location || x.component != y.component)) || x.format != y.format ||
                x.array.dims_count != y.array.dims_count || x.member_count != y.member_count ||
                x.numeric.scalar.width != y.numeric.scalar.width || x.numeric.vector.component_count != y.numeric.vector.component_count) return false;
            for (uint32_t i = 0; i < x.array.dims_count; ++i) if (x.array.dims[i] != y.array.dims[i]) return false;
            for (uint32_t i = 0; i < x.member_count; ++i) if (!self(self, x.members[i], y.members[i])) return false;
            return true;
        };
        const auto flatten = [](const auto& self, const SpvReflectInterfaceVariable* variable, std::vector<const SpvReflectInterfaceVariable*>& out) -> void {
            if (variable->member_count && !variable->array.dims_count)
                for (uint32_t i = 0; i < variable->member_count; ++i) self(self, &variable->members[i], out);
            else out.push_back(variable);
        };
        auto compatible = [&](SpvReflectInterfaceVariable* const* used, uint32_t count,
                             SpvReflectInterfaceVariable* const* supplied, uint32_t available) {
            std::vector<const SpvReflectInterfaceVariable*> needs, has;
            for (uint32_t i = 0; i < count; ++i) flatten(flatten, used[i], needs);
            for (uint32_t i = 0; i < available; ++i) flatten(flatten, supplied[i], has);
            for (const auto* variable : needs) {
                bool found = false;
                for (const auto* candidate : has) found |= same_variable(same_variable, *variable, *candidate);
                if (!found) return false;
            }
            return true;
        };
        require(compatible(b.input_variables, b.input_variable_count, a.input_variables, a.input_variable_count),
                "Native shader changes an input location");
        require(compatible(b.output_variables, b.output_variable_count, a.output_variables, a.output_variable_count),
                "Native shader changes an output location");
        if (stage != VK_SHADER_STAGE_FRAGMENT_BIT)
            require(compatible(a.output_variables, a.output_variable_count, b.output_variables, b.output_variable_count),
                    "Native shader removes an output used by the next stage");
        reason.clear(); return true;
    } catch (const std::exception& e) { reason = e.what(); return false; }
}

ShaderCommands::ShaderCommands(ResourceCommands& commands,ShaderHost host):impl_(std::make_unique<Impl>(commands)){
    require(host.get_device_proc && host.objects && host.properties.apiVersion>=commands.device_.api_version &&
        commands.device_.api_version>=VK_API_VERSION_1_1 && (commands.device_.queue_flags&VK_QUEUE_COMPUTE_BIT),
        "Shaders require explicit compute queue/device capabilities");
    require(host.sampler_objects && host.sampler_objects->limit()<=host.properties.limits.maxSamplerAllocationCount,"Owned sampler quota exceeds the device limit");
    auto& d=*impl_->device;d.resource=commands.device_;d.host=std::move(host);d.domain=commands.domain_;
    require(d.host.validator.is_absolute(),"SPIR-V validator DLL path must be explicit and absolute");
    d.validator=LoadLibraryExW(d.host.validator.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(d.validator!=nullptr,"Cannot load configured SPIR-V validator");
#define X(name) d.name=reinterpret_cast<decltype(d.name)>(GetProcAddress(d.validator,#name));require(d.name!=nullptr,"Missing validator entry " #name);
    VALIDATOR_FUNCS(X)
#undef X
#define X(name) d.name=reinterpret_cast<PFN_vk##name>(d.host.get_device_proc(d.resource.device,"vk" #name));require(d.name!=nullptr,"Missing shader dispatch vk" #name);
    SHADER_FUNCS(X)
#undef X
}
ShaderCommands::~ShaderCommands()=default;
const ShaderHost& ShaderCommands::host() const {return impl_->device->host;}
Compute ShaderCommands::compile(const HlslProgram& program){
    auto d=impl_->device;const auto& limits=d->host.properties.limits;
    require(program.stage==VK_SHADER_STAGE_COMPUTE_BIT && program.vulkan_version<=d->resource.api_version &&
        program.spirv.size()>=5 && program.spirv[0]==SpvMagicNumber,"Shader stage/target is not compatible with this compute domain");
    shader_capabilities(program,*d);
    d->validate(program);
    auto usage=shader_usage(program);
    require(usage.workgroup_bytes<=limits.maxComputeSharedMemorySize,"Shader shared storage exceeds device limit");
    uint64_t threads=1;for(unsigned i=0;i<3;++i){require(program.threads[i] && program.threads[i]<=limits.maxComputeWorkGroupSize[i],"Shader local size exceeds the device limit");threads*=program.threads[i];}
    require(threads<=limits.maxComputeWorkGroupInvocations,"Shader workgroup invocation count exceeds device limit");
    auto result=std::make_shared<ShaderPipelineState>(d);result->programs={program};result->usage=std::move(usage);
    layout(*result);const auto dev=d->resource.device;
    const auto words=driver_module_words(program.spirv,d->host.properties);
    VkShaderModuleCreateInfo module={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=words.size()*4;module.pCode=words.data();
    checked(d->CreateShaderModule(dev,&module,nullptr,&result->module),"Create compute shader module");
    VkComputePipelineCreateInfo pipeline={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=result->layout;
    pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=result->module;pipeline.stage.pName="main";
    checked(d->CreateComputePipelines(dev,d->host.pipeline_cache,1,&pipeline,nullptr,&result->pipeline),"Create compute pipeline");
    return std::make_shared<ComputePipeline>(std::move(result));
}
void ShaderCommands::layout(ShaderPipelineState& result){
    auto d=impl_->device;const auto& limits=d->host.properties.limits;
    std::vector<VkDescriptorSetLayoutBinding> bindings;std::set<uint32_t> used;uint32_t total=0;
    auto validate=[&](const std::map<VkDescriptorType,uint32_t>& counts,uint32_t size,bool stage){
        auto count=[&](VkDescriptorType type){const auto it=counts.find(type);return it==counts.end()?0:it->second;};
        const auto sampled=count(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)+count(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        const auto storage=count(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)+count(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        require((!stage || size-count(VK_DESCRIPTOR_TYPE_SAMPLER)<=limits.maxPerStageResources) &&
            sampled<=(stage?limits.maxPerStageDescriptorSampledImages:limits.maxDescriptorSetSampledImages) &&
            storage<=(stage?limits.maxPerStageDescriptorStorageImages:limits.maxDescriptorSetStorageImages) &&
            count(VK_DESCRIPTOR_TYPE_SAMPLER)<=(stage?limits.maxPerStageDescriptorSamplers:limits.maxDescriptorSetSamplers) &&
            count(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)<=(stage?limits.maxPerStageDescriptorUniformBuffers:limits.maxDescriptorSetUniformBuffers) &&
            count(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)<=(stage?limits.maxPerStageDescriptorStorageBuffers:limits.maxDescriptorSetStorageBuffers),"Shader descriptor usage exceeds device limits");
    };
    for(const auto& program:result.programs){std::map<VkDescriptorType,uint32_t> counts;uint32_t stage_total=0;
        for(const auto& b:program.bindings){require(b.count && b.count<=2048 && stage_total<=2048-b.count &&
            total<=8192-b.count && used.insert(b.binding).second,"Shader descriptor count/identity invalid");stage_total+=b.count;total+=b.count;
            switch(b.type){case VK_DESCRIPTOR_TYPE_SAMPLER:case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:break;
            default:throw std::runtime_error("Shader descriptor type needs another adapter");}
            counts[b.type]+=b.count;result.counts[b.type]+=b.count;result.bindings.push_back(b);
            bindings.push_back({b.binding,b.type,b.count,VkShaderStageFlags(program.stage),nullptr});
        }
        validate(counts,stage_total,true);
    }
    validate(result.counts,total,false);
    const auto dev=d->resource.device;VkDescriptorSetLayoutCreateInfo descriptor={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount=uint32_t(bindings.size());descriptor.pBindings=bindings.data();
    checked(d->CreateDescriptorSetLayout(dev,&descriptor,nullptr,&result.descriptors),"Create shader descriptor layout");
    VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&result.descriptors;
    checked(d->CreatePipelineLayout(dev,&layout,nullptr,&result.layout),"Create shader pipeline layout");
}
Sampler ShaderCommands::sampler(const VkSamplerCreateInfo& info){
    const auto d=impl_->device;const auto& limits=d->host.properties.limits;
    require(info.sType==VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO && !info.pNext && !info.flags && !info.unnormalizedCoordinates,
        "Sampler extensions/unnormalized coordinates need another adapter");
    require((info.magFilter==VK_FILTER_NEAREST || info.magFilter==VK_FILTER_LINEAR) &&
        (info.minFilter==VK_FILTER_NEAREST || info.minFilter==VK_FILTER_LINEAR) &&
        (info.mipmapMode==VK_SAMPLER_MIPMAP_MODE_NEAREST || info.mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR),"Invalid core sampler filter");
    for(auto mode:{info.addressModeU,info.addressModeV,info.addressModeW})require(mode>=VK_SAMPLER_ADDRESS_MODE_REPEAT && mode<=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,"Sampler address mode needs an enabled extension");
    require(std::isfinite(info.mipLodBias) && std::abs(info.mipLodBias)<=limits.maxSamplerLodBias &&
        std::isfinite(info.minLod) && std::isfinite(info.maxLod) && info.minLod<=info.maxLod &&
        info.borderColor>=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK && info.borderColor<=VK_BORDER_COLOR_INT_OPAQUE_WHITE &&
        info.compareOp>=VK_COMPARE_OP_NEVER && info.compareOp<=VK_COMPARE_OP_ALWAYS,"Invalid core sampler parameters");
    require(!info.anisotropyEnable || (d->resource.enabled_features.samplerAnisotropy && std::isfinite(info.maxAnisotropy) &&
        info.maxAnisotropy>=1 && info.maxAnisotropy<=limits.maxSamplerAnisotropy),"Sampler anisotropy is not enabled or exceeds device limits");
    auto result=std::make_shared<ShaderSampler::Impl>(d);result->info=info;
    checked(d->CreateSampler(d->resource.device,&info,nullptr,&result->handle),"Create shader sampler");return std::make_shared<ShaderSampler>(std::move(result));
}
void ShaderCommands::dispatch(const Compute& pipeline,const std::vector<ShaderDescriptor>& descriptors,std::array<uint32_t,3> groups){
    record(pipeline,descriptors,groups,{},0);
}
void ShaderCommands::dispatch_indirect(const Compute& pipeline,const std::vector<ShaderDescriptor>& descriptors,const Buffer& arguments,VkDeviceSize offset){
    require(bool(arguments),"Indirect dispatch needs an owned argument buffer");record(pipeline,descriptors,{1,1,1},arguments,offset);
}
std::shared_ptr<ShaderInvocation> ShaderCommands::descriptors(const std::shared_ptr<ShaderPipelineState>& pipeline,const std::vector<ShaderDescriptor>& descriptors){
    auto& commands=impl_->commands;const auto d=impl_->device;const auto& limits=d->host.properties.limits;
    auto invocation=std::make_shared<ShaderInvocation>(pipeline);invocation->owners=descriptors;
    std::map<std::pair<uint32_t,uint32_t>,const ShaderDescriptor*> provided;
    for(const auto& descriptor:descriptors)require(provided.emplace(std::make_pair(descriptor.binding,descriptor.element),&descriptor).second,"Duplicate shader descriptor");
    size_t expected=0;for(const auto& b:pipeline->bindings)expected+=b.count;
    require(descriptors.size()==expected,"Shader descriptor count does not match reflection");
    std::vector<VkDescriptorBufferInfo> buffers;std::vector<VkDescriptorImageInfo> images;std::vector<VkWriteDescriptorSet> writes;
    buffers.reserve(expected);images.reserve(expected);writes.reserve(expected);invocation->views.reserve(expected);
    for(const auto& binding:pipeline->bindings)for(uint32_t element=0;element<binding.count;++element){
        const auto found=provided.find({binding.binding,element});require(found!=provided.end(),"Missing shader descriptor array element");const auto& value=*found->second;
        auto storage_format=[&](VkFormat format,bool buffer){
            const auto& f=format_info(format);bool reads=false,writes=false,atomic=false;
            for(const auto& use:pipeline->usage.storage_images)if(use.binding==binding.binding && use.element==element){
                reads|=use.read;writes|=use.write;atomic|=use.atomic;}
            if(binding.image_format)require(binding.image_format==f.storage_format,"Storage view format differs from the shader format");
            else if(reads || writes){
                require(d->resource.format_features2 && d->resource.GetFormatProperties2,"Unformatted storage needs enabled format feature queries");
                require((!reads || d->resource.enabled_features.shaderStorageImageReadWithoutFormat) &&
                    (!writes || d->resource.enabled_features.shaderStorageImageWriteWithoutFormat),"Unformatted storage access feature is not enabled");
                VkFormatProperties3 extra{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};properties.pNext=&extra;
                d->resource.GetFormatProperties2(d->resource.physical,format,&properties);
                const auto available=buffer?extra.bufferFeatures:extra.optimalTilingFeatures;
                const VkFormatFeatureFlags2 required=(reads?VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT:0)|
                    (writes?VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT:0);
                require((available&required)==required,"Storage view format does not support unformatted shader access");
            }
            if(atomic){VkFormatProperties properties{};require(d->resource.GetFormatProperties,"Storage atomic format query unavailable");
                d->resource.GetFormatProperties(d->resource.physical,format,&properties);
                require(buffer?bool(properties.bufferFeatures&VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT):
                    bool(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT),"Storage view format lacks atomic support");}
        };
        VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstBinding=binding.binding;write.dstArrayElement=element;write.descriptorCount=1;write.descriptorType=binding.type;
        if(value.null){
            require(binding.type!=VK_DESCRIPTOR_TYPE_SAMPLER && !value.buffer && !value.image && !value.sampler,"Null shader descriptor carries a resource");
            require(d->host.null_descriptors,"A null source resource needs nullDescriptor enabled on the device");
            if(binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binding.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
                buffers.push_back({VK_NULL_HANDLE,0,VK_WHOLE_SIZE});write.pBufferInfo=&buffers.back();
            }else if(binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || binding.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){
                invocation->views.push_back(VK_NULL_HANDLE);write.pTexelBufferView=&invocation->views.back();
            }else{images.push_back({VK_NULL_HANDLE,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL});write.pImageInfo=&images.back();}
            writes.push_back(write);continue;
        }
        if(binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binding.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || binding.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){
            require(value.buffer && !value.image && !value.sampler,"Shader buffer descriptor requires exactly one owned buffer");commands.validate(value.buffer,value.offset,value.size);
            const bool uniform=binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,storage=binding.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if(uniform || storage){const auto usage=uniform?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                const auto alignment=uniform?limits.minUniformBufferOffsetAlignment:limits.minStorageBufferOffsetAlignment;
                require((value.buffer->usage()&usage) && alignment && value.offset%alignment==0 &&
                    value.size<=(uniform?limits.maxUniformBufferRange:limits.maxStorageBufferRange) &&
                    (uniform?value.size>=binding.block_size:(!binding.stride || value.size>=binding.stride)),"Shader buffer range/usage/alignment invalid");
                buffers.push_back({value.buffer->buffer(),value.offset,value.size});write.pBufferInfo=&buffers.back();
            }else{const auto& format=format_info(value.format);const bool read=binding.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                // The offset alignment of every texel usage the buffer has (the view's creation checks each).
                VkDeviceSize alignment=limits.minTexelBufferOffsetAlignment;
                if(d->host.texel_alignment && format.bytes){
                    const auto& t=d->host.texel_alignment_properties;const VkDeviceSize texel=format.bytes%3==0?format.bytes/3:format.bytes;
                    auto need=[&](VkDeviceSize bytes,VkBool32 single){return single?std::min(bytes,texel):bytes;};
                    alignment=1;
                    if(value.buffer->usage()&VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)
                        alignment=std::lcm(alignment,need(t.uniformTexelBufferOffsetAlignmentBytes,t.uniformTexelBufferOffsetSingleTexelAlignment));
                    if(value.buffer->usage()&VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
                        alignment=std::lcm(alignment,need(t.storageTexelBufferOffsetAlignmentBytes,t.storageTexelBufferOffsetSingleTexelAlignment));
                }
                require(format.aspects==VK_IMAGE_ASPECT_COLOR_BIT && !format.compression && alignment &&
                    value.offset%alignment==0 && value.offset%format.bytes==0 && value.size%format.bytes==0 &&
                    value.size/format.bytes<=limits.maxTexelBufferElements && binding.scalar_width==32 && binding.scalar_type==format.scalar_type &&
                    (value.buffer->usage()&(read?VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)),"Shader texel-buffer view is incompatible");
                if(!read)storage_format(value.format,true);
                VkFormatProperties properties{};require(d->resource.GetFormatProperties,"Shader format query unavailable");d->resource.GetFormatProperties(d->resource.physical,value.format,&properties);
                require(properties.bufferFeatures&(read?VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT:VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT),"Texel-buffer format unsupported");
                VkBufferViewCreateInfo view={VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};view.buffer=value.buffer->buffer();view.offset=value.offset;view.range=value.size;view.format=value.format;
                VkBufferView handle=VK_NULL_HANDLE;checked(d->CreateBufferView(d->resource.device,&view,nullptr,&handle),"Create shader buffer view");
                invocation->views.push_back(handle);write.pTexelBufferView=&invocation->views.back();
            }
        }else if(binding.type==VK_DESCRIPTOR_TYPE_SAMPLER){
            require(value.sampler && value.sampler->impl_ && value.sampler->impl_->device->domain==d->domain && !value.buffer && !value.image,"Shader sampler requires a sampler from this domain");
            images.push_back({value.sampler->impl_->handle,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED});write.pImageInfo=&images.back();
        }else{
            require(value.image && !value.buffer && !value.sampler,"Shader image descriptor requires exactly one owned view");commands.validate(value.image->owner());
            const auto& format=format_info(value.image->format());const auto& description=value.image->owner()->description();const bool sampled=binding.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            const auto aspects=value.image->range().aspectMask;const auto scalar=aspects==VK_IMAGE_ASPECT_STENCIL_BIT?2:format.scalar_type;
            require(value.image->type()==view_type(binding) && binding.image_multisample==(description.samples!=VK_SAMPLE_COUNT_1_BIT) &&
                (binding.image_depth!=1 || aspects==VK_IMAGE_ASPECT_DEPTH_BIT) && binding.scalar_width==32 && binding.scalar_type==scalar &&
                (value.image->usage()&(sampled?VK_IMAGE_USAGE_SAMPLED_BIT:VK_IMAGE_USAGE_STORAGE_BIT)),"Shader image type/format/usage is incompatible");
            if(!sampled)storage_format(value.image->format(),false);
            images.push_back({VK_NULL_HANDLE,value.image->view(),VK_IMAGE_LAYOUT_GENERAL});write.pImageInfo=&images.back();
        }
        writes.push_back(write);
    }
    // Only actual sampled-image constructors pair these descriptors. Fetch-only
    // views and unrelated arrays must not inherit another image's filter rules.
    for(const auto& pair:pipeline->usage.sampled_pairs){
        const auto& image=provided.at({pair.image_binding,pair.image_element})->image;
        if(!image)continue; // A null descriptor samples zero; no filter support applies.
        const auto& info=provided.at({pair.sampler_binding,pair.sampler_element})->sampler->impl_->info;
        if(!info.compareEnable && (info.magFilter==VK_FILTER_LINEAR || info.minFilter==VK_FILTER_LINEAR || info.mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR)){
            VkFormatProperties properties{};d->resource.GetFormatProperties(d->resource.physical,image->format(),&properties);
            require(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,"Sampled image does not support the bound linear sampler");}
        require(!info.compareEnable || image->range().aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT,"Comparison sampler requires a depth view");
    }
    {
        // A set a completed submission returned, else one of the newest pool, else a new pool.
        auto& sets=pipeline->sets;std::lock_guard<std::mutex> lock(sets.mutex);
        if(!sets.free.empty()){invocation->set=sets.free.back();sets.free.pop_back();++commands.stats_.sets_reused;}
        else{
            if(!sets.left){
                std::vector<VkDescriptorPoolSize> sizes;
                for(const auto& count:pipeline->counts)sizes.push_back({count.first,count.second*ShaderPipelineState::kSetsPerPool});
                VkDescriptorPoolCreateInfo pool={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=ShaderPipelineState::kSetsPerPool;
                pool.poolSizeCount=uint32_t(sizes.size());pool.pPoolSizes=sizes.data();VkDescriptorPool handle=VK_NULL_HANDLE;
                checked(d->CreateDescriptorPool(d->resource.device,&pool,nullptr,&handle),"Create dispatch descriptor pool");
                sets.pools.push_back(handle);sets.left=ShaderPipelineState::kSetsPerPool;++commands.stats_.descriptor_pools;
            }
            VkDescriptorSetAllocateInfo allocate={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=sets.pools.back();
            allocate.descriptorSetCount=1;allocate.pSetLayouts=&pipeline->descriptors;
            checked(d->AllocateDescriptorSets(d->resource.device,&allocate,&invocation->set),"Allocate dispatch descriptor set");--sets.left;
        }
    }
    for(auto& write:writes)write.dstSet=invocation->set;d->UpdateDescriptorSets(d->resource.device,uint32_t(writes.size()),writes.data(),0,nullptr);
    return invocation;
}
void ShaderCommands::record(const Compute& pipeline,const std::vector<ShaderDescriptor>& descriptors,std::array<uint32_t,3> groups,const Buffer& arguments,VkDeviceSize offset){
    auto& commands=impl_->commands;const auto d=impl_->device;const auto& limits=d->host.properties.limits;
    require(pipeline && pipeline->impl_ && pipeline->impl_->device->domain==d->domain,"Compute pipeline belongs to another execution domain");
    if(arguments){commands.validate(arguments,offset,sizeof(VkDispatchIndirectCommand));
        require(offset%4==0 && (arguments->usage()&VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT),"Indirect dispatch argument usage/alignment invalid");}
    for(unsigned i=0;i<3;++i)require(groups[i]<=limits.maxComputeWorkGroupCount[i],"Dispatch group count exceeds device limits");
    // Zero dispatch still has already flushed INI parameters in the frontend,
    // but accesses no descriptors and records no GPU work.
    if(!groups[0] || !groups[1] || !groups[2])return;
    auto invocation=this->descriptors(pipeline->impl_,descriptors);invocation->arguments=arguments;
    // Retain every descriptor owner before any recorded command refers to it.
    const auto command=commands.command();commands.pending_->shader_owners_.push_back(invocation);
    for(const auto& value:descriptors)if(value.image)commands.transition(value.image->owner(),VK_IMAGE_LAYOUT_GENERAL);
    commands.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|(arguments?VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT:0),
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT|(arguments?VK_ACCESS_INDIRECT_COMMAND_READ_BIT:0));
    d->CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->impl_->pipeline);
    d->CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->impl_->layout,0,1,&invocation->set,0,nullptr);
    if(arguments)d->CmdDispatchIndirect(command,arguments->buffer(),offset);
    else d->CmdDispatch(command,groups[0],groups[1],groups[2]);
}
}
