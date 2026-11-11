// Pending edits belong to one root phase; applied GPU layout survives until
// the intercepted draw's AfterDraw, including all matches and the post root.
static void original_matched_layouts(Device& d,const std::filesystem::path& root){
    const auto programs=layout_programs(root);const auto original=std::make_shared<frontend::SourceInputLayout>(layout_elements(),programs[0].input_signature);
    for(unsigned mode=0;mode<4;++mode){const auto directory=root/std::to_string(mode);std::filesystem::create_directories(directory/"Mods");
        std::filesystem::copy_file(root/"Mods/layout.hlsl",directory/"Mods/layout.hlsl",std::filesystem::copy_options::overwrite_existing);
        std::string text=std::string(layout_resources)+R"ini(
[CustomShaderMain]
vs = layout.hlsl
ps = layout.hlsl
cull = none
depth_enable = false
topology = triangle_list
vb0 = ref ResourceVertices
draw = 3, 0
[CustomShaderConsume]
draw = 0, 0
[TextureOverrideA]
hash = 12345678
match_priority = 0
vb0->elementoffset(ALPHA, 0) = 16
)ini";
        if(mode==0)text+="[TextureOverrideB]\nhash = 12345678\nmatch_priority = 1\nrun = CustomShaderMain\n";
        if(mode==1)text+="post run = CustomShaderMain\n";
        if(mode==2)text+="run = CustomShaderConsume\npost run = CustomShaderMain\n";
        if(mode==3)text+=R"ini(run = CustomShaderMain
[TextureOverrideB]
hash = 12345678
match_priority = 1
run = CustomShaderFail
[CustomShaderFail]
vs = layout.hlsl
ps = layout.hlsl
topology = point_list
vb0 = ref ResourceVertices
draw = 3, 0
)ini";
        ShaderFixture f(d,directory,text.c_str());f.backend->set_input_layout(original);auto output=f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image;
        f.backend->resources().begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);frontend::RuntimeDraw draw;draw.hash=0x12345678;draw.count=3;
        assets::FilePlan result;result.skip_original=true;const bool success=f.ini->draw(draw,result,f.why);
        check(f.backend->input_layout()==original,"All matched pre/post phases restore the original layout exactly once");
        if(mode==3){
            // D-162: a later matched section's CustomShader that fails is its command's alone; the draw's
            // lists end and the plan is made. The owned scope decides for its recorded work as in a Present:
            // dropped whole when the failure came after it recorded (nothing reaches the committed output).
            check(success,"A later matched section's failing command leaves the draw's lists running to their end");
            if(f.backend->resources().failed()){
                refuses([&]{f.backend->resources().finish();},"Later matched failure cannot submit earlier recorded GPU work");f.backend->resources().discard();
                const auto actual=f.backend->resources().commands().readback(d.queue,output,image_region(output));
                check(std::all_of(actual.begin(),actual.end(),[](auto byte){return byte==0;}),"Failed multi-match drawing leaves committed output untouched");
            }else f.finish();
        }else {if(!success)throw std::runtime_error(f.why);f.finish();auto elements=layout_elements();if(mode!=1)elements[0].offset=16;
            const auto expected=native_layout_pixels(programs[0],programs[1],programs[0],frontend::SourceInputLayout(elements,programs[0].input_signature));
            check(f.backend->resources().commands().readback(d.queue,output,image_region(output))==expected,
                "Pending layout edits cross matches but only applied GPU layouts cross independent phases");}
        output.reset();f.release();
    }
}
