#include "palette_bank.h"
#include <algorithm>
#include <cstring>

namespace efmi::core {
namespace {
uint8_t dynamic_poses(const efvk::InstanceEntry& entry)
{
    uint32_t offsets[2];std::memcpy(offsets,entry.data()+80,8);
    return (offsets[0]>=efvk::MergedSkeleton::kStaticRows?1:0) | (offsets[1]>=efvk::MergedSkeleton::kStaticRows?2:0);
}
}
bool PaletteBank::configure(PaletteDefinition input,std::string& reason)
{
    reason.clear();auto fail=[&](const char* text){reason=text;return false;};
    if(!input.bones || input.bones>65536 || !input.instances || input.instances>64 || input.components.empty() || input.components.size()>256 ||
       (512ull+2ull*input.bones)*48*input.instances>efvk::MergedSkeleton::kMaxBytes)return fail("Palette dimensions exceed the CPU budget");
    std::vector<uint32_t> ids;std::vector<uint32_t> dense(input.components.size(),UINT32_MAX);
    for(uint32_t id=0;id<input.components.size();++id){
        const auto& component=input.components[id];
        if(uint64_t(component.range.first)+component.range.count>input.bones)return fail("Palette component exceeds bone count");
        if(component.range.count)ids.push_back(id);
        for(const auto& lod:component.lods){
            if((!lod.available && !lod.indices.empty()) ||
               (!lod.indices.empty() && lod.indices.size()!=component.range.count))return fail("Palette remap length is incompatible");
        }
    }
    std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b){return input.components[a].range.first<input.components[b].range.first;});
    std::vector<efvk::BoneComponent> ordered;
    for(uint32_t id:ids){
        const auto& range=input.components[id].range;
        // Bones between components stay as initialized, as the Core's merged skeleton leaves them; two
        // components may share bones, as its pools allow (BUG-093).
        dense[id]=static_cast<uint32_t>(ordered.size());ordered.push_back(range);
    }
    definition_=std::move(input);dense_=std::move(dense);ordered_=std::move(ordered);
    palettes_.clear();serial_=0;begun_=false;return true;
}
bool PaletteBank::begin_update(uint64_t serial,std::string& reason)
{
    reason.clear();
    if(!definition_.bones || (begun_ && serial<serial_)){reason="Palette update is unconfigured or went backwards";return false;}
    for(auto& item:palettes_)if(!item.second.skeleton.begin_update(serial,reason))return false;
    serial_=serial;begun_=true;return true;
}
efvk::BoneImportResult PaletteBank::import_component(uint64_t identity,uint32_t component,uint32_t lod,
    const efvk::InstanceEntry& entry,efvk::BoneWindow current,efvk::BoneWindow previous,float scale,std::string& reason)
{
    reason.clear();auto fail=[&](const char* text){reason=text;return efvk::BoneImportResult::Rejected;};
    if(!begun_ || !identity || component>=dense_.size() || dense_[component]==UINT32_MAX || lod>=2)
        return fail("Palette import requires an active identity, component and supported LOD");
    const auto& remap=definition_.components[component].lods[lod];
    if(!remap.available)return fail("Palette LOD remap is unassigned");
    auto found=palettes_.find(identity);Palette pending;
    if(found==palettes_.end()){
        if(palettes_.size()>=definition_.instances)return fail("Palette instance capacity exhausted");
        if(!pending.skeleton.configure(definition_.bones,1,ordered_,reason) || !pending.skeleton.begin_update(serial_,reason))
            return efvk::BoneImportResult::Rejected;
        pending.coverage.resize(ordered_.size(),0);
    }
    auto& target=found==palettes_.end()?pending:found->second;
    const auto result=target.skeleton.import_component(0,dense_[component],lod,entry,current,previous,remap.indices,scale,reason);
    if(result==efvk::BoneImportResult::Rejected)return result;
    if(result==efvk::BoneImportResult::Imported)target.coverage[dense_[component]]|=dynamic_poses(entry);
    if(found==palettes_.end())palettes_.emplace(identity,std::move(pending));
    return result;
}
bool PaletteBank::snapshot(uint64_t identity,const efvk::InstanceEntry& entry,
    std::shared_ptr<const PaletteSnapshot>& output,std::string& reason) const
{
    reason.clear();auto fail=[&](const char* text){reason=text;return false;};
    auto found=palettes_.find(identity);
    if(!begun_ || found==palettes_.end())return fail("Palette identity has no imported master");
    const auto needed=dynamic_poses(entry);const auto& palette=found->second;
    for(uint8_t available:palette.coverage)if((available&needed)!=needed)return fail("Palette has missing component poses");
    auto next=std::make_shared<PaletteSnapshot>();next->identity=identity;next->update=serial_;
    if(!palette.skeleton.patch_entry(0,entry,next->entry,reason))return false;
    next->bones=palette.skeleton.bytes();output=std::move(next);return true;
}
}
