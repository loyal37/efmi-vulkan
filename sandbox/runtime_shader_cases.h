#include "../frontend/xxmi/runtime_host.h"
#include "globals.h"
#include <d3dcompiler.h>
#include <cmath>

static SourceShaderState known_source_shaders(){SourceShaderState state;for(auto& stage:state.stages)stage.known=true;return state;}
static std::shared_ptr<const ShaderBindings> native_shader_declarations(){
    const char* source=R"hlsl(cbuffer Fixed : register(b2) {float4 fixed_values[3];};
cbuffer Dynamic : register(b3) {float4 dynamic_values[5];};
Texture2D<float4> Image : register(t4);
StructuredBuffer<float4> Structured : register(t7);
ByteAddressBuffer Raw : register(t8);
Buffer<uint> Typed : register(t9);
float4 main(uint i:SV_VertexID):SV_Position {
 return fixed_values[2]+dynamic_values[i%5]+Image.Load(int3(0,0,0))+Structured[0]+Raw.Load(0)+Typed[0];
})hlsl";
    ID3DBlob* bytecode=nullptr;ID3DBlob* errors=nullptr;
    const auto result=D3DCompile(source,strlen(source),"source-facts.hlsl",nullptr,nullptr,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytecode,&errors);
    if(errors)errors->Release();check(SUCCEEDED(result) && bytecode,"Native declaration fixture compiles");
    auto declarations=source_shader_declarations(bytecode->GetBufferPointer(),bytecode->GetBufferSize());
    check(declarations->constant_buffers[2].type==ShaderConstantBufferType::IMMEDIATE_INDEXED && declarations->constant_buffers[2].size==3,
        "Pinned native reader preserves immediate constant declaration size in registers");
    check(declarations->constant_buffers[3].type==ShaderConstantBufferType::DYNAMIC_INDEXED && declarations->constant_buffers[3].size==5,
        "Pinned native reader preserves dynamic constant indexing");
    check(declarations->resources[4].type==ShaderResourceType::TYPED && declarations->resources[4].dimension==D3D_SRV_DIMENSION_TEXTURE2D &&
        declarations->resources[7].type==ShaderResourceType::STRUCTURED && declarations->resources[7].stride==16 &&
        declarations->resources[8].type==ShaderResourceType::RAW && declarations->resources[8].dimension==D3D_SRV_DIMENSION_BUFFEREX,
        "Pinned native reader preserves typed, structured and raw source slots");
    std::vector<unsigned char> invalid(static_cast<unsigned char*>(bytecode->GetBufferPointer()),static_cast<unsigned char*>(bytecode->GetBufferPointer())+bytecode->GetBufferSize());
    bytecode->Release();invalid[0]='X';bool refused=false;try{source_shader_declarations(invalid.data(),invalid.size());}catch(const std::exception&){refused=true;}
    check(refused,"Malformed native declarations refuse instead of fabricating source slots");return declarations;
}
static void runtime_shader_cases(const std::filesystem::path& folder){
    const auto root=std::filesystem::absolute(folder/"shader-hooks");std::filesystem::create_directories(root);
    auto declarations=native_shader_declarations();
    auto facts=known_source_shaders();for(unsigned i=0;i<6;++i)facts.stages[i]={true,true,true,i+1,declarations};
    for(unsigned mode=0;mode<5;++mode){
        std::string text="namespace = shader_hooks\n[Constants]\nglobal $peek = 0\nglobal $pre = 0\nglobal $post = 0\nglobal $original = 0\nglobal $tail = 0\nglobal $mode = "+std::to_string(mode)+"\n";
        for(unsigned i=1;i<=6;++i){text+="[ShaderOverride"+std::to_string(i)+"]\nhash = "+std::to_string(i)+"\n$pre = $pre * 10 + "+std::to_string(i)+"\n";
            if(mode==0)text+="$peek = ps-t1\npost $peek = ps-t1\n";
            if(i==1)text+="if $mode == 1\npre handling = abort\nendif\nif $mode == 2\nhandling = skip\nendif\n$tail = $tail + 1\n";
            text+="post $post = $post * 10 + "+std::to_string(i)+"\n";
            if(i==1)text+="if $mode == 3\npost handling = abort\nendif\npost $tail = $tail + 10\n";
        }
        const auto path=root/(std::to_string(mode)+".ini");write(path,text.c_str());
        RuntimeConfig config{root,{path},root/"settings","shader"+std::to_string(mode)};config.shader_hooks=true;
        auto extension=std::make_shared<MatchReferenceExtension>();std::string why;auto runtime=IniRuntime::load(config,extension,why);if(!runtime)throw std::runtime_error(why);
        unsigned originals=0;RuntimeShaderCall call;call.shaders=facts;call.call.type=mode==4?DrawCall::Dispatch:DrawCall::DrawIndexedInstanced;
        call.call.IndexCount=3;call.call.InstanceCount=1;
        call.original=[&](CommandListState& state,std::wstring&){++originals;
            check(!state.post && !state.aborted && state.rt_width==-1,"Original operation gets independent transient state");
            check(command_list_globals.at(L"$\\shader_hooks\\pre").fval==(mode==4?6:12345) && command_list_globals.at(L"$\\shader_hooks\\post").fval==0,
                "All source pre stages precede the original operation and every post stage follows it");
            command_list_globals.at(L"$\\shader_hooks\\original").fval=1;return true;};
        check(runtime->intercept(call,why),"Original shader stage hooks execute");auto status=runtime->status();
        check(status.variables.at("$\\shader_hooks\\pre")== (mode==4?6:12345) && status.variables.at("$\\shader_hooks\\post")== (mode==4?6:12345),
            "Stage roots keep source graphics/compute order and isolate abort");
        check(originals==(mode==2?0:1) && status.variables.at("$\\shader_hooks\\original")==float(originals),"Pre skip suppresses only the original operation");
        check(status.variables.at("$\\shader_hooks\\tail")== (mode==4?0:mode==1?10:mode==3?1:11),"Abort stops only its independent root");
        check(status.shader_calls==1 && extension->completed==(mode==4?0:1) && extension->dispatched==(mode==4?1:0) && !extension->failed,"Draw and dispatch completion use their distinct source lifetimes");
        if(mode==0){check(extension->queries.size()==10,"Every matched stage executes its own pre/post root");
            for(unsigned i=0;i<10;++i)check(extension->queries[i]==std::make_pair(i>=5,-1.0f),"Cached root queries do not leak between shader stages");}
        call.call.hunting_skip=true;check(runtime->intercept(call,why) && runtime->status().variables==status.variables,"Hunting-skipped calls execute neither shader roots nor original work");
    }
    const auto path=root/"queries.ini";write(path,R"ini(namespace = shader_queries
[Constants]
global $v = 0
global $h = 0
global $p = 0
global $c = 0
global $cb_type = 0
global $cb_size = 0
global $srv_type = 0
global $srv_stride = 0
global $srv_dimension = 0
global $cb_mask = 9
global $srv_mask = 9
global $slot = 2
global $resource_slot = 7
[ShaderOverrideV]
hash = 1
filter_index = 17
[ShaderOverrideP]
hash = 5
filter_index = -4
[ShaderOverrideC]
hash = 6
[Present]
$v = vs
$h = hs
$p = ps
$c = cs
$cb_type = vs->cb_type($slot)
$cb_size = vs->cb_size($slot)
$srv_type = vs->srv_type($resource_slot)
$srv_stride = vs->srv_stride($resource_slot)
$srv_dimension = vs->srv_dimension($resource_slot)
$cb_mask = vs->cb_mask
$srv_mask = vs->srv_mask
)ini");
    RuntimeConfig config{root,{path},root/"settings","queries"};config.shader_hooks=true;std::string why;
    auto extension=std::make_shared<MatchReferenceExtension>();auto runtime=IniRuntime::load(config,extension,why);if(!runtime)throw std::runtime_error(why);
    auto query=facts;query.stages[1]={true,false,false,0,{}};check(runtime->bind_source_shaders(query,why) && runtime->present({},1,0,why),"Tracked shader facts drive original queries");
    auto status=runtime->status();auto value=[&](const char* name){return status.variables.at(std::string("$\\shader_queries\\")+name);};
    check(value("v")==17 && value("p")==-4 && value("c")==1 && value("h")==0 && std::signbit(value("h")),"Native shader filters preserve explicit/default indices and negative zero");
    check(value("cb_type")==1 && value("cb_size")==3 && value("srv_type")==2 && value("srv_stride")==16 && value("srv_dimension")==1,
        "Original evaluator uses DXBC declaration facts");
    check(value("cb_mask")==0 && value("srv_mask")==0,"Pinned mask members preserve actual zero behavior rather than inventing bit masks");
    query.stages[1]={true,true,false,0,{}};query.stages[4]={true,true,true,99,{}};
    check(runtime->bind_source_shaders(query,why) && runtime->present({},2,0,why),"Bound unknown-hash and unmatched source shader queries execute");status=runtime->status();
    check(value("h")==0 && !std::signbit(value("h")) && value("p")==0 && !std::signbit(value("p")),"Unknown hash and unmatched override return positive zero");
    // A query without its declaration facts is never read from scaffolding: it reads the table's value
    // (D-163: a register that is not declared, type 0 and -1 for its stride), the notice names the reason,
    // the assignment runs and the Present goes on. Before, the assignment did nothing (D-155).
    auto warnings=[&](const char* text){size_t n=0;for(const auto& m:runtime->status().diagnostic_messages)n+=m.find(text)!=std::string::npos;return n;};
    query.stages[0].declarations.reset();check(runtime->bind_source_shaders(query,why) && runtime->present({},3,0,why) && warnings("declarations")>=1,
        "Unavailable declaration facts are not read from scaffolding; the Present goes on with the warning");
    status=runtime->status();check(value("cb_type")==0 && value("srv_stride")==-1 && value("v")==17,
        "A query nothing answers reads the table's value; the stage's filter value is still its own");
    query.stages[0]={};check(runtime->bind_source_shaders(query,why) && runtime->present({},4,0,why) && warnings("declarations")>=1,
        "Unknown stage never becomes an unbound shader; the Present goes on with the warning");
    status=runtime->status();check(value("v")==0 && !std::signbit(value("v")),"An unknown stage reads a bound shader no section names, never the unbound -0");
    auto conflict=facts;auto changed=std::make_shared<ShaderBindings>(*declarations);changed->constant_buffers[2].size=99;
    check(runtime->bind_source_shaders(facts,why),"Restore known source facts before rejected update");
    conflict.stages[1]={true,true,true,1,changed};check(!runtime->bind_source_shaders(conflict,why) && runtime->present({},5,0,why),"Conflicting declarations refuse without replacing the previous source facts");
    // Original member expressions are evaluated once, then guarded before the source cast.
    for(const char* index:{"14","-1","1 / 0","0 / 0"}){const auto bad=root/"bad-slot.ini";
        const std::string text="namespace = bad_slot\n[Constants]\nglobal $slot = "+std::string(index)+"\nglobal $value = 0\n[Present]\n$value = vs->cb_size($slot)\n";
        write(bad,text.c_str());config.inputs={bad};auto candidate=IniRuntime::load(config,nullptr,why);if(!candidate)throw std::runtime_error(why);
        // Guarded before the unsigned conversion: the query reads the table's value for a register that is
        // not declared (-1) with the warning, and the Present goes on (D-163).
        const bool presented=candidate->bind_source_shaders(facts,why) && candidate->present({},1,0,why);
        const auto after=candidate->status();
        check(presented && after.variables.at("$\\bad_slot\\value")==-1 && after.diagnostics>=1,"Invalid source declaration slot is guarded before unsigned conversion; the query reads -1 with the warning");}
    const auto duplicates=root/"duplicates.ini";
    for(bool allowed:{false,true}){std::string text="namespace = duplicate_shaders\n[Constants]\nglobal $pre = 0\nglobal $post = 0\n";
        for(unsigned i=1;i<=2;++i){text+="[ShaderOverride"+std::to_string(i)+"]\nhash = 1\n";
            if(allowed)text+="allow_duplicate_hash = true\n";
            text+="$pre = $pre * 10 + "+std::to_string(i)+"\npost $post = $post * 10 + "+std::to_string(i+2)+"\n";}
        write(duplicates,text.c_str());config.inputs={duplicates};auto candidate=IniRuntime::load(config,nullptr,why);
        if(!candidate)throw std::runtime_error(why);
        // The original warns about a possible mod conflict and merges the sections all the same.
        bool conflict_reported=false;
        for(const auto& line:candidate->status().diagnostic_messages)conflict_reported|=line.find("Duplicate ShaderOverride")!=std::string::npos;
        check(conflict_reported!=allowed,allowed?"Allowed duplicate hashes load without a warning":"Accidental duplicate hashes are reported as the original conflict warning");RuntimeShaderCall call;call.call.type=DrawCall::Draw;call.shaders=facts;unsigned originals=0;
        call.original=[&](CommandListState&,std::wstring&){++originals;check(command_list_globals.at(L"$\\duplicate_shaders\\pre").fval==12,"Allowed duplicate hashes merge original pre commands");return true;};
        check(candidate->intercept(call,why) && originals==1 && candidate->status().variables.at("$\\duplicate_shaders\\post")==34,
            "Allowed duplicate hashes merge post commands after one original operation");
    }
    const auto counters=root/"counters.ini";write(counters,R"ini(namespace = shader_counters
[Constants]
global $before = 0
global $after = 0
global $pre_frame = 0
global $post_frame = 0
[Present]
$before = draw_number * 10 + dispatch_number
$pre_frame = frame_number
post $after = draw_number * 10 + dispatch_number
post $post_frame = frame_number
)ini");
    config.inputs={counters};auto candidate=IniRuntime::load(config,nullptr,why);if(!candidate)throw std::runtime_error(why);
    RuntimeShaderCall call;call.call.type=DrawCall::Draw;call.shaders=known_source_shaders();unsigned originals=0;
    call.original=[&](CommandListState&,std::wstring&){++originals;return true;};
    check(candidate->intercept(call,why),"Unmatched source draw still reaches the original boundary");call.call.type=DrawCall::Dispatch;
    check(candidate->intercept(call,why) && candidate->present({},9,0,why),"Original counters cross the owned Present boundary");
    status=candidate->status();check(originals==2 && status.variables.at("$\\shader_counters\\before")==11 && status.variables.at("$\\shader_counters\\after")==0 &&
        status.variables.at("$\\shader_counters\\pre_frame")==9 && status.variables.at("$\\shader_counters\\post_frame")==10,
        "Present resets call counters and advances frame only between its source phases");
    const auto zero=root/"zero.ini";write(zero,R"ini(namespace = shader_zero
[Constants]
global $count = 0
global $initial = 0
$initial = vs
[ShaderOverrideZero]
hash = 0
filter_index = 7
$count = $count + 1
post $count = $count + 10
)ini");
    config.inputs={zero};auto initial=std::make_shared<SourceShaderState>(known_source_shaders());
    initial->stages[0]={true,true,true,0,declarations};config.initial_shaders=initial;
    candidate=IniRuntime::load(config,nullptr,why);if(!candidate)throw std::runtime_error(why);
    check(candidate->status().variables.at("$\\shader_zero\\initial")==7,"Constants see explicit initial tracked source facts and parsed filter indices");
    call.call.type=DrawCall::Draw;call.shaders=*initial;call.shaders.stages[1]={true,true,true,0,declarations};originals=0;
    check(candidate->intercept(call,why) && originals==1 && candidate->status().variables.at("$\\shader_zero\\count")==22,
        "Source hash0 matches VS/PS but not zero-hash HS/DS/GS roots");
    call.call.type=DrawCall::Dispatch;check(candidate->intercept(call,why) && originals==2 && candidate->status().variables.at("$\\shader_zero\\count")==33,
        "Source compute lookup includes hash0 even with an unbound shader");
    call.call.type=DrawCall::Draw;call.shaders.stages[0]={};
    check(!candidate->intercept(call,why) && originals==2 && candidate->status().variables.at("$\\shader_zero\\count")==33,
        "Unknown source stage refuses before running any matched root or original work");
    call.shaders.stages[0]={true,true,false,0,{}};
    check(!candidate->intercept(call,why) && originals==2,"Unknown hash cannot silently become a hash0 match");
    // Without shader hooks the `$initial = vs` of [Constants] is refused; as a Constants command on its own it
    // does nothing (the value stays 0) with the warning, and the generation loads (D-155): not silently.
    config.shader_hooks=false;config.initial_shaders.reset();
    {auto hookless=IniRuntime::load(config,nullptr,why);
     check(hookless && hookless->status().variables.at("$\\shader_zero\\initial")==0 && hookless->status().diagnostics>=1,
         "Character-only generations load shader hook declarations with the refused query warned about, never evaluated");}
    for(const char* legacy:{"depth_filter = depth_active\n","model = vs_5_0\n"}){
        const std::string text=std::string("[ShaderOverrideLegacy]\nhash = 1\n")+legacy;write(zero,text.c_str());config.shader_hooks=true;
        check(!IniRuntime::load(config,nullptr,why) && why.find("Legacy shader depth/model")!=std::string::npos,
            "Legacy binary substitution refuses explicitly rather than changing command filtering");}
}
