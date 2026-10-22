#include "auto_sprint_feature.hpp"
#include <utility>

AutoSprintFeature::AutoSprintFeature(AutoSprintEvents& events, SendSprint send)
    : events_(events), send_(std::move(send)) {
    frameId_ = events_.on<InputFrameEvent>(
        [this](const InputFrameEvent& frame) { onInputFrame(frame); });
}

AutoSprintFeature::~AutoSprintFeature() {
    events_.off<InputFrameEvent>(frameId_);
    release();
}

void AutoSprintFeature::onInputFrame(const InputFrameEvent& frame) {
    // Input snapshots are emitted only when Minecraft's own key or cursor
    // state changes; this keeps the feature event-driven and avoids polling.
    if (frame.shouldSprint()) {
        if (frame.leftCtrlHeld) {
            // The physical key has taken ownership of Ctrl. Do not release it
            // when the sprint condition later becomes false.
            ownSprint_ = false;
        } else if (!ownSprint_ && send_ && send_(true)) {
            ownSprint_ = true;
        }
    } else {
        if (ownSprint_ && frame.leftCtrlHeld)
            ownSprint_ = false;
        else
            release();
    }
}

void AutoSprintFeature::release() {
    if (!ownSprint_) return;
    ownSprint_ = false;
    if (send_) send_(false);
}
