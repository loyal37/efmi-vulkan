# EFMI Vulkan - Project overview

> Created 2026-10-11. The structure follows `F:\EFMI MCP\.ai-memory`; the content covers this project only.

## Goal

Run existing EFMI merged-skeleton mods of Arknights: Endfield on the game's native Vulkan renderer, with the mod directories (INI, BUF, DDS) unchanged. Today EFMI works only because the launcher forces the game onto Direct3D 11.

The goal since 2026-10-19 is general: every installed Mod, and characters never captured, through shared mechanisms; the acceptance bar is the original's experience (the first frame is the Mod, no stand-in, no frame-rate loss). What is in and out of scope today, with the reasons, is `docs/architecture-review-2.1.md`; the full plan with facts, decisions, gates and milestones is `docs/plan.md`.

## Components and technology

- `layer/`: the injected DLL in C++17, MSVC x64: the Vulkan hooks and tracker, the probe pipe, and the replacement (packages, the folder generation, skeleton import, pipeline variants, textures, the owned Present, notices). The replacement is also built alone as the exchangeable module `efmi_vk_backend.dll`.
- `frontend/`: the original XXMI parser and interpreter, compiled unchanged from `F:\vulkan\XXMI-Libs-Package\DirectX11`, with the adapters around it (`frontend/xxmi`), the owned GPU runtime (`frontend/gpu`) and file loading (`frontend/assets`).
- `sandbox/`: deterministic Vulkan test programs and the regression suite (`sandbox/run_all.py`, by groups). Everything that can be tested without the game is tested here.
- `server/`: the Python package `efmi_vk` (standard library only): pipe client, command line and a stdio MCP server.
- `scripts/`: builds, checks (among them the original as the judge, `check_dx11_judge.py`), captures and replays.
- The code's structure as it is now: `.ai-memory/ARCHITECTURE.md`.

## Entry points on this machine

- Project: `F:\efmi vulkan`. Vulkan headers: `F:\vulkan\Vulkan-Headers` (tag `vulkan-sdk-1.4.321.0`, matching the installed runtime `vulkan-1.dll` 1.4.321). Khronos VVL 1.4.321 and SPIRV-Tools are built under F:/vulkan; the sandbox passes core/synchronization validation. No full SDK or glslc is installed.
- MSVC: `D:\Microsoft` (found by `vswhere`). Python: `D:\Python312\python.exe`. Git: `D:\git`.
- EFMI workspace (read-only): `D:\XXMI-Launcher-Portable-v2.1.6\EFMI`. Game (read-only): `D:\Hypergryph Launcher\games\EndField Game`. Unity player log and PSO caches: `%USERPROFILE%\AppData\LocalLow\Hypergryph\Endfield\`.
- Sibling project for DX11 capture (read-only reference): `F:\EFMI MCP`.
- Reference sources: `SpectrumQT/XXMI-Libs-Package` (branch `master`, engine in `DirectX11/`), `lekarsenten/wwmi-dx12` (branch `main`, GPL-3.0).

## Long-term constraints

- Change only this project. The EFMI/XXMI installation, the launcher configuration, existing mods and the game files are read-only.
- The layer goes into the real game only when the user says so on the spot; gate G1 carries an account risk. Nothing is ever done to evade or hide from the anti-cheat.
- No system-wide change without being asked: the tests enable the layer through environment variables of their own child processes only.
- Everything in the workspace is English without Chinese characters; commit after every verified change; no push until a remote is named. See `AGENTS.md`.
