#pragma once
#include <bedrock/api/events.hpp>
#include <functional>

// Sample-owned input snapshot event. Not a BedrockDynamicSDK core dependency.
// A complete state every frame avoids lost KeyEvent edges.
struct InputFrameEvent {
    bool minecraftFocused{};
    bool gameplayCursorHidden{};
    bool forwardHeld{};
    bool shiftHeld{};
    bool leftCtrlHeld{};

    [[nodiscard]] bool shouldSprint() const noexcept {
        return minecraftFocused && gameplayCursorHidden && forwardHeld && !shiftHeld;
    }
};
using AutoSprintEvents = bedrock::EventBus<InputFrameEvent>;

class AutoSprintFeature {
public:
    using SendSprint = std::function<bool(bool)>;
    explicit AutoSprintFeature(AutoSprintEvents& events, SendSprint send);
    ~AutoSprintFeature();

    AutoSprintFeature(const AutoSprintFeature&) = delete;
    AutoSprintFeature& operator=(const AutoSprintFeature&) = delete;

    [[nodiscard]] bool ownsSprint() const noexcept { return ownSprint_; }
private:
    void onInputFrame(const InputFrameEvent& frame);
    void release();
    AutoSprintEvents& events_;
    SendSprint send_;
    bool ownSprint_{};
    bedrock::EventChannel<InputFrameEvent>::ListenerId frameId_{};
};
