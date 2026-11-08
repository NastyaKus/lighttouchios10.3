> Снимок 0.4.0 расширяет установщик 0.3.1 модулем вывода/профилирования; ветка `codex/graphics-gles`, engine patch 0004. См. GRAPHICS.md и GRAPHICS-VALIDATION.json. Сведения ниже описывают исходную рабочую 0.3.1; восстановление и guest tools в этой ветке не менялись.

# Исходники подготовки и восстановления Ember VM 0.3.1

Рабочий APK собран из 52a1718f1083be7cbcb7c17661a45fae75a66252. В снимок
добавлены только документы о подтверждённом запуске/составе исходников.
База порта Android: NastyaKus/lighttouchios10.3 @
d453a1b4563dffd8d90f9a43b86ab45b15580e8d, исходная ветка ccr-38a9e977-ffrrse.
Рабочая локальная ветка: codex/inferno-android-restore. Архив iPhone 5 исключён.

## Карта кода

- app/src/main/java/dev/embervm/android/restore/: DER/IMG4, AP/SEP tickets,
  AES SEP firmware, ZIP/ZIP64/ranges/CRC, HFSX ramdisk editing, подготовка девяти
  дисков, USB/usbmux/TCP/ASR/restored, сохранение прогресса, контроль завершения.
- RestorePrepService, RestoreActivity, VmService, VmConfig и UI в app/src/main/:
  выбор локальных входов, staging, процессы, параметры QEMU, запуск и диагностика.
- guest/: исходник координатора, bounded binary-plist editor launchd cache,
  карта 45 dyld замен, публичные Darwin ABI declarations, TAPI link interface,
  Linux build.sh и проверяемый GPL Mach-O. Apple SDK для этого C кода не нужен.
- engine/: сборка зависимостей/движка и три Android patch файла, включая
  исправление coroutine context/PAC. Исходный QEMU загружается по точному commit.
- app/src/test/ и tools/: JVM, synthetic USB/ASR/форматы, Swift parity,
  C/bplist/sanitizer tests, HFSX/guest probes, фикстуры и генераторы.
- PHONE-*.md, RESTORE.md, VALIDATION.md: причины ошибок и границы проверок.
  Итог реального телефона: PHONE-BOOT-VERIFIED.md.

## Последовательность восстановления

1. Локальные IPSW/ticket/SEP ROM/key, строго 18A5351d n104ap Erase.
2. Подготовка в restore-staging: девять sparse дисков, компоненты, IMG4 tickets,
   SEP и ramdisk. Это ещё не загрузочный установленный iOS.
3. Stock restore ramdisk → USB/restored/ASR → Status 0/ACK → host stop/checkpoint.
4. Отдельный patch-only boot, nvmefwupdater --skip, без USB restore client.
5. Readback всех 45 patch sites. Если полностью совпали, dyld cache не
   переписывается; иначе штатный C++ patcher с обязательным readback после exit 0.
6. На System меняются Disabled flags ровно пяти служб в launchd binary cache,
   с оригинальной копией/atomic write/readback. Data на этом этапе не монтируется.
7. sync, clean System unmount, cleanup/done, host engine exit 0, активация.
8. Обычный запуск iOS. Он уже подтверждён пользователем на одном телефоне;
   совместимость с другими устройствами и полный набор функций не проверены.

## Сборка Android на Linux

Нужны JDK 17 или 21, SDK/platform/build-tools 35, NDK 28.2.13676358,
CMake 3.22.1, meson/ninja/pkg-config, autoconf/automake/libtool/m4, Python 3,
Git/curl и доступ к pinned dependency sources. В наших проверках: JDK 21,
целевой bytecode 17; Gradle wrapper 8.11.1, Kotlin 2.1.20, AGP 8.9.2.

```sh
export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/28.2.13676358
export JOBS=2
InfernoAndroid/engine/build-deps.sh
InfernoAndroid/engine/build-engine.sh
cd InfernoAndroid
printf 'sdk.dir=/path/to/android-sdk\n' > local.properties
./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
# app/build/outputs/apk/debug/app-debug.apk
```

Нужен интернет для Gradle/engine dependencies и pinned guest-tools IPA.
Проект сам проверяет hashes и не получает Apple IPSW/ROM/key автоматически.
Без собранного engine Gradle может собрать APK, который не запустит VM;
проверять ENGINE_BUNDLED и наличие build/engine/libqemu-aarch64-softmmu.so.
Исходный .github workflow приложен отдельно как CI reference; для автономного
репозитория его нужно положить в .github/workflows/. Он не публиковался нами.

Пересборка C daemon без Mac:

```sh
CLANG="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" \
MACHO_LD="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/ld64.lld" \
  guest/build.sh
```

При смене LLVM SHA-256 бинарника может измениться. Проверить результат,
обновить daemonHash в app/build.gradle.kts и собрать APK. Приватный ключ
подписи прежнего APK в исходники не включён: чужая debug подпись обычно
не позволит установить самостоятельную сборку поверх нашей без миграции данных.
Не удалять рабочую установку ради проверки исходников.

## Проверки

```sh
./gradlew :app:testDebugUnitTest :app:lintDebug
python3 tools/guest-daemon-test/run.py
python3 tools/launchd-cache-test/run.py
EMBER_SANITIZE_FLAGS='-fsanitize=address,undefined -fno-pie -no-pie' \
  python3 tools/launchd-cache-test/run.py
```

Последний релиз: 73 JVM, 15 daemon, 22 bplist tests; bplist также ASan+UBSan.
Guest probe требует caller-owned Apple boot inputs и создаёт только НОВЫЕ
scratch disks; он не выполняет настоящий restore и не использует рабочий root.
Инструкции tools/guest-daemon-test, tools/patch-boot-probe и guest/README.md.

## Внешние исходники

- QEMU fork: https://github.com/MakrSas/Inferno @
  b76fa575831d94a80db169ffb3c76d9094a648bd. Полный upstream engine не vendored
  в Android-проект: build-engine.sh получает его и применяет наши patches.
- Swift port reference: https://github.com/MakrSas/Inferno-iOS @
  4bc39702871f5a118d96acbad4df7a675ddf14e0. В source archive приложены
  использованные Swift файлы и handoff, без Apple resource templates.
- inferno_fs_patcher в APK берётся из Inferno-iOS v0.3.1 IPA с SHA-256
  c66b56de80d55b989df1c70ebcf4b321751a6703a9d253c9ff349f434fd7c8dc.
  Его бинарник не менялся. Приложен доступный source reference InfernoFSPatcher
  3008aaaa0909824a71ee3040c9edc58703c250f5 (AGPL); побайтовая воспроизводимость
  IPA-бинарника из этого checkout/другого toolchain не установлена.

В архиве нет ОС iOS/дисков, Apple firmware/SEP ROM/tickets/ключей, пользовательской
диагностики, SDK/Gradle caches или приватного signing key. Pinned URLs/hashes,
наши полные исходники/patches и synthetic tests включены. GitHub push ранее
вернул 403 Resource not accessible by integration; изменения сохранены локально,
для переноса прилагаются consolidated diff и отдельные commit patches.
