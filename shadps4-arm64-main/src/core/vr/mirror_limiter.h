// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
namespace Core::Vr {
class MirrorLimiter {
public:
    using Clock = std::chrono::steady_clock;
    bool Due(Clock::time_point now, bool local_headset_frame, double fps) {
        // Desktop-only and exported/remote presentation must never be throttled here.
        if (!local_headset_frame) {
            next = {};
            return true;
        }
        if (fps <= 0.0)
            return false;
        if (now < next)
            return false;
        const auto period =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{1.0 / fps});
        if (next == Clock::time_point{} || now - next >= period)
            next = now + period;
        else
            next += period;
        return true;
    }

private:
    Clock::time_point next{};
};
} // namespace Core::Vr
