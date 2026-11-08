# Гостевой GPU bridge: исследовательский прототип

Цель ветки `codex/guest-gpu-bridge`: полноценное ускорение графики гостевой iOS
для обычных приложений, в первую очередь GarageBand. Этот снимок реализует
обмен командами и экспериментальный GLES frontend; полноценный графический драйвер ещё
предстоит разработать. В нём нет поддержки GarageBand, Metal или AGX.

## Новый этап 0.7.0: [GPU broker](../GPU-BROKER.md)

Root-owned mailbox и отдельный mobile клиент с denied raw-доступом проверены
в реальном iOS 14 guest; физический Android-прогон нового режима ещё требуется.
Полноценная графика существующих приложений остаётся незавершённой.

## Новый этап: GLES API и offscreen EAGLContext

В 0.6.0 добавлен отдельный эксперимент с GPU present и Android PixelCopy:
[GPU-PRESENT.md](../GPU-PRESENT.md). Прежние 0.5.0 Adreno результаты остаются
подтверждением offscreen режима. Один window-прогон 0.6.0 на Adreno 740 также
подтверждён: 60 presents и четыре точных PixelCopy samples первого кадра,
[SURFACE-VALIDATION.json](SURFACE-VALIDATION.json). Обычные приложения не подключены.

Вслед за первым семикомандным тестом добавлены библиотека с обычными C
сигнатурами `gl*`, компиляция переданных гостем GLSL shaders, VBO, текстуры,
blend/scissor, пакетная передача команд, ошибки GL и владение контекстом по
потокам. Отдельный Objective-C тест создаёт наш offscreen `EAGLContext` API 2
через обычные selectors. На iOS 14 он прошёл **40 проверок**: 194 API calls,
18 batches, 4 draw calls, 5 shader compilations; всего 119 transport commands.

Это тестовая библиотека, к которой явно прилинкован наш probe. Она пока не
подключает графику существующих приложений iOS. Класс регистрируется только
при отсутствии родного `EAGLContext` в отдельном диагностическом процессе;
подмена класса Apple не выполняется. `CAEAGLLayer`, вывод окна и Metal ещё
отсутствуют. Принимающий GL driver по-прежнему **Mesa llvmpipe на Linux**;
сборка Android arm64 выполнена. Позже два телефонных прогона подтвердили
offscreen GLES/EAGL bridge на Adreno 740: [ADRENO-VALIDATION.json](ADRENO-VALIDATION.json).

Точный процесс воспроизведения, API coverage, ограничения и ошибки сборки:
[GLES-API.md](GLES-API.md). Результаты нового этапа:
[GLES-VALIDATION.json](GLES-VALIDATION.json). `VALIDATION.json` ниже сохраняет
результаты предыдущего семикомандного этапа и его исторические хеши.

## Что действительно выполнено

ARM64 Mach-O программа запущена **в настоящем ядре iOS 14 18A5351d**, из
изолированного restore ramdisk. Она обнаружила выделенный диагностический NVMe
namespace `/dev/rdisk2`, отправила семь команд, дождалась fence и получила
ожидаемые RGBA-пиксели: красный центр треугольника и синий угол фона.

Также подтверждены создание IOSurface 64×64, CPU mapping, запись пикселя и его
чтение после unlock/lock. Это отдельная проверка гостевой shared-memory основы,
ещё не GPU↔IOSurface sharing.

На Linux принимающий процесс исполнил команды через EGL/OpenGL ES 3 на **Mesa
llvmpipe**, то есть программном драйвере. Это доказывает гостевой ABI/обмен и
вызовы GL с правильным результатом. Выполнение на физическом Adreno в этом
тесте не проверено. Android arm64 библиотека того же исполнителя собрана NDK.

| Компонент | Статус |
|---|---|
| Бинарный протокол, CRC, длины, sequence/replay | реализован, тесты + sanitizers |
| Канал iOS-гость ↔ host через отдельный namespace | подтверждён реальным гостем |
| GL create/clear/triangle/fence/readback/destroy | реализованы, результат проверен гостем |
| Host executor Android arm64 | сборка и два offscreen прогона Adreno 740 подтверждены |
| IOSurface create/lock/unlock/повторное чтение в госте | проверены; GPU interop ещё нет |
| C GLES subset + наш offscreen EAGLContext API 2 | проверены в отдельном iOS probe |
| EAGL/OpenGLES/Metal существующего iOS приложения | не подключены |
| IOSurface / CoreAnimation / WindowServer integration | не реализована |
| Ускорение SpringBoard/GarageBand | не реализовано |

Программа явно отправляет наш диагностический протокол. Приложения iOS сами
такие команды не посылают. Успех треугольника нельзя считать включением
системного GPU-ускорения.

Модуль изолирован в `gpu-bridge/`; boot test включается дополнительным
`--gpu-host` в существующем тестовом CLI. Обычный Android APK 0.4.0, рабочий
гостевой диск и режим восстановления не используют этот код автоматически.

В 0.5.0-gpulab появился отдельный Android runner: [GPU-LAB.md](../GPU-LAB.md).
Обычный запуск iOS его не использует. Два нормальных Android запуска с Adreno
740 подтверждены присланной диагностикой; остальные lifecycle-сценарии ещё не проверены.
`GLES-VALIDATION.json` сохраняет результаты/хеши этапа f71d4f0;
текущая проверка Android-сборки находится в `../GPU-LAB-VALIDATION.json`.

## Устройство прототипа

- `protocol.[ch]`: `EGP1`, version 1, 20-byte LE header, максимум 4076 bytes
  payload. CRC-32/ISO-HDLC покрывает первые 16 bytes header и payload.
- Header: magic[4], version u16, opcode u16, sequence u32, payload size u32,
  CRC u32. Response opcode = request opcode | 0x8000; response payload
  начинается со status u32. Sequence начинается с 1/HELLO.
- `session.[ch]`: только один outstanding запрос. Повтор последнего пакета
  возвращает сохранённый ответ без повторного выполнения. Изменённый запрос
  с тем же sequence и пропуски sequence отклоняются. Padding не часть payload.
- `executor.[ch]`: один принадлежащий worker thread EGL context, private FBO,
  один surface ≤512×512. Guest видит собственные handles, без host pointers и
  GLuint. Границы, размеры, координаты, NaN и stale handles проверяются.
- `file-host.[ch]`: создание **нового** 16 MiB sparse channel через O_EXCL и
  O_NOFOLLOW. Identity + random session nonce в page 0. Request page 1,
  response page 2, всё по 4096 bytes. Guest сверяет identity перед записью.
- `guest-probe.c`: standalone явный command producer; одинаковый C-код для
  Linux теста и ARM64 iOS. В iOS без `--image` ищет только namespace с нашим
  marker. `--registry-only` выполняет только чтение доступных IOKit services.
- `build-android.sh`: отдельная `libember_guest_gpu_probe.so`, API
  `egpu_file_host(...)`. Она пока не связана с production JNI/UI.

Базовый диагностический протокол поддерживает HELLO, CREATE(width,height), CLEAR(handle,RGBA8),
TRIANGLE(handle,6 float32 NDC,RGBA8), READ_POINTS(handle,count,xy pairs), FENCE,
DESTROY(handle). Его legacy probe использует фиксированный shader. Расширение
GLES_BATCH/GLES_QUERY добавляет передаваемые shaders, небольшие textures и
blending; полное API, depth attachment, compute и window presentation ещё
не реализованы. Readback нужен для проверки результата, не для будущего
покадрового отображения.

Отрисовка и ожидание драйвера происходят в отдельном host-процессе, вне QEMU
BQL. Этот прототип EGL владеет display целиком: встраивать его в процесс с
другими EGL users без переработки lifetime нельзя. Поллинг имеет timeout и
cancel callback; зависший внутри GL драйвер требует завершения отдельного
процесса. Busy-poll scratch channel также не заявляется финальным транспортом
для рендеринга в реальном времени.

## Воспроизведение без Mac

Linux host: C compiler, Python 3, EGL/GLES3 development headers, Mesa llvmpipe.
Debian: `libegl-dev libgles-dev libgles2 libgl1-mesa-dri`.

```sh
gpu-bridge/build.sh
gpu-bridge/test.sh
python3 gpu-bridge/integration-test.py --build build/gpu-bridge
python3 gpu-bridge/test-boot-monitor.py
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  ASAN_OPTIONS=detect_leaks=0 gpu-bridge/test.sh
```

Для iOS executable используется generic LLVM из Android NDK 28.2.13676358;
Apple SDK не нужен. `libSystem.tbd` содержит только публичный link interface.
Подпись с диагностическими entitlements делает Procursus ldid. Сборка требует
Linux `git make g++ pkg-config libssl-dev libplist-dev`:

```sh
gpu-bridge/build-ldid.sh
ANDROID_NDK_HOME=/path/to/android-ndk \
  LDID="$PWD/build/ldid/src/ldid" gpu-bridge/build.sh
ANDROID_NDK_HOME=/path/to/android-ndk gpu-bridge/build-android.sh
```

Pinned signer source: ProcursusTeam/ldid @
`af86971ae72ec3ed3d0a699107c4e882324c941b`. Полученные binaries под GPL вместе
с исходниками; для этой сборки в CodeDirectory используется ad-hoc подпись.
Она применима к исследовательскому гостю Inferno и не является подписью Apple.

Настоящий iOS boot требует уже имеющихся у запускающего Apple inputs. В
публичный архив они не входят. Нужны Linux QEMU из pinned MakrSas engine,
research kernelcache 18A5351d, stock restore ramdisk IM4P и подготовленные ROM,
SEP firmware, root ticket, device tree, ramdisk trustcache. Работа только с
НОВЫМ scratch directory; тест создаёт свои девять дисков, не использует
восстановленный root и не посылает StartRestore.

```sh
./gradlew :app:compileDebugKotlin
JAVA_HOME=/path/to/jdk GRADLE_USER_HOME=/path/to/gradle-cache \
  tools/patch-ramdisk-probe/build-with-tools.sh /owned/stock.im4p \
  /scratch/injected.im4p /scratch/probe.im4p build/gpu-bridge/guest-probe-ios
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 tools/patch-boot-probe/run.py \
    --qemu /path/to/linux/qemu-system-aarch64 --inputs /owned/flat-inputs \
    --kernel /owned/kernelcache.research.iphone12b --ramdisk /scratch/probe.im4p \
    --gpu-host "$PWD/build/gpu-bridge/host-probe" --work /scratch/NEW-boot
```

Для legacy probe ожидается `gpu_commands_verified`, `guest_readback_verified: true`, host
`success=1 commands=7 draws=2 readback_pixels=2`. Это не APFS/FS-patcher тест и
не загрузка установленной iOS. Результат текущего прогона — VALIDATION.json.

Первый реальный прогон завершился отказом: `System Policy ... deny(1)
file-read-data /dev/rdisk*`. Простого linker ad-hoc signing недостаточно для
raw disk access. После добавления четырёх raw-storage/platform ключей обмен прошёл.
Затем IOSurfaceCreate вернул NULL с `deny(1) iokit-open
IOSurfaceRootUserClient`. Для этой проверки добавлен точный user-client class
в `com.apple.security.iokit-user-client-class`. Итоговые пять ключей находятся
в `probe.entitlements.plist`. Основной patcher и его
entitlements не менялись. Это проверено самим гостем, а не только ldid -e.

IOKit загрузился по versioned path
`/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit`. Поиск services
завершился status 0: `IOSurfaceRoot=1`, а `IOAccelerator`, `IOGPU`,
`AGXAccelerator`, `AppleParavirtGPU` — 0. Это наблюдение конкретного ramdisk
запуска; обычная установленная iOS в этом тесте не загружалась. Полного
OpenGLES/EAGL пользовательского стека в доступном тестовом окружении нет.

В одном прогоне host успешно завершил обмен раньше, чем guest успел записать
PASS в console.log. Монитор ошибочно сообщил failure. Исправлен порядок:
после host exit 0 даётся до пяти секунд на guest PASS, обязательно проверяются
host counters `(commands,draws,pixels)=(7,2,2)`. Три синтетических regression
теста проверяют этот race, nonzero exit и отсутствие guest PASS.

## Что блокирует полноценное ускорение

1. **Guest frontend/driver.** Нужен реально работающий EAGL/IOAccelerator либо
   IOGPU/Metal frontend, принимающий команды существующих приложений. Наш
   offscreen-адаптер этого ещё не делает. Требуются наблюдение обычного
   iOS 14 runtime, соответствующие private ABI и отдельный тест guest driver.
2. **API coverage и shaders.** Экспериментальный subset уже выполняет shader
   compilation, textures, VBO и часть state. Нужны полный resource lifetime,
   render passes, больше форматов и API, synchronization и conformance tests.
3. **IOSurface и compositor.** Нужно связать guest shared buffers, форматы,
   ownership/fences с CoreAnimation/WindowServer и выводом. GL readback каждого
   кадра не является подходящей финальной архитектурой.
4. **Гостевые software patches.** Рабочая 18A5351d намеренно отключает Metal,
   OpenGLES assumptions и часть renderers. Включать их можно только после
   появления соответствующего backend, в отдельной копии гостевого комплекта.
5. **Android runtime.** Изолированный worker и два успешных offscreen прогона
   на Adreno 740 подтверждены. Нужны проверки фона, отмены, потери context,
   падения драйвера, памяти и дальнейшего пути window/shared-buffer presentation.
6. **GarageBand.** Только после graphics integration проверяются конкретная
   версия приложения, её реальные graphics API, совместимость, звук и задержка.
   Даже полный GPU не заменяет проверки скорости TCG/audio.

Для развития выбран paravirtual command bridge. Эмуляция полного Apple AGX —
альтернативный значительно больший путь: MMU/UAT, ASC firmware/queues/events,
Apple command encoders и перевод shaders. Публичные Asahi kernel-driver notes
описывают только часть этого стека и не предоставляют готовый A13→Adreno
эмулятор. Готового guest driver для нашей iOS 14 из них также не получается.

Проверенные источники:

- [Pinned engine](https://github.com/MakrSas/Inferno/tree/b76fa575831d94a80db169ffb3c76d9094a648bd): displaypipe + software pixman.
- [Upstream apple-gfx](https://github.com/ChefKissInc/Inferno/blob/master/hw/display/apple-gfx.m): требует macOS ParavirtualizedGraphics.framework; прямого Android backend нет.
- [AGX kernel interface notes](https://github.com/AsahiLinux/docs/blob/main/docs/hw/soc/agx.md): firmware, UAT, channels, work queues; userspace/shaders вне области документа.
- [Procursus ldid](https://github.com/ProcursusTeam/ldid/tree/af86971ae72ec3ed3d0a699107c4e882324c941b): Linux signer.

Честный текущий результат: guest→host GL command roundtrip подтверждён;
полноценное ускорение приложений iOS остаётся незавершённой разработкой.
