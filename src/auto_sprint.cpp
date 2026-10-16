#ifdef _WIN32
// Sample Bedrock client: auto sprint ONLY. No GUI, speed modifications, or game-memory writes.
// Version identity is selected LIVE from GitHub manifest.json; unsupported builds do nothing.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "auto_sprint_policy.hpp"
#include "bedrock/remote_catalog.hpp"
#include "bedrock/resolver.hpp"

namespace {
enum Status : unsigned { NotStarted=0, Loading=1, Running=2,
                         UnsupportedOrOffline=3, Stopped=4 };
std::atomic<unsigned> g_status{NotStarted};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_ownCtrl{false};

// Simulated LEFT CTRL (standard Bedrock sprint binding); SendInput sends ordinary
// OS input, so we do not rely on a volatile Actor or MoveInputComponent layout.
bool injectCtrl(bool down) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0x1D; // Windows x86 scan code: left control
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1,&input,sizeof(input)) == 1;
}
bool minecraftForeground() noexcept {
    HWND focused = GetForegroundWindow();
    if (!focused) return false;
    DWORD owner{};
    GetWindowThreadProcessId(focused,&owner);
    return owner == GetCurrentProcessId();
}
bool gameplayCursorHidden() noexcept {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    return GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING);
}
bool held(int key) noexcept { return (GetAsyncKeyState(key) & 0x8000) != 0; }

void releaseOwnedCtrl() noexcept {
    // Inject only the key-up that pairs with this DLL's own key-down.
    if (g_ownCtrl.exchange(false)) injectCtrl(false);
}

DWORD WINAPI run(void*) {
    g_status.store(Loading);
    // Prevent a running worker from being unloaded while it executes.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&run), &pinned)) {
        g_status.store(UnsupportedOrOffline);
        return 1;
    }

    bedrock::ProfileChoice choice;
    std::string json, error;
    // 1. Download the manifest via HTTPS.
    // 2. Select an exact executable build, falling back from packaged file SHA
    //    to mapped .text SHA.
    // 3. Verify SHA256 of the single downloaded version JSON.
    // 4. Validate the JSON's own executable identity and build the resolver.
    if (!bedrock::fetchSupportedProfile(json,choice,error)) {
        OutputDebugStringA(("Bedrock AutoSprint inactive: "+error+"\n").c_str());
        g_status.store(UnsupportedOrOffline);
        return 0; // Fail closed: no input injection.
    }
    bedrock::Resolver resolver;
    if (!resolver.loadJson(json,error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        OutputDebugStringA(("Bedrock AutoSprint inactive: "+error+"\n").c_str());
        g_status.store(UnsupportedOrOffline);
        return 0;
    }
    // Require the edition-specific client input function to be present in the
    // matching database. This is a sanity check; we do not invoke its unknown ABI.
    auto input = resolver.resolve(
        "ClientInputUpdateSystemInternal::tickUpdateClientInput", true);
    if (!input || !input->exact_build || input->method != "exact-rva") {
        OutputDebugStringA("Bedrock AutoSprint inactive: client input anchor absent.\n");
        g_status.store(UnsupportedOrOffline);
        return 0;
    }
    const auto note = "Bedrock AutoSprint active for "+choice.version+
                      " (forward=W, sprint=LeftCtrl).\n";
    OutputDebugStringA(note.c_str());
    g_status.store(Running);

    while (!g_stop.load(std::memory_order_acquire)) {
        const bool shouldPress = bedrock::shouldAutoSprint(
            minecraftForeground(),gameplayCursorHidden(),
            held('W'),held(VK_LSHIFT) || held(VK_RSHIFT));

        if (shouldPress && !g_ownCtrl.load(std::memory_order_relaxed)) {
            // Do not "own" the key if Ctrl was already down before this tick.
            if (!held(VK_LCONTROL) && injectCtrl(true)) g_ownCtrl.store(true);
        } else if (!shouldPress) {
            releaseOwnedCtrl();
        }
        Sleep(20);
    }
    releaseOwnedCtrl();
    g_status.store(Stopped);
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) unsigned AutoSprint_GetStatus() {
    return g_status.load(std::memory_order_acquire);
}
extern "C" __declspec(dllexport) void AutoSprint_Stop() {
    g_stop.store(true, std::memory_order_release);
}

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE h = CreateThread(nullptr,0,run,nullptr,0,nullptr);
        if (h) CloseHandle(h);
        else g_status.store(UnsupportedOrOffline);
    } else if (reason == DLL_PROCESS_DETACH) {
        // No blocking work under loader lock. Normal termination is processed
        // by worker in AutoSprint_Stop(); the DLL is pinned until process exit.
        g_stop.store(true);
    }
    return TRUE;
}
#endif
