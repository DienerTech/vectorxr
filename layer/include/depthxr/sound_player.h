#pragma once

#include <filesystem>
#include <memory>

#include "depthxr/settings.h"

namespace depthxr {

struct SoundPlayerState;

// Best-effort audible feedback for binding activate/deactivate transitions.
// PlayTransition is non-blocking and safe to call from the OpenXR hot path:
// file validation and playback happen on a dedicated worker thread, and rapid
// toggles collapse to the most recent request.
class SoundPlayer {
  public:
    static SoundPlayer& Instance();

    // Plays the activate or deactivate sound for `sound` when enabled. An empty
    // custom path resolves to the bundled default under <layer_dir>/sounds/.
    void PlayTransition(const SoundFeedback& sound, bool activated, const std::filesystem::path& layer_dir);

    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

  private:
    SoundPlayer();
    ~SoundPlayer();

    std::shared_ptr<SoundPlayerState> state_;
};

} // namespace depthxr
