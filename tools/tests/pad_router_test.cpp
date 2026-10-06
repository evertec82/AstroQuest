// Checks what the Quest app makes of the controllers a player has (quest-host/cpp/pad_router.h):
// which of a gamepad and the headset's own controllers the game is played with, what the
// headset's controllers' buttons and sticks become, the right stick as a finger on the
// touchpad, the buttons that do the touchpad's gestures, and what buttons mean about the view
// besides.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I quest-host/cpp /I shadps4-arm64-main/src/input
//       tools/tests/pad_router_test.cpp /Fe:build/tests/pad_router_test.exe
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pad_router.h"

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

/// A router with what it sends to the game and what it says kept for looking at.
struct Bench {
    PadRouter router;
    PadRouter::Output out;
    int sent{};
    std::vector<std::string> notices;
    double time{};

    Bench() {
        router.SetSink([this](const PadRouter::Output& output) {
            out = output;
            ++sent;
        });
        router.SetNotice([this](const char* text) { notices.emplace_back(text); });
    }

    /// Time passes, as it does once for every picture the headset shows.
    void Pass(double seconds, const TouchState* touch = nullptr) {
        const double until = time + seconds;
        while (time < until - 1e-9) {
            time += 1.0 / 90.0;
            if (touch != nullptr) {
                router.SetTouch(*touch, time);
            } else {
                router.Tick(time);
            }
        }
    }

    bool Said(const std::string& part) const {
        for (const std::string& notice : notices) {
            if (notice.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    PadRouter::Source Source() {
        return router.Look().source;
    }
};

static TouchState Held() {
    TouchState touch;
    touch.present = true;
    return touch;
}

int main() {
    using Source = PadRouter::Source;
    namespace Button = PadButton;

    {
        Bench bench;
        bench.router.SetPlaying(true, 0.0);
        bench.Pass(0.1);
        Check(bench.Source() == Source::None, "nothing to play with: no controller");
        const PadState& pad = bench.out.pad;
        Check(bench.sent == 1 && pad.buttons == 0 && pad.left_x == 128 && pad.left_y == 128 &&
                  pad.right_x == 128 && pad.right_y == 128 && pad.left_trigger == 0 &&
                  pad.right_trigger == 0 && !pad.touch_down && !pad.has_motion && !bench.out.blowing,
              "and the game is given a resting gamepad");
    }

    // --- a gamepad on its own ---------------------------------------------------------------
    {
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        bench.router.SetPlaying(true, 0.0);
        Check(bench.Source() == Source::Gamepad, "a gamepad alone is what the game is played with");

        GamepadState pad;
        pad.buttons = Button::Square | Button::L1;
        pad.left_x = 1.0f;
        pad.left_y = -1.0f;
        pad.left_trigger = 0.5f;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.out.pad.buttons == (Button::Square | Button::L1), "its buttons are the game's");
        Check(bench.out.pad.left_x == 255 && bench.out.pad.left_y == 1 && bench.out.pad.right_x == 128 &&
                  bench.out.pad.right_y == 128,
              "its sticks are the game's (right and towards the player are the high numbers)");
        Check(bench.out.pad.left_trigger == 128 && bench.out.pad.right_trigger == 0, "and its triggers");
        Check(!bench.out.pad.has_motion, "a gamepad that says nothing of how it is held has no motion sensors");
        Check(bench.router.TakeRecenter() == 0, "square and L1 mean nothing about the view");

        const float gyro[3]{0.1f, 0.2f, 0.3f};
        const float accel[3]{1.0f, 9.0f, 2.0f};
        bench.router.SetGamepadMotion(gyro, accel, bench.time);
        Check(bench.out.pad.has_motion && bench.out.pad.gyro[1] == 0.2f && bench.out.pad.accel[2] == 2.0f,
              "its motion sensors are passed on");
        Check(bench.router.Look().gamepad_motion && bench.router.Look().gamepad_accel[1] == 9.0f,
              "and the host knows it has them");

        // The first press of X.
        pad = {};
        pad.buttons = Button::Cross;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.router.TakeRecenter() == PadRouter::RecenterSeat, "the first press of X takes the seat");
        Check(bench.Said("first press of X"), "and says so");
        bench.router.SetGamepad({}, bench.time);
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.router.TakeRecenter() == 0, "the second does not");

        // OPTIONS.
        pad = {};
        pad.buttons = Button::Options;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.5);
        Check(bench.out.pad.buttons == Button::Options, "the game sees OPTIONS");
        Check(bench.router.TakeRecenter() == 0, "OPTIONS held for half a second resets nothing");
        bench.Pass(0.6);
        Check(bench.router.TakeRecenter() == (PadRouter::RecenterPad | PadRouter::RecenterSeat),
              "held for a second it resets the view");
        bench.Pass(2.0);
        Check(bench.router.TakeRecenter() == 0, "once");
        bench.router.SetGamepad({}, bench.time);
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.3);
        bench.router.SetGamepad({}, bench.time);
        bench.Pass(1.0);
        Check(bench.router.TakeRecenter() == 0, "OPTIONS pressed briefly resets nothing");

        // A gamepad that is gone.
        pad = {};
        pad.buttons = Button::Triangle;
        bench.router.SetGamepad(pad, bench.time);
        bench.router.SetGamepadConnected(false, false, bench.time);
        Check(bench.out.pad.buttons == 0 && !bench.out.pad.has_motion && bench.Source() == Source::None,
              "nothing is held down on a gamepad that is gone");
    }

    // --- a gamepad's touchpad, or what stands in for it ---------------------------------------
    {
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        bench.router.SetPlaying(true, 0.0);
        GamepadState pad;
        pad.touch_down = true;
        pad.touch_x = 0.25f;
        pad.touch_y = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_x == 480 && bench.out.pad.touch_y == 1079,
              "a finger on the gamepad's own touchpad is where it is");
        pad.right_y = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        Check(bench.out.pad.touch_x == 480 && bench.out.pad.touch_y == 1079 && bench.out.pad.right_y == 255,
              "the right stick moves no finger while a real one is on the pad");
        pad.touch_down = false;
        bench.router.SetGamepad(pad, bench.time);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_x == 480,
              "the real finger lifts from where it was, and the stick's is not down the moment it does");
        pad.right_y = 0.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);

        // The catapult at the end of a level: pull the stick towards you and let go.
        pad.right_y = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.05);
        Check(bench.out.pad.touch_down, "the right stick pulled puts a finger on the pad");
        bench.Pass(0.35);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 1079 && bench.out.pad.touch_x == 960,
              "at the pad's near edge");
        Check(bench.out.pad.right_y == 255, "(a gamepad's right stick stays a stick as well)");
        pad.right_y = 0.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_y == 1079,
              "let go, the finger lifts there, not back in the middle: the catapult shoots");

        // A button for the press.
        pad = {};
        pad.buttons = Button::TouchPad;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_x == 960 && bench.out.pad.touch_y == 540,
              "a button that presses the touchpad touches it in the middle");
        bench.router.SetGamepad({}, bench.time);
        Check(!bench.out.pad.touch_down, "and lets go of it");

        bench.router.SetStickTouchpad(false);
        pad = {};
        pad.right_x = -1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.3);
        Check(!bench.out.pad.touch_down && bench.out.pad.right_x == 1,
              "switched off, the right stick is only a stick");
    }

    // --- the headset's own controllers on their own -------------------------------------------
    {
        Bench bench;
        bench.router.SetPlaying(true, 0.0);
        TouchState touch = Held();
        bench.Pass(0.1, &touch);
        Check(bench.Source() == Source::Touch, "the headset's controllers alone play the game once they are picked up");
        Check(bench.Said("headset's own controllers"), "and the log says so");

        const auto buttons = [&](const TouchState& state) {
            bench.Pass(0.05, &state);
            return bench.out.pad.buttons;
        };
        touch = Held();
        touch.a = true;
        Check(buttons(touch) == Button::Cross, "A is cross");
        Check(bench.router.TakeRecenter() == PadRouter::RecenterSeat, "and its first press takes the seat");
        touch = Held();
        touch.b = true;
        Check(buttons(touch) == Button::Square, "B is square");
        touch = Held();
        touch.x = true;
        Check(buttons(touch) == Button::Circle && !bench.out.blowing, "X is circle");
        touch = Held();
        touch.y = true;
        Check(buttons(touch) == Button::Triangle && !bench.out.blowing, "Y is triangle");
        touch.x = true;
        Check(buttons(touch) == 0 && bench.out.blowing, "X and Y together are neither: they blow");
        touch = Held();
        Check(buttons(touch) == 0 && !bench.out.blowing, "for as long as they are held");
        touch.left_grip = 0.9f;
        touch.right_grip = 0.4f;
        Check(buttons(touch) == Button::L1, "a grip squeezed is L1, one half squeezed is nothing");
        touch = Held();
        touch.right_grip = 1.0f;
        Check(buttons(touch) == Button::R1, "the other grip is R1");
        touch = Held();
        touch.left_trigger = 0.05f;
        touch.right_trigger = 0.5f;
        Check(buttons(touch) == Button::R2 && bench.out.pad.left_trigger == 0 && bench.out.pad.right_trigger == 128,
              "a trigger pulled is R2 by as much as it is pulled; one not quite at rest is not pulled");
        touch = Held();
        touch.left_trigger = 1.0f;
        Check(buttons(touch) == Button::L2 && bench.out.pad.left_trigger == 255, "the other trigger is L2");
        touch = Held();
        touch.menu = true;
        Check(buttons(touch) == Button::Options, "the left controller's menu button is OPTIONS");
        bench.Pass(1.1, &touch);
        Check(bench.router.TakeRecenter() == (PadRouter::RecenterPad | PadRouter::RecenterSeat),
              "held for a second it resets the view");
        touch = Held();
        touch.left_stick_in = true;
        Check(buttons(touch) == Button::L3 && !bench.out.pad.touch_down, "the left stick pressed in is L3");
        touch = Held();
        touch.right_stick_in = true;
        Check(buttons(touch) == Button::TouchPad && bench.out.pad.touch_down && bench.out.pad.touch_x == 960 &&
                  bench.out.pad.touch_y == 540,
              "the right stick pressed in presses the touchpad, in its middle");
        touch.left_stick_in = true;
        Check(buttons(touch) == 0 && !bench.out.pad.touch_down, "both sticks pressed in press nothing");
        Check(bench.router.TakeRecenter() == (PadRouter::RecenterPad | PadRouter::RecenterSeat),
              "they reset the view");
        bench.Pass(0.5, &touch);
        Check(bench.router.TakeRecenter() == 0, "once");

        // Sticks: the runtime has away from the player as up, a gamepad the other way round.
        touch = Held();
        touch.left_x = -1.0f;
        touch.left_y = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.left_x == 1 && bench.out.pad.left_y == 1,
              "the left stick pushed away and to the left is the gamepad's, pushed away and to the left");

        // The right stick is the finger on the touchpad, and nothing else.
        touch = Held();
        bench.Pass(0.2, &touch);
        touch.right_y = -1.0f;
        bench.Pass(0.4, &touch);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 1079 && bench.out.pad.touch_x == 960,
              "the right stick pulled towards the player drags a finger to the pad's near edge");
        Check(bench.out.pad.right_x == 128 && bench.out.pad.right_y == 128,
              "while the game's right stick stays where it is");
        touch.right_y = 0.0f;
        bench.Pass(0.2, &touch);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_y == 1079, "let go, the finger lifts there");
        Check(!bench.out.pad.has_motion, "the headset's controllers have no motion sensors to pass on: they are tracked");

        // The game loses the player's attention (the headset's menu): nothing stays held.
        touch = Held();
        touch.a = true;
        touch.left_x = 1.0f;
        bench.Pass(0.05, &touch);
        TouchState away = touch;
        away.present = false;
        bench.Pass(0.05, &away);
        Check(bench.out.pad.buttons == 0 && bench.out.pad.left_x == 128 && bench.Source() == Source::Touch,
              "what was held is let go when the controllers fall silent, and they stay the controller");
    }

    // --- both at hand ---------------------------------------------------------------------------
    {
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        bench.router.SetPlaying(true, 0.0);
        const float gyro[3]{};
        const float accel[3]{0.0f, 9.81f, 0.0f};
        bench.router.SetGamepadMotion(gyro, accel, 0.0);
        TouchState touch = Held();
        bench.Pass(0.1, &touch);
        Check(bench.Source() == Source::Gamepad, "with both at hand and neither used yet: the gamepad");

        // What a hand can do without a controller in it never takes over from the gamepad.
        touch.left_trigger = 1.0f;
        touch.right_trigger = 1.0f;
        touch.left_grip = 1.0f;
        touch.right_grip = 1.0f;
        touch.menu = true;
        bench.Pass(0.2, &touch);
        Check(bench.Source() == Source::Gamepad && bench.out.pad.buttons == 0,
              "triggers, grips and the menu button of the headset's controllers do not take over");
        touch = Held();
        bench.Pass(0.1, &touch);
        touch.a = true;
        bench.Pass(0.05, &touch);
        Check(bench.Source() == Source::Touch && bench.out.pad.buttons == Button::Cross,
              "a button under a thumb does, and is the game's at once");
        Check(!bench.out.pad.has_motion, "the gamepad's motion sensors no longer count then");

        touch = Held();
        bench.Pass(0.1, &touch);
        bench.router.SetGamepadMotion(gyro, accel, bench.time);
        Check(bench.Source() == Source::Touch && !bench.out.pad.has_motion,
              "a gamepad that only lies there, sensors running, takes nothing back");
        GamepadState pad;
        pad.left_x = 0.3f;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.Source() == Source::Touch, "nor does a stick that does not rest at its centre");
        pad.left_x = 0.9f;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.Source() == Source::Gamepad && bench.out.pad.left_x == 242 && bench.out.pad.has_motion,
              "a stick pushed does");
        bench.Pass(0.1, &touch);
        touch.right_x = 0.9f;
        bench.Pass(0.05, &touch);
        Check(bench.Source() == Source::Touch, "and a stick of the headset's controllers takes it back");
        pad = {};
        pad.buttons = Button::Circle;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.Source() == Source::Gamepad && bench.out.pad.buttons == Button::Circle,
              "a button of the gamepad takes over again");
        pad = {};
        pad.right_trigger = 0.8f;
        bench.router.SetGamepad({}, bench.time);
        touch = Held();
        touch.b = true;
        bench.Pass(0.05, &touch);
        Check(bench.Source() == Source::Touch, "(back to the headset's)");
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.Source() == Source::Gamepad, "so does a trigger of the gamepad");

        // The gamepad's battery gives out.
        bench.router.SetGamepadConnected(false, false, bench.time);
        touch = Held();
        bench.Pass(0.05, &touch);
        Check(bench.Source() == Source::Touch, "the gamepad gone, the headset's controllers play on");
        // It comes back, and the headset's controllers are put down.
        bench.router.SetGamepadConnected(true, true, bench.time);
        Check(bench.Source() == Source::Touch, "a gamepad that comes back does not take over by being there");
        TouchState away;
        bench.Pass(0.2, &away);
        Check(bench.Source() == Source::Touch, "nor because the headset's controllers were put down");
        pad = {};
        pad.buttons = Button::Cross;
        bench.router.SetGamepad(pad, bench.time);
        Check(bench.Source() == Source::Gamepad, "its first button does");
        const int switches = static_cast<int>(bench.notices.size());
        bench.Pass(1.0, &away);
        Check(static_cast<int>(bench.notices.size()) == switches, "and nothing changes while nothing is pressed");
    }

    // --- the host's own menu before the game ---------------------------------------------------
    {
        Bench bench;
        TouchState touch = Held();
        bench.Pass(0.1, &touch);
        Check(bench.router.TouchMenu() == 0, "the menu: nothing held, nothing said");
        touch.left_x = -0.9f;
        bench.Pass(0.05, &touch);
        Check(bench.router.TouchMenu() == PadRouter::MenuLeft, "the left stick to the left is left");
        touch = Held();
        touch.right_x = 0.9f;
        bench.Pass(0.05, &touch);
        Check(bench.router.TouchMenu() == PadRouter::MenuRight, "the right stick to the right is right");
        touch = Held();
        touch.right_trigger = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.router.TouchMenu() == PadRouter::MenuConfirm, "a trigger confirms");
        Check(bench.out.pad.buttons == 0 && bench.sent <= 1, "none of which reaches the game");
        Check(bench.router.TakeRecenter() == 0, "or resets anything");

        // The game starts with the trigger that confirmed still pulled, and A pressed besides.
        touch.a = true;
        bench.Pass(0.05, &touch);
        Check(bench.router.TouchMenu() == PadRouter::MenuConfirm, "A confirms as well");
        bench.router.SetPlaying(true, bench.time);
        bench.Pass(0.2, &touch);
        Check(bench.out.pad.buttons == 0 && bench.out.pad.right_trigger == 0,
              "what confirmed the menu is not pressed in the game");
        Check(bench.router.TakeRecenter() == 0, "and takes no seat");
        touch.right_trigger = 0.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.buttons == 0, "A stays held back while it is held");
        touch.a = false;
        bench.Pass(0.05, &touch);
        touch.a = true;
        touch.right_trigger = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.buttons == (Button::Cross | Button::R2 | Button::TouchPad) &&
                  bench.out.pad.right_trigger == 255 && bench.out.pad.touch_down,
              "let go and pressed again, both are the game's (and the trigger presses the touchpad)");
        Check(bench.router.TakeRecenter() == PadRouter::RecenterSeat, "and that press of A takes the seat");

        TouchState away;
        bench.Pass(0.05, &away);
        Check(bench.router.TouchMenu() == 0, "controllers that were put down say nothing to a menu");
    }
    {
        // The player chooses in the menu with the headset's controllers while a gamepad is on.
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        TouchState touch = Held();
        bench.Pass(0.1, &touch);
        Check(bench.Source() == Source::Gamepad, "a gamepad that is on is the controller before the menu");
        bench.router.Use(Source::Touch, bench.time);
        Check(bench.Source() == Source::Touch, "choosing in the menu with the headset's controllers makes them it");
        bench.router.Use(Source::Gamepad, bench.time);
        Check(bench.Source() == Source::Gamepad, "and choosing with the gamepad, the gamepad");
        TouchState away;
        bench.Pass(0.1, &away);
        bench.router.Use(Source::Touch, bench.time);
        Check(bench.Source() == Source::Gamepad, "controllers nobody holds are not chosen with");
    }

    // --- the touchpad's gestures, on buttons -----------------------------------------------------
    {
        Bench bench;
        bench.router.SetPlaying(true, 0.0);
        TouchState touch = Held();
        bench.Pass(0.2, &touch);
        Check(bench.router.Look().gesture_buttons, "the headset's controllers have no touchpad: buttons do its gestures");

        // The trigger of the hand that holds the controller in the game presses the pad.
        touch.right_trigger = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.buttons == (Button::R2 | Button::TouchPad) && bench.out.pad.touch_down &&
                  bench.out.pad.touch_x == 960 && bench.out.pad.touch_y == 540,
              "the right trigger presses the touchpad, in its middle (and is still R2)");
        bench.Pass(2.0, &touch);
        Check((bench.out.pad.buttons & Button::TouchPad) != 0 && bench.out.pad.touch_down,
              "for as long as it is pulled");
        touch = Held();
        bench.Pass(0.1, &touch);
        Check(bench.out.pad.buttons == 0 && !bench.out.pad.touch_down, "and lets go of it");

        // Its grip swipes forward, once.
        touch.right_grip = 1.0f;
        bench.Pass(0.03, &touch);
        Check(bench.out.pad.buttons == Button::R1 && bench.out.pad.touch_down && bench.out.pad.touch_y == 863,
              "the right grip puts a finger near the player's edge of the pad (and is still R1)");
        int least = 1079;
        for (int i = 0; i < 60; ++i) {
            bench.Pass(0.011, &touch);
            if (bench.out.pad.touch_down) {
                least = std::min<int>(least, bench.out.pad.touch_y);
            }
        }
        Check(least == 129 && !bench.out.pad.touch_down && bench.out.pad.buttons == Button::R1,
              "takes it to the far edge and off, once, however long the grip is held");
        touch = Held();
        bench.Pass(0.1, &touch);

        // The other hand's trigger pulls back, and lets go when it is let go.
        touch.left_trigger = 1.0f;
        bench.Pass(0.03, &touch);
        Check(bench.out.pad.buttons == Button::L2 && bench.out.pad.touch_down && bench.out.pad.touch_y == 216,
              "the left trigger puts a finger near the far edge (and is still L2)");
        bench.Pass(1.0, &touch);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 1047, "pulls it back to the near edge and holds it there");
        touch = Held();
        bench.Pass(0.1, &touch);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_y == 1047, "and lets go there: the catapult shoots");

        // A gesture comes before the stick's finger.
        touch.right_y = -1.0f;
        bench.Pass(0.5, &touch);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 1079, "(the stick has pulled a finger back)");
        touch.right_trigger = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 540 && (bench.out.pad.buttons & Button::TouchPad) != 0,
              "a button's gesture takes the pad from the stick's finger");
        touch = Held();
        bench.Pass(0.3, &touch);

        // The other way round for a player whose left controller is the one in the game.
        bench.router.SetPadHand(0);
        touch.left_trigger = 1.0f;
        bench.Pass(0.05, &touch);
        Check(bench.out.pad.buttons == (Button::L2 | Button::TouchPad), "with the left controller as the one in the game, its trigger presses");
        touch = Held();
        bench.Pass(0.3, &touch);
        touch.right_trigger = 1.0f;
        bench.Pass(0.5, &touch);
        Check(bench.out.pad.buttons == Button::R2 && bench.out.pad.touch_down && bench.out.pad.touch_y == 1047,
              "and the right trigger pulls");
        touch = Held();
        touch.left_grip = 1.0f;
        bench.Pass(0.5, &touch);
        touch = Held();
        bench.Pass(0.1, &touch);
        touch.left_grip = 1.0f;
        bench.Pass(0.03, &touch);
        Check(bench.out.pad.touch_down && bench.out.pad.touch_y == 863, "and its grip swipes");
    }
    {
        // A gamepad without a touchpad.
        Bench bench;
        bench.router.SetGamepadConnected(true, false, 0.0);
        bench.router.SetPlaying(true, 0.0);
        bench.Pass(0.2);
        Check(bench.router.Look().gesture_buttons, "a gamepad without a touchpad: buttons do its gestures");
        GamepadState pad;
        pad.buttons = Button::R2;
        pad.right_trigger = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.3);
        Check(bench.out.pad.buttons == (Button::R2 | Button::TouchPad) && bench.out.pad.touch_down &&
                  bench.out.pad.right_trigger == 255,
              "R2 presses the touchpad");
        bench.router.SetGamepad({}, bench.time);
        bench.Pass(0.2);
        Check(bench.out.pad.buttons == 0 && !bench.out.pad.touch_down, "and lets go of it");
        pad = {};
        pad.buttons = Button::R1;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.03);
        Check(bench.out.pad.buttons == Button::R1 && bench.out.pad.touch_down && bench.out.pad.touch_y == 863,
              "R1 swipes forward");
        bench.router.SetGamepad({}, bench.time);
        bench.Pass(0.6);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_y == 129, "to the far edge and off, though R1 was only tapped");
        pad = {};
        pad.buttons = Button::L2;
        pad.left_trigger = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.02);
        bench.router.SetGamepad({}, bench.time);
        bench.Pass(0.2);
        Check(bench.out.pad.touch_down, "L2 tapped pulls a finger back, all of the way");
        bench.Pass(0.4);
        Check(!bench.out.pad.touch_down && bench.out.pad.touch_y == 1047, "and lets go at the near edge");

        // OPTIONS is counted, for a host that shows the controls with the game's pause.
        const uint32_t before = bench.router.Look().options_presses;
        pad = {};
        pad.buttons = Button::Options;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        bench.router.SetGamepad({}, bench.time);
        bench.Pass(0.1);
        Check(bench.router.Look().options_presses == before + 1, "a press of OPTIONS is counted once");
    }
    {
        // A gamepad with a touchpad of its own keeps its buttons to itself.
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        bench.router.SetPlaying(true, 0.0);
        Check(!bench.router.Look().gesture_buttons, "a gamepad with a touchpad needs no buttons for it");
        GamepadState pad;
        pad.buttons = Button::R2 | Button::R1 | Button::L2;
        pad.right_trigger = 1.0f;
        pad.left_trigger = 1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.5);
        Check(bench.out.pad.buttons == (Button::R2 | Button::R1 | Button::L2) && !bench.out.pad.touch_down,
              "its triggers and shoulder buttons are only that");
    }
    {
        // The view turns by steps: the grip that has no other meaning held, the right stick
        // flicked to a side.
        Bench bench;
        bench.router.SetPlaying(true, 0.0);
        TouchState touch = Held();
        bench.Pass(0.2, &touch);
        Check(bench.router.TakeTurn() == 0, "nothing turns the view by itself");
        touch.right_x = 1.0f;
        bench.Pass(0.3, &touch);
        Check(bench.router.TakeTurn() == 0 && bench.out.pad.touch_down,
              "the right stick alone is a finger on the touchpad, not a turn");
        touch.right_x = 0.0f;
        bench.Pass(0.6, &touch);
        touch.left_grip = 1.0f;
        bench.Pass(0.2, &touch);
        Check(bench.router.TakeTurn() == 0, "the grip alone turns nothing");
        touch.right_x = 1.0f;
        bench.Pass(0.2, &touch);
        Check(bench.router.TakeTurn() == 1, "the left grip held and the stick flicked right: a step to the right");
        Check(!bench.out.pad.touch_down && bench.out.pad.right_x == 128,
              "and the stick is no finger and no stick to the game meanwhile");
        Check((bench.out.pad.buttons & Button::L1) != 0, "the grip is still L1 to the game");
        bench.Pass(0.5, &touch);
        Check(bench.router.TakeTurn() == 0, "a stick held to the side turns once");
        touch.right_x = 0.0f;
        bench.Pass(0.1, &touch);
        touch.right_x = -1.0f;
        bench.Pass(0.1, &touch);
        touch.right_x = 0.0f;
        bench.Pass(0.1, &touch);
        touch.right_x = -1.0f;
        bench.Pass(0.1, &touch);
        Check(bench.router.TakeTurn() == -2, "two flicks to the left: two steps to the left");
        Check(bench.Said("the view turns a step to the left"), "and it says so");
        // The stick already at a side when the grip comes down turns nothing.
        touch.left_grip = 0.0f;
        touch.right_x = 0.0f;
        bench.Pass(0.6, &touch);
        touch.right_x = 1.0f;
        bench.Pass(0.1, &touch);
        touch.left_grip = 1.0f;
        bench.Pass(0.3, &touch);
        Check(bench.router.TakeTurn() == 0, "a stick that was at a side before the grip was held turns nothing");
        // With the controller of the game in the left hand, it is the right grip.
        Bench left;
        left.router.SetPadHand(0);
        left.router.SetPlaying(true, 0.0);
        TouchState other = Held();
        left.Pass(0.2, &other);
        other.left_grip = 1.0f;
        left.Pass(0.2, &other);
        other.right_x = 1.0f;
        left.Pass(0.3, &other);
        Check(left.router.TakeTurn() == 0, "pad hand left: the left grip swipes, it does not turn");
        other = Held();
        left.Pass(1.0, &other);
        other.right_grip = 1.0f;
        left.Pass(0.2, &other);
        other.right_x = -1.0f;
        left.Pass(0.2, &other);
        Check(left.router.TakeTurn() == -1, "pad hand left: the right grip held and a flick turn");
    }
    {
        // On a gamepad it is L1, whatever else the gamepad has.
        Bench bench;
        bench.router.SetGamepadConnected(true, true, 0.0);
        bench.router.SetPlaying(true, 0.0);
        GamepadState pad;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        pad.buttons = Button::L1;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        pad.right_x = -1.0f;
        bench.router.SetGamepad(pad, bench.time);
        bench.Pass(0.2);
        Check(bench.router.TakeTurn() == -1, "a gamepad: L1 held and the right stick flicked left turn the view");
        Check(bench.out.pad.right_x == 128 && !bench.out.pad.touch_down,
              "and the stick says nothing else meanwhile");
    }

    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
