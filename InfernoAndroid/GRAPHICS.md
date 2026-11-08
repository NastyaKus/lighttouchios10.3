# Ember VM 0.4.0: экспериментальный GLES-вывод

Рабочая 0.3.1 сохранена тегом `embervm-0.3.1-working` (52a1718f).
Разработка: `codex/graphics-gles`, поверх d9f88345. Архив iPhone 5 не менялся.

## Что реализовано

- CPU по умолчанию; GLES 3 выбирается в настройках на следующий запуск VM.
- Восстановление и отдельный patch-only boot принудительно используют CPU.
- EGL/GLES 3 на отдельном render thread. Текстура RGBA8 получает исходные
  BGRA-байты; fragment shader переставляет R/B и выставляет alpha=1.
- `GL_UNPACK_ROW_LENGTH` и прямоугольник damage: загружаются изменённые строки,
  без CPU swizzle, PBO, glReadPixels или дополнительных потоков копирования.
- На resize/generation/context/surface change первый upload полный. Пока целый
  кадр не получен, не читается неинициализированная память. На FRAME_NONE без
  pending damage/surface refresh нет upload, draw или swap.
- Android AChoreographer синхронизирует оба backend; прежняя задержка
  «работа + 16,7 мс сна» убрана. Guest display timer и IRQ не изменены.
- Владельцем EGL и активного ANativeWindow является только render thread.
  JNI передаёт новый window с refcount/serial; старый освобождается после EGL
  teardown. GPU driver/dequeue/swap не вызываются под window mutex или QEMU BQL.
- EGL creation error → CPU. При ошибке draw/swap допускается одно пересоздание
  контекста за сеанс, повторная ошибка → CPU. Это не перезапускает QEMU.
- Маркер незакрытого GPU-сеанса остаётся при native crash/LMK/force-stop.
  Следующая попытка идёт на CPU. QEMU exit 0 вместе с подтверждённым завершением
  render thread/driver teardown очищает маркер; разрешить
  повтор GLES вручную можно в настройках при остановленной VM. Маркер сам по
  себе не устанавливает причину сбоя: процесс мог завершиться из-за памяти.
- Фактический GLES/резервный CPU виден поверх экрана. Локальные метрики и
  60-секундные benchmark reports включаются в штатный ZIP диагностики.

Это ускорение **показа готового кадра на Android**. Внутри iOS по-прежнему
работают программный рендеринг и pixman. Metal/OpenGLES гостя не включены.
Ни прирост FPS, ни успешная загрузка именно новой версии на телефоне пока
не подтверждены. Подтверждённая загрузка 0.3.1 описана в PHONE-BOOT-VERIFIED.md.

## Как проверить на телефоне

1. Установить APK 0.4.0 поверх 0.3.1: тот же package и debug signing certificate.
   Данные приложения сохраняются. Повторная подготовка/восстановление не нужны.
2. Оставить прежний профиль: 3 AP + SEP (ползунок 4), RAM 4 ГБ, 828×1792,
   TB 512 МБ, split-wx выключен. Не менять boot-args между замерами.
3. Сначала выбрать CPU. Запустить уже установленную iOS, дождаться SpringBoard
   и ещё минуты покоя. Открыть консоль кнопкой терминала → «Записать замер ·
   60 с». Панель закроется, отсчёт появится сверху.
4. В течение минуты повторять один сценарий: листать домашние экраны,
   прокручивать один и тот же список Settings, открывать/закрывать одинаковые
   окна. Сделать три замера. Не сворачивать приложение и не поворачивать экран.
5. Остановить VM, выбрать GLES 3, снова запустить и повторить те же три замера
   после прогрева. Проверить надпись «GLES3 · вывод». «CPU · резерв» означает,
   что GPU-путь не активен; смотреть fallback_reason в диагностике.
6. На экране восстановления сохранить диагностику обычной кнопкой. Там будут
   `graphics/bench-*.json`, `latest.json`, `latest.csv`, обычные VM engine/guest
   logs и Android exit history. Для сохранения диагностики не нужно запускать
   восстановление. Прошивки, ключ SEP, ticket и диски в архив не включаются.
7. 414×896 проверять отдельной серией, с теми же остальными настройками.
   Не сравнивать результаты разных разрешений как эффект одного backend.

Избегать сильного нагрева и сравнивать при одинаковых условиях питания.
Сильный thermal throttling делает серию непригодной. При проблемах выбрать
CPU и перезапустить VM; гостевой комплект не переподготавливать.

## Метрики и критерий

Engine patch 0004 добавляет **optional** `inferno_display_metrics_read_v1(out,
size)`: struct version/size, cumulative uint64 counters и 128-bin histogram.
Старые FrameInfo, display_read, display_stats и аргументы гостя не изменены.
JNI ищет новый symbol через optional dlsym: старый engine остаётся совместим,
но без новой диагностики результат benchmark неполон.

- `pixman_cpu_ns`: CLOCK_THREAD_CPUTIME_ID непосредственно вокруг composite.
- `copy_cpu_ns` / `copy_wait_ns`: CPU копирования damage / wall wait mutex
  display_read. `copy_bytes` — реально скопированные байты.
- `main_cpu_ns`: CPU QEMU main thread от attach, сэмплируется его callbacks;
  это не wall time и не сумма CPU vCPU/TCG.
- `present_cpu_ns`: CPU render thread вокруг CPU/GLES presentation; отдельно
  `swizzle_cpu_ns`, `window_lock_wall_ns`, `present_wall_ns`.
- `read_wall_ns` включает время вызова display_read. `upload_bytes`, context
  creations, fallbacks, surface changes; host/guest frame counts отдельно.
- Guest frame gaps: монотонные host timestamps guest commits, histogram bins
  по 1 мс, последняя >=127 мс. Это не заявленная частота дисплея iPhone и не
  end-to-end touch latency. p50/p95 — верхние границы соответствующего bin.
- CPU submission/queue timings **не являются GPU execution timestamps**.
  Подробные строки vendor/renderer/GL version записываются при GLES init.

Сэмплирование раз в секунду не сбрасывает FPS-счётчики. CSV ограничен первыми
900 строками сеанса; snapshot обновляется дальше. Хранятся последние 12 полных
benchmark reports, поэтому шесть замеров каждого разрешения нужно экспортировать
до начала следующих. Смена surface/backend в течение замера делает его неполным.

После извлечения шести JSON одной серии:

```sh
python3 tools/compare-graphics.py graphics/bench-*.json
```

Promotion criterion: медиана CPU времени presentation на показанный кадр
снижается >=20%, а медианный guest frame gap не хуже CPU более чем на 10%.
Скрипт требует три подходящих результата каждого backend, проверяет профиль,
fallback/thermal состояние и учитывает неопределённость histogram bin.
Вывод «критерий не подтверждён» не равен доказательству регрессии.
CPU ms/second также приводятся: рост количества кадров сам по себе не выдаётся
за экономию CPU.

## Проверки в Linux и ограничения

```sh
GL_PREFIX=/usr tests/test-gles.sh       # EGL/GLES3 headers + Mesa/llvmpipe
python3 tests/test-graphics-report.py
./gradlew :app:testDebugUnitTest :app:assembleDebug :app:lintDebug
```

Результаты конкретной сборки — GRAPHICS-VALIDATION.json. EGL test компилирует
тот же gles_renderer.c, что APK: BGRA/alpha/orientation, padded stride, dirty
rectangle, retained texture, bounds, resize/recreate и injected swap failure.
Он работает на Mesa software driver. Это не Android instrumentation test,
не прогон Adreno, не проверка SurfaceFlinger или автоматического JNI fallback
целиком. Android surface loss/rotation, background/foreground, driver context
loss и native-crash latch требуют проверки на телефоне. Здесь нет Android
устройства или эмулятора. Все открытые проверки перечислены в JSON.

В среде сборки исправлены две конкретные проблемы: Meson не находил свой Python
module (нужен PYTHONPATH при локальной распаковке), find не обходил symlink
build directory (build-engine.sh теперь использует find -H). Для host EGL test
не было GLES headers/libGLESv2; Debian libglvnd пакеты распакованы локально,
без изменения Android dependencies. Первый host link завершился
`cannot find -lEGL`; исправлена локальная runtime library symlink.

## Следующие этапы: условия начала

**2D offload:** только если pixman занимает >=15% CPU QEMU main loop в GLES
сериях. Сейчас перенос не сделан: реальных замеров этой версии нет. Проект
требует bounded snapshot queue вне BQL/GP lock, копирования guest DMA в собственные
буферы, тех же scale/filter/source semantics, корректного FRAME_PROCESSED IRQ,
dirty/generation handling и golden-image сравнения с pixman. Нельзя отдавать
GPU указатель на изменяемую guest RAM или держать BQL во время driver wait.
В текущем model у нескольких planes PIXMAN_OP_SRC и TODO source/dest positions;
переписывание как обычного alpha compositor изменило бы поведение гостя.

**Настоящий guest GPU bridge:** отдельный опыт на копии гостевого диска.
В pinned MakrSas/Inferno b76fa575 модель вывода — apple_displaypipe_v4, готового
AGX renderer/driver bridge нет. Исходный InfernoFSPatcher (3008aaaa, main.cpp
149–174, 366+) отключает Metal и предположения о доступности OpenGLES, чтобы
работал software path. Удаление этих патчей само по себе не создаст устройство.

Первый исследовательский прототип должен проверить один IOSurface/shared buffer
с явными ownership/synchronization и readback, затем один ограниченный GL ES
draw через guest-to-host command transport. Для SpringBoard понадобятся
совместимость CoreAnimation/compositor, IOSurface mappings/formats/fences,
shader translation и интеграция в iOS user/kernel graphics interfaces.
OpenGL ES/Vulkan хоста, virgl или ANGLE не предоставляют iOS-драйвер автоматически.
Включение Metal, эмуляция полного AGX, игры и KVM в эту сборку не входят.

Исходники ориентира: https://github.com/MakrSas/Inferno/tree/b76fa575831d94a80db169ffb3c76d9094a648bd
и https://git.chefkiss.dev/AppleHax/InfernoFSPatcher (точная используемая ревизия
указана в SOURCE-HANDOFF.md). Подмена engine base не требуется для данного этапа.
