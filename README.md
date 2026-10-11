# FFXV DLSS4

Replaces the DLSS 1.0 built into **FINAL FANTASY XV Windows Edition** with current DLSS Super
Resolution (NGX SDK 310.9.1). That includes the 2nd-gen transformer presets **M** and **L**.

## Install

Copy these files next to `ffxv_s.exe`:

| File | Purpose |
|---|---|
| `winmm.dll` | the mod (proxy for the system `winmm.dll`) |
| `nvngx_dlss.dll` | DLSS 310.9.1 from the NVIDIA DLSS SDK |
| `FFXV_DLSS4.ini` | settings (preset, etc.) |

Then choose **TAA** as the anti-aliasing option. The mod runs DLSS (preset `TAAPreset`, L by
default) in place of the game's TAA pass, at any resolution, with the game's own TAA jitter.
`ReplaceTAA=0` keeps the game's TAA.

The game's own **NVIDIA DLSS** option also works, but the game only offers it at 4K and does not
jitter the camera in that mode, so DLSS cannot anti-alias edges as well there.

To uninstall, delete `winmm.dll` (and the two other files).

`FFXV_DLSS4.log` is written next to the exe. It shows the DLSS version loaded, the resolutions,
the detected jitter mode and depth convention.

## How it works

The game links NGX r1.2 statically. It resolves `NVSDK_NGX_D3D11_*` from the driver through its
`GetProcAddress` import. The mod hooks that import and serves its own implementation:

* `GetParameters` returns a parameter object that reports both DLSS 1 features (`#\x01`, `#\x09`) as available.
* **Feature 1** (DLSS 1 temporal stage, render resolution, before post-processing) copies the
  jittered frame through. It also snapshots depth and motion vectors, which the game recycles later in the frame.
* **Feature 9** (DLSS 1 `dlisp` upscaler, after post-processing) runs one DLSS SR evaluation from
  render resolution to output resolution with the chosen preset.
* Jitter and depth are read from the game's `NVDLSSVelocityDecode` pass:
  `cbMotMat.g_jitterOfs` and `g_depthTex`. D3D11 vtable hooks capture them. Reversed-Z is detected from the
  game's depth clears.
* If the game only uses the temporal stage, DLSS runs as DLAA (HDR) there instead.

TAA mode (`ReplaceTAA=1`):

* The game loads `d3d11.dll` dynamically; the mod catches `D3D11CreateDevice` through the same
  `GetProcAddress` hook and detours the context methods in `d3d11.dll` (MinHook, `third_party/minhook`).
  The runtime rewrites the context vtable while the game renders, so vtable patches miss most draws.
* The TemporalAA pixel shader is recognised by `cbTemporalAA` in its reflection data. When it is
  drawn, the mod takes its inputs (`g_jitterTex` colour, `g_depthTex`, `g_velocityTex`,
  `g_uvJitterOffset`, `g_motionMatrix`), rebuilds motion vectors the way that shader reprojects,
  runs DLSS into the pass's render target and skips the game's draw. If anything is missing, the
  game's TAA runs instead.
* Below 100 % resolution scale the game upscales the post-processed frame with a plain textured
  draw (render → display resolution). The mod finds that draw. The TAA pass then only passes the
  jittered frame through and stages its inputs, and DLSS upscales at that draw, so the
  resolution scale works like a DLSS quality mode (75 % ≈ Quality, 50 % = Balanced).
  At 100 % DLSS runs at the TAA pass as DLAA.
* The menu's 50 % is DLSS Balanced: when `GraphicsConfig.ini` says `RenderingResolutionRatio=50`, the
  mod hands the game 58 while it reads the file (ntdll read hooks), and the menu keeps showing 50 %. The
  file is only read at startup, so after choosing 50 % in the menu, restart the game. For DLSS
  Performance, write `RenderingResolutionRatio=49` in the file (the game accepts any percentage).

## OptiScaler (DLSS-NR)

The mod works with OptiScaler (`dxgi.dll`) set to `Dx11Upscaler = dlss_12`. OptiScaler intercepts the
mod's DLSS calls and runs them through its D3D12 bridge, so its features (DLSS-NR under `[DlssNr]`)
apply to the mod's DLSS output. The mod keeps NGX initialised until the game exits, because
the bridge's NR teardown faults inside the driver's NGX shutdown.

## Tuning

If something looks wrong, change one setting at a time in `FFXV_DLSS4.ini`:

* Shimmering or a "swimming" image when standing still: try `JitterScaleX=-1` and/or `JitterScaleY=-1`, or `JitterMode=Delta`.
* Smearing or ghosting on moving objects or camera turns: try `MVScaleX=-1` / `MVScaleY=-1`.
* Disocclusion artifacts at object edges: try `DepthInverted=0`.

## Build

Requires VS 2022 Build Tools. The needed DLSS SDK parts (headers, `nvsdk_ngx_s.lib`) are in
`third_party/DLSS` and come from https://github.com/NVIDIA/DLSS (v310.9.1). `nvngx_dlss.dll` is not
in this repository: take it from `lib/Windows_x86_64/rel` of that SDK release.

```
build.bat                  -> build\winmm.dll
python tools\gen_proxy.py  -> regenerates src\proxy_exports.h from System32\winmm.dll
```

`tools/test_harness.cpp` replays the game's NGX call sequence on a real D3D11 device. That
includes the velocity-decode pass and a stand-in TemporalAA pass that the mod must replace. It checks the mod without starting the game. Build it as
`ffxv_test.exe` (the mod only activates in processes whose name contains `ffxv`), and put
`winmm.dll` and `nvngx_dlss.dll` next to it.
