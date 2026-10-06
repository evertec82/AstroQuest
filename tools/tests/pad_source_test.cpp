// Tries Input::PadSource (shadps4-arm64-main/src/input/pad_source.h): which of a PC's gamepad
// and the headset's own controllers plays, by which was used last.
//   source tools/env-win.sh
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src \
//       tools/tests/pad_source_test.cpp /Fe:build/pad_source_test.exe
#include <cstdio>

#include "input/pad_source.h"

static int failed = 0;

static void Check(const char* what, bool got, bool want) {
    if (got == want) {
        std::printf("ok    %s\n", what);
    } else {
        ++failed;
        std::printf("FAIL  %s: got %d, want %d\n", what, got, want);
    }
}

/// Frames of the headset from `from` to `to` seconds, 90 a second, with nothing used.
static bool Frames(Input::PadSource& source, double from, double to, bool gamepad) {
    bool plays = source.HeadsetPlays();
    for (double t = from; t < to; t += 1.0 / 90.0) {
        plays = source.Update(t, gamepad, false);
    }
    return plays;
}

int main() {
    {
        Input::PadSource source;
        Check("no gamepad: the headset's controllers play", source.Update(0.0, false, false), true);
        Check("a gamepad that comes later does not take over by being there",
              Frames(source, 0.1, 2.0, true), true);
        Check("its first use takes over", source.GamepadUsed(2.0), true);
        Check("and the headset's controllers no longer play", source.Update(2.01, true, false),
              false);
        Check("a second use takes nothing over", source.GamepadUsed(2.1), false);
    }
    {
        // A gamepad lies on the desk; the player holds the headset's controllers.
        Input::PadSource source;
        Check("a gamepad from the start plays", source.Update(0.0, true, false), false);
        Check("still so while nothing is used", Frames(source, 0.0, 5.0, true), false);
        Check("a button of the headset's controllers: not at once",
              source.Update(5.0, true, true), false);
        Check("nor a moment later", source.Update(5.1, true, false), false);
        Check("but once the gamepad has said nothing for a while",
              source.Update(5.0 + Input::PadSource::Echo + 0.01, true, false), true);
        Check("and it stays so", Frames(source, 5.3, 20.0, true), true);
        Check("until the gamepad is used", source.GamepadUsed(20.0), true);
        Check("which then plays", source.Update(20.01, true, false), false);
        Check("and the headset's controllers take over again when used",
              (source.Update(30.0, true, true), source.Update(30.3, true, false)), true);
    }
    {
        // A gamepad made of the headset's controllers: it says what they say.
        Input::PadSource source;
        source.Update(0.0, true, false);
        bool plays = false;
        for (int press = 0; press < 20; ++press) {
            const double t = 1.0 + press * 0.7;
            // The copy comes a little before or a little after.
            const double copy = t + (press % 2 == 0 ? -0.03 : 0.04);
            if (copy < t) {
                source.GamepadUsed(copy);
            }
            plays = plays || source.Update(t, true, true);
            if (copy >= t) {
                plays = plays || source.Update(copy - 0.001, true, false);
                source.GamepadUsed(copy);
            }
            plays = plays || Frames(source, copy + 0.01, t + 0.69, true);
        }
        Check("a gamepad that echoes the headset's controllers keeps playing", plays, false);
    }
    {
        // Both in use at once (somebody else holds the gamepad): the gamepad keeps it.
        Input::PadSource source;
        source.Update(0.0, true, false);
        source.GamepadUsed(1.0);
        Check("used right after the gamepad: not taken over",
              (source.Update(1.1, true, true), source.Update(1.5, true, false)), false);
        Check("used long after the gamepad: taken over",
              (source.Update(3.0, true, true), source.Update(3.3, true, false)), true);
    }
    {
        // The gamepad goes away while it plays.
        Input::PadSource source;
        source.Update(0.0, true, false);
        Check("the gamepad is gone: the headset's controllers play",
              source.Update(1.0, false, false), true);
        Check("back, it waits to be used", source.Update(2.0, true, false), true);
    }
    std::printf("failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
