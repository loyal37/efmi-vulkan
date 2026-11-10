#include "owned_runtime.h"
#include "owned_generation.h"

namespace efmi::frontend {
struct OwnedRuntime::Impl {
    std::shared_ptr<GpuShaderRuntime> gpu;
};
OwnedRuntime::OwnedRuntime(gpu::ResourceDevice device,gpu::ShaderHost host,const Tools& tools,std::filesystem::path working):impl_(std::make_unique<Impl>()){
    impl_->gpu=std::make_shared<GpuShaderRuntime>(device,std::move(host),tools.compiler,gpu::ResourceBudgets{},std::move(working));
    impl_->gpu->enable_cached_bytecode(tools.bytecode,tools.mode);
    // Without its tools' identity the cache stays off; the generation compiles as before.
    if(!tools.cache.empty())try{impl_->gpu->enable_cache(std::make_shared<gpu::ShaderCache>(tools.cache,std::vector<std::filesystem::path>{tools.compiler,tools.bytecode}));}
    catch(const std::exception&){}
}
OwnedRuntime::~OwnedRuntime()=default;
std::shared_ptr<RuntimeExtension> OwnedRuntime::combine(std::shared_ptr<RuntimeExtension> native){return owned_generation(std::move(native),impl_->gpu);}
void OwnedRuntime::begin(VkQueue queue){impl_->gpu->resources().begin(queue,GpuResourceRuntime::ExecutionSite::OutsideRenderPass);}
void OwnedRuntime::begin_deferred(){impl_->gpu->resources().begin(VK_NULL_HANDLE,GpuResourceRuntime::ExecutionSite::Deferred);}
void OwnedRuntime::begin_draw(){impl_->gpu->resources().begin(VK_NULL_HANDLE,GpuResourceRuntime::ExecutionSite::Draw);}
bool OwnedRuntime::pending() const {return impl_->gpu->resources().pending();}
VkQueue OwnedRuntime::queue() const {return impl_->gpu->resources().queue();}
void OwnedRuntime::rebind(VkQueue queue){impl_->gpu->resources().rebind(queue);}
void OwnedRuntime::suspend(){impl_->gpu->resources().suspend();}
void OwnedRuntime::discard(){impl_->gpu->resources().discard();}
bool OwnedRuntime::scope_active() const {return impl_->gpu->resources().scope_active();}
bool OwnedRuntime::failed() const {return impl_->gpu->resources().failed();}
std::shared_ptr<gpu::ResourceSubmission> OwnedRuntime::finish(const gpu::ResourceCommands::SubmitSync& sync,VkQueue queue){
    return impl_->gpu->resources().finish(sync,queue);
}
void OwnedRuntime::backbuffer(gpu::Image image,DXGI_FORMAT format,bool shader_resource){
    impl_->gpu->resources().backbuffer(std::move(image),format,D3D11_BIND_RENDER_TARGET|(shader_resource?D3D11_BIND_SHADER_RESOURCE:0));
}
gpu::ResourceCommands& OwnedRuntime::commands(){return impl_->gpu->resources().commands();}
OwnedRuntime::ShaderStats OwnedRuntime::shader_stats() const{
    const auto& s=impl_->gpu->shader_stats();return {s.compiled,s.translated,s.compile_us,s.translate_us,s.cached};
}
bool OwnedRuntime::backbuffer_targeted() const {return impl_->gpu->resources().backbuffer_targeted();}
}
