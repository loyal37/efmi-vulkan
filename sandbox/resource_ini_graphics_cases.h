static const char* ini_graphics_hlsl=R"hlsl(struct V{float4 position:SV_Position;nointerpolation uint2 ids:TEXCOORD0;};
#ifdef VERTEX_SHADER
V main(uint id:SV_VertexID,uint instance:SV_InstanceID){V v;uint i=id%3;v.position=float4(i==2?6:-2,i==1?6:-2,1,2);v.ids=uint2(id,instance);return v;}
#else
Texture1D<float4> Parameters:register(t120);
float4 main(V v):SV_Target{return float4(Parameters.Load(int2(0,0)).x,v.ids,v.position.w);}
#endif
)hlsl";
static const char* ini_graphics_resources=R"ini(namespace = compute
[Constants]
x = 3
o0 = ref set_viewport ResourceOutput
clear = o0 0 0 0 0
[ResourceOutput]
type = Texture2D
width = 4
height = 4
array = 1
mips = 1
msaa = 1
format = R32G32B32A32_FLOAT
bind_flags = render_target shader_resource
[ResourceIndices]
type = Buffer
format = R32_UINT
data = R32_UINT 99 99 3 4 5
bind_flags = index_buffer
[ResourceAutoIndices]
type = Buffer
format = R32_UINT
data = R32_UINT 0 1 2
bind_flags = index_buffer
[ResourceVertices]
type = Buffer
stride = 16
data = R32_FLOAT 0 0 0 0 0 0 0 0 0 0 0 0
bind_flags = vertex_buffer
[ResourceArguments]
type = Buffer
data = R32_UINT 111 222 3 2 6 7
misc_flags = drawindirect_args
[ResourceIndexedArguments]
type = Buffer
data = R32_UINT 111 222 3 2 2 4294967293 7
misc_flags = drawindirect_args
)ini";
static std::pair<gpu::HlslProgram,gpu::HlslProgram> ini_graphics_programs(const std::filesystem::path& root){
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/graphics.hlsl",ini_graphics_hlsl);
    gpu::HlslCompiler compiler(L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll");gpu::HlslRequest request;request.source=root/"Mods/graphics.hlsl";request.working_directory=root;
    std::pair<gpu::HlslProgram,gpu::HlslProgram> programs;std::string why;request.stage='v';if(!compiler.compile(request,programs.first,why))throw std::runtime_error(why);++checks;
    request.stage='p';if(!compiler.compile(request,programs.second,why))throw std::runtime_error(why);++checks;return programs;
}
static void native_ini_parameters(ID3D11Device* device,ID3D11DeviceContext* context,float x){
    const float data[]={x,0,0,0};D3D11_TEXTURE1D_DESC description{1,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
    D3D11_SUBRESOURCE_DATA initial{data,16,0};ImageNativeReference::Ptr<ID3D11Texture1D> image;ImageNativeReference::Ptr<ID3D11ShaderResourceView> view;
    check(SUCCEEDED(device->CreateTexture1D(&description,&initial,&image)) && SUCCEEDED(device->CreateShaderResourceView(image.Get(),nullptr,&view)),"Native original graphics parameters");
    auto* handle=view.Get();context->PSSetShaderResources(120,1,&handle);
}
static void original_ini_graphics(Device& d,const std::filesystem::path& root){
    const auto programs=ini_graphics_programs(root);const auto& vs=programs.first;const auto& ps=programs.second;
    struct Case{const char* name;const char* command;bool indexed=false,instanced=false,automatic=false;};
    const Case cases[]={
        {"direct","draw = 3, 6\n"},{"instanced","drawinstanced = 3, 2, 6, 7\n",false,true},
        {"indexed","ib = ref ResourceIndices\ndrawindexed = 3, 2, -3\n",true},
        {"indexed-instanced","ib = ref ResourceIndices\ndrawindexedinstanced = 3, 2, 2, -3, 7\n",true,true},
        {"indirect","drawinstancedindirect = ResourceArguments, 8\n",false,true},
        {"indexed-indirect","ib = ref ResourceIndices\ndrawindexedinstancedindirect = ResourceIndexedArguments, 8\n",true,true},
        {"auto-vertex","vb0 = ref ResourceVertices\ndraw = auto\n",false,false,true},
        {"auto-index","ib = ref ResourceAutoIndices\ndrawindexed = auto\n",true,false,true}
    };
    for(const auto& test:cases){const auto directory=root/test.name;std::filesystem::create_directories(directory/"Mods");write(directory/"Mods/graphics.hlsl",ini_graphics_hlsl);
        const std::string text=std::string(ini_graphics_resources)+"[CustomShaderMain]\nvs = graphics.hlsl\nps = graphics.hlsl\ntopology = triangle_list\ncull = none\ndepth_enable = false\n"+
            test.command+"[Present]\nrun = CustomShaderMain\n";ShaderFixture f(d,directory,text.c_str());f.present();
        auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
        GraphicsReferenceOptions options;options.first_vertex=test.automatic?0:6;
        if(test.instanced){options.instances=2;options.first_instance=7;}
        if(test.indexed){options.indices=test.automatic?std::vector<uint32_t>{0,1,2}:std::vector<uint32_t>{99,99,3,4,5};options.first_index=test.automatic?0:2;options.base_vertex=test.automatic?0:-3;}
        const auto expected=native_linked_graphics(vs,ps,options,[](auto* device,auto* context){native_ini_parameters(device,context,3);});
        check(actual==expected,"Original INI graphics draw arguments and parameters match native DX11 bytes");
        check(f.backend->draws()==1 && f.backend->graphics_pipelines()==1,"Original INI emits one graphics invocation and caches its pipeline");
        f.present();check(f.backend->draws()==2 && f.backend->graphics_pipelines()==1,"Original INI reuses pipeline across source calls");
        check(f.backend->graphics_state().topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED && f.backend->graphics_state().depth.DepthEnable,"Original shader call restores source fixed-function state");
        output.reset();f.release();
    }
}
static void original_ini_graphics_nesting(Device& d,const std::filesystem::path& root){
    const auto programs=ini_graphics_programs(root);
    const std::string ini=std::string(ini_graphics_resources)+R"ini(
[CustomShaderMain]
vs = graphics.hlsl
ps = graphics.hlsl
topology = triangle_list
cull = none
depth_enable = false
draw = 3, 0
run = CustomShaderMasked
run = CustomShaderNullVertex
run = CustomShaderNullPixel
x = 9
draw = 3, 0
[CustomShaderMasked]
blend_state_merge = true
mask = 0
x = 6
draw = 3, 0
[CustomShaderNullVertex]
vs = null
draw = 3, 0
[CustomShaderNullPixel]
ps = null
draw = 3, 0
[Present]
run = CustomShaderMain
draw = 3, 0
)ini";
    ShaderFixture f(d,root,ini.c_str());f.present();
    auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
    const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
    const auto expected=native_linked_graphics(programs.first,programs.second,{},[](auto* device,auto* context){native_ini_parameters(device,context,9);});
    check(actual==expected,"Nested source null stages and write masks restore caller stages/state");
    check(f.backend->draws()==4 && f.backend->graphics_pipelines()==3,"Null VS and unbound outer VS produce no GPU draw; null PS remains a graphics invocation");
    output.reset();f.release();
}
static void original_graphics_creation_defaults(){
    ImageNativeReference native;auto state=frontend::source_graphics_defaults();
    state.depth.DepthEnable=FALSE;state.depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;state.depth.DepthFunc=D3D11_COMPARISON_GREATER;
    state.depth.StencilReadMask=3;state.depth.StencilWriteMask=7;state.depth.FrontFace.StencilFunc=state.depth.BackFace.StencilFunc=D3D11_COMPARISON_FUNC(0);
    state.blend.RenderTarget[0].SrcBlend=D3D11_BLEND(0);state.blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP(0);
    state.blend.RenderTarget[0].RenderTargetWriteMask=5;state.blend.RenderTarget[1].RenderTargetWriteMask=2;
    ImageNativeReference::Ptr<ID3D11DepthStencilState> depth;ImageNativeReference::Ptr<ID3D11BlendState> blend;
    check(SUCCEEDED(native.device->CreateDepthStencilState(&state.depth,&depth)) && SUCCEEDED(native.device->CreateBlendState(&state.blend,&blend)),"Native creation ignores inactive source-state fields");
    D3D11_DEPTH_STENCIL_DESC z;D3D11_BLEND_DESC b;depth->GetDesc(&z);blend->GetDesc(&b);
    const auto effective=frontend::source_graphics_normalize(state);
    check(equal_source_depth(effective.depth,z) && equal_source_blend(effective.blend,b),"Source graphics normalization matches native effective descriptions");
    state.depth.StencilEnable=TRUE;depth.Reset();check(FAILED(native.device->CreateDepthStencilState(&state.depth,&depth)),"Native active invalid stencil state fails creation");
    check(equal_source_depth(frontend::source_graphics_normalize(state).depth,frontend::source_graphics_defaults().depth),"Failed original stencil-state creation binds native null-state defaults");
}
static void original_ini_graphics_sampling(Device& d,const std::filesystem::path& root){
    const float texels[]={1,2,3,4};
    for(bool point:{false,true}){const auto directory=root/(point?"point":"linear");
        write_image_dds(directory/"Mods/image.dds",{"samples",DXGI_FORMAT_R32_FLOAT,3,2,2,1,1,1,4,1});
        {std::fstream file(directory/"Mods/image.dds",std::ios::in|std::ios::out|std::ios::binary);file.seekp(148);file.write(reinterpret_cast<const char*>(texels),sizeof(texels));check(bool(file),"Write source graphics sampler texels");}
        std::string shader=ini_graphics_hlsl;const auto first=shader.find("Texture1D<float4>");const auto last=shader.find("#endif",first);
        shader.replace(first,last-first,"Texture2D<float> Image:register(t0);SamplerState Filter:register(s0);SamplerState Other:register(s3);\nfloat4 main(V v):SV_Target{return float4(Image.SampleLevel(Filter,float2(.5,.5),0),Image.SampleLevel(Other,float2(.5,.5),0),0,v.position.w);}\n");
        std::filesystem::create_directories(directory/"Mods");write(directory/"Mods/graphics.hlsl",shader.c_str());
        gpu::HlslCompiler compiler(L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll");gpu::HlslRequest request;request.source=directory/"Mods/graphics.hlsl";request.working_directory=directory;gpu::HlslProgram vs,ps;std::string why;
        request.stage='v';if(!compiler.compile(request,vs,why))throw std::runtime_error(why);request.stage='p';if(!compiler.compile(request,ps,why))throw std::runtime_error(why);
        const std::string ini=std::string(ini_graphics_resources)+"[ResourceImage]\nfilename = image.dds\n[CustomShaderMain]\nvs = graphics.hlsl\nps = graphics.hlsl\ntopology = triangle_list\ncull = none\ndepth_enable = false\nps-t0 = ref ResourceImage\nsampler = "+
            (point?"point_filter":"linear_filter")+"\ndraw = 3, 0\n[Present]\nrun = CustomShaderMain\n";
        ShaderFixture f(d,directory,ini.c_str());VkSamplerCreateInfo sampler={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.minFilter=sampler.magFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler.maxLod=1;
        f.backend->set_sampler(L'p',3,&sampler);f.present();auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
        const auto expected=native_linked_graphics(vs,ps,{},[&](auto* device,auto* context){
            D3D11_TEXTURE2D_DESC td{2,2,1,1,DXGI_FORMAT_R32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};D3D11_SUBRESOURCE_DATA initial{texels,8,0};
            ImageNativeReference::Ptr<ID3D11Texture2D> image;ImageNativeReference::Ptr<ID3D11ShaderResourceView> view;
            check(SUCCEEDED(device->CreateTexture2D(&td,&initial,&image)) && SUCCEEDED(device->CreateShaderResourceView(image.Get(),nullptr,&view)),"Native source sampler image");auto* t=view.Get();context->PSSetShaderResources(0,1,&t);
            D3D11_SAMPLER_DESC desc{};desc.Filter=point?D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT:D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;desc.MaxAnisotropy=1;desc.ComparisonFunc=D3D11_COMPARISON_ALWAYS;desc.MaxLOD=1;
            ImageNativeReference::Ptr<ID3D11SamplerState> first,other;check(SUCCEEDED(device->CreateSamplerState(&desc,&first)),"Native original comparison-filter sampler");
            desc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;check(SUCCEEDED(device->CreateSamplerState(&desc,&other)),"Native independent sampler slot");auto* a=first.Get();auto* b=other.Get();context->PSSetSamplers(0,1,&a);context->PSSetSamplers(3,1,&b);
        });
        check(actual==expected,"Original INI comparison-filter ordinary sampling matches native and preserves other sampler slots");
        for(unsigned i=0;i<16;++i)check(value(actual,i*16)==(point?4.0f:2.5f) && value(actual,i*16+4)==4,"Original source filter retains point/linear distinction");
        check(!f.backend->graphics_state().pixel_samplers[0],"CustomShader restores original pixel sampler override");output.reset();f.release();
    }
}
static void original_ini_graphics_state_pixels(Device& d,const std::filesystem::path& root){
    const auto programs=ini_graphics_programs(root);
    const std::string ini=std::string(ini_graphics_resources)+R"ini(
[CustomShaderMain]
vs = graphics.hlsl
ps = graphics.hlsl
topology = triangle_list
cull = none
depth_enable = false
blend = add blend_factor zero
alpha = add blend_factor zero
blend_factor[0] = 0.25
blend_factor[1] = 0.5
blend_factor[2] = 0.75
blend_factor[3] = 0.5
scissor_enable = true
draw = 3, 0
[Present]
run = CustomShaderMain
)ini";
    ShaderFixture f(d,root,ini.c_str());f.backend->set_scissor({1,1,3,4});f.present();auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
    const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
    const auto expected=native_linked_graphics(programs.first,programs.second,{},[](auto* device,auto* context){native_ini_parameters(device,context,3);
        D3D11_BLEND_DESC desc=CD3D11_BLEND_DESC(D3D11_DEFAULT);auto& b=desc.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_BLEND_FACTOR;
        ImageNativeReference::Ptr<ID3D11BlendState> blend;check(SUCCEEDED(device->CreateBlendState(&desc,&blend)),"Native original graphics blend state");const float factors[]={.25f,.5f,.75f,.5f};context->OMSetBlendState(blend.Get(),factors,UINT_MAX);
        D3D11_RASTERIZER_DESC rs=CD3D11_RASTERIZER_DESC(D3D11_DEFAULT);rs.CullMode=D3D11_CULL_NONE;rs.ScissorEnable=TRUE;ImageNativeReference::Ptr<ID3D11RasterizerState> raster;
        check(SUCCEEDED(device->CreateRasterizerState(&rs,&raster)),"Native original graphics scissor state");context->RSSetState(raster.Get());const D3D11_RECT rect{1,1,3,4};context->RSSetScissorRects(1,&rect);
    });
    check(actual==expected,"Original INI blending and inherited scissor rectangle match native DX11 pixels");output.reset();f.release();
}
static void original_ini_graphics_depth(Device& d,const std::filesystem::path& root){
    for(bool null_pixel:{false,true})for(bool greater:{false,true}){const auto directory=root/(std::string(null_pixel?"null":"pixel")+(greater?"-greater":"-less"));
        const auto programs=ini_graphics_programs(directory);
        const std::string ini=std::string(ini_graphics_resources)+R"ini(
[ResourceDepth]
type = Texture2D
width = 4
height = 4
array = 1
mips = 1
msaa = 1
format = D32_FLOAT
bind_flags = depth_stencil shader_resource
[CustomShaderMain]
vs = graphics.hlsl
)ini"+"ps = "+(null_pixel?"null":"graphics.hlsl")+"\ntopology = triangle_list\ncull = none\ndepth_func = "+(greater?"greater":"less")+R"ini(
od = ref ResourceDepth
clear = od depth 0.25
draw = 3, 0
[Present]
run = CustomShaderMain
od = ref ResourceDepth
)ini";
        ShaderFixture f(d,directory,ini.c_str());f.present();auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        auto depth=f.backend->resources().binding(L'\0',ResourceCopyTargetType::DEPTH_STENCIL_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
        const auto expected=native_linked_graphics(programs.first,programs.second,{},[&](auto* device,auto* context){native_ini_parameters(device,context,3);
            D3D11_TEXTURE2D_DESC td{4,4,1,1,DXGI_FORMAT_D32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL,0,0};ImageNativeReference::Ptr<ID3D11Texture2D> image;ImageNativeReference::Ptr<ID3D11DepthStencilView> view;
            check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&image)) && SUCCEEDED(device->CreateDepthStencilView(image.Get(),nullptr,&view)),"Native original INI depth target");
            context->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH,.25f,0);ImageNativeReference::Ptr<ID3D11RenderTargetView> color;context->OMGetRenderTargets(1,&color,nullptr);auto* rtv=color.Get();context->OMSetRenderTargets(1,&rtv,view.Get());
            auto desc=CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);desc.DepthFunc=greater?D3D11_COMPARISON_GREATER:D3D11_COMPARISON_LESS;ImageNativeReference::Ptr<ID3D11DepthStencilState> state;
            check(SUCCEEDED(device->CreateDepthStencilState(&desc,&state)),"Native original INI depth comparison");context->OMSetDepthStencilState(state.Get(),0);if(null_pixel)context->PSSetShader(nullptr,nullptr,0);
        });
        check(actual==expected,"Original INI depth comparison and null-pixel color preservation match native DX11");
        const auto depth_bytes=f.backend->resources().commands().readback(d.queue,depth,image_region(depth,0,0,1,VK_IMAGE_ASPECT_DEPTH_BIT));
        for(unsigned i=0;i<16;++i)check(value(depth_bytes,i*4)==(greater?.5f:.25f),"Original INI null pixel shader still writes accepted depth");
        output.reset();depth.reset();f.release();
    }
}
static void original_ini_graphics_caller_and_failure(Device& d,const std::filesystem::path& root){
    for(bool automatic:{false,true}){const auto directory=root/(automatic?"auto-caller":"caller");const auto programs=ini_graphics_programs(directory);
        const std::string ini=std::string(ini_graphics_resources)+R"ini(
[CustomShaderMain]
vs = graphics.hlsl
ps = graphics.hlsl
topology = triangle_list
cull = none
depth_enable = false
)ini"+(automatic?"ib = ref ResourceAutoIndices\ndrawindexedinstanced = auto\n":"ib = ref ResourceIndices\ndraw = from_caller\n")+R"ini(
[TextureOverrideDraw]
hash = 12345678
run = CustomShaderMain
[Present]
run = CustomShaderMain
)ini";
        ShaderFixture f(d,directory,ini.c_str());f.present();check(f.backend->draws()==0,"Original caller-dependent draw without a draw context is a no-op");
        f.backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        frontend::RuntimeDraw call;call.hash=0x12345678;call.count=3;call.first=2;call.base=-3;call.instances=2;call.first_instance=7;assets::FilePlan plan;
        check(f.ini->draw(call,plan,f.why),"Original INI graphics receives caller draw metadata");f.finish();auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));GraphicsReferenceOptions options;options.instances=2;options.first_instance=7;
        options.indices=automatic?std::vector<uint32_t>{0,1,2}:std::vector<uint32_t>{99,99,3,4,5};options.first_index=automatic?0:2;options.base_vertex=automatic?0:-3;
        const auto expected=native_linked_graphics(programs.first,programs.second,options,[](auto* device,auto* context){native_ini_parameters(device,context,3);});
        check(actual==expected && f.backend->draws()==1,"Source from-caller/automatic instancing agrees with native argument semantics");output.reset();f.release();
    }
    const auto directory=root/"refused";ini_graphics_programs(directory);
    const std::string ini=std::string(ini_graphics_resources)+R"ini(
[CustomShaderMain]
vs = graphics.hlsl
ps = graphics.hlsl
topology = triangle_list
cull = none
depth_enable = false
draw = 3, 0
run = CustomShaderUnsupported
[CustomShaderUnsupported]
topology = point_list
draw = 3, 0
[Present]
run = CustomShaderMain
)ini";
    ShaderFixture f(d,directory,ini.c_str());f.backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    const bool presented=f.ini->present({},1,0,f.why);
    check(f.backend->draws()==1 && f.backend->graphics_state().topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,"A draw the layer cannot make restores nested source state after preceding recorded work");
    // The nested draw does nothing; the draw recorded before it is the scope's work, submitted unless the
    // failing draw had recorded part of its own (then the scope is dropped and the target stays as it was).
    const bool dropped=f.backend->resources().failed();
    declined(*f.ini,f.backend->resources(),presented,"topology","Unsupported source graphics leaves its draw undone and is said");
    auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
    const auto bytes=f.backend->resources().commands().readback(d.queue,output,image_region(output));
    check(std::all_of(bytes.begin(),bytes.end(),[](auto b){return !b;})==dropped,"The draw before the refused one is drawn, or a dropped scope leaves the committed target unchanged");output.reset();f.release();
}
