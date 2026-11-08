# 0.3.1: настройка служб на System без монтирования Data

Диагностика телефона 0.3.0 подтверждает сразу две исправленные стадии:

```
patch plan check 18A5351d-v031-software matched=45 total=45
existing complete patch set reused; cache not rewritten
unmount /mnt9 says 0 errno=0
mounting /dev/disk0s1s2 on /mnt8
```

Далее нет ни результата mount_apfs, ни five services, ни done. Предыдущая
попытка оставалась там около 10 минут и была перезапущена пользователем;
следующая повторила ожидание. RESTORING в архиве — состояние живой попытки,
не успешное завершение. У System cleanly-unmounted flag, все девять размеров
дисков и квитанция восстановления корректны. Это не полный контроль APFS.

Причина блокировки mount_apfs(Data) не установлена. В restore ramdisk пропущен
boot task keybag; разблокировка тома/SEP — возможная зависимость, но отсутствие
явной ошибки в журнале не доказывает конкретную проблему с ключами. SEP/NVRAM
не очищаются и не подменяются. Увеличение таймаута не считается решением.

## Доступный путь без Data

Для пяти служб нужен Disabled=true в системном кэше launchd, а запись
`/db/com.apple.xpc.launchd/disabled.plist` на Data была альтернативой Swift
daemon, выбранной ради простоты XML. Возвращаем изменение именно System
кэша `/System/Library/xpc/launchd.plist`, описанное в исходном процессе Inferno.
Локальные технические ориентиры: MakrSas/Inferno-iOS PATCHER-HANDOFF.md (five
services) и netlab/agent/install.m (LaunchDaemons cache, SpringBoard guard,
атомарная замена). Существующий boot arg launchd_unsecure_cache=1 сохранён.

0.3.1 вообще не открывает Data для монтирования и не ждёт его device node.
После проверки/применения dyld карты, пока System ещё на /mnt9:

1. Прочитать binary plist, найти LaunchDaemons и ровно по одному job с Label:
   com.apple.voicemail.vmd, com.apple.CommCenter,
   com.apple.CommCenterMobileHelper, com.apple.CommCenterRootHelper,
   com.apple.locationd. Дополнительно требуется единственный SpringBoard.
2. Установить только этим пяти словарям Disabled=true. Оригинальные объекты,
   остальные ключи/значения и ссылки сохраняются; добавляются копии пяти
   словарей и общие новые key/true nodes. Общий false НЕ меняется: другие
   службы остаются включёнными.
3. Сохранить первую исходную копию launchd.plist.ember-original. Записать
   временный файл, fsync, прочитать/проверить, rename на live cache, ещё раз
   прочитать и проверить все пять служб.
4. Только после readback вывести five services switched off, sync, успешно
   отключить System, вывести cleanup complete/done, остановить QEMU и активировать.

Если все пять уже Disabled=true, редактор возвращает исходный файл побайтово,
а daemon не переписывает его. Повтор не затирает первую резервную копию.
При отсутствии cache/target, неверной структуре, unsupported формате или I/O
ошибке успех запрещён; выполняется очистка System и сохраняется диагностика.
Root образ не пересоздаётся, ASR/StartRestore не повторяются.

## Linux implementation и проверки

Новый portable C редактор guest/launchd-cache.c не требует CoreFoundation,
SDK Apple, Mac или jailbreak bootstrap. Поддерживаются bplist00, 1/2/4/8-byte
refs/offsets, extended counts, ASCII/UTF-16 keys, shared objects. Проверяются
границы, контейнерные ссылки, типы/уникальность нужных полей, ASCII/surrogates,
даты в диапазоне годов 1..9999. XML, неизвестные object kinds, исчерпание
текущей ref-width и файлы >32 MiB отклоняются без публикации нового cache.
Это целевой редактор, не универсальный конвертер plist.

Проверено:

- 73 JVM tests, включая System-ready без Data-ready.
- 15 host daemon tests: новый cache, повтор, исходная копия, отсутствие Data
  node, запрет Data mount, ошибки записи/структуры и unmount, строгий cleanup.
- 22 проверки формата с независимым Python plistlib: shared false, UID/data/
  integer/date/Unicode, UTF-16 ключи, extended dict count 14→15, большой cache,
  отсутствие/дубликаты labels, усечения/offsets/ref-width, idempotence и 100
  воспроизводимых мутаций. Тот же набор под AddressSanitizer+UBSan без ошибок.
  Первый fuzz-прогон выявил принятие не-ASCII байта в ASCII object — добавлена
  полная валидация строк и явный regression test.
- В реальном restore ramdisk на Linux QEMU исполняется та же C логика с
  синтетическим launchd cache и маленькой тестовой dyld картой. Два цикла,
  сохранение/проверка binary plist, настоящий tmpfs unmount и чистый QEMU exit.
  Подключены только новые scratch disks, root sentinels сравниваются.
- HFSX rewrite/readback daemon, APK signature/16KiB alignment, неизменные
  Android native libs. Результаты — validation-results.json.

Это ещё не проверка изменения настоящего launchd cache пользователя: его файл
не экспортировался. Также не подтверждены APFS/первый iOS boot на телефоне.
Устранена зависимость стадии патчей от Data mount; доступность Data при обычном
boot может остаться отдельным блокером. Такой случай потребует normal boot log,
не безусловной пометки iOS готова. Bootstrap/bash и новые службы не добавляются.

## Обновление

Остановить текущую попытку, установить APK поверх 0.3.0, сохранить данные.
Нажать **Завершить патчи → Запустить патчи**. Полная подготовка/erase не нужны.
После явного завершения можно впервые проверить обычный запуск iOS и сохранить
диагностику результата. Если получена ошибка cache schema — прислать новую
диагностику; текущий набор/SEP state не удалять.
