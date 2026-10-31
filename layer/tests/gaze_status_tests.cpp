#include "depthxr/gaze_status.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace depthxr;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    GazeStatus gaze;
    const auto now = GazeStatus::Clock::now();
    const auto later = now + std::chrono::milliseconds(100);
    Check(gaze.Describe(false, true, now).tone == 0, "layer-enabled extension is not a game request");
    Check(gaze.Describe(true, false, now).text == "Extension unavailable", "unavailable extension");
    Check(gaze.Describe(true, true, now).text == "Enabled / no pose queries yet", "enabled alone is not receiving");
    gaze.Observe(true, true, {0,0,0,1}, false, now);
    Check(gaze.Describe(true,true,now).tone == 2, "first sample cannot prove motion");
    gaze.Observe(true, true, {0,0,0,-1}, true, later);
    Check(gaze.Describe(true,true,later).tone == 2, "quaternion sign is not motion");
    gaze.Observe(true, true, {0,0.1f,0,0.995f}, true, later);
    Check(gaze.Describe(true,true,later).tone == 1, "changing tracked pose");
    gaze.Observe(true, true, {0,0.1f,0,0.995f}, true, now + std::chrono::seconds(3));
    Check(gaze.Describe(true,true,now+std::chrono::seconds(3)).text == "Receiving / poses unchanged", "valid retained pose must not stay green");
    Check(gaze.Describe(true,true,now+std::chrono::seconds(6)).text == "No recent game pose queries", "query freshness independent of OSD heartbeat");
    gaze.Observe(false,false,{},true,later);
    Check(gaze.Describe(true,true,later).tone == 3, "invalid pose clears validity immediately");
    gaze.Observe(true,false,{0,0,0,1},true,later);
    Check(gaze.Describe(true,true,later).text == "Valid pose / not tracked", "tracked flag separate from validity");
    gaze.Observe(true,true,{0,0.2f,0,0.98f},false,later);
    Check(gaze.Describe(true,true,later).tone == 2, "different base spaces are not comparable movement");
    gaze.Observe(true,true,{0,std::numeric_limits<float>::quiet_NaN(),0,1},true,later);
    Check(gaze.Describe(true,true,later).tone == 3, "non-finite orientation invalid");
    gaze.Reset();
    Check(gaze.Describe(true,true,later).text == "Enabled / no pose queries yet", "session reset drops old samples");
}
