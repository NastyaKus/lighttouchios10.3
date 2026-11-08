# Системный GPU: состояние реализации 18A5351d

Это промежуточная реализация, а не готовое ускорение iOS. Принятый план ещё
не выполнен целиком. Нового включателя GPU в приложении нет: обычный запуск
остаётся программным. Сборка APK, синтетический GLES и запуск нашей программы
в ramdisk не подтверждают работу ускоренного SpringBoard.

Ветка `codex/system-gpu-18a5351d`, база `c435b4165623c1c56016c6930c31f391a35e71bb`.
Изменения ограничены InfernoAndroid. Установщик, рабочий root пользователя,
QEMU engine и архив iPhone 5 не изменялись.

## Реализованные компоненты

| Компонент | Результат и граница |
|---|---|
| `SystemGpuProfile` | Независимая sparse-копия root и NAND/SEP состояния под VM lease; SHA-256 при копировании и повторном чтении; отмена/cleanup; без writable backing chain. Состояние только COPIED/SOFTWARE. Ещё не вызвано из UI. |
| `SystemMachO`, `SystemGpuPreparation` | Точная проверка stock backboardd UUID/SHA-256, LC_LOAD_DYLIB в проверенный нулевой header padding; без смещения сегментов. Возвращается кандидат, он не устанавливается. |
| `SystemCodeSigner`, `SystemCodeSignature` | JNI ldid для Linux/Android arm64; ad-hoc; сохранение исходных XML entitlements/identifier/platform/flags. Проверяются хеши CodeDirectory всех страниц и встроенных специальных слотов, включая __TEXT,__info_plist. |
| `SystemTrustCache` | TrustCache v1 / rtsc IM4P; добавление конкретных CodeDirectory hashes, без замены AP ticket и kernel patches. Сохраняет исходные записи, включая одинаковые дубликаты; конфликтующие отклоняются. |
| `SystemGpuCachePatch` | Разбор .InfernoOriginalBytes, проверка SHA-256 всех 45 исходных участков; подготовка возврата только двух инструкций main EAGL renderer. Другие 43 software/no-Metal замены должны совпадать. Файл cache этим компонентом не меняется. |
| `SystemGpuWire`, `SystemGpuEndpoint`, `system/client.*` | Постоянный EGP2, взаимный HMAC, boot/run binding, CRC/sequence; upload и явный readback до 64 МиБ частями с SHA-256. C-клиент проверен через Kotlin/JNI с настоящим ES3 backend. В VmService канал ещё не подключён. |
| `system/host.*`, `SystemGpuNativeBackend`, `SystemGpuHostService/Connection` | Исполнитель GLES3, шейдеры, uniform arrays/matrices, VBO/indexed draw, VAO/FBO/RBO, mipmaps, sharegroups, виртуальные имена, lifetime удалённых объектов; JNI и отдельный `:system_gpu` процесс. Сервис и клиент зарегистрированы/собираются, обычная VM их пока не использует. Нативный код исполнен на Linux/Mesa; новое выполнение на Adreno не проверено. |
| `gpu-bridge/system/surface.*` | ES3 texture/FBO pool до 128 МиБ; полный экран 828×1792; RGBA/BGRA dirty rectangles, row stride, версии, явное CPU-чтение; прямой GPU blit текстуры в window/pbuffer без диагностического readback. Это сторона хоста, привязки к настоящим гостевым IOSurface ещё нет. |
| `gpu-bridge/system/eagl-native.*` | Перехват семи методов настоящего Apple EAGLContext: exact cache UUID и type encodings, типизированные IMP, вызов оригиналов, counters и снятие перехвата. Проверен в настоящем госте iOS. Пока передаёт вызовы без изменений; GLI/IOSurface frontend не реализован. |
| Инвентаризация ABI | Проверен stock cache именно 18A5351d; разобраны реальные EAGL selectors/type encodings и 457 публичных GL trampolines. Это не завершённый hook manifest. |

Обычные `GpuHostService` и EGP1-пробы не подменялись новым транспортом.
`ember_system_surface` компилируется как отдельная static library для Android.
Тот же surface-код включён в новую JNI `ember_system_gpu`; обычный работающий
renderer эту библиотеку не использует. JNI signer можно включить
в development APK явно, см. ниже. Само наличие этой библиотеки ничего не
устанавливает и не включает.

## Что установлено по настоящим файлам iOS 14

Исходник — официальный Apple IPSW iPhone11,8/iPhone12,1 14.0 beta 5,
18A5351d. URL и ZIP-member факты в `tools/system-gpu/stock-member.json`.
SystemVersion.plist проверен: 14.0 / 18A5351d. Выполнено чтение stock APFS,
а не восстановление или загрузка установленной системы.

- Stock System DMG: `038-44337-083.dmg`, stored member, 5155389281 байт,
  ZIP data offset 142921551. Это НЕ готовый root для Inferno.
- Stock cache: 2052489216 байт, UUID
  `11ad39c9-e3c9-3067-a337-7287744fdcb5`, SHA-256 в ABI JSON.
- backboardd: 1121392 байта, UUID `145adb09-0b51-3ef4-8a1c-59cb046cbc5f`.
  Проверены 13720 байт header padding; исходные entitlements 4807 байт.
- Main `CA::WindowServer::EAGLServer::renderer()`:
  VM `0x188158bdc`, file `0x8158bdc`.
- iOS 14 CA GLESContext хранит macro context по `+0x780`.
  Перенос `+0x7f8` из другой версии ОС был бы ошибкой.
- Native renderer требует EAGL API 3. API 2 он отвергает.
- Таблица macro context: слово 0 = rctx, slot N по `8*(N+1)`.
  `glGetString` = slot 117. Максимальный найденный публичный slot 1032.
- Вызовы через BRAAZ/BLRAAZ требуют instruction key A, discriminator zero.
  Это установлено дизассемблированием. **Криптографическая PAC-проверка не
  подтверждена:** Inferno apple-gxf по умолчанию задаёт pauth-noop=true в TCG
  (`target/arm/cpu64.c:683` на зафиксированном engine). Поэтому положительный
  PACIZA/BLRAAZ тест не доказывает authentication. При реально меняющейся
  подписи новая fixture дополнительно требует отказа на повреждённом PAC;
  в текущем штатном engine она честно сообщает enforcement=unavailable.

Полная инвентаризация идентичностей и публичных slots:
`gpu-bridge/system/18A5351d-abi.json`. В ней намеренно
`hook_manifest_verified=false`, `system_gpu_verified=false`.
Публичный GL trampoline ещё не доказывает точную сигнатуру частного GLI slot:
некоторые exports объединяются, например DeleteShader/DeleteProgram.

Изменённый stock backboardd локально сформирован и подписан через ту же JNI
обёртку, с криптографическим readback. Исходный backboardd тоже прошёл проверку
его первоначальных CodeDirectory page/special-slot hashes до переподписи,
поэтому успешная новая подпись не маскирует повреждение извлечённого файла.
Прямой exec кандидата backboardd в restore ramdisk завершился SIGKILL (9),
как и контрольный запуск stock backboardd. Добавление конкретных CDhashes в
TrustCache, 4 ГиБ RAM, canonical path и stock System chroot это не исправили.
Через `waitid(WNOWAIT)` и `proc_pidinfo(PROC_PIDEXITREASONBASICINFO)` получена
причина ядра: **namespace=9, code=8, flags=0** — `OS_REASON_EXEC` /
`EXEC_EXIT_REASON_SECURITY_POLICY`. По публичному XNU это отказ MAC policy
при image activation (`imgp->ip_mac_return != 0`), до конструктора библиотеки.
Конкретный модуль/условие policy пока не установлены. Нельзя называть это
ошибкой отрисовки, нехваткой RAM или доказанной ошибкой подписи. Entitlements
не расширялись. Отсутствующий liblinkguard — weak dependency, его отсутствие
само по себе не объясняет этот отказ.

Наша **собственная** arm64e программа с LC_LOAD_DYLIB успешно исполняется под
ядром iOS 14 и проверяет SHA-256/HMAC/CRC. Linux Swift 6.2 clang 17 + ld64-609
с `-fptrauth-abi-version=0` сохраняют subtype **0x80000002** до/после подписи.
NDK/LLD ранее молча выдавал arm64 subtype 0; старый Apple clang 11/13 — preview
subtype 2. `check-guest-arch.py` теперь отвергает оба варианта в native-режиме.
Ни opcode control, ни версионированный заголовок при PAC-NOOP не считаются
проверкой защищённого arm64e ABI.

В отдельном disposable System chroot загружен точный stock dyld cache
`11ad39c9-e3c9-3067-a337-7287744fdcb5`. Доступны 6/7 проверенных frameworks:
Foundation, StudyLog, QuartzCore, OpenGLES, IOSurface, IOMobileFramebuffer.
В настоящем Apple EAGLContext проверены установка перехватов, forwarding,
BOOL class-method ABI, вложенный commonInit, повторная установка/снятие и
восстановление оригинального IMP. Маркер:
`EMBER-EAGL: PASS native_class interpose forward restore`.
Родной `initWithAPI:3` возвращает nil: аппаратного GLES frontend в госте пока нет.

Источник System экспортируется read-only. Записи доступны только в новом
ограниченном QCOW2 overlay (128 МиБ physical-write budget), исходный root
пользователя не подключается. У native EAGL-пробы unmount вернул EBUSY (16),
после чего остановлена вся scratch VM; чистое размонтирование **не подтверждено**.
У неуспешного backboardd exec unmount прошёл (0/errno 0). Оба результата
сохраняются отдельно. Это не готовый профиль и не восстановленный образ.
Apple cache/backboardd/ramdisk/ROM/ticket в исходники и выдаваемый архив не входят.

## Оставшаяся реализация и блокеры

1. Дописать настоящий arm64e frontend для нативного EAGLContext backboardd:
   common init, API=3, getMacroContextPrivate, current context, sharegroups,
   параметры, IOSurface texture attach и lifecycle. Слой перехвата нативного
   класса уже проверен, но сейчас он намеренно вызывает исходную реализацию. Диагностический адаптер
   `eagl-adapter.c` из 0.7 для этого не подходит: он регистрирует собственный
   класс только при отсутствии класса Apple и поддерживает API 2.
2. Реализовать/проверить используемые GLI entrypoints с точными типами.
   Старый subset на 64 объекта/16×16 текстуры не подходит. Нужны client-side
   vertex arrays, indexed draw, shader/query semantics, uniform arrays/matrices,
   fences, private Apple operations и преобразования shader extensions.
   Найденные публичные aliases не заменяют эту работу.
3. Соединить настоящие гостевые IOSurface ID, dirty CPU lock/unlock и GPU writes
   с новым host pool; определить финальную поверхность IOMFB/WindowServer,
   ориентацию, fence/lifetime и прямой вывод в Android Surface.
   EGP2 upload и обратный явный CPU-readback уже разбиты на chunks до 64 МиБ.
   Текстуры используют EGLImage: общая storage, отдельные sampler states.
   Установка связей с реальными гостевыми IOSurface ещё отсутствует.
4. Дописать установку в **копию** APFS: maintenance ramdisk, перенос кандидата
   backboardd/dylib/root broker, System launchd cache, journal/readback и
   корректный unmount. Подготовка LC_LOAD_DYLIB сама этого не делает.
   Sandbox доступа настоящего backboardd с сохранёнными entitlements пока
   не проверен; старый тест mobile UID этого не доказывает.
5. Реализовать root broker → NCM/slirp → loopback EGP2, persistent GPU service
   на срок жизни VM, Binder/window lifecycle, crash latch, переключение
   профиля и откат на исходный software root. Постоянный сервис и его VM-клиент
   написаны, но к обычному VmService ещё не подключены. Сквозной системы нет.
6. На установленной iOS проверить SpringBoard/Settings, затем замерить p95
   frame time в обеих сценах при одинаковых настройках и температуре.
   Критерий плана: ≥20% улучшения и отсутствие визуальных ошибок.
   Здесь нет подключённого Android/Adreno и установленного системного гостя;
   эта приёмка недоступна. Это ограничение проверки **отдельно** от ещё
   недописанных компонентов выше. Audio, Metal, GarageBand, iOS15/16 не сделаны.

Нельзя ставить COPIED-профиль активным, возвращать renderer до установки и
проверки frontend или считать HELLO/успешный GLES draw признаком ускорения iOS.
Рабочую установку на телефоне ради этого checkpoint переустанавливать не нужно.

## Воспроизводимые проверки на Linux

В корне InfernoAndroid, JDK 21 / Gradle cache / Android SDK35 / NDK28.2.13676358.
Новые JVM tests не требуют Apple данных. Все пути ниже задаёт запускающий.

```sh
export OUT=/path/to/private/build/egp2
# Linux: C compiler + OpenSSL development headers/libraries.
gpu-bridge/system/build.sh
./gradlew :app:testDebugUnitTest \
  -PsystemGpuPeer="$OUT/egp2-peer-linux" -PtestTmpDir=/large/local/tmp
# Без systemGpuPeer только межъязыковой process test пропускается.

GL_PREFIX=/path/to/mesa/usr OUT=/path/to/private/build/surface \
  gpu-bridge/system/test-surface.sh
GL_PREFIX=/path/to/mesa/usr gpu-bridge/test.sh
python tools/system-gpu/test-tools.py  # dependency: requests, 10 tests
python tools/patch-boot-probe/test-system-results.py  # 5 tests
python gpu-bridge/test-boot-monitor.py  # 12 process-ordering tests
```

Surface-тест использует настоящие EGL/GLES3 вызовы и проверяет pbuffer pixels;
на этой машине backend = Mesa llvmpipe. Это не замер Adreno и не скорость iOS.
Для ASan/UBSan: `TEST_CFLAGS='-fsanitize=address,undefined -fno-pie -no-pie'`;
в Mesa-проверке `LSAN_OPTIONS=detect_leaks=0` (внешние драйверные caches).

Подпись — без Mac/SDK, только наш TAPI ABI stub и pinned ldid:

```sh
git clone https://github.com/ProcursusTeam/ldid /path/to/ldid
git -C /path/to/ldid checkout af86971ae72ec3ed3d0a699107c4e882324c941b
python gpu-bridge/build-codesign.py --ldid /path/to/ldid --java "$JAVA_HOME" \
  --ndk "$ANDROID_NDK_HOME" --out /path/to/private/build/codesign
./gradlew :app:assembleDebug :app:lintDebug \
  -PsystemCodeSigner=/path/to/private/build/codesign/libember_codesign.so
```

Linux JNI verification build: убрать `--ndk`; нужен libplist-dev и libssl-dev.
Можно явно указать `--plist-prefix` и `--plist-library`.
После `:app:compileDebugKotlin`, для caller-owned stock backboardd:

```sh
SIGNER_DIR=/path/to/linux-jni OUT=/path/to/private/cli \
  tools/system-gpu/prepare-backboardd.sh /private/stock-backboardd /private/NEW-candidate
```

Это создаст Apple executable локально; не устанавливает его и не включает GPU.
Полные entitlements и Apple bytes не выводятся в log. SHA-256/UUID жёстко
сверяются до подписи. Native Android library построена, 16К ELF alignment
проверен, присутствие в APK проверено; на Android её JNI ещё не исполнялся.
Лицензии и pinned source hashes — `gpu-bridge/system/licenses/`, build script.

## Исследование stock IPSW без полной распаковки

`tools/system-gpu/remote_member.py` — bounded read-only HTTPS ranges и NBD.
`stock-member.json` содержит URL/offset/size, без ключей и бинарных данных.
`build-reader.py` — адаптер к **предыдущей Linux research-сборке**, а не
самодостаточный сборщик QEMU из чистого checkout. Он ожидает lab's compiled
QEMU block objects, APFS source и `fs-patcher-legacyblock-build-command.json`.
Использованные деревья: ChefKiss Inferno cc4302a99167abec69b714cfd00c38caece7e7de
и Inferno-current 50079bf3a06b5edd4084c49746b3317d1be1a50b.
Это ограничение переносимости инструмента, не Android зависимость установщика.
Если cache уже извлечён доступным способом, весь анализ/генерация независимы:

```sh
python -m pip install requests capstone==5.0.6
python tools/system-gpu/dyld14.py --cache /private/dyld_shared_cache_arm64e \
  --dispatch --out /private/NEW-inventory.json
python tools/system-gpu/generate-cache-plan.py --cache /private/dyld_shared_cache_arm64e \
  --out app/src/main/java/dev/embervm/android/SystemGpuCachePlan.kt --check
```

Конкретные ошибки и обходы:
- QEMU raw offset/size отверг odd-sized DMG: `Specified size is not multiple of 512`.
  Использован точный read-only member descriptor + existing Linux file/NBD shim,
  затем `driver=dmg`, без округления Apple DMG.
- APFS helper искал только GPT4096: `No GPT found`. Stock DMG содержит GPT512;
  добавлен ограниченный разбор этой геометрии и bare NXSB, без изменения Android engine.
- NDK не поддерживает ptrauth и LLD теряет arm64e subtype. Swift 5.8 clang 13
  ещё использует preview ABI (`incompatible arm64e ABI compiler`). Решение:
  зафиксированный Swift 6.2 clang + versioned ABI 0 + ld64 и проверка заголовка.
  Штатный PAC-NOOP engine не изменялся; authentication остаётся непроверенной.
- Direct backboardd exec: SIGKILL / exec security policy (9/8), включая COW
  stock System chroot. Это текущий блокер проверки инъекции в реальный процесс.
- Dyld/CA отсутствуют в restore ramdisk. Один DYLD_ROOT_PATH их не подключил;
  disposable System chroot действительно выбрал нужный cache. Unmount может
  вернуть EBUSY из-за живого shared region; это отражается в result.json.
- Read-only NBD раньше истекал после простоя. Теперь timeout ограничивает
  начатый запрос; простой между запросами разрешён, запись возвращает EROFS.
- Native NoSigner не выставлял CS_ADHOC: криптографический verifier отказал.
  Исправлено flags|CS_ADHOC, flags и hashes проверяются после подписи.
- Первый verifier не учитывал __info_plist special slot: `Unverifiable external
  signing slot`. Добавлен встроенный Info.plist и тесты повреждения этого слота.
- JVM sparse-copy tests требуют свободного резерва: используйте testTmpDir на
  большом filesystem. JAVA_TOOL_OPTIONS не нужен: он загрязняет stdout lock test.

## Сборка гостевой arm64e библиотеки без Mac

```sh
python tools/system-gpu/fetch-toolchain.py --out /private/NEW-linux-toolchain
export GUEST_CLANG=/private/NEW-linux-toolchain/clang/usr/bin/clang
export GUEST_LD=/private/NEW-linux-toolchain/linker/bin/ld
export GUEST_ARCH=arm64e
export OUT=/private/NEW-build
export LDID=/path/to/pinned/ldid
export ANDROID_NDK_HOME=/path/to/ndk
WITH_STOCK_SYSTEM=1 WITH_STOCK_CHROOT=1 tools/system-gpu/build-loader-test.sh
```

Fetcher проверяет полные SHA-256 обоих архивов перед публикацией каталога;
компилятор/ld не запускаются во время скачивания. Нужны ~1,1 ГБ трафика и
~240 МиБ места. Provenance записывается рядом. Apple SDK не используется;
`libSystem.tbd` — собственный список публичных ABI symbols.

## Повторение runtime-проверок

`tools/system-gpu/run-loader-test.sh` требует JAVA_HOME, GRADLE_USER_HOME,
ANDROID_NDK_HOME, pinned LDID, SIGNER_DIR (Linux JNI), NEW OUT, Linux QEMU,
BOOT_INPUTS, KERNEL и STOCK_RAMDISK. Все Apple inputs предоставляет запускающий.
Предварительно: `:app:compileDebugKotlin` для CLI helper classes.

- `MODE=loader-control` (default): NDK arm64, HFS ramdisk + пустой scratch root;
  проверяет own dependency/crypto/opcode roundtrip, без System.
- `MODE=native-loader`: проверяет subtype 0x80000002, own dependency/crypto и
  отдельно сообщает, доступна ли настоящая PAC authentication.
- `MODE=native-eagl`: настоящий stock cache/class, установка/forwarding/снятие
  семи native method hooks. Не создаёт наш API3 context и не ускоряет iOS.
- `MODE=backboardd`: pinned STOCK_BACKBOARDD, добавление зависимости/подпись с
  исходными entitlements и COW canonical path. Ограниченный exec с exit reason;
  в проверенной среде ожидается описанный выше отказ 9/8, не success.

Последние три режима требуют GUEST_CLANG/GUEST_LD, SYSTEM_NBD_SOCKET
(read-only export **декодированного** stock DMG), QEMU_IMG. Если core не умеет
NBD URI, дополнительно SYSTEM_NBD_STUB точного размера 7695611392 и настроенный
file/NBD LD_PRELOAD adapter. Этот adapter и QEMU/APFS research build — внешние
предпосылки стенда, они не создаются wrapper-ом. Их ограничение переносимости
описано выше. Сокет должен существовать; writable export недопустим.

Wrapper сам добавляет LC_LOAD_DYLIB через production Kotlin/JNI helpers,
включает CDhash **получившегося подписанного** executable (не исходника) и
библиотеки в отдельный rtsc, собирает EMBLOAD1 и ramdisk существующим
`build-with-tools.sh`, запускает VM и пишет scoped result.json.
В него никогда не ставятся system_gpu_verified/first_ios_boot_verified=true.
Всё содержимое OUT приватно: там могут быть Apple executable, tickets и
сгенерированный ramdisk. В репозиторий или source-архив оно не включается.

На предыдущем checkpoint: 127 JVM tests / 0 failures / 0 skipped; assembleDebug и lintDebug
успешны. Нативные Surface/EGP2 проверки и Android library compile пройдены;
новый код гостевого перехвата проверен в iOS на Linux. Физического Android для
нового system frontend нет. Development APK этой ветки **не включает** GPU для
обычного запуска и не требует установки на телефон ради этого checkpoint.

## Продолжение: исполнитель хоста и точная граница результата

Добавлен `gpu-bridge/system/host.c`: его код компилируется и для Android JNI,
и для Linux. Полный кадр 828×1792 получен через индексированный draw с шейдерами,
uniform matrix, BGRA IOSurface texture с padded rows и общими контекстами.
EGLImage views действительно разделяют storage, сохраняя независимый sampler;
GPU-запись через view видна в исходной surface. Между transfer/guest/presenter
стоят GPU fences в обоих направлениях. Это Linux Mesa llvmpipe, **программный**
рендерер хоста; не доказательство аппаратного ускорения на телефоне.

Сквозной C-клиент → HMAC EGP2 → Kotlin endpoint → JNI → тот же ES3 исполнитель
обработал и вернул 5935104 байта полного кадра с проверкой SHA-256. Это собственный
C-клиент на Linux, не системный рендерер iOS. Транспорт не подменяет `backboardd`.
Пакетирование вызовов, большие readbacks и отдельный сервис уже реализованы;
гостевой GLI frontend, root broker/NCM, финальная поверхность WindowServer,
установщик/профили/откат и проверка на Android ещё не завершены.

Новый watchdog публикует timeout даже при застрявшем native вызове, включая
EGL initialization. Сервис сам наблюдает это состояние и завершает отдельный
GPU-процесс. Общего диагностического лимита в пять минут нет.

Для отказа exec проведён дополнительный контроль: **наша** программа с точными
entitlements backboardd тоже получила kernel reason 9/8 до main. Удаление только
`seatbelt-profiles` у этой **собственной контрольной программы** позволило ей
войти в main и загрузить Foundation/StudyLog. Дальше launcher остановил её своим
20-секундным пределом. Это сужает отказ до применения named sandbox profile,
но не доказывает, что профиль отсутствует: может отличаться разрешённый parent/
spawn context или доверие к назначению профиля. Права настоящего backboardd не
менялись; отключение его sandbox не является предлагаемым решением.

Формат команд и воспроизводимая сборка: [SYSTEM-GPU-HOST.md](SYSTEM-GPU-HOST.md).
