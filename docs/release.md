# The playable release (D-135)

The user, 2026-10-27: release under GPLv3; the release text tells players how to install and use it;
nothing for development goes into it, players hold a playable version.

## What it is

`EFMI-Vulkan-X.Y.Z.zip`, one folder `EFMI-Vulkan` the player unpacks into the EFMI folder of the XXMI
Launcher (next to `EFMI\Mods`):

- `efmi_vk_inject.dll`: the layer with the inject entry, built with `EFVK_NO_PROBE` and without
  `layer/src/ipc.cpp`: no probe pipe (its access list let any interactive user's process drive the
  elevated game, module exchange included), no sampler, nothing outside the game reads or changes it.
- `dxcompiler.dll`, `SPIRV-Tools-shared.dll`, `efmi-vkd3d-shader.dll`: loaded from this folder by full path
  when a Mod's HLSL or DX11 bytecode is compiled. `dxil.dll` is not shipped (SPIR-V only, and its
  Microsoft licence does not go with a GPL package).
- `efmi_vk.cfg`: `m2_mods = auto` (the first folder upward from the DLL's that holds `Mods` and
  `Core\EFMI`, so EFMI\EFMI-Vulkan and EFMI\Mods\EFMI-Vulkan both work; 2.0.9's `../Mods` found nothing
  in the latter, BUG-073; a named folder may be relative to the DLL's folder), replacement on, the
  settings the game was played with. The layer's own folder is never part of the Mods folder's content.
- `switch_renderer.bat` and `switch_renderer.ps1`: the switch between the DirectX 11 and the Vulkan
  configuration of the launcher's EFMI profile (the README's steps 3 and 4 and their reversal),
  done by editing the five values of the EFMI importer in the launcher's config and signing the
  injected-libraries value with the launcher's own key pair; with EFF installed, its proxy
  `vulkan-1.dll` is parked for Vulkan and restored for DirectX 11. Windows PowerShell only, no
  disk scan, a backup of the config before every write.
- `README.txt` (install and use), `LICENSE.txt` (GPLv3), `THIRD_PARTY_NOTICES.md`, `licenses\`.

Not in it: tests, the sandbox, the replacement module files, the layer manifest, symbols (kept in
`layer/build/release` for crash logs), logs, settings, caches.

Why not one DLL: DXC exists only as Microsoft's DLL (a static build of it is unsupported and large);
vkd3d-shader is built with MinGW GCC, the layer with MSVC; loading DLLs from memory is how cheats
hide code and is never done here. SPIRV-Tools could be linked statically if one file less matters.

## Steps

1. `python scripts/build.py layer sandbox release`: the development build (for the checks) and the
   release in `build/release/EFMI-Vulkan`, `build/release/EFMI-Vulkan-X.Y.Z.zip` and its `.sha256`.
2. The touched regression areas, then `python scripts/check_release.py build/release/EFMI-Vulkan-X.Y.Z.zip`
   (contents, no probe in the binary, the release injected into the sandbox with the installed Mods, and
   unpacked into an EFMI folder of its own and into its Mods folder).
3. Commit, fast-forward main, push; tag `vX.Y.Z` on that commit.
4. `gh release create vX.Y.Z build/release/EFMI-Vulkan-X.Y.Z.zip --title "EFMI Vulkan X.Y.Z" --notes-file ...`:
   the notes say how to install and use it, and the archive's SHA-256.
