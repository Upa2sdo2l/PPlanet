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
| Scalar Parameter | `AtlasTilesPerRow` | значение по умолчанию 18 |
| Scalar Parameter | `AtlasTexels` | значение по умолчанию 1170 |
| Scalar Parameter | `MorphSeconds` | значение по умолчанию 0.35 |

Тип сэмплера у двух Texture Sample оставьте тем, который предложит редактор для текстуры по умолчанию (Color или Linear Color), но **не Normal**: нормаль декодируется вручную.

### World Position Offset (вершинный шейдер)

1. Добавьте ноду **Custom**:
   - Output Type: **CMOT Float 3**
   - Code: всё из `PlanetTerrain_WPO.hlsl` ниже пунктирной линии
   - Inputs — **8 штук, в этом порядке и с этими именами**:

   | # | Имя входа | Что подключить |
   | --- | --- | --- |
   | 1 | `UV` | TexCoord[0] |
   | 2 | `LocalPos` | Pre-Skinned Local Position |
   | 3 | `PosAtlas` | `PlanetPosAtlas` (Texture Object Parameter) |
   | 4 | `CD0` | PerInstanceCustomData3Vector, Data Index **0** |
   | 5 | `CD1` | PerInstanceCustomData3Vector, Data Index **3** |
   | 6 | `TimeSec` | Time |
   | 7 | `TilesPerRow` | `AtlasTilesPerRow` |
   | 8 | `MorphSeconds` | `MorphSeconds` |

2. Выход Custom → **Transform Vector** (Source: **Local Space**, Destination: **World Space**) → пин **World Position Offset**.

### Нормаль и цвет (пиксельный шейдер)

1. Вторая нода **Custom**:
   - Output Type: **CMOT Float 2**
   - Code: всё из `PlanetTerrain_AtlasUV.hlsl` ниже пунктирной линии
   - Inputs по порядку: `UV` (TexCoord[0]), `CD0` (PerInstanceCustomData3Vector, Data Index 0), `TilesPerRow` (`AtlasTilesPerRow`), `AtlasTexels` (`AtlasTexels`)
2. Выход → нода **Vertex Interpolator** (UV считается в вершинном шейдере, а читается в пиксельном).
3. Выход Vertex Interpolator → вход **UVs** у `PlanetNormalAtlas` и у `PlanetBiomeAtlas`. Если редактор ругается на размерность, вставьте Component Mask (R G).
4. Нормаль: `PlanetNormalAtlas` RGB → **× 2** → **− 1** → **Normalize** → пин **Normal**.
5. Биомы: `PlanetBiomeAtlas` RGBA — это те же веса, что раньше лежали в Vertex Color (R трава, G скалы, B песок, A снег). Скопируйте из старого материала часть, которая смешивает цвета, и подключите вместо ноды Vertex Color.

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
