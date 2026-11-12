#include "skeleton_plan.h"
#include <algorithm>
#include <cmath>

namespace efmi::frontend {
bool read_skeleton_plan(uint32_t bones,uint32_t instances,uint32_t lods,
    CustomResourcePool& offsets,CustomResourcePool& counts,CustomResourcePool& remaps,
    const DrawPlanAdapter& adapter,const FilePlanCompiler& files,SkeletonFilePlan& output,std::wstring& reason)
{
    reason.clear();
    auto fail=[&](const wchar_t* text){reason=text;return false;};
    const size_t components=counts.GetPoolSize();
    if(!bones || bones>65536 || !instances || (512ull+2ull*bones*instances)*48>(64ull<<20) ||
       !lods || lods>4 || !components || components>256 || offsets.GetPoolSize()!=components || remaps.GetPoolSize()!=components*lods)
        return fail(L"Skeleton dimensions or pool sizes are incompatible");
    for(auto* pool:{&offsets,&counts,&remaps}){
        auto* p=pool->ResolvePool();
        if((p->index_type!=PoolIndexType::RING && p->index_type!=PoolIndexType::STATIC) || p->expiration_timeout_frames!=UINT32_MAX)
            return fail(L"Skeleton metadata requires nonexpiring direct-index pools");
    }
    auto integer=[](CommandListVariable* value,uint32_t& out){
        if(!value || !std::isfinite(value->fval) || value->fval<0 || value->fval>65536 || std::floor(value->fval)!=value->fval)return false;
        out=static_cast<uint32_t>(value->fval);return true;
    };
    SkeletonFilePlan next;next.bones=bones;next.instances=instances;next.lod_count=lods;next.components.resize(components);
    std::vector<std::pair<uint32_t,uint32_t>> intervals;
    for(size_t c=0;c<components;++c){
        auto& item=next.components[c];
        if(!integer(offsets.GetVariable(float(c),false,true,false),item.first) ||
           !integer(counts.GetVariable(float(c),false,true,false),item.count) ||
           uint64_t(item.first)+item.count>bones)return fail(L"Skeleton component range is invalid");
        if(item.count)intervals.emplace_back(item.first,item.count);
        // Core's pool stride may still be four. Only LOD0/LOD1 are in scope.
        for(uint32_t lod=0;lod<std::min(lods,SkeletonFilePlan::supported_lods);++lod){
            auto* declaration=remaps.GetResource(float(c*lods+lod),false,true,false);
            if(!declaration)return fail(L"Skeleton remap pool resource is absent");
            if(!adapter.has_reference(declaration))continue;
            auto* selected=adapter.referenced_resource(declaration);
            auto& remap=item.lods[lod];
            if(!selected){remap.source=RemapSource::Identity;continue;}
            DrawPlan file;PlanItem bind;bind.operation=PlanOperation::BindIndex;bind.resource=selected;file.items.push_back(bind);
            assets::FilePlan owned;
            if(!files.compile(file,owned,reason))return false;
            const auto& resolved=owned.items.at(0).resource;
            if(!resolved || resolved->index_type!=VK_INDEX_TYPE_UINT16)return fail(L"Skeleton remap must be a plain R16_UINT file");
            remap.source=RemapSource::File;remap.path=resolved->path;
        }
    }
    // Bones between components stay as the Core's initializer left them (a Mod may declare its components
    // with bones no component imports, BUG-048), and two components may share bones: to the Core each is
    // an offset and a count, and its importer writes that range whatever else lies there (BUG-093).
    (void)intervals;
    output=std::move(next);return true;
}
}
