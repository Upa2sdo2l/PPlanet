# Текстурированный материал планеты

Делает поверхность из восьми слоёв (Sand, DryGrass, Grass, Forest, Jungle, Tundra, Snow, Rock):

- вблизи — детальные текстуры (масштаб Near) с parallax, переходы между слоями по карте высот
  (трава в щелях между камнями, снег сначала в ложбинах);
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
| Scalar Parameter | `NearEnd` | 3000 (до 30 м — детальный масштаб и parallax) |
| Scalar Parameter | `FarStart` | 150000 (с 1.5 км текстуры уходят в цвет) |
| Scalar Parameter | `FarEnd` | 600000 (с 6 км — только цвет) |
| Scalar Parameter | `ParallaxDepth` | 0.05 |
| Scalar Parameter | `ParallaxSteps` | 8 |

Координаты (делаются в графе, в двойной точности — так текстуры не «дрожат» вдали от центра мира):

- **World Position** (режим Absolute World Position) → **Divide** на `TileNear` → **Frac** → это `UVNear`.
  Так же для `TileMid` (`UVMid`) и `TileMacro` (`UVMacro`).
- Ещё одна **World Position** в режиме **Camera Relative World Position** → это `RelPos`.
- `P0` = **Append**(`TileNear`, `TileMid`, `TileMacro`, `NearEnd`) — три ноды Append подряд.
- `P1` = **Append**(`FarStart`, `FarEnd`, `ParallaxDepth`, `ParallaxSteps`).

Нода **Custom**:

- Output Type **CMOT Float 3**;
- **Include File Paths**: `/PlanetErosion/PlanetTerrainSurface.ush`;
- Code — из `PlanetTerrain_Surface.hlsl` ниже пунктирной линии (5 строк);
- Inputs по порядку: `W0` (PlanetBiomeAtlas RGBA), `W1` (PlanetBiomeAtlas2 RGBA), `N` (PlanetNormalAtlas
  RGB ×2 −1), `RelPos`, `UVNear`, `UVMid`, `UVMacro`, `AH` (`PlanetLayersAH`), `NRA` (`PlanetLayersNRA`),
  `C0`…`C7` (`LayerSand` … `LayerRock`), `P0`, `P1`;
- **Additional Outputs**: `OutNormal` (CMOT Float 3), `OutRoughness` (CMOT Float 1), `OutAO` (CMOT Float 1).

Подключение: основной выход → **Base Color**; `OutNormal` → **Normal** (у материала Tangent Space Normal
выключен, как и было); `OutRoughness` → **Roughness**; `OutAO` → **Ambient Occlusion**.

## 5. Настройка и стоимость

- Повторение текстуры заметно → увеличьте `TileMid`; мыльно под ногами → уменьшите `TileNear`.
- Parallax: `ParallaxDepth` 0.03–0.08; `ParallaxSteps` 8–16 (дороже). 0 — выключить.
- Значения по умолчанию рассчитаны на ноутбучную RTX 3050 (4 ГБ). Память: 16 текстур 2K в BC7 —
  около 90 МБ вместе с мипами; при нехватке видеопамяти упакуйте с `--size 1024` (~22 МБ).
- Резкая граница «текстуры → цвет» вдали → раздвиньте `FarStart`/`FarEnd` или поправьте средние цвета.
- Стоимость на пиксель: дальше `FarEnd` — 0 выборок; средняя дистанция — 9; ближе `NearEnd` — до ~25
  (с parallax на 8 шагов). Это обычный уровень для ландшафтного материала.

## Если что-то не так

| Симптом | Причина |
| --- | --- |
| Ошибка «couldn't find include /PlanetErosion/PlanetTerrainSurface.ush» | плагин PlanetErosion не загружен или ветка не обновлена |
| Чёрная/розовая поверхность | порядок или sRGB у массивов; разный размер текстур в массиве |
| Свет на рельефе с «неправильной» стороны у одного слоя | нормаль OpenGL: перепакуйте с `--flip-green` |
| Текстуры «плывут» или дрожат при движении | пришлите видео/скриншот — проверим точность координат |
| Видны швы-линии по сетке тайлов | пришлите скриншот (выбор мипов на границе тайла) |
