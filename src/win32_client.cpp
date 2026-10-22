#ifdef _WIN32
// Adapt verified Minecraft input events into the sample feature. No OS key
// polling or SendInput is used; Ctrl uses the native HIDController input route.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <bedrock/console.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/file_log_sink.hpp>
#include "auto_sprint_feature.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
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
bedrock::GameApi gameApi;
std::unique_ptr<bedrock::GameHooks> gameHooks;
bedrock::Logger logger{"AutoSprint"};
bedrock::FileLogSink fileLog;
std::mutex eventMutex;
// Serialize feature state across native input and cursor callbacks.
std::recursive_mutex eventDispatchMutex;
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
    if (!fileLog.open(directory / L"AutoSprint.log")) return false;
    fileLog.attach(logger);
    return true;
}

InputFrameEvent inputFrameLocked() {
    return InputFrameEvent{
        gameplayActive, gameplayActive, forwardHeld, shiftHeld, ctrlHeld};
}

void onNativeKeyboard(const bedrock::NativeKeyEvent& input) {
    const int inputKey = static_cast<int>(input.inputKey);
    const bool down = input.pressed;
    if (!loggedFirstKey.exchange(true, std::memory_order_acq_rel))
        logger.info("native HIDController route active; first InputKey=", inputKey,
                    " down=", down ? 1 : 0);
    std::lock_guard<std::recursive_mutex> dispatchLock(eventDispatchMutex);
    AutoSprintEvents* events{};
    InputFrameEvent frame;
    {
        std::lock_guard lock(eventMutex);
        if (inputKey == kInputW) forwardHeld = down;
        if (isShiftKey(inputKey)) {
            for (std::size_t i = 0; i < kShiftCodes.size(); ++i)
                if (kShiftCodes[i] == inputKey) shiftKeyDown[i] = down;
            shiftHeld = std::any_of(shiftKeyDown.begin(), shiftKeyDown.end(),
                                    [](bool value) { return value; });
        }
        if (isCtrlKey(inputKey)) {
            for (std::size_t i = 0; i < kCtrlCodes.size(); ++i)
                if (kCtrlCodes[i] == inputKey) ctrlKeyDown[i] = down;
            ctrlHeld = std::any_of(ctrlKeyDown.begin(), ctrlKeyDown.end(),
                                   [](bool value) { return value; });
        }
        events = activeEvents;
        frame = inputFrameLocked();
    }
    // Native HID calls do not re-enter the raw callback or impersonate a
    // physical Ctrl transition. Never emit while holding eventMutex.
    if (events) events->emit(frame);
}

void onGameplay(const bedrock::GameplayEvent& input) {
    std::lock_guard<std::recursive_mutex> dispatchLock(eventDispatchMutex);
    AutoSprintEvents* events{};
    InputFrameEvent frame;
    bool changed{};
    bool wasActive{};
    {
        std::lock_guard lock(eventMutex);
        wasActive = gameplayActive;
        changed = gameplayActive != input.active;
        gameplayActive = input.active;
        if (!input.active) {
            forwardHeld = shiftHeld = ctrlHeld = false;
            shiftKeyDown.fill(false);
            ctrlKeyDown.fill(false);
        }
        if (changed || (!input.active && wasActive)) {
            events = activeEvents;
            frame = inputFrameLocked();
        }
    }
    if (events) events->emit(frame);
    if (changed && input.active) logger.info("Minecraft gameplay input active");
    if (wasActive && !input.active) logger.info("Minecraft gameplay input inactive");
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
    const auto installSdkHooks = [&] {
        gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
        if (gameHooks->installNativeKeyboard(resolver, profile, error) &&
            gameHooks->installGameplay(resolver, error)) return true;
        gameHooks.reset();
        return false;
    };
    bool hooksReady = installSdkHooks();
    // A cached definition can lag the current SDK metadata; check its bundled
    // exact-build profile before making the normal latest-definition refresh.
    if (!hooksReady) {
        bedrock::Resolver bundled;
        std::string bundledError;
        if (bundled.loadJson(bedrock::generated::kSymbolDatabase, bundledError) &&
            bundled.attachWindowsMainModule(version.identity, bundledError) &&
            bundled.exactBuild() &&
            bedrock::parseNativeKeyboardLayout(
                bedrock::generated::kSymbolDatabase, bundledError)) {
            resolver = std::move(bundled);
            profile = bedrock::generated::kSymbolDatabase;
            version.source = bedrock::ProfileSource::Embedded;
            hooksReady = installSdkHooks();
            if (hooksReady) logger.info("using bundled exact-build native keyboard definition");
        }
    }
    if (!hooksReady && version.source != bedrock::ProfileSource::Remote) {
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
                hooksReady = installSdkHooks();
                if (hooksReady) logger.info("latest exact-build profile loaded [",
                    bedrock::profileSourceName(version.source), "]");
            } else {
                logger.error("Refreshed profile could not attach to this exact executable: ", error);
            }
        } else {
            logger.warn("Could not refresh exact-build profile: ", error);
        }
    }
    if (!hooksReady) {
        logger.error("SDK native keyboard/gameplay hooks unavailable: ", error);
        status.store(Status::Unsupported);
        return 0;
    }

    AutoSprintEvents events;
    std::optional<AutoSprintFeature> feature;
    feature.emplace(events, [](bool down) {
        auto keyboard = gameApi.localPlayer().getKeyboard();
        if (!(down ? keyboard.press(kInputCtrl) : keyboard.release(kInputCtrl))) return false;
        logger.info("HIDController::", down ? "onKeyDown" : "onKeyUp", " Ctrl");
        return true;
    });
    {
        std::lock_guard lock(eventMutex);
        activeEvents = &events;
        gameplayActive = forwardHeld = shiftHeld = ctrlHeld = false;
    }
    gameApi.Event.Enum.NativeKeyboard.then(&onNativeKeyboard);
    gameApi.Event.Enum.Gameplay.then(&onGameplay);
    logger.info("SDK native keyboard/gameplay events registered");
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
    // Unsubscribe first: the broker drains in-flight callbacks, including any
    // event emission after its state lock has been released.
    gameHooks.reset();
    {
        std::lock_guard<std::recursive_mutex> dispatchLock(eventDispatchMutex);
        {
            std::lock_guard lock(eventMutex);
            activeEvents = nullptr;
        }
        feature.reset();
    }
    status.store(Status::Stopped);
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
