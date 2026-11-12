// Native CPU tests and a bounded binary replay entry for uncommitted capture data.
#include "../layer/src/merged_skeleton.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <xmmintrin.h>

using namespace efvk;
static int passed = 0, failed = 0;
static void check(bool ok, const char* name)
{
    if (ok) ++passed; else { ++failed; printf("FAIL: %s\n", name); }
}
template<class T> static T read(const uint8_t* src) { T v; std::memcpy(&v, src, sizeof(v)); return v; }
template<class T> static void write(uint8_t* dst, T v) { std::memcpy(dst, &v, sizeof(v)); }
static InstanceEntry entry(uint32_t current = 2000, uint32_t previous = 4000)
{
    InstanceEntry out;
    for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(i ^ 0x93);
    write(out.data() + 80, current); write(out.data() + 84, previous);
    return out;
}
static std::vector<uint8_t> source(uint32_t count, float start)
{
    // Matrix 0 is the implicit transform, intentionally distinct from imported matrices.
    std::vector<uint8_t> out((count + 1) * 48);
    for (uint32_t i = 0; i <= count; ++i)
        for (uint32_t j = 0; j < 12; ++j) write(out.data() + i * 48 + j * 4, start + float(i * 16 + j));
    return out;
}
static BoneWindow view(const std::vector<uint8_t>& bytes) { return {bytes.data(), bytes.size()}; }
static float bone(const MergedSkeleton& s, uint32_t instance, uint32_t pose, uint32_t index, uint32_t f = 0)
{
    return read<float>(s.bytes().data() + s.rows_for_instance(instance) * 16 + (pose * s.bones() + index) * 48 + f * 4);
}
__declspec(noinline) static float scalar_scaled(float value, float scale)
{
    volatile float input = value, factor = scale;
    return input * factor; // Original scalar contract, with runtime rounding and FTZ/DAZ.
}
static void packed_import_tests()
{
    const unsigned saved_control = _mm_getcsr();
    struct Restore { unsigned control; ~Restore() { _mm_setcsr(control); } } restore{saved_control};
    MergedSkeleton s; std::string reason; s.configure(5, 1, {{0,5}}, reason);
    const std::vector<uint16_t> remap{3,0,3,1,2};
    const uint32_t patterns[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
        0x3f800000, 0xbf800000, 0x3eaaaaab, 0x00800001, 0x7effffff, 0xfeffffff};
    std::vector<uint8_t> bytes(3 + 5 * 48);
    for (size_t i = 0; i < 5 * 12; ++i) write(bytes.data() + 3 + i * 4, patterns[i % 12]);
    write(bytes.data() + 3, std::numeric_limits<float>::quiet_NaN()); // Unused implicit row.
    const BoneWindow window{bytes.data() + 3, bytes.size() - 3};
    uint64_t serial = 1;
    for (unsigned rounding : {_MM_ROUND_NEAREST, _MM_ROUND_DOWN, _MM_ROUND_UP, _MM_ROUND_TOWARD_ZERO})
        for (unsigned flush : {0u, 0x8040u}) for (float scale : {1.f, -0.f, .25f, -4.f}) {
            _mm_setcsr((saved_control & ~0xe040u) | rounding | flush);
            s.begin_update(serial++, reason);
            auto expected = s.bytes(); bool finite = true;
            for (uint32_t pose = 0; pose < 2; ++pose) for (uint32_t i = 0; i < 5; ++i)
                for (uint32_t lane = 0; lane < 12; ++lane) {
                    const float value = scalar_scaled(read<float>(window.data + (remap[i] + 1) * 48 + lane * 4), scale);
                    finite &= std::isfinite(value);
                    write(expected.data() + 1536 * 16 + (pose * 5 + i) * 48 + lane * 4, value);
                }
            if (!finite) expected = s.bytes();
            const auto result = s.import_component(0,0,0,entry(),window,window,remap,scale,reason);
            check(result == (finite ? BoneImportResult::Imported : BoneImportResult::Rejected) &&
                  s.bytes() == expected && s.updated(0,0) == finite,
                  "packed import agrees byte for byte with scalar rounding, subnormals, signed zero and overflow");
        }
    _mm_setcsr(saved_control);
    for (uint32_t lane = 0; lane < 12; ++lane) for (bool infinity : {false,true}) {
        auto current = source(5, 10), previous = source(5, 100);
        write(previous.data() + 5 * 48 + lane * 4,
              infinity ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN());
        s.begin_update(serial++, reason); const auto before = s.bytes();
        check(s.import_component(0,0,0,entry(),view(current),view(previous),{},1,reason) == BoneImportResult::Rejected &&
              s.bytes() == before && !s.updated(0,0), "each nonfinite lane in the late pose leaves both poses and stamps unchanged");
        previous = source(5, 100);
        check(s.import_component(0,0,0,entry(),view(current),view(previous),{},1,reason) == BoneImportResult::Imported &&
              bone(s,0,1,4,11) == 191.f, "reused staging recovers completely after a failed late pose");
    }
}
static void native_tests()
{
    MergedSkeleton s; std::string why;
    auto in = entry(), patched = entry(8,9), untouched = patched;
    auto cur = source(3, 10), prev = source(3, 100);
    check(!s.begin_update(0, why) && !why.empty(), "unconfigured update rejected");
    {
        // BUG-048: components with bones between them, as the Core allows. BUG-093: so are components that
        // share bones (each import writes its own range, the later one stays); disorder refuses.
        MergedSkeleton g; std::string reason;
        check(g.configure(10, 1, {{2,3},{7,2}}, reason) && g.bytes().size() == (512 + 2 * 10) * 48,
              "disjoint components with bones between and after them configure");
        auto gap_source = source(3, 10), gap_previous = source(3, 100);
        check(g.begin_update(1, reason) && g.import_component(0,1,0,entry(),view(gap_source),view(gap_previous),{},1,reason) == BoneImportResult::Imported &&
              bone(g,0,0,7) != 0 && bone(g,0,0,0) == 0 && bone(g,0,0,6) == 0,
              "a component imports at its own range; the bones between stay zero, as the Core leaves them");
        check(!g.configure(10, 1, {{7,2},{2,3}}, reason) && !g.configure(10, 1, {{8,3}}, reason),
              "disordered or out-of-range components refuse");
        MergedSkeleton shared;
        auto first_source = source(3, 10), first_previous = source(3, 100), second_source = source(2, 50), second_previous = source(2, 500);
        check(shared.configure(10, 1, {{2,3},{4,2}}, reason) && shared.begin_update(1, reason) &&
              shared.import_component(0,0,0,entry(),view(first_source),view(first_previous),{},1,reason) == BoneImportResult::Imported &&
              bone(shared,0,0,2) != 0 && bone(shared,0,0,4) != 0 && bone(shared,0,0,5) == 0,
              "components that share bones configure, and the first one's import fills its range");
        const float before = bone(shared,0,0,4), kept = bone(shared,0,0,2);
        check(shared.import_component(0,1,0,entry(),view(second_source),view(second_previous),{},1,reason) == BoneImportResult::Imported &&
              bone(shared,0,0,4) != before && bone(shared,0,0,5) != 0 && bone(shared,0,0,2) == kept,
              "the second one's import writes the bones they share and its own, and leaves the first one's others");
    }
    check(!s.patch_entry(0, in, patched, why) && patched == untouched, "unconfigured patch is atomic");
    check(s.configure(5, 2, {{0,3},{3,2}}, why), "configure two instances and components");
    check(s.bytes().size() == (512 + 2 * 5 * 2) * 48, "exact two-pose layout size");
    check(s.rows_for_instance(0) == 1536 && s.rows_for_instance(1) == 1566 && s.rows_for_instance(2) == 0,
          "instance slices do not overlap");
    bool identity = true;
    for (uint32_t b = 0; b < 512; ++b)
        for (uint32_t f = 0; f < 12; ++f)
            identity &= read<float>(s.bytes().data() + b * 48 + f * 4) == ((f == 0 || f == 5 || f == 10) ? 1.f : 0.f);
    check(identity, "all 512 implicit bones initialized to identity");
    check(!s.updated(0,0) && bone(s,0,0,0) == 0, "dynamic master begins deterministic and unseen");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{},1,why) == BoneImportResult::Rejected,
          "import requires logical update");
    check(s.begin_update(0, why), "serial zero is a valid first update");
    check(s.import_component(0,0,1,in,view(cur),view(prev),{2,0,2},2,why) == BoneImportResult::Imported,
          "LOD remap with duplicate source and scale imports");
    check(bone(s,0,0,0) == 116 && bone(s,0,0,1) == 52 && bone(s,0,0,2) == 116,
          "remap direction is destination to source and skips implicit matrix");
    check(bone(s,0,1,0) == 296 && bone(s,0,1,1,11) == 254, "previous pose and all row terms scaled separately");
    check(bone(s,1,0,0) == 0 && bone(s,0,0,3) == 0, "other instance and component unchanged");
    check(s.updated(0,0) && !s.updated(1,0) && !s.updated(0,1), "update stamps are per component and instance");
    check(s.patch_entry(0, in, patched, why), "entry patch succeeds");
    bool unchanged = true;
    for (size_t i = 0; i < in.size(); ++i) if (i < 80 || i >= 88) unchanged &= in[i] == patched[i];
    // The patched offsets name the matrix before merged bone zero: the vertex shader adds one matrix.
    check(unchanged && read<uint32_t>(patched.data()+80) == 1533 && read<uint32_t>(patched.data()+84) == 1548,
          "only dynamic bone offsets changed; flags and all other entry bytes retained");
    check(s.patch_entry(1, in, in, why) && read<uint32_t>(in.data()+80) == 1563 && read<uint32_t>(in.data()+84) == 1578,
          "in-place patch supports nonzero instance");
    {
        // Measured under DX11 with the core active: every entry of the 499-bone sample that the core
        // patched holds (1533, 3030) in constant 5 (docs/dx11-baseline.md).
        MergedSkeleton sample; InstanceEntry measured;
        check(sample.configure(499, 8, {{0,499}}, why) && sample.patch_entry(0, entry(), measured, why) &&
              read<uint32_t>(measured.data()+80) == 1533 && read<uint32_t>(measured.data()+84) == 3030,
              "patched offsets equal the values the core wrote under DX11");
    }
    in = entry();
    auto saved = s.bytes();
    check(s.import_component(0,0,1,in,{}, {},{},1,why) == BoneImportResult::AlreadyCurrent && s.bytes() == saved,
          "same LOD repeats need no source read");
    check(s.import_component(0,0,2,in,view(prev),view(cur),{},1,why) == BoneImportResult::AlreadyCurrent && s.bytes() == saved,
          "coarser LOD cannot replace a better import");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{},1,why) == BoneImportResult::Imported && bone(s,0,0,0) == 26,
          "finer LOD replaces previous import in same update");
    check(s.begin_update(1, why) && !s.updated(0,0) && bone(s,0,0,0) == 26, "new update retains master without current stamps");
    check(!s.begin_update(0, why), "backwards update rejected");
    saved = s.bytes();
    auto short_previous = view(prev); --short_previous.size;
    check(s.import_component(0,0,0,in,view(cur),short_previous,{},1,why) == BoneImportResult::Rejected &&
          s.bytes() == saved && !s.updated(0,0), "short previous pose cannot partly change current pose or stamp");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{0,1},1,why) == BoneImportResult::Rejected,
          "wrong remap length rejected");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{0,1,65535},1,why) == BoneImportResult::Rejected,
          "out-of-range remap cannot read beyond window");
    check(s.import_component(2,0,0,in,view(cur),view(prev),{},1,why) == BoneImportResult::Rejected &&
          s.import_component(0,2,0,in,view(cur),view(prev),{},1,why) == BoneImportResult::Rejected &&
          s.import_component(0,0,4,in,view(cur),view(prev),{},1,why) == BoneImportResult::Rejected,
          "invalid instance component and LOD rejected");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{},std::numeric_limits<float>::infinity(),why) == BoneImportResult::Rejected,
          "nonfinite scale rejected");
    auto bad = prev;
    write(bad.data() + 3 * 48 + 44, std::numeric_limits<float>::quiet_NaN());
    check(s.import_component(0,0,0,in,view(cur),view(bad),{},1,why) == BoneImportResult::Rejected &&
          s.bytes() == saved && !s.updated(0,0), "late nonfinite bone rejects both poses atomically");
    check(s.import_component(0,0,0,in,view(cur),view(prev),{},std::numeric_limits<float>::max(),why) == BoneImportResult::Rejected,
          "finite scale overflow rejected");
    // Bounds are based on used remapped bones, not the nominal component count or window tail.
    auto one = source(1, 30);
    check(s.import_component(0,0,1,in,view(one),view(one),{0,0,0},1,why) == BoneImportResult::Imported,
          "exact small mapped palette accepted after earlier failure");
    check(s.import_component(1,0,0,in,view(cur),view(prev),{},1,why) == BoneImportResult::Imported &&
          bone(s,1,0,0) == 26 && bone(s,0,0,0) == 46, "second instance imports independently");
    auto frozen = s.bytes();
    check(s.begin_update(2, why), "next update begins");
    auto stat = entry(0, 768);
    check(s.import_component(0,0,0,stat,{}, {},{},1,why) == BoneImportResult::Imported && s.bytes() == frozen,
          "static offsets skip both imports and retain master");
    check(s.patch_entry(0,stat,patched,why) && patched == stat, "static offsets are preserved byte for byte");
    auto mixed = entry(1535,1536);
    check(s.import_component(0,1,0,mixed,{},view(prev),{},-1,why) == BoneImportResult::Imported &&
          bone(s,0,0,3) == 0 && bone(s,0,1,3) == -116, "1536 threshold and independent pose import");
    check(s.patch_entry(1,mixed,patched,why) && read<uint32_t>(patched.data()+80) == 1535 &&
          read<uint32_t>(patched.data()+84) == 1578, "mixed static/dynamic offset patch");
    check(frozen != s.bytes() && read<float>(frozen.data() + 1536 * 16 + (5+3)*48) == 0,
          "copied snapshot is independent of later master writes");
    // Source pointers can be unaligned. Only consumed matrices must be finite.
    std::vector<uint8_t> unaligned(cur.size()+1); std::memcpy(unaligned.data()+1,cur.data(),cur.size());
    write(unaligned.data()+1, std::numeric_limits<float>::quiet_NaN());
    check(s.import_component(1,1,0,in,{unaligned.data()+1,cur.size()},view(prev),{},1,why) == BoneImportResult::Imported,
          "unaligned input and unused implicit NaN never cause an extra read");
    saved = s.bytes();
    check(!s.configure(5,2,{{2,2},{0,3}},why) && s.bytes() == saved, "disordered layout rejected without losing master");
    { MergedSkeleton tail; check(tail.configure(5,2,{{0,3}},why), "bones after the last component stay as initialized (BUG-048)"); }
    check(!s.configure(5,0,{{0,5}},why) && !s.configure(65537,1,{{0,65537}},why), "invalid dimensions rejected");
    check(!s.configure(65536,0xffffffff,{{0,65536}},why) && s.bytes() == saved, "large arithmetic bounded before allocation");
    check(s.configure(1,1,{{0,1}},why) && !s.updated(0,0) && bone(s,0,0,0) == 0,
          "successful reconfiguration resets data and stamps");
}

// Binary replay is test tooling only. Production never reads this format or any capture file.
class Reader {
public:
    std::vector<uint8_t> data;
    size_t at = 0;
    explicit Reader(const char* path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        auto size = in.tellg();
        if (!in || size < 0 || size > (64 << 20)) throw std::runtime_error("invalid replay file size");
        data.resize(size_t(size)); in.seekg(0); in.read(reinterpret_cast<char*>(data.data()), size);
        if (!in) throw std::runtime_error("replay read failed");
    }
    const uint8_t* take(size_t n) {
        if (n > data.size() - at) throw std::runtime_error("truncated replay");
        const auto* result = data.data() + at; at += n; return result;
    }
    template<class T> T scalar() { return read<T>(take(sizeof(T))); }
};
static int replay(const char* input, const char* output)
{
    Reader r(input);
    if (std::memcmp(r.take(8), "EFMIBN01", 8)) throw std::runtime_error("replay magic");
    uint32_t bones = r.scalar<uint32_t>(), instances = r.scalar<uint32_t>();
    uint32_t components = r.scalar<uint32_t>(), records = r.scalar<uint32_t>();
    if (components > 65536 || records > 4096) throw std::runtime_error("replay budget");
    std::vector<BoneComponent> ranges;
    for (uint32_t i=0; i<components; ++i) ranges.push_back({r.scalar<uint32_t>(),r.scalar<uint32_t>()});
    MergedSkeleton s; std::string reason;
    if (!s.configure(bones,instances,ranges,reason)) throw std::runtime_error(reason);
    if (s.bytes().size() * uint64_t(records) > (256ull<<20)) throw std::runtime_error("replay output budget");
    std::ofstream out(output, std::ios::binary); out.write("EFMIBO01",8);
    out.write(reinterpret_cast<const char*>(&records),4);
    for (uint32_t i=0; i<records; ++i) {
        uint64_t serial = r.scalar<uint64_t>();
        uint32_t instance = r.scalar<uint32_t>(), component = r.scalar<uint32_t>(), lod = r.scalar<uint32_t>();
        float scale = r.scalar<float>();
        InstanceEntry entry, patched; std::memcpy(entry.data(),r.take(256),256);
        uint32_t count = r.scalar<uint32_t>();
        if (count > 65536) throw std::runtime_error("remap budget");
        std::vector<uint16_t> remap;
        for (uint32_t j=0; j<count; ++j) remap.push_back(r.scalar<uint16_t>());
        uint32_t n = r.scalar<uint32_t>(); BoneWindow current{r.take(n),n};
        n = r.scalar<uint32_t>(); BoneWindow previous{r.take(n),n};
        if (!s.begin_update(serial,reason)) throw std::runtime_error(reason);
        auto result = s.import_component(instance,component,lod,entry,current,previous,remap,scale,reason);
        if (result == BoneImportResult::Rejected) throw std::runtime_error(reason);
        if (!s.patch_entry(instance,entry,patched,reason)) throw std::runtime_error(reason);
        uint32_t result_word = static_cast<uint32_t>(result), size = uint32_t(s.bytes().size());
        out.write(reinterpret_cast<const char*>(&result_word),4);
        out.write(reinterpret_cast<const char*>(patched.data()),256);
        out.write(reinterpret_cast<const char*>(&size),4);
        out.write(reinterpret_cast<const char*>(s.bytes().data()),size);
    }
    if (r.at != r.data.size() || !out) throw std::runtime_error("extra input or output write failure");
    printf("bone replay: %u records written\n",records);
    return 0;
}
int main(int argc, char** argv)
{
    try {
        if (argc == 4 && !strcmp(argv[1],"--replay")) return replay(argv[2],argv[3]);
        if (argc != 1) throw std::runtime_error("usage: bone_test [--replay input output]");
        native_tests();
        packed_import_tests();
        printf("bone native: %d checks passed, %d failed\n",passed,failed);
        return failed ? 1 : 0;
    } catch (const std::exception& e) { printf("FAILED: %s\n", e.what()); return 1; }
}
