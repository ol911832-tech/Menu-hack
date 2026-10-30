# Mods Kuang para Geometry Dash (actualizados a 2.2091 — 24-oct-2026)

Tres mods que funcionan **sin Geode**, más un instalador. Todos se cargan con un único cargador
(`winmm.dll`) que va en la carpeta del juego.

| Mod | Archivo en el juego | Programa | Qué hace |
|---|---|---|---|
| Kuang Frame Extrapolation 1.2.0 | `kfe.dll` | `KuangFrameExtrapolation.exe` | Cámara, iconos, estelas (normal y wave), fondo y objetos movidos por triggers suaves a cualquier FPS (extrapolación o interpolación). |
| Kuang Optimizer | `kopt.dll` | `KuangOptimizer.exe` (se abre como administrador) | Menos input lag (vaciar la cola de la GPU, sin VSync, prioridades, temporizador, plan de energía) y ajustes de Windows mientras juegas. |
| Kuang Menu 0.14.0 | `kmenu.dll` | Ninguno: vive dentro del juego, se abre con **Tab** | Menú tipo Mega Hack: Noclip, Speedhack, Safe Mode, FPS Bypass, hitboxes, Click Between/On Steps del juego y **CBF exacto**, Input Boost, FPS estables, Visibility Throttle, Render Stats con perfil detallado, Smart Start Pos que aprende el estado exacto y cambio de start pos con teclas, controles de Frame Extrapolation, textos en pantalla (FPS, CPS, reloj, intentos, mejor run...), Jukebox, trail con tu imagen, idioma EN/ES, color. |
| Cargador | `winmm.dll` | (no tiene) | Reenvía todo al `winmm.dll` real de Windows y carga los tres mods de arriba si están en la carpeta. |

## Actualizar todo en un clic (recomendado)

Doble clic en **`ACTUALIZAR.bat`**. Sin preguntas:

1. cierra los programas Kuang (instalador, Frame Extrapolation, Optimizer);
2. borra `build\` y `dist\` (compilación limpia: no queda nada de una versión vieja);
3. compila todo desde cero, incluido el propio instalador;
4. abre `KuangInstaller.exe` en modo automático: cierra Geometry Dash de forma normal (para que guarde), instala los mods, comprueba byte por byte que quedaron los nuevos y se cierra solo a los 4 segundos si todo salió bien. Si algo falla, se queda abierto con el error en rojo.

Pide permisos de administrador una vez (el Optimizer corre como administrador y hay que poder cerrarlo). Tus ajustes (`kfe.ini`, `kopt.ini`, `kmenu.ini`, `kmenu-smartsp.txt`) no se tocan. Instala los mods que tengas marcados en el instalador (si no hay ninguno, todos).

En el instalador normal también está la casilla **Clean build** (activada por defecto): con "Compile from the source code first" borra `build\` y `dist\` antes de compilar.

## Instalar (lo fácil)

1. Solo la primera vez: doble clic en `instalar-herramientas.bat`. Instala Visual Studio Build Tools (C++ y Clang) y CMake.
2. Doble clic en `KuangInstaller.exe`.
3. Marca los mods que quieres y pulsa **Install / Update**.

El instalador:
- cierra Geometry Dash y los programas Kuang si están abiertos (te pregunta antes);
- compila;
- copia los archivos al juego y comprueba byte por byte que se instaló la versión nueva.

Cada mod tiene un botón **Settings**: abre el programa de KFE u Optimizer, o `kmenu.ini` en el caso del menú (que se configura dentro del juego con Tab).

**Uninstall all** quita los mods y el cargador. Tus ajustes (`kfe.ini`, `kopt.ini`, `kmenu.ini`) no se borran.

> La primera vez todavía no existe `KuangInstaller.exe`: ejecuta `compilar.bat` una vez y aparecerá en esta carpeta.

`KuangInstaller.exe` lleva dentro los tres mods y los dos programas ya compilados. Se puede pasar solo, sin el código,
a otra PC, y ahí instala todo sin herramientas. Con la casilla "Compile from the source code first" usa tu código más reciente.
Los programas de KFE y Optimizer se instalan en `%LOCALAPPDATA%\Programs\Kuang\`.

## Compilar a mano

`compilar.bat` compila todo y deja el resultado en `build\Release\` y en `dist\`. El registro queda en `compilar.log`.
Cierra antes Geometry Dash y los programas Kuang; si no, Windows no deja reemplazar los archivos (sale "Permission denied").

Por dentro hace esto:
```
cmake -S . -B build -A x64 -T ClangCL
cmake --build build --config Release
```

## Carpetas del código

```
CMakeLists.txt            todos los proyectos y cómo se copian a dist\
compilar.bat              compila todo
instalar-herramientas.bat Build Tools + CMake (una vez)
shared/kfe_shared.h       nombres, versión y la memoria compartida entre kfe.dll y su programa
proxy/winmm_proxy.cpp     el cargador: 186 funciones reenviadas a System32\winmm.dll + carga kfe/kopt/kmenu
mod/kfe.cpp               Frame Extrapolation (hooks a updateCamera, update, CCScheduler::update, drawScene, updateObjectSection)
app/app.cpp               programa de KFE (ImGui + DirectX 11)
optimizer/kopt.cpp        parte del Optimizer que va dentro del juego
optimizer/app_opt.cpp     programa del Optimizer (ajustes de Windows, teclado, puntos de restauración...)
optimizer/*.inc, *.hpp    tablas de ajustes y textos que vienen de tu GD Optimizer
menu/kmenu.cpp            Kuang Menu (ImGui + OpenGL dentro del juego, hook a CCEGLView::swapBuffers)
installer/installer.cpp   Kuang Installer (lleva los mods dentro, recursos 101-106)
cmake/soft_copy.cmake     copia a dist\ sin romper la compilación si un archivo está abierto
third_party/imgui         Dear ImGui 1.91.9 (licencia MIT)
third_party/minhook       MinHook (licencia BSD-2)
```

## Datos de Geometry Dash 2.2091 que usan los mods

Las direcciones de funciones se buscaron por sus bytes iniciales en el `GeometryDash.exe` 2.2091 y los offsets de campos se comprobaron
desensamblándolo. En esta versión `GJBaseGameLayer` perdió otra vez los 0x40 bytes que tenía antes de `m_player1` (el parche del 20-oct
los había añadido): todos sus campos (y los de `PlayLayer`) desde +0xdb8 bajaron -0x40. `timeWarp` (+0x330), `CCNode`, `GameObject`,
`PlayerObject`, `LevelSettingsObject` y `GJGameLevel` no se movieron.
Antes de enganchar nada, cada mod compara los primeros bytes de la función. Si el juego se actualiza y no coinciden,
el mod se desactiva solo, lo anota en su `.log` y no rompe el juego.

| Qué | Dónde |
|---|---|
| GJBaseGameLayer::update / updateCamera / getModifiedDelta | 0x3406d0 / 0x345c60 / 0x340630 |
| GJBaseGameLayer::updateObjectSection | 0x330ba0 |
| PlayLayer::destroyPlayer / resetLevel | 0x512390 / 0x518c00 |
| player1 / player2 / objectLayer | +0xdb8 / +0xdc0 / +0x10c8 |
| m_isTestMode (Safe Mode) / m_extraDelta / m_started | +0x3328 / +0x3350 / +0x3358 |
| m_debugDrawNode / m_anticheatSpike | +0x3870 / +0x38e0 |
| PlayLayer m_attemptLabel / m_decimalPercentage / intentos | +0x3a88 / +0x3a98 / +0x3164 |
| PlayerObject m_regularTrail / m_streakStrokeWidth | +0x7a8 / +0x92c |

## Archivos que se crean en la carpeta del juego

- `kfe.ini`, `kopt.ini`, `kmenu.ini`: los ajustes de cada mod.
- `kfe.log`, `kopt.log`, `kmenu.log`: qué pasó al cargar. Mándalos si algo falla.
- `kmenu-trail-N.png`: la imagen de tu trail personalizado.
- `winmm.dll.kfe-backup`: solo si ya había otro `winmm.dll` antes de instalar. Al desinstalar se devuelve a su sitio.

## Cambios (22-oct-2026)

- **Frame Extrapolation 1.1.3**: arreglada la vibración del ícono (y de lo que sigue a la cámara) durante los triggers de Zoom. La cámara se predice ahora en coordenadas del mundo + escala, igual que el ícono; sin costo de rendimiento.
- **Kuang Menu 0.8.0**:
  - *Smart Start Pos* aprende el estado exacto (modo, velocidad, tamaño, gravedad, dual) al pasar por cada start pos jugando desde el inicio o desde uno ya aprendido; se guarda por nivel en `kmenu-smartsp.txt`. Mientras no esté aprendido, usa los portales. El aviso del Start Pos Switcher dice "exacto" o "por portales".
  - *Input Boost* (columna Global): mide cuántos FPS aguanta tu PC sin tirones y reparte los frames parejos en un múltiplo de 240.
  - *Visibility Throttle*: modo Auto (Hz del monitor), sin "(experimental)".
  - *Render Stats*: sin "(experimental)"; tercera línea con triggers / colores / objetos y línea `[deepprofile]` en `kmenu.log`.
  - El campo de FPS ahora está justo debajo de FPS Bypass.

## Cambios 0.8.1

- *Smart Start Pos*: un portal cuenta en cuanto el jugador lo toca (borde del portal, no su centro). Si hay varios portales del mismo tipo apilados en una columna, solo cuenta el que está a la altura más cercana al start pos. En `kmenu.log` queda qué portales de gravedad usó y cuáles ignoró para el start pos desde el que juegas.
- *Input Boost*: ya no se deja engañar por los frames que traen un paso de física (1 de cada ~8 a 2000 FPS). Mide la mediana en ventanas de 3 s, cambia de a 240 FPS, solo tras varias ventanas iguales, y nunca baja de 960 FPS ni de los Hz del monitor.

## Cambios 0.8.2

- *Smart Start Pos* cuenta también los orbes y pads de gravedad antes del start pos (orbe azul, verde, dash verde y araña; pad azul y araña): cada uno invierte la gravedad, como si lo tocaras. Se puede apagar en el menú de la opción (tres puntos) para niveles con orbes falsos u opcionales. Columnas apiladas: solo cuenta el que está a la altura más cercana al start pos.

## Frame Extrapolation 1.2.0

- *Smooth trails* (activado por defecto, `smooth_trails` en `kfe.ini`, casilla en el programa): el final de la estela normal, la del fuego de la nave/OVNI y la del wave queda pegado al ícono. El juego solo alarga las estelas en cada paso de física, así que entre pasos terminaban un poco detrás del ícono.
  - Estela normal y fuego: antes de dibujar se les añade un punto temporal donde se dibuja el ícono y se quita después.
  - Wave: el juego ya la reconstruye cada frame; durante esa reconstrucción se adelanta solo su punta y luego se devuelve el valor. Sin trabajo extra.
  - La física y los puntos guardados de las estelas no se tocan. Solo actúa cuando el ícono va por delante (en interpolación, cuando va por detrás, no se toca para no doblar la estela en las esquinas del wave).

## Cambios Kuang Menu 0.14.9

- **CBF: reloj de pasos encendido por defecto** (si ya tenías `gl.cbf.acc` guardado en `kmenu.ini`, se respeta tu valor). Sigue pudiéndose apagar en la columna Global.
- **KuangSetup.exe: instalador tipo Geode.** Asistente de 5 pasos (bienvenida, carpeta del juego detectada sola vía Steam, opciones, progreso, final). Lleva todos los mods dentro, se registra en «Aplicaciones instaladas» (desinstalador `KuangUninstall.exe`), crea accesos directos y no necesita Geode. `--uninstall` desinstala; `--silent` instala sin ventana. Código en `setup/`.
- **GitHub Actions**: `.github/workflows/build.yml` compila todo en Windows y sube `KuangSetup.exe` como artefacto (y como Release si subes una etiqueta `v*`). Sin probar dentro de Windows aquí.

## Cambios Kuang Menu 0.14.8

- **CBF: reloj de pasos (prueba)**: interruptor nuevo en la columna Global, debajo de CBF (apagado por defecto; con él apagado todo funciona igual que en la 0.14.7). El juego solo corre pasos enteros de física y guarda el tiempo sobrante para el frame siguiente, así que cada clic se aplica antes de lo que ocurrió (de 0 a 4 ms, distinto en cada frame, también a velocidad normal). Con el interruptor encendido se suma ese sobrante a la hora de cada clic (velocidad normal, Speedhack y Time Warp). Detalles, comprobación con el exe y prueba en `NOTAS-CBF.md` (0.14.8). Sin probar dentro del juego.

## Cambios Kuang Menu 0.8.3

- **Latencia de entrada** (columna Status, nuevo texto en pantalla): mide desde que Windows recibió tu tecla o clic hasta que la física la aplicó, usando la hora exacta que el propio juego le pone a cada pulsación. También escribe `[inputlat]` en `kmenu.log` cada 10 s (con cuánto esperó la tecla al siguiente frame).
- **Input Boost** además sube a prioridad máxima el hilo de entrada del juego (libcocos2d de esta versión ya lee el teclado con Raw Input en su propio hilo y marca cada pulsación con la hora exacta), para que la hora se tome al instante aunque la CPU esté ocupada.
- No se añadió un lector de teclado propio: el juego ya lo trae (Raw Input + hora exacta + Click Between Steps), y leer el teclado otra vez antes de la física no adelanta nada porque el juego ya lo lee justo después de la espera de Input Boost.

## Cambios Kuang Menu 0.8.4

- **Input Boost** ahora solo sube la prioridad del hilo de entrada del juego. Ya no toca los FPS.
- **FPS estables** (nueva, columna Global, apagada por defecto): lo que antes hacía Input Boost con los FPS. Dentro de los niveles pone un tope automático que tu PC siempre aguanta y reparte los frames parejos. La física de esta versión va siempre a 240 pasos por segundo sin importar los FPS, así que no cambia nada de la física: es como escribir menos FPS en FPS Bypass.

## Cambios Kuang Menu 0.8.5

- *Latencia de entrada*: ahora cuenta también las pulsaciones que el juego aplica "de una vez" (antes se las saltaba y por eso no salía nada).
- *Smart Start Pos*: la línea del log solo se escribe cuando cambia el start pos o su resultado, no en cada intento.

## Cambios Kuang Menu 0.9.0

- **Mostrar trayectoria** (columna Jugador, apagada por defecto), con las mismas opciones que la de QOLMod: los dos caminos (mantener y soltar), el que coincide con lo que haces ahora en el color Mantener y el otro en Soltar, colores por jugador, J1 y J2 por separado, rastro de hitbox por jugador, y las hitboxes reales (exterior e interior, medidas por el juego para cada vehículo y tamaño) donde moriría. Usa la física del propio juego sobre una copia oculta del jugador: sigue bloques, pinchos, rampas, pads (con la fuerza del juego, también los de reversa) y portales de modo, gravedad, tamaño y velocidad, sin activar nada del nivel. Los orbes no se siguen, igual que en QOLMod. Además se puede cambiar el largo.
- **Utilidades** como en Mega Hack: botones de Click / Izquierda / Derecha para J1 y J2 (se mantienen mientras los mantienes), Reiniciar nivel, Modo práctica, Descompletar nivel (con confirmación), Ajustes del juego, carpetas, y Crashear el juego (con confirmación).
- **Descompletar nivel**: deja el nivel en 0% en este PC: progreso normal y de práctica, intentos, saltos, tiempo, récords de plataforma, y sus marcas de completado / estrellas / demon / monedas, restándolas de tus estadísticas para poder ganarlas otra vez. Solo niveles online. El leaderboard online no se puede tocar: ese récord está en el servidor de RobTop.
- **Arreglo de porcentaje** (columna Jugador, encendido): en algunos niveles, al empezar desde un start pos, el porcentaje salía en 100% o empezaba en 0%. Ahora empieza en el porcentaje real del start pos.
- **Sin animaciones** (columna Global): acceso directo a la opción Fast Menu del propio juego. Es el mismo interruptor del juego, nada más.
- **Jukebox**: la canción elegida se guarda al momento y por ID de nivel (y por ID de canción), así que ya no se pierde al salir del nivel o del juego. Solo se quita con "Quitar".

## Cambios Kuang Menu 0.10.3

- **CBF exacto** (columna Global, apagado por defecto): el Click Between Steps del juego solo parte el paso de física por la mitad (rejilla de 1/480 s); esto lo corta en el instante exacto del clic. Detalles, límites y cómo probarlo en `NOTAS-CBF.md`.

## Cambios Kuang Menu 0.10.7

- **CBF exacto**: con Time Warp de nivel (≠ 1) vuelve al corte por la mitad del juego. La "corrección" de la 0.10.6 partía de una idea equivocada: el reloj del paso (`m_stepTime`) sí avanza el largo real de cada paso, así que el clic se aplicaba en la parte equivocada. Se quitó su interruptor. Con Time Warp normal (1) todo sigue igual: un corte exacto por cada clic del paso.
- **Precision Fix** quitado del menú: en esta versión `CCDirector::calculateDeltaTime` nunca se ejecuta (el bucle del juego ya mide cada frame con QueryPerformanceCounter). Tu log lo confirma: 0 frames en toda la sesión. No hacía nada.

## Cambios Kuang Menu 0.10.8

- **Smart Start Pos, gravedad** (cuando el start pos todavía no está aprendido):
  - Los **pads** ya no "invierten" a ciegas: un pad de suelo deja la gravedad invertida y uno de techo (girado 180° o volteado) la deja normal. Antes, dos pads seguidos se anulaban aunque el jugador solo pudiera tocar uno.
  - El **orbe dash verde (1704)** ya no cuenta como cambio de gravedad (no lo es); ahora cuenta el **orbe dash rosa (1751)**, que sí la cambia.
  - En columnas de portales apilados se elige el que está en el **camino del jugador** (estimado entre el último portal usado y el start pos), no siempre el más cercano a la altura del start pos.
  - Los start pos **aprendidos** siguen mandando (son exactos). Si alguno se aprendió mal con versiones viejas, usa "Olvidar este nivel" en el menú de Smart Start Pos.
- **Input Boost** vuelve a encontrar el hilo de entrada del juego en el exe nuevo (lo busca por sus bytes en vez de una dirección fija, así sobrevive a futuros parches).

## Cambios Kuang Menu 0.10.9

- **Trail personalizado**:
  - Nueva opción **Solo en el ship** (activada por defecto): la imagen se pone en el trail de la nave mientras vuelas y el trail normal vuelve al salir de la nave.
  - Si la imagen no se puede cargar (archivo borrado, formato raro), el menú lo dice debajo del nombre y `kmenu.log` escribe `custom trail: could not load the image ...`. Cuando carga escribe `custom trail: image loaded` y, al ponerla en el trail, `custom trail: on the ship trail of player 1`.
  - Textos corregidos: la imagen va a lo largo del trail (ancho de borde a borde, alto a lo largo), como la dibuja el juego.

## Cambios Kuang Menu 0.11.0

- **CBF exacto con Time Warp, bien hecho** (interruptor debajo de CBF exacto, activado por defecto). Con Time Warp ≠ 1 el juego mide el paso en tiempo de Time Warp pero los clics van en tiempo real: con Time Warp < 1 aplicaba los clics **antes de tiempo** (fallo del propio juego) y solo cortaba por la mitad. Ahora la ventana del corte usa el largo real del paso y el límite de `processQueuedButtons` se corrige igual (en el hook que ya existía), así que el corte es exacto también ahí. El contador que el juego sube por parte (+0x3f4) se compensa con el valor real de cada Time Warp (antes, 500 fijo).
- **Más preciso en los extremos del paso**: el corte se permite entre 1/64 y 63/64 del paso (antes 1/32): error máximo 0.065 ms (antes 0.13 ms).

## Cambios Kuang Menu 0.14.0

- El botón **Kuang Mods** ahora abre una pantalla aparte (escena propia con fundido, como la de Geode): fondo azul del juego, arte de las esquinas, flecha de volver (o Escape) y pestañas **Instalados**, **Descargar** y **Recientes**. Instalados: lista de mods instalados y sus opciones. Descargar: los mods Kuang que no tienes; "Instalar" abre Kuang Installer si lo encuentra (carpeta del juego o %LOCALAPPDATA%\Programs\Kuang); todavía no hay descarga por internet. Recientes: últimos cambios.
- Botón redibujado con los colores y bordes de los botones redondos del juego (aro blanco, aro negro, verde en dos tonos, letra dorada con borde negro y sombra), al mismo tamaño que el de Newgrounds.
- La fila del menú principal se detecta con sus botones visibles en la misma línea (el juego tiene un quinto botón oculto ahí).

## Cambios Kuang Menu 0.13.0

- **Botón "Kuang Mods" en el menú principal**, como el de Geode: un botón redondo verde con una K al final de la fila de abajo (logros, ajustes, estadísticas, Newgrounds); la fila se recentra. Abre una ventana con el estilo normal de Geometry Dash (fondo marrón, casillas, flechas de página, botones (i)): a la izquierda Kuang Menu, CBF, Frame Extrapolation y Optimizer con su estado; a la derecha sus opciones. Optimizer: todas las que el mod aplica dentro del juego (activado, cola de GPU, V-Sync, prioridad de CPU, GPU y disco, vigilancia, afinidad, boost, temporizador y plan de energía) y un botón para abrir la app con los ajustes de Windows que piden administrador. Mientras está abierta, el menú principal no recibe clics; Escape o la X la cierran. La imagen del botón se escribe sola como `kmenu-mods-btn.png` en la carpeta del juego.
- **CBF con Speedhack**: los clics se colocaban mal cuando el juego no corría ningún paso en un frame (Speedhack lento con FPS altos): todos los clics de ese frame caían al inicio del paso siguiente, por eso el spam salía irregular. Ahora la hora de cada clic también tiene en cuenta el tiempo que el juego tiene pendiente (acumulador de pasos, +0x3350), y los clics que el juego deja en la cola para el frame siguiente se vuelven a colocar desde su hora real. A velocidad 1x no cambia nada.

## Cambios Kuang Menu 0.12.0

- **Ventana Mods** (botón "Mods" arriba en la columna Menu Hack), estilo Geode: lista Kuang Menu, CBF, Kuang Frame Extrapolation y Kuang Optimizer con su estado (activo, desactivado, iniciando, error, no instalado). Cada uno se activa o desactiva ahí y sus ajustes se cambian ahí mismo: KFE (modo, predicción, fondo, rotación, objetos, estelas, log) y Optimizer (cola de GPU, V-Sync, prioridades, vigilancia, afinidad, boost, temporizador). Escribe `kfe.ini` / `kopt.ini`, que esos mods releen cada medio segundo. Lo que necesita administrador sigue en la app del Optimizer.
- **CBF**: una sola opción llamada "CBF". El interruptor de Time Warp se quitó; el corte exacto con Time Warp va siempre incluido. Si el módulo CBF está desactivado en Mods, la fila CBF de la columna Global sale opaca con el aviso de activarlo desde Mods, y el juego usa su propio Click Between Steps.
- **Restablecer diseño y tamaño**: el botón de la columna Menu Hack ahora también devuelve el tamaño del menú (1.00x) y el ancho de columna (212) a los de fábrica, además de las filas.

## Cambios Kuang Menu 0.11.1

- **Trail personalizado: por fin se ve.** El fallo venía de siempre: las funciones de textura, mezcla y color del trail (CCMotionStreak) esperan un puntero a una parte interna del objeto (+0x158 para textura/mezcla, +0x140 para color) y se les pasaba el objeto entero. La imagen se escribía en el campo equivocado y `getBlendFunc` (que además devuelve el resultado por un puntero oculto) crasheaba: en tu log, `exception in visual options` justo después de `custom trail: on the ship trail`. Ahora se llama con las partes correctas, comprobadas contra `CCMotionStreak::draw`, y antes de tocar nada se comprueba que el objeto sea de verdad un CCMotionStreak.
- Revisado en tu log: Input Boost ya encuentra el hilo de entrada (`priority time-critical`), CBF exacto corta bien, Smart Start Pos aprende, sin más errores.
