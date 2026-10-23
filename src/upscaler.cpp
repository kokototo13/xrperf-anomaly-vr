#include "upscaler.h"

#include <algorithm>
#include <cstdint>

#include "log.h"

// FSR constant setup runs on the CPU (the same header provides the GPU code for the shaders)
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include "NIS_Config.h"

#include "copy_ps.h"
#include "fsr_easu_ps.h"
#include "fsr_rcas_ps.h"
#include "fullscreen_vs.h"
#include "nis_cs.h"

#include <cstring>

namespace xrperf {

namespace {

// Swapchain textures are usually TYPELESS so that apps can pick the view format.
// We read and write raw values (UNORM views), keeping the image in the game's
// gamma space, which is also what FSR expects ("perceptual space").
DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

struct EasuConstants {
    AU1 con0[4], con1[4], con2[4], con3[4];
    AU1 outOffset[4];
};

struct RcasConstants {
    AU1 con0[4];
};

} // namespace

bool Upscaler::Init(ID3D11Device* device) {
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_device)))) {
        Log("upscaler: ID3D11Device1 not available");
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(&context);
    if (FAILED(context.As(&m_context))) {
        Log("upscaler: ID3D11DeviceContext1 not available");
        return false;
    }

    D3D_FEATURE_LEVEL level = m_device->GetFeatureLevel();
    UINT stateFlags = (m_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                          ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                          : 0;
    HRESULT hr = m_device->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION,
                                                    __uuidof(ID3D11Device1), nullptr, &m_state);
    if (FAILED(hr)) {
        Log("upscaler: CreateDeviceContextState failed 0x%08lx", hr);
        return false;
    }

    hr = m_device->CreateVertexShader(g_FullscreenVS, sizeof(g_FullscreenVS), nullptr, &m_vs);
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(g_FsrEasuPS, sizeof(g_FsrEasuPS), nullptr, &m_easu);
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(g_FsrRcasPS, sizeof(g_FsrRcasPS), nullptr, &m_rcas);
    if (FAILED(hr)) {
        Log("upscaler: shader creation failed 0x%08lx", hr);
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(EasuConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cb, nullptr, &m_constants);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr))
        hr = m_device->CreateSamplerState(&sd, &m_sampler);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr))
        hr = m_device->CreateRasterizerState(&rd, &m_rasterizer);
    if (FAILED(hr)) {
        Log("upscaler: resource creation failed 0x%08lx", hr);
        return false;
    }

    for (auto& t : m_timing) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        m_device->CreateQuery(&qd, &t.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&qd, &t.begin);
        m_device->CreateQuery(&qd, &t.end);
    }

    Log("upscaler: ready (feature level 0x%x, context state flags 0x%x), NIS %s", level, stateFlags,
        InitNis() ? "available" : "unavailable");
    return true;
}

bool Upscaler::InitNis() {
    HRESULT hr = m_device->CreateComputeShader(g_NisCS, sizeof(g_NisCS), nullptr, &m_nis);
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(g_CopyPS, sizeof(g_CopyPS), nullptr, &m_copy);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(NISConfig);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr))
        hr = m_device->CreateBuffer(&cb, nullptr, &m_nisConstants);

    // Filter coefficient tables from the SDK: kPhaseCount rows of kFilterSize floats,
    // stored as RGBA32F texels (kFilterSize / 4 per row)
    auto makeCoefs = [&](const float (*coefs)[kFilterSize], ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFilterSize / 4;
        td.Height = kPhaseCount;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{coefs, kFilterSize * sizeof(float), 0};
        ComPtr<ID3D11Texture2D> tex;
        HRESULT r = m_device->CreateTexture2D(&td, &data, &tex);
        if (SUCCEEDED(r))
            r = m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        return r;
    };
    if (SUCCEEDED(hr))
        hr = makeCoefs(coef_scale, m_nisCoefScale);
    if (SUCCEEDED(hr))
        hr = makeCoefs(coef_usm, m_nisCoefUsm);

    if (FAILED(hr)) {
        Log("upscaler: NIS setup failed 0x%08lx", hr);
        m_nis.Reset();
        return false;
    }
    return true;
}

void Upscaler::CollectTimings() {
    for (auto& t : m_timing) {
        if (!t.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 begin, end;
        if (m_context->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(t.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(t.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue; // not ready yet
        t.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || end < begin)
            continue;
        double ms = (end - begin) * 1000.0 / dj.Frequency;
        m_gpuSumMs += ms;
        m_gpuWorstMs = std::max(m_gpuWorstMs, ms);
        ++m_gpuFrames;
    }
}

void Upscaler::BeginFrame() {
    if (!m_context || !m_timing[0].disjoint)
        return;
    CollectTimings();
    TimingQuery& t = m_timing[m_timingSlot];
    if (t.pending)
        return; // GPU is far behind, skip measuring this frame
    m_context->Begin(t.disjoint.Get());
    m_context->End(t.begin.Get());
    m_timingOpen = true;
}

void Upscaler::EndFrame() {
    if (!m_timingOpen)
        return;
    TimingQuery& t = m_timing[m_timingSlot];
    m_context->End(t.end.Get());
    m_context->End(t.disjoint.Get());
    t.pending = true;
    m_timingOpen = false;
    m_timingSlot = (m_timingSlot + 1) % kTimingSlots;
}

bool Upscaler::TakeGpuStats(double* avgMs, double* worstMs) {
    if (m_gpuFrames == 0)
        return false;
    *avgMs = m_gpuSumMs / m_gpuFrames;
    *worstMs = m_gpuWorstMs;
    m_gpuSumMs = m_gpuWorstMs = 0.0;
    m_gpuFrames = 0;
    return true;
}

void Upscaler::Shutdown() {
    for (auto& t : m_timing)
        t = TimingQuery{};
    m_timingOpen = false;
    m_srvs.clear();
    m_rtvs.clear();
    m_nisCoefUsm.Reset();
    m_nisCoefScale.Reset();
    m_nisConstants.Reset();
    m_copy.Reset();
    m_nis.Reset();
    m_intermediateUav.Reset();
    m_intermediateRtv.Reset();
    m_intermediateSrv.Reset();
    m_intermediate.Reset();
    m_rasterizer.Reset();
    m_sampler.Reset();
    m_constants.Reset();
    m_rcas.Reset();
    m_easu.Reset();
    m_vs.Reset();
    m_state.Reset();
    m_context.Reset();
    m_device.Reset();
}

ID3D11ShaderResourceView* Upscaler::GetSrv(ID3D11Texture2D* texture) {
    auto it = m_srvs.find(texture);
    if (it != m_srvs.end())
        return it->second.Get();

    D3D11_TEXTURE2D_DESC td;
    texture->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = ViewFormat(td.Format);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = m_device->CreateShaderResourceView(texture, &vd, &srv);
    if (FAILED(hr)) {
        Log("upscaler: CreateShaderResourceView failed 0x%08lx (format %d)", hr, td.Format);
        return nullptr;
    }
    Log("upscaler: input %ux%u format %d, read as %d", td.Width, td.Height, td.Format, vd.Format);
    return (m_srvs[texture] = srv).Get();
}

ID3D11RenderTargetView* Upscaler::GetRtv(ID3D11Texture2D* texture) {
    auto it = m_rtvs.find(texture);
    if (it != m_rtvs.end())
        return it->second.Get();

    D3D11_TEXTURE2D_DESC td;
    texture->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = ViewFormat(td.Format);
    vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = m_device->CreateRenderTargetView(texture, &vd, &rtv);
    if (FAILED(hr)) {
        Log("upscaler: CreateRenderTargetView failed 0x%08lx (format %d)", hr, td.Format);
        return nullptr;
    }
    Log("upscaler: output %ux%u format %d, written as %d", td.Width, td.Height, td.Format, vd.Format);
    return (m_rtvs[texture] = rtv).Get();
}

bool Upscaler::EnsureIntermediate(UINT width, UINT height) {
    if (m_intermediate) {
        D3D11_TEXTURE2D_DESC td;
        m_intermediate->GetDesc(&td);
        if (td.Width >= width && td.Height >= height)
            return true;
        m_intermediateUav.Reset();
        m_intermediateRtv.Reset();
        m_intermediateSrv.Reset();
        m_intermediate.Reset();
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    // Render target for FSR's EASU pass, unordered access for the NIS compute shader
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_intermediate);
    if (SUCCEEDED(hr))
        hr = m_device->CreateUnorderedAccessView(m_intermediate.Get(), nullptr, &m_intermediateUav);
    if (SUCCEEDED(hr))
        hr = m_device->CreateShaderResourceView(m_intermediate.Get(), nullptr, &m_intermediateSrv);
    if (SUCCEEDED(hr))
        hr = m_device->CreateRenderTargetView(m_intermediate.Get(), nullptr, &m_intermediateRtv);
    if (FAILED(hr)) {
        Log("upscaler: intermediate texture %ux%u failed 0x%08lx", width, height, hr);
        m_intermediate.Reset();
        return false;
    }
    return true;
}

bool Upscaler::Upscale(ID3D11Texture2D* input, const PixelRect& inRect, ID3D11Texture2D* output,
                       const PixelRect& outRect, float sharpness, UpscaleMethod method) {
    if (!m_state || (method == UpscaleMethod::Nis && !m_nis))
        return false;

    D3D11_TEXTURE2D_DESC inDesc, outDesc;
    input->GetDesc(&inDesc);
    output->GetDesc(&outDesc);

    ID3D11ShaderResourceView* inSrv = GetSrv(input);
    ID3D11RenderTargetView* outRtv = GetRtv(output);
    if (!inSrv || !outRtv || !EnsureIntermediate(outDesc.Width, outDesc.Height))
        return false;

    // Everything below runs in our own pipeline state; the game's is restored at the end
    ComPtr<ID3DDeviceContextState> gameState;
    m_context->SwapDeviceContextState(m_state.Get(), &gameState);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(outRect.x);
    vp.TopLeftY = static_cast<float>(outRect.y);
    vp.Width = static_cast<float>(outRect.width);
    vp.Height = static_cast<float>(outRect.height);
    vp.MaxDepth = 1.0f;

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->RSSetViewports(1, &vp);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());

    if (method == UpscaleMethod::Nis)
        RunNis(inSrv, inDesc, inRect, outRtv, outDesc, outRect, sharpness);
    else
        RunFsr(inSrv, inDesc, inRect, outRtv, outRect, sharpness);

    m_context->SwapDeviceContextState(gameState.Get(), nullptr);
    return true;
}

void Upscaler::RunNis(ID3D11ShaderResourceView* inSrv, const D3D11_TEXTURE2D_DESC& inDesc, const PixelRect& inRect,
                      ID3D11RenderTargetView* outRtv, const D3D11_TEXTURE2D_DESC& outDesc, const PixelRect& outRect,
                      float sharpness) {
    // Pass 1: NIS compute shader, game image -> intermediate (full output size)
    NISConfig config{};
    NVScalerUpdateConfig(config, sharpness, inRect.x, inRect.y, inRect.width, inRect.height, inDesc.Width,
                         inDesc.Height, outRect.x, outRect.y, outRect.width, outRect.height, outDesc.Width,
                         outDesc.Height, NISHDRMode::None);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_nisConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &config, sizeof(config));
        m_context->Unmap(m_nisConstants.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {inSrv, m_nisCoefScale.Get(), m_nisCoefUsm.Get()};
    m_context->CSSetShader(m_nis.Get(), nullptr, 0);
    m_context->CSSetConstantBuffers(0, 1, m_nisConstants.GetAddressOf());
    m_context->CSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->CSSetShaderResources(0, 3, srvs);
    m_context->CSSetUnorderedAccessViews(0, 1, m_intermediateUav.GetAddressOf(), nullptr);
    // Block size must match NIS_BLOCK_WIDTH / NIS_BLOCK_HEIGHT in nis_cs.hlsl
    m_context->Dispatch((outRect.width + 31) / 32, (outRect.height + 23) / 24, 1);

    ID3D11ShaderResourceView* nullSrvs[3] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    m_context->CSSetShaderResources(0, 3, nullSrvs);
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

    // Pass 2: copy intermediate -> runtime swapchain image
    m_context->OMSetRenderTargets(1, &outRtv, nullptr);
    m_context->PSSetShader(m_copy.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, m_intermediateSrv.GetAddressOf());
    m_context->Draw(3, 0);

    m_context->PSSetShaderResources(0, 1, nullSrvs);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

void Upscaler::RunFsr(ID3D11ShaderResourceView* inSrv, const D3D11_TEXTURE2D_DESC& inDesc, const PixelRect& inRect,
                      ID3D11RenderTargetView* outRtv, const PixelRect& outRect, float sharpness) {
    // Pass 1: EASU, game image -> intermediate (full output size)
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* c = static_cast<EasuConstants*>(mapped.pData);
        FsrEasuConOffset(c->con0, c->con1, c->con2, c->con3, static_cast<AF1>(inRect.width),
                         static_cast<AF1>(inRect.height), static_cast<AF1>(inDesc.Width),
                         static_cast<AF1>(inDesc.Height), static_cast<AF1>(outRect.width),
                         static_cast<AF1>(outRect.height), static_cast<AF1>(inRect.x), static_cast<AF1>(inRect.y));
        c->outOffset[0] = static_cast<AU1>(outRect.x);
        c->outOffset[1] = static_cast<AU1>(outRect.y);
        c->outOffset[2] = c->outOffset[3] = 0;
        m_context->Unmap(m_constants.Get(), 0);
    }
    m_context->OMSetRenderTargets(1, m_intermediateRtv.GetAddressOf(), nullptr);
    m_context->PSSetShader(m_easu.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &inSrv);
    m_context->Draw(3, 0);

    // Pass 2: RCAS, intermediate -> runtime swapchain image
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    if (SUCCEEDED(m_context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* c = static_cast<RcasConstants*>(mapped.pData);
        // RCAS takes "stops" of sharpness reduction: 0 = max, 2 = soft
        FsrRcasCon(c->con0, std::clamp(2.0f - 2.0f * sharpness, 0.0f, 2.0f));
        m_context->Unmap(m_constants.Get(), 0);
    }
    m_context->OMSetRenderTargets(1, &outRtv, nullptr);
    m_context->PSSetShader(m_rcas.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, m_intermediateSrv.GetAddressOf());
    m_context->Draw(3, 0);

    m_context->PSSetShaderResources(0, 1, &nullSrv);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace xrperf
