#pragma once

namespace bedrock {
// Testable and platform-independent AutoSprint policy.
// Sprint only while actively moving forward in the gameplay viewport.
inline bool shouldAutoSprint(bool foregroundMinecraft, bool gameplayCursorHidden,
                             bool holdingForward, bool holdingSneak) noexcept {
    return foregroundMinecraft && gameplayCursorHidden &&
           holdingForward && !holdingSneak;
}
} // namespace bedrock
