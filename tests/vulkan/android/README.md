# Android Vulkan/OpenXR diagnostic

This source-owned NativeActivity probe uses the shared direct Vulkan OpenXR
host. It draws a head-locked triangle into a two-layer swapchain for 120
rendered frames. It is not game integration, a world-projection correctness
test, a performance test, or a foveation test (foveation is disabled here).
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
priority is macOS game execution through KosmicKrisp; this probe has been
compiled and packaged but has not been installed or run.

The offscreen ARM64 core pixel suite is independent of the Activity probe:

```sh
python3 tools/vulkan/test_android_emulator.py --serial emulator-5580 \
  --sdk /path/to/android-sdk --build-dir /path/to/build-vulkan-android-core
```

That helper accepts emulator serials only and never enumerates devices. Start
the emulator separately. Fixture directory environment overrides in the test
binary allow staging only source-owned assets rather than game data.
