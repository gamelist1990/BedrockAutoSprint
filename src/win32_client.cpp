#ifdef _WIN32
// Adapt verified Minecraft input events into the sample feature. No OS key
// polling or SendInput is used; generated Ctrl transitions use Keyboard::feed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <bedrock/events.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <bedrock/console.hpp>
#include <bedrock/logger.hpp>
#include "auto_sprint_feature.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
enum class Status : unsigned {
    NotStarted=0, Loading=1, Running=2, Unsupported=3, Stopped=4, Failed=5
};
std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<HANDLE> stopEvent{nullptr};
using KeyboardFeedFn = bool(__fastcall*)(int, bool);
using CursorFn = void(__fastcall*)(void*);
std::atomic<KeyboardFeedFn> originalKeyboardFeed{nullptr};
std::atomic<CursorFn> originalGrabCursor{nullptr};
std::atomic<CursorFn> originalReleaseCursor{nullptr};
std::array<void*, 3> hookTargets{};
bool ownsMinHook = false;
bedrock::Logger logger{"AutoSprint"};
std::mutex logMutex;
std::ofstream logFile;
std::mutex eventMutex;
AutoSprintEvents* activeEvents{};
bool gameplayActive{};
bool forwardHeld{};
bool shiftHeld{};
bool ctrlHeld{};
std::array<bool, 5> shiftKeyDown{};
std::array<bool, 5> ctrlKeyDown{};
std::atomic<bool> loggedFirstKey{false};

constexpr int kInputW = 'W';
constexpr int kInputCtrl = 0x11;
constexpr std::array<int, 5> kShiftCodes{0x10, 0xA0, 0xA1, 340, 344};
constexpr std::array<int, 5> kCtrlCodes{0x11, 0xA2, 0xA3, 341, 345};

bool isShiftKey(int key) noexcept {
    return key == 0x10 || key == 0xA0 || key == 0xA1 || key == 340 || key == 344;
}

bool isCtrlKey(int key) noexcept {
    return key == 0x11 || key == 0xA2 || key == 0xA3 || key == 341 || key == 345;
}

bool initializeFileLog() {
    std::wstring userProfile(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"USERPROFILE", userProfile.data(), static_cast<DWORD>(userProfile.size()));
    if (!length || length >= userProfile.size()) return false;
    userProfile.resize(length);
    const auto directory = std::filesystem::path(userProfile) / L"Documents" /
        L"PEXData" / L"AutoSprint";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    logFile.open(directory / L"AutoSprint.log", std::ios::binary | std::ios::app);
    if (!logFile) return false;
    logger.setSink([](bedrock::LogLevel, std::string_view line, void*) {
        std::lock_guard lock(logMutex);
        OutputDebugStringA(std::string(line).c_str());
        if (logFile) {
            logFile.write(line.data(), static_cast<std::streamsize>(line.size()));
            logFile.flush();
        }
    });
    return true;
}

void emitInputFrameLocked() {
    if (!activeEvents) return;
    activeEvents->emit(InputFrameEvent{
        gameplayActive, gameplayActive, forwardHeld, shiftHeld, ctrlHeld});
}

bool __fastcall onKeyboardFeed(int inputKey, bool down) {
    const auto original = originalKeyboardFeed.load(std::memory_order_acquire);
    const bool result = original ? original(inputKey, down) : false;
    if (!loggedFirstKey.exchange(true, std::memory_order_acq_rel))
        logger.info("Keyboard::feed active; first InputKey=", inputKey,
                    " down=", down ? 1 : 0);
    std::lock_guard lock(eventMutex);
    if (inputKey == kInputW) forwardHeld = down;
    if (isShiftKey(inputKey)) {
        for (std::size_t i = 0; i < kShiftCodes.size(); ++i)
            if (kShiftCodes[i] == inputKey) shiftKeyDown[i] = down;
        shiftHeld = std::any_of(shiftKeyDown.begin(), shiftKeyDown.end(), [](bool value) { return value; });
    }
    if (isCtrlKey(inputKey)) {
        for (std::size_t i = 0; i < kCtrlCodes.size(); ++i)
            if (kCtrlCodes[i] == inputKey) ctrlKeyDown[i] = down;
        ctrlHeld = std::any_of(ctrlKeyDown.begin(), ctrlKeyDown.end(), [](bool value) { return value; });
    }
    emitInputFrameLocked();
    return result;
}

void __fastcall onGrabCursor(void* self) {
    if (auto original = originalGrabCursor.load(std::memory_order_acquire)) original(self);
    std::lock_guard lock(eventMutex);
    gameplayActive = true;
    logger.info("Minecraft gameplay input active");
    emitInputFrameLocked();
}

void __fastcall onReleaseCursor(void* self) {
    {
        std::lock_guard lock(eventMutex);
        gameplayActive = false;
        emitInputFrameLocked();
        forwardHeld = shiftHeld = ctrlHeld = false;
        shiftKeyDown.fill(false);
        ctrlKeyDown.fill(false);
        logger.info("Minecraft gameplay input inactive");
    }
    if (auto original = originalReleaseCursor.load(std::memory_order_acquire)) original(self);
}

template <class Original>
bool createHook(void* target, void* detour, std::atomic<Original>& original) {
    Original captured = nullptr;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&captured)) != MH_OK)
        return false;
    original.store(captured, std::memory_order_release);
    return true;
}

void removeHooks() noexcept {
    for (void* target : hookTargets) if (target) MH_DisableHook(target);
    for (void* target : hookTargets) if (target) MH_RemoveHook(target);
    hookTargets.fill(nullptr);
    if (ownsMinHook) MH_Uninitialize();
    ownsMinHook = false;
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
    const bool fileLogging = initializeFileLog();
    logger.info("start; persistent log ", fileLogging ? "enabled" : "unavailable");
    bedrock::ProfileChoice version;
    std::string profile, error;
    if (!bedrock::fetchSupportedProfile(profile, version, error,
            [](std::string_view stage) { logger.info("profile ", stage); })) {
        logger.warn("Unsupported build or network error: ", error);
        status.store(Status::Unsupported);
        return 0;
    }
    bedrock::Resolver resolver;
    logger.info("resolver load");
    if (!resolver.loadJson(profile, error)) {
        logger.error("Profile JSON failed: ", error);
        status.store(Status::Unsupported);
        return 0;
    }
    logger.info("resolver attach");
    if (!resolver.attachWindowsMainModule(version.identity, error) || !resolver.exactBuild()) {
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Unsupported);
        return 0;
    }
    logger.info("resolver attached to exact Minecraft build");
    std::optional<bedrock::Resolution> keyboard, grab, release;
    const auto exact = [](const auto& symbol) {
        return symbol && symbol->exact_build && symbol->method == "exact-rva";
    };
    const auto resolveRequired = [&] {
        keyboard = resolver.resolve("Keyboard::feed");
        grab = resolver.resolve("ClientInstance::grabCursor", true);
        release = resolver.resolve("ClientInstance::releaseCursor", true);
    };
    const auto requirementsMet = [&] {
        return exact(keyboard) && keyboard->verified && exact(grab) && exact(release);
    };
    resolveRequired();
    if (!requirementsMet() && version.source != bedrock::ProfileSource::Remote) {
        logger.warn("Required symbols missing in cached profile; checking latest exact-build GitHub definition");
        std::string refreshedProfile;
        bedrock::ProfileChoice refreshedVersion;
        if (bedrock::refreshSupportedProfile(version.identity, refreshedProfile, refreshedVersion, error,
                [](std::string_view stage) { logger.info("profile ", stage); })) {
            if (resolver.loadJson(refreshedProfile, error) &&
                resolver.attachWindowsMainModule(refreshedVersion.identity, error) &&
                resolver.exactBuild()) {
                profile = std::move(refreshedProfile);
                version = std::move(refreshedVersion);
                resolveRequired();
                logger.info("latest exact-build profile loaded [",
                            bedrock::profileSourceName(version.source), "]");
            } else {
                logger.error("Refreshed profile could not attach to this exact executable: ", error);
            }
        } else {
            logger.warn("Could not refresh exact-build profile: ", error);
        }
    }
    if (!requirementsMet()) {
        logger.error("Required Minecraft input symbols unavailable keyboard=", bool(keyboard),
                     " grab=", bool(grab), " release=", bool(release));
        status.store(Status::Unsupported);
        return 0;
    }

    hookTargets[0] = reinterpret_cast<void*>(keyboard->address);
    hookTargets[1] = reinterpret_cast<void*>(grab->address);
    hookTargets[2] = reinterpret_cast<void*>(release->address);
    logger.info("MinHook initialize");
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        logger.error("MinHook initialization failed: ", static_cast<int>(initialized));
        status.store(Status::Failed);
        return 1;
    }
    ownsMinHook = initialized == MH_OK;

    AutoSprintEvents events;
    std::optional<AutoSprintFeature> feature;
    feature.emplace(events, [](bool down) {
        if (auto original = originalKeyboardFeed.load(std::memory_order_acquire))
            return original(kInputCtrl, down);
        return false;
    });
    {
        std::lock_guard lock(eventMutex);
        activeEvents = &events;
        gameplayActive = forwardHeld = shiftHeld = ctrlHeld = false;
    }
    const bool created =
        createHook(hookTargets[0], reinterpret_cast<void*>(&onKeyboardFeed), originalKeyboardFeed) &&
        createHook(hookTargets[1], reinterpret_cast<void*>(&onGrabCursor), originalGrabCursor) &&
        createHook(hookTargets[2], reinterpret_cast<void*>(&onReleaseCursor), originalReleaseCursor);
    if (!created) {
        {
            std::lock_guard lock(eventMutex);
            activeEvents = nullptr;
        }
        removeHooks();
        logger.error("Could not create Minecraft input hooks");
        status.store(Status::Failed);
        return 1;
    }
    logger.info("Minecraft input hooks created");
    bool queued = true;
    for (void* target : hookTargets) queued = queued && MH_QueueEnableHook(target) == MH_OK;
    if (!queued || MH_ApplyQueued() != MH_OK) {
        {
            std::lock_guard lock(eventMutex);
            activeEvents = nullptr;
        }
        removeHooks();
        logger.error("Could not enable Minecraft input hooks");
        status.store(Status::Failed);
        return 1;
    }
    logger.info("Minecraft input hooks enabled");
    status.store(Status::Running);
    logger.info("AutoSprint ready on Minecraft ", version.version, " [",
                bedrock::profileSourceName(version.source), "]");
    HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!signal) {
        logger.error("Could not create stop event");
        stopRequested.store(true, std::memory_order_release);
    } else {
        stopEvent.store(signal, std::memory_order_release);
        if (stopRequested.load(std::memory_order_acquire)) SetEvent(signal);
        WaitForSingleObject(signal, INFINITE);
    }
    {
        std::lock_guard lock(eventMutex);
        activeEvents = nullptr;
    }
    // Feature releases only the Ctrl transition it generated; the original
    // Minecraft Keyboard::feed trampoline is still available here.
    feature.reset();
    status.store(Status::Stopped);
    removeHooks();
    originalKeyboardFeed.store(nullptr, std::memory_order_release);
    originalGrabCursor.store(nullptr, std::memory_order_release);
    originalReleaseCursor.store(nullptr, std::memory_order_release);
    stopEvent.store(nullptr, std::memory_order_release);
    // This DLL is pinned for the worker lifetime; let process teardown reclaim
    // the small stop-event handle to avoid racing a concurrent Stop call.
    logger.info("AutoSprint stopped");
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned AutoSprint_GetStatus() {
    return static_cast<unsigned>(status.load(std::memory_order_acquire));
}
extern "C" __declspec(dllexport) void AutoSprint_Stop() {
    stopRequested.store(true, std::memory_order_release);
    if (HANDLE signal = stopEvent.load(std::memory_order_acquire)) SetEvent(signal);
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
