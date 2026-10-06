// What ASTRO BOT Rescue Mission makes of a finger on the touchpad, as far as its code was
// read (version 1.00: the pad is read at 0xc04cc3, the level-end catapult is 0x436fc0), for
// the tests of fingers that are moved by sticks and buttons.
//
// The game reads the pad once for every frame it draws and keeps the finger's place as
// x = raw x / 1920 and y = 1 - raw y / 950 (the far edge of the pad is up). The emulator
// hands it raw y = place * 941.
#pragma once

#include <cmath>
#include <functional>

namespace GameModel {

struct Finger {
    bool down{};
    float x{0.5f};
    float y{0.5f};
    bool pressed{};
};

struct Seen {
    /// Frames the game saw a finger on the pad in, and touches it told apart.
    int frames{};
    int touches{};
    /// Where it first and last saw the finger of the last touch, in its own terms.
    float first_x{}, first_y{}, last_x{}, last_y{};
    /// The furthest the finger got from one frame to the next.
    float largest_step{};
    /// Frames in which the pad was pressed, and how often it went down.
    int pressed_frames{};
    int presses{};
    /// Catapults shot.
    int shots{};
    /// Frames without a finger between two touches, at the least (0: never two touches).
    int least_gap{};
};

/// Has `at(seconds)` say where the finger is, a thousand times a second, and the game look
/// `rate` times a second, the first time `phase` of a frame after the start.
inline Seen Watch(double rate, double phase, double length,
                  const std::function<Finger(double)>& at) {
    static constexpr float MinTouchDuration = 0.25f;
    static constexpr float MinTouchGestureAmplitude = 0.4f;
    Seen seen;
    const double frame = 1.0 / rate;
    double next_look = phase * frame;
    bool was_down = false;
    bool was_pressed = false;
    float duration = 0.0f;
    float start_y = 0.0f;
    float before_x = 0.0f, before_y = 0.0f;
    int gap = 0;
    bool had_touch = false;
    Finger finger;
    for (double time = 0.0; time < length; time += 0.001) {
        finger = at(time);
        if (time + 1e-9 < next_look) {
            continue;
        }
        next_look += frame;
        const float x = std::floor(finger.x * 1920.0f) / 1920.0f;
        const float y = 1.0f - std::floor(finger.y * 941.0f) / 950.0f;
        if (finger.down) {
            ++seen.frames;
            if (!was_down) {
                ++seen.touches;
                if (had_touch) {
                    seen.least_gap = seen.least_gap == 0 ? gap : (gap < seen.least_gap ? gap : seen.least_gap);
                }
                had_touch = true;
                seen.first_x = x;
                seen.first_y = y;
                seen.largest_step = 0.0f;
                start_y = y;
                duration = 0.0f;
            } else {
                const float step = std::hypot(x - before_x, y - before_y);
                seen.largest_step = step > seen.largest_step ? step : seen.largest_step;
            }
            duration += static_cast<float>(frame);
            seen.last_x = x;
            seen.last_y = y;
            before_x = x;
            before_y = y;
            gap = 0;
        } else {
            if (was_down && duration > MinTouchDuration &&
                start_y - seen.last_y > MinTouchGestureAmplitude) {
                ++seen.shots;
            }
            ++gap;
        }
        if (finger.pressed) {
            ++seen.pressed_frames;
            if (!was_pressed) {
                ++seen.presses;
            }
        }
        was_down = finger.down;
        was_pressed = finger.pressed;
    }
    return seen;
}

} // namespace GameModel
