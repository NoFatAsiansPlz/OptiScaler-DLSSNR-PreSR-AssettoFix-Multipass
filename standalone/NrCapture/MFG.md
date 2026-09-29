# Built-in RTX 40 Multi Frame Generation

Select **3x** or **4x** in Display Filter and start capture. On a verified Ada GPU, the executable prepares the MFG unlock automatically before the first frame-generation feature is created. No separate unlock DLL, ReShade, ASI loader, or NVIDIA App setting change is required. The existing NVIDIA runtime files are still required.

The implementation reuses the repository's MFG temporal-kernel, provider, wrapper-capacity and software-pacing helpers. It corrects Ada's fixed midpoint interpolation as well as opening the frame-count limits. The selected D3D12 adapter is matched by LUID and NVAPI architecture; RTX 50-series continues using native frame generation.

Only recognized loaded provider/plugin images are modified, inside Display Filter's own process. Both provider gates must match uniquely, every recognized temporal descriptor must be redirected successfully, and the active Streamline wrapper's capacity and software-pacing layout must be recognized. An unsupported or incomplete match leaves the verified limit at 2x; select 2x and inspect `capture.log` for the reason. The app never claims a higher multiplier merely because a patch was requested.

Off/2x do not initiate the unlock. Once prepared, the provider and its rebuilt temporal descriptors remain owned until process exit. Capture restarts revalidate the current Streamline plugin; the plugin is allowed to unload normally. Restart the app after changing runtime versions.

## Validation, 2026-09-22

- Build passed with Visual Studio 2022.
- Host-only regression checks passed for legacy/310.9 gate patterns, missing/duplicate patterns, failed protection preparation without byte changes, and session eligibility reset.
- The bundled and NVIDIA OTA DLSS-G providers each matched eight temporal descriptors; the rebuilt PTX redirected all eight in isolated mappings without executing vendor code.
- Bundled and OTA Streamline wrappers matched their frame-count and software-pacing sites.
- The existing provider, ceiling, PTX and pacing helper tests passed.
- The RTX 5090 integration test checks that the Ada unlock is skipped and native FG continues through repeated capture restarts, including NR compatibility, two passes, private DLSS, resizing and 2x/3x/4x presentation.

RTX 40-series hardware is not available on this machine. Actual Ada frame quality, pacing and stability require hardware validation; mapped-image tests and frame counters do not establish those results.

Credits and licenses are in `MFGUnlock_LICENSE.txt`. Source helpers derive from the OptiScaler MFG implementation and the work credited there, including dashdogy's RTX40MFG-Unlock and the RenoDX MFG addon.
