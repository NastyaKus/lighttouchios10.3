# 0.2.9: отсутствующий /sbin/umount в restore ramdisk

Диагностика 0.2.8 с телефона подтверждает:

```
patching .../dyld_shared_cache_arm64e, 1957 MB
the patcher exited with 0
Reverted ...dyld_shared_cache_arm64e
Changes applied successfully.
unmounting /mnt9
umount /mnt9 says 127
ERROR: patch-only boot incomplete
```

Исправление выбора sidecar в 0.2.8 сработало; настоящий FS patcher завершился
с кодом 0. Текущий отказ — следующий шаг, запуск /sbin/umount. Код 127 выдаёт
наш run_to при неудачном execv. В настоящем 18A5351d HFSX catalog проверено:

- /sbin/umount, /bin/umount, /usr/bin/umount — отсутствуют;
- /sbin/mount_apfs и /sbin/mount — присутствуют;
- /bin/sh — отсутствует.

Предыдущий синтетический тест 0.2.8 подставлял fake umount и не проверял это
требование окружения. Он доказывал порядок координации, но пропустил реальную
зависимость. Старый upstream daemon также вызывал отсутствующий executable,
но игнорировал его код; причина его последующего зависания на Data ещё не
установлена и не объявляется исправленной этим изменением.

## Исправление

Daemon вызывает публичный Darwin unmount(path, 0) из гостевой libSystem.
Внешняя программа/оболочка не нужны. MNT_FORCE не применяется. Лог содержит
результат и errno; любая ошибка запрещает done/активацию. Порядок сохраняется:
FS patcher exit 0 → unmount System → mount Data → атомарная запись overrides →
unmount Data → done → остановка QEMU → активация.

Guest binary собран на Linux LLVM 19 из Android NDK 28.2.13676358;
SHA-256 2622cf025395bdf0cddcc0a648514904b373cc8684613ea17fef1375e6c66001.
Подпись ad-hoc, исходники и link interface включены в guest/. QEMU/JNI,
inferno_fs_patcher, диски и SEP state при обновлении APK не заменяются.

## Проверки

- 69 JVM-тестов и 9 host C tests. Последние линкуют test-only wrapper для
  Linux umount2: реальные host mountpoints не отключаются. Проверяются отсутствие
  umount executable, нормальный порядок и запрет done при EBUSY на каждом томе.
- Новый test_no_umount_executable_required запускается на неизменённом daemon
  0.2.8 из 1ad85f9: воспроизводит says 127 / failure. На новом daemon проходит.
- Настоящий HFSX catalog проверен отдельно от mocks. Synthetic guest probe
  дополнен реальными штатными mount_tmpfs /mnt9 и unmount(...,0), а также вызовом для
  отсутствующей точки. Fake cache/mount APFS/patcher остаются fake; проверка
  tmpfs не объявляется проверкой монтирования восстановленного APFS.
- Итоги гостевых запусков, сборки, lint, signing/alignment и SHA —
  validation-results.json. Неудачные прогоны также отмечаются в нём.

Дополнительно внутри гостя получено:

```
EMBER NATIVE UNMOUNT RESULT mount=0 unmount=0 errno=0 missing=-1 missing_errno=2
EMBER DAEMON SYNTHETIC GUEST RESULT first=0 second=0
```

Настоящий временный tmpfs создан штатным mount_tmpfs на /mnt9 и отключён
нашим процессом с ad-hoc подписью через unmount(2).
Вызов на отсутствующей точке вернул ENOENT. После этого выполнены два fake
цикла. В helper для fake циклов unmount перехвачен, а приведённый отдельный
вызов **реальный**. Гость остановлен с QEMU exit 0, контрольные данные scratch
root прежние. Это не проверка APFS.

Неудачные проверки не скрыты: первый guest запуск panic до launchd/начала
программы (причина не установлена); devfs и tmpfs в новой папке под корнем
получили System Policy deny file-mount. Штатная точка /mnt9 разрешена; для неё
описанная проверка прошла. Права на force detach не добавлялись.

На телефоне по-прежнему нужно проверить Data unlock/mount, сохранение overrides,
unmount обоих настоящих APFS-томов, активацию и первый запуск iOS.

## Обновление

Установить APK поверх 0.2.8 с теми же данными, после остановки старой VM.
Затем **Завершить патчи → Запустить патчи**. Не выполнять подготовку или полный
restore заново: квитанция и все девять размеров дисков в полученной диагностике
корректны. Полная целостность APFS этими размерами не доказывается.
Если следующий шаг не завершится, сохранить новую диагностику до повтора.
