# Гравитация планеты: настройка

Используется встроенная в UE 5.4+ кастомная гравитация `CharacterMovementComponent`
(`SetGravityDirection`), плагины не нужны. Код: `PlanetGravityComponent.*`, `PlanetPlayerController.*`.

## 1. Контроллер

В вашем GameMode (Blueprint или C++) поставьте **Player Controller Class = `PlanetPlayerController`**.
Он поворачивает камеру относительно местного «верха»: мышь крутит взгляд вокруг нормали к
планете, а при ходьбе вокруг планеты камера плавно поворачивается вместе с горизонтом.

Если у вас уже есть свой контроллер на C++, унаследуйте его от `APlanetPlayerController`.
Если контроллер — Blueprint, поменяйте ему родителя (Class Settings → Parent Class).

## 2. Персонаж

1. В Blueprint персонажа: **Add Component → Planet Gravity**. Поле `Planet` можно оставить
   пустым — компонент сам найдёт ближайшую планету.
2. Событие движения (в шаблоне Third Person это `IA_Move` / `Move`): шаблон берёт
   направления из `Get Control Rotation` → yaw → `Get Forward Vector` / `Get Right Vector`.
   На планете это неверно (yaw считается вокруг мировой оси Z). Замените на:
   `Planet Gravity` → **Get Movement Basis** → `Forward` в `Add Movement Input` для оси Y
   ввода, `Right` — для оси X.
3. Spring Arm камеры: **Use Pawn Control Rotation** включено (как в шаблоне).

Сила тяжести — обычная мировая (Project Settings → Physics → Default Gravity Z) × `Gravity
Scale` у Character Movement. Ходьба, прыжки, определение пола и наклон капсулы работают
относительно направления к центру планеты.

## 3. Физические объекты

Любому актору, у которого корневой компонент симулирует физику (камни, ящики), добавьте
**Planet Gravity**. Компонент выключит ему обычную гравитацию (она всегда тянет вниз по Z) и
будет тянуть к центру планеты. `Gravity Scale` компонента — множитель силы.

## Ограничения

- Мультиплеер не проверялся (у Epic известны проблемы репликации кастомной гравитации).
- Планета не должна вращаться/двигаться во время игры.
- Pawn без CharacterMovement (свой летательный аппарат и т.п.) — только через физику
  (пункт 3) или своим кодом с `GetGravityDirection()` компонента.
