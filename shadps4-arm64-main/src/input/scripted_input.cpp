// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/vr_runtime.h"
#include "input/controller.h"
#include "input/scripted_input.h"
#include "input/pad_gestures.h"
#include "input/stick_finger.h"

namespace Input {

namespace {

using Buttons = Libraries::Pad::OrbisPadButtonDataOffset;

std::atomic<float> microphone_level{0.0f};
std::atomic<bool> blowing{false};

struct Step {
    double start;
    double end;
    Buttons buttons{Buttons::None};
    // Left x, left y, right x, right y; negative means "leave centred".
    std::array<int, 4> sticks{-1, -1, -1, -1};
    std::optional<Core::Vr::Pose> head;
    std::optional<Core::Vr::Pose> pad;
    std::optional<Core::Vr::Quat> pad_rotation;
    // Where a host sees the hands hold the controller: x, y, z and heading in degrees.
    std::optional<std::array<float, 4>> hands;
    bool hands_lost{};
    // A finger on the touchpad, 0..1 across and down.
    std::optional<std::array<float, 2>> touch;
    // The right stick as it stands in for that finger (Input::StickFinger), -1..1 to the right
    // and towards the player.
    std::optional<std::array<float, 2>> finger;
    // The buttons that do the touchpad's gestures on a controller without one
    // (Input::PadGestures), held while the line lasts.
    PadGestures::Controls gestures;
    // The place a controller that nothing locates is held to be at: moved by so many metres
    // (right, up, towards the player), or switched between the standard one and the player's.
    std::optional<Core::Vr::Vec3> place_move;
    bool place_switch{};
    // What the microphone hears: noise of this loudness (root mean square, 1 is full scale).
    float microphone{};
    bool recenter{};
    // The headset taken off the head (false) or put back on (true), from this line on.
    std::optional<bool> worn;
};

std::vector<float> ParseNumbers(const std::string& text) {
    std::vector<float> numbers;
    std::istringstream stream{text};
    std::string item;
    while (std::getline(stream, item, ',')) {
        numbers.push_back(std::stof(item));
    }
    return numbers;
}

Core::Vr::Quat ParseRotation(const std::vector<float>& numbers, size_t first) {
    static constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
    const auto angle = [&](size_t index) {
        return first + index < numbers.size() ? numbers[first + index] * Radians : 0.0f;
    };
    return Core::Vr::FromYawPitchRoll(angle(0), angle(1), angle(2));
}

std::optional<Core::Vr::Pose> ParsePose(const std::string& text) {
    const auto numbers = ParseNumbers(text);
    if (numbers.size() < 3) {
        return std::nullopt;
    }
    Core::Vr::Pose pose;
    pose.position = {numbers[0], numbers[1], numbers[2]};
    pose.orientation = ParseRotation(numbers, 3);
    return pose;
}

const std::unordered_map<std::string, Buttons> ButtonNames = {
    {"cross", Buttons::Cross},       {"circle", Buttons::Circle}, {"square", Buttons::Square},
    {"triangle", Buttons::Triangle}, {"up", Buttons::Up},         {"down", Buttons::Down},
    {"left", Buttons::Left},         {"right", Buttons::Right},   {"l1", Buttons::L1},
    {"r1", Buttons::R1},             {"l2", Buttons::L2},         {"r2", Buttons::R2},
    {"l3", Buttons::L3},             {"r3", Buttons::R3},         {"options", Buttons::Options},
    {"touchpad", Buttons::TouchPad},
};

std::vector<Step> ParseScript(const std::filesystem::path& script) {
    std::vector<Step> steps;
    std::ifstream file{script};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream tokens{line};
        double start = 0.0;
        double hold = 0.0;
        if (!(tokens >> start >> hold)) {
            continue;
        }
        Step step{.start = start, .end = start + hold};
        std::string token;
        while (tokens >> token) {
            if (const auto it = ButtonNames.find(token); it != ButtonNames.end()) {
                step.buttons |= it->second;
                continue;
            }
            if (token.starts_with("head=")) {
                step.head = ParsePose(token.substr(5));
                continue;
            }
            if (token.starts_with("pad=")) {
                step.pad = ParsePose(token.substr(4));
                continue;
            }
            if (token.starts_with("padrot=")) {
                step.pad_rotation = ParseRotation(ParseNumbers(token.substr(7)), 0);
                continue;
            }
            if (token == "hands=off") {
                step.hands_lost = true;
                continue;
            }
            if (token.starts_with("hands=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.hands = std::array<float, 4>{numbers[0], numbers[1], numbers[2],
                                                      numbers.size() > 3 ? numbers[3] : 0.0f};
                }
                continue;
            }
            if (token.starts_with("mic=")) {
                step.microphone = std::stof(token.substr(4));
                continue;
            }
            if (token == "recenter") {
                step.recenter = true;
                continue;
            }
            if (token == "worn=0" || token == "worn=1") {
                step.worn = token.back() == '1';
                continue;
            }
            if (token.starts_with("touch=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 2) {
                    step.touch = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token.starts_with("finger=")) {
                const auto numbers = ParseNumbers(token.substr(7));
                if (numbers.size() >= 2) {
                    step.finger = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token == "gesture=press" || token == "gesture=swipe" || token == "gesture=pull") {
                step.gestures.press = step.gestures.press || token == "gesture=press";
                step.gestures.swipe = step.gestures.swipe || token == "gesture=swipe";
                step.gestures.pull = step.gestures.pull || token == "gesture=pull";
                continue;
            }
            if (token == "place=switch") {
                step.place_switch = true;
                continue;
            }
            if (token.starts_with("place=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.place_move = Core::Vr::Vec3{numbers[0], numbers[1], numbers[2]};
                }
                continue;
            }
            static constexpr std::array<std::string_view, 4> AxisNames = {"lx=", "ly=", "rx=",
                                                                           "ry="};
            for (size_t axis = 0; axis < AxisNames.size(); ++axis) {
                if (token.starts_with(AxisNames[axis])) {
                    step.sticks[axis] = std::stoi(token.substr(AxisNames[axis].size()));
                }
            }
        }
        steps.push_back(step);
    }
    return steps;
}

void Replay(std::vector<Step> steps, std::filesystem::path live_script) {
    Common::SetCurrentThreadName("shadPS4:ScriptedInput");
    const auto begin = std::chrono::steady_clock::now();
    double last_end = 0.0;
    for (const Step& step : steps) {
        last_end = std::max(last_end, step.end);
    }

    // The script's poses are counted from where the player sits.
    Core::Vr::Runtime::Instance().FixSeat();
    std::vector<bool> started(steps.size());

    Buttons previous_buttons{Buttons::None};
    std::array<int, 6> previous_axes{128, 128, 128, 128, 0, 0};
    std::optional<std::array<float, 2>> previous_touch;
    StickFinger stick_finger;
    PadGestures gestures;
    std::filesystem::file_time_type live_stamp{};
    while (true) {
        if (!live_script.empty()) {
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(live_script, error);
            if (!error && stamp != live_stamp) {
                live_stamp = stamp;
                steps = ParseScript(live_script);
                started.resize(steps.size());
                LOG_INFO(Input, "Live script now has {} steps", steps.size());
            }
        }
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

        Buttons buttons{Buttons::None};
        std::array<int, 6> axes{128, 128, 128, 128, 0, 0};
        const Step* head_step = nullptr;
        const Step* pad_step = nullptr;
        const Step* hands_step = nullptr;
        std::optional<std::array<float, 2>> touch;
        std::array<float, 2> finger{};
        PadGestures::Controls controls;
        float microphone = 0.0f;
        for (const Step& step : steps) {
            if (now >= step.start) {
                // Poses persist, the most recently started line wins.
                if (step.head && (head_step == nullptr || step.start >= head_step->start)) {
                    head_step = &step;
                }
                if ((step.pad || step.pad_rotation) &&
                    (pad_step == nullptr || step.start >= pad_step->start)) {
                    pad_step = &step;
                }
                if ((step.hands || step.hands_lost) &&
                    (hands_step == nullptr || step.start >= hands_step->start)) {
                    hands_step = &step;
                }
            }
            if (now < step.start || now >= step.end) {
                continue;
            }
            if (step.touch) {
                touch = step.touch;
            }
            if (step.finger) {
                finger = *step.finger;
            }
            controls.press = controls.press || step.gestures.press;
            controls.swipe = controls.swipe || step.gestures.swipe;
            controls.pull = controls.pull || step.gestures.pull;
            microphone = std::max(microphone, step.microphone);
            buttons |= step.buttons;
            for (size_t axis = 0; axis < step.sticks.size(); ++axis) {
                if (step.sticks[axis] >= 0) {
                    axes[axis] = step.sticks[axis];
                }
            }
        }
        microphone_level.store(microphone, std::memory_order_relaxed);
        // What buttons do on the touchpad comes before what the stick does there.
        if (const auto made = gestures.Update(now, controls); made.down && !touch) {
            touch = std::array<float, 2>{made.x, made.y};
            if (made.pressed) {
                buttons |= Buttons::TouchPad;
            }
            stick_finger.Reset();
        } else if (const auto stick = stick_finger.Update(now, finger[0], finger[1]);
                   stick.down && !touch) {
            // (A stick at rest between its lines: that is what lifts its finger.)
            touch = std::array<float, 2>{stick.x, stick.y};
        }
        auto& vr = Core::Vr::Runtime::Instance();
        if (head_step != nullptr) {
            vr.UpdateHead({.pose = *head_step->head, .tracked = true});
        }
        for (size_t i = 0; i < steps.size(); ++i) {
            if (now < steps[i].start || started[i]) {
                continue;
            }
            // Things that happen once, when their line starts.
            started[i] = true;
            if (steps[i].recenter) {
                // The player asks for the view to be reset, wherever the head is right now.
                vr.RequestRecenter();
            }
            if (steps[i].place_move) {
                vr.MoveOwnPadPlace(*steps[i].place_move);
            }
            if (steps[i].place_switch) {
                vr.SwitchPadPlace();
            }
            if (steps[i].worn) {
                LOG_INFO(Input, "Scripted: the headset is {}", *steps[i].worn ? "put on"
                                                                              : "taken off");
                vr.SetHeadsetWorn(*steps[i].worn);
            }
        }
        if (pad_step != nullptr) {
            if (pad_step->pad) {
                vr.UpdatePad({.pose = *pad_step->pad, .tracked = true});
            } else {
                vr.UpdatePadOrientation(*pad_step->pad_rotation, {});
            }
        }

        if (hands_step != nullptr) {
            if (hands_step->hands) {
                const auto& hands = *hands_step->hands;
                vr.UpdatePadPosition({hands[0], hands[1], hands[2]}, {});
                vr.UpdatePadYawReference(hands[3] * std::numbers::pi_v<float> / 180.0f);
            } else {
                vr.ClearPadPosition();
            }
        }

        // The analog triggers follow their digital buttons.
        axes[4] = True(buttons & Buttons::L2) ? 255 : 0;
        axes[5] = True(buttons & Buttons::R2) ? 255 : 0;

        if (buttons != previous_buttons || axes != previous_axes || touch != previous_touch) {
            auto* controllers = Common::Singleton<GameControllers>::Instance();
            (*controllers)[0]->ApplyRemoteState(buttons, axes, touch.has_value(),
                                                touch ? (*touch)[0] : 0.0f,
                                                touch ? (*touch)[1] : 0.0f);
            previous_buttons = buttons;
            previous_axes = axes;
            previous_touch = touch;
        }
        if (live_script.empty() && now > last_end + 1.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    microphone_level.store(0.0f, std::memory_order_relaxed);
    LOG_INFO(Input, "Scripted input finished");
}

} // namespace

float ScriptedMicrophoneLevel() {
    // (0.36: a little above what ASTRO BOT takes for blowing as hard as can be.)
    return std::max(microphone_level.load(std::memory_order_relaxed),
                    blowing.load(std::memory_order_relaxed) ? 0.36f : 0.0f);
}

void SetBlowing(bool blowing_now) {
    if (blowing.exchange(blowing_now, std::memory_order_relaxed) != blowing_now && blowing_now) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Input,
                     "Blowing with buttons: the game hears it as blowing into the microphone");
        }
    }
}

void StartScriptedInput(const std::filesystem::path& script) {
    auto steps = ParseScript(script);
    if (steps.empty()) {
        LOG_ERROR(Input, "No usable steps in input script {}", script.string());
        return;
    }
    LOG_INFO(Input, "Replaying {} input steps from {}", steps.size(), script.string());
    const char* live = std::getenv("SHADPS4_LIVE_INPUT");
    std::thread{Replay, std::move(steps), std::filesystem::path{live ? live : ""}}.detach();
}

} // namespace Input
