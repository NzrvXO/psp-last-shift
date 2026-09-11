# LAST SHIFT

Minimalist 3D warehouse game for the Sony PSP, written in C against the native PSP SDK.

**[Русский](#русский)** · **[English](#english)**

![platform](https://img.shields.io/badge/platform-Sony%20PSP-blue) ![language](https://img.shields.io/badge/language-C-orange) ![build](https://img.shields.io/badge/build-EBOOT.PBP-green)

---

## Русский

### Об игре

Вы работаете один в ночную смену на небольшом складе. Терминал выдаёт заказ, вы находите нужные коробки среди стеллажей, относите их в зону погрузки и получаете следующий. Всего за смену — пять заказов.

Свет на складе неровный: под лампами светло, между ними полумрак, а лампы ещё и выбивает. Тип коробки написан на этикетке, и **прочитать её можно только при свете** — поэтому фонарь в руках не украшение, а рабочий инструмент. Батарея у него не вечная.

К середине смены цифры перестают сходиться. Врагов, погонь и скримеров нет — только свет, звук и мелкие несоответствия.

### Управление

| Кнопка | Действие |
|---|---|
| Аналоговый стик | Движение |
| **X** | Взять коробку · положить · сдать в зоне погрузки · сбросить щиток |
| **○** | Включить/выключить фонарь |
| **△** (удерживать) | Показать карточку заказа |
| **R** | Бег (расходует выносливость) |
| **START** | Пауза |

В меню: **↑/↓** — выбор, **X** — подтвердить.

### Правила

- 5 заказов за смену, по 3 минуты на каждый
- 100 очков за сданную коробку
- Бонус: 2 очка за каждую оставшуюся секунду при выполнении заказа
- Между заказами склад пополняется заново — коробки появляются в других местах и в другом количестве
- Заряда фонаря хватает примерно на 90 секунд непрерывной работы, батарейки лежат на складе
- Лампы периодически выбивает; вернуть свет можно на щитке у западной стены

### Сборка

Нужен toolchain **pspdev**. Если он установлен локально, достаточно:

```bash
make
```

Если toolchain не установлен, проще собрать в Docker — ничего ставить в систему не придётся:

```bash
docker run --rm -v "$(pwd):/build" -w /build pspdev/pspdev:latest make
```

На выходе — `EBOOT.PBP` в корне проекта.

> На Windows в Git Bash перед `docker` нужен `MSYS_NO_PATHCONV=1`, иначе Git Bash подменит пути внутри контейнера.

### Запуск

**В эмуляторе:** откройте `EBOOT.PBP` в [PPSSPP](https://www.ppsspp.org/).

**На реальной PSP:** нужна кастомная прошивка — официальная не запускает неподписанный homebrew. Скопируйте файл на карту памяти так:

```
ms0:/PSP/GAME/LASTSHIFT/EBOOT.PBP
```

Затем отключите USB и откройте `Игра → Memory Stick`.

### Как всё устроено

Игра собирается в один `EBOOT.PBP` без единого файла ресурсов — всё содержимое генерируется кодом при запуске:

- **Текстуры** (картон, бетон, металл) — процедурные, тайлятся, модулируются цветом вершин
- **Шрифт** — растровый 5×7, глифы рисуются 2D-квадами; горизонтальные отрезки пикселей склеиваются в один прямоугольник
- **Звук и музыка** — синтез на лету: эффекты меандром, фоновый эмбиент — три расстроенных голоса без терции со своими медленными нарастаниями, поверх отфильтрованный шум

Несколько решений, продиктованных особенностями железа:

- **Освещение считается на CPU**, а не через `GU_LIGHTING`: аппаратный свет требует нормалей в вершинах, что означало бы тащить нормаль через каждый меш. Вклад ламп в пол запекается по вершинам (он статичен), фонарь добавляется поверх каждый кадр и только для вершин в радиусе конуса.
- **Пол и стены нарезаны на мелкие сегменты.** Аппаратный трансформ PSP не клиппит крупные полигоны, задевающие ближнюю плоскость, — он выбрасывает их целиком, и большой пол исчезал под камерой.
- **2D-слой рисуется с отключённым отсечением граней**: в экранных координатах ось Y направлена вниз, поэтому интерфейсные квады намотаны в обратную сторону относительно мировой геометрии.

### Структура проекта

```
src/main.c      весь исходный код игры
Makefile        сборка через pspsdk
assets/         пусто — всё генерируется кодом
prompt.md       исходное техническое задание
```

Весь код сейчас лежит в одном файле на ~2100 строк. Для проекта такого размера это уже перебор: порядок объявлений начинает диктовать решения, а подсистемы ничем не отделены друг от друга. Разбиение по файлам (аудио, рендер, мир, заказы, интерфейс) — ближайшая задача.

### Технологии

C · PSP SDK (pspsdk) · PSP GU · sceCtrl · sceAudio. Без внешних зависимостей, движков и файлов ресурсов.

---

## English

### About

You work the night shift alone in a small warehouse. A terminal hands you an order, you find the right boxes among the shelving, carry them to the loading bay, and the next order comes in. Five orders to a shift.

The lighting is uneven: bright pools under the lamps, gloom between them, and the lamps trip on their own. A box's type is printed on its label, and **the label can only be read in light** — which makes the flashlight a tool rather than a decoration. Its battery does not last forever.

Halfway through the shift the numbers stop adding up. There are no enemies, no chases and no jump scares — only light, sound, and small discrepancies.

### Controls

| Button | Action |
|---|---|
| Analog stick | Move |
| **X** | Pick up box · put down · deliver at loading bay · reset breaker |
| **○** | Toggle flashlight |
| **△** (hold) | Show order card |
| **R** | Sprint (drains stamina) |
| **START** | Pause |

In menus: **↑/↓** to select, **X** to confirm.

### Rules

- 5 orders per shift, 3 minutes each
- 100 points per delivered box
- Bonus: 2 points for every second left when an order is filled
- The warehouse restocks between orders — boxes appear in new places and in different quantities
- A full battery lasts about 90 seconds of continuous light; spares are scattered around the warehouse
- Lamps trip from time to time; power comes back at the breaker panel on the west wall

### Building

You need the **pspdev** toolchain. If it is installed locally:

```bash
make
```

If it isn't, Docker builds it without installing anything on your system:

```bash
docker run --rm -v "$(pwd):/build" -w /build pspdev/pspdev:latest make
```

The result is `EBOOT.PBP` in the project root.

> On Windows under Git Bash, prefix the command with `MSYS_NO_PATHCONV=1`, otherwise Git Bash rewrites the paths inside the container.

### Running

**In an emulator:** open `EBOOT.PBP` in [PPSSPP](https://www.ppsspp.org/).

**On real hardware:** custom firmware is required — retail firmware will not run unsigned homebrew. Copy the file to the memory stick as:

```
ms0:/PSP/GAME/LASTSHIFT/EBOOT.PBP
```

Then disconnect USB and open `Game → Memory Stick`.

### How it works

The game builds into a single `EBOOT.PBP` with no asset files at all — everything is generated in code at startup:

- **Textures** (cardboard, concrete, metal) are procedural, tiling, and modulated by vertex color
- **Font** is a 5×7 bitmap drawn as 2D quads, with horizontal runs of pixels merged into single rectangles
- **Audio** is synthesised live: square-wave effects, and an ambient bed of three detuned voices with no third, each swelling on its own slow cycle, over filtered noise

A few decisions were driven by the hardware:

- **Lighting is evaluated on the CPU** rather than through `GU_LIGHTING`, which needs vertex normals and would mean carrying a normal through every mesh. The lamps' contribution to the floor is baked per vertex since it never moves; the flashlight is added on top each frame, and only for vertices within reach of the cone.
- **Floor and walls are cut into small segments.** The PSP's hardware transform does not clip large polygons that reach past the near plane — it discards them outright, and a single large floor vanished whenever the camera came near it.
- **The 2D layer draws with face culling off**: screen space has Y pointing down, so UI quads wind the opposite way from world geometry.

### Project layout

```
src/main.c      all of the game's source
Makefile        pspsdk build
assets/         empty — everything is generated in code
prompt.md       the original specification
```

All the code currently lives in one ~2100-line file. For a project this size that is past the point of being helpful: declaration order starts dictating decisions, and nothing separates the subsystems. Splitting it up (audio, renderer, world, orders, UI) is the next task.

### Built with

C · PSP SDK (pspsdk) · PSP GU · sceCtrl · sceAudio. No external dependencies, no engine, no asset files.
