// Creates an OpenXR instance through the game's openxr_loader.dll and prints
// what the runtime (and the xrperf layer, if registered) report.
//
// usage: xrperf_test.exe [path\to\openxr_loader.dll]

#include <windows.h>

#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    const char* loaderPath = argc > 1 ? argv[1] : "C:\\S.T.A.L.K.E.R. Anomaly\\bin\\openxr_loader.dll";
    HMODULE loader = LoadLibraryA(loaderPath);
    if (!loader) {
        printf("cannot load %s (error %lu)\n", loaderPath, GetLastError());
        return 1;
    }
    auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!getProc) {
        printf("xrGetInstanceProcAddr not exported\n");
        return 1;
    }

    PFN_xrEnumerateApiLayerProperties enumLayers = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    getProc(XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", reinterpret_cast<PFN_xrVoidFunction*>(&enumLayers));
    getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&createInstance));

    uint32_t layerCount = 0;
    enumLayers(0, &layerCount, nullptr);
    std::vector<XrApiLayerProperties> layers(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
    enumLayers(layerCount, &layerCount, layers.data());
    printf("API layers visible to the loader: %u\n", layerCount);
    for (auto& l : layers)
        printf("  %s - %s\n", l.layerName, l.description);

    const char* extensions[] = {"XR_KHR_D3D11_enable"};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "xrperf_test");
    strcpy_s(ci.applicationInfo.engineName, "xrperf");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = createInstance(&ci, &instance);
    printf("xrCreateInstance -> %d\n", r);
    if (XR_FAILED(r))
        return 1;

    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumViews = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    getProc(instance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&getSystem));
    getProc(instance, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&enumViews));
    getProc(instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroyInstance));

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = getSystem(instance, &si, &system);
    printf("xrGetSystem -> %d%s\n", r, r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? " (headset not connected?)" : "");
    if (XR_SUCCEEDED(r)) {
        uint32_t n = 0;
        enumViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        enumViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, views.data());
        for (uint32_t i = 0; i < n; ++i)
            printf("view %u: recommended %ux%u, max %ux%u\n", i, views[i].recommendedImageRectWidth,
                   views[i].recommendedImageRectHeight, views[i].maxImageRectWidth, views[i].maxImageRectHeight);
    }

    destroyInstance(instance);
    return 0;
}
