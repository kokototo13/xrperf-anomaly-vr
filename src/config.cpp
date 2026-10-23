#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace xrperf {

namespace {

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::wstring ReadString(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)), file.c_str());
    return buf;
}

float ReadFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float def) {
    std::wstring s = ReadString(file, section, key, L"");
    if (s.empty())
        return def;
    try {
        return std::stof(s);
    } catch (...) {
        return def;
    }
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

} // namespace

Config LoadConfig() {
    Config cfg;
    cfg.path = ModuleDir() + L"\\xrperf.ini";
    const std::wstring& f = cfg.path;

    cfg.enabled = GetPrivateProfileIntW(L"general", L"enabled", 1, f.c_str()) != 0;
    cfg.log = GetPrivateProfileIntW(L"general", L"log", 1, f.c_str()) != 0;
    cfg.statsInterval = ReadFloat(f, L"general", L"stats_interval", 5.0f);
    cfg.renderScale = std::clamp(ReadFloat(f, L"resolution", L"render_scale", 1.0f), 0.25f, 2.0f);
    std::wstring upscaler = Lower(ReadString(f, L"resolution", L"upscaler", L"fsr"));
    cfg.upscaler = upscaler == L"nis"    ? Config::Upscaler::Nis
                   : upscaler == L"none" ? Config::Upscaler::None
                                         : Config::Upscaler::Fsr;
    cfg.sharpness = std::clamp(ReadFloat(f, L"resolution", L"sharpness", 0.75f), 0.0f, 1.0f);
    std::wstring list = ReadString(f, L"general", L"processes", L"AnomalyDX11");
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos)
            end = list.size();
        std::wstring item = list.substr(start, end - start);
        item.erase(0, item.find_first_not_of(L" \t"));
        item.erase(item.find_last_not_of(L" \t") + 1);
        if (!item.empty())
            cfg.processes.push_back(Lower(item));
        start = end + 1;
    }
    return cfg;
}

bool ProcessMatches(const Config& cfg, std::wstring* exeName) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe(path);
    exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    if (exeName)
        *exeName = exe;

    std::wstring lower = Lower(exe);
    for (const auto& p : cfg.processes) {
        if (p == L"*" || lower.find(p) != std::wstring::npos)
            return true;
    }
    return false;
}

} // namespace xrperf
