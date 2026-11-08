#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""import.py against a fake adb whose /sdcard is a temporary folder and whose
helper is a host build of ember-import. Checks sparse, verified, read-only copy."""
import hashlib, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE_ADB = r'''#!/usr/bin/env python3
import os, shutil, subprocess, sys
root = os.environ["FAKE_SDCARD_ROOT"]
args = sys.argv[1:]
if args and args[0] == "-s": args = args[2:]
def fix(text): return text.replace("/sdcard", root + "/sdcard").replace("/data/local/tmp", root + "/tmp")
if args == ["get-state"]: print("device"); sys.exit(0)
if args[0] == "push":
    dst = fix(args[2]); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copyfile(args[1], dst); sys.exit(0)
if args[0] == "shell":
    cmd = args[1]
    if cmd.startswith("pm path "): print("package:/data/app/x/base.apk"); sys.exit(0)
    if cmd.startswith("am force-stop"): sys.exit(0)
    if cmd.startswith("am start"):
        pkg = cmd.split("-n ")[1].split("/")[0].strip("'")
        os.makedirs(f"{root}/sdcard/Android/data/{pkg}/files", exist_ok=True); sys.exit(0)
    sys.exit(subprocess.run(["sh", "-c", fix(cmd)]).returncode)
sys.exit(1)
'''

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""): h.update(block)
    return h.hexdigest()

def main():
    work = tempfile.mkdtemp(prefix="ember-import-test-")
    try:
        helper = os.path.join(work, "ember-import")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", os.path.join(HERE, "ember-import.c"), "-o", helper], check=True)
        adb = os.path.join(work, "adb"); open(adb, "w").write(FAKE_ADB); os.chmod(adb, 0o755)
        src = os.path.join(work, "sdcard/Android/data/dev.embervm.android/files")
        os.makedirs(src + "/InfernoData/Restore")
        with open(src + "/InfernoData/root", "wb") as f:
            f.truncate(256 << 20); f.seek(100 << 20); f.write(os.urandom(4096))
        for name in ["nvram", "firmware", "Restore/kernelcache.release.iphone12b", "root.part"]:
            open(f"{src}/InfernoData/{name}", "wb").write(os.urandom(70000))
        open(src + "/AppleSEPROM-Cebu-B1", "wb").write(os.urandom(4096))
        open(src + "/guest-console.log", "w").write("not copied")
        before = {p: (sha(os.path.join(src, p)), os.stat(os.path.join(src, p)).st_mtime_ns) for p in
                  ["InfernoData/root", "InfernoData/nvram", "AppleSEPROM-Cebu-B1"]}
        env = dict(os.environ, FAKE_SDCARD_ROOT=work); os.makedirs(work + "/tmp")
        run = lambda *extra: subprocess.run([sys.executable, os.path.join(HERE, "import.py"), "--adb", adb, "--helper", helper, *extra],
                                            env=env, capture_output=True, text=True)
        dry = run("--dry-run"); assert dry.returncode == 0, dry.stdout + dry.stderr
        dst = os.path.join(work, "sdcard/Android/data/dev.embervm.android.gpu/files")
        assert not os.path.exists(dst + "/InfernoData"), "dry run copied files"
        p = run(); assert p.returncode == 0, p.stdout + p.stderr
        for rel in ["InfernoData/root", "InfernoData/nvram", "InfernoData/firmware", "InfernoData/Restore/kernelcache.release.iphone12b", "AppleSEPROM-Cebu-B1"]:
            assert sha(os.path.join(src, rel)) == sha(os.path.join(dst, rel)), rel
        assert not os.path.exists(dst + "/InfernoData/root.part") and not os.path.exists(dst + "/guest-console.log")
        assert os.stat(dst + "/InfernoData/root").st_blocks * 512 < (8 << 20), "root was not copied sparse"
        assert os.path.isfile(dst + "/import-manifest.txt") and not os.path.exists(work + "/tmp/ember-import")
        for rel, (digest, mtime) in before.items():
            assert (sha(os.path.join(src, rel)), os.stat(os.path.join(src, rel)).st_mtime_ns) == (digest, mtime), "source changed: " + rel
        again = run(); assert again.returncode != 0 and "--replace" in again.stderr, "existing files must not be overwritten silently"
        assert run("--replace").returncode == 0
        print("PASS import-from-installed: dry run, sparse verified copy, read-only source, skipped .part/logs, replace guard")
    finally:
        shutil.rmtree(work)

if __name__ == "__main__":
    main()
