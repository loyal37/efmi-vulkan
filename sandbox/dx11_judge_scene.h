// The judge's scene (docs/architecture-review-2.1.md, P3-1): the program is the "game". Each frame it
// draws two indexed quads of known index bytes, so the original runs the [TextureOverride] sections matched
// by a draw's index region hash, as it runs a Mod's sections in the game (through a ShaderRegex of the
// judge's d3dx.ini that checks `ib` at every draw, as the EFMI Core's does). What the lists did is read off
// the picture and the input assembler: the colour at two points of each of four places, and the primitives
// each game draw put through (its own and the lists' draws).
//
// The picture, 256 x 256, four places of 128 x 128:
//   A (top left): the game's first quad      B (top right): the game's second quad
//   C (bottom left), D (bottom right): empty; a Mod's vertex files put quads there
// A quad's vertices are (left, top), (right, top), (left, bottom), (right, bottom) as two floats each
// (stride 8, POSITION). The first game draw's indices are 0 1 2, 2 1 3; the second's 0 1 2, 1 3 2 (the same
// triangles, other bytes, so another region hash). The pixel shader returns texel (0, 0) of t0: the game's
// first texture is (64, 64, 64, 255), its second (128, 128, 128, 255); no texture reads (0, 0, 0, 0), which
// the cleared target (0, 0, 0, 255) is not.
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace judge_scene {
inline void quad(float left, float top, float out[8])
{
    const float right = left + 1.0f, bottom = top - 1.0f;
    const float v[8] = {left, top, right, top, left, bottom, right, bottom};
    memcpy(out, v, sizeof(v));
}
struct Scene {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11Buffer* vb[2] = {};
    ID3D11Buffer* ib[2] = {};
    ID3D11ShaderResourceView* texture[2] = {};
    ID3D11Query* statistics[2] = {};
    ID3D11Texture2D* staging = nullptr;
};
inline bool failed(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr)) return false;
    fprintf(stderr, "judge scene: %s failed: 0x%08lx\n", what, hr);
    return true;
}
inline bool create(ID3D11Device* device, Scene& s)
{
    static const char source[] =
        "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
        "Texture2D t : register(t0);\n"
        "float4 ps(float4 p : SV_Position) : SV_Target { return t.Load(int3(0, 0, 0)); }\n";
    ID3DBlob *vs = nullptr, *ps = nullptr, *errors = nullptr;
    if (failed(D3DCompile(source, sizeof(source) - 1, "judge", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors), "vertex shader compilation")) return false;
    if (failed(D3DCompile(source, sizeof(source) - 1, "judge", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors), "pixel shader compilation")) return false;
    if (failed(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s.vs), "CreateVertexShader")) return false;
    if (failed(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s.ps), "CreatePixelShader")) return false;
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (failed(device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &s.layout), "CreateInputLayout")) return false;
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    if (failed(device->CreateRasterizerState(&raster, &s.raster), "CreateRasterizerState")) return false;
    const uint16_t indices[2][6] = {{0, 1, 2, 2, 1, 3}, {0, 1, 2, 1, 3, 2}};
    const uint8_t shade[2] = {64, 128};
    for (int i = 0; i < 2; ++i) {
        float positions[8]; quad(i ? 0.0f : -1.0f, 1.0f, positions);
        D3D11_BUFFER_DESC vb{}; vb.ByteWidth = sizeof(positions); vb.Usage = D3D11_USAGE_DEFAULT; vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = positions;
        if (failed(device->CreateBuffer(&vb, &data, &s.vb[i]), "vertex buffer")) return false;
        D3D11_BUFFER_DESC ib{}; ib.ByteWidth = sizeof(indices[i]); ib.Usage = D3D11_USAGE_DEFAULT; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        data.pSysMem = indices[i];
        if (failed(device->CreateBuffer(&ib, &data, &s.ib[i]), "index buffer")) return false;
        uint8_t texels[4 * 4 * 4];
        for (int t = 0; t < 16; ++t) { texels[t * 4] = texels[t * 4 + 1] = texels[t * 4 + 2] = shade[i]; texels[t * 4 + 3] = 255; }
        D3D11_TEXTURE2D_DESC texture{}; texture.Width = texture.Height = 4; texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        data.pSysMem = texels; data.SysMemPitch = 16;
        ID3D11Texture2D* image = nullptr;
        if (failed(device->CreateTexture2D(&texture, &data, &image), "texture")) return false;
        if (failed(device->CreateShaderResourceView(image, nullptr, &s.texture[i]), "texture view")) return false;
        image->Release();
        D3D11_QUERY_DESC query{}; query.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        if (failed(device->CreateQuery(&query, &s.statistics[i]), "statistics query")) return false;
    }
    D3D11_TEXTURE2D_DESC staging{}; staging.Width = staging.Height = 256; staging.MipLevels = staging.ArraySize = 1;
    staging.Format = DXGI_FORMAT_R8G8B8A8_UNORM; staging.SampleDesc.Count = 1; staging.Usage = D3D11_USAGE_STAGING; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return !failed(device->CreateTexture2D(&staging, nullptr, &s.staging), "staging texture");
}
// One frame's two game draws, each with the state the game sets for it (a list's bindings do not reach
// the next draw: the game binds its own again, as a real game does).
inline void draw(ID3D11DeviceContext* context, const Scene& s, ID3D11RenderTargetView* target, bool measure)
{
    const D3D11_VIEWPORT viewport = {0, 0, 256, 256, 0, 1};
    for (int i = 0; i < 2; ++i) {
        const UINT stride = 8, offset = 0;
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport); context->RSSetState(s.raster);
        context->IASetInputLayout(s.layout); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 1, &s.vb[i], &stride, &offset);
        context->IASetIndexBuffer(s.ib[i], DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(s.vs, nullptr, 0); context->PSSetShader(s.ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &s.texture[i]);
        if (measure) context->Begin(s.statistics[i]);
        // As the game draws its characters (DRAW_TYPE 4 in the Mods' lists), one instance.
        context->DrawIndexedInstanced(6, 1, 0, 0, 0);
        if (measure) context->End(s.statistics[i]);
    }
}
// After the last frame's draws, before its presentation: the eight points and the two primitive counts,
// one JSON line on stdout.
inline bool report(ID3D11DeviceContext* context, const Scene& s, ID3D11Texture2D* back)
{
    context->CopyResource(s.staging, back);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (failed(context->Map(s.staging, 0, D3D11_MAP_READ, 0, &map), "mapping the picture")) return false;
    printf("{\"pixels\":{");
    static const char* const names = "ABCD";
    for (int place = 0; place < 4; ++place)
        for (int triangle = 0; triangle < 2; ++triangle) {
            // A point inside the quad's first triangle (towards its top left) and one inside its second.
            const int x = (place & 1) * 128 + (triangle ? 96 : 32), y = (place >> 1) * 128 + (triangle ? 96 : 32);
            const uint8_t* p = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + size_t(x) * 4;
            printf("%s\"%c%d\":[%u,%u,%u,%u]", place || triangle ? "," : "", names[place], triangle + 1, p[0], p[1], p[2], p[3]);
        }
    context->Unmap(s.staging, 0);
    printf("},\"primitives\":[");
    for (int i = 0; i < 2; ++i) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
        HRESULT hr = S_FALSE;
        for (int tries = 0; tries < 2000 && (hr = context->GetData(s.statistics[i], &data, sizeof(data), 0)) == S_FALSE; ++tries) Sleep(1);
        if (failed(hr, "reading the statistics")) return false;
        printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(data.IAPrimitives));
    }
    printf("]}\n");
    return true;
}
}
