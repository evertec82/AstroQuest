// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/// What the emulated DualShock 4 looks like right now. Buttons use the PS4 pad bit layout.
struct PadState {
    uint32_t buttons{};
    uint8_t left_x{128}, left_y{128}, right_x{128}, right_y{128};
    uint8_t left_trigger{}, right_trigger{};
    bool touch_down{};
    uint16_t touch_x{}, touch_y{};
    bool has_motion{};
    float gyro[3]{};
    float accel[3]{0.0f, 9.81f, 0.0f};

    bool operator==(const PadState&) const = default;
};
