Standalone Display Filter preview with optional DLSS Frame Generation, HDR output controls, private-DLSS edge correction, and bounded production logging.

Download and extract **display-filter-v0.3.0-preview.zip**, then launch `display_filter.exe`. Keep the `runtime` folder beside it. DLSS Super Resolution and the Streamline/DLSS Frame Generation runtimes and licenses are included. Supply your own compatible `nvngx_dlssnr.dll` through the runtime selector.

Changes:
- Detailed timing diagnostics are off by default. Normal performance summaries are logged every 30 seconds; the on-screen FPS counter still updates normally.
- `capture.log` and one `capture.log.1` backup are limited to 2 MiB each, including oversized logs from previous builds.
- Retains HDR peak-brightness and black-level controls, plus the private-DLSS residual correction for bright edge artifacts.
- Since v0.2.0: selectable DXGI display capture, independent capture delivery, optional 2x/3x/4x DLSS FG, and separate IN/OUT FPS readings. The output FPS target is divided by the FG multiplier to set the real processing limit.
- Retains the FG window-lifecycle compatibility fixes and motion-processing optimizations.

Validation: Release build, isolated log-rotation checks, headless HDR/control and private-DLSS edge checks, and hidden 2x/3x/4x FG integration passed with D3D12 validation. No game was launched for these checks. Live image quality and performance remain dependent on the captured source, GPU and driver.

For troubleshooting only, `NR_CAPTURE_TIMINGS=1` enables detailed pipeline logs and sampled GPU stage timings; logging remains bounded.

This is a binary-only standalone preview. As with previous Display Filter releases, the tag points to an existing OptiScaler repository baseline, not a standalone-source snapshot. Existing OptiScaler game releases are unchanged.

Executable SHA-256: `C0DE3E38AD5E29C5452AA52C7BC5725D8B6A27159B1B46B452E06E182793A3FD`

ZIP SHA-256: `609DB6642F97BCDD74F8E95B684A8F9D6FA5770FBEE8DED6FCE1E36668B8C9AD`
