# Проверка Ember VM на Linux

Дата обновления: 2026-11-05 (UTC). База Android: `d453a1b4563dffd8d90f9a43b86ab45b15580e8d`.
Ветка изменений: локальная `codex/inferno-android-restore`.

## Среда и границы результата

Debian 13 x86_64, 2 CPU, лимит RAM 8 ГиБ, рабочий накопитель 7.9 ГиБ.
SDK platform/build-tools 35, NDK 28.2.13676358, CMake 3.22.1, Gradle 8.11.1,
Kotlin 2.1.20 / AGP 8.9.2. Для сборки установлен полноценный OpenJDK 21
(целевой bytecode Java/Kotlin 17). Для сравнения — Swift 6.0.3 Ubuntu 24.04.

Доступен Linux build/JVM стенд. Подключённых Android-устройств нет (`adb devices -l`
вернул пустой список); `/dev/kvm` отсутствует. Android UI, SELinux/Unix socket,
физическая установка APK и реальная эмуляция на телефоне здесь не проверены.
Ни настоящий IPSW, ни SEP ROM, ни ticket/key в рамках этой разработки не скачивались.
Реальное восстановление iOS не запускалось.

## Выполнено

- Оригинальные `engine/build-deps.sh` и `build-engine.sh` успешно собрали
  Android arm64 engine `b76fa575831d94a80db169ffb3c76d9094a648bd` (~14 МиБ).
  Применены только исходные две Android patches из базовой ветки. В diff нет
  изменений `InfernoAndroid/engine/`.
- Получен Inferno-iOS v0.3.1 IPA (9 552 246 байт). SHA-256 совпал с заданным:
  `c66b56de80d55b989df1c70ebcf4b321751a6703a9d253c9ff349f434fd7c8dc`.
  В assets включены только два guest executable и provenance.
- Kotlin/JVM тесты и Android lint выполняются командой:

  ```sh
  ./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
  ```

  Итоговый результат и число тестов зафиксированы в поставляемом
  `validation-results.json`; XML JUnit и lint report приложены в архиве проверки.
  Lint warnings о новых версиях зависимостей, sparse-storage API и ресурсах
  не означают ошибку восстановления. Ошибки lint не подавлялись baseline.
- ZIP64 дополнительно проверяется на разреженном **синтетическом** архиве с
  локальным header и central directory за отметкой 4 ГиБ. Большой IPSW не нужен;
  физически записываются только несколько сотен байт.
- Независимая byte-level fake USB модель проверяет перечисление конфигурации,
  VERSION/setup, TCP handshake, узкое окно 256 байт и широкое окно, 16 КиБ MTU,
  ASYNC promise, short OUT, несоответствие transfer ID, sequence gap и NODEV до
  появления гостя. Тест не исполняет JNI и не даёт замеров реального ASR.

## Побайтовое сравнение с неизменённым Swift

Команда:

```sh
tools/swift-parity/run.sh /path/to/Inferno-iOS /path/to/swiftc
```

Оригинал строго на `4bc39702871f5a118d96acbad4df7a675ddf14e0`.
Обе реализации работают на одних синтетических ticket/manifest/IM4P/HFSX;
`cmp` для каждого результата успешен:

| Файл | SHA-256 |
|---|---|
| ap.der | `41e1a02dce1b335ca756533cf3a9b302514ca459f14983644fd18b90c5b1e5a1` |
| sep.der | `db98693716247e96140c22f7832d722e527fa216534d31eb94ad92d0613140d8` |
| personalized.img4 | `ab35806478728a2b686ea45729d5c5d41bf09f0fae5217194f5431ef9b91d260` |
| job.plist | `d518f06b3882c05dd165b67ce6a4fa82c6bba18115d2641af842c7d72164e601` |
| ramdisk.im4p | `6157498e4218afab5b5a4125b0584d84a77708fe78536a1351b5e27f6bd71f52` |

Это проверка преобразований, а не проверка совместимости реального Apple ramdisk.
Пустой диск/синтетический ticket из тестов нельзя использовать как гостевой комплект.

## Конкретные ошибки и исправления

| Наблюдение | Решение / состояние |
|---|---|
| `Toolchain installation ... does not provide [JAVA_COMPILER]` | В стенде был только JRE. Установлен JDK; `java.home` исправлен на task-local JDK |
| Gradle download timeouts при прямом HTTPS | Gradle настроен на предоставленный HTTPS proxy; TLS verification сохранена |
| CMake не загрузил `librhash` | Выбран SDK CMake 3.22.1 первым в PATH, dependencies/engine успешно собраны |
| Gradle Kotlin DSL `Unresolved reference: nio` | Явные imports Files/StandardCopyOption вместо перекрытого `java` accessor |
| Kotlin 2.1 `break continue in inline lambdas ... since 2.2` | Убран continue из inline lambda |
| ASR fragmented XML: `PI must not start with xml ... @2:5` | Не вся завершающая новая строка приходит с `</plist>`; XmlStream убирает leading whitespace перед следующим XML declaration. Тест 1/7-byte fragments и coalesced documents |
| Lint `VibratorManager#getDefaultVibrator requires API 31` | Сохранён minSdk 29, добавлен fallback Vibrator для Android 10–11 |
| Lint `StateFlow.value should not be called within composition` | Используется collectAsState для импорта |
| Статический аудит: daemon печатает `done` даже при nonzero exit | Успех требует exit 0, five-services marker и done; проверено тестами |
| Статический аудит: USB уходит после final status | На стадии патчей читается только консоль, mux больше не обслуживается |
| NODEV/STALL до старта гостевого USB | Перечисление повторяется на том же соединении; timeout не означает успех |
| GitHub create_branch: HTTP **403**, `Resource not accessible by integration` | Подключённая GitHub-интеграция не имеет права создать ref. Push не выполнен. Изменения сохранены локально; поставляются source ZIP и git-format-patch |

Полный restore/first boot остаётся блокером до испытания на arm64 Android с
правильными пользовательскими файлами. Точная последовательность и этапы
диагностики — в [RESTORE.md](RESTORE.md). Успешная сборка не подтверждает запуск iOS.

## Обновление 0.2.1 после сообщения с телефона

39 JVM tests прошли, Android lint — без ошибок. Добавлены пять регрессионных
тестов live/stale lease, реального завершения дочернего процесса, архивирования
логов и отказа при недостающем входном файле до очистки диска. APK подписан тем
же сертификатом, что 0.2.0, и имеет versionCode 3. Engine и JNI .so побайтово
идентичны файлам из ранее выданной APK. Исправлена Kotlin-обработка попыток;
отдельно добавлена диагностика Android и сохранение исключений.

Первоначальная причина исчезновения процесса на телефоне не установлена по
двум скриншотам. Вторичная ошибка stale-marker воспроизводится по коду старой
версии и исправлена. Полный restore и первый boot на телефоне не объявляются
успешными. Диагностический экспорт через Android SAF проверен сборкой/lint;
его выполнение и ApplicationExitInfo требуется проверить на телефоне.

## 0.2.3 — исправление локализованного ARM64 context switch

Новый tombstone показал longjmp+460/autiasp (d50323bf) →
qemu_coroutine_switch → qio_channel_readv_full_all_eof →
usb_inferno_host_msg_loop_co → coroutine_bootstrap/trampoline.
Добавлен Android/AArch64-only patch из подхода AlexZorzi/Inferno-Android,
с private symbol names и корректными compiler attributes.

Собран обновлённый engine на прежнем pinned commit; JNI не изменён.
Дизассемблирование подтвердило, что qemu_coroutine_switch вызывает приватный
контекстный примитив вместо bionic sigsetjmp/siglongjmp. Повторный запуск
engine/build-engine.sh не пере-применяет patch и не сбрасывает исходники.
42 JVM tests и lint без ошибок, APK подпись совпадает с предыдущими версиями,
zipalign 16 KiB проходит. Отдельный ARM64 ELF regression прошёл на cortex-a57
и max: регистры/FP/SP, текущий x18, 0→1, 100000 переключений стеков на каждом.
C harness собран с branch protection; на поддерживающем CPU включён PAC.

Это испытание примитива и конкретная правка наблюдавшегося пути падения.
Повтор восстановления на реальном Android и полный restore/first boot
остаются следующей проверкой. Частные логи/tombstone телефона не публикуются.

## 0.2.4 — приём usbmux после VERSION

Диагностика 0.2.3 с телефона подтверждает прохождение USB SETUP и согласование
usbmux v2; прежний SIGILL в этой попытке не повторился. Следующая остановка —
управляемое `IllegalArgumentException: Mux magic` до подключения restored.
Убраны отсутствующие в Swift проверки host magic и непрерывности mux-номеров
у входящих пакетов. TCP byte sequence/окно и USB identity/length проверяются.
Добавлен отдельный ограниченный trace заголовков; точные байты прежнего
проблемного заголовка из старой диагностики восстановить нельзя.

Регрессионные тесты покрывают отличающиеся reserved word, повтор/пропуск/wrap
mux-счётчиков, отдельное подавление TCP duplicate, split/coalesced mux и
VERSION→v2 transition, размер кадра больше USB MTU и отказ при невозможной
длине. Подробности и ограничения — [PHONE-MUX.md](PHONE-MUX.md).
Engine и JNI в APK сверяются побайтово с 0.2.3. Результаты сборки и проверок
зафиксированы в validation-results.json. Испытание новой версии на телефоне,
полный restore и первый boot iOS остаются непроверенными.

## 0.2.5 — OUT NAK, ответы гостя и выборочный mux replay

На телефоне 0.2.4 согласовала usbmux и запустила restored/ASR. Последний
guest progress — 68%, затем endpoint 0x81 TRB allocation/IO timeout и
управляемая ошибка хоста `USB OUT NAK 60s`. Входящий reserved word реально
равен 0xfaceface. Исправление 0.2.4 прошло ранее проблемный этап; завершение
восстановления этим не подтверждается.

0.2.5 учитывает cumulative ACK host mux-пакетов, повторяет неподтверждённый
остаток с чтением IN между отправками и обслуживает IN при свободном TCP-окне.
Стресс-модель с потерей кадра и очередью из двух записей выявила дополнительную
проблему backlog; она исправлена в том же изменении. Проверяются wrap 65535→0,
старые/невозможные TCP ACK, NAK с входящими данными и сохранённый предел 60 с.
Подробности и ограничения — [PHONE-NAK.md](PHONE-NAK.md). Native engine/JNI
сверяются с предыдущим APK; на Android новое поведение ещё предстоит проверить.

Финально: 55 JVM-тестов, 0 failures/errors/skipped; lint 0 ошибок / 23 warning.
Три новых pressure/replay-теста отдельно запущены на исходном GuestUsb.kt
0.2.4 с адаптером clock/constructor: все три воспроизводят ожидаемые отказы.
Воспроизводящий runner — tools/usb-replay-regression/run.py; JSON и stdout
включены в архив проверки. Сертификат обновления совпал, zipalign 16 KiB
прошёл, все native .so идентичны 0.2.4. Android restore 0.2.5 ещё не испытан.

## 0.2.6 — отдельный запуск патчей после успешного restored

Получена диагностика реального 0.2.5: Status 0/ACK и гостевые SUCCESS подтверждены.
Патчер начал обработку 1957-МиБ кэша, но restored_external вызвал /sbin/reboot,
launchd начал shutdown и ядро запаниковало с `Halt/Restart Timed Out`.
Конечный InterruptedException был следствием ручной отмены ожидания.

Теперь полный restore запускает stock ramdisk и останавливается после Status 0.
Патчи запускаются отдельной кнопкой в свежем :vm; диски сохраняются, job
restored_external в отдельной копии ramdisk отключён, StartRestore не вызывается.
До начала доступны проверка/миграция квитанции, после — прежние строгие условия
успеха. Kernel panic немедленно прерывает ожидание патчера.

64 JVM-теста прошли; прежние пять Swift parity hashes сохранились.
Производственный rewrite отдельно прошёл на имевшемся реальном HFSX ramdisk:
compressed job заменён 418-байтовым XML, прочитан обратно, исходный файл и длина
IM4P сохранены. Это проверка формата, не гостевая загрузка. Native engine/JNI
не менялись. Полный отчёт и границы — [PHONE-REBOOT.md](PHONE-REBOOT.md).

## 0.2.7 — NAND --skip и независимость patch-only от USB

Диагностика 0.2.6 показала валидную квитанцию/диски, загрузку patch ramdisk и
FW update not complete / dummy block device. Daemon ждёт разделы; хост остался
в USB accept и получил -22 при ручном STOP. В успешном 0.2.5 restore недостающий
шаг выполнялся командой /usr/bin/nvmefwupdater --skip.

В новом patch ramdisk добавлен отдельный job этой штатной команды, с повтором
только при неуспехе. Восстановитель остаётся выключенным. Хост сразу наблюдает
консоль и не ждёт USB; ожидание разделов ограничено 3 минутами отдельно от кэша.

67 JVM-тестов прошли. Реальный HFSX rewrite проверен и прочитан обратно.
Кроме этого, Linux QEMU на base b76fa575... **действительно загрузил ramdisk**:
новый job перевёл NVMe в modesel=0 и сообщил Skipped firmware update successfully
без USB accept/StartRestore. Контрольные блоки отдельного scratch root сохранились,
QEMU штатно вышел с кодом 0. Два прогона — 20 и 12 с; scratch root не содержит
APFS/iOS. Монтирование пользовательского APFS и FS patcher на Android остаются
непроверенными. Подробности, runner и ограничения — [PHONE-NVME.md](PHONE-NVME.md).

## 0.2.8: точный путь кэша и обновление daemon

См. [PHONE-CACHE.md](PHONE-CACHE.md). APK собран, 69 JVM-тестов и
8 host-тестов C daemon прошли; lint без ошибок. Проверены подпись обновления,
16 KiB ZIP alignment и совпадение всех Android native .so с 0.2.7.
На настоящем HFSX ramdisk проверены байты заменённого daemon, оба job, длина
IM4P и неизменность источника. Старт нового arm64 daemon в настоящем ramdisk
наблюдался в Linux QEMU/TCG; это не доказательство патчей реального APFS.
Результаты дополнительных probes и их границы — validation-results.json.

## 0.2.9: native unmount

См. [PHONE-UNMOUNT.md](PHONE-UNMOUNT.md). Старый baseline воспроизводит
отсутствие umount executable; 9 host tests проверяют исправление и EBUSY.
Настоящий вызов Darwin unmount проверяется отдельно на временном tmpfs на /mnt9
в scratch guest; синтетический цикл не подменяет проверку APFS на телефоне.
Конкретные результаты запусков — validation-results.json.

## 0.3.0: повтор/cleanup

72 JVM и 13 host C tests; полная production карта проверяется на sparse файле,
а guest probe использует явно синтетическую мини-карту. Проверяются запрет
успеха при несовпадении одного участка и задержка обработки child failure до
cleanup. См. PHONE-REPEAT.md и validation-results.json.

## 0.3.1: System launchd cache

73 JVM, 15 daemon и 22 binary-plist проверки (также ASan+UBSan).
Новый guest probe меняет синтетический launchd cache в настоящем ramdisk,
без Data mount. Это не проверка пользовательского cache или нормального boot.
См. PHONE-DATA.md и validation-results.json для результата и ограничений.
