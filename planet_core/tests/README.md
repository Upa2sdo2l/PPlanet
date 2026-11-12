# Стенд ядра planet_core

Проверяет ядро без Unreal: нужны только компилятор C++17 и FastNoise2.
В игру не входит. В сборке Unreal файл `stand.cpp` пустой (весь код под `PLANET_CORE_STAND`), копировать его в проект не обязательно.

## Сборка (Linux, g++)

```
git clone https://github.com/Auburn/FastNoise2.git fn2
cmake -S fn2 -B fn2/build -DCMAKE_BUILD_TYPE=Release -DFASTNOISE2_TOOLS=OFF -DFASTNOISE2_TESTS=OFF
cmake --build fn2/build -j

g++ -O2 -std=c++17 -DPLANET_CORE_STAND \
    -I planet_core -I fn2/include -I fn2/build/_deps/fastsimd-src/include \
    planet_core/tests/stand.cpp planet_core/*.cpp \
    fn2/build/Release/lib/libFastNoise.a -lpthread -o stand
```

Проверено с FastNoise2 `c35d604` (master, ноябрь 2026). Числа высот зависят от версии FastNoise2 и набора инструкций процессора (AVX2 / AVX-512); проверки «прошёл / не прошёл» от них не зависят.

Санитайзеры: добавить `-O1 -g -fsanitize=address,undefined` или `-fsanitize=thread` (для `threads`).

## Запуск

`./stand check` — все проверки с результатом «прошёл / не прошёл», около 2 минут. Код возврата 1, если хоть одна не прошла.

| Тест | Что проверяет | Условие |
|---|---|---|
| `basic` | все пути ядра у шести вариантов рельефа (Musgrave, Hybrid, Uber, Uber+ridges, V4, Original) | нет NaN и бесконечностей; высота не выше `MaxHeightEstimate` |
| `threads` | 4 потока считают чанки на одном графе | числа те же, что в одном потоке |
| `seams` | общие вершины соседних чанков: тот же LOD, LOD+1, рёбра и углы куба | высота, эрозия, маска и fade совпадают точно |
| `stream` | планировщик: чанк с коллизией не уходит, пока у замены нет своей | старый чанк уходит через `SettleFrames` после последней сетки коллизии; 0 кадров без коллизии |
| `basis` | Uber: нет ступени высоты на окружностях \|z\| = 0.9 | шаги в 1 м через окружность такие же, как в других местах |

Поле `Mountain` в `seams` только печатается: оно расходится на общих вершинах у Musgrave и V4 (известно, на GPU не уходит).

Замеры, только печатают числа:

| Тест | Что печатает |
|---|---|
| `point` | высота в точке (`EvaluateHeight` + `DeltaAt`) против вершин чанка |
| `jump` | шаги высоты в 1 м через \|z\| = 0.9 и 0.6 |
| `coast` | ступень высоты на урезе воды |
| `faults` | пары точек в 1 м выше 1200 м: доля ступеней больше 3 и 10 м (`FAULT_DUMP=1` печатает места) |
| `bound` | максимум высоты по трём сидам против `MaxHeightEstimate` |
| `land` | фактическая доля суши против заданной |
| `lod` | листья и размер чанка под камерой: центр грани, ребро, угол куба |
| `cost` | время `Build`, уровня моря и чанка L13 (море / равнины / горы) |

## Когда запускать

После любой правки в `planet_core`: `./stand check`. Если менялась форма рельефа, дополнительно `faults`, `coast`, `bound` и `cost`.
