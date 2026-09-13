# VRFusion 0.5 validation status

## Implemented

- One-click `VRFusion.exe` controller.
- Automatic HKCU OpenXR implicit-layer registration and stale-path cleanup.
- Controller heartbeat/control protocol; OpenXR layer capture is dormant without a live controller.
- Automatic OpenXR/SteamVR backend selection and switching.
- Hidden internal backend windows.
- Stable `VRFusion Output` monitor/Window-Capture surface owned by the controller.
- D3D11 shared-texture consumer in the controller with keyed-mutex synchronization and GPU copy before display.
- Existing shared GPU output remains available to the OBS plugin.
- Single-instance controller behavior.
- D3D11 OpenXR MSAA color resolve path.
- Safe fallback when MSAA depth cannot be reconstructed.
- OpenXR depth-center reconstruction and color-fusion fallback from 0.4 retained.
- GitHub Actions Windows x64 artifact workflow.
- Legacy `VRFusionLauncher.exe` retained as a compatibility shim.

## Source/package checks performed in this environment

The final source package is checked for:

- JSON validity of the OpenXR manifest.
- balanced C/C++ braces/parentheses with a structural checker.
- expected source/binary target references in CMake/build scripts.
- absence of obsolete 0.4 product-version strings in active source/documentation.
- SHA-256 source manifest consistency.
- ZIP integrity by extraction/test.

## Not honestly validated here

This environment does not provide Windows SDK/D3D11 runtime, MSVC link libraries, SteamVR/OpenXR runtime, OBS, or a physical HMD. Therefore the following must not be represented as passed until run on Windows:

- MSVC x64 compile/link.
- API-layer loading in a real OpenXR title.
- SteamVR mirror capture with a real headset.
- MSAA resolve behavior on real OpenXR swapchains.
- D3D keyed-mutex interoperability across the controller/backend processes on a real GPU driver.
- OBS plugin loading/capture.
- headset-specific FOV/canted-display quality.
- measured GPU/CPU latency and frame pacing.

## First real Windows test order

1. Run `scripts\build.bat`.
2. Start `dist\VRFusion.exe` before the VR game.
3. Confirm the controller says `READY`, then `LIVE` after the title starts.
4. Confirm `VRFusion Output` shows a continuously updating combined image.
5. Run `VRFusionDoctor.exe` and save the output if anything is not `READY`/advancing.
6. Test a D3D11 OpenXR title with depth, a title without depth, and a SteamVR fallback title.
7. Test OBS using `VRFusion GPU Capture` when the plugin is built, otherwise Window Capture on `VRFusion Output`.

## Known limitations

- OpenXR backend remains D3D11-only.
- D3D11 MSAA color is supported, but MSAA depth falls back to non-depth stereo fusion.
- The implicit API layer must already be registered when an OpenXR process creates its instance. On the very first VRFusion run, an already-running OpenXR game may need one restart. Future launches require no manual setup.
- Always-loaded implicit-layer bookkeeping is intentionally minimal while the controller heartbeat is absent, but real-title compatibility still requires testing.
- Depth disocclusions expose scene regions no physical eye saw; the compositor fills from its angular color background rather than hallucinating missing scene data.
