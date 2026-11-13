// Included only by the native test build of character_backend.cpp. These cases exercise the
// production hash, retirement and scratch cleanup paths; no source-extracted implementation.
namespace efvk {
void character_cache_cases(void (*check)(bool, const char*))
{
    Device device;
    std::vector<uint8_t> bytes(1024, 7);
    const auto memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(0xc040));
    const auto buffer = reinterpret_cast<VkBuffer>(uintptr_t(0xc040));
    auto& mem = device.mems[memory]; mem.size = bytes.size();
    mem.mapped = bytes.data(); mem.map_size = bytes.size();
    auto& buf = device.bufs[buffer]; buf.id = 1040; buf.size = bytes.size(); buf.mem = memory;
    DrawRec draw; draw.ib = buffer; draw.ib_off = 128; draw.first = 16; draw.count = 128; draw.index_size = 2;
    const size_t start = 160, size = 256;
    CharacterBackend::Index index;
    uint32_t hash = 0;
    check(index_region_hash(&device, draw, 0, index, hash) && hash == crc32c(0, bytes.data() + start, size),
          "character hash uses the bound range and first index");
    index.by_hash[hash] = {};
    bool stable = true;
    for (uint64_t frame = 1; frame <= 160; ++frame)
        stable &= index_region_hash(&device, draw, frame, index, hash) && hash == crc32c(0, bytes.data() + start, size);
    check(stable, "character hash remains correct after the former stability threshold");
    bytes[start] ^= 0x55; // A coherent host write has no API call and no tracked generation.
    check(index_region_hash(&device, draw, 161, index, hash) && hash == crc32c(0, bytes.data() + start, size),
          "stable positive match invalidates immediately on coherent mapped reuse");
    index.by_hash[hash] = {};
    bytes[start + size - 1] ^= 0x33;
    check(index_region_hash(&device, draw, 161, index, hash) && hash == crc32c(0, bytes.data() + start, size),
          "positive match validates a second reuse within the same rendered frame");
    bytes[start + 128] ^= 0x11; buf.note_write(start, size);
    check(index_region_hash(&device, draw, 162, index, hash) && hash == crc32c(0, bytes.data() + start, size),
          "tracked interior writes invalidate the region cache");
    const auto reads = g_quick_reads.load();
    check(index_region_hash(&device, draw, 163, index, hash) && g_quick_reads.load() == reads,
          "negative matches retain bounded fingerprint backoff");

    CharacterGeneration generation(&device, {}, {});
    StaticMaterial material;
    std::vector<std::weak_ptr<const PipelineVariant>> retired;
    auto command_owner = std::make_shared<PipelineVariant>(VK_NULL_HANDLE, VariantDispatch{});
    auto other = std::make_shared<PipelineVariant>(VK_NULL_HANDLE, VariantDispatch{});
    generation.variant_lookup.emplace(std::make_tuple(999u, 0u, &material, false, 0u), other);
    for (uint32_t id = 1; id <= 256; ++id) {
        auto variant = id == 1 ? command_owner : std::make_shared<PipelineVariant>(VK_NULL_HANDLE, VariantDispatch{});
        retired.push_back(variant);
        generation.variant_lookup.emplace(std::make_tuple(id, 0u, &material, false, 0u), variant);
        generation.variant_lookup.emplace(std::make_tuple(id, 1u, &material, true, 0u), variant);
        variant.reset(); generation_retire(generation, id);
    }
    check(generation.variant_lookup.size() == 1 && !retired[0].expired(),
          "pipeline retirement removes all its draw keys and preserves command ownership");
    bool released = true;
    for (size_t i = 1; i < retired.size(); ++i) released &= retired[i].expired();
    check(released, "retired pipeline variants without commands release during long sessions");
    command_owner.reset();
    check(retired[0].expired(), "retired variant releases after its final command completes");
    generation_retire(generation, 999);
    check(generation.variant_lookup.empty() && other.use_count() == 1,
          "retiring another pipeline preserves its independent external owner");

    std::weak_ptr<const void> image;
    std::shared_ptr<const void> completed_later;
    auto record = [&](bool fail) {
        CharacterGeneration::ScratchScope scope{generation};
        generation.scratch_material_views.resize(1);
        auto value = std::make_shared<int>(42); image = value;
        generation.scratch_material_views[0].push_back({VK_NULL_HANDLE, value});
        completed_later = value;
        generation.scratch_variants.push_back(other);
        if (fail) throw std::runtime_error("partial draw");
    };
    record(false);
    check(!image.expired() && generation.scratch_material_views[0].empty() && generation.scratch_variants.empty(),
          "draw exit releases scratch owners but preserves in-flight image ownership");
    const auto capacity = generation.scratch_material_views[0].capacity();
    completed_later.reset();
    check(image.expired(), "invisible character cannot pin evicted images after command completion");
    try { record(true); } catch (const std::runtime_error&) {}
    completed_later.reset();
    check(image.expired() && generation.scratch_variants.empty() && other.use_count() == 1,
          "partial draw exception releases both image and pipeline scratch owners");
    check(generation.scratch_material_views[0].capacity() == capacity && capacity > 0,
          "scratch cleanup preserves reusable inner storage capacity");
}
} // namespace efvk
