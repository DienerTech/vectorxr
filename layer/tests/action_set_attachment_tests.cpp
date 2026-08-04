#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "depthxr/delayed_action_set_attachment.h"

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

using Attachment = depthxr::DelayedActionSetAttachment<std::uintptr_t>;
constexpr std::uintptr_t kPrivateActionSet = 900;

struct MockRuntime {
    bool next_result{true};
    std::vector<std::vector<std::uintptr_t>> attachments;

    void Attach(Attachment& attachment, const Attachment::Attempt& attempt) {
        attachments.push_back(attempt.action_sets);
        attachment.CompleteAttachment(attempt, next_result);
    }
};

void TestActionlessApplicationGetsOneDelayedAttachment() {
    Attachment attachment;
    MockRuntime runtime;
    attachment.SetPrivateActionSet(kPrivateActionSet);

    for (int frame = 0; frame < 101; ++frame) {
        attachment.NoteFrameBoundary();
        Expect(!attachment.PrepareFallbackAttachment().has_value(),
               "Eye-gaze fallback attached before the 100-frame application grace period elapsed");
    }

    attachment.NoteFrameBoundary();
    auto fallback = attachment.PrepareFallbackAttachment();
    Expect(fallback.has_value() && fallback->fallback,
           "Actionless application did not produce a delayed eye-gaze fallback attachment");
    runtime.Attach(attachment, *fallback);

    Expect(runtime.attachments.size() == 1 &&
               runtime.attachments[0] == std::vector<std::uintptr_t>{kPrivateActionSet},
           "Fallback attachment did not contain exactly the private eye-gaze action set");
    Expect(attachment.PrivateActionSetAttached() && attachment.SessionAttachmentCompleted(),
           "Successful fallback did not mark the private action set attached");
    Expect(!attachment.PrepareFallbackAttachment().has_value(),
           "Successful fallback attempted a second irreversible session attachment");
}

void TestApplicationAttachmentWinsAndIncludesPrivateSet() {
    Attachment attachment;
    MockRuntime runtime;
    attachment.SetPrivateActionSet(kPrivateActionSet);
    const std::uintptr_t application_sets[] = {10, 20};

    auto attempt = attachment.PrepareApplicationAttachment(application_sets);
    runtime.Attach(attachment, attempt);

    Expect(runtime.attachments[0] ==
               std::vector<std::uintptr_t>({10, 20, kPrivateActionSet}),
           "Application attachment did not preserve app sets before the private eye-gaze set");
    for (int frame = 0; frame < 250; ++frame) {
        attachment.NoteFrameBoundary();
    }
    Expect(!attachment.PrepareFallbackAttachment().has_value(),
           "Application-owned attachment was followed by an invalid fallback attachment");
}

void TestLateApplicationAttachmentStillWins() {
    Attachment attachment;
    MockRuntime runtime;
    attachment.SetPrivateActionSet(kPrivateActionSet);
    for (int frame = 0; frame < 101; ++frame) {
        attachment.NoteFrameBoundary();
    }

    const std::uintptr_t application_set = 33;
    auto attempt = attachment.PrepareApplicationAttachment(
        std::span<const std::uintptr_t>(&application_set, 1));
    runtime.Attach(attachment, attempt);
    attachment.NoteFrameBoundary();

    Expect(!attachment.PrepareFallbackAttachment().has_value(),
           "Late application attachment did not suppress the delayed fallback");
    Expect(runtime.attachments.size() == 1 &&
               runtime.attachments[0] ==
                   std::vector<std::uintptr_t>({application_set, kPrivateActionSet}),
           "Late application attachment did not include both action sets");
}

void TestMissingPrivateResourcesNeverAttach() {
    Attachment attachment;
    for (int frame = 0; frame < 250; ++frame) {
        attachment.NoteFrameBoundary();
    }
    Expect(!attachment.PrepareFallbackAttachment().has_value(),
           "Fallback was attempted without an eye-gaze action set");
}

void TestRejectedFallbackDoesNotRetry() {
    Attachment attachment;
    MockRuntime runtime;
    runtime.next_result = false;
    attachment.SetPrivateActionSet(kPrivateActionSet);
    for (int frame = 0; frame < 102; ++frame) {
        attachment.NoteFrameBoundary();
    }

    auto fallback = attachment.PrepareFallbackAttachment();
    Expect(fallback.has_value(), "Mock runtime rejection test never reached the fallback");
    runtime.Attach(attachment, *fallback);
    for (int frame = 0; frame < 250; ++frame) {
        attachment.NoteFrameBoundary();
    }

    Expect(!attachment.PrivateActionSetAttached() &&
               !attachment.PrepareFallbackAttachment().has_value() &&
               runtime.attachments.size() == 1,
           "Rejected fallback was retried or incorrectly marked attached");
}

void TestLatePrivateResourcesCannotCauseSecondAttachment() {
    Attachment attachment;
    MockRuntime runtime;
    const std::uintptr_t application_set = 44;
    auto attempt = attachment.PrepareApplicationAttachment(
        std::span<const std::uintptr_t>(&application_set, 1));
    runtime.Attach(attachment, attempt);
    attachment.SetPrivateActionSet(kPrivateActionSet);
    for (int frame = 0; frame < 250; ++frame) {
        attachment.NoteFrameBoundary();
    }

    Expect(!attachment.PrepareFallbackAttachment().has_value() &&
               !attachment.PrivateActionSetAttached(),
           "Late-created private set attempted a forbidden second session attachment");
}

} // namespace

int main() {
    TestActionlessApplicationGetsOneDelayedAttachment();
    TestApplicationAttachmentWinsAndIncludesPrivateSet();
    TestLateApplicationAttachmentStillWins();
    TestMissingPrivateResourcesNeverAttach();
    TestRejectedFallbackDoesNotRetry();
    TestLatePrivateResourcesCannotCauseSecondAttachment();
    std::cout << "depthxr_action_set_attachment_tests passed\n";
    return 0;
}
