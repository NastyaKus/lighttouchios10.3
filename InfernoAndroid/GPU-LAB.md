# Ember VM 0.5.0-gpulab

Отдельный Android-стенд проверяет гостевые GLES/EAGL команды на драйвере GPU
телефона. **Графика обычной iOS и GarageBand ещё не ускорена.** CPU/GLES3 в
настройках обычного запуска по-прежнему выбирает способ показа готового кадра.

## Проверка на телефоне

1. Установить APK поверх текущего Ember VM. Удалять приложение, импортировать
   архив или повторять восстановление не требуется.
2. Остановить обычную VM, восстановление и импорт. Дождаться завершения.
3. Ниже настроек открыть **«GPU-стенд · тест команд iOS»**.
4. Нажать **«Проверить гостевые GPU-команды»**. Нужно 512 МиБ свободного места.
   Тест создаёт новый разреженный root 32 GiB; это не 32 GiB занятого места.
5. Дождаться PASSED/FAILED. После запуска VM ожидание ограничено пятью минутами.
   На экране и в уведомлении есть остановка. Здесь не появится SpringBoard:
   probe работает в restore ramdisk и проверяет offscreen-отрисовку.
6. Нажать **«Сохранить диагностику ZIP»**, выбрать «Загрузки» в системном диалоге
   и подтвердить сохранение. При ошибке сохранить ZIP до переустановки приложения.

В PASSED требуется минимум 40 проверок, 4 draw calls, 5 shader compilations,
совпадение счётчиков гостя/хоста и пикселей. Renderer Adreno в успешном отчёте
подтвердит выполнение тестовой связки на его драйвере. Это не benchmark FPS
и не подтверждение ускорения приложений. Software renderer также записывается
в отчёт и не считается аппаратным выполнением.

Стенд читает загрузочные компоненты имеющегося набора 18A5351d и ничего не
скачивает. Если отсутствует `stock.im4p` в списке входов, фактически нужен
`InfernoData/Restore/038-44135-124.dmg` из подготовленного IPSW.
Если BuildManifest отсутствует, допускается desktop kit с фиксированными
именами этой сборки. Другие версии iOS не поддерживаются.

ZIP содержит result.json, driver strings, counters, console/engine logs и
доступные native crash traces. Сохранение во время теста тоже включает текущие
логи. Диски/прошивки/ROM/ticket/key не экспортируются. Остаются три последних
набора логов. При повторе удаляются только помеченные приватные scratch-папки.

## Изоляция

- `GpuLabActivity` живёт в основном процессе с UI.
- Foreground `GpuLabService` в `:gpulab` владеет QEMU и wake lock.
- Bound `GpuHostService` в `:gpu` создаёт EGL на worker. Там нет Activity/HWUI;
  его lifetime связан с foreground service. Зависший GL call ограничен процессом.
- Общий `VmLease` исключает одновременные VM, restore, import, IPSW prep и тест.
  У владельца QEMU lock освобождается ядром при смерти процесса. Локальный
  registry исключает опасное повторное открытие/закрытие lock inode.
- Kotlin `buildGpuProbe` одним HFSX rewrite ставит наш probe и dylib в копию
  ramdisk, отключает restored_external и добавляет nvmefwupdater --skip.
  Restore, USB accept, StartRestore и FS patcher не запускаются.
- Root/root.qcow2, firmware, NVRAM и SEP state пользователя не копируются и не
  прикрепляются. Создаются девять новых sparse-дисков. ROM, ticket, SEP firmware,
  kernel, DeviceTree и trustcache копируются из исходного набора только для чтения.
- PASSED требует guest PASS, host counters, успешного выключения QEMU и
  сохранности контрольных областей scratch root. Restore checkpoint не меняется.

## Проверено здесь

Unit tests проверяют отдельные диски/аргументы, сохранность source kit,
marker/channel validation, HFSX jobs, JNI fields и отрицательные evidence cases.
Межпроцессные тесты проверяют lock и release после SIGKILL.

Тот же JNI C wrapper через desktop JVM и Mesa EGL прошёл GLES roundtrip,
проверку всех 12 полей ответа, отмену и отказ перезаписи существующего файла.
Через него прошёл и реальный iOS 14 release kernel с ramdisk, подготовленным
новым Kotlin GpuLabKit/buildGpuProbe: 40 checks, 194 API calls, 18 batches,
4 draws, 5 shader compilations. Host — Linux llvmpipe software.

**Android Service/Binder lifecycle, реальный Adreno и установка APK на телефон
здесь не проверены:** подключённого Android устройства нет. Успешная сборка
APK не выдаётся за успешный аппаратный тест. Итоги — GPU-LAB-VALIDATION.json.

### Последующая проверка на телефоне

В двух присланных диагностических прогонах 0.5.0 на Android API 36 проверено
выполнение на **Qualcomm / Adreno (TM) 740 / OpenGL ES 3.2**. В каждом: 40 checks,
119 последовательных transport commands без ошибок, 194 API calls, 18 batches,
4 draws, 5 shader compile attempts (один shader намеренно ошибочный),
16 391 readback pixels. Guest PASS, host.log и оба JSON-отчёта согласованы.
QEMU завершился с кодом 0, контрольные области scratch root сохранились.

Полные прогоны заняли 46,752 и 47,466 секунды, включая подготовку и загрузку
гостя. Это не измерение времени GPU или прироста FPS. Проверены два обычных
запуска и повтор в свежих процессах; фон, отмена, нехватка памяти и recovery
после падения драйвера этим не проверены. Записи SIGNALED/status=9 после PASS
согласуются с предусмотренным завершением :gpu и :gpulab через killProcess.

Обезличенная сводка — [gpu-bridge/ADRENO-VALIDATION.json](gpu-bridge/ADRENO-VALIDATION.json).
Исходный пользовательский ZIP и его логи в репозитории не сохранены.
Графика существующих приложений, Metal и compositor остаются неподключёнными.

## Воспроизведение компонентов на Linux

Из InfernoAndroid нужны JDK 21, NDK 28.2.13676358 и EGL/GLES development headers.

```sh
./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
JAVA_HOME=/path/to/jdk gpu-bridge/jni-test/build.sh
JAVA_HOME=/path/to/jdk EGPU_JNI_BUILD="$PWD/build/gpu-jni" \
  EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 gpu-bridge/jni-test/test.py --client build/gles-client/gles-api-probe-linux
```

Apple inputs должны принадлежать запускающему, в APK/архиве их нет. Новый
scratch run должен быть вне source kit, имя `run-<digits>-<12 lowercase hex>`.

```sh
./gradlew :app:compileDebugKotlin
JAVA_HOME=/path/to/jdk GRADLE_USER_HOME=/path/to/gradle-cache \
  tools/gpu-lab-probe/prepare.sh /owned/kit /scratch/run-1-abcdef123456
JAVA_HOME=/path/to/jdk EGPU_JNI_BUILD="$PWD/build/gpu-jni" \
  EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 tools/patch-boot-probe/run.py \
    --qemu /path/to/linux/qemu-system-aarch64 \
    --inputs /scratch/run-1-abcdef123456 \
    --kernel /scratch/run-1-abcdef123456/InfernoData/Restore/kernelcache.release.iphone12b \
    --ramdisk /scratch/run-1-abcdef123456/gpu-probe.im4p \
    --gpu-host "$PWD/gpu-bridge/jni-test/host.py" --gpu-gles-api --gpu-eagl \
    --work /scratch/NEW-boot
```

## Исправленные ошибки

1. Cleanup папки без marker выдавал FileNotFoundException вместо явного отказа:
   marker.isFile проверяется до чтения, отрицательный тест проходит.
2. Linux JNI build требовал _DEFAULT_SOURCE для O_NOFOLLOW/O_CLOEXEC:
   исправлены flags fixture, Android NDK уже поддерживал эти флаги.
3. POSIX lock снимался при закрытии второго fd после неудачной локальной
   tryLock. Воспроизведено отдельным JVM (неожиданный ACQUIRED), исправлено
   registry до открытия fd и межпроцессным regression test.
4. Длинный Unix socket path заменён коротким cache path, подходящим также для
   дополнительных Android user profiles.

После отчёта телефона остаются фактические driver/lifecycle ошибки, большой
resource/shared-memory transport, drawable/IOSurface presentation и подключение
к API существующих приложений. Metal, compositor, GarageBand и звук не готовы.
