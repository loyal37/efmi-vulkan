#pragma once
// The state a replaced draw's lists run against (docs/architecture-review-2.1.md Phase 1, D-161).
//
// The original's commands are calls on D3D11's device context. A resource assignment sets a slot of
// that context (ResourceCopyTarget::SetResource: IASetVertexBuffers, XXSetShaderResources,
// XXSetConstantBuffers, OMSetRenderTargets, ...), whatever the slot and whether or not the pipeline
// reads it, and it cannot fail; a draw then reads the context as it stands. What D3D11's runtime
// gives the original for nothing is what this file states for the Vulkan layer:
//
// - the context itself: DrawState, one value per slot of the input assembler, of every stage's
//   shader resources and constant buffers, of the output merger and of stream output. A slot the
//   lists did not set holds the application's binding of that slot of the matched draw;
// - a value that moves: `ResourceX = ref ps-t0`, `ps-t3 = ref ResourceX`, `vb3 = vb0` carry "the
//   application's binding of slot S" between slots and declarations as D3D11 carries the object;
// - what a draw makes of the state: decided when the draw is recorded, per slot, by honour(), the one
//   table of what the layer can give a draw in Vulkan terms. An assignment never refuses.
//
// D3D11's runtime also keeps a bound object alive and orders the calls of one context; the plan needs
// neither: its values name declarations of the generation, which outlive the plan, and slots of the
// matched draw, which mean something for the length of that draw's lists only (SlotValue::Foreign after).
#include "CommandList.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace efmi::frontend {

// A buffer the owned GPU work of a replaced draw wrote (P2, D-087): a Vulkan handle on the
// application's device, bound as it is. `keep` holds the allocation while a value or an item lives.
struct OwnedPlanBuffer { uint64_t buffer = 0, offset = 0, size = 0; uint32_t stride = 0; std::shared_ptr<void> keep; };

enum class SlotKind : uint8_t { Vertex, Index, ShaderResource, ConstantBuffer, RenderTarget, DepthTarget, UnorderedAccess, StreamOutput };

// D3D11's slot counts, as the original's parser bounds them (ParseTargetPipelineSlot).
constexpr unsigned kVertexSlots = 32, kResourceSlots = 128, kConstantSlots = 14, kTargetSlots = 8, kAccessSlots = 64, kStreamSlots = 4;
constexpr unsigned kStages = 6; // v, h, d, g, p, c
constexpr unsigned kPixelStage = 4, kComputeStage = 5;

struct SlotId {
    SlotKind kind = SlotKind::Vertex;
    uint8_t stage = 0;  // the index of the stage's letter in "vhdgpc" where the slot is a stage's, else 0
    uint16_t slot = 0;
    bool operator==(const SlotId& o) const { return kind == o.kind && stage == o.stage && slot == o.slot; }
    bool operator!=(const SlotId& o) const { return !(*this == o); }
};
// `vb3`, `ib`, `ps-t5`, `vs-cb1`, `o0`, `oD`, `cs-u2`, `so1`: as a Mod writes the slot.
std::wstring slot_name(const SlotId&);
// The slot a pipeline target names with its slot number resolved; false for a target that is no slot
// of the context (a declaration, null, IniParams, the back buffer) or a number D3D11 has no slot for.
bool slot_of(const ResourceCopyTarget& target, unsigned resolved, SlotId& out);

struct SlotValue {
    enum class Kind : uint8_t {
        Application, // the application's binding of slot `source` of the matched draw
        Resource,    // a resource of the Mod: `resource`, a file or what the generation declares
        Owned,       // a buffer the draw's owned GPU work wrote: `owned`, of the declaration `resource`
        Null,        // nothing
        Foreign      // a resource the plan does not hold: IniParams, the cursor, the back buffer, a
                     // view of a region, or an application binding taken in an earlier draw
    };
    Kind kind = Kind::Null;
    SlotId source{};
    CustomResource* resource = nullptr;
    std::shared_ptr<const OwnedPlanBuffer> owned;
    const CommandListCommand* by = nullptr; // the command that put it, for the notice
    bool null() const { return kind == Kind::Null; }
    bool same(const SlotValue& o) const
    {
        if (kind != o.kind) return false;
        switch (kind) {
        case Kind::Application: return source == o.source;
        case Kind::Resource: return resource == o.resource;
        case Kind::Owned: return owned == o.owned;
        default: return true;
        }
    }
    static SlotValue application(const SlotId& slot) { SlotValue v; v.kind = Kind::Application; v.source = slot; return v; }
};

// The context of one execution. Sparse: the lists of a draw set a handful of the thousand slots, and
// an execution starts by forgetting them (no table is cleared).
class DrawState {
public:
    static constexpr unsigned kCells = kVertexSlots + 1 + kStages * kResourceSlots + kStages * kConstantSlots + kTargetSlots + 1 +
                                       2 * kAccessSlots + kStreamSlots;
    static unsigned cell(const SlotId&);
    static SlotId slot(unsigned cell);
    struct Entry {
        uint16_t cell = 0;
        SlotValue value;  // what the slot holds
        SlotValue issued; // what the plan's items so far leave bound there (issue())
    };
    void reset() { entries_.clear(); }
    // What the slot holds: the application's own binding where the lists set nothing.
    SlotValue get(const SlotId& id) const
    {
        const Entry* e = find(cell(id));
        return e ? e->value : SlotValue::application(id);
    }
    bool in_place(const SlotId& id) const
    {
        const Entry* e = find(cell(id));
        return !e || (e->value.kind == SlotValue::Kind::Application && e->value.source == id);
    }
    void set(const SlotId& id, SlotValue value)
    {
        const unsigned c = cell(id);
        if (Entry* e = const_cast<Entry*>(find(c))) { e->value = std::move(value); return; }
        at_[c] = uint16_t(entries_.size());
        Entry e; e.cell = uint16_t(c); e.value = std::move(value); e.issued = SlotValue::application(id);
        entries_.push_back(std::move(e));
    }
    // The slots the lists set, in the order they first set them.
    std::vector<Entry>& entries() { return entries_; }
    const std::vector<Entry>& entries() const { return entries_; }
private:
    const Entry* find(unsigned c) const
    {
        const unsigned at = at_[c];
        return at < entries_.size() && entries_[at].cell == c ? &entries_[at] : nullptr;
    }
    std::vector<Entry> entries_;
    std::array<uint16_t, kCells> at_{};
};

// What a draw makes of one slot of the state it sees.
enum class Honour : uint8_t {
    Honoured,    // the draw is issued with that binding
    Ignored,     // the draw's pipeline does not read the slot: nothing to do, nothing to say
    Degraded,    // the layer cannot bind it there: the application's binding stays, a notice says so
    NotDrawable, // the draw would not reach the game's target: that draw is left out, a notice says so
    GameDraw     // the Mod's geometry cannot be drawn with it: the matched draw stays the game's
};
// What the host knows of the pipeline the matched draw runs with.
struct PipelineFacts {
    // Its shader stages as Vulkan stage bits (vertex 1, tessellation control 2, evaluation 4, geometry 8,
    // fragment 16); 0 when not known, and every graphics stage is then taken as present.
    uint32_t stages = 0;
    // Bit N: its vertex input reads binding N, DX11's input slot N. Every bit when not known.
    uint32_t vertex_slots = UINT32_MAX;
    // Bit N: its pass has colour target N (DX11's oN); bit 8: a depth target (oD). Every bit when not known.
    uint32_t targets = UINT32_MAX;
};
enum class SlotClass : uint8_t { VertexBuffer, UnreadVertex, IndexBuffer, PixelTexture, StageBinding, AbsentStage, Compute, Output, AbsentOutput, PixelAccess, StreamOutput, Count };
enum class ValueClass : uint8_t { InPlace, ModResource, Null, MovedAlike, Other, Count };
struct HonourRow {
    SlotClass slots; ValueClass value; Honour honour;
    const char* slot_text; const char* value_text; const char* note;
};
// The table, one row per class of slot and of value. honour() reads it; the execution matrix prints it.
const std::vector<HonourRow>& honour_table();
SlotClass slot_class(const SlotId&, const PipelineFacts&);
ValueClass value_class(const SlotId&, const SlotValue&);
Honour honour(const SlotId&, const SlotValue&, const PipelineFacts&);
const char* honour_name(Honour);

// An operand the host cannot answer for a replaced draw (P1-4): the value the expression goes on with
// and the notice that says so, in place of refusing the list. One table; the execution matrix prints it.
enum class OperandGap : uint8_t {
    RenderTargetSize, Scissor, HostSample, ShaderFilter, ShaderDeclaredType, ShaderDeclaredSize,
    ResourceFilter, ResourceIdentity, ResourceProperty, ResourceHash, Evaluation, Count
};
struct OperandRow { OperandGap gap; float value; const char* operand; const char* basis; };
const std::vector<OperandRow>& operand_table();
float operand_value(OperandGap);

// ---- definitions ----

namespace draw_state_detail {
constexpr unsigned kIndexCell = kVertexSlots;
constexpr unsigned kResourceCells = kIndexCell + 1;
constexpr unsigned kConstantCells = kResourceCells + kStages * kResourceSlots;
constexpr unsigned kTargetCells = kConstantCells + kStages * kConstantSlots;
constexpr unsigned kDepthCell = kTargetCells + kTargetSlots;
constexpr unsigned kAccessCells = kDepthCell + 1;
constexpr unsigned kStreamCells = kAccessCells + 2 * kAccessSlots;
constexpr wchar_t kStageLetters[] = L"vhdgpc";
inline int stage_of(wchar_t letter)
{
    for (int i = 0; i < int(kStages); ++i) if (kStageLetters[i] == letter) return i;
    return -1;
}
}
using namespace draw_state_detail;
static_assert(kStreamCells + kStreamSlots == DrawState::kCells, "the cells are the slots, each once");

inline unsigned DrawState::cell(const SlotId& id)
{
    switch (id.kind) {
    case SlotKind::Vertex: return id.slot;
    case SlotKind::Index: return kIndexCell;
    case SlotKind::ShaderResource: return kResourceCells + id.stage * kResourceSlots + id.slot;
    case SlotKind::ConstantBuffer: return kConstantCells + id.stage * kConstantSlots + id.slot;
    case SlotKind::RenderTarget: return kTargetCells + id.slot;
    case SlotKind::DepthTarget: return kDepthCell;
    case SlotKind::UnorderedAccess: return kAccessCells + (id.stage == kComputeStage ? kAccessSlots : 0) + id.slot;
    default: return kStreamCells + id.slot;
    }
}
inline SlotId DrawState::slot(unsigned c)
{
    SlotId id;
    if (c < kIndexCell) { id.kind = SlotKind::Vertex; id.slot = uint16_t(c); }
    else if (c == kIndexCell) id.kind = SlotKind::Index;
    else if (c < kConstantCells) {
        id.kind = SlotKind::ShaderResource; id.stage = uint8_t((c - kResourceCells) / kResourceSlots); id.slot = uint16_t((c - kResourceCells) % kResourceSlots);
    } else if (c < kTargetCells) {
        id.kind = SlotKind::ConstantBuffer; id.stage = uint8_t((c - kConstantCells) / kConstantSlots); id.slot = uint16_t((c - kConstantCells) % kConstantSlots);
    } else if (c < kDepthCell) { id.kind = SlotKind::RenderTarget; id.slot = uint16_t(c - kTargetCells); }
    else if (c == kDepthCell) id.kind = SlotKind::DepthTarget;
    else if (c < kStreamCells) {
        id.kind = SlotKind::UnorderedAccess; id.stage = uint8_t(c - kAccessCells >= kAccessSlots ? kComputeStage : kPixelStage);
        id.slot = uint16_t((c - kAccessCells) % kAccessSlots);
    } else { id.kind = SlotKind::StreamOutput; id.slot = uint16_t(c - kStreamCells); }
    return id;
}

inline std::wstring slot_name(const SlotId& id)
{
    const std::wstring number = std::to_wstring(id.slot);
    const std::wstring stage(1, kStageLetters[id.stage < kStages ? id.stage : 0]);
    switch (id.kind) {
    case SlotKind::Vertex: return L"vb" + number;
    case SlotKind::Index: return L"ib";
    case SlotKind::ShaderResource: return stage + L"s-t" + number;
    case SlotKind::ConstantBuffer: return stage + L"s-cb" + number;
    case SlotKind::RenderTarget: return L"o" + number;
    case SlotKind::DepthTarget: return L"oD";
    case SlotKind::UnorderedAccess: return stage + L"s-u" + number;
    default: return L"so" + number;
    }
}

inline bool slot_of(const ResourceCopyTarget& target, unsigned resolved, SlotId& out)
{
    const int stage = stage_of(target.shader_type);
    switch (target.type) {
    case ResourceCopyTargetType::VERTEX_BUFFER:
        if (resolved >= kVertexSlots) return false;
        out = {SlotKind::Vertex, 0, uint16_t(resolved)}; return true;
    case ResourceCopyTargetType::INDEX_BUFFER:
        out = {SlotKind::Index, 0, 0}; return true;
    case ResourceCopyTargetType::SHADER_RESOURCE:
        if (stage < 0 || resolved >= kResourceSlots) return false;
        out = {SlotKind::ShaderResource, uint8_t(stage), uint16_t(resolved)}; return true;
    case ResourceCopyTargetType::CONSTANT_BUFFER:
        if (stage < 0 || resolved >= kConstantSlots) return false;
        out = {SlotKind::ConstantBuffer, uint8_t(stage), uint16_t(resolved)}; return true;
    case ResourceCopyTargetType::RENDER_TARGET:
        if (resolved >= kTargetSlots) return false;
        out = {SlotKind::RenderTarget, 0, uint16_t(resolved)}; return true;
    case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
        out = {SlotKind::DepthTarget, 0, 0}; return true;
    case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
        if ((stage != int(kPixelStage) && stage != int(kComputeStage)) || resolved >= kAccessSlots) return false;
        out = {SlotKind::UnorderedAccess, uint8_t(stage), uint16_t(resolved)}; return true;
    case ResourceCopyTargetType::STREAM_OUTPUT:
        if (resolved >= kStreamSlots) return false;
        out = {SlotKind::StreamOutput, 0, uint16_t(resolved)}; return true;
    default: return false;
    }
}

inline const std::vector<HonourRow>& honour_table()
{
    using S = SlotClass; using V = ValueClass; using H = Honour;
    static const char* const vb = "vbN that the pipeline's vertex input reads";
    static const char* const unread_vb = "vbN that the pipeline's vertex input does not read";
    static const char* const ib = "ib";
    static const char* const pt = "ps-tN";
    static const char* const stage = "vs-tN, hs-tN, ds-tN, gs-tN and a graphics stage's cbN, where the draw's pipeline has that stage";
    static const char* const absent = "a tN or cbN of a graphics stage the draw's pipeline has not";
    static const char* const compute = "cs-tN, cs-cbN, cs-uN";
    static const char* const output = "oN, oD that the draw's pass has";
    static const char* const no_output = "oN, oD that the draw's pass has not";
    static const char* const access = "ps-uN";
    static const char* const stream = "soN";
    static const char* const in_place = "the application's binding of that slot";
    static const char* const mod = "a resource of the Mod (a file, a declared resource, what the draw's owned work wrote)";
    static const char* const null = "null";
    static const char* const moved = "the application's binding of another slot of the same kind and stage";
    static const char* const other = "the application's binding of another kind of slot, or a resource the plan does not hold (IniParams, the cursor, the back buffer, a region view, a binding taken in an earlier draw)";
    static const char* const stays = "the application's binding stays for this draw; one notice per command";
    static const char* const unread = "a draw does not read it";
    static const char* const game = "the Mod's geometry cannot be drawn with it: the matched draw stays the game's; the lists run to their end";
    static const char* const target = "the draw would not reach the game's target: that draw alone is left out, the application's own draw is hidden; one notice per command";
    static const std::vector<HonourRow> rows = {
        {S::VertexBuffer, V::InPlace, H::Honoured, vb, in_place, "nothing to bind"},
        {S::VertexBuffer, V::ModResource, H::Honoured, vb, mod, "bound"},
        {S::VertexBuffer, V::Null, H::Honoured, vb, null, "unbound; with a null position buffer the draw shows nothing, as the original's"},
        {S::VertexBuffer, V::MovedAlike, H::GameDraw, vb, moved, game},
        {S::VertexBuffer, V::Other, H::GameDraw, vb, other, game},
        {S::UnreadVertex, V::InPlace, H::Ignored, unread_vb, in_place, unread},
        {S::UnreadVertex, V::ModResource, H::Ignored, unread_vb, mod, unread},
        {S::UnreadVertex, V::Null, H::Ignored, unread_vb, null, unread},
        {S::UnreadVertex, V::MovedAlike, H::Ignored, unread_vb, moved, unread},
        {S::UnreadVertex, V::Other, H::Ignored, unread_vb, other, unread},
        {S::IndexBuffer, V::InPlace, H::Honoured, ib, in_place, "nothing to bind"},
        {S::IndexBuffer, V::ModResource, H::Honoured, ib, mod, "bound"},
        {S::IndexBuffer, V::Null, H::Honoured, ib, null, "unbound; an indexed draw then shows nothing, as the original's"},
        {S::IndexBuffer, V::MovedAlike, H::GameDraw, ib, moved, game},
        {S::IndexBuffer, V::Other, H::GameDraw, ib, other, game},
        {S::PixelTexture, V::InPlace, H::Honoured, pt, in_place, "nothing to bind"},
        {S::PixelTexture, V::ModResource, H::Honoured, pt, mod, "bound; a resource that is no image the fragment shader can sample leaves the application's texture, with a notice"},
        {S::PixelTexture, V::Null, H::Honoured, pt, null, "unbound"},
        {S::PixelTexture, V::MovedAlike, H::Degraded, pt, moved, stays},
        {S::PixelTexture, V::Other, H::Degraded, pt, other, stays},
        {S::StageBinding, V::InPlace, H::Honoured, stage, in_place, "nothing to bind"},
        {S::StageBinding, V::ModResource, H::Degraded, stage, mod, stays},
        {S::StageBinding, V::Null, H::Degraded, stage, null, stays},
        {S::StageBinding, V::MovedAlike, H::Degraded, stage, moved, stays},
        {S::StageBinding, V::Other, H::Degraded, stage, other, stays},
        {S::AbsentStage, V::InPlace, H::Ignored, absent, in_place, unread},
        {S::AbsentStage, V::ModResource, H::Ignored, absent, mod, unread},
        {S::AbsentStage, V::Null, H::Ignored, absent, null, unread},
        {S::AbsentStage, V::MovedAlike, H::Ignored, absent, moved, unread},
        {S::AbsentStage, V::Other, H::Ignored, absent, other, unread},
        {S::Compute, V::InPlace, H::Ignored, compute, in_place, unread},
        {S::Compute, V::ModResource, H::Ignored, compute, mod, unread},
        {S::Compute, V::Null, H::Ignored, compute, null, unread},
        {S::Compute, V::MovedAlike, H::Ignored, compute, moved, unread},
        {S::Compute, V::Other, H::Ignored, compute, other, unread},
        {S::Output, V::InPlace, H::Honoured, output, in_place, "the game's target"},
        {S::Output, V::ModResource, H::NotDrawable, output, mod, target},
        {S::Output, V::Null, H::NotDrawable, output, null, target},
        {S::Output, V::MovedAlike, H::NotDrawable, output, moved, target},
        {S::Output, V::Other, H::NotDrawable, output, other, target},
        {S::AbsentOutput, V::InPlace, H::Honoured, no_output, in_place, "nothing is bound there"},
        {S::AbsentOutput, V::ModResource, H::NotDrawable, no_output, mod, target},
        {S::AbsentOutput, V::Null, H::Honoured, no_output, null, "nothing is bound there either way"},
        {S::AbsentOutput, V::MovedAlike, H::NotDrawable, no_output, moved, target},
        {S::AbsentOutput, V::Other, H::NotDrawable, no_output, other, target},
        {S::PixelAccess, V::InPlace, H::Honoured, access, in_place, "nothing to bind"},
        {S::PixelAccess, V::ModResource, H::Degraded, access, mod, stays},
        {S::PixelAccess, V::Null, H::Degraded, access, null, stays},
        {S::PixelAccess, V::MovedAlike, H::Degraded, access, moved, stays},
        {S::PixelAccess, V::Other, H::Degraded, access, other, stays},
        {S::StreamOutput, V::InPlace, H::Honoured, stream, in_place, "nothing to bind"},
        {S::StreamOutput, V::ModResource, H::Degraded, stream, mod, "no stream output is made for this draw; one notice per command"},
        {S::StreamOutput, V::Null, H::Degraded, stream, null, "no stream output is made for this draw; one notice per command"},
        {S::StreamOutput, V::MovedAlike, H::Degraded, stream, moved, "no stream output is made for this draw; one notice per command"},
        {S::StreamOutput, V::Other, H::Degraded, stream, other, "no stream output is made for this draw; one notice per command"},
    };
    return rows;
}

inline SlotClass slot_class(const SlotId& id, const PipelineFacts& pipeline)
{
    const uint32_t shader_stages = pipeline.stages;
    // Vulkan's stage bits in the order of "vhdg p": vertex, tessellation control, evaluation, geometry, fragment.
    static const uint32_t bits[kStages] = {0x1u, 0x2u, 0x4u, 0x8u, 0x10u, 0x20u};
    switch (id.kind) {
    case SlotKind::Vertex: return pipeline.vertex_slots >> id.slot & 1u ? SlotClass::VertexBuffer : SlotClass::UnreadVertex;
    case SlotKind::Index: return SlotClass::IndexBuffer;
    case SlotKind::RenderTarget: return pipeline.targets >> id.slot & 1u ? SlotClass::Output : SlotClass::AbsentOutput;
    case SlotKind::DepthTarget: return pipeline.targets >> 8 & 1u ? SlotClass::Output : SlotClass::AbsentOutput;
    case SlotKind::StreamOutput: return SlotClass::StreamOutput;
    case SlotKind::UnorderedAccess: return id.stage == kComputeStage ? SlotClass::Compute : SlotClass::PixelAccess;
    default: break;
    }
    if (id.stage == kComputeStage) return SlotClass::Compute;
    if (shader_stages && !(shader_stages & bits[id.stage])) return SlotClass::AbsentStage;
    return id.kind == SlotKind::ShaderResource && id.stage == kPixelStage ? SlotClass::PixelTexture : SlotClass::StageBinding;
}
inline ValueClass value_class(const SlotId& id, const SlotValue& value)
{
    switch (value.kind) {
    case SlotValue::Kind::Application:
        if (value.source == id) return ValueClass::InPlace;
        return value.source.kind == id.kind && value.source.stage == id.stage ? ValueClass::MovedAlike : ValueClass::Other;
    case SlotValue::Kind::Resource: case SlotValue::Kind::Owned: return ValueClass::ModResource;
    case SlotValue::Kind::Null: return ValueClass::Null;
    default: return ValueClass::Other;
    }
}
inline Honour honour(const SlotId& id, const SlotValue& value, const PipelineFacts& pipeline)
{
    static const auto lookup = [] {
        std::array<std::array<Honour, size_t(ValueClass::Count)>, size_t(SlotClass::Count)> table{};
        for (const auto& row : honour_table()) table[size_t(row.slots)][size_t(row.value)] = row.honour;
        return table;
    }();
    return lookup[size_t(slot_class(id, pipeline))][size_t(value_class(id, value))];
}
inline const char* honour_name(Honour h)
{
    switch (h) {
    case Honour::Honoured: return "honoured";
    case Honour::Ignored: return "ignored";
    case Honour::Degraded: return "degraded";
    case Honour::NotDrawable: return "not-drawable";
    default: return "game-draw";
    }
}

inline const std::vector<OperandRow>& operand_table()
{
    static const std::vector<OperandRow> rows = {
        {OperandGap::RenderTargetSize, -1.0f, "rt_width, rt_height where the host does not know the draw's target",
         "the original's value while no 2D target is bound at o0 (ProcessParamRTSize leaves its -1)"},
        {OperandGap::Scissor, 0.0f, "scissor_left, scissor_top, scissor_right, scissor_bottom",
         "the rectangle RSGetScissorRects returns where none is set"},
        {OperandGap::HostSample, 0.0f, "cursor_hotspot_x, cursor_hotspot_y without a host sample (effective_dpi then reads 96)",
         "the original's values without a cursor, and its least DPI"},
        {OperandGap::ShaderFilter, 0.0f, "vs, hs, ds, gs, ps as a value where nothing answers for the game's shader (cs reads -0)",
         "a bound shader that no ShaderOverride names (process_shader_filter); a draw has no compute shader"},
        {OperandGap::ShaderDeclaredType, 0.0f, "<stage>->cb_type(n), ->srv_type(n) the host cannot read off the game's shader",
         "the type of a register the shader does not declare"},
        {OperandGap::ShaderDeclaredSize, -1.0f, "<stage>->cb_size(n), ->srv_stride(n), ->srv_dimension(n) the host cannot read off the game's shader",
         "the original's answer for a register that is not declared"},
        {OperandGap::ResourceFilter, 0.0f, "a slot, a declaration or `this` as a value where the layer cannot identify the game's resource",
         "a bound resource that no TextureOverride matches (process_texture_filter)"},
        {OperandGap::ResourceIdentity, 1.0f, "`===`, `!==` on a resource nothing identifies (the value is an identity of the operand, never the 0 that reads null)",
         "the original hashes the resource object's pointer. The game's binding of a slot has the slot's identity instead, so one game resource at two slots compares unequal"},
        {OperandGap::ResourceProperty, -1.0f, "->stride, ->format, ->width, ->height, ->array, ->mips, ->size, ->offset, ->bind_flags of a resource the host cannot describe",
         "ResourcePropertyResult::UNKNOWN, the original's own answer for a property it cannot read"},
        {OperandGap::ResourceHash, -1.0f, "->region_hash, ->spatial_hash of a buffer the host cannot read",
         "ResourcePropertyResult::UNKNOWN"},
        {OperandGap::Evaluation, 0.0f, "a query past the layer's bound of 65536 queries in one execution, or outside a command list",
         "the layer's own bound, which the original has not"},
    };
    return rows;
}
inline float operand_value(OperandGap gap)
{
    for (const auto& row : operand_table()) if (row.gap == gap) return row.value;
    return 0.0f;
}

} // namespace efmi::frontend
