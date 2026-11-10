#include "gpu_resources.h"
#include "globals.h"
#include <stdexcept>

namespace efmi::frontend {
namespace {
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
std::wstring widen(const char* text){std::string s(text);return {s.begin(),s.end()};}
}
void GpuResourceRuntime::publish_parameters(){
    active();
    if(failed_)return; // the scope's work is dropped
    require(parameters_.image && G && G->iniParams.size()==parameters_.image_description.texture.Width,
            "GPU parameter allocation does not match this generation");
    static_assert(sizeof(DirectX::XMFLOAT4)==16);
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.imageExtent={UINT(G->iniParams.size()),1,1};
    // write owns a staging copy immediately. Later CPU assignments cannot
    // change the values recorded at this original command-list boundary.
    commands_.write(parameters_.image,region,G->iniParams.data(),G->iniParams.size()*16);
    ++parameter_uploads_;
}
bool GpuResourceRuntime::initialize_parameters(CommandListState&,std::wstring& why){
    try{
        require(installed_ && !parameters_initialized_ && G,"GPU parameters require a fresh installed generation");
        const auto count=G->iniParams.size();
        require(count<=D3D11_REQ_TEXTURE1D_U_DIMENSION,"GPU parameters exceed the original Texture1D width limit");
        if(count){
            active();
            gpu::ImageDescription desc;desc.type=VK_IMAGE_TYPE_1D;desc.format=VK_FORMAT_R32G32B32A32_SFLOAT;
            desc.extent={UINT(count),1,1};desc.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
            parameters_.image=commands_.create_image(desc);
            auto& source=parameters_.image_description;source.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE1D;
            source.texture={UINT(count),1,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DYNAMIC,
                D3D11_BIND_SHADER_RESOURCE,D3D11_CPU_ACCESS_WRITE,0};
            parameters_.format=DXGI_FORMAT_R32G32B32A32_FLOAT;
            image_view(parameters_,GpuResourceBinding::View::ShaderResource,true);
            publish_parameters();
            // Logical initial stage bindings only. Explicit INI assignments can
            // replace/unbind these; later publication must not restore them.
            if(G->IniParamsReg>=0){
                require(G->IniParamsReg<D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,"GPU parameter register invalid");
                for(auto stage:std::wstring(L"vhdgpc"))slots_[{stage,ResourceCopyTargetType::SHADER_RESOURCE,unsigned(G->IniParamsReg)}]=parameters_;
            }
        }
        parameters_initialized_=true;return true;
    }catch(const std::exception& e){execution_failed();why=widen(e.what());return false;}
}
bool GpuResourceRuntime::flush_parameters(CommandListState& state,bool force,std::wstring& why){
    try{
        require(parameters_initialized_,"GPU parameters used before initialization");
        // Empty reservation has no texture in the original device. Keep its
        // dirty flag, just as CommandListFlushState does without mIniTexture.
        if(!parameters_.image || (!force && !state.update_params))return true;
        recorded_=true;publish_parameters();state.update_params=false;return true;
    }catch(const std::exception& e){execution_failed();why=widen(e.what());return false;}
}
}
