// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <optional>

/// Where a gamepad is and how it is held, told by the two hands around it. A gamepad cannot
/// be tracked, the hands that hold it can: the palms a controller's width apart give away
/// where it is, the line from one to the other is its sideways axis (which way it is turned,
/// and how far it is rolled to a side), and the way the hands point is how far its front is
/// tilted up or down.
///
/// Everything is in the headset's space: metres, +X right, +Y up, -Z forward.
namespace HeldPad {

struct Vec {
    float x{}, y{}, z{};
};

struct Quat {
    float x{}, y{}, z{}, w{1.0f};
};

struct Hands {
    Vec left_palm;
    Vec right_palm;
    /// The way each hand points, from the wrist to the knuckles (along the middle finger's
    /// bone in the palm), each of length one: if the headset can tell.
    bool pointing_known{};
    Vec left_pointing;
    Vec right_pointing;
};

struct Pose {
    /// The middle of the gamepad.
    Vec position;
    /// The hands are level enough for the gamepad's heading and attitude to be told.
    bool turned_known{};
    /// Heading: 0 when the gamepad points straight ahead (-Z), positive to the left.
    float yaw{};
    /// How far the hands point above level, in radians: measured, or `assumed_pitch` where
    /// the headset cannot tell.
    float pitch{};
    bool pitch_measured{};
    /// The complete attitude: -Z is the way the gamepad's front points, +Y out of its face.
    Quat orientation;
};

inline Vec Sub(const Vec& a, const Vec& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float Dot(const Vec& a, const Vec& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec Cross(const Vec& a, const Vec& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(const Vec& a) {
    return std::sqrt(Dot(a, a));
}

inline Vec Scale(const Vec& a, float by) {
    return {a.x * by, a.y * by, a.z * by};
}

inline Vec Rotate(const Quat& q, const Vec& v) {
    const Vec u{q.x, q.y, q.z};
    const Vec t = Scale(Cross(u, v), 2.0f);
    const Vec s = Cross(u, t);
    return {v.x + q.w * t.x + s.x, v.y + q.w * t.y + s.y, v.z + q.w * t.z + s.z};
}

/// The rotation that takes +X to `right`, +Y to `up` and +Z to `back`: three directions of
/// length one, each at a right angle to the others.
inline Quat FromAxes(const Vec& right, const Vec& up, const Vec& back) {
    const float trace = right.x + up.y + back.z;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(up.z - back.y) / s, (back.x - right.z) / s, (right.y - up.x) / s, 0.25f * s};
    } else if (right.x > up.y && right.x > back.z) {
        const float s = std::sqrt(1.0f + right.x - up.y - back.z) * 2.0f;
        q = {0.25f * s, (up.x + right.y) / s, (back.x + right.z) / s, (up.z - back.y) / s};
    } else if (up.y > back.z) {
        const float s = std::sqrt(1.0f + up.y - right.x - back.z) * 2.0f;
        q = {(up.x + right.y) / s, 0.25f * s, (back.y + up.z) / s, (back.x - right.z) / s};
    } else {
        const float s = std::sqrt(1.0f + back.z - right.x - up.y) * 2.0f;
        q = {(back.x + right.z) / s, (back.y + up.z) / s, 0.25f * s, (right.y - up.x) / s};
    }
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

/// Part of the way from one attitude to another: `blend` of it, 0 to 1.
inline Quat Towards(const Quat& from, const Quat& to, float blend) {
    // (The same attitude has two quaternions: the nearer one.)
    const float side = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0.0f
                           ? -1.0f
                           : 1.0f;
    Quat q{from.x + (to.x * side - from.x) * blend, from.y + (to.y * side - from.y) * blend,
           from.z + (to.z * side - from.z) * blend, from.w + (to.w * side - from.w) * blend};
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(length > 1e-6f)) {
        return to;
    }
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

/// Hands further apart than a gamepad is wide, or closer together, are not holding one:
/// nothing then. `tilt` is added to how far the hands point up, for where a gamepad's front
/// points by more or less than the hands around it do; `assumed_pitch` is how far up the
/// hands are taken to point while the headset cannot tell.
inline std::optional<Pose> Locate(const Hands& hands, float tilt, float assumed_pitch) {
    const Vec across = Sub(hands.right_palm, hands.left_palm);
    const float distance = Length(across);
    if (!(distance > 0.05f && distance < 0.32f)) {
        return std::nullopt;
    }
    Pose pose;
    pose.position = {(hands.left_palm.x + hands.right_palm.x) * 0.5f,
                     (hands.left_palm.y + hands.right_palm.y) * 0.5f,
                     (hands.left_palm.z + hands.right_palm.z) * 0.5f};
    pose.pitch = assumed_pitch;
    const float level = std::hypot(across.x, across.z);
    if (level > 0.6f * distance) {
        // The line from the left to the right palm is the gamepad's sideways axis; its
        // heading follows from that.
        pose.turned_known = true;
        pose.yaw = std::atan2(-across.z, across.x);
        // The palms close around the grips, which sit behind and below the middle of the
        // gamepad.
        pose.position.x += -std::sin(pose.yaw) * 0.035f;
        pose.position.z += -std::cos(pose.yaw) * 0.035f;

        const Vec right = Scale(across, 1.0f / distance);
        // Level and at a right angle to that line: where the front points if it is neither
        // tilted up nor down. What is up for a gamepad held like that goes with it.
        const Vec level_front_long = Cross({0.0f, 1.0f, 0.0f}, right);
        const Vec level_front = Scale(level_front_long, 1.0f / Length(level_front_long));
        const Vec level_up = Cross(right, level_front);
        if (hands.pointing_known) {
            // Both hands point inwards as much as forwards: what they agree on, without
            // what goes along the line between them, is the way the gamepad points.
            Vec pointing{hands.left_pointing.x + hands.right_pointing.x,
                         hands.left_pointing.y + hands.right_pointing.y,
                         hands.left_pointing.z + hands.right_pointing.z};
            pointing = Sub(pointing, Scale(right, Dot(pointing, right)));
            if (Length(pointing) > 0.5f) {
                pose.pitch = std::atan2(Dot(pointing, level_up), Dot(pointing, level_front));
                pose.pitch_measured = true;
            }
        }
        const float pitch = pose.pitch + tilt;
        const Vec front{level_front.x * std::cos(pitch) + level_up.x * std::sin(pitch),
                        level_front.y * std::cos(pitch) + level_up.y * std::sin(pitch),
                        level_front.z * std::cos(pitch) + level_up.z * std::sin(pitch)};
        pose.orientation = FromAxes(right, Cross(right, front), Scale(front, -1.0f));
    }
    pose.position.y += 0.015f;
    return pose;
}

} // namespace HeldPad
