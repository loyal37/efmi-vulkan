#include "palette_files.h"
#include <algorithm>
namespace efmi::frontend {
bool load_palette_files(const SkeletonFilePlan& input,core::PaletteDefinition& output,std::string& reason)
{
    reason.clear();auto fail=[&](const char* text){reason=text;return false;};
    core::PaletteDefinition next;next.bones=input.bones;next.instances=input.instances;
    if(input.components.size()>256 || !input.lod_count || input.lod_count>4)return fail("Invalid palette file plan dimensions");
    next.components.resize(input.components.size());
    for(size_t c=0;c<input.components.size();++c){
        const auto& source=input.components[c];auto& component=next.components[c];
        component.range={source.first,source.count};
        for(uint32_t lod=0;lod<std::min(input.lod_count,2u);++lod){
            const auto& file=source.lods[lod];auto& target=component.lods[lod];
            // A LOD whose pool element nothing assigned binds nothing in the original Core
            // (MergedSkeleton.ini: `cs-t1 = ref Pool..._LodRemaps[component * levels + lod]`, then
            // `z0 = @cs-t1 != 0`): no remap, the same as an element assigned null.
            target.available=true;
            if(file.source!=RemapSource::File)continue;
            if(file.source!=RemapSource::File || !file.path.is_absolute() || !source.count || source.count>65536)
                return fail("Invalid R16 remap file declaration");
        }
    }
    // Validate all dimensions and ranges before opening any source asset.
    core::PaletteBank validation;
    if(!validation.configure(next,reason))return false;
    for(size_t c=0;c<input.components.size();++c){
        const auto& source=input.components[c];
        for(uint32_t lod=0;lod<std::min(input.lod_count,2u);++lod){
            const auto& file=source.lods[lod];auto& target=next.components[c].lods[lod];
            if(file.source!=RemapSource::File)continue;
            HANDLE handle=CreateFileW(file.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE)return fail("Cannot open a stable R16 remap file");
            struct Close {HANDLE handle;~Close(){CloseHandle(handle);}} close{handle};
            LARGE_INTEGER size{};const DWORD expected=source.count*2;
            if(!GetFileSizeEx(handle,&size) || size.QuadPart!=expected)return fail("R16 remap file length differs from component count");
            target.indices.resize(source.count);DWORD read=0;
            if(!ReadFile(handle,target.indices.data(),expected,&read,nullptr) || read!=expected)return fail("R16 remap file read is incomplete");
        }
    }
    output=std::move(next);return true;
}
}
