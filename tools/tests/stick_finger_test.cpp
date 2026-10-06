// Checks the finger a stick moves over a touchpad (controllers that have no touchpad): that a
// stick let go lifts the finger where it was, not back at the centre it flies to, that a stick
// moved through its centre drags without lifting, and that a stick which drifts touches nothing.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src /I tools/tests
//       tools/tests/stick_finger_test.cpp /Fe:build/tests/stick_finger_test.exe
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/stick_finger.h"
#include "touch_game_model.h"

using Input::StickFinger;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

struct Run {
    int touches{};
    bool down{};
    double touched_at{-1.0};
    // Where the finger came down and where it lifted, the last time it did either.
    float first_x{}, first_y{}, last_x{}, last_y{};
    // The nearest and the furthest it got, and when it lifted.
    float least_y{1.0f}, most_y{0.0f};
    double lifted_at{-1.0};
};

/// Has a stick go where `at(seconds)` says, read `rate` times a second for `length` seconds.
static Run Play(double rate, double length,
                const std::function<void(double, float&, float&)>& at) {
    StickFinger finger;
    Run run;
    for (double time = 0.0; time < length; time += 1.0 / rate) {
        float x = 0.0f, y = 0.0f;
        at(time, x, y);
        const StickFinger::Touch touch = finger.Update(time, x, y);
        if (touch.down && !run.down) {
            ++run.touches;
            run.touched_at = time;
            run.first_x = touch.x;
            run.first_y = touch.y;
        }
        if (touch.down) {
            run.last_x = touch.x;
            run.last_y = touch.y;
            run.least_y = std::fmin(run.least_y, touch.y);
            run.most_y = std::fmax(run.most_y, touch.y);
        } else if (run.down) {
            run.lifted_at = time;
        }
        run.down = touch.down;
    }
    return run;
}

/// From `from` to `to` between two moments, at rest before and after.
static float Ramp(double time, double begin, double end, float from, float to) {
    if (time <= begin) {
        return from;
    }
    if (time >= end) {
        return to;
    }
    return from + (to - from) * static_cast<float>((time - begin) / (end - begin));
}

/// A stick let go at `when` from `from`: back at its centre within a few hundredths of a second.
static float LetGo(double time, double when, float from) {
    return time < when ? from : from * std::exp(-static_cast<float>((time - when) / 0.008));
}

int main() {
    for (const double rate : {72.0, 90.0, 120.0, 250.0}) {
        const std::string hz = " (read " + std::to_string(static_cast<int>(rate)) + " times a second)";

        // Pulled towards the player, held, let go: the catapult.
        Run run = Play(rate, 1.2, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.5 ? Ramp(time, 0.1, 0.25, 0.0f, 1.0f) : LetGo(time, 0.5, 1.0f);
        });
        Check(run.touches == 1 && !run.down, "pulled and let go: one touch, lifted" + hz);
        Check(std::abs(run.first_y - 0.25f) < 0.01f && std::abs(run.first_x - 0.5f) < 0.01f,
              "it comes down behind the middle of the pad, on the side the stick comes from" + hz);
        Check(run.last_y > 0.99f, "it lifts at the pad's near edge, not back at the centre" + hz);
        Check(run.lifted_at > 0.5 && run.lifted_at < 0.62, "it lifts within a moment" + hz);

        // Pulled and let go at once, without holding.
        run = Play(rate, 1.0, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.2 ? Ramp(time, 0.1, 0.2, 0.0f, 1.0f) : LetGo(time, 0.2, 1.0f);
        });
        Check(run.touches == 1 && run.last_y > 0.9f, "pulled and let go at once" + hz);
        Check(run.lifted_at - run.touched_at > 0.26, "and still down for a quarter of a second" + hz);

        // Pulled four fifths of the way only: dragged far enough for the catapult all the same.
        run = Play(rate, 1.2, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.5 ? Ramp(time, 0.1, 0.25, 0.0f, 0.8f) : LetGo(time, 0.5, 0.8f);
        });
        Check(run.touches == 1 && run.last_y - run.first_y > 0.45f,
              "four fifths of the way is the whole way" + hz);

        // Flicked away from the player.
        run = Play(rate, 1.0, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.16 ? Ramp(time, 0.1, 0.16, 0.0f, -1.0f) : LetGo(time, 0.16, -1.0f);
        });
        // (Read seventy times a second, a flick is seen at four or five places: the furthest
        // of them is where the finger lifts.)
        Check(run.touches == 1 && run.last_y < 0.01f, "flicked forward: lifts at the far edge" + hz);

        // Pushed forward, then pulled back through the centre in one go, then let go.
        run = Play(rate, 1.5, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.4   ? Ramp(time, 0.1, 0.2, 0.0f, -1.0f)
                : time < 0.8 ? Ramp(time, 0.4, 0.52, -1.0f, 1.0f)
                             : LetGo(time, 0.8, 1.0f);
        });
        Check(run.touches == 1, "through the centre without lifting" + hz);
        Check(run.least_y < 0.01f && run.last_y > 0.99f, "dragged from the far edge to the near" + hz);

        // The same, slowly: a third of a second from one side to the other.
        run = Play(rate, 2.0, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.4   ? Ramp(time, 0.1, 0.2, 0.0f, -1.0f)
                : time < 1.2 ? Ramp(time, 0.4, 0.75, -1.0f, 1.0f)
                             : LetGo(time, 1.2, 1.0f);
        });
        Check(run.touches == 1 && run.last_y > 0.99f, "slowly through the centre without lifting" + hz);

        // Brought back by hand, slowly: the finger comes back with it and lifts near the middle.
        run = Play(rate, 2.0, [](double time, float& x, float& y) {
            x = 0.0f;
            y = time < 0.5 ? Ramp(time, 0.1, 0.25, 0.0f, 1.0f) : Ramp(time, 0.5, 1.2, 1.0f, 0.0f);
        });
        Check(run.touches == 1 && run.last_y < 0.72f && run.most_y > 0.99f,
              "brought back by hand: the finger comes back with it" + hz);

        // Sideways, and round the rim: the finger follows.
        run = Play(rate, 1.5, [](double time, float& x, float& y) {
            const float out = time < 1.0 ? Ramp(time, 0.1, 0.2, 0.0f, 1.0f) : LetGo(time, 1.0, 1.0f);
            const float angle = Ramp(time, 0.3, 0.9, 0.0f, 1.5708f);
            x = out * std::cos(angle);
            y = out * std::sin(angle);
        });
        Check(run.touches == 1 && std::abs(run.last_x - 0.5f) < 0.03f && run.last_y > 0.99f,
              "round the rim from the right to the near edge" + hz);

        // A stick that drifts: it never rests, and never touches.
        run = Play(rate, 1.0, [](double, float& x, float& y) {
            x = 0.32f;
            y = 0.05f;
        });
        Check(run.touches == 0, "a stick that drifts touches nothing" + hz);

        // Two pulls: two touches.
        run = Play(rate, 2.0, [](double time, float& x, float& y) {
            x = 0.0f;
            const double in = std::fmod(time, 1.0);
            y = in < 0.5 ? Ramp(in, 0.1, 0.25, 0.0f, 1.0f) : LetGo(in, 0.5, 1.0f);
        });
        Check(run.touches == 2, "pulled twice: two touches" + hz);

        // Resting just short of where a touch begins, with a tremble: nothing.
        run = Play(rate, 1.0, [](double time, float& x, float& y) {
            x = 0.0f;
            y = 0.2f + 0.03f * std::sin(static_cast<float>(time) * 80.0f);
        });
        Check(run.touches == 0, "a stick short of touching does not touch" + hz);
    }

    // What the game makes of it: it looks at the pad once a frame, and at thirty frames a
    // second (which is what a Quest manages in a level) a stick is at its end before it has
    // looked twice.
    const auto stick = [](const std::function<void(double, float&, float&)>& at) {
        // (The host moves the finger for every picture the headset shows.)
        struct State {
            StickFinger finger;
            double moved{-1.0};
            GameModel::Finger now;
        };
        auto state = std::make_shared<State>();
        return [state, at](double time) {
            if (time - state->moved >= 1.0 / 90.0) {
                state->moved = time;
                float x = 0.0f, y = 0.0f;
                at(time, x, y);
                const StickFinger::Touch touch = state->finger.Update(time, x, y);
                state->now = {touch.down, touch.x, touch.y, false};
            }
            return state->now;
        };
    };
    for (const double rate : {26.0, 30.0, 45.0, 60.0}) {
        bool pulled = true, flicked = true, half = true, swiped = true, twice = true;
        for (const double phase : {0.0, 0.2, 0.4, 0.6, 0.8}) {
            // The catapult: the stick pulled back as fast as a thumb does it, held, let go.
            GameModel::Seen seen = GameModel::Watch(rate, phase, 1.5, stick([](double time, float& x, float& y) {
                x = 0.0f;
                y = time < 0.7 ? Ramp(time, 0.1, 0.14, 0.0f, 1.0f) : LetGo(time, 0.7, 1.0f);
            }));
            pulled = pulled && seen.touches == 1 && seen.shots == 1;
            // Pulled and let go at once.
            seen = GameModel::Watch(rate, phase, 1.5, stick([](double time, float& x, float& y) {
                x = 0.0f;
                y = time < 0.14 ? Ramp(time, 0.1, 0.14, 0.0f, 1.0f) : LetGo(time, 0.14, 1.0f);
            }));
            flicked = flicked && seen.touches == 1 && seen.shots == 1;
            // Pulled six tenths of the way only.
            seen = GameModel::Watch(rate, phase, 1.5, stick([](double time, float& x, float& y) {
                x = 0.0f;
                y = time < 0.7 ? Ramp(time, 0.1, 0.14, 0.0f, 0.6f) : LetGo(time, 0.7, 0.6f);
            }));
            half = half && seen.shots == 1;
            // A flick forward: a swipe the game can follow, from near the player to the far edge.
            seen = GameModel::Watch(rate, phase, 1.5, stick([](double time, float& x, float& y) {
                x = 0.0f;
                y = time < 0.14 ? Ramp(time, 0.1, 0.14, 0.0f, -1.0f) : LetGo(time, 0.14, -1.0f);
            }));
            swiped = swiped && seen.touches == 1 && seen.frames >= 5 && seen.first_y < 0.3f &&
                     seen.last_y > 0.98f && seen.largest_step < 0.45f && seen.shots == 0;
            // Two flicks, one right after the other: two swipes with the finger off between.
            seen = GameModel::Watch(rate, phase, 1.8, stick([](double time, float& x, float& y) {
                x = 0.0f;
                const double in = std::fmod(time, 0.6);
                y = in < 0.14 ? Ramp(in, 0.1, 0.14, 0.0f, -1.0f) : LetGo(in, 0.14, -1.0f);
            }));
            twice = twice && seen.touches == 3 && seen.least_gap >= 1;
        }
        const std::string fps = " (a game of " + std::to_string(static_cast<int>(rate)) + " frames a second)";
        Check(pulled, "the stick pulled back fast, held and let go shoots the catapult" + fps);
        Check(flicked, "so does the stick pulled back and let go at once" + fps);
        Check(half, "and the stick pulled back six tenths of the way" + fps);
        Check(swiped, "a flick forward is a swipe from near the player to the far edge, seen on its way" + fps);
        Check(twice, "flicks one after the other are swipes one after the other" + fps);
    }

    // A finger taken off by something else (a real one took over): the stick has to rest first.
    StickFinger finger;
    finger.Update(0.00, 0.0f, 0.0f);
    Check(finger.Update(0.01, 0.0f, 0.9f).down, "touching");
    finger.Reset();
    Check(!finger.Update(0.02, 0.0f, 0.9f).down && !finger.Update(0.03, 0.0f, 0.9f).down,
          "taken off: not touching again while the stick stays out");
    finger.Update(0.04, 0.0f, 0.0f);
    Check(finger.Update(0.05, 0.0f, 0.9f).down, "touching again after the stick has rested");

    std::printf("failed: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
