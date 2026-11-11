#include "owned_generation.h"

namespace efmi::frontend {
namespace {
class OwnedGeneration final : public RuntimeExtension {
    std::shared_ptr<RuntimeExtension> native_;
    std::shared_ptr<GpuShaderRuntime> gpu_;
    bool native_phase_=false;
    struct NativePhase {
        bool& flag;bool previous;
        explicit NativePhase(bool& value):flag(value),previous(value){flag=true;}
        ~NativePhase(){flag=previous;}
    };
public:
    OwnedGeneration(std::shared_ptr<RuntimeExtension> native,std::shared_ptr<GpuShaderRuntime> gpu):native_(std::move(native)),gpu_(std::move(gpu)){
        if(!native_ || !gpu_)throw std::runtime_error("An owned generation needs both a native and a GPU extension");
    }
    std::wstring declarations() override {return native_->declarations();}
    std::vector<std::wstring> declaration_units() override {return native_->declaration_units();}
    std::vector<std::wstring> native_shaders() override {return native_->native_shaders();}
    bool custom_shader_execution() const override {return true;}
    std::vector<std::wstring> install(DrawPlanAdapter& adapter) override {
        auto boundaries=native_->install(adapter);
        gpu_->exclude_shaders(native_->native_shaders());
        auto more=gpu_->install(adapter);boundaries.insert(boundaries.end(),more.begin(),more.end());
        using Site=GpuResourceRuntime::ExecutionSite;
        adapter.owned_scope([this]{return !native_phase_ && gpu_->resources().scope_active() && gpu_->resources().site()!=Site::Draw;});
        adapter.draw_scope([this]{return !native_phase_ && gpu_->resources().scope_active() && gpu_->resources().site()==Site::Draw;});
        return boundaries;
    }
    bool initialize_parameters(CommandListState& state,std::wstring& why) override {return gpu_->initialize_parameters(state,why);}
    bool prepare(DrawPlanAdapter& adapter,CommandListState& state,std::wstring& why) override {
        NativePhase phase(native_phase_);return native_->prepare(adapter,state,why) && gpu_->prepare(adapter,state,why);
    }
    bool validate_overrides(std::wstring& why) override {return native_->validate_overrides(why) && gpu_->validate_overrides(why);}
    bool before_draw(const RuntimeDraw& draw,CommandListState& state,std::wstring& why) override {
        return native_->before_draw(draw,state,why) && gpu_->before_draw(draw,state,why);
    }
    void draw_hooks(const RuntimeDraw& draw,DrawHooks& hooks) override {native_->draw_hooks(draw,hooks);}
    void after_draw() noexcept override {native_->after_draw();gpu_->after_draw();}
    void after_dispatch() noexcept override {native_->after_dispatch();gpu_->after_dispatch();}
    void library_hook(LibraryHook hook) noexcept override {native_->library_hook(hook);gpu_->library_hook(hook);}
    void execution_failed() noexcept override {native_->execution_failed();gpu_->execution_failed();}
};
}
std::shared_ptr<RuntimeExtension> owned_generation(std::shared_ptr<RuntimeExtension> native,std::shared_ptr<GpuShaderRuntime> gpu){
    return std::make_shared<OwnedGeneration>(std::move(native),std::move(gpu));
}
}
