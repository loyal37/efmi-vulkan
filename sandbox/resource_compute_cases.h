static gpu::HlslProgram compute_program(const std::filesystem::path& root,const char* text){
    std::filesystem::create_directories(root);write(root/"compute.hlsl",text);
    gpu::HlslCompiler compiler(L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll");
    gpu::HlslRequest request;request.source=root/"compute.hlsl";request.working_directory=root;
    gpu::HlslProgram program;std::string why;if(!compiler.compile(request,program,why))throw std::runtime_error(why);++checks;return program;
}
static gpu::ShaderHost shader_host(Device& d){gpu::ShaderHost h;h.get_device_proc=d.get_device_proc;h.properties=d.properties;
    h.float_controls=d.float_controls;h.float_properties=d.float_properties;
    h.validator=L"F:/vulkan/Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/build/source/SPIRV-Tools-shared.dll";
    h.scalar_block_layout=d.scalar_layout;h.shader_draw_parameters=d.draw_parameters;h.vertex_divisor=d.vertex_divisor;h.vertex_zero_divisor=d.vertex_zero_divisor;
    h.divisor_first_instance=d.divisor_first_instance;h.max_vertex_divisor=d.max_vertex_divisor;h.null_descriptors=d.null_descriptors;
    h.texel_alignment=d.texel_alignment;h.texel_alignment_properties=d.texel_alignment_properties;return h;}
static gpu::ShaderDescriptor buffer_descriptor(char type,unsigned slot,const gpu::Buffer& buffer,VkFormat format=VK_FORMAT_UNDEFINED){
    gpu::ShaderDescriptor value;value.binding=gpu::HlslCompiler::binding('c',type,slot);value.buffer=buffer;value.size=buffer->size();value.format=format;return value;
}
static gpu::ImageView compute_image(gpu::ResourceCommands& commands,VkFormat format,const void* value,bool output=false){
    gpu::ImageDescription description;description.format=format;description.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
        (output?VK_IMAGE_USAGE_STORAGE_BIT:VK_IMAGE_USAGE_SAMPLED_BIT);
    auto image=commands.create_image(description);if(value)commands.write(image,image_region(image),value,4);
    return commands.view(image,VK_IMAGE_VIEW_TYPE_2D,format,image->full_range());
}
static void compute_descriptors(Device& d,const std::filesystem::path& root){
    auto program=compute_program(root,R"hlsl(cbuffer Parameters : register(b2) {uint Add;};
StructuredBuffer<uint> Input : register(t4);
ByteAddressBuffer Raw : register(t7);
Buffer<uint> Typed : register(t8);
Texture2D<float> Images[2] : register(t10);
Texture2D<float> Smooth : register(t12);
SamplerState Linear : register(s0);
RWStructuredBuffer<uint4> Output : register(u1);
RWTexture2D<uint> ImageOut : register(u2);
RWBuffer<uint> TypedOut : register(u3);
[numthreads(1,1,1)] void main(){uint4 value=uint4(Input[0]+Add,Raw.Load(0)+Typed[0],
    (uint)(Images[0].Load(int3(0,0,0))+Images[1].Load(int3(0,0,0))),
    (uint)round(Smooth.SampleLevel(Linear,float2(.5,.5),0)*100));
    Output[0]=value;ImageOut[uint2(0,0)]=value.x+value.y+value.z+value.w;TypedOut[0]=value.w;}
)hlsl");
    gpu::ResourceBudgets budgets;auto host=shader_host(d);
    {gpu::ResourceCommands commands(d.resource,budgets);gpu::ShaderCommands shaders(commands,host);auto pipeline=shaders.compile(program);
        const uint32_t cb[4]={3,0,0,0},a=5,b=7,c=11;const float pixels[3]={13,17,.5f};
        auto uniform=commands.create(16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,cb,16);
        auto input=commands.create(4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&a,4);
        auto raw=commands.create(4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&b,4);
        auto typed=commands.create(4,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,&c,4);
        auto output=commands.create(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true);
        auto typed_output=commands.create(4,VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,nullptr,0,true);
        auto first=compute_image(commands,VK_FORMAT_R32_SFLOAT,&pixels[0]);auto second=compute_image(commands,VK_FORMAT_R32_SFLOAT,&pixels[1]);
        auto smooth=compute_image(commands,VK_FORMAT_R32_SFLOAT,&pixels[2]);auto image_out=compute_image(commands,VK_FORMAT_R32_UINT,nullptr,true);
        VkSamplerCreateInfo sampler_info={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler_info.magFilter=sampler_info.minFilter=VK_FILTER_LINEAR;
        sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler_info.addressModeU=sampler_info.addressModeV=sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        auto sampler=shaders.sampler(sampler_info);
        std::vector<gpu::ShaderDescriptor> bindings={buffer_descriptor('b',2,uniform),buffer_descriptor('t',4,input),buffer_descriptor('t',7,raw),
            buffer_descriptor('t',8,typed,VK_FORMAT_R32_UINT),buffer_descriptor('u',1,output),buffer_descriptor('u',3,typed_output,VK_FORMAT_R32_UINT)};
        for(auto item:{std::make_pair(10u,first),std::make_pair(10u,second),std::make_pair(12u,smooth)}){
            gpu::ShaderDescriptor value;value.binding=gpu::HlslCompiler::binding('c','t',item.first);value.element=item.second==second?1:0;value.image=item.second;bindings.push_back(value);}
        gpu::ShaderDescriptor value;value.binding=gpu::HlslCompiler::binding('c','u',2);value.image=image_out;bindings.push_back(value);
        value={};value.binding=gpu::HlslCompiler::binding('c','s',0);value.sampler=sampler;bindings.push_back(value);
        shaders.dispatch(pipeline,bindings,{1,1,1});auto bytes=commands.readback(d.queue,output,0,16);const uint32_t expected[4]={8,18,30,50};
        check(bytes.size()==16 && !memcmp(bytes.data(),expected,16),"Compute reads original constant/structured/raw/typed/array/sampler descriptors");
        bytes=commands.readback(d.queue,image_out->owner(),image_region(image_out->owner()));uint32_t result=0;memcpy(&result,bytes.data(),4);
        check(result==106,"Compute writes actual storage-image pixels");bytes=commands.readback(d.queue,typed_output,0,4);memcpy(&result,bytes.data(),4);
        check(result==50,"Compute writes actual storage texel-buffer values");
        auto second_output=commands.create(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true);
        const uint32_t changed[4]={100,0,0,0};auto second_cb=commands.create(16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,changed,16);
        shaders.dispatch(pipeline,bindings,{1,1,1});auto switched=bindings;switched[0].buffer=second_cb;switched[4].buffer=second_output;
        shaders.dispatch(pipeline,switched,{1,1,1});bytes=commands.readback(d.queue,output,0,16);
        check(!memcmp(bytes.data(),expected,16),"First recorded dispatch retains its own descriptor contents");
        bytes=commands.readback(d.queue,second_output,0,16);const uint32_t second_expected[]={105,18,30,50};
        check(!memcmp(bytes.data(),second_expected,16),"Later descriptor changes affect only the later dispatch");switched.clear();
        auto wrong=bindings;wrong[0].offset=1;refuses([&]{shaders.dispatch(pipeline,wrong,{1,1,1});},"Misaligned shader constant buffer refuses");
        wrong=bindings;wrong.back().sampler.reset();refuses([&]{shaders.dispatch(pipeline,wrong,{1,1,1});},"Unbound shader sampler refuses without null emulation");
        wrong=bindings;wrong[3].format=VK_FORMAT_R32_SFLOAT;refuses([&]{shaders.dispatch(pipeline,wrong,{1,1,1});},"Typed buffer scalar mismatch refuses");
        wrong=bindings;wrong.push_back(wrong.front());refuses([&]{shaders.dispatch(pipeline,wrong,{1,1,1});},"Duplicate descriptor refuses");
        refuses([&]{shaders.dispatch(pipeline,bindings,{d.properties.limits.maxComputeWorkGroupCount[0]+1,1,1});},"Oversized dispatch refuses before recording");
        gpu::ResourceCommands other(d.resource);gpu::ShaderCommands other_shaders(other,host);
        refuses([&]{other_shaders.dispatch(pipeline,bindings,{1,1,1});},"Cross-domain compute pipeline refuses");
        const auto count=host.objects->used();shaders.dispatch(pipeline,{}, {0,1,1});check(host.objects->used()==count,"Zero dispatch accesses no descriptor and allocates no invocation");
        auto excessive=program;excessive.threads[0]=d.properties.limits.maxComputeWorkGroupSize[0]+1;
        refuses([&]{shaders.compile(excessive);},"Oversized local dimensions refuse");
        auto unsupported=program;unsupported.capabilities.push_back(9);refuses([&]{shaders.compile(unsupported);},"Unenabled float16 capability refuses");
        auto invalid=program;invalid.spirv.back()=0;refuses([&]{shaders.compile(invalid);},"Malformed executable refuses before Vulkan pipeline creation");
        auto tiny=host;tiny.objects=std::make_shared<assets::AllocationBudget>(0);gpu::ShaderCommands denied(commands,tiny);
        refuses([&]{denied.compile(program);},"Pipeline object budget refuses before Vulkan object creation");check(!tiny.objects->used(),"Failed shader lease leaves the budget unchanged");
        shaders.dispatch(pipeline,bindings,{1,1,1});check(host.objects->used()>count,"Pending dispatch retains an invocation object");
        commands.discard();check(host.objects->used()==count,"Discard releases descriptor invocation without waiting");
        // Actual submitted owners survive retirement of all external handles.
        std::weak_ptr<gpu::ImageViewResource> view=first;shaders.dispatch(pipeline,bindings,{1,1,1});auto ticket=commands.submit(d.queue);
        bindings.clear();pipeline.reset();sampler.reset();first.reset();second.reset();smooth.reset();image_out.reset();wrong.clear();
        check(!view.expired(),"Submitted compute retains descriptor image views");ticket->wait();ticket.reset();
        check(view.expired(),"Completed compute releases retained image views");
    }
    check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used(),"Compute allocations and object leases are released");
    auto append=compute_program(root/"append",R"hlsl(AppendStructuredBuffer<uint> Output : register(u2);
[numthreads(1,1,1)] void main(){Output.Append(7);})hlsl");
    {gpu::ResourceCommands commands(d.resource,budgets);gpu::ShaderCommands shaders(commands,host);auto pipeline=shaders.compile(append);
        auto output=commands.create(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true),counter=commands.create(4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true);
        auto a=buffer_descriptor('u',2,output),b=buffer_descriptor('u',2,counter);b.binding=gpu::HlslCompiler::binding('c','u',2,true);
        shaders.dispatch(pipeline,{a,b},{2,1,1});auto bytes=commands.readback(d.queue,counter,0,4);uint32_t count;memcpy(&count,bytes.data(),4);
        check(count==2,"Separate reflected UAV counter receives actual atomic updates");bytes=commands.readback(d.queue,output,0,16);const uint32_t expected[]={7,7,0,0};
        check(!memcmp(bytes.data(),expected,16),"Append structured writes use the remapped counter and output descriptors");
    }
    check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used(),"Counter shader retires all owners");
}
static void compute_scalar_layout(Device& ordinary,const std::filesystem::path& root){
    auto program=compute_program(root,R"hlsl(StructuredBuffer<float3> Input : register(t1);
RWStructuredBuffer<float3> Output : register(u1);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID){Output[id.x]=Input[1-id.x];})hlsl");
    {gpu::ResourceCommands resources(ordinary.resource);gpu::ShaderCommands shaders(resources,shader_host(ordinary));
        refuses([&]{shaders.compile(program);},"DirectX packed float3 layout refuses when the device feature is not enabled");}
    Device enabled;enabled.init(false,true);gpu::ResourceBudgets budgets;auto host=shader_host(enabled);
    {gpu::ResourceCommands resources(enabled.resource,budgets);gpu::ShaderCommands shaders(resources,host);auto pipeline=shaders.compile(program);
        const float data[]={1,2,3,4,5,6},expected[]={4,5,6,1,2,3};
        auto input=resources.create(sizeof(data),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,data,sizeof(data));
        auto output=resources.create(sizeof(data),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,nullptr,0,true);
        shaders.dispatch(pipeline,{buffer_descriptor('t',1,input),buffer_descriptor('u',1,output)},{2,1,1});
        auto bytes=resources.readback(enabled.queue,output,0,sizeof(data));
        check(!memcmp(bytes.data(),expected,sizeof(data)),"Enabled scalar layout preserves native 12-byte structured element stride");
    }
    check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used(),"Scalar-layout resources and pipelines retire");
}
struct ShaderFixture {
    Device& device;gpu::ResourceBudgets budgets;gpu::ShaderHost host;
    std::shared_ptr<frontend::GpuShaderRuntime> backend;
    std::shared_ptr<frontend::IniRuntime> ini;
    std::string why;unsigned frame=0;
    ShaderFixture(Device& d,const std::filesystem::path& root,const char* ini_text,bool shader_hooks=false):device(d),host(shader_host(d)){
        std::filesystem::create_directories(root/"Mods");write(root/"Mods/mod.ini",ini_text);
        frontend::RuntimeConfig config{root,{},root/"settings","shader",root/"Mods"};
        config.shader_hooks=shader_hooks;
        backend=std::make_shared<frontend::GpuShaderRuntime>(d.resource,host,L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll",budgets);
        backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        ini=frontend::IniRuntime::load(config,backend,why);if(!ini)throw std::runtime_error("Load shader fixture: "+why);++checks;finish();
    }
    void finish(){auto ticket=backend->resources().finish();if(ticket)ticket->wait();}
    void present(unsigned frame_value=0){backend->resources().begin(device.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present({},frame_value?frame_value:++frame,0,why))throw std::runtime_error("Shader Present: "+why);++checks;finish();}
    float variable(const char* name){return ini->status().variables.at(std::string("$\\compute\\")+name);}
    void release(){ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used() && !host.sampler_objects->used(),"Original shader generation retires resource and object budgets");}
};
static void original_compute(Device& d,const std::filesystem::path& root){
    std::filesystem::create_directories(root/"Mods/sub");
    write(root/"Mods/sub/values.hlsl","#define SCALE 2\n");
    write(root/"Mods/compute.hlsl",R"hlsl(#include "sub/values.hlsl"
Texture1D<float4> Parameters : register(t120);
RWStructuredBuffer<float> Output : register(u3);
[numthreads(1,1,1)] void main(){Output[0]=Parameters.Load(int2(0,0)).x*SCALE;})hlsl");
    write(root/"other.hlsl",R"hlsl(RWStructuredBuffer<float> Output : register(u3);
[numthreads(1,1,1)] void main(){Output[0]=99;})hlsl");
    ShaderFixture f(d,root,R"ini(namespace = compute
[Constants]
global $first = 0
global $nested = 0
global $restored = 0
global $limited = 0
global $calls = 0
global $phase = 0
global $null_value = 0
global $outside = 0
x = 3
[ResourceOutput]
type = StructuredBuffer
stride = 4
data = R32_FLOAT 0
bind_flags = unordered_access shader_resource
[CustomShaderMain]
cs = compute.hlsl
cs-u3 = ResourceOutput
dispatch = iniparams->width, 1, 1
store = $first, cs-u3, 0
run = CustomShaderNested
store = $nested, cs-u3, 0
run = CustomShaderNull
store = $null_value, cs-u3, 0
x = 7
dispatch = 1, 1, 1
store = $restored, cs-u3, 0
post $phase = 1
[CustomShaderNested]
cs = other.hlsl
dispatch = 1, 1, 1
[CustomShaderNull]
cs = null
dispatch = 1, 1, 1
[CustomShaderLimited]
max_executions_per_frame = 1
$calls = $calls + 1
[Present]
pre run = CustomShaderMain
x = 9
dispatch = 1, 1, 1
store = $outside, cs-u3, 0
x = 7
pre run = CustomShaderLimited
pre run = CustomShaderLimited
$limited = $calls
reset_per_frame_limits = CustomShaderLimited
pre run = CustomShaderLimited
)ini");
    f.present();check(f.variable("first")==6 && f.variable("nested")==99 && f.variable("null_value")==99 && f.variable("restored")==14,
        "Original CustomShader runs direct compute, root fallback, inherited descriptors and nested shader restoration");
    check(f.variable("phase")==1 && f.variable("limited")==1 && f.variable("calls")==2,"Original post phase, per-frame limit and reset share source state");
    check(f.backend->dispatches()==3,"Explicit null compute stage produces no dispatch");
    check(f.variable("outside")==14,"Returning from CustomShader restores the unbound outer compute stage");
    f.present();check(f.variable("first")==14 && f.variable("calls")==4 && f.backend->dispatches()==6,"Original shader pipeline is reused across frame-counter resets");
    f.release();
}
static void original_shader_scopes(Device& d,const std::filesystem::path& root){
    ShaderFixture f(d,root,R"ini(namespace = compute
[Constants]
global $unreachable = 0
global $post_shader = 0
global $post_root = 0
o0 = ref set_viewport ResourceOriginal
ps-u1 = ref ResourceBuffer
[ResourceOriginal]
type = Texture2D
width = 4
height = 5
array = 1
mips = 1
msaa = 1
format = R32_FLOAT
bind_flags = render_target shader_resource
[ResourceInside]
type = Texture2D
width = 8
height = 9
array = 1
mips = 1
msaa = 1
format = R32_FLOAT
bind_flags = render_target shader_resource
[ResourceBuffer]
type = StructuredBuffer
stride = 4
data = R32_FLOAT 2
bind_flags = shader_resource unordered_access
[ResourceOther]
type = StructuredBuffer
stride = 4
data = R32_FLOAT 3
bind_flags = shader_resource unordered_access
[CustomShaderAbort]
o0 = ref set_viewport ResourceInside
ps-u1 = ref ResourceOther
cs-u3 = ref ResourceOther
cs-t8 = ref ResourceOther
handling = abort
post $post_shader = 1
[Present]
pre run = CustomShaderAbort
$unreachable = 1
post $post_root = 1
)ini");
    using T=ResourceCopyTargetType;auto& resources=f.backend->resources();
    const auto original=resources.binding(0,T::RENDER_TARGET,0).image->image();
    const auto uav=resources.binding(L'p',T::UNORDERED_ACCESS_VIEW,1).buffer->buffer();
    f.present();
    check(!f.variable("unreachable") && !f.variable("post_shader") && f.variable("post_root")==1,"CustomShader abort shares nested state but root post still executes");
    check(resources.binding(0,T::RENDER_TARGET,0).image->image()==original && resources.binding(L'p',T::UNORDERED_ACCESS_VIEW,1).buffer->buffer()==uav &&
        resources.viewport().Width==4 && resources.viewport().Height==5,"CustomShader restores OM resources and viewport after abort");
    check(resources.binding(L'c',T::UNORDERED_ACCESS_VIEW,3).buffer && !resources.binding(L'c',T::SHADER_RESOURCE,8),
        "Ordinary compute UAV survives CustomShader scope; its conflicting SRV remains null");
    // As the original (ParseCustomShaderSections): a CustomShader that does not compile is skipped
    // with a warning and the candidate loads; it cannot disturb an already-working generation.
    auto bad_root=root/"bad";std::filesystem::create_directories(bad_root/"Mods");
    write(bad_root/"Mods/mod.ini","[CustomShaderDormant]\ncs = broken.hlsl\n");write(bad_root/"Mods/broken.hlsl","this is invalid HLSL");
    auto bad=std::make_shared<frontend::GpuShaderRuntime>(d.resource,f.host,L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll",f.budgets);
    bad->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    frontend::RuntimeConfig config{bad_root,{},bad_root/"settings","bad",bad_root/"Mods"};std::string why;
    {auto candidate=frontend::IniRuntime::load(config,bad,why);
        const auto status=candidate?candidate->status():frontend::RuntimeStatus{};
        check(candidate && std::any_of(status.diagnostic_messages.begin(),status.diagnostic_messages.end(),[](const std::string& m){
            return m.find("Error compiling custom shader")!=std::string::npos && m.find("compilation failed")!=std::string::npos;}),
            "Dormant invalid HLSL is skipped with the original warning and the candidate loads");
        if(auto ticket=bad->resources().finish())ticket->wait();candidate.reset();}
    bad.reset();
    f.present();check(f.variable("post_root")==1,"Prior generation remains executable beside the candidate");
    f.release();
    auto recursive_root=root/"recursive";
    ShaderFixture recursive(d,recursive_root,"namespace = compute\n[CustomShaderRecursive]\nrun = CustomShaderRecursive\n[Present]\nrun = CustomShaderRecursive\n");
    recursive.backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    declined(*recursive.ini,recursive.backend->resources(),recursive.ini->present({},1,0,why),"",
             "Recursive CustomShader calls end at the shared execution budget and are said");
    recursive.release();
}
