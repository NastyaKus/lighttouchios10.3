# Ember VM 0.6.0-gpupresent

Добавлен экспериментальный путь **guest renderbuffer → GPU blit → Android
SurfaceView**. Существующие приложения iOS, SpringBoard, Metal и GarageBand
ещё не подключены. Это отдельный тестовый гость, рабочий образ не меняется.

## Подтверждённый телефонный прогон

6 ноября проверена присланная диагностика 0.6.0 на Android API 36, Qualcomm /
Adreno 740. Согласованы guest console, 316 последовательных transport commands
без ошибок, host.log и JSON-отчёты. Подтверждены 40 GLES checks и 191 present
check, 60 window presents, одно создание Surface и отсутствие её потерь.

PixelCopy успешно захватил кадр serial=1: четыре контрольных цвета совпали
точно, включая цвет из nonce текущего guest-канала. Run ID и поколение Surface
совпали. Это независимая проверка четырёх точек одного window-кадра; каждый
пиксель всех 60 кадров не сравнивался. QEMU завершился с кодом 0, контрольные
области scratch root сохранились. Общий запуск занял 70,278 секунды вместе с
подготовкой, загрузкой и намеренными задержками анимации; FPS этим не измерялся.

Обезличенная сводка — [gpu-bridge/SURFACE-VALIDATION.json](gpu-bridge/SURFACE-VALIDATION.json).
Оригинальные ZIP, логи, идентификаторы и nonce в репозитории не сохраняются.
GPU-PRESENT-VALIDATION.json ниже остаётся историческим снимком локальных
проверок перед этой телефонной диагностикой. Подтверждён один обычный window
прогон; повтор, фон, поворот, отмена и восстановление после ошибок ещё не проверены.

## Проверка на телефоне

1. Остановить VM, распаковать обновление и установить APK поверх Ember VM.
   Удалять приложение и повторять восстановление не требуется.
2. Открыть «GPU-стенд · тест команд iOS».
3. Нажать новую кнопку **«Показать GPU-кадр гостя»** рядом с окном предпросмотра.
4. Оставить экран стенда открытым. После загрузки тестовой iOS должны появиться
   четыре цветные области и движущийся оранжевый треугольник примерно на 6–10 секунд.
   Верхний левый цвет меняется между запусками; остальные — зелёный, синий и белый.
5. Дождаться PASSED/FAILED и сохранить ZIP диагностики. В нём дополнительно есть
   `display-result.json` с результатом PixelCopy и ожидаемыми/фактическими цветами.

Старая кнопка «Проверить гостевые GPU-команды» оставлена для offscreen теста.
В новом режиме PASSED требует одновременно старые 40 проверок, 60 успешных
window presents, подтверждение гостя и совпадение PixelCopy с текущим запуском
и поколением Surface. Одного успешного eglSwapBuffers недостаточно.

Если треугольник виден, а тест не пройден, сохранить ZIP: возможна ошибка
цветового пространства, ориентации или управления Surface. Исчезновение Surface
при уходе с экрана может прервать тест. Это не повод заново восстанавливать iOS.

## Что реализовано

- Ограниченный настоящий GLES FBO/RBO API: generation, binding, RGBA8 storage,
  attachment, completeness, размер, удаление. Backend использует GL objects;
  host GLuint гостю не передаются. Лимит RBO 512×512, остальных ресурсов — как в GLES-API.md.
- `EAGLContext renderbufferStorage:fromDrawable:` и `presentRenderbuffer:` для
  **нашего EmberDiagnosticDrawable**. Это явный диагностический объект, не
  реализация CAEAGLLayer/QuartzCore. Подмена штатного EAGLContext по-прежнему запрещена.
- Wire command 0x102 PRESENT: renderbuffer ID + возрастающий serial. Ответ
  содержит outcome, serial, счётчики и четыре sample colors. Replay последнего
  transport sequence возвращает старый ответ, не выполняя повторный swap.
- Отдельный EGL context делит renderbuffer с контекстом команд гостя. Producer
  fence + glFlush и consumer glWaitSync упорядочивают доступ. GL state гостя
  сохраняется при возвращении в его контекст.
- GPU копирует renderbuffer в mirror texture, затем в window framebuffer.
  Полного кадра через CPU на этом пути нет. Диагностика читает четыре пикселя
  на present; PixelCopy один раз на поколение Surface считывает Bitmap 64×64.
  VBO/texture uploads приложения — отдельные операции, не включённые в счётчик
  CPU upload пути present. Это GPU copies, а не заявление полного zero-copy.
- Surface передаётся через Binder в изолированный :gpu процесс. Только его
  worker вызывает EGL. ANativeWindow references удерживаются отдельно для
  control plane и renderer; смена поколения пересоздаёт presenter.
- :gpu следит за смертью Binder владельца :gpulab, даже если preview UI остаётся
  привязанным. Каждый завершённый/отменённый запуск заканчивает одноразовый
  GPU process, следующий получает свежий executor.
- Контрольный цвет получен из nonce scratch-канала. PixelCopy дополнительно
  проверяет run ID, generation и frame serial: старый буфер не принимается
  за результат нового запуска. Публикуются только цвета теста, не screenshot iOS.

## Проверки и их границы

Linux Mesa llvmpipe: 60 headless presents и 191 дополнительная проверка,
поверх прежних 40. Реальный iOS 14 release kernel также прошёл 316 transport
commands: 1262 API calls, 85 batches, 64 draws, 7 compiler invocations (включая
намеренно ошибочный shader). Mirror samples проверены гостем. Это pbuffer,
`window_frames=0`, не подтверждение Android Surface.

Native tests проверяют shared RBO colors, сохранение producer context/state,
exactly-once replay, отказ без обязательного window и пересоздание headless
presenter. ASan/UBSan включены для native unit и полного C client roundtrip.
JVM tests проверяют формат статистики, RGBA/origin, отсутствие ложного успеха
для pbuffer, чужого run/generation/serial и неверных цветов. JNI fixture проверяет
счётчики через реальный desktop JVM и тот же JNI wrapper.

Android Surface/Binder lifecycle и PixelCopy здесь **не запускались на телефоне**.
Предыдущие два успешных Adreno-прогона 0.5.0 подтверждают offscreen часть, но
не доказывают работу нового window path. APK сборка не считается таким доказательством.
Результаты сборки и проверок — GPU-PRESENT-VALIDATION.json.

Анимация намеренно ограничена задержкой 100 ms в госте и scratch transport
polling. 60 кадров — длина теста, не 60 FPS и не benchmark ускорения iOS.

## Воспроизведение

Из InfernoAndroid, с JDK 21, NDK 28.2.13676358, ldid и EGL/GLES headers:

```sh
gpu-bridge/build.sh
ANDROID_NDK_HOME=/path/to/ndk LDID=/path/to/ldid gpu-bridge/build-gles-client.sh
gpu-bridge/test.sh
python3 gpu-bridge/integration-test.py --build build/gpu-bridge \
  --client build/gles-client/present-api-probe-linux --gles-api --present
python3 gpu-bridge/test-boot-monitor.py
JAVA_HOME=/path/to/jdk gpu-bridge/jni-test/build.sh
JAVA_HOME=/path/to/jdk EGPU_JNI_BUILD="$PWD/build/gpu-jni" \
  EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
  python3 gpu-bridge/jni-test/test.py --client build/gles-client/present-api-probe-linux --present
./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
```

Реальный guest boot использует собственные Apple inputs и NEW sparse directory,
как описано в GPU-LAB.md/GLES-API.md. В build-with-tools.sh первым override
передать `build/gles-client/eagl-present-probe-ios`, вторым — нашу dylib.
В patch-boot-probe/run.py добавить `--gpu-present` к `--gpu-gles-api --gpu-eagl`.
Этот CLI проверяет headless presents и явно сохраняет android_surface_verified=false.

Дальше остаются реальный телефонный window тест, расширение API/ресурсов и
transport, guest broker/driver, IOSurface/compositor и ABI графики приложений.
Системные software patches и основной QEMU engine этим изменением не тронуты.
