#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace depthxr {

// Coordinates a layer-owned action set with the session's single, irreversible
// xrAttachSessionActionSets call. Applications normally get the private set
// appended to their own attachment. Applications that never use OpenXR actions
// receive a delayed, private-only fallback attachment after a frame grace period.
template <typename ActionSetHandle>
class DelayedActionSetAttachment {
  public:
    struct Attempt {
        std::vector<ActionSetHandle> action_sets;
        bool includes_private_action_set{false};
        bool fallback{false};
    };

    explicit DelayedActionSetAttachment(std::uint64_t fallback_grace_frames = 100) noexcept
        : fallback_grace_frames_(fallback_grace_frames) {}

    void Reset() noexcept {
        private_action_set_ = ActionSetHandle{};
        completed_frames_ = 0;
        session_attachment_completed_ = false;
        private_action_set_attached_ = false;
        fallback_attempted_ = false;
        has_seen_frame_boundary_ = false;
    }

    void SetPrivateActionSet(ActionSetHandle action_set) noexcept {
        if (private_action_set_ == action_set) {
            return;
        }
        private_action_set_ = action_set;
        private_action_set_attached_ = false;
        fallback_attempted_ = false;
    }

    void ClearPrivateActionSet() noexcept {
        private_action_set_ = ActionSetHandle{};
        private_action_set_attached_ = false;
        fallback_attempted_ = false;
    }

    [[nodiscard]] Attempt PrepareApplicationAttachment(
        std::span<const ActionSetHandle> application_action_sets) const {
        Attempt attempt;
        attempt.action_sets.assign(application_action_sets.begin(), application_action_sets.end());
        if (private_action_set_ != ActionSetHandle{} &&
            std::find(attempt.action_sets.begin(), attempt.action_sets.end(), private_action_set_) ==
                attempt.action_sets.end()) {
            attempt.action_sets.push_back(private_action_set_);
        }
        attempt.includes_private_action_set =
            private_action_set_ != ActionSetHandle{} &&
            std::find(attempt.action_sets.begin(), attempt.action_sets.end(), private_action_set_) !=
                attempt.action_sets.end();
        return attempt;
    }

    [[nodiscard]] std::optional<Attempt> PrepareFallbackAttachment() noexcept {
        if (private_action_set_ == ActionSetHandle{} || session_attachment_completed_ ||
            private_action_set_attached_ || fallback_attempted_ ||
            completed_frames_ <= fallback_grace_frames_) {
            return std::nullopt;
        }

        fallback_attempted_ = true;
        Attempt attempt;
        attempt.action_sets.push_back(private_action_set_);
        attempt.includes_private_action_set = true;
        attempt.fallback = true;
        return attempt;
    }

    void CompleteAttachment(const Attempt& attempt, bool succeeded) noexcept {
        if (!succeeded) {
            return;
        }
        session_attachment_completed_ = true;
        if (attempt.includes_private_action_set) {
            private_action_set_attached_ = true;
        }
    }

    void NoteCompletedFrame() noexcept {
        if (completed_frames_ != UINT64_MAX) {
            ++completed_frames_;
        }
    }

    // Called once at each application xrBeginFrame boundary. The first call
    // starts frame zero; each later boundary proves the prior logical frame
    // completed far enough for the application to continue its frame loop.
    void NoteFrameBoundary() noexcept {
        if (has_seen_frame_boundary_) {
            NoteCompletedFrame();
        } else {
            has_seen_frame_boundary_ = true;
        }
    }

    [[nodiscard]] bool SessionAttachmentCompleted() const noexcept {
        return session_attachment_completed_;
    }

    [[nodiscard]] bool PrivateActionSetAttached() const noexcept {
        return private_action_set_attached_;
    }

    [[nodiscard]] bool FallbackAttempted() const noexcept {
        return fallback_attempted_;
    }

    [[nodiscard]] std::uint64_t CompletedFrames() const noexcept {
        return completed_frames_;
    }

  private:
    ActionSetHandle private_action_set_{};
    std::uint64_t fallback_grace_frames_{100};
    std::uint64_t completed_frames_{0};
    bool session_attachment_completed_{false};
    bool private_action_set_attached_{false};
    bool fallback_attempted_{false};
    bool has_seen_frame_boundary_{false};
};

} // namespace depthxr
