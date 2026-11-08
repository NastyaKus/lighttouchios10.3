#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy a prepared iOS set from the working Ember VM into the side-by-side
test build, over adb, without touching the working copy.

    python3 import.py                 # dev.embervm.android -> dev.embervm.android.gpu
    python3 import.py --dry-run       # show what would be copied, change nothing
    python3 import.py --replace       # allow overwriting files the test build already has

Android 11+ hides another app's Android/data folder from the file picker, so
the in-app import cannot reach it; adb shell can. Every file is copied by
ember-import (pushed to /data/local/tmp for the run and removed afterwards):
zero blocks are skipped so a sparse raw root stays small, and each file is
renamed into place only after its read-back SHA-256 matches the source.
The source app's files are only ever opened read-only.
"""
import argparse
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE_HELPER = "/data/local/tmp/ember-import"
ITEMS = ["InfernoData", "AppleSEPROM-Cebu-B1"]
SKIP_SUFFIXES = (".part",)
MAIN_ACTIVITY = "dev.embervm.android.MainActivity"


class Adb:
    def __init__(self, adb, serial):
        self.base = [adb] + (["-s", serial] if serial else [])

    def run(self, *args, check=True):
        p = subprocess.run(self.base + list(args), capture_output=True, text=True)
        if check and p.returncode != 0:
            raise SystemExit(f"adb {' '.join(args)} завершился с кодом {p.returncode}:\n{p.stderr.strip() or p.stdout.strip()}")
        return p

    def shell(self, command, check=True):
        return self.run("shell", command, check=check)


def q(path):
    return shlex.quote(path)


def human(n):
    for unit in ("Б", "КиБ", "МиБ", "ГиБ", "ТиБ"):
        if n < 1024 or unit == "ТиБ":
            return f"{n:.1f} {unit}" if unit != "Б" else f"{n} {unit}"
        n /= 1024


def files_dir(package):
    return f"/sdcard/Android/data/{package}/files"


def installed(adb, package):
    return adb.shell(f"pm path {q(package)}", check=False).stdout.strip().startswith("package:")


def list_source(adb, root):
    present = [item for item in ITEMS if adb.shell(f"test -e {q(root + '/' + item)} && echo yes", check=False).stdout.strip() == "yes"]
    if not present:
        return []
    paths = " ".join(q(item) for item in present)
    out = adb.shell(f"cd {q(root)} && find {paths} -type f -exec stat -c '%s %b %B %n' {{}} +").stdout
    entries = []
    for line in out.splitlines():
        size, blocks, block_size, name = line.split(" ", 3)
        if name.endswith(SKIP_SUFFIXES):
            continue
        entries.append((name, int(size), int(blocks) * int(block_size)))
    return sorted(entries)


def available_bytes(adb, path):
    out = adb.shell(f"df -k {q(path)}").stdout.strip().splitlines()
    fields = out[-1].split()
    return int(fields[3]) * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--adb", default=os.environ.get("ADB", "adb"))
    ap.add_argument("-s", "--serial", help="серийный номер телефона, если подключено несколько")
    ap.add_argument("--from", dest="source", default="dev.embervm.android")
    ap.add_argument("--to", dest="target", default="dev.embervm.android.gpu")
    ap.add_argument("--helper", default=os.path.join(HERE, "ember-import-arm64"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--replace", action="store_true", help="перезаписывать уже имеющиеся файлы тестовой сборки")
    args = ap.parse_args()

    if args.source == args.target:
        raise SystemExit("--from и --to совпадают")
    adb = Adb(args.adb, args.serial)
    state = adb.run("get-state", check=False)
    if state.returncode != 0 or state.stdout.strip() != "device":
        raise SystemExit("Телефон не виден по adb. Включи отладку по USB, подключи телефон и подтверди доступ.")
    for package in (args.source, args.target):
        if not installed(adb, package):
            raise SystemExit(f"Не установлено приложение {package}.")
    if not os.path.isfile(args.helper):
        raise SystemExit(f"Нет помощника {args.helper}; собери его: tools/import-from-installed/build.sh")
    with open(args.helper, "rb") as f:
        helper_hash = hashlib.sha256(f.read()).hexdigest()

    src_root, dst_root = files_dir(args.source), files_dir(args.target)
    print(f"Останавливаю {args.source} и {args.target}…")
    for package in (args.source, args.target):
        adb.shell(f"am force-stop {q(package)}")
    if adb.shell(f"ls {q(src_root)}", check=False).returncode != 0:
        raise SystemExit(f"Нет доступа к {src_root}. На этой версии Android adb может не видеть Android/data;\n"
                         "тогда скопируй InfernoData и AppleSEPROM-Cebu-B1 с компьютера и выбери «Импорт → Папка».")
    if adb.shell(f"test -d {q(dst_root)}", check=False).returncode != 0:
        # The app creates its own external files folder the first time it starts.
        adb.shell(f"am start -W -n {q(args.target + '/' + MAIN_ACTIVITY)}")
        time.sleep(2)
        adb.shell(f"am force-stop {q(args.target)}")
        if adb.shell(f"test -d {q(dst_root)}", check=False).returncode != 0:
            raise SystemExit(f"Тестовая сборка не создала {dst_root}. Открой её один раз вручную и повтори.")

    entries = list_source(adb, src_root)
    if not entries:
        raise SystemExit(f"В {src_root} нет ни InfernoData, ни AppleSEPROM-Cebu-B1.")
    existing = {name for name, _, _ in list_source(adb, dst_root)}
    clash = [name for name, _, _ in entries if name in existing]
    logical = sum(size for _, size, _ in entries)
    physical = sum(used for _, _, used in entries)
    free = available_bytes(adb, dst_root)
    print(f"Файлов: {len(entries)}, логически {human(logical)}, реально занято {human(physical)}; свободно {human(free)}.")
    for name, size, used in entries:
        mark = "  (уже есть)" if name in existing else ""
        print(f"  {name}  {human(size)}  [{human(used)}]{mark}")
    if clash and not args.replace:
        raise SystemExit(f"В тестовой сборке уже есть {len(clash)} из этих файлов. Повтори с --replace, чтобы перезаписать их.")
    reserve = 1 << 30
    if physical + reserve > free:
        raise SystemExit(f"Не хватает места: нужно {human(physical + reserve)} вместе с запасом 1 ГиБ.")
    if args.dry_run:
        print("--dry-run: ничего не скопировано.")
        return

    adb.run("push", args.helper, REMOTE_HELPER)
    adb.shell(f"chmod 755 {REMOTE_HELPER}")
    remote_hash = adb.shell(f"sha256sum {REMOTE_HELPER}").stdout.split()[0]
    if remote_hash != helper_hash:
        adb.shell(f"rm -f {REMOTE_HELPER}", check=False)
        raise SystemExit("Помощник на телефоне не совпал с локальным по SHA-256.")

    manifest = [f"source={args.source}", f"target={args.target}", f"time={time.strftime('%Y-%m-%dT%H:%M:%S%z')}",
                f"helper_sha256={helper_hash}"]
    failed = []
    try:
        for i, (name, size, used) in enumerate(entries, 1):
            src, dst = f"{src_root}/{name}", f"{dst_root}/{name}"
            print(f"[{i}/{len(entries)}] {name} ({human(size)})…", flush=True)
            adb.shell(f"mkdir -p {q(os.path.dirname(dst))}")
            started = time.time()
            out = adb.shell(f"{REMOTE_HELPER} copy {q(src)} {q(dst)}", check=False).stdout.strip().splitlines()
            line = out[-1] if out else "FAIL adb 0 no output"
            if not line.startswith("OK "):
                print(f"    ОШИБКА: {line}")
                failed.append(name)
                continue
            _, copied, written, digest = line.split()
            if int(copied) != size:
                print(f"    ОШИБКА: размер {copied} вместо {size}")
                failed.append(name)
                continue
            print(f"    готово за {time.time() - started:.0f} с, записано {human(int(written))}, SHA-256 {digest[:16]}…")
            manifest.append(f"{digest}  {size}  {name}")
    finally:
        adb.shell(f"rm -f {REMOTE_HELPER}", check=False)

    with tempfile.NamedTemporaryFile("w", delete=False, suffix=".txt") as f:
        f.write("\n".join(manifest) + "\n")
        local_manifest = f.name
    try:
        adb.run("push", local_manifest, f"{dst_root}/import-manifest.txt")
    finally:
        os.unlink(local_manifest)
    if failed:
        raise SystemExit(f"Не скопировано: {len(failed)} — {', '.join(failed)}. Рабочая копия {args.source} не изменена; "
                         "можно просто повторить с --replace.")
    print(f"Готово. Открой «Ember GPU (тест)» → «Файлы образа»: там должно быть «Всё на месте».\n"
          f"Рабочая копия в {args.source} не изменялась.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("Прервано. Недописанные файлы остаются только как *.part в тестовой сборке.")
