# CBF exacto (Kuang Menu 0.10.0)

Opción **CBF exacto** en la columna Global del menú (junto a Click Between Steps / Click On Steps). Apagada por defecto.

## Qué corrige

El Click Between Steps de RobTop en 2.2091 (`GJBaseGameLayer::update`, 0x3406d0) solo parte un paso de física **por la mitad**:
- un clic que llega en la 2.ª mitad del paso se aplica en el medio del paso;
- un clic que llega en la 1.ª mitad se aplica al inicio del paso.

Todos los clics caen en una rejilla de 1/480 s y siempre **antes** de cuando ocurrieron (hasta 2.08 ms). El CBF exacto corta el paso
justo en la hora que el propio juego le puso al clic (Raw Input + QueryPerformanceCounter, sin tocarlo): primera parte = hasta el clic,
segunda parte = el resto. El error baja a nanosegundos (el corte se limita a [1/32, 31/32] del paso, error máximo 0.13 ms en los extremos).

Lo demás sigue siendo del juego: el hilo de entrada, las marcas de tiempo, qué clics importan (`0x33ab10`), los contadores de pasos
(+1 por parte, +2 por paso entero, igual que antes), el flush del último paso. **Speedhack del menú:** también funciona. El Speedhack escala el delta time (scheduler de cocos), así que los pasos de un frame cubren `s` veces
su duración real. Al llegar cada clic al juego (`0x6713c0`) su hora se reescala: `t0 + (hora − t0) · s`, con `t0` = `m_stepTime` (0x38e8). Con el Time Warp
propio de un nivel (campo +0x330 ≠ 1) las rutinas hacen lo mismo que el juego (mitad y mitad).

## Cómo está hecho

Tres saltos de 14 bytes dentro de `GJBaseGameLayer::update` van a rutinas en memoria propia (`menu/kcbf_blob.h`, fuente en `menu/kcbf_cave.S`):

| Sitio (RVA) | Bytes | Qué reemplaza |
|---|---|---|
| 0x340a54 | 27 | límite inferior de la ventana de corte (antes: mitad del paso) |
| 0x340ac2 | 57 | el corte: ahora fracción exacta en vez de 0.5 |
| 0x341803 | 33 | fin de la 1.ª parte: prepara la 2.ª con (1 − fracción) |

- Se activan/desactivan escribiendo los bytes (los originales se guardan en el propio .h) desde el hook de render: mismo hilo que `update`, nunca dentro de él.
- Si **un solo byte** de los tres sitios no coincide con 2.2091, no se toca nada y el menú lo dice (`kmenu.log`: "exact CBF: the game's code does not match").
- La fracción del primer trozo se reduce 1e-6 para que el redondeo a `float` nunca deje el clic en el trozo anterior.
- `kmenu.log` escribe `[cbf] exact cuts in the last 10 s: N` mientras está activo.
- Solo hay un corte por paso (como en el juego). Si dos clics caen en el mismo paso (p. ej. J1 y J2), el segundo se aplica en el instante del primero (error < 4.17 ms solo entre ellos).

## Probado

No he podido ejecutar el juego (no hay Windows aquí). Las tres rutinas se probaron en simulación nativa (600 000 casos aleatorios): el clic nunca se aplica
en el trozo anterior, siempre en el siguiente, error ≤ 4 ns, las dos partes suman exactamente el paso, y con time warp ≠ 1 salen
exactamente las mitades de siempre. **Falta la prueba real en el juego**: si crashea, apaga la opción (o borra `gl.cbf=1` de `kmenu.ini`) y manda `kmenu.log`.

## Si RobTop saca otro parche

Los tres sitios dependen de los offsets de campo (0x38e8 tiempo del paso, 0x38f0 bandera de corte, 0x330 time warp) y de los registros que usa
el compilador (xmm6/7/14/15, r13d, rsp+0x38/0x68, rbp-0x80). Si `verificar-exe` mueve `update`, hay que volver a localizar los tres bloques
y regenerar el blob (`clang -c -target x86_64-linux-gnu menu/kcbf_cave.S`, ajustar los desplazamientos).

---

# Revisión del CBF exacto y Precision Fix (Kuang Menu 0.10.1)

## CBF exacto: qué se comprobó
- Los 3 sitios (0x340a54, 0x340ac2, 0x341803) coinciden **byte a byte** con tu `GeometryDash.exe` 2.2091. Los saltos de vuelta (0x340a6f, 0x340afb, 0x340b1d) y el `jl 0x340b40` del original también cuadran con las rutinas.
- No encontré un fallo en las rutinas. Lo que faltaba era una forma de **ver** si corta: ahora, con la opción activa, el menú muestra **"Cortes exactos aplicados: N"** debajo de la opción.
  - N sube al hacer clic dentro de un nivel → el parche funciona (la diferencia con el del juego es de hasta 2 ms, y a ~5000 FPS cada frame ya dura 0.2 ms, así que se nota poco).
  - N se queda en 0 con clics en nivel → el juego no toma esa ruta: comprueba que el Click Between Steps del juego esté activo, y mándame `kmenu.log`.
- Sigue sin probarse dentro del juego (aquí no hay Windows).

## Precision Fix (nuevo, columna Global, apagado por defecto)
Las direcciones del Geode (0x736f6, 0x73716, 0x73703, 0x1a84d8) son de la DLL 2.2081 y **no sirven** en la 2.2091 (en tu DLL caen en código de rutas de archivos). Por eso es una implementación propia:
- `CCDirector::calculateDeltaTime` (libcocos2d.dll, RVA 0xc1840) saca el tiempo del frame de un `timeval` en **microsegundos enteros** (`gettimeofday`, RVA 0x81450). A 5000 FPS (frames de 200 µs) eso es ±0.5 % de temblor por frame.
- Un salto de 14 bytes en 0xc1887 (51 bytes, hasta 0xc18ba) lleva a una rutina (`menu/kpfix_cave.S`, `kpfix_blob.h`) que calcula el tiempo con `QueryPerformanceCounter` (≈100 ns o mejor).
- Seguridad: si los bytes de la DLL no coinciden, no toca nada; si el "último tiempo" del juego cambia por su cuenta (carga de nivel/pausa) o el paso supera 1 s, usa la cuenta original ese frame. Se quita desde el menú sin reiniciar.
- `kmenu.log` escribe `[pfix] precise frames: ...` cada 10 s.
- Probado en simulación nativa (200 000 frames): error máximo 3e-11 s, cae al cálculo original en paradas y reinicios. **No probado en el juego.**

## 0.10.2: clics que se perdían al hacer spam
Causa probable: con el corte exacto, si en el mismo paso de física caían una pulsación y una suelta, el corte se hacía en la **primera** y ambas se aplicaban en el mismo instante (el tap duraba 0). Ahora, con varios clics en el mismo paso, el corte se hace en el **último**: los anteriores se aplican al inicio del paso y el último en su instante exacto, así pulsar y soltar nunca coinciden. Con un solo clic en el paso todo sigue exacto. Probado en simulación (3 clics → corte en el último; 1 clic → en el suyo); sin probar en el juego.

## 0.10.3: dos cortes por paso
Si en un mismo paso de física caen varios clics (pulsar + soltar, spam), ahora se hacen **dos cortes**: uno en el primer clic y otro en el **último** (si hay al menos 1/32 de paso entre ambos). El paso queda en 3 partes: antes del primero / entre ambos / después del último, así pulsar y soltar nunca caen en el mismo instante y cada uno se aplica en su hora exacta. Con un solo clic en el paso sigue habiendo un solo corte. Con Time Warp ≠ 1 sigue el corte por la mitad de siempre.
- La parte intermedia es un "pase" extra del bucle de física (como el primero de un paso partido). Los contadores que el juego sube una vez por parte (+0x3e0, +0x3e8, +0x3f4, +0x3668) se bajan antes de ese pase para que un paso sume lo mismo que con dos partes.
- Ya no se mueve el primer clic al inicio del paso (lo que hacía la 0.10.2).
- El menú muestra "pasos con 2 cortes: N". Si el juego se comporta raro (física, contador de intentos, checkpoints), apaga CBF exacto y mándame `kmenu.log`.
- Probado en simulación nativa (cortes, fracciones, contadores, orden de pases); sin probar en el juego.
- Retraso restante: la física corre a 240 Hz y el resultado de un clic solo se ve cuando termina el paso que lo contiene (hasta ~4 ms). Ningún CBF lo elimina: el CBF cambia en qué instante cuenta el clic, no cuándo se pinta.

---

# 0.10.4: Smart Start Pos, dual falso
- Causa: el dual se aprendía leyendo si el segundo icono estaba visible, y se aplicaba aunque el nivel no tuviera ningún portal de dual. Ahora un start pos solo puede ser dual si el nivel empieza en dual o hay un portal de dual (286) en/antes de ese start pos. Vale también para lo ya guardado en `kmenu-smartsp.txt` (se corrige al aplicarlo, no hace falta borrarlo).
- CBF exacto: revisado de nuevo, sin cambios (no se tocó el código máquina porque no se puede probar sin el juego). Sin probar en el juego.

---

# 0.10.5: un corte por cada clic
- Antes (0.10.3) un paso con varios clics solo se cortaba en el **primero** y el **último**; los de en medio se aplicaban al instante del primero (error de hasta medio paso, ~2 ms, en spam). Ahora se corta en **cada clic** del paso: el primero hace el corte normal y los siguientes (hasta 4 más) se guardan en una cola y cada uno se ejecuta como un pase intermedio, en su hora exacta.
- Un clic que cae a menos de 1/32 de paso (0.13 ms) del corte anterior se aplica en ese corte anterior (error < 0.13 ms). Con Time Warp ≠ 1 sigue el corte por la mitad de siempre.
- Los contadores que el juego sube por cada parte se bajan en cada pase intermedio, igual que en 0.10.3, así un paso suma lo mismo sin importar cuántos cortes tenga.
- El menú dice ahora "pasos con varios cortes: N".
- Los sitios que se parchean en el juego (0x340a54, 0x340ac2, 0x341803) y los bytes originales no cambian; solo cambió el código de las rutinas (`kcbf_cave.S` → `kcbf_blob.h`, ahora 978 bytes; los desplazamientos de las rutinas son 0x100, 0x13e y 0x294).
- Probado con un banco de pruebas nativo que ejecuta las rutinas reales con memoria falsa del juego (1, 2, 6 clics, clics pegados, extremos y Time Warp): las partes suman siempre 1, hay un pase intermedio por cada corte en cola, las fracciones suben y los contadores quedan en cero neto. **Sin probar dentro del juego**: si crashea o la física se ve rara, apaga CBF exacto y manda `kmenu.log`.

---

# 0.10.6: CBF exacto también con Time Warp
- Antes, en un nivel con su propio Time Warp (campo +0x330 ≠ 1) las rutinas hacían el corte por la mitad de siempre. Ahora hacen el corte exacto también ahí, y con varios clics en el mismo paso se aplica igual que en 0.10.5.
- Por qué el juego no puede: el reloj del juego (`m_stepTime`) avanza `g` (tiempo de juego) por paso, pero los clics están en tiempo real y la ventana del paso mide `d = g / tw`. En el paso `i` de un frame, el paso empieza de verdad en `t0 + i·(d − g)`, no en `t0`. Con un solo paso por frame (FPS muy altos, `i = 0`) coinciden; con varios pasos por frame el desfase crece con `i`. La rutina corrige `t0` con `i·(d − g)` (`i` = `[rsp+0x28]`, `g` = xmm14) y usa ese inicio tanto para elegir los clics como para calcular las fracciones. Con tw = 1 el resultado es idéntico bit a bit al de 0.10.5.
- Interruptor nuevo en el menú, debajo de CBF exacto: **CBF exacto con Time Warp** (activado por defecto; `gl.cbf.tw` en `kmenu.ini`). Apagado, vuelve el corte por la mitad del juego con Time Warp ≠ 1, como antes.
- Probado con el banco de pruebas nativo (tw = 1, 0.5, 2, 0.25; pasos 0 a 7; con el interruptor apagado): el inicio corregido y la ventana cuadran con lo esperado, la primera fracción cae donde está el clic, las partes suman 1 y los contadores quedan en cero neto. **Sin probar dentro del juego.** Si algún nivel con Time Warp se siente peor, apaga el interruptor y avísame.

---

# 0.10.7: revisión con el exe del 24-oct y tu kmenu.log

- **Funciona**: tu log (0.10.3) muestra más de 7 000 cortes exactos en una sesión, sin errores.
- **Time Warp (quitado el arreglo de la 0.10.6)**: en `update` (0x3406d0) el paso `i` empieza en `m_stepTime`, que ya avanza `xmm14` (largo real del paso) en cada parte (0x341596). Lo que no cuadra con Time Warp es otra cosa: la ventana de clics y el límite de `processQueuedButtons` (0x33a9a0: aplica los clics con hora < `m_stepTime` + `dt` de la parte) usan `[rsp+0x68]` = `dt / tw`. Sumar `i·(d − g)` al inicio desplazaba la ventana y, con varios pasos por frame, el clic podía caer en la parte equivocada. Un corte exacto con Time Warp necesitaría cambiar también ese `dt` de cada parte, y ese valor se usa en más sitios (0x3d8, vtable+0x428). Por eso con Time Warp ≠ 1 se deja el corte por la mitad del juego (el interruptor "con Time Warp" queda apagado y oculto; el código sigue en el blob, sin usarse).
- **Contador 0x3f4 en los pases intermedios**: el juego le suma `round(tw·1000)·0.5` por parte; la rutina resta 500, que solo es correcto con tw = 1. Como los pases intermedios ya solo existen con tw = 1, ahora siempre cuadra.
- **Precision Fix**: `CCDirector::calculateDeltaTime` (libcocos 0xc1840) no tiene ninguna llamada en esta versión. El bucle de `CCApplication` (~0x76060) mide el frame con QPC y llama a `setDeltaTime` (0xc2e00), así que el tiempo del frame ya es preciso. Tu log: `precise frames: 0 ... original-math frames: 0` toda la sesión. Se quitó del menú y queda apagado.
- Comprobado: el blob `kcbf_blob.h` es exactamente `kcbf_cave.S` ensamblado (solo cambian 4 bytes de relleno entre los datos y el código).
- Visto de paso: `input thread: not found in this version` → Input Boost no encuentra el hilo de entrada en el exe nuevo (hay que volver a buscar esa dirección).

---

# 0.11.0: Time Warp exacto (bien) y límites 1/64

- Con Time Warp `tw` ≠ 1, en `update`: `m_stepTime` avanza `g` = xmm14 (largo real del paso) por paso, pero la ventana de clics y el límite que se pasa a `processQueuedButtons` (0x33a9a0, un solo llamador: 0x342961) usan `d = g / tw` (`[rsp+0x60]` / `[rsp+0x68]`). Con `tw < 1` el juego aplica clics que todavía no habían pasado (hasta `g·(1/tw − 1)` antes).
- Arreglo: cave0 usa `g` como largo de la ventana cuando `tw` ≠ 1 (sin el desplazamiento `i·(d−g)` de la 0.10.6), y el hook `h_procQueued` multiplica el `dt` que recibe por `tw` (solo con CBF exacto activo y el interruptor encendido; el hook ya existía para el medidor de latencia). Así la ventana, el corte y el límite usan el mismo tiempo real que los clics.
- Pase intermedio: el juego suma `trunc(roundf(tw·1000)·0.5)` a +0x3f4 por parte; cave2 resta `floor((tw·1000 + 0.5)/2)` en vez de 500.
- `kMin`/`kMax` 1/64 y 63/64. Offsets de las rutinas: cave0 0x100, cave1 0x174, cave2 0x2d2 (blob 1066 bytes, regenerado desde `kcbf_cave.S`).
- Si el hook de `processQueuedButtons` no se pudo poner, el modo Time Warp no se activa (corte por la mitad del juego). Sin probar dentro del juego.


## 0.13.0: Speedhack y el acumulador de pasos

- `update` (0x3406d0) sólo corre pasos enteros: 0x340630 hace `n = floor((dt + resto) / paso)` y guarda el resto en `+0x3350` (segundos de juego). `m_stepTime` (0x38e8) se pone al final de cada update a la hora QPC del inicio de ese update (0x34190f / 0x341e6b), también en frames sin pasos.
- Así, los pasos del update N empiezan en `U(N-1)` (tiempo real) pero simulan el tiempo de juego que va `resto(N-1)` por detrás. La posición correcta de un clic en tiempo real `c` es `U + s·(c − U) + resto`. Sin el resto, con Speedhack lento y FPS altos (frames con 0 pasos) los clics de un frame sin pasos caían todos al inicio del siguiente paso y los del frame siguiente sólo en su primera parte: spam irregular.
- `queueKey` (0x6713c0, virtual del teclado) corre en el hilo principal antes del update y mete el clic directo en `+0x3400`. Los clics que quedan en la cola tras el update se recolocan desde su hora real en el render hook (`cbfRemapSEH`, tabla hora puesta → hora real, igualdad exacta del double).
- Sólo con Speedhack ≠ 1 y Time Warp = 1 se suma el resto; a 1x la hora del clic no se toca (igual que antes).

# 0.14.7: CBF como en la 0.12.0
- A petición: se devuelve el funcionamiento de la 0.12.0. Las rutinas (`kcbf_cave.S`, `kcbf_blob.h`) eran idénticas byte a byte; lo único distinto era el reajuste de la hora de los clics con Speedhack ≠ 1 (acumulador y `cbfRemapSEH`, de la 0.13.0), que se quitó. A velocidad 1x no cambia nada. Con Speedhack lento y FPS altos vuelve el spam irregular que arreglaba la 0.13.0.

---

# 0.14.8: reloj de pasos (interruptor de prueba, apagado por defecto)

- Interruptor nuevo: columna Global, debajo de CBF (`gl.cbf.acc` en `kmenu.ini`; necesita el CBF exacto activo). **Apagado = idéntico a la 0.14.7.** Vale a velocidad normal, con el Speedhack del menú y con el Time Warp propio de un nivel.
- Comprobado desensamblando `update` del `GeometryDash.exe` 2.2091 (los 3 sitios del parche, `processQueuedButtons` y `queueKey` coinciden byte a byte):
  - `0x340630`: `n = floor((dt + m_extraDelta) / paso)`, `paso = min(1, timeWarp) / 240`, todo en las unidades de `dt`; deja `total − n·paso` en `m_extraDelta` (+0x3350, double). Con Time Warp las unidades no cambian, por eso la misma fórmula vale con `tw ≠ 1`.
  - `m_stepTime` (+0x38e8) solo se pone a la hora de inicio del update (0x341e6b) si ese update corrió pasos. **Un frame sin pasos no lo toca** (`jae 0x341f79`, salta al epílogo) y solo suma su `dt` a `m_extraDelta`. (Las notas de la 0.13.0 decían lo contrario.)
  - Los pasos del update siguiente empiezan `resto` por detrás del reloj con el que se comparan los clics, donde `resto` = `m_extraDelta` que dejó el ÚLTIMO update con pasos (no el valor actual, que incluye los frames vacíos). El juego pone cada clic `resto` demasiado pronto (0 a 4,17 ms).
- Con el interruptor: al llegar el clic (`0x6713c0`) su hora pasa a `m_stepTime + s·(c − m_stepTime) + resto` (s = Speedhack del menú). El hook de render guarda `resto` cada vez que `m_stepTime` cambia y recoloca desde su hora real los clics que quedan en la cola (tabla "hora puesta → hora real", hasta 16). Con Time Warp ≠ 1 hace falta el corte exacto con Time Warp activo (si no, queda como la 0.14.7). El medidor de latencia usa la hora real.
- `kmenu.log`: `[cbf] step clock: N presses placed in the last 10 s, leftover time avg X ms, max Y ms`. El menú muestra "Clics colocados: N (sobrante medio …, max …)".
- Prueba (código real + `update` simulado como en el exe; 60 s, ~8 clics/s, 140 a 5000 FPS; Speedhack 0.5/1/2; Time Warp 0.5/2): 0.14.7 → error medio de −0,3 a −2,3 ms (hasta −4,17 ms; a 2x hasta +9,6 ms); con el interruptor → 0,0000 ms en todos los casos, 0 clics perdidos. **No probado dentro del juego** (aquí no hay Windows).
- Error corregido durante esta revisión: la primera versión usaba el `m_extraDelta` actual; con FPS altos (frames sin pasos) iba hasta +4 ms tarde.
