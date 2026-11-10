#pragma once
#include "runtime.h"
#include <map>
#include <set>
#include <tuple>
#include "../../layer/src/character_package.h"

namespace efmi::frontend {
// The live renderer supplies identity, LOD0/1 bone import and immutable palette
// ownership. This extension implements the same EFMIv1 interface for original
// mod commands and checks its evaluated metadata against that renderer contract.
std::shared_ptr<RuntimeExtension> character_core(const std::string& mod_namespace, const efvk::CharacterPackage&,
                                               bool rabbit_textures = false);
struct CharacterDiscovery;
// As above; `facts` receives what the generation states besides the package (identify_components).
std::shared_ptr<RuntimeExtension> character_core(const std::string& mod_namespace, const efvk::CharacterPackage&,
                                               std::shared_ptr<CharacterDiscovery> facts);

// Directory generation (D-078): no offline package. The same extension takes its contract from the
// generation it is loaded into: the mod's mesh sections from the original override registry, with
// the component and LOD each assigns, and bones, components and LOD remaps from the original Core
// initializer. Filled while IniRuntime::load runs; complete once it succeeded.
struct CharacterDiscovery {
    efvk::CharacterPackage contract;                // bones, components (LOD1 remaps read), sections; no streams
    std::vector<std::filesystem::path> remap_files; // per component of the contract: its LOD1 remap file, empty for identity
    size_t left_to_game = 0;                        // mesh sections of the mod the contract has no part for
    // How many of an object's components the original Core must see at one place before it takes the
    // object as the Mod's: the constant the Mod's lists assign to $\EFMIv1\identification_min_components
    // (EFMI exports set 3 around SpatialIdentity_IdentifyComponentInstances), else the Core's default.
    uint32_t identify_components = 0;
    // Constant element offsets the Mod's lists set (`vbN->ElementOffset(SEMANTIC, i) = X`), by input
    // slot, upper-case semantic and index. A Mod that moves an element for its own stream and sets it
    // back names the game's offset with the second value (D-085).
    std::map<std::tuple<uint32_t, std::string, uint32_t>, std::set<uint32_t>> element_offsets;
    // The generation loaded EFMITTL with the interface its bridge knows (D-113): the host supplies
    // the library's shader hook facts and records its captures.
    bool efmittl = false;
    // The Mod's mesh and hide sections as the override registry declares them, with the Core's own
    // component ids; known once the overrides are parsed, before any declaration runs (the offline
    // dry run lists them for a Mod whose declarations need the GPU).
    std::vector<efvk::CharacterSection> declared;
    // What the original does for the Mod's sections and the layer does not (D-137, D-138): a line each
    // (UTF-8, the section named), for the host to show; the rest of the Mod loads.
    std::vector<std::string> gaps;
    // Why the generation could not prepare this Mod (its skeleton declarations, its contract), empty when
    // it did; a folder generation that refused names here every Mod to leave out (UTF-8).
    std::string refused;
};
// The version the installed Core declares (`global $version` of its main.ini), which its RegisterMod
// compares a Mod's required version with; 0 when not known. Before the generation loads.
void character_core_version(RuntimeExtension& core, float version);
std::shared_ptr<RuntimeExtension> character_discovery(const std::string& mod_namespace, std::shared_ptr<CharacterDiscovery> output);
// A folder generation (D-131, docs/folder-generation.md): every character Mod of the Mods folder in
// one generation, by its namespace as the parser names it (lower case), each discovered into its output as above, in the order the draws name them
// (RuntimeDraw::mod). A native Core call answers the Mod whose list runs; another Mod's list on a draw
// finds the instance not its own, as the original Core's identification tells it.
std::shared_ptr<RuntimeExtension> character_folder(const std::vector<std::pair<std::wstring, std::shared_ptr<CharacterDiscovery>>>& mods);
// Offline, without a GPU (the dry run): `inner` with the Mod's CustomShader sections declared, so a Mod
// that has them loads and its sections plan; the lists that run them refuse outside the owned domain.
std::shared_ptr<RuntimeExtension> declarations_only(std::shared_ptr<RuntimeExtension> inner);
// The namespaces of the libraries this extension declares itself (RabbitFX, D-082), lower case: a
// generation never loads their INIs from the Mods folder (D-112).
std::vector<std::string> character_core_libraries();
}
