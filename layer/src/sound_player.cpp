#include "depthxr/sound_player.h"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#endif

namespace depthxr {
namespace {

// Sounds longer than this are ignored, so a user who accidentally points a
// binding at a whole song never gets minutes of audio playing back.
constexpr double kMaxSoundSeconds = 5.0;

std::filesystem::path FromUtf8(const std::string& value) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(value.c_str()));
}

uint32_t ReadU32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

// Returns the playback duration of a PCM/WAV file, or nullopt if the file is
// missing or not a parseable RIFF/WAVE container. Only chunk headers are read;
// the audio samples themselves are skipped, so this stays cheap.
std::optional<double> WavDurationSeconds(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    unsigned char riff[12];
    if (!file.read(reinterpret_cast<char*>(riff), sizeof(riff))) {
        return std::nullopt;
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    bool have_fmt = false;
    bool have_data = false;

    while (!(have_fmt && have_data)) {
        unsigned char chunk[8];
        if (!file.read(reinterpret_cast<char*>(chunk), sizeof(chunk))) {
            break;
        }
        const uint32_t chunk_size = ReadU32(chunk + 4);
        const std::streamoff padded = static_cast<std::streamoff>(chunk_size) + (chunk_size & 1);

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (chunk_size < sizeof(fmt) || !file.read(reinterpret_cast<char*>(fmt), sizeof(fmt))) {
                return std::nullopt;
            }
            byte_rate = ReadU32(fmt + 8);
            have_fmt = true;
            const std::streamoff remaining = padded - static_cast<std::streamoff>(sizeof(fmt));
            if (remaining > 0) {
                file.seekg(remaining, std::ios::cur);
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            file.seekg(padded, std::ios::cur);
        } else {
            file.seekg(padded, std::ios::cur);
        }
    }

    if (!have_fmt || !have_data || byte_rate == 0) {
        return std::nullopt;
    }
    return static_cast<double>(data_size) / static_cast<double>(byte_rate);
}

} // namespace

struct SoundPlayerState {
    std::mutex mutex;
    std::condition_variable cv;
    std::wstring pending; // path queued for playback
    bool has_pending{false};
    bool stop{false};
    std::wstring playing; // outlives the async PlaySound call; worker-thread only
};

SoundPlayer& SoundPlayer::Instance() {
    static SoundPlayer instance;
    return instance;
}

SoundPlayer::SoundPlayer() : state_(std::make_shared<SoundPlayerState>()) {
#if defined(_WIN32)
    // The worker holds its own reference to the shared state and is detached, so
    // the destructor never has to join it (joining during DLL unload risks a
    // loader-lock deadlock). It drains and exits once `stop` is set.
    std::shared_ptr<SoundPlayerState> state = state_;
    std::thread([state]() {
        for (;;) {
            std::wstring path;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait(lock, [&] { return state->has_pending || state->stop; });
                if (state->stop) {
                    return;
                }
                path = std::move(state->pending);
                state->has_pending = false;
            }

            const std::optional<double> duration = WavDurationSeconds(std::filesystem::path(path));
            if (!duration.has_value() || *duration > kMaxSoundSeconds) {
                continue; // missing, unreadable, or too long — skip silently
            }

            state->playing = std::move(path);
            // SND_ASYNC returns immediately and replaces any sound already
            // playing from this call, so toggling on/off never stacks audio.
            PlaySoundW(state->playing.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        }
    }).detach();
#endif
}

SoundPlayer::~SoundPlayer() {
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
    }
    state_->cv.notify_all();
    PlaySoundW(nullptr, nullptr, SND_PURGE);
#endif
}

void SoundPlayer::PlayTransition(const SoundFeedback& sound, bool activated, const std::filesystem::path& layer_dir) {
    if (!sound.enabled) {
        return;
    }

#if defined(_WIN32)
    const std::string& custom = activated ? sound.activate_sound : sound.deactivate_sound;
    std::filesystem::path resolved;
    if (!custom.empty()) {
        resolved = FromUtf8(custom);
    } else if (!layer_dir.empty()) {
        resolved = layer_dir / "sounds" / (activated ? L"activate.wav" : L"deactivate.wav");
    } else {
        return; // no custom path and no known layer directory to resolve a default
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pending = resolved.wstring();
        state_->has_pending = true;
    }
    state_->cv.notify_one();
#else
    (void)activated;
    (void)layer_dir;
#endif
}

} // namespace depthxr
