#ifdef _WIN32
// Adapt verified Minecraft input events into the sample feature. No OS key
// polling or SendInput is used; generated Ctrl transitions use Keyboard::feed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/hook_runtime.h>
#include <bedrock/events.hpp>
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
std::array<BDS_HookToken, 3> hookTokens{};
struct NextFrame { BDS_HookNext next{}; void* frame{}; };
thread_local NextFrame keyboardFrame, cursorFrame;
thread_local NextFrame* activeCursorFrame{};
thread_local bool syntheticCtrlFeed{};
bool __fastcall callNextKeyboard(int key, bool down) {
    BDS_KeyboardArgs args{key, down ? 1 : 0, 0};
    if (keyboardFrame.next) keyboardFrame.next(keyboardFrame.frame, &args);
    else BDS_HookDispatch(BDS_HOOK_KEYBOARD_FEED, &args);
    return args.result != 0;
}
void __fastcall callNextCursor(void* self) {
    if (activeCursorFrame && activeCursorFrame->next) {
        BDS_CursorArgs args{self}; activeCursorFrame->next(activeCursorFrame->frame, &args);
    }
}
bedrock::Logger logger{"AutoSprint"};
bedrock::FileLogSink fileLog;
std::mutex eventMutex;
// EventBus and AutoSprintFeature are single-owner objects. Serialize callbacks
// from concurrent Minecraft hook threads; recursive entry is needed when a
// synthetic transition synchronously passes through Keyboard::feed again.
std::recursive_mutex eventDispatchMutex;
AutoSprintEvents* activeEvents{};
bool gameplayActive{};
bool forwardHeld{};
bool shiftHeld{};
bool ctrlHeld{};
std::array<bool, 5> shiftKeyDown{};
std::array<bool, 5> ctrlKeyDown{};
std::atomic<bool> loggedFirstKey{false};

class SyntheticCtrlScope {
public:
    SyntheticCtrlScope() noexcept : previous_(syntheticCtrlFeed) {
        syntheticCtrlFeed = true;
    }
    ~SyntheticCtrlScope() { syntheticCtrlFeed = previous_; }
    SyntheticCtrlScope(const SyntheticCtrlScope&) = delete;
    SyntheticCtrlScope& operator=(const SyntheticCtrlScope&) = delete;
private:
    bool previous_;
};

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

bool __fastcall onKeyboardFeed(int inputKey, bool down) {
    const auto original = originalKeyboardFeed.load(std::memory_order_acquire);
    const bool result = original ? original(inputKey, down) : false;
    if (!loggedFirstKey.exchange(true, std::memory_order_acq_rel))
        logger.info("Keyboard::feed active; first InputKey=", inputKey,
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
        if (isCtrlKey(inputKey) && !syntheticCtrlFeed) {
            for (std::size_t i = 0; i < kCtrlCodes.size(); ++i)
                if (kCtrlCodes[i] == inputKey) ctrlKeyDown[i] = down;
            ctrlHeld = std::any_of(ctrlKeyDown.begin(), ctrlKeyDown.end(),
                                   [](bool value) { return value; });
        }
        events = activeEvents;
        frame = inputFrameLocked();
    }
    // Event handlers may inject a synthetic Keyboard::feed event, which
    // re-enters this hook. Never emit while holding eventMutex.
    if (events && !syntheticCtrlFeed) events->emit(frame);
    return result;
}

void __fastcall onGrabCursor(void* self) {
    if (auto original = originalGrabCursor.load(std::memory_order_acquire)) original(self);
    std::lock_guard<std::recursive_mutex> dispatchLock(eventDispatchMutex);
    AutoSprintEvents* events{};
    InputFrameEvent frame;
    bool changed{};
    {
        std::lock_guard lock(eventMutex);
        changed = !gameplayActive;
        gameplayActive = true;
        if (!changed) return;
        events = activeEvents;
        frame = inputFrameLocked();
    }
    if (events) events->emit(frame);
    logger.info("Minecraft gameplay input active");
}

void __fastcall onReleaseCursor(void* self) {
    std::lock_guard<std::recursive_mutex> dispatchLock(eventDispatchMutex);
    AutoSprintEvents* events{};
    InputFrameEvent frame;
    bool wasActive{};
    bool changed{};
    {
        std::lock_guard lock(eventMutex);
        wasActive = gameplayActive;
        changed = gameplayActive || forwardHeld || shiftHeld || ctrlHeld;
        gameplayActive = false;
        forwardHeld = shiftHeld = ctrlHeld = false;
        shiftKeyDown.fill(false);
        ctrlKeyDown.fill(false);
        if (changed) {
            events = activeEvents;
            frame = inputFrameLocked();
        }
    }
    if (events) events->emit(frame);
    if (wasActive) logger.info("Minecraft gameplay input inactive");
    if (auto original = originalReleaseCursor.load(std::memory_order_acquire)) original(self);
}

void BDS_HOOK_CALL keyboardMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_KeyboardArgs*>(raw);
    const auto old = keyboardFrame; keyboardFrame = {next, frame};
    args->result = onKeyboardFeed(args->key, args->down != 0) ? 1 : 0;
    keyboardFrame = old;
}
void BDS_HOOK_CALL cursorMiddleware(void* user, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_CursorArgs*>(raw);
    const bool grabbing = reinterpret_cast<std::uintptr_t>(user) == 1;
    const auto oldFrame = cursorFrame; cursorFrame = {next, frame};
    const auto oldActive = activeCursorFrame; activeCursorFrame = &cursorFrame;
    if (grabbing) onGrabCursor(args->self); else onReleaseCursor(args->self);
    activeCursorFrame = oldActive; cursorFrame = oldFrame;
}

void removeHooks() noexcept {
    for (auto& token : hookTokens) {
        if (token) BDS_HookUnsubscribe(token);
        token = nullptr;
    }
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
    const auto resolveRequired = [&] {
        keyboard = bedrock::resolveSharedHook(resolver, "Keyboard::feed", BDS_HOOK_KEYBOARD_FEED);
        grab = bedrock::resolveSharedHook(resolver, "ClientInstance::grabCursor", BDS_HOOK_GRAB_CURSOR);
        release = bedrock::resolveSharedHook(resolver, "ClientInstance::releaseCursor", BDS_HOOK_RELEASE_CURSOR);
    };
    const auto valid = [](const auto& symbol) {
        return symbol && bedrock::isUsableSharedHookResolution(*symbol);
    };
    const auto requirementsMet = [&] {
        return valid(keyboard) && valid(grab) && valid(release);
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

    originalKeyboardFeed.store(&callNextKeyboard);
    originalGrabCursor.store(&callNextCursor);
    originalReleaseCursor.store(&callNextCursor);
    AutoSprintEvents events;
    std::optional<AutoSprintFeature> feature;
    feature.emplace(events, [](bool down) {
        BDS_KeyboardArgs args{kInputCtrl, down ? 1 : 0, 0};
        // Keep native input on the current Minecraft hook thread. Mark the
        // synchronous nested callback so it doesn't treat our Ctrl event as
        // physical input or recursively emit another AutoSprint event.
        {
            SyntheticCtrlScope syntheticScope;
            BDS_HookDispatch(BDS_HOOK_KEYBOARD_FEED, &args);
        }
        logger.info("Minecraft sprint Ctrl ", down ? "pressed" : "released");
        // BDS_HookDispatch has no native Keyboard::feed result contract;
        // ownership tracks whether we submitted the transition.
        return true;
    });
    {
        std::lock_guard lock(eventMutex);
        activeEvents = &events;
        gameplayActive = forwardHeld = shiftHeld = ctrlHeld = false;
    }
    const bool created =
        BDS_HookSubscribe(BDS_HOOK_KEYBOARD_FEED, reinterpret_cast<void*>(keyboard->address), keyboardMiddleware, nullptr, &hookTokens[0]) &&
        BDS_HookSubscribe(BDS_HOOK_GRAB_CURSOR, reinterpret_cast<void*>(grab->address), cursorMiddleware, reinterpret_cast<void*>(1), &hookTokens[1]) &&
        BDS_HookSubscribe(BDS_HOOK_RELEASE_CURSOR, reinterpret_cast<void*>(release->address), cursorMiddleware, nullptr, &hookTokens[2]);
    if (!created) {
        {
            std::lock_guard lock(eventMutex);
            activeEvents = nullptr;
        }
        removeHooks();
        logger.error("Could not subscribe to the shared Minecraft hook runtime");
        status.store(Status::Failed);
        return 1;
    }
    logger.info("Minecraft input hooks registered with shared runtime");
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
    removeHooks();
    {
        std::lock_guard lock(eventMutex);
        activeEvents = nullptr;
    }
    // Feature releases only the Ctrl transition it generated; the original
    // Minecraft Keyboard::feed trampoline is still available here.
    feature.reset();
    status.store(Status::Stopped);
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
