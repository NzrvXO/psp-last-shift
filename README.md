# LAST SHIFT

Пет-проект: 3D-игра для Sony PSP, написанная с нуля на C поверх нативного PSP SDK. Без движка, без сторонних библиотек и без единого файла ресурсов — текстуры, шрифт, звук и музыка генерируются кодом при запуске.

A pet project: a 3D game for the Sony PSP written from scratch in C on the native PSP SDK. No engine, no third-party libraries and no asset files — textures, font, sound and music are all generated in code at startup.

**[Русский](#русский)** · **[English](#english)**

![platform](https://img.shields.io/badge/platform-Sony%20PSP-blue) ![language](https://img.shields.io/badge/language-C99-orange) ![deps](https://img.shields.io/badge/dependencies-none-lightgrey) ![assets](https://img.shields.io/badge/asset%20files-0-lightgrey)

---

## Русский

### Что это

Интерес был не в игре, а в том, чтобы собрать всё самому на железе 2004 года: графический конвейер, освещение, вывод текста, синтез звука — поверх голого `pspsdk` и `sceGu`, без слоёв абстракции, которые обычно скрывают, как это работает.

Игра — минималистичный склад: собрать заказ из коробок, отнести в зону погрузки, повторить пять раз. Она существует ровно затем, чтобы техническим решениям было что обслуживать.

**Цифры:** `EBOOT.PBP` — 165 КБ · один файл исходников на ~2100 строк · ноль файлов ресурсов · ноль зависимостей.

### Всё генерируется кодом

Игра собирается в один `EBOOT.PBP`, которому нечего искать на карте памяти.

**Текстуры** — три тайлящиеся поверхности 64×64 (картон, бетон, металл), собираются попиксельно при старте: зерно, полосы гофры, деформационные швы, вертикальная шлифовка. Режим `GU_TFX_MODULATE` умножает их на цвет вершины, поэтому освещение и затенение граней работают поверх текстуры.

**Шрифт** — растровый 5×7 на 44 глифа, зашит в таблицу битовых масок. Отрисовывается 2D-квадами, по одному вызову на символ. Горизонтальные отрезки зажжённых пикселей склеиваются в один прямоугольник: строка `#####` — это один квад, а не пять, что примерно втрое сокращает геометрию текста.

**Звук** — синтез в реальном времени в отдельном потоке. Эффекты — меандр по коротким последовательностям нот. Фоновый эмбиент — три расстроенных голоса без терции (поэтому звучит неопределённо, ни мажорно, ни минорно), каждый со своим медленным нарастанием; периоды нарастаний несоизмеримы, так что подложка не зацикливается на слух. Снизу — отфильтрованный однополюсным фильтром шум вместо гула помещения. Синус берётся из таблицы на 256 значений: вызывать `sinf` на каждый сэмпл для нескольких голосов на 44 кГц было бы самой дорогой операцией в игре.

### Решения, продиктованные железом

Самое интересное в проекте — места, где PSP не прощает наивной реализации.

**Освещение считается на CPU.** Аппаратный `GU_LIGHTING` требует нормалей в вершинах, то есть нормаль пришлось бы тащить через каждый меш в игре. Вместо этого свет от ламп вычисляется на процессоре и запекается в цвета вершин пола — он статичен, поэтому пересчитывается только когда лампа гаснет. Фонарь игрока движется каждый кадр и добавляется поверх запечённого уровня, причём тригонометрия считается только для вершин в радиусе конуса — несколько сотен из трёх с половиной тысяч.

**Пол и стены нарезаны на мелкие сегменты.** Аппаратный трансформ PSP не клиппит крупные полигоны, задевающие ближнюю плоскость, — он выбрасывает их целиком. Большой пол исчезал под камерой, а стены обрезались по диагонали. Пол — запечённый меш из 24×24 плиток (3456 вершин, один вызов отрисовки), стены и стеллажи режутся на куски по 2 юнита.

**2D-слой рисуется с отключённым отсечением граней.** В экранных координатах ось Y направлена вниз, поэтому интерфейсные квады намотаны в обратную сторону относительно мировой геометрии — при включённом культинге GPU выбрасывал весь интерфейс целиком.

**Динамические вершины берутся из `sceGuGetMemory`.** Геометрия, которую CPU пишет каждый кадр, живёт внутри текущего display list, а не в собственном буфере — это снимает вопрос сброса кэша данных перед чтением со стороны GE.

**Схема буфера глубины подобрана под матрицу проекции.** Расхожая «перевёрнутая» схема (`GEQUAL` + диапазон 65535→0) не сочетается со стандартной перспективой `sceGumPerspective` и даёт мерцание из-за потери точности; используется прямая. Ближняя и дальняя плоскости сведены к 1 и 100 — 16-битный буфер глубины не прощает диапазона в три порядка.

### Об игре

Ночная смена на складе. Терминал выдаёт заказ, вы ищете нужные коробки, относите в зону погрузки, получаете следующий — пять заказов за смену.

Тип коробки написан на этикетке, и прочитать её можно только при свете, поэтому фонарь — рабочий инструмент, а не украшение; батарея у него не вечная. Лампы периодически выбивает, вернуть свет можно на щитке. К середине смены цифры перестают сходиться.

| Кнопка | Действие |
|---|---|
| Аналоговый стик | Движение |
| **X** | Взять · положить · сдать в зоне погрузки · сбросить щиток |
| **○** | Фонарь |
| **△** (удерживать) | Карточка заказа |
| **R** | Бег |
| **START** | Пауза |

### Сборка

Нужен toolchain **pspdev**. Если он установлен локально:

```bash
make
```

Если нет — Docker собирает, ничего не ставя в систему:

```bash
docker run --rm -v "$(pwd):/build" -w /build pspdev/pspdev:latest make
```

На выходе — `EBOOT.PBP` в корне проекта.

> На Windows в Git Bash нужен префикс `MSYS_NO_PATHCONV=1`, иначе Git Bash подменит пути внутри контейнера.

**Запуск:** `EBOOT.PBP` открывается в [PPSSPP](https://www.ppsspp.org/). На реальной PSP нужна кастомная прошивка — официальная не запускает неподписанный homebrew; файл кладётся в `ms0:/PSP/GAME/LASTSHIFT/EBOOT.PBP`.

### Известные компромиссы

Весь код лежит в одном файле на ~2100 строк. Для такого размера это уже перебор: порядок объявлений начинает диктовать решения, а подсистемы ничем не отделены друг от друга. Разбиение по файлам — ближайшая задача.

Отсечение невидимой геометрии не реализовано: сцена маленькая, рисуется целиком. Анимация персонажа — процедурная, без скелета и кадров.

---

## English

### What this is

The point wasn't the game — it was building everything myself on 2004 hardware: the graphics pipeline, lighting, text rendering, audio synthesis, all on bare `pspsdk` and `sceGu`, with none of the abstraction layers that usually hide how any of it works.

The game is a minimal warehouse: fill an order of boxes, carry them to the loading bay, repeat five times. It exists so the technical work has something to serve.

**Numbers:** `EBOOT.PBP` is 165 KB · one ~2100-line source file · zero asset files · zero dependencies.

### Everything is generated in code

The game builds into a single `EBOOT.PBP` with nothing to look for on the memory stick.

**Textures** are three tiling 64×64 surfaces (cardboard, concrete, metal), built pixel by pixel at startup: grain, corrugation stripes, expansion joints, vertical brushing. `GU_TFX_MODULATE` multiplies them by the vertex color, so lighting and per-face shading survive on top of the texture.

**The font** is a 5×7 bitmap of 44 glyphs baked into a table of bit masks, drawn as 2D quads with one draw call per character. Horizontal runs of lit pixels are merged into a single rectangle, so a row of `#####` is one quad rather than five — roughly a third of the geometry for the same text.

**Audio** is synthesised live on its own thread. Effects are square waves over short note sequences. The ambient bed is three detuned voices with no third — which is why it stays ambiguous rather than sounding major or minor — each swelling on its own slow cycle, with deliberately incommensurable periods so the pad never audibly loops. Underneath sits noise through a one-pole lowpass standing in for room tone. Sine comes from a 256-entry table: calling `sinf` per sample for several voices at 44 kHz would be the most expensive thing in the game.

### Decisions the hardware forced

The interesting part of the project is where the PSP refuses to forgive a naive implementation.

**Lighting is evaluated on the CPU.** Hardware `GU_LIGHTING` needs vertex normals, which would mean carrying a normal through every mesh in the game. Instead the lamps are computed on the CPU and baked into the floor's vertex colors — they never move, so it is only recomputed when a lamp goes out. The player's flashlight moves every frame and is added on top of the baked level, with the trigonometry running only for vertices within reach of the cone: a few hundred out of three and a half thousand.

**Floor and walls are cut into small segments.** The PSP's hardware transform does not clip large polygons that reach past the near plane — it discards them outright. A single large floor vanished under the camera and walls were sliced off diagonally. The floor is a baked mesh of 24×24 tiles (3456 vertices, one draw call); walls and shelving are cut into 2-unit pieces.

**The 2D layer draws with face culling off.** Screen space has Y pointing down, so UI quads wind the opposite way from world geometry — with culling on, the GPU discarded the entire interface.

**Dynamic vertices come from `sceGuGetMemory`.** Geometry the CPU writes each frame lives inside the current display list rather than its own buffer, which removes the question of flushing the data cache before the GE reads it.

**The depth buffer convention matches the projection matrix.** The commonly copied reversed setup (`GEQUAL` with a 65535→0 range) does not pair correctly with a standard `sceGumPerspective` matrix and flickers from lost precision; this uses the forward convention. Near and far are pulled in to 1 and 100 — a 16-bit depth buffer does not forgive a range of three orders of magnitude.

### About the game

A night shift at a warehouse. A terminal hands you an order, you find the boxes, carry them to the loading bay, and the next order comes in — five to a shift.

A box's type is printed on its label and the label can only be read in light, which makes the flashlight a tool rather than a decoration; its battery does not last forever. Lamps trip from time to time and power comes back at the breaker panel. Halfway through the shift the numbers stop adding up.

| Button | Action |
|---|---|
| Analog stick | Move |
| **X** | Pick up · put down · deliver at loading bay · reset breaker |
| **○** | Flashlight |
| **△** (hold) | Order card |
| **R** | Sprint |
| **START** | Pause |

### Building

You need the **pspdev** toolchain. If it's installed locally:

```bash
make
```

If not, Docker builds it without installing anything on your system:

```bash
docker run --rm -v "$(pwd):/build" -w /build pspdev/pspdev:latest make
```

The result is `EBOOT.PBP` in the project root.

> On Windows under Git Bash, prefix with `MSYS_NO_PATHCONV=1`, otherwise Git Bash rewrites the paths inside the container.

**Running:** open `EBOOT.PBP` in [PPSSPP](https://www.ppsspp.org/). Real hardware needs custom firmware — retail firmware will not run unsigned homebrew; the file goes to `ms0:/PSP/GAME/LASTSHIFT/EBOOT.PBP`.

### Known trade-offs

All the code lives in one ~2100-line file. For a project this size that is past the point of being helpful: declaration order starts dictating decisions, and nothing separates the subsystems. Splitting it up is the next task.

There is no visibility culling — the scene is small and drawn in full. Character animation is procedural, with no skeleton and no keyframes.
