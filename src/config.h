#pragma once

#include <string>
#include <vector>

namespace xrperf {

struct Config {
    bool enabled = true;
    bool log = true;
    std::vector<std::wstring> processes;
    float statsInterval = 5.0f;
    float renderScale = 1.0f;
    enum class Upscaler { None, Fsr, Nis };
    Upscaler upscaler = Upscaler::Fsr; // None: the runtime stretches the smaller image itself (bilinear)
    float sharpness = 0.75f; // RCAS, 0..1

    // Fixed foveated rendering (NVIDIA VRS)
    bool vrsEnabled = false;
    float vrsInner = 0.55f;
    float vrsOuter = 0.80f;

    std::wstring path; // where it was read from, for the log
};

// Reads xrperf.ini that sits next to xrperf_layer.dll
Config LoadConfig();

// True if the current exe name contains one of cfg.processes (case-insensitive)
bool ProcessMatches(const Config& cfg, std::wstring* exeName);

} // namespace xrperf
