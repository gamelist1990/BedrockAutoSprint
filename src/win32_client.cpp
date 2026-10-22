#ifdef _WIN32
// Sample-owned source of UpdateEvent/KeyEvent/GameplayEvent, and manifest gating.
// The SDK never starts polling, sets sprint, or injects player input.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/events.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <bedrock/console.hpp>
#include <bedrock/logger.hpp>
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
#ifdef BEDROCK_AUTOSPRINT_DEBUG_CONSOLE
    // Explicitly opt-in; never create a console in DllMain.
    bedrock::Console::open(L"Bedrock AutoSprint Debug");
#endif
    bedrock::Logger logger{"AutoSprint"};
    logger.debug("Starting version verification");
    bedrock::ProfileChoice version;
    std::string profile, error;
    // Optional symbol module, linked only by mods that need version data.
    if (!bedrock::fetchSupportedProfile(profile, version, error)) {
        logger.warn("Unsupported build or network error: ", error);
        status.store(Status::Unsupported);
        return 0;
    }
    bedrock::Resolver resolver;
    if (!resolver.loadJson(profile, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild() ||
        !resolver.resolve(
            "ClientInputUpdateSystemInternal::tickUpdateClientInput", true)) {
        logger.warn("Incomplete version compatibility: ", error);
        status.store(Status::Unsupported);
        return 0;
    }

    // Same input capture as the previously working direct loop; only the
    // transport into the client feature is event-based now.
    AutoSprintEvents events;
    AutoSprintFeature feature(events, &sendCtrl);
    status.store(Status::Running);
    logger.info("AutoSprint ready on Minecraft ", version.version, " [",
                bedrock::profileSourceName(version.source), "]");
    while (!stopRequested.load(std::memory_order_acquire)) {
        const bool focused = clientFocused();
        const bool cursorHidden = gameplayInputActive();
        const bool forward = keyHeld('W');
        const bool shift = keyHeld(VK_LSHIFT) || keyHeld(VK_RSHIFT);
        const bool ctrl = keyHeld(VK_LCONTROL);
        events.emit(InputFrameEvent{focused, cursorHidden, forward, shift, ctrl});
        Sleep(20);
    }
    // Feature destructor releases only its own injected sprint key.
    status.store(Status::Stopped);
    logger.info("AutoSprint stopped");
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
