# Тест параллакса «по клеткам» (подход IHP) на тестовом объекте

Ветка `feature/parallax-test` (от `feature/terrain-v4`). Планета не меняется: её материал вызывает
`PlanetSurface`, а он теперь вызывает `PlanetSurfaceEx` с выключенным параллаксом, поэтому
картинка и стоимость прежние.

## Что это

Луч от камеры уходит в рельеф карт высот двух главных слоёв. Текстуры пикселя читаются там, где
луч его встретил: камни перекрывают друг друга, щели между ними уходят вглубь. Вершины не
двигаются, коллизия не меняется, силуэт остаётся плоским.

Есть три режима (`PxMode`), чтобы сравнивать на одном кадре:

| PxMode | Что | Как ищет пересечение |
|---|---|---|
| 0 | выкл | — |
| 1 | обычный POM | равными шагами по фильтрованной высоте, потом уточнение; тонкий гребень между шагами пропускается |
| 2 | по клеткам | проходит текстуру тексель за текселем и точно решает пересечение внутри каждого (квадратное уравнение) |

На проверочном стенде режим 2 совпал с перебором до 3·10⁻⁵ глубины на всех 1200 случайных лучах.
Обычный POM с 16 шагами ошибся больше чем на 5 % глубины в 44 случаях из 1200, худший — на 64 %.

Код: `GpuErosion/Shaders/PlanetTerrainParallax.ush` (трассировщики) и `PlanetSurfaceEx` в
`GpuErosion/Shaders/PlanetTerrainSurface.ush`.

## 1. Файлы

Скопируйте в `Source/FastNoiseTest/`:

- `GpuErosion/Shaders/PlanetTerrainParallax.ush` — новый;
- `GpuErosion/Shaders/PlanetTerrainSurface.ush` — обновлён;
- `MaterialHLSL/ParallaxTest_Weights.hlsl`, `MaterialHLSL/ParallaxTest_Surface.hlsl`, этот файл — новые;
- `Tools/parallax_harness.cpp.txt` — проверочный стенд (для меня, в сборку не попадает).

C++ не менялся, пересобирать проект не нужно. **Перезапустите редактор**: в шейдерной папке
появился новый файл.

## 2. Тестовый уровень

1. **File → New Level → Basic.** В нём уже есть Directional Light (Atmosphere Sun Light), Sky
   Atmosphere, Sky Light и туман. Без Sky Atmosphere не будет микротеней:
   `SkyAtmosphereLightDirection` вернёт ноль.
2. **Сфера — мини-планета.** Place Actors → Shapes → Sphere. Scale 400 (радиус 200 м), Location Z −20000:
   верх сферы будет около нуля.
   - Зачем сфера: текстуры проецируются на мировые оси X/Y/Z, а нормаль на сфере смотрит во все
     стороны. Как на планете, почти везде проекция «косая» относительно поверхности, а на
     широтах около 45° стыкуются две плоскости проекции. Эти места и нужно проверить.
3. Для сравнения можно положить плоскость (Shapes → Plane, Scale 100) с поворотом, например (20, 35, 0).
4. Освещение: оставьте как в уровне планеты. Удобнее всего скопировать её Directional Light и
   Post Process Volume, чтобы экспозиция и солнце совпадали.

## 3. Материал `M_ParallaxTest`

1. В Content Browser **Duplicate** `M_PlanetTerrainGPU` → `M_ParallaxTest`. Все параметры, текстуры
   и UV-ноды уже на месте.
2. В копии **удалите связь World Position Offset**: вершинный шейдер планеты читает атласы чанков,
   на сфере их нет.
3. **Веса слоёв вместо атласа биомов.** Новая нода **Custom**, код из `ParallaxTest_Weights.hlsl`:
   - Output Type **CMOT Float 4** (по умолчанию стоит Float 3 — обязательно поменять);
   - Inputs: `LayerA`, `LayerB`, `Mix`, `BlendUV`;
   - Additional Outputs: `OutW1`, Output Type **CMOT Float 4** (по умолчанию Float 1).

   Подключите:

   | Вход | Нода | Значение по умолчанию |
   |---|---|---|
   | `LayerA` | Scalar Parameter `LayerA` | 7 (Rock) |
   | `LayerB` | Scalar Parameter `LayerB` | 2 (Grass) |
   | `Mix` | Scalar Parameter `LayerMix` | 0.5 |
   | `BlendUV` | Absolute World Position → Divide на Scalar Parameter `LayerBlendScale` (2000) → Frac | |

   Основной выход → вход `W0` ноды поверхности, `OutW1` → вход `W1`.
4. **Нормаль:** во вход `N` ноды поверхности — нода **VertexNormalWS** (вместо атласа нормалей).
5. **Нода поверхности.** Include File Paths не меняется: `/PlanetErosion/PlanetTerrainSurface.ush` (новый
   файл он подключает сам). Код — из `ParallaxTest_Surface.hlsl`. Полный список Inputs, 24 штуки, имена
   буква в букву:

   | # | Вход | Что подключить |
   |---|---|---|
   | 1 | `W0` | основной выход ноды весов (п. 3) |
   | 2 | `W1` | `OutW1` ноды весов |
   | 3 | `N` | **VertexNormalWS** |
   | 4 | `RelPos` | World Position, режим Camera Relative World Position |
   | 5 | `UVNear` | Absolute World Position / `TileNear` → Frac |
   | 6 | `UVMid` | Absolute World Position / `TileMid` → Frac |
   | 7 | `UVMacro` | Absolute World Position / `TileMacro` → Frac |
   | 8 | `AH` | Texture Object Parameter `PlanetLayersAH` |
   | 9 | `NRA` | Texture Object Parameter `PlanetLayersNRA` |
   | 10–17 | `C0` … `C7` | `LayerSand` … `LayerRock` |
   | 18 | `P0` | Append(`TileNear`, `TileMid`, `TileMacro`, `NearEnd`) |
   | 19 | `P1` | Append(`FarStart`, `FarEnd`, `ReliefDepth` 0.03, `ShadowSteps` 8) — **4 числа** |
   | 20 | `P2` | Append(`CavityStrength` 0.6, `MicroShadowStrength` 0.6, 0, 0) |
   | 21 | `SunDir` | **SkyAtmosphereLightDirection**, Light Index 0 |
   | 22 | `P3` | Append(`PxMode`, `PxDepth`, `PxEnd`, `PxBudget`) |
   | 23 | `P4` | Append(`PxDebug`, `PxMipBias`, `PxGrazeFade`, 0) |

   Append склеивает по два входа: четыре числа = Append(Append(Append(a, b), c), d), нули — нода Constant.

   **Если материал планеты собран по старой инструкции** (20 входов, есть `CamVec`, нет `P2` и
   `SunDir`, в `P1` два числа): `CamVec` можно оставить или удалить, он не используется; недостающие
   входы и параметры добавьте по таблице.

   Выходы, как у материала планеты: основной → Base Color, `OutNormal` → Normal (Tangent Space
   Normal выключен), `OutRoughness` → Roughness, `OutAO` → Ambient Occlusion.
6. Новые параметры:

   | Нода | Имя | По умолчанию | Что |
   |---|---|---|---|
   | Scalar Parameter | `PxMode` | 2 | 0 выкл, 1 POM, 2 по клеткам |
   | Scalar Parameter | `PxDepth` | 0 | глубина в долях TileNear; 0 = как `ReliefDepth` (0.03 × 3 м = 9 см) |
   | Scalar Parameter | `PxEnd` | 2000 | дальность, см; затухание с 0.6 × End. Не больше `NearEnd` |
   | Scalar Parameter | `PxBudget` | 32 | шагов или клеток на пиксель, 4–64 |
   | Scalar Parameter | `PxDebug` | 0 | 1 — сколько шагов, 2 — глубина попадания, 3 — мип |
   | Scalar Parameter | `PxMipBias` | 0 | + грубее карта высот (дешевле, мягче) |
   | Scalar Parameter | `PxGrazeFade` | 0.15 | при взгляде почти вдоль поверхности параллакс плавно гаснет |

   `P3` = Append(`PxMode`, `PxDepth`, `PxEnd`, `PxBudget`); `P4` = Append(`PxDebug`, `PxMipBias`,
   `PxGrazeFade`, Constant 0).
7. Apply, Save. Создайте **Material Instance** `MI_ParallaxTest` и назначьте его сфере: параметры
   удобно крутить на лету.

## 4. Что смотреть

1. **Вблизи, 2–10 м, на верхушке сферы:** `PxMode` 0 → 1 → 2 на одном кадре. Камни должны выглядеть
   объёмными и перекрывать друг друга.
2. **«Плывёт» ли.** Камерой вбок и по кругу, на видео. Это главное, из-за чего параллакс когда-то
   выключили.
3. **Косые места сферы:** экватор, широты ~45° (стык двух плоскостей проекции) и под углом 30–60°
   к осям. Ищите швы и «двоение».
4. **Скользящий взгляд** (почти вдоль поверхности): срезанные края, ступеньки. Сравните 1 и 2.
5. **Отладка:**
   - `PxDebug 1`: синий — мало шагов, красный — много, тусклый — бюджет кончился;
   - `PxDebug 2`: глубина попадания;
   - `PxDebug 3`: мип карты высот (разные цвета — разные мипы).
6. **Террасы.** Высота лежит в альфе `T_PlanetLayers_AH` (сжатие Default = 8 бит блоками 4×4). Если
   вблизи видны мелкие ступени, это сжатие, а не параллакс. Тогда нужен отдельный массив высот
   (BC4/G16).
7. **Глубина:** `PxDepth` 0.02 / 0.04 / 0.06.
8. **Точность далеко от центра мира.** Сдвиньте сферу на X = 250000000 (2500 км, как радиус
   планеты) и повторите п. 1–2.

## 5. Цена

- `stat gpu`, строка **BasePass**, сфера на весь экран, 1080p, `r.ScreenPercentage 100`.
- Сравнить `PxMode` 0 / 1 / 2 и `PxBudget` 16 / 32 / 64.
- Для подробностей — `ProfileGPU` на одном кадре.

## 6. Что прислать

- Скриншоты с одной точки: `PxMode` 0, 1, 2 и `PxDebug` 1.
- Цифры BasePass из п. 5.
- Короткое видео бокового движения камеры в режиме 2.
- При ошибке компиляции — текст ошибки из окна материала (Stats) или из лога.

## Ограничения теста

- Трассируется только главная плоскость проекции и масштаб Near. Вторая плоскость и масштабы Mid
  и Macro читаются в той же найденной точке мира.
- Вершинного смещения («своя тесселяция») на сфере нет, поэтому параллакс идёт на полную глубину.
  Вариант «только остаток мельче шага вершин» проверяется уже на планете.
- Pixel Depth Offset не используется: трава и камни-меши стыкуются с плоской поверхностью.

## Если что-то не так

| Симптом | Причина |
|---|---|
| couldn't find include `/PlanetErosion/PlanetTerrainParallax.ush` | файл не скопирован или редактор не перезапущен |
| Ошибка про `PlanetSurfaceEx` | не обновлён `PlanetTerrainSurface.ush` |
| `use of undeclared identifier 'P2'` / `'SunDir'` / `'P3'` | у ноды поверхности нет этих входов, см. таблицу в п. 3.5 |
| `vector swizzle 'w' is out of bounds` в `W0.w` | у ноды весов Output Type не Float 4 (по умолчанию Float 3), у `OutW1` тоже нужен Float 4 |
| Материал не меняется после правки `.ush` | поменяйте комментарий `// px1` в конце кода ноды, Apply, Save |
| Нет разницы между режимами | `PxEnd` = 0, камера дальше `PxEnd` или `PxDepth`/`ReliefDepth` = 0 |
| Чёрные пятна от солнца | нет Sky Atmosphere или у Directional Light выключен Atmosphere Sun Light |
