// Binding sounds hand PlaySound a clip in memory, which it reads while the
// clip plays: a clip still held is stopped before the next one frees it.
#include "depthxr/sound_player.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    std::vector<unsigned char> playing;
    std::vector<std::string> calls;
    const unsigned char* played = nullptr;
    const auto play = [&](const unsigned char* clip) {
        calls.push_back("play");
        played = clip;
    };

    // The first clip has nothing to stop.
    std::vector<unsigned char> first(4096, 1);
    const unsigned char* first_data = first.data();
    depthxr::ReplacePlayingSound(playing, std::move(first), [&] { calls.push_back("stop"); }, play);
    Expect(calls == std::vector<std::string>{"play"}, "the first clip plays without a stop");
    Expect(played == first_data && playing.data() == first_data, "the first clip plays from the held buffer");

    // The next clip stops the first while its memory is still held.
    calls.clear();
    std::vector<unsigned char> second(8192, 2);
    const unsigned char* second_data = second.data();
    bool first_held_at_stop = false;
    depthxr::ReplacePlayingSound(
        playing, std::move(second),
        [&] {
            calls.push_back("stop");
            first_held_at_stop = playing.data() == first_data && playing.size() == 4096;
        },
        play);
    Expect(calls == (std::vector<std::string>{"stop", "play"}), "a held clip is stopped before the next plays");
    Expect(first_held_at_stop, "the clip is still held when it is stopped");
    Expect(played == second_data && playing.data() == second_data && playing.size() == 8192,
           "the next clip plays from the held buffer");

    std::cout << "sound player tests passed\n";
    return 0;
}
