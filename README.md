# VRFusion 0.5 — one-click dual-eye VR spectator output

VRFusion converts the two VR eye views into one normal widescreen spectator image for a monitor, recording, and OBS.

Version 0.5 changes the normal workflow to one-click operation. The user-facing entry point is now only:

```text
VRFusion.exe
```

Run it before the VR game. VRFusion performs the OpenXR setup for the current user, waits for a VR title, chooses the best available backend, starts the internal compositor, and publishes one stable `VRFusion Output` window plus the existing shared GPU output.

## Normal use

1. Extract the complete `dist` folder somewhere permanent.
2. Double-click `VRFusion.exe`.
3. Start the VR runtime/game.
4. Use the `VRFusion Output` window on the monitor.
5. In OBS, either use the native `VRFusion GPU Capture` source when the OBS plugin is installed, or add a Window Capture for `VRFusion Output` once. The window remains stable even if VRFusion changes backend.

No PowerShell OpenXR install/enable command is required for normal use.

### First-run OpenXR note

An OpenXR API layer must be loaded when the game creates its OpenXR instance. If VRFusion has never been run on the PC and an OpenXR game is already running, close and reopen that game once after the first VRFusion launch. After registration exists, normal use is simply `VRFusion.exe` before the game.

## Automatic backend selection

VRFusion chooses in this order:

```text
OpenXR + depth  -> center-eye depth reconstruction
OpenXR color    -> stereo angular fusion
SteamVR         -> dual-eye compositor fallback
No VR runtime   -> wait automatically
```

The backend executables are internal in normal use:

- `VRFusionXRView.exe` — OpenXR color/depth compositor.
- `VRFusionSteamVR.exe` — SteamVR/OpenVR fallback.
- both are started with `--headless` by `VRFusion.exe`.
- `VRFusion Output` is owned by the controller and stays the same source during backend switches.

## What 0.5 adds

- New one-click `VRFusion.exe` controller.
- Automatic per-user OpenXR API-layer registration in `HKCU`; no administrator rights are required for this registration.
- Portable-folder repair: stale VRFusion layer registry entries are removed when the folder is moved.
- New control/heartbeat IPC. The implicit OpenXR layer remains dormant unless a live `VRFusion.exe` heartbeat requests capture.
- The OpenXR layer no longer depends on `VRFUSION_OPENXR` being inherited by Steam or the game launcher.
- A running OpenXR process that already loaded the registered layer can start/stop capture when VRFusion starts/exits without recreating the OpenXR instance.
- Automatic switching between OpenXR and SteamVR fallback.
- Hidden backend windows in normal mode.
- Stable user-facing `VRFusion Output` window rendered from the shared GPU texture.
- Single-instance protection: opening `VRFusion.exe` twice focuses the existing controller instead of creating competing capture controllers.
- D3D11 OpenXR MSAA color resolve support. MSAA depth remains a safe color-fusion fallback rather than attempting invalid depth resolve.
- Existing OpenXR depth reconstruction, stabilization, adaptive stereo seam, telemetry, shared GPU output, and OBS source protocol are retained.
- Windows x64 GitHub Actions workflow that builds and uploads `VRFusion-Windows-x64.zip`.

## Controller buttons

`Diagnostics` opens `VRFusionDoctor.exe`.

`Show output` restores the stable preview if it was closed/hidden.

`Restart capture` restarts only the internal compositor while keeping the controller/setup alive.

`Exit` stops the active backend and the capture heartbeat.

## OBS

The preferred path is the native source in `obs-plugin/`:

```text
VRFusion GPU Capture
```

It opens VRFusion's D3D11 shared texture, uses keyed-mutex synchronization, and keeps the OBS scene independent from the selected VR backend.

Without the plugin, add a normal Window Capture for:

```text
VRFusion Output
```

This window is intentionally owned by the controller rather than by the OpenXR/SteamVR backend, so OBS does not need a different source when the backend changes.

## Build on Windows

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 C++ workload / MSVC
- CMake 3.24+
- internet access on the first configure so CMake can fetch OpenVR 2.15.6 and OpenXR SDK 1.1.63 headers

Build and package:

```bat
scripts\build.bat
```

The packaged folder is written to:

```text
dist\
```

Important binaries include:

```text
VRFusion.exe                 one-click controller + stable output
VRFusionSteamVR.exe          internal SteamVR compositor
VRFusionXRView.exe           internal OpenXR compositor
VRFusionOpenXRLayer.dll      implicit OpenXR capture layer
VRFusionDoctor.exe           diagnostics
VRFusionSharedProbe.exe      shared-output probe
VRFusionOpenXRProbe.exe      OpenXR-capture probe
openvr_api.dll
XR_APILAYER_VRFusion_capture.json
vrfusion.ini
```

`VRFusionLauncher.exe` is retained only as a compatibility shim for old shortcuts; it now starts `VRFusion.exe`.

## GitHub Actions build

The repository includes:

```text
.github/workflows/windows-build.yml
```

A push to `main`/`master`, a `v*` tag, or manual workflow dispatch runs the x64 Windows build and uploads `VRFusion-Windows-x64.zip` as a workflow artifact.

## Configuration

`vrfusion.ini` still controls compositor quality/output:

```ini
width=1920
height=1080
fps=60
mode=fusion
zoom=1.08
feather=0.020
seam_contrast=4.0
stabilization=1
stabilization_ms=75
max_stabilization_deg=7
openxr_mesh_step=3
openxr_depth_discontinuity_ratio=1.25
```

The controller itself intentionally uses automatic defaults; normal users should not need to choose a backend or touch the OpenXR scripts.

## Capture model

### OpenXR depth path

When an OpenXR title submits `XrCompositionLayerDepthInfoKHR`, VRFusion copies color/depth data before `xrReleaseSwapchainImage`, then reconstructs a virtual spectator camera located between the physical eyes. Large depth discontinuities are rejected and uncovered regions fall back to the color compositor.

### OpenXR without depth

VRFusion uses the projection/FOV metadata from both eyes and produces a single angularly aligned widescreen view with an adaptive seam.

### SteamVR fallback

VRFusion obtains the left/right undistorted mirror textures through OpenVR and produces the same final shared-output protocol.

## Current technical limits

- OpenXR capture is currently D3D11. D3D12 OpenXR titles still need a future backend or the SteamVR path when available.
- MSAA color swapchains are now resolved; MSAA depth is not reconstructed in 0.5.
- A universal center-eye image cannot be perfectly reconstructed when the title does not submit usable scene depth.
- Newly revealed surfaces in depth reprojection cannot be recovered if neither physical eye saw them; VRFusion uses the color compositor as the fallback rather than inventing content.
- A real HMD/runtime test is still required for final compatibility/performance validation.

## Diagnostics

Normally diagnostics are unnecessary. If something fails, run:

```bat
VRFusionDoctor.exe
```

It reports controller heartbeat, OpenXR registration/capture status, SteamVR state, final GPU output, backend, depth availability, frame counters, publish drops, and compositor timing.
