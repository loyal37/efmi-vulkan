// Original production INI -> owned Vulkan commands -> GPU readback. Headless.
#include "../frontend/xxmi/gpu_resources.h"
#include "../frontend/gpu/hlsl_compiler.h"
#include "../frontend/gpu/graphics_link.h"
#include "../frontend/gpu/graphics_commands.h"
#include "../frontend/xxmi/gpu_shaders.h"
#include "../frontend/xxmi/graphics_state.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <limits>
using namespace efmi;
static_assert(!std::is_copy_constructible_v<gpu::BufferResource> && !std::is_copy_assignable_v<gpu::ResourceSubmission>,"GPU handles have unique owners");
static unsigned checks=0;
static std::atomic<unsigned> errors{0},warnings{0};
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);++checks;}
static void vkcheck(VkResult r,const char* why){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(why)+": "+std::to_string(r));}
namespace efvk {uint32_t crc32c(uint32_t seed,const void* data,size_t size){
    uint32_t h=~seed;const auto* b=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i){h^=b[i];for(int j=0;j<8;++j)h=(h>>1)^(0x82f63b78u&(0u-(h&1)));}return ~h;
}}
static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*){
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++errors;else ++warnings;
    printf("VALIDATION: %s\n",data->pMessage);return VK_FALSE;
}
struct Device {
    PFN_vkGetDeviceProcAddr get_device_proc=nullptr;
    VkInstance instance=VK_NULL_HANDLE;gpu::ResourceDevice resource;VkQueue queue=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    bool scalar_layout=false,draw_parameters=false;
    bool float_controls=false;
    bool null_descriptors=false;
    // The instance and device API version; 1.3 turns texelBufferAlignment on (D-151), as in the game.
    uint32_t api=VK_API_VERSION_1_1;bool texel_alignment=false;
    VkPhysicalDeviceTexelBufferAlignmentProperties texel_alignment_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
    VkPhysicalDeviceFloatControlsProperties float_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    bool vertex_divisor=false,vertex_zero_divisor=false,divisor_first_instance=false;uint32_t max_vertex_divisor=1;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance=nullptr;PFN_vkDestroyDevice destroy_device=nullptr;
    PFN_vkDeviceWaitIdle wait_idle=nullptr;PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug=nullptr;
    ~Device(){if(resource.device){wait_idle(resource.device);destroy_device(resource.device,nullptr);}
        if(messenger)destroy_debug(instance,messenger,nullptr);if(instance)destroy_instance(instance,nullptr);}
    void init(bool unrestricted_depth=false,bool scalar_block_layout=false,bool shader_draw_parameters=false,bool geometry=false,bool divisors=false,bool float_modes=false,bool preserve_float=false,bool typed_formats=false,bool null_descriptor=false){
        auto loader=LoadLibraryW(L"vulkan-1.dll");check(loader!=nullptr,"Vulkan loader");
        auto gi=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        auto create=reinterpret_cast<PFN_vkCreateInstance>(gi(nullptr,"vkCreateInstance"));
        VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="efmi_resource_commands";app.apiVersion=api;
        const char* layer="VK_LAYER_KHRONOS_validation";const char* ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debug={VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=diagnostic;
        VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.pNext=&debug;
        ci.enabledLayerCount=ci.enabledExtensionCount=1;ci.ppEnabledLayerNames=&layer;ci.ppEnabledExtensionNames=&ext;
        vkcheck(create(&ci,nullptr,&instance),"Create test instance");
#define GET(name) auto name=reinterpret_cast<PFN_vk##name>(gi(instance,"vk" #name));check(name!=nullptr,"vk" #name);
        GET(DestroyInstance);destroy_instance=DestroyInstance;
        GET(CreateDebugUtilsMessengerEXT);GET(DestroyDebugUtilsMessengerEXT);destroy_debug=DestroyDebugUtilsMessengerEXT;
        vkcheck(CreateDebugUtilsMessengerEXT(instance,&debug,nullptr,&messenger),"Create debug messenger");
        GET(EnumeratePhysicalDevices);GET(GetPhysicalDeviceQueueFamilyProperties);GET(GetPhysicalDeviceMemoryProperties);
        GET(CreateDevice);GET(GetDeviceProcAddr);GET(GetPhysicalDeviceFeatures);GET(GetPhysicalDeviceProperties);
        GET(GetPhysicalDeviceFeatures2);
        GET(GetPhysicalDeviceProperties2);
        get_device_proc=GetDeviceProcAddr;
        GET(GetPhysicalDeviceImageFormatProperties);GET(GetPhysicalDeviceFormatProperties);
        GET(GetPhysicalDeviceFormatProperties2);
        GET(EnumerateDeviceExtensionProperties);
#undef GET
        uint32_t count=0;vkcheck(EnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate test devices");
        std::vector<VkPhysicalDevice> physical(count);vkcheck(EnumeratePhysicalDevices(instance,&count,physical.data()),"Read test devices");
        VkPhysicalDevice selected=VK_NULL_HANDLE;
        for(auto p:physical){
            if(float_modes){VkPhysicalDeviceFloatControlsProperties modes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
                VkPhysicalDeviceProperties2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};available.pNext=&modes;GetPhysicalDeviceProperties2(p,&available);
                if(!modes.shaderDenormFlushToZeroFloat32)continue;}
            uint32_t n=0;GetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);
            GetPhysicalDeviceQueueFamilyProperties(p,&n,q.data());for(uint32_t i=0;i<n;++i)if(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){selected=p;resource.queue_family=i;resource.queue_flags=q[i].queueFlags;break;}
            if(selected)break;}
        check(selected!=VK_NULL_HANDLE,"Graphics queue available");GetPhysicalDeviceMemoryProperties(selected,&resource.memory);
        VkPhysicalDeviceFeatures features{};GetPhysicalDeviceFeatures(selected,&features);
        GetPhysicalDeviceProperties(selected,&properties);
        check(properties.apiVersion>=api,"Requested Vulkan version available");
        resource.physical=selected;resource.api_version=api;
        if(api>=VK_API_VERSION_1_3){VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            query.pNext=&texel_alignment_properties;GetPhysicalDeviceProperties2(selected,&query);texel_alignment=true;}
        resource.GetImageFormatProperties=GetPhysicalDeviceImageFormatProperties;resource.GetFormatProperties=GetPhysicalDeviceFormatProperties;
        resource.GetFormatProperties2=GetPhysicalDeviceFormatProperties2;
        resource.enabled_features.textureCompressionBC=features.textureCompressionBC;
        resource.enabled_features.textureCompressionETC2=features.textureCompressionETC2;
        resource.enabled_features.textureCompressionASTC_LDR=features.textureCompressionASTC_LDR;
        resource.enabled_features.imageCubeArray=features.imageCubeArray;
        resource.enabled_features.shaderStorageImageMultisample=features.shaderStorageImageMultisample;
        if(typed_formats){resource.enabled_features.shaderStorageImageReadWithoutFormat=features.shaderStorageImageReadWithoutFormat;
            resource.enabled_features.shaderStorageImageWriteWithoutFormat=features.shaderStorageImageWriteWithoutFormat;}
        if(geometry){check(features.geometryShader && features.depthClamp,"Requested geometry and depth clamp features available");
            resource.enabled_features.geometryShader=VK_TRUE;resource.enabled_features.depthClamp=VK_TRUE;}
        float priority=1;VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=resource.queue_family;q.queueCount=1;q.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&q;
        dc.pEnabledFeatures=&resource.enabled_features;
        const char* depth_extension=VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME;
        std::vector<const char*> enabled_extensions;
        if(typed_formats){uint32_t n=0;vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,nullptr),"Count storage format extensions");
            std::vector<VkExtensionProperties> extensions(n);vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,extensions.data()),"Read storage format extensions");
            for(const auto& e:extensions)if(!strcmp(e.extensionName,VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) && e.specVersion>=2)resource.format_features2=true;
            check(resource.format_features2,"Requested complete storage format properties available");enabled_extensions.push_back(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);}
        if(float_modes || preserve_float){uint32_t n=0;vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,nullptr),"Count float control extensions");
            std::vector<VkExtensionProperties> extensions(n);vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,extensions.data()),"Read float control extensions");
            for(const auto& e:extensions)if(!strcmp(e.extensionName,VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME))float_controls=true;
            check(float_controls,"Requested float control extension available");enabled_extensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties2.pNext=&float_properties;
            GetPhysicalDeviceProperties2(selected,&properties2);
            if(float_modes)check(float_properties.shaderDenormFlushToZeroFloat32,"Native DXBC float32 flush mode supported");
            check(float_properties.shaderSignedZeroInfNanPreserveFloat32 && float_properties.shaderRoundingModeRTEFloat32,
                "Native DXBC float32 NaN, infinity, signed zero and nearest rounding supported");}
        if(unrestricted_depth){
            uint32_t available=0;vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&available,nullptr),"Enumerate device extensions");
            std::vector<VkExtensionProperties> extensions(available);
            vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&available,extensions.data()),"Read device extensions");
            for(const auto& extension:extensions)if(!strcmp(extension.extensionName,depth_extension))resource.enabled_depth_range_unrestricted=true;
            check(resource.enabled_depth_range_unrestricted,"Requested unrestricted-depth test extension available");
            enabled_extensions.push_back(depth_extension);
        }
        VkPhysicalDeviceScalarBlockLayoutFeatures scalar={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
        if(scalar_block_layout){VkPhysicalDeviceFeatures2 available={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};available.pNext=&scalar;
            GetPhysicalDeviceFeatures2(selected,&available);check(scalar.scalarBlockLayout,"Requested scalar block layout feature available");
            enabled_extensions.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);dc.pNext=&scalar;scalar_layout=true;}
        VkPhysicalDeviceShaderDrawParametersFeatures draw={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        if(shader_draw_parameters){VkPhysicalDeviceFeatures2 available={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};available.pNext=&draw;
            GetPhysicalDeviceFeatures2(selected,&available);check(draw.shaderDrawParameters,"Requested shader draw parameters feature available");
            check(features.drawIndirectFirstInstance,"Requested indirect first-instance feature available");resource.enabled_features.drawIndirectFirstInstance=VK_TRUE;
            draw.pNext=const_cast<void*>(dc.pNext);dc.pNext=&draw;draw_parameters=true;}
        VkPhysicalDeviceVertexAttributeDivisorFeatures divisor={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
        if(divisors){uint32_t n=0;vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,nullptr),"Count divisor extensions");std::vector<VkExtensionProperties> extensions(n);
            vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,extensions.data()),"Read divisor extensions");
            check(std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return !strcmp(e.extensionName,VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);}),"Requested KHR vertex divisor extension available");
            VkPhysicalDeviceFeatures2 f={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&divisor;GetPhysicalDeviceFeatures2(selected,&f);
            check(divisor.vertexAttributeInstanceRateDivisor && divisor.vertexAttributeInstanceRateZeroDivisor,"Requested instance divisor features available");
            VkPhysicalDeviceVertexAttributeDivisorProperties p={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES};
            VkPhysicalDeviceProperties2 properties2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties2.pNext=&p;GetPhysicalDeviceProperties2(selected,&properties2);
            vertex_divisor=divisor.vertexAttributeInstanceRateDivisor;vertex_zero_divisor=divisor.vertexAttributeInstanceRateZeroDivisor;
            divisor_first_instance=p.supportsNonZeroFirstInstance;max_vertex_divisor=p.maxVertexAttribDivisor;
            check(divisor_first_instance,"Requested divisor nonzero first-instance support available");
            enabled_extensions.push_back(VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);divisor.pNext=const_cast<void*>(dc.pNext);dc.pNext=&divisor;}
        // Only nullDescriptor: D3D11 null views. Robust buffer and image access stay disabled.
        VkPhysicalDeviceRobustness2FeaturesEXT robust={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        if(null_descriptor){uint32_t n=0;vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,nullptr),"Count robustness extensions");std::vector<VkExtensionProperties> extensions(n);
            vkcheck(EnumerateDeviceExtensionProperties(selected,nullptr,&n,extensions.data()),"Read robustness extensions");
            check(std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return !strcmp(e.extensionName,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);}),"Requested robustness2 extension available");
            VkPhysicalDeviceFeatures2 f={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&robust;GetPhysicalDeviceFeatures2(selected,&f);
            check(robust.nullDescriptor,"Requested null descriptor feature available");robust.robustBufferAccess2=robust.robustImageAccess2=VK_FALSE;
            enabled_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);robust.pNext=const_cast<void*>(dc.pNext);dc.pNext=&robust;null_descriptors=true;}
        dc.enabledExtensionCount=uint32_t(enabled_extensions.size());dc.ppEnabledExtensionNames=enabled_extensions.data();
        vkcheck(CreateDevice(selected,&dc,nullptr,&resource.device),"Create test device");
#define GET(name) auto name=reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(resource.device,"vk" #name));check(name!=nullptr,"vk" #name);
        GET(DestroyDevice);destroy_device=DestroyDevice;GET(DeviceWaitIdle);wait_idle=DeviceWaitIdle;GET(GetDeviceQueue);GetDeviceQueue(resource.device,resource.queue_family,0,&queue);
#undef GET
#define X(name) resource.dispatch.name=reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(resource.device,"vk" #name));
        EFMI_RESOURCE_COMMAND_FUNCS(X)
#undef X
    }
};
template<class F> static void refuses(F f,const char* why){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,why);}
// A command of a Present, Constants or key list that cannot act does nothing, the list goes on and the
// generation says so (D-155; docs/architecture-review-2.1.md P0-4), as the original's: the Present
// succeeded, a text of what the layer did not do names `text`, and the scope submits its other work, or,
// when the command failed after recording part of its own, is refused and discarded. Either way the
// generation runs on: these assertions said "the Present refuses and the generation is dead" before.
// Scopes that went on after a declined command, and scopes dropped because it had recorded part of its work.
static unsigned declined_kept=0,declined_dropped=0;
template<class Ini,class Runtime> static void declined(Ini& ini,Runtime& resources,bool presented,const std::string& text,const char* why)
{
    check(presented,why);
    const auto gaps=ini.status().gaps;bool told=false;std::string all;
    for(const auto& gap:gaps){told=told || gap.find(text)!=std::string::npos;all+=gap+" | ";}
    if(!told)throw std::runtime_error(std::string(why)+": nothing the generation said names '"+text+"': "+all);
    ++checks;
    if(!resources.scope_active())return;
    if(resources.failed()){++declined_dropped;refuses([&]{resources.finish();},"A scope whose command failed after recording cannot submit");resources.discard();}
    else{++declined_kept;if(auto ticket=resources.finish())ticket->wait();}
}
static float value(const std::vector<uint8_t>& bytes,size_t offset=0){float v;check(offset+4<=bytes.size(),"Readback value in bounds");memcpy(&v,bytes.data()+offset,4);return v;}
#include "resource_image_cases.h"
static void low_level(Device& d){
    gpu::ResourceBudgets budgets;auto commands=std::make_unique<gpu::ResourceCommands>(d.resource,budgets);
    const uint32_t data[]={0x01020304,0x11223344,0xaabbccdd,0x55667788};
    auto a=commands->create(sizeof(data),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,data,sizeof(data));
    auto b=commands->create(sizeof(data),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true);
    commands->copy(a,4,b,0,8);commands->fill(b,8,8,0xdeadbeef);
    auto bytes=commands->readback(d.queue,b,0,sizeof(data));const uint32_t expected[]={data[1],data[2],0xdeadbeef,0xdeadbeef};
    check(bytes.size()==sizeof(expected) && !memcmp(bytes.data(),expected,sizeof(expected)),"GPU copy and fill ordering/readback bytes");
    commands->copy(a,0,a,12,4);bytes=commands->readback(d.queue,a,0,sizeof(data));
    check(!memcmp(bytes.data()+12,data,4),"Non-overlapping same-buffer copy");
    refuses([&]{commands->copy(a,0,a,4,8);},"Overlapping copies refused");
    refuses([&]{commands->copy(a,12,b,0,8);},"Source copy overflow refused");
    refuses([&]{commands->fill(a,1,4,1);},"Misaligned clear refused");
    gpu::ResourceCommands other(d.resource,budgets);refuses([&]{other.fill(a,0,4,1);},"Cross-domain resource refused");
    gpu::ResourceBudgets tiny;tiny.resident=std::make_shared<assets::AllocationBudget>(1);
    {gpu::ResourceCommands denied(d.resource,tiny);refuses([&]{denied.create(16,0);},"Budget failure before oversized allocation");}
    check(tiny.resident->used()==0,"Failed allocation leaves no budget charge");
    commands->fill(b,0,16,0x1234);std::weak_ptr<gpu::BufferResource> weak=b;auto ticket=commands->submit(d.queue);
    a.reset();b.reset();commands.reset();check(!weak.expired(),"Submitted command retains resources after executor retirement");
    ticket->wait();check(ticket->ready(),"Fence-backed completion");ticket.reset();check(weak.expired(),"Completed ticket releases last resource owner");
    check(budgets.resident->used()==0 && budgets.staging->used()==0,"All GPU allocation ledgers released");
}
static const char* fixture=R"ini(namespace = gpu
[Constants]
global $before = 0
global $copied = 0
global $alias = 0
global $description = 0
global $branch = 0
global $null_value = 77
global $bounds = 99
global $typed0 = 0
global $typed3 = 0
global $size = 0
global $stride = 0
global $slot = 2
global $phase = 0
global $fetch = 0
global $pool = 0
global $limit = 0
global $reset = 0
global $small = 0
global $cb = 0
global $srv_noop = 0
global $srv_cache = 0
global $uav_clear = 0
global $view_format = 0
global $view_region = 0
global $raw_float = 0
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = shader_resource unordered_access
[ResourceCopy]
bind_flags = shader_resource unordered_access
[ResourceAlias]
[ResourceSaved]
[ResourceDescription]
bind_flags = shader_resource unordered_access
[ResourceNull]
[ResourceTyped]
type = Buffer
format = R32G32B32A32_FLOAT
array = 2
bind_flags = shader_resource unordered_access
[ResourceFetched]
[ResourceFetched2]
[ResourceLimited]
max_copies_per_frame = 1
bind_flags = shader_resource unordered_access
[ResourceSmall]
type = StructuredBuffer
stride = 12
data = R32_FLOAT 1 2 3 4 5 6
bind_flags = shader_resource unordered_access
[ResourceCB]
type = Buffer
data = R32_FLOAT 10 20 30 40 50 60 70 80
bind_flags = constant_buffer
[PoolBuffers]
pool_size = 3
pool_element_type_switch_reset = false
[ResourceRegion]
[ResourceViewSource]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceViewResource]
[ResourceViewCache]
[ResourceUav]
[ResourceViewFormat]
format = R32_UINT
[ResourceViewRegion]
[ResourceRawTyped]
type = ByteAddressBuffer
format = R32G32_FLOAT
array = 2
bind_flags = shader_resource unordered_access
[Present]
if $phase == 0
 store = $before, ResourceSource, 5
 ResourceCopy = copy ResourceSource
 ResourceAlias = ref ResourceSource
 cs-u0 = ref ResourceAlias
 clear = cs-u0 0x40000000
 store = $alias, ResourceSource, 5
 store = $copied, ResourceCopy, 2
 if $copied == 3 && $alias == 2
  $branch = 1
 endif
 ResourceSaved = ref ResourceCopy
 ResourceCopy = unless_null ref ResourceNull
 ResourceDescription = copy_desc ResourceCopy
 clear = ResourceDescription 0x40800000
 store = $description, ResourceDescription, 3
 ResourceCopy = null
 store = $null_value, ResourceCopy, 0
 store = $bounds, ResourceSource, 99
 clear = ResourceTyped 1.25 2.5 3.75 4.5
 store = $typed0, ResourceTyped, 0
 store = $typed3, ResourceTyped, 7
 $size = ResourceSource->size
 $stride = ResourceSource->stride
 ps-t0 = ref ResourceSaved
 ps-t1 = ref ResourceDescription
 cs-t[$slot] = ref ResourceTyped
 ResourceFetched = ref ps-t0
 ResourceFetched2 = ref ps-t1
 store = $fetch, ResourceFetched, 7
 PoolBuffers[0] = ref ResourceFetched
 $PoolBuffers[1] = 9
 $PoolBuffers[0] = 5
 store = $pool, PoolBuffers[0], 6
 ResourceLimited = copy ResourceFetched
 ResourceLimited = copy ResourceDescription
 store = $limit, ResourceLimited, 0
 reset_per_frame_limits = ResourceLimited
 ResourceLimited = copy ResourceDescription
 store = $reset, ResourceLimited, 0
 store = $small, ResourceSmall, 1
 cs-cb0 = ref ResourceCB->Region(16, 16)
 store = $cb, cs-cb0, 0
 ResourceRegion = ref ResourceTyped->Region(16, 32)
 clear = ResourceRegion 6.25
 cs-t3 = ref ResourceRegion
 $PoolBuffers[*] = null
 cs-t4 = ref PoolBuffers[0]
 PoolBuffers[*] = null
 cs-t5 = ref PoolBuffers[0]
 ps-t6 = ref ResourceViewSource
 ResourceViewResource = ref ps-t6
 clear = ResourceViewResource 5
 store = $srv_noop, ResourceViewSource, 0
 ResourceViewCache = ref ResourceViewResource
 ResourceViewCache = ref ResourceViewSource
 clear = ResourceViewCache 6
 store = $srv_cache, ResourceViewSource, 0
 cs-u6 = ref ResourceViewResource
 ResourceUav = ref cs-u6
 clear = ResourceUav 7
 store = $uav_clear, ResourceViewSource, 2
 ResourceViewFormat = ref ResourceUav
 clear = ResourceViewFormat 8
 store = $view_format, ResourceViewSource, 2
 ResourceViewRegion = ref cs-u6->Region(0,8)
 clear = ResourceViewRegion 9
 store = $view_region, ResourceViewSource, 3
 clear = ResourceRawTyped 11.5 12.5
 store = $raw_float, ResourceRawTyped, 3
endif
$phase = $phase + 1
)ini";
static void write(const std::filesystem::path& path,const char* text){std::ofstream out(path,std::ios::binary);out<<text;check(bool(out),"Write isolated test INI");}
static void ini_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/resource.ini",fixture);
    RuntimeConfig config{root,{},root/"settings","resources",root/"Mods"};
    gpu::ResourceBudgets budgets;auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);std::string why;
    auto ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error("Load original INI: "+why);++checks;
    refuses([&]{backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::InsideRenderPass);},"Active render-pass scope refused");
    {
        auto unscoped=std::make_shared<GpuResourceRuntime>(d.resource);auto probe=IniRuntime::load(config,unscoped,why);
        if(!probe)throw std::runtime_error("Load original INI without a scope: "+why);
        declined(*probe,*unscoped,probe->present({},1,0,why),"scope","Without a host scope the GPU commands do nothing and the Present goes on");
    }
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error("Original GPU commands: "+why);++checks;
    auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    const auto values=ini->status().variables;
    const std::pair<const char*,float> expected[]={{"before",6},{"copied",3},{"alias",2},{"description",4},{"branch",1},{"null_value",77},
        {"bounds",0},{"typed0",1.25f},{"typed3",4.5f},{"size",32},{"stride",8},{"phase",1},
        {"fetch",8},{"pool",7},{"limit",1},{"reset",4},{"small",2},{"cb",10},
        {"srv_noop",1},{"srv_cache",1},{"uav_clear",7},{"view_format",8},{"view_region",9},{"raw_float",12.5f}};
    for(auto pair:expected){const auto name=std::string("$\\gpu\\")+pair.first;const auto it=values.find(name);
        if(it==values.end() || it->second!=pair.second){for(const auto& p:values)printf("VARIABLE %s %.9g\n",p.first.c_str(),p.second);throw std::runtime_error("GPU original variable mismatch: "+name);}++checks;}
    auto saved=backend->binding(L'p',T::SHADER_RESOURCE,0);auto desc=backend->binding(L'p',T::SHADER_RESOURCE,1);
    auto typed=backend->binding(L'c',T::SHADER_RESOURCE,2);
    check(saved.buffer && desc.buffer && typed.buffer,"Optimized batch and dynamic stage bindings retain owned resources");
    auto region=backend->binding(L'c',T::SHADER_RESOURCE,3);
    check(region.buffer==typed.buffer && region.offset==16 && region.visible_size==0 && region.view_offset==16 && region.view_size==16,
          "Original view helper interprets its size metadata as an end boundary");
    auto cb=backend->binding(L'c',T::CONSTANT_BUFFER,0);check(cb.offset==16 && cb.visible_size==16,"Constant-buffer reference region retained");
    check(backend->binding(L'c',T::SHADER_RESOURCE,4).buffer==saved.buffer && !backend->binding(L'c',T::SHADER_RESOURCE,5).buffer,
          "Mixed pool variable reset preserves GPU owner; resource reset unbinds");
    auto region_bytes=backend->commands().readback(d.queue,typed.buffer,0,32);
    check(value(region_bytes)==1.25f && value(region_bytes,16)==6.25f && value(region_bytes,28)==6.25f,"GPU clear affects only referenced region");
    auto read=backend->commands().readback(d.queue,saved.buffer,0,32);check(value(read,8)==3,"Copy survives source clear and destination unbind");
    read=backend->commands().readback(d.queue,desc.buffer,0,32);for(size_t i=0;i<32;i+=4)check(value(read,i)==4,"Description-copy resource was cleared on GPU");
    auto next_backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto next=IniRuntime::load(config,next_backend,why);
    check(bool(next),"Second original INI generation loads");next_backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!next->present({},2,0,why))throw std::runtime_error(why);auto next_ticket=next_backend->finish();if(next_ticket)next_ticket->wait();next_ticket.reset();
    check(next_backend->binding(L'p',T::SHADER_RESOURCE,0).buffer!=saved.buffer,"Reload isolates mutable GPU resources");
    write(root/"Mods/resource.ini","[Present]\nthis_is_not_an_original_command = 1\n");
    {
        // The original loader reports the unknown line and loads the rest (D-067).
        auto warned=IniRuntime::load(config,std::make_shared<GpuResourceRuntime>(d.resource,budgets),why);
        check(warned && warned->status().diagnostics>=1,"An unknown command line is reported and skipped, as by the original loader");
    }
    // A section the generation cannot execute: a CustomShader without shader execution. (An acting
    // ShaderOverride loads inert since D-089.)
    write(root/"Mods/resource.ini","[CustomShaderUnadapted]\ncs = absent.hlsl\n");
    {
        // As a CustomShader the original could not compile: declared, its runs doing nothing, the rest of
        // the generation loaded, and said (it refused the whole candidate before; P0-6).
        auto inert=IniRuntime::load(config,std::make_shared<GpuResourceRuntime>(d.resource,budgets),why);
        bool told=false;
        if(inert)for(const auto& gap:inert->status().gaps)told=told || gap.find("CustomShader")!=std::string::npos;
        check(inert && told,"A CustomShader without shader execution loads inert and is said, beside a working generation");
    }
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);check(ini->present({},3,0,why),"Working generation survives bad candidate");
    ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    next.reset();next_backend.reset();ini.reset();backend.reset();check(saved.buffer->buffer()!=VK_NULL_HANDLE,"Retained binding survives both runtime retirements");
    saved={};desc={};typed={};region={};cb={};check(budgets.resident->used()==0 && budgets.staging->used()==0,"Reload and retained bindings release shared GPU budgets");
}
static void file_and_failures(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    const float bytes[]={9,8,7,6};{std::ofstream out(root/"Mods/file.buf",std::ios::binary);out.write(reinterpret_cast<const char*>(bytes),sizeof(bytes));}
    const std::string base=R"ini(namespace = files
[Constants]
global $result = 0
global $padding = 1
global $size = 0
global $alias_size = 0
[ResourceSource]
type = Buffer
format = R32_FLOAT
filename = file.buf
byte_width = 32
bind_flags = shader_resource unordered_access
[ResourceOut]
bind_flags = shader_resource unordered_access
[Present]
)ini";
    RuntimeConfig config{root,{},root/"settings","files",root/"Mods"};std::string why;
    write(root/"Mods/files.ini",(base+"store = $result, ResourceSource, 2\nstore = $padding, ResourceSource, 7\n$size = ResourceSource->size\nResourceOut = ref ResourceSource\n$alias_size = ResourceOut->size\ncs-t0 = ref ResourceSource\n").c_str());
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource);auto ini=IniRuntime::load(config,backend,why);
    check(bool(ini),"File-backed original resource declaration");backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();
    check(ini->status().variables.at("$\\files\\result")==7 && ini->status().variables.at("$\\files\\padding")==0,"File bytes and enlarged zero padding reach GPU store");
    check(ini->status().variables.at("$\\files\\size")==16,"Original file size metadata remains distinct from enlarged allocation");
    check(ini->status().variables.at("$\\files\\alias_size")==32,"Plain reference uses original destination-size metadata, not source file size");
    auto held=backend->binding(L'c',T::SHADER_RESOURCE,0);auto output=backend->commands().readback(d.queue,held.buffer,0,32);
    check(!memcmp(output.data(),bytes,sizeof(bytes)),"Original file bytes round trip exactly");
    struct Failure{const char* command;const char* reason;};
    const Failure bad[]={
        {"clear = ResourceSource 2\ndrawindexed = 3,0,0\n","shader execution"},
    };
    {   // D-153: a copy of a region past the buffer (undefined in D3D11) copies nothing; the copy is made, reads
        // zero, and the list goes on.
        write(root/"Mods/files.ini",(base+"ResourceOut = copy ResourceSource->Region(40, 16)\nstore = $result, ResourceOut, 0\n"
                                     "$size = ResourceOut->size\n").c_str());
        auto copier=std::make_shared<GpuResourceRuntime>(d.resource);auto candidate=IniRuntime::load(config,copier,why);
        check(bool(candidate),"Outside region fixture loads");copier->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        check(candidate->present({},2,0,why) && candidate->status().variables.at("$\\files\\result")==0,
              "A copy of a region past its buffer copies nothing and the list goes on");
        auto done=copier->finish();if(done)done->wait();candidate.reset();copier.reset();
    }
    // D-151: a raw view of a buffer without the raw flag, and a slot reference of a region outside the buffer,
    // are views D3D11 does not make: nothing is bound and the list goes on.
    for(const char* command:{"cs-u0 = raw ref ResourceSource\n","cs-u0 = ref ResourceSource->Region(40, 16)\n"}){
        write(root/"Mods/files.ini",(base+command).c_str());
        auto unviewed=std::make_shared<GpuResourceRuntime>(d.resource);auto candidate=IniRuntime::load(config,unviewed,why);
        check(bool(candidate),"Raw view fixture loads");unviewed->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        check(candidate->present({},2,0,why) && !unviewed->binding(L'c',T::UNORDERED_ACCESS_VIEW,0).buffer,
              "A view D3D11 does not make binds nothing and the list goes on");
        auto done=unviewed->finish();if(done)done->wait();candidate.reset();unviewed.reset();
    }
    for(const auto& item:bad){
        write(root/"Mods/files.ini",(base+item.command).c_str());gpu::ResourceBudgets budgets;
        auto rejected=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto candidate=IniRuntime::load(config,rejected,why);
        if(!candidate)throw std::runtime_error("Negative fixture did not reach execution: "+why);
        rejected->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*candidate,*rejected,candidate->present({},2,0,why),item.reason,"An unsupported GPU command does nothing and the Present goes on");
        // The failure was the command's: the generation's next scope runs.
        rejected->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*candidate,*rejected,candidate->present({},3,0,why),item.reason,"The generation's next scope runs after a declined command");
        candidate.reset();rejected.reset();check(budgets.resident->used()==0 && budgets.staging->used()==0,"Failure path releases unsubmitted resources");
    }
    output=backend->commands().readback(d.queue,held.buffer,0,32);
    check(!memcmp(output.data(),bytes,sizeof(bytes)),"Rejected generations leave old GPU resource contents intact");
}
static void range_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    const std::string source=R"ini(namespace = ranges
[Constants]
global $first = 2
global $last = 4
global $cleared = 0
global $tail = 0
global $copied = 0
global $counter = 0
global $unless = 0
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = shader_resource unordered_access
[ResourceOther]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[ResourceCB]
type = Buffer
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = constant_buffer
[PoolCB]
pool_size = 4
bind_flags = constant_buffer
[PoolRows]
pool_size = 3
[PoolViews]
pool_size = 3
[PoolOut]
pool_size = 3
max_copies_per_frame = 1
[PoolCopies]
pool_size = 3
bind_flags = shader_resource unordered_access
[Present]
PoolRows[0] = ref ResourceSource
PoolRows[1] = ref ResourceOther
PoolRows[2] = null
cs-t[$first:$last] = ref PoolRows[0:2]
cs-u[$first:$last] = ref PoolRows[0:2]
clear = cs-u2 0x40000000
store = $cleared, ResourceSource, 0
store = $tail, ResourceSource, 2
cs-u[$first:$last] = null
cs-t[$first:$last] = ref PoolRows[0:2]
ps-t6 = ref ResourceSource
PoolViews[0] = ref ps-t6
ps-t[10:11] = ref PoolViews[0]
PoolOut[0:1] = ref ps-t[10:11]
PoolOut[0] = ref ResourceOther
store = $counter, PoolOut[0], 0
PoolOut[2] = ref ResourceOther
ps-t11 = null
PoolOut[1:2] = unless_null ref ps-t[10:11]
store = $unless, PoolOut[2], 0
ps-t[10:11] = unless_null null
ps-t[20:21] = no_view_cache ref PoolViews[0]
cs-t[30:31] = ref PoolRows[-1:0]
cs-u[40:41] = copy PoolRows[0:1]
clear = cs-u40 0x40400000
PoolCopies[0:1] = copy cs-u[40:41]
store = $copied, PoolCopies[0], 7
vs-cb0 = ref ResourceCB->Region(16, 16)
vs-cb[0:1] = unless_null null
PoolCB[0:0] = ref vs-cb[0:0]
vs-cb[2:3] = ref PoolCB[0]
PoolCB[1:2] = ref vs-cb[0:1]
vs-cb4 = ref PoolCB[1]->Region(16, 16)
)ini";
    write(root/"Mods/ranges.ini",source.c_str());RuntimeConfig config{root,{},root/"settings","ranges",root/"Mods"};
    gpu::ResourceBudgets budgets;auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);std::string why;
    auto ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);++checks;
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    auto vars=ini->status().variables;
    for(const auto& expected:std::vector<std::pair<std::string,float>>{{"cleared",2},{"tail",3},{"copied",3},{"counter",9},{"unless",9}})
        check(vars.at("$\\ranges\\"+expected.first)==expected.second,"Original range execution variable result");
    auto a=backend->binding(L'c',T::SHADER_RESOURCE,2),b=backend->binding(L'c',T::SHADER_RESOURCE,3),null=backend->binding(L'c',T::SHADER_RESOURCE,4);
    check(a.buffer && b.buffer && a.buffer!=b.buffer && !null.buffer,"Dynamic range maps pool slots and explicit null");
    check(a.view_size==8 && b.view_size==8,"Original plain range fresh structured views expose one element");
    auto full=backend->binding(L'p',T::SHADER_RESOURCE,10),other=backend->binding(L'p',T::SHADER_RESOURCE,11);
    check(full.buffer==a.buffer && full.view_size==32 && !other.buffer,"Range broadcast preserves existing source view and unless-null slots");
    check(backend->binding(L'p',T::SHADER_RESOURCE,20).view_size==32,"No-view-cache retains borrowed source view");
    // The pinned engine computes the remainder with size_t pool_size. For a
    // three-element pool, -1 converts to SIZE_MAX and selects element zero.
    check(backend->binding(L'c',T::SHADER_RESOURCE,30).buffer==a.buffer && backend->binding(L'c',T::SHADER_RESOURCE,31).buffer==a.buffer,
          "Negative non-power-of-two range preserves original unsigned remainder");
    auto copy=backend->binding(L'c',T::UNORDERED_ACCESS_VIEW,40);
    auto bytes=backend->commands().readback(d.queue,a.buffer,0,32);check(value(bytes)==2 && value(bytes,28)==8,"Range copy clear leaves source bytes intact");
    check(copy.buffer!=a.buffer && copy.view_size==32,"Range copy uses full original single-slot copy semantics");
    auto cb0=backend->binding(L'v',T::CONSTANT_BUFFER,0),cb2=backend->binding(L'v',T::CONSTANT_BUFFER,2),cb4=backend->binding(L'v',T::CONSTANT_BUFFER,4);
    check(cb0.buffer && cb0.offset==16 && cb0.visible_size==16 && cb0.region && !backend->binding(L'v',T::CONSTANT_BUFFER,1).buffer,
          "Unless-null range preserves the complete constant-buffer region binding");
    check(cb2.buffer==cb0.buffer && cb2.offset==0 && !cb2.visible_size && !cb2.region,
          "Plain constant-buffer range broadcast binds the whole resource");
    check(cb4.buffer==cb0.buffer && cb4.offset==16 && cb4.visible_size==16 && cb4.region,
          "Constant-buffer range fetch preserves allocation and region metadata");
    ini.reset();backend.reset();a={};b={};null={};full={};other={};copy={};cb0={};cb2={};cb4={};
    check(budgets.resident->used()==0 && budgets.staging->used()==0,"Range caches release with their generation");
    const std::vector<std::pair<std::string,std::string>> bad={
        {"cs-t[0:1] = ref PoolRows[0:2]\n","ranges differ"},
        {"cs-t[127:129] = ref ResourceSource\n","range is invalid"},
        {"$first = 1 / 0\ncs-t[$first:$first] = ref ResourceSource\n","outside signed integer"},
    };
    const auto declarations=source.substr(0,source.find("[Present]"))+"[Present]\n";
    for(const auto& failure:bad){write(root/"Mods/ranges.ini",(declarations+failure.first).c_str());
        backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);ini=IniRuntime::load(config,backend,why);
        if(!ini)throw std::runtime_error("Range fixture must parse: "+why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*ini,*backend,ini->present({},2,0,why),failure.second,"An invalid range binds nothing and the Present goes on");
        ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Invalid range leaves no allocations");
    }
}
static void copy_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    write(root/"Mods/copies.ini",R"ini(namespace = copies
[Constants]
global $source = 0
global $destination = 0
global $cached = 0
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = shader_resource unordered_access
[ResourceOther]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 9 10 11 12 13 14 15 16
bind_flags = shader_resource unordered_access
[ResourceNarrow]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[ResourceDestination]
bind_flags = shader_resource unordered_access
[ResourceSaved]
[Present]
ResourceDestination = ref ResourceSource
ResourceDestination = copy ResourceOther
clear = ResourceDestination 0x41a00000
store = $source, ResourceSource, 0
store = $destination, ResourceDestination, 0
ResourceSaved = ref ResourceDestination
ResourceDestination = copy ResourceNarrow
ResourceDestination = copy_desc ResourceSource
store = $cached, ResourceDestination, 0
ps-t0 = ref ResourceSource
ps-t1 = ref ResourceDestination
ps-t2 = ref ResourceSaved
)ini");
    RuntimeConfig config{root,{},root/"settings","copies",root/"Mods"};gpu::ResourceBudgets budgets;std::string why;
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    const auto vars=ini->status().variables;
    check(vars.at("$\\copies\\source")==1,"Copy destination cannot overwrite a referenced source allocation");
    check(vars.at("$\\copies\\destination")==20 && vars.at("$\\copies\\cached")==20,"Description cache retains owned contents across size changes");
    auto source=backend->binding(L'p',T::SHADER_RESOURCE,0),destination=backend->binding(L'p',T::SHADER_RESOURCE,1),saved=backend->binding(L'p',T::SHADER_RESOURCE,2);
    check(destination.buffer!=source.buffer && destination.buffer==saved.buffer,"Copy description pool restores the original owned allocation");
    ini.reset();backend.reset();source={};destination={};saved={};check(!budgets.resident->used() && !budgets.staging->used(),"All copy-pool descriptions release with the generation");
}
static void whole_pool_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    const std::string source=R"ini(namespace = wholepool
[Constants]
global $phase = 0
global $copied = 0
global $source = 0
global $limit0 = 0
global $limit1 = 0
global $limit2 = 0
global $mixed = 0
global $selector = 0
global $source_stride = 0
global $unless = 0
global $null = 77
[ResourceSource]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceOther]
type = Buffer
format = R32_FLOAT
stride = 8
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[ResourceLarge]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 21 22 23 24 25 26 27 28
bind_flags = shader_resource unordered_access
[ResourceNull]
[PoolCopies]
pool_size = 3
bind_flags = shader_resource unordered_access
[PoolProxy]
pool_size = 1
bind_flags = shader_resource unordered_access
[PoolRefs]
pool_size = 3
[PoolLimited]
pool_size = 3
max_copies_per_frame = 1
bind_flags = shader_resource unordered_access
[PoolMixed]
pool_size = 3
pool_element_type_switch_reset = false
pool_variable_default_value = 5
bind_flags = shader_resource unordered_access
[PoolDynamic]
pool_size = 3
pool_variable_default_value = 1
[PoolWindows]
pool_size = 2
[PoolAgain]
pool_size = 2
bind_flags = shader_resource unordered_access
[PoolNull]
pool_size = 2
[PoolInvalid]
pool_size = 2
byte_width = 8
bind_flags = shader_resource unordered_access
[CommandListRepeat]
PoolAgain[*] = copy ResourceSource
[Present]
if $phase == 0
 PoolCopies = ResourceSource
 store = $copied, PoolCopies[2], 3
 clear = PoolCopies[0] 20
 store = $source, ResourceSource, 0
 ps-t0 = ref PoolCopies[0]
 ps-t1 = ref PoolCopies[1]
 ps-t2 = ref PoolCopies[2]
 PoolProxy = ref PoolCopies
 PoolProxy[*] = copy ResourceOther
 ps-t6 = ref PoolCopies[0]
 ps-t7 = ref PoolCopies[1]
 ps-t8 = ref PoolCopies[2]
 PoolCopies = copy ResourceLarge
 PoolCopies[*] = copy_desc ResourceSource
 ps-t3 = ref PoolCopies[0]
 ps-t4 = ref PoolCopies[1]
 ps-t5 = ref PoolCopies[2]
 PoolRefs[*] = ref ResourceSource
 ps-t10 = ref PoolRefs[0]
 ps-t11 = ref PoolRefs[1]
 ps-t12 = ref PoolRefs[2]
 PoolLimited[1] = ref ResourceOther
 PoolLimited = copy ResourceSource
 store = $limit0, PoolLimited[0], 0
 store = $limit1, PoolLimited[1], 0
 store = $limit2, PoolLimited[2], 0
 $PoolMixed[*] = 7
 PoolMixed[*] = copy ResourceSource
 $mixed = $PoolMixed[1]
 ps-t15 = ref PoolMixed[1]
 $PoolMixed[*] = null
 ps-t16 = ref PoolMixed[1]
 PoolMixed[*] = null
 ps-t17 = ref PoolMixed[1]
 PoolDynamic[1] = ref ResourceSource
 PoolDynamic[2] = ref ResourceOther
 $PoolDynamic[0] = 0
 PoolDynamic = ref PoolDynamic[$PoolDynamic[0] + 1]
 $selector = $PoolDynamic[0]
 $source_stride = PoolDynamic[0]->sourcestride
 ps-t20 = ref PoolDynamic[0]
 ps-t21 = ref PoolDynamic[1]
 ps-t22 = ref PoolDynamic[2]
 PoolWindows[*] = ref ResourceSource->Region(4, 12)
 ps-t25 = ref PoolWindows[0]
 ps-t26 = ref PoolWindows[1]
 PoolNull = ref ResourceSource
 PoolNull = unless_null ref ResourceNull
 store = $unless, PoolNull[1], 0
 PoolNull[*] = ref ResourceNull
 store = $null, PoolNull[1], 0
 ps-t27 = ref PoolNull[0]
 run = CommandListRepeat
 clear = PoolAgain[0] 30
 clear = PoolAgain[1] 40
else
 run = CommandListRepeat
 PoolLimited[*] = copy ResourceSource
 ps-t30 = ref PoolLimited[0]
 ps-t31 = ref PoolLimited[1]
 ps-t32 = ref PoolLimited[2]
endif
ps-t28 = ref PoolAgain[0]
ps-t29 = ref PoolAgain[1]
$phase = $phase + 1
)ini";
    write(root/"Mods/pools.ini",source.c_str());RuntimeConfig config{root,{},root/"settings","pools",root/"Mods"};
    gpu::ResourceBudgets budgets;std::string why;auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    auto ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);++checks;
    auto present=[&](unsigned frame){backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present({},frame,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();++checks;};
    auto binding=[&](unsigned slot){return backend->binding(L'p',T::SHADER_RESOURCE,slot);};
    auto first=[&](unsigned slot){auto b=binding(slot);check(bool(b.buffer),"Whole-pool slot retains its resource");return value(backend->commands().readback(d.queue,b.buffer,0,4));};
    present(1);const auto vars=ini->status().variables;
    const std::pair<const char*,float> expected[]={{"copied",4},{"source",1},{"limit0",1},{"limit1",9},{"limit2",1},
        {"mixed",7},{"selector",1},{"source_stride",8},{"unless",1},{"null",77}};
    for(const auto& e:expected){const auto actual=vars.at("$\\wholepool\\"+std::string(e.first));
        if(actual!=e.second)throw std::runtime_error(std::string(e.first)+" expected "+std::to_string(e.second)+", actual "+std::to_string(actual));++checks;}
    for(unsigned i=0;i<3;++i){check(binding(i).buffer==binding(i+3).buffer,"Whole-pool copy_desc restores each owned description");
        check(first(i)==(i?1:20),"Whole-pool description cache restores previously initialized bytes");
        check(first(i+6)==9 && binding(i+6).buffer!=binding(i).buffer,"Proxy copies use a separate allocation for a different stride description");
        check(binding(10+i).buffer==binding(10).buffer,"Whole-pool ref shares the same source owner");
        check(binding(20+i).buffer==binding(10).buffer,"Dynamic source resource is resolved before pool element mutations");
        if(i)check(binding(i).buffer!=binding(0).buffer,"Whole-pool copies have independent destination owners");}
    check(binding(15).buffer==binding(16).buffer && !binding(17).buffer,"Mixed scalar reset keeps resources; resource reset retains existing slots only");
    for(unsigned i:{25u,26u})check(binding(i).buffer==binding(10).buffer && binding(i).view_offset==4 && binding(i).view_size==8,
                                 "Whole-pool references preserve original view region end boundaries");
    check(!binding(27).buffer && first(10)==1,"Resolved null unbinds every element and leaves original bytes intact");
    check(first(28)==30 && first(29)==40,"Separate pooled destinations can be cleared independently");
    present(2);check(first(28)==1 && first(29)==1,"The same whole-pool instruction works again without a mutated target");
    for(unsigned i=30;i<33;++i)check(first(i)==1,"Whole-pool copy counters reset independently on the next frame");
    auto held=binding(3).buffer;ini.reset();backend.reset();check(budgets.resident->used()>0,"A slot owner survives whole-pool generation retirement");
    held.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Whole-pool aliases and cached descriptions release all allocations");
    const auto rejected=source.substr(0,source.find("[CommandListRepeat]"))+"[Present]\nPoolInvalid = copy ResourceSource\n";
    write(root/"Mods/pools.ini",rejected.c_str());backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    declined(*ini,*backend,ini->present({},3,0,why),"incompatible allocation sizes","An invalid whole-pool copy description does nothing and is said");
    ini.reset();backend.reset();
    check(!budgets.resident->used() && !budgets.staging->used(),"Rejected whole-pool generation releases its source and staging");
}
static void buffer_options_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/options.ini",R"ini(namespace = options
[Constants]
global $resolved = 0
global $copy_resolved = 0
global $restored = 0
global $source = 0
global $mono = 0
global $pool = 0
global $range = 0
[ResourceSource]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceOther]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[ResourceLarge]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 21 22 23 24 25 26 27 28
bind_flags = shader_resource unordered_access
[ResourceOut]
bind_flags = shader_resource unordered_access
[PoolOptions]
pool_size = 2
bind_flags = shader_resource unordered_access
[CommandListRange]
cs-u[4:5] = resolve_msaa ResourceOther
[Present]
ResourceOut = copy ResourceOther
ps-t0 = ref ResourceOut
ResourceOut = resolve_msaa ResourceSource
store = $resolved, ResourceOut, 0
ResourceOut = copy resolve_msaa ResourceSource
store = $copy_resolved, ResourceOut, 3
ResourceOut = copy_desc resolve_msaa ResourceLarge
clear = ResourceOut 50
ResourceOut = resolve_msaa ResourceSource
store = $restored, ResourceOut, 1
ps-t1 = ref ResourceOut
cs-u0 = mono set_viewport ref ResourceSource
clear = cs-u0 11
store = $source, ResourceSource, 3
ResourceOut = mono set_viewport copy ResourceOther
store = $mono, ResourceOut, 2
PoolOptions = copy ResourceOther
PoolOptions[*] = resolve_msaa ResourceSource
store = $pool, PoolOptions[1], 0
cs-u[1:2] = mono set_viewport copy ResourceOther
clear = cs-u1 30
run = CommandListRange
clear = cs-u4 60
clear = cs-u5 70
run = CommandListRange
store = $range, cs-u4, 1
)ini");
    RuntimeConfig config{root,{},root/"settings","options",root/"Mods"};gpu::ResourceBudgets budgets;std::string why;
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    const auto vars=ini->status().variables;
    const std::pair<const char*,float> expected[]={{"resolved",9},{"copy_resolved",12},{"restored",10},{"source",11},{"mono",11},{"pool",9},{"range",60}};
    for(const auto& e:expected){const auto actual=vars.at("$\\options\\"+std::string(e.first));
        if(actual!=e.second)throw std::runtime_error(std::string(e.first)+" expected "+std::to_string(e.second)+", actual "+std::to_string(actual));++checks;}
    check(backend->binding(L'p',T::SHADER_RESOURCE,0).buffer==backend->binding(L'p',T::SHADER_RESOURCE,1).buffer,
          "Buffer resolve reuses the owned description without copying source bytes");
    for(const auto& e:std::vector<std::pair<unsigned,float>>{{0,11},{1,30},{2,9},{4,60},{5,70}}){
        auto b=backend->binding(L'c',T::UNORDERED_ACCESS_VIEW,e.first);check(bool(b.buffer),"Option-bearing commands bind real owned buffers");
        check(value(backend->commands().readback(d.queue,b.buffer,0,4))==e.second,"Scalar and range options preserve source-defined byte effects");}
    ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Buffer option caches retire with their generation");
}
static void keyed_pools_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/keyed.ini",R"ini(namespace = keyed
[Constants]
global $phase = 0
global $missing = 0
global $evicted = 0
global $allocated = 0
global $mixed = 0
global $default = 0
global $default_index = 0
global $frame = 0
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceOther]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[PoolKeyed]
pool_size = 2
pool_index_type = fifo
bind_flags = shader_resource unordered_access
[PoolAlloc]
pool_size = 2
pool_index_type = fifo
pool_allocate_slot_on_missing = true
[PoolMixed]
pool_size = 2
pool_index_type = fifo
pool_element_type_switch_reset = false
[PoolDefault]
pool_size = 2
pool_index_type = fifo
type = StructuredBuffer
stride = 8
data = R32_FLOAT 41 42 43 44
bind_flags = shader_resource unordered_access
[PoolExpire]
pool_size = 2
pool_expiration_timeout_frames = 1
[PoolRefresh]
pool_size = 2
pool_expiration_timeout_frames = 1
pool_expiration_refresh_on_read = true
[PoolKeep]
pool_size = 2
pool_expiration_timeout_frames = 1
pool_expiration_reset_elements = false
[PoolExpireKey]
pool_size = 2
pool_index_type = fifo
pool_expiration_timeout_frames = 1
[PoolFromSlots]
pool_size = 2
pool_index_type = fifo
bind_flags = shader_resource unordered_access
[Present]
$frame = frame_time
if $phase == 0
 PoolKeyed[11] = copy ResourceSource
 ps-t0 = ref PoolKeyed[11]
 PoolKeyed[22] = ref ResourceOther
 ps-t1 = ref PoolKeyed[22]
 $missing = #PoolKeyed[99]
 ps-t2 = ref PoolKeyed[99]
 PoolKeyed[33] = copy ResourceOther
 $evicted = #PoolKeyed[11]
 ps-t3 = ref PoolKeyed[33]
 PoolAlloc[11] = ref ResourceSource
 PoolAlloc[22] = ref ResourceOther
 $allocated = #PoolAlloc[33]
 ps-t4 = ref PoolAlloc[33]
 PoolMixed[11] = ref ResourceSource
 $PoolMixed[11] = 7
 PoolMixed[22] = ref ResourceOther
 PoolMixed[33] = ref ResourceOther
 $mixed = $PoolMixed[33]
 store = $default, PoolDefault[44], 0
 $default_index = #PoolDefault[44]
 PoolExpire[0] = ref ResourceSource
 PoolRefresh[0] = ref ResourceOther
 PoolKeep[0] = ref ResourceSource
 PoolExpireKey[11] = ref ResourceSource
 ps-t10 = ref PoolExpire[0]
 ps-t11 = ref PoolRefresh[0]
 ps-t12 = ref PoolKeep[0]
 ps-t13 = ref PoolExpireKey[11]
 cs-t50 = ref ResourceSource
 cs-t51 = ref ResourceOther
 PoolFromSlots[0:1] = ref cs-t[50:51]
 ps-t[52:53] = ref PoolFromSlots[0:1]
 PoolFromSlots[*] = copy ResourceSource
 ps-t[54:55] = ref PoolFromSlots[0:1]
else if $phase == 1
 ps-t20 = ref PoolExpire[0]
 ps-t21 = ref PoolRefresh[0]
 ps-t22 = ref PoolKeep[0]
else if $phase == 2
 ps-t30 = ref PoolExpire[0]
 ps-t31 = ref PoolRefresh[0]
 ps-t32 = ref PoolKeep[0]
 ps-t33 = ref PoolExpireKey[11]
 PoolExpireKey[22] = ref ResourceOther
 ps-t34 = ref PoolExpireKey[22]
else
 ps-t41 = ref PoolRefresh[0]
endif
$phase = $phase + 1
)ini");
    RuntimeConfig config{root,{},root/"settings","keyed",root/"Mods"};gpu::ResourceBudgets budgets;std::string why;
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);++checks;RuntimeInput input;input.frame_seconds=1.0f/120;
    auto present=[&](unsigned frame){backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present(input,frame,float(frame)/120,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();++checks;};
    auto binding=[&](unsigned slot){return backend->binding(L'p',T::SHADER_RESOURCE,slot);};
    auto first=[&](unsigned slot){auto b=binding(slot);check(bool(b.buffer),"Keyed pool retains a readable binding");return value(backend->commands().readback(d.queue,b.buffer,0,4));};
    present(1);const auto vars=ini->status().variables;
    const std::pair<const char*,float> expected[]={{"missing",-1},{"evicted",-1},{"allocated",0},{"mixed",7},{"default",41},{"default_index",-1},{"frame",input.frame_seconds}};
    for(const auto& e:expected){const auto actual=vars.at("$\\keyed\\"+std::string(e.first));
        if(actual!=e.second)throw std::runtime_error(std::string(e.first)+" expected "+std::to_string(e.second)+", actual "+std::to_string(actual));++checks;}
    check(!binding(2).buffer,"Missing keyed lookup does not allocate or bind an unrelated slot");
    check(binding(0).buffer==binding(3).buffer && first(0)==9,"FIFO replacement reuses owned mutable copy storage retained by old bindings");
    check(first(4)==1,"Read allocation preserves the original slot's resource instead of clearing it");
    check(first(52)==1 && first(53)==9 && first(54)==1 && first(55)==1,"Ranges and whole-pool writes address physical slots of a keyed pool");
    check(binding(52).buffer!=binding(54).buffer && binding(54).buffer!=binding(55).buffer,"Keyed pool references and per-slot copy owners stay distinct");
    present(2);check(first(20)==1 && first(21)==9 && first(22)==1,"Expiry threshold is strictly greater than the configured frame timeout");
    present(3);check(!binding(30).buffer && !binding(33).buffer,"Expired direct resources clear and expired FIFO keys detach");
    check(first(31)==9 && first(32)==1 && first(34)==9,"Read refresh, no-reset expiry and reassignment retain their independent semantics");
    check(first(10)==1 && first(13)==1,"Expired pool entries do not destroy resources still bound elsewhere");
    present(5);check(!binding(41).buffer && first(11)==9,"Read-refreshed pool expires after a gap without invalidating earlier bindings");
    auto held=binding(0).buffer;ini.reset();backend.reset();check(budgets.resident->used()>0,"Retained keyed copy survives generation retirement");
    held.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Keyed and expired resource caches release all GPU budgets");
}
static void spatial_pools_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    // Source packing is [X:12][Y:8][Z:12]. A GPU store feeds these exact float
    // bit patterns to the original spatial index, independently of its helper.
    const uint32_t keys[]={0x0640a064,0x0650a064,0x0670a064,0x0690a064,0x12c0a064,0x2580a064};
    {std::ofstream out(root/"Mods/keys.buf",std::ios::binary);out.write(reinterpret_cast<const char*>(keys),sizeof(keys));}
    const std::string source=R"ini(namespace = spatialgpu
[Constants]
global $phase = 0
global $base = 0
global $near = 0
global $two = 0
global $four = 0
global $far = 0
global $next = 0
global $frame = 0
global $draw_frame = 0
[ResourceKeys]
type = Buffer
format = R32_FLOAT
filename = keys.buf
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceOther]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 9 10 11 12
bind_flags = shader_resource unordered_access
[PoolSpace]
pool_size = 2
pool_index_type = spatial
pool_spatial_radius = 1
pool_expiration_timeout_frames = 10
pool_expiration_refresh_on_read = true
[TextureOverrideProbe]
hash = 12345678
ps-t3 = ref PoolSpace[$four]
$draw_frame = frame_time
[Present]
$frame = frame_time
if $phase == 0
 store = $base, ResourceKeys, 0
 store = $near, ResourceKeys, 1
 store = $two, ResourceKeys, 2
 store = $four, ResourceKeys, 3
 store = $far, ResourceKeys, 4
 store = $next, ResourceKeys, 5
 PoolSpace[$base] = ref ResourceSource
 ps-t0 = ref PoolSpace[$near]
 PoolSpace[$far] = ref ResourceOther
else if $phase == 1
 ps-t1 = ref PoolSpace[$two]
else if $phase == 2
 ps-t2 = ref PoolSpace[$four]
else
 PoolSpace[$next] = ref ResourceSource
 ps-t4 = ref PoolSpace[$next]
 ps-t5 = ref PoolSpace[$base]
endif
$phase = $phase + 1
)ini";
    write(root/"Mods/spatial.ini",source.c_str());RuntimeConfig config{root,{},root/"settings","spatial",root/"Mods"};
    gpu::ResourceBudgets budgets;std::string why;auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    auto ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);++checks;
    auto present=[&](unsigned frame,float duration){RuntimeInput input;input.frame_seconds=duration;
        backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present(input,frame,float(frame),why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();++checks;};
    auto binding=[&](unsigned slot){return backend->binding(L'p',T::SHADER_RESOURCE,slot);};
    present(1,1.0f/120);check(bool(binding(0).buffer),"Spatial neighbor within the original radius retains its resource");
    check(value(backend->commands().readback(d.queue,binding(0).buffer,0,4))==1,"Spatial lookup refers to the actual original source bytes");
    present(2,1.0f/60);check(binding(1).buffer==binding(0).buffer,"Host frame duration expands original spatial radius at lower frame rates");
    present(3,1.0f/240);check(!binding(2).buffer,"Higher frame rate preserves original ceil radius and refuses an out-of-radius lookup");
    RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;draw.frame=4;draw.frame_seconds=1.0f/60;assets::FilePlan plan;
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->draw(draw,plan,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    check(binding(3).buffer==binding(0).buffer && ini->status().variables.at("$\\spatialgpu\\draw_frame")==draw.frame_seconds,
          "Draw context supplies the same original frame_time used by spatial resource lookup");
    present(5,1.0f/120);check(binding(4).buffer==binding(0).buffer && !binding(5).buffer,"Full spatial pool reuses a physical slot and detaches its distant old key");
    const auto phase=ini->status().variables.at("$\\spatialgpu\\phase");RuntimeInput invalid;invalid.frame_seconds=-1;
    check(!ini->present(invalid,6,6,why) && why.find("frame duration")!=std::string::npos,"Invalid host frame duration refuses before original commands");
    draw.frame_seconds=std::numeric_limits<float>::infinity();
    check(!ini->draw(draw,plan,why) && why.find("frame duration")!=std::string::npos,"Nonfinite draw duration cannot reach original numeric casts");
    check(ini->status().variables.at("$\\spatialgpu\\phase")==phase,"Rejected clocks leave original variables unchanged");
    ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Spatial pools and repeated clock contexts release all GPU owners");
    const auto rejected=source.substr(0,source.find("[TextureOverrideProbe]"))+"[Present]\nPoolSpace[0] = ref ResourceSource\n";
    write(root/"Mods/spatial.ini",rejected.c_str());backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);invalid.frame_seconds=std::numeric_limits<float>::max();
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    declined(*ini,*backend,ini->present(invalid,6,6,why),"radius outside","An overflowing spatial radius stops its command before the original's unsigned conversion");
    ini.reset();backend.reset();
    check(!budgets.resident->used() && !budgets.staging->used(),"Rejected spatial command releases source and staging owners");
    const auto cpu_failure=source.substr(0,source.find("[TextureOverrideProbe]"))+
        "[Present]\nps-t0 = ref ResourceSource\n$phase = #PoolSpace[0]\n"
        "[TextureOverrideProbe]\nhash = 12345678\nps-t0 = ref ResourceSource\n$phase = #PoolSpace[0]\n";
    write(root/"Mods/spatial.ini",cpu_failure.c_str());backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    // In a Present each command stands for itself: the CPU query that fails does nothing, and the GPU
    // commands recorded before it are submitted (a draw's lists, below, still stand or fall together).
    declined(*ini,*backend,ini->present(invalid,6,6,why),"radius outside","A CPU query that fails after GPU commands were recorded does nothing");
    ini.reset();backend.reset();
    check(!budgets.resident->used() && !budgets.staging->used(),"CPU-failed generation discards all pending GPU owners");
    backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);draw.frame_seconds=invalid.frame_seconds;
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    check(!ini->draw(draw,plan,why) && why.find("radius outside")!=std::string::npos,"Draw CPU failure propagates its original pool context");
    refuses([&]{backend->finish();},"Failed draw cannot publish earlier GPU commands from its scope");
    backend->discard();ini.reset();backend.reset();
    check(!budgets.resident->used() && !budgets.staging->used(),"Failed draw releases pending allocations after discard");
}
static void regions_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    const std::string declarations=R"ini(namespace = regions
[Constants]
global $raw = 0
global $nocache = 0
[ResourceSource]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = shader_resource unordered_access
[ResourceCopy]
bind_flags = shader_resource unordered_access
[ResourceWindow]
bind_flags = shader_resource unordered_access
[ResourceVertex]
type = Buffer
stride = 12
data = R32_FLOAT 1 2 3 4 5 6 7 8 9 10 11 12 13 14
bind_flags = vertex_buffer
[ResourceCB]
type = Buffer
data = R32_FLOAT 101 102 103 104 105 106 107 108
bind_flags = constant_buffer
[ResourceCBWindow]
bind_flags = constant_buffer
[ResourceCBExplicit]
bind_flags = constant_buffer
[ResourceSmall]
type = StructuredBuffer
stride = 12
data = R32_FLOAT 1 2 3 4 5 6
bind_flags = shader_resource unordered_access
[ResourceRounded]
bind_flags = constant_buffer
[ResourceRaw]
type = ByteAddressBuffer
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceTypedAlias]
format = R32_FLOAT
bind_flags = shader_resource unordered_access
[ResourceTemporary]
bind_flags = shader_resource unordered_access
[ResourceTruncated]
byte_width = 16
bind_flags = shader_resource unordered_access
[ResourceStrideAlias]
stride = 16
bind_flags = shader_resource
[ResourceIndex]
type = Buffer
format = R16_UINT
stride = 4
data = R16_UINT 1 2 3 4 5 6 7 8
bind_flags = index_buffer
[ResourceIndexCopy]
bind_flags = shader_resource
[Present]
)ini";
    const std::string commands=R"ini(ResourceCopy = copy ResourceSource
ResourceCopy = copy ResourceSource->Region(8, 32)
ps-t0 = ref ResourceCopy
ResourceWindow = copy ResourceSource
ResourceWindow = copy ResourceSource->Region(8, 16)
ps-t1 = ref ResourceWindow
vb0 = ref ResourceVertex
cs-t2 = copy vb0
cs-t3 = copy vb0->Region(12, 56)
vs-cb0 = ref ResourceCB->Region(16, 16)
ResourceCBWindow = copy vs-cb0
ResourceCBExplicit = copy vs-cb0->Region(0, 16)
vs-cb1 = ref ResourceCBWindow
vs-cb2 = ref ResourceCBExplicit
ResourceRounded = copy ResourceCB
ResourceRounded = copy ResourceSmall
vs-cb3 = ref ResourceRounded
ResourceTypedAlias = ref ResourceRaw
ps-t4 = raw ref ResourceTypedAlias
cs-u4 = raw ref ResourceTypedAlias
clear = cs-u4 0x42280000
store = $raw, ResourceRaw, 0
cs-u4 = null
ps-t4 = raw ref ResourceTypedAlias
ps-t5 = ref ResourceTypedAlias
ResourceTemporary = no_view_cache ref ps-t5
clear = ResourceTemporary 0x422c0000
store = $nocache, ResourceRaw, 0
ResourceStrideAlias = ref ResourceSource
ps-t7 = ref ResourceStrideAlias
ib = ref ResourceIndex->Region(2, 16)
ResourceIndexCopy = copy ib
ps-t8 = ref ResourceIndexCopy
)ini";
    write(root/"Mods/regions.ini",(declarations+commands).c_str());RuntimeConfig config{root,{},root/"settings","regions",root/"Mods"};
    gpu::ResourceBudgets budgets;std::string why;auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
    auto ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error(why);
    backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    auto bytes_at=[&](wchar_t stage,T type,unsigned slot,UINT size){const auto b=backend->binding(stage,type,slot);
        check(b.buffer && b.description.ByteWidth>=size,"Region result has an owned allocation");
        return backend->commands().readback(d.queue,b.buffer,0,size);};
    const float shifted[]={3,4,5,6,7,8,7,8},window[]={3,4,3,4,5,6,7,8};
    auto data=bytes_at(L'p',T::SHADER_RESOURCE,0,32);check(!memcmp(data.data(),shifted,32),"Region shift preserves unwritten cached tail");
    data=bytes_at(L'p',T::SHADER_RESOURCE,1,32);check(!memcmp(data.data(),window,32),"Original non-CB region size is an end boundary");
    data=bytes_at(L'c',T::SHADER_RESOURCE,2,48);
    for(unsigned i=0;i<12;++i)check(value(data,i*4)==float(i+1),"Vertex-to-structured copy preserves complete elements");
    auto converted=backend->binding(L'c',T::SHADER_RESOURCE,2);
    check(converted.description.ByteWidth==48 && converted.description.StructureByteStride==12 && converted.view_size==48,
          "Vertex conversion rounds the allocation to complete structures");
    data=bytes_at(L'c',T::SHADER_RESOURCE,3,36);
    for(unsigned i=0;i<9;++i)check(value(data,i*4)==float(i+4),"Region conversion rounds copied bytes by source stride");
    data=bytes_at(L'v',T::CONSTANT_BUFFER,1,16);check(value(data)==105 && value(data,12)==108,"CB source visible range is copied without its original offset");
    data=bytes_at(L'v',T::CONSTANT_BUFFER,2,16);check(value(data)==101 && value(data,12)==104,"Explicit CB source region overrides the bound window");
    data=bytes_at(L'v',T::CONSTANT_BUFFER,3,32);check(value(data)==1 && value(data,20)==6 && value(data,24)==107 && value(data,28)==108,
          "CB size rounds up while uncopied cached padding remains unchanged");
    check(backend->binding(L'p',T::SHADER_RESOURCE,4).view_format==DXGI_FORMAT_R32_TYPELESS &&
          backend->binding(L'p',T::SHADER_RESOURCE,5).view_format==DXGI_FORMAT_R32_FLOAT,"Raw modifier creates a raw view without changing a typed alias");
    const auto vars=ini->status().variables;
    check(vars.at("$\\regions\\raw")==42 && vars.at("$\\regions\\nocache")==43,"No-view-cache drops a custom SRV before its next clear");
    check(backend->binding(L'p',T::SHADER_RESOURCE,7).source_stride==16,
          "First custom reference captures the overridden stride, not an invented initial source stride");
    data=bytes_at(L'p',T::SHADER_RESOURCE,8,14);const uint16_t indices[]={2,3,4,5,6,7,8};
    check(!memcmp(data.data(),indices,sizeof(indices)) && backend->binding(L'p',T::SHADER_RESOURCE,8).source_stride==2,
          "Index-buffer copies use bound format width and offset, independent of declaration stride");
    ini.reset();backend.reset();
    // The converted binding above deliberately outlives this generation.
    check(budgets.resident->used()>0 && !budgets.staging->used(),"Retained region binding outlives generation retirement");
    {   // D-153: a region ending before its offset is a box D3D11 leaves undefined; the copy copies nothing.
        write(root/"Mods/regions.ini",(declarations+"ResourceCopy = copy ResourceSource->Region(16, 8)\n").c_str());
        backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);ini=IniRuntime::load(config,backend,why);
        if(!ini)throw std::runtime_error("Region fixture must parse: "+why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        check(ini->present({},2,0,why),"A region ending before its offset copies nothing and the list goes on");
        auto done=backend->finish();if(done)done->wait();ini.reset();backend.reset();
    }
    const std::vector<std::pair<std::string,std::string>> bad={
        {"ResourceTruncated = copy ResourceSource\n","incompatible allocation sizes"},
        {"ResourceTruncated = copy ResourceSource->Region(8, 32)\n","truncates"},
    };
    for(const auto& failure:bad){write(root/"Mods/regions.ini",(declarations+failure.first).c_str());
        backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);ini=IniRuntime::load(config,backend,why);
        if(!ini)throw std::runtime_error("Region rejection must parse: "+why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*ini,*backend,ini->present({},2,0,why),failure.second,"An invalid copy region copies nothing and is said");
        ini.reset();backend.reset();
    }
    converted={};check(!budgets.resident->used() && !budgets.staging->used(),"Successful and rejected region generations release all allocations");
}
static void view_metadata_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    std::filesystem::create_directories(root/"Mods");
    write(root/"Mods/views.ini",R"ini(namespace = view_metadata
[Constants]
global $cached_null = 0
[ResourceRaw]
type = ByteAddressBuffer
data = R32_FLOAT 1 2 3 4
bind_flags = shader_resource unordered_access
[ResourceTyped]
format = R32_FLOAT
bind_flags = shader_resource unordered_access
[ResourceSaved]
bind_flags = shader_resource unordered_access
[PoolViews]
pool_size = 2
[Present]
ResourceTyped = ref ResourceRaw
cs-u0 = ref ResourceTyped
PoolViews[0:0] = ref cs-u[0:0]
cs-u0 = null
ps-t0 = ref PoolViews[0]
ps-t1 = ref ResourceTyped
ResourceSaved = ref ps-t1
ResourceSaved = null
ResourceSaved = ref ResourceTyped
clear = ResourceSaved 0x40a00000
store = $cached_null, ResourceRaw, 0
)ini");
    RuntimeConfig config{root,{},root/"settings","view_metadata",root/"Mods"};gpu::ResourceBudgets budgets;std::string why;
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    check(backend->binding(L'p',T::SHADER_RESOURCE,0).view_format==DXGI_FORMAT_R32_TYPELESS,
          "Custom pool source does not infer typed metadata from a fetched view");
    check(ini->status().variables.at("$\\view_metadata\\cached_null")==1,
          "Original null keeps a compatible cached SRV for the next reference");
    ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Null/view metadata fixture releases all owners");
}
static void properties_gpu(Device& d,const std::filesystem::path& root){
    using namespace frontend;using T=ResourceCopyTargetType;
    struct Case{const char* name;const char* expression;float value;};
    const std::vector<Case> early={
        {"early_format","ResourceUnassigned->format",41},{"early_stride","ResourceUnassigned->stride",16},
        {"early_width","ResourceUnassigned->width",7},{"early_height","ResourceUnassigned->height",8},
        {"early_array","ResourceUnassigned->array",9},{"early_mips","ResourceUnassigned->mips",3},
        {"early_source","ResourceUnassigned->sourcestride",0},
    };
    const std::vector<Case> expected={
        {"size","ResourceData->size",32},{"stride","ResourceData->stride",8},{"source","ResourceData->sourcestride",0},
        {"format","ResourceData->format",-1},{"width","ResourceData->width",-4},{"height","ResourceData->height",-4},
        {"array","ResourceData->array",-4},{"mips","ResourceData->mips",-4},{"flags","ResourceData->bindflags",136},
        {"slot_stride","ps-t0->stride",8},{"slot_source","ps-t0->sourcestride",-1},{"slot_flags","ps-t0->bindflags",136},
        {"slot_offset","ps-t0->offset",-1},{"slot_same","@ps-t0 == @ResourceData",1},
        {"null_size","ResourceUnassigned->size",-2},{"null_id","@ResourceUnassigned",0},
        {"null_slot_format","ps-t30->format",-2},{"null_slot_flags","ps-t30->bindflags",0},
        {"null_slot_size","ps-t30->size",-2},{"null_slot_offset","ps-t30->offset",-2},{"null_slot_hash","ps-t30->hashregion(0, 4)",-2},
        {"cb_size","vs-cb0->size",16},{"cb_hash","vs-cb0->hashregion(0, 16)",-1},{"cb_stride","vs-cb0->stride",16},{"cb_offset","vs-cb0->offset",16},
        {"ib_format","ib->format",57},{"ib_stride","ib->stride",4},{"ib_size","ib->size",16},
        {"alias_null_source","ResourceAlias->sourcestride",16},{"alias_null_size","ResourceAlias->size",-2},
        {"pool_source","PoolViews[0]->sourcestride",16},{"pool_format","PoolViews[0]->format",-1},
    };
    const std::vector<Case> drawn={{"vb_offset","vb0->offset",24},{"ib_offset","ib->offset",6},
        {"vb_hash","vb0->hashregion(0, 4)",-1},{"ib_spatial","ib->spatialhash(0, 1, 2, 1.5)",-1},{"this_format","this->format",57},{"this_same","@this == @ib",1},{"this_offset","this->offset",-1}};
    std::string source="namespace = properties\n[Constants]\n";
    for(const auto* group:{&early,&expected,&drawn})for(const auto& c:*group)source+="global $"+std::string(c.name)+" = 0\n";
    for(const auto& c:early)source+="$"+std::string(c.name)+" = "+c.expression+"\n";
    source+=R"ini([ResourceUnassigned]
format = R32_FLOAT
stride = 16
width = 7
height = 8
array = 9
mips = 3
[ResourceData]
type = StructuredBuffer
stride = 8
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = shader_resource unordered_access
[ResourceAlias]
stride = 16
bind_flags = shader_resource
[PoolViews]
pool_size = 2
[ResourceCB]
type = Buffer
data = R32_FLOAT 1 2 3 4 5 6 7 8
bind_flags = constant_buffer
[ResourceIB]
type = Buffer
format = R16_UINT
stride = 4
data = R16_UINT 1 2 3 4 5 6 7 8
bind_flags = index_buffer
[ResourceVertex]
type = Buffer
stride = 12
data = R32_FLOAT 1 2 3 4 5 6 7 8 9 10 11 12
bind_flags = vertex_buffer
[ResourceThis]
bind_flags = shader_resource
[Present]
ps-t0 = ref ResourceData
cs-u0 = ref ResourceData
cs-u0 = null
ps-t0 = ref ResourceData
ResourceAlias = ref ResourceData
PoolViews[0] = ref ResourceAlias
PoolViews[0:0] = ref ps-t[0:0]
ResourceAlias = null
vs-cb0 = ref ResourceCB->Region(16, 16)
ib = ref ResourceIB->Region(2, 16)
vb0 = ref ResourceVertex->Region(12, 48)
)ini";
    for(const auto& c:expected)source+="$"+std::string(c.name)+" = "+c.expression+"\n";
    source+="[TextureOverrideTest]\nhash = 12345678\nmatch_index_count = 3\n";
    for(const auto& c:drawn)source+="$"+std::string(c.name)+" = "+c.expression+"\n";
    source+="ResourceThis = copy this->Region(4, 16)\nps-t9 = ref ResourceThis\n";
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/properties.ini",source.c_str());
    RuntimeConfig config{root,{},root/"settings","properties",root/"Mods"};gpu::ResourceBudgets budgets;std::string why;
    auto backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);auto ini=IniRuntime::load(config,backend,why);
    if(!ini)throw std::runtime_error(why);check(!budgets.resident->used() && !budgets.staging->used(),"Declaration property queries do not allocate in Constants");
    auto assert_cases=[&](const std::vector<Case>& cases){const auto vars=ini->status().variables;for(const auto& c:cases){
        const auto actual=vars.at("$\\properties\\"+std::string(c.name));
        if(actual!=c.value)throw std::runtime_error(std::string(c.name)+" expected "+std::to_string(c.value)+", actual "+std::to_string(actual));++checks;}};
    assert_cases(early);backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    if(!ini->present({},1,0,why))throw std::runtime_error(why);assert_cases(expected);
    RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;draw.base=1;draw.first=2;draw.frame=1;
    assets::FilePlan plan;if(!ini->draw(draw,plan,why))throw std::runtime_error(why);assert_cases(drawn);
    auto ticket=backend->finish();if(ticket)ticket->wait();ticket.reset();
    auto binding=backend->binding(L'p',T::SHADER_RESOURCE,9);auto bytes=backend->commands().readback(d.queue,binding.buffer,0,12);
    const uint16_t indices[]={3,4,5,6,7,8};check(!memcmp(bytes.data(),indices,sizeof(indices)),"This-resource region arguments survive target resolution");
    binding={};ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Property and draw-context fixture releases all owners");
    const std::vector<std::pair<std::string,std::string>> bad={
        {"vb0 = ref ResourceVertex\n$vb_offset = vb0->offset\n","requires a draw context"},
        {"vb0 = ref ResourceVertex\n$vb_offset = vb0->hashregion(0, 4)\n","requires a draw context"},
    };
    for(const auto& failure:bad){const auto rejected=source.substr(0,source.find("[Present]"))+"[Present]\n"+failure.first;
        write(root/"Mods/properties.ini",rejected.c_str());backend=std::make_shared<GpuResourceRuntime>(d.resource,budgets);
        ini=IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error("Property rejection must parse: "+why);
        backend->begin(d.queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*ini,*backend,ini->present({},2,0,why),failure.second,"A property without its draw context leaves its command undone");
        ini.reset();backend.reset();
        check(!budgets.resident->used() && !budgets.staging->used(),"Rejected property query releases its generation");
    }
}
#include "resource_image_ini_cases.h"
#include "resource_image_conversion_cases.h"
#include "resource_png_cases.h"
#include "resource_parameter_consumer.h"
#include "resource_parameter_cases.h"
#include "resource_shader_cases.h"
#include "resource_compute_cases.h"
#include "resource_usage_cases.h"
#include "resource_binding_cases.h"
#include "resource_graphics_link_cases.h"
#include "resource_graphics_state_cases.h"
#include "resource_graphics_cases.h"
#include "resource_ini_graphics_cases.h"
#include "resource_geometry_cases.h"
#include "resource_text_cases.h"
#include "resource_layout_cases.h"
#include "resource_divisor_cases.h"
#include "resource_vertex_input_cases.h"
#include "resource_match_cases.h"
#include "resource_shader_hook_cases.h"
#include "resource_identity_cases.h"
#include "resource_regex_cases.h"
#include "resource_bytecode_cases.h"
#include "resource_bytecode_views.h"
#include "resource_backbuffer_cases.h"
#include "resource_panel_cases.h"
#include "resource_owned_generation_cases.h"
#include "resource_operand_cases.h"
#include "resource_reference_cases.h"
// The layer's side of the original as the judge (D-150, scripts/check_dx11_judge.py): an INI folder's
// [Present] under the owned GPU runtime (resources and CustomShaders) `frames` times with a 256x256 back
// buffer, then every variable as one JSON line.
static int present_ini(const std::filesystem::path& root,unsigned frames){
    // The game's Vulkan version: texel-buffer views start at a single texel there (D-151).
    Device d;d.api=VK_API_VERSION_1_3;d.init(false,false,true,false,false,false,true,true,true);
    gpu::ResourceBudgets budgets;auto host=shader_host(d);
    auto gpu=std::make_shared<frontend::GpuShaderRuntime>(d.resource,host,L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll",budgets);
    frontend::RuntimeConfig config{root,{},root/"settings","judge",root/"Mods"};
    auto& resources=gpu->resources();std::string why;
    resources.begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    auto ini=frontend::IniRuntime::load(config,gpu,why);
    if(!ini){fprintf(stderr,"load: %s\n",why.c_str());return 1;}
    {auto ticket=resources.finish();if(ticket)ticket->wait();}
    gpu::ImageDescription description;description.format=VK_FORMAT_R8G8B8A8_UNORM;description.extent={256,256,1};
    description.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto image=resources.commands().create_image(description);
    for(unsigned frame=1;frame<=frames;++frame){
        resources.begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        resources.backbuffer(image,DXGI_FORMAT_R8G8B8A8_UNORM);
        if(!ini->present({},frame,0,why)){fprintf(stderr,"present %u: %s\n",frame,why.c_str());return 1;}
        auto ticket=resources.finish();if(ticket)ticket->wait();
    }
    std::string json="{";
    for(const auto& v:ini->status().variables){
        std::string key;for(char c:v.first){if(c=='\\' || c=='"')key+='\\';key+=c;}
        char value[64];snprintf(value,sizeof(value),"%.9g",double(v.second));
        if(json.size()>1)json+=",";json+="\""+key+"\":"+value;
    }
    printf("%s}\n",json.c_str());
    resources.backbuffer(nullptr,DXGI_FORMAT_UNKNOWN);image.reset();ini.reset();gpu.reset();
    // The judge's layer side runs under the validation layer: a message fails the case.
    if(errors || warnings){fprintf(stderr,"validation errors=%u warnings=%u\n",unsigned(errors),unsigned(warnings));return 1;}
    return 0;
}
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==4 && !wcscmp(argv[1],L"--present-ini")){
        try{return present_ini(std::filesystem::absolute(argv[2]),unsigned(_wtoi(argv[3])));}
        catch(const std::exception& e){fprintf(stderr,"present-ini: %s\n",e.what());return 1;}
    }
    try{check(argc==2,"Fixture path required");{Device device;device.init(false,false,true,false,false,false,true,true,true);low_level(device);image_commands(device);const auto root=std::filesystem::absolute(argv[1]);
        ini_gpu(device,root/"inline");file_and_failures(device,root/"files");range_gpu(device,root/"ranges");copy_gpu(device,root/"copies");regions_gpu(device,root/"regions");
        view_metadata_gpu(device,root/"view-metadata");properties_gpu(device,root/"properties");
        whole_pool_gpu(device,root/"whole-pool");buffer_options_gpu(device,root/"options");
        keyed_pools_gpu(device,root/"keyed");spatial_pools_gpu(device,root/"spatial");
        image_source_reference();image_ini_declarations(device,root/"image-declarations");
        image_ini_clears(device,root/"image-clears");image_ini_depth_resolve(device,root/"image-depth");
        image_ini_dds(device,root/"image-dds");image_ini_png(device,root/"image-png",root/"png-panel");image_ini_file_cache(device,root/"image-file-cache");
        image_ini_depth_dds(device,root/"image-depth-dds");image_ini_failures(device,root/"image-failures");
        image_ini_pool_lifetime(device,root/"image-pool-lifetime");
        image_ini_integer_rtv(device,root/"image-integer-rtv");
        image_depth_conversion(device,root/"image-depth-conversion");
        parameter_boundaries(device,root/"parameter-boundaries");parameter_draws_and_dirty(device,root/"parameter-draws");
        parameter_keys(device,root/"parameter-keys");parameter_registers_and_failures(device,root/"parameter-failures");
        parameter_lifetime(device,root/"parameter-lifetime");parameter_draw_failure(device,root/"parameter-draw-failure");
        shader_compiler_gpu(device,root/"shader-compiler");
        compute_descriptors(device,root/"compute-descriptors");original_compute(device,root/"original-compute");
        compute_scalar_layout(device,root/"compute-scalar-layout");
        original_shader_scopes(device,root/"original-shader-scopes");
        compute_shared_memory(device,root/"compute-shared-memory");compute_sample_pairs(device,root/"compute-sample-pairs");
        compute_indirect(device,root/"compute-indirect");original_indirect(device,root/"original-indirect");
        original_sampling(device,root/"original-sampling");original_buffer_hazards(device,root/"original-buffer-hazards");
        original_binding_batches(device,root/"binding-batches");original_restore_hazards(device,root/"restore-hazards");
        original_image_binding_ranges(device);original_attachment_bindings(device,root/"attachment-bindings");
        original_pixel_binding_sets(device,root/"pixel-binding-sets");graphics_link_pixels(device,root/"graphics-link");
        graphics_coordinate_pixels(device,root/"graphics-coordinates");original_graphics_state_descriptions();owned_graphics(device,root/"owned-graphics");
        original_graphics_creation_defaults();original_ini_graphics(device,root/"ini-graphics");original_backbuffer(device,root/"backbuffer");imported_backbuffer(device,root/"imported-backbuffer");original_image_panel(device,root/"panel");original_owned_operands(device,root/"owned-operands");owned_generation_cases(device,root/"owned-generation");original_ini_graphics_nesting(device,root/"ini-graphics-nesting");
        original_ini_graphics_sampling(device,root/"ini-graphics-sampling");original_ini_graphics_state_pixels(device,root/"ini-graphics-state");
        original_ini_graphics_depth(device,root/"ini-graphics-depth");original_ini_graphics_caller_and_failure(device,root/"ini-graphics-caller");
        original_ini_geometry(device,root/"ini-geometry");original_core_text(root/"core-text");
        source_layout_creation(root/"source-layouts");original_layout_pixels(device,root/"ini-layout-pixels");original_layout_edits(device,root/"ini-layout-edits");
        original_layout_append(device,root/"ini-layout-append");original_layout_divisors(device,root/"ini-layout-divisors");
        original_vertex_input_types(device,root/"ini-input-types");vertex_input_lowering_guards(device,root/"input-lowering");
        original_matched_layouts(device,root/"matched-layouts");
        original_shader_hooks(device,root/"source-shader-hooks");original_compute_hooks(device,root/"source-compute-hooks");
        native_resource_match_reference(device,root/"source-resource-matcher");owned_resource_matching(device,root/"owned-resource-matcher");
        original_core_regex(device,root/"core-regex");original_core_regex(device,root/"core-regex-pattern",true);
        bytecode_execution(device,root/"bytecode-software",gpu::DxbcFloat32Mode::Software);
        bytecode_graphics(device,root/"bytecode-software-graphics",gpu::DxbcFloat32Mode::Software);
        bytecode_resources(device,root/"bytecode-software-resources",gpu::DxbcFloat32Mode::Software);
        bytecode_float32(device,root/"bytecode-float32");
        bytecode_ini_views(device,root/"bytecode-ini-views");
        original_reference_fallback(device,root/"reference-fallback");
        {Device bytecode_device;bytecode_device.init(false,false,true,false,false,true);
            printf("Bytecode float-control device: %s\n",bytecode_device.properties.deviceName);
            bytecode_execution(bytecode_device,root/"bytecode-compute");bytecode_graphics(bytecode_device,root/"bytecode-graphics");
            bytecode_resources(bytecode_device,root/"bytecode-resources");
            bytecode_float32(bytecode_device,root/"bytecode-float32-hardware",gpu::DxbcFloat32Mode::Hardware);}}
        check(errors==0 && warnings==0,"Clean validation including device shutdown");
        printf("resource_gpu: %u checks passed; validation errors=%u warnings=%u\n",checks,unsigned(errors),unsigned(warnings));
        printf("declined commands: %u scopes went on, %u dropped\n",declined_kept,declined_dropped);return 0;
    }catch(const std::exception& e){printf("FAIL after %u checks: %s\n",checks,e.what());return 1;}
}
