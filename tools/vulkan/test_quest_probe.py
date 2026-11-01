"""Run source-owned Quest Vulkan/XR tests under the shared session lock.

The diagnostic APK remains installed; the session always stops that app,
restores proximity automation, and removes its unique temporary test files.
No other apps, tracking settings or Guardian state are changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def run_session(args):
    SERIAL="2G97C5ZJ0G02DQ"
    ADB=str(args.sdk.resolve()/"platform-tools/adb")
    TOOLKIT=Path(__file__).resolve().parents[2]
    BUILD=args.build_dir.resolve()
    APK=args.apk.resolve()
    OUTPUT=BUILD/time.strftime("quest-probe-%Y%m%d-%H%M%S")
    OUTPUT.mkdir(parents=True)
    PACKAGE="org.lindbergh.vulkanprobe"
    run_id=uuid.uuid4().hex
    REMOTE="/data/local/tmp/lindbergh-vulkan-probe-"+str(os.getpid())
    started=False
    proximity=False

    def adb(*args, check=True, timeout=45):
        result=subprocess.run([ADB,"-s",SERIAL,*map(str,args)],capture_output=True,text=True,timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(str(args)+"\n"+result.stdout+result.stderr)
        return result

    try:
        print("Locked Quest session evidence:",OUTPUT,flush=True)
        if adb("get-state").stdout.strip() != "device": raise RuntimeError("Quest is not online")
        metadata={"serial":SERIAL,"apk_sha256":hashlib.sha256(APK.read_bytes()).hexdigest(),
                  "native_sha256":hashlib.sha256((BUILD/"lindbergh_vulkan_selftest").read_bytes()).hexdigest(),
                  "foveation_required":True,"run_id":run_id}
        (OUTPUT/"manifest.json").write_text(json.dumps(metadata,indent=2)+"\n")
        adb("shell","mkdir","-p",REMOTE)
        adb("push",BUILD/"lindbergh_vulkan_selftest",
            BUILD/"legacy-fixture/fixture.lvsp",
            TOOLKIT/"tests/vulkan/legacy.vert",TOOLKIT/"tests/vulkan/legacy.frag",REMOTE+"/")
        result=adb("shell",f"cd {REMOTE} && chmod 755 lindbergh_vulkan_selftest && LV_TEST_SHADER_DIR=. LV_TEST_SOURCE_DIR=. ./lindbergh_vulkan_selftest",check=False)
        (OUTPUT/"core-selftest.log").write_text(result.stdout+result.stderr)
        print("Core selftest exit",result.returncode,flush=True)
        # Keep XR startup independent: a failed pixel case must not hide whether
        # the loader/session/foveation path can be created on this runtime.
        adb("install","-r",APK)
        adb("shell","am","force-stop",PACKAGE)
        proximity=True
        adb("shell","am","broadcast","-a","com.oculus.vrpowermanager.prox_close")
        adb("shell","input","keyevent","KEYCODE_WAKEUP")
        # A unique intent token and fresh PID keep stale logs out of the verdict.
        # Save device wall time for context; it is not the freshness predicate.
        (OUTPUT/"device-time.txt").write_text(adb("shell","date").stdout)
        started=True
        launch=adb("shell","am","start","-n",PACKAGE+"/android.app.NativeActivity","--ez","foveation","true","--es","probe_run",run_id)
        (OUTPUT/"launch.log").write_text(launch.stdout+launch.stderr)
        pid=""
        for _ in range(10):
            pid=adb("shell","pidof",PACKAGE,check=False).stdout.strip()
            if pid:break
            time.sleep(.2)
        if not pid:raise RuntimeError("probe has no live PID after launch")
        (OUTPUT/"pid.txt").write_text(pid+"\n")
        final=""
        screenshot=False
        for _ in range(25):
            raw=adb("logcat","-d","-t","4000","--pid="+pid).stdout
            (OUTPUT/"logcat-raw.txt").write_text(raw)
            marker="START "+run_id+":"
            position=raw.find(marker)
            final=raw[position:] if position >= 0 else ""
            (OUTPUT/"logcat.txt").write_text(final)
            if not screenshot and "FIRST:" in final and "PASS:" not in final and "FAIL:" not in final:
                with (OUTPUT/"screen-during.png").open("wb") as output:
                    subprocess.run([ADB,"-s",SERIAL,"exec-out","screencap","-p"],stdout=output,check=True,timeout=15)
                screenshot=True
            if "LindberghVulkanProbe" in final and ("PASS:" in final or "FAIL:" in final):break
            time.sleep(1)
        # Capture compositor state too; this does not assert the scene is visible.
        (OUTPUT/"activity.txt").write_text(adb("shell","dumpsys","activity","activities",check=False).stdout)
        with (OUTPUT/"screen-after.png").open("wb") as output:
            subprocess.run([ADB,"-s",SERIAL,"exec-out","screencap","-p"],stdout=output,check=True,timeout=15)
        print("\n".join(line for line in final.splitlines() if "LindberghVulkanProbe" in line),flush=True)
        if "PASS: 120 Vulkan OpenXR stereo frames; foveation active" not in final:
            raise RuntimeError("no fresh foveated XR PASS; inspect "+str(OUTPUT))
        if result.returncode:raise RuntimeError("XR passed but core pixel selftest failed; inspect "+str(OUTPUT))
    finally:
        # All cleanup runs before quest3-lock exits. Never touch another app or
        # tracking/Guardian configuration, and never remove the shared lock file.
        cleanup=[]
        if started:cleanup.append(("shell","am","force-stop",PACKAGE))
        if proximity:cleanup.append(("shell","am","broadcast","-a","com.oculus.vrpowermanager.automation_disable"))
        cleanup.append(("shell","rm","-rf",REMOTE))
        failures=[]
        for command in cleanup:
            try:
                response=adb(*command,check=False)
                if response.returncode:failures.append(str(command)+": "+response.stderr)
            except Exception as error:failures.append(str(command)+": "+str(error))
        (OUTPUT/"cleanup.txt").write_text("\n".join(failures) if failures else "All cleanup commands succeeded\n")
        print("Quest cleanup "+("FAILED: "+str(failures) if failures else "complete"),flush=True)
        if failures:raise RuntimeError("Quest session cleanup failed; inspect "+str(OUTPUT))


def main():
    toolkit=Path(__file__).resolve().parents[2]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk",type=Path,default=os.environ.get("ANDROID_HOME",str(Path.home()/"Library/Android/sdk")))
    parser.add_argument("--build-dir",type=Path,required=True)
    parser.add_argument("--apk",type=Path,default=toolkit/"tests/vulkan/android/build/outputs/apk/debug/LindberghVulkanProbe-debug.apk")
    parser.add_argument("--locked-session",action="store_true",help=argparse.SUPPRESS)
    args=parser.parse_args()
    if not args.locked_session:
        wrapper=Path.home()/".local/bin/quest3-lock"
        if not wrapper.is_file(): parser.error("shared quest3-lock wrapper is missing")
        return subprocess.call([sys.executable,str(wrapper),sys.executable,str(Path(__file__).resolve()),
                                *sys.argv[1:],"--locked-session"])
    run_session(args)
    return 0


if __name__=="__main__":
    sys.exit(main())
