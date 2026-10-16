#ifdef _WIN32
// Sample-owned source of UpdateEvent/KeyEvent/GameplayEvent, and manifest gating.
// The SDK never starts polling, sets sprint, or injects player input.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/events.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include "auto_sprint_feature.hpp"
#include <array>
#include <atomic>
#include <string>

namespace {
enum class Status : unsigned {
    NotStarted=0, Loading=1, Running=2, Unsupported=3, Stopped=4, Failed=5
};
std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};

bool keyHeld(int code) noexcept {
    return (GetAsyncKeyState(code) & 0x8000) != 0;
}
bool sendCtrl(bool down) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0x1D; // Win32 scan code for left Ctrl.
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &input, sizeof(input)) == 1;
}
bool clientFocused() noexcept {
    HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    if (!foreground) return false;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}
bool gameplayInputActive() noexcept {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    return clientFocused() &&
           GetCursorInfo(&cursor) &&
           !(cursor.flags & CURSOR_SHOWING);
}

DWORD WINAPI startClient(void*) {
    // Pin sample code while this worker is active; no blocking initialization
    // or network call inside DllMain.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&startClient), &pinned)) {
        status.store(Status::Failed);
        return 1;
    }
    status.store(Status::Loading);
    bedrock::ProfileChoice version;
    std::string profile, error;
    // Optional symbol module, linked only by mods that need version data.
    if (!bedrock::fetchSupportedProfile(profile, version, error)) {
        OutputDebugStringA(("AutoSprint skipped: " + error + "\n").c_str());
        status.store(Status::Unsupported);
        return 0;
    }
    bedrock::Resolver resolver;
    if (!resolver.loadJson(profile, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild() ||
        !resolver.resolve(
            "ClientInputUpdateSystemInternal::tickUpdateClientInput", true)) {
        OutputDebugStringA("AutoSprint skipped: incomplete version compatibility.\n");
        status.store(Status::Unsupported);
        return 0;
    }

    // The event source is intentionally CLIENT-OWNED, not hidden in the SDK.
    // UpdateEvent = a periodic platform poll, *NOT* native Minecraft game tick.
    bedrock::ClientEvents events;
    AutoSprintFeature feature(events, &sendCtrl, [] { return !keyHeld(VK_LCONTROL); });
    bool previousFocus = false;
    bool previousGameplay = false;
    struct KeySample { int code; bool down; };
    std::array<KeySample, 3> keys{{
        {'W',false}, {VK_LSHIFT,false}, {VK_RSHIFT,false}
    }};
    std::uint64_t seq = 0;
    status.store(Status::Running);
    while (!stopRequested.load(std::memory_order_acquire)) {
        const bool focused = clientFocused();
        const bool gameplay = gameplayInputActive();
        if (focused != previousFocus) {
            events.emit(bedrock::FocusEvent{focused});
            previousFocus = focused;
        }
        if (gameplay != previousGameplay) {
            events.emit(bedrock::GameplayEvent{gameplay});
            previousGameplay = gameplay;
        }
        for (auto& key : keys) {
            const bool now = keyHeld(key.code);
            if (now != key.down) {
                events.emit(bedrock::KeyEvent{
                    static_cast<unsigned>(key.code), now, false});
                key.down = now;
            }
        }
        events.emit(bedrock::UpdateEvent{++seq});
        Sleep(20);
    }
    // Feature destructor emits release when its injected key remains owned.
    status.store(Status::Stopped);
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned AutoSprint_GetStatus() {
    return static_cast<unsigned>(status.load(std::memory_order_acquire));
}
extern "C" __declspec(dllexport) void AutoSprint_Stop() {
    stopRequested.store(true, std::memory_order_release);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, startClient, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else status.store(Status::Failed);
    } else if (reason == DLL_PROCESS_DETACH) {
        stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
#endif
