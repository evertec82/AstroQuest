// Checks what the Quest app makes of two hands holding a gamepad (quest-host/cpp/held_pad.h):
// where the gamepad is, which way it is turned, how far it is rolled and how far its front
// is tilted up, each with the right sign.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I quest-host/cpp tools/tests/held_pad_test.cpp
//       /Fe:build/tests/held_pad_test.exe
#include <cmath>
#include <cstdio>
#include <string>

#include "held_pad.h"

using namespace HeldPad;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

static bool Near(float a, float b, float within = 0.002f) {
    return std::abs(a - b) <= within;
}

static bool Near(const Vec& a, const Vec& b, float within = 0.002f) {
    return Near(a.x, b.x, within) && Near(a.y, b.y, within) && Near(a.z, b.z, within);
}

static constexpr float Pi = 3.14159265f;

static float Radians(float degrees) {
    return degrees * Pi / 180.0f;
}

/// Two hands around a gamepad whose middle is at `middle`, turned `yaw` to the left and with
/// its right side `roll` above level, the hands pointing `pitch` above level.
static Hands Hold(const Vec& middle, float yaw, float roll, float pitch, float apart = 0.2f) {
    const Vec right{std::cos(yaw) * std::cos(roll), std::sin(roll), -std::sin(yaw) * std::cos(roll)};
    const Vec level_front{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const Vec level_up = Cross(right, level_front);
    const Vec pointing{level_front.x * std::cos(pitch) + level_up.x * std::sin(pitch),
                       level_front.y * std::cos(pitch) + level_up.y * std::sin(pitch),
                       level_front.z * std::cos(pitch) + level_up.z * std::sin(pitch)};
    Hands hands;
    hands.left_palm = Sub(middle, Scale(right, apart * 0.5f));
    hands.right_palm = Sub(middle, Scale(right, -apart * 0.5f));
    hands.pointing_known = true;
    // Each hand points inwards as well, by a third of a right angle.
    const auto inwards = [&](float side) {
        const Vec v{pointing.x * std::cos(0.5f) + right.x * side * std::sin(0.5f),
                    pointing.y * std::cos(0.5f) + right.y * side * std::sin(0.5f),
                    pointing.z * std::cos(0.5f) + right.z * side * std::sin(0.5f)};
        return v;
    };
    hands.left_pointing = inwards(1.0f);
    hands.right_pointing = inwards(-1.0f);
    return hands;
}

int main() {
    const Vec x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f};

    // The rotation from three directions, and back.
    {
        const Quat q = FromAxes(x, y, z);
        Check(Near(q.x, 0.0f) && Near(q.y, 0.0f) && Near(q.z, 0.0f) && Near(std::abs(q.w), 1.0f),
              "right, up and back where they always are: no rotation");
        bool all = true;
        for (int i = 0; i < 200; ++i) {
            // Any attitude: turned, then tilted, then rolled.
            const float a = 0.37f * static_cast<float>(i), b = 0.11f * static_cast<float>(i) - 1.4f,
                        c = 0.23f * static_cast<float>(i);
            const Vec right{std::cos(a) * std::cos(c), std::sin(c), -std::sin(a) * std::cos(c)};
            const Vec level_front{-std::sin(a), 0.0f, -std::cos(a)};
            const Vec level_up = Cross(right, level_front);
            const Vec front{level_front.x * std::cos(b) + level_up.x * std::sin(b),
                            level_front.y * std::cos(b) + level_up.y * std::sin(b),
                            level_front.z * std::cos(b) + level_up.z * std::sin(b)};
            const Vec up = Cross(right, front);
            const Quat turned = FromAxes(right, up, Scale(front, -1.0f));
            all = all && Near(Rotate(turned, x), right, 0.001f) && Near(Rotate(turned, y), up, 0.001f) &&
                  Near(Rotate(turned, z), Scale(front, -1.0f), 0.001f);
        }
        Check(all, "any three directions make the rotation that takes the axes to them");
    }

    // Held level, straight ahead.
    {
        const auto pose = Locate(Hold({0.0f, -0.3f, -0.4f}, 0.0f, 0.0f, 0.0f), 0.0f, 0.5f);
        Check(pose.has_value() && pose->turned_known, "two hands a gamepad's width apart hold one");
        Check(Near(pose->position, {0.0f, -0.285f, -0.435f}),
              "its middle is a little ahead of and above the middle between the palms");
        Check(Near(pose->yaw, 0.0f) && Near(pose->pitch, 0.0f) && pose->pitch_measured,
              "level hands pointing ahead: neither turned nor tilted");
        Check(Near(Rotate(pose->orientation, {0.0f, 0.0f, -1.0f}), {0.0f, 0.0f, -1.0f}) &&
                  Near(Rotate(pose->orientation, y), y),
              "its front points straight ahead, its face up");
    }

    // Not a gamepad.
    Check(!Locate(Hold({}, 0.0f, 0.0f, 0.0f, 0.4f), 0.0f, 0.0f).has_value(),
          "hands further apart than a gamepad is wide hold none");
    Check(!Locate(Hold({}, 0.0f, 0.0f, 0.0f, 0.03f), 0.0f, 0.0f).has_value(),
          "nor do hands that touch");

    // Turned to the left.
    {
        const auto pose = Locate(Hold({0.1f, -0.3f, -0.4f}, Radians(30.0f), 0.0f, 0.0f), 0.0f, 0.0f);
        Check(pose && Near(pose->yaw, Radians(30.0f)), "turned to the left: a heading to the left");
        const Vec front = Rotate(pose->orientation, {0.0f, 0.0f, -1.0f});
        Check(Near(front, {-0.5f, 0.0f, -std::cos(Radians(30.0f))}), "its front points to the left");
        Check(Near(pose->position.x, 0.1f - 0.5f * 0.035f) &&
                  Near(pose->position.z, -0.4f - std::cos(Radians(30.0f)) * 0.035f),
              "and its middle is ahead of the palms the way it points");
    }
    {
        const auto pose = Locate(Hold({}, Radians(-45.0f), 0.0f, 0.0f), 0.0f, 0.0f);
        Check(pose && Near(pose->yaw, Radians(-45.0f)) &&
                  Rotate(pose->orientation, {0.0f, 0.0f, -1.0f}).x > 0.7f,
              "turned to the right: a heading to the right");
    }

    // Rolled: the right hand higher.
    {
        const auto pose = Locate(Hold({}, 0.0f, Radians(20.0f), 0.0f), 0.0f, 0.0f);
        const Vec right = Rotate(pose->orientation, x);
        const Vec front = Rotate(pose->orientation, {0.0f, 0.0f, -1.0f});
        Check(pose && Near(right.y, std::sin(Radians(20.0f))), "the right hand higher: its right side is up");
        Check(Near(front, {0.0f, 0.0f, -1.0f}) && Near(pose->pitch, 0.0f),
              "and its front still points level and ahead");
    }

    // Tilted: the hands point up.
    {
        const auto pose = Locate(Hold({}, 0.0f, 0.0f, Radians(30.0f)), 0.0f, 0.0f);
        const Vec front = Rotate(pose->orientation, {0.0f, 0.0f, -1.0f});
        Check(pose && pose->pitch_measured && Near(pose->pitch, Radians(30.0f)),
              "hands pointing up: tilted up by as much, however far they point inwards");
        Check(Near(front, {0.0f, 0.5f, -std::cos(Radians(30.0f))}), "its front points up");
        Check(Rotate(pose->orientation, y).z > 0.49f, "and its face leans towards the player");
    }
    {
        const auto pose = Locate(Hold({}, 0.0f, 0.0f, Radians(-40.0f)), 0.0f, 0.0f);
        Check(pose && Near(pose->pitch, Radians(-40.0f)) &&
                  Rotate(pose->orientation, {0.0f, 0.0f, -1.0f}).y < -0.6f,
              "hands pointing down: tilted down");
    }
    {
        const auto pose = Locate(Hold({}, 0.0f, 0.0f, Radians(30.0f)), Radians(-10.0f), 0.0f);
        Check(pose && Near(pose->pitch, Radians(30.0f)) &&
                  Near(Rotate(pose->orientation, {0.0f, 0.0f, -1.0f}).y, std::sin(Radians(20.0f))),
              "a gamepad taken to point lower than the hands does, by so much");
    }

    // All at once.
    {
        const float yaw = Radians(25.0f), roll = Radians(-15.0f), pitch = Radians(35.0f);
        const auto pose = Locate(Hold({0.05f, -0.2f, -0.5f}, yaw, roll, pitch), 0.0f, 0.0f);
        const Vec right = Rotate(pose->orientation, x);
        const Vec front = Rotate(pose->orientation, {0.0f, 0.0f, -1.0f});
        const Vec up = Rotate(pose->orientation, y);
        Check(pose && Near(pose->yaw, yaw) && Near(pose->pitch, pitch) && Near(right.y, std::sin(roll)),
              "turned, rolled and tilted at once: each is told apart");
        Check(Near(Dot(right, front), 0.0f) && Near(Dot(right, up), 0.0f) && Near(Dot(front, up), 0.0f) &&
                  Near(Length(right), 1.0f) && Near(Length(front), 1.0f),
              "and what comes out is a rotation");
    }

    // What the headset cannot tell.
    {
        Hands hands = Hold({}, 0.0f, 0.0f, Radians(50.0f));
        hands.pointing_known = false;
        const auto pose = Locate(hands, 0.0f, Radians(20.0f));
        Check(pose && pose->turned_known && !pose->pitch_measured && Near(pose->pitch, Radians(20.0f)) &&
                  Near(Rotate(pose->orientation, {0.0f, 0.0f, -1.0f}).y, std::sin(Radians(20.0f))),
              "where the hands point unknown: tilted the way it was taken to be");
    }
    {
        Hands hands;
        hands.left_palm = {0.0f, -0.4f, -0.4f};
        hands.right_palm = {0.02f, -0.2f, -0.4f};
        const auto pose = Locate(hands, 0.0f, 0.0f);
        Check(pose && !pose->turned_known && Near(pose->position, {0.01f, -0.285f, -0.4f}),
              "one hand above the other: somewhere, but turned no telling how");
    }

    // Following, a little behind.
    {
        const Quat quarter = FromAxes({0.0f, 0.0f, -1.0f}, y, x);
        const Quat half = Towards(Quat{}, quarter, 0.5f);
        const Vec front = Rotate(half, {0.0f, 0.0f, -1.0f});
        Check(Near(front, {-std::sqrt(0.5f), 0.0f, -std::sqrt(0.5f)}, 0.01f),
              "half the way to a quarter turn is an eighth of a turn");
        const Quat other{-quarter.x, -quarter.y, -quarter.z, -quarter.w};
        const Vec same = Rotate(Towards(Quat{}, other, 0.5f), {0.0f, 0.0f, -1.0f});
        Check(Near(same, front, 0.01f), "whichever of its two quaternions the attitude is given as");
        Check(Near(Rotate(Towards(Quat{}, quarter, 1.0f), x), {0.0f, 0.0f, -1.0f}, 0.01f),
              "all the way is there");
    }

    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
