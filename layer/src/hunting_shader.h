// HackerDevice::CreatePinkHuntingResources: float4(1, 0, 1, 1) at SV_Target0, with no resources.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace efvk {
inline std::vector<uint32_t> hunting_pink_shader(const std::string& entry)
{
    // SPIR-V 1.0, Shader capability, Logical / GLSL450. IDs 1..11, bound 12.
    std::vector<uint32_t> words{0x07230203, 0x00010000, 0, 12, 0};
    auto op = [&](uint16_t code, std::initializer_list<uint32_t> args) {
        words.push_back(uint32_t(args.size() + 1) << 16 | code);
        words.insert(words.end(), args);
    };
    op(17, {1}); op(14, {0, 1}); // OpCapability Shader; OpMemoryModel.
    const size_t strings = (entry.size() + 1 + 3) / 4;
    words.push_back(uint32_t(4 + strings) << 16 | 15); // OpEntryPoint Fragment %10 <name> %6.
    words.push_back(4); words.push_back(10);
    const size_t at = words.size(); words.resize(at + strings, 0);
    memcpy(words.data() + at, entry.c_str(), entry.size()); words.push_back(6);
    op(16, {10, 7}); op(71, {6, 30, 0}); // OriginUpperLeft; output Location 0.
    op(19, {1}); op(33, {2, 1}); op(22, {3, 32}); op(23, {4, 3, 4}); op(32, {5, 3, 4});
    op(43, {3, 7, 0}); op(43, {3, 8, 0x3f800000}); op(44, {4, 9, 8, 7, 8, 8});
    op(59, {5, 6, 3}); op(54, {1, 10, 0, 2}); op(248, {11}); op(62, {6, 9}); op(253, {}); op(56, {});
    return words;
}
}
