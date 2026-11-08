# Исполнитель GLES3 для EGP2

Здесь описана реализованная часть хостовой стороны. GPU-драйвер iOS ещё не готов.
`SystemGpuNativeBackend` исполняет команды в `system/host.c`; отдельный bound
`SystemGpuHostService` живёт в `:system_gpu`. VM-side клиент связывает время
жизни сервиса с binder VM, передаёт Surface и получает состояние/ошибки.
Обычный VmService пока его не создаёт: гостевой frontend/установщик отсутствуют.

## Данные и владение

- Один EGL worker; до восьми гостевых контекстов с отдельным состоянием.
  Явные sharegroups разделяют логические объекты; FBO/VAO остаются локальными.
- На wire идут виртуальные идентификаторы, а не GLuint или адреса хоста.
  Удалённый объект сохраняется, пока другой контекст/VAO/FBO ещё ссылается на
  него. Повторное выделение того же виртуального имени не портит старую ссылку.
- Pool IOSurface: 128 МиБ. Дополнительный учёт объектов: 128 МиБ, включая CPU
  mirrors index buffers и shader source. Это учёт наших storage, не предел
  всей памяти vendor driver. Размер одного массива ограничен 64 МиБ.
- EGLImage views разделяют GPU storage, сохраняя настройки sampler отдельно.
  CPU upload BGRA преобразуется только для переданного dirty rectangle.
  Presentation не делает CPU readback. GPU fences упорядочивают запись, вывод
  и следующее изменение surface; освобождение бюджета дожидается GPU.
- Чтение пикселей выполняется только по явной команде. Ошибка GL доступна через
  GET_ERROR; неизвестные/повреждённые команды прерывают канал с ошибкой.
  Полная спецификация GLES3 ещё не покрыта этим command set.

## CALL ABI

Внешний EGP2 header остаётся 32 байта, version=2. CALL payload:
`upload_id:u64 + command`. Все числовые поля little-endian. Command начинается
с opcode:u32; список и порядок opcodes — `gpu-bridge/system/commands.h`.
Остальные поля соответствуют явным typed handlers в `host.c`, без host pointers.

BATCH: `opcode:u32, count:u32`, затем `count` записей:
`command_bytes:u32, upload_offset:u32, upload_bytes:u32, command[...]`.
Весь framing и диапазоны upload проверяются до исполнения первого вызова.
Queries, вложенные batch и операции transfer surface внутри batch запрещены.
Обычные команды и массивы uniforms/буферов/текстур можно объединять.
Payload-массивы текстур tight-packed; padded rows предусмотрены у SURFACE_UPLOAD.

HELLO может запросить FEATURE_READBACK=1 в третьем u32, остальные параметры
`major=3, minor=0, reserved=0`. Старые клиенты с feature=0 продолжают работать.
Новый явный readback:

1. READBACK_CALL=10 имеет такой же payload, как CALL. Ответ после status:
   `id:u64, size:u32, sha256[32]`. Максимум 64 МиБ, включая пустой результат.
2. READBACK_CHUNK=11: `id:u64, offset:u32, count:u32`. Ответ после status:
   `id:u64, offset:u32, bytes[count]`. Count ≤65520; offset строго последовательный.
3. READBACK_RELEASE=12: `id:u64`, только после полного чтения.

Пока результат не освобождён, разрешены только чтение chunks, release или CLOSE.
C-клиент проверяет контекст, sequence, ID, длины и SHA-256. Только один result
направления host→guest находится в полёте. Разрыв канала очищает его.
Ключ запуска не пишется в intents, command line, source или diagnostics.

## Проверка и сборка

```sh
OUT=/private/egp2 gpu-bridge/system/build.sh
OUT=/private/host GL_PREFIX=/path/to/mesa/usr gpu-bridge/system/test-host.sh
# Тот же native JNI bridge на Linux для JVM integration:
cc -std=c11 -O1 -fPIC -shared -Wall -Wextra -Werror \
  -I"$GL_PREFIX/include" -Iapp/src/main/cpp -Igpu-bridge \
  -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
  app/src/main/cpp/system_gpu_jni.c gpu-bridge/system/host.c \
  gpu-bridge/system/surface.c app/src/main/cpp/gles_renderer.c \
  -L"$GL_PREFIX/lib/x86_64-linux-gnu" -lEGL -lGLESv2 -lpthread \
  -o /private/host/libember_system_gpu.so
./gradlew :app:testDebugUnitTest \
  -PsystemGpuNativeDir=/private/host \
  -PsystemGpuNativeClient=/private/egp2/egp2-native-client-linux \
  -PsystemGpuPeer=/private/egp2/egp2-peer-linux \
  -PsystemGpuGlPrefix="$GL_PREFIX" -PtestTmpDir=/large/tmp
./gradlew :app:assembleDebug :app:lintDebug
```

Нативный host test проверяет полный indexed draw, матрицу, padded BGRA,
разделяемые контексты, независимые EGLImage samplers, GPU-запись через view,
удаление/повторное имя буфера при живой ссылке другого VAO, index bounds и
предварительную проверку batch. Cross-language test действительно загружает
JNI, исполняет GLES и получает полный кадр через chunked readback. Отдельно
проверяются запросы с неправильным контекстом/offset, преждевременный release,
необъявленные features и застрявшие setup/GL calls.

Для ASan/UBSan: TEST_CFLAGS='-fsanitize=address,undefined -fno-pie -no-pie'
и LSAN_OPTIONS=detect_leaks=0 только из-за внешних Mesa driver caches.
Выполнение native Android library на телефоне и ускорение SpringBoard/Settings
по этим результатам не подтверждены.
