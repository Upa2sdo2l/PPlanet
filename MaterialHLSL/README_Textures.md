# Текстурированный материал планеты

Делает поверхность из восьми слоёв (Sand, DryGrass, Grass, Forest, Jungle, Tundra, Snow, Rock):

- вблизи — детальные текстуры (масштаб Near), переходы между слоями по карте высот (трава в щелях
  между камнями, снег сначала в ложбинах), объём без parallax: ложбины темнее, камешки отбрасывают
  микротени от солнца, а у камеры вершины сетки реально поднимаются картами высот («своя
  тесселяция», раздел 6);
- на средних дистанциях — те же текстуры крупнее (Mid) плюс очень крупная вариация (Macro), чтобы
  не было видно повторения;
- дальше FarStart текстуры плавно уходят в средний цвет слоя, после FarEnd текстуры не читаются
  вовсе: с орбиты — чистые цвета биомов и рельеф из атласа нормалей.

Проекция biplanar по двум главным мировым осям: UV не нужны, на сфере и на скалах нет растяжений.
Код — `GpuErosion/Shaders/PlanetTerrainSurface.ush`, материал подключает его через Include, так что
обновления приходят через git без перевставки кода.

Сначала проверьте биомы на цветовом материале (`README.md`, пункт 5), потом переходите сюда.

## 1. Текстуры

Нужно 8 бесшовных наборов PBR (2K достаточно): цвет, нормаль, roughness, высота (displacement),
по возможности AO. Бесплатно и без ограничений (CC0): **ambientCG.com**, **polyhaven.com**; также Fab/Megascans.

| # | Слой | Что искать |
| --- | --- | --- |
| 0 | Sand | sand, beach sand, desert sand |
| 1 | DryGrass | dry grass, savanna ground, dry ground |
| 2 | Grass | grass ground, meadow |
| 3 | Forest | forest floor, leaves ground |
| 4 | Jungle | jungle floor, moss ground |
| 5 | Tundra | moss and lichen, tundra, rocky moss |
| 6 | Snow | snow |
| 7 | Rock | rock cliff, rock face |

Карта высот важнее всего: от неё зависят parallax и красивые переходы между слоями.

## 2. Упаковка (скрипт `Tools/pack_terrain_layers.py`)

Разложите наборы по папкам с именами слоёв (`Sand`, `DryGrass`, … `Rock`) и запустите:

```
pip install pillow numpy
python Tools/pack_terrain_layers.py <папка с наборами> <папка результата> --size 2048
```

Скрипт сам находит карты по именам файлов, переводит нормали OpenGL в DirectX (если в имени есть
`gl`), и пишет на каждый слой два файла:

- `T_Layer<i>_<Слой>_AH.png` — цвет RGB + высота в A;
- `T_Layer<i>_<Слой>_NRA.png` — нормаль X/Y, roughness, AO;

и `layers.txt` со средним цветом каждого слоя (Hex sRGB) — это цвета для дальней дистанции.

Если нормаль какого-то набора выглядит «вывернутой» (свет с другой стороны), перезапустите с
`--flip-green ИмяСлоя`.

## 3. Импорт в Unreal

1. Импортируйте все 16 PNG.
2. Все `_AH`: **sRGB вкл**, Compression **Default**.
3. Все `_NRA`: **sRGB выкл**, Compression **BC7**.
4. Выделите 8 текстур `_AH` → правый клик → **Create Texture Array**. Назовите `T_PlanetLayers_AH`.
   Откройте его и проверьте порядок в **Source Textures**: строго 0 Sand … 7 Rock.
5. То же для `_NRA` → `T_PlanetLayers_NRA` (sRGB выкл, Compression BC7).

## 4. Материал `M_PlanetTerrainGPU`

Старую цветовую ноду `PlanetTerrain_Layers` можно удалить; её 8 цветовых параметров (`LayerSand` …
`LayerRock`) остаются — выставьте им средние цвета из `layers.txt`.

Добавьте параметры:

| Нода | Имя | Значение по умолчанию |
| --- | --- | --- |
| Texture Object Parameter | `PlanetLayersAH` | `T_PlanetLayers_AH` |
| Texture Object Parameter | `PlanetLayersNRA` | `T_PlanetLayers_NRA` |
| Scalar Parameter | `TileNear` | 300 (см: текстура повторяется каждые 3 м) |
| Scalar Parameter | `TileMid` | 3000 |
| Scalar Parameter | `TileMacro` | 40000 |
| Scalar Parameter | `NearEnd` | 3000 (до 30 м — детальный масштаб) |
| Scalar Parameter | `FarStart` | 150000 (с 1.5 км текстуры уходят в цвет) |
| Scalar Parameter | `FarEnd` | 600000 (с 6 км — только цвет) |
| Scalar Parameter | `ReliefDepth` | 0.03 (глубина рельефа карты высот в долях тайла: 0.03 × 3 м = 9 см) |
| Scalar Parameter | `ShadowSteps` | 8 (шагов микротеней) |
| Scalar Parameter | `CavityStrength` | 0.6 (затемнение ложбин) |
| Scalar Parameter | `MicroShadowStrength` | 0.6 (плотность микротеней) |

Координаты (делаются в графе, в двойной точности — так текстуры не «дрожат» вдали от центра мира):

- **World Position** (режим Absolute World Position) → **Divide** на `TileNear` → **Frac** → это `UVNear`.
  Так же для `TileMid` (`UVMid`) и `TileMacro` (`UVMacro`).
- Ещё одна **World Position** в режиме **Camera Relative World Position** → это `RelPos`.
- `P0` = **Append**(`TileNear`, `TileMid`, `TileMacro`, `NearEnd`) — три ноды Append подряд.
- `P1` = **Append**(`FarStart`, `FarEnd`, `ReliefDepth`, `ShadowSteps`).
- `P2` = **Append**(`CavityStrength`, `MicroShadowStrength`, 0, 0) — нули: ноды Constant.
- **SkyAtmosphereLightDirection** (Light Index 0) — направление на солнце; это `SunDir`.

Нода **Custom**:

- Output Type **CMOT Float 3**;
- **Include File Paths**: `/PlanetErosion/PlanetTerrainSurface.ush`;
- Code — из `PlanetTerrain_Surface.hlsl` ниже пунктирной линии;
- Inputs по порядку: `W0` (PlanetBiomeAtlas RGBA), `W1` (PlanetBiomeAtlas2 RGBA), `N` (PlanetNormalAtlas
  RGB ×2 −1), `RelPos`, `UVNear`, `UVMid`, `UVMacro`, `AH` (`PlanetLayersAH`), `NRA` (`PlanetLayersNRA`),
  `C0`…`C7` (`LayerSand` … `LayerRock`), `P0`, `P1`, `P2`, `SunDir` (22 входа);
- **Additional Outputs**: `OutNormal` (CMOT Float 3), `OutRoughness` (CMOT Float 1), `OutAO` (CMOT Float 1).

Подключение: основной выход → **Base Color**; `OutNormal` → **Normal** (у материала Tangent Space Normal
выключен, как и было); `OutRoughness` → **Roughness**; `OutAO` → **Ambient Occlusion**.

**После обновления `PlanetTerrainSurface.ush`** Unreal может продолжать брать из кэша старую
скомпилированную версию материала (изменение подключаемого файла он не всегда замечает). Перезапустите
редактор и поменяйте комментарий в конце поля Code Custom-ноды (например `// v3` → `// v4`), затем
Apply и Save: материал перекомпилируется с новым файлом.

## 5. Настройка и стоимость

- Повторение текстуры заметно → увеличьте `TileMid`; мыльно под ногами → уменьшите `TileNear`.
- Рельеф без parallax (текстура никуда не сдвигается, поэтому ничего не «плывёт»):
  - `CavityStrength` 0–1: насколько темнеют низкие места карты высот (ложбины, щели между камнями);
  - `MicroShadowStrength` 0–1: плотность теней, которые бугорки отбрасывают от солнца;
  - `ReliefDepth`: высота рельефа карты высот в долях тайла; больше — длиннее микротени;
  - `ShadowSteps` 4–16: качество микротеней (8 — норма, 0 в `MicroShadowStrength` — выключить).
  Солнце берётся из SkyAtmosphereLightDirection; без Sky Atmosphere на уровне микротеней нет.
- Значения по умолчанию рассчитаны на ноутбучную RTX 3050 (4 ГБ). Память: 16 текстур 2K в BC7 —
  около 90 МБ вместе с мипами; при нехватке видеопамяти упакуйте с `--size 1024` (~22 МБ).
- Резкая граница «текстуры → цвет» вдали → раздвиньте `FarStart`/`FarEnd` или поправьте средние цвета.
- Стоимость на пиксель: дальше `FarEnd` — 0 выборок; средняя дистанция — 9 + микротени; ближе
  `NearEnd` — до ~20. Обычный уровень для ландшафтного материала.

## 6. «Своя тесселяция»: геометрия у камеры

Ниже MaxLOD (14) шум уже упирается в точность float, поэтому новые чанки не генерируются. Вместо
этого ближайшие чанки делятся дальше на «производные» (`planet_core/PlanetDetail.h`): они ничего не
строят на CPU, а читают тайл своего базового чанка и в вершинном шейдере поднимаются картами высот
текстур (`GpuErosion/Shaders/PlanetTerrainVertex.ush`). С `DetailLevels` = 4 вершины под ногами
стоят через ~0.3 м, дальше кольцами 0.6 / 1.2 / 2.3 м.

- Материал: нода WPO и нода AtlasUV по `README.md` (разделы WPO и «Нормаль и цвет»).
- `DisplaceNear` (см) — размах бугров из текстур масштаба Near (камни, рябь), `DisplaceMid` (см) — из
  масштаба Mid (кочки, дюны). Смещение отфильтровано под шаг сетки: грубые чанки получают только
  крупные бугры, мелочь остаётся микротеням и нормалям.
- Актор → **Planet → GPU Terrain → Detail**: `bTerrainDetail`, `DetailLevels` (0–6),
  `DetailSplitDistanceFactor` (меньше — меньше инстансов), `DetailMaxInstances`,
  `DisplacementBoundsMarginMetres` (запас границ чанка: ≥ половины `DisplaceNear` + `DisplaceMid`).
- Смещение не зависит от камеры: тени (VSM) кэшируются как обычно, бугры не «растут» при подходе.
  Новые уровни при приближении проявляются морфингом (`MorphSeconds`).
- Коллизия — по поверхности без смещения: при ±10–20 см ноги местами чуть утопают или висят.
- `planet.Stats 1` показывает в конце строки `Detail N/M from K chunks: L15:… L18:…`.

## Если что-то не так

| Симптом | Причина |
| --- | --- |
| Ошибка «couldn't find include /PlanetErosion/PlanetTerrainSurface.ush» | плагин PlanetErosion не загружен или ветка не обновлена |
| Чёрная/розовая поверхность | порядок или sRGB у массивов; разный размер текстур в массиве |
| Свет на рельефе с «неправильной» стороны у одного слоя | нормаль OpenGL: перепакуйте с `--flip-green` |
| Текстуры «плывут» или дрожат при движении | пришлите видео/скриншот — проверим точность координат |
| Дырки-«искры» или щели между чанками у ног | пришлите скриншот с `planet.Stats 1`; временно `DetailLevels` = 0 |
| Бугры висят в воздухе или проваливаются по краям экрана | увеличьте `DisplacementBoundsMarginMetres` (действует на новые чанки) |
| Видны швы-линии по сетке тайлов | пришлите скриншот (выбор мипов на границе тайла) |
