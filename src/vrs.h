#pragma once

#include <d3d11.h>

#include <cstdint>

namespace xrperf {

struct VrsSettings {
    bool enabled = false;
    float inner = 0.55f; // full rate inside this radius (1.0 = edge of the image, measured on each axis)
    float outer = 0.80f; // 2x2 between inner and outer, 4x4 beyond outer
};

// Fixed foveated rendering through NVIDIA Variable Rate Shading (NVAPI, D3D11).
//
// The layer hooks ID3D11DeviceContext::OMSetRenderTargets on the game's immediate
// context. Whenever the game binds a render target the size of an eye image, VRS is
// switched on with a radial pattern (full rate in the middle, coarser towards the
// edges); for every other target (shadow maps, small buffers, the layer's own
// upscaling) it is switched off.
namespace Vrs {

// Initializes NVAPI, checks VRS support and installs the hook. False if unsupported.
bool Init(ID3D11Device* device);
void Shutdown();

void SetSettings(const VrsSettings& settings);
// Size of the images the game renders per eye; only render targets of this size get VRS
void SetEyeSize(uint32_t width, uint32_t height);
// Turns VRS off on the immediate context (before the layer's own rendering)
void Disable();
// Puts the hook back if something else overwrote it (chaining to whatever took its place)
void Reassert();

// Render target binds that got VRS / all binds since the last call
void TakeStats(uint64_t* vrsBinds, uint64_t* allBinds);

} // namespace Vrs

} // namespace xrperf
