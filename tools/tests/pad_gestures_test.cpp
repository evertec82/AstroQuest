// Checks the touchpad gestures that buttons make (controllers that have no touchpad): that
// the pad is pressed for as long as its button is held, that a button swipes forward once
// for each press, that a pull is held and let go with its button, and that the game sees each
// of them for what it is, however few frames a second it draws.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src /I tools/tests
//       tools/tests/pad_gestures_test.cpp /Fe:build/tests/pad_gestures_test.exe
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "input/pad_gestures.h"
#include "touch_game_model.h"

using Input::PadGestures;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

/// The gestures as a host makes them, ninety times a second, of what `at(seconds)` says the
/// buttons do.
static std::function<GameModel::Finger(double)> Made(
    const std::function<PadGestures::Controls(double)>& at) {
    struct State {
        PadGestures gestures;
        double moved{-1.0};
        GameModel::Finger now;
    };
    auto state = std::make_shared<State>();
    return [state, at](double time) {
        if (time - state->moved >= 1.0 / 90.0) {
            state->moved = time;
            const PadGestures::Touch touch = state->gestures.Update(time, at(time));
            state->now = {touch.down, touch.x, touch.y, touch.pressed};
        }
        return state->now;
    };
}

static bool Between(double time, double from, double to) {
    return time >= from && time < to;
}

int main() {
    // By themselves.
    {
        PadGestures gestures;
        PadGestures::Touch touch = gestures.Update(0.0, {});
        Check(!touch.down && !touch.pressed && !gestures.IsActive(), "no button, no finger");

        touch = gestures.Update(0.10, {.press = true});
        Check(touch.down && touch.pressed && touch.x == 0.5f && touch.y == 0.5f,
              "the press button presses the pad in its middle, at once");
        touch = gestures.Update(0.15, {});
        Check(touch.down && touch.pressed, "however briefly: it stays pressed for a moment");
        touch = gestures.Update(0.30, {});
        Check(!touch.down && !touch.pressed, "and is let go");
        touch = gestures.Update(0.50, {.press = true});
        touch = gestures.Update(3.00, {.press = true});
        Check(touch.down && touch.pressed, "held, it stays pressed");
        touch = gestures.Update(3.01, {});
        Check(!touch.down && !touch.pressed, "until the button is let go");

        gestures.Reset();
        touch = gestures.Update(4.00, {.swipe = true});
        Check(touch.down && !touch.pressed && touch.y == PadGestures::SwipeFrom,
              "the swipe button puts a finger near the player's edge");
        float least = 1.0f;
        bool lifted = false;
        double lifted_at = 0.0;
        for (double time = 4.01; time < 5.0; time += 0.011) {
            touch = gestures.Update(time, {.swipe = true});
            if (touch.down) {
                least = std::fmin(least, touch.y);
            } else if (!lifted) {
                lifted = true;
                lifted_at = time;
            }
        }
        Check(lifted && least == PadGestures::SwipeTo && lifted_at > 4.25 && lifted_at < 4.45,
              "takes it to the far edge and off, in a third of a second, though the button stays held");
        Check(!gestures.IsActive(), "once: a button that is held does not swipe again");
    }

    for (const double rate : {26.0, 30.0, 45.0, 60.0}) {
        bool pressed = true, tapped = true, swiped = true, twice = true, pulled = true, tugged = true,
             held = true, after = true;
        for (const double phase : {0.0, 0.2, 0.4, 0.6, 0.8}) {
            // Water: the button held for a second.
            GameModel::Seen seen = GameModel::Watch(rate, phase, 2.0, Made([](double time) {
                return PadGestures::Controls{.press = Between(time, 0.2, 1.2)};
            }));
            pressed = pressed && seen.presses == 1 && seen.touches == 1 && seen.shots == 0 &&
                      seen.pressed_frames >= static_cast<int>(rate * 0.95) &&
                      seen.pressed_frames <= static_cast<int>(rate * 1.15) + 1;
            // The button touched for a blink.
            seen = GameModel::Watch(rate, phase, 1.0, Made([](double time) {
                return PadGestures::Controls{.press = Between(time, 0.2, 0.22)};
            }));
            tapped = tapped && seen.presses == 1 && seen.pressed_frames >= 3;
            // One swipe.
            seen = GameModel::Watch(rate, phase, 1.0, Made([](double time) {
                return PadGestures::Controls{.swipe = Between(time, 0.2, 0.25)};
            }));
            swiped = swiped && seen.touches == 1 && seen.frames >= 5 && seen.first_y < 0.25f &&
                     seen.last_y > 0.85f && seen.largest_step < 0.45f && seen.shots == 0 &&
                     seen.presses == 0;
            // The button pressed three times in a hurry: two swipes at least, apart.
            seen = GameModel::Watch(rate, phase, 2.0, Made([](double time) {
                return PadGestures::Controls{.swipe = Between(time, 0.20, 0.25) || Between(time, 0.35, 0.40) ||
                                                      Between(time, 0.75, 0.80)};
            }));
            twice = twice && seen.touches == 3 && seen.least_gap >= 1;
            // The catapult: pulled and held, let go.
            seen = GameModel::Watch(rate, phase, 2.0, Made([](double time) {
                return PadGestures::Controls{.pull = Between(time, 0.2, 1.0)};
            }));
            pulled = pulled && seen.touches == 1 && seen.shots == 1 && seen.first_y > 0.75f &&
                     seen.last_y < 0.05f;
            // Pulled and let go at once.
            seen = GameModel::Watch(rate, phase, 1.5, Made([](double time) {
                return PadGestures::Controls{.pull = Between(time, 0.2, 0.23)};
            }));
            tugged = tugged && seen.touches == 1 && seen.shots == 1;
            // Held for five seconds: nothing shoots before it is let go.
            seen = GameModel::Watch(rate, phase, 5.0, Made([](double time) {
                return PadGestures::Controls{.pull = time >= 0.2};
            }));
            held = held && seen.touches == 1 && seen.shots == 0 && seen.last_y < 0.05f;
            // A pull asked for in the middle of a swipe comes after it.
            seen = GameModel::Watch(rate, phase, 2.5, Made([](double time) {
                return PadGestures::Controls{.swipe = Between(time, 0.2, 0.3), .pull = Between(time, 0.3, 1.2)};
            }));
            after = after && seen.touches == 2 && seen.shots == 1 && seen.least_gap >= 1;
        }
        const std::string fps = " (a game of " + std::to_string(static_cast<int>(rate)) + " frames a second)";
        Check(pressed, "the pad is pressed for as long as its button is held" + fps);
        Check(tapped, "a blink of the button is a press the game sees" + fps);
        Check(swiped, "a swipe is seen from near the player to the far edge, on its way" + fps);
        Check(twice, "swipes one after the other are apart" + fps);
        Check(pulled, "pulled, held and let go: the catapult shoots" + fps);
        Check(tugged, "pulled and let go at once: it shoots as well" + fps);
        Check(held, "held, it does not shoot" + fps);
        Check(after, "a pull in the middle of a swipe comes after the swipe" + fps);
    }

    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
