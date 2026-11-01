"""Build and run source-owned Vulkan pixel tests on an explicitly selected emulator.

Never discovers devices or selects physical hardware. Start an ARM64 Android
emulator separately, then pass its emulator-NNNN serial. No game data is used.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="emulator-NNNN only")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_HOME"))
    parser.add_argument("--ndk-version", default="27.3.13750724")
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"emulator-[0-9]+", args.serial):
        parser.error("--serial must explicitly name an Android emulator")
    if args.sdk is None:
        parser.error("pass --sdk or set ANDROID_HOME")
    sdk = args.sdk.resolve()
    adb = sdk / "platform-tools" / "adb"
    # Every ADB operation below uses this fixed emulator serial.
    def device(*command):
        run(adb, "-s", args.serial, *command)
    device("get-state")
    toolkit = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    run("cmake", "-S", toolkit / "src/renderer/vulkan", "-B", build, "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(sdk / "ndk" / args.ndk_version / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29", "-DANDROID_STL=c++_static",
        "-DBUILD_TESTING=ON", "-DLINDBERGH_VULKAN_SDL=OFF")
    run("cmake", "--build", build, "--target", "lindbergh_vulkan_selftest", "-j4")
    remote = "/data/local/tmp/lindbergh-vulkan-test"
    device("shell", "mkdir", "-p", remote)
    device("push", build / "lindbergh_vulkan_selftest", build / "legacy-fixture/fixture.lvsp",
           toolkit / "tests/vulkan/legacy.vert", toolkit / "tests/vulkan/legacy.frag", remote + "/")
    device("shell", f"cd {remote} && chmod 755 lindbergh_vulkan_selftest && "
           "LV_TEST_SHADER_DIR=. LV_TEST_SOURCE_DIR=. ./lindbergh_vulkan_selftest")


if __name__ == "__main__":
    main()
