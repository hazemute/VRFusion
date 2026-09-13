# VRFusion 0.2 status

## Implemented

- SteamVR/OpenVR background initialization.
- SteamVR-selected DXGI adapter and D3D11 device.
- Separate left/right undistorted compositor mirror SRVs.
- Raw per-eye projection from `GetProjectionRaw`.
- Full 3x3 eye-to-head orientation handling from `GetEyeToHeadTransform`.
- Head-centered bounds reconstructed from all four eye projection corners.
- Perspective-correct 16:9 crop with configurable overscan/zoom.
- GPU-only fused spectator shader.
- Contrast-aware adaptive seam to reduce high-disparity blending.
- Fused / left / right / side-by-side view modes.
- Rotation-only spectator stabilization from compositor HMD pose.
- Quaternion SLERP smoothing and maximum stabilization lag clamp.
- Flip-model DXGI preview swap chain with one-frame maximum latency.
- Runtime hotkeys and INI reload for non-resource settings.
- Independent shared D3D11 texture with `IDXGIKeyedMutex` producer/consumer synchronization.
- Shared-memory metadata containing GPU LUID, dimensions, format, handle and frame counter.
- Frame-ready named event.
- `VRFusionSharedProbe` second-process validation utility.
- Optional native OBS input-source code using libobs shared-texture and keyed-mutex APIs.
- Release packaging script.

## Source/API validation performed here

- Checked the current Valve OpenVR release listing; SDK 2.15.6 is still marked latest at the time this project was prepared.
- Cross-checked `GetMirrorTextureD3D11`, `ReleaseMirrorTextureD3D11` and `GetLastPoseForTrackedDeviceIndex` against current OpenVR headers.
- Checked the project for balanced C++ delimiters and stale development-marker tokens in source files.
- Checked that the shared-output producer and probe use opposite keyed-mutex keys and that the preview path never blocks on a missing consumer.
- Removed the unused experimental OpenXR directory left over from 0.1.

## Not executable in this environment

- Native MSVC compilation: this environment is Linux and does not contain the Windows SDK headers/libraries required for D3D11/DXGI builds.
- SteamVR/HMD runtime test: no VR headset or SteamVR compositor is attached here.
- OBS live capture test for the same reason.

The included Windows build script is intentionally strict: a compiler or linker error stops packaging rather than silently producing a partial `dist` folder.

## Known technical limitations

- SteamVR mirror textures contain final color, not guaranteed scene depth.
- True center-eye reprojection of close geometry therefore remains impossible in the generic SteamVR backend.
- Rotational stabilization is depth-independent; translation is intentionally not stabilized because doing so without depth would cause larger parallax errors.
- A SteamVR compositor restart while VRFusion is running can invalidate mirror resources; restart VRFusion after restarting SteamVR.
- The shared GPU protocol is local-machine, same-adapter D3D11 only.

## Recommended next milestone

Add an OpenXR API-layer backend that observes submitted `XrCompositionLayerProjection` views and consumes `XrCompositionLayerDepthInfoKHR` when available. With depth, VRFusion can reproject both eyes into an actual center camera instead of only choosing/blending angular coverage.
