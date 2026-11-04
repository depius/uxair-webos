#!/usr/bin/env python3
"""Package the NDL probe as its own native app, with an H.264 test stream.

    tools/package-ndl-probe.py <test.h264>     (after tools/build.sh)
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = ROOT / '.cache/arm-webos-linux-gnueabi_sdk-buildroot'

def main():
    sample = pathlib.Path(sys.argv[1])
    stage = ROOT / 'build/ndl-probe-app'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    shutil.copy2(ROOT / 'packaging/ndl-probe-appinfo.json', stage / 'appinfo.json')
    shutil.copy2(ROOT / 'packaging/icon.png', stage / 'icon.png')
    shutil.copy2(ROOT / 'packaging/largeIcon.png', stage / 'largeIcon.png')
    shutil.copy2(ROOT / 'build/ndl-probe', stage / 'ndl-probe')
    shutil.copy2(sample, stage / 'test.h264')
    subprocess.run([str(SDK / 'bin/arm-webos-linux-gnueabi-strip'), str(stage / 'ndl-probe')], check=True)
    subprocess.run(['python3', str(ROOT / 'tools/make-ipk.py'), str(stage),
                    str(ROOT / 'dist/com.github.depius.uxair.ndlprobe_0.0.1_arm.ipk')], check=True)

if __name__ == '__main__':
    main()
