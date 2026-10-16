#include "auto_sprint_feature.hpp"
#include <utility>

AutoSprintFeature::AutoSprintFeature(
    bedrock::ClientEvents& events, SendSprint send, CanPress canPress)
    : events_(events), send_(std::move(send)), canPress_(std::move(canPress)) {
    keyId_ = events_.on<bedrock::KeyEvent>([this](const auto& e) {
        switch (e.virtualKey) {
            case 'W': forward_ = e.pressed; break;
            case 0xA0: leftShift_ = e.pressed; break;  // VK_LSHIFT
            case 0xA1: rightShift_ = e.pressed; break; // VK_RSHIFT
        }
    });
    gameplayId_ = events_.on<bedrock::GameplayEvent>(
        [this](const auto& e) { active_ = e.active; });
    updateId_ = events_.on<bedrock::UpdateEvent>(
        [this](const auto&) { update(); });
}

AutoSprintFeature::~AutoSprintFeature() {
    release();
    events_.off<bedrock::KeyEvent>(keyId_);
    events_.off<bedrock::GameplayEvent>(gameplayId_);
    events_.off<bedrock::UpdateEvent>(updateId_);
}

void AutoSprintFeature::update() {
    const bool shouldSprint = active_ && forward_ && !leftShift_ && !rightShift_;
    if (shouldSprint) {
        if (!ownSprint_ && canPress_ && canPress_() && send_ && send_(true))
            ownSprint_ = true;
    } else {
        release();
    }
}

void AutoSprintFeature::release() {
    if (ownSprint_) {
        ownSprint_ = false;
        if (send_) send_(false);
    }
}
