# VRFusion native OBS source

This optional plugin consumes VRFusion's synchronized final D3D11 texture directly inside OBS. It works with either final-output producer:

- `VRFusion.exe` — SteamVR/OpenVR fallback compositor.
- `VRFusionXRView.exe` — OpenXR color/depth spectator viewer.

Run only one producer at a time.

The source appears in OBS as:

```text
VRFusion GPU Capture
```

The plugin opens the producer texture using `gs_texture_open_shared`, attempts keyed-mutex consumer key `1` without blocking OBS, performs a GPU-to-GPU copy into an OBS-owned texture, releases producer key `0`, and renders the most recently completed copy. VRFusion 0.5 uses final-output protocol v2; the plugin also watches the published frame counter and logs a warning if the producer stops advancing.

## Build prerequisites

Use development headers/import library matching the OBS version you actually run.

```powershell
cmake -S obs-plugin -B build-obs -A x64 `
  -DOBS_INCLUDE_DIR="C:\path\to\obs-studio\libobs" `
  -DOBS_LIB="C:\path\to\obs.lib"

cmake --build build-obs --config Release
```

Copy `vrfusion-obs.dll` into the plugin binary location appropriate for that OBS installation and restart OBS. Then add **VRFusion GPU Capture** as a source.

The plugin is intentionally not part of the default `scripts\build.bat`: a normal VRFusion build machine is not guaranteed to have OBS development files installed.
