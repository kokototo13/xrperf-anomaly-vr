#include "gpu_timer.h"

#include <algorithm>

namespace xrperf {

void GpuFrameTimer::Init(ID3D11Device* device) {
    Shutdown();
    device->GetImmediateContext(&m_context);
    for (auto& s : m_slots) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device->CreateQuery(&qd, &s.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &s.begin);
        device->CreateQuery(&qd, &s.end);
    }
    m_session.reserve(90 * 60 * 30);
}

void GpuFrameTimer::Shutdown() {
    for (auto& s : m_slots)
        s = Slot{};
    m_context.Reset();
    m_open = false;
    m_slot = 0;
    m_windowSum = m_windowWorst = 0.0;
    m_windowCount = 0;
    m_session.clear();
}

void GpuFrameTimer::Collect() {
    for (auto& s : m_slots) {
        if (!s.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 begin, end;
        if (m_context->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(s.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(s.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        s.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || end < begin)
            continue;
        double ms = (end - begin) * 1000.0 / dj.Frequency;
        m_windowSum += ms;
        m_windowWorst = std::max(m_windowWorst, ms);
        ++m_windowCount;
        m_session.push_back(static_cast<float>(ms));
    }
}

void GpuFrameTimer::FrameBegin() {
    if (!m_context || !m_slots[0].disjoint || m_open)
        return;
    Collect();
    Slot& s = m_slots[m_slot];
    if (s.pending)
        return; // GPU is far behind; skip this frame
    m_context->Begin(s.disjoint.Get());
    m_context->End(s.begin.Get());
    m_open = true;
}

void GpuFrameTimer::FrameEnd() {
    if (!m_open)
        return;
    Slot& s = m_slots[m_slot];
    m_context->End(s.end.Get());
    m_context->End(s.disjoint.Get());
    s.pending = true;
    m_open = false;
    m_slot = (m_slot + 1) % kSlots;
}

bool GpuFrameTimer::TakeWindow(double* avgMs, double* worstMs) {
    if (m_windowCount == 0)
        return false;
    *avgMs = m_windowSum / m_windowCount;
    *worstMs = m_windowWorst;
    m_windowSum = m_windowWorst = 0.0;
    m_windowCount = 0;
    return true;
}

} // namespace xrperf
