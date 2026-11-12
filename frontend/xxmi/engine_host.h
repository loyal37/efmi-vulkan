#pragma once
// What the runtime's host shims keep per generation (D-129, docs/engine-generations.md): the
// resource root its files resolve against and the original warnings its load and execution reported.
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
struct EngineHostState {
    std::wstring resource_root;
    size_t diagnostic_count = 0;
    std::deque<std::pair<size_t, std::string>> diagnostics;
    // What the layer did not do as the original does (LayerGap, D-137): the texts in order, the latest
    // 64 kept, and how many there were; the host shows each once.
    size_t gap_count = 0;
    std::deque<std::pair<size_t, std::string>> gaps;
    // The hunting keys the input dispatcher met (HuntingAction values), until the generation hands them on.
    std::deque<uint32_t> hunting;
};
// The generation the calling thread entered, or null outside every generation.
EngineHostState* EngineHost();
