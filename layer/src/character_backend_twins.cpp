// The application's pipelines without the character dither (D-133): while a Mod's discard gate is set, a
// pipeline whose fragment stage has the dither is bound as its twin.
#include "character_backend_internal.h"

namespace efvk {
namespace character_internal {
// D-133: a ShaderRegex replacement's discard gate is set (RabbitFX's IniParams[172].w = $censor): the
// character dither is not drawn, as the replaced shader skips its discard. Set at each presentation.
std::atomic<bool> g_dither_off{false};
// Whether the Mods have such a gate at all: then both states are made ahead of the key, as the original's
// replaced shader exists from the game's shader creation and the key only flips its branch.
std::atomic<bool> g_dither_gated{false};
// Whether a pipeline's fragment stage has the character dither, by the pipeline's id (read once).
std::shared_mutex g_dither_mx;
std::unordered_map<uint32_t, bool> g_dither_pipelines;
bool pipeline_dither(uint32_t id, const GraphicsRecipe& recipe)
{
    {
        std::shared_lock<std::shared_mutex> lock(g_dither_mx);
        const auto found = g_dither_pipelines.find(id);
        if (found != g_dither_pipelines.end()) return found->second;
    }
    bool dither = false;
    for (const auto& stage : recipe.stages)
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage.shader) dither = efmi::gpu::dither_kills(stage.shader->code) > 0;
    std::unique_lock<std::shared_mutex> lock(g_dither_mx);
    if (g_dither_pipelines.size() >= 16384) g_dither_pipelines.clear();
    g_dither_pipelines[id] = dither;
    return dither;
}
} // namespace character_internal

namespace {
// A recording that bound twins keeps them until it completed: the application may destroy its pipeline
// meanwhile, and the variant cache retire the twin with it.
struct TwinOwner final : CommandOwner {
    std::vector<std::shared_ptr<const PipelineVariant>> held;
    void reset_after_completion() override { held.clear(); used = false; }
    bool seal() override { return false; }
};
} // namespace

// At the application's graphics pipeline bind (D-133): while a discard gate is set (RabbitFX's anti-
// dither), a pipeline whose fragment stage has the character dither is bound as its twin without it, as
// the original's replaced shader skips the discard on every draw it makes. The twin is a variant of the
// pipeline (its own vertex stage and images, the dither branch steered away) built by the variant cache's
// workers; the first bind waits for it, and binds the application's pipeline only when that wait runs out
// or the twin cannot be made. Recording thread; the gate off costs one load.
VkPipeline character_bind(CharacterBackend& b, CmdBuf* cb, VkPipeline pipeline)
{
    // RabbitFX's state alone decides, as under DX11 its replaced shaders do whatever the character Mods do.
    if (!g_dither_off.load(std::memory_order_relaxed) || !b.variants || !cb) return pipeline;
    uint32_t id = 0;
    std::shared_ptr<const GraphicsRecipe> recipe;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        const auto pipe = b.device->pipes.find(pipeline);
        if (pipe == b.device->pipes.end() || !pipe->second.recipe || !pipe->second.layout) return pipeline;
        id = pipe->second.id; recipe = pipe->second.recipe; layout = pipe->second.layout;
    }
    if (!pipeline_dither(id, *recipe)) return pipeline;
    std::shared_ptr<const PipelineVariant> twin;
    {
        std::shared_lock<std::shared_mutex> lock(b.twins_mx);
        if (b.twins_failed.count(id)) return pipeline;
        const auto found = b.twins.find(id);
        if (found != b.twins.end()) twin = found->second;
    }
    if (!twin) {
        // Prepared when the game created the pipeline (character_created), so normally made by now; a twin
        // still being built is waited for (bounded), as a replaced draw waits for its variant (D-103).
        const auto result = b.variants->await(twin_identity(id), recipe, layout, recipe->vertex, twin_patch(), std::chrono::milliseconds(4000));
        std::unique_lock<std::shared_mutex> lock(b.twins_mx);
        if (result.variant) twin = b.twins[id] = result.variant;
        else {
            if (result.state == VariantState::Failed && b.twins_failed.insert(id).second)
                logf("character dither: no twin for pipeline %u: %s", id, result.reason.c_str());
            return pipeline;
        }
    }
    // BUG-089: the generic owner_for dropped every package's snapshot batch this recording had not used yet.
    const auto held = character_owner_for(cb->owners, &b.twins, [] { return std::make_shared<TwinOwner>(); });
    if (!held) return pipeline;
    auto& owner = static_cast<TwinOwner&>(*held);
    owner.used = true;
    if (std::find(owner.held.begin(), owner.held.end(), twin) == owner.held.end()) owner.held.push_back(twin);
    ++b.twin_binds;
    return twin->pipeline;
}
// At the application's graphics pipeline creation (D-133): when the Mods have a discard gate, the twin of a
// pipeline with the character dither is prepared at once, behind the variants draws wait for, as the
// original replaces the shader when the game creates it; the key then only changes what is bound.
void prepare_twin(CharacterBackend& b, uint32_t id, const std::shared_ptr<const GraphicsRecipe>& recipe,
                  const std::shared_ptr<const PipelineLayoutInfo>& layout)
{
    if (!recipe || !layout || !pipeline_dither(id, *recipe)) return;
    const auto result = b.variants->prepare(twin_identity(id), recipe, layout, recipe->vertex, twin_patch());
    if (result.state == VariantState::Queued) ++b.twins_prepared;
}
void character_created(CharacterBackend& b, VkPipeline pipeline)
{
    if (!g_dither_gated.load(std::memory_order_relaxed) || !b.variants) return;
    uint32_t id = 0;
    std::shared_ptr<const GraphicsRecipe> recipe;
    std::shared_ptr<const PipelineLayoutInfo> layout;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        const auto pipe = b.device->pipes.find(pipeline);
        if (pipe == b.device->pipes.end()) return;
        id = pipe->second.id; recipe = pipe->second.recipe; layout = pipe->second.layout;
    }
    prepare_twin(b, id, recipe, layout);
}
// A backend made while the game already has its pipelines (a module exchange): their twins, as above.
void prepare_twins(CharacterBackend& b)
{
    if (!g_dither_gated.load(std::memory_order_relaxed) || !b.variants) return;
    std::vector<std::tuple<uint32_t, std::shared_ptr<const GraphicsRecipe>, std::shared_ptr<const PipelineLayoutInfo>>> pipes;
    {
        std::shared_lock<std::shared_mutex> resources(b.device->res_mx);
        pipes.reserve(b.device->pipes.size());
        for (const auto& pipe : b.device->pipes)
            if (pipe.second.recipe && pipe.second.layout) pipes.emplace_back(pipe.second.id, pipe.second.recipe, pipe.second.layout);
    }
    for (const auto& [id, recipe, layout] : pipes) prepare_twin(b, id, recipe, layout);
}

} // namespace efvk
