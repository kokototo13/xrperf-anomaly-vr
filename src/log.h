#pragma once

namespace xrperf {

void LogOpen(bool enabled);
void Log(const char* fmt, ...);

// Appends one line to xrperf_loads.log, for every process the loader puts the
// layer into (also the ones that are not hooked). Used to debug "layer not loaded".
void LogLoadEvent(const char* fmt, ...);

} // namespace xrperf
