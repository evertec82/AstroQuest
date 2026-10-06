#include <cmath>
#include <cstdio>
#include <limits>

#include "core/vr/headset_fov_cache.h"

using Core::Vr::CachedHeadsetFov;
using Core::Vr::HeadsetIdentity;
using nlohmann::json;

json Cache(const HeadsetIdentity& identity, const json& tangents) {
    return json{{"runtime", identity.runtime},
                {"system", identity.system},
                {"vendor_id", identity.vendor_id},
                {"fov_tan", tangents}};
}

int main() {
    const HeadsetIdentity index{"SteamVR 2.17.10", "Valve Index", 0x28de};
    const HeadsetIdentity quest{"VirtualDesktopXR 1.0.0", "Quest 3", 0x2833};
    const json tangents{0.8f, 1.2f, 1.1f, 0.9f};
    const auto cache = Cache(index, tangents);
    const auto saved = CachedHeadsetFov(json::parse(cache.dump()), index);
    if (!saved || std::abs(saved->tan_out - 0.8f) > 1e-6f ||
        std::abs(saved->tan_in - 1.2f) > 1e-6f || std::abs(saved->tan_top - 1.1f) > 1e-6f ||
        std::abs(saved->tan_bottom - 0.9f) > 1e-6f) {
        std::fprintf(stderr, "Matching headset cache did not round-trip\n");
        return 1;
    }
    for (const HeadsetIdentity& other :
         {quest, HeadsetIdentity{quest.runtime, index.system, index.vendor_id},
          HeadsetIdentity{index.runtime, quest.system, index.vendor_id},
          HeadsetIdentity{index.runtime, index.system, quest.vendor_id},
          HeadsetIdentity{"SteamVR 2.18.0", index.system, index.vendor_id}, HeadsetIdentity{},
          HeadsetIdentity{index.runtime, "", index.vendor_id},
          HeadsetIdentity{"", index.system, index.vendor_id}}) {
        if (CachedHeadsetFov(cache, other)) {
            std::fprintf(stderr, "Accepted a cache for a different or unknown headset\n");
            return 1;
        }
    }
    if (CachedHeadsetFov(Cache(quest, tangents), index) ||
        CachedHeadsetFov(Cache({}, tangents), {})) {
        std::fprintf(stderr, "Accepted a cross-headset or unidentified cache\n");
        return 1;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const json& invalid : {json{}, json::object(), json{0.8f, 1.2f, 1.1f},
                                json{0.8f, 1.2f, 1.1f, 0.9f, 1.0f}, json{"0.8", 1.2f, 1.1f, 0.9f},
                                json{nullptr, 1.2f, 1.1f, 0.9f}, json{0.0f, 1.2f, 1.1f, 0.9f},
                                json{0.8f, -1.2f, 1.1f, 0.9f}, json{0.8f, 1.2f, 11.0f, 0.9f},
                                json{0.8f, 1.2f, 1.1f, nan}, json{infinity, 1.2f, 1.1f, 0.9f}}) {
        if (CachedHeadsetFov(Cache(index, invalid), index)) {
            std::fprintf(stderr, "Accepted invalid cached tangents\n");
            return 1;
        }
    }
    for (const json& invalid :
         {json{}, json::array(), json{{"fov_tan", tangents}},
          json{{"runtime", index.runtime}, {"system", index.system}, {"fov_tan", tangents}},
          json{
              {"runtime", index.runtime}, {"system", index.system}, {"vendor_id", index.vendor_id}},
          json::parse("{broken", nullptr, false)}) {
        if (CachedHeadsetFov(invalid, index)) {
            std::fprintf(stderr, "Accepted a legacy, incomplete or malformed cache\n");
            return 1;
        }
    }
    std::puts("Headset FOV cache identity and validation checks passed");
    return 0;
}
