#pragma once
#include <bedrock/events.hpp>
#include <functional>

// This is the ENTIRE mod feature, owned by the sample project, not the SDK.
class AutoSprintFeature {
public:
    // Return true when the client successfully sent a sprint key transition.
    using SendSprint = std::function<bool(bool)>; // true = press, false = release
    using CanPress = std::function<bool()>; // don't override a user's held Ctrl

    AutoSprintFeature(bedrock::ClientEvents& events,
                      SendSprint send, CanPress canPress);
    ~AutoSprintFeature();

    AutoSprintFeature(const AutoSprintFeature&) = delete;
    AutoSprintFeature& operator=(const AutoSprintFeature&) = delete;

private:
    void update();
    void release();

    bedrock::ClientEvents& events_;
    SendSprint send_;
    CanPress canPress_;
    bool active_{};
    bool forward_{};
    bool leftShift_{};
    bool rightShift_{};
    bool ownSprint_{};
    bedrock::EventChannel<bedrock::KeyEvent>::ListenerId keyId_{};
    bedrock::EventChannel<bedrock::GameplayEvent>::ListenerId gameplayId_{};
    bedrock::EventChannel<bedrock::UpdateEvent>::ListenerId updateId_{};
};
