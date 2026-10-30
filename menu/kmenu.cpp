// Kuang Menu - in-game mod menu for Geometry Dash 2.2091 (no Geode).
// Phase 1: the menu itself (Tab), language, accent color, search, tooltips, saved layout,
// status labels and utility buttons. Later phases add the rest of the options.
//
// How it draws: CCEGLView::swapBuffers is hooked; right before the frame is shown, Dear ImGui
// draws on top with the OpenGL2 backend. Input comes from the game window (subclassed WndProc).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <shellapi.h>
#include <commdlg.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#include <atomic>
#include <GL/gl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl2.h"
#include "backends/imgui_impl_win32.h"
#include "nlohmann/json.hpp"
#include "../shared/kfe_shared.h"
#include "../optimizer/kopt_shared.h"
#include "kcbf_blob.h"
#include "kpfix_blob.h"

#define KMENU_VERSION "0.14.10"
#define KMENU_VERSION_W L"0.11.1"

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
std::wstring g_dir;
FILE* g_log = nullptr;
void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_log, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// ---------------------------------------------------------------------------
// Settings file (kmenu.ini: key=value lines)
// ---------------------------------------------------------------------------
std::map<std::string, std::string> g_cfg;
bool g_cfgDirty = false;
double g_cfgDirtyAt = 0;

std::wstring cfgPath() { return g_dir + L"kmenu.ini"; }

void cfgLoad() {
    FILE* f = _wfopen(cfgPath().c_str(), L"rb");
    if (!f) return;
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        auto eq = s.find('=');
        if (eq == std::string::npos || s[0] == '#') continue;
        g_cfg[s.substr(0, eq)] = s.substr(eq + 1);
    }
    std::fclose(f);
}

void cfgSave() {
    FILE* f = _wfopen(cfgPath().c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# Kuang Menu settings\n");
    for (auto& [k, v] : g_cfg) std::fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    std::fclose(f);
    g_cfgDirty = false;
}

void cfgTouch() { g_cfgDirty = true; g_cfgDirtyAt = ImGui::GetTime(); }
bool cfgBool(const std::string& k, bool d) { auto it = g_cfg.find(k); return it == g_cfg.end() ? d : it->second == "1"; }
void cfgSetBool(const std::string& k, bool v) { g_cfg[k] = v ? "1" : "0"; cfgTouch(); }
float cfgFloat(const std::string& k, float d) { auto it = g_cfg.find(k); return it == g_cfg.end() ? d : static_cast<float>(std::atof(it->second.c_str())); }
void cfgSetFloat(const std::string& k, float v) { char b[32]; std::snprintf(b, sizeof(b), "%.4f", v); g_cfg[k] = b; cfgTouch(); }
int cfgInt(const std::string& k, int d) { auto it = g_cfg.find(k); return it == g_cfg.end() ? d : std::atoi(it->second.c_str()); }
void cfgSetInt(const std::string& k, int v) { g_cfg[k] = std::to_string(v); cfgTouch(); }
std::string cfgStr(const std::string& k, const std::string& d) { auto it = g_cfg.find(k); return it == g_cfg.end() ? d : it->second; }
void cfgSetStr(const std::string& k, const std::string& v) { g_cfg[k] = v; cfgTouch(); }

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------
int g_lang = 0; // 0 English, 1 Español

struct Text { const char* key; const char* en; const char* es; };
const Text kTexts[] = {
    // columns
    {"cat.kuang", "Menu Hack", "Menu Hack"},
    {"cat.status", "Status", "Estado"},
    {"cat.utility", "Utility", "Utilidades"},
    // settings column
    {"set.search", "Search...", "Buscar..."},
    {"set.language", "Language", "Idioma"},
    {"set.accent", "Accent color", "Color"},
    {"set.custom", "Custom", "Personalizado"},
    {"set.custom.d", "Very light or very dark colors are adjusted so the text always stays readable.",
        "Los colores muy claros o muy oscuros se ajustan para que el texto siempre se lea."},
    {"set.key", "Menu key", "Tecla del menú"},
    {"set.press", "Press a key...", "Pulsa una tecla..."},
    {"set.scale", "Menu size", "Tamaño del menú"},
    {"set.anim", "Animations", "Animaciones"},
    {"set.anim.d", "Smooth open/close and switch animations.", "Animaciones suaves al abrir, cerrar y en los interruptores."},
    {"set.dim", "Dim background", "Oscurecer el fondo"},
    {"set.dim.d", "Darkens the game behind the menu while it is open.", "Oscurece el juego detrás del menú mientras está abierto."},
    {"set.reset", "Reset layout and size", "Restablecer diseño y tamaño"},
    {"set.reset.d", "Puts the columns back in their default rows and the menu size and column width back to default.", "Devuelve las columnas a sus filas por defecto y el tamaño del menú y el ancho de columna a los de fábrica."},
    {"set.hint", "Drag the columns by their title. Hover an option to see what it does.",
        "Arrastra las columnas por el título. Pasa el ratón por una opción para ver qué hace."},
    // status
    {"st.fps", "FPS Counter", "Contador de FPS"},
    {"st.fps.d", "Shows the frames per second the game is drawing.", "Muestra los cuadros por segundo que dibuja el juego."},
    {"st.cps", "CPS Counter", "Contador de CPS"},
    {"st.cps.d", "Clicks per second (mouse, Space, Up, W) and your highest this session.",
        "Clics por segundo (ratón, Espacio, Arriba, W) y tu máximo en esta sesión."},
    {"st.clock", "Clock", "Reloj"},
    {"st.clock.d", "Current time of day.", "La hora actual."},
    {"st.session", "Session Time", "Tiempo de sesión"},
    {"st.session.d", "How long the game has been open.", "Cuánto tiempo lleva abierto el juego."},
    {"st.message", "Message", "Mensaje"},
    {"st.message.d", "A custom text of yours on screen.", "Un texto tuyo en pantalla."},
    {"st.always", "Show outside levels", "Mostrar fuera de los niveles"},
    {"st.always.d", "Also shows the labels in menus, not only while playing.", "Muestra los textos también en los menús, no solo al jugar."},
    {"st.style", "Label style", "Estilo de los textos"},
    {"st.corner", "Position", "Posición"},
    {"st.size", "Size", "Tamaño"},
    {"st.opacity", "Opacity", "Opacidad"},
    {"st.tl", "Top left", "Arriba izquierda"},
    {"st.tr", "Top right", "Arriba derecha"},
    {"st.bl", "Bottom left", "Abajo izquierda"},
    {"st.br", "Bottom right", "Abajo derecha"},
    {"st.text", "Text", "Texto"},
    // utility
    {"ut.appdata", "Open save folder", "Abrir carpeta de guardado"},
    {"ut.appdata.d", "Opens %LOCALAPPDATA%\\GeometryDash, where your saves and songs are.",
        "Abre %LOCALAPPDATA%\\GeometryDash, donde están tus partidas y canciones."},
    {"ut.resources", "Open Resources", "Abrir Resources"},
    {"ut.resources.d", "Opens the game's Resources folder (textures, sounds).", "Abre la carpeta Resources del juego (texturas, sonidos)."},
    {"ut.game", "Open game folder", "Abrir carpeta del juego"},
    {"ut.game.d", "Opens the folder where GeometryDash.exe is.", "Abre la carpeta donde está GeometryDash.exe."},
    {"ut.config", "Open menu settings file", "Abrir archivo de ajustes"},
    {"ut.config.d", "Opens kmenu.ini, where everything you change here is saved.", "Abre kmenu.ini, donde se guarda todo lo que cambias aquí."},
    {"ut.p1", "Player 1", "Jugador 1"},
    {"ut.p2", "Player 2", "Jugador 2"},
    {"ut.click", "Click", "Click"},
    {"ut.left", "Left", "Izquierda"},
    {"ut.right", "Right", "Derecha"},
    {"ut.btn.d", "Holds that button for that player while you hold it here, exactly like the real key (jump, or move left/right in platformer). Only inside a level.",
        "Mantiene ese botón de ese jugador mientras lo mantengas aquí, igual que la tecla real (saltar, o moverse a izquierda/derecha en plataforma). Solo dentro de un nivel."},
    {"ut.inlevel", "Only inside a level", "Solo dentro de un nivel"},
    {"ut.endscreen", "Not on the Level Complete screen", "No en la pantalla de nivel completado"},
    {"ut.paused", "Close the pause menu first", "Cierra primero el menú de pausa"},
    {"ut.restart", "Restart Level", "Reiniciar nivel"},
    {"ut.restart.d", "Starts the attempt again from the beginning (or from the start pos you picked), the same as the game's restart.",
        "Empieza el intento de nuevo desde el principio (o desde el start pos elegido), igual que el reinicio del juego."},
    {"ut.practice", "Practice Mode", "Modo práctica"},
    {"ut.practice.d", "Turns the game's practice mode on or off in the level you are playing, without opening the pause menu.",
        "Activa o desactiva el modo práctica del juego en el nivel que estás jugando, sin abrir el menú de pausa."},
    {"ut.unc", "Uncomplete Level", "Descompletar nivel"},
    {"ut.unc.d", "Puts the level you are playing (or whose page is open) back to 0%: normal and practice progress, attempts, jumps, time, platformer bests, and its completed / stars / demon / coins marks in your save, taking those stars, moons, demon and coins off your stats so you can earn them again. Online levels only. It cannot touch the online leaderboard: that record is kept on RobTop's server.",
        "Deja el nivel que juegas (o cuya página está abierta) en 0%: progreso normal y de práctica, intentos, saltos, tiempo, récords de plataforma, y sus marcas de completado / estrellas / demon / monedas en tu partida, restando esas estrellas, lunas, demon y monedas de tus estadísticas para que puedas ganarlas otra vez. Solo niveles online. No puede tocar el leaderboard online: ese récord lo guarda el servidor de RobTop."},
    {"ut.unc.body", "Erase all progress of \"%s\" on this PC? It goes back to 0%%, and its stars, demon and coins come off your stats. This cannot be undone.\n\nThe online leaderboard keeps your record: it lives on RobTop's server and no mod can delete it.",
        "¿Borrar todo el progreso de \"%s\" en este PC? Vuelve al 0%%, y sus estrellas, demon y monedas se restan de tus estadísticas. No se puede deshacer.\n\nEl leaderboard online conserva tu récord: está en el servidor de RobTop y ningún mod puede borrarlo."},
    {"ut.unc.yes", "Erase progress", "Borrar progreso"},
    {"ut.cancel", "Cancel", "Cancelar"},
    {"ut.unc.none", "Open a level or its page first", "Abre primero un nivel o su página"},
    {"ut.unc.online", "Only online levels can be uncompleted", "Solo se pueden descompletar niveles online"},
    {"ut.unc.fail", "Could not reset the level (see kmenu.log)", "No se pudo reiniciar el nivel (mira kmenu.log)"},
    {"ut.unc.done", "Level back to 0%", "Nivel de vuelta al 0%"},
    {"ut.unc.done2", "Level back to 0% (re-open its page to see it)", "Nivel de vuelta al 0% (vuelve a abrir su página para verlo)"},
    {"ut.settings", "Game Settings", "Ajustes del juego"},
    {"ut.settings.d", "Opens the game's own settings window from anywhere, even in the middle of a level.",
        "Abre la ventana de ajustes del propio juego desde cualquier sitio, incluso a mitad de un nivel."},
    {"ut.crash", "Crash Game", "Crashear el juego"},
    {"ut.crash.d", "Closes the game instantly, without saving the game's progress (the menu's settings are saved).",
        "Cierra el juego al instante, sin guardar el progreso del juego (los ajustes del menú sí se guardan)."},
    {"ut.crash.body", "Close Geometry Dash right now? Anything the game has not saved yet is lost.",
        "¿Cerrar Geometry Dash ahora mismo? Lo que el juego no haya guardado se pierde."},
    {"ut.crash.yes", "Crash", "Crashear"},
    {"misc.none", "No results", "Sin resultados"},
    {"set.width", "Column width", "Ancho de las columnas"},
    {"st.cheat", "Cheat Indicator", "Indicador de trampas"},
    {"st.cheat.d", "A dot on screen: green = no cheats, red = a cheat is on, dark orange = you used a cheat in this attempt but it is off now. When you complete the level it stays on screen showing how you passed it.",
        "Un punto en pantalla: verde = sin trampas, rojo = hay una trampa activa, naranja oscuro = usaste una trampa en este intento pero ya está apagada. Al completar el nivel se queda en pantalla mostrando cómo lo pasaste."},
    {"st.all", "All labels", "Todos los textos"},
    {"st.reset", "Reset label positions", "Restablecer posición de los textos"},
    {"st.drag", "With the menu open, drag any label on screen to move it.",
        "Con el menú abierto, arrastra cualquier texto en pantalla para moverlo."},
    {"cps.green", "Green while clicking", "Verde al hacer clic"},
    {"cps.max", "Show highest", "Mostrar máximo"},
    {"cps.unit", "Show unit (CPS)", "Mostrar unidad (CPS)"},
    {"cps.total", "Show total clicks", "Mostrar clics totales"},
    {"cps.reset", "Reset counters", "Reiniciar contadores"},
    {"cat.player", "Player", "Jugador"},
    {"cat.global", "Global", "Global"},
    {"pl.noclip", "Noclip", "Noclip"},
    {"pl.nchb", "Hitboxes on Noclip hit", "Hitboxes al chocar con Noclip"},
    {"pl.nchb.d", "While Noclip is on, shows the hitboxes only while you are touching something deadly.",
        "Con Noclip activado, muestra las hitboxes solo mientras estás tocando algo mortal."},
    {"pl.noclip.d", "You don't die when you touch obstacles. Turns on Safe Mode.",
        "No mueres al tocar obstáculos. Activa el Safe Mode."},
    {"gl.speed", "Speedhack", "Speedhack"},
    {"gl.speed.d", "Changes the speed of the whole game. Turns on Safe Mode.",
        "Cambia la velocidad de todo el juego. Activa el Safe Mode."},
    {"gl.speed.v", "Speed", "Velocidad"},
    {"gl.speed.audio", "Change music speed too", "Cambiar también la velocidad de la música"},
    {"gl.speed.type", "Exact value (0.0001 - 100)", "Valor exacto (0.0001 - 100)"},
    {"gl.safe", "Safe Mode", "Safe Mode"},
    {"gl.safe.d", "Progress, new bests and completions are not saved. It turns on by itself while any cheat is on.",
        "No se guarda el progreso, ni récords, ni completados. Se activa solo mientras haya una trampa encendida."},
    {"pl.hitbox", "Show Hitboxes", "Mostrar hitboxes"},
    {"pl.hitbox.d", "Draws the hitboxes of the player and the objects, also outside practice mode.",
        "Dibuja las hitboxes del jugador y de los objetos, también fuera del modo práctica."},
    {"pl.accurate", "Accurate Percentage", "Porcentaje exacto"},
    {"pl.accurate.d", "Shows the percentage with decimals.", "Muestra el porcentaje con decimales."},
    {"pl.pctfix", "Percentage Fix", "Percentage Fix (arreglo de %)"},
    {"pl.pctfix.d", "In this version the percentage is counted by time. Starting from a start pos the game works out how much time that point is worth, and in some levels it gets it wrong: the bar starts at 0% or shows 100% right away. This notices those two cases when the attempt starts and puts the percentage where that start pos really is. Only the percentage you see from start positions; nothing is saved (start pos runs never save progress).",
        "En esta versión el porcentaje se cuenta por tiempo. Al empezar desde un start pos el juego calcula cuánto tiempo vale ese punto, y en algunos niveles se equivoca: la barra empieza en 0% o muestra 100% de golpe. Esto detecta esos dos casos al empezar el intento y pone el porcentaje donde de verdad está ese start pos. Solo el porcentaje que ves desde start pos; no se guarda nada (las partidas desde start pos nunca guardan progreso)."},
    {"pl.noatt", "Hide Attempts", "Ocultar intentos"},
    {"pl.noatt.d", "Hides the \"Attempt\" text in the level.", "Oculta el texto de \"Intento\" en el nivel."},
    {"cat.extras", "Extras", "Extras"},
    {"tr.on", "Custom Trail", "Trail personalizado"},
    {"tr.on.d", "Replaces the trail of your ship with an image of yours (or of every vehicle, if you untick \"Only on the ship\"). The image is laid along the trail: its width goes across the trail and its height along it.",
        "Cambia el trail de tu nave por una imagen tuya (o el de todos los vehículos, si quitas \"Solo en el ship\"). La imagen se pone a lo largo del trail: su ancho va de borde a borde y su alto a lo largo."},
    {"tr.fail", "The image could not be loaded: choose it again (PNG or JPG).", "No se pudo cargar la imagen: elígela otra vez (PNG o JPG)."},
    {"tr.pick", "Choose image...", "Elegir imagen..."},
    {"tr.none", "No image chosen yet", "Todavía no hay imagen"},
    {"tr.width", "Trail width", "Ancho del trail"},
    {"tr.keep", "Keep the image's own colors", "Mantener los colores de la imagen"},
    {"tr.ship", "Only on the ship", "Solo en el ship"},
    {"tr.glow", "Glow (additive blending)", "Brillo (mezcla aditiva)"},
    {"tr.tip", "Tip: a long, tall image works best, since the trail is long and thin. The game only shows the regular trail in some moments (always while flying the ship), so with \"Only on the ship\" off you will not see it on the cube except after pads and orbs.",
        "Consejo: funciona mejor una imagen alargada (más alta que ancha), porque el trail es largo y fino. El juego solo muestra el trail normal en algunos momentos (siempre al volar la nave), así que sin \"Solo en el ship\" no lo verás en el cubo salvo tras pads y orbes."},
    {"cat.startpos", "Start Pos", "Start Pos"},
    {"cat.kfe", "Frame Extrapolation", "Frame Extrapolation"},
    {"st.ncd", "Noclip Deaths", "Muertes con Noclip"},
    {"st.ncd.d", "How many separate times you touched something deadly with Noclip on. One touch counts once, however long you stay inside (only shows while Noclip is on).",
        "Cuántas veces distintas tocaste algo mortal con Noclip. Un toque cuenta una sola vez, sin importar cuánto tiempo te quedes dentro (solo se ve con Noclip activo)."},
    {"st.ncd.fmt", "Deaths:", "Muertes:"},
    {"st.nca", "Noclip Accuracy", "Precisión con Noclip"},
    {"st.nca.d", "Percent of the physics steps of the attempt in which you were not touching anything deadly, counted like Mega Hack (only shows while Noclip is on).",
        "Porcentaje de pasos de física del intento en los que no tocaste nada mortal, contado como en Mega Hack (solo se ve con Noclip activo)."},
    {"st.nca.fmt", "Accuracy:", "Precisión:"},
    {"st.rs", "Render Stats", "Estadísticas de render"},
    {"st.rs.d", "Measures, per frame, how many draw calls and triangles the game sends and how long the CPU spends on logic, on submitting the drawing, and waiting in the buffer swap (swap is mostly GPU time when VSync is off). Also writes a line to kmenu.log every 10 s inside a level. It only measures, it changes nothing in the game. Use it to see whether a level is limited by the CPU or the GPU. The third line splits the logic into triggers (move, rotate, follow...), colors and the per-object loop: the parts a Bismuth-style renderer moves to the GPU (also in kmenu.log as [deepprofile]).",
        "Mide, por frame, cuántas llamadas de dibujo y triángulos envía el juego y cuánto tarda la CPU en la lógica, en enviar el dibujado y esperando en el intercambio de buffers (swap es sobre todo tiempo de GPU con VSync apagado). También escribe una línea en kmenu.log cada 10 s dentro de un nivel. Solo mide, no cambia nada del juego. Sirve para ver si un nivel está limitado por la CPU o por la GPU. La tercera línea separa la lógica en triggers (move, rotate, follow...), colores y el recorrido de objetos: las partes que un renderer estilo Bismuth pasa a la GPU (también en kmenu.log como [deepprofile])."},
    {"gl.vis", "Visibility Throttle", "Limitar visibilidad"},
    {"gl.vis.d", "At very high FPS the game redoes the colors and effects of every visible object on every single frame. This runs that update at most N times per second (the time of the skipped frames is added up, so rotations and animations keep their speed). It changes nothing in the physics or the attempt, only how often the visuals refresh. Below that FPS it does nothing. Auto uses your monitor's refresh rate: faster updates than that cannot be seen anyway. Turn it off if you see objects flicker or pop in late. Measure the gain with Render Stats.",
        "A FPS muy altos el juego rehace los colores y efectos de cada objeto visible en cada frame. Esto ejecuta esa actualización como máximo N veces por segundo (el tiempo de los frames omitidos se suma, así que rotaciones y animaciones mantienen su velocidad). No cambia nada de la física ni del intento, solo con qué frecuencia se refresca lo visual. Por debajo de ese FPS no hace nada. Auto usa los Hz de tu monitor: actualizar más rápido que eso no se llega a ver. Apágalo si ves objetos parpadear o aparecer tarde. Mide la ganancia con Estadísticas de render."},
    {"gl.vis.hz", "Updates per second", "Actualizaciones por segundo"},
    {"gl.vis.auto", "Auto (monitor refresh rate)", "Auto (Hz del monitor)"},
    {"gl.vis.autohz", "Using %d per second (your monitor).", "Usando %d por segundo (tu monitor)."},
    {"gl.vis.type", "Exact value (1 - 100000)", "Valor exacto (1 - 100000)"},
    {"st.rs.l1", "Draw calls", "Llamadas de dibujo"},
    {"st.il", "Input Latency", "Latencia de entrada"},
    {"st.il.d", "Time from the moment Windows received your key or click (the game stamps every press with the exact time on its own input thread) until the physics applied it. Most of it is the wait for the physics step that contains your press: GD runs physics 240 times a second, so that part is up to 4.2 ms (about 2 ms on average) and is how the game works; it can only shrink by changing the physics. The second number is the slow end (95th percentile). Inside a level it also writes a line to kmenu.log every 10 s, with how long your key waited for the next frame. It only measures.",
        "Tiempo desde que Windows recibió tu tecla o clic (el juego marca cada pulsación con la hora exacta en su propio hilo de entrada) hasta que la física la aplicó. Casi todo es la espera al paso de física que contiene tu pulsación: GD calcula la física 240 veces por segundo, así que esa parte llega hasta 4.2 ms (unos 2 ms de media) y es como funciona el juego; solo se reduce cambiando la física. El segundo número es el extremo lento (percentil 95). Dentro de un nivel también escribe una línea en kmenu.log cada 10 s, con cuánto esperó tu tecla al siguiente frame. Solo mide."},
    {"st.il.fmt", "Input", "Entrada"},
    {"st.rs.l2", "CPU ms: logic", "CPU ms: lógica"},
    {"st.rs.trig", "triggers", "triggers"},
    {"st.rs.col", "colors", "colores"},
    {"st.rs.obj", "objects", "objetos"},
    {"st.att", "Attempts", "Intentos"},
    {"st.att.d", "The attempt number of the level you are playing.", "El número de intento del nivel que juegas."},
    {"st.att.fmt", "Attempt", "Intento"},
    {"st.best", "Best Run", "Mejor run"},
    {"st.best.d", "Your longest run in this level since you opened it (from where you started to where you died).",
        "Tu run más larga en este nivel desde que lo abriste (de donde empezaste a donde moriste)."},
    {"st.best.fmt", "Best run:", "Mejor run:"},
    {"gl.fps", "FPS Bypass", "FPS Bypass"},
    {"gl.fps.d", "Type any FPS below and press Enter. For more FPS than your monitor's Hz, VSync must be off.",
        "Escribe los FPS que quieras abajo y pulsa Enter. Para más FPS que los Hz de tu monitor, el VSync debe estar apagado."},
    {"gl.fps.v", "Limit", "Límite"},
    {"gl.fps.unl", "No limit", "Sin límite"},
    {"gl.fps.p", "Precise limiter", "Limitador preciso"},
    {"gl.fps.p.d", "Paces the frames with a high-resolution timer instead of the game's own limit, so they arrive evenly (fewer lines and tearing at a fixed FPS). Used up to 10000 FPS; above that there is no limit. Frame times are logged in kmenu.log.",
        "Reparte los frames con un temporizador de alta resolución en vez del límite del propio juego, para que lleguen parejos (menos líneas y tearing con FPS fijos). Se usa hasta 10000 FPS; por encima no hay límite. Los tiempos de frame se anotan en kmenu.log."},
    {"gl.ib", "Input Boost", "Input Boost"},
    {"gl.ib.d", "Gives the game's own input thread the highest priority. In this version the game reads your keyboard and mouse on a separate thread (Raw Input) and stamps every press with the exact time; with this on, that stamp is taken at once even when the CPU is busy. It does not change the FPS, the physics or the inputs themselves. On a fast PC the difference is usually too small to notice: measure it with Input Latency.",
        "Le da la prioridad más alta al propio hilo de entrada del juego. En esta versión el juego lee tu teclado y ratón en un hilo aparte (Raw Input) y marca cada pulsación con la hora exacta; con esto la hora se toma al instante aunque la CPU esté ocupada. No cambia los FPS, la física ni las pulsaciones. En una PC rápida la diferencia suele ser demasiado pequeña para notarla: mídela con Latencia de entrada."},
    {"gl.cbf", "CBF", "CBF"},
    {"gl.cbf.d", "Click Between Frames, exact: the game only cuts a physics step in two halves (a click lands on a 1/480 s grid, up to 2 ms early); this cuts the step at the exact instant of the click, also with this menu's Speedhack and in levels with their own Time Warp. It turns the game's Click Between Steps on. If the game's code is not the 2.2091 one it stays off and changes nothing. See kmenu.log. It also puts each click on the clock of the physics steps (the leftover time the game keeps for the next frame), so clicks land exactly at any speed.",
        "Click Between Frames exacto: el juego solo parte un paso de la física en dos mitades (un clic cae en una rejilla de 1/480 s, hasta 2 ms antes); esto corta el paso en el instante exacto del clic, también con el Speedhack de este menú y en niveles con su propio Time Warp. Activa el Click Between Steps del juego. Si el código del juego no es el de 2.2091 se queda apagado y no cambia nada. Ver kmenu.log. También coloca cada clic en el reloj de los pasos de la física (el tiempo sobrante que el juego guarda para el frame siguiente), así los clics caen exactos a cualquier velocidad."},
    {"gl.cbf.off", "CBF is turned off in the Mods menu (Menu Hack column). Turn it on there to use it.", "El CBF está desactivado en el menú Mods (columna Menu Hack). Actívalo ahí para poder usarlo."},
    {"gl.cbf.fail", "CBF: this game version does not match (nothing was changed)", "CBF: esta versión del juego no coincide (no se cambió nada)"},
    {"mods.btn", "Mods", "Mods"},
    {"mods.d", "Every Kuang mod in the game folder: turn each one on or off and change its settings here.", "Todos los mods Kuang de la carpeta del juego: actívalos, desactívalos y cambia sus ajustes aquí."},
    {"mods.on", "Active", "Activo"},
    {"mods.off", "Off", "Desactivado"},
    {"mods.missing", "Not installed", "No instalado"},
    {"mods.wait", "Starting...", "Iniciando..."},
    {"mods.err", "Error", "Error"},
    {"mods.enable", "Enabled", "Activado"},
    {"mods.close", "Close", "Cerrar"},
    {"mods.menu.d", "This menu. It is always on; press its key to open or close it.", "Este menú. Siempre está activo; pulsa su tecla para abrirlo o cerrarlo."},
    {"mods.cbf.d", "Exact Click Between Frames (a module of this menu). Off here: the game's own Click Between Steps is used and the CBF row of the Global column is greyed out.", "Click Between Frames exacto (un módulo de este menú). Desactivado aquí: se usa el Click Between Steps propio del juego y la fila CBF de la columna Global queda opaca."},
    {"mods.cbf.where", "The CBF switch itself is in the Global column.", "El interruptor del CBF está en la columna Global."},
    {"mods.kfe.d", "Smooths what you see between physics steps. Changes apply in half a second.", "Suaviza lo que ves entre pasos de la física. Los cambios se aplican en medio segundo."},
    {"mods.opt.d", "Process, GPU and timer tweaks for the game. Changes apply in half a second.", "Ajustes de proceso, GPU y temporizador para el juego. Los cambios se aplican en medio segundo."},
    {"mods.opt.app", "Windows tweaks that need administrator rights (helper, registry tweaks, keyboard core, compatibility, recording) are in the Kuang Optimizer app.", "Los ajustes de Windows que necesitan permisos de administrador (helper, ajustes del registro, núcleo del teclado, compatibilidad, grabación) están en la app Kuang Optimizer."},
    {"mods.open.hack", "Open the Menu Hack", "Abrir el Menu Hack"},
    {"mods.open.app", "Open the app", "Abrir la app"},
    {"mods.app.kfe.d", "Opens the Kuang Frame Extrapolation app (status and the same settings).", "Abre la app Kuang Frame Extrapolation (estado y los mismos ajustes)."},
    {"mods.app.missing", "The app is not installed (it goes in %LOCALAPPDATA%\\Programs\\Kuang). Install it with Kuang Installer.", "La app no está instalada (va en %LOCALAPPDATA%\\Programs\\Kuang). Instálala con Kuang Installer."},
    {"kfe.inter.d", "On: interpolation (shows between the last two steps, always exact, a bit of delay). Off: extrapolation (predicts ahead, no delay).", "Activado: interpolación (muestra entre los dos últimos pasos, siempre exacto, algo de retraso). Desactivado: extrapolación (predice hacia delante, sin retraso)."},
    {"kfe.smart.d", "Predicts better at jumps, orbs and pads so the extrapolation does not overshoot.", "Predice mejor en saltos, orbes y pads para que la extrapolación no se pase."},
    {"kfe.bg.d", "Also smooths the background and ground.", "También suaviza el fondo y el suelo."},
    {"kfe.rot.d", "Also smooths the icon's rotation.", "También suaviza la rotación del icono."},
    {"kfe.obj.d", "Also smooths objects moved by triggers.", "También suaviza los objetos que mueven los triggers."},
    {"kfe.trails.d", "Also smooths the trail and wave trail.", "También suaviza la estela y la estela del wave."},
    {"kfe.debug.d", "Writes more details to kfe.log. Leave it off unless something fails.", "Escribe más detalles en kfe.log. Déjalo apagado salvo que algo falle."},
    {"opt.flush.d", "Waits for the graphics card to finish each frame, so frames do not pile up. May lower FPS.", "Espera a que la tarjeta gráfica termine cada frame para que no se acumulen. Puede bajar los FPS."},
    {"opt.vsync.d", "Turns vertical sync off while the game runs.", "Apaga la sincronización vertical mientras juegas."},
    {"opt.prio.d", "Raises Geometry Dash's CPU priority.", "Sube la prioridad de CPU de Geometry Dash."},
    {"opt.watchdog.d", "Every 2 seconds it puts the priorities, cores, timer and power plan back if another program changed them.", "Cada 2 segundos vuelve a poner prioridades, núcleos, temporizador y plan de energía si otro programa los cambió."},
    {"opt.gpu.d", "Asks Windows to have the graphics card serve Geometry Dash first.", "Pide a Windows que la tarjeta gráfica atienda primero a Geometry Dash."},
    {"opt.aff.d", "One thread per physical core and no core 0 (P cores only on Intel with E cores).", "Un hilo por núcleo físico y sin el núcleo 0 (solo núcleos P en Intel con núcleos E)."},
    {"opt.boost.d", "Turns off Windows' dynamic thread priority boost for the game.", "Desactiva el aumento dinámico de prioridad de hilos de Windows para el juego."},
    {"opt.timer.d", "Asks Windows for the highest clock resolution while the game is open.", "Pide a Windows la máxima resolución del reloj mientras el juego está abierto."},
    {"opt.io", "High I/O priority", "Prioridad alta de disco"},
    {"opt.io.d", "High disk priority for the game. It only applies with the external watchdog (helper) installed from the app.", "Prioridad alta de disco para el juego. Solo se aplica con el vigilante externo (helper) instalado desde la app."},
    {"opt.plan", "Power plan", "Plan de energía"},
    {"opt.plan.d", "Click to go through your power plans. The game uses it while it is open; 'Don't change' leaves yours.", "Pulsa para pasar por tus planes de energía. El juego lo usa mientras está abierto; 'No cambiar' deja el tuyo."},
    {"opt.plan.keep", "Don't change", "No cambiar"},
    {"km.tab.inst", "Installed", "Instalados"},
    {"km.tab.down", "Download", "Descargar"},
    {"km.tab.recent", "Recent", "Recientes"},
    {"km.install", "Install", "Instalar"},
    {"km.all", "You have every Kuang mod installed.", "Tienes todos los mods Kuang instalados."},
    {"km.down.d", "Kuang mods you do not have yet. Install opens Kuang Installer: tick the mod there and press Install. There is no online download yet.", "Mods Kuang que aún no tienes. Instalar abre Kuang Installer: marca ahí el mod y pulsa Instalar. Todavía no hay descarga por internet."},
    {"km.noinst", "Kuang Installer was not found (game folder or %LOCALAPPDATA%\\Programs\\Kuang). Open it from where you downloaded it.", "No se encontró Kuang Installer (carpeta del juego o %LOCALAPPDATA%\\Programs\\Kuang). Ábrelo desde donde lo descargaste."},
    {"km.r1", "Kuang Mods button and screen on the main menu.", "Botón y pantalla Kuang Mods en el menú principal."},
    {"km.r2", "CBF with Speedhack: clicks placed evenly when spamming.", "CBF con Speedhack: clics bien colocados al hacer spam."},
    {"km.r3", "CBF in a single option, layout and size reset.", "CBF en una sola opción, restablecer diseño y tamaño."},
    {"km.r4", "Custom ship trail image fixed.", "Arreglada la imagen personalizada del trail de la nave."},
    {"km.r5", "Smoother camera, icons and moving objects at any FPS.", "Cámara, iconos y objetos más suaves a cualquier FPS."},
    {"mods.notinst", "Its .dll is not in the game folder. Install it with Kuang Installer.", "Su .dll no está en la carpeta del juego. Instálalo con Kuang Installer."},
    {"kfe.mode", "Mode", "Modo"},
    {"kfe.extra", "Extrapolation", "Extrapolación"},
    {"kfe.inter", "Interpolation", "Interpolación"},
    {"kfe.smart", "Smart prediction", "Predicción inteligente"},
    {"kfe.bg", "Smooth background", "Suavizar fondo"},
    {"kfe.rot", "Smooth rotation", "Suavizar rotación"},
    {"kfe.obj", "Smooth moving objects", "Suavizar objetos en movimiento"},
    {"kfe.trails", "Smooth trails", "Suavizar estelas"},
    {"kfe.debug", "Debug log", "Log de depuración"},
    {"opt.flush", "Flush GPU queue", "Vaciar la cola de la GPU"},
    {"opt.vsync", "Force V-Sync off", "Forzar V-Sync apagado"},
    {"opt.prio", "High CPU priority", "Prioridad alta de CPU"},
    {"opt.watchdog", "Continuous watchdog", "Vigilancia continua"},
    {"opt.gpu", "High GPU priority", "Prioridad alta de GPU"},
    {"opt.aff", "No core 0 and no SMT", "Sin núcleo 0 y sin SMT"},
    {"opt.boost", "Disable thread boost", "Desactivar el boost de hilos"},
    {"opt.timer", "0.5 ms timer", "Temporizador de 0.5 ms"},
    {"gl.pfix", "Precision Fix", "Precision Fix"},
    {"gl.pfix.d", "The game measures the time of each frame in whole microseconds, which at very high FPS is up to a 1 % error per frame (the frame time jitters). This computes the frame time straight from the high-resolution clock instead (about 100 ns or better). Only changes the frame time; does nothing else. Works with libcocos2d.dll 2.2091.",
        "El juego mide el tiempo de cada frame en microsegundos enteros, lo que con FPS muy altos es hasta un 1 % de error por frame (el tiempo del frame tiembla). Esto calcula el tiempo del frame directamente con el reloj de alta resolución (unos 100 ns o mejor). Solo cambia el tiempo del frame, nada más. Funciona con libcocos2d.dll 2.2091."},
    {"gl.pfix.fail", "Precision Fix: this libcocos2d.dll does not match 2.2091 (nothing was changed)", "Precision Fix: esta libcocos2d.dll no coincide con la 2.2091 (no se cambió nada)"},
    {"gl.sfps", "Steady FPS", "FPS estables"},
    {"gl.sfps.d", "Inside levels, caps the FPS at a value your PC can always hold and paces the frames evenly (the highest multiple of 240, the physics rate, with headroom). With uncapped FPS the frames come unevenly, so this can feel smoother; the FPS you see will be lower and fixed. It is like typing a lower FPS in FPS Bypass, chosen automatically: physics in this version always runs 240 times a second whatever the FPS, so it changes nothing in the physics. The FPS you typed in FPS Bypass still works as a maximum.",
        "Dentro de los niveles, pone un tope de FPS que tu PC siempre aguanta y reparte los frames parejos (el múltiplo de 240, la velocidad de la física, más alto con margen). Con FPS sin tope los frames llegan disparejos, así que puede sentirse más fluido; los FPS que ves serán más bajos y fijos. Es como escribir menos FPS en FPS Bypass, elegido solo: en esta versión la física siempre va a 240 por segundo sin importar los FPS, así que no cambia nada de la física. Los FPS que escribiste en FPS Bypass siguen siendo el máximo."},
    {"gl.ib.on", "Steady at %d FPS", "Estable a %d FPS"},
    {"gl.ib.measuring", "Measuring your PC...", "Midiendo tu PC..."},
    {"gl.fps.hint", "For more FPS than your monitor's refresh rate, VSync must be off (Kuang Optimizer can turn it off).",
        "Para tener más FPS que los Hz de tu monitor, el VSync debe estar apagado (Kuang Optimizer puede apagarlo)."},
    {"kfe.missing", "Kuang Frame Extrapolation is not installed. Install it with KuangInstaller.exe.",
        "Kuang Frame Extrapolation no está instalado. Instálalo con KuangInstaller.exe."},
    {"kfe.st.on", "Active", "Activo"},
    {"kfe.st.off", "Off", "Apagado"},
    {"kfe.st.err", "Error - open kfe.log", "Error: revisa kfe.log"},
    {"kfe.nodes", "smoothed", "suavizados"},
    {"kfe.objs", "moving objects", "objetos en movimiento"},
    {"kfe.name", "Frame Extrapolation", "Frame Extrapolation"},
    {"kfe.on.d", "Turns Kuang Frame Extrapolation on or off, instantly.", "Activa o apaga Kuang Frame Extrapolation al instante."},
    {"kfe.mode", "Mode", "Modo"},
    {"kfe.extra", "Extrapolation", "Extrapolación"},
    {"kfe.extra.d", "Zero delay: draws where everything is right now.", "Cero retraso: dibuja dónde está todo en este instante."},
    {"kfe.inter", "Interpolation", "Interpolación"},
    {"kfe.inter.d", "Never guesses, adds about 2 ms of visual delay.", "Nunca adivina; añade unos 2 ms de retraso visual."},
    {"kfe.smart", "Smart prediction", "Predicción inteligente"},
    {"kfe.smart.d", "Accounts for gravity and a slowing camera.", "Tiene en cuenta la gravedad y la cámara que frena."},
    {"kfe.bg", "Smooth background and ground", "Suavizar fondo y suelo"},
    {"kfe.bg.d", "The background and the ground move as smoothly as the camera.", "El fondo y el suelo se mueven tan suaves como la cámara."},
    {"kfe.obj", "Smooth moving objects", "Suavizar objetos en movimiento"},
    {"kfe.obj.d", "Objects moved by triggers don't shake against the camera.", "Los objetos que mueven los triggers no tiemblan contra la cámara."},
    {"kfe.rot", "Smooth rotation", "Suavizar rotación"},
    {"kfe.rot.d", "Smooths the icon's rotation when physics drives it.", "Suaviza la rotación del icono cuando la mueve la física."},
    {"cat.jukebox", "Jukebox", "Jukebox"},
    {"gv.hitbox", "Show Hitboxes", "Mostrar hitboxes"},
    {"gv.hitbox.d", "The game's own option: shows hitboxes while in practice mode.", "La opción del propio juego: muestra las hitboxes en el modo práctica."},
    {"gv.trail", "Hitbox Trail", "Rastro de hitboxes"},
    {"gv.trail.d", "The game's own option: shows a hitbox trail in practice mode.", "La opción del propio juego: muestra un rastro de hitboxes en el modo práctica."},
    {"gv.death", "Hitboxes on Death", "Hitboxes al morir"},
    {"gv.death.d", "The game's own option: shows hitboxes when you die, in normal and practice mode.", "La opción del propio juego: muestra las hitboxes al morir, en modo normal y práctica."},
    {"gv.deathtrail", "Hitbox Trail on Death", "Rastro de hitboxes al morir"},
    {"gv.deathtrail.d", "The game's own option: shows the hitbox trail when you die, in normal and practice mode.", "La opción del propio juego: muestra el rastro de hitboxes al morir, en modo normal y práctica."},
    {"gv.highlight", "Trail Highlight (solid red)", "Rastro resaltado (rojo sólido)"},
    {"gv.highlight.d", "Draws the hitbox trail in solid red with no fade.", "Dibuja el rastro de hitboxes en rojo sólido, sin desvanecer."},
    {"gv.noplayer", "Disable Player Hitbox", "Ocultar la hitbox del jugador"},
    {"gv.noplayer.d", "Hides the player's own hitbox when hitboxes are shown.", "Oculta la hitbox del jugador cuando se muestran las hitboxes."},
    {"gv.more", "More hitbox options...", "Más opciones de hitboxes..."},
    {"gv.more.d", "The other hitbox settings of the game.", "Las otras opciones de hitboxes del juego."},
    {"gv.hint", "These are the game's own settings: changing them here is the same as in its options menu.",
        "Son opciones del propio juego: cambiarlas aquí es lo mismo que en su menú de opciones."},
    {"gv.cbs", "Click Between Steps", "Click Between Steps"},
    {"gv.cbs.d", "The game's own option: lets a click land between physics steps.", "La opción del propio juego: deja que un clic caiga entre los pasos de la física."},
    {"gv.cos", "Click On Steps", "Click On Steps"},
    {"tj.on", "Show Trajectory", "Mostrar trayectoria"},
    {"tj.on.d", "Draws both paths the player can take from here: if you hold and if you let go. The path that matches what you are doing now gets the Hold colour, the other one the Release colour (both can be changed per player). Where it would die you see its real hitboxes (outer red, inner blue), measured by the game for that vehicle and size, like QOLMod. It is worked out with the game's own physics on a hidden copy of the player, so it follows blocks, spikes, slopes, pads (with the game's own strength for each vehicle and size, reverse pads too) and mode / gravity / size / speed portals. Orbs are not followed, the same as QOLMod (they need a click at the right moment).",
        "Dibuja los dos caminos que puede tomar el jugador desde aquí: si mantienes y si sueltas. El camino que coincide con lo que haces ahora lleva el color Mantener y el otro el color Soltar (los dos se cambian por jugador). Donde moriría ves sus hitboxes reales (exterior rojo, interior azul), medidas por el juego para ese vehículo y tamaño, como QOLMod. Se calcula con la física del propio juego sobre una copia oculta del jugador, así que sigue bloques, pinchos, rampas, pads (con la fuerza que usa el juego para cada vehículo y tamaño, también los de reversa) y portales de modo / gravedad / tamaño / velocidad. Los orbes no se siguen, igual que en QOLMod (necesitan un clic en el momento justo)."},
    {"tj.len", "Length", "Largo"},
    {"tj.p1", "Player 1", "Jugador 1"},
    {"tj.p2", "Player 2 (dual)", "Jugador 2 (dual)"},
    {"tj.hold", "Hold", "Mantener"},
    {"tj.rel", "Release", "Soltar"},
    {"tj.trail", "Hitbox trail", "Rastro de hitbox"},
    {"tj.tip", "How far ahead it looks. It moves in the game's own steps (240 per second), so longer costs more FPS on heavy levels. It only recalculates when the player moves.",
        "Cuánto mira por delante. Avanza con los mismos pasos del juego (240 por segundo), así que más largo cuesta más FPS en niveles pesados. Solo se recalcula cuando el jugador se mueve."},
    {"gl.noanim", "No Animations", "Sin animaciones"},
    {"gl.noanim.d", "Shortcut to the game's own Fast Menu option (Settings): removes the animations between menus. It is the same switch, so it shows and changes the game's own setting and nothing else.",
        "Acceso directo a la opción Fast Menu del propio juego (Ajustes): quita las animaciones entre menús. Es el mismo interruptor, así que muestra y cambia el ajuste del juego y nada más."},
    {"gv.cos.d", "The game's own option: lets you click on every physics step, whatever your refresh rate.", "La opción del propio juego: puedes hacer clic en cada paso de la física, sin importar tu tasa de refresco."},
    {"sp.smart", "Smart Start Pos", "Smart Start Pos"},
    {"sp.smart.d", "Each start pos gets the game mode, speed, size, gravity and dual the player really has there. Every time you pass a start pos playing from the beginning (or from an already learned one), the exact state is learned from the player itself, so orbs, pads and triggers count too. Until then it is worked out from the portals before it. Learned states are saved per level in kmenu-smartsp.txt.",
        "Cada start pos toma el modo, la velocidad, el tamaño, la gravedad y el dual que el jugador tiene de verdad ahí. Cada vez que pasas por un start pos jugando desde el inicio (o desde uno ya aprendido), se aprende el estado exacto leyendo al propio jugador, así que orbes, pads y triggers también cuentan. Mientras tanto se calcula con los portales anteriores. Lo aprendido se guarda por nivel en kmenu-smartsp.txt."},
    {"sp.orbs", "Count gravity orbs and pads", "Contar orbes y pads de gravedad"},
    {"sp.orbs.d", "Blue, green, green dash and spider orbs, and blue and spider pads before the start pos also change the gravity, as if you hit them. Turn it off for levels with fake or optional orbs. Applied on the next restart.",
        "Los orbes azul, verde, dash verde y araña, y los pads azul y araña antes del start pos también cambian la gravedad, como si los tocaras. Apágalo en niveles con orbes falsos u opcionales. Se aplica al reiniciar."},
    {"sp.learned", "Learned in this level: %d of %d", "Aprendidos en este nivel: %d de %d"},
    {"sp.forget", "Forget this level", "Olvidar este nivel"},
    {"sp.exact", "exact", "exacto"},
    {"sp.portals", "from portals", "por portales"},
    {"sp.switch", "Start Pos Switcher", "Cambiar Start Pos con teclas"},
    {"sp.switch.d", "Change to the previous / next start pos of the level with two keys (the game's own start positions).",
        "Cambia a la start pos anterior o siguiente del nivel con dos teclas (las start pos del propio juego)."},
    {"sp.keys", "Switcher keys", "Teclas"},
    {"sp.prev", "Previous", "Anterior"},
    {"sp.next", "Next", "Siguiente"},
    {"sp.keys.d", "Click a key box and press the new key. Esc cancels.", "Haz clic en una tecla y pulsa la nueva. Esc cancela."},
    {"sp.none", "This level has no start positions", "Este nivel no tiene start pos"},
    {"sp.start", "From the start", "Desde el inicio"},
    {"jb.nolevel", "Open a level (its page or play it) and it shows up here.", "Abre un nivel (su página o juégalo) y aparecerá aquí."},
    {"jb.song", "Song", "Canción"},
    {"jb.official", "official song", "canción oficial"},
    {"jb.now", "Playing:", "Suena:"},
    {"jb.original", "the original song", "la canción original"},
    {"jb.sfh", "Song File Hub...", "Song File Hub..."},
    {"jb.sfh.d", "Songs from songfilehub.com made for this level's song.", "Canciones de songfilehub.com hechas para la canción de este nivel."},
    {"jb.local", "Choose a file (MP3/OGG)...", "Elegir archivo (MP3/OGG)..."},
    {"jb.local.d", "Use a song file from your PC for this level.", "Usa un archivo de canción de tu PC para este nivel."},
    {"jb.reset", "Use the original song", "Usar la canción original"},
    {"jb.restart", "Restart the level to hear the change.", "Reinicia el nivel para escuchar el cambio."},
    {"jb.loading", "Loading the Song File Hub list...", "Cargando la lista de Song File Hub..."},
    {"jb.listfail", "Could not load the list. Check your internet and open it again.", "No se pudo cargar la lista. Revisa tu internet y ábrela otra vez."},
    {"jb.none", "Song File Hub has no songs for this level's song yet.", "Song File Hub todavía no tiene canciones para la canción de este nivel."},
    {"jb.nocustom", "This level uses an official song; Song File Hub has songs for Newgrounds songs.", "Este nivel usa una canción oficial; Song File Hub tiene canciones para las de Newgrounds."},
    {"jb.verified", "verified for this level", "verificada para este nivel"},
    {"jb.use", "Use", "Usar"},
    {"jb.done", "Song ready - restart the level", "Canción lista: reinicia el nivel"},
    {"jb.fail", "The download failed.", "La descarga falló."},
    {"gl.safe.on", "Safe Mode is on: this attempt won't be saved.", "Safe Mode activo: este intento no se guardará."},
    {"gl.safe.hint", "Turn the cheats off and restart the attempt to save progress again.",
        "Apaga las trampas y reinicia el intento para volver a guardar progreso."},
};

const char* T(const char* key) {
    for (auto& t : kTexts) if (std::strcmp(t.key, key) == 0) return g_lang == 1 ? t.es : t.en;
    return key;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct Preset { const char* name; ImVec4 color; };
const Preset kPresets[] = {
    {"Kuang", ImVec4(0.49f, 0.36f, 1.00f, 1)},   // violet
    {"Sakura", ImVec4(1.00f, 0.36f, 0.62f, 1)},
    {"Cyan", ImVec4(0.18f, 0.80f, 0.95f, 1)},
    {"Mint", ImVec4(0.26f, 0.88f, 0.62f, 1)},
    {"Sun", ImVec4(1.00f, 0.66f, 0.22f, 1)},
    {"Ruby", ImVec4(0.95f, 0.26f, 0.33f, 1)},
};

ImVec4 g_accent = kPresets[0].color;
float g_scale = 1.0f;
float g_colW = 212.0f;
float g_lastScale = -1;   // forces the style to be rebuilt when it changes
bool g_anim = true;
bool g_dim = true;

ImVec4 hueShift(ImVec4 c, float deg) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    h = std::fmod(h + deg / 360.0f + 1.0f, 1.0f);
    ImVec4 o(0, 0, 0, c.w);
    ImGui::ColorConvertHSVtoRGB(h, s, v, o.x, o.y, o.z);
    return o;
}
ImVec4 mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
ImU32 U(ImVec4 c, float alpha = 1.0f) { c.w *= alpha * ImGui::GetStyle().Alpha; return ImGui::ColorConvertFloat4ToU32(c); }
float S(float v) { return v * g_scale; }

const ImVec4 kBg(0.105f, 0.108f, 0.125f, 0.97f);
const ImVec4 kRow(0.15f, 0.155f, 0.18f, 1);
const ImVec4 kText(0.94f, 0.94f, 0.96f, 1);
const ImVec4 kDim(0.62f, 0.63f, 0.68f, 1);

ImFont* g_font = nullptr;
ImFont* g_fontBold = nullptr;
ImFont* g_fontLabel = nullptr;

// ---------------------------------------------------------------------------
// Game window / input
// ---------------------------------------------------------------------------
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
bool g_menuOpen = false;
bool g_gdOpen = false, g_gdWantClose = false; // the "Kuang Mods" popup of the main menu (see gdTick)
float g_menuT = 0;            // 0 closed .. 1 open (animated)
int g_menuKey = VK_TAB;
bool g_waitingKey = false;
std::deque<double> g_clicks;  // timestamps (s) of clicks for CPS
int g_cpsMax = 0;
long long g_clickTotal = 0;
bool g_mouseHeld = false;
bool g_keyHeld[3] = {};       // Space, Up, W
double g_releasedAt = 0;      // for the green fade
LARGE_INTEGER g_qpf;

double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

std::string keyName(int vk) {
    if (vk == VK_TAB) return "Tab";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = static_cast<LONG>(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_RCONTROL || vk == VK_RMENU)
        lp |= 1 << 24;
    char buf[64] = {};
    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0) return buf;
    return "Key " + std::to_string(vk);
}


void countClick() {
    g_clicks.push_back(now());
    g_clickTotal++;
}
bool anyHeld() { return g_mouseHeld || g_keyHeld[0] || g_keyHeld[1] || g_keyHeld[2]; }

void* g_playLayer = nullptr; // set by inLevelSEH (only a real PlayLayer, not the editor)
void* g_gameLayer = nullptr; // PlayLayer or LevelEditorLayer (both are a GJBaseGameLayer)
// start pos switcher keys (used by the window procedure)
bool g_spSwitcher = true;
int g_spKeyPrev = VK_LEFT, g_spKeyNext = VK_RIGHT;
int* g_waitingSpKey = nullptr;       // which start pos key is being set
volatile int g_spPending = 0;        // -1 / +1 queued from the window procedure

// platformer levels use Left/Right to move: the switcher keys are left to the game there
bool isPlatformerSEH(void* gl) {
    __try {
        auto ls = *reinterpret_cast<unsigned char**>(reinterpret_cast<uintptr_t>(gl) + 0xdc8); // m_levelSettings
        return ls && ls[0x155] != 0;                                                         // m_platformerMode
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LRESULT CALLBACK hookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    bool firstPress = keyDown && !(lp & (1 << 30));
    // CPS (counted even with the menu closed)
    if (msg == WM_LBUTTONDOWN) { countClick(); g_mouseHeld = true; }
    if (msg == WM_LBUTTONUP) { g_mouseHeld = false; g_releasedAt = now(); }
    {
        int k = wp == VK_SPACE ? 0 : wp == VK_UP ? 1 : wp == 'W' ? 2 : -1;
        if (k >= 0 && firstPress) { countClick(); g_keyHeld[k] = true; }
        if (k >= 0 && (msg == WM_KEYUP || msg == WM_SYSKEYUP)) { g_keyHeld[k] = false; g_releasedAt = now(); }
    }
    if (msg == WM_KILLFOCUS) { g_mouseHeld = false; g_keyHeld[0] = g_keyHeld[1] = g_keyHeld[2] = false; }

    if (g_gdOpen && firstPress && wp == VK_ESCAPE) { g_gdWantClose = true; return 0; }
    if (ImGui::GetCurrentContext()) {
        if (g_waitingSpKey && firstPress) {
            if (wp != VK_ESCAPE) {
                *g_waitingSpKey = static_cast<int>(wp);
                cfgSetInt(g_waitingSpKey == &g_spKeyPrev ? "sp.prev" : "sp.next", *g_waitingSpKey);
            }
            g_waitingSpKey = nullptr;
            return 0;
        }
        if (g_spSwitcher && firstPress && g_playLayer && !isPlatformerSEH(g_playLayer) &&
            !(g_menuOpen && ImGui::GetIO().WantCaptureKeyboard)) {
            if (static_cast<int>(wp) == g_spKeyPrev) { g_spPending = -1; return 0; }
            if (static_cast<int>(wp) == g_spKeyNext) { g_spPending = 1; return 0; }
        }
        if (g_waitingKey && firstPress) {
            if (wp != VK_ESCAPE) { g_menuKey = static_cast<int>(wp); cfgSetInt("menu.key", g_menuKey); }
            g_waitingKey = false;
            return 0;
        }
        if (firstPress && static_cast<int>(wp) == g_menuKey) {
            g_menuOpen = !g_menuOpen;
            return 0;
        }
        if (g_menuOpen || g_menuT > 0.01f) {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            ImGuiIO& io = ImGui::GetIO();
            bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
            bool key = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
            if (g_menuOpen && mouse && io.WantCaptureMouse) return 0;
            if (g_menuOpen && key && io.WantCaptureKeyboard) return 0;
        }
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Game state (no GD offsets needed for phase 1)
// ---------------------------------------------------------------------------
using FnV = void (*)(void*);
void* (*g_sharedDirector)() = nullptr;
void* (*g_runningScene)(void*) = nullptr;

const char* rttiName(void* obj) {
    auto vt = *reinterpret_cast<void***>(obj);
    auto col = reinterpret_cast<const uint32_t*>(vt[-1]);
    if (!col || col[0] != 1) return nullptr;
    uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];
    return reinterpret_cast<const char*>(base + col[3] + 16);
}


// children of a cocos node: the CCArray lives at +0xd0 and its data at +0x38 (same layout as for the scene)
const char* kEndLayerRtti = ".?AVEndLevelLayer@@";
bool g_endScreen = false; // the "Level Complete" panel is on screen right now

bool nodeHasChildNamed(void* node, const char* rtti) {
    __try {
        void* ch = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(node) + 0xd0);
        if (!ch) return false;
        void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ch) + 0x38);
        if (!data) return false;
        unsigned c = *reinterpret_cast<unsigned*>(data);
        void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(data) + 0x10);
        for (unsigned i = 0; i < c && i < 32; i++) {
            const char* n = rttiName(arr[i]);
            if (n && std::strcmp(n, rtti) == 0) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool inLevelSEH() {
    g_playLayer = nullptr;
    g_gameLayer = nullptr;
    g_endScreen = false;
    bool found = false;
    __try {
        void* dir = g_sharedDirector();
        if (!dir) return false;
        void* scene = g_runningScene(dir);
        if (!scene) return false;
        void* ch = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(scene) + 0xd0);
        if (!ch) return false;
        void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ch) + 0x38);
        if (!data) return false;
        unsigned c = *reinterpret_cast<unsigned*>(data);
        void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(data) + 0x10);
        for (unsigned i = 0; i < c && i < 32; i++) {
            const char* n = rttiName(arr[i]);
            if (!n) continue;
            if (std::strcmp(n, ".?AVPlayLayer@@") == 0) { g_playLayer = g_gameLayer = arr[i]; found = true; }
            else if (std::strcmp(n, ".?AVLevelEditorLayer@@") == 0) { if (!g_gameLayer) g_gameLayer = arr[i]; found = true; }
            else if (std::strcmp(n, kEndLayerRtti) == 0) g_endScreen = true;
        }
        // the panel may also be a child of the PlayLayer instead of the scene
        if (g_playLayer && !g_endScreen) g_endScreen = nodeHasChildNamed(g_playLayer, kEndLayerRtti);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

bool g_inLevel = false;
double g_lastLevelCheck = 0;
double g_sessionStart = 0;
double g_fps = 0;
int g_fpsFrames = 0;
double g_fpsStart = 0;

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
std::string g_search;

bool matches(const char* label) {
    if (g_search.empty()) return true;
    std::string a = label, b = g_search;
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

void tooltip(const char* desc) {
    if (!desc || !*desc || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(260));
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

// like ImGui's ShadeVertsLinearColorGradientKeepAlpha, but the gradient colors' alpha counts too
// (vertex alpha x gradient alpha), so faded/transparent gradients work
void shadeGradient(ImDrawList* dl, int v0, int v1, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
    ImVec2 d(p1.x - p0.x, p1.y - p0.y);
    float len2 = d.x * d.x + d.y * d.y;
    float inv = len2 > 0 ? 1.0f / len2 : 0.0f;
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(c0), b = ImGui::ColorConvertU32ToFloat4(c1);
    for (int i = v0; i < v1; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = std::clamp(((v.pos.x - p0.x) * d.x + (v.pos.y - p0.y) * d.y) * inv, 0.f, 1.f);
        float va = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        ImVec4 c(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, (a.w + (b.w - a.w) * t) * va);
        v.col = ImGui::ColorConvertFloat4ToU32(c);
    }
}

// a full-width row with a label and an animated switch; returns true when toggled.
// `extra` adds a small gear on the right that opens the popup named popupId.
bool toggleRow(const char* id, const char* label, const char* desc, bool* v, const char* popupId = nullptr) {
    if (!matches(label)) return false;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    float width = ImGui::GetContentRegionAvail().x, h = S(27);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float gearW = popupId ? S(22) : 0;
    bool openPopup = false;
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("##row", ImVec2(width - gearW, h));
    bool hovered = ImGui::IsItemHovered();
    tooltip(desc);
    if (pressed) *v = !*v;
    // animated knob
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID("##t");
    float t = st->GetFloat(key, *v ? 1.f : 0.f);
    float target = *v ? 1.f : 0.f;
    t = g_anim ? t + (target - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f) : target;
    st->SetFloat(key, t);

    auto dl = w->DrawList;
    // no switch: the whole row is the button. On = accent glow from the left, bright text and a bar.
    ImVec2 q(p.x + width, p.y + h);
    if (t > 0.01f) {
        int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, 1)), S(6));
        shadeGradient(dl, v0, dl->VtxBuffer.Size, p, ImVec2(q.x, p.y),
            U(g_accent, 0.60f * t), U(hueShift(g_accent, 38), 0.16f * t));
        dl->AddRectFilled(ImVec2(p.x, p.y + S(5)), ImVec2(p.x + S(3), q.y - S(5)), U(ImVec4(1, 1, 1, 0.95f), t), S(2));
    }
    if (hovered) dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, 0.06f)), S(6));
    if (hovered && ImGui::IsMouseDown(0)) dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, 0.05f)), S(6));
    ImVec4 tc = mix(kDim, ImVec4(1, 1, 1, 1), t);
    float tx = p.x + S(10) + S(3) * t;
    if (t > 0.5f) {
        ImGui::PushFont(g_fontBold);
        dl->AddText(ImVec2(tx, p.y + (h - ImGui::GetFontSize()) * 0.5f), U(tc), label);
        ImGui::PopFont();
    } else {
        dl->AddText(ImVec2(tx, p.y + (h - ImGui::GetFontSize()) * 0.5f), U(tc), label);
    }
    if (popupId) {
        ImGui::SameLine(0, 0);
        ImVec2 gp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##gear", ImVec2(gearW, h))) openPopup = true;
        bool gh = ImGui::IsItemHovered();
        ImVec2 c(gp.x + gearW * 0.5f, gp.y + h * 0.5f);
        float r = S(4.5f);
        // three dots "more options" icon
        for (int i = -1; i <= 1; i++) dl->AddCircleFilled(ImVec2(c.x, c.y + i * r), S(1.4f), U(gh ? kText : kDim));
    }
    ImGui::PopID();
    if (openPopup) ImGui::OpenPopup(popupId);
    return pressed;
}

// ---- gradient helpers (the grey/black parts fade too, not only the accent ones)
void gradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool vertical = false,
    ImDrawFlags flags = 0) {
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, U(ImVec4(1, 1, 1, 1)), rounding, flags);
    shadeGradient(dl, v0, dl->VtxBuffer.Size, a, vertical ? ImVec2(a.x, b.y) : ImVec2(b.x, a.y), c0, c1);
}

void gradText(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 c0, ImU32 c1, ImFont* font = nullptr, float size = 0) {
    int v0 = dl->VtxBuffer.Size;
    if (!font) font = ImGui::GetFont();
    if (size <= 0) size = ImGui::GetFontSize();
    dl->AddText(font, size, pos, U(ImVec4(1, 1, 1, 1)), text);
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    shadeGradient(dl, v0, dl->VtxBuffer.Size, pos, ImVec2(pos.x + std::max(ts.x, 1.f), pos.y), c0, c1);
}

// grey caption above a control, faded from light grey to an accent-tinted grey
void caption(const char* text) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::Dummy(ts);
    gradText(ImGui::GetWindowDrawList(), ImVec2(p.x + S(2), p.y), text, U(ImVec4(0.80f, 0.81f, 0.86f, 1)),
        U(mix(kDim, g_accent, 0.55f)));
}

// grey multi-line hint, faded vertically
void hintText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(0);
    auto dl = ImGui::GetWindowDrawList();
    int v0 = dl->VtxBuffer.Size;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::TextUnformatted(text);
    ImVec2 q = ImGui::GetItemRectMax();
    shadeGradient(dl, v0, dl->VtxBuffer.Size, p, ImVec2(q.x, p.y),
        U(ImVec4(0.66f, 0.67f, 0.72f, 1)), U(mix(kDim, g_accent, 0.45f), 0.85f));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// pill button drawn by hand so it can have a gradient; `selected` paints it with the accent
bool pillButton(const char* id, const char* label, ImVec2 size, bool selected) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    bool pressed = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    auto dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    if (selected) gradRect(dl, p, q, U(g_accent), U(hueShift(g_accent, 38)), S(6));
    else gradRect(dl, p, q, U(ImVec4(0.20f, 0.205f, 0.235f, 1)), U(ImVec4(0.135f, 0.14f, 0.16f, 1)), S(6));
    if (hovered) dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, held ? 0.12f : 0.07f)), S(6));
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f);
    if (selected) dl->AddText(tp, U(ImVec4(1, 1, 1, 1)), label);
    else gradText(dl, tp, label, U(kText), U(mix(kText, g_accent, 0.35f)));
    return pressed;
}

bool buttonRow(const char* id, const char* label, const char* desc) {
    if (!matches(label)) return false;
    float width = ImGui::GetContentRegionAvail().x, h = S(27);
    ImGui::PushID(id);
    bool pressed = pillButton("##btn", label, ImVec2(width, h), false);
    tooltip(desc);
    ImGui::PopID();
    return pressed;
}

// slider with a faded background (ImGui draws the slider itself on top of it)
bool gradSlider(const char* id, float* v, float mn, float mx, const char* fmt, ImGuiSliderFlags flags = 0) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth();
    if (w <= 0) w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight();
    gradRect(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + h), U(ImVec4(0.20f, 0.205f, 0.235f, 1)),
        U(ImVec4(0.13f, 0.135f, 0.155f, 1)), S(6));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, g_accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, hueShift(g_accent, 25));
    ImGui::SetNextItemWidth(w);
    bool r = ImGui::SliderFloat(id, v, mn, mx, fmt, flags);
    ImGui::PopStyleColor(5);
    return r;
}

// ---------------------------------------------------------------------------
// Columns: they never overlap. A column dropped on top of another slides to the nearest free spot,
// and popups open beside the column, in free space, so they never cover another column.
// ---------------------------------------------------------------------------
std::map<std::string, bool> g_collapsed;
bool g_resetLayout = false;
struct ColRect { ImVec2 min, max; int frame; };
std::map<std::string, ColRect> g_colRects;   // last known rectangle of every column
std::map<std::string, ImVec2> g_pendingMove; // where a column must go next frame
std::string g_dragCol;                       // column being dragged by its title
int g_frameNo = 0;
constexpr int kLayoutVersion = 5;            // bump to re-arrange everybody once after an update

bool rectsOverlap(ImVec2 a0, ImVec2 a1, ImVec2 b0, ImVec2 b1, float gap) {
    return a0.x < b1.x + gap && a1.x + gap > b0.x && a0.y < b1.y + gap && a1.y + gap > b0.y;
}

// does a rectangle hit any visible column (except `skip`)?
const ColRect* hitColumn(ImVec2 a, ImVec2 b, const std::string& skip, float gap) {
    for (auto& [id, r] : g_colRects) {
        if (id == skip || g_frameNo - r.frame > 2) continue;
        if (rectsOverlap(a, b, r.min, r.max, gap)) return &r;
    }
    return nullptr;
}

// nearest free top-left for a box of `size`, starting at `want`
ImVec2 freeSpot(ImVec2 want, ImVec2 size, const std::string& skip) {
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float gap = S(10);
    auto clampIn = [&](ImVec2 p) {
        return ImVec2(std::clamp(p.x, 0.f, std::max(0.f, disp.x - size.x)), std::clamp(p.y, 0.f, std::max(0.f, disp.y - size.y)));
    };
    want = clampIn(want);
    if (!hitColumn(want, ImVec2(want.x + size.x, want.y + size.y), skip, gap)) return want;
    // candidates: right/left/below/above of every column, closest to `want` wins
    std::vector<ImVec2> cands;
    for (auto& [id, r] : g_colRects) {
        if (id == skip || g_frameNo - r.frame > 2) continue;
        cands.push_back(ImVec2(r.max.x + gap, want.y));
        cands.push_back(ImVec2(r.min.x - gap - size.x, want.y));
        cands.push_back(ImVec2(want.x, r.max.y + gap));
        cands.push_back(ImVec2(want.x, r.min.y - gap - size.y));
        cands.push_back(ImVec2(r.max.x + gap, r.min.y));
        cands.push_back(ImVec2(r.min.x, r.max.y + gap));
    }
    float best = FLT_MAX;
    ImVec2 bestP = want;
    for (auto c : cands) {
        if (c.x < 0 || c.y < 0 || c.x + size.x > disp.x || c.y + size.y > disp.y) continue;
        if (hitColumn(c, ImVec2(c.x + size.x, c.y + size.y), skip, gap * 0.9f)) continue;
        float d = (c.x - want.x) * (c.x - want.x) + (c.y - want.y) * (c.y - want.y);
        if (d < best) { best = d; bestP = c; }
    }
    return bestP;
}

// default arrangement: a row from the left, wrapping to a second row when the screen ends
ImVec2 defaultColumnPos(int index) {
    float w = S(g_colW), gap = S(12);
    float disp = ImGui::GetIO().DisplaySize.x;
    int perRow = std::max(1, static_cast<int>((disp - S(14)) / (w + gap)));
    int row = index / perRow, col = index % perRow;
    return ImVec2(S(14) + col * (w + gap), S(14) + row * S(380));
}

// popups: call right before BeginPopup(id); they open next to the current column, never on top of another one
std::map<std::string, ImVec2> g_popupSize;
std::map<std::string, int> g_popupSeen;   // frame number a popup was last drawn
void placePopup(const char* id) {
    if (!ImGui::IsPopupOpen(id)) return;
    auto seen = g_popupSeen.find(id);
    bool justOpened = seen == g_popupSeen.end() || g_frameNo - seen->second > 1;
    if (justOpened && !g_popupSize.count(id)) {
        // size unknown yet: lay it out off-screen this one frame, place it for real the next
        ImGui::SetNextWindowPos(ImVec2(-10000, -10000), ImGuiCond_Always);
        return;
    }
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImVec2 size = g_popupSize.count(id) ? g_popupSize[id] : ImVec2(S(260), S(200));
    float gap = S(10);
    ImVec2 want(w->Pos.x + w->Size.x + gap, ImGui::GetCursorScreenPos().y - S(30));
    ImVec2 pos = freeSpot(want, size, "");
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
}
void notePopupSize(const char* id) {
    g_popupSize[id] = ImGui::GetWindowSize();
    g_popupSeen[id] = g_frameNo;
}

// returns true if the body should be drawn
bool beginColumn(const char* id, const char* titleKey, int index) {
    float width = S(g_colW);
    ImVec2 def = defaultColumnPos(index);
    std::string px = std::string("pos.") + id + ".x", py = std::string("pos.") + id + ".y";
    bool hasPos = g_cfg.count(px) && g_cfg.count(py);
    ImVec2 pos = hasPos ? ImVec2(cfgFloat(px, 0) * g_scale, cfgFloat(py, 0) * g_scale) : def;
    if (g_resetLayout) ImGui::SetNextWindowPos(def, ImGuiCond_Always);
    else if (g_pendingMove.count(id)) { ImGui::SetNextWindowPos(g_pendingMove[id], ImGuiCond_Always); g_pendingMove.erase(id); }
    else ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(0)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBg);
    ImGui::PushStyleColor(ImGuiCol_Border, U(g_accent, 0.35f));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImVec2 wp = w->Pos;
    float hh = S(34);
    auto dl = w->DrawList;
    // body: dark, faded vertically with a hint of the accent at the top
    gradRect(dl, ImVec2(wp.x, wp.y + hh), ImVec2(wp.x + w->Size.x, wp.y + w->Size.y), U(mix(kBg, g_accent, 0.10f)),
        U(ImVec4(0.075f, 0.078f, 0.09f, 0.97f)), S(10), true, ImDrawFlags_RoundCornersBottom);
    // header: accent gradient
    ImVec4 a2 = hueShift(g_accent, 38);
    gradRect(dl, wp, ImVec2(wp.x + width, wp.y + hh), U(g_accent), U(a2), S(10), false, ImDrawFlags_RoundCornersTop);
    ImGui::PushFont(g_fontBold);
    const char* title = T(titleKey);
    ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(wp.x + (width - ts.x) * 0.5f + 1, wp.y + (hh - ts.y) * 0.5f + 1), U(ImVec4(0, 0, 0, 0.25f)), title);
    dl->AddText(ImVec2(wp.x + (width - ts.x) * 0.5f, wp.y + (hh - ts.y) * 0.5f), U(ImVec4(1, 1, 1, 1)), title);
    ImGui::PopFont();
    bool collapsed = g_collapsed[id];
    float cx = wp.x + S(14), cy = wp.y + hh * 0.5f;
    dl->AddLine(ImVec2(cx - S(4), cy), ImVec2(cx + S(4), cy), U(ImVec4(1, 1, 1, 0.9f)), S(2));
    if (collapsed) dl->AddLine(ImVec2(cx, cy - S(4)), ImVec2(cx, cy + S(4)), U(ImVec4(1, 1, 1, 0.9f)), S(2));

    // header: click = fold, drag = move
    ImGui::SetCursorScreenPos(wp);
    ImGui::InvisibleButton("##hdr", ImVec2(width, hh));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) {
        g_dragCol = id;
        ImGui::SetWindowPos(ImVec2(w->Pos.x + io.MouseDelta.x, w->Pos.y + io.MouseDelta.y));
    }
    if (ImGui::IsItemDeactivated()) {
        if (g_dragCol == id) {
            // dropped: slide out of any column it landed on, then save
            ImVec2 np = freeSpot(w->Pos, w->Size, id);
            if (np.x != w->Pos.x || np.y != w->Pos.y) g_pendingMove[id] = np;
            cfgSetFloat(px, np.x / g_scale);
            cfgSetFloat(py, np.y / g_scale);
            g_dragCol.clear();
        } else {
            g_collapsed[id] = !collapsed;
            cfgSetBool(std::string("col.") + id, !collapsed);
        }
    }
    if (g_resetLayout || !hasPos) { cfgSetFloat(px, (g_resetLayout ? def.x : pos.x) / g_scale); cfgSetFloat(py, (g_resetLayout ? def.y : pos.y) / g_scale); }
    ImGui::Dummy(ImVec2(0, S(4)));
    return !collapsed;
}

void endColumn() {
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    g_colRects[w->Name] = {w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), g_frameNo};
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Status labels
// ---------------------------------------------------------------------------
// Each label can be turned on, moved anywhere (drag it with the menu open), resized and faded on its own.
struct Label {
    const char* id; const char* key; const char* desc;
    bool on = false;
    float x = -1, y = -1;   // fraction of the screen; x is the left edge, or the right edge when `right`
    bool right = false;
    float scale = 1.0f, opacity = 0.9f;
    int corner = -1;                 // 0 TL, 1 TR, 2 BL, 3 BR when snapped with the buttons
    float lastW = 0, lastH = 0;      // size drawn last frame (pixels)
};
Label g_labels[] = {
    {"fps", "st.fps", "st.fps.d"},
    {"cps", "st.cps", "st.cps.d"},
    {"clock", "st.clock", "st.clock.d"},
    {"session", "st.session", "st.session.d"},
    {"message", "st.message", "st.message.d"},
    {"cheat", "st.cheat", "st.cheat.d"},
    {"attempts", "st.att", "st.att.d"},
    {"bestrun", "st.best", "st.best.d"},
    {"ncdeaths", "st.ncd", "st.ncd.d"},
    {"ncacc", "st.nca", "st.nca.d"},
    {"rstats", "st.rs", "st.rs.d"},
    {"inputlat", "st.il", "st.il.d"},
};
constexpr int kLabelCount = sizeof(g_labels) / sizeof(g_labels[0]);
enum { L_FPS, L_CPS, L_CLOCK, L_SESSION, L_MESSAGE, L_CHEAT, L_ATTEMPTS, L_BESTRUN, L_NCDEATHS, L_NCACC, L_RSTATS, L_INPUTLAT };
// input latency meter: values shown by the label (ms, -1 = nothing measured yet)
double g_latShowAvg = -1, g_latShowP95 = -1;

// ---- render stats (phase 1 of the renderer work: only measures). Filled by the OpenGL hooks installed the
// first time the label is turned on; averages over the last ~0.5 s of frames.
struct RenderStats {
    double logicMs = 0, drawMs = 0, swapMs = 0;   // CPU: game logic / submitting the drawing / buffer swap wait
    double visMs = -1;                            // part of the logic spent in PlayLayer::updateVisibility (-1 = not measured)
    double draws = 0, tris = 0, texBinds = 0, blendChanges = 0; // per frame
    bool valid = false;
};
RenderStats g_rs;
// deep profile: time inside the game functions that Bismuth-style rendering would replace
enum { PR_STEP, PR_MOVE, PR_ROTATE, PR_FOLLOW, PR_PFOLLOW, PR_AFOLLOW, PR_TRANSFORM, PR_DYNAMIC, PR_COLORS, PR_PREVIS, PR_COUNT };
bool g_profHooked = false, g_profTried = false;
double g_rsProfMs[PR_COUNT] = {};   // averages per frame (ms) of the last window
bool g_rsInstalled = false;
// Visibility Throttle: run PlayLayer::updateVisibility at most g_visHz times per second (experimental, off by default)
bool g_visThrottle = false;
float g_visHz = 480.0f;
bool g_visAuto = true;      // use the monitor's refresh rate instead of g_visHz
int g_monitorHz = 0;        // refresh rate of the monitor the game is on (0 = unknown)
double g_monitorHzRead = 0;
// refresh rate of the monitor that shows the game (re-read every 2 s: the window can move to another screen)
int monitorHz() {
    double t = now();
    if (g_monitorHz > 0 && t - g_monitorHzRead < 2.0) return g_monitorHz;
    g_monitorHzRead = t;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(g_hwnd ? g_hwnd : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        g_monitorHz = static_cast<int>(dm.dmDisplayFrequency);
    else if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        g_monitorHz = static_cast<int>(dm.dmDisplayFrequency);
    if (g_monitorHz <= 0) g_monitorHz = 60;
    return g_monitorHz;
}
double visHzNow() { return g_visAuto ? static_cast<double>(monitorHz()) : static_cast<double>(g_visHz); }
constexpr float kVisHzMin = 1.0f, kVisHzMax = 100000.0f; // typed values may be anything in this range

// ---- noclip deaths / accuracy (frames where you would have died, while the level moves)
bool g_noclip = false;
bool g_ncHit = false, g_ncTouching = false; // g_ncTouching: you are inside something deadly right now
double g_ncLastHitT = 0;       // time of the last hit reported by the destroyPlayer hook
int g_ncDeaths = 0;
// Accuracy is counted in physics steps, like Mega Hack: dead steps / steps played. GD simulates 240 steps per
// game second whatever the FPS, so the steps played come from the time the level was moving.
constexpr double kPhysicsTps = 240.0;
double g_ncSteps = 0, g_ncDeadSteps = 0;
int g_ncHitCalls = 0;                 // destroyPlayer calls since the last drawn frame
double g_ncPrevT = 0, g_ncMoveT = 0;  // previous frame time, last time the percentage moved
float g_ncLastPct = -1;
bool g_ncHitbox = true;        // show the hitboxes only while you are touching something deadly with Noclip
bool g_hbForce = false;        // read by the shouldDraw hook: "draw the hitboxes now"
bool g_hbHooked = false;
int g_ncLog = 0;               // how many hits were written to kmenu.log (debugging aid)
// The hook only fires on physics steps, but the screen can draw many more frames than that (high FPS,
// Frame Extrapolation). So "still touching" means "a hit arrived within the last kNcGap seconds of game
// time" instead of "a hit arrived in this exact frame"; otherwise one long touch looked like dozens of them.
constexpr double kNcGap = 0.04;
void noclipReset() { g_ncHit = g_ncTouching = false; g_ncLastHitT = 0; g_ncDeaths = 0; g_ncSteps = g_ncDeadSteps = 0; g_ncHitCalls = 0; g_ncPrevT = g_ncMoveT = 0; g_ncLastPct = -1; }

// ---- attempts and best run (this session, per level)
float (*g_getPercent)(void*) = nullptr;   // PlayLayer::getCurrentPercent
void* g_runLayer = nullptr;
float g_runStart = 0, g_bestS = -1, g_bestE = -1;
bool g_runActive = false;

float percentSEH(void* pl) {
    __try {
        return (g_getPercent && pl) ? g_getPercent(pl) : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}
int attemptsSEH(void* pl) {
    __try {
        return pl ? *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pl) + 0x3164) : 0; // GJBaseGameLayer attempt counter
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void runStarted(void* pl) {
    if (pl != g_runLayer) { g_runLayer = pl; g_bestS = g_bestE = -1; }
    noclipReset();
    g_runStart = percentSEH(pl);
    g_runActive = true;
}
void runEnded(void* pl, float end) {
    if (!g_runActive || pl != g_runLayer) return;
    g_runActive = false;
    if (end > 100.f) end = 100.f;
    if (g_bestE < 0 || end - g_runStart > g_bestE - g_bestS) { g_bestS = g_runStart; g_bestE = end; }
}

struct StatusOpts {
    bool always = false;
    float size = 1.0f, opacity = 1.0f;   // multiply every label
    std::string text = "Kuang Menu";
    bool cpsGreen = true, cpsMax = true, cpsUnit = true, cpsTotal = false;
};
StatusOpts g_st;
int g_dragLabel = -1;
ImVec2 g_dragOffset;

std::string lk(const Label& l, const char* f) { return std::string("lb.") + l.id + "." + f; }

void saveLabel(const Label& l) {
    cfgSetBool(lk(l, "on"), l.on);
    cfgSetFloat(lk(l, "x"), l.x);
    cfgSetFloat(lk(l, "y"), l.y);
    cfgSetBool(lk(l, "right"), l.right);
    cfgSetFloat(lk(l, "scale"), l.scale);
    cfgSetFloat(lk(l, "opacity"), l.opacity);
    cfgSetInt(lk(l, "corner"), l.corner);
}

void loadStatus() {
    for (auto& l : g_labels) {
        l.on = cfgBool(lk(l, "on"), false);
        l.x = cfgFloat(lk(l, "x"), -1);
        l.y = cfgFloat(lk(l, "y"), -1);
        l.right = cfgBool(lk(l, "right"), false);
        l.scale = cfgFloat(lk(l, "scale"), 1.0f);
        l.opacity = cfgFloat(lk(l, "opacity"), 0.9f);
        l.corner = cfgInt(lk(l, "corner"), -1);
    }
    g_st.always = cfgBool("st.always", false);
    g_st.size = cfgFloat("st.size", 1.0f);
    g_st.opacity = cfgFloat("st.opacity", 1.0f);
    g_st.text = cfgStr("st.text", "Kuang Menu");
    g_st.cpsGreen = cfgBool("cps.green", true);
    g_st.cpsMax = cfgBool("cps.max", true);
    g_st.cpsUnit = cfgBool("cps.unit", true);
    g_st.cpsTotal = cfgBool("cps.total", false);
}

bool anyLabelOn() {
    for (auto& l : g_labels) if (l.on) return true;
    return false;
}

bool cheatsActive();
bool safeModeActive();
int cheatShown();

// text of a label; `col` can be changed (CPS goes green while you click)
std::string labelText(int i, ImVec4& col) {
    char b[160];
    switch (i) {
    case L_FPS: std::snprintf(b, sizeof(b), "%.0f FPS", g_fps); return b;
    case L_CPS: {
        double t = now();
        while (!g_clicks.empty() && t - g_clicks.front() > 1.0) g_clicks.pop_front();
        int cps = static_cast<int>(g_clicks.size());
        g_cpsMax = std::max(g_cpsMax, cps);
        std::string s = std::to_string(cps);
        if (g_st.cpsMax) s += "/" + std::to_string(g_cpsMax);
        if (g_st.cpsTotal) s += "/" + std::to_string(g_clickTotal);   // 3/5/30 = now / highest / total clicks
        if (g_st.cpsUnit) s += " CPS";
        if (g_st.cpsGreen) {
            float k = anyHeld() ? 1.f : std::max(0.f, 1.f - static_cast<float>(t - g_releasedAt) / 0.15f);
            col = mix(col, ImVec4(0.30f, 1.0f, 0.42f, col.w), k);
        }
        return s;
    }
    case L_CLOCK: { SYSTEMTIME st; GetLocalTime(&st); std::snprintf(b, sizeof(b), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond); return b; }
    case L_SESSION: {
        int sec = static_cast<int>(now() - g_sessionStart);
        std::snprintf(b, sizeof(b), "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
        return b;
    }
    case L_MESSAGE: return g_st.text;
    case L_NCDEATHS:
        if (!g_playLayer || !g_noclip) return "";
        std::snprintf(b, sizeof(b), "%s %d", T("st.ncd.fmt"), g_ncDeaths);
        return b;
    case L_NCACC:
        if (!g_playLayer || !g_noclip) return "";
        std::snprintf(b, sizeof(b), "%s %.2f%%", T("st.nca.fmt"),
            g_ncSteps >= 1.0 ? 100.0 * (1.0 - std::min(g_ncDeadSteps, g_ncSteps) / g_ncSteps) : 100.0);
        return b;
    case L_RSTATS:
        if (!g_rs.valid) return "...";
        if (g_rs.visMs >= 0)
            std::snprintf(b, sizeof(b), "%s %.0f | %.1fk tris | tex %.0f | blend %.0f\n%s %.2f (vis %.2f) | draw %.2f | swap %.2f",
                T("st.rs.l1"), g_rs.draws, g_rs.tris / 1000.0, g_rs.texBinds, g_rs.blendChanges,
                T("st.rs.l2"), g_rs.logicMs, g_rs.visMs, g_rs.drawMs, g_rs.swapMs);
        else
            std::snprintf(b, sizeof(b), "%s %.0f | %.1fk tris | tex %.0f | blend %.0f\n%s %.2f | draw %.2f | swap %.2f",
                T("st.rs.l1"), g_rs.draws, g_rs.tris / 1000.0, g_rs.texBinds, g_rs.blendChanges,
                T("st.rs.l2"), g_rs.logicMs, g_rs.drawMs, g_rs.swapMs);
        if (g_profHooked) {
            size_t len = std::strlen(b);
            double objs = std::max(0.0, g_rs.visMs - g_rsProfMs[PR_COLORS] - g_rsProfMs[PR_PREVIS]);
            std::snprintf(b + len, sizeof(b) - len, "\n%s %.2f | %s %.2f | %s %.2f", T("st.rs.trig"), g_rsProfMs[PR_STEP], T("st.rs.col"),
                g_rsProfMs[PR_COLORS], T("st.rs.obj"), objs);
        }
        return b;
    case L_INPUTLAT:
        if (g_latShowAvg < 0) { std::snprintf(b, sizeof(b), "%s -", T("st.il.fmt")); return b; }
        std::snprintf(b, sizeof(b), "%s %.2f ms (p95 %.2f)", T("st.il.fmt"), g_latShowAvg, g_latShowP95);
        return b;
    case L_ATTEMPTS:
        if (!g_playLayer) return "";
        std::snprintf(b, sizeof(b), "%s %d", T("st.att.fmt"), attemptsSEH(g_playLayer));
        return b;
    case L_BESTRUN:
        if (!g_playLayer) return "";
        if (g_bestE < 0 || g_runLayer != g_playLayer) std::snprintf(b, sizeof(b), "%s -", T("st.best.fmt"));
        else std::snprintf(b, sizeof(b), "%s %.0f-%.0f%%", T("st.best.fmt"), std::floor(g_bestS), std::floor(g_bestE));
        return b;
    }
    return "";
}

void drawStatus() {
    bool editing = g_menuOpen;
    if (!g_inLevel && !g_st.always && !editing) return;
    auto dl = ImGui::GetForegroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 disp = io.DisplaySize;
    float pad = S(10);
    // labels that sit in a corner (snapped, or not placed yet = top-left) stack by their real heights, so
    // different sizes never overlap and the stack re-flows by itself when a size changes
    float stack[4] = {0, 0, 0, 0};
    bool hoverAny = false;
    for (int i = 0; i < kLabelCount; i++) {
        Label& l = g_labels[i];
        if (!l.on) continue;
        ImVec4 col(1, 1, 1, std::clamp(l.opacity * g_st.opacity, 0.f, 1.f));
        std::string text = labelText(i, col);
        float size = g_fontLabel->FontSize * 0.5f * l.scale * g_st.size;
        ImVec2 ts = i == L_CHEAT ? ImVec2(size * 0.62f, size) : g_fontLabel->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        if (i != L_CHEAT && text.empty()) continue;
        l.lastW = ts.x;
        l.lastH = ts.y;
        // position: stacked in its corner (0 TL, 1 TR, 2 BL, 3 BR; no saved spot = top-left), or a free spot
        ImVec2 pos;
        int corner = l.corner >= 0 ? l.corner : ((l.x < 0 || l.y < 0) ? 0 : -1);
        if (corner >= 0) {
            bool right = corner == 1 || corner == 3;
            pos.x = right ? disp.x - pad - ts.x : pad;
            pos.y = corner >= 2 ? disp.y - pad - stack[corner] - ts.y : pad + stack[corner];
            if (g_dragLabel != i) stack[corner] += ts.y + std::max(S(2.f), ts.y * 0.12f);
        } else {
            pos = ImVec2(l.right ? l.x * disp.x - ts.x : l.x * disp.x, l.y * disp.y);
        }
        // dragging (only with the menu open)
        ImVec2 mn(pos.x - S(4), pos.y - S(2)), mx(pos.x + ts.x + S(4), pos.y + ts.y + S(2));
        bool hover = editing && g_dragLabel < 0 && ImGui::IsMouseHoveringRect(mn, mx, false) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive();
        if (hover) {
            hoverAny = true;
            if (ImGui::IsMouseClicked(0)) { g_dragLabel = i; g_dragOffset = ImVec2(io.MousePos.x - pos.x, io.MousePos.y - pos.y); }
        }
        if (g_dragLabel == i) {
            hoverAny = true;
            pos = ImVec2(std::clamp(io.MousePos.x - g_dragOffset.x, 0.f, disp.x - ts.x), std::clamp(io.MousePos.y - g_dragOffset.y, 0.f, disp.y - ts.y));
            mn = ImVec2(pos.x - S(4), pos.y - S(2));
            mx = ImVec2(pos.x + ts.x + S(4), pos.y + ts.y + S(2));
            if (!ImGui::IsMouseDown(0)) {
                // anchor to the nearest side so the label grows away from the screen edge
                l.right = pos.x + ts.x * 0.5f > disp.x * 0.5f;
                l.x = (l.right ? pos.x + ts.x : pos.x) / disp.x;
                l.y = pos.y / disp.y;
                l.corner = -1;
                saveLabel(l);
                g_dragLabel = -1;
            }
        }
        if (editing) {
            float a = g_menuT;
            dl->AddRectFilled(mn, mx, U(g_accent, (g_dragLabel == i ? 0.30f : hover ? 0.22f : 0.10f) * a), S(5));
            dl->AddRect(mn, mx, U(g_accent, 0.8f * a), S(5), 0, S(1.2f));
        }
        if (i == L_CHEAT) {
            int cs = cheatShown();
            ImVec4 c = cs == 1 ? ImVec4(1.0f, 0.27f, 0.30f, col.w)          // red: a cheat is on
                     : cs == 2 ? ImVec4(0.80f, 0.36f, 0.04f, col.w)         // dark orange: used, now off
                               : ImVec4(0.30f, 1.0f, 0.42f, col.w);         // green: clean
            ImVec2 cc(pos.x + ts.x * 0.5f, pos.y + ts.y * 0.55f);
            dl->AddCircleFilled(cc, ts.x * 0.5f, IM_COL32(0, 0, 0, static_cast<int>(170 * col.w)));
            dl->AddCircleFilled(cc, ts.x * 0.5f - S(1.5f), ImGui::ColorConvertFloat4ToU32(c));
            continue;
        }
        ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(190 * col.w));
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) dl->AddText(g_fontLabel, size, ImVec2(pos.x + dx * 1.3f, pos.y + dy * 1.3f), shadow, text.c_str());
        dl->AddText(g_fontLabel, size, pos, ImGui::ColorConvertFloat4ToU32(col), text.c_str());
    }
    // a click on a label must not reach the game (you'd jump)
    if (hoverAny) ImGui::SetNextFrameWantCaptureMouse(true);
}

// Small badge in the top-left corner of the "Level Complete" panel: a dot with how the level was passed
// (same colors as the Cheat Indicator) and the menu version. It follows the Cheat Indicator option.
// The panel is centered and scales with the screen height, so its corner is placed from the height.
bool g_endShownLogged = false;
void drawEndBadge() {
    if (!g_endScreen || !g_labels[L_CHEAT].on) { g_endShownLogged = g_endScreen; return; }
    if (!g_endShownLogged) { g_endShownLogged = true; logf("level complete panel found: cheat badge shown"); }
    auto dl = ImGui::GetForegroundDrawList();
    ImVec2 d = ImGui::GetIO().DisplaySize;
    float k = d.y;
    float a = std::clamp(g_labels[L_CHEAT].opacity * g_st.opacity, 0.f, 1.f);
    int cs = cheatShown();
    ImVec4 c = cs == 1 ? ImVec4(1.0f, 0.27f, 0.30f, a)
             : cs == 2 ? ImVec4(0.80f, 0.36f, 0.04f, a)
                       : ImVec4(0.30f, 1.0f, 0.42f, a);
    ImVec2 cc(d.x * 0.5f - 0.528f * k, 0.170f * k);
    float r = 0.0085f * k;
    dl->AddCircleFilled(cc, r + 1.5f, IM_COL32(0, 0, 0, static_cast<int>(170 * a)));
    dl->AddCircleFilled(cc, r, ImGui::ColorConvertFloat4ToU32(c));
    char b[32];
    std::snprintf(b, sizeof(b), "v%s", KMENU_VERSION);
    float size = 0.024f * k;
    ImVec2 ts = g_fontLabel->CalcTextSizeA(size, FLT_MAX, 0, b);
    dl->AddText(g_fontLabel, size, ImVec2(cc.x + r * 2.2f, cc.y - ts.y * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(115 * a)), b);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void openPath(const std::wstring& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// accent colors are kept readable: never so light that white text disappears, never so dark it looks off
ImVec4 safeAccent(ImVec4 c) {
    float h, sat, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, sat, v);
    v = std::clamp(v, 0.42f, 1.0f);
    ImVec4 o(0, 0, 0, 1);
    ImGui::ColorConvertHSVtoRGB(h, sat, v, o.x, o.y, o.z);
    float lum = 0.2126f * o.x + 0.7152f * o.y + 0.0722f * o.z;
    const float maxLum = 0.58f;
    if (lum > maxLum) {
        // pull toward a darker, more saturated version of the same hue
        float k = maxLum / lum;
        o = ImVec4(o.x * k, o.y * k, o.z * k, 1);
    }
    return o;
}

void setAccent(ImVec4 c) {
    g_accent = safeAccent(c);
    cfgSetStr("accent", std::to_string(g_accent.x) + "," + std::to_string(g_accent.y) + "," + std::to_string(g_accent.z));
    g_lastScale = -1; // re-apply the style colors
}

void popupTitle(const char* t);

extern bool g_modsOpen;
int modsActiveCount();
void drawKuangColumn(int index) {
    if (!beginColumn("kuang", "cat.kuang", index)) { endColumn(); return; }
    {
        char v[64];
        std::snprintf(v, sizeof(v), "v%s  \xC2\xB7  GD 2.2091", KMENU_VERSION);
        caption(v);
    }
    {
        char mb[64];
        std::snprintf(mb, sizeof(mb), "%s  (%d/4)", T("mods.btn"), modsActiveCount());
        if (buttonRow("mods", mb, T("mods.d"))) g_modsOpen = !g_modsOpen;
    }
    // search
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", g_search.c_str());
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
        gradRect(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + h), U(ImVec4(0.20f, 0.205f, 0.235f, 1)),
            U(ImVec4(0.13f, 0.135f, 0.155f, 1)), S(6));
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        if (ImGui::InputTextWithHint("##search", T("set.search"), buf, sizeof(buf))) g_search = buf;
        ImGui::PopStyleColor();
    }
    // language
    ImGui::Dummy(ImVec2(0, S(2)));
    caption(T("set.language"));
    const char* langs[] = {"English", "Espa\xC3\xB1ol"};
    float half = (ImGui::GetContentRegionAvail().x - S(4)) * 0.5f;
    for (int i = 0; i < 2; i++) {
        if (i) ImGui::SameLine(0, S(4));
        ImGui::PushID(i);
        if (pillButton("##lang", langs[i], ImVec2(half, S(24)), g_lang == i)) { g_lang = i; cfgSetInt("lang", i); }
        ImGui::PopID();
    }
    // accent color: presets + custom (opens beside the column)
    ImGui::Dummy(ImVec2(0, S(2)));
    caption(T("set.accent"));
    float sw = (ImGui::GetContentRegionAvail().x - S(4) * 6) / 7.0f;
    auto dl = ImGui::GetWindowDrawList();
    bool openColor = false;
    for (int i = 0; i < 7; i++) {
        if (i) ImGui::SameLine(0, S(4));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        bool custom = i == 6;
        ImVec4 c = custom ? g_accent : kPresets[i].color;
        if (ImGui::InvisibleButton("##preset", ImVec2(sw, sw))) {
            if (custom) openColor = true;
            else setAccent(c);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", custom ? T("set.custom") : kPresets[i].name);
        ImGui::PopID();
        if (custom) {
            // rainbow square = "pick your own"
            ImU32 cols[4] = {IM_COL32(255, 80, 120, 255), IM_COL32(255, 200, 60, 255), IM_COL32(60, 200, 255, 255), IM_COL32(150, 90, 255, 255)};
            dl->AddRectFilledMultiColor(p, ImVec2(p.x + sw, p.y + sw), cols[0], cols[1], cols[2], cols[3]);
            dl->AddRect(p, ImVec2(p.x + sw, p.y + sw), U(ImVec4(0, 0, 0, 0.35f)), 0, 0, S(1));
            continue;
        }
        gradRect(dl, p, ImVec2(p.x + sw, p.y + sw), U(c), U(hueShift(c, 38)), S(3), true);
        bool cur = std::fabs(g_accent.x - c.x) < 0.01f && std::fabs(g_accent.y - c.y) < 0.01f && std::fabs(g_accent.z - c.z) < 0.01f;
        if (cur) dl->AddRect(ImVec2(p.x - S(1.5f), p.y - S(1.5f)), ImVec2(p.x + sw + S(1.5f), p.y + sw + S(1.5f)), U(ImVec4(1, 1, 1, 1)), S(4), 0, S(2));
    }
    if (openColor) ImGui::OpenPopup("pop.color"); // same ID scope as BeginPopup below
    placePopup("pop.color");
    if (ImGui::BeginPopup("pop.color")) {
        popupTitle(T("set.custom"));
        float col3[3] = {g_accent.x, g_accent.y, g_accent.z};
        ImGui::SetNextItemWidth(S(200));
        if (ImGui::ColorPicker3("##picker", col3, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoLabel)) {
            setAccent(ImVec4(col3[0], col3[1], col3[2], 1));
        }
        hintText(T("set.custom.d"));
        notePopupSize("pop.color");
        ImGui::EndPopup();
    }
    // menu key
    ImGui::Dummy(ImVec2(0, S(2)));
    caption(T("set.key"));
    std::string kn = g_waitingKey ? T("set.press") : keyName(g_menuKey);
    if (pillButton("##key", kn.c_str(), ImVec2(-1, S(24)), g_waitingKey)) g_waitingKey = true;
    // size / width
    caption(T("set.scale"));
    float sc = g_scale;
    ImGui::SetNextItemWidth(-1);
    if (gradSlider("##scale", &sc, 0.6f, 2.0f, "%.2fx")) { g_scale = sc; cfgSetFloat("ui.scale", sc); }
    caption(T("set.width"));
    ImGui::SetNextItemWidth(-1);
    if (gradSlider("##width", &g_colW, 170.0f, 320.0f, "%.0f")) cfgSetFloat("ui.width", g_colW);
    ImGui::Dummy(ImVec2(0, S(2)));
    if (toggleRow("anim", T("set.anim"), T("set.anim.d"), &g_anim)) cfgSetBool("ui.anim", g_anim);
    if (toggleRow("dim", T("set.dim"), T("set.dim.d"), &g_dim)) cfgSetBool("ui.dim", g_dim);
    if (buttonRow("reset", T("set.reset"), T("set.reset.d"))) {
        g_scale = 1.0f; g_colW = 212.0f;
        cfgSetFloat("ui.scale", g_scale); cfgSetFloat("ui.width", g_colW);
        for (auto it = g_cfg.begin(); it != g_cfg.end();) it = it->first.rfind("pos.", 0) == 0 ? g_cfg.erase(it) : std::next(it);
        cfgTouch();
        g_resetLayout = true;
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    hintText(T("set.hint"));
    endColumn();
}

void popupTitle(const char* t) {
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
    ImGui::Separator();
}

bool popupCheck(const char* label, bool* v) {
    ImGui::PushStyleColor(ImGuiCol_CheckMark, g_accent);
    bool r = ImGui::Checkbox(label, v);
    ImGui::PopStyleColor();
    return r;
}

// put a label in a corner: drawStatus() stacks it under (or above) the other labels of that corner every frame
void snapLabel(int i, int corner) {
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float margin = S(10);
    Label& l = g_labels[i];
    l.right = corner == 1 || corner == 3;
    l.x = l.right ? (disp.x - margin) / disp.x : margin / disp.x;
    l.y = corner >= 2 ? (disp.y - margin) / disp.y : margin / disp.y;
    l.corner = corner;
    saveLabel(l);
}

void drawStatusColumn(int index) {
    bool body = beginColumn("status", "cat.status", index);
    if (body) {
        for (int i = 0; i < kLabelCount; i++) {
            Label& l = g_labels[i];
            std::string pop = std::string("pop.") + l.id;
            if (toggleRow(l.id, T(l.key), T(l.desc), &l.on, pop.c_str())) saveLabel(l);
            placePopup(pop.c_str());
            if (ImGui::BeginPopup(pop.c_str())) {
                popupTitle(T(l.key));
                if (i == L_MESSAGE) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", g_st.text.c_str());
                    ImGui::TextUnformatted(T("st.text"));
                    ImGui::SetNextItemWidth(S(220));
                    if (ImGui::InputText("##msg", buf, sizeof(buf))) { g_st.text = buf; cfgSetStr("st.text", g_st.text); }
                }
                if (i == L_CPS) {
                    if (popupCheck(T("cps.green"), &g_st.cpsGreen)) cfgSetBool("cps.green", g_st.cpsGreen);
                    if (popupCheck(T("cps.max"), &g_st.cpsMax)) cfgSetBool("cps.max", g_st.cpsMax);
                    if (popupCheck(T("cps.unit"), &g_st.cpsUnit)) cfgSetBool("cps.unit", g_st.cpsUnit);
                    if (popupCheck(T("cps.total"), &g_st.cpsTotal)) cfgSetBool("cps.total", g_st.cpsTotal);
                    if (ImGui::Button(T("cps.reset"))) { g_cpsMax = 0; g_clickTotal = 0; g_clicks.clear(); }
                    ImGui::Separator();
                }
                ImGui::SetNextItemWidth(S(170));
                if (ImGui::InputFloat(T("st.size"), &l.scale, 0.1f, 0.5f, "%.2f")) { l.scale = std::clamp(l.scale, 0.1f, 20.0f); saveLabel(l); }
                ImGui::SetNextItemWidth(S(170));
                if (gradSlider(T("st.opacity"), &l.opacity, 0.05f, 1.0f, "%.2f")) saveLabel(l);
                caption(T("st.corner"));
                const char* cn[] = {T("st.tl"), T("st.tr"), T("st.bl"), T("st.br")};
                for (int c = 0; c < 4; c++) {
                    if (c % 2) ImGui::SameLine(0, S(4));
                    ImGui::PushID(c);
                    if (pillButton("##corner", cn[c], ImVec2(S(128), S(24)), l.corner == c)) snapLabel(i, c);
                    ImGui::PopID();
                }
                ImGui::PushTextWrapPos(S(260));
                hintText(T("st.drag"));
                ImGui::PopTextWrapPos();
                notePopupSize(pop.c_str());
                ImGui::EndPopup();
            }
        }
        if (toggleRow("always", T("st.always"), T("st.always.d"), &g_st.always, "pop.all")) cfgSetBool("st.always", g_st.always);
        placePopup("pop.all");
        if (ImGui::BeginPopup("pop.all")) {
            popupTitle(T("st.all"));
            ImGui::SetNextItemWidth(S(170));
            if (gradSlider(T("st.size"), &g_st.size, 0.3f, 3.0f, "%.2fx")) cfgSetFloat("st.size", g_st.size);
            ImGui::SetNextItemWidth(S(170));
            if (gradSlider(T("st.opacity"), &g_st.opacity, 0.05f, 1.0f, "%.2f")) cfgSetFloat("st.opacity", g_st.opacity);
            if (ImGui::Button(T("st.reset"))) {
                for (auto& l : g_labels) { l.x = l.y = -1; l.right = false; l.corner = -1; saveLabel(l); }
            }
            ImGui::PushTextWrapPos(S(260));
            hintText(T("st.drag"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.all");
            ImGui::EndPopup();
        }
    }
    endColumn();
}

// ---------------------------------------------------------------------------
// Cheats (phase 3, first block)
// ---------------------------------------------------------------------------
bool g_speedOn = false;
float g_speed = 1.0f;
constexpr float kSpeedMin = 0.0001f, kSpeedMax = 100.0f; // 0.0001x ... 100x
bool g_speedAudio = true;
bool g_safeManual = false;
bool g_hooksOk = false;      // the game functions matched this GD version

bool cheatsActive() { return g_noclip || (g_speedOn && std::fabs(g_speed - 1.0f) > 0.001f); }

// safe mode state for the current PlayLayer
void* g_safeLayer = nullptr;
bool g_safeOrigTest = false;
bool g_safeAttempt = false;
// 2026-10-20 patch: GJBaseGameLayer gained 0x40 bytes before m_player1, so every GJBaseGameLayer /
// PlayLayer field from 0xdb8 up moved +0x40 (checked in the new GeometryDash.exe). CCNode, GameObject,
// PlayerObject, LevelSettingsObject and GJGameLevel fields did not move.
constexpr uintptr_t kTestMode = 0x3328;     // GJBaseGameLayer::m_isTestMode
constexpr uintptr_t kAnticheatSpike = 0x38e0; // GJBaseGameLayer::m_anticheatSpike

bool safeModeActive() { return g_safeManual || cheatsActive() || (g_safeLayer && g_safeAttempt); }

// Cheat Indicator color: 0 green = clean attempt, 1 red = a cheat is on right now,
// 2 dark orange = a cheat was used earlier in this attempt but it is off now.
// When the level is completed the result is frozen (g_cheatFinal) so the end screen keeps showing how you passed it.
int g_cheatFinal = -1;
int cheatLive() {
    if (g_safeManual || cheatsActive()) return 1;
    return (g_safeLayer && g_safeAttempt) ? 2 : 0;
}
int cheatShown() { return g_cheatFinal >= 0 ? g_cheatFinal : cheatLive(); }

void safeModeTick() {
    void* pl = g_playLayer;
    if (pl != g_safeLayer) {
        g_safeLayer = pl;
        g_safeAttempt = false;
        g_cheatFinal = -1;
        if (pl) g_safeOrigTest = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(pl) + kTestMode);
    }
    if (!pl) return;
    if (g_safeManual || cheatsActive()) g_safeAttempt = true;
    if (g_safeAttempt) *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(pl) + kTestMode) = true;
}

// ---------------------------------------------------------------------------
// Visual options (not cheats): hitboxes, accurate percentage, hide attempts, custom trail
// ---------------------------------------------------------------------------
bool g_accurate = false, g_hideAttempts = false;
bool g_pctFix = true;      // Percentage Bugfix (see pctFixTick)
constexpr uintptr_t kDebugDrawNode = 0x3870;  // GJBaseGameLayer::m_debugDrawNode
constexpr uintptr_t kAttemptLabel = 0x3a88;   // PlayLayer::m_attemptLabel
constexpr uintptr_t kDecimalPct = 0x3a98;     // PlayLayer::m_decimalPercentage
constexpr uintptr_t kPlayer1 = 0xdb8, kPlayer2 = 0xdc0;
extern bool g_traj, g_trajP1, g_trajP2, g_trajReady;
extern bool g_trajTrail[2];
extern float g_trajCol[2][2][3];
extern float g_trajLen;
bool g_trajSim = false, g_trajDead = false; // Show Trajectory is running its copy right now / the copy died
void* g_ghost[2] = {};                     // Show Trajectory's hidden copies of the player
void trajGhostDied(void* gl, void* ghost, void* object);
constexpr uintptr_t kRegularTrail = 0x7a8;    // PlayerObject::m_regularTrail (CCMotionStreak)
constexpr uintptr_t kStreakStroke = 0x92c;    // PlayerObject::m_streakStrokeWidth
constexpr uintptr_t kNodeVisible = 0x11e;     // CCNode::m_bVisible

template <class T> T& at(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

// remembers the game's own value so turning an option off gives it back
struct ByteOverride {
    void* layer = nullptr; bool orig = false; bool touched = false;
    void apply(void* obj, uintptr_t off, bool want, bool value) {
        if (obj != layer) { layer = obj; touched = false; }
        if (!obj) return;
        bool& b = at<bool>(obj, off);
        if (want) {
            if (!touched) { orig = b; touched = true; }
            b = value;
        } else if (touched) {
            b = orig;
            touched = false;
        }
    }
};
ByteOverride g_ovAccurate, g_ovAttempts;
void* g_attemptLabelSeen = nullptr;

// trail
bool g_trailOn = false, g_trailKeep = false, g_trailGlow = true;
bool g_trailShipOnly = true;        // the image only on the ship's trail (the game shows that trail mostly while flying)
float g_trailWidth = 1.0f;
std::string g_trailFile;            // file name inside the game folder (ASCII), e.g. kmenu-trail-3.png
volatile bool g_trailDirty = true;  // load the texture again
volatile bool g_trailPicking = false;
void* g_trailTex = nullptr;
struct BlendFunc { unsigned src, dst; };
struct Color3B { unsigned char r, g, b; };
void* (*g_texCache)() = nullptr;
void* (*g_addImage)(void*, const char*, bool) = nullptr;
void (*g_removeTexKey)(void*, const char*) = nullptr;
void* (*g_streakGetTex)(void*) = nullptr;
void (*g_streakSetTex)(void*, void*) = nullptr;
void (*g_streakSetStroke)(void*, float) = nullptr;
void (*g_streakSetBlend)(void*, BlendFunc) = nullptr;
BlendFunc* (*g_streakGetBlend)(void*, BlendFunc*) = nullptr; // MSVC member function: the struct comes back through a hidden pointer
// CCMotionStreak = CCNodeRGBA + CCTextureProtocol. The texture/blend functions expect `this` = the CCTextureProtocol part
// (+0x158: texture at +0x168, blend at +0x170, as CCMotionStreak::draw reads them) and setColor/getColor the CCRGBAProtocol
// part (+0x140). Calling them with the plain object pointer (as before 0.11.1) wrote into the wrong fields: the image never
// showed up and getBlendFunc crashed ("exception in visual options").
constexpr uintptr_t kStreakTexProto = 0x158, kStreakRgba = 0x140;
inline void* stTex(void* s) { return reinterpret_cast<char*>(s) + kStreakTexProto; }
inline void* stRgba(void* s) { return reinterpret_cast<char*>(s) + kStreakRgba; }
void (*g_nodeSetColor)(void*, const Color3B*) = nullptr;
const Color3B* (*g_nodeGetColor)(void*) = nullptr;
void (*g_drawNodeClear)(void*) = nullptr;
struct StreakOrig { void* tex; BlendFunc blend; Color3B color; };
std::map<void*, StreakOrig> g_streakOrig;
void* g_trailLayer = nullptr;
std::string g_loadedTrailKey;

std::string narrow(const std::wstring& w) {
    char b[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

std::wstring g_pickedPath;
std::atomic<bool> g_pickedReady{false};

DWORD WINAPI pickTrailThread(LPVOID) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        g_pickedPath = file;
        g_pickedReady = true;
    }
    g_trailPicking = false;
    return 0;
}

// main thread: copy the picked image into the game folder with a new name (the texture cache keys by name)
void applyPickedTrail() {
    if (!g_pickedReady) return;
    g_pickedReady = false;
    std::wstring src = g_pickedPath;
    size_t dot = src.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L".png" : src.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    int n = cfgInt("tr.n", 0) + 1;
    std::wstring name = L"kmenu-trail-" + std::to_wstring(n) + ext;
    if (!CopyFileW(src.c_str(), (g_dir + name).c_str(), FALSE)) { logf("could not copy the trail image"); return; }
    if (!g_trailFile.empty()) {
        std::wstring old(g_trailFile.begin(), g_trailFile.end());
        DeleteFileW((g_dir + old).c_str());
    }
    g_trailFile = std::string(name.begin(), name.end());
    cfgSetInt("tr.n", n);
    cfgSetStr("tr.file", g_trailFile);
    g_trailOn = true;
    cfgSetBool("tr.on", true);
    g_trailDirty = true;
}

void restoreStreaks() {
    for (auto& [streak, o] : g_streakOrig) {
        if (g_streakSetTex && o.tex) g_streakSetTex(stTex(streak), o.tex);
        if (g_streakSetBlend) g_streakSetBlend(stTex(streak), o.blend);
        if (g_nodeSetColor) g_nodeSetColor(stRgba(streak), &o.color);
    }
    g_streakOrig.clear();
}

void trailTick() {
    void* gl = g_gameLayer;
    if (gl != g_trailLayer) { g_streakOrig.clear(); g_trailLayer = gl; } // old streaks died with the level
    bool on = g_trailOn && !g_trailFile.empty() && g_addImage && g_streakSetTex;
    if (on && (g_trailDirty || !g_trailTex)) {
        void* cache = g_texCache();
        if (!g_loadedTrailKey.empty() && g_removeTexKey) g_removeTexKey(cache, g_loadedTrailKey.c_str());
        std::string full = narrow(g_dir) + g_trailFile;
        g_trailTex = (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES) ? g_addImage(cache, full.c_str(), false) : nullptr;
        g_loadedTrailKey = full;
        g_trailDirty = false;
        if (!g_trailTex) logf("custom trail: could not load the image %s (missing file or not a PNG/JPG the game can read; pick it again)", full.c_str());
        else logf("custom trail: image loaded (%s)", g_trailFile.c_str());
    }
    if (!gl) return;
    for (uintptr_t po : {kPlayer1, kPlayer2}) {
        void* player = at<void*>(gl, po);
        if (!player) continue;
        void* streak = at<void*>(player, kRegularTrail);
        if (!streak) continue;
        float base = at<float>(player, kStreakStroke);
        bool ship = at<bool>(player, 0x9c1); // PlayerObject::m_isShip
        if (on && g_trailTex && (ship || !g_trailShipOnly)) {
            if (!g_streakOrig.count(streak)) {
                static int logged = 0;
                if (logged++ < 3) logf("custom trail: on the %s trail of player %d", ship ? "ship" : "vehicle", po == kPlayer1 ? 1 : 2);
            }
            if (!g_streakOrig.count(streak)) {
                const char* cls = rttiName(streak);
                if (!cls || std::strcmp(cls, ".?AVCCMotionStreak@cocos2d@@") != 0) continue; // not the object we expect
                StreakOrig o{};
                o.tex = g_streakGetTex ? g_streakGetTex(stTex(streak)) : nullptr;
                if (g_streakGetBlend) g_streakGetBlend(stTex(streak), &o.blend);
                if (g_nodeGetColor) o.color = *g_nodeGetColor(stRgba(streak));
                g_streakOrig[streak] = o;
            }
            if (!g_streakGetTex || g_streakGetTex(stTex(streak)) != g_trailTex) g_streakSetTex(stTex(streak), g_trailTex);
            if (g_streakSetStroke && base > 0) g_streakSetStroke(streak, base * g_trailWidth);
            if (g_streakSetBlend) g_streakSetBlend(stTex(streak), g_trailGlow ? BlendFunc{0x302, 1} : BlendFunc{0x302, 0x303});
            if (g_trailKeep && g_nodeSetColor) { Color3B w{255, 255, 255}; g_nodeSetColor(stRgba(streak), &w); }
        } else if (g_streakOrig.count(streak)) {
            StreakOrig o = g_streakOrig[streak];
            if (o.tex) g_streakSetTex(stTex(streak), o.tex);
            if (g_streakSetStroke && base > 0) g_streakSetStroke(streak, base);
            if (g_streakSetBlend) g_streakSetBlend(stTex(streak), o.blend);
            if (g_nodeSetColor) g_nodeSetColor(stRgba(streak), &o.color);
            g_streakOrig.erase(streak);
        }
    }
}

void visualTick() {
    applyPickedTrail();
    void* pl = g_playLayer;
    g_ovAccurate.apply(pl, kDecimalPct, g_accurate, true);
    if (pl) {
        void* label = at<void*>(pl, kAttemptLabel);
        if (label != g_attemptLabelSeen) { g_attemptLabelSeen = label; g_ovAttempts = ByteOverride(); }
        g_ovAttempts.apply(label, kNodeVisible, g_hideAttempts, false);
    } else {
        g_attemptLabelSeen = nullptr;
        g_ovAttempts = ByteOverride();
    }
    trailTick();
}

void visualTickSEH() {
    __try {
        visualTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) { logf("exception in visual options (%08lx)", GetExceptionCode()); logged = true; }
    }
}

// ---------------------------------------------------------------------------
// The game's own functions we call (all checked against GD 2.2091 before use)
// ---------------------------------------------------------------------------
uintptr_t g_base = 0;
void* (*g_gmShared)() = nullptr;                        // GameManager::sharedState
bool (*g_gmGetVar)(void*, const char*) = nullptr;       // GameManager::getGameVariable
void (*g_gmSetVar)(void*, const char*, bool) = nullptr; // GameManager::setGameVariable
void (*g_updateDebugDrawSettings)(void*) = nullptr;     // PlayLayer::updateDebugDrawSettings
void (*g_toggleDebugDraw)(void*) = nullptr;             // PlayLayer: shows/hides the hitbox node as the game decides
void (*g_resetLevelFromStart)(void*) = nullptr;         // PlayLayer::resetLevelFromStart
void (*g_updateCamera)(void*, float) = nullptr;         // GJBaseGameLayer::updateCamera
void (*g_updateTestModeLabel)(void*) = nullptr;         // PlayLayer::updateTestModeLabel
void (*g_startMusic)(void*) = nullptr;                  // PlayLayer::startMusic
void (*g_objRetain)(void*) = nullptr;                   // CCObject::retain
void (*g_objRelease)(void*) = nullptr;                  // CCObject::release
// Utility actions
void (*g_handleButton)(void*, bool, int, bool) = nullptr;   // GJBaseGameLayer::handleButton(down, button, player1)
void (*g_togglePractice)(void*, bool) = nullptr;            // PlayLayer::togglePracticeMode
void (*g_openOptions)(void*, bool) = nullptr;               // creates the game's OptionsLayer on a node and shows it
void* (*g_gsmShared)() = nullptr;                           // GameStatsManager::sharedState
int (*g_gsmGetStat)(void*, const char*) = nullptr;          // GameStatsManager::getStat
void (*g_gsmSetStat)(void*, const char*, int) = nullptr;    // GameStatsManager::setStat
int (*g_platChecksum)(int, int, int, int, int, int, int, int) = nullptr; // checksum of a platformer best
void* (*g_dictObject)(void*, const std::string&) = nullptr; // CCDictionary::objectForKey
void (*g_dictRemove)(void*, const std::string&) = nullptr;  // CCDictionary::removeObjectForKey

constexpr uintptr_t kStartPosObj = 0x3270;   // GJBaseGameLayer::m_startPosObject
constexpr uintptr_t kStartPosArr = 0x3278;   // GJBaseGameLayer::m_startPosObjects (CCArray)
constexpr uintptr_t kPractice = 0x32e0;      // GJBaseGameLayer::m_isPracticeMode
constexpr uintptr_t kLevelSettings = 0xdc8;  // GJBaseGameLayer::m_levelSettings
constexpr uintptr_t kObjects = 0xdd0;        // GJBaseGameLayer::m_objects (CCArray)
constexpr uintptr_t kClickBetween = 0x38f1;  // GJBaseGameLayer::m_clickBetweenSteps
constexpr uintptr_t kClickOn = 0x38f2;       // GJBaseGameLayer::m_clickOnSteps
constexpr uintptr_t kObjectID = 0x40c;       // GameObject::m_objectID
constexpr uintptr_t kObjX = 0x3b8;           // GameObject x (double, the one physics uses)
constexpr uintptr_t kStartSettings = 0x740;  // StartPosObject::m_startSettings (LevelSettingsObject)
// LevelSettingsObject
constexpr uintptr_t kLsMode = 0x148, kLsSpeed = 0x14c, kLsMini = 0x150, kLsDual = 0x151, kLsMirror = 0x152, kLsFlipped = 0x171;
// GJGameLevel
constexpr uintptr_t kLvlID = 0x150, kLvlName = 0x158, kLvlAudioTrack = 0x2a4, kLvlSongID = 0x2a8;

// cocos CCArray -> (count, items)
bool arrayItems(void* arr, unsigned& count, void**& items) {
    count = 0;
    items = nullptr;
    if (!arr) return false;
    void* data = at<void*>(arr, 0x38);
    if (!data) return false;
    count = at<unsigned>(data, 0);
    items = at<void**>(data, 0x10);
    return items != nullptr;
}
bool arrayItemsSEH(void* arr, unsigned& count, void**& items) {
    __try {
        return arrayItems(arr, count, items);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
        items = nullptr;
        return false;
    }
}

// ---- game options (the same switches as in the game's own settings)
struct GameVar { const char* key; const char* label; const char* desc; int kind; }; // kind 0 hitbox setting, 1 CBS, 2 COS
const GameVar kGameVars[] = {
    {"0166", "gv.hitbox", "gv.hitbox.d", 0},
    {"0213", "gv.trail", "gv.trail.d", 0},
    {"0179", "gv.death", "gv.death.d", 0},
    {"0214", "gv.deathtrail", "gv.deathtrail.d", 0},
    {"0217", "gv.highlight", "gv.highlight.d", 0},
    {"0171", "gv.noplayer", "gv.noplayer.d", 0},
    {"0177", "gv.cbs", "gv.cbs.d", 1},
    {"0176", "gv.cos", "gv.cos.d", 2},
};
const GameVar kFastMenu = {"0168", "gl.noanim", "gl.noanim.d", 3}; // the game's "Fast Menu" switch

bool gameVarSEH(const char* key, bool* out) {
    __try {
        void* gm = g_gmShared ? g_gmShared() : nullptr;
        if (!gm || !g_gmGetVar) return false;
        *out = g_gmGetVar(gm, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void setGameVarSEH(const GameVar& v, bool value) {
    __try {
        void* gm = g_gmShared ? g_gmShared() : nullptr;
        if (!gm || !g_gmSetVar) return;
        g_gmSetVar(gm, v.key, value);
        // apply it to the level that is open right now, too
        if (v.kind == 0 && g_playLayer && g_updateDebugDrawSettings) g_updateDebugDrawSettings(g_playLayer);
        if (v.kind == 1 && g_gameLayer) at<bool>(g_gameLayer, kClickBetween) = value;
        if (v.kind == 2 && g_gameLayer) at<bool>(g_gameLayer, kClickOn) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// a row for a game option: shows the game's current value, clicking flips it in the game
void gameVarRow(const GameVar& v) {
    bool cur = false;
    if (!gameVarSEH(v.key, &cur)) return;
    bool val = cur;
    if (toggleRow(v.key, T(v.label), T(v.desc), &val)) setGameVarSEH(v, val);
}

// ---- Start Pos switcher (uses the game's own start positions, like the pause menu arrows)
std::string g_toast;
double g_toastUntil = 0;

void showToast(const std::string& s) { g_toast = s; g_toastUntil = now() + 1.6; }

extern bool g_smartSP;            // Smart Start Pos (below)
extern const char* g_spLastSource;

void switchStartPos(int dir) {
    void* pl = g_playLayer;
    if (!pl || !g_resetLevelFromStart || !g_objRetain || !g_objRelease) return;
    unsigned n = 0;
    void** items = nullptr;
    arrayItems(at<void*>(pl, kStartPosArr), n, items);
    if (n == 0) { showToast(T("sp.none")); return; }
    void* cur = at<void*>(pl, kStartPosObj);
    int idx = -1;
    for (unsigned i = 0; i < n; i++) if (items[i] == cur) idx = static_cast<int>(i);
    int next = idx + dir;
    if (next < -1) next = static_cast<int>(n) - 1;
    if (next >= static_cast<int>(n)) next = -1;
    void* sp = next < 0 ? nullptr : items[next];
    // same steps the game does when you pick another start pos in the pause menu
    if (sp) g_objRetain(sp);
    if (cur) g_objRelease(cur);
    at<void*>(pl, kStartPosObj) = sp;
    at<bool>(pl, kTestMode) = sp != nullptr;
    g_safeOrigTest = sp != nullptr;
    g_resetLevelFromStart(pl);
    auto vt = *reinterpret_cast<void***>(pl);
    reinterpret_cast<void (*)(void*, float)>(vt[0x430 / 8])(pl, 0.0f);
    if (g_updateCamera) g_updateCamera(pl, 0.0f);
    if (g_updateTestModeLabel) g_updateTestModeLabel(pl);
    if (g_startMusic) g_startMusic(pl);
    char b[96];
    if (next < 0) std::snprintf(b, sizeof(b), "%s", T("sp.start"));
    else if (g_smartSP && g_spLastSource)
        std::snprintf(b, sizeof(b), "Start Pos %d / %u  (%s)", next + 1, n, T(std::strcmp(g_spLastSource, "exact") == 0 ? "sp.exact" : "sp.portals"));
    else std::snprintf(b, sizeof(b), "Start Pos %d / %u", next + 1, n);
    showToast(b);
}

void switchStartPosSEH(int dir) {
    __try {
        switchStartPos(dir);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("start pos switch failed (%08lx)", GetExceptionCode());
    }
}

// ---- Smart Start Pos: each start pos gets the mode, speed, size, gravity, dual and mirror the player
// really has when reaching it from the beginning.
//  1) Learned (exact): every time you pass a start pos while playing from further back, the menu writes
//     down the state the player actually has there (read from the player itself, so orbs, pads, gravity
//     portals, triggers and anything else are already included). Saved in kmenu-smartsp.txt per level.
//  2) Until a start pos has been learned, it is worked out from the portals before it (as before, plus
//     a few portal types it did not know).
bool g_smartSP = false;
bool g_smartOrbs = true;          // also count gravity orbs and pads before the start pos
constexpr uintptr_t kNodeRotation = 0x38; // CCNode::m_fRotationX
void* g_smartLayer = nullptr;
struct LsBackup { int mode, speed; bool mini, dual, mirror, flipped; };
std::map<void*, LsBackup> g_smartBackup;

// PlayerObject fields (2026-10-20 patch, read from toggleFlyMode / toggleBirdMode / ... / flipGravity /
// togglePlayerScale / updateTimeMod in GeometryDash.exe)
constexpr uintptr_t kPoShip = 0x9c1, kPoUfo = 0x9c2, kPoBall = 0x9c3, kPoWave = 0x9c4, kPoRobot = 0x9c5,
    kPoSpider = 0x9c6, kPoUpsideDown = 0x9c7, kPoSwing = 0x9cc;
constexpr uintptr_t kPoSize = 0x9f8;   // float: 1.0 normal, 0.6 mini
constexpr uintptr_t kPoSpeed = 0x9fc;  // float: 0.7 / 0.9 / 1.1 / 1.3 / 1.6 for 0.5x / 1x / 2x / 3x / 4x
constexpr uintptr_t kNodePos = 0x4c;   // CCNode::m_obPosition (the player's node, in level units)
constexpr uintptr_t kObjY = 0x3c0;     // GameObject y (double)

struct SpState { int mode = 0, speed = 0; bool mini = false, dual = false, flipped = false; };
struct SpKey {
    uint64_t level; int x, y; // start pos position in tenths of a unit
    bool operator<(const SpKey& o) const { return std::tie(level, x, y) < std::tie(o.level, o.x, o.y); }
};
std::map<SpKey, SpState> g_spLearned;
bool g_spLearnedLoaded = false, g_spLearnedDirty = false;
double g_spLearnedSaveAt = 0;
uint64_t g_spLevelFp = 0;         // fingerprint of the level being played (objects + start positions)
void* g_spFpLayer = nullptr;
float g_spLastX = NAN;            // player 1 x on the previous frame (NAN right after a reset)
int g_spLearnedCount = 0;         // learned start positions of the current level (shown in the menu)
const char* g_spLastSource = nullptr; // "exact" / "portals" for the start pos you are playing from
// Only learn from a run whose starting state is known to be right: from the beginning of the level, or
// from a start pos that was itself learned. Starting from a guessed start pos could teach a wrong state.
bool g_spRunTrusted = false;

std::wstring spFile() { return g_dir + L"kmenu-smartsp.txt"; }

void spLoad() {
    g_spLearnedLoaded = true;
    FILE* f = _wfopen(spFile().c_str(), L"rb");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long lv;
        int x, y, mode, speed, mini, dual, flip;
        if (std::sscanf(line, "%llx|%d|%d|%d|%d|%d|%d|%d", &lv, &x, &y, &mode, &speed, &mini, &dual, &flip) == 8)
            g_spLearned[{lv, x, y}] = {mode, speed, mini != 0, dual != 0, flip != 0};
    }
    std::fclose(f);
}

void spSave() {
    g_spLearnedDirty = false;
    FILE* f = _wfopen(spFile().c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# Kuang Menu - Smart Start Pos learned states (level|x|y|mode|speed|mini|dual|upside down)\n");
    for (auto& [k, s] : g_spLearned)
        std::fprintf(f, "%llx|%d|%d|%d|%d|%d|%d|%d\n", static_cast<unsigned long long>(k.level), k.x, k.y, s.mode, s.speed,
            s.mini, s.dual, s.flipped);
    std::fclose(f);
}

// same level = same objects: count, ids and positions folded together (FNV-1a). Editing the level
// changes it, so old learned states are not used on a different layout.
uint64_t levelFingerprint(void* gl) {
    unsigned n = 0;
    void** objs = nullptr;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    if (!arrayItems(at<void*>(gl, kObjects), n, objs)) return 0;
    mix(n);
    for (unsigned i = 0; i < n; i++) {
        void* o = objs[i];
        if (!o) continue;
        mix(static_cast<uint64_t>(at<int>(o, kObjectID)));
        mix(static_cast<uint64_t>(static_cast<int64_t>(at<double>(o, kObjX) * 10.0)));
        mix(static_cast<uint64_t>(static_cast<int64_t>(at<double>(o, kObjY) * 10.0)));
    }
    return h;
}

SpKey spKeyOf(void* sp) {
    return {g_spLevelFp, static_cast<int>(std::lround(at<double>(sp, kObjX) * 10.0)), static_cast<int>(std::lround(at<double>(sp, kObjY) * 10.0))};
}

int speedIndex(float v) {
    struct { float v; int idx; } table[] = {{0.7f, 1}, {0.9f, 0}, {1.1f, 2}, {1.3f, 3}, {1.6f, 4}};
    for (auto& t : table) if (std::fabs(v - t.v) < 0.02f) return t.idx;
    return -1;
}

bool readPlayerState(void* gl, SpState& s) {
    void* p1 = at<void*>(gl, kPlayer1);
    if (!p1) return false;
    int sp = speedIndex(at<float>(p1, kPoSpeed));
    if (sp < 0) return false; // speed changed by something we do not know: do not learn a wrong value
    s.mode = at<bool>(p1, kPoShip) ? 1 : at<bool>(p1, kPoBall) ? 2 : at<bool>(p1, kPoUfo) ? 3 : at<bool>(p1, kPoWave) ? 4
        : at<bool>(p1, kPoRobot) ? 5 : at<bool>(p1, kPoSpider) ? 6 : at<bool>(p1, kPoSwing) ? 7 : 0;
    s.speed = sp;
    s.mini = at<float>(p1, kPoSize) < 0.8f;
    s.flipped = at<bool>(p1, kPoUpsideDown);
    void* p2 = at<void*>(gl, kPlayer2);
    s.dual = p2 && p2 != p1 && at<bool>(p2, kNodeVisible); // the second icon is only visible in dual mode
    return true;
}

// Dual can only be real at a start pos if the level starts in dual or has a "enter dual" portal (286) at/before it.
// Without this check a learned/guessed "dual" (second icon visible for some other reason) was applied to levels with no dual at all.
bool dualPossibleAt(void* gl, double sx) {
    void* level = at<void*>(gl, kLevelSettings);
    if (level && at<bool>(level, kLsDual)) return true;
    unsigned n = 0;
    void** objs = nullptr;
    if (!arrayItems(at<void*>(gl, kObjects), n, objs)) return false;
    for (unsigned i = 0; i < n; i++) {
        void* o = objs[i];
        if (o && at<int>(o, kObjectID) == 286 && at<double>(o, kObjX) - 16.5 < sx + 15.0) return true;
    }
    return false;
}

int learnedCountForLevel();

// every frame in a level: when player 1 goes past a start pos, write down its real state there
void smartLearnTick(void* gl) {
    if (!g_spLearnedLoaded) spLoad();
    if (g_spLearnedDirty && now() > g_spLearnedSaveAt) spSave();
    if (!gl) { g_spLastX = NAN; return; }
    if (gl != g_spFpLayer) { g_spFpLayer = gl; g_spLevelFp = levelFingerprint(gl); g_spLastX = NAN; }
    void* p1 = at<void*>(gl, kPlayer1);
    if (!p1) return;
    float x = at<float>(p1, kNodePos);
    float last = g_spLastX;
    g_spLastX = x;
    // only a normal step forward (not the first frame after a reset, a teleport or going backwards)
    if (!g_spRunTrusted || std::isnan(last) || x <= last || x - last > 60.0f) return;
    unsigned n = 0;
    void** sps = nullptr;
    if (!arrayItems(at<void*>(gl, kStartPosArr), n, sps)) return;
    for (unsigned i = 0; i < n; i++) {
        void* sp = sps[i];
        if (!sp) continue;
        double sx = at<double>(sp, kObjX);
        if (!(sx > last && sx <= x)) continue;
        SpState s;
        if (!readPlayerState(gl, s)) continue;
        if (s.dual && !dualPossibleAt(gl, sx)) s.dual = false; // second icon visible for another reason: not a real dual
        SpKey k = spKeyOf(sp);
        auto it = g_spLearned.find(k);
        bool changed = it == g_spLearned.end() || it->second.mode != s.mode || it->second.speed != s.speed ||
            it->second.mini != s.mini || it->second.dual != s.dual || it->second.flipped != s.flipped;
        if (changed) {
            g_spLearned[k] = s;
            g_spLearnedDirty = true;
            g_spLearnedSaveAt = now() + 2.0; // written a moment later, never in the middle of a run
            g_spLearnedCount = learnedCountForLevel();
            logf("smart start pos: learned x=%.0f (mode %d, speed %d, mini %d, dual %d, upside down %d)", sx, s.mode, s.speed,
                s.mini, s.dual, s.flipped);
        }
    }
}

void smartLearnTickSEH(void* gl) {
    __try {
        smartLearnTick(gl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) { logf("smart start pos learning failed (%08lx)", GetExceptionCode()); logged = true; }
    }
}

int learnedCountForLevel() {
    int c = 0;
    for (auto& [k, s] : g_spLearned) if (k.level == g_spLevelFp) c++;
    return c;
}

void forgetLearnedForLevel() {
    for (auto it = g_spLearned.begin(); it != g_spLearned.end();)
        it = it->first.level == g_spLevelFp ? g_spLearned.erase(it) : std::next(it);
    spSave();
}

void applySmartStartPos(void* gl) {
    unsigned nObj = 0, nSp = 0;
    void** objs = nullptr;
    void** sps = nullptr;
    if (!arrayItems(at<void*>(gl, kObjects), nObj, objs) || !arrayItems(at<void*>(gl, kStartPosArr), nSp, sps) || nSp == 0) return;
    if (!g_spLearnedLoaded) spLoad();
    if (gl != g_spFpLayer) { g_spFpLayer = gl; g_spLevelFp = levelFingerprint(gl); }
    // every object that changes the player's state, with its position
    // pads only fire when they face the player: a floor pad sends you to the ceiling (upside down), a ceiling pad
    // (rotated 180 or flipped vertically) back to the floor. pad: 0 = not a pad, 1 = floor, 2 = ceiling, 3 = wall (ignored)
    struct Portal { double x, y; int id; int family; float halfW; float rot; int pad; };
    std::vector<Portal> portals;
    static bool (*isFlipY)(void*) = nullptr;
    static bool flipLooked = false;
    if (!flipLooked) {
        flipLooked = true;
        if (HMODULE cc = GetModuleHandleW(L"libcocos2d.dll"))
            isFlipY = reinterpret_cast<bool (*)(void*)>(GetProcAddress(cc, "?isFlipY@CCSprite@cocos2d@@QEAA_NXZ"));
    }
    for (unsigned i = 0; i < nObj; i++) {
        void* o = objs[i];
        if (!o) continue;
        int id = at<int>(o, kObjectID);
        int family = -1;
        float halfW = 15.0f; // half width of the portal's hitbox (GD sizes)
        switch (id) {
        case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933: family = 0; halfW = 17.0f; break; // game modes
        case 200: case 201: case 202: case 203: case 1334: family = 1; halfW = 17.5f; break;                        // speeds
        case 10: case 11: case 2926: family = 2; halfW = 12.5f; break;                                              // gravity
        case 99: case 101: family = 3; halfW = 15.5f; break;                                                        // size
        case 286: case 287: family = 4; halfW = 16.5f; break;                                                       // dual
        case 45: case 46: family = 5; halfW = 16.5f; break;                                                         // mirror
        // gravity orbs and pads: blue orb, green orb, gravity (pink) dash orb, spider orb / blue pad, spider pad.
        // They share the gravity family, so an orb stacked over a portal is treated as an alternative path.
        // (1704 is the normal green dash orb: it does not change gravity, 1751 is the one that does.)
        case 84: case 1022: case 1751: case 3004: if (g_smartOrbs) { family = 2; halfW = 18.0f; } break;
        case 67: case 3005: if (g_smartOrbs) { family = 2; halfW = 12.5f; } break;
        default: break;
        }
        if (family < 0) continue;
        float rot = at<float>(o, kNodeRotation);
        int pad = 0;
        if (id == 67 || id == 3005) {
            float a = std::fmod(rot, 360.0f);
            if (a < 0) a += 360.0f;
            bool flipY = (isFlipY && isFlipY(o)) != (at<float>(o, 0x44) < 0); // CCSprite flip, or a negative Y scale
            if (a < 45.0f || a > 315.0f) pad = flipY ? 2 : 1;
            else if (std::fabs(a - 180.0f) < 45.0f) pad = flipY ? 1 : 2;
            else pad = 3; // on a wall (sideways gameplay): not used for gravity
        }
        portals.push_back({at<double>(o, kObjX), at<double>(o, kObjY), id, family, halfW, rot, pad});
    }
    std::sort(portals.begin(), portals.end(), [](const Portal& a, const Portal& b) { return a.x < b.x; });
    void* level = at<void*>(gl, kLevelSettings);
    void* current = at<void*>(gl, kStartPosObj);
    g_spLastSource = nullptr;
    for (unsigned s = 0; s < nSp; s++) {
        void* sp = sps[s];
        void* ls = sp ? at<void*>(sp, kStartSettings) : nullptr;
        if (!ls) continue;
        if (!g_smartBackup.count(ls))
            g_smartBackup[ls] = {at<int>(ls, kLsMode), at<int>(ls, kLsSpeed), at<bool>(ls, kLsMini), at<bool>(ls, kLsDual),
                at<bool>(ls, kLsMirror), at<bool>(ls, kLsFlipped)};
        // start from the level's own starting settings
        int mode = level ? at<int>(level, kLsMode) : 0, speed = level ? at<int>(level, kLsSpeed) : 0;
        bool mini = level && at<bool>(level, kLsMini), dual = level && at<bool>(level, kLsDual);
        bool mirror = level && at<bool>(level, kLsMirror), flipped = level && at<bool>(level, kLsFlipped);
        double sx = at<double>(sp, kObjX), sy = at<double>(sp, kObjY);
        bool explain = sp == current; // the start pos you play from: its gravity decisions go to kmenu.log
        std::string why;
        // A portal counts if the player (30 wide, centred on the start pos) already touches it or has passed
        // it: the game itself fires a portal that overlaps the start pos on the first frame anyway.
        // Portals of the same kind stacked in one column (two gravity portals one above the other, a fake
        // one as decoration...) are alternatives: only the one on the player's path is used. The path's height
        // at that x is estimated on the line from the last portal used to this start pos (before: the start pos
        // height for every column, which picked wrong portals far from the start pos).
        double refX = 0.0, refY = 105.0; // where the player starts a level (ground)
        size_t i = 0;
        while (i < portals.size()) {
            const Portal& first = portals[i];
            if (first.x - first.halfW >= sx + 15.0) break;
            size_t j = i + 1, pick = i;
            // same column = practically the same x (stacked alternatives); an orb followed by a portal a little
            // further on is a sequence the player goes through, not a choice
            auto pathY = [&](double x) {
                if (sx - refX < 1.0) return sy;
                double t = std::clamp((x - refX) / (sx - refX), 0.0, 1.0);
                return refY + (sy - refY) * t;
            };
            double py = pathY(first.x);
            while (j < portals.size() && portals[j].family == first.family && portals[j].x - first.x < 12.0 &&
                   portals[j].x - portals[j].halfW < sx + 15.0) {
                if (std::fabs(portals[j].y - py) < std::fabs(portals[pick].y - py)) pick = j;
                j++;
            }
            refX = portals[pick].x;
            refY = portals[pick].y;
            // other kinds in between end the column (they are applied on their own below)
            for (size_t k = i; k < j; k++) {
                const Portal& p = portals[k];
                if (p.family == first.family && k != pick) {
                    if (explain && p.family == 2) why += " skip " + std::to_string(p.id) + "@" + std::to_string(static_cast<int>(p.x)) + "," + std::to_string(static_cast<int>(p.y));
                    continue;
                }
                switch (p.id) {
                case 12: mode = 0; break;
                case 13: mode = 1; break;
                case 47: mode = 2; break;
                case 111: mode = 3; break;
                case 660: mode = 4; break;
                case 745: mode = 5; break;
                case 1331: mode = 6; break;
                case 1933: mode = 7; break;
                case 201: speed = 0; break;  // 1x
                case 200: speed = 1; break;  // 0.5x
                case 202: speed = 2; break;  // 2x
                case 203: speed = 3; break;  // 3x
                case 1334: speed = 4; break; // 4x
                case 10: flipped = false; break;
                case 11: flipped = true; break;
                case 2926: flipped = !flipped; break;
                // orbs flip the gravity (they need your click, so this is a guess; learned states are exact)
                case 84: case 1022: case 1751: case 3004: flipped = !flipped; break;
                // a pad decides the gravity by where it is: floor pad -> upside down, ceiling pad -> normal.
                // (A floor pad cannot be touched while you are already upside down, so this is never a wrong flip.)
                case 67: case 3005:
                    if (p.pad == 1) flipped = true;
                    else if (p.pad == 2) flipped = false;
                    break;
                case 99: mini = false; break;
                case 101: mini = true; break;
                case 286: dual = true; break;
                case 287: dual = false; break;
                case 45: mirror = true; break;
                case 46: mirror = false; break;
                }
                if (explain && p.family == 2)
                    why += " " + std::to_string(p.id) + "@" + std::to_string(static_cast<int>(p.x)) + "," + std::to_string(static_cast<int>(p.y)) +
                        (flipped ? "=up" : "=down");
            }
            i = j;
        }
        // the exact state, if you have already passed this start pos playing from further back
        auto it = g_spLearned.find(spKeyOf(sp));
        bool exact = it != g_spLearned.end();
        if (exact) {
            mode = it->second.mode;
            speed = it->second.speed;
            mini = it->second.mini;
            dual = it->second.dual;
            flipped = it->second.flipped;
        }
        if (dual && !dualPossibleAt(gl, sx)) dual = false; // also cleans bad states saved by older versions
        if (sp == current) {
            g_spLastSource = exact ? "exact" : "portals";
            static void* lastSp = nullptr;
            static int lastSig = -1;
            int sig = mode | speed << 4 | mini << 8 | dual << 9 | flipped << 10 | exact << 11 | mirror << 12;
            if (sp != lastSp || sig != lastSig) logf("smart start pos: playing from x=%.0f y=%.0f (%s) -> mode %d, speed %d, mini %d, dual %d, upside down %d | gravity portals:%s",
                sx, sy, exact ? "learned" : "portals", mode, speed, mini, dual, flipped, why.empty() ? " none" : why.c_str());
            lastSp = sp;
            lastSig = sig;
        }
        at<int>(ls, kLsMode) = mode;
        at<int>(ls, kLsSpeed) = speed;
        at<bool>(ls, kLsMini) = mini;
        at<bool>(ls, kLsDual) = dual;
        at<bool>(ls, kLsMirror) = mirror;
        at<bool>(ls, kLsFlipped) = flipped;
    }
    g_spLearnedCount = learnedCountForLevel();
}

void restoreSmartStartPos() {
    for (auto& [ls, b] : g_smartBackup) {
        at<int>(ls, kLsMode) = b.mode;
        at<int>(ls, kLsSpeed) = b.speed;
        at<bool>(ls, kLsMini) = b.mini;
        at<bool>(ls, kLsDual) = b.dual;
        at<bool>(ls, kLsMirror) = b.mirror;
        at<bool>(ls, kLsFlipped) = b.flipped;
    }
    g_smartBackup.clear();
}

// called right before every PlayLayer::resetLevel (the start pos settings are read during it)
void smartStartPosBeforeReset(void* pl) {
    __try {
        if (pl != g_smartLayer) { g_smartBackup.clear(); g_smartLayer = pl; } // new level: old pointers are gone
        g_spLastX = NAN; // the player jumps back: no "crossing" until it moves forward again
        if (g_smartSP) applySmartStartPos(pl);
        else if (!g_smartBackup.empty()) restoreSmartStartPos();
        void* cur = at<void*>(pl, kStartPosObj);
        g_spRunTrusted = cur == nullptr || (g_smartSP && g_spLastSource && std::strcmp(g_spLastSource, "exact") == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) { logf("smart start pos failed (%08lx)", GetExceptionCode()); logged = true; }
    }
}

// ---------------------------------------------------------------------------
// Jukebox: another song for a level (a file of yours, or one from Song File Hub)
// ---------------------------------------------------------------------------
struct LevelInfo { std::string name; int id = 0; int songID = 0; int track = 0; bool valid = false; };
LevelInfo g_lastLevel;            // the last level the game asked the song for
std::mutex g_jbMutex;

std::string jbKey(const LevelInfo& l) {
    std::string n = l.name;
    for (auto& c : n) if (c == '=' || c == '\n' || c == '\r') c = '_';
    return "jb." + n + "|" + std::to_string(l.songID) + "|" + std::to_string(l.track);
}
std::wstring songsDir() { return g_dir + L"kmenu-songs\\"; }

// A chosen song is remembered three ways, so it survives copies, renames and restarts:
//  1. by the level's online ID (the same level always, whatever its name),
//  2. by name + song (the old key, also the only one for levels that were never uploaded),
//  3. by the level's Newgrounds song (any copy of the level that uses that song).
// The first one that is set wins. "-" means "the original song" for that level on purpose.
std::string jbKeyId(const LevelInfo& l) { return l.id > 0 ? "jb.id." + std::to_string(l.id) : std::string(); }
std::string jbKeySong(const LevelInfo& l) { return l.songID > 0 ? "jb.song." + std::to_string(l.songID) : std::string(); }

bool jbFileExists(const std::string& name) {
    std::wstring file = songsDir() + std::wstring(name.begin(), name.end());
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string jbLookup(const LevelInfo& l) {
    for (const std::string& k : {jbKeyId(l), jbKey(l), jbKeySong(l)}) {
        if (k.empty()) continue;
        auto it = g_cfg.find(k);
        if (it == g_cfg.end() || it->second.empty()) continue;
        if (it->second == "-") return {};          // original song chosen for this level
        if (jbFileExists(it->second)) return it->second;
    }
    return {};
}

void cfgSave();
void jbAssign(const LevelInfo& l, const std::string& file) {
    for (const std::string& k : {jbKeyId(l), jbKey(l), jbKeySong(l)})
        if (!k.empty()) g_cfg[k] = file;
    cfgSave(); // right away: closing the game a moment later must not lose it
}

void jbClear(const LevelInfo& l) {
    if (!jbKeyId(l).empty()) g_cfg[jbKeyId(l)] = "-";
    g_cfg[jbKey(l)] = "-";
    if (!jbKeySong(l).empty()) g_cfg.erase(jbKeySong(l));
    cfgSave();
}

bool readLevelSEH(void* level, int* id, int* song, int* track, char* name, size_t nameSize) {
    __try {
        *id = at<int>(level, kLvlID);
        *song = at<int>(level, kLvlSongID);
        *track = at<int>(level, kLvlAudioTrack);
        char* str = reinterpret_cast<char*>(level) + kLvlName;
        size_t size = at<size_t>(str, 0x10), cap = at<size_t>(str, 0x18);
        if (size >= nameSize) size = nameSize - 1;
        const char* src = cap > 15 ? at<const char*>(str, 0) : str;
        std::memcpy(name, src, size);
        name[size] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// GJGameLevel::getAudioFileName(): the file the level plays. If the level has a song assigned, ours.
using GetAudioFile = std::string* (*)(void*, std::string*);
GetAudioFile o_getAudioFileName = nullptr;
std::string* h_getAudioFileName(void* level, std::string* ret) {
    std::string* r = o_getAudioFileName(level, ret);
    LevelInfo info;
    char name[256] = {};
    if (!readLevelSEH(level, &info.id, &info.songID, &info.track, name, sizeof(name))) return r;
    info.valid = true;
    info.name = name;
    {
        std::lock_guard<std::mutex> lk(g_jbMutex);
        g_lastLevel = info;
    }
    std::string chosen = jbLookup(info);
    if (!chosen.empty()) ret->assign(narrow(songsDir() + std::wstring(chosen.begin(), chosen.end())));
    return r;
}

// ---- downloads (WinHTTP)
std::atomic<int> g_dlProgress{0};   // 0..100
std::atomic<int> g_dlState{0};      // 0 idle, 1 working, 2 done, 3 failed
std::string g_dlMessage;

bool httpDownload(const std::wstring& url, const std::wstring& outFile, std::atomic<int>* progress) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring fullPath = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(L"KuangMenu/" KMENU_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, nullptr, nullptr,
                          uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        wchar_t lenBuf[32] = {};
        DWORD lenSz = sizeof(lenBuf);
        long long total = 0;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenBuf, &lenSz, nullptr)) total = _wtoi64(lenBuf);
        if (status == 200) {
            std::wstring tmp = outFile + L".part";
            FILE* f = _wfopen(tmp.c_str(), L"wb");
            if (f) {
                long long got = 0;
                char buf[65536];
                DWORD n = 0;
                ok = true;
                while (WinHttpReadData(r, buf, sizeof(buf), &n) && n > 0) {
                    fwrite(buf, 1, n, f);
                    got += n;
                    if (progress && total > 0) *progress = static_cast<int>(got * 100 / total);
                }
                fclose(f);
                ok = got > 0;
                if (ok) ok = MoveFileExW(tmp.c_str(), outFile.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                else DeleteFileW(tmp.c_str());
            }
        } else {
            logf("download failed: HTTP %lu", status);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---- Song File Hub index (the public list the Jukebox mod uses: FlafyDev/auto-nong-indexes)
struct SfhSong { std::string name, artist, url; std::vector<int> songs, levels; };
std::vector<SfhSong> g_sfh;
std::atomic<int> g_sfhState{0};   // 0 not loaded, 1 loading, 2 ready, 3 failed
const wchar_t* kSfhUrl = L"https://raw.githubusercontent.com/FlafyDev/auto-nong-indexes/v2/sfh-index.min.json";

DWORD WINAPI sfhLoadThread(LPVOID) {
    std::wstring file = songsDir() + L"sfh-index.json";
    CreateDirectoryW(songsDir().c_str(), nullptr);
    // refresh once a day
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool fresh = false;
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa)) {
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER a, b;
        a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
        b.LowPart = nowFt.dwLowDateTime; b.HighPart = nowFt.dwHighDateTime;
        fresh = b.QuadPart - a.QuadPart < 24ull * 3600 * 10000000ull;
    }
    std::atomic<int> dummy{0};
    if (!fresh && !httpDownload(kSfhUrl, file, &dummy) && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_sfhState = 3;
        return 0;
    }
    std::vector<SfhSong> list;
    try {
        FILE* f = _wfopen(file.c_str(), L"rb");
        if (!f) { g_sfhState = 3; return 0; }
        std::string text;
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        auto j = nlohmann::json::parse(text);
        for (auto& [id, s] : j["nongs"]["hosted"].items()) {
            SfhSong song;
            song.name = s.value("name", "");
            song.artist = s.value("artist", "");
            song.url = s.value("url", "");
            if (s.contains("songs")) for (auto& v : s["songs"]) if (v.is_number_integer()) song.songs.push_back(v.get<int>());
            if (s.contains("verifiedLevelIDs")) for (auto& v : s["verifiedLevelIDs"]) if (v.is_number_integer()) song.levels.push_back(v.get<int>());
            if (!song.url.empty()) list.push_back(std::move(song));
        }
    } catch (...) {
        g_sfhState = 3;
        return 0;
    }
    g_sfh = std::move(list);
    g_sfhState = 2;
    logf("Song File Hub index: %zu songs", g_sfh.size());
    return 0;
}

std::wstring widenUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct JbJob { std::wstring url; LevelInfo level; std::string file; };
JbJob g_jbJob;
LevelInfo g_jbAssignLevel;                   // handed to the main thread when a download finishes
std::string g_jbAssignFile;
std::atomic<bool> g_jbAssignReady{false};

DWORD WINAPI jbDownloadThread(LPVOID) {
    CreateDirectoryW(songsDir().c_str(), nullptr);
    std::wstring out = songsDir() + std::wstring(g_jbJob.file.begin(), g_jbJob.file.end());
    bool ok = httpDownload(g_jbJob.url, out, &g_dlProgress);
    if (ok) {
        g_jbAssignLevel = g_jbJob.level;
        g_jbAssignFile = g_jbJob.file;
        g_jbAssignReady = true;
    }
    g_dlState = ok ? 2 : 3;
    return 0;
}

std::wstring g_jbPicked;
std::atomic<bool> g_jbPickedReady{false};
LevelInfo g_jbPickLevel;
volatile bool g_jbPicking = false;
DWORD WINAPI jbPickThread(LPVOID) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Audio (*.mp3;*.ogg;*.wav)\0*.mp3;*.ogg;*.wav\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) { g_jbPicked = file; g_jbPickedReady = true; }
    g_jbPicking = false;
    return 0;
}

// main thread, every frame: apply finished downloads / picked files
void jukeboxTick() {
    if (g_jbAssignReady) {
        g_jbAssignReady = false;
        jbAssign(g_jbAssignLevel, g_jbAssignFile);
        showToast(T("jb.done"));
    }
    if (g_jbPickedReady) {
        g_jbPickedReady = false;
        std::wstring src = g_jbPicked;
        size_t dot = src.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L".mp3" : src.substr(dot);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        int n = cfgInt("jb.n", 0) + 1;
        std::wstring name = L"local-" + std::to_wstring(n) + ext;
        CreateDirectoryW(songsDir().c_str(), nullptr);
        if (CopyFileW(src.c_str(), (songsDir() + name).c_str(), FALSE)) {
            cfgSetInt("jb.n", n);
            jbAssign(g_jbPickLevel, std::string(name.begin(), name.end()));
            showToast(T("jb.done"));
        }
    }
}

void drawPlayerColumn(int index) {
    if (beginColumn("player", "cat.player", index)) {
        if (toggleRow("noclip", T("pl.noclip"), T("pl.noclip.d"), &g_noclip)) cfgSetBool("pl.noclip", g_noclip);
        if (g_hbHooked && toggleRow("nchitbox", T("pl.nchb"), T("pl.nchb.d"), &g_ncHitbox)) cfgSetBool("pl.nchb", g_ncHitbox);
        for (int i = 0; i < 4; i++) gameVarRow(kGameVars[i]);
        if (matches(T("gv.more"))) {
            if (buttonRow("hbmore", T("gv.more"), T("gv.more.d"))) ImGui::OpenPopup("pop.hitbox");
        }
        placePopup("pop.hitbox");
        if (ImGui::BeginPopup("pop.hitbox")) {
            popupTitle(T("gv.more"));
            for (int i = 4; i < 6; i++) {
                bool cur = false;
                if (!gameVarSEH(kGameVars[i].key, &cur)) continue;
                if (popupCheck(T(kGameVars[i].label), &cur)) setGameVarSEH(kGameVars[i], cur);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(kGameVars[i].desc));
            }
            ImGui::PushTextWrapPos(S(260));
            hintText(T("gv.hint"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.hitbox");
            ImGui::EndPopup();
        }
        if (toggleRow("accurate", T("pl.accurate"), T("pl.accurate.d"), &g_accurate)) cfgSetBool("pl.accurate", g_accurate);
        if (toggleRow("pctfix", T("pl.pctfix"), T("pl.pctfix.d"), &g_pctFix)) cfgSetBool("pl.pctfix", g_pctFix);
        if (g_trajReady && toggleRow("traj", T("tj.on"), T("tj.on.d"), &g_traj, "pop.traj")) cfgSetBool("tj.on", g_traj);
        placePopup("pop.traj");
        if (ImGui::BeginPopup("pop.traj")) {
            popupTitle(T("tj.on"));
            ImGui::SetNextItemWidth(S(150));
            if (gradSlider(T("tj.len"), &g_trajLen, 0.5f, 4.0f, "%.1f s")) cfgSetFloat("tj.len2", g_trajLen);
            for (int k = 0; k < 2; k++) {
                ImGui::PushID(k);
                ImGui::Separator();
                bool* en = k == 0 ? &g_trajP1 : &g_trajP2;
                if (popupCheck(T(k == 0 ? "tj.p1" : "tj.p2"), en)) cfgSetBool(k == 0 ? "tj.p1" : "tj.p2", *en);
                const ImGuiColorEditFlags cf = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha;
                for (int c = 0; c < 2; c++) {
                    if (c) ImGui::SameLine(0, S(14));
                    if (ImGui::ColorEdit3(T(c == 0 ? "tj.hold" : "tj.rel"), g_trajCol[k][c], cf)) {
                        char key[24];
                        std::snprintf(key, sizeof(key), "tj.c%d%d", k, c);
                        int v = (static_cast<int>(g_trajCol[k][c][0] * 255 + 0.5f) << 16) | (static_cast<int>(g_trajCol[k][c][1] * 255 + 0.5f) << 8) |
                            static_cast<int>(g_trajCol[k][c][2] * 255 + 0.5f);
                        cfgSetInt(key, v);
                    }
                }
                if (popupCheck(T("tj.trail"), &g_trajTrail[k])) cfgSetBool(k == 0 ? "tj.trail1" : "tj.trail2", g_trajTrail[k]);
                ImGui::PopID();
            }
            ImGui::PushTextWrapPos(S(260));
            hintText(T("tj.tip"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.traj");
            ImGui::EndPopup();
        }
        if (toggleRow("noatt", T("pl.noatt"), T("pl.noatt.d"), &g_hideAttempts)) cfgSetBool("pl.noatt", g_hideAttempts);
    }
    endColumn();
}

void keyButton(const char* id, int* key) {
    std::string label = (g_waitingSpKey == key) ? std::string(T("set.press")) : keyName(*key);
    if (pillButton(id, label.c_str(), ImVec2(S(120), S(24)), g_waitingSpKey == key)) g_waitingSpKey = key;
}

void drawStartPosColumn(int index) {
    if (beginColumn("startpos", "cat.startpos", index)) {
        if (toggleRow("smartsp", T("sp.smart"), T("sp.smart.d"), &g_smartSP, "pop.smartsp")) cfgSetBool("sp.smart", g_smartSP);
        placePopup("pop.smartsp");
        if (ImGui::BeginPopup("pop.smartsp")) {
            popupTitle(T("sp.smart"));
            if (popupCheck(T("sp.orbs"), &g_smartOrbs)) { cfgSetBool("sp.orbs", g_smartOrbs); }
            ImGui::PushTextWrapPos(S(260));
            hintText(T("sp.orbs.d"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.smartsp");
            ImGui::EndPopup();
        }
        if (g_smartSP && g_playLayer && g_playLayer == g_spFpLayer && !g_search.size()) {
            unsigned nSp = 0;
            void** sps = nullptr;
            if (arrayItemsSEH(at<void*>(g_playLayer, kStartPosArr), nSp, sps) && nSp > 0) {
                char lb[96];
                std::snprintf(lb, sizeof(lb), T("sp.learned"), g_spLearnedCount, static_cast<int>(nSp));
                hintText(lb);
                if (g_spLearnedCount > 0 && buttonRow("spforget", T("sp.forget"), T("sp.smart.d"))) {
                    forgetLearnedForLevel();
                    g_spLearnedCount = 0;
                }
            }
        }
        if (toggleRow("spswitch", T("sp.switch"), T("sp.switch.d"), &g_spSwitcher, "pop.spkeys")) cfgSetBool("sp.switch", g_spSwitcher);
        placePopup("pop.spkeys");
        if (ImGui::BeginPopup("pop.spkeys")) {
            popupTitle(T("sp.keys"));
            caption(T("sp.prev"));
            keyButton("##spprev", &g_spKeyPrev);
            caption(T("sp.next"));
            keyButton("##spnext", &g_spKeyNext);
            ImGui::PushTextWrapPos(S(260));
            hintText(T("sp.keys.d"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.spkeys");
            ImGui::EndPopup();
        }
        if (!g_search.size()) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s / %s", keyName(g_spKeyPrev).c_str(), keyName(g_spKeyNext).c_str());
            caption(b);
        }
    }
    endColumn();
}

void drawJukeboxColumn(int index) {
    if (beginColumn("jukebox", "cat.jukebox", index)) {
        LevelInfo li;
        {
            std::lock_guard<std::mutex> lk(g_jbMutex);
            li = g_lastLevel;
        }
        if (!li.valid) {
            hintText(T("jb.nolevel"));
        } else {
            ImGui::PushFont(g_fontBold);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(li.name.empty() ? "?" : li.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            char b[128];
            if (li.songID > 0) std::snprintf(b, sizeof(b), "ID %d  \xC2\xB7  %s %d", li.id, T("jb.song"), li.songID);
            else std::snprintf(b, sizeof(b), "ID %d  \xC2\xB7  %s", li.id, T("jb.official"));
            caption(b);
            std::string cur = jbLookup(li);
            ImGui::PushStyleColor(ImGuiCol_Text, cur.empty() ? kDim : ImVec4(0.45f, 0.95f, 0.6f, 1));
            ImGui::PushTextWrapPos(0);
            ImGui::Text("%s %s", T("jb.now"), cur.empty() ? T("jb.original") : cur.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(2)));
            if (buttonRow("sfh", T("jb.sfh"), T("jb.sfh.d"))) {
                if (g_sfhState == 0 || g_sfhState == 3) {
                    g_sfhState = 1;
                    if (HANDLE t = CreateThread(nullptr, 0, sfhLoadThread, nullptr, 0, nullptr)) CloseHandle(t);
                }
                ImGui::OpenPopup("pop.sfh");
            }
            if (buttonRow("jblocal", T("jb.local"), T("jb.local.d")) && !g_jbPicking) {
                g_jbPicking = true;
                g_jbPickLevel = li;
                if (HANDLE t = CreateThread(nullptr, 0, jbPickThread, nullptr, 0, nullptr)) CloseHandle(t);
            }
            if (!cur.empty() && buttonRow("jborig", T("jb.reset"), nullptr)) jbClear(li);
            if (g_dlState == 1) {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, g_accent);
                ImGui::ProgressBar(g_dlProgress / 100.0f, ImVec2(-1, S(6)), "");
                ImGui::PopStyleColor();
            } else if (g_dlState == 3) {
                ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("jb.fail"));
            }
            hintText(T("jb.restart"));

            placePopup("pop.sfh");
            if (ImGui::BeginPopup("pop.sfh")) {
                popupTitle(T("jb.sfh"));
                int st = g_sfhState;
                if (st == 1) hintText(T("jb.loading"));
                else if (st == 3) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("jb.listfail"));
                else if (st == 2) {
                    int shown = 0;
                    ImGui::BeginChild("##sfhlist", ImVec2(S(330), S(260)));
                    for (size_t i = 0; i < g_sfh.size(); i++) {
                        const SfhSong& song = g_sfh[i];
                        if (li.songID <= 0 || std::find(song.songs.begin(), song.songs.end(), li.songID) == song.songs.end()) continue;
                        shown++;
                        ImGui::PushID(static_cast<int>(i));
                        bool verified = std::find(song.levels.begin(), song.levels.end(), li.id) != song.levels.end();
                        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - S(60));
                        ImGui::TextUnformatted(song.name.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                        ImGui::Text("%s%s", song.artist.c_str(), verified ? "" : "");
                        ImGui::PopStyleColor();
                        if (verified) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.6f, 1), "%s", T("jb.verified")); }
                        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(54));
                        if (pillButton("##use", T("jb.use"), ImVec2(S(54), S(22)), false) && g_dlState != 1) {
                            g_jbJob.url = widenUtf8(song.url);
                            g_jbJob.level = li;
                            g_jbJob.file = "sfh-" + std::to_string(li.songID) + "-" + std::to_string(i) + ".mp3";
                            g_dlProgress = 0;
                            g_dlState = 1;
                            if (HANDLE t = CreateThread(nullptr, 0, jbDownloadThread, nullptr, 0, nullptr)) CloseHandle(t);
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                    if (!shown) hintText(li.songID > 0 ? T("jb.none") : T("jb.nocustom"));
                    ImGui::EndChild();
                }
                notePopupSize("pop.sfh");
                ImGui::EndPopup();
            }
        }
    }
    endColumn();
}

void drawExtrasColumn(int index) {
    if (beginColumn("extras", "cat.extras", index)) {
        if (toggleRow("trail", T("tr.on"), T("tr.on.d"), &g_trailOn, "pop.trail")) cfgSetBool("tr.on", g_trailOn);
        placePopup("pop.trail");
        if (ImGui::BeginPopup("pop.trail")) {
            popupTitle(T("tr.on"));
            caption(g_trailFile.empty() ? T("tr.none") : g_trailFile.c_str());
            if (g_trailOn && !g_trailFile.empty() && !g_trailTex && !g_trailDirty) hintText(T("tr.fail"));
            if (pillButton("##pick", T("tr.pick"), ImVec2(S(230), S(26)), false) && !g_trailPicking) {
                g_trailPicking = true;
                if (HANDLE t = CreateThread(nullptr, 0, pickTrailThread, nullptr, 0, nullptr)) CloseHandle(t);
            }
            ImGui::SetNextItemWidth(S(150));
            if (gradSlider(T("tr.width"), &g_trailWidth, 0.2f, 4.0f, "%.2fx")) cfgSetFloat("tr.width", g_trailWidth);
            if (popupCheck(T("tr.ship"), &g_trailShipOnly)) cfgSetBool("tr.ship", g_trailShipOnly);
            if (popupCheck(T("tr.keep"), &g_trailKeep)) cfgSetBool("tr.keep", g_trailKeep);
            if (popupCheck(T("tr.glow"), &g_trailGlow)) cfgSetBool("tr.glow", g_trailGlow);
            ImGui::PushTextWrapPos(S(260));
            hintText(T("tr.tip"));
            ImGui::PopTextWrapPos();
            notePopupSize("pop.trail");
            ImGui::EndPopup();
        }
    }
    endColumn();
}

// ---------------------------------------------------------------------------
// FPS bypass: the game's frame limit (CCApplication::setAnimationInterval)
// ---------------------------------------------------------------------------
bool g_fpsOn = false;
float g_fpsTarget = 360.0f;
void* (*g_sharedApp)() = nullptr;
void (*g_appSetInterval)(void*, double) = nullptr;
double (*g_dirGetInterval)(void*) = nullptr;
double g_origInterval = 0;
bool g_fpsApplied = false;
double g_fpsLastApply = 0;
constexpr float kPaceMaxFps = 10000.0f; // above this the game is simply left unlimited
bool g_fpsPrecise = true;
// Input Boost: inside a level, pace evenly at an FPS the PC can always hold (see frame work meter below)
bool g_inputBoost = false;  // input thread priority only
bool g_cbfExact = false, g_cbfFailed = false;
const bool g_cbfTw = true;      // exact cuts also with a level's Time Warp != 1 (always, since 0.12.0)
bool g_modCbf = true;           // "mod.cbf": the CBF module, switched in the Mods window
inline bool cbfWanted() { return g_modCbf && g_cbfExact; }
bool g_cbfAcc = true;           // always on since 0.14.10 (was the "gl.cbf.acc" test switch of 0.14.8): click times put on the clock of the physics steps (see cbfAccStamp)
inline bool cbfAccWanted() { return cbfWanted() && g_cbfAcc && !g_cbfFailed; }
unsigned long long g_cbfAccN = 0; // presses it moved, and the leftover time it added (for the menu)
double g_cbfAccSum = 0, g_cbfAccMax = 0;
volatile bool g_cbfTwLive = false; // that mode is really on right now (patched + processQueuedButtons hook ready)
bool g_pfixOn = false, g_pfixFailed = false, g_pfixPatched = false; // Precision Fix (see pfixTick)
unsigned char* g_pfixMem = nullptr;
unsigned char* g_cbfMemView(); // counters for the menu // exact Click Between Steps (see cbfTick)
bool g_steadyFps = false;   // automatic even FPS cap inside levels (was part of Input Boost)
float g_boostCap = 0;        // FPS chosen from the measurement (0 = not measured yet)
bool boostActive() { return g_steadyFps && g_inLevel; }
float paceTarget() {
    float t = g_fpsOn ? g_fpsTarget : kPaceMaxFps;
    if (boostActive() && g_boostCap > 0) t = std::min(t, g_boostCap);
    return t;
}
bool paceActive() {
    if (boostActive()) return g_boostCap > 0 && paceTarget() <= kPaceMaxFps;
    return g_fpsOn && g_fpsPrecise && g_fpsTarget <= kPaceMaxFps;
}

void applyFps() {
    if (!g_sharedApp || !g_appSetInterval) return;
    double t = now();
    // Input Boost turning on/off (entering or leaving a level, or a new measured FPS): apply right away
    static bool lastPace = false;
    static float lastTarget = 0;
    if (paceActive() != lastPace || paceTarget() != lastTarget) { lastPace = paceActive(); lastTarget = paceTarget(); g_fpsLastApply = 0; }
    if (g_fpsOn || boostActive()) {
        if (!g_fpsApplied && g_dirGetInterval && g_sharedDirector) {
            void* dir = g_sharedDirector();
            if (dir) g_origInterval = g_dirGetInterval(dir);
        }
        // re-applied every 2 s in case the game sets its own limit again (focus change, settings)
        if (!g_fpsApplied || g_fpsLastApply == 0 || t - g_fpsLastApply > 2.0) {
            // with the precise limiter the game's own limit is opened up (1 us) and we pace in swapBuffers
            double interval = paceActive() ? 1e-6 : (g_fpsOn ? 1.0 / std::max(1.0f, g_fpsTarget) : 1e-6);
            if (void* app = g_sharedApp()) g_appSetInterval(app, interval);
            g_fpsApplied = true;
            g_fpsLastApply = t;
        }
    } else if (g_fpsApplied) {
        if (void* app = g_sharedApp()) g_appSetInterval(app, g_origInterval > 0 ? g_origInterval : 1.0 / 60.0);
        g_fpsApplied = false;
    }
}

// ---------------------------------------------------------------------------
// Kuang Frame Extrapolation panel: the same switches as its app (kfe.ini, applied live by kfe.dll)
// ---------------------------------------------------------------------------
struct KfeOpts { bool enabled = true, interp = false, smart = true, bg = true, rot = true, obj = true; };
KfeOpts g_kfe;
double g_kfeRead = -10;
bool g_kfeInstalled = false;

std::wstring kfeIni() { return g_dir + L"kfe.ini"; }
bool iniBool(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(L"General", key, def ? 1 : 0, kfeIni().c_str()) != 0;
}
void readKfe() {
    g_kfeInstalled = GetFileAttributesW((g_dir + L"kfe.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    g_kfe.enabled = iniBool(L"enabled", true);
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"General", L"mode", L"extrapolation", mode, 32, kfeIni().c_str());
    g_kfe.interp = _wcsicmp(mode, L"interpolation") == 0;
    g_kfe.smart = iniBool(L"smart_prediction", true);
    g_kfe.bg = iniBool(L"smooth_background", true);
    g_kfe.rot = iniBool(L"smooth_rotation", true);
    g_kfe.obj = iniBool(L"smooth_objects", true);
}
void writeKfe(const wchar_t* key, const wchar_t* value) { WritePrivateProfileStringW(L"General", key, value, kfeIni().c_str()); }
void writeKfeBool(const wchar_t* key, bool v) { writeKfe(key, v ? L"1" : L"0"); }

bool readKfeStatus(KfeStatus& st) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, KFE_STATUS_MAPPING);
    if (!h) return false;
    bool ok = false;
    if (auto v = static_cast<KfeStatus*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(KfeStatus)))) {
        st = *v;
        UnmapViewOfFile(v);
        ok = st.magic == KFE_STATUS_MAGIC;
    }
    CloseHandle(h);
    return ok;
}

void drawGlobalColumn(int index) {
    if (beginColumn("global", "cat.global", index)) {
        if (toggleRow("speed", T("gl.speed"), T("gl.speed.d"), &g_speedOn, "pop.speed")) cfgSetBool("gl.speed", g_speedOn);
        placePopup("pop.speed");
        if (ImGui::BeginPopup("pop.speed")) {
            popupTitle(T("gl.speed"));
            ImGui::SetNextItemWidth(S(190));
            // logarithmic: fine control near 0.0001x and still reaches 100x (Ctrl+click also types a value)
            if (gradSlider(T("gl.speed.v"), &g_speed, kSpeedMin, kSpeedMax, "%.4gx",
                    ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat)) {
                g_speed = std::clamp(g_speed, kSpeedMin, kSpeedMax);
                cfgSetFloat("gl.speed.v", g_speed);
            }
            ImGui::SetNextItemWidth(S(190));
            if (ImGui::InputFloat("##speedtype", &g_speed, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                g_speed = std::clamp(g_speed, kSpeedMin, kSpeedMax);
                cfgSetFloat("gl.speed.v", g_speed);
            }
            ImGui::TextDisabled("%s", T("gl.speed.type"));
            float presets[] = {0.01f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 10.0f, 100.0f};
            for (int k = 0; k < 8; k++) {
                if (k % 4) ImGui::SameLine(0, S(3));
                char b[16];
                std::snprintf(b, sizeof(b), "%gx", presets[k]);
                if (ImGui::Button(b)) { g_speed = presets[k]; cfgSetFloat("gl.speed.v", g_speed); }
            }
            if (popupCheck(T("gl.speed.audio"), &g_speedAudio)) cfgSetBool("gl.speed.audio", g_speedAudio);
            notePopupSize("pop.speed");
            ImGui::EndPopup();
        }
        if (toggleRow("safe", T("gl.safe"), T("gl.safe.d"), &g_safeManual)) cfgSetBool("gl.safe", g_safeManual);
        gameVarRow(kGameVars[6]);
        gameVarRow(kGameVars[7]);
        if (!g_modCbf) ImGui::BeginDisabled();
        bool cbfClicked = toggleRow("cbfexact", T("gl.cbf"), T("gl.cbf.d"), &g_cbfExact);
        if (!g_modCbf) { ImGui::EndDisabled(); if (matches(T("gl.cbf"))) hintText(T("gl.cbf.off")); }
        else if (cbfClicked) {
            cfgSetBool("gl.cbf", g_cbfExact);
            if (g_cbfExact) { // it only does something with the game's own Click Between Steps on
                bool cur = false;
                if (gameVarSEH(kGameVars[6].key, &cur) && !cur) setGameVarSEH(kGameVars[6], true);
            }
        }
        if (g_cbfFailed) hintText(T("gl.cbf.fail"));
        if (cbfWanted() && !g_cbfFailed && matches(T("gl.cbf"))) { // proof that it is really cutting steps (0 = the game never took the split path)
            char b[160];
            unsigned long long c = g_cbfMemView() ? *reinterpret_cast<volatile uint64_t*>(g_cbfMemView() + kcbf_blob::kOffCount) : 0ull;
            unsigned long long c2 = g_cbfMemView() ? *reinterpret_cast<volatile uint64_t*>(g_cbfMemView() + kcbf_blob::kOffCntM) : 0ull;
            std::snprintf(b, sizeof b, g_lang == 1 ? "Cortes exactos aplicados: %llu (pasos con varios cortes: %llu)" : "Exact cuts applied: %llu (steps cut several times: %llu)", c, c2);
            hintText(b);
        }
        if (cbfAccWanted() && matches(T("gl.cbf"))) { // click times are always put on the clock of the physics steps (merged into CBF in 0.14.10)
            char b[160];
            std::snprintf(b, sizeof b, g_lang == 1 ? "Clics colocados: %llu (sobrante medio %.2f ms, max %.2f ms)" : "Presses placed: %llu (average leftover %.2f ms, max %.2f ms)",
                          g_cbfAccN, g_cbfAccN ? g_cbfAccSum / static_cast<double>(g_cbfAccN) * 1000.0 : 0.0, g_cbfAccMax * 1000.0);
            hintText(b);
        }
        if (toggleRow("fpsbypass", T("gl.fps"), T("gl.fps.d"), &g_fpsOn)) { cfgSetBool("gl.fps", g_fpsOn); g_fpsLastApply = 0; }
        if (matches(T("gl.fps"))) {
            // type the FPS like in Mega Hack; applied when you press Enter or click away
            int fps = static_cast<int>(g_fpsTarget);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
            gradRect(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + h), U(ImVec4(0.20f, 0.205f, 0.235f, 1)),
                U(ImVec4(0.13f, 0.135f, 0.155f, 1)), S(6));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##fpsval", &fps, 0, 0);
            ImGui::PopStyleColor();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_fpsTarget = static_cast<float>(std::clamp(fps, 1, 1000000));
                cfgSetFloat("gl.fps.v", g_fpsTarget);
                g_fpsLastApply = 0;
            }
        }
        if (toggleRow("fpsprecise", T("gl.fps.p"), T("gl.fps.p.d"), &g_fpsPrecise)) { cfgSetBool("gl.fps.p", g_fpsPrecise); g_fpsLastApply = 0; }
        if (toggleRow("inputboost", T("gl.ib"), T("gl.ib.d"), &g_inputBoost)) cfgSetBool("gl.ib", g_inputBoost);
        if (toggleRow("steadyfps", T("gl.sfps"), T("gl.sfps.d"), &g_steadyFps)) { cfgSetBool("gl.sfps", g_steadyFps); g_fpsLastApply = 0; }
        if (g_steadyFps && g_inLevel && !g_search.size()) {
            char ib[64];
            if (g_boostCap > 0) std::snprintf(ib, sizeof(ib), T("gl.ib.on"), static_cast<int>(paceTarget()));
            else std::snprintf(ib, sizeof(ib), "%s", T("gl.ib.measuring"));
            hintText(ib);
        }
        if (toggleRow("visthr", T("gl.vis"), T("gl.vis.d"), &g_visThrottle, "pop.vis")) cfgSetBool("gl.vis", g_visThrottle);
        placePopup("pop.vis");
        if (ImGui::BeginPopup("pop.vis")) {
            popupTitle(T("gl.vis"));
            if (popupCheck(T("gl.vis.auto"), &g_visAuto)) cfgSetBool("gl.vis.auto", g_visAuto);
            if (g_visAuto) {
                char hzb[96];
                std::snprintf(hzb, sizeof(hzb), T("gl.vis.autohz"), monitorHz());
                ImGui::TextDisabled("%s", hzb);
            }
            ImGui::BeginDisabled(g_visAuto);
            ImGui::SetNextItemWidth(S(190));
            // logarithmic like the speed slider: fine control at low values and still reaches 100000 (Ctrl+click also types)
            if (gradSlider(T("gl.vis.hz"), &g_visHz, kVisHzMin, kVisHzMax, "%.0f",
                    ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat)) {
                g_visHz = std::clamp(g_visHz, kVisHzMin, kVisHzMax);
                cfgSetFloat("gl.vis.hz", g_visHz);
            }
            ImGui::SetNextItemWidth(S(190));
            if (ImGui::InputFloat("##vishztype", &g_visHz, 0.0f, 0.0f, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                g_visHz = std::clamp(g_visHz, kVisHzMin, kVisHzMax);
                cfgSetFloat("gl.vis.hz", g_visHz);
            }
            ImGui::TextDisabled("%s", T("gl.vis.type"));
            ImGui::EndDisabled();
            notePopupSize("pop.vis");
            ImGui::EndPopup();
        }
        // the only Frame Extrapolation switch: on/off (the rest stays in its own app)
        if (now() - g_kfeRead > 2.0) { readKfe(); g_kfeRead = now(); }
        gameVarRow(kFastMenu); // the game's own option, nothing else
        if (g_kfeInstalled && toggleRow("kfeon", T("kfe.name"), T("kfe.on.d"), &g_kfe.enabled)) writeKfeBool(L"enabled", g_kfe.enabled);
        if (safeModeActive() && !g_search.size()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(T("gl.safe.on"));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            hintText(T("gl.safe.hint"));
        }
    }
    endColumn();
}

// ---- Utility actions (the same things Mega Hack's Utility tab does)

// player buttons: held while the mouse holds them, like the real key
struct HeldButton { void* layer = nullptr; int button = 0; bool p1 = true; bool down = false; };
HeldButton g_held[6];

bool pressButtonSEH(void* layer, bool down, int button, bool p1) {
    __try {
        if (!g_handleButton || !layer) return false;
        g_handleButton(layer, down, button, p1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// lets go of any button we still hold if the mouse is up, the menu closed or the level changed
void utilTick() {
    for (auto& h : g_held) {
        if (!h.down) continue;
        if (h.layer == g_gameLayer && g_menuOpen && ImGui::IsMouseDown(0)) continue;
        if (h.layer == g_gameLayer) pressButtonSEH(h.layer, false, h.button, h.p1);
        h.down = false;
        h.layer = nullptr;
    }
}

// one row: "Player N" and three buttons that stay pressed while held
void playerButtonsRow(int player) {
    static const char* const kNames[3] = {"ut.click", "ut.left", "ut.right"};
    const char* title = T(player == 1 ? "ut.p1" : "ut.p2");
    if (!matches(title) && !matches(T("ut.click"))) return;
    caption(title);
    float gap = S(6), w = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3, h = S(26);
    for (int b = 0; b < 3; b++) {
        HeldButton& hb = g_held[(player - 1) * 3 + b];
        if (b) ImGui::SameLine(0, gap);
        char id[16];
        std::snprintf(id, sizeof(id), "##pb%d%d", player, b);
        pillButton(id, T(kNames[b]), ImVec2(w, h), hb.down);
        tooltip(T("ut.btn.d"));
        if (ImGui::IsItemActivated()) {
            if (!g_gameLayer) showToast(T("ut.inlevel"));
            else if (pressButtonSEH(g_gameLayer, true, b + 1, player == 1)) hb = {g_gameLayer, b + 1, player == 1, true};
        }
        if (ImGui::IsItemDeactivated() && hb.down) {
            if (hb.layer == g_gameLayer) pressButtonSEH(hb.layer, false, hb.button, hb.p1);
            hb.down = false;
            hb.layer = nullptr;
        }
    }
}

bool sceneHasSEH(const char* rtti) {
    __try {
        void* dir = g_sharedDirector();
        void* scene = dir ? g_runningScene(dir) : nullptr;
        return scene && nodeHasChildNamed(scene, rtti);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// restart from the beginning (or from the chosen start pos): the same steps as the start pos switcher, without changing it
void restartLevel() {
    void* pl = g_playLayer;
    g_resetLevelFromStart(pl);
    auto vt = *reinterpret_cast<void***>(pl);
    reinterpret_cast<void (*)(void*, float)>(vt[0x430 / 8])(pl, 0.0f);
    if (g_updateCamera) g_updateCamera(pl, 0.0f);
    if (g_updateTestModeLabel) g_updateTestModeLabel(pl);
    if (g_startMusic) g_startMusic(pl);
}
void restartLevelSEH() {
    __try {
        restartLevel();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("restart failed (%08lx)", GetExceptionCode());
    }
}
void restartLevelAction() {
    if (!g_playLayer || !g_resetLevelFromStart) { showToast(T("ut.inlevel")); return; }
    if (g_endScreen) { showToast(T("ut.endscreen")); return; }
    if (sceneHasSEH(".?AVPauseLayer@@")) { showToast(T("ut.paused")); return; }
    restartLevelSEH();
}

bool practiceStateSEH(bool* on) {
    __try {
        if (!g_playLayer) return false;
        *on = at<bool>(g_playLayer, kPractice);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void setPracticeSEH(bool on) {
    __try {
        g_togglePractice(g_playLayer, on);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("practice toggle failed (%08lx)", GetExceptionCode());
    }
}
void setPracticeAction(bool on) {
    if (!g_playLayer || !g_togglePractice) { showToast(T("ut.inlevel")); return; }
    if (g_endScreen) { showToast(T("ut.endscreen")); return; }
    setPracticeSEH(on);
}

void openGameSettingsSEH() {
    __try {
        void* dir = g_sharedDirector();
        void* scene = dir ? g_runningScene(dir) : nullptr;
        if (!scene || !g_openOptions) return;
        g_openOptions(scene, false);
        g_menuOpen = false; // the game's window is under the menu: get out of its way
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("opening the settings failed (%08lx)", GetExceptionCode());
    }
}

// ---- Uncomplete Level: the level goes back to 0% everywhere the game keeps it on this PC
constexpr uintptr_t kLvlType = 0x4c4;        // GJGameLevel::m_levelType (3 = online level saved on this PC)
constexpr uintptr_t kLvlDaily = 0x3e0;       // GJGameLevel::m_dailyID
constexpr uintptr_t kLvlGauntlet = 0x2d1;    // GJGameLevel::m_gauntletLevel
constexpr uintptr_t kLvlStars = 0x3fc;       // GJGameLevel::m_stars
constexpr uintptr_t kLvlLength = 0x3cc;      // GJGameLevel::m_levelLength (5 = platformer)

bool isClassSEH(void* p, const char* rtti) {
    __try {
        if (!p || (reinterpret_cast<uintptr_t>(p) & 7)) return false;
        const char* n = rttiName(p);
        return n && std::strcmp(n, rtti) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the level that is being played, or the one whose page (LevelInfoLayer) is open
void* findCurrentLevelSEH() {
    __try {
        if (g_playLayer) {
            void* lvl = at<void*>(g_playLayer, 0x878);
            return isClassSEH(lvl, ".?AVGJGameLevel@@") ? lvl : nullptr;
        }
        void* dir = g_sharedDirector();
        void* scene = dir ? g_runningScene(dir) : nullptr;
        if (!scene) return nullptr;
        unsigned c = 0;
        void** arr = nullptr;
        if (!arrayItems(at<void*>(scene, 0xd0), c, arr)) return nullptr;
        for (unsigned i = 0; i < c && i < 32; i++) {
            if (!isClassSEH(arr[i], ".?AVLevelInfoLayer@@")) continue;
            for (uintptr_t off = 0x1a0; off < 0x400; off += 8) {
                void* p = at<void*>(arr[i], off);
                if (isClassSEH(p, ".?AVGJGameLevel@@")) return p;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// MSVC std::string inside a game object -> empty (keeps its buffer)
void clearGameString(void* s) {
    size_t cap = at<size_t>(s, 0x18);
    char* d = cap > 15 ? at<char*>(s, 0) : reinterpret_cast<char*>(s);
    if (d) d[0] = 0;
    at<size_t>(s, 0x10) = 0;
}

struct UncompleteResult { bool ok = false; int stars = 0, demons = 0, coins = 0; bool notOnline = false; };

// returns true if the key was there (and removes it)
bool dictTake(void* dict, const std::string& key) {
    if (!dict || !g_dictObject || !g_dictRemove) return false;
    if (!g_dictObject(dict, key)) return false;
    g_dictRemove(dict, key);
    return true;
}

void uncompleteLevel(void* lvl, UncompleteResult& r) {
    if (at<int>(lvl, kLvlType) != 3) { r.notOnline = true; return; }
    int id = at<int>(lvl, 0x148) - at<int>(lvl, 0x14c);
    int daily = at<int>(lvl, kLvlDaily);
    bool gauntlet = at<bool>(lvl, kLvlGauntlet);
    bool platformer = at<int>(lvl, kLvlLength) == 5;
    int stars = at<int>(lvl, kLvlStars);
    // 1) the level itself: percentages, attempts, jumps, clicks, time, platformer bests
    const uintptr_t seeds[] = {0x364, 0x370, 0x37c, 0x388, 0x39c, 0x3a8, 0x3b4}; // 3 ints each
    for (uintptr_t o : seeds) at<int>(lvl, o) = at<int>(lvl, o + 4) = at<int>(lvl, o + 8) = 0;
    at<int>(lvl, 0x3c0) = 0; // practice best
    // the game's own check value for those numbers (same formula it uses when it saves a percentage)
    bool valid = at<bool>(lvl, 0x398);
    at<int>(lvl, 0x394) = static_cast<int>(0xff5u * 0xff5u + 0x20a2u * 0xf97u + (valid ? 0x5cau : 0u) + 0xfd04a1f9u);
    for (uintptr_t o = 0x588; o <= 0x594; o += 4) at<int>(lvl, o) = 0;
    for (uintptr_t o = 0x5b8; o <= 0x5c4; o += 4) at<int>(lvl, o) = 0;
    if (g_platChecksum) at<int>(lvl, 0x5e8) = g_platChecksum(0, 0, 0, 0, 0, 0, 0, 0);
    clearGameString(reinterpret_cast<char*>(lvl) + 0x518); // personal bests (the progress bar marks)
    clearGameString(reinterpret_cast<char*>(lvl) + 0x598); // platformer best time
    clearGameString(reinterpret_cast<char*>(lvl) + 0x5c8); // platformer best points
    // 2) the save file's lists: completed, stars, demon, coins (so the stats can be earned again, not twice)
    void* gsm = g_gsmShared ? g_gsmShared() : nullptr;
    if (gsm && isClassSEH(gsm, ".?AVGameStatsManager@@")) {
        void* done = at<void*>(gsm, 0x2c8);
        void* coinsOk = at<void*>(gsm, 0x2d0);
        void* coinsWait = at<void*>(gsm, 0x2d8);
        std::string sid = std::to_string(id), sd = std::to_string(daily);
        dictTake(done, "c_" + sid);
        dictTake(done, "g_" + sid);
        if (daily > 0) { dictTake(done, "d_" + sd); dictTake(done, "e_" + sd); }
        bool hadStar = dictTake(done, "star_" + sid);
        hadStar |= dictTake(done, "gstar_" + sid);
        if (daily > 0) hadStar |= dictTake(done, "dstar_" + sd);
        bool hadDemon = dictTake(done, "demon_" + sid);
        hadDemon |= dictTake(done, "gdemon_" + sid);
        if (daily > 0) hadDemon |= dictTake(done, "ddemon_" + sd);
        for (int n = 0; n <= 3; n++) {
            std::string k = sid + "_" + std::to_string(n);
            std::string kg = k + "_g";
            std::string kd = k + "_" + sd;
            if (dictTake(coinsOk, k)) r.coins++;
            if (dictTake(coinsOk, kg)) r.coins++;
            if (daily > 0 && dictTake(coinsOk, kd)) r.coins++;
            dictTake(coinsWait, k);
            dictTake(coinsWait, kg);
            if (daily > 0) dictTake(coinsWait, kd);
        }
        (void)gauntlet;
        if (g_gsmGetStat && g_gsmSetStat) {
            auto sub = [&](const char* key, int v) {
                if (v <= 0) return;
                g_gsmSetStat(gsm, key, std::max(0, g_gsmGetStat(gsm, key) - v));
            };
            if (hadStar && stars > 0 && stars <= 10) { sub(platformer ? "28" : "6", stars); r.stars = stars; }
            if (hadDemon) { sub("5", 1); r.demons = 1; }
            sub("12", r.coins);
        }
    } else {
        logf("uncomplete: GameStatsManager not found, only the level itself was reset");
    }
    r.ok = true;
    logf("uncomplete: level %d reset (stars -%d, demons -%d, coins -%d)", id, r.stars, r.demons, r.coins);
}

bool uncompleteLevelSEH(void* lvl, UncompleteResult* r) {
    __try {
        uncompleteLevel(lvl, *r);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("uncomplete failed (%08lx)", GetExceptionCode());
        return false;
    }
}

bool levelNameSEH(void* lvl, char* out, size_t n) {
    int id = 0, song = 0, track = 0;
    return readLevelSEH(lvl, &id, &song, &track, out, n);
}

// a small confirm box beside the column; returns true when the user says yes
bool confirmPopup(const char* id, const char* title, const char* body, const char* yes) {
    bool r = false;
    placePopup(id);
    if (ImGui::BeginPopup(id)) {
        popupTitle(title);
        ImGui::PushTextWrapPos(S(260));
        hintText(body);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(4)));
        if (pillButton("##yes", yes, ImVec2(S(125), S(26)), true)) { r = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(0, S(8));
        if (pillButton("##no", T("ut.cancel"), ImVec2(S(125), S(26)), false)) ImGui::CloseCurrentPopup();
        notePopupSize(id);
        ImGui::EndPopup();
    }
    return r;
}

void* g_uncLevel = nullptr;     // level picked when the confirm box opened
char g_uncName[96] = {};

void drawUtilityColumn(int index) {
    if (beginColumn("utility", "cat.utility", index)) {
        // buttons for each player
        playerButtonsRow(1);
        playerButtonsRow(2);
        ImGui::Dummy(ImVec2(0, S(2)));
        // level actions
        if (buttonRow("restart", T("ut.restart"), T("ut.restart.d"))) restartLevelAction();
        bool prac = false;
        bool havePrac = practiceStateSEH(&prac);
        if (havePrac) {
            if (toggleRow("practice", T("ut.practice"), T("ut.practice.d"), &prac)) setPracticeAction(prac);
        } else if (buttonRow("practice", T("ut.practice"), T("ut.practice.d"))) {
            showToast(T("ut.inlevel"));
        }
        if (buttonRow("uncomplete", T("ut.unc"), T("ut.unc.d"))) {
            g_uncLevel = findCurrentLevelSEH();
            if (!g_uncLevel) showToast(T("ut.unc.none"));
            else {
                if (!levelNameSEH(g_uncLevel, g_uncName, sizeof(g_uncName)) || !g_uncName[0]) std::snprintf(g_uncName, sizeof(g_uncName), "?");
                ImGui::OpenPopup("pop.unc");
            }
        }
        {
            char body[512];
            std::snprintf(body, sizeof(body), T("ut.unc.body"), g_uncName);
            if (confirmPopup("pop.unc", T("ut.unc"), body, T("ut.unc.yes"))) {
                // the level must still be the one on screen
                void* lvl = findCurrentLevelSEH();
                UncompleteResult r;
                if (!lvl || lvl != g_uncLevel) showToast(T("ut.unc.none"));
                else if (!uncompleteLevelSEH(lvl, &r) || (!r.ok && !r.notOnline)) showToast(T("ut.unc.fail"));
                else if (r.notOnline) showToast(T("ut.unc.online"));
                else showToast(T(g_playLayer ? "ut.unc.done" : "ut.unc.done2"));
            }
        }
        if (buttonRow("settings", T("ut.settings"), T("ut.settings.d"))) openGameSettingsSEH();
        ImGui::Dummy(ImVec2(0, S(2)));
        // folders
        wchar_t la[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        if (buttonRow("appdata", T("ut.appdata"), T("ut.appdata.d"))) openPath(std::wstring(la) + L"\\GeometryDash");
        if (buttonRow("res", T("ut.resources"), T("ut.resources.d"))) openPath(g_dir + L"Resources");
        if (buttonRow("game", T("ut.game"), T("ut.game.d"))) openPath(g_dir);
        if (buttonRow("cfg", T("ut.config"), T("ut.config.d"))) { cfgSave(); openPath(cfgPath()); }
        ImGui::Dummy(ImVec2(0, S(2)));
        if (buttonRow("crash", T("ut.crash"), T("ut.crash.d"))) ImGui::OpenPopup("pop.crash");
        if (confirmPopup("pop.crash", T("ut.crash"), T("ut.crash.body"), T("ut.crash.yes"))) {
            cfgSave();
            logf("crash game: asked from the menu");
            __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT: ends the process at once, nothing can catch it
        }
    }
    endColumn();
}

// Packs the columns into tidy stacks: as many stacks as fit across the screen, each column goes under the
// shortest stack, in the menu's own order. Only used when a size change left columns overlapping or off screen.
const char* const kColOrder[] = {"kuang", "player", "global", "startpos", "status", "jukebox", "extras", "utility"};
constexpr int kColCount = 8;
bool g_reflowPending = false, g_reflowForce = false;

void reflowColumns(bool force) {
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ColRect rect[kColCount];
    bool have[kColCount] = {};
    for (int k = 0; k < kColCount; k++) {
        auto it = g_colRects.find(kColOrder[k]);
        if (it == g_colRects.end() || g_frameNo - it->second.frame > 2) continue;
        rect[k] = it->second;
        have[k] = true;
    }
    bool bad = force;
    for (int k = 0; k < kColCount && !bad; k++) {
        if (!have[k]) continue;
        float h = rect[k].max.y - rect[k].min.y;
        if (rect[k].min.x < -1 || rect[k].max.x > disp.x + 1 || rect[k].min.y < -1 || (h <= disp.y && rect[k].max.y > disp.y + 1)) bad = true;
        for (int j = 0; j < k && !bad; j++)
            if (have[j] && rectsOverlap(rect[k].min, rect[k].max, rect[j].min, rect[j].max, S(2))) bad = true;
    }
    if (!bad) return;
    float w = S(g_colW), gap = S(12), margin = S(14);
    int stacks = std::clamp(static_cast<int>((disp.x - 2 * margin + gap) / (w + gap)), 1, kColCount);
    float top[kColCount];
    for (int s2 = 0; s2 < kColCount; s2++) top[s2] = margin;
    for (int k = 0; k < kColCount; k++) {
        if (!have[k]) continue;
        int best = 0;
        for (int s2 = 1; s2 < stacks; s2++) if (top[s2] < top[best] - 0.5f) best = s2;
        ImVec2 np(margin + best * (w + gap), top[best]);
        float h = rect[k].max.y - rect[k].min.y;
        top[best] += h + gap;
        g_pendingMove[kColOrder[k]] = np;
        g_colRects[kColOrder[k]] = {np, ImVec2(np.x + w, np.y + h), rect[k].frame};
        cfgSetFloat(std::string("pos.") + kColOrder[k] + ".x", np.x / g_scale);
        cfgSetFloat(std::string("pos.") + kColOrder[k] + ".y", np.y / g_scale);
    }
}

// ---------------------------------------------------------------------------
// Mods window (like Geode's mod list): every Kuang mod in the game folder, its state, on/off and its settings.
// kfe.dll and kopt.dll re-read their .ini twice per second, so writing it here applies almost at once.
// ---------------------------------------------------------------------------
bool g_modsOpen = false;
int g_modSel = 0;

struct IniKey { const wchar_t* key; const char* tkey; bool def; };
const IniKey kKfeKeys[] = {{L"smart_prediction", "kfe.smart", true}, {L"smooth_background", "kfe.bg", true},
    {L"smooth_rotation", "kfe.rot", true}, {L"smooth_objects", "kfe.obj", true}, {L"smooth_trails", "kfe.trails", true},
    {L"debug_log", "kfe.debug", false}};
const IniKey kOptKeys[] = {{L"flush_gpu", "opt.flush", false}, {L"no_vsync", "opt.vsync", false},
    {L"high_priority", "opt.prio", false}, {L"watchdog", "opt.watchdog", true}, {L"gpu_priority", "opt.gpu", false},
    {L"affinity", "opt.aff", false}, {L"no_boost", "opt.boost", false}, {L"timer", "opt.timer", false},
    {L"io_priority", "opt.io", false}};

struct ExtMod {
    const wchar_t* dll; const wchar_t* ini; const wchar_t* mapping; uint32_t magic; size_t statusSize;
    const IniKey* keys; size_t nkeys;
    std::wstring path; std::map<std::wstring, bool> v; bool interp = false; double readAt = -10;
    const void* view = nullptr; double mapTry = -10;
};
ExtMod g_extKfe{KFE_DLL_NAME, KFE_INI_NAME, KFE_STATUS_MAPPING, KFE_STATUS_MAGIC, sizeof(KfeStatus), kKfeKeys, 6};
ExtMod g_extOpt{KOPT_DLL_NAME, KOPT_INI_NAME, KOPT_STATUS_MAPPING, KOPT_STATUS_MAGIC, sizeof(KoptStatus), kOptKeys, 9};

void extRead(ExtMod& m, bool force) {
    double t = ImGui::GetTime();
    if (!force && t - m.readAt < 2.0) return; // the apps can change it too
    m.readAt = t;
    m.path = g_dir + m.ini;
    const wchar_t* p = m.path.c_str();
    m.v[L"enabled"] = GetPrivateProfileIntW(L"General", L"enabled", 1, p) != 0;
    for (size_t i = 0; i < m.nkeys; i++) m.v[m.keys[i].key] = GetPrivateProfileIntW(L"General", m.keys[i].key, m.keys[i].def, p) != 0;
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"General", L"mode", L"extrapolation", mode, 32, p);
    m.interp = _wcsicmp(mode, L"interpolation") == 0;
}
void extPut(ExtMod& m, const wchar_t* k, const wchar_t* val) {
    WritePrivateProfileStringW(L"General", k, val, m.path.c_str());
    m.readAt = ImGui::GetTime(); // keep what we just set, re-read later
}
// shared status block the dll publishes (nullptr = not running)
const void* extStatus(ExtMod& m) {
    if (!m.view && ImGui::GetTime() - m.mapTry > 2.0) {
        m.mapTry = ImGui::GetTime();
        if (HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, m.mapping)) {
            m.view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, m.statusSize);
            CloseHandle(h);
        }
    }
    if (!m.view) return nullptr;
    // both structs start with magic, version, pid, state, heartbeat
    auto u = static_cast<const uint32_t*>(m.view);
    uint64_t hb = *reinterpret_cast<const uint64_t*>(u + 4);
    if (u[0] != m.magic || u[2] != GetCurrentProcessId() || GetTickCount64() - hb > 3000) return nullptr;
    return m.view;
}

// 0 not installed, 1 active, 2 off, 3 starting, 4 error; msg = the dll's own text
int modState(int i, const char** msg) {
    *msg = nullptr;
    if (i == 0) return 1;
    if (i == 1) return g_cbfFailed ? 4 : (g_modCbf ? 1 : 2);
    ExtMod& m = i == 2 ? g_extKfe : g_extOpt;
    if (!GetModuleHandleW(m.dll)) return 0;
    extRead(m, false);
    if (!m.v[L"enabled"]) return 2;
    const void* s = extStatus(m);
    if (!s) return 3;
    uint32_t st = static_cast<const uint32_t*>(s)[3];
    if (i == 2) {
        *msg = static_cast<const KfeStatus*>(s)->message;
        return st == KFE_STATE_ACTIVE ? 1 : st == KFE_STATE_STARTING ? 3 : 4;
    }
    *msg = static_cast<const KoptStatus*>(s)->message;
    return st == KOPT_STATE_ACTIVE ? 1 : st == KOPT_STATE_STARTING ? 3 : 4;
}
ImVec4 modStateColor(int s) {
    switch (s) {
    case 1: return ImVec4(0.35f, 0.85f, 0.45f, 1);
    case 3: return ImVec4(0.95f, 0.80f, 0.30f, 1);
    case 4: return ImVec4(0.95f, 0.35f, 0.35f, 1);
    default: return kDim;
    }
}
const char* modStateText(int s) {
    static const char* k[] = {"mods.missing", "mods.on", "mods.off", "mods.wait", "mods.err"};
    return T(k[std::clamp(s, 0, 4)]);
}
int modsActiveCount() {
    int n = 0;
    const char* m;
    for (int i = 0; i < 4; i++) n += modState(i, &m) == 1;
    return n;
}

void extToggles(ExtMod& m) {
    for (size_t i = 0; i < m.nkeys; i++) {
        bool& b = m.v[m.keys[i].key];
        if (toggleRow(m.keys[i].tkey, T(m.keys[i].tkey), nullptr, &b)) extPut(m, m.keys[i].key, b ? L"1" : L"0");
    }
}

void drawModsWindow() {
    if (!g_modsOpen) return;
    std::string search;
    search.swap(g_search); // the search box filters the columns, not this window
    const char* names[] = {"Kuang Menu", "CBF", "Kuang Frame Extrapolation", "Kuang Optimizer"};
    const char* vers[] = {KMENU_VERSION, KMENU_VERSION, KFE_VERSION_STR, KOPT_VERSION_STR};
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImVec2 size(std::min(S(620), disp.x - S(20)), std::min(S(420), disp.y - S(20)));
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(10)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBg);
    ImGui::PushStyleColor(ImGuiCol_Border, U(g_accent, 0.55f));
    ImGui::Begin("##kmods", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // stays above the columns
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    // header
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(T("mods.btn"));
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%d/4 %s", modsActiveCount(), T("mods.on"));
    ImGui::SameLine(ImGui::GetWindowWidth() - S(90));
    if (pillButton("##close", T("mods.close"), ImVec2(S(80), S(22)), false)) g_modsOpen = false;
    ImGui::Separator();
    // list
    float listW = S(200);
    ImGui::BeginChild("##modlist", ImVec2(listW, 0), false);
    for (int i = 0; i < 4; i++) {
        const char* msg;
        int st = modState(i, &msg);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = S(44);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##mod", ImVec2(w, h))) g_modSel = i;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        auto dl = ImGui::GetWindowDrawList();
        ImVec2 q(p.x + w, p.y + h);
        if (g_modSel == i) gradRect(dl, p, q, U(g_accent, 0.55f), U(hueShift(g_accent, 38), 0.15f), S(6));
        else gradRect(dl, p, q, U(ImVec4(0.17f, 0.175f, 0.20f, 1)), U(ImVec4(0.12f, 0.125f, 0.145f, 1)), S(6));
        if (hov) dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, 0.06f)), S(6));
        ImGui::PushFont(g_fontBold);
        dl->AddText(ImVec2(p.x + S(10), p.y + S(5)), U(kText), names[i]);
        ImGui::PopFont();
        dl->AddCircleFilled(ImVec2(p.x + S(14), p.y + S(31)), S(3.5f), U(modStateColor(st)));
        dl->AddText(ImVec2(p.x + S(22), p.y + S(24)), U(modStateColor(st)), modStateText(st));
        ImGui::Dummy(ImVec2(0, S(3)));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // details of the selected mod
    ImGui::BeginChild("##moddet", ImVec2(0, 0), false);
    {
        int i = g_modSel;
        const char* msg;
        int st = modState(i, &msg);
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted(names[i]);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", vers[i]);
        ImGui::TextColored(modStateColor(st), "%s", modStateText(st));
        if (msg && *msg) { ImGui::SameLine(); ImGui::TextDisabled("- %s", msg); }
        ImGui::Dummy(ImVec2(0, S(4)));
        if (i == 0) {
            hintText(T("mods.menu.d"));
        } else if (i == 1) {
            hintText(T("mods.cbf.d"));
            if (toggleRow("modcbf", T("mods.enable"), nullptr, &g_modCbf)) cfgSetBool("mod.cbf", g_modCbf);
            if (g_cbfFailed) hintText(T("gl.cbf.fail"));
            hintText(T("mods.cbf.where"));
        } else {
            ExtMod& m = i == 2 ? g_extKfe : g_extOpt;
            hintText(T(i == 2 ? "mods.kfe.d" : "mods.opt.d"));
            if (st == 0) {
                hintText(T("mods.notinst"));
            } else {
                extRead(m, false);
                bool& en = m.v[L"enabled"];
                if (toggleRow("enabled", T("mods.enable"), nullptr, &en)) extPut(m, L"enabled", en ? L"1" : L"0");
                if (!en) ImGui::BeginDisabled();
                if (i == 2) {
                    caption(T("kfe.mode"));
                    float half = (ImGui::GetContentRegionAvail().x - S(4)) * 0.5f;
                    if (pillButton("##ex", T("kfe.extra"), ImVec2(half, S(24)), !m.interp)) { m.interp = false; extPut(m, L"mode", L"extrapolation"); }
                    ImGui::SameLine(0, S(4));
                    if (pillButton("##in", T("kfe.inter"), ImVec2(half, S(24)), m.interp)) { m.interp = true; extPut(m, L"mode", L"interpolation"); }
                    if (const void* s = extStatus(m)) {
                        auto k = static_cast<const KfeStatus*>(s);
                        ImGui::TextDisabled("%.0f FPS  -  %.2f ticks/frame", k->fps, k->ticksPerFrame);
                    }
                } else if (const void* s = extStatus(m)) {
                    auto k = static_cast<const KoptStatus*>(s);
                    ImGui::TextDisabled("%.0f FPS  -  %s", k->fps, k->plan);
                }
                extToggles(m);
                if (!en) ImGui::EndDisabled();
                if (i == 3) hintText(T("mods.opt.app"));
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
    g_search.swap(search);
}

void drawMenu() {
    utilTick();
    float target = g_menuOpen ? 1.f : 0.f;
    float dt = ImGui::GetIO().DeltaTime;
    g_menuT = g_anim ? g_menuT + (target - g_menuT) * std::min(1.f, dt * 14.f) : target;
    if (std::fabs(g_menuT - target) < 0.003f) g_menuT = target;
    if (g_menuT <= 0.001f) return;
    float e = 1 - (1 - g_menuT) * (1 - g_menuT); // ease out
    ImGui::GetStyle().Alpha = e;
    if (g_dim) {
        ImVec2 d = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), d, IM_COL32(8, 8, 14, static_cast<int>(120 * e)));
    }
    int i = 0;
    drawKuangColumn(i++);
    drawPlayerColumn(i++);
    drawGlobalColumn(i++);
    drawStartPosColumn(i++);
    drawStatusColumn(i++);
    drawJukeboxColumn(i++);
    drawExtrasColumn(i++);
    drawUtilityColumn(i++);
    // a new menu size or column width: every column follows (its saved spot is stored unscaled), and once the
    // sizes have settled, anything that now overlaps or falls off the screen is packed again
    static float lastSc = -1, lastCw = -1;
    static int quiet = 100;
    if (lastSc < 0) { lastSc = g_scale; lastCw = g_colW; }
    if (g_scale != lastSc || g_colW != lastCw) {
        lastSc = g_scale; lastCw = g_colW; quiet = 0; g_reflowPending = true;
        for (int k = 0; k < kColCount; k++) {
            std::string px = std::string("pos.") + kColOrder[k] + ".x", py = std::string("pos.") + kColOrder[k] + ".y";
            ImVec2 def = defaultColumnPos(k);
            g_pendingMove[kColOrder[k]] = (g_cfg.count(px) && g_cfg.count(py)) ? ImVec2(cfgFloat(px, 0) * g_scale, cfgFloat(py, 0) * g_scale) : def;
        }
    } else if (quiet < 100) {
        quiet++;
    }
    if (g_resetLayout) { g_reflowPending = g_reflowForce = true; quiet = 0; } // "Reset layout": tidy packing
    g_resetLayout = false;
    if (g_reflowPending && quiet >= 5 && g_dragCol.empty()) {
        g_reflowPending = false;
        reflowColumns(g_reflowForce);
        g_reflowForce = false;
    }
    g_frameNo++;
    static int settled = 0;
    settled = g_menuOpen && g_menuT > 0.99f ? settled + 1 : 0;
    // a column that grew (unfolded, wider, bigger text) and now touches another one moves to free space
    if (g_dragCol.empty() && settled > 3 && !g_reflowPending && quiet >= 8) {
        static const char* order[] = {"kuang", "player", "global", "startpos", "status", "jukebox", "extras", "utility"};
        for (int k = 1; k < 8; k++) {
            auto it = g_colRects.find(order[k]);
            if (it == g_colRects.end()) continue;
            ColRect r = it->second;
            bool hit = false;
            for (int j = 0; j < k && !hit; j++) {
                auto o = g_colRects.find(order[j]);
                if (o != g_colRects.end() && rectsOverlap(r.min, r.max, o->second.min, o->second.max, S(4))) hit = true;
            }
            if (!hit) continue;
            ImVec2 size(r.max.x - r.min.x, r.max.y - r.min.y);
            ImVec2 np = freeSpot(r.min, size, order[k]);
            g_pendingMove[order[k]] = np;
            g_colRects[order[k]] = {np, ImVec2(np.x + size.x, np.y + size.y), r.frame};
            cfgSetFloat(std::string("pos.") + order[k] + ".x", np.x / g_scale);
            cfgSetFloat(std::string("pos.") + order[k] + ".y", np.y / g_scale);
        }
    }
    drawModsWindow();
    ImGui::GetStyle().Alpha = 1.0f;
}

// ---------------------------------------------------------------------------
// ImGui setup and render (inside swapBuffers)
// ---------------------------------------------------------------------------
bool g_imguiReady = false;

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10; s.FrameRounding = 6; s.PopupRounding = 8; s.GrabRounding = 6; s.ChildRounding = 8;
    s.ItemSpacing = ImVec2(6, 3); s.FramePadding = ImVec2(8, 4); s.WindowPadding = ImVec2(10, 10);
    s.PopupBorderSize = 1; s.WindowBorderSize = 1;
    s.ScaleAllSizes(g_scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.145f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = kRow;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.21f, 0.25f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.3f, 1);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_Button] = kRow;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.23f, 0.27f, 1);
    c[ImGuiCol_ButtonActive] = g_accent;
    c[ImGuiCol_Header] = kRow;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.23f, 0.27f, 1);
    c[ImGuiCol_HeaderActive] = g_accent;
    c[ImGuiCol_CheckMark] = g_accent;
    c[ImGuiCol_SliderGrab] = g_accent;
    c[ImGuiCol_SliderGrabActive] = g_accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.35f);
}

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    char winA[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, win, -1, winA, sizeof(winA), nullptr, nullptr);
    std::string fonts = std::string(winA) + "\\Fonts\\";
    auto load = [&](const char* file, float size) -> ImFont* {
        std::string p = fonts + file;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg);
    };
    // fonts are built at 2x and scaled down, so they stay sharp when the interface scale goes up
    g_font = load("segoeui.ttf", 30);
    if (!g_font) { ImFontConfig c; c.SizePixels = 26; g_font = io.Fonts->AddFontDefault(&c); }
    g_fontBold = load("seguisb.ttf", 30);
    if (!g_fontBold) g_fontBold = g_font;
    g_fontLabel = load("seguibl.ttf", 40);
    if (!g_fontLabel) g_fontLabel = g_fontBold;
    io.FontDefault = g_font;
}

void initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigErrorRecoveryEnableTooltip = false; // never show ImGui debug messages to the player
    io.ConfigErrorRecoveryEnableDebugLog = false;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplOpenGL2_Init();
    loadFonts();
    applyStyle();
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hookedWndProc)));
    g_imguiReady = true;
    logf("ImGui ready (window %p)", static_cast<void*>(g_hwnd));
}

// OpenGL functions cocos uses that the GL2 backend doesn't save/restore
using PFNUseProgram = void(APIENTRY*)(GLuint);
using PFNBindBuffer = void(APIENTRY*)(GLenum, GLuint);
using PFNBindVAO = void(APIENTRY*)(GLuint);
using PFNVertexAttribEnable = void(APIENTRY*)(GLuint);
using PFNGetVertexAttribiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFNActiveTexture = void(APIENTRY*)(GLenum);
PFNUseProgram glUseProgram_ = nullptr;
PFNBindBuffer glBindBuffer_ = nullptr;
PFNBindVAO glBindVertexArray_ = nullptr;
PFNVertexAttribEnable glEnableVertexAttribArray_ = nullptr, glDisableVertexAttribArray_ = nullptr;
PFNGetVertexAttribiv glGetVertexAttribiv_ = nullptr;
PFNActiveTexture glActiveTexture_ = nullptr;

void loadGl() {
    glUseProgram_ = reinterpret_cast<PFNUseProgram>(wglGetProcAddress("glUseProgram"));
    glBindBuffer_ = reinterpret_cast<PFNBindBuffer>(wglGetProcAddress("glBindBuffer"));
    glBindVertexArray_ = reinterpret_cast<PFNBindVAO>(wglGetProcAddress("glBindVertexArray"));
    glEnableVertexAttribArray_ = reinterpret_cast<PFNVertexAttribEnable>(wglGetProcAddress("glEnableVertexAttribArray"));
    glDisableVertexAttribArray_ = reinterpret_cast<PFNVertexAttribEnable>(wglGetProcAddress("glDisableVertexAttribArray"));
    glGetVertexAttribiv_ = reinterpret_cast<PFNGetVertexAttribiv>(wglGetProcAddress("glGetVertexAttribiv"));
    glActiveTexture_ = reinterpret_cast<PFNActiveTexture>(wglGetProcAddress("glActiveTexture"));
}

constexpr GLenum kGL_CURRENT_PROGRAM = 0x8B8D, kGL_ARRAY_BUFFER = 0x8892, kGL_ELEMENT_ARRAY_BUFFER = 0x8893,
    kGL_ARRAY_BUFFER_BINDING = 0x8894, kGL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895, kGL_VERTEX_ARRAY_BINDING = 0x85B5,
    kGL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622, kGL_ACTIVE_TEXTURE = 0x84E0, kGL_TEXTURE0 = 0x84C0;

void renderImGui() {
    // put GL in the plain state the GL2 backend expects, then give cocos its state back
    GLint prog = 0, abuf = 0, ebuf = 0, vao = 0, activeTex = kGL_TEXTURE0;
    glGetIntegerv(kGL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(kGL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(kGL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
    glGetIntegerv(kGL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(kGL_ACTIVE_TEXTURE, &activeTex);
    GLint attrOn[8] = {};
    if (glBindVertexArray_) glBindVertexArray_(0);
    for (GLuint i = 0; i < 8 && glGetVertexAttribiv_; i++) {
        glGetVertexAttribiv_(i, kGL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrOn[i]);
        if (attrOn[i] && glDisableVertexAttribArray_) glDisableVertexAttribArray_(i);
    }
    if (glUseProgram_) glUseProgram_(0);
    if (glBindBuffer_) { glBindBuffer_(kGL_ARRAY_BUFFER, 0); glBindBuffer_(kGL_ELEMENT_ARRAY_BUFFER, 0); }
    if (glActiveTexture_) glActiveTexture_(kGL_TEXTURE0);

    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    if (glActiveTexture_) glActiveTexture_(static_cast<GLenum>(activeTex));
    if (glBindBuffer_) { glBindBuffer_(kGL_ARRAY_BUFFER, abuf); glBindBuffer_(kGL_ELEMENT_ARRAY_BUFFER, ebuf); }
    if (glUseProgram_) glUseProgram_(prog);
    if (glBindVertexArray_) glBindVertexArray_(vao);
    for (GLuint i = 0; i < 8; i++)
        if (attrOn[i] && glEnableVertexAttribArray_) glEnableVertexAttribArray_(i);
}

FnV o_swapBuffers = nullptr;

void safeModeTickSEH() {
    __try {
        safeModeTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// --- speedhack: cocos scheduler time scale + FMOD master pitch
void* (*g_getScheduler)(void*) = nullptr;
void (*g_setTimeScale)(void*, float) = nullptr;
using FmodUpdate = int (*)(void*);
using FmodGetMaster = int (*)(void*, void**);
using FmodSetPitch = int (*)(void*, float);
FmodUpdate o_fmodUpdate = nullptr;
FmodGetMaster g_fmodGetMaster = nullptr;
FmodSetPitch g_fmodSetPitch = nullptr;
void* g_fmodSystem = nullptr;
float g_appliedSpeed = 1.0f, g_appliedPitch = 1.0f;

int h_fmodUpdate(void* sys) {
    g_fmodSystem = sys;
    float pitch = (g_speedOn && g_speedAudio) ? std::clamp(g_speed, 0.01f, 10.0f) : 1.0f; // audio can't follow 0.0001x / 100x
    if (pitch != g_appliedPitch && g_fmodGetMaster && g_fmodSetPitch) {
        void* master = nullptr;
        if (g_fmodGetMaster(sys, &master) == 0 && master) g_fmodSetPitch(master, pitch);
        g_appliedPitch = pitch;
    }
    return o_fmodUpdate(sys);
}

// Fallback when CCDirector::getScheduler / CCScheduler::setTimeScale are not exported by this
// libcocos2d.dll: scale the delta time ourselves inside CCScheduler::update.
bool g_speedViaHook = false;
float g_hookScale = 1.0f;
void (*o_schedUpdate)(void*, float) = nullptr;
void h_schedUpdate(void* self, float dt) { o_schedUpdate(self, dt * g_hookScale); }

void applySpeed() {
    float sp = g_speedOn ? std::clamp(g_speed, kSpeedMin, kSpeedMax) : 1.0f;
    g_hookScale = sp;
    if (g_speedViaHook || !g_getScheduler || !g_setTimeScale) return;
    // set again every frame while on (the game may reset it), and once more when going back to 1x
    if (sp == 1.0f && g_appliedSpeed == 1.0f) return;
    void* dir = g_sharedDirector();
    void* sch = dir ? g_getScheduler(dir) : nullptr;
    if (!sch) return;
    g_setTimeScale(sch, sp);
    if (sp != g_appliedSpeed) logf("speedhack: time scale %.4f", sp);
    g_appliedSpeed = sp;
}

// --- noclip: PlayLayer::destroyPlayer(PlayerObject*, GameObject*)
using DestroyPlayer = void (*)(void*, void*, void*);
DestroyPlayer o_destroyPlayer = nullptr;
void h_destroyPlayer(void* self, void* player, void* object) {
    if (g_trajSim && player && (player == g_ghost[0] || player == g_ghost[1])) { trajGhostDied(self, player, object); return; } // only the copy
    {
        void* spike = at<void*>(self, kAnticheatSpike);
        bool alreadyDead = at<bool>(self, 0x334a); // GJBaseGameLayer::m_playerDied
        if (!g_noclip && object != spike && !alreadyDead) runEnded(self, percentSEH(self));
    }
    if (g_noclip) {
        // the level's anti-cheat spike must still "kill" once, or GD flags the attempt
        void* spike = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + kAnticheatSpike);
        if (!object || object != spike) {
            g_ncHit = true;
            g_ncHitCalls++;
            g_ncLastHitT = now();
            if (g_ncLog < 12) logf("noclip: hit #%d (object %p)", ++g_ncLog, object);
            return;
        }
    }
    o_destroyPlayer(self, player, object);
}

// PlayLayer virtual "should the hitboxes be drawn now?". The game answers yes in practice mode with the
// option on, or after a real death with "Hitboxes on Death" on. Noclip never reaches that death, so while
// g_hbForce is set we answer yes ourselves; the game then draws them with its own code.
using ShouldDraw = bool (*)(void*);
ShouldDraw o_shouldDraw = nullptr;
bool h_shouldDraw(void* self) {
    if (g_hbForce) return true;
    return o_shouldDraw(self);
}

void toggleDebugDrawSEH(void* layer) {
    __try {
        if (layer && g_toggleDebugDraw) g_toggleDebugDraw(layer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Percentage Bugfix: GJBaseGameLayer counts the percentage by time (+0x3e8, half-steps) against the
// level's recorded length (GJGameLevel +0x538, steps). From a start pos the game sets the counter from the
// time that point is worth; when that goes wrong the counter starts at 0 or past the end.
int g_pctCheck = 0;               // frames left to check after a reset
constexpr uintptr_t kPlLevel = 0x878, kLvlTimestamp = 0x538, kLevelLength = 0x3318, kPctCounter = 0x3e8;
constexpr uintptr_t kPlayerNodePos = 0x4c; // CCNode::m_obPosition

void pctFixTick() {
    void* pl = g_playLayer;
    if (!pl || !at<void*>(pl, 0x3270 /* m_startPosObject */)) { g_pctCheck = 0; return; }
    void* lvl = at<void*>(pl, kPlLevel);
    int steps = lvl ? at<int>(lvl, kLvlTimestamp) : 0;
    float len = at<float>(pl, kLevelLength);
    void* p1 = at<void*>(pl, 0xdb8 /* m_player1 */);
    if (steps <= 0 || len <= 1.0f || !p1) { g_pctCheck = 0; return; } // percentage by position: nothing to fix
    float pPos = std::clamp(at<float>(p1, kPlayerNodePos) / len * 100.0f, 0.0f, 100.0f);
    int counter = at<int>(pl, kPctCounter);
    float pTime = counter / (2.0f * steps) * 100.0f;
    bool bug = pTime > 100.5f || pTime < -0.5f || (pTime < 0.5f && pPos >= 3.0f) || (pTime >= 99.5f && pPos <= 97.0f);
    if (!bug) return;
    at<int>(pl, kPctCounter) = static_cast<int>(std::lround(pPos / 100.0f * 2.0f * steps));
    logf("percentage bugfix: start pos at %.1f%% showed %.1f%% (counter %d of %d): fixed", pPos, pTime, counter, 2 * steps);
    g_pctCheck = 0;
}

void pctFixTickSEH() {
    if (!g_pctFix || g_pctCheck <= 0) return;
    g_pctCheck--;
    __try {
        pctFixTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pctCheck = 0;
        static bool logged = false;
        if (!logged) { logf("percentage bugfix failed (%08lx)", GetExceptionCode()); logged = true; }
    }
}

// --- new attempt: Safe Mode is decided again
using ResetLevel = void (*)(void*);
ResetLevel o_resetLevel = nullptr;
void h_resetLevel(void* self) {
    g_hbForce = false; // the game hides the hitboxes itself on a new attempt
    g_cheatFinal = -1; // new attempt: the cheat indicator is live again
    smartStartPosBeforeReset(self);
    o_resetLevel(self);
    g_pctCheck = 4; // the start pos counter is set during the reset and the first frames
    runStarted(self);
    if (self == g_safeLayer) {
        g_safeAttempt = g_safeManual || cheatsActive();
        // the game's own "test mode" is on exactly when you play from a start pos
        g_safeOrigTest = at<void*>(self, kStartPosObj) != nullptr;
        if (!g_safeAttempt) at<bool>(self, kTestMode) = g_safeOrigTest;
    }
}

struct GameHook { const char* name; uintptr_t rva; const unsigned char* sig; size_t sigLen; void* detour; void** orig; };
const unsigned char kSigDestroy[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
const unsigned char kSigReset[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x55, 0x57};
const unsigned char kSigAudio[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x60};

bool sigMatchSEH(const unsigned char* at, const unsigned char* sig, size_t n) {
    __try {
        return std::memcmp(at, sig, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// a function we only call: its first bytes must match (or start with a jump: another mod hooked it, still fine)
void* gameFn(uintptr_t base, uintptr_t rva, std::initializer_list<unsigned char> sig, const char* name) {
    auto p = reinterpret_cast<const unsigned char*>(base + rva);
    std::vector<unsigned char> v(sig);
    if (sigMatchSEH(p, v.data(), v.size())) return const_cast<unsigned char*>(p);
    unsigned char jmp = 0xE9;
    if (sigMatchSEH(p, &jmp, 1)) return const_cast<unsigned char*>(p);
    logf("%s does not match: the options that use it are off", name);
    return nullptr;
}

void resolveGameFunctions(uintptr_t base) {
    g_gmShared = reinterpret_cast<void* (*)()>(gameFn(base, 0x2762c0, {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05}, "GameManager::sharedState"));
    g_gmGetVar = reinterpret_cast<bool (*)(void*, const char*)>(gameFn(base, 0x27e5c0, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x81, 0xec, 0x80, 0, 0, 0}, "GameManager::getGameVariable"));
    g_gmSetVar = reinterpret_cast<void (*)(void*, const char*, bool)>(gameFn(base, 0x27e320, {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54}, "GameManager::setGameVariable"));
    g_updateDebugDrawSettings = reinterpret_cast<void (*)(void*)>(gameFn(base, 0x505c40, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9}, "PlayLayer::updateDebugDrawSettings"));
    g_resetLevelFromStart = reinterpret_cast<void (*)(void*)>(gameFn(base, 0x518a50, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}, "PlayLayer::resetLevelFromStart"));
    g_updateCamera = reinterpret_cast<void (*)(void*, float)>(gameFn(base, 0x345c60, {0x48, 0x8b, 0xc4, 0xf3, 0x0f, 0x11, 0x48, 0x10}, "GJBaseGameLayer::updateCamera"));
    g_updateTestModeLabel = reinterpret_cast<void (*)(void*)>(gameFn(base, 0x505ed0, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x55}, "PlayLayer::updateTestModeLabel"));
    g_getPercent = reinterpret_cast<float (*)(void*)>(gameFn(base, 0x512300, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x7e, 0x31, 0, 0, 0}, "PlayLayer::getCurrentPercent"));
    g_startMusic = reinterpret_cast<void (*)(void*)>(gameFn(base, 0x51aed0, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9}, "PlayLayer::startMusic"));
    g_handleButton = reinterpret_cast<void (*)(void*, bool, int, bool)>(gameFn(base, 0x33c5b0, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83}, "GJBaseGameLayer::handleButton"));
    g_togglePractice = reinterpret_cast<void (*)(void*, bool)>(gameFn(base, 0x519ff0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6, 0xfa, 0x48, 0x8b, 0xd9}, "PlayLayer::togglePracticeMode"));
    g_openOptions = reinterpret_cast<void (*)(void*, bool)>(gameFn(base, 0x474c40, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57}, "OptionsLayer opener"));
    g_gsmShared = reinterpret_cast<void* (*)()>(gameFn(base, 0x2d6890, {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05}, "GameStatsManager::sharedState"));
    g_gsmGetStat = reinterpret_cast<int (*)(void*, const char*)>(gameFn(base, 0x2db160, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0x05}, "GameStatsManager::getStat"));
    g_gsmSetStat = reinterpret_cast<void (*)(void*, const char*, int)>(gameFn(base, 0x2db290, {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x60}, "GameStatsManager::setStat"));
    g_platChecksum = reinterpret_cast<int (*)(int, int, int, int, int, int, int, int)>(gameFn(base, 0x2649c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x8b, 0xd2, 0x8b, 0xd9}, "GJGameLevel platformer checksum"));
    if (HMODULE cc = GetModuleHandleW(L"libcocos2d.dll")) {
        g_dictObject = reinterpret_cast<void* (*)(void*, const std::string&)>(GetProcAddress(cc, "?objectForKey@CCDictionary@cocos2d@@QEAAPEAVCCObject@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));
        g_dictRemove = reinterpret_cast<void (*)(void*, const std::string&)>(GetProcAddress(cc, "?removeObjectForKey@CCDictionary@cocos2d@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));
    }
}

void installGameHooks() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    GameHook hooks[] = {
        {"PlayLayer::destroyPlayer", 0x512390, kSigDestroy, sizeof(kSigDestroy), reinterpret_cast<void*>(&h_destroyPlayer), reinterpret_cast<void**>(&o_destroyPlayer)},
        {"PlayLayer::resetLevel", 0x518c00, kSigReset, sizeof(kSigReset), reinterpret_cast<void*>(&h_resetLevel), reinterpret_cast<void**>(&o_resetLevel)},
        {"GJGameLevel::getAudioFileName", 0x264790, kSigAudio, sizeof(kSigAudio), reinterpret_cast<void*>(&h_getAudioFileName), reinterpret_cast<void**>(&o_getAudioFileName)},
    };
    g_base = base;
    resolveGameFunctions(base);
    bool ok = true;
    for (auto& h : hooks)
        if (!sigMatchSEH(reinterpret_cast<const unsigned char*>(base + h.rva), h.sig, h.sigLen)) {
            logf("%s does not match this Geometry Dash version: cheats disabled", h.name);
            ok = false;
        }
    if (!ok) return;
    for (auto& h : hooks) {
        void* target = reinterpret_cast<void*>(base + h.rva);
        if (MH_CreateHook(target, h.detour, h.orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
            logf("could not hook %s", h.name);
            return;
        }
    }
    g_hooksOk = true;
    logf("game hooks ready (noclip, safe mode, smart start pos, jukebox)");
    // optional: hitboxes on a Noclip hit. If anything does not match, only this feature stays off.
    static const unsigned char kSigShouldDraw[] = {0x80, 0xb9, 0x88, 0x38, 0x00, 0x00, 0x00, 0x74, 0x09, 0x80, 0xb9, 0xe0};
    g_toggleDebugDraw = reinterpret_cast<void (*)(void*)>(gameFn(base, 0x505e40, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01}, "PlayLayer::toggleDebugDraw"));
    void* sd = reinterpret_cast<void*>(base + 0x505bc0);
    if (g_toggleDebugDraw && sigMatchSEH(reinterpret_cast<const unsigned char*>(sd), kSigShouldDraw, sizeof(kSigShouldDraw)) &&
        MH_CreateHook(sd, reinterpret_cast<void*>(&h_shouldDraw), reinterpret_cast<void**>(&o_shouldDraw)) == MH_OK && MH_EnableHook(sd) == MH_OK) {
        g_hbHooked = true;
        logf("hitboxes on Noclip hit: ready");
    } else {
        logf("hitboxes on Noclip hit: not available in this version");
    }
}

// ---- Show Trajectory: an invisible copy of the player runs ahead (once holding, once released) and the
// path is drawn in the level. Done our own way, with the game's own physics:
//  - the copy is a real PlayerObject made by the game (hidden, not a "play layer" player, so it never counts
//    jumps or plays effects), and it copies the real player every time the physics moves (mode, gravity,
//    size, speed, velocity, position...);
//  - each step is the game's own: PlayerObject::update, GJBaseGameLayer::checkCollisions, updateRotation;
//  - while it runs, the collision check only gets blocks, spikes and slopes. Portals (mode, gravity, size,
//    speed) are applied by us to the copy only, so no object in the level gets "used" and nothing else in the
//    level reacts (no triggers, no events, no coins, no death of the real player).
using FnCreatePlayer = void* (*)(int, int, void*, void*, bool);
using FnCopyAttr = void (*)(void*, void*);
using FnCheckCol = int (*)(void*, void*, float, bool);
using FnColObjs = void (*)(void*, void*, void*, int, float);
using FnPF = void (*)(void*, float);
using FnPButton = bool (*)(void*, int);
using FnToggle = void (*)(void*, bool, bool);
using FnBoolEvt = bool (*)(void*, void*, void*);
using FnGameEvt = void (*)(void*, int, int, int);
using FnAddPoint = void (*)(void*, uintptr_t); // CCPoint is passed through untouched
FnCreatePlayer g_poCreate = nullptr;
FnCopyAttr g_poCopy = nullptr;
FnCheckCol g_checkCol = nullptr;
FnColObjs o_colObjs = nullptr;
FnPF g_poRotate = nullptr;
FnPButton g_poPush = nullptr, g_poRelease = nullptr;
FnToggle g_poMode[7] = {};                 // fly, bird, roll, dart, robot, spider, swing
FnToggle g_poGravity = nullptr, g_poScale = nullptr;
void (*g_poTimeMod)(void*, float, bool) = nullptr;
void (*g_poBump)(void*, float, int, bool, void*) = nullptr;   // PlayerObject::bumpPlayer
void (*g_poReverse)(void*, void*) = nullptr;                  // PlayerObject::reversePlayer
void (*g_poUpdScale)(void*) = nullptr;                        // PlayerObject::updatePlayerScale
FnBoolEvt o_canActivate = nullptr;
FnGameEvt o_gameEvent = nullptr;
FnAddPoint o_addPoint = nullptr;
void* (*g_drawCreate)() = nullptr;
void (*g_drawClear)(void*) = nullptr;
bool (*g_drawSegment)(void*, const float*, const float*, float, const float*) = nullptr;
void (*g_drawNoArea)(void*) = nullptr;
void (*g_dictClear)(void*) = nullptr;
void (*g_nodeGetPos)(void*, float*, float*) = nullptr;

bool g_traj = false, g_trajP1 = true, g_trajP2 = true, g_trajReady = false;
bool g_trajTrail[2] = {false, false};   // per player: the hitbox at every point of the path
// per player: colour of the path that matches what you are doing now (hold) and of the other one (release)
float g_trajCol[2][2][3] = {{{0, 1, 0}, {0, 0.45f, 0}}, {{0, 1, 0}, {0, 0.45f, 0}}};
float g_trajLen = 2.0f;         // seconds ahead (game steps: 240 per second, the same size the game uses)
void* g_trajGl = nullptr;       // the level the copies and the drawing belong to
void* g_trajDraw = nullptr;
double g_trajLastT[2] = {-1, -1};
float g_trajLastP[2][2] = {};
bool g_trajHad = false;

constexpr uintptr_t kPoPlayLayerFlag = 0x7ec, kPoDead = 0x9c8, kPoUpside = 0x9c7, kPoParent = 0xc30, kPoTime = 0xaa8;
constexpr uintptr_t kObjType = 0x3a0, kObjOff1 = 0x28e, kObjOff2 = 0x3d2;
constexpr int kVtUpdate = 4, kVtVisible = 42, kVtAddChild = 57, kVtObjRect = 148;

template <class F> F vfn(void* o, int slot) { return reinterpret_cast<F>((*reinterpret_cast<void***>(o))[slot]); }
void setVisibleV(void* n, bool v) { if (n) vfn<void (*)(void*, bool)>(n, kVtVisible)(n, v); }
const float* objRect(void* o) { return vfn<const float* (*)(void*)>(o, kVtObjRect)(o); }
bool rectsHit(const float* a, const float* b) {
    return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}
bool isGhost(void* p) { return g_trajSim && p && (p == g_ghost[0] || p == g_ghost[1]); }

// portals the copy already went through in this run (so a gravity toggle does not flip every step)
void* g_trajUsed[64];
int g_trajUsedN = 0;
bool trajUse(void* o) {
    for (int i = 0; i < g_trajUsedN; i++) if (g_trajUsed[i] == o) return false;
    if (g_trajUsedN < 64) g_trajUsed[g_trajUsedN++] = o;
    return true;
}

void ghostMode(void* p, int mode) { // -1 cube, else index into g_poMode
    for (int m = 0; m < 7; m++) if (m != mode && g_poMode[m]) g_poMode[m](p, false, false);
    if (mode >= 0 && g_poMode[mode]) g_poMode[mode](p, true, false);
}

// a portal/changer touched by the copy: same result as the game, only on the copy
void ghostTouch(void* p, void* o) {
    int type = at<int>(o, kObjType), id = at<int>(o, kObjectID);
    float speed = 0;
    switch (id) {
    case 200: speed = 0.7f; break;
    case 201: speed = 0.9f; break;
    case 202: speed = 1.1f; break;
    case 203: speed = 1.3f; break;
    case 1334: speed = 1.6f; break;
    }
    bool isPad = type == 8 || type == 9 || type == 10 || type == 34 || type == 44;
    bool isPortal = isPad || speed > 0 || type == 3 || type == 4 || type == 42 || type == 5 || type == 6 || type == 16 ||
        type == 19 || type == 26 || type == 27 || type == 33 || type == 41 || type == 17 || type == 18;
    if (!isPortal || at<bool>(o, kObjOff1) || at<bool>(o, kObjOff2)) return;
    if (!rectsHit(objRect(p), objRect(o)) || !trajUse(o)) return;
    if (speed > 0) { if (g_poTimeMod) g_poTimeMod(p, speed, false); return; }
    if (isPad) {
        // the game's own pad strength (GJBaseGameLayer's pad code): pink and red depend on the vehicle and size
        float mod = 1.0f;
        bool mini = at<float>(p, 0x9f8) < 1.0f;
        if (type == 9) {
            if (at<bool>(p, kPoShip)) mod = 0.35f;
            else if (at<bool>(p, kPoUfo)) mod = 0.4f;
            else if (at<bool>(p, kPoBall) || at<bool>(p, kPoSpider)) mod = 0.7f;
            else mod = 0.65f;
        } else if (type == 34) {
            if (at<bool>(p, kPoShip)) mod = mini ? 0.95f : 0.63f;
            else if (at<bool>(p, kPoUfo)) mod = mini ? 0.98f : 0.6f;
            else mod = 1.25f;
        }
        if (at<bool>(o, 0x704) && g_poReverse) g_poReverse(p, o); // reverse pad
        if (g_poBump) g_poBump(p, mod, type, at<bool>(o, 0x41c), o);
        return;
    }
    switch (type) {
    case 3: if (g_poGravity) g_poGravity(p, true, true); break;
    case 4: if (g_poGravity) g_poGravity(p, false, true); break;
    case 42: if (g_poGravity) g_poGravity(p, !at<bool>(p, kPoUpside), true); break;
    case 6: ghostMode(p, -1); break;
    case 5: ghostMode(p, 0); break;
    case 19: ghostMode(p, 1); break;
    case 16: ghostMode(p, 2); break;
    case 26: ghostMode(p, 3); break;
    case 27: ghostMode(p, 4); break;
    case 33: ghostMode(p, 5); break;
    case 41: ghostMode(p, 6); break;
    case 17: if (g_poScale) g_poScale(p, false, false); break;
    case 18: if (g_poScale) g_poScale(p, true, false); break;
    }
}

// GJBaseGameLayer::collisionCheckObjects: for the copy, only solids / spikes / slopes go to the game
std::vector<void*> g_trajKeep;
void h_colObjs(void* gl, void* player, void* vec, int count, float dt) {
    if (!isGhost(player) || !vec) { o_colObjs(gl, player, vec, count, dt); return; }
    void** items = *reinterpret_cast<void***>(vec);
    g_trajKeep.clear();
    for (int i = 0; i < count; i++) {
        void* o = items[i];
        if (!o) continue;
        int type = at<int>(o, kObjType);
        if (type == 0 || type == 2 || type == 25 || type == 48) g_trajKeep.push_back(o);
        else ghostTouch(player, o);
    }
    if (g_trajKeep.empty()) return;
    void* fake[3] = {g_trajKeep.data(), g_trajKeep.data() + g_trajKeep.size(), g_trajKeep.data() + g_trajKeep.size()};
    o_colObjs(gl, player, fake, static_cast<int>(g_trajKeep.size()), dt);
}
// nothing in the level may be activated by the copy, and it fires no level events
bool h_canActivate(void* gl, void* player, void* obj) { return isGhost(player) ? false : o_canActivate(gl, player, obj); }
void h_gameEvent(void* gl, int ev, int a, int b) { if (!g_trajSim) o_gameEvent(gl, ev, a, b); }
void h_addPoint(void* streak, uintptr_t pt) { if (!g_trajSim) o_addPoint(streak, pt); }

void trajDropSEH() {
    __try {
        for (auto& g : g_ghost) if (g) { g_objRelease(g); g = nullptr; }
        if (g_trajDraw) { g_drawClear(g_trajDraw); g_objRelease(g_trajDraw); g_trajDraw = nullptr; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ghost[0] = g_ghost[1] = g_trajDraw = nullptr;
    }
    g_trajGl = nullptr;
    g_trajLastT[0] = g_trajLastT[1] = -1;
}

bool trajSetup(void* gl) {
    void* p1 = at<void*>(gl, kPlayer1);
    if (!p1) return false;
    void* layer = at<void*>(p1, kPoParent);
    if (!layer) return false;
    for (auto& g : g_ghost) {
        g = g_poCreate(1, 1, gl, layer, false);
        if (!g) return false;
        g_objRetain(g);
        vfn<void (*)(void*, void*, int)>(layer, kVtAddChild)(layer, g, -10);
        setVisibleV(g, false);
    }
    g_trajDraw = g_drawCreate();
    if (!g_trajDraw) return false;
    g_objRetain(g_trajDraw);
    if (g_drawNoArea) g_drawNoArea(g_trajDraw);
    vfn<void (*)(void*, void*, int)>(layer, kVtAddChild)(layer, g_trajDraw, 9999);
    g_trajGl = gl;
    logf("show trajectory: ready in this level");
    return true;
}

bool holdingButton(void* p, int button) {
    void* head = at<void*>(p, 0xbc0);
    if (!head) return false;
    void* n = at<void*>(head, 0x8); // root
    for (int guard = 0; n && !at<bool>(n, 0x19) && guard < 64; guard++) {
        int key = at<int>(n, 0x1c);
        if (key == button) return at<bool>(n, 0x20);
        n = at<void*>(n, key > button ? 0x0 : 0x10);
    }
    return false;
}

// the copy touched something deadly. The level's anti-cheat spike is not a real hazard: GD moves it onto the
// player to test that deaths work, so the copy must ignore it (it was ending paths in mid air).
int g_trajDeathLogs = 0;
void trajGhostDied(void* gl, void* ghost, void* object) {
    if (object && object == at<void*>(gl, kAnticheatSpike)) return;
    g_trajDead = true;
    if (g_trajDeathLogs < 15 && object) {
        g_trajDeathLogs++;
        float gx = 0, gy = 0;
        g_nodeGetPos(ghost, &gx, &gy);
        logf("[trajectory] copy died at %.1f, %.1f on object id %d type %d at %.1f, %.1f", gx, gy, at<int>(object, kObjectID),
            at<int>(object, kObjType), at<double>(object, kObjX), at<double>(object, kObjX + 8));
    }
}

void hideGhostBits(void* g) {
    setVisibleV(g, false);
    for (uintptr_t o : {0x7a8, 0x7b0, 0x7b8}) setVisibleV(at<void*>(g, o), false); // its trails
}

void drawRect(float x, float y, float w, float h, const float* c) {
    float p[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    for (int k = 0; k < 4; k++) g_drawSegment(g_trajDraw, p[k], p[(k + 1) % 4], 0.5f, c);
}
// the player's hitboxes measured by the game itself, exactly as QOLMod's hitboxes take them:
// GameObject::getObjectRect(scaleX, scaleY) with the vehicle size for the outer one and 0.3 for the inner one.
// So every vehicle, mini or not, gets its real size.
constexpr int kVtObjRectScaled = 147;
void scaledRect(void* g, float sx, float sy, float* out) {
    vfn<void* (*)(void*, float*, float, float)>(g, kVtObjRectScaled)(g, out, sx, sy);
}
void drawHitboxes(void* g, const float* outer, const float* inner) {
    float r[4];
    float size = at<float>(g, 0x9f8); // vehicle size (1 normal, 0.6 mini)
    scaledRect(g, size, size, r);
    drawRect(r[0], r[1], r[2], r[3], outer);
    scaledRect(g, 0.3f, 0.3f, r);
    drawRect(r[0], r[1], r[2], r[3], inner);
}

// one run of the copy from the real player; draws the path
void trajRun(void* gl, void* real, void* g, bool hold, bool nowHolding, const float* col, bool trail) {
    g_poCopy(g, real);
    // what copyAttributes leaves out but the physics needs
    at<bool>(g, 0x9c9) = at<bool>(real, 0x9c9);          // on ground
    at<bool>(g, 0xa14) = at<bool>(real, 0xa14);          // on ground (2)
    at<double>(g, 0x9b0) = at<double>(real, 0x9b0);      // fall speed
    at<bool>(g, 0x9cb) = at<bool>(real, 0x9cb);          // sideways
    at<bool>(g, 0xb78) = at<bool>(real, 0xb78);          // platformer
    at<double>(g, 0xb00) = at<double>(real, 0xb00);      // platformer x velocity
    std::memcpy(reinterpret_cast<char*>(g) + 0xae4, reinterpret_cast<char*>(real) + 0xae4, 0xafc - 0xae4);   // player states
    std::memcpy(reinterpret_cast<char*>(g) + 0xb7c, reinterpret_cast<char*>(real) + 0xb7c, 0xb9d - 0xb7c);   // gravity mod, forces
    at<bool>(g, kPoDead) = false;
    // copyAttributes changes mini size with the game's grow/shrink animation, which only plays on the real
    // player: put the size on at once, or the copy's hitboxes and collisions would have the wrong size
    g_poUpdScale(g);
    // a robot already holding keeps its jump going (a new press would not be the same jump)
    bool keep = hold && nowHolding && at<bool>(real, kPoRobot);
    if (!keep) {
        g_poRelease(g, 1);
        if (hold) g_poPush(g, 1);
    }
    hideGhostBits(g);
    g_trajDead = false;
    g_trajUsedN = 0;
    float prev[2], cur[2];
    g_nodeGetPos(g, &prev[0], &prev[1]);
    // the same step the game uses: 240 per second (dt 0.25 in the game's units), smaller in slow speed
    float tm = at<float>(gl, 0x330);
    const float dt = 0.25f * std::clamp(tm > 0 ? tm : 1.0f, 0.05f, 1.0f);
    int steps = static_cast<int>(g_trajLen * 240.0f);
    for (int i = 0; i < steps; i++) {
        for (uintptr_t o = 0x5b0; o <= 0x5c8; o += 8) if (void* d = at<void*>(g, o)) g_dictClear(d); // collision logs
        vfn<void (*)(void*, float)>(g, kVtUpdate)(g, dt);
        g_checkCol(gl, g, dt, false);
        g_poRotate(g, dt);
        g_poUpdScale(g);
        g_nodeGetPos(g, &cur[0], &cur[1]);
        g_drawSegment(g_trajDraw, prev, cur, 0.6f, col);
        if (trail) {
            float tc[4] = {col[0], col[1], col[2], 0.35f}, ti[4] = {0.25f, 0.55f, 1, 0.3f};
            drawHitboxes(g, tc, ti);
        }
        prev[0] = cur[0];
        prev[1] = cur[1];
        if (g_trajDead || at<bool>(g, kPoDead)) {
            // where it dies: the copy's real hitboxes there, as the game measures them (they change with the
            // vehicle and with mini size): outer in red, inner (the one that dies on blocks) in blue
            static const float red[4] = {1, 0.2f, 0.2f, 1}, blue[4] = {0.25f, 0.55f, 1, 1};
            drawHitboxes(g, red, blue);
            hideGhostBits(g);
            return;
        }
    }
    // alive at the end of the path: its hitbox there, faded in the path's colour
    float faded[4] = {col[0], col[1], col[2], 0.55f}, fadedIn[4] = {col[0], col[1], col[2], 0.35f};
    drawHitboxes(g, faded, fadedIn);
    hideGhostBits(g);
}

void trajTick(void* gl) {
    if (!g_traj || !g_trajReady) {
        if (g_trajHad && g_trajDraw) g_drawClear(g_trajDraw);
        g_trajHad = false;
        return;
    }
    if (gl != g_trajGl) {
        if (g_trajGl) trajDropSEH();
        if (!isClassSEH(gl, ".?AVPlayLayer@@")) return;
        if (!trajSetup(gl)) { logf("show trajectory: could not set up, off"); g_trajReady = false; return; }
    }
    void* pl[2] = {at<void*>(gl, kPlayer1), at<void*>(gl, kPlayer2)};
    bool dual = at<bool>(gl, 0x422);
    // only when the physics moved since last time (same input, same result)
    bool moved = false;
    for (int k = 0; k < 2; k++) {
        double t = pl[k] ? at<double>(pl[k], kPoTime) : -1;
        float x = 0, y = 0;
        if (pl[k]) g_nodeGetPos(pl[k], &x, &y);
        if (t != g_trajLastT[k] || x != g_trajLastP[k][0] || y != g_trajLastP[k][1]) moved = true;
        g_trajLastT[k] = t;
        g_trajLastP[k][0] = x;
        g_trajLastP[k][1] = y;
    }
    if (!moved && g_trajHad) return;
    g_drawClear(g_trajDraw);
    g_trajHad = true;
    if (!pl[0] || at<bool>(pl[0], kPoDead) || g_endScreen) return;
    g_trajSim = true;
    for (int k = 0; k < 2; k++) {
        if (k == 0 && !g_trajP1) continue;
        if (k == 1 && (!g_trajP2 || !dual || !pl[1] || at<bool>(pl[1], kPoDead))) break;
        bool now = holdingButton(pl[k], 1);
        for (int h = 0; h < 2; h++) {
            bool held = h == 0;
            const float* c3 = g_trajCol[k][held == now ? 0 : 1]; // the path you are on now gets the "hold" colour
            float col[4] = {c3[0], c3[1], c3[2], 1};
            trajRun(gl, pl[k], g_ghost[h], held, now, col, g_trajTrail[k]);
        }
    }
    g_trajSim = false;
}

void trajTickSEH(void* gl) {
    __try {
        trajTick(gl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_trajSim = false;
        logf("show trajectory failed (%08lx): turned off", GetExceptionCode());
        g_trajReady = false;
        g_ghost[0] = g_ghost[1] = g_trajDraw = nullptr; // do not touch them again
        g_trajGl = nullptr;
    }
}

// GJBaseGameLayer::updateCamera: once per frame while playing, after the physics; the path is made here
FnPF o_updateCameraHook = nullptr;
void h_updateCamera(void* gl, float dt) {
    o_updateCameraHook(gl, dt);
    if (g_traj || g_trajHad) trajTickSEH(gl);
}

// leaving the level: let go of the copies (the level may already be gone; they never touch it when freed)
void trajFrame() {
    if (g_trajGl && g_trajGl != g_playLayer) trajDropSEH();
}

void installTrajectory(uintptr_t base, HMODULE cc) {
    if (!o_destroyPlayer) { logf("show trajectory: needs the destroyPlayer hook, off"); return; }
    auto fn = [&](uintptr_t rva, std::initializer_list<unsigned char> sig, const char* n) { return gameFn(base, rva, sig, n); };
    g_poCreate = reinterpret_cast<FnCreatePlayer>(fn(0x4e4940, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10}, "PlayerObject::create"));
    g_poCopy = reinterpret_cast<FnCopyAttr>(fn(0x4fea30, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18}, "PlayerObject::copyAttributes"));
    g_checkCol = reinterpret_cast<FnCheckCol>(fn(0x31aae0, {0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0xf3, 0x0f, 0x11}, "GJBaseGameLayer::checkCollisions"));
    g_poRotate = reinterpret_cast<FnPF>(fn(0x4ef390, {0x40, 0x57, 0x48, 0x83, 0xec, 0x50, 0x80, 0xb9, 0x32, 0x0a}, "PlayerObject::updateRotation"));
    g_poPush = reinterpret_cast<FnPButton>(fn(0x4f6200, {0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xd0}, "PlayerObject::pushButton"));
    g_poUpdScale = reinterpret_cast<void (*)(void*)>(fn(0x4fe7c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xb9, 0x38, 0x0c}, "PlayerObject::updatePlayerScale"));
    g_poRelease = reinterpret_cast<FnPButton>(fn(0x4f64c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xfa}, "PlayerObject::releaseButton"));
    const uintptr_t modes[7] = {0x4f8790, 0x4f8ad0, 0x4f9840, 0x4f9230, 0x4f99d0, 0x4f9d80, 0x4f8dd0};
    for (int i = 0; i < 7; i++) g_poMode[i] = reinterpret_cast<FnToggle>(fn(modes[i], {0x48, 0x89, 0x5c, 0x24}, "PlayerObject mode toggle"));
    g_poGravity = reinterpret_cast<FnToggle>(fn(0x4f8440, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20}, "PlayerObject::flipGravity"));
    g_poScale = reinterpret_cast<FnToggle>(fn(0x4fecb0, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57}, "PlayerObject::togglePlayerScale"));
    g_poBump = reinterpret_cast<void (*)(void*, float, int, bool, void*)>(fn(0x4fdae0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x78, 0x0b}, "PlayerObject::bumpPlayer"));
    g_poReverse = reinterpret_cast<void (*)(void*, void*)>(fn(0x4f6740, {0x40, 0x56, 0x57, 0x48, 0x83, 0xec, 0x38, 0x80, 0xb9, 0x78, 0x0b}, "PlayerObject::reversePlayer"));
    g_poTimeMod = reinterpret_cast<void (*)(void*, float, bool)>(fn(0x4ff370, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56}, "PlayerObject::updateTimeMod"));
    auto ex = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(cc, n)); };
    g_drawCreate = reinterpret_cast<void* (*)()>(ex("?create@CCDrawNode@cocos2d@@SAPEAV12@XZ"));
    g_drawClear = reinterpret_cast<void (*)(void*)>(ex("?clear@CCDrawNode@cocos2d@@QEAAXXZ"));
    g_drawSegment = reinterpret_cast<bool (*)(void*, const float*, const float*, float, const float*)>(ex("?drawSegment@CCDrawNode@cocos2d@@QEAA_NAEBVCCPoint@2@0MAEBU_ccColor4F@2@@Z"));
    g_drawNoArea = reinterpret_cast<void (*)(void*)>(ex("?disableDrawArea@CCDrawNode@cocos2d@@QEAAXXZ"));
    g_dictClear = reinterpret_cast<void (*)(void*)>(ex("?removeAllObjects@CCDictionary@cocos2d@@QEAAXXZ"));
    g_nodeGetPos = reinterpret_cast<void (*)(void*, float*, float*)>(ex("?getPosition@CCNode@cocos2d@@UEAAXPEAM0@Z"));
    bool ok = g_poUpdScale && g_poCreate && g_poCopy && g_checkCol && g_poRotate && g_poPush && g_poRelease && g_poGravity && g_poScale &&
        g_poTimeMod && g_drawCreate && g_drawClear && g_drawSegment && g_dictClear && g_nodeGetPos && g_objRetain && g_objRelease;
    for (auto m : g_poMode) ok = ok && m;
    if (!ok) { logf("show trajectory: something is missing, off"); return; }
    struct H { uintptr_t rva; std::vector<unsigned char> sig; void* det; void** orig; const char* n; };
    H hs[] = {
        {0x31bce0, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x4c, 0x89, 0x40}, reinterpret_cast<void*>(&h_colObjs), reinterpret_cast<void**>(&o_colObjs), "collisionCheckObjects"},
        {0x31ef50, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57}, reinterpret_cast<void*>(&h_canActivate), reinterpret_cast<void**>(&o_canActivate), "canBeActivatedByPlayer"},
        {0x33ac80, {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x10, 0x49, 0x89, 0x6b}, reinterpret_cast<void*>(&h_gameEvent), reinterpret_cast<void**>(&o_gameEvent), "gameEventTriggered"},
        {0x3c0710, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83}, reinterpret_cast<void*>(&h_addPoint), reinterpret_cast<void**>(&o_addPoint), "HardStreak::addPoint"},
        {0x345c60, {0x48, 0x8b, 0xc4, 0xf3, 0x0f, 0x11, 0x48, 0x10}, reinterpret_cast<void*>(&h_updateCamera), reinterpret_cast<void**>(&o_updateCameraHook), "updateCamera"},
    };
    // a function may already start with a jump: another mod (Frame Extrapolation hooks updateCamera) got
    // there first. MinHook chains on top of it, so that is fine too.
    const unsigned char jmp = 0xE9;
    for (auto& h : hs)
        if (!sigMatchSEH(reinterpret_cast<const unsigned char*>(base + h.rva), h.sig.data(), h.sig.size()) &&
            !sigMatchSEH(reinterpret_cast<const unsigned char*>(base + h.rva), &jmp, 1)) {
            logf("show trajectory: %s does not match, off", h.n);
            return;
        }
    for (auto& h : hs) {
        void* t = reinterpret_cast<void*>(base + h.rva);
        if (MH_CreateHook(t, h.det, h.orig) != MH_OK || MH_EnableHook(t) != MH_OK) { logf("show trajectory: could not hook %s", h.n); return; }
    }
    g_trajReady = true;
    logf("show trajectory: ready");
}

void installTrailFns(HMODULE cc) {
    auto f = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(cc, n)); };
    g_texCache = reinterpret_cast<void* (*)()>(f("?sharedTextureCache@CCTextureCache@cocos2d@@SAPEAV12@XZ"));
    g_addImage = reinterpret_cast<void* (*)(void*, const char*, bool)>(f("?addImage@CCTextureCache@cocos2d@@QEAAPEAVCCTexture2D@2@PEBD_N@Z"));
    g_removeTexKey = reinterpret_cast<void (*)(void*, const char*)>(f("?removeTextureForKey@CCTextureCache@cocos2d@@QEAAXPEBD@Z"));
    g_streakGetTex = reinterpret_cast<void* (*)(void*)>(f("?getTexture@CCMotionStreak@cocos2d@@UEAAPEAVCCTexture2D@2@XZ"));
    g_streakSetTex = reinterpret_cast<void (*)(void*, void*)>(f("?setTexture@CCMotionStreak@cocos2d@@UEAAXPEAVCCTexture2D@2@@Z"));
    g_streakSetStroke = reinterpret_cast<void (*)(void*, float)>(f("?setStroke@CCMotionStreak@cocos2d@@QEAAXM@Z"));
    g_streakSetBlend = reinterpret_cast<void (*)(void*, BlendFunc)>(f("?setBlendFunc@CCMotionStreak@cocos2d@@UEAAXU_ccBlendFunc@2@@Z"));
    g_streakGetBlend = reinterpret_cast<BlendFunc* (*)(void*, BlendFunc*)>(f("?getBlendFunc@CCMotionStreak@cocos2d@@UEAA?AU_ccBlendFunc@2@XZ"));
    g_nodeSetColor = reinterpret_cast<void (*)(void*, const Color3B*)>(f("?setColor@CCNodeRGBA@cocos2d@@UEAAXAEBU_ccColor3B@2@@Z"));
    g_nodeGetColor = reinterpret_cast<const Color3B* (*)(void*)>(f("?getColor@CCNodeRGBA@cocos2d@@UEAAAEBU_ccColor3B@2@XZ"));
    g_drawNodeClear = reinterpret_cast<void (*)(void*)>(f("?clear@CCDrawNode@cocos2d@@QEAAXXZ"));
    g_objRetain = reinterpret_cast<void (*)(void*)>(f("?retain@CCObject@cocos2d@@QEAAXXZ"));
    g_objRelease = reinterpret_cast<void (*)(void*)>(f("?release@CCObject@cocos2d@@QEAAXXZ"));
    if (!g_texCache || !g_addImage || !g_streakSetTex) logf("trail functions missing: custom trail off");
}

// ---- No Animations: the game's Fast Menu (game variable 0168) + every scene transition with no duration
void installSpeedhack(HMODULE cc) {
    g_getScheduler = reinterpret_cast<void* (*)(void*)>(GetProcAddress(cc, "?getScheduler@CCDirector@cocos2d@@UEAAPEAVCCScheduler@2@XZ"));
    g_setTimeScale = reinterpret_cast<void (*)(void*, float)>(GetProcAddress(cc, "?setTimeScale@CCScheduler@cocos2d@@QEAAXM@Z"));
    logf("speedhack: getScheduler %s, setTimeScale %s", g_getScheduler ? "found" : "MISSING", g_setTimeScale ? "found" : "MISSING");
    if (!g_getScheduler || !g_setTimeScale) {
        void* su = reinterpret_cast<void*>(GetProcAddress(cc, "?update@CCScheduler@cocos2d@@UEAAXM@Z"));
        if (su && MH_CreateHook(su, reinterpret_cast<void*>(&h_schedUpdate), reinterpret_cast<void**>(&o_schedUpdate)) == MH_OK &&
            MH_EnableHook(su) == MH_OK) {
            g_speedViaHook = true;
            logf("speedhack: using the CCScheduler::update hook");
        } else {
            logf("speedhack: no way to change the game speed was found");
        }
    }
    HMODULE fm = GetModuleHandleW(L"fmod.dll");
    if (!fm) { logf("fmod.dll not found: speedhack audio off"); return; }
    g_fmodGetMaster = reinterpret_cast<FmodGetMaster>(GetProcAddress(fm, "?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z"));
    g_fmodSetPitch = reinterpret_cast<FmodSetPitch>(GetProcAddress(fm, "?setPitch@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z"));
    void* upd = reinterpret_cast<void*>(GetProcAddress(fm, "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ"));
    if (upd && MH_CreateHook(upd, reinterpret_cast<void*>(&h_fmodUpdate), reinterpret_cast<void**>(&o_fmodUpdate)) == MH_OK) MH_EnableHook(upd);
}

void pctFixTickSEH();    // percentage bugfix (below)
void inputThreadTick();  // input latency / input thread section (below)
void latencyTick();
extern bool g_latHooked;

void frame() {
    double t = now();
    g_fpsFrames++;
    if (t - g_fpsStart >= 0.5) { g_fps = g_fpsFrames / (t - g_fpsStart); g_fpsFrames = 0; g_fpsStart = t; }
    g_inLevel = inLevelSEH(); // every frame: the PlayLayer pointer must never be stale
    trajFrame();
    if (g_hooksOk) safeModeTickSEH();
    visualTickSEH();
    if (g_smartSP) smartLearnTickSEH(g_playLayer);
    pctFixTickSEH();
    inputThreadTick();
    if (g_latHooked) latencyTick();
    applySpeed();
    jukeboxTick();
    applyFps();
    // noclip stats: one sample per frame while the level is moving
    bool ncTouching = false;
    if (g_noclip && g_playLayer) {
        float pct = percentSEH(g_playLayer);
        // the grace time is game time: slower with Speedhack below 1x, but never shorter than a few frames
        double sp = g_speedOn ? std::clamp((double)g_speed, 0.05, 100.0) : 1.0;
        double gap = std::clamp(kNcGap / sp, 0.03, 0.5);
        ncTouching = g_ncLastHitT > 0 && (t - g_ncLastHitT) <= gap;
        // accuracy in physics steps: time the level was moving * 240, and the steps that had a hit
        double dtGame = (g_ncPrevT > 0 ? std::min(t - g_ncPrevT, 0.25) : 0.0) * sp;
        g_ncPrevT = t;
        if (pct != g_ncLastPct && g_ncLastPct >= 0) g_ncMoveT = t;
        if (g_ncHitCalls > 0 || (g_ncMoveT > 0 && t - g_ncMoveT < 0.06)) g_ncSteps += dtGame * kPhysicsTps;
        if (g_ncHitCalls > 0) {
            // at least one step ran in this frame; with a low FPS several did, and a hit never covers more steps than ran
            double stepsNow = std::max(1.0, std::round(dtGame * kPhysicsTps));
            g_ncDeadSteps += std::min(stepsNow, static_cast<double>(g_ncHitCalls));
        }
        g_ncHitCalls = 0;
        // one death per touch: counted when you go from "not touching" to "touching"
        if (ncTouching && !g_ncTouching) g_ncDeaths++;
        g_ncTouching = ncTouching;
        g_ncLastPct = pct;
        g_ncHit = false;
    } else {
        g_ncTouching = false;
    }
    // hitboxes while touching: the game draws them (see h_shouldDraw), we only switch the "yes" on and off
    if (g_hbHooked) {
        bool want = ncTouching && g_ncHitbox;
        if (want && !g_hbForce) { g_hbForce = true; toggleDebugDrawSEH(g_playLayer); }
        else if (!want && g_hbForce) { g_hbForce = false; toggleDebugDrawSEH(g_playLayer); }
    }
    // a finished level counts as a run to 100%
    if (g_runActive && g_playLayer == g_runLayer && g_playLayer && percentSEH(g_playLayer) >= 100.0f) {
        g_cheatFinal = cheatLive(); // freeze the indicator on the end screen
        runEnded(g_playLayer, 100.0f);
    }
    if (g_spPending) {
        int d = g_spPending;
        g_spPending = 0;
        if (g_hooksOk && g_playLayer) switchStartPosSEH(d);
    }

    if (!g_imguiReady) {
        HDC dc = wglGetCurrentDC();
        g_hwnd = dc ? WindowFromDC(dc) : nullptr;
        if (!g_hwnd) return;
        loadGl();
        initImGui();
    }
    bool toast = now() < g_toastUntil;
    if (!g_menuOpen && g_menuT <= 0.001f && !anyLabelOn() && !toast) return; // nothing to draw: zero cost

    if (g_lastScale != g_scale) { applyStyle(); g_lastScale = g_scale; }
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 0.5f * g_scale; // fonts are built at 2x
    io.MouseDrawCursor = g_menuOpen;
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawStatus();
    drawEndBadge();
    if (toast) {
        // small message at the bottom (start pos changed, song ready...)
        auto fdl = ImGui::GetForegroundDrawList();
        ImVec2 d = io.DisplaySize;
        float size = g_fontBold->FontSize * 0.5f * g_scale * 1.1f;
        ImVec2 ts = g_fontBold->CalcTextSizeA(size, FLT_MAX, 0, g_toast.c_str());
        float a = std::min(1.0f, static_cast<float>(g_toastUntil - now()) / 0.3f);
        ImVec2 p((d.x - ts.x) * 0.5f, d.y - ts.y - S(60));
        fdl->AddRectFilled(ImVec2(p.x - S(14), p.y - S(8)), ImVec2(p.x + ts.x + S(14), p.y + ts.y + S(8)),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.08f, 0.08f, 0.1f, 0.85f * a)), S(10));
        fdl->AddRect(ImVec2(p.x - S(14), p.y - S(8)), ImVec2(p.x + ts.x + S(14), p.y + ts.y + S(8)),
            ImGui::ColorConvertFloat4ToU32(ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.9f * a)), S(10), 0, S(1.5f));
        fdl->AddText(g_fontBold, size, p, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, a)), g_toast.c_str());
    }
    drawMenu();
    ImGui::Render();
    renderImGui();
    if (g_cfgDirty && ImGui::GetTime() - g_cfgDirtyAt > 0.5) cfgSave();
}

void frameSEH() {
    __try {
        frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) { logf("exception while drawing the menu (code %08lx)", GetExceptionCode()); logged = true; }
    }
}

// ---------------------------------------------------------------------------
// Precise frame limiter + frame-time meter (both run inside the swapBuffers hook)
// ---------------------------------------------------------------------------
HANDLE g_paceTimer = nullptr;
bool g_paceTimerTried = false;
double g_paceDeadline = 0;

// Absolute deadlines (no drift): the next frame is due exactly one interval after the previous
// DEADLINE, not after the moment the previous frame happened to finish. The long part of the wait
// uses a high-resolution waitable timer, the last ~1.5 ms is a short spin so it lands on the dot.
// Frame work meter (for Input Boost): how long the game needs per frame WITHOUT our wait, i.e. from the end
// of the previous wait to now.
// GD runs physics 240 times a second, so at ~2000 FPS only about 1 frame in 8 contains a physics step and
// that frame takes several times longer than the others. Those frames cannot be made to fit any interval
// (they just arrive a bit late, the deadline resyncs and the next ones are on time again), so the pace is
// chosen from the ordinary frames: the median work, with 60 % headroom. Changes are slow and in steps:
// down only after two windows agree, up only after three, never below the floor.
constexpr int kWorkBuckets = 2000;      // 10 us buckets up to 20 ms
uint32_t g_workHist[kWorkBuckets + 1];
uint32_t g_workN = 0, g_workHitches = 0;
double g_workWindow = 0, g_paceWaitEnd = 0;
int g_boostUpVotes = 0, g_boostDownVotes = 0;
constexpr double kBoostWindow = 3.0;    // seconds per measurement
constexpr float kBoostFloor = 960.0f;   // never pace slower than this (4 frames per physics step)

void workMeter(double t) {
    if (g_paceWaitEnd > 0) {
        double w = t - g_paceWaitEnd;
        if (w >= 0 && w < 0.02) { g_workHist[std::min(static_cast<int>(w / 10e-6), kWorkBuckets)]++; g_workN++; }
        else if (w >= 0.02) g_workHitches++; // loading, pause, alt-tab: says nothing about normal frames
    }
    if (g_workWindow == 0) g_workWindow = t;
    if (t - g_workWindow < kBoostWindow) return;
    // a window with long stops (a level loading, the pause menu) is thrown away
    if (g_steadyFps && g_inLevel && g_workN > 1000 && g_workHitches <= 2) {
        uint32_t want = g_workN / 2, acc = 0; // median
        int b = 0;
        for (; b <= kWorkBuckets; b++) { acc += g_workHist[b]; if (acc >= want) break; }
        double p50 = (b + 1) * 10e-6;
        float floorFps = std::max(kBoostFloor, static_cast<float>(std::ceil(monitorHz() / 240.0) * 240.0));
        float cap = static_cast<float>(std::floor(1.0 / (p50 * 1.6) / 240.0) * 240.0);
        cap = std::clamp(cap, floorFps, 9840.0f);
        if (g_boostCap == 0) { g_boostCap = cap; g_boostUpVotes = g_boostDownVotes = 0; }
        else if (cap < g_boostCap) {
            g_boostUpVotes = 0;
            // slower: after two windows in a row, and only one 240 step per window
            if (++g_boostDownVotes >= 2) { g_boostCap = std::max(cap, g_boostCap - 240.0f); g_boostDownVotes = 1; }
        } else if (cap > g_boostCap) {
            g_boostDownVotes = 0;
            // faster: after three windows in a row, one 240 step per window
            if (++g_boostUpVotes >= 3) { g_boostCap = std::min(cap, g_boostCap + 240.0f); g_boostUpVotes = 2; }
        } else {
            g_boostUpVotes = g_boostDownVotes = 0;
        }
        static float logged = 0;
        if (g_boostCap != logged) {
            logf("steady fps: median frame work %.3f ms -> steady %.0f FPS (floor %.0f)", p50 * 1000.0, g_boostCap, floorFps);
            logged = g_boostCap;
        }
    }
    std::memset(g_workHist, 0, sizeof(g_workHist));
    g_workN = 0;
    g_workHitches = 0;
    g_workWindow = t;
}

void paceFrame() {
    workMeter(now());
    if (!paceActive()) { g_paceDeadline = 0; g_paceWaitEnd = now(); return; }
    const double interval = 1.0 / paceTarget();
    double t = now();
    if (g_paceDeadline == 0) g_paceDeadline = t;
    g_paceDeadline += interval;
    if (g_paceDeadline < t - interval) g_paceDeadline = t; // more than a frame late: resync, never burst
    if (!g_paceTimerTried) {
        g_paceTimerTried = true;
        g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        logf("precise limiter: %s", g_paceTimer ? "high-resolution timer + spin" : "high-resolution timer not available, spinning only");
    }
    for (;;) {
        double rem = g_paceDeadline - now();
        if (rem <= 0) break;
        if (g_paceTimer && rem > 0.0025) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((rem - 0.0015) * 1e7);
            if (SetWaitableTimer(g_paceTimer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_paceTimer, 50);
        } else {
            YieldProcessor();
        }
    }
    g_paceWaitEnd = now();
}

// Frame-time histogram (25 us buckets up to 40 ms). Every 10 s inside a level it writes one line to
// kmenu.log: average FPS, percentiles, 1% low and how many frames were much slower than the median.
// That is what tells "1000 fps but it feels choppy" apart from "1000 fps and really smooth".
constexpr int kFtBuckets = 1600;
constexpr double kFtStep = 25e-6;
uint32_t g_ftHist[kFtBuckets + 1];
uint64_t g_ftN = 0;
double g_ftSum = 0, g_ftMax = 0, g_ftLast = 0, g_ftReport = 0;
uint32_t g_ftHitch = 0;
int g_ftLines = 0;

void frameMeter() {
    double t = now();
    if (g_ftLast > 0) {
        double dt = t - g_ftLast;
        if (dt > 0.1) {
            g_ftHitch++; // pause, loading, alt-tab: not counted as a frame
        } else {
            int b = std::min(static_cast<int>(dt / kFtStep), kFtBuckets);
            g_ftHist[b]++;
            g_ftN++;
            g_ftSum += dt;
            g_ftMax = std::max(g_ftMax, dt);
        }
    }
    g_ftLast = t;
    if (g_ftReport == 0) g_ftReport = t;
    if (t - g_ftReport < 10.0) return;
    if ((g_fpsOn || boostActive()) && g_inLevel && g_ftN > 200 && g_ftLines < 200) {
        auto pct = [&](double q) {
            uint64_t want = static_cast<uint64_t>(q * static_cast<double>(g_ftN)), acc = 0;
            for (int i = 0; i <= kFtBuckets; i++) { acc += g_ftHist[i]; if (acc >= want) return (i + 1) * kFtStep * 1000.0; }
            return kFtBuckets * kFtStep * 1000.0;
        };
        double p50 = pct(0.50), p99 = pct(0.99), p999 = pct(0.999);
        int slowFrom = static_cast<int>(p50 * 1.5 / (kFtStep * 1000.0)) + 1;
        uint64_t slow = 0;
        for (int i = std::min(slowFrom, kFtBuckets); i <= kFtBuckets; i++) slow += g_ftHist[i];
        logf("[frametime] %s target=%.0f avg=%.0f fps | p50=%.2f p99=%.2f p99.9=%.2f max=%.2f ms | 1%%low=%.0f fps | >1.5x median: %llu of %llu (%.2f%%) | stops>100ms: %u",
            boostActive() ? "steady-fps" : paceActive() ? "precise" : "game-limiter", static_cast<double>(paceActive() ? paceTarget() : g_fpsTarget), static_cast<double>(g_ftN) / g_ftSum, p50, p99, p999,
            g_ftMax * 1000.0, 1000.0 / p99, static_cast<unsigned long long>(slow), static_cast<unsigned long long>(g_ftN),
            100.0 * static_cast<double>(slow) / static_cast<double>(g_ftN), g_ftHitch);
        g_ftLines++;
    }
    std::memset(g_ftHist, 0, sizeof(g_ftHist));
    g_ftN = 0; g_ftSum = 0; g_ftMax = 0; g_ftHitch = 0; g_ftReport = t;
}

// ---------------------------------------------------------------------------
// Render stats: OpenGL hooks that only count. They run on the render thread, so plain counters are enough.
// Our own ImGui drawing is excluded (g_ownDraw) so the numbers are the game's.
// ---------------------------------------------------------------------------
bool g_ownDraw = false;
bool g_rsCounting = false;
struct FrameCount { uint64_t draws = 0, tris = 0, tex = 0, blend = 0; double clearAt = 0, visT = 0; double prof[PR_COUNT] = {}; };
FrameCount g_fc;

using PFNDrawElements = void (WINAPI*)(GLenum, GLsizei, GLenum, const void*);
using PFNDrawArrays = void (WINAPI*)(GLenum, GLint, GLsizei);
using PFNBindTexture = void (WINAPI*)(GLenum, GLuint);
using PFNBlendFunc = void (WINAPI*)(GLenum, GLenum);
using PFNClear = void (WINAPI*)(GLbitfield);
PFNDrawElements o_glDrawElements = nullptr;
PFNDrawArrays o_glDrawArrays = nullptr;
PFNBindTexture o_glBindTexture = nullptr;
PFNBlendFunc o_glBlendFunc = nullptr;
PFNClear o_glClear = nullptr;

inline uint64_t triCount(GLenum mode, GLsizei n) {
    if (n < 3) return 0;
    if (mode == GL_TRIANGLES) return static_cast<uint64_t>(n / 3);
    if (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) return static_cast<uint64_t>(n - 2);
    return 0;
}
void WINAPI h_glDrawElements(GLenum mode, GLsizei n, GLenum type, const void* idx) {
    if (g_rsCounting && !g_ownDraw) { g_fc.draws++; g_fc.tris += triCount(mode, n); }
    o_glDrawElements(mode, n, type, idx);
}
void WINAPI h_glDrawArrays(GLenum mode, GLint first, GLsizei n) {
    if (g_rsCounting && !g_ownDraw) { g_fc.draws++; g_fc.tris += triCount(mode, n); }
    o_glDrawArrays(mode, first, n);
}
void WINAPI h_glBindTexture(GLenum target, GLuint tex) {
    if (g_rsCounting && !g_ownDraw) g_fc.tex++;
    o_glBindTexture(target, tex);
}
void WINAPI h_glBlendFunc(GLenum s, GLenum d) {
    if (g_rsCounting && !g_ownDraw) g_fc.blend++;
    o_glBlendFunc(s, d);
}
void WINAPI h_glClear(GLbitfield mask) {
    // the first clear of the frame marks the end of the game logic and the start of the drawing
    if (g_rsCounting && !g_ownDraw && g_fc.clearAt == 0) g_fc.clearAt = now();
    o_glClear(mask);
}

// PlayLayer::updateVisibility(float) (0x50de00 in the 2026-10-20 build, found by its constants: the 75/110/90/150
// screen offsets, the color channels 1000/1005/1007 and the -0.3/0.4 color transform). It runs the per-object
// color/activation loop every frame. This hook only times it; it calls the original untouched.
const unsigned char kSigUpdateVis[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb0, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00};
constexpr uintptr_t kUpdateVisRva = 0x50de00;
using PFNUpdateVis = void (*)(void*, float);
PFNUpdateVis o_updateVisibility = nullptr;
bool g_visHooked = false, g_visTried = false;
void* g_visLayer = nullptr;
double g_visLast = 0, g_visPendingDt = 0;
void h_updateVisibility(void* self, float dt) {
    if (g_visThrottle && self == g_playLayer) {
        double t = now();
        if (self != g_visLayer) { g_visLayer = self; g_visLast = 0; g_visPendingDt = 0; }
        // dt == 0 is the refresh the game does right after a reset: never skip it. At low FPS nothing is skipped.
        if (dt > 0.0f && g_visLast != 0 && t - g_visLast < 1.0 / visHzNow()) {
            g_visPendingDt += dt; // the skipped time is handed over on the next real update
            return;
        }
        g_visLast = t;
        dt += static_cast<float>(g_visPendingDt);
        g_visPendingDt = 0;
    }
    if (!g_rsCounting) { o_updateVisibility(self, dt); return; }
    double t = now();
    o_updateVisibility(self, dt);
    g_fc.visT += now() - t;
}

// ---- Input latency meter + input thread priority -------------------------------------------------------
// In this build RobTop's libcocos2d already reads the keyboard and mouse on its own thread with Raw Input and
// stamps every press with QueryPerformanceCounter (seconds). The stamp travels with the press into
// GJBaseGameLayer::m_queuedButtons (24-byte entries, double timestamp at +0x10), and the physics applies it
// in the step that contains that time (Click Between Steps). So:
//  - the meter compares that stamp with "now" when the game consumes the press (processQueuedButtons,
//    0x33a9a0) and when the press reaches the game's queue (0x6713c0, keyboard actions);
//  - Input Boost raises that input thread to time-critical, so the stamp is taken at once.
double qpcSec() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}
struct LatWin {
    uint32_t hist[401] = {}; // 25 us buckets up to 10 ms
    uint32_t n = 0;
    double sum = 0, max = 0;
    void add(double s) {
        hist[std::min(static_cast<int>(s / 25e-6), 400)]++;
        n++; sum += s; max = std::max(max, s);
    }
    double pct(double q) const {
        uint32_t want = static_cast<uint32_t>(q * n), acc = 0;
        for (int i = 0; i <= 400; i++) { acc += hist[i]; if (acc >= want) return (i + 1) * 25e-6; }
        return 400 * 25e-6;
    }
};
LatWin g_latTotal, g_latQueue;       // window for the label (0.5 s ... until there are presses)
LatWin g_latLogTotal, g_latLogQueue; // window for kmenu.log (10 s)
double g_latLogAt = 0;
int g_latLogLines = 0;
bool g_latTried = false, g_latHooked = false, g_latCounting = false;
constexpr uintptr_t kQueuedButtons = 0x3400; // GJBaseGameLayer::m_queuedButtons (begin, end at +8)

using PFNProcQueued = void (*)(void*, float, bool);
using PFNQueueKey = bool (*)(void*, int, bool, double);
PFNProcQueued o_procQueued = nullptr;
PFNQueueKey o_queueKey = nullptr;

int readQueueStampsSEH(void* gl, double* out, int maxN, size_t& count) {
    __try {
        auto b = at<unsigned char*>(gl, kQueuedButtons), e = at<unsigned char*>(gl, kQueuedButtons + 8);
        count = (b && e > b) ? static_cast<size_t>(e - b) / 24 : 0;
        int n = static_cast<int>(std::min<size_t>(count, static_cast<size_t>(maxN)));
        for (int i = 0; i < n; i++) out[i] = *reinterpret_cast<double*>(b + i * 24 + 0x10);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
        return 0;
    }
}

// ---- CBF: click times on the clock of the physics steps (0.14.8, test switch "gl.cbf.acc", on by default since 0.14.9) -----
// GJBaseGameLayer::update (0x3406d0) only runs whole steps (0x340630: n = floor((dt + m_extraDelta) / step), step =
// min(1, timeWarp) / 240, everything in the same time units as dt). The time left over stays in m_extraDelta
// (+0x3350, double). Only an update that really ran steps sets m_stepTime (+0x38e8) to its start time (0x341e6b);
// a frame with no steps leaves it alone (jae 0x341f79) and just adds its dt to m_extraDelta. So the steps of the next
// update start `rest` behind the clock the click stamps are compared with (m_stepTime and on), where `rest` is the
// m_extraDelta left by the LAST update that ran steps (cbfAccTrackSEH keeps it). A click stamped c sits at
// m_stepTime + s * (c - m_stepTime) + rest inside those steps (s = this menu's Speedhack, 1 without it); the game
// puts it `rest` too early (0 to one step, different every frame). The same holds with a level Time Warp (the step
// shrinks with it, the units do not change; the exact cut with Time Warp must be live). With the switch on,
// h_queueKey gives the press that stamp when it reaches the game. The presses left in the queue after an update
// belong to the next one, whose clock is new: cbfAccTick (render hook, just after update, same thread) places them
// again from their real time.
struct CbfStamp { double placed, real; };
CbfStamp g_cbfStamps[16];  // stamps moved by us that may still be in the queue: placed (what the game has) -> real
int g_cbfStampN = 0;
double g_cbfLastStep = 0, g_cbfRestN = 0; // m_stepTime of the last update with steps, and the m_extraDelta it left
unsigned long long g_cbfAccWinN = 0;
double g_cbfAccWinSum = 0, g_cbfAccWinMax = 0, g_cbfAccLogAt = 0;

// the clock the next update starts from; false = leave the stamps alone (not in a level, no rest tracked yet, odd values)
bool cbfAccClockSEH(double* t0, double* rest, double* s) {
    __try {
        void* gl = g_gameLayer;
        if (!gl || !g_inLevel) return false;
        float tw = at<float>(gl, 0x330);
        if (!(tw > 0.0f) || (tw != 1.0f && !g_cbfTwLive)) return false; // a level Time Warp needs the exact cut with Time Warp
        double a = at<double>(gl, 0x38e8), r = g_cbfRestN;
        if (!(a > 0.0) || a != g_cbfLastStep || !(r >= 0.0) || r > 0.05) return false; // the rest we kept must belong to this m_stepTime
        *t0 = a; *rest = r; *s = static_cast<double>(g_hookScale);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// every frame (render hook, right after update): a new m_stepTime means an update ran steps, so m_extraDelta is its rest
void cbfAccTrackSEH() {
    __try {
        void* gl = g_gameLayer;
        if (!gl || !g_inLevel) { g_cbfLastStep = 0; return; }
        double a = at<double>(gl, 0x38e8);
        if (a != g_cbfLastStep) { g_cbfLastStep = a; g_cbfRestN = at<double>(gl, 0x3350); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cbfLastStep = 0;
    }
}

inline double cbfAccPlace(double real, double t0, double rest, double s) { return t0 + (real - t0) * s + rest; }

void cbfAccRemember(double placed, double real) {
    if (g_cbfStampN == 16) { std::memmove(g_cbfStamps, g_cbfStamps + 1, 15 * sizeof(CbfStamp)); g_cbfStampN = 15; }
    g_cbfStamps[g_cbfStampN].placed = placed;
    g_cbfStamps[g_cbfStampN].real = real;
    g_cbfStampN++;
}

// real time of a stamp we moved (any other stamp comes back as it is): the latency meter wants the real one
double cbfAccRealOf(double stamp) {
    for (int i = 0; i < g_cbfStampN; i++)
        if (g_cbfStamps[i].placed == stamp) return g_cbfStamps[i].real;
    return stamp;
}

// h_queueKey: the stamp the game should get for a press stamped `ts`; false = not usable now (the old path decides)
bool cbfAccStamp(double& ts) {
    double t0 = 0, rest = 0, s = 1;
    if (!cbfAccClockSEH(&t0, &rest, &s)) return false;
    double x = ts - t0;
    if (x < -0.25 || x > 1.0) return false;
    double p = cbfAccPlace(ts, t0, rest, s);
    cbfAccRemember(p, ts);
    g_cbfAccN++; g_cbfAccWinN++;
    g_cbfAccSum += rest; g_cbfAccWinSum += rest;
    g_cbfAccMax = std::max(g_cbfAccMax, rest); g_cbfAccWinMax = std::max(g_cbfAccWinMax, rest);
    ts = p;
    return true;
}

// The presses that stayed in the queue after the update were placed with its old clock: place them again with the
// clock the next update starts from (restore = put their real times back).
void cbfAccRemapSEH(bool restore) {
    __try {
        CbfStamp keep[16];
        int nk = 0;
        double t0 = 0, rest = 0, s = 1;
        bool haveClock = !restore && cbfAccClockSEH(&t0, &rest, &s);
        void* gl = g_gameLayer;
        unsigned char* b = nullptr;
        unsigned char* e = nullptr;
        if (gl) { b = at<unsigned char*>(gl, kQueuedButtons); e = at<unsigned char*>(gl, kQueuedButtons + 8); }
        size_t n = (b && e > b) ? std::min<size_t>(static_cast<size_t>(e - b) / 24, 64) : 0;
        for (size_t i = 0; i < n; i++) {
            double* p = reinterpret_cast<double*>(b + i * 24 + 0x10);
            for (int k = 0; k < g_cbfStampN; k++) {
                if (*p != g_cbfStamps[k].placed) continue;
                double real = g_cbfStamps[k].real;
                double np = real;
                if (haveClock && real - t0 >= -0.25 && real - t0 <= 1.0) np = cbfAccPlace(real, t0, rest, s);
                *p = np;
                if (np != real && nk < 16) { keep[nk].placed = np; keep[nk].real = real; nk++; }
                break;
            }
        }
        for (int k = 0; k < nk; k++) g_cbfStamps[k] = keep[k];
        g_cbfStampN = nk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cbfStampN = 0;
    }
}

// every frame, from the render hook (right after update)
void cbfAccTick() {
    bool want = cbfAccWanted();
    if (want) cbfAccTrackSEH();
    if (g_cbfStampN > 0) cbfAccRemapSEH(!want);
    if (!want) { g_cbfAccLogAt = 0; g_cbfLastStep = 0; return; }
    double t = now();
    if (g_cbfAccLogAt == 0) g_cbfAccLogAt = t;
    if (t - g_cbfAccLogAt < 10.0) return;
    g_cbfAccLogAt = t;
    if (g_cbfAccWinN)
        logf("[cbf] step clock: %llu presses placed in the last 10 s, leftover time avg %.2f ms, max %.2f ms",
             g_cbfAccWinN, g_cbfAccWinSum / static_cast<double>(g_cbfAccWinN) * 1000.0, g_cbfAccWinMax * 1000.0);
    g_cbfAccWinN = 0; g_cbfAccWinSum = g_cbfAccWinMax = 0;
}

void h_procQueued(void* gl, float dt, bool flush) {
    // Exact CBF with Time Warp: the game passes the part's length in Time-Warp time (step / tw), but m_stepTime and
    // the click stamps run in real time, so with tw < 1 it applied clicks too early (RobTop's own bug). Here the
    // limit becomes the part's real length, the same window the exact cut uses.
    if (g_cbfTwLive && gl) {
        float tw = at<float>(gl, 0x330);
        if (tw > 0.0f && tw != 1.0f) dt *= tw;
    }
    // flush = apply everything queued now (the game does this on steps where it does not split by time,
    // e.g. with Click Between Steps off): those are real presses too. Stale ones (reset, pause) are
    // filtered out below by the 100 ms limit.
    if (!g_latCounting) { o_procQueued(gl, dt, flush); return; }
    double ts[16];
    size_t before = 0, after = 0;
    int n = readQueueStampsSEH(gl, ts, 16, before);
    o_procQueued(gl, dt, flush);
    if (n == 0) return;
    double dummy[1];
    readQueueStampsSEH(gl, dummy, 0, after);
    if (after >= before) return;
    size_t used = std::min<size_t>(before - after, static_cast<size_t>(n)); // consumed from the front
    double t = qpcSec();
    for (size_t i = 0; i < used; i++) {
        double lat = t - cbfAccRealOf(ts[i]);
        if (lat >= 0 && lat < 0.1) { g_latTotal.add(lat); g_latLogTotal.add(lat); }
    }
}

// Speedhack here scales the delta time of the game (cocos scheduler), so a frame's steps cover s times its real
// duration, starting at GJBaseGameLayer::m_stepTime (0x38e8 = QPC time of the previous frame start). A click stamped x
// seconds after that point must be placed at x * s inside the steps, or the exact cut lands in the wrong place.
double cbfMapStampSEH(double ts) {
    __try {
        if (!g_gameLayer || !g_inLevel) return ts;
        double t0 = at<double>(g_gameLayer, 0x38e8);
        double x = ts - t0;
        if (x <= 0 || x > 0.5) return ts;
        return t0 + x * static_cast<double>(g_hookScale);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ts;
    }
}

bool h_queueKey(void* self, int action, bool down, double ts) {
    if (g_latCounting && ts > 0) {
        double w = qpcSec() - ts;
        if (w >= 0 && w < 0.1) { g_latQueue.add(w); g_latLogQueue.add(w); }
    }
    if (cbfWanted() && ts > 0) {
        // test switch on: the stamp goes on the clock of the physics steps; otherwise (or if that clock is not usable
        // right now, e.g. a level Time Warp) only the Speedhack rescale, exactly as in 0.14.7
        if (!(cbfAccWanted() && cbfAccStamp(ts)) && g_hookScale != 1.0f) ts = cbfMapStampSEH(ts);
    }
    return o_queueKey(self, action, down, ts);
}

void installLatHooks() {
    g_latTried = true;
    static const unsigned char sigProc[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x4c};
    static const unsigned char sigKey[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
    uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* proc = reinterpret_cast<void*>(exeBase + 0x33a9a0);
    void* key = reinterpret_cast<void*>(exeBase + 0x6713c0);
    if (!sigMatchSEH(static_cast<const unsigned char*>(proc), sigProc, sizeof(sigProc)) ||
        !sigMatchSEH(static_cast<const unsigned char*>(key), sigKey, sizeof(sigKey))) {
        logf("input latency: functions do not match this version (meter off)");
        return;
    }
    bool ok = MH_CreateHook(proc, reinterpret_cast<void*>(&h_procQueued), reinterpret_cast<void**>(&o_procQueued)) == MH_OK &&
        MH_EnableHook(proc) == MH_OK;
    if (ok && MH_CreateHook(key, reinterpret_cast<void*>(&h_queueKey), reinterpret_cast<void**>(&o_queueKey)) == MH_OK)
        MH_EnableHook(key);
    g_latHooked = ok;
    logf("input latency meter: %s", ok ? "ready" : "could not hook");
}

// ---------------------------------------------------------------------------
// "Kuang Mods" on the main menu, like Geode's button: a round button at the end of the bottom row (achievements,
// settings, stats, Newgrounds) that opens a normal Geometry Dash style popup with every Kuang mod and its settings.
// Built only from libcocos2d exports, from the render hook (main thread). A click only changes the state and marks the
// popup dirty; it is rebuilt on the next frame, never inside the menu's own touch handler.
// ---------------------------------------------------------------------------
#include "kmods_btn.h"

// CCSprite's colour and texture functions belong to its second and third base classes (CCRGBAProtocol at +0x140,
// CCTextureProtocol at +0x158, checked in CCSprite::initWithTexture and setColor of libcocos2d): the exported
// functions want a pointer to that part of the sprite, not to the sprite itself. Passing the sprite made the game throw.
constexpr uintptr_t kSprRgba = 0x140, kSprTexProto = 0x158;

namespace gdx {
void* (*spriteRaw)(const char*) = nullptr;
float (*scaleFactor)(void*) = nullptr;
void* sprite(const char* file);
void* (*spriteFrameRaw)(const char*) = nullptr;
void* (*frameCache)() = nullptr;
void* (*frameByName)(void*, const char*) = nullptr;
void* spriteFrame(const char* n);
void* (*spriteTex)(void*, const float*) = nullptr;   // CCRect const&
void* (*spriteGetTex)(void*) = nullptr;
void (*spriteColor)(void*, const unsigned char*) = nullptr;
void (*spriteFlipX)(void*, bool) = nullptr;
void* (*label)(const char*, const char*) = nullptr;
void* (*labelW)(const char*, const char*, float, int) = nullptr;
void (*labelColor)(void*, const unsigned char*) = nullptr;
void* (*menu)() = nullptr;
void* (*item)(void*, void*, void*, void*) = nullptr; // normal, selected, target, selector
void* (*node)() = nullptr;
void* (*layer)() = nullptr;
void* (*layerColor)(const unsigned char*, float, float) = nullptr;
void (*setPos)(void*, const float*) = nullptr;
void (*setScale)(void*, float) = nullptr;
void (*setScaleX)(void*, float) = nullptr;
void (*setScaleY)(void*, float) = nullptr;
void (*setAnchor)(void*, const float*) = nullptr;
void (*setSize)(void*, const float*) = nullptr;
const float* (*getSize)(void*) = nullptr;
const float* (*getPos)(void*) = nullptr;
void (*remove)(void*, bool) = nullptr;
void (*menuEnabled)(void*, bool) = nullptr;
void (*touchEnabled)(void*, bool) = nullptr;
void (*touchMode)(void*, int) = nullptr;
void (*touchPrio)(void*, int) = nullptr;
void (*keypad)(void*, bool) = nullptr;
float* (*winSize)(void*, float*) = nullptr;           // CCSize by value: hidden return pointer
bool (*isSelected)(void*) = nullptr;
bool (*isVisible)(void*) = nullptr;
int state = 0; // 0 not tried, 1 ready, -1 off
}

extern const char* g_gdStep;
// A texture file of the game by its full path in Resources, with the suffix of the texture quality in use
// (-uhd / -hd / none). Asking the game to find a short name ("GJ_square01.png") made it throw from the render
// hook; a full path loads fine (the main menu button image is loaded the same way).
void* gdx::sprite(const char* file) {
    g_gdStep = file;
    if (std::strchr(file, ':')) return gdx::spriteRaw(file); // already a full path
    std::string base(file), ext;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) { ext = base.substr(dot); base.resize(dot); }
    float f = gdx::scaleFactor(g_sharedDirector());
    const char* order[3] = {"-uhd", "-hd", ""};
    int first = f >= 3.5f ? 0 : f >= 1.5f ? 1 : 2;
    std::string dir = narrow(g_dir) + "Resources\\";
    for (int k = first; k < 3; k++) {
        std::string full = dir + base + order[k] + ext;
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES) return gdx::spriteRaw(full.c_str());
    }
    logf("mods button: %s not found in Resources, skipped", file);
    return nullptr;
}

void* gdx::spriteFrame(const char* n) {
    g_gdStep = n;
    void* fc = gdx::frameCache();
    if (!fc || !gdx::frameByName(fc, n)) { logf("mods button: sprite %s is not loaded, skipped", n); return nullptr; }
    return gdx::spriteFrameRaw(n);
}

bool gdxInit() {
    if (gdx::state) return gdx::state > 0;
    HMODULE cc = GetModuleHandleW(L"libcocos2d.dll");
    int miss = 0;
    auto f = [&](auto& p, const char* n) {
        p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(cc ? reinterpret_cast<void*>(GetProcAddress(cc, n)) : nullptr);
        if (!p) { miss++; logf("mods button: %s not found", n); }
    };
    f(gdx::spriteRaw, "?create@CCSprite@cocos2d@@SAPEAV12@PEBD@Z");
    f(gdx::scaleFactor, "?getContentScaleFactor@CCDirector@cocos2d@@QEAAMXZ");
    f(gdx::spriteFrameRaw, "?createWithSpriteFrameName@CCSprite@cocos2d@@SAPEAV12@PEBD@Z");
    f(gdx::frameCache, "?sharedSpriteFrameCache@CCSpriteFrameCache@cocos2d@@SAPEAV12@XZ");
    f(gdx::frameByName, "?spriteFrameByName@CCSpriteFrameCache@cocos2d@@QEAAPEAVCCSpriteFrame@2@PEBD@Z");
    f(gdx::spriteTex, "?createWithTexture@CCSprite@cocos2d@@SAPEAV12@PEAVCCTexture2D@2@AEBVCCRect@2@@Z");
    f(gdx::spriteGetTex, "?getTexture@CCSprite@cocos2d@@UEAAPEAVCCTexture2D@2@XZ");
    f(gdx::spriteColor, "?setColor@CCSprite@cocos2d@@UEAAXAEBU_ccColor3B@2@@Z");
    f(gdx::spriteFlipX, "?setFlipX@CCSprite@cocos2d@@QEAAX_N@Z");
    f(gdx::label, "?create@CCLabelBMFont@cocos2d@@SAPEAV12@PEBD0@Z");
    f(gdx::labelW, "?create@CCLabelBMFont@cocos2d@@SAPEAV12@PEBD0MW4CCTextAlignment@2@@Z");
    f(gdx::labelColor, "?setColor@CCLabelBMFont@cocos2d@@UEAAXAEBU_ccColor3B@2@@Z");
    f(gdx::menu, "?create@CCMenu@cocos2d@@SAPEAV12@XZ");
    f(gdx::item, "?create@CCMenuItemSprite@cocos2d@@SAPEAV12@PEAVCCNode@2@0PEAVCCObject@2@P842@EAAX1@Z@Z");
    f(gdx::node, "?create@CCNode@cocos2d@@SAPEAV12@XZ");
    f(gdx::layer, "?create@CCLayer@cocos2d@@SAPEAV12@XZ");
    f(gdx::layerColor, "?create@CCLayerColor@cocos2d@@SAPEAV12@AEBU_ccColor4B@2@MM@Z");
    f(gdx::setPos, "?setPosition@CCNode@cocos2d@@UEAAXAEBVCCPoint@2@@Z");
    f(gdx::setScale, "?setScale@CCNode@cocos2d@@UEAAXM@Z");
    f(gdx::setScaleX, "?setScaleX@CCNode@cocos2d@@UEAAXM@Z");
    f(gdx::setScaleY, "?setScaleY@CCNode@cocos2d@@UEAAXM@Z");
    f(gdx::setAnchor, "?setAnchorPoint@CCNode@cocos2d@@UEAAXAEBVCCPoint@2@@Z");
    f(gdx::setSize, "?setContentSize@CCNode@cocos2d@@UEAAXAEBVCCSize@2@@Z");
    f(gdx::getSize, "?getContentSize@CCNode@cocos2d@@UEBAAEBVCCSize@2@XZ");
    f(gdx::getPos, "?getPosition@CCNode@cocos2d@@UEAAAEBVCCPoint@2@XZ");
    f(gdx::remove, "?removeFromParentAndCleanup@CCNode@cocos2d@@UEAAX_N@Z");
    f(gdx::menuEnabled, "?setEnabled@CCMenu@cocos2d@@UEAAX_N@Z");
    f(gdx::touchEnabled, "?setTouchEnabled@CCLayer@cocos2d@@UEAAX_N@Z");
    f(gdx::touchMode, "?setTouchMode@CCLayer@cocos2d@@UEAAXW4ccTouchesMode@2@@Z");
    f(gdx::touchPrio, "?setTouchPriority@CCLayer@cocos2d@@UEAAXH@Z");
    f(gdx::keypad, "?setKeypadEnabled@CCLayer@cocos2d@@UEAAX_N@Z");
    f(gdx::winSize, "?getWinSize@CCDirector@cocos2d@@QEAA?AVCCSize@2@XZ");
    f(gdx::isSelected, "?isSelected@CCMenuItem@cocos2d@@UEAA_NXZ");
    gdx::isVisible = reinterpret_cast<bool (*)(void*)>(cc ? GetProcAddress(cc, "?isVisible@CCNode@cocos2d@@UEAA_NXZ") : nullptr);
    gdx::state = miss ? -1 : 1;
    if (!miss) logf("mods button: ready");
    return miss == 0;
}

void gAdd(void* parent, void* child, int z) { vfn<void (*)(void*, void*, int)>(parent, 57)(parent, child, z); } // CCNode::addChild(child, z)
void gPos(void* n, float x, float y) { float p[2] = {x, y}; gdx::setPos(n, p); }
void gAnchor(void* n, float x, float y) { float p[2] = {x, y}; gdx::setAnchor(n, p); }
int gChildren(void* node, void** out, int max) {
    void* ch = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(node) + 0xd0);
    if (!ch) return 0;
    void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ch) + 0x38);
    if (!data) return 0;
    unsigned c = *reinterpret_cast<unsigned*>(data);
    void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(data) + 0x10);
    int n = 0;
    for (unsigned i = 0; i < c && n < max; i++) out[n++] = arr[i];
    return n;
}
bool gIsA(void* o, const char* r) { const char* n = o ? rttiName(o) : nullptr; return n && std::strcmp(n, r) == 0; }

// the game's bitmap fonts only have ASCII: accents become plain letters
std::string gdText(const char* s) {
    std::string o;
    for (auto p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
        if (*p < 0x80) { o += static_cast<char>(*p); continue; }
        if (*p == 0xC3 && p[1]) {
            unsigned char c = *++p;
            char r = c >= 0xA0 && c <= 0xA5 ? 'a' : c == 0xA7 ? 'c' : c >= 0xA8 && c <= 0xAB ? 'e' : c >= 0xAC && c <= 0xAF ? 'i'
                : c == 0xB1 ? 'n' : c >= 0xB2 && c <= 0xB6 ? 'o' : c >= 0xB9 && c <= 0xBC ? 'u' : c >= 0x80 && c <= 0x85 ? 'A'
                : c >= 0x88 && c <= 0x8B ? 'E' : c >= 0x8C && c <= 0x8F ? 'I' : c == 0x91 ? 'N' : c >= 0x92 && c <= 0x96 ? 'O'
                : c >= 0x99 && c <= 0x9C ? 'U' : 0;
            if (r) o += r;
            continue;
        }
        if (*p == 0xC2 && p[1]) { if (*++p == 0xB7) o += '-'; continue; } // ¿ ¡ ° dropped, middle dot -> '-'
        while ((p[1] & 0xC0) == 0x80) p++; // other multi-byte characters dropped
    }
    return o;
}

// 9-slice from a standalone texture of the game (GJ_square01.png, GJ_button_01.png...), like CCScale9Sprite
void* gNine(const char* file, float w, float h) {
    g_gdStep = file;
    void* ref = gdx::sprite(file);
    if (!ref) return nullptr;
    void* tex = gdx::spriteGetTex(reinterpret_cast<char*>(ref) + kSprTexProto);
    const float* sz = gdx::getSize(ref);
    float tw = sz[0], th = sz[1], c = std::min(tw, th) / 3.0f;
    float cw = std::min(c, w * 0.5f), ch = std::min(c, h * 0.5f);
    void* box = gdx::node();
    float s[2] = {w, h};
    gdx::setSize(box, s);
    float sx[3] = {0, c, tw - c}, sw[3] = {c, tw - 2 * c, c};
    float sy[3] = {0, c, th - c}, sh[3] = {c, th - 2 * c, c};          // texture rows, top first
    float dx[3] = {0, cw, w - cw}, dw[3] = {cw, w - 2 * cw, cw};
    float dy[3] = {h - ch, ch, 0}, dh[3] = {ch, h - 2 * ch, ch};      // same rows on screen
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            if (dw[k] <= 0 || dh[r] <= 0) continue;
            float rect[4] = {sx[k], sy[r], sw[k], sh[r]};
            void* p = gdx::spriteTex(tex, rect);
            if (!p) continue;
            gAnchor(p, 0, 0);
            gPos(p, dx[k], dy[r]);
            gdx::setScaleX(p, dw[k] / sw[k]);
            gdx::setScaleY(p, dh[r] / sh[r]);
            gAdd(box, p, 0);
        }
    return box;
}
void* gLabel(const std::string& t, const char* font, float scale, float maxW) {
    void* l = gdx::label(t.c_str(), font);
    if (!l) return nullptr;
    float w = gdx::getSize(l)[0];
    if (maxW > 0 && w * scale > maxW) scale = maxW / std::max(w, 1.0f);
    gdx::setScale(l, scale);
    return l;
}

// centred multi-line chatFont text, wrapped by words here (one label per line)
const char* g_gdStep = "";
void gWrapped(void* parent, const std::string& text, float scale, float maxW, float cx, float cy) {
    std::vector<std::string> lines;
    std::string line, word;
    auto width = [&](const std::string& t) { void* l = gdx::label(t.c_str(), "chatFont.fnt"); return l ? gdx::getSize(l)[0] * scale : 0.0f; };
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string tryL = line.empty() ? word : line + " " + word;
        if (!line.empty() && width(tryL) > maxW) { lines.push_back(line); line = word; }
        else line = tryL;
        word.clear();
    };
    for (char ch : text) { if (ch == ' ' || ch == '\n') flushWord(); else word += ch; }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    float lh = 0;
    if (void* probe = gdx::label("Ag", "chatFont.fnt")) lh = gdx::getSize(probe)[1] * scale;
    float y0 = cy + (lines.size() - 1) * lh * 0.5f;
    for (size_t i = 0; i < lines.size(); i++)
        if (void* l = gLabel(lines[i], "chatFont.fnt", scale, 0)) { gPos(l, cx, y0 - i * lh); gAdd(parent, l, 2); }
}

// ---- state
struct GdItem { void* item; int code; float base, cur; };
std::vector<GdItem> g_gdItems;
void* g_gdScene = nullptr;
void* g_gdMenuLayer = nullptr;
void* g_gdBtn = nullptr;
float g_gdBtnBase = 1, g_gdBtnCur = 1;
bool g_gdBtnFailed = false, g_gdBtnLogged = false;
void* g_gdRoot = nullptr;
void* g_gdContent = nullptr;
std::vector<void*> g_gdOffMenus;
bool g_gdWantOpen = false, g_gdDirty = false;
int g_gdTab = 0, g_gdPage = 0, g_gdDesc = -1;
extern int g_kmTab;
std::wstring installerPath();
double g_gdScanAt = 0, g_gdBuiltAt = 0;

struct GdOpt { int kind; std::string label, desc; bool on, enabled; int act; }; // kind 0 toggle, 1 button
struct Plan { std::wstring guid; std::string name; };
std::vector<Plan> g_plans;
double g_plansAt = -100;

void loadPlans() {
    if (now() - g_plansAt < 10) return;
    g_plansAt = now();
    g_plans.clear();
    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    if (!pp) return;
    using En = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, int, ULONG, UCHAR*, DWORD*);
    using Rn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
    auto en = reinterpret_cast<En>(GetProcAddress(pp, "PowerEnumerate"));
    auto rn = reinterpret_cast<Rn>(GetProcAddress(pp, "PowerReadFriendlyName"));
    if (!en || !rn) return;
    for (ULONG i = 0; i < 32; i++) {
        GUID g;
        DWORD sz = sizeof(g);
        if (en(nullptr, nullptr, nullptr, 16 /*ACCESS_SCHEME*/, i, reinterpret_cast<UCHAR*>(&g), &sz) != ERROR_SUCCESS) break;
        wchar_t nm[128] = {};
        DWORD ns = sizeof(nm);
        rn(nullptr, &g, nullptr, nullptr, reinterpret_cast<UCHAR*>(nm), &ns);
        wchar_t gs[64];
        swprintf(gs, 64, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
            g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        g_plans.push_back({gs, narrow(nm)});
    }
}
std::wstring appPath(int tab) {
    wchar_t la[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    return std::wstring(la) + L"\\Programs\\Kuang\\" + (tab == 2 ? L"KuangFrameExtrapolation.exe" : L"KuangOptimizer.exe");
}

ExtMod* gdExt() { return g_gdTab == 2 ? &g_extKfe : g_gdTab == 3 ? &g_extOpt : nullptr; }

std::vector<GdOpt> gdOptions() {
    std::vector<GdOpt> v;
    if (g_gdTab == 0) {
        v.push_back({1, T("mods.open.hack"), T("mods.menu.d"), false, true, 3});
    } else if (g_gdTab == 1) {
        v.push_back({0, T("mods.enable"), T("mods.cbf.d"), g_modCbf, !g_cbfFailed, 1});
        v.push_back({0, T("gl.cbf"), g_modCbf ? T("gl.cbf.d") : T("gl.cbf.off"), g_cbfExact, g_modCbf && !g_cbfFailed, 2});
    } else {
        ExtMod& m = *gdExt();
        if (!GetModuleHandleW(m.dll)) return v;
        extRead(m, false);
        bool en = m.v[L"enabled"];
        v.push_back({0, T("mods.enable"), T(g_gdTab == 2 ? "mods.kfe.d" : "mods.opt.d"), en, true, 10});
        if (g_gdTab == 2) v.push_back({0, T("kfe.inter"), T("kfe.inter.d"), m.interp, en, 11});
        for (size_t k = 0; k < m.nkeys; k++) {
            std::string dk = std::string(m.keys[k].tkey) + ".d";
            v.push_back({0, T(m.keys[k].tkey), T(dk.c_str()), m.v[m.keys[k].key], en, 20 + static_cast<int>(k)});
        }
        if (g_gdTab == 3) {
            wchar_t cur[64] = {};
            GetPrivateProfileStringW(L"General", L"power_plan", L"", cur, 64, m.path.c_str());
            std::string name = T("opt.plan.keep");
            loadPlans();
            for (auto& p : g_plans) if (_wcsicmp(p.guid.c_str(), cur) == 0) name = p.name;
            v.push_back({1, std::string(T("opt.plan")) + ": " + name, T("opt.plan.d"), false, en, 40});
        }
        v.push_back({1, T("mods.open.app"), T(g_gdTab == 2 ? "mods.app.kfe.d" : "mods.opt.app"), false, true, 41});
    }
    return v;
}

void gdApply(int idx) {
    auto ops = gdOptions();
    if (idx < 0 || idx >= static_cast<int>(ops.size())) return;
    const GdOpt& o = ops[idx];
    g_gdDesc = idx;
    if (!o.enabled) return; // its description says why
    ExtMod* m = gdExt();
    switch (o.act) {
    case 1: g_modCbf = !g_modCbf; cfgSetBool("mod.cbf", g_modCbf); break;
    case 2:
        g_cbfExact = !g_cbfExact;
        cfgSetBool("gl.cbf", g_cbfExact);
        if (g_cbfExact) { bool cur = false; if (gameVarSEH(kGameVars[6].key, &cur) && !cur) setGameVarSEH(kGameVars[6], true); }
        break;
    case 3: g_menuOpen = true; g_gdWantClose = true; break;
    case 10: { bool& b = m->v[L"enabled"]; b = !b; extPut(*m, L"enabled", b ? L"1" : L"0"); break; }
    case 11: m->interp = !m->interp; extPut(*m, L"mode", m->interp ? L"interpolation" : L"extrapolation"); break;
    case 40: {
        loadPlans();
        wchar_t cur[64] = {};
        GetPrivateProfileStringW(L"General", L"power_plan", L"", cur, 64, m->path.c_str());
        int at = -1;
        for (size_t k = 0; k < g_plans.size(); k++) if (_wcsicmp(g_plans[k].guid.c_str(), cur) == 0) at = static_cast<int>(k);
        int next = at + 1; // "don't change" -> first plan -> ... -> last -> "don't change"
        extPut(*m, L"power_plan", next < static_cast<int>(g_plans.size()) ? g_plans[next].guid.c_str() : L"");
        break;
    }
    case 41: {
        std::wstring p = appPath(g_gdTab == 1 || g_gdTab == 0 ? 3 : g_gdTab);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) { g_gdDesc = -2; break; }
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    default:
        if (o.act >= 20 && o.act < 40 && m) {
            const IniKey& k = m->keys[o.act - 20];
            bool& b = m->v[k.key];
            b = !b;
            extPut(*m, k.key, b ? L"1" : L"0");
        }
    }
}

void gdCallback(void* /*target*/, void* sender) { // SEL_MenuHandler: (target->*sel)(sender)
    if (sender == g_gdBtn) { g_gdWantOpen = true; return; }
    for (auto& it : g_gdItems) {
        if (it.item != sender) continue;
        int c = it.code;
        if (c == 302) g_gdWantClose = true;
        else if (c >= 400 && c < 403) { g_kmTab = c - 400; g_gdPage = 0; g_gdDesc = -1; }
        else if (c >= 500 && c < 504) {
            std::wstring p = installerPath();
            if (p.empty()) g_gdDesc = -3;
            else ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (c >= 200 && c < 204) { g_gdTab = c - 200; g_gdPage = 0; g_gdDesc = -1; }
        else if (c == 300) g_gdPage--;
        else if (c == 301) g_gdPage++;
        else if (c >= 100 && c < 200) g_gdDesc = c - 100;
        else gdApply(c);
        g_gdDirty = true;
        return;
    }
}

void* gdAddItem(void* menu, void* image, int code, float scale, float x, float y) {
    void* it = gdx::item(image, nullptr, g_gdRoot, reinterpret_cast<void*>(&gdCallback));
    if (!it) return nullptr;
    gdx::setScale(it, scale);
    gPos(it, x, y);
    gAdd(menu, it, 0);
    g_gdItems.push_back({it, code, scale, scale});
    return it;
}

// visible items of a menu on the same line as the first visible one, sorted left to right
int gRow(void* menu, void** row) {
    void* it[16];
    int m = gChildren(menu, it, 16), r = 0;
    float y0 = 0;
    for (int k = 0; k < m; k++) {
        if (gdx::isVisible && !gdx::isVisible(it[k])) continue;
        float y = gdx::getPos(it[k])[1];
        if (r == 0) y0 = y;
        else if (std::fabs(y - y0) > 4) continue;
        row[r++] = it[k];
    }
    std::sort(row, row + r, [](void* a, void* b) { return gdx::getPos(a)[0] < gdx::getPos(b)[0]; });
    return r;
}

// the bottom row of the main menu: a CCMenu of the MenuLayer with 4-6 big buttons on one line, low on the screen
void gdAddButton() {
    if (g_gdBtnFailed) return;
    float ws[2] = {569, 320};
    gdx::winSize(g_sharedDirector(), ws);
    void* ch[64];
    int n = gChildren(g_gdMenuLayer, ch, 64);
    void* best = nullptr;
    float bestY = 1e9f;
    for (int i = 0; i < n; i++) {
        const char* cn = rttiName(ch[i]);
        if (!cn || !std::strstr(cn, "CCMenu@")) continue; // CCMenu or a subclass of it
        void* it[16];
        int m = gChildren(ch[i], it, 16);
        if (!g_gdBtnLogged && m > 0) {
            const float* mp = gdx::getPos(ch[i]);
            logf("mods button: menu %s at %.0f,%.0f with %d items, first at %.0f,%.0f size %.0fx%.0f", cn, mp[0], mp[1], m,
                gdx::getPos(it[0])[0], gdx::getPos(it[0])[1], gdx::getSize(it[0])[0], gdx::getSize(it[0])[1]);
        }
        void* row[16];
        int r = gRow(ch[i], row);
        if (r < 4 || r > 8) continue;
        float minH = 1e9f;
        for (int k = 0; k < r; k++) minH = std::min(minH, gdx::getSize(row[k])[1]);
        float wy = gdx::getPos(ch[i])[1] + gdx::getPos(row[0])[1];
        float mid = gdx::getPos(ch[i])[0] + (gdx::getPos(row[0])[0] + gdx::getPos(row[r - 1])[0]) * 0.5f; // centred row
        if (minH < 15 || wy > ws[1] * 0.5f || std::fabs(mid - ws[0] * 0.5f) > ws[0] * 0.25f) continue;
        if (wy < bestY) { bestY = wy; best = ch[i]; }
    }
    if (!best) {
        g_gdBtnFailed = true;
        if (!g_gdBtnLogged) logf("mods button: bottom row of the main menu not found (screen %.0fx%.0f, %d children)", ws[0], ws[1], n);
        g_gdBtnLogged = true;
        return;
    }
    std::wstring path = g_dir + L"kmenu-mods-btn.png";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeLow != sizeof(kModsBtnPng)) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, kModsBtnPng, sizeof(kModsBtnPng), &wr, nullptr); CloseHandle(h); }
    }
    void* spr = gdx::sprite(narrow(path).c_str());
    if (!spr) { g_gdBtnFailed = true; logf("mods button: could not load kmenu-mods-btn.png"); return; }
    void* it[16];
    int m = gRow(best, it);
    float minX = gdx::getPos(it[0])[0], maxX = gdx::getPos(it[m - 1])[0], y0 = gdx::getPos(it[0])[1], refH = gdx::getSize(it[m - 1])[1];
    float step = (maxX - minX) / (m - 1);
    void* btn = gdx::item(spr, nullptr, g_gdMenuLayer, reinterpret_cast<void*>(&gdCallback));
    if (!btn) { g_gdBtnFailed = true; return; }
    g_gdBtnBase = g_gdBtnCur = refH / std::max(gdx::getSize(spr)[1], 1.0f);
    gdx::setScale(btn, g_gdBtnBase);
    gAdd(best, btn, 0);
    gPos(btn, maxX + step, y0);
    // keep the row centred, like Geode does
    for (int k = 0; k < m; k++) { const float* p = gdx::getPos(it[k]); gPos(it[k], p[0] - step / 2, p[1]); }
    gPos(btn, maxX + step / 2, y0);
    g_gdBtn = btn;
    logf("mods button: added to the main menu");
}

// ---- the "Kuang Mods" screen: its own scene (like Geode's mod list), tabs Installed / Download / Recent
void* g_kmScene = nullptr;     // our scene while it exists (pushed over the main menu)
bool g_kmClosing = false;
int g_kmTab = 0;               // 0 installed, 1 download, 2 recent
void* (*g_sceneCreate)() = nullptr;
void* (*g_fadeCreate)(float, void*) = nullptr;
bool (*g_pushScene)(void*, void*) = nullptr;
bool (*g_popSceneT)(void*, float, int) = nullptr;
void (*g_popScene)(void*) = nullptr;

bool modInstalled(int i) { return i < 2 || GetModuleHandleW(i == 2 ? KFE_DLL_NAME : KOPT_DLL_NAME) != nullptr; }
std::wstring installerPath() {
    wchar_t la[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    for (const std::wstring& p : {g_dir + L"installer.exe", g_dir + L"Kuang-Installer\\installer.exe",
             std::wstring(la) + L"\\Programs\\Kuang\\installer.exe"})
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return {};
}

void gdTabs(void* menu, float cx, float y) {
    const char* keys[] = {"km.tab.inst", "km.tab.down", "km.tab.recent"};
    float bw = 112, gap = 8;
    for (int k = 0; k < 3; k++) {
        void* b = gNine(k == g_kmTab ? "GJ_button_01.png" : "GJ_button_04.png", bw, 30);
        if (!b) continue;
        if (void* l = gLabel(gdText(T(keys[k])), "goldFont.fnt", 0.6f, bw - 14)) { gPos(l, bw / 2, 16); gAdd(b, l, 1); }
        gdAddItem(menu, b, 400 + k, 1.0f, cx + (k - 1) * (bw + gap), y);
    }
}

// Installed: mod list on the left, options of the selected mod on the right (inside the rect x,y,W,H)
void gdInstalled(void* content, void* menu, float x, float y, float W, float H) {
    const unsigned char panel[4] = {0, 0, 0, 70}, none[4] = {0, 0, 0, 0}, sel[4] = {255, 255, 255, 45};
    const unsigned char grey[3] = {120, 120, 120};
    float top = y + H - 12, bottom = y + 12, lx = x + 12, lw = 150;
    if (void* lp = gdx::layerColor(panel, lw, top - bottom)) { gPos(lp, lx, bottom); gAdd(content, lp, 1); }
    const char* names[] = {"Kuang Menu", "CBF", "Frame Extrapolation", "Optimizer"};
    if (!modInstalled(g_gdTab)) g_gdTab = 0;
    int row = 0;
    for (int i = 0; i < 4; i++) {
        if (!modInstalled(i)) continue;
        void* box = gdx::layerColor(i == g_gdTab ? sel : none, lw - 4, 40);
        if (!box) continue;
        if (void* nl = gLabel(names[i], "bigFont.fnt", 0.45f, lw - 16)) { gAnchor(nl, 0, 0.5f); gPos(nl, 6, 27); gAdd(box, nl, 1); }
        const char* msg;
        int st = modState(i, &msg);
        // gold text = active, white = the rest (label colours are not used: see kSprRgba)
        if (void* s = gLabel(gdText(modStateText(st)), st == 1 ? "goldFont.fnt" : "chatFont.fnt", st == 1 ? 0.4f : 0.6f, lw - 16)) {
            gAnchor(s, 0, 0.5f);
            gPos(s, 6, 10);
            gAdd(box, s, 1);
        }
        gdAddItem(menu, box, 200 + i, 1.0f, lx + lw / 2, top - 24 - row++ * 44);
    }
    float rx0 = lx + lw + 8, rx1 = x + W - 12, rw = rx1 - rx0, descH = 62;
    if (void* rp = gdx::layerColor(panel, rw, top - bottom)) { gPos(rp, rx0, bottom); gAdd(content, rp, 1); }
    auto ops = gdOptions();
    int n = static_cast<int>(ops.size());
    int per = std::max(1, static_cast<int>((top - bottom - descH - 6) / 30));
    int pages = std::max(1, (n + per - 1) / per);
    g_gdPage = std::clamp(g_gdPage, 0, pages - 1);
    for (int j = 0; j < per; j++) {
        int i = g_gdPage * per + j;
        if (i >= n) break;
        const GdOpt& o = ops[i];
        float yy = top - 18 - j * 30;
        if (o.kind == 0) {
            if (void* ck = gdx::spriteFrame(o.on ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png")) {
                if (!o.enabled) gdx::spriteColor(reinterpret_cast<char*>(ck) + kSprRgba, grey);
                gdAddItem(menu, ck, i, 0.65f, rx0 + 18, yy);
            }
            if (void* l = gLabel(gdText(o.label.c_str()), "bigFont.fnt", 0.42f, rw - 70)) {
                gAnchor(l, 0, 0.5f);
                gPos(l, rx0 + 36, yy);
                gAdd(content, l, 2);
            }
        } else {
            float bw = rw - 44;
            if (void* b = gNine("GJ_button_01.png", bw, 24)) {
                if (void* l = gLabel(gdText(o.label.c_str()), "goldFont.fnt", 0.55f, bw - 14)) { gPos(l, bw / 2, 13); gAdd(b, l, 1); }
                gdAddItem(menu, b, i, 1.0f, rx0 + 6 + bw / 2, yy);
            }
        }
        if (void* inf = gdx::spriteFrame("GJ_infoIcon_001.png")) gdAddItem(menu, inf, 100 + i, 0.6f, rx1 - 14, yy);
    }
    if (pages > 1) {
        float ay = bottom + descH + 2;
        if (void* a = gdx::spriteFrame("GJ_arrow_01_001.png")) gdAddItem(menu, a, 300, 0.35f, rx0 + 12, ay);
        if (void* a = gdx::spriteFrame("GJ_arrow_01_001.png")) { gdx::spriteFlipX(a, true); gdAddItem(menu, a, 301, 0.35f, rx1 - 12, ay); }
        char pg[16];
        std::snprintf(pg, sizeof(pg), "%d/%d", g_gdPage + 1, pages);
        if (void* l = gLabel(pg, "chatFont.fnt", 0.6f, 0)) { gPos(l, rx0 + rw / 2, ay); gAdd(content, l, 2); }
    }
    std::string d;
    if (g_gdDesc == -2) d = T("mods.app.missing");
    else if (g_gdDesc >= 0 && g_gdDesc < n) d = ops[g_gdDesc].desc;
    else d = T(g_gdTab == 0 ? "mods.menu.d" : g_gdTab == 1 ? "mods.cbf.d" : g_gdTab == 2 ? "mods.kfe.d" : "mods.opt.d");
    const float ds = 0.6f;
    gWrapped(content, gdText(d.c_str()), ds, rw - 14, rx0 + rw / 2, bottom + descH / 2 - 2);
}

// a row of the Download / Recent lists: dark band with a title and a line of text
void gdRow(void* content, float x, float y, float w, const std::string& title, const std::string& text, float textW) {
    const unsigned char band[4] = {0, 0, 0, 70};
    if (void* b = gdx::layerColor(band, w, 46)) { gPos(b, x, y - 23); gAdd(content, b, 1); }
    if (void* l = gLabel(gdText(title.c_str()), "bigFont.fnt", 0.5f, textW)) { gAnchor(l, 0, 0.5f); gPos(l, x + 10, y + 9); gAdd(content, l, 2); }
    if (void* l = gLabel(gdText(text.c_str()), "chatFont.fnt", 0.65f, textW)) { gAnchor(l, 0, 0.5f); gPos(l, x + 10, y - 10); gAdd(content, l, 2); }
}

void gdDownload(void* content, void* menu, float x, float y, float W, float H) {
    struct M { int i; const char* name; const char* dk; };
    const M mods[] = {{2, "Kuang Frame Extrapolation", "mods.kfe.d"}, {3, "Kuang Optimizer", "mods.opt.d"}};
    float top = y + H - 34;
    int row = 0;
    for (const M& m : mods) {
        if (modInstalled(m.i)) continue;
        float yy = top - row++ * 54;
        gdRow(content, x + 12, yy, W - 24, m.name, T(m.dk), W - 160);
        if (void* b = gNine("GJ_button_01.png", 100, 28)) {
            if (void* l = gLabel(gdText(T("km.install")), "goldFont.fnt", 0.6f, 86)) { gPos(l, 50, 15); gAdd(b, l, 1); }
            gdAddItem(menu, b, 500 + m.i, 1.0f, x + W - 72, yy);
        }
    }
    std::string d = row == 0 ? T("km.all") : g_gdDesc == -3 ? T("km.noinst") : T("km.down.d");
    gWrapped(content, gdText(d.c_str()), 0.65f, W - 40, x + W / 2, row == 0 ? y + H / 2 : y + 34);
}

void gdRecent(void* content, float x, float y, float W, float H) {
    struct R { const char* title; const char* key; };
    const R items[] = {{"Kuang Menu 0.13.2", "km.r1"}, {"Kuang Menu 0.13.0", "km.r2"}, {"Kuang Menu 0.12.0", "km.r3"},
        {"Kuang Menu 0.11.1", "km.r4"}, {"Kuang Frame Extrapolation 1.2.0", "km.r5"}};
    float top = y + H - 34;
    int fit = std::max(1, static_cast<int>((H - 20) / 54));
    for (int k = 0; k < 5 && k < fit; k++) gdRow(content, x + 12, top - k * 54, W - 24, items[k].title, T(items[k].key), W - 44);
}

void gdBuild() {
    if (g_gdContent) { gdx::remove(g_gdContent, true); g_gdContent = nullptr; }
    g_gdItems.clear();
    g_gdDirty = false;
    g_gdBuiltAt = now();
    g_gdStep = "background";
    float ws[2] = {569, 320};
    gdx::winSize(g_sharedDirector(), ws);
    void* content = gdx::layer();
    gAdd(g_gdRoot, content, 1);
    g_gdContent = content;
    // background like the game's own screens: blue gradient and the corner art
    g_gdStep = "GJ_gradientBG.png";
    if (void* bg = gdx::sprite("GJ_gradientBG.png")) {
        const float* s = gdx::getSize(bg);
        gAnchor(bg, 0, 0);
        gdx::setScaleX(bg, ws[0] / std::max(s[0], 1.0f));
        gdx::setScaleY(bg, ws[1] / std::max(s[1], 1.0f));
        const unsigned char blue[3] = {0, 102, 255};
        gdx::spriteColor(reinterpret_cast<char*>(bg) + kSprRgba, blue);
        gAdd(content, bg, -2);
    }
    if (void* a = gdx::spriteFrame("GJ_sideArt_001.png")) { gAnchor(a, 0, 0); gPos(a, -2, -2); gAdd(content, a, -1); }
    if (void* a = gdx::spriteFrame("GJ_sideArt_001.png")) { gdx::spriteFlipX(a, true); gAnchor(a, 1, 0); gPos(a, ws[0] + 2, -2); gAdd(content, a, -1); }
    g_gdStep = "title";
    if (void* t = gLabel("Kuang Mods", "goldFont.fnt", 0.9f, 0)) { gPos(t, ws[0] / 2, ws[1] - 20); gAdd(content, t, 1); }
    void* menu = gdx::menu();
    gPos(menu, 0, 0);
    if (void* back = gdx::spriteFrame("GJ_arrow_03_001.png")) gdAddItem(menu, back, 302, 1.0f, 25, ws[1] - 25);
    g_gdStep = "tabs";
    gdTabs(menu, ws[0] / 2, ws[1] - 55);
    g_gdStep = "panel";
    float px = 40, py = 12, pw = ws[0] - 80, ph = ws[1] - 88;
    if (void* bg = gNine("GJ_square01.png", pw, ph)) { gPos(bg, px, py); gAdd(content, bg, 0); }
    if (g_kmTab == 0) gdInstalled(content, menu, px, py, pw, ph);
    else if (g_kmTab == 1) gdDownload(content, menu, px, py, pw, ph);
    else gdRecent(content, px, py, pw, ph);
    g_gdStep = "menu";
    gAdd(content, menu, 5);
    g_gdStep = "";
}

void gdOpen() {
    g_gdStep = "open";
    if (g_kmScene || !g_sceneCreate || !g_pushScene) return;
    void* scene = g_sceneCreate();
    void* root = scene ? gdx::layer() : nullptr;
    if (!root) return;
    gAdd(scene, root, 0);
    g_kmScene = scene;
    g_gdRoot = root;
    g_gdContent = nullptr;
    g_gdPage = 0;
    g_gdDesc = -1;
    gdBuild();
    g_gdStep = "push scene";
    void* dir = g_sharedDirector();
    void* tr = g_fadeCreate ? g_fadeCreate(0.5f, scene) : nullptr;
    g_pushScene(dir, tr ? tr : scene);
    g_kmClosing = false;
}

void gdClose() {
    if (!g_kmScene || g_kmClosing) return;
    g_kmClosing = true;
    void* dir = g_sharedDirector();
    // plain pop (no fade): the faded pop left the main menu black
    if (g_popScene) g_popScene(dir);
    else if (g_popSceneT) g_popSceneT(dir, 0.5f, 0 /*fade*/);
}

void gdBounce(void* it, float base, float& cur, float mul, float dt) {
    float target = gdx::isSelected(it) ? base * mul : base;
    cur += (target - cur) * std::min(1.0f, dt * 18.0f);
    gdx::setScale(it, cur);
}

void* g_gdBtnLayer = nullptr; // MenuLayer the button was put in (it survives while our scene is on top)

void gdTick() {
    if (!gdxInit() || !g_sharedDirector) return;
    if (!g_sceneCreate) {
        HMODULE cc = GetModuleHandleW(L"libcocos2d.dll");
        g_sceneCreate = reinterpret_cast<void* (*)()>(GetProcAddress(cc, "?create@CCScene@cocos2d@@SAPEAV12@XZ"));
        g_fadeCreate = reinterpret_cast<void* (*)(float, void*)>(GetProcAddress(cc, "?create@CCTransitionFade@cocos2d@@SAPEAV12@MPEAVCCScene@2@@Z"));
        g_pushScene = reinterpret_cast<bool (*)(void*, void*)>(GetProcAddress(cc, "?pushScene@CCDirector@cocos2d@@QEAA_NPEAVCCScene@2@@Z"));
        g_popSceneT = reinterpret_cast<bool (*)(void*, float, int)>(GetProcAddress(cc, "?popSceneWithTransition@CCDirector@cocos2d@@QEAA_NMW4PopTransition@2@@Z"));
        g_popScene = reinterpret_cast<void (*)(void*)>(GetProcAddress(cc, "?popScene@CCDirector@cocos2d@@QEAAXXZ"));
    }
    void* dir = g_sharedDirector();
    void* scene = dir ? g_runningScene(dir) : nullptr;
    static double last = now();
    float dt = static_cast<float>(std::min(0.1, now() - last));
    last = now();
    static void* logScene = nullptr;
    if (g_kmScene && scene != logScene) {
        logScene = scene;
        logf("mods screen: running scene is now %s", scene && rttiName(scene) ? rttiName(scene) : "none");
    }
    g_gdOpen = scene && scene == g_kmScene;
    if (g_gdOpen) { // our screen
        if (g_gdWantClose) { g_gdWantClose = false; gdClose(); }
        if (g_kmClosing) return;
        bool pressing = false;
        for (auto& it : g_gdItems) if (gdx::isSelected(it.item)) pressing = true;
        if (g_gdDirty || (!pressing && now() - g_gdBuiltAt > 2.0)) gdBuild(); // states refresh every 2 s
        for (auto& it : g_gdItems) gdBounce(it.item, it.base, it.cur, 1.15f, dt);
        return;
    }
    if (g_kmScene && g_kmClosing && scene && !gIsA(scene, ".?AVCCTransitionFade@cocos2d@@") &&
        rttiName(scene) && !std::strstr(rttiName(scene), "Transition")) {
        g_kmScene = g_gdRoot = g_gdContent = nullptr; // popped: the scene and everything in it is gone
        g_gdItems.clear();
        g_kmClosing = false;
        logf("mods screen: closed, back on %s", rttiName(scene));
    }
    if (scene != g_gdScene) {
        g_gdScene = scene;
        g_gdMenuLayer = nullptr;
        g_gdWantOpen = g_gdWantClose = false;
        g_gdBtnFailed = false;
        g_gdScanAt = 0;
    }
    if (!scene || g_kmScene) return; // a transition to or from our screen is running
    if (!g_gdMenuLayer) {
        void* ch[32]; // every frame until the main menu shows up, so the button is there from its first frame
        int n = gChildren(scene, ch, 32);
        for (int i = 0; i < n; i++) if (gIsA(ch[i], ".?AVMenuLayer@@")) g_gdMenuLayer = ch[i];
        if (!g_gdMenuLayer) return;
        if (g_gdMenuLayer != g_gdBtnLayer) g_gdBtn = nullptr; // another main menu: the button has to be added again
    }
    if (!g_gdBtn) { gdAddButton(); if (g_gdBtn) g_gdBtnLayer = g_gdMenuLayer; }
    if (g_gdBtn) gdBounce(g_gdBtn, g_gdBtnBase, g_gdBtnCur, 1.26f, dt);
    if (g_gdWantOpen) { g_gdWantOpen = false; gdOpen(); }
}

void gdTickSEH() {
    if (gdx::state < 0) return;
    __try {
        gdTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gdx::state = -1;
        logf("mods button: exception (%08lx) at step '%s', turned off for this session", GetExceptionCode(), g_gdStep);
    }
}

// ---- Kuang CBF: exact Click Between Steps ----------------------------------------------------------------
// The game's own Click Between Steps (GJBaseGameLayer::update, 0x3406d0) only splits a physics step in two
// HALVES: a click that arrives in the second half of a step is applied at the middle of it, one that arrives in the
// first half at its start, so every click lands on a 1/480 s grid (up to 2.08 ms early). This replaces the split by
// an exact one: the step is cut at the very instant the click was stamped (any fraction between 1/64 and 63/64 of
// the step), so the click lands where it really happened. Everything else (the Raw Input thread, the time stamps,
// which clicks matter, the step counters) stays the game's own.
// How: three 14-byte jumps inside GJBaseGameLayer::update go to small routines (kcbf_blob.h, source in
// kcbf_cave.S). They are put in when the option is switched on and the original bytes are written back when it is
// switched off, always from the render hook (same thread as update, never inside it). If a single byte of the three
// places differs from the 2.2091 ones nothing is touched. With a level's own Time Warp (GJBaseGameLayer +0x330 other than 1.0) the routines
// do exactly what the game does (split in half). The menu's Speedhack does not use that field (cocos scheduler
// scale): there the click stamps are rescaled in h_queueKey and the exact cut stays active.
unsigned char* g_cbfMem = nullptr;
bool g_cbfPatched = false;
uint64_t g_cbfLastCount = 0;
double g_cbfLogAt = 0;

bool cbfBytesMatchSEH(uintptr_t base) {
    for (const auto& s : kcbf_blob::kSites)
        if (!sigMatchSEH(reinterpret_cast<const unsigned char*>(base + s.rva), s.orig, s.len)) return false;
    return true;
}

bool cbfWriteSEH(void* dst, const void* src, size_t n) {
    __try {
        DWORD old = 0;
        if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(dst, src, n);
        DWORD tmp = 0;
        VirtualProtect(dst, n, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), dst, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool cbfApply() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!cbfBytesMatchSEH(base)) {
        logf("exact CBF: the game's code does not match 2.2091 (not applied, nothing was changed)");
        return false;
    }
    if (!g_cbfMem) {
        g_cbfMem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_cbfMem) { logf("exact CBF: could not allocate memory (%lu)", GetLastError()); return false; }
        std::memcpy(g_cbfMem, kcbf_blob::kBlob, kcbf_blob::kBlobSize);
        uint64_t ret[4] = {base + kcbf_blob::kSites[0].retRva, base + kcbf_blob::kSites[1].retRva,
                           base + kcbf_blob::kSites[2].retRva, base + kcbf_blob::kRet3Rva};
        std::memcpy(g_cbfMem + kcbf_blob::kOffRet0, ret, sizeof(ret)); // ret0..ret3 are consecutive slots
        uint64_t twOn = (g_cbfTw && g_latHooked) ? 1 : 0;
        g_cbfTwLive = twOn != 0;
        std::memcpy(g_cbfMem + kcbf_blob::kOffTw, &twOn, 8);
        FlushInstructionCache(GetCurrentProcess(), g_cbfMem, 4096);
    }
    // all three jumps are written; if one fails the ones already written are undone
    size_t done = 0;
    for (const auto& s : kcbf_blob::kSites) {
        unsigned char patch[64];
        std::memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xFF; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0; // jmp [rip+0]
        uint64_t to = reinterpret_cast<uint64_t>(g_cbfMem + s.caveOff);
        std::memcpy(patch + 6, &to, 8);
        if (!cbfWriteSEH(reinterpret_cast<void*>(base + s.rva), patch, s.len)) break;
        done++;
    }
    if (done != std::size(kcbf_blob::kSites)) {
        for (size_t i = 0; i < done; i++)
            cbfWriteSEH(reinterpret_cast<void*>(base + kcbf_blob::kSites[i].rva), kcbf_blob::kSites[i].orig, kcbf_blob::kSites[i].len);
        logf("exact CBF: could not write the code (undone)");
        return false;
    }
    g_cbfPatched = true;
    g_cbfLastCount = *reinterpret_cast<volatile uint64_t*>(g_cbfMem + kcbf_blob::kOffCount);
    logf("exact CBF: on (the game's Click Between Steps now cuts the step at the exact time of the click)");
    return true;
}

void cbfRevert() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool ok = true;
    for (const auto& s : kcbf_blob::kSites)
        ok = cbfWriteSEH(reinterpret_cast<void*>(base + s.rva), s.orig, s.len) && ok;
    g_cbfPatched = !ok; // if something failed it is still on; it is retried next frame
    if (ok) g_cbfTwLive = false;
    if (ok) logf("exact CBF: off (the game's own Click Between Steps)");
}

// every frame, from the render hook
void cbfTick() {
    if (g_cbfFailed) return;
    if (cbfWanted() && !g_cbfPatched) {
        if (!cbfApply()) {
            g_cbfFailed = true;
            g_cbfExact = false;
            cfgSetBool("gl.cbf", false);
        }
        return;
    }
    if (!cbfWanted() && g_cbfPatched) { cbfRevert(); return; }
    if (!g_cbfPatched || !g_cbfMem) return;
    bool twLive = g_cbfTw && g_latHooked; // needs the processQueuedButtons hook (see h_procQueued)
    *reinterpret_cast<volatile uint64_t*>(g_cbfMem + kcbf_blob::kOffTw) = twLive ? 1 : 0; // menu switch
    g_cbfTwLive = twLive;
    double t = now();
    if (g_cbfLogAt == 0) g_cbfLogAt = t;
    if (t - g_cbfLogAt < 10.0) return;
    g_cbfLogAt = t;
    uint64_t c = *reinterpret_cast<volatile uint64_t*>(g_cbfMem + kcbf_blob::kOffCount);
    if (c != g_cbfLastCount) {
        logf("[cbf] exact cuts in the last 10 s: %llu (total %llu)", static_cast<unsigned long long>(c - g_cbfLastCount), static_cast<unsigned long long>(c));
        g_cbfLastCount = c;
    }
}

unsigned char* g_cbfMemView() { return g_cbfMem; }

// ---- Kuang Precision Fix: frame time from the high-resolution clock ----------------------------------------
// CCDirector::calculateDeltaTime (libcocos2d.dll 2.2091, RVA 0xc1840) builds the frame time from a timeval in whole microseconds.
// One 14-byte jump replaces its time math; the routine (kpfix_blob.h) uses QueryPerformanceCounter directly. If the game's own
// "last time" ever differs from the one it remembers (level load / pause reset) or the step is odd (> 1 s), it runs the original math.
bool pfixBytesMatchSEH(uintptr_t base) {
    return sigMatchSEH(reinterpret_cast<const unsigned char*>(base + kpfix_blob::kRva), kpfix_blob::kOrig, kpfix_blob::kLen);
}

bool pfixApply() {
    HMODULE m = GetModuleHandleW(L"libcocos2d.dll");
    if (!m) { logf("Precision Fix: libcocos2d.dll not found"); return false; }
    uintptr_t base = reinterpret_cast<uintptr_t>(m);
    if (!pfixBytesMatchSEH(base)) { logf("Precision Fix: libcocos2d.dll does not match 2.2091 (not applied, nothing was changed)"); return false; }
    LARGE_INTEGER fq{};
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void* qpc = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "QueryPerformanceCounter")) : nullptr;
    if (!qpc || !QueryPerformanceFrequency(&fq) || fq.QuadPart <= 0) { logf("Precision Fix: no high-resolution clock"); return false; }
    if (!g_pfixMem) {
        g_pfixMem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_pfixMem) { logf("Precision Fix: could not allocate memory (%lu)", GetLastError()); return false; }
        std::memcpy(g_pfixMem, kpfix_blob::kBlob, kpfix_blob::kBlobSize);
    }
    double freq = static_cast<double>(fq.QuadPart);
    uint64_t ret = base + kpfix_blob::kRetRva, zero = 0, q = reinterpret_cast<uint64_t>(qpc);
    std::memcpy(g_pfixMem + kpfix_blob::kOffQpc, &q, 8);
    std::memcpy(g_pfixMem + kpfix_blob::kOffFreq, &freq, 8);
    std::memcpy(g_pfixMem + kpfix_blob::kOffRet, &ret, 8);
    std::memcpy(g_pfixMem + kpfix_blob::kOffLastQ, &zero, 8);
    std::memcpy(g_pfixMem + kpfix_blob::kOffLastTv, &zero, 8);
    FlushInstructionCache(GetCurrentProcess(), g_pfixMem, 4096);
    unsigned char patch[64];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xFF; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0; // jmp [rip+0]
    uint64_t to = reinterpret_cast<uint64_t>(g_pfixMem + kpfix_blob::kCaveOff);
    std::memcpy(patch + 6, &to, 8);
    if (!cbfWriteSEH(reinterpret_cast<void*>(base + kpfix_blob::kRva), patch, kpfix_blob::kLen)) { logf("Precision Fix: could not write the code"); return false; }
    g_pfixPatched = true;
    logf("Precision Fix: on (frame time from QueryPerformanceCounter, clock %.0f Hz)", freq);
    return true;
}

void pfixRevert() {
    HMODULE m = GetModuleHandleW(L"libcocos2d.dll");
    if (!m) { g_pfixPatched = false; return; }
    bool ok = cbfWriteSEH(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m) + kpfix_blob::kRva), kpfix_blob::kOrig, kpfix_blob::kLen);
    g_pfixPatched = !ok; // retried next frame if it failed
    if (ok) logf("Precision Fix: off (the game's own frame time)");
}

double g_pfixLogAt = 0;
uint64_t g_pfixLastN = 0;

// every frame, from the render hook (same thread as calculateDeltaTime)
void pfixTick() {
    if (g_pfixFailed) return;
    if (g_pfixOn && !g_pfixPatched) {
        if (!pfixApply()) { g_pfixFailed = true; g_pfixOn = false; cfgSetBool("gl.pfix", false); }
        return;
    }
    if (!g_pfixOn && g_pfixPatched) { pfixRevert(); return; }
    if (!g_pfixPatched || !g_pfixMem) return;
    double t = now();
    if (g_pfixLogAt == 0) g_pfixLogAt = t;
    if (t - g_pfixLogAt < 10.0) return;
    g_pfixLogAt = t;
    uint64_t c = *reinterpret_cast<volatile uint64_t*>(g_pfixMem + kpfix_blob::kOffCnt), f = *reinterpret_cast<volatile uint64_t*>(g_pfixMem + kpfix_blob::kOffFb);
    logf("[pfix] precise frames: %llu (+%llu in 10 s), original-math frames: %llu", static_cast<unsigned long long>(c), static_cast<unsigned long long>(c - g_pfixLastN), static_cast<unsigned long long>(f));
    g_pfixLastN = c;
}

// every frame: refresh the label values and write the 10 s line
void latencyTick() {
    if (g_latTotal.n >= 3) {
        g_latShowAvg = g_latTotal.sum / g_latTotal.n * 1000.0;
        g_latShowP95 = g_latTotal.pct(0.95) * 1000.0;
        if (g_latTotal.n >= 40) g_latTotal = LatWin(); // keep it a recent window
    }
    double t = now();
    if (g_latLogAt == 0) g_latLogAt = t;
    if (t - g_latLogAt < 10.0) return;
    if (g_inLevel && g_latLogTotal.n > 0 && g_latLogLines < 200) {
        logf("[inputlat] presses %u | key -> physics: avg %.3f ms, p95 %.3f, max %.3f | key -> game queue (waiting for the frame): avg %.3f ms, max %.3f",
            g_latLogTotal.n, g_latLogTotal.sum / g_latLogTotal.n * 1000.0, g_latLogTotal.pct(0.95) * 1000.0, g_latLogTotal.max * 1000.0,
            g_latLogQueue.n ? g_latLogQueue.sum / g_latLogQueue.n * 1000.0 : 0.0, g_latLogQueue.max * 1000.0);
        g_latLogLines++;
    }
    g_latLogTotal = LatWin();
    g_latLogQueue = LatWin();
    g_latLogAt = t;
}

// the game's input thread (std::thread kept by libcocos2d: the instruction that stores it is checked first)
HANDLE g_inputThread = nullptr;
int g_inputThreadOrig = THREAD_PRIORITY_NORMAL;
bool g_inputThreadBoosted = false;
double g_inputThreadLook = 0;

HANDLE findInputThreadSEH() {
    __try {
        HMODULE cc = GetModuleHandleW(L"libcocos2d.dll");
        if (!cc) return nullptr;
        // libcocos2d keeps the std::thread of its Raw Input reader in a global: right after _beginthreadex it does
        // "movups xmm0,[rsp+0x40]; movaps [rip+disp],xmm0". The place moves with every game update, so look for
        // those bytes in the code (first the old known spot, then a scan); there is exactly one in 2.2091.
        auto b = reinterpret_cast<const unsigned char*>(cc);
        static const unsigned char pat[] = {0x0f, 0x10, 0x44, 0x24, 0x40, 0x0f, 0x29, 0x05};
        const unsigned char* ins = nullptr;
        if (std::memcmp(b + 0x7ba93, pat, sizeof(pat)) == 0) ins = b + 0x7ba98;
        if (!ins) {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
            auto sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && !ins; i++, sec++) {
                if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                const unsigned char* p = b + sec->VirtualAddress;
                size_t n = sec->Misc.VirtualSize;
                for (size_t k = 0; k + sizeof(pat) + 4 <= n; k++)
                    if (p[k] == 0x0f && std::memcmp(p + k, pat, sizeof(pat)) == 0) { ins = p + k + 5; break; }
            }
        }
        if (!ins) return nullptr;
        int32_t disp;
        std::memcpy(&disp, ins + 3, 4);
        HANDLE h = *reinterpret_cast<HANDLE const*>(ins + 7 + disp);
        if (!h) return nullptr;
        DWORD id = GetThreadId(h);
        if (id == 0 || id == GetCurrentThreadId()) return nullptr;
        return h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void inputThreadTick() {
    bool want = g_inputBoost;
    if (want == g_inputThreadBoosted) return;
    if (!g_inputThread) {
        double t = now();
        if (t - g_inputThreadLook < 2.0) return; // the thread may start a bit after the game
        g_inputThreadLook = t;
        g_inputThread = findInputThreadSEH();
        if (!g_inputThread) {
            static int tries = 0;
            if (++tries == 5) logf("input thread: not found in this version (Input Boost works without it)");
            return;
        }
        g_inputThreadOrig = GetThreadPriority(g_inputThread);
        if (g_inputThreadOrig == THREAD_PRIORITY_ERROR_RETURN) g_inputThreadOrig = THREAD_PRIORITY_NORMAL;
    }
    if (SetThreadPriority(g_inputThread, want ? THREAD_PRIORITY_TIME_CRITICAL : g_inputThreadOrig)) {
        logf("input thread %lu: priority %s", GetThreadId(g_inputThread), want ? "time-critical" : "back to normal");
        g_inputThreadBoosted = want;
    } else {
        g_inputThreadBoosted = want; // do not retry every frame
        logf("input thread: could not change its priority (%lu)", GetLastError());
    }
}

// ---- deep profile hooks (only with Render Stats on). Addresses of the 2026-10-20 patch, confirmed by the call graph:
// GJBaseGameLayer::update -> processMoveActionsStep(0x334220) -> dynamic(0x336e00), transform(0x335280),
// rotate(0x334450), move(0x336170), player follow(0x336a50), advanced follow(0x337c40), follow(0x339100);
// PlayLayer::updateVisibility -> preUpdateVisibility(0x33f0f0), GJEffectManager::processColors(0x365640).
using PrV = void (*)(void*);
using PrF = void (*)(void*, float);
using PrB = void (*)(void*, bool);
using PrFB = void (*)(void*, float, bool);
using PrIF = void (*)(void*, int, float);
void* o_prof[PR_COUNT] = {};
struct ProfTimer {
    int k; double t0;
    explicit ProfTimer(int kk) : k(kk), t0(g_rsCounting ? now() : 0) {}
    ~ProfTimer() { if (t0 > 0) g_fc.prof[k] += now() - t0; }
};
void h_prStep(void* s, float dt, bool vis) { ProfTimer pt(PR_STEP); reinterpret_cast<PrFB>(o_prof[PR_STEP])(s, dt, vis); }
void h_prMove(void* s) { ProfTimer pt(PR_MOVE); reinterpret_cast<PrV>(o_prof[PR_MOVE])(s); }
void h_prRotate(void* s) { ProfTimer pt(PR_ROTATE); reinterpret_cast<PrV>(o_prof[PR_ROTATE])(s); }
void h_prFollow(void* s) { ProfTimer pt(PR_FOLLOW); reinterpret_cast<PrV>(o_prof[PR_FOLLOW])(s); }
void h_prPFollow(void* s, float dt) { ProfTimer pt(PR_PFOLLOW); reinterpret_cast<PrF>(o_prof[PR_PFOLLOW])(s, dt); }
void h_prAFollow(void* s, float dt) { ProfTimer pt(PR_AFOLLOW); reinterpret_cast<PrF>(o_prof[PR_AFOLLOW])(s, dt); }
void h_prTransform(void* s, bool vis) { ProfTimer pt(PR_TRANSFORM); reinterpret_cast<PrB>(o_prof[PR_TRANSFORM])(s, vis); }
void h_prDynamic(void* s, int type, float dt) { ProfTimer pt(PR_DYNAMIC); reinterpret_cast<PrIF>(o_prof[PR_DYNAMIC])(s, type, dt); }
void h_prColors(void* s) { ProfTimer pt(PR_COLORS); reinterpret_cast<PrV>(o_prof[PR_COLORS])(s); }
void h_prPreVis(void* s, float dt) { ProfTimer pt(PR_PREVIS); reinterpret_cast<PrF>(o_prof[PR_PREVIS])(s, dt); }

void installProfileHooks() {
    g_profTried = true;
    struct P { int k; uintptr_t rva; unsigned char sig[12]; void* detour; };
    const P list[] = {
        {PR_STEP, 0x334220, {0x48, 0x89, 0x5c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec}, reinterpret_cast<void*>(&h_prStep)},
        {PR_MOVE, 0x336170, {0x48, 0x8b, 0xc4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48}, reinterpret_cast<void*>(&h_prMove)},
        {PR_ROTATE, 0x334450, {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x57, 0x41, 0x55, 0x48, 0x8d, 0x68, 0xc8}, reinterpret_cast<void*>(&h_prRotate)},
        {PR_FOLLOW, 0x339100, {0x4c, 0x8b, 0xdc, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x68, 0x48, 0x8b}, reinterpret_cast<void*>(&h_prFollow)},
        {PR_PFOLLOW, 0x336a50, {0x4c, 0x8b, 0xdc, 0x55, 0x56, 0x41, 0x55, 0x48, 0x81, 0xec, 0xe0, 0x00}, reinterpret_cast<void*>(&h_prPFollow)},
        {PR_AFOLLOW, 0x337c40, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89}, reinterpret_cast<void*>(&h_prAFollow)},
        {PR_TRANSFORM, 0x335280, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48}, reinterpret_cast<void*>(&h_prTransform)},
        {PR_DYNAMIC, 0x336e00, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55}, reinterpret_cast<void*>(&h_prDynamic)},
        {PR_COLORS, 0x365640, {0x40, 0x55, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9}, reinterpret_cast<void*>(&h_prColors)},
        {PR_PREVIS, 0x33f0f0, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0xf3, 0x0f, 0x11, 0x48, 0x10}, reinterpret_cast<void*>(&h_prPreVis)},
    };
    uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // all or nothing: a partial set would give misleading numbers
    for (auto& e : list)
        if (!sigMatchSEH(reinterpret_cast<const unsigned char*>(exeBase + e.rva), e.sig, sizeof(e.sig))) {
            logf("deep profile: function at 0x%llx does not match this version (deep profile off)", static_cast<unsigned long long>(e.rva));
            return;
        }
    int ok = 0;
    for (auto& e : list) {
        void* target = reinterpret_cast<void*>(exeBase + e.rva);
        if (MH_CreateHook(target, e.detour, &o_prof[e.k]) == MH_OK && MH_EnableHook(target) == MH_OK) ok++;
    }
    g_profHooked = ok == PR_COUNT;
    logf("deep profile: %d of %d hooks ready", ok, static_cast<int>(PR_COUNT));
}

// installed the first time Render Stats or the Visibility Throttle is turned on, only if the bytes match this game version
void installVisHook() {
    g_visTried = true;
    // g_base may not be set yet (this runs on the first frame, before installGameHooks), so ask for it here
    uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* uv = reinterpret_cast<void*>(exeBase + kUpdateVisRva);
    if (exeBase && sigMatchSEH(reinterpret_cast<const unsigned char*>(uv), kSigUpdateVis, sizeof(kSigUpdateVis)) &&
        MH_CreateHook(uv, reinterpret_cast<void*>(&h_updateVisibility), reinterpret_cast<void**>(&o_updateVisibility)) == MH_OK &&
        MH_EnableHook(uv) == MH_OK) {
        g_visHooked = true;
        logf("updateVisibility hook ready (render stats timer, visibility throttle)");
    } else {
        logf("updateVisibility not found in this version (timer and throttle off)");
    }
}

void installGlStats() {
    g_rsInstalled = true; // one attempt only
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) { logf("render stats: opengl32.dll not found"); return; }
    int ok = 0;
    auto hook = [&](const char* name, void* detour, void** orig) {
        void* target = reinterpret_cast<void*>(GetProcAddress(gl, name));
        if (target && MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) ok++;
        else logf("render stats: could not hook %s", name);
    };
    hook("glDrawElements", reinterpret_cast<void*>(&h_glDrawElements), reinterpret_cast<void**>(&o_glDrawElements));
    hook("glDrawArrays", reinterpret_cast<void*>(&h_glDrawArrays), reinterpret_cast<void**>(&o_glDrawArrays));
    hook("glBindTexture", reinterpret_cast<void*>(&h_glBindTexture), reinterpret_cast<void**>(&o_glBindTexture));
    hook("glBlendFunc", reinterpret_cast<void*>(&h_glBlendFunc), reinterpret_cast<void**>(&o_glBlendFunc));
    hook("glClear", reinterpret_cast<void*>(&h_glClear), reinterpret_cast<void**>(&o_glClear));
    logf("render stats: %d of 5 OpenGL hooks ready", ok);
}

// window accumulators; every ~0.5 s they become the averages the label shows
double g_rsPrevEnd = 0, g_rsWinStart = 0;
double g_rsSumLogic = 0, g_rsSumDraw = 0, g_rsSumSwap = 0, g_rsSumVis = 0;
double g_rsSumProf[PR_COUNT] = {};
uint64_t g_rsSumDraws = 0, g_rsSumTris = 0, g_rsSumTex = 0, g_rsSumBlend = 0, g_rsFrames = 0;
double g_rsLogAt = 0;
int g_rsLogLines = 0;

// tDrawEnd: the game finished submitting its drawing (just before our menu draws); tSwapped: swap returned
void rsFrameEnd(double tDrawEnd, double tSwapStart, double tSwapped) {
    if (g_rsPrevEnd > 0 && g_fc.draws > 0) {
        double logicEnd = g_fc.clearAt > 0 ? g_fc.clearAt : tDrawEnd;
        g_rsSumLogic += std::max(0.0, logicEnd - g_rsPrevEnd);
        g_rsSumDraw += g_fc.clearAt > 0 ? std::max(0.0, tDrawEnd - g_fc.clearAt) : 0.0;
        g_rsSumSwap += std::max(0.0, tSwapped - tSwapStart);
        g_rsSumVis += g_fc.visT;
        for (int k = 0; k < PR_COUNT; k++) g_rsSumProf[k] += g_fc.prof[k];
        g_rsSumDraws += g_fc.draws; g_rsSumTris += g_fc.tris; g_rsSumTex += g_fc.tex; g_rsSumBlend += g_fc.blend;
        g_rsFrames++;
    }
    g_fc = FrameCount();
    double t = now();
    g_rsPrevEnd = t;
    if (g_rsWinStart == 0) g_rsWinStart = t;
    if (t - g_rsWinStart < 0.5) return;
    if (g_rsFrames > 0) {
        double n = static_cast<double>(g_rsFrames);
        g_rs.logicMs = g_rsSumLogic / n * 1000.0;
        g_rs.drawMs = g_rsSumDraw / n * 1000.0;
        g_rs.swapMs = g_rsSumSwap / n * 1000.0;
        g_rs.visMs = g_visHooked ? g_rsSumVis / n * 1000.0 : -1.0;
        g_rs.draws = static_cast<double>(g_rsSumDraws) / n;
        g_rs.tris = static_cast<double>(g_rsSumTris) / n;
        g_rs.texBinds = static_cast<double>(g_rsSumTex) / n;
        g_rs.blendChanges = static_cast<double>(g_rsSumBlend) / n;
        g_rs.valid = true;
        for (int k = 0; k < PR_COUNT; k++) g_rsProfMs[k] = g_rsSumProf[k] / n * 1000.0;
        if (g_inLevel && t - g_rsLogAt >= 10.0 && g_rsLogLines < 200 && g_profHooked) {
            const double* m = g_rsProfMs;
            double follow = m[PR_FOLLOW] + m[PR_PFOLLOW] + m[PR_AFOLLOW];
            double otherTrig = std::max(0.0, m[PR_STEP] - m[PR_MOVE] - m[PR_ROTATE] - follow - m[PR_TRANSFORM] - m[PR_DYNAMIC]);
            double objects = std::max(0.0, g_rs.visMs - m[PR_COLORS] - m[PR_PREVIS]);
            logf("[deepprofile] CPU ms per frame: triggers %.3f (move %.3f, rotate %.3f, follow %.3f, transform %.3f, dynamic %.3f, area/other %.3f) | "
                 "colors %.3f | visibility object loop %.3f (+ pre %.3f) | rest of logic (physics, collisions...) %.3f | draw submit %.3f | swap/GPU %.3f",
                m[PR_STEP], m[PR_MOVE], m[PR_ROTATE], follow, m[PR_TRANSFORM], m[PR_DYNAMIC], otherTrig, m[PR_COLORS], objects, m[PR_PREVIS],
                std::max(0.0, g_rs.logicMs - m[PR_STEP] - std::max(0.0, g_rs.visMs)), g_rs.drawMs, g_rs.swapMs);
        }
        if (g_inLevel && t - g_rsLogAt >= 10.0 && g_rsLogLines < 200) {
            g_rsLogAt = t;
            g_rsLogLines++;
            logf("[renderstats] %.0f fps | draw calls %.0f, triangles %.0f, texture binds %.0f, blend changes %.0f | CPU ms per frame: logic %.3f, draw submit %.3f, swap/GPU wait %.3f | updateVisibility %.3f",
                n / (t - g_rsWinStart), g_rs.draws, g_rs.tris, g_rs.texBinds, g_rs.blendChanges, g_rs.logicMs, g_rs.drawMs, g_rs.swapMs, g_rs.visMs);
        }
    }
    g_rsSumLogic = g_rsSumDraw = g_rsSumSwap = g_rsSumVis = 0;
    for (double& v : g_rsSumProf) v = 0;
    g_rsSumDraws = g_rsSumTris = g_rsSumTex = g_rsSumBlend = g_rsFrames = 0;
    g_rsWinStart = t;
}

void h_swapBuffers(void* self) {
    bool rsOn = g_labels[L_RSTATS].on;
    if (rsOn && !g_rsInstalled) installGlStats();
    if ((rsOn || g_visThrottle) && !g_visTried) installVisHook();
    if (rsOn && !g_profTried) installProfileHooks();
    bool latOn = g_labels[L_INPUTLAT].on;
    if ((latOn || cbfWanted()) && !g_latTried) installLatHooks();
    g_latCounting = latOn && g_latHooked;
    cbfTick();
    cbfAccTick();
    gdTickSEH();
    pfixTick();
    g_rsCounting = rsOn && o_glDrawElements;
    double t0 = g_rsCounting ? now() : 0;
    g_ownDraw = true;
    frameSEH();
    g_ownDraw = false;
    double t1 = g_rsCounting ? now() : 0;
    o_swapBuffers(self);
    if (g_rsCounting) rsFrameEnd(t0, t1, now());
    else if (g_rsPrevEnd != 0) { g_rsPrevEnd = 0; g_rsWinStart = 0; g_fc = FrameCount(); g_rs.valid = false; }
    paceFrame();
    frameMeter();
    if (g_rsCounting) g_rsPrevEnd = now();
}

void loadSettings() {
    cfgLoad();
    g_lang = cfgInt("lang", 0);
    g_scale = std::clamp(cfgFloat("ui.scale", 1.0f), 0.6f, 2.0f);
    g_colW = std::clamp(cfgFloat("ui.width", 212.0f), 170.0f, 320.0f);
    g_noclip = cfgBool("pl.noclip", false);
    g_ncHitbox = cfgBool("pl.nchb", true);
    g_speedOn = cfgBool("gl.speed", false);
    g_speed = std::clamp(cfgFloat("gl.speed.v", 1.0f), kSpeedMin, kSpeedMax);
    g_speedAudio = cfgBool("gl.speed.audio", true);
    g_safeManual = cfgBool("gl.safe", false);
    g_anim = cfgBool("ui.anim", true);
    g_dim = cfgBool("ui.dim", true);
    g_menuKey = cfgInt("menu.key", VK_TAB);
    std::string a = cfgStr("accent", "");
    float r, gg, b;
    if (std::sscanf(a.c_str(), "%f,%f,%f", &r, &gg, &b) == 3) g_accent = safeAccent(ImVec4(r, gg, b, 1));
    g_accurate = cfgBool("pl.accurate", false);
    g_hideAttempts = cfgBool("pl.noatt", false);
    g_pctFix = cfgBool("pl.pctfix", true);
    g_traj = cfgBool("tj.on", false);
    g_trajP1 = cfgBool("tj.p1", true);
    g_trajP2 = cfgBool("tj.p2", true);
    g_trajTrail[0] = cfgBool("tj.trail1", false);
    g_trajTrail[1] = cfgBool("tj.trail2", false);
    for (int k = 0; k < 2; k++)
        for (int c = 0; c < 2; c++) {
            char key[24];
            std::snprintf(key, sizeof(key), "tj.c%d%d", k, c);
            int v = cfgInt(key, -1);
            if (v >= 0) for (int i = 0; i < 3; i++) g_trajCol[k][c][i] = ((v >> (16 - 8 * i)) & 255) / 255.0f;
        }
    g_trajLen = std::clamp(cfgFloat("tj.len2", 2.0f), 0.5f, 4.0f);
    g_trailOn = cfgBool("tr.on", false);
    g_trailShipOnly = cfgBool("tr.ship", true);
    g_smartSP = cfgBool("sp.smart", false);
    g_smartOrbs = cfgBool("sp.orbs", true);
    g_fpsOn = cfgBool("gl.fps", false);
    g_visThrottle = cfgBool("gl.vis", false);
    g_visHz = std::clamp(cfgFloat("gl.vis.hz", 480.0f), kVisHzMin, kVisHzMax);
    g_visAuto = cfgBool("gl.vis.auto", true);
    g_fpsTarget = std::clamp(cfgFloat("gl.fps.v", 360.0f), 1.0f, 1000000.0f);
    g_fpsPrecise = cfgBool("gl.fps.p", true);
    g_inputBoost = cfgBool("gl.ib", false);
    g_cbfExact = cfgBool("gl.cbf", false);
    g_cbfAcc = true; // step clock is part of CBF since 0.14.10 (the old "gl.cbf.acc" setting is ignored)
    g_modCbf = cfgBool("mod.cbf", true);
    // Precision Fix stays off: in 2.2091 CCDirector::calculateDeltaTime is never called (the game's run loop in
    // libcocos2d already measures each frame with QueryPerformanceCounter and calls setDeltaTime), so the patch never ran.
    g_pfixOn = false;
    g_steadyFps = cfgBool("gl.sfps", false);
    g_spSwitcher = cfgBool("sp.switch", true);
    g_spKeyPrev = cfgInt("sp.prev", VK_LEFT);
    g_spKeyNext = cfgInt("sp.next", VK_RIGHT);
    g_trailFile = cfgStr("tr.file", "");
    g_trailWidth = std::clamp(cfgFloat("tr.width", 1.0f), 0.2f, 4.0f);
    g_trailKeep = cfgBool("tr.keep", false);
    g_trailGlow = cfgBool("tr.glow", true);
    // after an update that changes the columns, arrange them again once (old positions overlapped)
    if (cfgInt("layout.v", 0) != kLayoutVersion) {
        for (auto it = g_cfg.begin(); it != g_cfg.end();) it = it->first.rfind("pos.", 0) == 0 ? g_cfg.erase(it) : std::next(it);
        g_cfg["layout.v"] = std::to_string(kLayoutVersion);
        g_cfgDirty = true;
    }
    for (const char* c : {"kuang", "player", "global", "startpos", "status", "jukebox", "extras", "utility"}) g_collapsed[c] = cfgBool(std::string("col.") + c, false);
    loadStatus();
}

DWORD WINAPI initThread(LPVOID) {
    for (int i = 0; i < 600 && !GetModuleHandleW(L"libcocos2d.dll"); i++) Sleep(100);
    HMODULE cc = GetModuleHandleW(L"libcocos2d.dll");
    if (!cc) { logf("libcocos2d.dll not found"); return 0; }
    g_sharedDirector = reinterpret_cast<void* (*)()>(GetProcAddress(cc, "?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ"));
    g_runningScene = reinterpret_cast<void* (*)(void*)>(GetProcAddress(cc, "?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ"));
    void* swap = reinterpret_cast<void*>(GetProcAddress(cc, "?swapBuffers@CCEGLView@cocos2d@@UEAAXXZ"));
    if (!g_sharedDirector || !g_runningScene || !swap) { logf("cocos2d functions not found"); return 0; }
    // wait for the main menu (game and Geode finished their own setup)
    for (int i = 0; i < 3000; i++) {
        Sleep(100);
        void* dir = g_sharedDirector();
        if (dir && g_runningScene(dir)) break;
    }
    Sleep(200); // the loading screen is up: everything is set up while the game loads
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { logf("MinHook init failed"); return 0; }
    if (MH_CreateHook(swap, reinterpret_cast<void*>(&h_swapBuffers), reinterpret_cast<void**>(&o_swapBuffers)) != MH_OK ||
        MH_EnableHook(swap) != MH_OK) {
        logf("could not hook swapBuffers");
        return 0;
    }
    logf("hooked swapBuffers, menu key %s", keyName(g_menuKey).c_str());
    installSpeedhack(cc);
    installTrailFns(cc);
    g_sharedApp = reinterpret_cast<void* (*)()>(GetProcAddress(cc, "?sharedApplication@CCApplication@cocos2d@@SAPEAV12@XZ"));
    g_appSetInterval = reinterpret_cast<void (*)(void*, double)>(GetProcAddress(cc, "?setAnimationInterval@CCApplication@cocos2d@@UEAAXN@Z"));
    g_dirGetInterval = reinterpret_cast<double (*)(void*)>(GetProcAddress(cc, "?getAnimationInterval@CCDirector@cocos2d@@QEAANXZ"));
    installGameHooks();
    installTrajectory(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), cc);
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_qpf);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        std::wstring p = path;
        g_dir = p.substr(0, p.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_dir + L"kmenu.log").c_str(), L"w");
        logf("Kuang Menu %s loaded", KMENU_VERSION);
        loadSettings();
        g_sessionStart = now();
        g_fpsStart = now();
        if (HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
