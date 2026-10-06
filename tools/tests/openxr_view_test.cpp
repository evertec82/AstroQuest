#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

#include "core/vr/openxr_view.h"

using Core::Vr::ParallelStereoFov;
using Core::Vr::ParallelViewTangents;
using Core::Vr::Vec3;

bool Near(float got, float expected) {
    if (std::abs(got - expected) < 1e-5f) {
        return true;
    }
    std::fprintf(stderr, "Expected %.6f, got %.6f\n", expected, got);
    return false;
}

XrView View(float left, float right, float up, float down, float yaw) {
    return XrView{
        .type = XR_TYPE_VIEW,
        .next = nullptr,
        .pose = {.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)},
                 .position = {}},
        .fov = {std::atan(-left), std::atan(right), std::atan(up), std::atan(-down)},
    };
}

bool CheckView(float yaw) {
    const auto view = ParallelViewTangents(View(0.8f, 1.2f, 1.1f, 0.9f, yaw));
    const float depth =
        std::min(std::cos(yaw) - 0.8f * std::sin(yaw), std::cos(yaw) + 1.2f * std::sin(yaw));
    return view && Near(view->left, std::tan(std::atan(0.8f) + yaw)) &&
           Near(view->right, std::tan(std::atan(1.2f) - yaw)) && Near(view->up, 1.1f / depth) &&
           Near(view->down, 0.9f / depth);
}

bool CheckStereo(float yaw) {
    const auto stereo =
        ParallelStereoFov(View(0.8f, 1.2f, 1.1f, 0.9f, yaw), View(0.7f, 0.9f, 1.3f, 0.6f, -yaw));
    const float left_depth =
        std::min(std::cos(yaw) - 0.8f * std::sin(yaw), std::cos(yaw) + 1.2f * std::sin(yaw));
    const float right_depth =
        std::min(std::cos(yaw) + 0.7f * std::sin(yaw), std::cos(yaw) - 0.9f * std::sin(yaw));
    return stereo && Near(stereo->tan_out, std::tan(std::atan(0.9f) + yaw)) &&
           Near(stereo->tan_in, std::tan(std::atan(1.2f) - yaw)) &&
           Near(stereo->tan_top, std::max(1.1f / left_depth, 1.3f / right_depth)) &&
           Near(stereo->tan_bottom, std::max(0.9f / left_depth, 0.6f / right_depth));
}

int main() {
    const std::array corners{Vec3{-0.8f, 1.1f, -1.0f}, Vec3{1.2f, 1.1f, -1.0f},
                             Vec3{-0.8f, -0.9f, -1.0f}, Vec3{1.2f, -0.9f, -1.0f}};
    const auto parallel = ParallelViewTangents(corners);
    if (!parallel || !Near(parallel->left, 0.8f) || !Near(parallel->right, 1.2f) ||
        !Near(parallel->up, 1.1f) || !Near(parallel->down, 0.9f)) {
        return 1;
    }

    constexpr float cant = 5.0f * 3.14159265358979323846f / 180.0f;
    for (const float yaw : {0.0f, cant, -cant}) {
        if (!CheckView(yaw) || !CheckStereo(yaw)) {
            std::fprintf(stderr, "Failed quaternion or stereo conversion at yaw %.6f\n", yaw);
            return 1;
        }
    }
    const auto canted_ray = [](const Vec3& ray) {
        return Vec3{std::cos(cant) * ray.x + std::sin(cant) * ray.z, ray.y,
                    -std::sin(cant) * ray.x + std::cos(cant) * ray.z};
    };
    const auto canted = ParallelViewTangents({canted_ray(corners[0]), canted_ray(corners[1]),
                                              canted_ray(corners[2]), canted_ray(corners[3])});
    const float depth = std::cos(cant) - 0.8f * std::sin(cant);
    if (!canted || !Near(canted->left, std::tan(std::atan(0.8f) + cant)) ||
        !Near(canted->right, std::tan(std::atan(1.2f) - cant)) || !Near(canted->up, 1.1f / depth) ||
        !Near(canted->down, 0.9f / depth)) {
        return 1;
    }
    const auto mirrored =
        ParallelViewTangents({Vec3{-1.2f, 1.1f, -1.0f}, Vec3{0.8f, 1.1f, -1.0f},
                              Vec3{-1.2f, -0.9f, -1.0f}, Vec3{0.8f, -0.9f, -1.0f}});
    if (!mirrored || !Near(mirrored->left, 1.2f) || !Near(mirrored->right, 0.8f)) {
        return 1;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const Vec3 invalid :
         {Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{nan, 0, -1}, Vec3{0, infinity, -1}}) {
        if (ParallelViewTangents({invalid, corners[1], corners[2], corners[3]})) {
            std::fprintf(stderr, "Accepted an invalid view ray\n");
            return 1;
        }
    }
    const auto valid = View(0.8f, 1.2f, 1.1f, 0.9f, 0.0f);
    for (const XrQuaternionf invalid :
         {XrQuaternionf{}, XrQuaternionf{nan, 0, 0, 1}, XrQuaternionf{0, infinity, 0, 1}}) {
        const XrView view{.type = XR_TYPE_VIEW,
                          .next = nullptr,
                          .pose = {.orientation = invalid, .position = {}},
                          .fov = valid.fov};
        if (ParallelViewTangents(view) || ParallelStereoFov(view, valid) ||
            ParallelStereoFov(valid, view)) {
            std::fprintf(stderr, "Accepted an invalid eye orientation\n");
            return 1;
        }
    }
    const XrView invalid_fov{
        .type = XR_TYPE_VIEW,
        .next = nullptr,
        .pose = valid.pose,
        .fov = {nan, valid.fov.angleRight, valid.fov.angleUp, valid.fov.angleDown}};
    if (ParallelStereoFov(invalid_fov, valid) || ParallelStereoFov(valid, invalid_fov) ||
        ParallelStereoFov(View(0.8f, 1.2f, 1.1f, 0.9f, 2.0f), valid) ||
        ParallelStereoFov(View(20.0f, 1.2f, 1.1f, 0.9f, 0.0f), valid)) {
        std::fprintf(stderr, "Accepted invalid stereo optics\n");
        return 1;
    }
    std::puts("OpenXR quaternion, parallel, opposite-cant and stereo view checks passed");
    return 0;
}
