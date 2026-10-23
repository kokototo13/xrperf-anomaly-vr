// AMD FSR 1 RCAS (sharpening) as a pixel shader. Input and output share pixel
// coordinates (both are full output size), so SV_Position is used as-is.

cbuffer Constants : register(b0) {
    uint4 const0;
};

Texture2D InputTexture : register(t0);

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

float4 main(float4 pos : SV_Position) : SV_Target {
    AF3 color;
    FsrRcasF(color.r, color.g, color.b, AU2(pos.xy), const0);
    return float4(color, 1);
}
