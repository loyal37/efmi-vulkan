#include "skeleton_plan.h"
#include "palette_files.h"
#include <fstream>
#include <cstdio>
#include <limits>
using namespace efmi::frontend;
int HostSkeletonPlanTests()
{
    int passed=0,failed=0;
    auto check=[&](bool ok,const char* text){if(ok)++passed;else{++failed;printf("FAIL: %s\n",text);}};
    CustomResourcePool offsets,counts,remaps;CustomResource resource;
    offsets.name=L"pool_skeleton_test_offsets";counts.name=L"pool_skeleton_test_counts";remaps.name=L"pool_skeleton_test_remaps";
    for(auto* pool:{&offsets,&counts,&remaps}){
        pool->resource_template=&resource;
        pool->variable_template=std::make_unique<CommandListVariable>(pool->name,0,VariableFlags::NONE);
        pool->Initialize(3);
    }
    auto set=[&](CustomResourcePool& p,unsigned i,float value){p.GetVariable(float(i),false,true,true)->fval=value;};
    set(offsets,0,1);set(counts,0,1);set(counts,1,1);
    CustomResources files;FilePlanCompiler compiler(files);DrawPlanAdapter adapter;SkeletonFilePlan result;std::wstring reason;
    auto read=[&](unsigned bones=2,unsigned instances=3,unsigned lods=1){
        return read_skeleton_plan(bones,instances,lods,offsets,counts,remaps,adapter,compiler,result,reason);
    };
    check(read() && result.components.size()==3 && result.components[0].first==1 && result.components[2].count==0 &&
          result.components[0].lods[0].source==RemapSource::Unassigned,"unordered component ids and empty slots retain identity; absent LODs remain unassigned");
    const auto unchanged=[&](){return result.bones==2 && result.instances==3 && result.components.size()==3 && result.components[0].first==1;};
    // BUG-093: to the Core a component is an offset and a count; two on the same bones are read as the pools have them.
    set(offsets,0,0);check(read() && result.components[0].first==0 && result.components[0].count==1 && result.components[1].first==0 &&
        result.components[1].count==1,"components on the same bones are read as the pools give them");
    set(offsets,0,1);check(read() && unchanged(),"and apart again when the pools say so");
    set(offsets,0,1);set(counts,1,0);check(read() && unchanged() && result.components[1].count==0,
        "bones no component covers stay as the Core initialized them (BUG-048)");
    set(counts,1,1);set(offsets,0,.5f);check(!read() && unchanged(),"fractional component offsets are not truncated");
    set(offsets,0,std::numeric_limits<float>::quiet_NaN());check(!read() && unchanged(),"nonfinite component offsets refuse");
    set(offsets,0,2);check(!read() && unchanged(),"component bounds cannot exceed the skeleton");
    set(offsets,0,1);remaps.expiration_timeout_frames=1;check(!read() && unchanged(),"expiring metadata pools require a lifetime adapter");
    remaps.expiration_timeout_frames=UINT32_MAX;
    check(!read(2,3,2) && unchanged(),"remap pool dimensions must cover every component and LOD");
    check(!read(2,UINT32_MAX,1) && unchanged(),"skeleton byte budget checks wide instance arithmetic");
    check(read() && unchanged(),"a refused metadata read does not damage a later valid read");
    SkeletonFilePlan source;source.bones=2;source.instances=2;source.lod_count=2;source.components.resize(2);
    source.components[0].count=source.components[1].count=1;source.components[1].first=1;
    for(auto& c:source.components)for(auto& lod:c.lods)lod.source=RemapSource::Identity;
    std::filesystem::create_directories("build/tmp");
    const auto path=std::filesystem::absolute("build/tmp/palette-test-remap.buf");
    {std::ofstream file(path,std::ios::binary);const char bytes[2]={7,0};file.write(bytes,2);}
    source.components[0].lods[1]={RemapSource::File,path};
    efmi::core::PaletteDefinition owned;std::string error;
    check(load_palette_files(source,owned,error) && owned.components[0].lods[1].indices==std::vector<uint16_t>{7} &&
          owned.components[1].lods[0].available && owned.components[1].lods[0].indices.empty(),
          "exact R16 remap bytes and explicit identity are owned by the prepared definition");
    auto retained=[&](){return owned.components.size()==2 && owned.components[0].lods[1].indices==std::vector<uint16_t>{7};};
    {std::ofstream file(path,std::ios::binary|std::ios::app);file.put(0);}
    check(!load_palette_files(source,owned,error) && retained(),"oversized remap file refuses without changing prepared output");
    source.components[0].lods[1].path="relative.buf";
    check(!load_palette_files(source,owned,error) && retained(),"relative remap paths cannot depend on the application's working directory");
    source.components[0].lods[1]={RemapSource::Unassigned,{}};
    // The original Core binds nothing for a pool element no line assigned and takes no remap
    // (`z0 = @cs-t1 != 0`): the same as an element assigned null.
    check(load_palette_files(source,owned,error) && owned.components[0].lods[1].available && owned.components[0].lods[1].indices.empty(),
          "a LOD no line assigned has no remap, as the original Core binds nothing for it");
    source.components[1].first=0;
    check(load_palette_files(source,owned,error) && owned.components.size()==2,"components on the same bones load (BUG-093)");
    source.components[1].first=2;
    check(!load_palette_files(source,owned,error),"a component past the skeleton refuses before file loading");
    printf("XXMI skeleton metadata: %d checks passed, %d failed\n",passed,failed);return failed;
}
