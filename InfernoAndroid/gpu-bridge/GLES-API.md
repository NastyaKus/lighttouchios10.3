# GLES frontend: проверенный промежуточный результат

Цель — выполнять графические команды iOS на Android GPU. Этот этап добавляет
экспериментальную клиентскую библиотеку с GLES2 C ABI и собственный offscreen
адаптер `EAGLContext`. Они проверены в реальном ядре iOS 14, но ещё не являются
драйвером графики системы или существующих приложений. APK 0.4.0 их не включает.
APK 0.5.0-gpulab добавляет отдельный Android runner: [GPU-LAB.md](../GPU-LAB.md).
Два обычных запуска на Adreno 740 подтверждены диагностикой телефона:
[ADRENO-VALIDATION.json](ADRENO-VALIDATION.json). Остальные lifecycle-сценарии не проверены.
Ограниченный FBO/RBO и диагностический window present добавлены в 0.6.0:
[GPU-PRESENT.md](../GPU-PRESENT.md). Его один успешный телефонный прогон с
PixelCopy записан отдельно в [SURFACE-VALIDATION.json](SURFACE-VALIDATION.json).
Описанный ниже набор ограничений относится к исходному offscreen этапу.

## Проверка 5 ноября 2026

На исследовательском restore ramdisk 18A5351d тест Objective-C:

- создал API 2 context, назначил текущим, отключил и назначил повторно;
- подтвердил независимый currentContext второго pthread и отказ одновременного
  владения одним контекстом; API 3 и drawable/present явно отклонены;
- отправил исходники vertex/fragment shaders, получил результаты настоящего
  compiler/linker; некорректный shader отклонён с непустым compiler log;
- загрузил VBO и текстуру, проверил треугольник и четыре цвета текстурного quad;
- проверил blending, scissor, PACK/UNPACK alignment, ограничения буфера и ошибки;
- объединил десять GL calls в один transport request.

Итог: `PASS checks=40 packets=118` до закрытия; с DESTROY 119 запросов,
194 API calls, 18 batches, 4 draws, 5 shader compilations, 16 391 readback pixels.
Финальный прогон занял 12 секунд. EGL host: Mesa llvmpipe (software), Linux x86_64.
На этом исходном этапе Android backend был только собран NDK для arm64 API 29;
последующая телефонная проверка Adreno записана отдельно в ADRENO-VALIDATION.json.
Ни скорость, ни аппаратное выполнение этими результатами не подтверждаются.

Все девять дисков созданы заново; OS image отсутствовал, StartRestore не
отправлялся. Контрольные области scratch root сохранились. Установленная iOS,
SpringBoard, GarageBand и рабочий пользовательский root в этом тесте не использовались.

## Файлы и протокол

| Файл | Назначение |
|---|---|
| `gles2-subset.h`, `gles-client.c` | Публичные сигнатуры gl*, локальные ошибки, TLS, упаковка команд |
| `gles-wire.h` | GLES_BATCH 0x100, GLES_QUERY 0x101; capability bit 16 |
| `gles-host.[ch]` | Изолированные имена объектов, проверка пакетов, выполнение host GL |
| `eagl-adapter.[ch]` | Наш класс EAGLContext через публичный Objective-C runtime ABI |
| `gles-api-probe.c`, `eagl-api-probe.m` | C и Objective-C интеграционные проверки |
| `gles-host-test.c` | Некорректные batches, границы ресурсов, scalar query whitelist |
| `build-gles-client.sh` | Linux shared library/CLI и собственные Mach-O library/probes |

Payload batch: LE u32 count, затем count записей `(u16 opcode, u16 length,
payload[length])`. Максимум 128 команд, весь payload до 4076 bytes. Сначала
проверяется структура **всего** batch; при повреждённой последней записи
предыдущие команды не исполняются. GL state/resource errors при корректной
структуре поступают в `glGetError`, остальные команды batch продолжаются.
Копирование пользовательских данных выполняется до возврата из GL вызова.
Синхронный запрос flush-ит накопленный batch. Fence использует существующий
протокол. Последний повторный transport sequence по-прежнему не выполняется
дважды. Host GLuint и host pointers гостю не передаются.

Транспорт — диагностический raw namespace с marker/nonce, описанный в README.
Один context/канал, опрос с интервалом 10 ms и readback плитками 16×16: это
инструмент проверки ABI, не готовый путь вывода кадров в реальном времени.
Обычным sandboxed приложениям нельзя выдавать raw-disk доступ ради этого
прототипа. Для них нужен привилегированный guest broker/драйвер с контролируемым
IPC и shared buffers. Такой broker ещё не написан.

## Поддерживаемая часть API

Shader create/source/compile/status/log/delete; program create/attach/link/use,
attrib binding/location, uniform location/4f/1i; VBO gen/bind/upload/delete;
float vertex attributes; draw arrays; texture gen/bind/RGBA8 upload/parameters;
viewport, clear, blend, scissor; pack/unpack alignment; tiled RGBA8 readback;
flush/fence и часть queries. Полный перечень экспортов — в `gles2-subset.h`.

Ограничения текущего стенда:

- один offscreen surface до 512×512, без depth/stencil attachment;
- 63 ID ресурсов за жизнь context, без повторного выделения ID;
- source и VBO максимум 2048 bytes, texture максимум 16×16 RGBA8, level 0;
- 8 float vertex attributes, 8 texture units, до 1024 вершин за draw;
- только заранее сгенерированные buffer/texture names, без client-side arrays;
- shader/program deletion ещё не моделирует все deferred-lifetime правила;
  неизвестный buffer/texture ID при delete пока даёт ошибку вместо ignore;
- info logs ограничены 1024 bytes; нет полного набора glGet state;
- нет FBO/RBO API, indexed draws, sharegroups, API 3, window/drawable, Metal.

`glGetString(GL_VERSION)` прямо сообщает `Ember GLES2 API subset (not conformant)`.
Это не EGL/GLES conformance implementation. Обработка только scalar GLES2
`glGetProgramiv/glGetShaderiv` pnames предотвращает запись массива, например
COMPUTE_WORK_GROUP_SIZE GLES3, в один integer reply slot.

`egEAGLInstall()` применяется явно, в отдельном диагностическом процессе.
Если `EAGLContext` Apple уже существует, установка отказывается; swizzling
не выполняется. Проверен путь с отсутствующим родным классом в ramdisk.
Сосуществование с загруженным позже OpenGLES.framework не проверено и не поддержано.
Текущий context удерживается адаптером; перед выходом потока надо вызвать
`setCurrentContext:nil`, автоматического TLS destructor ещё нет.

## Воспроизведение на Linux без Apple SDK

Из каталога InfernoAndroid нужны C compiler, EGL/GLES development headers,
Python 3. Для Mach-O — Android NDK 28.2.13676358 и pinned ldid из README.
Не запускайте EGL host в процессе с другими EGL users: сейчас он владеет
display целиком. Ни poll timeout, ни cancel callback не прерывают зависший
внутри GL драйвер; host должен оставаться отдельным процессом.

```sh
gpu-bridge/build.sh
gpu-bridge/build-gles-client.sh
gpu-bridge/test.sh
python3 gpu-bridge/integration-test.py --build build/gpu-bridge
python3 gpu-bridge/integration-test.py --build build/gpu-bridge \
  --client build/gles-client/gles-api-probe-linux --gles-api
python3 gpu-bridge/test-boot-monitor.py
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  ASAN_OPTIONS=detect_leaks=0 gpu-bridge/test.sh

ANDROID_NDK_HOME=/path/to/ndk LDID=/path/to/ldid \
  gpu-bridge/build-gles-client.sh
ANDROID_NDK_HOME=/path/to/ndk gpu-bridge/build-android.sh
```

Для instrumented integration задайте TEST_CFLAGS при сборке **обоих** host и
client и отдельные OUT directories; затем передайте их integration-test.py.
Leak detection для Mesa отключена; ASan и UBSan остаются включены.

Реальный guest требует собственных Apple inputs, они в исходники/архив не
входят: ROM, ticket, SEP firmware, DeviceTree, trustcache, research kernel,
stock restore ramdisk 18A5351d. В flat-inputs используйте имена из README.
Нужны уже собранные Kotlin restore classes для HFSX-инъекции.

```sh
./gradlew :app:compileDebugKotlin
JAVA_HOME=/path/to/jdk GRADLE_USER_HOME=/path/to/cache \
  tools/patch-ramdisk-probe/build-with-tools.sh \
    /owned/stock.im4p /scratch/injected.im4p /scratch/eagl-probe.im4p \
    build/gles-client/eagl-api-probe-ios build/gles-client/libEmberGLES.dylib

EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 tools/patch-boot-probe/run.py \
    --qemu /path/to/linux/qemu-system-aarch64 --inputs /owned/flat-inputs \
    --kernel /owned/kernelcache.research.iphone12b \
    --ramdisk /scratch/eagl-probe.im4p \
    --gpu-host "$PWD/build/gpu-bridge/host-probe" --gpu-gles-api --gpu-eagl \
    --work /scratch/NEW-eagl-boot
```

Probe запускается из первого NFC slot ramdisk; второй slot содержит нашу dylib.
Её install_name соответствует этому пути, не system OpenGLES.framework.
Расширение build-with-tools.sh необязательно: старые вызовы сохранены.
Этот ramdisk **нельзя** импортировать как готовую iOS или использовать для
обычного восстановления. Его штатный restore daemon намеренно не запускается.

## Конкретные ошибки и их устранение

1. NDK `stdatomic.h` в freestanding Mach-O сборке требовал Android
   `sys/cdefs.h`. Убрана эта зависимость, ownership использует LLVM/GCC
   `__atomic` builtins; работа между pthread проверена реальным гостем.
2. Linker не находил `__tlv_bootstrap` для Mach-O TLS. Публичный link-interface
   libSystem дополнен этим символом; runtime проверен внутри iOS.
3. Objective-C compiler генерировал `objc_alloc`/`objc_release`, которых не
   было в минимальном TBD. Добавлен публичный libobjc link-interface;
   Foundation/Objective-C реализация по-прежнему загружается из guest OS.
4. GCC `-Werror=misleading-indentation` остановил первоначальные сборки.
   Исправлена структура строк; финальные Linux/NDK/Mach-O сборки проходят
   с `-Wall -Wextra -Werror`.
5. При ревью найден небезопасный scalar query path для GLES3 array pnames.
   Добавлен whitelist GLES2 scalar pnames и отрицательные тесты на host и guest.

## Оставшаяся работа

Android диагностический runner этого же probe реализован в 0.5.0-gpulab;
два обычных Service/guest/GPU запуска на реальном Adreno подтверждены.
Фон, отмена и восстановление после ошибок ещё требуют проверки.
Затем нужны большой upload/shared-memory transport,
resource lifetime/API coverage и drawable/IOSurface presentation. Подключение
к существующим iOS приложениям требует отдельного решения для их графического
ABI; offscreen класс из ramdisk этого не заменяет. System software patches
нельзя отменять до появления нужного backend. GarageBand, звук и задержка
проверяются только после этих этапов.
