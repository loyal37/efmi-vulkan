// Original layout creation signatures and INI edits, with native IA references.
static const char* layout_resources=R"ini(namespace = compute
[Constants]
global $phase = 0
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
[ResourceVertices]
type = Buffer
stride = 32
bind_flags = vertex_buffer
data = R32_FLOAT 1 2 3 4 10 20 30 40 2 4 6 8 20 40 60 80 3 6 9 12 30 60 90 120 4 8 12 16 40 80 120 160 5 10 15 20 50 100 150 200 6 12 18 24 60 120 180 240
)ini";
static std::vector<gpu::HlslProgram> layout_programs(const std::filesystem::path& root,bool swapped=false,bool unused=false){
    std::filesystem::create_directories(root/"Mods");std::string code=R"hlsl(struct O{float4 position:SV_Position;float4 color:COLOR0;};
#ifdef VERTEX_SHADER
struct I{)hlsl";
    code+=swapped?"float4 b:BETA;float4 a:ALPHA;":"float4 a:ALPHA;float4 b:BETA;";
    code+="uint id:SV_VertexID;}; O main(I i){O o;uint id=i.id%3;o.position=float4(id==2?3:-1,id==1?3:-1,0,1);o.color=";
    code+=unused?"i.a;":"i.a*100+i.b;";code+=R"hlsl(return o;}
#else
float4 main(O o):SV_Target{return o.color;}
#endif
)hlsl";write(root/"Mods/layout.hlsl",code.c_str());return geometry_programs(root/"Mods/layout.hlsl","vp");
}
static std::vector<frontend::SourceVertexElement> layout_elements(){return {{"alpha",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0},{"beta",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16}};}
static std::vector<uint8_t> native_layout_pixels(const gpu::HlslProgram& vertex,const gpu::HlslProgram& pixel,const gpu::HlslProgram& creation,
    const frontend::SourceInputLayout& layout,GraphicsReferenceOptions options={}){
    return native_linked_graphics(vertex,pixel,options,[&](auto* d,auto* c){const auto elements=layout.descriptions();ImageNativeReference::Ptr<ID3D11InputLayout> input;
        check(SUCCEEDED(d->CreateInputLayout(elements.data(),UINT(elements.size()),creation.dxbc.data(),creation.dxbc.size(),&input)),"Native original layout creation signature");
        c->IASetInputLayout(input.Get());float data[48];for(unsigned i=0;i<6;++i)for(unsigned j=0;j<4;++j){data[i*8+j]=float((i+1)*(j+1));data[i*8+4+j]=data[i*8+j]*10;}
        D3D11_BUFFER_DESC description{sizeof(data),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA initial{data,0,0};ImageNativeReference::Ptr<ID3D11Buffer> buffer;
        check(SUCCEEDED(d->CreateBuffer(&description,&initial,&buffer)),"Native layout vertex records");auto* handle=buffer.Get();const UINT stride=32,offset=0;c->IASetVertexBuffers(0,1,&handle,&stride,&offset);
    });
}
static void source_layout_creation(const std::filesystem::path& root){
    const auto programs=layout_programs(root),unused=layout_programs(root/"unused",false,true);ImageNativeReference native;
    auto compare=[&](std::vector<frontend::SourceVertexElement> elements,const gpu::HlslProgram& program,bool expected){
        std::vector<D3D11_INPUT_ELEMENT_DESC> descriptions;for(const auto& e:elements)descriptions.push_back({e.semantic.c_str(),e.index,e.format,e.slot,e.offset,e.classification,e.step});
        ImageNativeReference::Ptr<ID3D11InputLayout> result;const bool accepted=SUCCEEDED(native.device->CreateInputLayout(descriptions.data(),UINT(descriptions.size()),program.dxbc.data(),program.dxbc.size(),&result));
        bool translated=true;try{frontend::SourceInputLayout layout(elements,program.input_signature);}catch(const frontend::InvalidInputLayout&){translated=false;}
        check(accepted==expected && translated==accepted,"Owned source layout creation agrees with native success/failure");};
    const auto normal=layout_elements();compare(normal,programs[0],true);
    auto changed=normal;changed.pop_back();compare(changed,programs[0],false);compare(changed,unused[0],false);
    changed=normal;changed.push_back({"EXTRA",0,DXGI_FORMAT_R32_FLOAT,0,0});compare(changed,programs[0],true);
    changed.back().format=DXGI_FORMAT_UNKNOWN;compare(changed,programs[0],false);
    changed=normal;changed.push_back(changed[0]);compare(changed,programs[0],false);
    changed=normal;changed[1].offset=1;compare(changed,programs[0],false);
    changed=normal;changed[1].classification=D3D11_INPUT_PER_INSTANCE_DATA;changed[1].step=1;compare(changed,programs[0],false);
    changed=normal;changed[0].step=changed[1].step=1;compare(changed,programs[0],false);
    changed=normal;changed[1].slot=31;compare(changed,programs[0],true);changed[1].slot=32;compare(changed,programs[0],false);
    for(unsigned offset:{2048u,2052u,0xfffffffcu}){changed=normal;changed[1].offset=offset;compare(changed,programs[0],true);}
    for(unsigned format=0;format<116;++format){changed=normal;changed[0].format=DXGI_FORMAT(format);UINT support=0;
        const auto hr=native.device->CheckFormatSupport(DXGI_FORMAT(format),&support);compare(changed,programs[0],SUCCEEDED(hr) && (support&D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER));}
    changed=normal;changed[0].format=DXGI_FORMAT_R8_UINT;changed[1].format=DXGI_FORMAT_R16G16_UINT;changed[1].offset=D3D11_APPEND_ALIGNED_ELEMENT;
    frontend::SourceInputLayout append(changed,programs[0].input_signature);check(append.registers().at(1).offset==4,"Source APPEND follows format byte-width alignment");
    changed[1].slot=1;frontend::SourceInputLayout separate(changed,programs[0].input_signature);check(separate.registers().at(1).offset==0,"Source APPEND starts at zero in another slot");
}
static void original_layout_pixels(Device& d,const std::filesystem::path& root){
    const auto first=layout_programs(root/"signature0"),second=layout_programs(root/"signature1",true);
    for(unsigned which=0;which<8;++which){const bool instance=which>=4;const bool swapped=which<4 && (which&1);const bool creation_swapped=which<4 && (which&2);
        const auto directory=root/std::to_string(which);std::filesystem::create_directories(directory/"Mods");
        std::filesystem::copy_file(root/(swapped?"signature1":"signature0")/"Mods/layout.hlsl",directory/"Mods/layout.hlsl",std::filesystem::copy_options::overwrite_existing);
        auto elements=layout_elements();GraphicsReferenceOptions options;
        if(instance){for(auto& e:elements){e.classification=D3D11_INPUT_PER_INSTANCE_DATA;e.step=(which-4)/2;}options.instances=2;options.first_instance=which%2?2:0;}
        const auto& creation=creation_swapped?second[0]:first[0];const auto& vertex=swapped?second[0]:first[0];
        auto layout=std::make_shared<frontend::SourceInputLayout>(elements,creation.input_signature);
        const std::string ini=std::string(layout_resources)+R"ini(
[CustomShaderMain]
vs = layout.hlsl
ps = layout.hlsl
cull = none
depth_enable = false
topology = triangle_list
vb0 = ref ResourceVertices
)ini"+(instance?"drawinstanced = 3, 2, 0, "+std::to_string(options.first_instance):"draw = 3, 0")+"\n[Present]\nrun = CustomShaderMain\n";
        ShaderFixture f(d,directory,ini.c_str());f.backend->set_input_layout(layout);f.present();auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));const auto expected=native_layout_pixels(vertex,first[1],creation,*layout,options);
        check(actual==expected,"Original owned vertex register mapping and instance base/rate match native pixels");
        if(instance && elements[0].step==0){const float color[]={110.f*(1+options.first_instance),220.f*(1+options.first_instance),330.f*(1+options.first_instance),440.f*(1+options.first_instance)};
            for(unsigned i=0;i<16;++i)check(!memcmp(actual.data()+i*16,color,16),"Zero instance step preserves native StartInstanceLocation");}
        f.present();check(f.backend->graphics_pipelines()==1,"Input layout state reuses an identical graphics pipeline");output.reset();f.release();
    }
}
static void original_layout_edits(Device& d,const std::filesystem::path& root){
    const auto programs=layout_programs(root);const auto original=std::make_shared<frontend::SourceInputLayout>(layout_elements(),programs[0].input_signature);
    for(unsigned which=0;which<6;++which){const auto directory=root/std::to_string(which);std::filesystem::create_directories(directory/"Mods");
        std::filesystem::copy_file(root/"Mods/layout.hlsl",directory/"Mods/layout.hlsl",std::filesystem::copy_options::overwrite_existing);
        const std::string ini=std::string(layout_resources)+R"ini(
[CustomShaderMain]
vs = layout.hlsl
ps = layout.hlsl
cull = none
depth_enable = false
topology = triangle_list
vb0 = ref ResourceVertices
if $phase == 0
draw = 3, 0
)ini"+(which==5?"vb0->elementformat(ALPHA, 0) = R32G32_FLOAT":"vb0->elementoffset(ALPHA, 0) = "+std::string(which==2?"1":"16"))+R"ini(
run = CustomShaderConsume
endif
draw = 3, 0
$phase = $phase + 1
[CustomShaderConsume]
)ini"+(which==1?"dispatch = 0, 1, 1":"draw = 0, 0")+R"ini(
[Present]
run = CustomShaderMain
[TextureOverrideGeometry]
hash = 12345678
run = CustomShaderMain
)ini"+(which==4?"run = CustomShaderFail\n[CustomShaderFail]\nvs = layout.hlsl\nps = layout.hlsl\ntopology = point_list\nvb0 = ref ResourceVertices\ndraw = 3, 0\n":"");
        ShaderFixture f(d,directory,ini.c_str());f.backend->set_input_layout(original);
        if(which==3 || which==4){f.backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);frontend::RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;
            assets::FilePlan result;const bool succeeded=f.ini->draw(draw,result,f.why);
            if(which==4){
                // D-162: the CustomShader that fails is its command's alone; the draw's lists end, the
                // original layout is back, and the scope is dropped or kept as the owned runtime decides
                // (dropped when the failure came after it recorded work). It refused the whole draw before.
                check(succeeded && f.backend->input_layout()==original,"A draw whose CustomShader fails ends with its original input layout");
                if(f.backend->resources().failed()){
                    refuses([&]{f.backend->resources().finish();},"Failed layout draw root cannot submit partial work");f.backend->resources().discard();
                    auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
                    const auto bytes=f.backend->resources().commands().readback(d.queue,output,image_region(output));
                    check(std::all_of(bytes.begin(),bytes.end(),[](auto b){return !b;}),"Failed layout draw leaves its committed image unchanged");output.reset();
                }else f.finish();
                f.release();continue;}
            check(succeeded,"Original texture override invokes owned layout drawing");f.finish();
            check(f.backend->input_layout()==original,"Intercepted draw completion restores original layout after all source lists");}
        else f.present();
        auto edited_elements=layout_elements();if(which==5)edited_elements[0].format=DXGI_FORMAT_R32G32_FLOAT;
        else if(which!=2)edited_elements[0].offset=16;frontend::SourceInputLayout edited(edited_elements,programs[0].input_signature);
        auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
        check(actual==native_layout_pixels(programs[0],programs[1],programs[0],edited),"Nested original layout edits or failed creation produce native pixels");
        check(f.backend->graphics_pipelines()==(which==2?1u:2u),"Pipeline identity includes the effective input-layout attributes");
        if(which<2){check(f.backend->input_layout()!=original && f.backend->input_layout()->registers().at(0).offset==16,"Zero draw/dispatch consumes edits that survive CustomShader restore");
            f.present();check(f.backend->input_layout()->registers().at(0).offset==16,"Present completion preserves the original source layout lifetime");
            f.backend->after_draw();check(f.backend->input_layout()==original,"Explicit intercepted AfterDraw restores the saved layout");}
        if(which==2)check(f.backend->input_layout()==original,"Native invalid edited layout keeps the original object");
        if(which==5){check(f.backend->input_layout()->registers().at(1).offset==16,"Original format-edit realignment helper is preserved, including its enum-based boundary");f.backend->after_draw();}
        output.reset();f.release();
    }
}
static void original_layout_append(Device& d,const std::filesystem::path& root){
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/layout.hlsl",R"hlsl(struct O{float4 position:SV_Position;nointerpolation float4 color:COLOR0;};
#ifdef VERTEX_SHADER
O main(uint4 a:ALPHA,uint4 b:BETA,uint id:SV_VertexID){O o;o.position=float4(id==2?3:-1,id==1?3:-1,0,1);o.color=float4(a.x,b.xy,b.w);return o;}
#else
float4 main(O o):SV_Target{return o.color;}
#endif
)hlsl");const auto programs=geometry_programs(root/"Mods/layout.hlsl","vp");
    const uint8_t data[]={1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8};
    {std::ofstream file(root/"Mods/vertices.buf",std::ios::binary);file.write(reinterpret_cast<const char*>(data),sizeof(data));}
    auto elements=layout_elements();elements[0].format=DXGI_FORMAT_R8_UINT;elements[1].format=DXGI_FORMAT_R16G16_UINT;elements[1].offset=D3D11_APPEND_ALIGNED_ELEMENT;
    auto layout=std::make_shared<frontend::SourceInputLayout>(elements,programs[0].input_signature);
    const std::string ini=std::string(layout_resources)+R"ini(
[ResourcePacked]
type = Buffer
filename = vertices.buf
stride = 8
bind_flags = vertex_buffer
[CustomShaderMain]
vs = layout.hlsl
ps = layout.hlsl
cull = none
depth_enable = false
topology = triangle_list
vb0 = ref ResourcePacked
draw = 3, 0
[Present]
run = CustomShaderMain
)ini";ShaderFixture f(d,root,ini.c_str());f.backend->set_input_layout(layout);f.present();
    auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
    const auto expected=native_linked_graphics(programs[0],programs[1],{},[&](auto* device,auto* context){const auto descriptions=layout->descriptions();ImageNativeReference::Ptr<ID3D11InputLayout> input;
        check(SUCCEEDED(device->CreateInputLayout(descriptions.data(),UINT(descriptions.size()),programs[0].dxbc.data(),programs[0].dxbc.size(),&input)),"Native packed APPEND layout");
        D3D11_BUFFER_DESC desc{sizeof(data),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA initial{data,0,0};ImageNativeReference::Ptr<ID3D11Buffer> buffer;
        check(SUCCEEDED(device->CreateBuffer(&desc,&initial,&buffer)),"Native packed APPEND data");const UINT stride=8,offset=0;auto* handle=buffer.Get();context->IASetVertexBuffers(0,1,&handle,&stride,&offset);context->IASetInputLayout(input.Get());});
    check(actual==expected,"Original APPEND fetch and integer component defaults match native pixels");const float color[]={1,1541,2055,1};
    for(unsigned i=0;i<16;++i)check(!memcmp(actual.data()+i*16,color,16),"Packed input fetch uses aligned bytes and native missing-W default");output.reset();f.release();
}
