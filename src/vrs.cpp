#include "vrs.h"

#include <windows.h>
#include <wrl/client.h>

#include <nvapi.h>

#include <atomic>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace xrperf::Vrs {

namespace {

using Microsoft::WRL::ComPtr;

// ID3D11DeviceContext vtable slots
constexpr int kOMSetRenderTargets = 33;
constexpr int kOMSetRenderTargetsAndUnorderedAccessViews = 34;

using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                         ID3D11DepthStencilView*);
using PFN_OMSetRenderTargetsAndUAVs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                                ID3D11RenderTargetView* const*,
                                                                ID3D11DepthStencilView*, UINT, UINT,
                                                                ID3D11UnorderedAccessView* const*, const UINT*);

struct State {
    bool ready = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    void** vtable = nullptr;
    // What our hooks call next: d3d11 itself, or another hook that took the slot after us
    PFN_OMSetRenderTargets origSetRenderTargets = nullptr;
    PFN_OMSetRenderTargetsAndUAVs origSetRenderTargetsAndUavs = nullptr;
    // d3d11's own functions, used if a chained hook ends up calling back into us
    PFN_OMSetRenderTargets d3dSetRenderTargets = nullptr;
    PFN_OMSetRenderTargetsAndUAVs d3dSetRenderTargetsAndUavs = nullptr;
    int reassertCount = 0;

    VrsSettings settings;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;

    // Shading rate image: one texel per 16x16 pixel tile, holding an index into the rate table
    ComPtr<ID3D11Texture2D> rateTexture;
    ID3D11NvShadingRateResourceView* rateView = nullptr;
    bool patternDirty = true;

    // The engine may record draws on deferred contexts from worker threads, so VRS state is
    // kept per context and everything shared is behind a lock
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<ID3D11DeviceContext*, bool> active; // VRS currently on, per context
    bool loggedDeferredError = false;

    std::atomic<uint64_t> vrsBinds{0};
    std::atomic<uint64_t> allBinds{0};
    std::atomic<uint64_t> deferredBinds{0};
    std::atomic<uint64_t> otherDeviceBinds{0};
    std::atomic<uint64_t> hookCalls{0}; // every call that reached the hook, before any filtering
};

State s;

// Render target views are looked up once per thread in a row; cache the last answer
thread_local ID3D11RenderTargetView* t_lastRtv = nullptr;
thread_local bool t_lastRtvIsEye = false;
thread_local uint32_t t_lastEyeWidth = 0;
// Re-entrancy guard: if another hook we chain to calls back into us, go straight to d3d11
thread_local int t_hookDepth = 0;

bool BuildPattern() {
    if (s.rateView) {
        s.rateView->Release();
        s.rateView = nullptr;
    }
    s.rateTexture.Reset();

    const UINT w = (s.eyeWidth + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
    const UINT h = (s.eyeHeight + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
    std::vector<uint8_t> rates(static_cast<size_t>(w) * h);
    size_t counts[3] = {};
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            // -1..1 across the image on each axis, so the zones follow the image's aspect ratio
            float nx = ((x + 0.5f) * NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH) / s.eyeWidth * 2.0f - 1.0f;
            float ny = ((y + 0.5f) * NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT) / s.eyeHeight * 2.0f - 1.0f;
            float r = std::sqrt(nx * nx + ny * ny);
            uint8_t rate = r < s.settings.inner ? 0 : r < s.settings.outer ? 1 : 2;
            rates[static_cast<size_t>(y) * w + x] = rate;
            ++counts[rate];
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rates.data(), w, 0};
    HRESULT hr = s.device->CreateTexture2D(&td, &data, &s.rateTexture);
    if (FAILED(hr)) {
        Log("VRS: shading rate texture %ux%u failed 0x%08lx", w, h, hr);
        return false;
    }

    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    NvAPI_Status st = NvAPI_D3D11_CreateShadingRateResourceView(s.device.Get(), s.rateTexture.Get(), &vd, &s.rateView);
    if (st != NVAPI_OK) {
        Log("VRS: CreateShadingRateResourceView failed (%d)", st);
        s.rateTexture.Reset();
        return false;
    }

    size_t total = rates.size();
    Log("VRS: pattern for %ux%u eye (inner %.2f, outer %.2f): full rate %.0f%%, 2x2 %.0f%%, 4x4 %.0f%% of the image",
        s.eyeWidth, s.eyeHeight, s.settings.inner, s.settings.outer, 100.0 * counts[0] / total,
        100.0 * counts[1] / total, 100.0 * counts[2] / total);
    s.patternDirty = false;
    return true;
}

// Called with s.lock held exclusively
void Apply(ID3D11DeviceContext* ctx, bool on) {
    if (on && (s.patternDirty || !s.rateView)) {
        if (!BuildPattern())
            on = false;
    }
    bool& active = s.active[ctx];
    if (on == active)
        return;

    NvAPI_Status st = NVAPI_OK;
    if (on) {
        NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        for (auto& vp : viewports) {
            vp.enableVariablePixelShadingRate = true;
            for (auto& rate : vp.shadingRateTable)
                rate = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vp.shadingRateTable[1] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
            vp.shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        }
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
        desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        desc.numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        desc.pViewports = viewports;
        st = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, s.rateView);
        if (st == NVAPI_OK)
            st = NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &desc);
    } else {
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
        desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        desc.numViewports = 0; // disables VRS for all viewports
        st = NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &desc);
        NvAPI_D3D11_RSSetShadingRateResourceView(ctx, nullptr);
    }
    if (st != NVAPI_OK && !s.loggedDeferredError) {
        s.loggedDeferredError = true;
        Log("VRS: NVAPI refused to set shading rates on context %p (%s): %d", static_cast<void*>(ctx),
            ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate", st);
    }
    active = on && st == NVAPI_OK;
}

bool IsEyeTarget(ID3D11RenderTargetView* rtv) {
    if (rtv == t_lastRtv && t_lastEyeWidth == s.eyeWidth)
        return t_lastRtvIsEye;
    bool eye = false;
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td;
        static_cast<ID3D11Texture2D*>(res.Get())->GetDesc(&td);
        // MSAA targets included: with the game's MSAA on, its main passes render multisampled
        // (the driver lowers rates that would exceed what it supports for that sample count)
        eye = td.Width == s.eyeWidth && td.Height == s.eyeHeight;
    }
    t_lastRtv = rtv;
    t_lastRtvIsEye = eye;
    t_lastEyeWidth = s.eyeWidth;
    return eye;
}

// Contexts of other devices (if the engine has any) are left alone
bool IsGameContext(ID3D11DeviceContext* ctx) {
    if (ctx == s.context.Get())
        return true;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (dev.Get() != s.device.Get()) {
        ++s.otherDeviceBinds;
        return false;
    }
    return true;
}

void OnBind(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs) {
    ++s.hookCalls;
    if (!s.ready || !IsGameContext(ctx))
        return;
    ++s.allBinds;
    if (ctx != s.context.Get())
        ++s.deferredBinds;
    bool eye = s.settings.enabled && s.eyeWidth && count > 0 && rtvs && rtvs[0] && IsEyeTarget(rtvs[0]);
    if (eye)
        ++s.vrsBinds;
    AcquireSRWLockExclusive(&s.lock);
    Apply(ctx, eye);
    ReleaseSRWLockExclusive(&s.lock);
}

void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT count,
                                               ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (t_hookDepth > 0) {
        s.d3dSetRenderTargets(ctx, count, rtvs, dsv);
        return;
    }
    ++t_hookDepth;
    s.origSetRenderTargets(ctx, count, rtvs, dsv);
    --t_hookDepth;
    OnBind(ctx, count, rtvs);
}

void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* ctx, UINT count,
                                                      ID3D11RenderTargetView* const* rtvs,
                                                      ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                                      ID3D11UnorderedAccessView* const* uavs,
                                                      const UINT* initialCounts) {
    if (t_hookDepth > 0) {
        s.d3dSetRenderTargetsAndUavs(ctx, count, rtvs, dsv, uavStart, uavCount, uavs, initialCounts);
        return;
    }
    ++t_hookDepth;
    s.origSetRenderTargetsAndUavs(ctx, count, rtvs, dsv, uavStart, uavCount, uavs, initialCounts);
    --t_hookDepth;
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        OnBind(ctx, count, rtvs);
}

std::string ModuleOf(void* address) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    return name;
}

void PatchSlot(int slot, void* hook, void** original) {
    DWORD old;
    VirtualProtect(&s.vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (original)
        *original = s.vtable[slot];
    s.vtable[slot] = hook;
    VirtualProtect(&s.vtable[slot], sizeof(void*), old, &old);
}

} // namespace

bool Init(ID3D11Device* device) {
    Shutdown();

    NvAPI_Status st = NvAPI_Initialize();
    if (st != NVAPI_OK) {
        Log("VRS: NVAPI not available (%d), foveated rendering off", st);
        return false;
    }
    NV_D3D1x_GRAPHICS_CAPS caps{};
    st = NvAPI_D3D1x_GetGraphicsCapabilities(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    if (st != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
        Log("VRS: not supported by this GPU/driver (status %d), foveated rendering off", st);
        return false;
    }

    s.device = device;
    device->GetImmediateContext(&s.context);

    // The hook stays in the context's vtable until Shutdown; keep this dll loaded no matter
    // what, so a missed Shutdown can never leave the game calling into unloaded code
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&Init), &self);

    s.vtable = *reinterpret_cast<void***>(s.context.Get());
    PatchSlot(kOMSetRenderTargets, reinterpret_cast<void*>(&Hook_OMSetRenderTargets),
              reinterpret_cast<void**>(&s.origSetRenderTargets));
    PatchSlot(kOMSetRenderTargetsAndUnorderedAccessViews, reinterpret_cast<void*>(&Hook_OMSetRenderTargetsAndUAVs),
              reinterpret_cast<void**>(&s.origSetRenderTargetsAndUavs));
    s.d3dSetRenderTargets = s.origSetRenderTargets;
    s.d3dSetRenderTargetsAndUavs = s.origSetRenderTargetsAndUavs;
    Log("VRS: OMSetRenderTargets was %s", ModuleOf(reinterpret_cast<void*>(s.origSetRenderTargets)).c_str());
    s.ready = true;
    Log("VRS: supported, render target hook installed");
    return true;
}

void Shutdown() {
    if (s.ready) {
        AcquireSRWLockExclusive(&s.lock);
        Apply(s.context.Get(), false);
        s.active.clear();
        ReleaseSRWLockExclusive(&s.lock);
        // Only undo our own patch; someone may have hooked on top of us since
        if (s.vtable[kOMSetRenderTargets] == reinterpret_cast<void*>(&Hook_OMSetRenderTargets))
            PatchSlot(kOMSetRenderTargets, reinterpret_cast<void*>(s.origSetRenderTargets), nullptr);
        if (s.vtable[kOMSetRenderTargetsAndUnorderedAccessViews] ==
            reinterpret_cast<void*>(&Hook_OMSetRenderTargetsAndUAVs))
            PatchSlot(kOMSetRenderTargetsAndUnorderedAccessViews,
                      reinterpret_cast<void*>(s.origSetRenderTargetsAndUavs), nullptr);
    }
    if (s.rateView) {
        s.rateView->Release();
        s.rateView = nullptr;
    }
    s.rateTexture.Reset();
    s.context.Reset();
    s.device.Reset();
    s.vtable = nullptr;
    s.ready = false;
    s.patternDirty = true;
    s.eyeWidth = s.eyeHeight = 0;
}

void SetSettings(const VrsSettings& settings) {
    AcquireSRWLockExclusive(&s.lock);
    if (settings.inner != s.settings.inner || settings.outer != s.settings.outer)
        s.patternDirty = true;
    s.settings = settings;
    if (s.ready && !settings.enabled)
        Apply(s.context.Get(), false); // deferred contexts switch off at their next bind
    ReleaseSRWLockExclusive(&s.lock);
}

void SetEyeSize(uint32_t width, uint32_t height) {
    if (width == s.eyeWidth && height == s.eyeHeight)
        return;
    AcquireSRWLockExclusive(&s.lock);
    s.eyeWidth = width;
    s.eyeHeight = height;
    s.patternDirty = true;
    ReleaseSRWLockExclusive(&s.lock);
    Log("VRS: eye render size %ux%u", width, height);
}

// SwapDeviceContextState (used by the upscaler) makes d3d11 rewrite the context's vtable,
// which drops the hook every frame; this puts it back right after
void Reassert() {
    if (!s.ready)
        return;
    auto check = [](int slot, void* hook, auto& next) {
        void* current = s.vtable[slot];
        if (current == hook)
            return;
        // d3d11 restoring its own function is expected; anything else is another hook to chain to
        bool d3d = current == reinterpret_cast<void*>(s.d3dSetRenderTargets) ||
                   current == reinterpret_cast<void*>(s.d3dSetRenderTargetsAndUavs);
        if (++s.reassertCount <= 2 || !d3d)
            Log("VRS: render target hook (slot %d) reset by %s, putting it back%s", slot, ModuleOf(current).c_str(),
                s.reassertCount == 2 ? " (logged once more, then silent)" : "");
        using T = std::remove_reference_t<decltype(next)>;
        next = reinterpret_cast<T>(current);
        PatchSlot(slot, hook, nullptr);
    };
    check(kOMSetRenderTargets, reinterpret_cast<void*>(&Hook_OMSetRenderTargets), s.origSetRenderTargets);
    check(kOMSetRenderTargetsAndUnorderedAccessViews, reinterpret_cast<void*>(&Hook_OMSetRenderTargetsAndUAVs),
          s.origSetRenderTargetsAndUavs);
}

void Disable() {
    if (!s.ready)
        return;
    AcquireSRWLockExclusive(&s.lock);
    Apply(s.context.Get(), false);
    ReleaseSRWLockExclusive(&s.lock);
}

void TakeStats(uint64_t* vrsBinds, uint64_t* allBinds) {
    *vrsBinds = s.vrsBinds.exchange(0);
    *allBinds = s.allBinds.exchange(0);
    uint64_t deferred = s.deferredBinds.exchange(0);
    uint64_t other = s.otherDeviceBinds.exchange(0);
    uint64_t calls = s.hookCalls.exchange(0);
    bool hooked = s.vtable && s.vtable[kOMSetRenderTargets] == reinterpret_cast<void*>(&Hook_OMSetRenderTargets);
    Log("  VRS hook: %llu calls (%llu from deferred contexts, %llu from other devices), hook %s",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(deferred),
        static_cast<unsigned long long>(other), hooked ? "in place" : "REPLACED by someone else");
}

} // namespace xrperf::Vrs
