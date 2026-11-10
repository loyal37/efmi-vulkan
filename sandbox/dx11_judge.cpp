// The original as the judge (D-137, D-150): a D3D11 application of the test's own with the pinned XXMI
// d3d11.dll (3DMigoto) loaded from a project folder, presenting frames so the original runs its [Present]
// lists, until its persistent variables are saved (d3dx_user.ini). The folder's d3dx.ini names this program
// as the target; the user's installation is never touched.
// Usage: efmi_vk_dx11_judge.exe <folder with d3d11.dll and d3dx.ini> <frames> [scene]
// With `scene` the program also draws (dx11_judge_scene.h): two indexed quads a frame, whose index region
// hashes the case's [TextureOverride] sections match, and after the last frame the picture's points and
// the draws' primitives on stdout (docs/architecture-review-2.1.md, P3-1).
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
#include "dx11_judge_scene.h"

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w, m, a, b); }

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3 && !(argc == 4 && !wcscmp(argv[3], L"scene"))) { fprintf(stderr, "usage: efmi_vk_dx11_judge <xxmi folder> <frames> [scene]\n"); return 2; }
    const bool scene_mode = argc == 4;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring folder = argv[1];
    const int frames = _wtoi(argv[2]);
    // The original reads d3dx.ini next to itself; it loads the system d3d11.dll for the real device.
    SetDllDirectoryW(folder.c_str());
    HMODULE xxmi = LoadLibraryW((folder + L"\\d3d11.dll").c_str());
    if (!xxmi) { fprintf(stderr, "cannot load the original d3d11.dll (%lu)\n", GetLastError()); return 3; }
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(xxmi, "D3D11CreateDeviceAndSwapChain"));
    if (!create) { fprintf(stderr, "the original exports no D3D11CreateDeviceAndSwapChain\n"); return 3; }
    WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"efvk-dx11-judge";
    RegisterClassW(&wc);
    // Off screen and without the focus, as the project's test windows are.
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"efvk dx11 judge", WS_POPUP,
                                  -32000, -32000, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = 256; sd.BufferDesc.Height = 256; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = window; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* swap = nullptr; ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &context);
    if (FAILED(hr))
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &context);
    if (FAILED(hr)) { fprintf(stderr, "device creation through the original failed: 0x%08lx\n", hr); return 4; }
    ID3D11Texture2D* back = nullptr; swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr; device->CreateRenderTargetView(back, nullptr, &rtv);
    const float clear[4] = {0, 0, 0, 1};
    judge_scene::Scene scene;
    if (scene_mode && !judge_scene::create(device, scene)) return 5;
    for (int i = 0; i < frames; ++i) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        if (scene_mode) {
            const bool last = i == frames - 1;
            judge_scene::draw(context, scene, rtv, last);
            if (last && !judge_scene::report(context, scene, back)) return 6;
        }
        swap->Present(0, 0);
        Sleep(10);
    }
    if (!scene_mode) printf("presented %d frames\n", frames);
    rtv->Release(); back->Release(); context->Release(); device->Release(); swap->Release();
    DestroyWindow(window);
    return 0;
}
