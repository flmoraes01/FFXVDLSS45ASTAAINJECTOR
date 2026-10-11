// Simulates FFXV's NGX r1.2 call sequence against the mod (build/winmm.dll) on a real D3D11 device.
// Built as ffxv_test.exe (the mod only activates inside processes whose name contains "ffxv").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include "nvsdk_ngx.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

typedef NVSDK_NGX_Result(NVSDK_CONV* PInit)(unsigned long long, const wchar_t*, ID3D11Device*, NVSDK_NGX_Version);
typedef NVSDK_NGX_Result(NVSDK_CONV* PGetParams)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result(NVSDK_CONV* PScratch)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PCreate)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(NVSDK_CONV* PEval)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PRelease)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PShutdown)();

static const char* kDecode = R"(
cbuffer cbMotMat : register(b0) { float4x4 g_motionMatrix; float4 g_jitterOfs; };
Texture2D g_depthTex : register(t0);
Texture2D g_velocityTex : register(t1);
SamplerState pointSampler : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{ uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float d = g_depthTex.SampleLevel(pointSampler, uv, 0).r;
    float2 v = g_velocityTex.SampleLevel(pointSampler, uv, 0).xy;
    float4 p = mul(g_motionMatrix, float4(uv, d, 1));
    float2 mv = p.xy / p.w - uv + v * 0.001 + g_jitterOfs.xy * 0.5;
    return float4(mv * g_jitterOfs.zw, 0, 0);
}
float4 scene(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{ return float4(frac(uv * 8), 0.5, 1) * 4; }
)";

// Stand-in for FFXV's TemporalAA pixel shader: same names and cbuffer layout, flat red output.
static const char* kTaa = R"(
cbuffer cbTemporalAA : register(b0) {
    float4 g_screenSize; uint4 g_frameBits; float4 g_uvJitterOffset; float4x4 g_motionMatrix;
    float4x4 g_reconstructMatrix; float4 g_unprojectParams; float g_maxLuminanceInv; uint g_gamePaused;
    uint g_hairUseAlphaTest; uint g_waterResponsiveAA;
};
Texture2D g_jitterTex : register(t0);
Texture2D g_historyTex : register(t1);
Texture2D g_depthTex : register(t3);
Texture2D g_velocityTex : register(t6);
SamplerState pointSampler : register(s0);
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy * g_screenSize.zw;
    float k = g_jitterTex.SampleLevel(pointSampler, uv + g_uvJitterOffset.xy, 0).r + g_historyTex.SampleLevel(pointSampler, uv, 0).r
            + g_depthTex.SampleLevel(pointSampler, uv, 0).r + g_velocityTex.SampleLevel(pointSampler, uv, 0).r + g_motionMatrix[0][0];
    return float4(1, 0, 0, 0) + 1e-20 * k;
}
// Stand-in for the game's upscale draw (a plain textured draw): flat green.
float4 up(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(0, 1, 0, 1) + 1e-20 * g_jitterTex.SampleLevel(pointSampler, uv, 0);
}
)";

#define CHECK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAILED %s -> 0x%08lX\n", #x, _h); return 1; } } while (0)

static ID3D11Texture2D* Tex(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = 1; td.BindFlags = bind; ID3D11Texture2D* t = nullptr; d->CreateTexture2D(&td, nullptr, &t); return t;
}

static float Halton(int i, int b) { float f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i /= b; } return r; }

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH); *wcsrchr(dir, L'\\') = 0;
    wchar_t mod[MAX_PATH]; swprintf(mod, MAX_PATH, L"%s\\winmm.dll", dir);
    if (!LoadLibraryW(mod)) { printf("cannot load mod\n"); return 1; }

    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    Sleep(1500);   // let the mod's hook thread patch the shared device vtable (the context is hooked lazily)

    HMODULE ngx = LoadLibraryW(L"kernel32.dll");   // any module: the hook keys on the function name
    auto Init = (PInit)GetProcAddress(ngx, "NVSDK_NGX_D3D11_Init");
    auto GetParams = (PGetParams)GetProcAddress(ngx, "NVSDK_NGX_D3D11_GetParameters");
    auto Scratch = (PScratch)GetProcAddress(ngx, "NVSDK_NGX_D3D11_GetScratchBufferSize");
    auto Create = (PCreate)GetProcAddress(ngx, "NVSDK_NGX_D3D11_CreateFeature");
    auto Eval = (PEval)GetProcAddress(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
    auto Release = (PRelease)GetProcAddress(ngx, "NVSDK_NGX_D3D11_ReleaseFeature");
    auto Shutdown = (PShutdown)GetProcAddress(ngx, "NVSDK_NGX_D3D11_Shutdown");
    if (!Init || !Create || !Eval) { printf("shim not installed\n"); return 1; }

    NVSDK_NGX_Result r = Init(0x123456, dir, dev, (NVSDK_NGX_Version)0x12);
    printf("Init -> 0x%08X\n", r); if (r != NVSDK_NGX_Result_Success) return 1;

    NVSDK_NGX_Parameter* p; GetParams(&p);
    int a1 = 0, a9 = 0; p->Get("#\x01", &a1); p->Get("#\x09", &a9);
    printf("availability temporal=%d dlisp=%d\n", a1, a9);

    const UINT rw = 1720, rh = 720, ow = 3440, oh = 1440;
    size_t s1 = 0; p->Set("Width", rw); p->Set("Height", rh); Scratch((NVSDK_NGX_Feature)1, p, &s1);
    p->Set("Hint.HDR", 1); p->Set("Hint.UseFireflySwatter", 0);
    NVSDK_NGX_Handle *hT = nullptr, *hS = nullptr;
    printf("Create temporal -> 0x%08X\n", Create(ctx, (NVSDK_NGX_Feature)1, p, &hT));
    p->Set("OutWidth", ow); p->Set("OutHeight", oh); p->Set("Sharpness", 0.3f);
    printf("Create dlisp -> 0x%08X\n", Create(ctx, (NVSDK_NGX_Feature)9, p, &hS));

    ID3DBlob *vsb, *psb, *scb, *err = nullptr;
    if (FAILED(D3DCompile(kDecode, strlen(kDecode), 0, 0, 0, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kDecode, strlen(kDecode), 0, 0, 0, "ps", "ps_5_0", 0, 0, &psb, &err)) ||
        FAILED(D3DCompile(kDecode, strlen(kDecode), 0, 0, 0, "scene", "ps_5_0", 0, 0, &scb, &err))) {
        printf("%s\n", (char*)err->GetBufferPointer()); return 1;
    }
    ID3D11VertexShader* vs; ID3D11PixelShader *decode, *scene;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), 0, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), 0, &decode);
    dev->CreatePixelShader(scb->GetBufferPointer(), scb->GetBufferSize(), 0, &scene);

    auto color = Tex(dev, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto temporalOut = Tex(dev, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto ldr = Tex(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto mv = Tex(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto vel = Tex(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto depth = Tex(dev, rw, rh, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    auto out = Tex(dev, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView *mvRtv, *colorRtv; dev->CreateRenderTargetView(mv, 0, &mvRtv); dev->CreateRenderTargetView(color, 0, &colorRtv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView* dsv; dev->CreateDepthStencilView(depth, &dd, &dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *depthSrv, *velSrv; dev->CreateShaderResourceView(depth, &sd, &depthSrv); dev->CreateShaderResourceView(vel, 0, &velSrv);

    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb; dev->CreateBuffer(&bd, nullptr, &cb);
    D3D11_SAMPLER_DESC smp = {}; smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* ss; dev->CreateSamplerState(&smp, &ss);

    for (int f = 0; f < 60; ++f) {
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);   // reversed-Z clear, as the mod should detect
        D3D11_VIEWPORT vp = { 0, 0, (float)rw, (float)rh, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, 0, 0);
        ctx->OMSetRenderTargets(1, &colorRtv, nullptr); ctx->PSSetShader(scene, 0, 0); ctx->Draw(3, 0);

        // Velocity decode pass with an absolute Halton jitter in "2*UV" units
        D3D11_MAPPED_SUBRESOURCE m; ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float c[20] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        c[16] = 2.f * (Halton(f % 8 + 1, 2) - 0.5f) / rw; c[17] = 2.f * (Halton(f % 8 + 1, 3) - 0.5f) / rh; c[18] = 1; c[19] = 1;
        memcpy(m.pData, c, sizeof(c)); ctx->Unmap(cb, 0);
        ID3D11ShaderResourceView* srvs[2] = { depthSrv, velSrv };
        ctx->OMSetRenderTargets(1, &mvRtv, nullptr);
        ctx->PSSetShader(decode, 0, 0); ctx->PSSetShaderResources(0, 2, srvs); ctx->PSSetConstantBuffers(0, 1, &cb); ctx->PSSetSamplers(0, 1, &ss);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nul[2] = {}; ctx->PSSetShaderResources(0, 2, nul);

        p->Set("Color", (ID3D11Resource*)color); p->Set("MotionVectors", (ID3D11Resource*)mv); p->Set("Output", (ID3D11Resource*)temporalOut);
        p->Set("BlendFactor", 0.1f); p->Set("Reset", f == 0 ? 1 : 0); p->Set("Hint.HDR", 1);
        p->Set("MV.Scale.X", (float)rw); p->Set("MV.Scale.Y", (float)rh);
        NVSDK_NGX_Result rt = Eval(ctx, hT, p, nullptr);
        ctx->CopyResource(ldr, ldr);   // stand-in for post-processing
        p->Set("Width", (int)rw); p->Set("Height", (int)rh); p->Set("OutWidth", (int)ow); p->Set("OutHeight", (int)oh);
        p->Set("Sharpness", 0.3f); p->Set("Color", (ID3D11Resource*)ldr); p->Set("Output", (ID3D11Resource*)out);
        NVSDK_NGX_Result rs = Eval(ctx, hS, p, nullptr);
        if (f < 2 || f == 59) printf("frame %d: temporal 0x%08X spatial 0x%08X\n", f, rt, rs);
        if (rt != NVSDK_NGX_Result_Success || rs != NVSDK_NGX_Result_Success) return 2;
    }
    ctx->Flush();
    Release(hT); Release(hS);

    // TAA path: a stand-in for FFXV's TemporalAA pass (same resource and cbuffer names). The mod
    // must run DLSS in its place, so the output must hold the scene, not the shader's flat red.
    {
        ID3DBlob *tb = nullptr;
        if (FAILED(D3DCompile(kTaa, strlen(kTaa), 0, 0, 0, "ps", "ps_5_0", 0, 0, &tb, &err))) {
            printf("%s\n", (char*)err->GetBufferPointer()); return 1;
        }
        ID3D11PixelShader* taa; dev->CreatePixelShader(tb->GetBufferPointer(), tb->GetBufferSize(), 0, &taa);
        ID3DBlob* ub = nullptr;
        if (FAILED(D3DCompile(kTaa, strlen(kTaa), 0, 0, 0, "up", "ps_5_0", 0, 0, &ub, &err))) {
            printf("%s\n", (char*)err->GetBufferPointer()); return 1;
        }
        ID3D11PixelShader* upPs; dev->CreatePixelShader(ub->GetBufferPointer(), ub->GetBufferSize(), 0, &upPs);
        auto upOut = Tex(dev, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ID3D11RenderTargetView* upRtv; dev->CreateRenderTargetView(upOut, 0, &upRtv);
        auto taaOut = Tex(dev, rw, rh, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ID3D11RenderTargetView* taaRtv; dev->CreateRenderTargetView(taaOut, 0, &taaRtv);
        ID3D11ShaderResourceView* colorSrv; dev->CreateShaderResourceView(color, 0, &colorSrv);
        D3D11_BUFFER_DESC tcb = {}; tcb.ByteWidth = 208; tcb.Usage = D3D11_USAGE_DYNAMIC; tcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; tcb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* taaCb; dev->CreateBuffer(&tcb, nullptr, &taaCb);
        for (int f = 0; f < 30; ++f) {
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            D3D11_VIEWPORT vp = { 0, 0, (float)rw, (float)rh, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->VSSetShader(vs, 0, 0);
            ctx->OMSetRenderTargets(1, &colorRtv, nullptr); ctx->PSSetShader(scene, 0, 0); ctx->Draw(3, 0);
            D3D11_MAPPED_SUBRESOURCE m; ctx->Map(taaCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            float c[52] = {};
            c[0] = (float)rw; c[1] = (float)rh; c[2] = 1.f / rw; c[3] = 1.f / rh;                   // g_screenSize
            c[8] = (Halton(f % 8 + 1, 2) - 0.5f) / rw; c[9] = (Halton(f % 8 + 1, 3) - 0.5f) / rh;  // g_uvJitterOffset
            c[12] = 1; c[17] = 1; c[22] = 1; c[27] = 1;                                         // g_motionMatrix = identity
            memcpy(m.pData, c, sizeof(c)); ctx->Unmap(taaCb, 0);
            ID3D11ShaderResourceView* srvs[7] = { colorSrv, colorSrv, nullptr, depthSrv, nullptr, nullptr, velSrv };
            ctx->OMSetRenderTargets(1, &taaRtv, nullptr);
            ctx->PSSetShader(taa, 0, 0); ctx->PSSetShaderResources(0, 7, srvs); ctx->PSSetConstantBuffers(0, 1, &taaCb);
            ctx->PSSetSamplers(0, 1, &ss);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* nul7[7] = {}; ctx->PSSetShaderResources(0, 7, nul7);
            if (f >= 15) {   // from here on the "game" also upscales the TAA output to display size
                ID3D11ShaderResourceView* taaSrv; dev->CreateShaderResourceView(taaOut, 0, &taaSrv);
                D3D11_VIEWPORT ovp = { 0, 0, (float)ow, (float)oh, 0, 1 };
                ctx->RSSetViewports(1, &ovp);
                ctx->OMSetRenderTargets(1, &upRtv, nullptr);
                ctx->PSSetShader(upPs, 0, 0); ctx->PSSetShaderResources(0, 1, &taaSrv);
                ctx->Draw(3, 0);
                ctx->PSSetShaderResources(0, 1, nul7);
                taaSrv->Release();
            }
        }
        // Read back one pixel: the TAA shader writes pure red, the scene has blue = 0.5 * 4.
        auto staging = [&] { D3D11_TEXTURE2D_DESC td = {}; taaOut->GetDesc(&td); td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
                             td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&td, nullptr, &t); return t; }();
        ctx->CopyResource(staging, taaOut);
        D3D11_MAPPED_SUBRESOURCE rm; ctx->Map(staging, 0, D3D11_MAP_READ, 0, &rm);
        UINT px = *(UINT*)((BYTE*)rm.pData + rm.RowPitch * (rh / 2) + 4 * (rw / 2));
        ctx->Unmap(staging, 0);
        UINT blueBits = px >> 22;   // R11G11B10: blue is the top 10 bits
        printf("TAA output pixel 0x%08X (blue bits %u): %s\n", px, blueBits, blueBits ? "DLSS output" : "game TAA output (not replaced)");
        if (!blueBits) return 3;

        // The upscale target must hold DLSS's display-resolution output, not flat green.
        D3D11_TEXTURE2D_DESC ud = {}; upOut->GetDesc(&ud); ud.Usage = D3D11_USAGE_STAGING; ud.BindFlags = 0; ud.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* upStaging = nullptr; dev->CreateTexture2D(&ud, nullptr, &upStaging);
        ctx->CopyResource(upStaging, upOut);
        ctx->Map(upStaging, 0, D3D11_MAP_READ, 0, &rm);
        const USHORT* h = (const USHORT*)((BYTE*)rm.pData + rm.RowPitch * (oh / 2) + 8 * (ow / 2));
        USHORT red = h[0], green = h[1], blue = h[2];
        ctx->Unmap(upStaging, 0);
        printf("Upscale output pixel R=%04X G=%04X B=%04X: %s\n", red, green, blue, blue ? "DLSS upscale" : "game upscale (not replaced)");
        if (!blue) return 4;
    }
    Shutdown();
    printf("OK\n");
    return 0;
}
