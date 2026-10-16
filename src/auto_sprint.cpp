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
    // Restore the known-working direct-poll condition and key ownership logic,
    // with the input state delivered through the Event API every frame.
    if (frame.shouldSprint()) {
        if (!ownSprint_ && !frame.leftCtrlHeld && send_ && send_(true))
            ownSprint_ = true;
    } else {
        release();
    }
}

void AutoSprintFeature::release() {
    if (!ownSprint_) return;
    ownSprint_ = false;
    if (send_) send_(false);
}
