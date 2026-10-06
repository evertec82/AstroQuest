#include <cmath>
#include <cstdio>
#include <limits>

#include "core/vr/spectator_view.h"

using namespace Core::Vr;

bool Check(bool value, const char* name) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
    return value;
}

bool Near(float a, float b) {
    return std::abs(a - b) < 1e-5f;
}

bool CoversWidth(u32 width, const Fov& fov) {
    const auto regions = CombinedEyeRegions(width, fov);
    for (const auto& region : regions) {
        if (region.x + region.width > width || region.clip_x + region.clip_width > width ||
            (region.clip_width != 0 &&
             (region.clip_x < region.x ||
              region.clip_x + region.clip_width > region.x + region.width))) {
            return false;
        }
    }
    for (u32 pixel = 0; pixel < width; ++pixel) {
        const bool primary =
            pixel >= regions[0].clip_x && pixel < regions[0].clip_x + regions[0].clip_width;
        const bool secondary =
            pixel >= regions[1].clip_x && pixel < regions[1].clip_x + regions[1].clip_width;
        if (primary == secondary) {
            return false;
        }
    }
    return true;
}

int main() {
    bool ok = true;
    ok &= Check(ParseDesktopView("") == DesktopView::Stereo &&
                    ParseDesktopView("unknown") == DesktopView::Stereo &&
                    ParseDesktopView("spectator") == DesktopView::Spectator &&
                    ParseDesktopView("combined") == DesktopView::Combined,
                "desktop modes");
    const Fov simple{2.0f, 1.0f, 1.5f, 1.0f};
    const Fov reversed{1.0f, 2.0f, 1.5f, 1.0f};
    const Fov symmetric{1.0f, 1.0f, 1.0f, 1.0f};
    const Fov invalid{std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f, 1.0f};
    for (const float tangent :
         {0.0f, -1.0f, 1e-30f, 101.0f, std::numeric_limits<float>::infinity()}) {
        ok &= Check(!ValidSpectatorFov({1.0f, 1.0f, tangent, 1.0f}),
                    "reject unsafe projection bounds");
    }
    ok &= Check(Near(DesktopViewAspect(DesktopView::Stereo, simple, 0.9375f), 1.875f),
                "stereo keeps pixel aspect");
    ok &= Check(Near(DesktopViewAspect(DesktopView::Spectator, simple, 0.9375f), 0.9375f),
                "default single eye stays unchanged");
    ok &= Check(Near(DesktopViewAspect(DesktopView::Combined, simple, 0.9375f), 1.25f) &&
                    Near(DesktopViewAspect(DesktopView::Combined, reversed, 0.9375f), 1.25f),
                "combined aspect preserves eye proportions with extra peripheral coverage");
    ok &= Check(Near(DesktopViewAspect(DesktopView::Combined, symmetric, 0.9375f),
                     DesktopViewAspect(DesktopView::Spectator, symmetric, 0.9375f)),
                "symmetric combined aspect equals single eye");
    ok &= Check(Near(DesktopViewAspect(DesktopView::Combined, invalid, 0.9375f), 0.9375f),
                "invalid FOV falls back to one eye");
    for (const auto& fov : {simple, reversed, symmetric, invalid}) {
        for (const u32 width : {0u, 1u, 2u, 3u, 101u, 1920u}) {
            ok &= Check(CoversWidth(width, fov), "combined regions cover every pixel exactly once");
        }
    }
    const auto layout = CombinedEyeRegions(100, simple);
    ok &= Check(layout[0].x == 0 && layout[0].width == 75 && layout[1].x == 25 &&
                    layout[1].width == 75 && layout[1].clip_x == 75 && layout[1].clip_width == 25,
                "combined keeps primary eye and right peripheral strip");
    const auto mirror = CombinedEyeRegions(100, reversed);
    ok &= Check(mirror[0].x == 25 && mirror[0].width == 75 && mirror[1].clip_x == 0 &&
                    mirror[1].clip_width == 25,
                "reversed asymmetry keeps left peripheral strip");
    const auto single_fit = SpectatorContentRect(1920, 1080, 0.9375f, false);
    ok &= Check(single_fit.x == 454 && single_fit.y == 0 && single_fit.width == 1012 &&
                    single_fit.height == 1080,
                "uncropped single eye keeps side bars");
    const auto single_crop = SpectatorContentRect(1920, 1080, 0.9375f, true);
    ok &= Check(single_crop.x == 0 && single_crop.y == -484 && single_crop.width == 1920 &&
                    single_crop.height == 2048,
                "single eye fills width with centered vertical crop and no stretching");
    for (const auto& fov : {simple, reversed, symmetric, invalid}) {
        const float aspect = DesktopViewAspect(DesktopView::Combined, fov, 0.9375f);
        const auto crop = SpectatorContentRect(1920, 1080, aspect, true);
        ok &= Check(crop.x == 0 && crop.width == 1920 && crop.height >= 1080 && crop.y <= 0 &&
                        Near(static_cast<float>(crop.width) / crop.height, aspect) &&
                        CoversWidth(crop.width, fov),
                    "combined crop preserves proportions and peripheral coverage");
    }
    const auto combined_crop = SpectatorContentRect(1920, 1080, 1.25f, true);
    const auto combined_layout = CombinedEyeRegions(combined_crop.width, simple);
    ok &= Check(combined_crop.y == -228 && combined_crop.height == 1536 &&
                    Near(static_cast<float>(combined_layout[0].width) / combined_crop.height,
                         0.9375f),
                "combined crop keeps each eye's original proportions");
    const auto portrait = SpectatorContentRect(720, 1280, 1.25f, true);
    ok &= Check(portrait.x == 0 && portrait.y == 352 && portrait.width == 720 &&
                    portrait.height == 576,
                "portrait window preserves both sides");
    const auto odd = SpectatorContentRect(1919, 1079, 0.9375f, true);
    ok &= Check(odd.x == 0 && odd.width == 1919 && odd.height == 2046 && odd.y == -483,
                "odd desktop size has centered crop within one pixel");
    std::puts(ok ? "spectator view checks passed" : "spectator view checks failed");
    return ok ? 0 : 1;
}
