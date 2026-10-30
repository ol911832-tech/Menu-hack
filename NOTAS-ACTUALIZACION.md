# Cómo actualizar los mods cuando RobTop saca un parche

1. Copia el `GeometryDash.exe` nuevo a cualquier sitio y ejecuta:
   `python tools/verificar-exe.py "ruta\GeometryDash.exe"`
   Muestra qué funciones siguen en su sitio y, para las que no, las direcciones candidatas con su desplazamiento.
   Las funciones de una misma zona suelen moverse lo mismo (por ejemplo, todas las de `GameManager` -0x2670).
2. Cambia las direcciones en `mod/kfe.cpp` (namespace `gd`) y en `menu/kmenu.cpp` (`resolveGameFunctions` e `installGameHooks`).
3. Los offsets de campos (`kTestMode`, `kPlayer1`, ...) no se pueden sacar solo con firmas. Se comprueban desensamblando
   funciones del juego que los usen; ejemplos del parche del 20-oct-2026:
   - `getModifiedDelta` (justo antes de `GJBaseGameLayer::update`) usa `timeWarp` y `m_extraDelta`.
   - La función que formatea la cadena `"Attempt %i"` carga `m_attempts` justo antes y `m_attemptLabel` justo después.
   - `PlayLayer::resetLevel` llama a `updateCamera` y luego al slot virtual 0x430 con dt = 0.
4. Si un campo falla, los mods no deberían crashear: `kfe.dll` comprueba por RTTI que `m_player1` es un `PlayerObject` antes
   de tocar nada, y `kmenu.dll` desactiva las trampas si alguna firma no coincide. Mira `kfe.log` / `kmenu.log`.

## Última actualización: GeometryDash.exe 2.2091 (24-oct-2026)

- Se re-ubicaron las ~60 funciones (corrimientos de +0x2500 a +0x3e00 según la zona) con `verificar-exe.py` + la tabla `.pdata` del exe
  para quedarse solo con inicios de función reales. Los modos de vehículo se identificaron por el campo de bandera que escribe cada uno
  (0x9c1 nave ... 0x9cc swing). `handleButton` cambió el prólogo (firma nueva) y `getCurrentPercent` / `shouldDraw` llevan offsets nuevos en su firma.
- Offsets de `GJBaseGameLayer` / `PlayLayer` desde 0xdb8: -0x40 (comprobado en getModifiedDelta, getCurrentPercent, la función de "Attempt %i",
  shouldDraw, updateCamera y processQueuedButtons).
- Si algo falla: `kfe.log` / `kmenu.log` dicen qué hook no coincidió.
