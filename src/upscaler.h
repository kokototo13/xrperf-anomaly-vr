#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <unordered_map>

namespace xrperf {

struct PixelRect {
    int x, y, width, height;
};

enum class UpscaleMethod { Fsr, Nis };

// Spatial upscaling on the game's own D3D11 device: AMD FSR 1 (EASU + RCAS pixel
// shaders) or NVIDIA Image Scaling (one compute shader, then a copy into the output).
// The game's pipeline state is saved and restored around the work with
// SwapDeviceContextState, so the engine's cached state stays valid.
class Upscaler {
  public:
    bool Init(ID3D11Device* device);
    void Shutdown();

    // Upscales inRect of `input` into outRect of `output`. sharpness is 0..1.
    bool Upscale(ID3D11Texture2D* input, const PixelRect& inRect, ID3D11Texture2D* output, const PixelRect& outRect,
                 float sharpness, UpscaleMethod method);

    // GPU time spent in Upscale() since the last call, averaged per frame (ms). Call once per frame
    // around all Upscale() calls: BeginFrame() before, EndFrame() after.
    void BeginFrame();
    void EndFrame();
    bool TakeGpuStats(double* avgMs, double* worstMs);

  private:
    void CollectTimings();
    bool InitNis();
    void RunFsr(ID3D11ShaderResourceView* inSrv, const D3D11_TEXTURE2D_DESC& inDesc, const PixelRect& inRect,
                ID3D11RenderTargetView* outRtv, const PixelRect& outRect, float sharpness);
    void RunNis(ID3D11ShaderResourceView* inSrv, const D3D11_TEXTURE2D_DESC& inDesc, const PixelRect& inRect,
                ID3D11RenderTargetView* outRtv, const D3D11_TEXTURE2D_DESC& outDesc, const PixelRect& outRect,
                float sharpness);
    ID3D11ShaderResourceView* GetSrv(ID3D11Texture2D* texture);
    ID3D11RenderTargetView* GetRtv(ID3D11Texture2D* texture);
    bool EnsureIntermediate(UINT width, UINT height);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device1> m_device;
    ComPtr<ID3D11DeviceContext1> m_context;
    ComPtr<ID3DDeviceContextState> m_state;

    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_easu;
    ComPtr<ID3D11PixelShader> m_rcas;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_rasterizer;

    // NIS
    ComPtr<ID3D11ComputeShader> m_nis;
    ComPtr<ID3D11PixelShader> m_copy;
    ComPtr<ID3D11Buffer> m_nisConstants;
    ComPtr<ID3D11ShaderResourceView> m_nisCoefScale;
    ComPtr<ID3D11ShaderResourceView> m_nisCoefUsm;

    ComPtr<ID3D11Texture2D> m_intermediate;
    ComPtr<ID3D11ShaderResourceView> m_intermediateSrv;
    ComPtr<ID3D11RenderTargetView> m_intermediateRtv;
    ComPtr<ID3D11UnorderedAccessView> m_intermediateUav;

    // Timestamp queries, a few frames in flight so reading them never stalls the GPU
    struct TimingQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    static constexpr int kTimingSlots = 4;
    TimingQuery m_timing[kTimingSlots];
    int m_timingSlot = 0;
    bool m_timingOpen = false;
    double m_gpuSumMs = 0.0;
    double m_gpuWorstMs = 0.0;
    uint64_t m_gpuFrames = 0;

    // Swapchain images never change, so their views are created once
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> m_srvs;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> m_rtvs;
};

} // namespace xrperf
