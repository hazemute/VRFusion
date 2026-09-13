# VRFusion 0.2 — dual-eye VR spectator compositor

VRFusion is a Windows spectator-output application for SteamVR. It reads SteamVR's undistorted D3D11 mirror texture for the left and right eyes, reconstructs both views into one perspective-correct spectator canvas, optionally stabilizes headset rotation, and publishes the result both to a normal desktop preview and to a synchronized cross-process D3D11 texture.

The goal is a stream-friendly image that looks much closer to a normal game camera than a raw single-eye VR mirror.

## What changed in 0.2

- Full 3x3 per-eye orientation reprojection instead of yaw-only canted-display compensation.
- Head-centered FOV bounds calculated from all four projection corners of each eye.
- Perspective-preserving 16:9 crop; the dual-eye image is no longer independently stretched on X and Y.
- Rotation-only spectator stabilization using the HMD pose, quaternion SLERP and a configurable maximum lag angle.
- Stabilization overscan through configurable `zoom`.
- Contrast-aware stereo seam that becomes harder where left/right images disagree strongly, reducing close-object ghosting.
- Runtime modes: fused, left eye, right eye and side-by-side.
- Runtime hotkeys and INI reload.
- Flip-model DXGI preview swap chain with maximum frame latency set to one frame.
- Separate shared D3D11 output texture with keyed-mutex synchronization for a future native OBS source.
- `VRFusionSharedProbe.exe` verifies that a second process can open the GPU texture, synchronize on it and observe advancing frames.

## Current architecture

```text
SteamVR / OpenVR compositor
     |                         HMD tracking pose
     |                               |
     |                               v
     |                   quaternion spectator stabilizer
     |                               |
     +--> left mirror SRV -----------+---->
     |                                     3D ray reprojection
     +--> right mirror SRV ----------------> per-eye projection
                                           adaptive stereo seam
                                                   |
                                                   v
                                         private D3D11 output
                                           /             \
                                          /               \
                              preview swap chain       shared GPU texture
                              VRFusion Spectator       keyed mutex + event
                                      |                       |
                                      v                       v
                               OBS Game Capture       VRFusion GPU Capture
                                                     native OBS source / probe
```

## Requirements

- Windows 10 or Windows 11 x64.
- SteamVR running with a connected headset.
- A VR title using the SteamVR compositor.
- Visual Studio 2022/2026 Build Tools with **Desktop development with C++**.
- CMake 3.24 or newer.
- Internet access on the first CMake configure. The project downloads the pinned OpenVR SDK `v2.15.6`.

## Build

Run from Explorer or a terminal:

```bat
scripts\build.bat
```

The script configures an x64 Release build, compiles both executables and creates:

```text
dist\
  VRFusion.exe
  VRFusionSharedProbe.exe
  openvr_api.dll
  vrfusion.ini
  README.md
  STATUS.md
```

## Run

1. Start SteamVR and connect the headset.
2. Start a VR game.
3. Launch `VRFusion.exe`.
4. Capture the `VRFusion Spectator` window in OBS using **Game Capture**.

Recommended OBS canvas/output: `1920x1080`, `60 FPS`.

## Hotkeys

While the VRFusion window is focused:

```text
F1        fused dual-eye spectator view
F2        left-eye diagnostic view
F3        right-eye diagnostic view
F4        raw side-by-side diagnostic view
S         toggle spectator rotation stabilization
[ / ]     reduce / increase stereo seam feather
- / +     reduce / increase crop zoom
R         reload runtime-safe values from vrfusion.ini
F11       toggle borderless fullscreen preview
```

Changing `width` or `height` still requires a restart because those values determine GPU resources and swap-chain buffers.

## Configuration

Default `vrfusion.ini`:

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
show_fps=1
vsync=0
```

### `zoom`

`1.00` keeps the maximum available perspective-correct FOV. Values around `1.05` to `1.15` crop a little around the edges and provide room for rotational stabilization without exposing black areas during small head movements.

### `feather`

Controls the width of the left/right transition in tangent-FOV space. Smaller values reduce stereo ghosting but make a mismatch more abrupt.

### `seam_contrast`

Controls adaptive seam hardening. When the two eyes differ strongly at a pixel, VRFusion assumes the disagreement may be caused by close geometry/parallax and narrows the blend there. `0` disables this behavior.

### Stabilization

The stabilization path is rotation-only. It estimates a smoothed spectator orientation from the latest HMD pose and reprojects output rays back into the current eye views. `stabilization_ms` controls smoothing strength. `max_stabilization_deg` prevents the virtual camera from lagging arbitrarily far behind a fast head turn.

## Shared GPU output

VRFusion 0.2 also publishes a D3D11 texture for zero-copy consumers on the same GPU.

Protocol objects:

```text
Local\VRFusionSharedTexture_v1   shared-memory metadata
Local\VRFusionFrameReady_v1     frame-ready event
```

The metadata contains the output dimensions, DXGI format, producer process ID, adapter LUID, shared D3D11 handle and synchronized frame counter.

The shared texture uses `IDXGIKeyedMutex`:

```text
key 0 -> producer may write
key 1 -> consumer may read
```

The preview never waits on the consumer. If nobody is reading the shared texture, normal VRFusion rendering continues unaffected.

To validate the channel, run this while VRFusion is active:

```bat
VRFusionSharedProbe.exe
```

A healthy result ends with:

```text
VRFusion shared-output probe: PASS
```

### Native OBS source

The archive now includes `obs-plugin/`, an optional native OBS input source named **VRFusion GPU Capture**. It opens the shared handle with libobs, acquires consumer key `1`, performs a GPU-to-GPU copy into an OBS-owned texture, releases producer key `0`, and renders the most recent completed copy. No desktop/window capture is involved.

It is not part of the default application build because compiling an OBS module requires OBS development headers and the matching `obs.lib`. Build instructions are in `obs-plugin/README.md`.

## Important limitation: no scene depth yet

VRFusion 0.2 still receives final color mirror textures from SteamVR, not the game's scene depth. Therefore it can correctly combine angular coverage and remove most global stereo duplication, but it cannot mathematically synthesize a perfect camera located halfway between both physical eyes for very close geometry.

The adaptive seam reduces the visible error instead of pretending the missing depth does not matter.

A true center-eye reconstruction requires one of these paths:

1. An OpenXR API layer that intercepts color plus `XrCompositionLayerDepthInfoKHR` when a game submits depth.
2. Game-specific depth capture.
3. A stereo depth estimator, which costs substantially more GPU time and can introduce temporal artifacts.

The OpenXR depth path is the preferred next milestone.

## Troubleshooting

### SteamVR initialization error

SteamVR must be running and the headset must already be active before VRFusion starts.

### Mirror texture acquisition error

Wait until SteamVR Home/dashboard or the VR game is visibly running, then restart VRFusion.

### Black output in OBS

Use OBS **Game Capture** first. If another hook/capture tool conflicts with it, use Window Capture as a compatibility fallback.

### Stabilized view exposes a black edge

Increase `zoom`, reduce `stabilization_ms`, or reduce `max_stabilization_deg`.

### Close hand/controller looks discontinuous at the center

That is stereo parallax without scene depth. Try a smaller `feather` or a larger `seam_contrast`. The future depth backend is the complete solution.

## Project files

```text
src/main.cpp                compositor, tracking, D3D11 rendering
src/shared_protocol.hpp     cross-process GPU-output protocol
src/shared_probe.cpp        independent shared-texture verification tool
obs-plugin/                 optional native zero-copy OBS source
scripts/build.bat           one-click Windows build
scripts/build.ps1           CMake build + dist packaging
vrfusion.ini                runtime configuration
STATUS.md                   validation status and known limitations
```
