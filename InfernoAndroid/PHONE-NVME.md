# 0.2.7: патчер ждёт разделы, NAND остался в режиме firmware update

## Диагностика 0.2.6

Квитанция restored валидна; все девять рабочих дисков сохранили ожидаемые
размеры. Новый patch ramdisk загрузился, restored_external не запущен, паники
Halt/Restart больше нет. Единственная строка демона — waiting for the restore
to finish: этот текст означает ожидание /dev/disk0s1s1 и /dev/disk0s1s2.

Перед запуском launchd драйвер пишет **FW update not complete, create dummy
block device**, modesel=4. Поэтому он ещё не предоставил настоящие разделы
для монтирования. В отдельном режиме остался пропущенным этап инициализации NAND.

Хост дополнительно стоял в accept USB-сокета, хотя отключённый восстановитель
мог вообще не создавать USB-клиент. Конечный Unix accept failed (-22) появился
после ручного STOP/закрытия listener. Это вторичная ошибка остановки, а не
причина ожидания разделов.

## Восстановленный недостающий шаг

В успешном журнале 0.2.5 непосредственно перед появлением настоящего диска:

```text
executing /usr/bin/nvmefwupdater --skip
Setting modesel to 0
```

Команда уже входит в stock ramdisk. Это завершение handshake без обновления
прошивки, без StartRestore, создания разделов или ASR. В имеющемся research
kernel GetRestoreEnvironment проверяет именно **-restore**, а не только
nand-enable-reformat; параметры ядра не менялись. Менять их вслепую было бы
недостаточно: вместо этого выполняется наблюдавшийся штатный шаг --skip.

0.2.7 заменяет ещё одну существующую HFSX-запись — com.apple.KeyMaker.plist
(его исходная программа отсутствует в этом ramdisk) — отдельным launchd job:

- Label: dev.embervm.storage-ready;
- ProgramArguments: /usr/bin/nvmefwupdater, --skip;
- RunAtLoad=true; KeepAlive.SuccessfulExit=false; ThrottleInterval=5;
- stdout/stderr: /dev/console.

При ненулевом выходе job повторяется; при успехе не запускается заново.
restored_external остаётся Disabled=true. Прежний daemon и FS patcher не
перекомпилировались, installed OS/диски/SEP state при подготовке не очищаются.

В patch-only ветке ожидание accept полностью убрано: хост сразу читает консоль.
GUI различает ожидание разделов и обработку кэша. После сообщения о готовности
накопителя старое предупреждение firmware update не считается актуальным.
Если daemon уже ждёт разделы 3 минуты, приложение завершает попытку с явной
ошибкой и сохраняет набор. Этот короткий предел не ограничивает последующую
обработку большого кэша: для неё остаётся прежний общий предел в час.

## Что реально проверено на Linux

1. **67 JVM-тестов**, без failures/errors/skipped; lint без ошибок.
   Дополнены HFSX-проверки storage job/KeepAlive/--skip, отказ без публикации при
   отсутствующем record, разбор ожидания/готовности и граница 180000 мс без
   ложного timeout при работе с кэшем. Пять исходных Swift parity hashes прежние.
2. На имеющемся настоящем HFSX ramdisk оба job прочитаны обратно. Запись
   storage job — 599 байт; пути nvmefwupdater и restored_external существуют.
   Источник и длина IM4P сохранились. Это отдельная проверка формата.
3. **Загружен настоящий restore ramdisk** с обоими GPL guest tools и новыми
   jobs в Linux QEMU/TCG на том же исходном base b76fa575831d94a80db169ffb3c76d9094a648bd
   (Linux binary, не Android APK). 4 ядра, 2 ГиБ RAM для этого небольшого теста.
   Использованы ранее имевшиеся Apple-входы; IPSW/OS image не скачивались.
4. Все диски созданы отдельно, разреженными, в новой scratch-папке. Рабочие
   входные диски не использовались. В начале и конце scratch root записаны
   контрольные данные. USB listener намеренно **не принимает** соединения,
   StartRestore не посылается.
5. После старта guest job контроллер переключился в modesel=0 и создал обычные
   namespace devices. Получены `set_nvme_state: result=0` и **Skipped firmware
   update successfully**, daemon запущен. Первый прогон — 20 с, повтор
   воспроизводимым runner — 12 с. QMP quit завершил QEMU с кодом 0.
6. SHA-256 контрольных данных root до/после совпал:
   49e956f45567dd1ebac9cace73026e46c409799a853ab13e1d8918bc0b6acd57.
   Физический размер sparse root остался 131072 байта. Форматирование/erase
   в этом тесте не произошло.

Это проверка **инициализации накопителя и запуска ramdisk**. Scratch root не
содержит APFS/iOS, поэтому daemon закономерно ждёт отсутствующие тома; тест
не объявляет монтирование восстановленного образа, завершение FS patcher или
первую загрузку iOS успешными. Эти шаги ещё нужно проверить на телефоне.
Android native .so не менялись; версия APK, хеши и проверки — validation-results.json.

## Воспроизведение на своих файлах

После сборки Android-проекта на Linux, с JDK и Gradle cache:

```sh
JAVA_HOME=/path/to/jdk GRADLE_USER_HOME=/path/to/gradle-cache \
  tools/patch-ramdisk-probe/build-with-tools.sh \
  /path/to/stock.im4p /scratch/injected.im4p /scratch/patch-boot.im4p

python3 tools/patch-boot-probe/run.py \
  --qemu /path/to/linux/qemu-system-aarch64 \
  --inputs /path/to/owned-inferno-kit \
  --kernel /path/to/kernelcache.research.iphone12b \
  --ramdisk /scratch/patch-boot.im4p \
  --work /scratch/NEW-empty-probe
```

--work должен отсутствовать и находиться вне --inputs. Runner создаёт только
новые scratch disks и копии пяти входных файлов; собственный root/iOS image
из --inputs не подключается. Результат и команда — в новой папке. Не публиковать
содержащиеся там Apple inputs/ramdisk. В архив обновления входят только исходники,
синтетические тесты и JSON результатов; частных журналов пользователя там нет.

## Следующее действие на телефоне

Установить 0.2.7 поверх 0.2.6 и снова выбрать **Завершить патчи → Запустить
патчи**. Записанная iOS и квитанция подходят для этого. Не выбирать полное
восстановление и не очищать данные приложения. При ошибке сохранить новую
диагностику до следующего повтора; состояние APFS/частично изменённого кэша
остаётся отдельным ограничением, не проверенным этим Linux-тестом.
