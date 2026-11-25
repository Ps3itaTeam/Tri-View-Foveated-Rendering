# AMD FSR API headers

These four headers come unchanged from AMD's FidelityFX SDK 2.4.0 (FSR 4.1.2), commit 7539ae8 of
<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, folder `Kits/FidelityFX`:

- `api/include/ffx_api.h`
- `api/include/ffx_api_types.h`
- `api/include/dx12/ffx_api_dx12.h`
- `upscalers/include/ffx_upscale.h`

Each file carries AMD's MIT license notice at its top. The folder layout is the SDK's, so the relative includes inside
`ffx_upscale.h` work as they are.

The mod uses them only to call FSR 4 in the fovea itself (`d3d12::FsrDirect`): the functions are taken at run time from
the FSR DLLs in the game folder (`amd_fidelityfx_loader_dx12.dll` or `amd_fidelityfx_upscaler_dx12.dll`); nothing is
linked and no AMD binary is part of the mod.
