// NVIDIA Image Scaling (NIS) upscaler + sharpener, compute shader.
// Uses NVIDIA's NIS_Main.hlsl as is; block / thread group sizes are the SDK's
// recommended values for NVIDIA GPUs (must match the dispatch in upscaler.cpp).
#define NIS_SCALER 1
#define NIS_HDR_MODE 0
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 24
#define NIS_THREAD_GROUP_SIZE 128
#define NIS_USE_HALF_PRECISION 0
#include "NIS_Main.hlsl"
