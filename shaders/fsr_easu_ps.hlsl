// AMD FSR 1 EASU (upscaling) as a pixel shader.
// Renders into the output viewport; outOffset is the viewport origin so that
// FSR sees output pixel coordinates relative to the upscaled image.

cbuffer Constants : register(b0) {
    uint4 const0;
    uint4 const1;
    uint4 const2;
    uint4 const3;
    uint4 outOffset;
};

Texture2D InputTexture : register(t0);
SamplerState LinearClamp : register(s0);

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(LinearClamp, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(LinearClamp, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(LinearClamp, p, int2(0, 0)); }
#include "ffx_fsr1.h"

float4 main(float4 pos : SV_Position) : SV_Target {
    AU2 ip = AU2(pos.xy) - outOffset.xy;
    AF3 color;
    FsrEasuF(color, ip, const0, const1, const2, const3);
    return float4(color, 1);
}
