# GPU-рендер террейна: что настроить в редакторе

Код (`ProceduralPlanetGpu.cpp`) рисует всю планету одним инстансированным компонентом.
Форму каждому чанку задаёт материал, поэтому его нужно один раз собрать в редакторе по этой инструкции.
Пока материал не назначен, планета рисуется старым способом (RealtimeMesh), а в лог пишется предупреждение.

## 1. Build.cs

В `FastNoiseTest.Build.cs` добавьте два модуля в `PublicDependencyModuleNames`:

```csharp
"MeshDescription", "StaticMeshDescription"
```

Они нужны, чтобы собрать сетку 65×65 в рантайме.

## 2. Материал `M_PlanetTerrainGPU`

Создайте новый материал (Opaque, Default Lit). В панели Details материала:

- **Tangent Space Normal** — выключить (нормаль подаётся в мировых координатах).
- **Usage → Used with Instanced Static Meshes** — включить.
- **Num Customized UVs** не трогать.

### Параметры (имена должны совпадать буква в букву)

| Нода | Имя параметра | Примечание |
| --- | --- | --- |
| Texture Object Parameter | `PlanetPosAtlas` | любая текстура по умолчанию; в игре подставляется атлас |
| Texture Sample Parameter 2D | `PlanetNormalAtlas` | Sampler Source: **Shared: Clamp** |
| Texture Sample Parameter 2D | `PlanetBiomeAtlas` | Sampler Source: **Shared: Clamp** |
| Texture Sample Parameter 2D | `PlanetBiomeAtlas2` | Sampler Source: **Shared: Clamp** |
| Scalar Parameter | `AtlasTilesPerRow` | значение по умолчанию 18 |
| Scalar Parameter | `AtlasTexels` | значение по умолчанию 1170 |
| Scalar Parameter | `MorphSeconds` | значение по умолчанию 0.35 |

Тип сэмплера у двух Texture Sample оставьте тем, который предложит редактор для текстуры по умолчанию (Color или Linear Color), но **не Normal**: нормаль декодируется вручную.

### World Position Offset (вершинный шейдер)

Код вершинного шейдера лежит в `GpuErosion/Shaders/PlanetTerrainVertex.ush` и подключается
через Include, нода только вызывает его. Он ставит вершины по атласу, сшивает края с соседями,
делает морфинг, а у камеры ещё и «свою тесселяцию»: уровни детализации ниже MaxLOD и смещение
вершин картами высот текстур (`README_Textures.md`, раздел 6).

Сначала добавьте параметры:

| Нода | Имя |
| --- | --- |
| Texture Object Parameter | `PlanetNormalAtlasVS` |
| Texture Object Parameter | `PlanetBiomeAtlasVS` |
| Texture Object Parameter | `PlanetBiomeAtlas2VS` |
| Scalar Parameter | `DisplaceNear` (по умолчанию 10) |
| Scalar Parameter | `DisplaceMid` (по умолчанию 40) |

(Атласы в них подставляет код, текстура по умолчанию любая.)

1. Нода **Custom**:
   - Output Type: **CMOT Float 3**
   - Include File Paths: `/PlanetErosion/PlanetTerrainVertex.ush`
   - Code: всё из `PlanetTerrain_WPO.hlsl` ниже пунктирной линии
   - Inputs — **20 штук, в этом порядке и с этими именами**:

   | # | Имя входа | Что подключить |
   | --- | --- | --- |
   | 1 | `UV` | TexCoord[0] |
   | 2 | `LocalPos` | Pre-Skinned Local Position |
   | 3 | `PosAtlas` | `PlanetPosAtlas` (Texture Object Parameter) |
   | 4 | `NrmAtlas` | `PlanetNormalAtlasVS` |
   | 5 | `BioAtlas` | `PlanetBiomeAtlasVS` |
   | 6 | `Bio2Atlas` | `PlanetBiomeAtlas2VS` |
   | 7 | `AH` | `PlanetLayersAH` (тот же, что у текстурной ноды) |
   | 8–16 | `CD0` … `CD8` | PerInstanceCustomData3Vector, Data Index **0, 3, 6, 9, 12, 15, 18, 21, 24** |
   | 17 | `TimeSec` | Time |
   | 18 | `TilesPerRow` | `AtlasTilesPerRow` |
   | 19 | `MorphSeconds` | `MorphSeconds` |
   | 20 | `D0` | Append(`TileNear`, `TileMid`, `DisplaceNear`, `DisplaceMid`) |

2. Выход Custom → сразу в пин **World Position Offset** (нода возвращает смещение в мировых
   координатах; Transform Vector больше не нужен).

### Нормаль и цвет (пиксельный шейдер)

1. Вторая нода **Custom**:
   - Output Type: **CMOT Float 2**
   - Code: всё из `PlanetTerrain_AtlasUV.hlsl` ниже пунктирной линии
   - Inputs по порядку: `UV` (TexCoord[0]), `CD0` (PerInstanceCustomData3Vector, Data Index 0), `CD2` (PerInstanceCustomData3Vector, Data Index 6), `TilesPerRow` (`AtlasTilesPerRow`), `AtlasTexels` (`AtlasTexels`)
2. Выход → нода **Vertex Interpolator** (UV считается в вершинном шейдере, а читается в пиксельном).
3. Выход Vertex Interpolator → вход **UVs** у `PlanetNormalAtlas` и у `PlanetBiomeAtlas`. Если редактор ругается на размерность, вставьте Component Mask (R G).
4. Нормаль: `PlanetNormalAtlas` RGB → **× 2** → **− 1** → **Normalize** → пин **Normal**.
5. Цвет поверхности — восемь слоёв из двух атласов (ветка `feature/gravity-biomes`):
   - добавьте **Texture Sample Parameter 2D `PlanetBiomeAtlas2`** (как `PlanetBiomeAtlas`: Sampler Source **Shared: Clamp**, тот же вход UVs от Vertex Interpolator);
   - добавьте 8 **Vector Parameter** с цветами слоёв (в пикере цвета удобно вводить в поле **Hex sRGB**):

     | # | Параметр | Слой | Hex sRGB |
     | --- | --- | --- | --- |
     | 0 | `LayerSand` | песок (пляжи, пустыни) | `C7B385` |
     | 1 | `LayerDryGrass` | сухая трава (саванна, степь) | `A39152` |
     | 2 | `LayerGrass` | трава (луга) | `528033` |
     | 3 | `LayerForest` | лесная подстилка (умеренный лес, тайга) | `2B4F21` |
     | 4 | `LayerJungle` | тропический лес | `144D1A` |
     | 5 | `LayerTundra` | тундра (мох, лишайник) | `7A7861` |
     | 6 | `LayerSnow` | снег, лёд | `F0F2F7` |
     | 7 | `LayerRock` | скалы (крутые склоны) | `736E66` |

   - нода **Custom**: Output Type **CMOT Float 3**, Code — всё из `PlanetTerrain_Layers.hlsl` ниже пунктирной линии, входы по порядку: `W0` (`PlanetBiomeAtlas` RGBA), `W1` (`PlanetBiomeAtlas2` RGBA), `C0`…`C7` (параметры из таблицы по порядку);
   - выход → **Base Color**. Старую схему (трава/скалы/песок/снег) удалите: смысл каналов `PlanetBiomeAtlas` поменялся.

## 2б. Плагин эрозии

Атласы заполняют compute-шейдеры плагина `PlanetErosion`: настройка в `GpuErosion/README.md`.
В материале ничего менять не нужно.

## 3. Актор планеты

- **GPU Terrain → Terrain Renderer**: `GPUInstanced` (по умолчанию).
- **GPU Terrain → GPU Terrain Material**: `M_PlanetTerrainGPU`.
- Старый **Planet Material** оставьте: если переключить Terrain Renderer на `RealtimeMesh`, вернётся прежний рендер.

## Если что-то не так

| Симптом | Вероятная причина |
| --- | --- |
| Планета рисуется как раньше, в логе `GPUTerrainMaterial is not set` | материал не назначен актору |
| Планеты не видно совсем | грани развёрнуты от камеры: включите в материале **Two Sided**; если планета появилась, напишите мне |
| Каша из треугольников | неправильный порядок или имена входов у Custom-ноды WPO |
| Чёрная или однотонная планета | не подключены атласы нормалей и биомов, или у Texture Sample сэмплер типа Normal |
| На стыках чанков видно небо | у PerInstanceCustomData3Vector неверный Data Index (должно быть 0 и 3) |
| Морфинг не идёт или идёт рывком | Time в материале не совпадает с игровым временем: попробуйте MorphSeconds = 0, чтобы убедиться, что дело в нём |

`planet.Stats 1` в первой строке пишет, какой рендер сейчас активен.
