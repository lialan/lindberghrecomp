# Android Vulkan/OpenXR diagnostic

This source-owned NativeActivity probe uses the shared direct Vulkan OpenXR
host. It draws a head-locked triangle into a two-layer swapchain for 120
rendered frames. It is not game integration, a world-projection correctness
test or a performance test. Foveation is required by default on Quest; pass
`--ez foveation false` in the launch intent for an emulator without it.
The caller attaches its worker to the JavaVM and retains the Activity through
session teardown. Renderer resources complete before the device is released.

Build the renderer standalone with the Android NDK, `BUILD_TESTING=ON`,
`LINDBERGH_VULKAN_OPENXR=ON`, and `OpenXR_DIR` pointing to the extracted
Android loader AAR. Build target `lindbergh_vulkan_android_xr_probe`. Stage its
`.so` and the loader `.so` in a directory containing `arm64-v8a/`, then build
this standalone Gradle project with `-PnativeDir=/absolute/staging/directory`.
It can use the repository's `quest/gradlew` wrapper with `-p` pointing here.

Runtime success is the `LindberghVulkanProbe` log reporting 120 frames; a built
APK alone does not establish this. Installation/launch must name an explicit
emulator serial or an independently authorized physical device. The current
priority starts with macOS game execution through KosmicKrisp, followed by
standalone integration. The probe has now passed on Quest 3 (Adreno 740):
120 stereo frames, 1680x1760 per eye, two layers, medium foveation (`Fov=2`
in the runtime log). The source-owned native pixel suite also passes there.

The tested runtime returned null density-map images when the application
requested API 1.0.0. Requesting 1.0.34, matching the existing OpenGL host,
resolved enumeration. This is an observed compatibility result, not a claim
that the extension specification requires that patch version.

Tests held `~/.local/bin/quest3-lock` for the entire session, including cleanup.
Fresh PID-filtered logs are under ignored `build-vulkan-android-core/`
artifacts. Both during-session and post-session screencaps in the latest run
are black; the logs do not establish visible XR output. Game rendering, controls and combat
performance remain separate acceptance gates.

Reference requirements: [Khronos density-map image chaining](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainImageFoveationVulkanFB.html)
and [Meta native Vulkan foveation](https://developers.meta.com/vr/documentation/native/android/os-fixed-foveated-rendering/).

The offscreen ARM64 core pixel suite is independent of the Activity probe:

```sh
python3 tools/vulkan/test_android_emulator.py --serial emulator-5580 \
  --sdk /path/to/android-sdk --build-dir /path/to/build-vulkan-android-core
```

That helper accepts emulator serials only and never enumerates devices. Start
the emulator separately. Fixture directory environment overrides in the test
binary allow staging only source-owned assets rather than game data.

The reproducible device runner automatically invokes the shared lock wrapper:

```sh
python3 tools/vulkan/test_quest_probe.py --build-dir /path/to/build-vulkan-android-core \
  --sdk /path/to/android-sdk
```

It records APK/native binary hashes and logs matched to a unique launch token and fresh PID, runs the
pixel tests, installs the separate diagnostic package, and stops only that app
before restoring proximity automation. It never changes tracking or Guardian.
A busy lock returns 75. The APK remains installed; no game APK is replaced.
A screenshot taken during `FIRST` and before `PASS` is named `screen-during.png`;
the post-session image is separately named `screen-after.png`.
