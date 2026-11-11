// One generation with a native extension (replaced draws through the file plan) and the owned
// GPU runtime (only inside a host scope): the arrangement the character renderer uses in the game.
#include "../frontend/xxmi/owned_generation.h"
struct OwnedGenerationNative final : frontend::RuntimeExtension {
    size_t prepared_items=0;bool prepared_with_gpu_closed=false;
    std::vector<std::wstring> native_shaders() override {return {L"customshader\\owned\\native"};}
    std::vector<std::wstring> install(frontend::DrawPlanAdapter&) override {return {};}
    bool prepare(frontend::DrawPlanAdapter& adapter,CommandListState& state,std::wstring& why) override {
        // A native preparation relies on plan semantics; the GPU side must be closed here.
        auto& probe=explicitCommandListSections.at(L"commandlist\\owned\\probe");adapter.adapt(probe.command_list);
        frontend::DrawPlan plan;if(!adapter.execute(probe.command_list,state,plan,why))return false;
        prepared_items=plan.items.size();prepared_with_gpu_closed=prepared_items==2;return true;
    }
};
static void owned_generation_cases(Device& d,const std::filesystem::path& root){
    std::filesystem::create_directories(root/"Mods");write(root/"Mods/backbuffer.hlsl",backbuffer_hlsl);
    {std::ofstream mesh(root/"Mods/mesh.buf",std::ios::binary);mesh<<std::string(36,'\0');std::ofstream index(root/"Mods/index.ib",std::ios::binary);index<<std::string(6,'\0');}
    write(root/"Mods/owned.ini",R"ini(namespace = owned
[Constants]
global $width = 0
ResourceAlias = ref ResourceMesh
[ResourceMesh]
type = Buffer
stride = 12
filename = mesh.buf
[ResourceIndex]
type = Buffer
format = DXGI_FORMAT_R16_UINT
filename = index.ib
[ResourceAlias]
[CommandListProbe]
vb0 = ResourceMesh
drawindexed = 3,0,0
[CustomShaderNative]
vs = not-there.hlsl
[CustomShaderPanel]
vs = backbuffer.hlsl
ps = backbuffer.hlsl
blend = ADD SRC_ALPHA INV_SRC_ALPHA
cull = none
topology = triangle_strip
o0 = set_viewport bb
draw = 4, 0
[TextureOverrideMesh]
hash = 12345678
vb0 = ResourceAlias
ib = ResourceIndex
drawindexed = 3,0,0
[TextureOverrideShader]
hash = 22222222
run = CustomShaderPanel
[Present]
$width = bb->width
run = CustomShaderPanel
)ini");
    gpu::HlslCompiler compiler(L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll");gpu::HlslRequest request;
    request.source=root/"Mods/backbuffer.hlsl";request.working_directory=root;gpu::HlslProgram vs,ps;std::string why;
    request.stage='v';check(compiler.compile(request,vs,why),"Compile owned generation vertex shader");
    request.stage='p';check(compiler.compile(request,ps,why),"Compile owned generation pixel shader");
    gpu::ResourceBudgets budgets;auto host=shader_host(d);
    auto native=std::make_shared<OwnedGenerationNative>();
    auto gpu=std::make_shared<frontend::GpuShaderRuntime>(d.resource,host,L"F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll",budgets);
    frontend::RuntimeConfig config{root,{},root/"settings","owned",root/"Mods"};
    auto& resources=gpu->resources();resources.begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    auto ini=frontend::IniRuntime::load(config,frontend::owned_generation(native,gpu),why);
    if(!ini)throw std::runtime_error("Load owned generation: "+why);++checks;
    {auto ticket=resources.finish();if(ticket)ticket->wait();}
    check(native->prepared_with_gpu_closed,"The native preparation runs with the owned GPU callbacks closed");
    // A replaced draw opens no scope: the plan resolves the Constants reference to the file.
    frontend::RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;draw.frame=1;efmi::assets::FilePlan plan;
    check(ini->draw(draw,plan,why),"A replaced draw keeps the file plan while no scope is open");
    check(plan.items.size()==3 && plan.items[0].operation==efmi::assets::FileOperation::BindVertex && plan.items[0].resource &&
          plan.items[0].resource->path.filename()==L"mesh.buf" && plan.items[2].operation==efmi::assets::FileOperation::DrawIndexed,
          "A declaration reference made in Constants is mirrored into the plan");
    draw.hash=0x22222222;
    // D-162: without the host's scope the CustomShader's run does nothing, with a notice; the draw is planned.
    declined(*ini,resources,ini->draw(draw,plan,why),"owned GPU scope","A CustomShader inside a replaced draw does nothing without the host scope");
    // Present opens the scope and draws into the host back buffer.
    gpu::ImageDescription description;description.format=VK_FORMAT_R32G32B32A32_SFLOAT;description.extent={8,4,1};
    description.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    resources.begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    auto image=resources.commands().create_image(description);const auto initial=backbuffer_initial();
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={8,4,1};
    resources.commands().write(image,copy,initial.data(),initial.size()*4);
    resources.backbuffer(image,DXGI_FORMAT_R32G32B32A32_FLOAT);
    check(ini->present({},2,0,why),"Present runs the owned CustomShader inside the host scope");
    {auto ticket=resources.finish();if(ticket)ticket->wait();}
    check(ini->status().variables.at("$\\owned\\width")==8,"Present reads the host back buffer");
    check(resources.commands().readback(d.queue,image,image_region(image))==native_backbuffer_draw(vs,ps),
          "The owned Present draw of a mixed generation matches native DX11 bytes");
    resources.backbuffer(nullptr,DXGI_FORMAT_UNKNOWN);image.reset();ini.reset();gpu.reset();
    check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used(),"The mixed generation retires its owned budgets");
}
