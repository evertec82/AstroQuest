// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/known_title.h"
#include "core/linker.h"
#include "core/module.h"
#include "core/vr/vr_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core::KnownTitle {

namespace {

using Clock = std::chrono::steady_clock;

// Astro Bot Rescue Mission, CUSA12392, in the build whose code at SetRecentre reads as below
// (the function of its tracking manager that asks for the head's position to be taken anew).
constexpr u64 SetRecentre = 0xc48320;
constexpr u8 SetRecentreCode[] = {0x40, 0x0f, 0xb6, 0xc6, 0xff, 0xc0, 0x89, 0x07, 0xc3};

// The tracking manager, a singleton: the point it counts positions from, and for the headset
// the position counted from it.
constexpr u64 ManagerPointer = 0x2e025a8;
constexpr u64 ManagerOrigin = 0x6210;
constexpr u64 ManagerHeadState = 0x6848;
constexpr u64 StatePosition = 0x10;
constexpr u64 StatePositionValid = 0x54;

// The engine's frame rate and what it derives from it when the rate is set (its function at
// 0xe48fd0): the time one frame stands for, in seconds and in microseconds. Everything in the
// game that moves reads one of the latter two.
constexpr u64 EngineFrameRate = 0x16688a8;         // double
constexpr u64 EngineFrameSeconds = 0x16688b0;      // float
constexpr u64 EngineFrameMicroseconds = 0x16688b8; // u64
constexpr u64 ImageEnd = EngineFrameMicroseconds + sizeof(u64);

// What sets the size the scene is drawn at, a singleton made when first asked for: the size is
// one of a list (ConsoleSizes, the first three are for a television), a base (4 on the
// console, 6 on its Pro model) plus an offset that the game moves between a lowest and a
// highest one by how long it finds the GPU to take over its drawing. It reads that off
// timestamps which, emulated, tell how long the emulator took to pass the drawing on, not how
// long the GPU takes over it: left to itself, the game draws large where the GPU is busiest.
constexpr u64 ResolutionPointer = 0x2dff9e0;
constexpr u64 ResolutionBase = 0x0;     // s32
constexpr u64 ResolutionLevel = 0x4;    // s32
constexpr u64 ResolutionOffset = 0x20;  // s32
constexpr u64 ResolutionHighest = 0x28; // s32, an offset
constexpr u64 ResolutionLowest = 0x2c;  // s32, an offset
constexpr s32 FirstHeadsetLevel = 3;
constexpr s32 LastHeadsetLevel = 6;
// The sizes of the list, an eye, as the console has them.
constexpr std::array<std::array<u32, 2>, 7> ConsoleSizes{{
    {640, 360}, {1280, 720}, {1920, 1080}, {816, 870}, {960, 1080}, {1200, 1280}, {1440, 1536}}};

// Where the sizes of the headset's list are written down (see Larger): a table of widths, one of
// heights, the same once more in the switch of the function that makes the scene's targets
// (heights, then widths), and a table of pixel counts the title's own choice goes by.
constexpr u64 SizeWidths = 0x12da900;  // u32 [7]
constexpr u64 SizeHeights = 0x12da920; // u32 [7]
constexpr std::array<std::array<u64, 2>, 4> SizeSwitch{{
    {0xf2242f, 0xf22435}, {0xf2240d, 0xf22413}, {0xf22551, 0xf22557}, {0xf2255f, 0xf22565}}};
constexpr u64 SizePixels = 0x1645048; // u64, 32 bytes apart
// The pictures handed to the headset, made three pairs at a time: their width and height.
constexpr std::array<u64, 4> EyeSizes{0xc3fad0, 0xc3fad5, 0xc3fb5c, 0xc3fb61};
// What the targets are taken from: a pool of 200 MB (its size in two places), a smaller one of
// 10 MB for what goes with them (in three), and the heap of graphics memory both come from,
// 872 MB, which the title takes from the console's memory at its start.
constexpr std::array<u64, 2> TargetPool{0xef8bf7, 0xef8c4d};
constexpr std::array<u64, 3> SmallPool{0xf22194, 0xf221be, 0xf221dd};
constexpr u64 GraphicsHeap = 0x1269708; // u64
constexpr u32 ConsoleTargetPool = 0xc800000;
constexpr u32 ConsoleSmallPool = 0xa00000;
constexpr u64 ConsoleGraphicsHeap = 0x36800000;

/// The title drawing larger than it does on the console: every size of the headset's list (and
/// the pictures handed to the headset) grown by the same factor, and the memory that takes.
/// SHADPS4_TITLE_EYE_WIDTH=<pixels> is the width of the largest, 1440 on the console.
struct Larger {
    double factor{1.0};
    std::array<std::array<u32, 2>, 7> sizes{ConsoleSizes};
    u32 target_pool{ConsoleTargetPool};
    u32 small_pool{ConsoleSmallPool};
    u64 graphics_heap{ConsoleGraphicsHeap};
    /// What the console's memory has to grow by for it, in MB.
    s32 extra_memory_mb{};
};

const Larger& GetLarger() {
    static const Larger larger = [] {
        Larger result;
        const char* value = std::getenv("SHADPS4_TITLE_EYE_WIDTH");
        const s32 width = value != nullptr ? std::atoi(value) : 0;
        if (width <= static_cast<s32>(ConsoleSizes[LastHeadsetLevel][0])) {
            return result;
        }
        result.factor = std::min(static_cast<double>(width), 4320.0) / ConsoleSizes[6][0];
        const auto to_eighths = [&](u32 size) {
            return static_cast<u32>(std::lround(size * result.factor / 8.0)) * 8;
        };
        for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
            result.sizes[level] = {to_eighths(ConsoleSizes[level][0]),
                                   to_eighths(ConsoleSizes[level][1])};
        }
        // What they take grows with their pixels, and some room besides.
        static constexpr u64 MB = 1ull << 20;
        const double pixels = result.factor * result.factor;
        const auto megabytes = [&](u64 bytes) {
            return (static_cast<u64>(static_cast<double>(bytes) * pixels * 1.1) + MB - 1) / MB * MB;
        };
        result.target_pool = static_cast<u32>(megabytes(ConsoleTargetPool));
        result.small_pool = static_cast<u32>(megabytes(ConsoleSmallPool));
        const u64 growth = (result.target_pool - ConsoleTargetPool) +
                           (result.small_pool - ConsoleSmallPool) +
                           static_cast<u64>(200.0 * MB * (pixels - 1.0));
        result.graphics_heap = ConsoleGraphicsHeap + growth;
        result.extra_memory_mb = static_cast<s32>((growth + 256 * MB) / (256 * MB) * 256);
        return result;
    }();
    return larger;
}

/// "1440x1536" and the like, for a level of the list.
std::string SizeName(s32 level) {
    if (level < 0 || level > LastHeadsetLevel) {
        return "a size not known";
    }
    const auto& size = GetLarger().sizes[level];
    return fmt::format("{}x{}", size[0], size[1]);
}

/// Where the title's image starts if it is the build described above, 0 otherwise.
VAddr KnownBase() {
    static const VAddr base = []() -> VAddr {
        if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392") {
            return 0;
        }
        const Module* eboot = Common::Singleton<Linker>::Instance()->GetModule(0);
        if (eboot == nullptr || !eboot->IsValid() ||
            eboot->aligned_base_size < std::max(ManagerPointer + sizeof(u64), ImageEnd)) {
            return 0;
        }
        const VAddr image = eboot->GetBaseAddress();
        if (std::memcmp(reinterpret_cast<const void*>(image + SetRecentre), SetRecentreCode,
                        sizeof(SetRecentreCode)) != 0) {
            LOG_INFO(Core, "This is another build of CUSA12392 than the one known from inside: "
                           "it is left to itself");
            return 0;
        }
        LOG_INFO(Core, "CUSA12392 in the build known from inside");
        return image;
    }();
    return base;
}

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void Write(VAddr address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

/// The time step the title was made for.
constexpr double Nominal = 1.0 / 60.0;
/// A frame that takes several times what frames take is a load or a hitch: nothing the game is
/// to catch up with by running fast afterwards, and nothing that says how fast it draws.
constexpr double Stall = 0.25;
/// The most refreshes of the headset a frame is ever given.
constexpr s32 SlowestPace = 6;
/// The refreshes of the display frames are given at the moment, for the thread that makes
/// the headset's.
std::atomic<u32> frame_pace{0};

struct Settings {
    bool time_step{true};
    /// Frames longer than this are not made up for: the game slows down instead of taking
    /// steps its physics were never tried with.
    double longest_step{1.0 / 20.0};

    enum class Resolution { Title, Pinned, Governed };
    Resolution resolution{Resolution::Governed};
    s32 pinned_level{FirstHeadsetLevel};

    /// Refreshes of the display a frame is given: 0 is chosen by what the title manages, 1
    /// and more is that many throughout.
    s32 pace{};
    /// The fewest the choice may come to where the display refreshes faster than the title
    /// was made to draw: 2 is the title's own way (a frame for every two refreshes), 1 has it
    /// draw a frame for every refresh, faster than it ever did on the console.
    s32 fastest_pace{2};
    /// The most frames a second wanted, 0 for no such limit: frames are given at least as many
    /// refreshes as keep them to that.
    double fps_cap{};
};

const Settings& GetSettings() {
    static const Settings settings = [] {
        Settings parsed;
        if (const char* value = std::getenv("SHADPS4_TITLE_TIMESTEP"); value != nullptr) {
            const double rate = std::atof(value);
            if (rate <= 0.0) {
                parsed.time_step = false;
            } else {
                parsed.longest_step = 1.0 / std::clamp(rate, 10.0, 60.0);
            }
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_RESOLUTION"); value != nullptr) {
            const std::string_view text{value};
            const s32 level = std::atoi(value);
            if (text == "title") {
                parsed.resolution = Settings::Resolution::Title;
            } else if (level >= FirstHeadsetLevel && level <= LastHeadsetLevel) {
                parsed.resolution = Settings::Resolution::Pinned;
                parsed.pinned_level = level;
            }
        }
        if (const char* value = std::getenv("SHADPS4_VR_PACE"); value != nullptr) {
            const s32 pace = std::atoi(value);
            parsed.pace = pace >= 1 ? std::min(pace, SlowestPace) : 0;
        }
        if (const char* value = std::getenv("SHADPS4_VR_FASTEST_PACE"); value != nullptr) {
            parsed.fastest_pace = std::atoi(value) == 1 ? 1 : 2;
        }
        // SHADPS4_VR_FPS_CAP=<frames a second>: as many as that at most, as many refreshes of
        // the display a frame as that takes (at 120 Hz: 120, 60, 40 or 30; at 90 Hz: 90, 45 or
        // 30). Frames come faster than the console's 60 only this way.
        if (const char* value = std::getenv("SHADPS4_VR_FPS_CAP"); value != nullptr) {
            const double cap = std::atof(value);
            if (cap >= 10.0) {
                parsed.fps_cap = std::min(cap, 240.0);
            }
        }
        return parsed;
    }();
    return settings;
}

/// Chooses how many refreshes of the headset a frame is given, and the size the scene is drawn
/// at, from what the title manages and how busy the GPU really is. Asked every frame with what
/// the frame took; answers with the size to hold the title to.
///
/// The title waits for two refreshes to pass before it goes on to the next frame, and goes
/// on at once when the frame took longer than that: it then draws as fast as it can, at a
/// rate that has nothing to do with the display's, and its frames are shown for two refreshes
/// or for three as they happen to fall, which is what jerky motion is. So the question is
/// never "how fast" alone but "how many refreshes, and what fits into them":
///  - the pace is the fastest at which the smallest size wanted fits with room to spare. That
///    size is the console's own smallest (960x1080); only at the slowest pace there is - the
///    last that still makes 30 frames a second - the smallest size of all (816x870) will do.
///    The emulated headset refreshes at half that pace (FramePace), which is what the title
///    keeps time by, and every frame is shown for as long as the next;
///  - the size is the largest that fits that pace with room to spare;
///  - what a size costs is taken from what it was seen to cost, and a step that had to be
///    taken back makes the next one wait longer (and a size count as dearer).
class Governor {
public:
    /// For `fixed`: the size is the title's own business.
    static constexpr s32 NotMine = -1;

    struct Summary {
        s32 pace;
        double slot;
        double load;
    };

    /// `fixed_pace_`: refreshes to give every frame, 0 to choose; `fastest_pace_`: the fewest
    /// the choice may come to on a display that refreshes faster than the title draws.
    Governor(s32 fixed_pace_, s32 fastest_pace_, double fps_cap_)
        : fixed_pace{fixed_pace_}, fastest_pace{fastest_pace_}, fps_cap{fps_cap_},
          pace{fixed_pace_ >= 1 ? fixed_pace_ : 2} {
        // The original ten-minute hold can outlast a temporary demanding scene. Allow
        // bounded probes sooner without removing the existing backoff after failed probes.
        if (const char* value = std::getenv("SHADPS4_VR_RETRY_SECONDS"); value != nullptr) {
            const s32 seconds = std::atoi(value);
            if (seconds > 0) {
                retry_after = std::chrono::seconds{std::clamp(seconds, 15, 600)};
            }
        }
        LOG_INFO(Core, "Full-refresh recovery may retry after {} seconds; fixed pace {}",
                 retry_after.count(), fixed_pace);
    }

    /// `fixed` is a size to keep to, 0 where the size is to be chosen here, NotMine where the
    /// title chooses it. Answers with the size to hold the title to, 0 for none.
    s32 Level(double frame, Clock::time_point now, s32 fixed) {
        const float rate = Vr::Runtime::Instance().HeadsetRefreshRate();
        refresh = 1.0 / static_cast<double>(rate > 0.0f ? rate : 120.0f);
        if (fixed > 0) {
            level = fixed;
        }
        // A display that refreshes no faster than the title draws on the console (60 times a
        // second, or half as often as a faster one where the system makes up every other
        // picture itself): the title's two refreshes for a frame would halve its frame
        // rate, one refresh for a frame is its own.
        s32 fastest_now = refresh > SlowDisplay ? 1 : fastest_pace;
        if (fps_cap > 0.0) {
            // The fewest refreshes that keep frames to the cap.
            fastest_now = std::max<s32>(
                1, static_cast<s32>(std::ceil(1.0 / (refresh * fps_cap) - 0.01)));
        }
        if (fixed_pace == 0 && (fastest_now != fastest || pace < fastest_now)) {
            // The display turned out to be another kind than was thought (its rate is only
            // known once the headset is there): no waiting to find out what fits.
            if (fastest_now == 1 && refresh > SlowDisplay) {
                pace = 1;
            } else {
                pace = std::max(pace, fastest_now);
            }
        }
        fastest = fastest_now;
        if (!started) {
            started = true;
            window_start = now;
            changed = now;
            Vulkan::FrameStats::TakeGpuLoad();
        } else {
            if (frame <= Stall) {
                window_time += frame;
                ++window_frames;
            }
            if (now - window_start >= Window) {
                Decide(now, fixed);
            }
        }
        return fixed == NotMine ? 0 : level;
    }

    /// The refreshes of the display a frame is given.
    u32 Pace() const {
        return static_cast<u32>(pace);
    }

    /// What frames were given and how busy the GPU was since this was last asked.
    Summary TakeSummary() {
        const Summary summary{
            .pace = pace,
            .slot = pace * refresh,
            .load = summary_windows != 0 ? summary_load / summary_windows : 0.0,
        };
        summary_load = 0.0;
        summary_windows = 0;
        return summary;
    }

private:
    void Decide(Clock::time_point now, s32 fixed) {
        const float load = Vulkan::FrameStats::TakeGpuLoad();
        const u32 frames = window_frames;
        const double taken = frames != 0 ? window_time / frames : 0.0;
        window_start = now;
        window_time = 0.0;
        window_frames = 0;
        if (load < 0.0f || frames < FewestFrames) {
            // Loading, or nothing drawn: nothing to go by.
            tight_windows = 0;
            faster_windows = 0;
            return;
        }
        summary_load += std::min(load, 1.0f);
        ++summary_windows;

        const bool sized = fixed == 0;
        const bool paced = fixed_pace == 0;
        const double slot = pace * refresh;
        // How long the GPU worked on a frame (at least: one that never rests may have more to
        // do than it gets done).
        const double gpu = std::min(load, 1.0f) * taken;
        const auto since_change = now - changed;
        const auto cost_at = [&](s32 other) { return gpu * cost[other] / cost[level]; };
        // The slowest pace frames are ever held to, and the smallest size wanted at a pace.
        const s32 slowest = std::max(std::max(2, fastest), static_cast<s32>(SlowestWanted / refresh));
        const auto smallest_at = [&](s32 refreshes_given) {
            return !paced || refreshes_given >= slowest ? FirstHeadsetLevel : UsualLevel;
        };
        // The largest size that fits a pace with room to spare, if any does.
        const auto largest_fitting = [&](s32 refreshes_given, s32 from, s32 down_to) {
            for (s32 size = from; size > down_to; --size) {
                if (cost_at(size) * (1.0 + SizeMargin) <= refreshes_given * refresh) {
                    return size;
                }
            }
            return down_to;
        };
        if (paced && pace > slowest) {
            // The display refreshes more slowly than it did.
            SetPace(slowest, now, taken, load);
            tight_windows = 0;
            faster_windows = 0;
            return;
        }
        if (now - last_regret > std::max<Clock::duration>(4 * patience, Calm)) {
            patience = FirstPatience;
        }
        if (now - last_pace_regret > std::max<Clock::duration>(4 * pace_patience, Calm)) {
            pace_patience = FirstPatience;
        }

        if (taken > slot * (1.0 + Over)) {
            // Frames do not fit the refreshes they are given: the title is on its own pace.
            faster_windows = 0;
            ++tight_windows;
            if (tight_windows < 2 || since_change < SettleDown) {
                return;
            }
            // A GPU that hardly rests is what holds frames up, and a smaller picture helps;
            // one that rests for long is not, and a smaller picture would change nothing.
            const bool gpu_short = load > GpuShort;
            const auto slower = [&](s32 to) {
                if (now - went_faster < PaceRegret) {
                    // The faster pace was a mistake.
                    pace_patience = std::min(pace_patience * 2, LongestPatience);
                    last_pace_regret = now;
                    if (pace == 1) {
                        // Left to itself, as it was: this is what the title's own work takes.
                        own_time = taken;
                    }
                }
                SetPace(to, now, taken, load);
                went_faster = {};
                faster_allowed = now + pace_patience;
            };
            if (sized && gpu_short && (level > smallest_at(pace) || (paced && pace < slowest))) {
                if (now - went_up < Regret) {
                    // The step up was a mistake: that size costs more than was thought.
                    patience = std::min(patience * 2, LongestPatience);
                    for (s32 size = level; size <= LastHeadsetLevel; ++size) {
                        cost[size] *= Dearer;
                    }
                    last_regret = now;
                }
                // Straight to what the GPU's time says will do: the largest size that fits
                // this pace, or the next pace down and the largest that fits that. (A GPU
                // that never rests may need more than it is seen to take; then this comes
                // round again.)
                const s32 smallest = smallest_at(pace);
                s32 size = level > smallest ? largest_fitting(pace, level - 1, smallest) : level;
                const bool fits = cost_at(size) * (1.0 + SizeMargin) <= slot;
                const bool other_pace = !fits && paced && pace < slowest;
                if (other_pace) {
                    slower(pace + 1);
                    size = largest_fitting(pace, level, smallest_at(pace));
                }
                if (size != level) {
                    Change(size, now, taken, load, other_pace);
                }
                went_up = {};
                up_allowed = now + patience;
                tight_windows = 0;
            } else if (paced && pace < slowest && tight_windows >= 3) {
                // The title's own work is what takes long, or the size is not this one's to
                // choose: the pace that what frames take now fits into.
                const s32 needed = static_cast<s32>(std::ceil(taken * (1.0 + Over) / refresh));
                slower(std::clamp(needed, pace + 1, slowest));
                went_up = {};
                tight_windows = 0;
            }
            return;
        }
        tight_windows = 0;

        // A faster pace, when the smallest size wanted for it would fit with room to spare.
        // (Whether the title's own work fits it as well only shows when it is tried.)
        // (A frame for every refresh is not tried again for long where the title's own work was
        // seen not to fit one, as on a display that refreshes faster than the title can draw
        // whatever its size: every try is a few seconds of frames that come unevenly.)
        const bool own_fits = pace != 2 || own_time == 0.0 ||
                              own_time * (1.0 + Over) <= refresh ||
                              now - last_pace_regret > retry_after;
        if (paced && pace > fastest && now > faster_allowed && own_fits) {
            const s32 smallest = sized ? smallest_at(pace - 1) : level;
            if (cost_at(smallest) * (1.0 + PaceMargin) <= (pace - 1) * refresh) {
                if (++faster_windows >= FasterWindows && since_change > SettleUp) {
                    faster_windows = 0;
                    const s32 size = sized ? largest_fitting(pace - 1, level, smallest) : level;
                    SetPace(pace - 1, now, taken, load);
                    if (size != level) {
                        Change(size, now, taken, load, true);
                    }
                    went_up = {};
                    went_faster = now;
                }
                return;
            }
        }
        faster_windows = 0;
        // A larger picture, when it would fit the pace with room to spare.
        if (sized && level < LastHeadsetLevel && since_change > SettleUp && now > up_allowed &&
            cost_at(level + 1) * (1.0 + SizeMargin) <= slot) {
            Change(level + 1, now, taken, load);
            went_up = now;
        }
    }

    /// `paced`: the size goes with a pace that has just changed.
    void Change(s32 to, Clock::time_point now, double taken, float load, bool paced = false) {
        if (paced) {
            LOG_INFO(Core,
                     "The scene is drawn at {} an eye from now on instead of {}: the largest "
                     "that fits the {:.1f} ms frames are given now",
                     SizeName(to), SizeName(level), pace * refresh * 1e3);
        } else {
            LOG_INFO(Core,
                     "The scene is drawn at {} an eye from now on instead of {}, which is {} for "
                     "the {:.1f} ms frames are given: they took {:.1f} ms, with the GPU busy "
                     "{:.0f}% of the time",
                     SizeName(to), SizeName(level),
                     to < level ? "too much" : "less than there is room", pace * refresh * 1e3,
                     taken * 1e3, load * 100.0f);
        }
        level = to;
        changed = now;
    }

    void SetPace(s32 to, Clock::time_point now, double taken, float load) {
        LOG_INFO(Core,
                 "Frames are given {} refreshes from now on ({:.0f} a second), {} was {}: frames "
                 "took {:.1f} ms at {}, the GPU was busy {:.0f}% of the time",
                 to, 1.0 / (to * refresh), pace, to > pace ? "too few" : "more than needed",
                 taken * 1e3, SizeName(level), load * 100.0f);
        pace = to;
        changed = now;
    }

    /// The console's smallest size in most levels.
    static constexpr s32 UsualLevel = 4;
    /// Frames are never held to a pace slower than this (in seconds a frame): a title that
    /// cannot make it either is left to draw as fast as it can.
    static constexpr double SlowestWanted = 0.0345;
    /// A display whose refreshes are further apart than this refreshes no faster than the
    /// title draws.
    static constexpr double SlowDisplay = 1.0 / 65.0;
    static constexpr auto Window = std::chrono::milliseconds{500};
    static constexpr u32 FewestFrames = 5;
    static constexpr auto SettleDown = std::chrono::milliseconds{900};
    static constexpr auto SettleUp = std::chrono::milliseconds{2400};
    static constexpr auto Regret = std::chrono::seconds{8};
    static constexpr auto PaceRegret = std::chrono::seconds{15};
    static constexpr auto Calm = std::chrono::seconds{90};
    static constexpr auto FirstPatience = std::chrono::seconds{6};
    static constexpr auto LongestPatience = std::chrono::seconds{120};
    static constexpr u32 FasterWindows = 4;
    /// By how much of what they are given frames may take longer, on average, and still count
    /// as fitting: frames that fit take exactly what they are given.
    static constexpr double Over = 0.03;
    /// The share of the time the GPU must be busy to be what holds frames up.
    static constexpr float GpuShort = 0.85f;
    /// Room a larger size, and a faster pace, must leave.
    static constexpr double SizeMargin = 0.15;
    static constexpr double PaceMargin = 0.10;
    /// What a size that had to be taken back is counted as costing more.
    static constexpr double Dearer = 1.06;

    const s32 fixed_pace;
    const s32 fastest_pace;
    const double fps_cap;
    s32 fastest{2};
    bool started{};
    s32 level{UsualLevel};
    s32 pace;
    double refresh{1.0 / 120.0};
    // GPU time of a frame at each size, in terms of the smallest (measured on the headset with
    // the GPU as the limit; a size that proves dearer is marked up).
    std::array<double, 7> cost{1.0, 1.0, 1.0, 1.0, 1.08, 1.27, 1.54};
    Clock::time_point window_start;
    double window_time{};
    u32 window_frames{};
    u32 tight_windows{};
    u32 faster_windows{};
    Clock::time_point changed;
    Clock::time_point up_allowed;
    Clock::time_point faster_allowed;
    Clock::time_point last_regret;
    Clock::time_point last_pace_regret;
    std::chrono::seconds patience{FirstPatience};
    std::chrono::seconds pace_patience{FirstPatience};
    std::chrono::seconds retry_after{600};
    // What frames took when one refresh each was last found to be too few for them.
    double own_time{};
    // When the size last went up and the pace last got faster, if that still stands.
    Clock::time_point went_up;
    Clock::time_point went_faster;
    double summary_load{};
    u32 summary_windows{};
};

// The size the title draws its scene at right now, as an index into ConsoleSizes; -1 when
/// it has not got that far. Holds it to `wanted` on the way unless that is 0.
s32 TendResolution(VAddr base, s32 wanted) {
    const u64 control = Read<u64>(base + ResolutionPointer);
    if (control == 0) {
        return -1;
    }
    const s32 level = Read<s32>(control + ResolutionLevel);
    if (wanted != 0 && level >= FirstHeadsetLevel) {
        const s32 offset = wanted - Read<s32>(control + ResolutionBase);
        Write<s32>(control + ResolutionOffset, offset);
        Write<s32>(control + ResolutionHighest, offset);
        Write<s32>(control + ResolutionLowest, offset);
    }
    return level;
}

/// Keeps the title's time step at what its frames take. Asked every frame with what the frame
/// took; answers with the step in effect from now on.
class TimeStep {
public:
    /// `shortest`: the least a step may be. The title's own (a sixtieth of a second) unless
    /// it is given a refresh for every frame on a display that is faster than that.
    double Next(double frame, double longest, double shortest) {
        if (frame > Stall) {
            return step;
        }
        stepped += step;
        average += (frame - average) * Follow;
        owed = std::clamp(owed + frame - step, -MostOwed, MostOwed);
        step = std::clamp(average + owed * Repay, std::min(shortest, longest), longest);
        // At the rate the title was made for, exactly what it would use itself.
        if (std::abs(average - Nominal) < 0.0004 && std::abs(owed) < 0.004) {
            step = Nominal;
            owed = 0.0;
        }
        return step;
    }

    /// Game time gone by since this was last asked.
    double TakeStepped() {
        return std::exchange(stepped, 0.0);
    }

private:
    // How much of the difference to the last frame goes into what frames are taken to take.
    // A step that follows every frame would be wrong twice over where long and short frames
    // alternate, which they do when the display's refreshes do not divide by the frame rate.
    static constexpr double Follow = 0.15;
    // Real time the game's clock has fallen behind (or run ahead) by, at most, and how much
    // of it a frame makes up for. This is what keeps what is seen in step with what is heard.
    static constexpr double MostOwed = 0.1;
    static constexpr double Repay = 0.1;

    double average{Nominal};
    double step{Nominal};
    double owed{};
    double stepped{};
};

} // namespace

void OnFrameSubmitted() {
    const VAddr base = KnownBase();
    if (base == 0) {
        return;
    }
    const Settings& settings = GetSettings();

    static bool running = false;
    static Clock::time_point last;
    static TimeStep time_step;
    static Governor governor{settings.pace, settings.fastest_pace, settings.fps_cap};
    // For the log.
    static Clock::time_point report_time;
    static double report_real = 0.0;
    static u32 report_frames = 0;
    static double step = Nominal;

    const auto now = Clock::now();
    if (!running) {
        running = true;
        last = now;
        report_time = now;
        LOG_INFO(Core,
                 "The title's time step {}; the size of its scene is {}; the refreshes a frame "
                 "is given are {}",
                 settings.time_step ? "follows what its frames take" : "is left alone",
                 settings.resolution == Settings::Resolution::Title    ? "left to the title"
                 : settings.resolution == Settings::Resolution::Pinned ? "held to one"
                                                                       : "chosen by how busy the "
                                                                         "GPU is",
                 settings.pace == 0 ? "chosen by what the title manages" : "one number");
        return;
    }
    const double frame = std::chrono::duration<double>(now - last).count();
    last = now;

    s32 wanted = 0;
    switch (settings.resolution) {
    case Settings::Resolution::Title:
        governor.Level(frame, now, Governor::NotMine);
        break;
    case Settings::Resolution::Pinned:
        wanted = governor.Level(frame, now, settings.pinned_level);
        break;
    case Settings::Resolution::Governed:
        wanted = governor.Level(frame, now, 0);
        break;
    }
    const s32 resolution = TendResolution(base, wanted);
    frame_pace.store(governor.Pace(), std::memory_order_relaxed);

    if (settings.time_step) {
        // Shorter steps than the title's own only where it is made to draw faster than it
        // was made for: a frame for every refresh of a display faster than 60 Hz.
        const double shortest = governor.Pace() == 1 ? 1.0 / 250.0 : Nominal;
        step = time_step.Next(frame, settings.longest_step, shortest);
        Write<double>(base + EngineFrameRate, 1.0 / step);
        Write<float>(base + EngineFrameSeconds, static_cast<float>(step));
        Write<u64>(base + EngineFrameMicroseconds, static_cast<u64>(step * 1e6));
    }

    if (frame <= Stall) {
        report_real += frame;
        ++report_frames;
    }
    if (now - report_time >= std::chrono::seconds{10} && report_frames != 0) {
        const double stepped =
            settings.time_step ? time_step.TakeStepped() : Nominal * report_frames;
        const Governor::Summary summary = governor.TakeSummary();
        LOG_INFO(Core,
                 "The title's clock: frames take {:.1f} ms, its time step is {:.1f} ms, the game "
                 "ran at {:.0f}% of its speed over the last {:.0f} s (it would have at {:.0f}% "
                 "left to itself); it draws the scene at {} an eye and is given {} refreshes a "
                 "frame ({:.1f} ms), the GPU busy {:.0f}% of the time",
                 report_real / report_frames * 1e3, step * 1e3, 100.0 * stepped / report_real,
                 report_real, 100.0 * Nominal * report_frames / report_real,
                 SizeName(resolution),
                 summary.pace, summary.slot * 1e3, summary.load * 100.0);
        report_time = now;
        report_real = 0.0;
        report_frames = 0;
    }
}

u32 FramePace() {
    return frame_pace.load(std::memory_order_relaxed);
}

void Prepare() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392") {
        return;
    }
    const Larger& larger = GetLarger();
    if (larger.extra_memory_mb == 0) {
        return;
    }
    if (EmulatorSettings.GetExtraDmemInMBytes() < larger.extra_memory_mb) {
        EmulatorSettings.SetExtraDmemInMBytes(larger.extra_memory_mb);
    }
    LOG_INFO(Core, "The title is to draw at up to {} an eye: its memory grows by {} MB",
             SizeName(LastHeadsetLevel), EmulatorSettings.GetExtraDmemInMBytes());
}

void OnGameLoaded(VAddr base, u64 size) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392" || size < ImageEnd ||
        std::memcmp(reinterpret_cast<const void*>(base + SetRecentre), SetRecentreCode,
                    sizeof(SetRecentreCode)) != 0) {
        return;
    }
    const Larger& larger = GetLarger();
    if (larger.factor == 1.0) {
        return;
    }
    // Every place is checked for what the console's build has there before anything is
    // written: a title that turns out to be other than thought is left as it is.
    struct Change {
        u64 at;
        u64 was;
        u64 now;
        u32 bytes;
    };
    std::vector<Change> changes;
    for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
        const auto& was = ConsoleSizes[level];
        const auto& now = larger.sizes[level];
        changes.push_back({SizeWidths + 4 * level, was[0], now[0], 4});
        changes.push_back({SizeHeights + 4 * level, was[1], now[1], 4});
        changes.push_back({SizeSwitch[level - FirstHeadsetLevel][0], was[1], now[1], 4});
        changes.push_back({SizeSwitch[level - FirstHeadsetLevel][1], was[0], now[0], 4});
        changes.push_back({SizePixels + 32 * level, u64{was[0]} * was[1], u64{now[0]} * now[1], 8});
    }
    const auto& eye_was = ConsoleSizes[LastHeadsetLevel];
    const auto& eye_now = larger.sizes[LastHeadsetLevel];
    for (u32 i = 0; i < EyeSizes.size(); ++i) {
        changes.push_back({EyeSizes[i], eye_was[i % 2], eye_now[i % 2], 4});
    }
    for (const u64 at : TargetPool) {
        changes.push_back({at, ConsoleTargetPool, larger.target_pool, 4});
    }
    for (const u64 at : SmallPool) {
        changes.push_back({at, ConsoleSmallPool, larger.small_pool, 4});
    }
    changes.push_back({GraphicsHeap, ConsoleGraphicsHeap, larger.graphics_heap, 8});

    for (const Change& change : changes) {
        u64 found = 0;
        std::memcpy(&found, reinterpret_cast<const void*>(base + change.at), change.bytes);
        if (found != change.was) {
            LOG_WARNING(Core,
                        "The title has {:#x} at {:#x} where {:#x} was expected: it draws at the "
                        "console's sizes",
                        found, change.at, change.was);
            return;
        }
    }
    for (const Change& change : changes) {
        std::memcpy(reinterpret_cast<void*>(base + change.at), &change.now, change.bytes);
    }
    LOG_INFO(Core,
             "The title draws at up to {} an eye instead of 1440x1536 ({:.2f} times as wide), "
             "the smallest {}; its render targets have {} MB, its graphics memory {} MB",
             SizeName(LastHeadsetLevel), larger.factor, SizeName(FirstHeadsetLevel),
             larger.target_pool >> 20, larger.graphics_heap >> 20);
}

void NoteView(const Vr::Vec3& tracker_head) {
    static constexpr auto Interval = std::chrono::seconds{10};

    const VAddr base = KnownBase();
    if (base == 0) {
        return;
    }
    static std::mutex mutex;
    static Vr::Vec3 last_origin;
    static Clock::time_point last_report;
    static bool reported = false;

    const std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
    const u64 manager = Read<u64>(base + ManagerPointer);
    if (manager == 0) {
        return;
    }
    const auto origin = Read<Vr::Vec3>(manager + ManagerOrigin);
    const u64 state = Read<u64>(manager + ManagerHeadState);
    if (state == 0 || Read<u8>(state + StatePositionValid) == 0) {
        return;
    }
    const auto head = Read<Vr::Vec3>(state + StatePosition);

    const bool moved = !reported || std::abs(origin.x - last_origin.x) > 0.01f ||
                       std::abs(origin.y - last_origin.y) > 0.01f ||
                       std::abs(origin.z - last_origin.z) > 0.01f;
    const auto now = Clock::now();
    if (!moved && now - last_report < Interval) {
        return;
    }
    // Towards the camera of a PlayStation VR is where the player faces: ahead.
    LOG_INFO(Core_Vr,
             "{} {:.2f} {:.2f} {:.2f} of the tracker's space; the head, at {:.2f} {:.2f} {:.2f}, "
             "is {:.2f} m to the right of that, {:.2f} above and {:.2f} ahead",
             moved ? "The title now takes the player to sit at"
                   : "The title has the player's seat at",
             origin.x, origin.y, origin.z, tracker_head.x, tracker_head.y, tracker_head.z, head.x,
             head.y, -head.z);
    last_origin = origin;
    last_report = now;
    reported = true;
}

} // namespace Core::KnownTitle
