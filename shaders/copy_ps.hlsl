// Copies the intermediate texture into the output viewport pixel for pixel
// (input and output share coordinates, both are full output size)
Texture2D InputTexture : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target {
    return float4(InputTexture.Load(int3(pos.xy, 0)).rgb, 1);
}
