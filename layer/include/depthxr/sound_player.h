#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "depthxr/settings.h"

namespace depthxr {

struct SoundPlayerState;

// The sound worker's hand-off (exposed for tests): makes `next` the clip in
// `playing` and starts it with `play`. PlaySound reads a SND_MEMORY|SND_ASYNC
// clip from our buffer while it plays, and starting another sound does not
// wait for that, so a clip still held is ended with `stop` (synchronous)
// before its memory is freed.
void ReplacePlayingSound(std::vector<unsigned char>& playing,
                         std::vector<unsigned char>&& next,
                         const std::function<void()>& stop,
                         const std::function<void(const unsigned char*)>& play);

// Best-effort audible feedback for binding activate/deactivate transitions.
// PlayTransition is non-blocking and safe to call from the OpenXR hot path:
// file validation and playback happen on a dedicated worker thread, and rapid
// toggles collapse to the most recent request.
class SoundPlayer {
  public:
    static SoundPlayer& Instance();

    // Plays the activate or deactivate sound for `sound` when enabled. An empty
    // custom path resolves to the bundled default under <layer_dir>/sounds/;
    // actions with their own cues (e.g. pivot origin set/release) pass distinct
    // default file names. `volume_percent` (0-100) scales playback; sounds are
    // loaded into memory so 16-bit PCM can be attenuated.
    void PlayTransition(const SoundFeedback& sound,
                        bool activated,
                        const std::filesystem::path& layer_dir,
                        int volume_percent,
                        const wchar_t* default_activate_file = L"activate.wav",
                        const wchar_t* default_deactivate_file = L"deactivate.wav");

    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

  private:
    SoundPlayer();
    ~SoundPlayer();

    std::shared_ptr<SoundPlayerState> state_;
};

} // namespace depthxr
