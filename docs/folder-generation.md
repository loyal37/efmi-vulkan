# One generation for the Mods folder (D-131, docs/3dmigoto-flows.md section 3)

## What the original does

3DMigoto parses d3dx.ini, the Core (Core\EFMI\main.ini and what it includes) and every enabled INI of
the Mods folder (`include_recursive = Mods`, `exclude_recursive = DISABLED*`) into one global state:
one variable table (`$\namespace\name`), one set of resources, lists and overrides, one `[Present]`
list with every file's `[Present]` appended, one IniParams. A draw runs every TextureOverride whose
hash it matches, in the order the original's matcher gives, whichever Mod declared it. F10 parses
all of it again and swaps it in at once.

The Core keeps no per-Mod identity: `CommandListRegisterMod` only sets `$mod_id` to 1, or -1 when the
versions are incompatible. A Mod calls the Core's lists after pointing the Core's resources at its
own (`ResourceMergedSkeleton = ref ...`) and setting the Core's variables (component id, LOD); the
Core's own variables are scratch that every call sets again.

A section the original cannot use (a typo, an unknown key) is a warning on its overlay; the rest of
that file and every other Mod load.

## What the layer does today

A generation per character Mod (an enabled INI with `[CommandListInitializeMergedSkeleton]`, D-078):
the four Core files, the Mod's INI, the other enabled INIs of its folder, and the libraries it calls
(D-112). With 22 Mods that is 22 parses of the Core, 22 variable spaces, 22 owned GPU runtimes and 22
Present lists per frame (226 us in the vk03 replay, the Core's share in each), and an F10 that loads
22 generations one after another. Mods that are not character Mods are not loaded at all. A
candidate the layer cannot execute is refused as a whole; that is one Mod.

## The design

One INI generation for the folder, as the original; per character Mod the device-side package as
today, carved out of it.

- **The folder program.** One `IniRuntime` with the Core's files, every enabled INI of the Mods folder
  (the original's include rules) and nothing else; one owned GPU runtime; one Present list per
  frame; one settings save. Its loads follow D-129 (a background load, an exchange at a Present).
- **The Core bridge serves every Mod.** Discovery yields one contract per character Mod namespace
  (the sections, components and LOD remaps that `discover` finds today, from the same original
  registry). Per draw the layer's `RuntimeDraw` names the package the device index resolved, as it
  already carries its component, LOD and identity; the native Core handlers take that Mod's
  contract and prepared skeleton. `RegisterMod` sets `$mod_id` as the installed Core does.
- **Packages stay per Mod.** Meshes, the image catalogue, bone ranges, materials, identities and
  variants are the Mod's, made from its contract as now; a draw is planned by the folder program
  (every matching section of every Mod, in the original's order) and recorded by the package whose
  section the device index resolved.
- **A bad Mod drops only itself.** What the layer refuses today for a whole candidate is decided per
  Mod: the Mod's sections are left to the game and its package is not made; the other Mods load. A
  refused Mod is named with its reason, as the original names its warnings.
- **F10 and a changed folder.** One load of the folder; when it is ready every package switches at
  the same Present (no frame with some Mods old and some new, none with the game's mesh between);
  meshes and images whose files did not change are shared (BUG-061's file identity, extended to
  meshes).

## Order of work, each step test first

1. The Core bridge for several Mods in one runtime (frontend): the installed Mods folder loaded as
   one generation gives, per character Mod, the same contract as its own generation today; a
   captured frame plans the same draws through it. Fails today: there is no folder discovery.
2. The layer on one folder program: packages carved from it; one Present list per frame; the
   replays' pictures identical to the per-Mod generations'; the Present cost per frame measured.
3. Isolation: a folder with one Mod the layer refuses loads every other Mod (test: a Mod with an
   unadapted section beside two good ones).
4. F10 and folder changes: every package switches at one Present; unchanged mesh files are not
   read again.
5. Memory and the payloads (ISSUE-011): mesh payloads freed once on the device, read again from the
   file when a package needs them (re-upload, the first plan's bone stream).

The folder generation's inputs (D-154, D-155): the installation's d3dx.ini's own sections first (not
its [Include] or [Loader]), the four Core files, then every enabled INI of the Mods folder in the
original's include order (each folder's `.ini` files sorted without regard to case, then its folders;
DISABLED* and desktop.ini left out), except the native libraries' files (their key and ShaderRegex
sections join through the native interface) and the folders of character Mods the isolation left out.
A Mod that is no character Mod runs its Present and keys, keeps its settings and acts through its
texture-hash sections in replaced draws; it is no package. Its mesh sections (a constant
`match_index_count`) are matched at draws when its INI lies in a character Mod's folder: they are
sections without a component of that Mod's package (D-173, 2026-11-11). Those of an INI outside every
character Mod's folder act on no draw yet.

## Status (2026-10-27, 2.0.0)

Steps 1 to 4 are in 2.0.0: the bridge (scripts/check_folder_generation.py), the layer on one folder
generation (`folder_create`, `folder_frame`, `folder_frame_and_exchange`), isolation (a Mod that refuses
alone is left out, the live INI scene's third Mod) and the exchange of every package at one
presentation. Measured on the vk03 replay with the 20 installed Mods (same build, `m2_folder_generation`
off and on): presentation work 221 to 146 us per frame, recording 410 to 420 us, final targets
identical.

Step 5 is in 2.0.1 (ISSUE-011): a package keeps no mesh bytes once validated, as the original keeps
no host copy of a buffer it made from a file (`LoadBufferFromFile` frees its bytes once `CreateBuffer`
took them). Each stream keeps its file, offset and the checksum of the bytes validated at load; the
upload when the package activates reads the files again (a few threads at once, each into its own
streams' part of the mapped memory, through a bounce buffer the checksum reads), and so does a stream
a live plan first adopts for bones. A file whose bytes changed since the load refuses with its name.
Actual configuration with the installed Mods folder: private memory 712 to 298 MiB. Activation upload
of a 23.7 MB character 4.5 ms (5 ms with the held payload; 16 ms when one thread read the files).
