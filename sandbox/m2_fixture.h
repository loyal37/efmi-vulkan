// Game-shaped streams and rolling bone offsets for exercising the actual DLL draw hook.
#pragma once
#include "variant_fixture.h"

namespace m2_fixture {
// With `extra`, the fragment shader takes its color from a uniform block at set 3 binding 0
// instead of the instance entry: a pipeline of the application that needs a fourth set.
// With `instanced` the vertex shader reads the instance entry of its instance (16 vectors per instance, by the
// instance index), as the game's character shaders read theirs; an instanced draw then draws each instance where
// its own entry puts it (P2-4).
inline std::vector<uint32_t> shader(bool vertex,bool extra=false,bool instanced=false)
{
    using namespace spv;
    const Op IAdd=static_cast<Op>(128),IMul=static_cast<Op>(132),Bitcast=static_cast<Op>(124);
    Builder b;
    auto main=b.id(),bone_var=b.id(),instance_var=b.id(),output=b.id(),pv=b.id(),extra_var=b.id();
    instanced=instanced && vertex;
    auto instance_index=b.id(),ti=b.id(),pii=b.id(),c16=b.id();
    extra=extra && !vertex;
    auto position=b.id(),texcoord=b.id(),weights=b.id(),indices=b.id();
    auto tv=b.id(),fn=b.id(),tf=b.id(),tu=b.id(),v2=b.id(),v3=b.id(),v4=b.id(),u4=b.id();
    auto len=b.id(),c0=b.id(),c1=b.id(),c3=b.id(),c5=b.id(),f0=b.id(),f1=b.id(),zero4=b.id();
    auto arr=b.id(),bone_type=b.id(),instance_type=b.id(),pb=b.id(),pi=b.id(),puf=b.id();
    auto in2=b.id(),in3=b.id(),in4=b.id(),inu4=b.id(),out4=b.id(),outpv=b.id();
    b.op(OpCapability,{CapabilityShader});b.op(OpMemoryModel,{AddressingLogical,MemoryGLSL450});
    if(vertex && instanced)b.entry_point(ModelVertex,main,"main",{position,texcoord,weights,indices,output,instance_index});
    else if(vertex)b.entry_point(ModelVertex,main,"main",{position,texcoord,weights,indices,output});
    else{b.entry_point(ModelFragment,main,"main",{output});b.op(OpExecutionMode,{main,ModeOriginUpperLeft});}
    b.op(OpDecorate,{bone_var,DecorationDescriptorSet,0});b.op(OpDecorate,{bone_var,DecorationBinding,7});
    b.op(OpDecorate,{instance_var,DecorationDescriptorSet,2});b.op(OpDecorate,{instance_var,DecorationBinding,0});
    if(extra){b.op(OpDecorate,{extra_var,DecorationDescriptorSet,3});b.op(OpDecorate,{extra_var,DecorationBinding,0});}
    b.op(OpDecorate,{bone_type,DecorationBufferBlock});b.op(OpMemberDecorate,{bone_type,0,DecorationOffset,0});
    b.op(OpMemberDecorate,{bone_type,0,DecorationNonWritable});
    b.op(OpDecorate,{instance_type,DecorationBlock});b.op(OpMemberDecorate,{instance_type,0,DecorationOffset,0});
    b.op(OpDecorate,{arr,6,16});
    if(vertex){
        b.op(OpDecorate,{position,DecorationLocation,0});b.op(OpDecorate,{texcoord,DecorationLocation,1});
        b.op(OpDecorate,{weights,DecorationLocation,2});b.op(OpDecorate,{indices,DecorationLocation,3});
        b.op(OpDecorate,{pv,DecorationBlock});b.op(OpMemberDecorate,{pv,0,DecorationBuiltIn,BuiltInPosition});
        if(instanced)b.op(OpDecorate,{instance_index,DecorationBuiltIn,43u}); // InstanceIndex
    }else b.op(OpDecorate,{output,DecorationLocation,0});
    b.op(OpTypeVoid,{tv});b.op(OpTypeFunction,{fn,tv});b.op(OpTypeFloat,{tf,32});b.op(OpTypeInt,{tu,32,0});
    b.op(OpTypeVector,{v2,tf,2});b.op(OpTypeVector,{v3,tf,3});b.op(OpTypeVector,{v4,tf,4});b.op(OpTypeVector,{u4,tu,4});
    b.op(OpConstant,{tu,len,4096});b.op(OpConstant,{tu,c0,0});b.op(OpConstant,{tu,c1,1});
    b.op(OpConstant,{tu,c3,3});b.op(OpConstant,{tu,c5,5});b.op(OpConstant,{tf,f0,0});b.op(OpConstant,{tf,f1,0x3f800000});
    if(instanced){b.op(OpTypeInt,{ti,32,1});b.op(OpConstant,{tu,c16,16});}
    b.op(OpConstantComposite,{v4,zero4,f0,f0,f0,f0});b.op(OpTypeArray,{arr,v4,len});
    b.op(OpTypeStruct,{bone_type,arr});b.op(OpTypeStruct,{instance_type,arr});
    b.op(OpTypePointer,{pb,StorageUniform,bone_type});b.op(OpTypePointer,{pi,StorageUniform,instance_type});
    b.op(OpTypePointer,{puf,StorageUniform,v4});b.op(OpTypePointer,{out4,StorageOutput,v4});
    if(vertex){
        b.op(OpTypeStruct,{pv,v4});b.op(OpTypePointer,{outpv,StorageOutput,pv});
        b.op(OpTypePointer,{in2,StorageInput,v2});b.op(OpTypePointer,{in3,StorageInput,v3});
        b.op(OpTypePointer,{in4,StorageInput,v4});b.op(OpTypePointer,{inu4,StorageInput,u4});
        b.op(OpVariable,{in3,position,StorageInput});b.op(OpVariable,{in2,texcoord,StorageInput});
        b.op(OpVariable,{in4,weights,StorageInput});b.op(OpVariable,{inu4,indices,StorageInput});
        b.op(OpVariable,{outpv,output,StorageOutput});
        if(instanced){b.op(OpTypePointer,{pii,StorageInput,ti});b.op(OpVariable,{pii,instance_index,StorageInput});}
    }else b.op(OpVariable,{out4,output,StorageOutput});
    b.op(OpVariable,{pb,bone_var,StorageUniform});b.op(OpVariable,{pi,instance_var,StorageUniform});
    if(extra)b.op(OpVariable,{pi,extra_var,StorageUniform});
    auto label=b.id();b.op(OpFunction,{tv,main,0,fn});b.op(OpLabel,{label});
    auto load4=[&](uint32_t var,uint32_t index){
        auto ptr=b.id(),value=b.id();b.op(OpAccessChain,{puf,ptr,var,c0,index});b.op(OpLoad,{v4,value,ptr});return value;
    };
    if(vertex){
        uint32_t at5=c5,at0=c0;
        if(instanced){
            auto id=b.id(),idu=b.id(),base=b.id();at5=b.id();at0=b.id();
            b.op(OpLoad,{ti,id,instance_index});b.op(Bitcast,{tu,idu,id});b.op(IMul,{tu,base,idu,c16});
            b.op(IAdd,{tu,at5,base,c5});b.op(IAdd,{tu,at0,base,c0});
        }
        auto config=load4(instance_var,at5),bits=b.id(),current=b.id(),previous=b.id();
        b.op(Bitcast,{u4,bits,config});b.op(OpCompositeExtract,{tu,current,bits,0});b.op(OpCompositeExtract,{tu,previous,bits,1});
        auto blend=b.id(),bone=b.id(),rows=b.id(),at_current=b.id(),at_previous=b.id();
        b.op(OpLoad,{u4,blend,indices});b.op(OpCompositeExtract,{tu,bone,blend,0});b.op(IMul,{tu,rows,bone,c3});
        // As in the game's vertex shaders, row = offset + 3 + 3 * index: the offset in the entry names
        // the matrix before bone zero (the core's importer skips it, its patch subtracts it).
        auto first_current=b.id(),first_previous=b.id();
        b.op(IAdd,{tu,first_current,current,c3});b.op(IAdd,{tu,first_previous,previous,c3});
        b.op(IAdd,{tu,at_current,first_current,rows});b.op(IAdd,{tu,at_previous,first_previous,rows});
        auto cur=load4(bone_var,at_current),prev=load4(bone_var,at_previous),sum=b.id();b.op(OpFAdd,{v4,sum,cur,prev});
        auto w=b.id(),wx=b.id(),w4=b.id(),weighted=b.id();b.op(OpLoad,{v4,w,weights});b.op(OpCompositeExtract,{tf,wx,w,0});
        b.op(OpCompositeConstruct,{v4,w4,wx,wx,wx,wx});b.op(OpFMul,{v4,weighted,sum,w4});
        auto pos=b.id(),x=b.id(),y=b.id(),z=b.id(),pos4=b.id();b.op(OpLoad,{v3,pos,position});
        b.op(OpCompositeExtract,{tf,x,pos,0});b.op(OpCompositeExtract,{tf,y,pos,1});b.op(OpCompositeExtract,{tf,z,pos,2});
        b.op(OpCompositeConstruct,{v4,pos4,x,y,z,f1});
        auto uv=b.id(),u=b.id(),v=b.id(),uv4=b.id(),unused=b.id();b.op(OpLoad,{v2,uv,texcoord});
        b.op(OpCompositeExtract,{tf,u,uv,0});b.op(OpCompositeExtract,{tf,v,uv,1});
        b.op(OpCompositeConstruct,{v4,uv4,u,v,f0,f0});b.op(OpFMul,{v4,unused,uv4,zero4});
        auto translated=b.id(),moved=b.id(),final=b.id(),out=b.id(),instance=load4(instance_var,at0);
        b.op(OpFAdd,{v4,translated,pos4,weighted});b.op(OpFAdd,{v4,moved,translated,instance});b.op(OpFAdd,{v4,final,moved,unused});
        b.op(OpAccessChain,{out4,out,output,c0});b.op(OpStore,{out,final});
    }else{
        auto tint=load4(bone_var,c0),color=load4(extra?extra_var:instance_var,c1),result=b.id();b.op(OpFMul,{v4,result,tint,color});b.op(OpStore,{output,result});
    }
    b.op(OpReturn,{});b.op(OpFunctionEnd,{});return b.finish();
}

inline efvk::GraphicsRecipe recipe(bool instanced=false)
{
    variant_fixture::RecipeFixture f;
    auto r=f.capture();
    for(uint32_t i=0;i<2;++i){
        auto s=std::make_shared<efvk::ShaderInfo>();s->id=i+1;s->code=shader(i==0,false,instanced);s->size=static_cast<uint32_t>(s->code.size()*4);
        r.stages[i].shader=s;
    }
    r.vertex.bindings={{0,16,VK_VERTEX_INPUT_RATE_VERTEX},{1,8,VK_VERTEX_INPUT_RATE_VERTEX},
                       {2,12,VK_VERTEX_INPUT_RATE_VERTEX},{3,16,VK_VERTEX_INPUT_RATE_VERTEX}};
    r.vertex.attributes={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,1,VK_FORMAT_R32G32_SFLOAT,0},
                        {2,2,VK_FORMAT_R16G16B16A16_UNORM,0},{3,2,VK_FORMAT_R8G8B8A8_UINT,8}};
    return r;
}
} // namespace m2_fixture
