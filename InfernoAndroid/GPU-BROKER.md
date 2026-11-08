# Ember VM 0.7.0-gpubroker

Добавлен гостевой IPC-брокер: отдельный клиент `mobile` передаёт GLES-команды
через Unix stream, а raw-диск диагностического канала открывает только root
брокер. Это следующий проверяемый этап command bridge. Обычные приложения iOS,
SpringBoard, Metal и GarageBand пока не подключены к этому пути.

## Проверка на телефоне

1. Остановить VM. Установить APK **поверх** Ember VM, без удаления приложения.
   Восстанавливать или заново готовить iOS не нужно.
2. Открыть GPU-стенд и нажать **«Проверить GPU через брокер»** над предпросмотром.
3. Держать экран открытым до PASSED/FAILED. После загрузки тестового гостя
   должны появиться цветные области и движущийся треугольник.
4. Сохранить «Диагностику ZIP» и передать её для проверки. Нужен именно новый
   запуск: прежний успешный Surface-тест не проверял брокер и права клиента.

Рабочий iOS-диск в тестовую машину не подключается. Требуется 512 МиБ свободного
места для временного комплекта. Старые кнопки offscreen и прямого Surface-теста
сохранены. Измерение FPS и сравнение скорости iOS этот тест не выполняет.

PASSED требует одновременно:

- uid/gid 501 у клиента, отказ открытия фактического raw-устройства на чтение
  и запись с EPERM/EACCES;
- аутентификацию клиента брокером, совпадение количества переданных команд
  со счётчиком Android GPU-хоста, exit 0 клиента и завершение очистки брокера;
- прежние 40 GLES checks, EAGL/TLS checks, 191 present check и 60 window presents;
- успешный PixelCopy четырёх контрольных точек кадра того же запуска и поколения
  Surface. Это проверка одного захваченного кадра, а не каждого пикселя анимации;
- остановку QEMU с кодом 0 и сохранность контрольных областей временного root.

Отчёт дополнен `brokerRequested`, `brokerIsolationVerified` и явным
`appStoreSandboxVerified=false`. Консоль брокера остаётся в guest-console.log.

## Что проверено локально

6 ноября 2026 года фактический iOS 14 release kernel 18A5351d прошёл новый режим
на Linux QEMU: клиент uid/gid 501 без entitlements получил EPERM на оба открытия
`/dev/rdisk2`, брокер подтвердил peer uid/gid, выполнил 316 команд и дождался exit 0
и очистки. Выполнены 1262 API calls, 85 batches, 64 draws и 7 shader compiler
invocations, включая намеренно ошибочные шейдеры. 60 presents проверили 240
sample pixels; host здесь Mesa llvmpipe, `window_frames=0`. Финальный прогон занял
48 секунд, включая загрузку и намеренные задержки анимации: это не benchmark.
QEMU exit 0, sparse root занимал 131072 физических байта, его контрольные области
совпали. Восстановление системы и APFS-патчи этим тестом не запускались.

100 JVM tests, native tests с ASan/UBSan, полный Linux broker roundtrip с
санитайзерами, тот же путь через настоящий JVM/JNI wrapper и 12 monitor tests
прошли. Старые direct offscreen/present/legacy Linux roundtrips также прошли.
Все шесть пересобранных guest binaries воспроизведены байт в байт. Android
native libraries, QEMU engine, guest filesystem patcher/daemon и QEMU data
совпадают с 0.6.0. Подпись обновления сохранена; APK выровнен для 16-КиБ страниц.

Новый брокер на физическом Android ещё не проверен. Предыдущий присланный прогон
0.6.0 подтвердил Surface/PixelCopy на Adreno 740, но не автоматически новый IPC
режим. Подробные результаты и SHA-256 находятся в GPU-BROKER-VALIDATION.json.

## Конкретные ошибки при разработке

Первый реальный guest boot завершился `errno=2` при создании сокета в `/var/run`:
такого доступного пути в restore ramdisk нет. Попытка создать собственную
директорию в `/` дала `stage=directory errno=30` (EROFS): корень ramdisk read-only.

Исправление не требует APFS или записи в системный диск. Штатный подписанный
`/sbin/mount_tmpfs` создаёт временный 1-МиБ tmpfs в существующем `/mnt9`. В нём
создаётся root-owned каталог с группой mobile и режимом 0750, сокет — 0660.
После клиента брокер удаляет собственный сокет/каталог и размонтирует tmpfs.
`cleanup=1` печатается только после успешной очистки; ошибка очистки даёт FAIL.
Первый unit-test также выявил неверную генерацию тестового sequence=0 через
encoder, который правильно запрещает его: malformed fixture теперь собирается
явным повреждением поля и пересчётом CRC. Обнаружены и исправлены compile errors
из-за повторного `#undef NULL` и недостающего feature macro для dprintf.

## Протокол и границы

Launcher создаёт независимый случайный 128-битный capability, не использующий nonce
mailbox, и передаёт его ребёнку через унаследованный read-only pipe fd 3.
Capability не передаётся в argv/environment/логи. Raw fd и listener имеют
CLOEXEC; перед exec ребёнок закрывает их, очищает supplementary groups и
сбрасывает gid/uid в 501. Клиент — самостоятельный Mach-O с собственным C/ObjC
кодом, системными libSystem/libobjc и пустыми entitlements, а не библиотека,
загружаемая в root-процесс. Персональные Apple-файлы в APK не входят.

Брокер проверяет getpeereid и capability; клиент проверяет root peer. Hello —
32 байта: `EGB1`, u32 LE version=1, token[16], reserved[8]=0. Ответ — 80 байт:
`EGB1`, version=1, u32 status, u32 identity-length, identity[64]. При отказе
identity-length и identity нулевые. Затем идут страницы EGP1 по 4096 байт,
с прежними CRC/sequence/size проверками. Обслуживается один клиент/один context,
один outstanding request. Первый запрос — HELLO seq=1. Точная копия последнего
запроса получает cached reply без повторной GPU-операции; изменённый duplicate,
gap, bad CRC, response вместо request и неполная страница закрывают сессию.
Ресурсы, команды и shader-параметры дополнительно проверяет существующий executor.

Число отвергнутых подключений ограничено восемью; hello/accept/frames/mailbox
имеют deadlines. Read/write учитывают частичные передачи, EINTR/EAGAIN; для
сокетов подавлен SIGPIPE. Брокер не удаляет существующий чужой endpoint.
Это одноразовый диагностический сервер, не готовый многопользовательский daemon.

`mobile` и отсутствие raw-доступа **не доказывают** работу App Store sandbox,
container/Seatbelt policy или подключения произвольного приложения. Наш EAGL
adapter устанавливается явно, только если штатного EAGLContext нет. Drawable
остаётся EmberDiagnosticDrawable; CAEAGLLayer/IOSurface/WindowServer, штатная
загрузка OpenGLES/Metal, полный resource lifetime и multi-context не реализованы.
Основные software patches рабочего образа здесь не изменены.

Следующие блокеры: проверить брокер на Adreno; затем отдельно проверить маршрут
загрузки frontend в обычном приложении и его реальные API, расширять GLES
coverage и drawable/IOSurface synchronization. Подключение GarageBand, звук и
задержка требуют отдельных проверок; этот milestone их не подтверждает.

## Воспроизведение

На Linux нужны JDK 21, NDK 28.2.13676358, C compiler, EGL/GLES headers/libraries,
Python 3 и pinned Procursus ldid из `gpu-bridge/build-ldid.sh`.

```sh
OUT="$PWD/build/broker" gpu-bridge/build.sh
OUT="$PWD/build/broker" ANDROID_NDK_HOME=/path/to/ndk LDID=/path/to/ldid \
  gpu-bridge/build-gles-client.sh
OUT="$PWD/build/broker" ANDROID_NDK_HOME=/path/to/ndk LDID=/path/to/ldid \
  gpu-bridge/build-broker.sh
gpu-bridge/test.sh
python3 gpu-bridge/integration-test.py --build build/broker --gles-api --present --broker
python3 gpu-bridge/test-boot-monitor.py
JAVA_HOME=/path/to/jdk gpu-bridge/jni-test/build.sh
JAVA_HOME=/path/to/jdk EGPU_JNI_BUILD="$PWD/build/gpu-jni" \
  LD_LIBRARY_PATH=/path/to/GL/lib EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 gpu-bridge/jni-test/test.py --client build/broker/broker-client-linux --present --broker
./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
```

Для ASan/UBSan задать `TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
при сборке Linux binaries и unit tests; `ASAN_OPTIONS=detect_leaks=0` отключает
Mesa leak checking. Клиент Linux работает со своим uid и не доказывает iOS права.

Реальный boot требует своих Apple inputs, указанных в GPU-LAB.md, и нового
scratch-каталога. `tools/patch-ramdisk-probe/build-with-tools.sh` получает stock
IM4P, два новых output paths, `build/broker/broker-launcher-ios`, затем
`build/broker/broker-client-ios`. Запуск `tools/patch-boot-probe/run.py` получает
`--gpu-host build/broker/host-probe --gpu-gles-api --gpu-eagl --gpu-present --gpu-broker`.
Он требует всех broker markers и согласованных счётчиков и явно записывает
`android_surface_verified=false`. В APK stageGpuTools проверяет SHA-256 пяти
включаемых helpers. Исходники/бинарники этого этапа — GPL-3.0-or-later.
