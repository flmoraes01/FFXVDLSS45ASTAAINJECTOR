// FFXV DLSS4 - replaces the DLSS 1.0 (NGX r1.2 "dlisp") integration of FINAL FANTASY XV
// Windows Edition with the current DLSS Super Resolution (transformer presets K/L/M).
//
// How the game uses DLSS 1 (reverse engineered from ffxv_s.exe):
//   * The NGX r1.2 SDK is linked statically. It loads the driver's _nvngx.dll and resolves
//     NVSDK_NGX_D3D11_{Init,Shutdown,GetParameters,GetScratchBufferSize,CreateFeature,
//     EvaluateFeature,ReleaseFeature} through the exe's GetProcAddress import.
//   * Availability is queried with Get(int) on the keys "#\x01" and "#\x09".
//   * Feature 1  = temporal stage (render res, HDR, before post-processing):
//                  Color, MotionVectors, Output, BlendFactor, Reset, MV.Scale.X/Y.
//   * Feature 9  = spatial upscaler "dlisp" (after post-processing): Width/Height ->
//                  OutWidth/OutHeight, Color, Output, Sharpness.
//   * Motion vectors come from the NVDLSSVelocityDecode pass (PS: t0 = g_depthTex,
//     t1 = g_velocityTex, b0 = cbMotMat { float4x4 g_motionMatrix; float4 g_jitterOfs; }).
//
// Replacement strategy:
//   * GetProcAddress of those seven names returns the Shim_* functions below.
//   * Temporal stage: copies Color to Output (the jittered frame flows through post-FX),
//     and snapshots depth + motion vectors (the game recycles those textures).
//   * Spatial stage: one DLSS SR evaluation, render res -> display res, using the snapshots
//     and the jitter read from cbMotMat. Without a spatial stage it runs DLAA instead.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"
#include "proxy_exports.h"
#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "version.lib")

// ------------------------------------------------------------------------------------------
// Logging / config
// ------------------------------------------------------------------------------------------
static wchar_t g_gameDir[MAX_PATH];
static FILE* g_log;
static std::mutex g_logMtx;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_logMtx);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

enum class JitterMode { Auto, Absolute, Delta, Off };

static struct Config {
    bool enabled = true;
    int preset = NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    int depthInverted = -1;           // -1 = auto (detected from depth clears)
    JitterMode jitter = JitterMode::Auto;
    float jitterSignX = 1.f, jitterSignY = 1.f;
    float mvScaleX = 1.f, mvScaleY = 1.f;
    bool mvJittered = false;
    bool verbose = false;
    bool replaceTaa = true;           // run DLSS in place of the game's TAA pass
    int taaPreset = NVSDK_NGX_DLSS_Hint_Render_Preset_L;
} g_cfg;

static int ParsePreset(const wchar_t* s)
{
    if (!_wcsicmp(s, L"Default")) return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    if (!_wcsicmp(s, L"J")) return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
    if (!_wcsicmp(s, L"K")) return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    if (!_wcsicmp(s, L"L")) return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    if (!_wcsicmp(s, L"M")) return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
}

static void LoadConfig()
{
    std::wstring ini = std::wstring(g_gameDir) + L"\\FFXV_DLSS4.ini";
    auto str = [&](const wchar_t* key, const wchar_t* def) {
        static wchar_t buf[64];
        GetPrivateProfileStringW(L"DLSS", key, def, buf, 64, ini.c_str());
        return std::wstring(buf);
    };
    auto flt = [&](const wchar_t* key, float def) {
        std::wstring v = str(key, L"");
        return v.empty() ? def : (float)_wtof(v.c_str());
    };
    g_cfg.enabled = GetPrivateProfileIntW(L"DLSS", L"Enabled", 1, ini.c_str()) != 0;
    g_cfg.preset = ParsePreset(str(L"Preset", L"M").c_str());
    std::wstring d = str(L"DepthInverted", L"Auto");
    g_cfg.depthInverted = !_wcsicmp(d.c_str(), L"Auto") ? -1 : (_wtoi(d.c_str()) ? 1 : 0);
    std::wstring j = str(L"JitterMode", L"Auto");
    g_cfg.jitter = !_wcsicmp(j.c_str(), L"Absolute") ? JitterMode::Absolute
                 : !_wcsicmp(j.c_str(), L"Delta") ? JitterMode::Delta
                 : !_wcsicmp(j.c_str(), L"Off") ? JitterMode::Off : JitterMode::Auto;
    g_cfg.jitterSignX = flt(L"JitterScaleX", 1.f);
    g_cfg.jitterSignY = flt(L"JitterScaleY", 1.f);
    g_cfg.mvScaleX = flt(L"MVScaleX", 1.f);
    g_cfg.mvScaleY = flt(L"MVScaleY", 1.f);
    g_cfg.mvJittered = GetPrivateProfileIntW(L"DLSS", L"MVJittered", 0, ini.c_str()) != 0;
    g_cfg.verbose = GetPrivateProfileIntW(L"DLSS", L"VerboseLog", 0, ini.c_str()) != 0;
    g_cfg.replaceTaa = GetPrivateProfileIntW(L"DLSS", L"ReplaceTAA", 1, ini.c_str()) != 0;
    g_cfg.taaPreset = ParsePreset(str(L"TAAPreset", L"L").c_str());
    Log("Config: enabled=%d preset=%d depthInverted=%d jitterMode=%d jitterScale=(%g,%g) mvScale=(%g,%g) mvJittered=%d replaceTAA=%d taaPreset=%d",
        g_cfg.enabled, g_cfg.preset, g_cfg.depthInverted, (int)g_cfg.jitter, g_cfg.jitterSignX, g_cfg.jitterSignY,
        g_cfg.mvScaleX, g_cfg.mvScaleY, g_cfg.mvJittered, g_cfg.replaceTaa, g_cfg.taaPreset);
}

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool MinHookReady()
{
    static const bool ok = [] { MH_STATUS s = MH_Initialize(); return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED; }();
    return ok;
}

// ------------------------------------------------------------------------------------------
// Per-frame capture from the velocity decode pass (jitter + depth)
// ------------------------------------------------------------------------------------------
static SRWLOCK g_cbLock = SRWLOCK_INIT;
static std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> g_cbShadow;   // CPU copy of small constant buffers
struct PendingMap { void* data; UINT size; };
static std::unordered_map<ID3D11Resource*, PendingMap> g_cbPending;
static std::unordered_map<ID3D11Resource*, void*> g_cbLastPtr;                  // big (ring) CBs seen at the decode pass
static std::unordered_map<ID3D11Resource*, bool> g_cbWatchedBig;

static SRWLOCK g_captureLock = SRWLOCK_INIT;
static ID3D11PixelShader* g_decodePS[8];
static std::atomic<int> g_decodePSCount{0};
static ID3D11Resource* g_lastMvRes;            // last MotionVectors resource from the temporal stage (fallback detection)
static struct Capture {
    ID3D11ShaderResourceView* depthSrv = nullptr;   // g_depthTex of the decode pass
    float jitterOfs[4] = {};
    bool jitterValid = false;
    UINT64 frame = 0;
} g_cap;
static UINT64 g_decodeCount;

static SRWLOCK g_clearLock = SRWLOCK_INIT;
static std::unordered_map<ID3D11Resource*, float> g_depthClears;   // last depth clear value per depth texture

static bool IsDecodePS(ID3D11PixelShader* ps)
{
    int n = g_decodePSCount.load();
    for (int i = 0; i < n; ++i) if (g_decodePS[i] == ps) return true;
    return false;
}

static void AddDecodePS(ID3D11PixelShader* ps)
{
    AcquireSRWLockExclusive(&g_captureLock);
    int n = g_decodePSCount.load();
    if (!IsDecodePS(ps) && n < 8) { g_decodePS[n] = ps; g_decodePSCount.store(n + 1); Log("Velocity decode shader identified: %p", ps); }
    ReleaseSRWLockExclusive(&g_captureLock);
}

static bool ReadCB(ID3D11Buffer* buf, UINT offset, UINT size, void* out)
{
    bool ok = false;
    AcquireSRWLockShared(&g_cbLock);
    auto it = g_cbShadow.find(buf);
    if (it != g_cbShadow.end() && it->second.size() >= offset + size) { memcpy(out, it->second.data() + offset, size); ok = true; }
    else {
        auto lp = g_cbLastPtr.find(buf);
        if (lp != g_cbLastPtr.end() && lp->second) { memcpy(out, (uint8_t*)lp->second + offset, size); ok = true; }
    }
    ReleaseSRWLockShared(&g_cbLock);
    return ok;
}

static void CaptureDecodePass(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);

    ID3D11Buffer* cb = nullptr; UINT first = 0, num = 0;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1))) {
        ctx1->PSGetConstantBuffers1(0, 1, &cb, &first, &num);
        ctx1->Release();
    } else {
        ctx->PSGetConstantBuffers(0, 1, &cb);
    }

    float mm[20] = {}; bool jok = false;   // cbMotMat: g_motionMatrix (16 floats) + g_jitterOfs (4)
    float* jit = mm + 16;
    D3D11_BUFFER_DESC bd = {};
    if (cb) {
        cb->GetDesc(&bd);
        if (bd.ByteWidth > 256) {
            AcquireSRWLockExclusive(&g_cbLock);
            g_cbWatchedBig[cb] = true;
            ReleaseSRWLockExclusive(&g_cbLock);
        }
        jok = ReadCB(cb, first * 16, sizeof(mm), mm);
    }

    AcquireSRWLockExclusive(&g_captureLock);
    SafeRelease(g_cap.depthSrv);
    g_cap.depthSrv = srv; srv = nullptr;
    if (jok) memcpy(g_cap.jitterOfs, jit, sizeof(jit));
    g_cap.jitterValid = jok;
    g_cap.frame = ++g_decodeCount;
    ReleaseSRWLockExclusive(&g_captureLock);

    if (g_decodeCount <= 3 || (g_cfg.verbose && g_decodeCount <= 400))
        Log("Decode pass #%llu: cb=%p size=%u usage=%u first=%u num=%u jitterOfs=%s(%.7f, %.7f, %.4f, %.4f) depthSrv=%p",
            g_decodeCount, cb, bd.ByteWidth, bd.Usage, first, num, jok ? "" : "UNREAD ", jit[0], jit[1], jit[2], jit[3], g_cap.depthSrv);
    // A jittered projection shows up in the motion matrix as an oscillating xy translation
    // while the camera is still; log it to tell whether the game jitters at all.
    if (g_cfg.verbose && g_decodeCount <= 400)
        Log("  motionMatrix rows: [%.6f %.6f %.6f %.6f] [%.6f %.6f %.6f %.6f] [%.6f %.6f %.6f %.6f] [%.7f %.7f %.6f %.6f]",
            mm[0], mm[1], mm[2], mm[3], mm[4], mm[5], mm[6], mm[7], mm[8], mm[9], mm[10], mm[11], mm[12], mm[13], mm[14], mm[15]);
    SafeRelease(cb);
}

// ------------------------------------------------------------------------------------------
// TAA probe (diagnostic): logs what the game's TemporalAA passes bind, as groundwork for running
// DLSS in place of the game's TAA. Shaders are recognised by "cbTemporalAA" in their reflection
// data (RDEF chunk of the DXBC), which also gives the names of every bound resource.
// ------------------------------------------------------------------------------------------
struct RdefBind { std::string name; UINT type, slot; };          // type: D3D_SHADER_INPUT_TYPE
struct RdefVar { std::string name; UINT offset, size; };
struct RdefCB { std::string name; UINT size; std::vector<RdefVar> vars; };
struct ShaderInfo {
    std::vector<RdefBind> binds; std::vector<RdefCB> cbs; std::atomic<int> hits{0};
    // Resolved from the reflection data; -1 when the shader does not have them.
    int colorSlot = -1, depthSlot = -1, velSlot = -1, cbSlot = -1;
    int screenSizeOfs = -1, jitterOfs = -1, motionOfs = -1;
    UINT cbSize = 0;
};

static SRWLOCK g_taaLock = SRWLOCK_INIT;
static std::unordered_map<void*, ShaderInfo*> g_taaShaders;
static std::atomic<int> g_taaCount{0};

static bool ParseRdef(const void* code, SIZE_T len, ShaderInfo& si)
{
    auto b = (const uint8_t*)code;
    if (len < 32 || memcmp(b, "DXBC", 4)) return false;
    UINT chunks = *(const UINT*)(b + 28);
    for (UINT c = 0; c < chunks && 32 + (c + 1) * 4 <= len; ++c) {
        UINT off = *(const UINT*)(b + 32 + c * 4);
        if ((SIZE_T)off + 8 > len || memcmp(b + off, "RDEF", 4)) continue;
        UINT size = *(const UINT*)(b + off + 4);
        if ((SIZE_T)off + 8 + size > len) return false;
        const uint8_t* d = b + off + 8;
        auto u = [&](UINT o) { return o + 4 <= size ? *(const UINT*)(d + o) : 0u; };
        auto str = [&](UINT o) { std::string s; while (o < size && d[o] && s.size() < 64) s += (char)d[o++]; return s; };
        UINT varStride = ((u(16) >> 8) & 0xFF) >= 5 ? 40 : 24;
        for (UINT i = 0, n = std::min(u(8), 64u); i < n; ++i) {
            UINT o = u(12) + i * 32;
            si.binds.push_back({ str(u(o)), u(o + 4), u(o + 20) });
        }
        for (UINT i = 0, n = std::min(u(0), 16u); i < n; ++i) {
            UINT o = u(4) + i * 24;
            RdefCB cb{ str(u(o)), u(o + 12), {} };
            for (UINT v = 0, nv = std::min(u(o + 4), 64u); v < nv; ++v) {
                UINT vo = u(o + 8) + v * varStride;
                cb.vars.push_back({ str(u(vo)), u(vo + 4), u(vo + 8) });
            }
            si.cbs.push_back(std::move(cb));
        }
        return true;
    }
    return false;
}

// Writes the shader's disassembly next to the game (diagnostic: motion-vector conventions).
static void DumpShader(const void* code, SIZE_T len, const char* tag, void* shader)
{
    ID3DBlob* dis = nullptr;
    if (FAILED(D3DDisassemble(code, len, 0, nullptr, &dis)) || !dis) return;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\FFXV_DLSS4_%hs_%p.asm", g_gameDir, tag, shader);
    if (FILE* f = _wfopen(path, L"wb")) { fputs((const char*)dis->GetBufferPointer(), f); fclose(f); }
    dis->Release();
    Log("Wrote %s shader disassembly: %ls", tag, path);
}

// Which passes read the TAA output (diagnostic: where the image is upscaled to display size).
static std::string DescribeView(ID3D11View* v);
static bool TexDesc(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d);
static std::atomic<ID3D11Resource*> g_taaOut{nullptr};
static std::atomic<int> g_consumerScan{0};
static std::atomic<int> g_scanLogged{0};

static std::atomic<UINT> g_taaW{0}, g_taaH{0};

// Bytecode of every shader the game creates (VerboseLog only), to name the resources of the
// passes the scan below reports and to dump their disassembly.
static SRWLOCK g_codeLock = SRWLOCK_INIT;
static std::unordered_map<void*, std::vector<uint8_t>> g_shaderCode;

static void RememberShader(void* shader, const void* code, SIZE_T len)
{
    if (!g_cfg.verbose || !shader || !code) return;
    AcquireSRWLockExclusive(&g_codeLock);
    g_shaderCode[shader].assign((const uint8_t*)code, (const uint8_t*)code + len);
    ReleaseSRWLockExclusive(&g_codeLock);
}

static std::string ShaderResourceNames(void* shader, bool dump, const char* tag)
{
    std::vector<uint8_t> code;
    AcquireSRWLockShared(&g_codeLock);
    auto it = g_shaderCode.find(shader);
    if (it != g_shaderCode.end()) code = it->second;
    ReleaseSRWLockShared(&g_codeLock);
    if (code.empty()) return "";
    if (dump) DumpShader(code.data(), code.size(), tag, shader);
    ShaderInfo si;
    if (!ParseRdef(code.data(), code.size(), si)) return "";
    std::string names;
    for (auto& b : si.binds) if (b.type != D3D_SIT_SAMPLER) names += " " + b.name;
    return names;
}

// One frame of passes around the TAA (diagnostic): passes that read the TAA output, and passes
// that read a render-resolution texture and write a larger target (the upscale to display size).
static void ScanTaaConsumer(ID3D11DeviceContext* ctx, void* shader, bool compute)
{
    if (g_consumerScan.load() <= 0) return;
    --g_consumerScan;
    UINT rw = g_taaW.load(), rh = g_taaH.load();
    ID3D11ShaderResourceView* srv[8] = {};
    compute ? ctx->CSGetShaderResources(0, 8, srv) : ctx->PSGetShaderResources(0, 8, srv);
    int taaSlot = -1, renderSlot = -1;
    for (int i = 0; i < 8; ++i) {
        if (!srv[i]) continue;
        ID3D11Resource* r = nullptr; srv[i]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (r) r->GetType(&dim);
        if (r && r == g_taaOut.load() && taaSlot < 0) taaSlot = i;
        if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && renderSlot < 0) {
            D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
            if (d.Width == rw && d.Height == rh) renderSlot = i;
        }
        SafeRelease(r); srv[i]->Release();
    }
    if (taaSlot < 0 && renderSlot < 0) return;

    ID3D11View* outView = nullptr;
    if (compute) { ID3D11UnorderedAccessView* uav = nullptr; ctx->CSGetUnorderedAccessViews(0, 1, &uav); outView = uav; }
    else { ID3D11RenderTargetView* rtv = nullptr; ctx->OMGetRenderTargets(1, &rtv, nullptr); outView = rtv; }
    UINT ow = 0, oh = 0;
    if (outView) {
        ID3D11Resource* r = nullptr; outView->GetResource(&r);
        D3D11_TEXTURE2D_DESC d;
        if (r && TexDesc(r, d)) { ow = d.Width; oh = d.Height; }
        SafeRelease(r);
    }
    bool upscale = renderSlot >= 0 && ow > rw && oh > rh;
    if (taaSlot < 0 && !upscale) { SafeRelease(outView); return; }

    if (++g_scanLogged <= 40) {
        std::string target = DescribeView(outView);
        std::string names = ShaderResourceNames(shader, upscale, "upscale");
        if (upscale)
            Log("UPSCALE candidate %s %p reads %ux%u at t%d -> %s |%s", compute ? "CS" : "PS", shader, rw, rh, renderSlot, target.c_str(), names.c_str());
        else
            Log("TAA output read by %s %p at t%d -> %s |%s", compute ? "CS" : "PS", shader, taaSlot, target.c_str(), names.c_str());
    }
    SafeRelease(outView);
}

static void RegisterTaaShader(void* shader, const void* code, SIZE_T len, const char* kind)
{
    auto* si = new ShaderInfo();
    if (!ParseRdef(code, len, *si)) { delete si; return; }
    std::string desc;
    for (auto& bd : si->binds) { char t[96]; snprintf(t, sizeof t, " %s(t%u:%u)", bd.name.c_str(), bd.type, bd.slot); desc += t; }
    for (auto& cb : si->cbs) {
        desc += " | " + cb.name + "{";
        for (auto& v : cb.vars) { char t[96]; snprintf(t, sizeof t, " %s@%u/%u", v.name.c_str(), v.offset, v.size); desc += t; }
        desc += " }";
    }
    for (auto& bd : si->binds) {
        if (bd.name == "g_jitterTex") si->colorSlot = (int)bd.slot;
        else if (bd.name == "g_depthTex") si->depthSlot = (int)bd.slot;
        else if (bd.name == "g_velocityTex") si->velSlot = (int)bd.slot;
        else if (bd.name == "cbTemporalAA") si->cbSlot = (int)bd.slot;
    }
    for (auto& cb : si->cbs) {
        if (cb.name != "cbTemporalAA") continue;
        si->cbSize = cb.size;
        for (auto& v : cb.vars) {
            if (v.name == "g_screenSize") si->screenSizeOfs = (int)v.offset;
            else if (v.name == "g_uvJitterOffset") si->jitterOfs = (int)v.offset;
            else if (v.name == "g_motionMatrix") si->motionOfs = (int)v.offset;
        }
    }
    AcquireSRWLockExclusive(&g_taaLock);
    g_taaShaders[shader] = si;
    g_taaCount.store((int)g_taaShaders.size());
    ReleaseSRWLockExclusive(&g_taaLock);
    Log("TAA %s shader %p:%s", kind, shader, desc.c_str());
    if (g_cfg.verbose) DumpShader(code, len, "taa", shader);
}

static ShaderInfo* FindTaaShader(void* shader)
{
    AcquireSRWLockShared(&g_taaLock);
    auto it = g_taaShaders.find(shader);
    ShaderInfo* si = it == g_taaShaders.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_taaLock);
    return si;
}

static std::string DescribeView(ID3D11View* v)
{
    if (!v) return "null";
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return "?";
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    char t[128];
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        snprintf(t, sizeof t, "%p %ux%u fmt=%d mips=%u", r, d.Width, d.Height, d.Format, d.MipLevels);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        snprintf(t, sizeof t, "%p buffer %u bytes", r, d.ByteWidth);
    } else snprintf(t, sizeof t, "%p dim=%d", r, dim);
    r->Release();
    return t;
}

static void ProbeTaaPass(ID3D11DeviceContext* ctx, ShaderInfo* si, bool compute)
{
    int hit = ++si->hits;
    // Arm the consumer scan 120 frames after the TAA resolution settles (menus and gameplay can
    // render at different scales), once per resolution.
    if (g_cfg.verbose && !compute) {
        static UINT lastW, lastH; static int framesAtSize;
        ID3D11RenderTargetView* rtv = nullptr; ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* r = nullptr; rtv->GetResource(&r);
            D3D11_TEXTURE2D_DESC d;
            if (r && TexDesc(r, d)) {
                if (d.Width != lastW || d.Height != lastH) { lastW = d.Width; lastH = d.Height; framesAtSize = 0; }
                if (++framesAtSize == 120) {
                    g_taaW = d.Width; g_taaH = d.Height; g_taaOut = r; g_scanLogged = 0; g_consumerScan = 20000;
                    Log("Pass scan armed for TAA resolution %ux%u", d.Width, d.Height);
                }
            }
            SafeRelease(r); rtv->Release();
        }
    }
    if (hit > 3 && !(g_cfg.verbose && hit <= 8)) return;
    std::string out;
    char t[256];
    ID3D11DeviceContext1* ctx1 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1);
    for (auto& b : si->binds) {
        if (b.type == D3D_SIT_CBUFFER) {
            ID3D11Buffer* cb = nullptr; UINT first = 0, num = 0;
            if (ctx1) compute ? ctx1->CSGetConstantBuffers1(b.slot, 1, &cb, &first, &num) : ctx1->PSGetConstantBuffers1(b.slot, 1, &cb, &first, &num);
            else compute ? ctx->CSGetConstantBuffers(b.slot, 1, &cb) : ctx->PSGetConstantBuffers(b.slot, 1, &cb);
            snprintf(t, sizeof t, "\n    cb %s b%u = %p", b.name.c_str(), b.slot, cb); out += t;
            for (auto& rc : si->cbs) {
                if (rc.name != b.name || !cb) continue;
                D3D11_BUFFER_DESC bd; cb->GetDesc(&bd);
                if (bd.ByteWidth > 256) { AcquireSRWLockExclusive(&g_cbLock); g_cbWatchedBig[cb] = true; ReleaseSRWLockExclusive(&g_cbLock); }
                std::vector<uint8_t> data(std::min(rc.size, 4096u));
                if (!ReadCB(cb, first * 16, (UINT)data.size(), data.data())) { out += " (contents not captured yet)"; break; }
                for (auto& v : rc.vars) {
                    snprintf(t, sizeof t, "\n      %s =", v.name.c_str()); out += t;
                    for (UINT k = 0; k < std::min(v.size / 4, 8u) && v.offset + k * 4 + 4 <= data.size(); ++k) {
                        float f; memcpy(&f, data.data() + v.offset + k * 4, 4);
                        snprintf(t, sizeof t, " %.6g", f); out += t;
                    }
                }
            }
            SafeRelease(cb);
        } else if (b.type == D3D_SIT_SAMPLER) {
            continue;
        } else if (b.type == D3D_SIT_UAV_RWTYPED || (b.type >= D3D_SIT_UAV_RWSTRUCTURED && b.type != D3D_SIT_BYTEADDRESS)) {
            ID3D11UnorderedAccessView* uav = nullptr;
            if (compute) ctx->CSGetUnorderedAccessViews(b.slot, 1, &uav);
            snprintf(t, sizeof t, "\n    uav %s u%u = %s", b.name.c_str(), b.slot, DescribeView(uav).c_str()); out += t;
            SafeRelease(uav);
        } else {
            ID3D11ShaderResourceView* srv = nullptr;
            compute ? ctx->CSGetShaderResources(b.slot, 1, &srv) : ctx->PSGetShaderResources(b.slot, 1, &srv);
            snprintf(t, sizeof t, "\n    srv %s t%u = %s", b.name.c_str(), b.slot, DescribeView(srv).c_str()); out += t;
            SafeRelease(srv);
        }
    }
    SafeRelease(ctx1);
    if (!compute) {
        ID3D11RenderTargetView* rtv[4] = {};
        ctx->OMGetRenderTargets(4, rtv, nullptr);
        for (int i = 0; i < 4; ++i) if (rtv[i]) { snprintf(t, sizeof t, "\n    rtv%d = %s", i, DescribeView(rtv[i]).c_str()); out += t; rtv[i]->Release(); }
        D3D11_VIEWPORT vp; UINT n = 1; ctx->RSGetViewports(&n, &vp);
        if (n) { snprintf(t, sizeof t, "\n    viewport %gx%g at (%g,%g)", vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY); out += t; }
    }
    Log("TAA pass %s hit #%d:%s", compute ? "CS" : "PS", hit, out.c_str());
}

// ------------------------------------------------------------------------------------------
// D3D11 hooks
// The device vtable is static inside d3d11.dll (shared by every device) and is patched through a
// throwaway device. Context methods are detoured in d3d11.dll (see DetourContext) as soon as the
// game creates its device or touches a context (D3D11CreateDevice, GetImmediateContext*,
// CreateDeferredContext*, CreatePixelShader, NGX Init).
// ------------------------------------------------------------------------------------------
using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_CreateComputeShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
using PFN_CreateDeferredContext = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using PFN_GetImmediateContext = void(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11DeviceContext**);
using PFN_GetImmediateContextN = void(STDMETHODCALLTYPE*)(ID3D11Device*, void**);   // GetImmediateContext1/2/3
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
// CreateDeferredContext1/2/3 share this shape (the out pointer is an ID3D11DeviceContext1/2/3**).
using PFN_CreateDeferredContextN = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, void**);

static PFN_CreatePixelShader o_CreatePixelShader;
static PFN_CreateComputeShader o_CreateComputeShader;
static PFN_CreateDeferredContext o_CreateDeferredContext;
static PFN_CreateDeferredContextN o_CreateDeferredContext1, o_CreateDeferredContext2, o_CreateDeferredContext3;
static PFN_GetImmediateContext o_GetImmediateContext;
static PFN_GetImmediateContextN o_GetImmediateContext1, o_GetImmediateContext2, o_GetImmediateContext3;

// Trampolines to the original d3d11.dll context methods (filled by MinHook in DetourContext).
static struct CtxOrig {
    PFN_DrawIndexed drawIndexed; PFN_Draw draw; PFN_DrawIndexedInstanced drawIndexedInstanced; PFN_DrawInstanced drawInstanced; PFN_Map map; PFN_Unmap unmap;
    PFN_UpdateSubresource updateSubresource; PFN_ClearDSV clearDSV; PFN_Dispatch dispatch; PFN_ExecuteCommandList executeCommandList;
} g_orig;

static bool Contains(const void* data, SIZE_T len, const char* s)
{
    size_t n = strlen(s) + 1;   // include the terminator: names in RDEF are NUL terminated
    const char* p = (const char*)data;
    for (SIZE_T i = 0; i + n <= len; ++i) if (p[i] == s[0] && !memcmp(p + i, s, n)) return true;
    return false;
}

static void HookContext(ID3D11DeviceContext* ctx);

static HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11PixelShader** out)
{
    HRESULT hr = o_CreatePixelShader(dev, code, len, cl, out);
    if (SUCCEEDED(hr) && out) RememberShader(*out, code, len);
    if (SUCCEEDED(hr) && out && *out && code && len < 16384 &&
        Contains(code, len, "cbMotMat") && Contains(code, len, "g_jitterOfs") && Contains(code, len, "g_velocityTex")) {
        AddDecodePS(*out);
        if (g_cfg.verbose) DumpShader(code, len, "decode", *out);
        ID3D11DeviceContext* imm = nullptr;
        o_GetImmediateContext(dev, &imm);
        if (imm) { HookContext(imm); imm->Release(); }
    }
    if (SUCCEEDED(hr) && out && *out && code && Contains(code, len, "cbTemporalAA")) {
        RegisterTaaShader(*out, code, len, "PS");
        ID3D11DeviceContext* imm = nullptr;
        o_GetImmediateContext(dev, &imm);
        if (imm) { HookContext(imm); imm->Release(); }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateComputeShader(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11ComputeShader** out)
{
    HRESULT hr = o_CreateComputeShader(dev, code, len, cl, out);
    if (SUCCEEDED(hr) && out) RememberShader(*out, code, len);
    if (SUCCEEDED(hr) && out && *out && code && Contains(code, len, "cbTemporalAA")) {
        RegisterTaaShader(*out, code, len, "CS");
        ID3D11DeviceContext* imm = nullptr;
        o_GetImmediateContext(dev, &imm);
        if (imm) { HookContext(imm); imm->Release(); }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out)
{
    HRESULT hr = o_CreateDeferredContext(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookContext(*out);
    return hr;
}

static HRESULT DeferredContextN(PFN_CreateDeferredContextN orig, ID3D11Device* dev, UINT flags, void** out)
{
    HRESULT hr = orig(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookContext((ID3D11DeviceContext*)*out);   // ID3D11DeviceContextN derives from ID3D11DeviceContext
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext1(ID3D11Device* d, UINT f, void** o) { return DeferredContextN(o_CreateDeferredContext1, d, f, o); }
static HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext2(ID3D11Device* d, UINT f, void** o) { return DeferredContextN(o_CreateDeferredContext2, d, f, o); }
static HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext3(ID3D11Device* d, UINT f, void** o) { return DeferredContextN(o_CreateDeferredContext3, d, f, o); }

static void STDMETHODCALLTYPE Hk_GetImmediateContext1(ID3D11Device* d, void** o) { o_GetImmediateContext1(d, o); if (o && *o) HookContext((ID3D11DeviceContext*)*o); }
static void STDMETHODCALLTYPE Hk_GetImmediateContext2(ID3D11Device* d, void** o) { o_GetImmediateContext2(d, o); if (o && *o) HookContext((ID3D11DeviceContext*)*o); }
static void STDMETHODCALLTYPE Hk_GetImmediateContext3(ID3D11Device* d, void** o) { o_GetImmediateContext3(d, o); if (o && *o) HookContext((ID3D11DeviceContext*)*o); }

static void STDMETHODCALLTYPE Hk_GetImmediateContext(ID3D11Device* dev, ID3D11DeviceContext** out)
{
    o_GetImmediateContext(dev, out);
    if (out && *out) HookContext(*out);
}

static bool ReplaceTaa(ID3D11DeviceContext* ctx, ShaderInfo* si);
static bool ReplaceUpscale(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);

// Returns true when the mod produced the draw's output itself and the game's draw must be skipped.
static bool OnDraw(ID3D11DeviceContext* ctx)
{
    if (!g_cfg.enabled) return false;
    if (g_cfg.verbose) {   // liveness: shows whether draw hooks keep firing after the first frames
        static std::atomic<UINT64> draws{0}, last{0};
        UINT64 n = ++draws, now = GetTickCount64(), prev = last.load();
        if (now - prev >= 5000 && last.compare_exchange_strong(prev, now))
            Log("Draw hooks alive: %llu draws so far (ctx %p, vtable %p)", n, ctx, *(void***)ctx);
    }
    ID3D11PixelShader* ps = nullptr;
    bool haveDecode = g_decodePSCount.load() > 0;
    if (haveDecode || g_taaCount.load() > 0) {
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (ps) ps->Release();   // only compared by address; the context keeps it alive
        if (ps && haveDecode && IsDecodePS(ps)) { CaptureDecodePass(ctx); return false; }
        if (ps) if (ShaderInfo* si = FindTaaShader(ps)) { ProbeTaaPass(ctx, si, false); return ReplaceTaa(ctx, si); }
        if (ps && ReplaceUpscale(ctx, ps)) return true;
        if (ps) ScanTaaConsumer(ctx, ps, false);
        if (haveDecode) return false;
        ps = nullptr;
    }
    // Fallback when the shader was created before our hooks: the decode pass renders into the
    // texture the game later passes as "MotionVectors" (pooled, normally stable across frames).
    ID3D11Resource* mv = g_lastMvRes;
    if (!mv) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* r = nullptr; rtv->GetResource(&r); rtv->Release();
    if (r) r->Release();
    if (r != mv) return false;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) { AddDecodePS(ps); ps->Release(); CaptureDecodePass(ctx); }
    return false;
}

static void STDMETHODCALLTYPE Hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c) { if (!OnDraw(ctx)) g_orig.drawIndexed(ctx, a, b, c); }
static void STDMETHODCALLTYPE Hk_Draw(ID3D11DeviceContext* ctx, UINT a, UINT b) { if (!OnDraw(ctx)) g_orig.draw(ctx, a, b); }
static void STDMETHODCALLTYPE Hk_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e) { if (!OnDraw(ctx)) g_orig.drawIndexedInstanced(ctx, a, b, c, d, e); }
static void STDMETHODCALLTYPE Hk_DrawInstanced(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d) { if (!OnDraw(ctx)) g_orig.drawInstanced(ctx, a, b, c, d); }

static void STDMETHODCALLTYPE Hk_Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z)
{
    if (g_cfg.enabled && g_taaCount.load() > 0) {
        ID3D11ComputeShader* cs = nullptr;
        ctx->CSGetShader(&cs, nullptr, nullptr);
        if (cs) { cs->Release(); if (ShaderInfo* si = FindTaaShader(cs)) ProbeTaaPass(ctx, si, true); else ScanTaaConsumer(ctx, cs, true); }
    }
    g_orig.dispatch(ctx, x, y, z);
}

static bool ConstantBufferSize(ID3D11Resource* res, UINT& size)
{
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d; ((ID3D11Buffer*)res)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
    size = d.ByteWidth;
    return true;
}

static HRESULT STDMETHODCALLTYPE Hk_Map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
{
    HRESULT hr = g_orig.map(ctx, res, sub, type, flags, out);
    UINT size;
    if (SUCCEEDED(hr) && out && res && type != D3D11_MAP_READ && ConstantBufferSize(res, size)) {
        AcquireSRWLockExclusive(&g_cbLock);
        if (size <= 256) g_cbPending[res] = { out->pData, size };
        else if (g_cbWatchedBig.count(res)) g_cbLastPtr[res] = out->pData;
        ReleaseSRWLockExclusive(&g_cbLock);
    }
    return hr;
}

static void STDMETHODCALLTYPE Hk_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
{
    if (res) {
        AcquireSRWLockExclusive(&g_cbLock);
        auto it = g_cbPending.find(res);
        if (it != g_cbPending.end()) {
            auto& sh = g_cbShadow[res];
            sh.resize(it->second.size);
            memcpy(sh.data(), it->second.data, it->second.size);
            g_cbPending.erase(it);
        }
        ReleaseSRWLockExclusive(&g_cbLock);
    }
    g_orig.unmap(ctx, res, sub);
}

static void STDMETHODCALLTYPE Hk_UpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp)
{
    UINT size;
    if (res && src && ConstantBufferSize(res, size) && size <= 65536) {
        UINT lo = box ? box->left : 0, hi = box ? box->right : size;
        if (hi <= size && lo < hi) {
            AcquireSRWLockExclusive(&g_cbLock);
            auto& sh = g_cbShadow[res];
            sh.resize(size);
            memcpy(sh.data() + lo, src, hi - lo);
            ReleaseSRWLockExclusive(&g_cbLock);
        }
    }
    g_orig.updateSubresource(ctx, res, sub, box, src, rp, dp);
}

static void STDMETHODCALLTYPE Hk_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
{
    static std::atomic<UINT64> n{0};
    UINT64 i = ++n;
    if (i == 1 || i == 1000) Log("ExecuteCommandList #%llu: the game records rendering on deferred contexts", i);
    g_orig.executeCommandList(ctx, list, restore);
}

static void STDMETHODCALLTYPE Hk_ClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (dsv && (flags & D3D11_CLEAR_DEPTH)) {
        ID3D11Resource* r = nullptr; dsv->GetResource(&r);
        if (r) {
            AcquireSRWLockExclusive(&g_clearLock);
            if (g_depthClears.size() > 256) g_depthClears.clear();
            g_depthClears[r] = depth;
            ReleaseSRWLockExclusive(&g_clearLock);
            r->Release();
        }
    }
    g_orig.clearDSV(ctx, dsv, flags, depth, stencil);
}

static void* PatchSlot(void** vt, int idx, void* hook)
{
    void* prev = vt[idx];
    if (prev == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return prev;
}

static std::atomic<bool> g_d3dHooked{false};
static std::mutex g_hookMtx;

// Context methods are detoured in d3d11.dll itself: the runtime keeps the context vtable inside the
// context object and rewrites it while the game renders, so vtable patches only catch a fraction of
// the draws. One detour per method, taken from the first context seen (the game's device).
static struct CtxHook { int slot; void* hook; void** orig; const char* name; void* target; bool warned; } g_ctxHooks[] = {
    { 12, (void*)Hk_DrawIndexed, (void**)&g_orig.drawIndexed, "DrawIndexed" },
    { 13, (void*)Hk_Draw, (void**)&g_orig.draw, "Draw" },
    { 14, (void*)Hk_Map, (void**)&g_orig.map, "Map" },
    { 15, (void*)Hk_Unmap, (void**)&g_orig.unmap, "Unmap" },
    { 20, (void*)Hk_DrawIndexedInstanced, (void**)&g_orig.drawIndexedInstanced, "DrawIndexedInstanced" },
    { 21, (void*)Hk_DrawInstanced, (void**)&g_orig.drawInstanced, "DrawInstanced" },
    { 41, (void*)Hk_Dispatch, (void**)&g_orig.dispatch, "Dispatch" },
    { 48, (void*)Hk_UpdateSubresource, (void**)&g_orig.updateSubresource, "UpdateSubresource" },
    { 53, (void*)Hk_ClearDSV, (void**)&g_orig.clearDSV, "ClearDepthStencilView" },
    { 58, (void*)Hk_ExecuteCommandList, (void**)&g_orig.executeCommandList, "ExecuteCommandList" },
};

static void DetourContext(ID3D11DeviceContext* ctx)
{
    void** vt = *(void***)ctx;
    std::lock_guard<std::mutex> lk(g_hookMtx);
    if (!MinHookReady()) { static bool logged; if (!logged) { logged = true; Log("ERROR: MinHook initialisation failed"); } return; }
    int added = 0;
    for (auto& h : g_ctxHooks) {
        void* fn = vt[h.slot];
        if (h.target) {
            if (fn != h.target && !h.warned) { h.warned = true; Log("Context %p: %s points to %p, not the detoured %p (calls through it are not seen)", ctx, h.name, fn, h.target); }
            continue;
        }
        MH_STATUS st = MH_CreateHook(fn, h.hook, h.orig);
        if (st == MH_OK) st = MH_EnableHook(fn);
        if (st != MH_OK) { Log("Detour of %s at %p failed: %s", h.name, fn, MH_StatusToString(st)); continue; }
        h.target = fn;
        ++added;
    }
    if (added) Log("Detoured %d context methods (%s context %p, vtable %p)", added,
                   ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred", ctx, vt);
}

// The ID3D11DeviceContext1/2/3 interfaces of a context can carry their own tables; check them all.
static void HookContext(ID3D11DeviceContext* ctx)
{
    DetourContext(ctx);
    const IID* iids[] = { &__uuidof(ID3D11DeviceContext1), &__uuidof(ID3D11DeviceContext2), &__uuidof(ID3D11DeviceContext3) };
    for (const IID* iid : iids) {
        ID3D11DeviceContext* c = nullptr;
        if (FAILED(ctx->QueryInterface(*iid, (void**)&c)) || !c) continue;
        if (*(void***)c != *(void***)ctx) DetourContext(c);
        c->Release();
    }
}

static void InstallD3DHooks(ID3D11Device* dev)
{
    std::lock_guard<std::mutex> lk(g_hookMtx);
    if (g_d3dHooked) return;
    void** vt = *(void***)dev;
    if (void* p = PatchSlot(vt, 15, (void*)Hk_CreatePixelShader)) o_CreatePixelShader = (PFN_CreatePixelShader)p;
    if (void* p = PatchSlot(vt, 18, (void*)Hk_CreateComputeShader)) o_CreateComputeShader = (PFN_CreateComputeShader)p;
    if (void* p = PatchSlot(vt, 27, (void*)Hk_CreateDeferredContext)) o_CreateDeferredContext = (PFN_CreateDeferredContext)p;
    // ID3D11Device1..3 extend the same vtable; only patch the slots the device really has.
    auto has = [&](REFIID iid) {
        IUnknown* u = nullptr;
        if (FAILED(dev->QueryInterface(iid, (void**)&u))) return false;
        bool same = *(void***)u == vt; u->Release(); return same;
    };
    if (has(__uuidof(ID3D11Device1))) {
        if (void* p = PatchSlot(vt, 43, (void*)Hk_GetImmediateContext1)) o_GetImmediateContext1 = (PFN_GetImmediateContextN)p;
        if (void* p = PatchSlot(vt, 44, (void*)Hk_CreateDeferredContext1)) o_CreateDeferredContext1 = (PFN_CreateDeferredContextN)p;
    }
    if (has(__uuidof(ID3D11Device2))) {
        if (void* p = PatchSlot(vt, 50, (void*)Hk_GetImmediateContext2)) o_GetImmediateContext2 = (PFN_GetImmediateContextN)p;
        if (void* p = PatchSlot(vt, 51, (void*)Hk_CreateDeferredContext2)) o_CreateDeferredContext2 = (PFN_CreateDeferredContextN)p;
    }
    if (has(__uuidof(ID3D11Device3))) {
        if (void* p = PatchSlot(vt, 61, (void*)Hk_GetImmediateContext3)) o_GetImmediateContext3 = (PFN_GetImmediateContextN)p;
        if (void* p = PatchSlot(vt, 62, (void*)Hk_CreateDeferredContext3)) o_CreateDeferredContext3 = (PFN_CreateDeferredContextN)p;
    }
    if (void* p = PatchSlot(vt, 40, (void*)Hk_GetImmediateContext)) o_GetImmediateContext = (PFN_GetImmediateContext)p;
    g_d3dHooked = true;
    Log("D3D11 device hooks installed (device vtable %p)", vt);
}

static DWORD WINAPI HookThread(void*)
{
    // Wait for d3d11.dll and hook the shared device vtable through a throwaway device,
    // ideally before the game creates its device and shaders.
    for (int i = 0; i < 120000 && !g_d3dHooked; ++i) {
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m) {
            auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(m, "D3D11CreateDevice");
            ID3D11Device* dev = nullptr;
            if (create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr))) {
                InstallD3DHooks(dev);
                dev->Release();
            }
            break;
        }
        Sleep(1);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// GPU helpers. Work done by the mod (blits, DLSS) saves and restores the pipeline state it can
// touch, so the game's state is untouched. (SwapDeviceContextState is not usable: the runtime
// rewrites the context vtable afterwards and would silently remove our hooks.)
// ------------------------------------------------------------------------------------------
static ID3D11Device* g_dev;
static ID3D11VertexShader* g_blitVS;
static ID3D11PixelShader* g_blitPS;
static ID3D11SamplerState* g_sampPoint;
static ID3D11SamplerState* g_sampLinear;
static ID3D11PixelShader* g_mvPS;
static ID3D11Buffer* g_mvCB;

static const char* kBlitHLSL = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return src.SampleLevel(samp, uv, 0);
}

// Motion vectors (previous - current, in UV) rebuilt the way FFXV's TemporalAA pass reprojects:
// g_velocityTex holds the UV delta of moving objects (y == 1 marks "static, use the camera",
// |x| >= 4 carries a flag that is stripped); otherwise the camera reprojection
// prev = (g_motionMatrix * (uv, depth, 1)).xy / w is used.
cbuffer cbMV : register(b0) { float4 mcol0; float4 mcol1; float4 mcol3; float4 invSize; };
Texture2D depthTex : register(t0);
Texture2D velTex : register(t1);
float2 psmv(float4 pos : SV_Position) : SV_Target
{
    int3 p = int3(pos.xy, 0);
    float2 uv = pos.xy * invSize.xy;
    float2 v = velTex.Load(p).xy;
    if (v.y != 1.0) {
        if (abs(v.x) >= 4.0) v.x -= 4.0 * sign(v.x);
        return v;
    }
    float4 q = float4(uv, depthTex.Load(p).r, 1.0);
    return float2(dot(mcol0, q), dot(mcol1, q)) / dot(mcol3, q) - uv;
}
)";

static bool InitGpuHelpers()
{
    if (g_blitPS) return true;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) {
        Log("Blit shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(vs); SafeRelease(ps); SafeRelease(err); return false;
    }
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_blitVS);
    g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_blitPS);
    vs->Release(); ps->Release();
    ID3DBlob* mv = nullptr;
    if (SUCCEEDED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), "mv", nullptr, nullptr, "psmv", "ps_5_0", 0, 0, &mv, &err))) {
        g_dev->CreatePixelShader(mv->GetBufferPointer(), mv->GetBufferSize(), nullptr, &g_mvPS);
        mv->Release();
    } else {
        Log("Motion-vector shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
    }
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 64; cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, nullptr, &g_mvCB);
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  g_dev->CreateSamplerState(&sd, &g_sampPoint);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; g_dev->CreateSamplerState(&sd, &g_sampLinear);
    return g_blitVS && g_blitPS && g_sampPoint && g_sampLinear;
}

// Snapshot of the D3D11 pipeline bindings the mod or NGX may change.
struct StateBlock {
    static const UINT kSrv = 16, kCb = 14, kSmp = 16, kUav = 8;
    ID3D11InputLayout* layout = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN; UINT ibOffset = 0;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11ComputeShader* cs = nullptr;
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
    ID3D11Buffer* vsCb[kCb] = {}; ID3D11Buffer* psCb[kCb] = {}; ID3D11Buffer* csCb[kCb] = {};
    ID3D11ShaderResourceView* vsSrv[kSrv] = {}; ID3D11ShaderResourceView* psSrv[kSrv] = {}; ID3D11ShaderResourceView* csSrv[kSrv] = {};
    ID3D11SamplerState* vsSmp[kSmp] = {}; ID3D11SamplerState* psSmp[kSmp] = {}; ID3D11SamplerState* csSmp[kSmp] = {};
    ID3D11UnorderedAccessView* csUav[kUav] = {};
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4] = {}; UINT sampleMask = 0;
    ID3D11DepthStencilState* depthState = nullptr; UINT stencilRef = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT vpCount = 0;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT scCount = 0;

    void Capture(ID3D11DeviceContext* c)
    {
        c->IAGetInputLayout(&layout); c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset); c->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->CSGetShader(&cs, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr); c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
        c->VSGetConstantBuffers(0, kCb, vsCb); c->PSGetConstantBuffers(0, kCb, psCb); c->CSGetConstantBuffers(0, kCb, csCb);
        c->VSGetShaderResources(0, kSrv, vsSrv); c->PSGetShaderResources(0, kSrv, psSrv); c->CSGetShaderResources(0, kSrv, csSrv);
        c->VSGetSamplers(0, kSmp, vsSmp); c->PSGetSamplers(0, kSmp, psSmp); c->CSGetSamplers(0, kSmp, csSmp);
        c->CSGetUnorderedAccessViews(0, kUav, csUav);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask); c->OMGetDepthStencilState(&depthState, &stencilRef);
        c->RSGetState(&rs);
        vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&vpCount, vp);
        scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetScissorRects(&scCount, sc);
    }

    void Apply(ID3D11DeviceContext* c)
    {
        c->IASetInputLayout(layout); c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset); c->IASetIndexBuffer(ib, ibFmt, ibOffset);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->CSSetShader(cs, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0); c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0);
        c->VSSetConstantBuffers(0, kCb, vsCb); c->PSSetConstantBuffers(0, kCb, psCb); c->CSSetConstantBuffers(0, kCb, csCb);
        // Unbind UAV/RT outputs first so restoring SRVs that alias them is not refused.
        ID3D11UnorderedAccessView* nullUav[kUav] = {};
        c->CSSetUnorderedAccessViews(0, kUav, nullUav, nullptr);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->VSSetShaderResources(0, kSrv, vsSrv); c->PSSetShaderResources(0, kSrv, psSrv); c->CSSetShaderResources(0, kSrv, csSrv);
        c->VSSetSamplers(0, kSmp, vsSmp); c->PSSetSamplers(0, kSmp, psSmp); c->CSSetSamplers(0, kSmp, csSmp);
        c->CSSetUnorderedAccessViews(0, kUav, csUav, nullptr);
        c->OMSetBlendState(blend, blendFactor, sampleMask); c->OMSetDepthStencilState(depthState, stencilRef);
        c->RSSetState(rs);
        c->RSSetViewports(vpCount, vp); c->RSSetScissorRects(scCount, sc);
    }

    ~StateBlock()
    {
        SafeRelease(layout); SafeRelease(vb); SafeRelease(ib);
        SafeRelease(vs); SafeRelease(ps); SafeRelease(cs); SafeRelease(gs); SafeRelease(hs); SafeRelease(ds);
        for (auto& p : vsCb) SafeRelease(p); for (auto& p : psCb) SafeRelease(p); for (auto& p : csCb) SafeRelease(p);
        for (auto& p : vsSrv) SafeRelease(p); for (auto& p : psSrv) SafeRelease(p); for (auto& p : csSrv) SafeRelease(p);
        for (auto& p : vsSmp) SafeRelease(p); for (auto& p : psSmp) SafeRelease(p); for (auto& p : csSmp) SafeRelease(p);
        for (auto& p : csUav) SafeRelease(p);
        for (auto& p : rtv) SafeRelease(p); SafeRelease(dsv);
        SafeRelease(blend); SafeRelease(depthState); SafeRelease(rs);
    }
};

// Runs fn and restores the game's pipeline state afterwards.
template <class F> static void Isolated(ID3D11DeviceContext* ctx, F fn)
{
    StateBlock sb;
    sb.Capture(ctx);
    fn();
    sb.Apply(ctx);
}

static void Blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT w, UINT h, bool linear)
{
    Isolated(ctx, [&] {
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);   // src may still be bound as an output elsewhere
        ctx->OMSetRenderTargets(1, &dst, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_blitVS, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g_blitPS, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &src);
        ctx->PSSetSamplers(0, 1, linear ? &g_sampLinear : &g_sampPoint);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSrv);
    });
}

// A render target + SRV (+UAV) texture owned by the mod.
struct OwnTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; UINT bind = 0;
    void Release() { SafeRelease(rtv); SafeRelease(srv); SafeRelease(tex); w = h = 0; }
    bool Ensure(UINT W, UINT H, DXGI_FORMAT F, UINT B)
    {
        if (tex && W == w && H == h && F == fmt && B == bind) return true;
        Release();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = F;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = B;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) { Log("CreateTexture2D %ux%u fmt %d failed", W, H, F); return false; }
        if (B & D3D11_BIND_RENDER_TARGET) g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
        if (B & D3D11_BIND_SHADER_RESOURCE) g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        w = W; h = H; fmt = F; bind = B;
        return true;
    }
};

static DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
    }
}

static DXGI_FORMAT FormatFamily(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default: return f;
    }
}

static bool TexDesc(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d)
{
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ((ID3D11Texture2D*)r)->GetDesc(&d);
    return true;
}

// Views on game textures, cached per resource (the game recycles pooled textures).
static std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> g_srvCache;
static std::unordered_map<ID3D11Resource*, ID3D11RenderTargetView*> g_rtvCache;

static ID3D11ShaderResourceView* GameSRV(ID3D11Resource* r)
{
    auto it = g_srvCache.find(r);
    if (it != g_srvCache.end()) return it->second;
    D3D11_TEXTURE2D_DESC d;
    if (!TexDesc(r, d) || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = TypedFormat(d.Format);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(g_dev->CreateShaderResourceView(r, &sd, &v))) return nullptr;
    if (g_srvCache.size() > 64) { for (auto& kv : g_srvCache) kv.second->Release(); g_srvCache.clear(); }
    return g_srvCache[r] = v;
}

static ID3D11RenderTargetView* GameRTV(ID3D11Resource* r)
{
    auto it = g_rtvCache.find(r);
    if (it != g_rtvCache.end()) return it->second;
    D3D11_TEXTURE2D_DESC d;
    if (!TexDesc(r, d) || !(d.BindFlags & D3D11_BIND_RENDER_TARGET)) return nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = TypedFormat(d.Format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* v = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(r, &rd, &v))) return nullptr;
    if (g_rtvCache.size() > 64) { for (auto& kv : g_rtvCache) kv.second->Release(); g_rtvCache.clear(); }
    return g_rtvCache[r] = v;
}

// Copies src into dst: CopyResource when layouts are compatible, otherwise a (scaling) blit.
static bool CopyTex(ID3D11DeviceContext* ctx, ID3D11Resource* src, ID3D11Resource* dst)
{
    D3D11_TEXTURE2D_DESC s, d;
    if (!TexDesc(src, s) || !TexDesc(dst, d)) return false;
    if (s.Width == d.Width && s.Height == d.Height && s.SampleDesc.Count == d.SampleDesc.Count &&
        s.MipLevels == d.MipLevels && s.ArraySize == d.ArraySize && FormatFamily(s.Format) == FormatFamily(d.Format)) {
        ctx->CopyResource(dst, src);
        return true;
    }
    ID3D11ShaderResourceView* srv = GameSRV(src);
    ID3D11RenderTargetView* rtv = GameRTV(dst);
    if (!srv || !rtv) return false;
    Blit(ctx, srv, rtv, d.Width, d.Height, s.Width != d.Width || s.Height != d.Height);
    return true;
}

// ------------------------------------------------------------------------------------------
// DLSS (current SDK)
// ------------------------------------------------------------------------------------------
static bool g_ngxReady;
static ID3D11Device* g_ngxDev;   // device NGX was initialised on
static NVSDK_NGX_Parameter* g_ngxCaps;

struct DlssInstance {
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    UINT inW = 0, inH = 0, outW = 0, outH = 0; int flags = 0, preset = 0;
    UINT64 evals = 0;   // evaluations since creation (verbose log)
    void Release()
    {
        if (handle) NVSDK_NGX_D3D11_ReleaseFeature(handle);
        if (params) NVSDK_NGX_D3D11_DestroyParameters(params);
        handle = nullptr; params = nullptr;
    }
};
static DlssInstance g_dlss;

static NVSDK_NGX_PerfQuality_Value QualityForRatio(UINT inW, UINT outW)
{
    float r = (float)inW / (float)outW;
    if (r >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (r >= 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (r >= 0.54f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (r >= 0.42f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

static void LogDlssVersion()
{
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    wchar_t path[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, path, MAX_PATH);
    DWORD h = 0, sz = m ? GetFileVersionInfoSizeW(path, &h) : 0;
    if (sz) {
        std::vector<uint8_t> buf(sz);
        VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
        if (GetFileVersionInfoW(path, 0, sz, buf.data()) && VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi) {
            Log("DLSS module: %ls  version %u.%u.%u.%u", path, HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            return;
        }
    }
    Log("DLSS module: %ls", path);
}

static bool EnsureDlss(ID3D11DeviceContext* ctx, UINT inW, UINT inH, UINT outW, UINT outH, int flags, int preset)
{
    if (g_dlss.handle && g_dlss.inW == inW && g_dlss.inH == inH && g_dlss.outW == outW && g_dlss.outH == outH && g_dlss.flags == flags &&
        g_dlss.preset == preset)
        return true;
    g_dlss.Release();
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&g_dlss.params))) return false;
    const char* presetKeys[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
    for (const char* k : presetKeys) NVSDK_NGX_Parameter_SetUI(g_dlss.params, k, (unsigned)preset);

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = inW; cp.Feature.InHeight = inH;
    cp.Feature.InTargetWidth = outW; cp.Feature.InTargetHeight = outH;
    cp.Feature.InPerfQualityValue = QualityForRatio(inW, outW);
    cp.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r;
    Isolated(ctx, [&] { r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_dlss.handle, g_dlss.params, &cp); });
    if (NVSDK_NGX_FAILED(r)) {
        Log("DLSS create failed 0x%08X (%ux%u -> %ux%u flags 0x%X)", r, inW, inH, outW, outH, flags);
        g_dlss.Release();
        return false;
    }
    g_dlss.inW = inW; g_dlss.inH = inH; g_dlss.outW = outW; g_dlss.outH = outH; g_dlss.flags = flags; g_dlss.preset = preset;
    g_dlss.evals = 0;
    Log("DLSS created: %ux%u -> %ux%u quality=%d flags=0x%X preset=%d", inW, inH, outW, outH, cp.Feature.InPerfQualityValue, flags, preset);
    LogDlssVersion();
    return true;
}

// ------------------------------------------------------------------------------------------
// Jitter reconstruction from cbMotMat.g_jitterOfs
// The decode shader does  mv_uv += 0.5 * g_jitterOfs.xy , i.e. 0.5*xy is a jitter term in UV.
// Depending on the engine it is either the absolute jitter of the frame or the change since
// the previous frame. "Auto" decides from the magnitude (absolute jitter never exceeds 0.5px).
// ------------------------------------------------------------------------------------------
static struct JitterState {
    bool decided = false; JitterMode mode = JitterMode::Absolute;
    int samples = 0; float maxAbs = 0;
    float accX = 0, accY = 0;
    float histX[64] = {}, histY[64] = {}; int histN = 0, histI = 0;
} g_jit;

static void ComputeJitter(UINT w, UINT h, float& outX, float& outY)
{
    outX = outY = 0;
    float raw[4]; bool valid;
    AcquireSRWLockShared(&g_captureLock);
    memcpy(raw, g_cap.jitterOfs, sizeof(raw)); valid = g_cap.jitterValid;
    ReleaseSRWLockShared(&g_captureLock);
    if (!valid || g_cfg.jitter == JitterMode::Off) return;

    float px = 0.5f * raw[0] * (float)w, py = 0.5f * raw[1] * (float)h;   // jitter term in render pixels
    JitterMode mode = g_cfg.jitter;
    if (mode == JitterMode::Auto) {
        if (!g_jit.decided) {
            g_jit.maxAbs = std::max(g_jit.maxAbs, std::max(fabsf(px), fabsf(py)));
            if (++g_jit.samples >= 32) {
                g_jit.decided = true;
                g_jit.mode = g_jit.maxAbs > 0.55f ? JitterMode::Delta : JitterMode::Absolute;
                Log("Jitter auto-detect: max |term| = %.3f px -> %s", g_jit.maxAbs, g_jit.mode == JitterMode::Delta ? "Delta" : "Absolute");
                if (g_jit.maxAbs == 0.f)
                    Log("WARNING: g_jitterOfs is always zero. DLSS gets no sub-pixel jitter and cannot anti-alias edges "
                        "(most visible below 4K). Run with VerboseLog=1 standing still to inspect the motion matrix.");
            }
        }
        mode = g_jit.mode;
    }
    if (mode == JitterMode::Delta) {
        // Integrate the per-frame change and remove the running mean (jitter sequences are zero-mean).
        g_jit.accX += px; g_jit.accY += py;
        g_jit.histX[g_jit.histI] = g_jit.accX; g_jit.histY[g_jit.histI] = g_jit.accY;
        g_jit.histI = (g_jit.histI + 1) % 64; g_jit.histN = std::min(g_jit.histN + 1, 64);
        float mx = 0, my = 0;
        for (int i = 0; i < g_jit.histN; ++i) { mx += g_jit.histX[i]; my += g_jit.histY[i]; }
        px = g_jit.accX - mx / g_jit.histN; py = g_jit.accY - my / g_jit.histN;
    }
    outX = px * g_cfg.jitterSignX;
    outY = py * g_cfg.jitterSignY;
}

// ------------------------------------------------------------------------------------------
// NGX r1.2 parameter object handed to the game
// ------------------------------------------------------------------------------------------
struct ShimParams final : NVSDK_NGX_Parameter {
    struct V { enum T { U64, F32, F64, U32, I32, PTR } t; union { unsigned long long u64; float f; double d; unsigned u32; int i; void* p; }; };
    std::unordered_map<std::string, V> m;

    ShimParams() { Reset(); }
    template <class X> void put(const char* n, V::T t, X x) { V v; v.t = t; v.u64 = 0; memcpy(&v.u64, &x, sizeof(X)); m[n] = v; }
    void Set(const char* n, unsigned long long v) override { put(n, V::U64, v); }
    void Set(const char* n, float v) override { put(n, V::F32, v); }
    void Set(const char* n, double v) override { put(n, V::F64, v); }
    void Set(const char* n, unsigned int v) override { put(n, V::U32, v); }
    void Set(const char* n, int v) override { put(n, V::I32, v); }
    void Set(const char* n, ID3D11Resource* v) override { put(n, V::PTR, (void*)v); }
    void Set(const char* n, ID3D12Resource* v) override { put(n, V::PTR, (void*)v); }
    void Set(const char* n, void* v) override { put(n, V::PTR, v); }

    const V* find(const char* n) const { auto it = m.find(n); return it == m.end() ? nullptr : &it->second; }
    double num(const V& v) const
    {
        switch (v.t) { case V::U64: return (double)v.u64; case V::F32: return v.f; case V::F64: return v.d;
                       case V::U32: return v.u32; case V::I32: return v.i; default: return 0; }
    }
    template <class X> NVSDK_NGX_Result getNum(const char* n, X* o) const
    {
        const V* v = find(n); if (!v || !o || v->t == V::PTR) return NVSDK_NGX_Result_Fail;
        *o = (X)num(*v); return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override { return getNum(n, o); }
    NVSDK_NGX_Result Get(const char* n, float* o) const override { return getNum(n, o); }
    NVSDK_NGX_Result Get(const char* n, double* o) const override { return getNum(n, o); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override { return getNum(n, o); }
    NVSDK_NGX_Result Get(const char* n, int* o) const override { return getNum(n, o); }
    NVSDK_NGX_Result getPtr(const char* n, void** o) const
    {
        const V* v = find(n); if (!v || !o || v->t != V::PTR) return NVSDK_NGX_Result_Fail;
        *o = v->p; return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override { return getPtr(n, (void**)o); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override { return getPtr(n, (void**)o); }
    NVSDK_NGX_Result Get(const char* n, void** o) const override { return getPtr(n, o); }
    void Reset() override
    {
        m.clear();
        Set("#\x01", 1);   // r1.2 availability of feature 1 (temporal)
        Set("#\x09", 1);   // r1.2 availability of feature 9 (dlisp upscaler)
    }

    ID3D11Resource* res(const char* n) const { void* p = nullptr; getPtr(n, &p); return (ID3D11Resource*)p; }
    int i(const char* n, int def = 0) const { int v = def; getNum(n, &v); return v; }
    float f(const char* n, float def = 0) const { float v = def; getNum(n, &v); return v; }
};

struct ShimHandle { unsigned int id; int feature; UINT w, hgt, outW, outH; };   // layout-compatible with NVSDK_NGX_Handle { unsigned int Id; }
static int g_liveSpatial;

static struct FrameInputs {
    OwnTex depth, mv;          // snapshots taken at the temporal stage
    bool haveDepth = false, haveMv = false;
    float mvScaleX = 1, mvScaleY = 1;
    int reset = 1;
    UINT renderW = 0, renderH = 0;
} g_fi;

// Reversed-Z is read from the game's last clear of the depth texture the inputs come from.
static int DepthInvertedFor(ID3D11ShaderResourceView* depthSrv)
{
    if (g_cfg.depthInverted >= 0) return g_cfg.depthInverted;
    ID3D11Resource* r = nullptr;
    if (depthSrv) depthSrv->GetResource(&r);
    int inv = -1;
    if (r) {
        AcquireSRWLockShared(&g_clearLock);
        auto it = g_depthClears.find(r);
        if (it != g_depthClears.end()) inv = it->second < 0.5f ? 1 : 0;
        ReleaseSRWLockShared(&g_clearLock);
        r->Release();
    }
    static int logged = -2;
    if (inv != logged) { Log("Depth inverted: %s", inv < 0 ? "unknown (assuming yes)" : inv ? "yes (cleared to 0)" : "no (cleared to 1)"); logged = inv; }
    return inv < 0 ? 1 : inv;
}

static int DepthInvertedFlag()
{
    ID3D11ShaderResourceView* srv = nullptr;
    AcquireSRWLockShared(&g_captureLock);
    srv = g_cap.depthSrv; if (srv) srv->AddRef();
    ReleaseSRWLockShared(&g_captureLock);
    int inv = DepthInvertedFor(srv);
    SafeRelease(srv);
    return inv;
}

// Snapshot depth (as R32F) and motion vectors (as RG16F) before the game recycles them.
static void SnapshotInputs(ID3D11DeviceContext* ctx, ID3D11Resource* mvRes, UINT w, UINT h)
{
    ID3D11ShaderResourceView* depthSrv = nullptr;
    AcquireSRWLockShared(&g_captureLock);
    depthSrv = g_cap.depthSrv; if (depthSrv) depthSrv->AddRef();
    ReleaseSRWLockShared(&g_captureLock);

    g_fi.haveDepth = false;
    if (depthSrv && g_fi.depth.Ensure(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) {
        Blit(ctx, depthSrv, g_fi.depth.rtv, w, h, false);
        g_fi.haveDepth = true;
    }
    SafeRelease(depthSrv);

    g_fi.haveMv = false;
    D3D11_TEXTURE2D_DESC md;
    ID3D11ShaderResourceView* mvSrv = mvRes ? GameSRV(mvRes) : nullptr;
    if (mvSrv && TexDesc(mvRes, md) && g_fi.mv.Ensure(md.Width, md.Height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) {
        Blit(ctx, mvSrv, g_fi.mv.rtv, md.Width, md.Height, false);
        g_fi.haveMv = true;
    }
}

// Fallback inputs when the decode pass was not observed: flat depth, zero motion.
static void FallbackInputs(ID3D11DeviceContext* ctx, UINT w, UINT h)
{
    const float zero[4] = {};
    if (!g_fi.haveDepth && g_fi.depth.Ensure(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        ctx->ClearRenderTargetView(g_fi.depth.rtv, zero);
    if (!g_fi.haveMv && g_fi.mv.Ensure(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        ctx->ClearRenderTargetView(g_fi.mv.rtv, zero);
}

static OwnTex g_outTmp;

// One DLSS evaluation with the depth / motion vectors staged in g_fi.
static bool EvaluateDlss(ID3D11DeviceContext* ctx, ID3D11Resource* color, ID3D11Resource* output, UINT inW, UINT inH,
                         int flags, int preset, float jitterX, float jitterY, float mvScaleX, float mvScaleY)
{
    D3D11_TEXTURE2D_DESC od;
    if (!TexDesc(output, od)) return false;
    if (g_cfg.mvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (!EnsureDlss(ctx, inW, inH, od.Width, od.Height, flags, preset)) return false;

    // DLSS writes through a UAV; route through an own texture when the game's target has none.
    ID3D11Resource* dst = output;
    if (!(od.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        if (!g_outTmp.Ensure(od.Width, od.Height, TypedFormat(od.Format), D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE)) return false;
        dst = g_outTmp.tex;
    }

    FallbackInputs(ctx, inW, inH);
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = dst;
    ep.pInDepth = g_fi.depth.tex;
    ep.pInMotionVectors = g_fi.mv.tex;
    ep.InJitterOffsetX = jitterX;
    ep.InJitterOffsetY = jitterY;
    ep.InRenderSubrectDimensions = { inW, inH };
    ep.InReset = g_fi.reset;
    ep.InMVScaleX = mvScaleX;
    ep.InMVScaleY = mvScaleY;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    NVSDK_NGX_Result r;
    Isolated(ctx, [&] { r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_dlss.handle, g_dlss.params, &ep); });
    UINT64 evals = ++g_dlss.evals;
    if (NVSDK_NGX_FAILED(r)) { Log("DLSS evaluate failed 0x%08X", r); return false; }
    if (evals <= 2 || (g_cfg.verbose && evals <= 400))
        Log("DLSS eval #%llu: %ux%u -> %ux%u jitter=(%.4f, %.4f) mvScale=(%g, %g) reset=%d depth=%d mv=%d",
            evals, inW, inH, od.Width, od.Height, ep.InJitterOffsetX, ep.InJitterOffsetY, ep.InMVScaleX, ep.InMVScaleY,
            ep.InReset, g_fi.haveDepth, g_fi.haveMv);
    if (dst != output) ctx->CopyResource(output, dst);
    g_fi.reset = 0;
    return true;
}

// DLSS for the game's own NGX path (inputs captured at the DLSS 1 temporal stage).
static bool RunDlss(ID3D11DeviceContext* ctx, ID3D11Resource* color, ID3D11Resource* output, UINT inW, UINT inH, bool hdr)
{
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (DepthInvertedFlag()) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    float jx, jy;
    ComputeJitter(inW, inH, jx, jy);
    return EvaluateDlss(ctx, color, output, inW, inH, flags, g_cfg.preset, jx, jy,
                        g_fi.mvScaleX * g_cfg.mvScaleX, g_fi.mvScaleY * g_cfg.mvScaleY);
}

// ------------------------------------------------------------------------------------------
// DLSS in place of the game's TemporalAA pass (any resolution, the game's own TAA jitter).
// Inputs come from the pass's bindings: jittered colour (g_jitterTex), depth, velocity and
// cbTemporalAA. DLSS writes the pass's output (render target 0) and the game's TAA draw is
// skipped. Anything missing falls back to the game's TAA.
// ------------------------------------------------------------------------------------------
static bool DrawMotionVectors(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* vel,
                              const float* m, UINT w, UINT h)
{
    if (!g_mvPS || !g_mvCB || !g_fi.mv.Ensure(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        return false;
    // The game's shader reads the matrix column-wise: prev.x = dot(column 0, (uv, depth, 1)), etc.
    float c[16] = { m[0], m[4], m[8], m[12],  m[1], m[5], m[9], m[13],  m[3], m[7], m[11], m[15],
                    1.f / w, 1.f / h, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(g_mvCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    memcpy(map.pData, c, sizeof(c));
    ctx->Unmap(g_mvCB, 0);
    Isolated(ctx, [&] {
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11ShaderResourceView* srv[2] = { depth, vel };
        ctx->PSSetShaderResources(0, 2, nullSrv);
        ctx->OMSetRenderTargets(1, &g_fi.mv.rtv, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_blitVS, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g_mvPS, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_mvCB);
        ctx->PSSetShaderResources(0, 2, srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSrv);
    });
    return true;
}

// Below 100 % resolution scale the game upscales the post-processed frame to display size with
// a plain textured draw. In that case DLSS runs there (render -> display resolution, like the
// game's own DLSS 1 path) and the TAA pass only stages its inputs and passes the jittered frame
// through. At 100 % there is no such draw and DLSS runs at the TAA pass as DLAA.
static struct TaaUpscale {
    UINT64 frame = 0;                // TAA passes seen
    UINT64 lastUpscaleFrame = 0;     // frame in which the upscale draw was last seen
    std::atomic<ID3D11PixelShader*> ps{nullptr};   // the game's upscale pixel shader, once identified
    int searchDraws = 0;             // draws left to look for it after the TAA pass
    bool staged = false;             // this frame's TAA pass passed through and staged DLSS inputs
    bool upscaling = false;          // current mode (for reset and logging)
    UINT w = 0, h = 0; int flags = 0; float jitterX = 0, jitterY = 0;
} g_up;

static bool ReplaceTaa(ID3D11DeviceContext* ctx, ShaderInfo* si)
{
    if (!g_cfg.replaceTaa || !g_ngxReady || !g_blitPS) return false;
    ++g_up.frame;
    g_up.staged = false;
    g_up.searchDraws = g_up.ps.load() ? 0 : 4000;
    if (si->colorSlot < 0 || si->depthSlot < 0 || si->velSlot < 0 || si->cbSlot < 0 ||
        si->screenSizeOfs < 0 || si->jitterOfs < 0 || si->motionOfs < 0) {
        static bool logged;
        if (!logged) { logged = true; Log("TAA replacement unavailable: the TAA shader does not expose the expected inputs"); }
        return false;
    }

    // cbTemporalAA contents (screen size, jitter, reprojection matrix).
    ID3D11Buffer* cb = nullptr; UINT first = 0, num = 0;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1))) { ctx1->PSGetConstantBuffers1(si->cbSlot, 1, &cb, &first, &num); ctx1->Release(); }
    else ctx->PSGetConstantBuffers(si->cbSlot, 1, &cb);
    std::vector<uint8_t> data(si->cbSize);
    bool cbOk = cb && ReadCB(cb, first * 16, si->cbSize, data.data());
    SafeRelease(cb);
    if (!cbOk) return false;
    auto f = [&](int ofs) { return (const float*)(data.data() + ofs); };
    UINT w = (UINT)f(si->screenSizeOfs)[0], h = (UINT)f(si->screenSizeOfs)[1];
    float jitterU = f(si->jitterOfs)[0], jitterV = f(si->jitterOfs)[1];
    float motion[16]; memcpy(motion, f(si->motionOfs), sizeof(motion));

    ID3D11ShaderResourceView *color = nullptr, *depth = nullptr, *vel = nullptr;
    ctx->PSGetShaderResources(si->colorSlot, 1, &color);
    ctx->PSGetShaderResources(si->depthSlot, 1, &depth);
    ctx->PSGetShaderResources(si->velSlot, 1, &vel);
    ID3D11RenderTargetView* outRtv = nullptr;
    ctx->OMGetRenderTargets(1, &outRtv, nullptr);
    ID3D11Resource *colorRes = nullptr, *outRes = nullptr;
    if (color) color->GetResource(&colorRes);
    if (outRtv) outRtv->GetResource(&outRes);

    bool ok = false;
    D3D11_TEXTURE2D_DESC cd;
    if (colorRes && outRes && depth && vel && w && h && TexDesc(colorRes, cd) && cd.Width >= w && cd.Height >= h) {
        if (g_fi.renderW != w || g_fi.renderH != h) { g_fi.reset = 1; g_fi.renderW = w; g_fi.renderH = h; }
        g_fi.haveDepth = g_fi.depth.Ensure(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (g_fi.haveDepth) Blit(ctx, depth, g_fi.depth.rtv, w, h, false);
        g_fi.haveMv = DrawMotionVectors(ctx, depth, vel, motion, w, h);
        int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        if (DepthInvertedFor(depth)) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        // The TAA shader un-jitters by sampling the colour at uv + g_uvJitterOffset.
        float jx = jitterU * w * g_cfg.jitterSignX, jy = jitterV * h * g_cfg.jitterSignY;
        bool upscaling = g_up.lastUpscaleFrame && g_up.lastUpscaleFrame + 2 >= g_up.frame;
        if (upscaling != g_up.upscaling) {
            g_up.upscaling = upscaling;
            g_fi.reset = 1;
            Log(upscaling ? "DLSS moves to the game's upscale pass (render %ux%u)" : "DLSS runs at the TAA pass (DLAA, %ux%u)", w, h);
        }
        if (!g_fi.haveDepth || !g_fi.haveMv) ok = false;
        else if (upscaling) {
            // The jittered frame flows through post-processing; DLSS runs at the upscale draw.
            ok = CopyTex(ctx, colorRes, outRes);
            g_up.staged = ok; g_up.w = w; g_up.h = h; g_up.flags = flags; g_up.jitterX = jx; g_up.jitterY = jy;
        } else {
            ok = EvaluateDlss(ctx, colorRes, outRes, w, h, flags, g_cfg.taaPreset, jx, jy,
                              (float)w * g_cfg.mvScaleX, (float)h * g_cfg.mvScaleY);
        }
    }
    SafeRelease(colorRes); SafeRelease(outRes); SafeRelease(outRtv);
    SafeRelease(color); SafeRelease(depth); SafeRelease(vel);

    static int state = -1;   // log transitions only
    if ((int)ok != state) { state = ok; Log(ok ? "TAA pass replaced by DLSS (%ux%u)" : "TAA replacement failed, game TAA used (%ux%u)", w, h); }
    return ok;
}

// The game's upscale draw: reads a texture at the TAA's render resolution and writes a larger
// render target, after the TAA pass of the same frame.
static bool ReplaceUpscale(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (!g_cfg.replaceTaa || !g_up.frame) return false;
    ID3D11PixelShader* known = g_up.ps.load();
    if (known ? ps != known : g_up.searchDraws <= 0) return false;
    if (!known) --g_up.searchDraws;
    if (g_fi.renderW == 0) return false;

    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource *src = nullptr, *dst = nullptr;
    srv->GetResource(&src);
    if (rtv) rtv->GetResource(&dst);
    bool replaced = false;
    D3D11_TEXTURE2D_DESC sd, dd;
    if (src && dst && TexDesc(src, sd) && TexDesc(dst, dd) && sd.Width == g_fi.renderW && sd.Height == g_fi.renderH &&
        dd.Width > sd.Width && dd.Height > sd.Height) {
        if (!known) {
            g_up.ps = ps;
            Log("Game upscale pass identified: PS %p, %ux%u -> %ux%u (fmt %d)", ps, sd.Width, sd.Height, dd.Width, dd.Height, dd.Format);
        }
        g_up.lastUpscaleFrame = g_up.frame;
        if (g_up.staged && g_up.w == sd.Width && g_up.h == sd.Height) {
            replaced = EvaluateDlss(ctx, src, dst, g_up.w, g_up.h, g_up.flags, g_cfg.taaPreset, g_up.jitterX, g_up.jitterY,
                                    (float)g_up.w * g_cfg.mvScaleX, (float)g_up.h * g_cfg.mvScaleY);
            g_up.staged = false;
            static int state = -1;
            if ((int)replaced != state) {
                state = replaced;
                Log(replaced ? "Upscale pass replaced by DLSS (%ux%u -> %ux%u)" : "DLSS upscale failed, game upscale used (%ux%u -> %ux%u)",
                    sd.Width, sd.Height, dd.Width, dd.Height);
            }
        }
    }
    SafeRelease(src); SafeRelease(dst); SafeRelease(rtv); srv->Release();
    return replaced;
}

// ------------------------------------------------------------------------------------------
// NGX r1.2 entry points handed to the game
// ------------------------------------------------------------------------------------------
static NVSDK_NGX_Result NVSDK_CONV Shim_Init(unsigned long long appId, const wchar_t* path, ID3D11Device* dev, NVSDK_NGX_Version ver)
{
    Log("NGX Init: appId=0x%llX ver=0x%X device=%p path=%ls", appId, (unsigned)ver, dev, path ? path : L"(null)");
    if (!dev) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (!g_d3dHooked) InstallD3DHooks(dev);
    { ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm); if (imm) { HookContext(imm); imm->Release(); } }
    if (g_dev != dev) { SafeRelease(g_dev); g_dev = dev; g_dev->AddRef(); }
    if (!InitGpuHelpers()) Log("Warning: GPU helpers unavailable");
    if (g_ngxReady && g_ngxDev == dev) return NVSDK_NGX_Result_Success;   // NGX stays up for the process (see Shim_Shutdown)

    static const wchar_t* paths[1];
    paths[0] = g_gameDir;
    NVSDK_NGX_FeatureCommonInfo fi = {};
    fi.PathListInfo.Path = paths;
    fi.PathListInfo.Length = 1;
    fi.LoggingInfo.LoggingCallback = [](const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) { Log("NGX: %s", msg); };
    fi.LoggingInfo.MinimumLoggingLevel = g_cfg.verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init(appId, path ? path : g_gameDir, dev, &fi);
    if (NVSDK_NGX_FAILED(r)) { Log("NVSDK_NGX_D3D11_Init failed 0x%08X", r); return r; }
    int avail = 0;
    if (g_ngxCaps) { NVSDK_NGX_D3D11_DestroyParameters(g_ngxCaps); g_ngxCaps = nullptr; }
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D11_GetCapabilityParameters(&g_ngxCaps)))
        NVSDK_NGX_Parameter_GetI(g_ngxCaps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    Log("DLSS Super Resolution available: %d", avail);
    if (!avail) { NVSDK_NGX_D3D11_Shutdown1(dev); return NVSDK_NGX_Result_FAIL_FeatureNotSupported; }
    g_ngxReady = true; g_ngxDev = dev;
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_Shutdown()
{
    Log("NGX Shutdown");
    g_dlss.Release();
    g_fi.depth.Release(); g_fi.mv.Release(); g_outTmp.Release();
    // NGX itself is left up: the game only shuts down on exit, and OptiScaler's DLSS-NR bridge
    // faults inside the driver's NGX shutdown while its GPU work is still retiring.
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_GetParameters(NVSDK_NGX_Parameter** out)
{
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out = new ShimParams();   // r1.2 has no DestroyParameters; the game keeps these for its lifetime
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_GetScratchBufferSize(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t* out)
{
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out = 256;   // the game allocates this buffer; DLSS does not need it
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_CreateFeature(ID3D11DeviceContext*, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
{
    if (!out || !p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* sp = static_cast<ShimParams*>(p);
    auto* h = new ShimHandle{};
    h->id = (unsigned)(uintptr_t)h;
    h->feature = (int)feature;
    h->w = sp->i("Width"); h->hgt = sp->i("Height"); h->outW = sp->i("OutWidth"); h->outH = sp->i("OutHeight");
    if (feature == 9) ++g_liveSpatial;
    Log("CreateFeature %d (%s): %ux%u -> %ux%u HDR=%d", (int)feature, feature == 1 ? "temporal" : feature == 9 ? "dlisp upscaler" : "?",
        h->w, h->hgt, h->outW, h->outH, sp->i("Hint.HDR"));
    *out = reinterpret_cast<NVSDK_NGX_Handle*>(h);
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* h = reinterpret_cast<ShimHandle*>(handle);
    Log("ReleaseFeature %d", h->feature);
    if (h->feature == 9) --g_liveSpatial;
    if (g_liveSpatial <= 0) g_dlss.Release();
    g_fi.reset = 1;
    delete h;
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV Shim_EvaluateFeature(ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback)
{
    if (!ctx || !handle || !p || !g_ngxReady) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* h = reinterpret_cast<const ShimHandle*>(handle);
    auto* sp = static_cast<const ShimParams*>(p);
    ID3D11Resource* color = sp->res("Color");
    ID3D11Resource* output = sp->res("Output");
    D3D11_TEXTURE2D_DESC cd;
    if (!color || !output || !TexDesc(color, cd)) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    static int logged[16];
    if (h->feature >= 0 && h->feature < 16 && logged[h->feature]++ < 2) {
        D3D11_TEXTURE2D_DESC od = {}, md = {}; TexDesc(output, od);
        ID3D11Resource* mv = sp->res("MotionVectors"); if (mv) TexDesc(mv, md);
        Log("Evaluate feature %d: color %ux%u fmt=%d bind=0x%X | output %ux%u fmt=%d bind=0x%X | mv %ux%u fmt=%d | MV.Scale=(%g,%g) Reset=%d HDR=%d BlendFactor=%g Sharpness=%g W=%d H=%d",
            h->feature, cd.Width, cd.Height, cd.Format, cd.BindFlags, od.Width, od.Height, od.Format, od.BindFlags,
            md.Width, md.Height, md.Format, sp->f("MV.Scale.X"), sp->f("MV.Scale.Y"), sp->i("Reset"), sp->i("Hint.HDR"),
            sp->f("BlendFactor"), sp->f("Sharpness"), sp->i("Width"), sp->i("Height"));
    }

    if (h->feature == 1) {
        ID3D11Resource* mv = sp->res("MotionVectors");
        g_lastMvRes = mv;
        g_fi.mvScaleX = sp->f("MV.Scale.X", 1.f);
        g_fi.mvScaleY = sp->f("MV.Scale.Y", 1.f);
        if (sp->i("Reset")) g_fi.reset = 1;
        g_fi.renderW = cd.Width; g_fi.renderH = cd.Height;
        SnapshotInputs(ctx, mv, cd.Width, cd.Height);
        if (g_liveSpatial > 0) {
            // Upscaling happens at the spatial stage; let the jittered frame flow through post-FX.
            if (!CopyTex(ctx, color, output)) Log("Temporal passthrough copy failed");
            return NVSDK_NGX_Result_Success;
        }
        // No upscaler stage: anti-alias at native resolution (DLAA) in HDR.
        return RunDlss(ctx, color, output, cd.Width, cd.Height, sp->i("Hint.HDR") != 0) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    }

    if (h->feature == 9) {
        UINT inW = sp->i("Width", (int)cd.Width), inH = sp->i("Height", (int)cd.Height);
        if (!inW || !inH) { inW = cd.Width; inH = cd.Height; }
        if (RunDlss(ctx, color, output, inW, inH, false)) return NVSDK_NGX_Result_Success;
        // Keep the picture alive if DLSS failed: plain bilinear upscale.
        return CopyTex(ctx, color, output) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    }
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

// ------------------------------------------------------------------------------------------
// GetProcAddress hook on the exe's import table
// ------------------------------------------------------------------------------------------
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static PFN_GetProcAddress o_GetProcAddress;

// The game loads d3d11.dll dynamically and its renderer copies the context's method pointers right
// after device creation, so hooks installed later never see its draws. Hooking here, before the
// device is handed back, puts our functions into those copies.
static PFN_D3D11_CREATE_DEVICE o_D3D11CreateDevice;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN o_D3D11CreateDeviceAndSwapChain;

static void HookNewDevice(ID3D11Device** dev, ID3D11DeviceContext** ctx)
{
    if (!dev || !*dev) return;
    InstallD3DHooks(*dev);
    ID3D11DeviceContext* imm = ctx ? *ctx : nullptr;
    if (imm) HookContext(imm);
    else { (*dev)->GetImmediateContext(&imm); if (imm) { HookContext(imm); imm->Release(); } }
    Log("Game device %p created; context hooked before first use", *dev);
}

static HRESULT WINAPI Hk_D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT f, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                                           UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** ctx)
{
    HRESULT hr = o_D3D11CreateDevice(a, t, sw, f, fl, nfl, sdk, dev, ofl, ctx);
    if (SUCCEEDED(hr)) HookNewDevice(dev, ctx);
    return hr;
}

static HRESULT WINAPI Hk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT f, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                                                       UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev,
                                                       D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** ctx)
{
    HRESULT hr = o_D3D11CreateDeviceAndSwapChain(a, t, sw, f, fl, nfl, sdk, sd, sc, dev, ofl, ctx);
    if (SUCCEEDED(hr)) HookNewDevice(dev, ctx);
    return hr;
}

// Resolves an export without going through any GetProcAddress hook.
static FARPROC RawGetProcAddress(HMODULE mod, LPCSTR name)
{
    using PFN_LdrGetProcedureAddress = LONG(NTAPI*)(HMODULE, void* ansiName, ULONG ordinal, void** out);
    static auto ldr = (PFN_LdrGetProcedureAddress)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrGetProcedureAddress");
    struct { USHORT len, max; const char* buf; } str = {};
    ULONG ordinal = 0;
    if (IS_INTRESOURCE(name)) ordinal = (ULONG)(ULONG_PTR)name;
    else { str.len = (USHORT)strlen(name); str.max = str.len + 1; str.buf = name; }
    void* p = nullptr;
    if (!ldr || ldr(mod, ordinal ? nullptr : &str, ordinal, &p) < 0) return nullptr;
    return (FARPROC)p;
}

static FARPROC WINAPI Hk_GetProcAddress(HMODULE mod, LPCSTR name)
{
    // Other GetProcAddress hooks (OptiScaler's) may forward to the exe's import entry, which is
    // this function; the nested call then resolves directly instead of recursing.
    static thread_local bool inside;
    if (inside) {
        static bool logged;
        if (!logged) { logged = true; Log("GetProcAddress hook re-entered through another hook; resolving directly"); }
        return RawGetProcAddress(mod, name);
    }
    inside = true;
    FARPROC real = o_GetProcAddress(mod, name);
    inside = false;
    if (!g_cfg.enabled || !name || IS_INTRESOURCE(name)) return real;
    if (real && !strcmp(name, "D3D11CreateDevice")) {
        o_D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)real;
        Log("Redirected %s", name);
        return (FARPROC)Hk_D3D11CreateDevice;
    }
    if (real && !strcmp(name, "D3D11CreateDeviceAndSwapChain")) {
        o_D3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)real;
        Log("Redirected %s", name);
        return (FARPROC)Hk_D3D11CreateDeviceAndSwapChain;
    }
    if (strncmp(name, "NVSDK_NGX_D3D11_", 16) != 0) return real;
    static const struct { const char* n; void* f; } table[] = {
        { "NVSDK_NGX_D3D11_Init", (void*)Shim_Init },
        { "NVSDK_NGX_D3D11_Shutdown", (void*)Shim_Shutdown },
        { "NVSDK_NGX_D3D11_GetParameters", (void*)Shim_GetParameters },
        { "NVSDK_NGX_D3D11_GetScratchBufferSize", (void*)Shim_GetScratchBufferSize },
        { "NVSDK_NGX_D3D11_CreateFeature", (void*)Shim_CreateFeature },
        { "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Shim_EvaluateFeature },
        { "NVSDK_NGX_D3D11_ReleaseFeature", (void*)Shim_ReleaseFeature },
    };
    for (auto& e : table)
        if (!strcmp(name, e.n)) { Log("Redirected %s", name); return (FARPROC)e.f; }
    return real;
}

static bool HookExeImport(const char* dll, const char* func, void* hook, void** orig)
{
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func)) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------
// 50 % resolution scale = DLSS Balanced
// The game keeps the resolution scale as a percentage in its graphics settings object (offset
// 0x20, accepted range 50..200). Two of its functions are hooked: the GraphicsConfig.ini loader
// (startup) and the function that applies the settings (startup and every menu change). When the
// scale is 50 it becomes 58 before the game uses it. The menu keeps showing 50 %.
// Both functions are matched by their first bytes; another game build leaves them unhooked.
// ------------------------------------------------------------------------------------------
static const int kBalancedRatio = 58;
static const uintptr_t kLoadSettingsRva = 0x7a7160, kApplySettingsRva = 0x7a7d30;
static const uint8_t kLoadSettingsBytes[] = { 0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x70 };
static const uint8_t kApplySettingsBytes[] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xd9 };

using PFN_LoadSettings = bool(__fastcall*)(void* settings, void* a, void* b);
using PFN_ApplySettings = void(__fastcall*)(void* settings);
static PFN_LoadSettings o_LoadSettings;
static PFN_ApplySettings o_ApplySettings;

static void PreferBalanced(void* settings)
{
    int* ratio = (int*)((uint8_t*)settings + 0x20);
    if (!settings || *ratio != 50) return;
    *ratio = kBalancedRatio;
    Log("Resolution scale 50%% set to %d%% (DLSS Balanced)", kBalancedRatio);
}

static bool __fastcall Hk_LoadSettings(void* settings, void* a, void* b)
{
    bool r = o_LoadSettings(settings, a, b);
    PreferBalanced(settings);
    return r;
}

static void __fastcall Hk_ApplySettings(void* settings)
{
    PreferBalanced(settings);
    o_ApplySettings(settings);
}

static void HookResolutionScale()
{
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    const struct { uintptr_t rva; const uint8_t* bytes; size_t n; void* hook; void** orig; const char* name; } hooks[] = {
        { kLoadSettingsRva, kLoadSettingsBytes, sizeof(kLoadSettingsBytes), (void*)Hk_LoadSettings, (void**)&o_LoadSettings, "settings loader" },
        { kApplySettingsRva, kApplySettingsBytes, sizeof(kApplySettingsBytes), (void*)Hk_ApplySettings, (void**)&o_ApplySettings, "settings apply" },
    };
    const DWORD imageSize = ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    if (!MinHookReady()) { Log("ERROR: MinHook initialisation failed"); return; }
    for (auto& h : hooks)
        if (h.rva + h.n > imageSize || memcmp(base + h.rva, h.bytes, h.n)) { Log("Game %s not found (other game build); 50%% resolution scale stays 50%%", h.name); return; }
    for (auto& h : hooks) {
        MH_STATUS st = MH_CreateHook(base + h.rva, h.hook, h.orig);
        if (st == MH_OK) st = MH_EnableHook(base + h.rva);
        if (st != MH_OK) { Log("Detour of game %s failed: %s", h.name, MH_StatusToString(st)); return; }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    GetModuleFileNameW(nullptr, g_gameDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_gameDir, L'\\')) *s = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"ffxv")) return TRUE;   // only act inside the game

    std::wstring logPath = std::wstring(g_gameDir) + L"\\FFXV_DLSS4.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    Log("FFXV DLSS4 loaded (NGX SDK %s)", "310.9.1");
    LoadConfig();
    if (!g_cfg.enabled) { Log("Disabled in FFXV_DLSS4.ini"); return TRUE; }

    if (!HookExeImport("kernel32.dll", "GetProcAddress", (void*)Hk_GetProcAddress, (void**)&o_GetProcAddress))
        Log("ERROR: could not hook GetProcAddress import");
    if (g_cfg.replaceTaa) HookResolutionScale();   // before the game reads its settings
    if (HANDLE t = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}
