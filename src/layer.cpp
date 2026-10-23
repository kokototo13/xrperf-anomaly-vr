// xrperf: OpenXR API layer for S.T.A.L.K.E.R. Anomaly VR.
//
// The OpenXR loader puts this dll between the game and the runtime (SteamVR).
// Every call we care about goes through a Layer_* function here first, then to
// the next layer / runtime through the pointers stored in g.
//
// Upscaling: the game is told to render at render_scale of the headset's resolution.
// For every eye swapchain the game creates, the layer creates a second, full-size
// swapchain. In xrEndFrame the game's image is upscaled with FSR or NIS into that swapchain,
// and the submitted layers are rewritten to point at it.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "gpu_timer.h"
#include "log.h"
#include "upscaler.h"

using namespace xrperf;

#define LAYER_NAME "XR_APILAYER_XRPERF_anomaly"

namespace {

struct SwapchainInfo {
    XrSwapchainCreateInfo info;
    std::vector<ID3D11Texture2D*> images;
    std::deque<uint32_t> acquired; // acquired but not yet released, oldest first
    int lastReleased = -1;         // the image the game finished, i.e. the one it is about to submit

    // Full-size swapchain created by the layer; submitted instead of the game's
    XrSwapchain output = XR_NULL_HANDLE;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    std::vector<ID3D11Texture2D*> outputImages;
};

struct State {
    Config cfg;
    bool hooked = false;

    XrInstance instance = XR_NULL_HANDLE;
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance nextDestroyInstance = nullptr;
    PFN_xrEnumerateViewConfigurationViews nextEnumerateViewConfigurationViews = nullptr;
    PFN_xrCreateSession nextCreateSession = nullptr;
    PFN_xrDestroySession nextDestroySession = nullptr;
    PFN_xrCreateSwapchain nextCreateSwapchain = nullptr;
    PFN_xrDestroySwapchain nextDestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages nextEnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage nextAcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage nextWaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage nextReleaseSwapchainImage = nullptr;
    PFN_xrWaitFrame nextWaitFrame = nullptr;
    PFN_xrBeginFrame nextBeginFrame = nullptr;
    PFN_xrEndFrame nextEndFrame = nullptr;

    std::mutex swapchainMutex;
    std::unordered_map<XrSwapchain, SwapchainInfo> swapchains;

    // Upscaling
    uint32_t maxImageWidth = 0;
    uint32_t maxImageHeight = 0;
    uint32_t recommendedWidth = 0; // the runtime's own recommendation, before render_scale
    uint32_t recommendedHeight = 0;
    ID3D11Device* device = nullptr; // the game's device, AddRef'd
    Upscaler upscaler;

    // Game GPU time per frame. Only measured when xrBeginFrame comes from the thread
    // that calls xrEndFrame (the render thread), since the D3D11 context is not thread-safe.
    GpuFrameTimer gpuTimer;
    DWORD renderThread = 0;
    bool loggedBeginThread = false;
    bool upscaling = false; // the upscaler is set up and swapchains get a full-size twin
    bool loggedFirstUpscale = false;
    uint64_t upscaleFailures = 0;

    // xrperf.ini is re-read when it changes, so the upscaler can be switched in game
    ULONGLONG lastConfigCheck = 0;
    FILETIME configWriteTime{};

    bool loggedViews = false;
    std::string lastLayerSignature;
    int layerLogsLeft = 20;

    // frame timing, measured between consecutive xrEndFrame calls
    double qpcToMs = 0.0;
    LONGLONG lastEndFrame = 0;
    LONGLONG windowStart = 0;
    uint64_t windowFrames = 0;
    uint64_t windowSlowFrames = 0;
    double windowSumMs = 0.0;
    double windowWorstMs = 0.0;
    double displayPeriodMs = 0.0;

    // Whole-session summary (written at xrDestroySession). Frames that submit no
    // layers (loading screens) are left out.
    std::vector<float> sessionFrameMs;
    uint64_t sessionMissed = 0;
    double sessionReducedRateMs = 0.0; // time the runtime ran below the headset's normal rate
    double baseDisplayPeriodMs = 0.0;  // shortest display period seen = the headset's normal rate
};

State g;

const char* UpscalerName() {
    switch (g.cfg.upscaler) {
    case Config::Upscaler::Fsr: return "FSR";
    case Config::Upscaler::Nis: return "NIS";
    default: return "none";
    }
}

const char* DxgiFormatName(int64_t f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    default: return "?";
    }
}

std::string Narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

template <typename T>
void Resolve(const char* name, T& out) {
    out = nullptr;
    g.nextGetInstanceProcAddr(g.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out));
}

void LogAdapter(ID3D11Device* device) {
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        Log("D3D11 device on GPU: %s (VRAM %llu MB), feature level 0x%x", Narrow(desc.Description).c_str(),
            static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)), device->GetFeatureLevel());
        adapter->Release();
    }
    dxgiDevice->Release();
}

void LogFrameLayers(const XrFrameEndInfo* info) {
    // Only log when the set of submitted layers changes (e.g. menu vs gameplay)
    std::string sig;
    for (uint32_t i = 0; i < info->layerCount; ++i)
        sig += std::to_string(info->layers[i]->type) + ",";
    if (sig == g.lastLayerSignature || g.layerLogsLeft <= 0)
        return;
    g.lastLayerSignature = sig;
    --g.layerLogsLeft;

    Log("xrEndFrame: %u layer(s), blend mode %d", info->layerCount, info->environmentBlendMode);
    std::lock_guard<std::mutex> lock(g.swapchainMutex);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            Log("  [%u] projection, %u view(s), flags 0x%llx", i, proj->viewCount,
                static_cast<unsigned long long>(proj->layerFlags));
            for (uint32_t v = 0; v < proj->viewCount; ++v) {
                const XrSwapchainSubImage& sub = proj->views[v].subImage;
                auto it = g.swapchains.find(sub.swapchain);
                bool hasDepth = false;
                for (auto* n = reinterpret_cast<const XrBaseInStructure*>(proj->views[v].next); n; n = n->next)
                    hasDepth |= n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                Log("      eye %u: swapchain %p (%ux%u, array %u) rect %d,%d %dx%d slice %u%s", v,
                    static_cast<void*>(sub.swapchain), it != g.swapchains.end() ? it->second.info.width : 0,
                    it != g.swapchains.end() ? it->second.info.height : 0,
                    it != g.swapchains.end() ? it->second.info.arraySize : 0, sub.imageRect.offset.x,
                    sub.imageRect.offset.y, sub.imageRect.extent.width, sub.imageRect.extent.height,
                    sub.imageArrayIndex, hasDepth ? ", + depth" : "");
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(base);
            Log("  [%u] quad %.2fx%.2f m, rect %dx%d", i, quad->size.width, quad->size.height,
                quad->subImage.imageRect.extent.width, quad->subImage.imageRect.extent.height);
        } else {
            Log("  [%u] layer type %d", i, base->type);
        }
    }
}

void UpdateFrameStats(bool hasLayers) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g.lastEndFrame == 0) {
        g.lastEndFrame = g.windowStart = now.QuadPart;
        return;
    }
    double ms = (now.QuadPart - g.lastEndFrame) * g.qpcToMs;
    g.lastEndFrame = now.QuadPart;

    if (hasLayers && g.displayPeriodMs > 0.0) {
        if (g.baseDisplayPeriodMs == 0.0 || g.displayPeriodMs < g.baseDisplayPeriodMs)
            g.baseDisplayPeriodMs = g.displayPeriodMs;
        g.sessionFrameMs.push_back(static_cast<float>(ms));
        if (ms > g.displayPeriodMs * 1.5)
            ++g.sessionMissed;
        if (g.displayPeriodMs > g.baseDisplayPeriodMs * 1.25)
            g.sessionReducedRateMs += ms;
    }

    ++g.windowFrames;
    g.windowSumMs += ms;
    g.windowWorstMs = std::max(g.windowWorstMs, ms);
    if (g.displayPeriodMs > 0.0 && ms > g.displayPeriodMs * 1.5)
        ++g.windowSlowFrames;

    double windowMs = (now.QuadPart - g.windowStart) * g.qpcToMs;
    if (g.cfg.statsInterval <= 0.0f || windowMs < g.cfg.statsInterval * 1000.0)
        return;

    double avg = g.windowSumMs / g.windowFrames;
    Log("frames: %.1f fps, avg %.2f ms, worst %.2f ms, missed vsync %llu/%llu (headset %.1f Hz)", 1000.0 / avg, avg,
        g.windowWorstMs, static_cast<unsigned long long>(g.windowSlowFrames),
        static_cast<unsigned long long>(g.windowFrames), g.displayPeriodMs > 0.0 ? 1000.0 / g.displayPeriodMs : 0.0);
    double gameAvg, gameWorst;
    if (g.gpuTimer.TakeWindow(&gameAvg, &gameWorst))
        Log("  game GPU: avg %.2f ms, worst %.2f ms per frame (budget %.2f ms)", gameAvg, gameWorst,
            g.displayPeriodMs);
    double fsrAvg, fsrWorst;
    if (g.upscaling && g.upscaler.TakeGpuStats(&fsrAvg, &fsrWorst))
        Log("  %s GPU cost: avg %.3f ms, worst %.3f ms per frame (both eyes)", UpscalerName(), fsrAvg, fsrWorst);

    g.windowStart = now.QuadPart;
    g.windowFrames = g.windowSlowFrames = 0;
    g.windowSumMs = g.windowWorstMs = 0.0;
}

// ---------------------------------------------------------------------------
// Upscaling helpers

std::vector<ID3D11Texture2D*> EnumerateImages(XrSwapchain swapchain) {
    uint32_t count = 0;
    if (XR_FAILED(g.nextEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0)
        return {};
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(g.nextEnumerateSwapchainImages(swapchain, count, &count,
                                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        return {};
    std::vector<ID3D11Texture2D*> textures;
    for (auto& image : images)
        textures.push_back(image.texture);
    return textures;
}

// Eye images are plain single-sample color swapchains; depth and the like are left alone
bool IsEyeColorSwapchain(const XrSwapchainCreateInfo& info) {
    return (info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
           !(info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && info.arraySize == 1 &&
           info.faceCount == 1 && info.sampleCount == 1 && info.mipCount == 1;
}

// Creates the full-size twin of a game swapchain. Called with swapchainMutex held.
void CreateOutputSwapchain(XrSession session, SwapchainInfo& sc) {
    XrSwapchainCreateInfo ci = sc.info;
    ci.next = nullptr;
    ci.createFlags = 0;
    ci.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    // Dividing back by render_scale can be off by a pixel, so if the game used the size we
    // handed out, go straight back to the runtime's recommendation
    auto fullSize = [](uint32_t size, uint32_t recommended, uint32_t maxSize) {
        if (recommended && std::abs(std::lround(recommended * g.cfg.renderScale) - static_cast<long>(size)) <= 1)
            return recommended;
        return std::min<uint32_t>(static_cast<uint32_t>(std::lround(size / g.cfg.renderScale)),
                                  maxSize ? maxSize : UINT32_MAX);
    };
    ci.width = fullSize(sc.info.width, g.recommendedWidth, g.maxImageWidth);
    ci.height = fullSize(sc.info.height, g.recommendedHeight, g.maxImageHeight);

    XrResult r = g.nextCreateSwapchain(session, &ci, &sc.output);
    if (XR_FAILED(r)) {
        Log("  output swapchain %ux%u failed: %d, this eye will not be upscaled", ci.width, ci.height, r);
        sc.output = XR_NULL_HANDLE;
        return;
    }
    sc.outputWidth = ci.width;
    sc.outputHeight = ci.height;
    sc.outputImages = EnumerateImages(sc.output);
    Log("  -> upscaled to %ux%u in our swapchain %p (%zu images)", ci.width, ci.height,
        static_cast<void*>(sc.output), sc.outputImages.size());
}

// Rewrites projection views that point at an upscaled game swapchain. The storage
// vectors keep the patched structs alive until the runtime's xrEndFrame returns.
bool UpscaleFrame(const XrFrameEndInfo* info, std::vector<const XrCompositionLayerBaseHeader*>& layers,
                  std::vector<XrCompositionLayerProjection>& projections,
                  std::vector<std::vector<XrCompositionLayerProjectionView>>& views) {
    std::lock_guard<std::mutex> lock(g.swapchainMutex);

    // Each output swapchain is acquired once per frame, even if both eyes share a game swapchain
    std::unordered_map<XrSwapchain, uint32_t> acquiredOutputs;
    bool patched = false;
    g.upscaler.BeginFrame();

    layers.assign(info->layers, info->layers + info->layerCount);
    projections.reserve(info->layerCount);
    views.reserve(info->layerCount);

    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        projections.push_back(*proj);
        views.emplace_back(proj->views, proj->views + proj->viewCount);
        projections.back().views = views.back().data();
        layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());

        for (auto& view : views.back()) {
            auto it = g.swapchains.find(view.subImage.swapchain);
            if (it == g.swapchains.end())
                continue;
            SwapchainInfo& sc = it->second;
            if (!sc.output || sc.lastReleased < 0 || sc.lastReleased >= static_cast<int>(sc.images.size()) ||
                sc.outputImages.empty())
                continue;

            auto acq = acquiredOutputs.find(sc.output);
            if (acq == acquiredOutputs.end()) {
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(g.nextAcquireSwapchainImage(sc.output, &ai, &index)))
                    continue;
                if (XR_FAILED(g.nextWaitSwapchainImage(sc.output, &wi))) {
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    g.nextReleaseSwapchainImage(sc.output, &ri);
                    continue;
                }
                acq = acquiredOutputs.emplace(sc.output, index).first;
            }

            const XrRect2Di& in = view.subImage.imageRect;
            double sx = static_cast<double>(sc.outputWidth) / sc.info.width;
            double sy = static_cast<double>(sc.outputHeight) / sc.info.height;
            PixelRect inRect{in.offset.x, in.offset.y, in.extent.width, in.extent.height};
            PixelRect outRect{static_cast<int>(std::lround(in.offset.x * sx)),
                              static_cast<int>(std::lround(in.offset.y * sy)),
                              static_cast<int>(std::lround(in.extent.width * sx)),
                              static_cast<int>(std::lround(in.extent.height * sy))};

            UpscaleMethod method =
                g.cfg.upscaler == Config::Upscaler::Nis ? UpscaleMethod::Nis : UpscaleMethod::Fsr;
            if (!g.upscaler.Upscale(sc.images[sc.lastReleased], inRect, sc.outputImages[acq->second], outRect,
                                    g.cfg.sharpness, method)) {
                ++g.upscaleFailures;
                continue;
            }
            if (!g.loggedFirstUpscale) {
                g.loggedFirstUpscale = true;
                Log("first upscaled frame: %dx%d -> %dx%d", inRect.width, inRect.height, outRect.width,
                    outRect.height);
            }
            view.subImage.swapchain = sc.output;
            view.subImage.imageRect = {{outRect.x, outRect.y}, {outRect.width, outRect.height}};
            view.subImage.imageArrayIndex = 0;
            patched = true;
        }
    }

    g.upscaler.EndFrame();
    for (auto& [swapchain, index] : acquiredOutputs) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        g.nextReleaseSwapchainImage(swapchain, &ri);
    }
    return patched;
}

FILETIME ConfigWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    GetFileAttributesExW(g.cfg.path.c_str(), GetFileExInfoStandard, &attr);
    return attr.ftLastWriteTime;
}

// Picks up upscaler / sharpness changes from xrperf.ini (FSR.bat, NIS.bat, OFF.bat).
// render_scale is not applied here: the swapchains were sized when the game started.
void CheckConfigReload() {
    ULONGLONG now = GetTickCount64();
    if (now - g.lastConfigCheck < 1000)
        return;
    g.lastConfigCheck = now;

    FILETIME t = ConfigWriteTime();
    if (CompareFileTime(&t, &g.configWriteTime) == 0)
        return;
    g.configWriteTime = t;

    Config fresh = LoadConfig();
    if (fresh.upscaler == g.cfg.upscaler && fresh.sharpness == g.cfg.sharpness)
        return;
    g.cfg.upscaler = fresh.upscaler;
    g.cfg.sharpness = fresh.sharpness;
    Log("config reloaded: upscaler %s, sharpness %.2f%s", UpscalerName(), g.cfg.sharpness,
        fresh.renderScale != g.cfg.renderScale ? " (render_scale change applies on next game start)" : "");
}

void ShutdownUpscaling() {
    g.gpuTimer.Shutdown();
    g.renderThread = 0;
    g.loggedBeginThread = false;
    g.upscaler.Shutdown();
    if (g.device) {
        g.device->Release();
        g.device = nullptr;
    }
    g.upscaling = false;
}

// ---------------------------------------------------------------------------
// Hooked OpenXR functions

XrResult XRAPI_CALL Layer_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                            XrViewConfigurationType viewConfigurationType,
                                                            uint32_t viewCapacityInput, uint32_t* viewCountOutput,
                                                            XrViewConfigurationView* views) {
    XrResult r = g.nextEnumerateViewConfigurationViews(instance, systemId, viewConfigurationType, viewCapacityInput,
                                                       viewCountOutput, views);
    if (XR_FAILED(r) || !views || viewCapacityInput == 0)
        return r;

    for (uint32_t i = 0; i < *viewCountOutput && i < viewCapacityInput; ++i) {
        XrViewConfigurationView& v = views[i];
        uint32_t w = v.recommendedImageRectWidth;
        uint32_t h = v.recommendedImageRectHeight;
        g.maxImageWidth = v.maxImageRectWidth;
        g.maxImageHeight = v.maxImageRectHeight;
        g.recommendedWidth = w;
        g.recommendedHeight = h;
        if (g.cfg.renderScale != 1.0f) {
            v.recommendedImageRectWidth =
                std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(w * g.cfg.renderScale)), 16u,
                                     v.maxImageRectWidth);
            v.recommendedImageRectHeight =
                std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(h * g.cfg.renderScale)), 16u,
                                     v.maxImageRectHeight);
        }
        if (!g.loggedViews)
            Log("view %u: runtime recommends %ux%u (max %ux%u), game gets %ux%u (render_scale %.2f)", i, w, h,
                v.maxImageRectWidth, v.maxImageRectHeight, v.recommendedImageRectWidth,
                v.recommendedImageRectHeight, g.cfg.renderScale);
    }
    g.loggedViews = true;
    return r;
}

XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                          XrSession* session) {
    for (auto* n = reinterpret_cast<const XrBaseInStructure*>(createInfo->next); n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            auto* binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(n);
            Log("xrCreateSession: D3D11 binding");
            if (binding->device) {
                LogAdapter(binding->device);
                ShutdownUpscaling();
                g.device = binding->device;
                g.device->AddRef();
                g.gpuTimer.Init(g.device);
                // Set up whenever the game renders below native, even with upscaler=none,
                // so FSR / NIS can be switched on later without restarting the game
                if (g.cfg.renderScale < 1.0f) {
                    g.upscaling = g.upscaler.Init(g.device);
                    Log("upscaling %s: %s (render_scale %.2f, sharpness %.2f)",
                        g.upscaling ? "ready" : "FAILED to start, runtime will stretch the image", UpscalerName(),
                        g.cfg.renderScale, g.cfg.sharpness);
                } else {
                    Log("upscaling off: native resolution (render_scale %.2f)", g.cfg.renderScale);
                }
            }
        } else {
            Log("xrCreateSession: graphics binding type %d (not D3D11)", n->type);
        }
    }
    XrResult r = g.nextCreateSession(instance, createInfo, session);
    Log("xrCreateSession -> %d", r);
    return r;
}

XrResult XRAPI_CALL Layer_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                            XrSwapchain* swapchain) {
    XrResult r = g.nextCreateSwapchain(session, createInfo, swapchain);
    Log("xrCreateSwapchain %ux%u array %u mips %u samples %u format %lld (%s) usage 0x%llx -> %d, handle %p",
        createInfo->width, createInfo->height, createInfo->arraySize, createInfo->mipCount, createInfo->sampleCount,
        static_cast<long long>(createInfo->format), DxgiFormatName(createInfo->format),
        static_cast<unsigned long long>(createInfo->usageFlags), r,
        XR_SUCCEEDED(r) ? static_cast<void*>(*swapchain) : nullptr);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard<std::mutex> lock(g.swapchainMutex);
        SwapchainInfo& sc = g.swapchains[*swapchain];
        sc.info = *createInfo;
        sc.info.next = nullptr;
        if (g.upscaling && IsEyeColorSwapchain(*createInfo)) {
            sc.images = EnumerateImages(*swapchain);
            CreateOutputSwapchain(session, sc);
        }
    }
    return r;
}

XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain) {
    {
        std::lock_guard<std::mutex> lock(g.swapchainMutex);
        auto it = g.swapchains.find(swapchain);
        if (it != g.swapchains.end()) {
            if (it->second.output)
                g.nextDestroySwapchain(it->second.output);
            g.swapchains.erase(it);
        }
    }
    return g.nextDestroySwapchain(swapchain);
}

XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo,
                                                  uint32_t* index) {
    XrResult r = g.nextAcquireSwapchainImage(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard<std::mutex> lock(g.swapchainMutex);
        auto it = g.swapchains.find(swapchain);
        if (it != g.swapchains.end())
            it->second.acquired.push_back(*index);
    }
    return r;
}

XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                  const XrSwapchainImageReleaseInfo* releaseInfo) {
    XrResult r = g.nextReleaseSwapchainImage(swapchain, releaseInfo);
    if (XR_SUCCEEDED(r)) {
        // Images are released in the order they were acquired
        std::lock_guard<std::mutex> lock(g.swapchainMutex);
        auto it = g.swapchains.find(swapchain);
        if (it != g.swapchains.end() && !it->second.acquired.empty()) {
            it->second.lastReleased = static_cast<int>(it->second.acquired.front());
            it->second.acquired.pop_front();
        }
    }
    return r;
}

void LogSessionSummary() {
    auto& f = g.sessionFrameMs;
    if (f.empty())
        return;
    double total = 0.0;
    for (float ms : f)
        total += ms;
    // 1% low: average frame rate over the slowest 1% of frames
    std::vector<float> sorted(f);
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());
    size_t n1 = std::max<size_t>(1, sorted.size() / 100);
    double slowSum = 0.0;
    for (size_t i = 0; i < n1; ++i)
        slowSum += sorted[i];

    Log("=== session summary (%s, render_scale %.2f) ===", g.upscaling ? UpscalerName() : "no upscaling",
        g.cfg.renderScale);
    Log("  %zu frames over %.0f s, headset %.0f Hz", f.size(), total / 1000.0,
        g.baseDisplayPeriodMs > 0.0 ? 1000.0 / g.baseDisplayPeriodMs : 0.0);
    Log("  avg %.1f fps (%.2f ms), 1%% low %.1f fps", 1000.0 * f.size() / total, total / f.size(),
        1000.0 * n1 / slowSum);
    Log("  missed vsync: %llu frames (%.1f%%), runtime at reduced rate: %.1f s (%.1f%% of the time)",
        static_cast<unsigned long long>(g.sessionMissed), 100.0 * g.sessionMissed / f.size(),
        g.sessionReducedRateMs / 1000.0, 100.0 * g.sessionReducedRateMs / total);

    std::vector<float> gpu(g.gpuTimer.SessionSamples());
    if (!gpu.empty()) {
        std::sort(gpu.begin(), gpu.end());
        double sum = 0.0;
        for (float ms : gpu)
            sum += ms;
        Log("  game GPU ms: avg %.2f, median %.2f, p95 %.2f (headset budget %.2f)", sum / gpu.size(),
            gpu[gpu.size() / 2], gpu[gpu.size() * 95 / 100], g.baseDisplayPeriodMs);
    }
}

XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session) {
    Log("xrDestroySession (upscale failures: %llu)", static_cast<unsigned long long>(g.upscaleFailures));
    LogSessionSummary();
    XrResult r = g.nextDestroySession(session);
    {
        std::lock_guard<std::mutex> lock(g.swapchainMutex);
        g.swapchains.clear();
    }
    ShutdownUpscaling();
    return r;
}

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo,
                                      XrFrameState* frameState) {
    XrResult r = g.nextWaitFrame(session, frameWaitInfo, frameState);
    if (XR_SUCCEEDED(r) && frameState->predictedDisplayPeriod > 0)
        g.displayPeriodMs = frameState->predictedDisplayPeriod / 1.0e6;
    return r;
}

XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) {
    XrResult r = g.nextBeginFrame(session, frameBeginInfo);
    if (XR_SUCCEEDED(r) && g.renderThread) {
        DWORD thread = GetCurrentThreadId();
        if (!g.loggedBeginThread) {
            g.loggedBeginThread = true;
            Log("xrBeginFrame thread %lu, xrEndFrame thread %lu: game GPU timing %s", thread, g.renderThread,
                thread == g.renderThread ? "on" : "off (different threads)");
        }
        if (thread == g.renderThread)
            g.gpuTimer.FrameBegin();
    }
    return r;
}

XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (!g.renderThread)
        g.renderThread = GetCurrentThreadId();
    else if (GetCurrentThreadId() == g.renderThread)
        g.gpuTimer.FrameEnd(); // before our own FSR work, so it is not counted as the game's
    LogFrameLayers(frameEndInfo);
    UpdateFrameStats(frameEndInfo->layerCount > 0);

    CheckConfigReload();
    if (g.upscaling && g.cfg.upscaler != Config::Upscaler::None && frameEndInfo->layerCount > 0) {
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        std::vector<XrCompositionLayerProjection> projections;
        std::vector<std::vector<XrCompositionLayerProjectionView>> views;
        if (UpscaleFrame(frameEndInfo, layers, projections, views)) {
            XrFrameEndInfo patched = *frameEndInfo;
            patched.layers = layers.data();
            return g.nextEndFrame(session, &patched);
        }
    }
    return g.nextEndFrame(session, frameEndInfo);
}

XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance) {
    Log("xrDestroyInstance");
    PFN_xrDestroyInstance next = g.nextDestroyInstance;
    g.instance = XR_NULL_HANDLE;
    g.hooked = false;
    return next(instance);
}

// ---------------------------------------------------------------------------
// Loader plumbing

XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!g.nextGetInstanceProcAddr)
        return XR_ERROR_HANDLE_INVALID;

    if (std::strncmp(name, "xr", 2) == 0) {
        const char* fn = name + 2;
        // xrDestroyInstance is always hooked so the state gets reset
#define XRPERF_MATCH(n, hooked)                                                                                        \
    if ((hooked) && std::strcmp(fn, #n) == 0 && g.next##n) {                                                           \
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xr##n);                                                 \
        return XR_SUCCESS;                                                                                             \
    }
        XRPERF_MATCH(DestroyInstance, true)
        XRPERF_MATCH(EnumerateViewConfigurationViews, g.hooked)
        XRPERF_MATCH(CreateSession, g.hooked)
        XRPERF_MATCH(DestroySession, g.hooked)
        XRPERF_MATCH(CreateSwapchain, g.hooked)
        XRPERF_MATCH(DestroySwapchain, g.hooked)
        XRPERF_MATCH(AcquireSwapchainImage, g.hooked)
        XRPERF_MATCH(ReleaseSwapchainImage, g.hooked)
        XRPERF_MATCH(WaitFrame, g.hooked)
        XRPERF_MATCH(BeginFrame, g.hooked)
        XRPERF_MATCH(EndFrame, g.hooked)
#undef XRPERF_MATCH
    }

    return g.nextGetInstanceProcAddr(instance, name, function);
}

XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        !layerInfo->nextInfo || layerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        std::strcmp(layerInfo->nextInfo->layerName, LAYER_NAME) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;

    g.cfg = LoadConfig();
    g.configWriteTime = ConfigWriteTime();
    std::wstring exe;
    bool matches = ProcessMatches(g.cfg, &exe);
    g.hooked = g.cfg.enabled && matches;
    LogLoadEvent("xrCreateApiLayerInstance in %s: enabled=%d process_match=%d -> %s", Narrow(exe).c_str(),
                 g.cfg.enabled, matches, g.hooked ? "hooked" : "pass-through");
    if (g.hooked) {
        LogOpen(g.cfg.log);
        Log("xrperf layer loaded into %s", Narrow(exe).c_str());
        Log("config %s: render_scale %.2f, upscaler %s, sharpness %.2f, stats_interval %.1f s",
            Narrow(g.cfg.path).c_str(), g.cfg.renderScale, UpscalerName(), g.cfg.sharpness,
            g.cfg.statsInterval);
        Log("app: %s v%u, engine: %s v%u, api 0x%llx", info->applicationInfo.applicationName,
            info->applicationInfo.applicationVersion, info->applicationInfo.engineName,
            info->applicationInfo.engineVersion, static_cast<unsigned long long>(info->applicationInfo.apiVersion));
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            Log("  extension: %s", info->enabledExtensionNames[i]);
        for (uint32_t i = 0; i < info->enabledApiLayerCount; ++i)
            Log("  explicit layer: %s", info->enabledApiLayerNames[i]);
    }

    // Pass creation down the chain, skipping ourselves
    XrApiLayerCreateInfo chained = *layerInfo;
    chained.nextInfo = layerInfo->nextInfo->next;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chained, instance);
    if (XR_FAILED(r)) {
        if (g.hooked)
            Log("xrCreateInstance failed further down the chain: %d", r);
        return r;
    }

    g.instance = *instance;
    g.nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    Resolve("xrDestroyInstance", g.nextDestroyInstance);
    Resolve("xrEnumerateViewConfigurationViews", g.nextEnumerateViewConfigurationViews);
    Resolve("xrCreateSession", g.nextCreateSession);
    Resolve("xrDestroySession", g.nextDestroySession);
    Resolve("xrCreateSwapchain", g.nextCreateSwapchain);
    Resolve("xrDestroySwapchain", g.nextDestroySwapchain);
    Resolve("xrEnumerateSwapchainImages", g.nextEnumerateSwapchainImages);
    Resolve("xrAcquireSwapchainImage", g.nextAcquireSwapchainImage);
    Resolve("xrWaitSwapchainImage", g.nextWaitSwapchainImage);
    Resolve("xrReleaseSwapchainImage", g.nextReleaseSwapchainImage);
    Resolve("xrWaitFrame", g.nextWaitFrame);
    Resolve("xrBeginFrame", g.nextBeginFrame);
    Resolve("xrEndFrame", g.nextEndFrame);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g.qpcToMs = 1000.0 / static_cast<double>(freq.QuadPart);
    g.loggedViews = false;
    g.lastLayerSignature.clear();
    g.layerLogsLeft = 20;
    g.lastEndFrame = 0;
    g.sessionFrameMs.clear();
    g.sessionFrameMs.reserve(90 * 60 * 30);
    g.sessionMissed = 0;
    g.sessionReducedRateMs = 0.0;
    g.baseDisplayPeriodMs = 0.0;

    if (g.hooked)
        Log("instance created, hooks active");
    return r;
}

} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* apiLayerRequest) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    LogLoadEvent("negotiate from %s (loader interface %u-%u)", Narrow(exePath).c_str(),
                 loaderInfo ? loaderInfo->minInterfaceVersion : 0, loaderInfo ? loaderInfo->maxInterfaceVersion : 0);

    if (!loaderInfo || !apiLayerRequest || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        (layerName && std::strcmp(layerName, LAYER_NAME) != 0))
        return XR_ERROR_INITIALIZATION_FAILED;

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
