// Records SteamVR's per-frame timings (the same numbers as the "Advanced Frame Timing"
// graph) while a game runs, then prints a summary. Runs as an OpenVR background app,
// so it never starts SteamVR or takes over the headset.
//
// usage: vr_timing_logger.exe <label> [output.csv]
//   Waits for a scene app (the game) to appear, records until it goes away, then exits.

#include <windows.h>

#include <openvr.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Sample {
    uint32_t index;
    double time;
    float totalGpuMs;      // whole frame on the GPU: game + compositor
    float compositorGpuMs; // SteamVR's own work (distortion etc.)
    float appGpuMs;        // what the game used = total - compositor
    float clientIntervalMs;
    uint32_t presents;
    uint32_t mispresented;
    uint32_t dropped;
    uint32_t flags;
};

double Percentile(std::vector<float> v, double p) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
    return v[i];
}

double Mean(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v)
        s += x;
    return v.empty() ? 0.0 : s / v.size();
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // progress lines show up immediately when redirected
    std::string label = argc > 1 ? argv[1] : "run";
    std::string csvPath = argc > 2 ? argv[2] : label + ".csv";

    vr::EVRInitError err = vr::VRInitError_None;
    while (true) {
        vr::VR_Init(&err, vr::VRApplication_Background);
        if (err == vr::VRInitError_None)
            break;
        printf("waiting for SteamVR (%s)\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        Sleep(3000);
    }
    vr::IVRCompositor* compositor = vr::VRCompositor();
    if (!compositor) {
        printf("no compositor interface\n");
        return 1;
    }

    float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                             vr::Prop_DisplayFrequency_Float);
    printf("[%s] SteamVR connected, headset %.1f Hz. Waiting for the game...\n", label.c_str(), hz);

    std::vector<Sample> samples;
    uint32_t lastIndex = 0;
    bool seenScene = false;
    DWORD lastScene = 0;
    ULONGLONG goneSince = 0;

    while (true) {
        uint32_t scene = compositor->GetCurrentSceneFocusProcess();
        if (scene) {
            if (!seenScene)
                printf("game pid %u is rendering, recording\n", scene);
            seenScene = true;
            lastScene = scene;
            goneSince = 0;
        } else if (seenScene) {
            if (!goneSince)
                goneSince = GetTickCount64();
            if (GetTickCount64() - goneSince > 3000)
                break; // the game has been gone for a few seconds: done
        }

        vr::Compositor_FrameTiming t[128];
        t[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        uint32_t n = scene ? compositor->GetFrameTimings(t, 128) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (t[i].m_nFrameIndex <= lastIndex)
                continue;
            lastIndex = t[i].m_nFrameIndex;
            Sample s;
            s.index = t[i].m_nFrameIndex;
            s.time = t[i].m_flSystemTimeInSeconds;
            s.totalGpuMs = t[i].m_flTotalRenderGpuMs;
            s.compositorGpuMs = t[i].m_flCompositorRenderGpuMs;
            s.appGpuMs = std::max(0.0f, s.totalGpuMs - s.compositorGpuMs);
            s.clientIntervalMs = t[i].m_flClientFrameIntervalMs;
            s.presents = t[i].m_nNumFramePresents;
            s.mispresented = t[i].m_nNumMisPresented;
            s.dropped = t[i].m_nNumDroppedFrames;
            s.flags = t[i].m_nReprojectionFlags;
            samples.push_back(s);
        }
        Sleep(250);
    }
    vr::VR_Shutdown();

    FILE* f = fopen(csvPath.c_str(), "w");
    if (f) {
        fprintf(f, "frame,time_s,total_gpu_ms,compositor_gpu_ms,app_gpu_ms,client_interval_ms,presents,mispresented,"
                   "dropped,reprojection_flags\n");
        for (auto& s : samples)
            fprintf(f, "%u,%.4f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,0x%x\n", s.index, s.time, s.totalGpuMs,
                    s.compositorGpuMs, s.appGpuMs, s.clientIntervalMs, s.presents, s.mispresented, s.dropped,
                    s.flags);
        fclose(f);
    }

    // Summary. Frames with no GPU numbers (loading screens) are left out.
    std::vector<float> app, total, interval;
    size_t reprojected = 0, motionSmoothing = 0, droppedFrames = 0;
    for (auto& s : samples) {
        if (s.totalGpuMs <= 0.0f)
            continue;
        app.push_back(s.appGpuMs);
        total.push_back(s.totalGpuMs);
        interval.push_back(s.clientIntervalMs);
        if (s.flags & (vr::VRCompositor_ReprojectionReason_Cpu | vr::VRCompositor_ReprojectionReason_Gpu) ||
            s.presents > 1)
            ++reprojected;
        if (s.flags & vr::VRCompositor_ReprojectionMotion)
            ++motionSmoothing;
        droppedFrames += s.dropped;
    }
    double budget = hz > 0 ? 1000.0 / hz : 0.0;
    size_t overBudget = std::count_if(total.begin(), total.end(), [&](float v) { return v > budget; });

    printf("\n=== %s: %zu frames, headset %.0f Hz (budget %.2f ms) ===\n", label.c_str(), app.size(), hz, budget);
    printf("game GPU ms:   avg %.2f  median %.2f  p95 %.2f  p99 %.2f\n", Mean(app), Percentile(app, 0.5),
           Percentile(app, 0.95), Percentile(app, 0.99));
    printf("total GPU ms:  avg %.2f  median %.2f  p95 %.2f  (game + SteamVR)\n", Mean(total), Percentile(total, 0.5),
           Percentile(total, 0.95));
    printf("over budget:   %zu frames (%.1f%%)\n", overBudget, app.empty() ? 0.0 : 100.0 * overBudget / app.size());
    printf("reprojected:   %zu frames (%.1f%%), motion smoothing %zu, dropped %zu\n", reprojected,
           app.empty() ? 0.0 : 100.0 * reprojected / app.size(), motionSmoothing, droppedFrames);
    printf("frame interval ms: avg %.2f  p95 %.2f\n", Mean(interval), Percentile(interval, 0.95));
    printf("csv: %s\n", csvPath.c_str());
    return 0;
}
