# Эрозия на GPU: настройка

Эрозия, нормали и биомы чанков теперь считаются compute-шейдерами. Глобальные шейдеры
должны регистрироваться до старта движка, поэтому нужен маленький плагин `PlanetErosion`.
Весь его код и шейдеры лежат здесь, в репозитории (`GpuErosion/`), и обновляются через git.
Плагин в папке проекта — это только три файла-«заглушки», их нужно создать один раз.

## 1. Плагин (один раз)

Создайте в папке проекта (рядом с `Source`, не внутри него) такую структуру:

```
<Проект>/Plugins/PlanetErosion/
    PlanetErosion.uplugin
    Source/PlanetErosion/
        PlanetErosion.Build.cs
        Private/PlanetErosionModule.cpp
```

Содержимое — файлы из `GpuErosion/PluginTemplate/`, без расширения `.txt`:

| Файл в `PluginTemplate/` | Куда положить |
| --- | --- |
| `PlanetErosion.uplugin.txt` | `Plugins/PlanetErosion/PlanetErosion.uplugin` |
| `PlanetErosion.Build.cs.txt` | `Plugins/PlanetErosion/Source/PlanetErosion/PlanetErosion.Build.cs` |
| `PlanetErosionModule.cpp.txt` | `Plugins/PlanetErosion/Source/PlanetErosion/Private/PlanetErosionModule.cpp` |

В репозитории у них расширение `.txt` нарочно: иначе UBT собрал бы их ещё раз как часть
модуля игры.

Плагин ищет код и шейдеры по пути `<Проект>/Source/FastNoiseTest/GpuErosion`. Если
репозиторий лежит в другой папке, поправьте путь в `PlanetErosion.Build.cs` и в
`StartupModule` в `PlanetErosionGpu.inl`.

## 2. Build.cs игры

В `FastNoiseTest.Build.cs` добавьте модуль плагина в `PublicDependencyModuleNames`:

```csharp
"PlanetErosion"
```

После этого: правый клик по `.uproject` → **Generate Visual Studio project files**, затем
сборка. При первом запуске редактор скомпилирует два новых шейдера (несколько секунд).

## 3. Материал

Атласы теперь render target'ы, но в материале ничего менять не нужно. Тип сэмплера у
`PlanetNormalAtlas` и `PlanetBiomeAtlas` оставьте прежним (Color): редактор проверяет его
по текстуре по умолчанию, а атлас в игре всё равно линейный, и данные читаются без
гамма-коррекции.

## 4. Настройки актора

- **Planet → Erosion**: включение и параметры эрозии. Значения по умолчанию подобраны
  для `MountainAmplitude` 10000 м. Если горы заметно выше, увеличьте `ScaleMetres`.
- **Planet → Noise Params → Mountains → Mountain Octaves**: рекомендую **7** вместо 12.
  Мелкие октавы гребневого шума эрозия всё равно заменяет своими оврагами, а без них
  рельеф чище.
- Эрозия работает только с рендером `GPUInstanced`. На `RealtimeMesh` рельеф без эрозии.

## 5. Как проверить

- `planet.Stats 1` — строка `erosion ON | GPU tiles eroded N/s | heights read back N/s, X frames after dispatch | collision meshes N/s`.
- `stat gpu` — проходы `PlanetErosion` и `PlanetTerrainTiles` (видны, когда строятся новые чанки).
- `stat Planet` — те же числа (`GPU: tile heights read back per second`, `GPU: height read-back latency`).
- `planet.Bench 32 14` — строка `erosion inputs` (CPU-часть эрозии на каждый тайл).

## Как это устроено

1. Воркер (CPU) считает шум на сетке 67×67 (чанк плюс кольцо соседей), уклон для
   оврагов (по 4 отсчёта в 481 точке), маску суши, и собирает тайл: 4489 вершин по 48 байт.
2. За кадр все новые тайлы уходят одним пакетом в два compute-прохода:
   - `ErodeCS` — высота эрозии в каждой из 4489 точек;
   - `WriteCS` — позиция, нормаль (по эродированным соседям) и биомы каждой вершины, сразу в атласы.
3. Высоты эрозии каждого тайла копируются с GPU обратно (17 КБ, через 2–3 кадра). Меш
   коллизии строится из них, когда чанк попадает в радиус коллизии: эрозия на CPU не
   считается, коллизия совпадает с картинкой точно. Если высоты почему-то не пришли за 300
   кадров, чанк получает коллизию без эрозии и в лог пишется предупреждение.
4. Высота под игроком (`GetHeightAt`) тоже включает эрозию (считается на CPU по одной
   точке, ~20 мкс; от картинки отличается на сантиметры).

Стыки чанков любых LOD совпадают до миллиметров: высота в точке зависит только от самой
точки, а не от того, какой чанк её считает.

## Если что-то не так

| Симптом | Вероятная причина |
| --- | --- |
| Ошибка сборки `PLANETEROSION_API` не определён | в `FastNoiseTest.Build.cs` не добавлен `"PlanetErosion"` |
| Редактор закрывается с `[PlanetErosion] shader folder not found` | код лежит не в `Source/FastNoiseTest/GpuErosion` |
| В логе `GPU terrain needs SM5 compute shaders` | редактор запущен в режиме мобильного превью; планета рисуется через RealtimeMesh |
| Редактор падает при старте с ошибкой шейдера | пришлите текст ошибки из лога |
| Планета чёрная или без рельефа | шейдеры не отработали: проверьте `stat gpu` (проходы PlanetErosion) |
| Персонаж висит в воздухе или проваливается | расхождение коллизии и картинки: пришлите скриншот с `planet.Stats 1` |

Код эрозии (`PlanetErosion.*`, `Shaders/*`) — производная от кода runevision под лицензией
MPL 2.0: изменения в этих файлах должны оставаться открытыми; на остальной проект лицензия
не распространяется.
