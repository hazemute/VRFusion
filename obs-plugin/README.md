# VRFusion native OBS source

This optional plugin consumes VRFusion's synchronized shared D3D11 texture directly inside OBS. It avoids Desktop/Window/Game Capture of the preview window.

The source appears in OBS as:

```text
VRFusion GPU Capture
```

The plugin opens the producer texture using `gs_texture_open_shared`, acquires keyed-mutex key `1`, performs a GPU-to-GPU copy into an OBS-owned texture, releases key `0` back to VRFusion, and renders the last completed copy. The source never waits for a new VRFusion frame; if no fresh key is ready, OBS keeps the previous frame.

## Build prerequisites

You need headers and the import library from the same OBS build/version you will run.

```powershell
cmake -S obs-plugin -B build-obs -A x64 `
  -DOBS_INCLUDE_DIR="C:\path\to\obs-studio\libobs" `
  -DOBS_LIB="C:\path\to\obs.lib"

cmake --build build-obs --config Release
```

Copy `vrfusion-obs.dll` into the matching OBS plugin binary directory according to your OBS installation/package layout, restart OBS, then add **VRFusion GPU Capture** as a source.

This plugin source is included as the zero-copy integration path, but it is not compiled by the main `scripts\build.bat` because a normal Windows machine does not necessarily have OBS development headers/libraries installed.
