// Original IniParams publication, read through Vulkan transfers and a native
// compute consumer. The DrawExecutor below snapshots boundaries; it does not
// implement the original CustomShader language or application draw integration.
struct ParameterFixture {
    struct Extension : frontend::RuntimeExtension {
        std::shared_ptr<frontend::GpuResourceRuntime> backend;
        bool hunting_skip=false;
        std::vector<std::wstring> install(frontend::DrawPlanAdapter& adapter) override {return backend->install(adapter);}
        bool initialize_parameters(CommandListState& state,std::wstring& why) override {return backend->initialize_parameters(state,why);}
        bool before_draw(const frontend::RuntimeDraw&,CommandListState& state,std::wstring&) override {state.call_info->hunting_skip=hunting_skip;return true;}
        void execution_failed() noexcept override {backend->execution_failed();}
    };
    using T=ResourceCopyTargetType;
    Device& device;
    gpu::ResourceBudgets budgets;
    std::shared_ptr<frontend::GpuResourceRuntime> backend;
    std::shared_ptr<frontend::IniRuntime> ini;
    std::shared_ptr<Extension> extension;
    std::vector<gpu::Image> draws;
    std::string why;
    unsigned frame=0;
    bool refuse_draw=false;
    ParameterFixture(Device& d,const std::filesystem::path& root,const std::string& source):device(d){
        std::filesystem::create_directories(root/"Mods");write(root/"Mods/parameters.ini",source.c_str());
        frontend::RuntimeConfig config{root,{},root/"settings","parameters",root/"Mods"};
        backend=std::make_shared<frontend::GpuResourceRuntime>(d.resource,budgets);
        backend->draw_commands([this](DrawCommand&,CommandListState&,std::wstring& why){
            if(refuse_draw){why=L"Parameter test draw refusal";return false;}
            const auto image=backend->ini_parameters().image;
            check(bool(image),"Draw boundary has a parameter image");
            auto copy=backend->commands().create_image(image->description());
            VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent=image->extent();
            backend->commands().copy(image,copy,region);draws.push_back(copy);return true;
        });
        backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        extension=std::make_shared<Extension>();extension->backend=backend;
        ini=frontend::IniRuntime::load(config,extension,why);
        if(!ini)throw std::runtime_error("Load parameter fixture: "+why);++checks;
        finish();
    }
    void finish(){auto ticket=backend->finish();if(ticket)ticket->wait();}
    void present(frontend::RuntimeInput input={}){
        backend->begin(device.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present(input,++frame,0,why))throw std::runtime_error("Parameter Present: "+why);++checks;finish();
    }
    std::vector<uint8_t> read(const gpu::Image& image){check(bool(image),"Parameter image exists");
        return backend->commands().readback(device.queue,image,image_region(image));}
    std::vector<uint8_t> read(){return read(backend->ini_parameters().image);}
    gpu::Image slot(unsigned index){return backend->binding(L'p',T::SHADER_RESOURCE,index).image;}
    void expect(unsigned index,float expected){const auto bytes=read(slot(index));check(value(bytes)==expected,"Published parameter snapshot has expected GPU value");}
    float variable(const char* name){return ini->status().variables.at(std::string("$\\parameters\\")+name);}
    void release(){draws.clear();ini.reset();extension.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Parameter generation releases all allocation budgets");}
};
static void parameter_boundaries(Device& d,const std::filesystem::path& root){
    ParameterFixture f(d,root,R"ini(namespace = parameters
[Constants]
global persist $saved = 0
global $width = 0
global $format = 0
global $stride = 0
global $height = 0
global $mips = 0
global $flags = 0
ResourceZero = copy iniparams
x = 1
w3 = 40
ResourceUnpublished = copy iniparams
post ResourcePre = copy iniparams
post x = 2
post ResourcePost = copy iniparams
post $saved = 9
$width = iniparams->width
$height = iniparams->height
$mips = iniparams->mips
$format = iniparams->format
$stride = iniparams->stride
$flags = iniparams->bindflags
[ResourceZero]
[ResourceUnpublished]
[ResourcePre]
[ResourcePost]
[ResourceNested]
[ResourceAbortPre]
[ResourceAbortPost]
[CommandListNested]
x = 3
ResourceNested = copy iniparams
[CommandListAbort]
x = 4
handling = abort
x = 99
[Present]
pre run = CommandListNested
pre run = CommandListAbort
x = 99
post ResourceAbortPre = copy iniparams
post x = 5
post ResourceAbortPost = copy iniparams
post ps-t0 = ref ResourceZero
post ps-t1 = ref ResourceUnpublished
post ps-t2 = ref ResourcePre
post ps-t3 = ref ResourcePost
post ps-t4 = ref ResourceNested
post ps-t5 = ref ResourceAbortPre
post ps-t6 = ref ResourceAbortPost
)ini");
    check(f.backend->parameter_uploads()==3,"Initialization publishes zeros, Constants pre and Constants post separately");
    auto bytes=f.read();check(bytes.size()==64 && value(bytes)==2 && value(bytes,60)==40,"GPU vector includes highest reserved register and Constants post result");
    check(f.ini->status().dirty && f.variable("saved")==9,"Post-Constants persistent assignment remains dirty");
    check(f.variable("width")==4 && f.variable("height")==1 && f.variable("mips")==1 && f.variable("format")==2 &&
          f.variable("stride")==-3 && f.variable("flags")==8,"Original iniparams resource properties and buffer-only sentinel");
    const auto& description=f.backend->ini_parameters().image_description;
    check(description.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D && description.texture.Usage==D3D11_USAGE_DYNAMIC &&
          description.texture.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE,"Original dynamic source description survives Vulkan allocation");
    for(auto stage:std::wstring(L"vhdgpc"))check(f.backend->binding(stage,ParameterFixture::T::SHADER_RESOURCE,120).image==f.backend->ini_parameters().image,
        "Default parameter slot is seeded for every shader stage");
    f.present();const float expected[]={0,0,1,1,2,4,4};for(unsigned i=0;i<std::size(expected);++i)f.expect(i,expected[i]);
    check(value(f.read())==5 && f.backend->parameter_uploads()==5,"Nested abort flushes once at pre root and post is an independent root");
    f.release();
}
static void parameter_draws_and_dirty(Device& d,const std::filesystem::path& root){
    ParameterFixture f(d,root,R"ini(namespace = parameters
[Constants]
global $phase = 0
x = 0
[Present]
if $phase == 0
 x = -0
elif $phase == 1
 y = 1
elif $phase == 2
 y = 1
elif $phase == 3
 x = 0 / 0
elif $phase == 4
 x = 0 / 0
elif $phase == 5
 x = 11
 draw = 0, 0
 x = 22
 dispatch = 1, 1, 1
 x = 33
 ps-t120 = null
endif
$phase = $phase + 1
)ini");
    check(f.backend->parameter_uploads()==1,"All-zero Constants still publish initial zeros once");
    f.present();auto bytes=f.read();uint32_t word=1;memcpy(&word,bytes.data(),4);
    check(word==0 && f.backend->parameter_uploads()==1,"Signed-zero-only assignment does not flush");
    f.present();bytes=f.read();memcpy(&word,bytes.data(),4);
    check(word==0x80000000 && value(bytes,4)==1 && f.backend->parameter_uploads()==2,"Later dirty component publishes the whole vector including negative zero");
    f.present();check(f.backend->parameter_uploads()==2,"Unchanged finite parameter does not upload");
    f.present();check(std::isnan(value(f.read())) && f.backend->parameter_uploads()==3,"NaN assignment is dirty");
    f.present();check(std::isnan(value(f.read())) && f.backend->parameter_uploads()==4,"Repeated NaN remains dirty under original float comparison");
    f.present();check(f.draws.size()==2 && f.backend->parameter_uploads()==7,"Zero-count draw, dispatch and root each flush their pending changes");
    check(value(f.read(f.draws[0]))==11 && value(f.read(f.draws[1]))==22 && value(f.read())==33,"Draw and dispatch GPU snapshots preserve distinct publication times");
    {
        ParameterConsumer consumer(d);consumer.init(std::filesystem::current_path()/"parameters.spv");
        for(unsigned i=0;i<f.draws.size();++i){auto view=f.backend->commands().view(f.draws[i],VK_IMAGE_VIEW_TYPE_1D,VK_FORMAT_R32G32B32A32_SFLOAT,
                f.draws[i]->full_range(),VK_IMAGE_USAGE_SAMPLED_BIT);
            const auto observed=consumer.read(f.backend->commands(),view);
            check(value(observed)==float((i+1)*11) && value(observed,4)==1,"HLSL Texture1D reads boundary snapshots on the GPU");}
        const auto observed=consumer.read(f.backend->commands(),f.backend->ini_parameters().image_view);
        check(value(observed)==33 && value(observed,4)==1,"HLSL reads the final full parameter vector through its persistent SRV");
    }
    check(!f.slot(120) && bool(f.backend->ini_parameters().image),"Parameter update does not restore an explicitly unbound logical slot");
    f.release();
}
static void parameter_keys(Device& d,const std::filesystem::path& root){
    ParameterFixture f(d,root,R"ini(namespace = parameters
[Constants]
x = 2
[ResourceBefore]
[ResourceAfter]
[KeyActivate]
key = no_modifiers VK_F1
type = activate
x = 7
run = CommandListObserve
[CommandListObserve]
ResourceBefore = copy iniparams
post ResourceAfter = copy iniparams
[KeyHold]
key = no_modifiers VK_F2
type = hold
x = 9
[KeySame]
key = no_modifiers VK_F3
type = activate
x = 7
[Present]
ps-t0 = ref ResourceBefore
ps-t1 = ref ResourceAfter
)ini");
    frontend::RuntimeInput input;input.foreground=true;f.present(input);
    input.keys[VK_F1]=true;f.present(input);f.expect(0,2);f.expect(1,7);
    check(value(f.read())==7 && f.backend->parameter_uploads()==3,"Activation publishes before deferred post callbacks");
    input.keys.fill(false);f.present(input);input.keys[VK_F2]=true;f.present(input);
    check(value(f.read())==9,"Hold transition without post callback still publishes");
    input.keys.fill(false);f.present(input);check(value(f.read())==7,"Hold release restores the earlier GPU parameter");
    const auto uploads=f.backend->parameter_uploads();input.keys[VK_F3]=true;f.present(input);
    check(value(f.read())==7 && f.backend->parameter_uploads()==uploads+1,"Transition forces upload even when target value is unchanged");
    f.release();
}
static void parameter_registers_and_failures(Device& d,const std::filesystem::path& root){
    for(int reg:{7,-1,128}){
        ParameterFixture f(d,root/std::to_string(reg),"namespace = parameters\n[Rendering]\nini_params = "+std::to_string(reg)+
            "\n[Constants]\nx = 6\n[Present]\nps-t2 = iniparams\n");
        f.present();check(f.slot(2)==f.backend->ini_parameters().image,"Special iniparams source defaults to reference");
        check(bool(f.slot(7))==(reg==7) && !f.slot(120),"Configured register or disabled out-of-range/negative register");
        f.release();
    }
    ParameterFixture empty(d,root/"empty","namespace = parameters\n[Constants]\nglobal $v = 1\n[Present]\nps-t1 = iniparams\n");
    empty.present();check(!empty.backend->ini_parameters().image && !empty.slot(1) && !empty.backend->parameter_uploads(),"Empty reservation has no image or implicit parameter upload");empty.release();
    const std::vector<std::pair<std::string,std::string>> failures={
        {"x16384 = 1\n","width limit"},
        {"x = 1\nResourceBad = copy iniparams\n[ResourceBad]\nwidth = 0\n","dimensions"},
    };
    for(size_t index=0;index<failures.size();++index){const auto path=root/("failure"+std::to_string(index));
        std::filesystem::create_directories(path/"Mods");write(path/"Mods/fail.ini",("[Constants]\n"+failures[index].first).c_str());
        frontend::RuntimeConfig config{path,{},path/"settings","failure",path/"Mods"};gpu::ResourceBudgets budgets;
        auto backend=std::make_shared<frontend::GpuResourceRuntime>(d.resource,budgets);backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        std::string why;auto ini=frontend::IniRuntime::load(config,backend,why);
        // The parameters' own allocation refuses the generation (index 0); a command of [Constants] that
        // cannot act does nothing and the generation loads, as the original's (index 1, D-155).
        check(bool(ini)==(index==1),"A parameter allocation that cannot be made refuses; an invalid Constants command does not");
        if(!ini){
            if(why.find(failures[index].second)==std::string::npos)throw std::runtime_error("Wrong parameter failure: "+why);++checks;
            refuses([&]{backend->finish();},"Failed initialization cannot submit partial zero/Constants uploads");backend->discard();
        }else{declined(*ini,*backend,true,failures[index].second,"An invalid copy in Constants does nothing and is said");ini.reset();}
        backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Failed initialization releases allocations");
    }
    const auto path=root/"scope";std::filesystem::create_directories(path/"Mods");write(path/"Mods/scope.ini","[Constants]\nx = 1\n");
    frontend::RuntimeConfig config{path,{},path/"settings","scope",path/"Mods"};gpu::ResourceBudgets budgets;
    auto backend=std::make_shared<frontend::GpuResourceRuntime>(d.resource,budgets);std::string why;
    check(!frontend::IniRuntime::load(config,backend,why) && why.find("scope")!=std::string::npos,"Parameter initialization requires explicit host queue scope");
    backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Missing scope cannot allocate GPU parameters");
    budgets.resident=std::make_shared<assets::AllocationBudget>(1);
    backend=std::make_shared<frontend::GpuResourceRuntime>(d.resource,budgets);backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    check(!frontend::IniRuntime::load(config,backend,why),"Parameters honor shared allocation budget");
    refuses([&]{backend->finish();},"Parameter allocation failure poisons scope");backend->discard();backend.reset();
    check(!budgets.resident->used() && !budgets.staging->used(),"Rejected parameter budget leaves no charge");
}
static void parameter_lifetime(Device& d,const std::filesystem::path& root){
    ParameterFixture first(d,root/"first","[Constants]\nx = 3\n[Present]\nx = x + 1\n");
    ParameterFixture next(d,root/"next","[Constants]\nx = 8\ny64 = 90\n[Present]\nx = x + 1\n");
    first.present();next.present();check(value(first.read())==4 && value(next.read())==9,"Separate generations publish to independent GPU images");
    check(first.backend->ini_parameters().image->extent().width==1 && next.backend->ini_parameters().image->extent().width==65,"Reload reservation gets its own resized parameter allocation");
    {ParameterConsumer consumer(d);consumer.init(std::filesystem::current_path()/"parameters.spv");
        const auto observed=consumer.read(next.backend->commands(),next.backend->ini_parameters().image_view);
        check(observed.size()==65*16 && value(observed)==9 && value(observed,64*16+4)==90 && value(observed,63*16)==0,
            "HLSL GetDimensions and high parameter indices consume the full resized vector");}
    auto view=first.backend->ini_parameters().image_view;std::weak_ptr<gpu::ImageResource> weak=view->owner();
    first.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    check(first.ini->present({},2,0,first.why),"Record retiring generation update");auto ticket=first.backend->finish();
    first.ini.reset();first.extension.reset();first.backend.reset();check(!weak.expired(),"Submission and view retain retired parameter allocation");
    view.reset();check(!weak.expired(),"Fence ticket retains retired parameter allocation after views release");
    ticket->wait();ticket.reset();check(weak.expired() && !first.budgets.resident->used() && !first.budgets.staging->used(),"Last fence owner releases retired parameters and staging");
    check(value(next.read())==9,"Retiring old generation does not alter current parameters");next.release();
}
static void parameter_draw_failure(Device& d,const std::filesystem::path& root){
    ParameterFixture f(d,root,R"ini(namespace = parameters
[Constants]
x = 1
[TextureOverrideMesh]
hash = 12345678
match_index_count = 3
x = 2
draw = 0, 0
x = 99
)ini");
    frontend::RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;assets::FilePlan plan;
    f.extension->hunting_skip=true;f.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    check(f.ini->draw(draw,plan,f.why),"Hunting-skipped draw list still executes");f.finish();
    check(f.draws.empty() && value(f.read())==99 && f.backend->parameter_uploads()==3,"Hunting skip bypasses injected draw and its flush; root still publishes");
    f.extension->hunting_skip=false;f.refuse_draw=true;
    f.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    // D-162: the draw command the adapter refuses does nothing, with its reason in a notice; the list goes
    // on to `x = 99` and the draw's lists end as a Present's do (it refused the whole draw before).
    declined(*f.ini,*f.backend,f.ini->draw(draw,plan,f.why),"Parameter test draw refusal","A draw command the adapter refuses does nothing and its list goes on");
    check(value(f.read())==99,"The parameters after the refused draw command are the list's last");f.release();
}
