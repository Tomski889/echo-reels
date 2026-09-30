// Echo Arcade plugin loader: a minimal dinput8.dll for installs whose bin\win10\dbgcore.dll is not EchoLoader
// (e.g. EchoRelay's patch). It loads bin\win10\plugins\<file> for every {"file": "..."} entry in
// echoloader.json, the list EchoLoader uses, so EchoArcade.dll starts the same way.
//
// Another mod that was installed as dinput8.dll (ReShade, ...) is kept as dinput8.chain.dll and loaded too;
// DirectInput8Create is forwarded to it, or else to the system dinput8.dll.
#include <windows.h>
#include <cstdio>
#include <string>

namespace {

wchar_t g_dir[MAX_PATH] = L"";
HMODULE g_chain = nullptr, g_system = nullptr;

// Marker the installer looks for to recognise this DLL.
extern "C" __declspec(dllexport) const char EchoArcadeLoaderMarker[] = "Echo Arcade plugin loader";

void log(const char* format, const char* name, bool ok) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(g_dir) + L"\\ArcadeLoader.log").c_str(), L"a") == 0 && f) {
        std::fprintf(f, format, name, ok ? "loaded" : "could not be loaded");
        std::fputc('\n', f);
        std::fclose(f);
    }
}

DWORD WINAPI loadPlugins(void*) {
    const std::wstring dir(g_dir);
    FILE* file = nullptr;
    if (_wfopen_s(&file, (dir + L"\\echoloader.json").c_str(), L"rb") != 0 || !file) return 0;
    std::string json;
    char buffer[4096];
    for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) json.append(buffer, read);
    std::fclose(file);

    for (size_t at = json.find("\"file\""); at != std::string::npos; at = json.find("\"file\"", at + 6)) {
        const size_t open = json.find('"', json.find(':', at) + 1);
        const size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
        if (close == std::string::npos) break;
        const std::string name = json.substr(open + 1, close - open - 1);
        if (name.empty() || name.find_first_of("\\/:") != std::string::npos) continue;  // plain names only
        const std::wstring path = dir + L"\\plugins\\" + std::wstring(name.begin(), name.end());
        log("plugin %s: %s", name.c_str(), LoadLibraryW(path.c_str()) != nullptr);
    }
    return 0;
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create create = g_chain ? reinterpret_cast<Create>(GetProcAddress(g_chain, "DirectInput8Create")) : nullptr;
    if (!create) {
        if (!g_system) {
            wchar_t path[MAX_PATH];
            GetSystemDirectoryW(path, MAX_PATH);
            wcscat_s(path, L"\\dinput8.dll");
            g_system = LoadLibraryW(path);
        }
        create = g_system ? reinterpret_cast<Create>(GetProcAddress(g_system, "DirectInput8Create")) : nullptr;
    }
    return create ? create(instance, version, riid, out, outer) : E_FAIL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        GetModuleFileNameW(module, g_dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = L'\0';
        const std::wstring chain = std::wstring(g_dir) + L"\\dinput8.chain.dll";
        if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES) g_chain = LoadLibraryW(chain.c_str());
        // Not from DllMain (loader lock): plugins start their own threads
        if (HANDLE thread = CreateThread(nullptr, 0, loadPlugins, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
