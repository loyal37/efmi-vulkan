#include "shader_fixes.h"
#include "character_backend_internal.h"
#include "../../frontend/gpu/native_shader.h"
#include <fstream>
#include <regex>

namespace efvk {
std::shared_ptr<const NativeShaderSet> load_shader_fixes(Device* device, const std::shared_ptr<const NativeShaderSet>& previous,
                                                       const std::map<uint32_t, int>& depth_filters)
{
    static std::atomic<uint64_t> serial{0};
    auto next = std::make_shared<NativeShaderSet>();
    next->depth_filters = depth_filters;
    next->compatible = &efmi::gpu::native_shader_interface;
    const auto tools = module_folder();
    const auto folder = (g_cfg.m2_settings.empty() ? tools / "settings" :
                         std::filesystem::absolute(std::filesystem::u8path(g_cfg.m2_settings))) / "ShaderFixes";
    efmi::gpu::ShaderHost host;
    host.validator = tools / "SPIRV-Tools-shared.dll";
    host.float_controls = device->float_controls; host.float_properties = device->float_properties;
    host.shader_draw_parameters = device->draw_parameters;
    const uint32_t api = std::min(device->app_api_version, device->api_version);
    std::error_code error;
    if (!std::filesystem::exists(folder, error) && !error) return next;
    struct Interface { std::shared_ptr<const ShaderInfo> shader; std::string entry; };
    std::map<uint64_t, std::vector<Interface>> interfaces;
    {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        for (const auto& pipe : device->pipes) if (pipe.second.recipe)
            for (const auto& stage : pipe.second.recipe->stages) if (stage.shader)
                interfaces[(uint64_t(stage.stage) << 32) | stage.shader->hash].push_back({stage.shader, stage.entry});
    }
    const std::regex naming("^([0-9a-fA-F]{8})-(vs|hs|ds|gs|ps)_replace\\.spv$", std::regex::icase);
    std::filesystem::directory_iterator it(folder, error), end;
    while (!error && it != end) {
        const auto path = it->path();
        std::smatch match;
        const std::string name = path.filename().u8string();
        if (it->is_regular_file(error) && std::regex_match(name, match, naming)) {
            const uint32_t hash = uint32_t(std::stoul(match[1].str(), nullptr, 16));
            const char letter = char(std::tolower(match[2].str()[0]));
            const auto stage = letter == 'v' ? VK_SHADER_STAGE_VERTEX_BIT : letter == 'h' ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT :
                               letter == 'd' ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : letter == 'g' ? VK_SHADER_STAGE_GEOMETRY_BIT :
                               VK_SHADER_STAGE_FRAGMENT_BIT;
            const uint64_t key = (uint64_t(stage) << 32) | hash;
            std::string reason;
            const auto size = std::filesystem::file_size(path, error);
            auto shader = std::make_shared<ShaderInfo>();
            if (error || size < 20 || size > (16u << 20) || size % 4) reason = "invalid SPIR-V file size";
            else {
                shader->code.resize(size_t(size / 4));
                std::ifstream file(path, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(shader->code.data()), std::streamsize(size))) reason = "cannot read SPIR-V file";
                else efmi::gpu::validate_native_shader(shader->code, stage, host, device->enabled_features, api, reason);
            }
            if (reason.empty()) {
                const auto known = interfaces.find(key);
                if (known != interfaces.end()) for (const auto& original : known->second)
                    if (!efmi::gpu::native_shader_interface(original.shader->code, shader->code, stage, original.entry, reason)) break;
            }
            if (reason.empty()) {
                shader->hash = hash; shader->size = uint32_t(size);
                next->modules[key] = std::move(shader);
            } else {
                next->warnings.push_back(name + ": " + reason);
                // ReloadShader keeps the last good replacement on a compilation/validation failure.
                if (previous) {
                    const auto old = previous->modules.find(key);
                    if (old != previous->modules.end()) next->modules[key] = old->second;
                }
            }
            error.clear();
        }
        it.increment(error);
    }
    if (error) {
        next->warnings.push_back("Cannot read ShaderFixes directory: " + error.message());
        if (previous) next->modules = previous->modules;
    }
    if (!next->modules.empty()) {
        bool same = previous && previous->modules.size() == next->modules.size() && previous->depth_filters == next->depth_filters;
        if (same) for (const auto& item : next->modules) {
            const auto old = previous->modules.find(item.first);
            if (old == previous->modules.end() || old->second->code != item.second->code) { same = false; break; }
        }
        next->revision = same ? previous->revision : ++serial;
    }
    return next;
}
}
