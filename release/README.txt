EFMI Vulkan
===========

Runs your installed EFMI Mods (XXMI Launcher) for Arknights: Endfield while the game renders with
Vulkan instead of DirectX 11.

Install
-------

1. Close the game and the XXMI Launcher.
2. Unpack the archive into the EFMI folder of your XXMI Launcher, the folder that holds Mods and
   Core. You then have EFMI\EFMI-Vulkan\efmi_vk_inject.dll next to EFMI\Mods. (Unpacked into
   EFMI\Mods it works too.)
3. Open the XXMI Launcher, choose EFMI, open its settings:
   - XXMI DLL Injection Mode: Skip.
   - Advanced, Custom Libraries: tick Inject Libraries and enter the full path of
     efmi_vk_inject.dll, for example D:\XXMI Launcher\EFMI\EFMI-Vulkan\efmi_vk_inject.dll.
4. Close the launcher. Open "XXMI Launcher Config.json" in the launcher's folder (next to the
   launcher program) with a text editor. Under "Importers", "EFMI", "Importer", change
     "d3d11_mode_cmd_args": "-force-d3d11"
   to
     "d3d11_mode_cmd_args": ""
   and save. The game then starts with Vulkan.
5. Start the game from the XXMI Launcher.

Steps 3 and 4 can be done by switch_renderer.bat instead, see the next section.

Switching between DirectX 11 and Vulkan
---------------------------------------

switch_renderer.bat in this folder switches the EFMI profile of the XXMI Launcher between the two
paths. Close the game and the launcher, double-click it, and start the game from the launcher
again. It shows the present state and what it changes:

- XXMI DLL Injection Mode (Skip for Vulkan, Direct for DirectX 11), the game's -force-d3d11
  argument (removed for Vulkan) and Inject Libraries (efmi_vk_inject.dll for Vulkan; other
  libraries listed there are set aside under Vulkan and come back with DirectX 11). These are the
  five values of the EFMI importer in "XXMI Launcher Config.json"; nothing else in the file is
  touched, and a copy goes to EFMI-Vulkan\backups first.
- The Inject Libraries value is signed with the launcher's own key pair (Resources\Security in the
  launcher's folder), so the launcher accepts the change without its Reset/Keep dialog.
- With EFF installed, its proxy vulkan-1.dll in the game folder is renamed to
  vulkan-1.dll.eff-parked for Vulkan (the layer cannot hook through that proxy, and no Mod would
  show) and renamed back for DirectX 11. Nothing else in the game folder is touched.

It needs only what Windows provides (Windows PowerShell). The launcher is found from the EFMI
folder's parent, the registry entry of its installer or Windows' usage records, its shortcuts, or
a running launcher; nothing is searched beyond that. If none of those fits, the window asks for
"XXMI Launcher Config.json" (drop the file in and press Enter), and the place is kept in
switch_renderer.cfg next to the script. Optional settings in that file: eff_park = 0 leaves the
game folder alone; vulkan_keep_other_libraries = 1 keeps other Inject Libraries under Vulkan;
dx11_inject_mode and dx11_cmd_args are the values written back for DirectX 11 (DIRECT,
-force-d3d11). Command-line use: switch_renderer.ps1 -Mode dx11, vulkan, toggle or status, and
-DryRun to show the change without making it.

Use
---

- Install, enable and disable Mods in EFMI\Mods as usual. Their keys work as with EFMI.
- F10 reloads the Mods folder after you add, remove or edit Mods.
- RabbitFX's anti-dither key (\) works while RabbitFX is installed.
- The Mods' saved settings are in EFMI-Vulkan\settings.

If something goes wrong
-----------------------

- The log is EFMI-Vulkan\efmi_vk_layer.log. Please attach it when you report a problem.
- To go back to DirectX 11: run switch_renderer.bat again, or by hand: untick Inject Libraries, set
  XXMI DLL Injection Mode back to Direct and set "d3d11_mode_cmd_args" back to "-force-d3d11"
  (launcher closed). If the game folder holds vulkan-1.dll.eff-parked, remove the suffix.

Licence
-------

GNU General Public License version 3 (LICENSE.txt). Source code:
https://github.com/loyal37/efmi-vulkan. Third-party components: THIRD_PARTY_NOTICES.md and the
licenses folder.
