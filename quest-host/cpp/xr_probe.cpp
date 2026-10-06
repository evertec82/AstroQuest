// SPDX-License-Identifier: GPL-2.0-or-later

// What can be learnt about the headset's OpenXR runtime without a session, and so without
// anybody wearing the headset: that the app finds the runtime at all, which of the extensions
// the host relies on it has and, if it lets an instance be made for something that is not an
// activity, what it says about the system and what its performance counters are called.
// The last step runs the host itself for a few seconds: everything it sets up before a session
// begins (session, spaces, swapchains, hand trackers), which is as far as it gets with nobody
// wearing the headset.
// Runs from the SandboxShell instrumentation like the self test.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>

#include "core_process.h"
#include "gl_frames.h"
#include "pad_router.h"
#include "xr_host.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "log.h"

namespace {

/// The extensions the host enables when they are there, and the two it cannot do without.
constexpr const char* Wanted[] = {
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
    XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
    XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
    XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME,
    XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME,
    XR_EXT_HAND_TRACKING_EXTENSION_NAME,
    XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME,
    XR_META_PERFORMANCE_METRICS_EXTENSION_NAME,
};

template <typename Function>
Function GetFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    xrGetInstanceProcAddr(instance, name, &function);
    return reinterpret_cast<Function>(function);
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_com_astrobotquest_vrhost_SandboxShell_nativeXrProbe(
    JNIEnv* env, jclass, jobject context, jstring log_file, jint level) {
    const char* log_path = env->GetStringUTFChars(log_file, nullptr);
    SetLogFile(log_path);
    env->ReleaseStringUTFChars(log_file, log_path);

    std::string report;
    const auto note = [&](const char* format, auto... values) {
        char line[1024];
        std::snprintf(line, sizeof(line), format, values...);
        LOGI("xr probe: %s", line);
        report += line;
        report += '\n';
    };

    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    if (level >= 3) {
        // The host as the app runs it, with no emulator behind it and nobody to show a session
        // to: it sets itself up, waits for a session that does not come, and is told to stop.
        CoreProcess core;
        PadRouter pads;
        StatusImage status;
        XrHostOptions options;
        options.probe = true;
        XrHostStatus host_status;
        std::atomic<bool> quit{false};
        std::atomic<uint32_t> recenter_requests{0};
        std::thread stopper{[&quit] {
            std::this_thread::sleep_for(std::chrono::seconds{5});
            quit = true;
        }};
        RunXrHost(vm, context, core, pads, status, options, host_status, quit,
                  recenter_requests);
        stopper.join();
        note("the host ran and stopped: session %s, refresh rate %.0f Hz",
             host_status.session_running ? "running" : "not running",
             host_status.refresh_rate.load());
        return env->NewStringUTF(report.c_str());
    }
    const auto initialize_loader =
        GetFunction<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (initialize_loader == nullptr) {
        note("%s", "the loader has no xrInitializeLoaderKHR");
        return env->NewStringUTF(report.c_str());
    }
    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = vm;
    loader_info.applicationContext = context;
    const XrResult loader_result = initialize_loader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
    note("loader initialised: %d", static_cast<int>(loader_result));
    if (XR_FAILED(loader_result)) {
        return env->NewStringUTF(report.c_str());
    }

    uint32_t count = 0;
    const XrResult enumerate_result =
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (count != 0) {
        xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
    }
    note("the runtime lists %u extensions (%d)", count, static_cast<int>(enumerate_result));
    std::vector<const char*> enabled;
    for (const char* name : Wanted) {
        const auto found = std::find_if(available.begin(), available.end(), [&](const auto& item) {
            return std::strcmp(item.extensionName, name) == 0;
        });
        if (found != available.end()) {
            note("  %s: version %u", name, found->extensionVersion);
            enabled.push_back(name);
        } else {
            note("  %s: MISSING", name);
        }
    }
    // All of them, for the log only.
    std::string all;
    for (const auto& item : available) {
        all += std::string{all.empty() ? "" : " "} + item.extensionName;
    }
    LOGI("xr probe: extensions: %s", all.c_str());
    if (level < 2) {
        return env->NewStringUTF(report.c_str());
    }

    // An activity is what belongs here; there is none without somebody to show it to. Whether
    // the runtime takes the app's context instead is part of what is being found out.
    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = vm;
    android_info.applicationActivity = context;
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    instance_info.next = &android_info;
    std::strcpy(instance_info.applicationInfo.applicationName, "Astro VR Host");
    instance_info.applicationInfo.applicationVersion = 1;
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    instance_info.enabledExtensionNames = enabled.data();
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult create_result = xrCreateInstance(&instance_info, &instance);
    note("instance created: %d", static_cast<int>(create_result));
    if (XR_FAILED(create_result)) {
        return env->NewStringUTF(report.c_str());
    }

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &properties))) {
        note("runtime: %s %u.%u.%u", properties.runtimeName,
             static_cast<unsigned>(XR_VERSION_MAJOR(properties.runtimeVersion)),
             static_cast<unsigned>(XR_VERSION_MINOR(properties.runtimeVersion)),
             static_cast<unsigned>(XR_VERSION_PATCH(properties.runtimeVersion)));
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    const XrResult system_result = xrGetSystem(instance, &system_info, &system);
    note("system: %d", static_cast<int>(system_result));
    if (XR_SUCCEEDED(system_result)) {
        XrSystemHandTrackingPropertiesEXT hands{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        system_properties.next = &hands;
        if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &system_properties))) {
            note("  %s: swapchain images up to %ux%u, %u layers, hand tracking %d",
                 system_properties.systemName,
                 system_properties.graphicsProperties.maxSwapchainImageWidth,
                 system_properties.graphicsProperties.maxSwapchainImageHeight,
                 system_properties.graphicsProperties.maxLayerCount,
                 static_cast<int>(hands.supportsHandTracking));
        }
        uint32_t view_count = 0;
        XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                         {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count,
                views)) &&
            view_count == 2) {
            note("  an eye: %ux%u recommended, %ux%u at most",
                 views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
                 views[0].maxImageRectWidth, views[0].maxImageRectHeight);
        }
    }

    const auto enumerate_counters = GetFunction<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA>(
        instance, "xrEnumeratePerformanceMetricsCounterPathsMETA");
    uint32_t counter_count = 0;
    if (enumerate_counters != nullptr &&
        XR_SUCCEEDED(enumerate_counters(instance, 0, &counter_count, nullptr))) {
        std::vector<XrPath> paths(counter_count);
        enumerate_counters(instance, counter_count, &counter_count, paths.data());
        std::string names;
        for (uint32_t i = 0; i < counter_count && i < paths.size(); ++i) {
            char name[XR_MAX_PATH_LENGTH]{};
            uint32_t length = 0;
            if (XR_SUCCEEDED(xrPathToString(instance, paths[i], sizeof(name), &length, name))) {
                names += std::string{names.empty() ? "" : " "} + name;
            }
        }
        note("%u performance counters: %s", counter_count, names.c_str());
    }

    xrDestroyInstance(instance);
    note("%s", "instance destroyed");
    return env->NewStringUTF(report.c_str());
}
