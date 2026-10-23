#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace xrperf {

// Measures how long the GPU spends on the game's frame: a timestamp when the game
// calls xrBeginFrame and another when it calls xrEndFrame (before the layer adds its
// own work). Queries are read a few frames later, so measuring never stalls the GPU.
class GpuFrameTimer {
  public:
    void Init(ID3D11Device* device);
    void Shutdown();

    void FrameBegin();
    void FrameEnd();

    // Average / worst since the last call, in ms. False if nothing was measured.
    bool TakeWindow(double* avgMs, double* worstMs);
    const std::vector<float>& SessionSamples() const { return m_session; }

  private:
    void Collect();

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Slot {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    static constexpr int kSlots = 6;

    ComPtr<ID3D11DeviceContext> m_context;
    Slot m_slots[kSlots];
    int m_slot = 0;
    bool m_open = false;

    double m_windowSum = 0.0;
    double m_windowWorst = 0.0;
    uint64_t m_windowCount = 0;
    std::vector<float> m_session;
};

} // namespace xrperf
