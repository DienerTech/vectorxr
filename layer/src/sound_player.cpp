#include "depthxr/sound_player.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
// Hard ceiling on how much audio we will read into memory, regardless of the
// (untrusted) chunk sizes in the file header.
constexpr std::uintmax_t kMaxFileBytes = 16u * 1024u * 1024u;

std::filesystem::path FromUtf8(const std::string& value) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(value.c_str()));
}

uint16_t ReadU16(const unsigned char* bytes) {
    return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t ReadU32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

struct WavInfo {
    size_t data_offset{0};
    size_t data_size{0};
    uint32_t byte_rate{0};
    uint16_t audio_format{0};
    uint16_t bits_per_sample{0};
};

// Parses RIFF/WAVE chunk headers from an in-memory image. Returns false for
// anything that isn't a recognizable WAV container.
bool ParseWav(const std::vector<unsigned char>& bytes, WavInfo& info) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* chunk = bytes.data() + pos;
        const uint32_t chunk_size = ReadU32(chunk + 4);
        const size_t body = pos + 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16 && body + 16 <= bytes.size()) {
            info.audio_format = ReadU16(bytes.data() + body);
            info.byte_rate = ReadU32(bytes.data() + body + 8);
            info.bits_per_sample = ReadU16(bytes.data() + body + 14);
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            info.data_offset = body;
            info.data_size = std::min(static_cast<size_t>(chunk_size), bytes.size() - body);
            have_data = true;
        }

        pos = body + chunk_size + (chunk_size & 1);
    }

    return have_fmt && have_data && info.byte_rate > 0;
}

std::optional<std::vector<unsigned char>> ReadFile(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxFileBytes) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}

// Attenuates 16-bit PCM samples in place. Other formats are left untouched (the
// clip still plays, just at its authored level).
void ScalePcm16(std::vector<unsigned char>& bytes, const WavInfo& info, double gain) {
    if (info.audio_format != 1 || info.bits_per_sample != 16) {
        return;
    }

    const size_t end = std::min(info.data_offset + info.data_size, bytes.size());
    for (size_t i = info.data_offset; i + 1 < end; i += 2) {
        int16_t sample = static_cast<int16_t>(ReadU16(bytes.data() + i));
        double scaled = std::lround(sample * gain);
        scaled = std::clamp(scaled, -32768.0, 32767.0);
        const int16_t result = static_cast<int16_t>(scaled);
        bytes[i] = static_cast<unsigned char>(result & 0xff);
        bytes[i + 1] = static_cast<unsigned char>((result >> 8) & 0xff);
    }
}

} // namespace

struct SoundPlayerState {
    std::mutex mutex;
    std::condition_variable cv;
    std::wstring pending; // path queued for playback
    double pending_gain{1.0};
    bool has_pending{false};
    bool stop{false};
    std::vector<unsigned char> playing; // outlives the async PlaySound call; worker-thread only
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
            double gain = 1.0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait(lock, [&] { return state->has_pending || state->stop; });
                if (state->stop) {
                    return;
                }
                path = std::move(state->pending);
                gain = state->pending_gain;
                state->has_pending = false;
            }

            std::optional<std::vector<unsigned char>> bytes = ReadFile(std::filesystem::path(path));
            if (!bytes.has_value()) {
                continue; // missing or unreadable — skip silently
            }

            WavInfo info;
            if (!ParseWav(*bytes, info)) {
                continue; // not a WAV container
            }
            if (static_cast<double>(info.data_size) / static_cast<double>(info.byte_rate) > kMaxSoundSeconds) {
                continue; // too long
            }

            if (gain < 0.999) {
                ScalePcm16(*bytes, info, std::clamp(gain, 0.0, 1.0));
            }

            state->playing = std::move(*bytes);
            // SND_ASYNC returns immediately and replaces any sound already
            // playing from this call, so toggling on/off never stacks audio.
            PlaySoundW(reinterpret_cast<LPCWSTR>(state->playing.data()), nullptr,
                       SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
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

void SoundPlayer::PlayTransition(const SoundFeedback& sound,
                                 bool activated,
                                 const std::filesystem::path& layer_dir,
                                 int volume_percent,
                                 const wchar_t* default_activate_file,
                                 const wchar_t* default_deactivate_file) {
    if (!sound.enabled) {
        return;
    }

#if defined(_WIN32)
    const std::string& custom = activated ? sound.activate_sound : sound.deactivate_sound;
    std::filesystem::path resolved;
    if (!custom.empty()) {
        resolved = FromUtf8(custom);
    } else if (!layer_dir.empty()) {
        resolved = layer_dir / "sounds" / (activated ? default_activate_file : default_deactivate_file);
    } else {
        return; // no custom path and no known layer directory to resolve a default
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->pending = resolved.wstring();
        state_->pending_gain = std::clamp(volume_percent, 0, 100) / 100.0;
        state_->has_pending = true;
    }
    state_->cv.notify_one();
#else
    (void)activated;
    (void)layer_dir;
    (void)volume_percent;
    (void)default_activate_file;
    (void)default_deactivate_file;
#endif
}

} // namespace depthxr
