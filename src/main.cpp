#include <Arduino.h>
#include <TFT_eSPI.h>   // configurado 100% por build_flags en platformio.ini
#include <math.h>       // la necesitamos para sin()/cos() de la animacion de explosion

// --------------------------------------------------------------------------
// Resolucion logica que usamos para dibujar. OJO: asumimos que la pantalla
// queda en horizontal (480 de ancho x 320 de alto) despues de setRotation()
// en setup(). Si tu pantalla especifica es distinta, o queda rotada, avisa
// y se ajustan estos dos numeros y la linea de setRotation().
// --------------------------------------------------------------------------
#define ANCHO 480
#define ALTO 320

// Los pines del bus del TFT (CS, DC, RST, WR, RD, D0-D7) NO se definen aca:
// ya estan configurados por build_flags en tu platformio.ini
// (TFT_CS=5, TFT_DC=21, TFT_RST=15, TFT_WR=22, TFT_RD=23, TFT_D0..D7).
// TFT_eSPI los toma solo al compilar gracias a USER_SETUP_LOADED=1.

// --------------------------------------------------------------------------
// Mapeo de pines fisicos que SI seguimos manejando manualmente
// (ninguno de estos choca con los que ya usa el bus del TFT)
// --------------------------------------------------------------------------
#define PIN_JOY1_Y 32   // eje Y del joystick del jugador 1
#define PIN_JOY2_Y 33   // eje Y del joystick del jugador 2 (o navegacion en menus)
#define PIN_BTN1 26     // boton unico: confirmar en menus, pausar en el juego
#define PIN_BUZZER 4    // buzzer pasivo (el pin 25 quedo ocupado por el bus del TFT)

// --------------------------------------------------------------------------
// Objeto de la pantalla fisica + sprite monocromo que usamos como buffer
// --------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();          // maneja la comunicacion real con el TFT
TFT_eSprite oled = TFT_eSprite(&tft); // "pantalla" logica sobre la que dibujamos

// El sprite es de 1 bit por pixel: cada pixel vale 0 o 1. setBitmapColor()
// (llamado en setup) define a que color real se traduce cada valor al
// mandarlo a pantalla. Usamos estos dos nombres en vez de SSD1306_WHITE/
// SSD1306_BLACK para que el resto del codigo quede igual de legible.
#define COLOR_ON  1
#define COLOR_OFF 0

// --------------------------------------------------------------------------
// Paleta visual inspirada en la version simple de 1.txt.
// El sprite sigue siendo monocromo para no disparar el uso de RAM; despues
// de enviarlo al TFT superponemos solo los elementos que queremos en color.
// --------------------------------------------------------------------------
#define COLOR_FONDO TFT_BLACK
#define COLOR_BOLA  TFT_WHITE
#define COLOR_P1    TFT_CYAN
#define COLOR_P2    TFT_ORANGE
#define COLOR_LINEA 0x4208
#define COLOR_TEXTO TFT_WHITE

// ==========================================================================
// AUDIO - efectos de sonido
// ==========================================================================
// Usamos tone() en vez de generar el tono a mano porque no bloquea la
// ejecucion: el ESP32 se encarga de apagar el pin solo cuando se cumple
// la duracion indicada. Los delay() cortos que aparecen dentro de algunos
// efectos (power-up, punto, explosion) sirven para poder encadenar varias
// notas seguidas y que se escuchen como una secuencia y no como un solo
// tono; el costo es que esos milisegundos el loop principal queda
// "congelado", pero como son efectos de menos de 300ms no se nota en la
// jugabilidad.

void sonarTono(int frecuencia, int duracion) {
  tone(PIN_BUZZER, frecuencia, duracion);
}

void sonidoRebotePared() {
  sonarTono(350, 25); // Tono grave y corto
}

void sonidoRebotePaleta() {
  sonarTono(750, 35); // Tono medio
}

void sonidoPowerUp() {
  // Arpegio ascendente rapido
  sonarTono(800, 40);
  delay(40);
  sonarTono(1200, 40);
  delay(40);
  sonarTono(1600, 60);
}

void sonidoPunto() {
  // Tono descendente al perder/ganar punto
  sonarTono(523, 100); // C5
  delay(100);
  sonarTono(261, 200); // C4
}

void sonidoNavegacion() {
  sonarTono(1000, 15); // Click leve
}

void sonidoBoton() {
  sonarTono(1400, 40); // Confirmacion
}

void sonidoExplosionBuzzer() {
  // Ruido de frecuencias bajas aleatorias para simular explosion
  for (int i = 0; i < 4; i++) {
    sonarTono(random(120, 350), 30);
    delay(35);
  }
}

// (La musica de fondo se define mas abajo, porque depende de la variable
// factorVel, y esa se declara junto con el resto de la fisica de la pelota)

// ==========================================================================
// MAQUINA DE ESTADOS DEL JUEGO
// ==========================================================================
enum EstadoJuego { BIENVENIDA, MENU, DIFICULTAD, PUNTOS, JUEGO, PAUSA, EXPLOSION, GAME_OVER };
EstadoJuego estadoActual = BIENVENIDA;

enum ModoJuego { MODO_PVP, MODO_PVE };
ModoJuego modoActual = MODO_PVP;

enum Dificultad { FACIL, MEDIO, DIFICIL };
Dificultad dificultadActual = MEDIO;

// --- Variables de navegacion de los distintos menus ---
int opcionSeleccionada = 0;  // menu principal: 0 = PvP, 1 = PvE
int opcionDificultad = 1;    // 0 = facil, 1 = medio, 2 = dificil
int opcionPuntos = 0;        // 0 = 5, 1 = 10, 2 = 15 puntos
int maxPuntos = 5;           // puntos necesarios para ganar la partida
int ganador = 0;             // 0 = nadie todavia, 1 = J1, 2 = J2

// Variable de navegacion para la pantalla Game Over
int opcionGameOver = 0; // 0: Rejugar, 1: Menu Principal

// palancaCentrada funciona como "debounce" del joystick cuando lo usamos
// para navegar menus: no queremos que un solo movimiento hacia arriba
// se registre 30 veces por segundo, entonces exigimos que el joystick
// vuelva al centro antes de aceptar otro movimiento.
bool palancaCentrada = true;
bool botonSuelto = false; // mismo concepto pero para el boton (evita rebotes)

uint32_t tEntradaEstado = 0; // en que momento (millis) entramos al estado actual
uint32_t tUltimoFrame = 0;   // para controlar la tasa de refresco del loop

const uint32_t DURACION_BIENVENIDA = 5000; // cuanto se queda la pantalla de inicio
const uint16_t T_FRAME = 33; // ~30 FPS. El bus paralelo del TFT es rapido,
                              // asi que este valor no deberia hacer falta
                              // subirlo, pero si se siente lento se puede
                              // probar con 40 o 50.

// --------------------------------------------------------------------------
// Paletas
// (medidas re-escaladas para el TFT de 480x320; antes eran para 128x64)
// --------------------------------------------------------------------------
const int ANCHO_PALETA = 12;
const int ALTO_PALETA = 70;
const int PALETA1_X = 10;                            // paleta izquierda
const int PALETA2_X = ANCHO - 10 - ANCHO_PALETA;     // paleta derecha (458)

// Limites VISIBLES y FISICOS de la cancha.
// La franja superior queda reservada para marcador/HUD y abajo dejamos
// unos pixeles de margen para que el borde inferior se vea claramente.
const int Y_LIMITE_SUP = 48;
const int Y_LIMITE_INF = 310;
const int GROSOR_LIMITE = 2;
const int Y_JUEGO_MIN = Y_LIMITE_SUP + GROSOR_LIMITE;
const int Y_JUEGO_MAX = Y_LIMITE_INF - GROSOR_LIMITE;
const int ALTURA_CANCHA = Y_JUEGO_MAX - Y_JUEGO_MIN;
const int Y_CENTRO_CANCHA = (Y_JUEGO_MIN + Y_JUEGO_MAX) / 2;

// Las dos paletas arrancan centradas respecto a la CANCHA visible.
int paleta1Y = Y_CENTRO_CANCHA - ALTO_PALETA / 2;
int paleta2Y = Y_CENTRO_CANCHA - ALTO_PALETA / 2;

// El alto puede cambiar temporalmente por el power-up de "agrandar",
// por eso separamos "alto base" (ALTO_PALETA) de "alto actual".
int altoPaleta1Actual = ALTO_PALETA;
int altoPaleta2Actual = ALTO_PALETA;

// --------------------------------------------------------------------------
// Power-ups: duracion del bonus de tamaño y del congelamiento
// --------------------------------------------------------------------------
const int BONUS_ALTURA = 35;
const uint32_t DURACION_POWERUP = 5000;

bool powerUpJ1Activo = false;
bool powerUpJ2Activo = false;
uint32_t tInicioPowerUpJ1 = 0;
uint32_t tInicioPowerUpJ2 = 0;
bool congeladoJ1 = false; // si esta en true, ese jugador no puede mover su paleta
bool congeladoJ2 = false;

// --------------------------------------------------------------------------
// Power-up de "bala": en vez de simular la pelota atravesando la paleta en
// tiempo real, apenas se agarra el item el juego entra en el estado
// EXPLOSION, se congela todo, se muestra una animacion de la "bala"
// viajando hacia la paleta rival y explotando, y al terminar se le da el
// punto automaticamente al jugador que disparo.
// --------------------------------------------------------------------------
int jugadorDisparo = 0;             // quien disparo la bala; se lleva el punto al terminar
int explosionX = 0;                 // punto de impacto (centro de la paleta rival)
int explosionY = 0;
float trayInicioX = 0;              // inicio de la trayectoria animada (bola al momento del disparo)
float trayInicioY = 0;
float trayFinX = 0;                 // fin de la trayectoria animada (paleta rival)
float trayFinY = 0;
const uint32_t DURACION_EXPLOSION = 3000;   // duracion total de la animacion (3s)
const uint32_t DURACION_VUELO_BALA = 700;   // de esos 3s, lo que tarda la "bala" en llegar

int ultimoGolpeador = 0; // ultimo jugador que le pego a la pelota (para saber a quien darle el item)

// --------------------------------------------------------------------------
// Item / power-up que aparece en la cancha
// --------------------------------------------------------------------------
bool itemActivo = false;
float itemX = 0;
float itemY = 0;
int tipoItem = 0; // 0 = agrandar paleta, 1 = congelar rival, 2 = bala
uint32_t tUltimoItem = 0;
const int TAM_ITEM = 26;
const uint32_t TIEMPO_SPAWN_ITEM = 8000; // cada cuanto aparece un item nuevo
const int MARGEN_ITEM_X = 120; // no dejamos que aparezca muy cerca de ninguna paleta
const int MARGEN_ITEM_Y = 70;

// --------------------------------------------------------------------------
// Lectura y filtrado del joystick
// --------------------------------------------------------------------------
// El ADC del ESP32 (0-4095) es bastante ruidoso, y ademas el joystick casi
// nunca queda exactamente en el centro (2048) cuando lo soltamos. Para
// que la paleta no tiemble sola resolvimos esto con tres cosas:
//  1) Un filtro pasa-bajos (media movil exponencial / EMA) con ALFA_FILTRO,
//     que suaviza la lectura entre frame y frame.
//  2) Una "zona muerta" (ZONA_MUERTA_ADC) alrededor del centro: cualquier
//     desviacion chica se ignora, asi el joystick puede estar "flojo" sin
//     que la paleta se mueva sola.
//  3) Una curva de respuesta no lineal (variable EXPO en destinoPaleta) que
//     da mas precision cerca del centro y mas velocidad en los extremos.
// Estos tres valores dependen del ADC, no de la pantalla, asi que quedan
// identicos a como estaban con la OLED.
const float ALFA_FILTRO = 0.25;
const float EXPO = 0.60;
const int PASO_MAX = 14; // cuanto se puede mover la paleta como maximo por frame
const int ZONA_MUERTA_ADC = 120;

float filtroJoy1 = 2048.0; // arranca centrado, se actualiza en destinoPaleta()
float filtroJoy2 = 2048.0;

// Centro REAL del joystick para navegar menus.
// Algunos joysticks/ADC del ESP32 no descansan exactamente en 2048, por eso
// lo calibramos al iniciar y comparamos el movimiento respecto a ese centro.
int centroJoy1Menu = 2048;
const int UMBRAL_MENU = 430;
const int ZONA_CENTRO_MENU = 220;

// Prototipos de helpers de centrado (se implementan mas abajo).
int yPaletaCentradaEnCancha(int altoPaleta);
float yPelotaCentradaEnCancha();

// --------------------------------------------------------------------------
// Fisica de la pelota
// (velocidades tambien re-escaladas: la pantalla es ~4 veces mas grande,
// entonces la pelota tiene que moverse ~4 veces mas rapido en pixeles para
// que se sienta con la misma velocidad relativa que en la OLED)
// --------------------------------------------------------------------------
const int TAM_BOLA = 12;

const float INITIAL_VEL_X = 5.4;
const float MAX_VEL_Y = 9.5;
const float FACTOR_ACELERACION = 1.06; // cuanto se acelera la pelota en cada rebote
const float MAX_FACTOR = 3.0;          // tope de aceleracion acumulada
const float LIMITE_VEL_Y = 15.0;

float bolaX = ANCHO / 2.0;
float bolaY = (Y_JUEGO_MIN + Y_JUEGO_MAX - TAM_BOLA) / 2.0;
float velX = INITIAL_VEL_X;
float velY = 5.5;
float factorVel = 1.0; // multiplicador de velocidad que va subiendo con cada golpe

// Guardamos las ultimas 3 posiciones de la pelota solo para dibujar una
// pequeña estela detras de ella (efecto visual, no afecta la fisica).
float trailX[3] = { ANCHO / 2.0, ANCHO / 2.0, ANCHO / 2.0 };
float trailY[3] = {
  (Y_JUEGO_MIN + Y_JUEGO_MAX - TAM_BOLA) / 2.0,
  (Y_JUEGO_MIN + Y_JUEGO_MAX - TAM_BOLA) / 2.0,
  (Y_JUEGO_MIN + Y_JUEGO_MAX - TAM_BOLA) / 2.0
};

// Screen shake: cuando alguien anota, sacudimos la pantalla unos frames
// dibujando todo con un pequeño offset aleatorio.
int shakeFrames = 0;
const int SHAKE_AMPLITUD = 4;
const int SHAKE_DURACION = 6;

int puntosJ1 = 0;
int puntosJ2 = 0;

// ==========================================================================
// AUDIO - musica de fondo estilo arcade clasico (loop, NO bloqueante)
// ==========================================================================
// La idea aca es que la musica suene "sola" sin usar delay() en ningun
// momento, para no interferir con el control de las paletas ni con la
// fisica de la pelota. Para eso guardamos en que nota vamos y en que
// momento (millis) empezo a sonar, y en cada frame revisamos si ya paso
// el tiempo de esa nota para pasar a la siguiente.
//
// La melodia es una composicion propia (un arpegio simple tipo chiptune,
// nada sacado de ningun juego existente). La velocidad de reproduccion se
// ajusta con factorVel: mientras mas rapido va la pelota, mas rapido suena
// la musica, para que se sienta la tension igual que en varios arcades
// clasicos.

#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_G5  784

struct NotaMusical {
  int frecuencia; // 0 = silencio
  int duracion;   // en ms, a velocidad normal (factorVel = 1.0)
};

// Melodia en loop. Las duraciones de aca son la base "a velocidad normal";
// el ajuste por factorVel se hace despues, en avanzarNotaMelodia().
const NotaMusical MELODIA_ARCADE[] = {
  { NOTE_C4, 150 }, { NOTE_E4, 150 }, { NOTE_G4, 150 }, { NOTE_C5, 150 },
  { NOTE_G4, 150 }, { NOTE_E4, 150 }, { NOTE_C4, 150 }, { NOTE_E4, 150 },
  { NOTE_F4, 150 }, { NOTE_A4, 150 }, { NOTE_C5, 150 }, { NOTE_F4, 150 },
  { NOTE_C5, 150 }, { NOTE_A4, 150 }, { NOTE_F4, 150 }, { NOTE_A4, 150 },
  { NOTE_G4, 150 }, { NOTE_B4, 150 }, { NOTE_D5, 150 }, { NOTE_G5, 300 },
  { 0, 200 } // pequeño silencio antes de repetir el loop
};
const int NUM_NOTAS_MELODIA = sizeof(MELODIA_ARCADE) / sizeof(MELODIA_ARCADE[0]);

int notaActualMelodia = -1;      // -1 = todavia no arranco, fuerza la primera nota
uint32_t tInicioNota = 0;
uint32_t duracionNotaActual = 0;

// Pasa a la siguiente nota del loop y la manda a sonar. Es la unica
// funcion que realmente "toca" algo; actualizarMusica() solo decide
// cuando llamarla.
void avanzarNotaMelodia() {
  notaActualMelodia++;
  if (notaActualMelodia >= NUM_NOTAS_MELODIA) {
    notaActualMelodia = 0; // volvemos al principio del loop
  }

  int frecuencia = MELODIA_ARCADE[notaActualMelodia].frecuencia;
  int duracionBase = MELODIA_ARCADE[notaActualMelodia].duracion;

  // Entre mas rapido va la pelota (factorVel), mas corta hacemos la nota,
  // o sea que la melodia suena mas rapido. Ponemos un piso de 40ms para
  // que no se vuelva un pitido continuo imposible de distinguir.
  int duracion = (int)(duracionBase / factorVel);
  if (duracion < 40) {
    duracion = 40;
  }

  duracionNotaActual = duracion;
  tInicioNota = millis();

  if (frecuencia > 0) {
    tone(PIN_BUZZER, frecuencia, duracion);
  } else {
    noTone(PIN_BUZZER); // nota de "silencio" del loop
  }
}

// Se llama una vez por frame mientras estamos jugando. No bloquea nada:
// solo compara tiempos con millis() y, si corresponde, dispara la
// siguiente nota.
void actualizarMusica() {
  if (notaActualMelodia < 0) {
    avanzarNotaMelodia(); // primera vez que se llama, arranca el loop
    return;
  }

  if (millis() - tInicioNota >= duracionNotaActual) {
    avanzarNotaMelodia();
  }
}

// ==========================================================================
// FUNCIONES DE CONTROL DE ESTADO Y REINICIO
// ==========================================================================

// Cambia de estado y guarda en que momento entramos, para que cada
// pantalla pueda medir "cuanto tiempo llevo aca" con millis() - tEntradaEstado.
void cambiarEstado(EstadoJuego nuevo) {
  estadoActual = nuevo;
  tEntradaEstado = millis();
}

// Deja las 3 posiciones de la estela pegadas a la pelota (se usa cuando
// la pelota se teletransporta al centro, para que no se vea un rastro
// larguisimo cruzando toda la pantalla).
void reiniciarEstela() {
  for (int i = 0; i < 3; i++) {
    trailX[i] = bolaX;
    trailY[i] = bolaY;
  }
}

// Apaga cualquier power-up activo y borra el item en pantalla. La
// llamamos cada vez que se anota un punto, para que ningun efecto se
// arrastre de un rally al siguiente.
void limpiarPowerUps() {
  itemActivo = false;
  tUltimoItem = millis();
  altoPaleta1Actual = ALTO_PALETA;
  altoPaleta2Actual = ALTO_PALETA;
  powerUpJ1Activo = false;
  powerUpJ2Activo = false;
  congeladoJ1 = false;
  congeladoJ2 = false;
  ultimoGolpeador = 0;
}

// Se llama despues de cada punto (menos cuando ya se gano la partida):
// vuelve la pelota al centro y la manda para el lado del que perdio el
// punto (por eso el signo de velX se invierte respecto al que tenia).
void resetPelota() {
  bolaX = ANCHO / 2.0;
  bolaY = yPelotaCentradaEnCancha();
  factorVel = 1.0;
  velX = (velX > 0) ? -INITIAL_VEL_X : INITIAL_VEL_X;
  velY = 5.5;
  reiniciarEstela();
  limpiarPowerUps();
}

// Deja todo como al principio de una partida nueva (se llama al elegir
// los puntos a jugar, y tambien desde "Volver a jugar" en Game Over).
void reiniciarJuego() {
  bolaX = ANCHO / 2.0;
  bolaY = yPelotaCentradaEnCancha();
  factorVel = 1.0;
  velX = INITIAL_VEL_X;
  velY = 5.5;
  paleta1Y = yPaletaCentradaEnCancha(ALTO_PALETA);
  paleta2Y = yPaletaCentradaEnCancha(ALTO_PALETA);
  puntosJ1 = 0;
  puntosJ2 = 0;
  ganador = 0;
  shakeFrames = 0;
  jugadorDisparo = 0;
  reiniciarEstela();
  limpiarPowerUps();

  // Reinicia la melodia de fondo desde el principio
  notaActualMelodia = -1;

  // Volvemos a leer el joystick "en frio" para que el filtro arranque
  // desde la posicion real y no desde 2048 a la fuerza.
  filtroJoy1 = analogRead(PIN_JOY1_Y);
  filtroJoy2 = analogRead(PIN_JOY2_Y);
}


// ==========================================================================
// CAPA VISUAL EN COLOR
// ==========================================================================
// El buffer principal sigue siendo de 1 bit para conservar la memoria del
// ESP32. Despues de pushSprite() repintamos solo los elementos principales
// directamente sobre el TFT. Asi obtenemos la estetica de 1.txt sin perder
// las funciones nuevas ni reservar un sprite RGB enorme.
void aplicarEstiloCanchaColor(int offX, int offY, bool dibujarRed = true) {
  // Bordes fijos: no se desplazan con el screen-shake para que siempre quede
  // claro cual es la zona jugable real.
  tft.drawFastHLine(0, Y_LIMITE_SUP, ANCHO, COLOR_LINEA);
  tft.drawFastHLine(0, Y_LIMITE_INF, ANCHO, COLOR_LINEA);

  // Red central gris, como en la version visual de referencia.
  if (dibujarRed) {
    for (int y = Y_JUEGO_MIN + 8; y < Y_JUEGO_MAX - 8; y += 26) {
      tft.drawFastVLine(ANCHO / 2 + offX, y + offY, 13, COLOR_LINEA);
    }
  }

  // Marcadores diferenciados por jugador.
  tft.setTextSize(4);
  tft.setTextColor(COLOR_P1, COLOR_FONDO);
  tft.setCursor(190 + offX, 10 + offY);
  tft.print(puntosJ1);

  tft.setTextColor(COLOR_P2, COLOR_FONDO);
  tft.setCursor(270 + offX, 10 + offY);
  tft.print(puntosJ2);

  // Paletas en los mismos colores del marcador.
  tft.fillRect(PALETA1_X + offX, paleta1Y + offY,
               ANCHO_PALETA, altoPaleta1Actual, COLOR_P1);
  tft.fillRect(PALETA2_X + offX, paleta2Y + offY,
               ANCHO_PALETA, altoPaleta2Actual, COLOR_P2);
}

void aplicarEstiloSelector(int x, int y) {
  tft.setTextSize(3);
  tft.setTextColor(COLOR_P1, COLOR_FONDO);
  tft.setCursor(x, y);
  tft.print(">");
}

// ==========================================================================
// PANTALLA DE BIENVENIDA
// ==========================================================================
// Simple splash screen con el nombre del juego, autores y una barrita de
// "carga" que en realidad es solo un temporizador visual.
void mostrarBienvenida() {
  uint32_t transcurrido = millis() - tEntradaEstado;

  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(6);
  oled.setCursor(168, 30);
  oled.print("PONG");

  oled.setTextSize(3);
  oled.setCursor(123, 110);
  oled.print("Sergio - Luis");

  oled.setTextSize(2);
  oled.setCursor(126, 150);
  oled.print("Microcontroladores");

  int anchoBarra = map(transcurrido, 0, DURACION_BIENVENIDA, 0, 400);
  anchoBarra = constrain(anchoBarra, 0, 400);
  oled.drawRect(40, 260, 400, 30, COLOR_ON);
  oled.fillRect(40, 260, anchoBarra, 30, COLOR_ON);

  oled.pushSprite(0, 0);

  // Barra de carga en cyan para conservar la estetica del 1.txt.
  tft.drawRect(40, 260, 400, 30, COLOR_TEXTO);
  if (anchoBarra > 2) {
    tft.fillRect(42, 262, anchoBarra - 2, 26, COLOR_P1);
  }

  if (transcurrido >= DURACION_BIENVENIDA) {
    cambiarEstado(MENU);
  }
}

// ==========================================================================
// LECTURA DEL JOYSTICK PARA MENUS
// ==========================================================================
// Devuelve -1 = arriba, +1 = abajo, 0 = sin movimiento.
// A diferencia de usar limites fijos 3000/1000, esto funciona aunque el
// joystick quede centrado, por ejemplo, en 1800, 2300 o 2700.
int leerDireccionMenu() {
  int valorY = analogRead(PIN_JOY1_Y);
  int delta = valorY - centroJoy1Menu;

  // Para habilitar un nuevo movimiento exigimos primero volver al centro.
  if (abs(delta) <= ZONA_CENTRO_MENU) {
    palancaCentrada = true;
    return 0;
  }

  if (!palancaCentrada) {
    return 0;
  }

  if (delta >= UMBRAL_MENU) {
    palancaCentrada = false;
    return -1;
  }

  if (delta <= -UMBRAL_MENU) {
    palancaCentrada = false;
    return 1;
  }

  return 0;
}

// Calcula la coordenada Y para que una paleta quede centrada ENTRE los dos
// limites visibles de la cancha, no respecto a toda la pantalla de 320 px.
int yPaletaCentradaEnCancha(int altoPaleta) {
  return Y_CENTRO_CANCHA - altoPaleta / 2;
}

// Centro vertical de la pelota dentro de la cancha visible.
float yPelotaCentradaEnCancha() {
  return Y_JUEGO_MIN + (ALTURA_CANCHA - TAM_BOLA) / 2.0;
}

// ==========================================================================
// MENUS (seleccion de modo, dificultad y puntos a jugar)
// ==========================================================================
// Las tres funciones de menu siguen el mismo patron: usamos el joystick
// como si fuera un "arriba/abajo" (leyendo valores extremos del ADC) y el
// boton como "confirmar". El truco de palancaCentrada evita que un solo
// movimiento del joystick se lea varias veces seguidas.

void atenderMenu() {
  int direccion = leerDireccionMenu();

  if (direccion < 0) {
    opcionSeleccionada = 0;
    sonidoNavegacion();
  } else if (direccion > 0) {
    opcionSeleccionada = 1;
    sonidoNavegacion();
  }

  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(5);
  oled.setCursor(180, 20);
  oled.print("MENU");

  oled.drawFastHLine(0, 90, ANCHO, COLOR_ON);

  oled.setTextSize(3);
  oled.setCursor(90, 130);
  oled.print("PvP (2 Jugadores)");
  oled.setCursor(90, 190);
  oled.print("PvE (vs ESP32)");

  if (opcionSeleccionada == 0) {
    oled.setCursor(40, 130);
  } else {
    oled.setCursor(40, 190);
  }
  oled.print(">");

  oled.pushSprite(0, 0);

  tft.drawFastHLine(0, 90, ANCHO, COLOR_LINEA);
  aplicarEstiloSelector(40, (opcionSeleccionada == 0) ? 130 : 190);

  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();

    if (opcionSeleccionada == 0) {
      modoActual = MODO_PVP;
      opcionPuntos = 0;
      cambiarEstado(PUNTOS);
    } else {
      // Si eligen PvE, primero preguntamos la dificultad de la IA
      modoActual = MODO_PVE;
      opcionDificultad = 1;
      cambiarEstado(DIFICULTAD);
    }
  }
}

void atenderMenuDificultad() {
  int direccion = leerDireccionMenu();

  if (direccion < 0) {
    if (opcionDificultad > 0) {
      opcionDificultad--;
    }
    sonidoNavegacion();
  } else if (direccion > 0) {
    if (opcionDificultad < 2) {
      opcionDificultad++;
    }
    sonidoNavegacion();
  }

  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(3);
  oled.setCursor(150, 15);
  oled.print("DIFICULTAD");
  oled.drawFastHLine(0, 60, ANCHO, COLOR_ON);

  oled.setCursor(110, 100);
  oled.print("Facil");
  oled.setCursor(110, 160);
  oled.print("Medio");
  oled.setCursor(110, 220);
  oled.print("Dificil");

  int posY = 100 + (opcionDificultad * 60);
  oled.setCursor(50, posY);
  oled.print(">");

  oled.pushSprite(0, 0);

  tft.drawFastHLine(0, 60, ANCHO, COLOR_LINEA);
  aplicarEstiloSelector(50, posY);

  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();

    if (opcionDificultad == 0) {
      dificultadActual = FACIL;
    } else if (opcionDificultad == 1) {
      dificultadActual = MEDIO;
    } else {
      dificultadActual = DIFICIL;
    }

    opcionPuntos = 0;
    cambiarEstado(PUNTOS);
  }
}

void atenderMenuPuntos() {
  int direccion = leerDireccionMenu();

  if (direccion < 0) {
    if (opcionPuntos > 0) {
      opcionPuntos--;
    }
    sonidoNavegacion();
  } else if (direccion > 0) {
    if (opcionPuntos < 2) {
      opcionPuntos++;
    }
    sonidoNavegacion();
  }

  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(3);
  oled.setCursor(105, 15);
  oled.print("PUNTOS A GANAR");
  oled.drawFastHLine(0, 60, ANCHO, COLOR_ON);

  oled.setCursor(110, 100);
  oled.print("5 Puntos");
  oled.setCursor(110, 160);
  oled.print("10 Puntos");
  oled.setCursor(110, 220);
  oled.print("15 Puntos");

  int posY = 100 + (opcionPuntos * 60);
  oled.setCursor(50, posY);
  oled.print(">");

  oled.pushSprite(0, 0);

  tft.drawFastHLine(0, 60, ANCHO, COLOR_LINEA);
  aplicarEstiloSelector(50, posY);

  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();

    if (opcionPuntos == 0) {
      maxPuntos = 5;
    } else if (opcionPuntos == 1) {
      maxPuntos = 10;
    } else {
      maxPuntos = 15;
    }

    // Ya tenemos modo, dificultad (si aplica) y puntos: arrancamos la partida
    reiniciarJuego();
    cambiarEstado(JUEGO);
  }
}

// ==========================================================================
// POWER-UPS: aparicion y expiracion
// ==========================================================================
void gestionarPowerUps() {
  uint32_t ahora = millis();

  if (!itemActivo && (ahora - tUltimoItem >= TIEMPO_SPAWN_ITEM)) {
    // Elegimos una posicion en la zona central de la cancha, dejando
    // margen para que no aparezca pegado a ninguna paleta
    itemX = random(MARGEN_ITEM_X, ANCHO - MARGEN_ITEM_X - TAM_ITEM);
    itemY = random(Y_JUEGO_MIN + 20, Y_JUEGO_MAX - 20 - TAM_ITEM);

    // Sorteo del tipo de item: le dimos menos probabilidad a la bala
    // (tipoItem 2) porque es el efecto mas fuerte del juego
    int sorteo = random(0, 10);
    if (sorteo < 4) {
      tipoItem = 0; // 40% agrandar paleta
    } else if (sorteo < 8) {
      tipoItem = 1; // 40% congelar rival
    } else {
      tipoItem = 2; // 20% bala
    }

    itemActivo = true;
    tUltimoItem = ahora;
  }

  if (powerUpJ1Activo && (ahora - tInicioPowerUpJ1 >= DURACION_POWERUP)) {
    powerUpJ1Activo = false;
    altoPaleta1Actual = ALTO_PALETA;
    congeladoJ2 = false;
  }

  if (powerUpJ2Activo && (ahora - tInicioPowerUpJ2 >= DURACION_POWERUP)) {
    powerUpJ2Activo = false;
    altoPaleta2Actual = ALTO_PALETA;
    congeladoJ1 = false;
  }
}

// ==========================================================================
// CONTROL DE LAS PALETAS
// ==========================================================================

// Convierte la lectura cruda del joystick en la posicion Y a la que la
// paleta deberia moverse. Aca esta concentrado casi todo el "ajuste fino"
// del control del juego (filtro EMA + zona muerta + curva de respuesta).
int destinoPaleta(int pin, float &filtro) {
  int lectura = analogRead(pin);

  // Filtro pasa-bajos exponencial: filtro_nuevo = filtro_viejo + alfa*(lectura - filtro_viejo)
  filtro = filtro + ALFA_FILTRO * (lectura - filtro);

  float desviacion = filtro - 2048.0; // 2048 = centro teorico del ADC de 12 bits

  if (desviacion > -ZONA_MUERTA_ADC && desviacion < ZONA_MUERTA_ADC) {
    desviacion = 0.0; // dentro de la zona muerta, se considera "centrado"
  } else if (desviacion > 0) {
    desviacion = desviacion - ZONA_MUERTA_ADC; // "descontamos" la zona muerta
  } else {
    desviacion = desviacion + ZONA_MUERTA_ADC;
  }

  // Normalizamos a un rango de -1 a 1
  float x = desviacion / (2048.0 - ZONA_MUERTA_ADC);

  if (x > 1.0) {
    x = 1.0;
  }
  if (x < -1.0) {
    x = -1.0;
  }

  // Curva de respuesta: EXPO controla cuanto pesa la parte cubica (mas
  // agresiva en los extremos) frente a la parte lineal (mas predecible)
  float curva = EXPO * x * x * x + (1.0 - EXPO) * x;

  float recorrido = (ALTURA_CANCHA - ALTO_PALETA) / 2.0;
  float centro = Y_JUEGO_MIN + recorrido;
  int destino = (int)(centro - curva * recorrido);

  return constrain(destino, Y_JUEGO_MIN, Y_JUEGO_MAX - ALTO_PALETA);
}

// En vez de saltar directo a la posicion destino, la paleta se mueve a
// lo sumo PASO_MAX pixeles por frame, para que el movimiento se vea suave.
void moverHacia(int &posicion, int destino) {
  int diferencia = destino - posicion;

  if (diferencia > PASO_MAX) {
    posicion += PASO_MAX;
  } else if (diferencia < -PASO_MAX) {
    posicion -= PASO_MAX;
  } else {
    posicion = destino; // ya esta lo bastante cerca, ajustamos directo
  }
}

// Mueve las dos paletas. La paleta 1 siempre se controla con el joystick 1.
// La paleta 2 depende del modo: en PvP usa el segundo joystick, en PvE la
// maneja una IA sencilla que persigue la pelota con cierto margen de error
// y una velocidad, ambos definidos por la dificultad elegida en el menu.
void moverPaletas() {
  if (!congeladoJ1) {
    int destino1 = destinoPaleta(PIN_JOY1_Y, filtroJoy1);
    destino1 = constrain(destino1, Y_JUEGO_MIN, Y_JUEGO_MAX - altoPaleta1Actual);
    moverHacia(paleta1Y, destino1);
  }
  paleta1Y = constrain(paleta1Y, Y_JUEGO_MIN, Y_JUEGO_MAX - altoPaleta1Actual);

  if (modoActual == MODO_PVP) {
    if (!congeladoJ2) {
      int destino2 = destinoPaleta(PIN_JOY2_Y, filtroJoy2);
      destino2 = constrain(destino2, Y_JUEGO_MIN, Y_JUEGO_MAX - altoPaleta2Actual);
      moverHacia(paleta2Y, destino2);
    }
  } else {
    // --- IA del jugador 2 (modo PvE) ---
    if (!congeladoJ2) {
      int velIA = 10;
      int margenError = 15;

      if (dificultadActual == FACIL) {
        velIA = 6;
        margenError = 30; // reacciona lento y con mas margen de error
      } else if (dificultadActual == MEDIO) {
        velIA = 10;
        margenError = 15;
      } else if (dificultadActual == DIFICIL) {
        velIA = 14;
        margenError = 5; // casi no falla
      }

      // Solo persigue la pelota cuando viene hacia ella (velX > 0), para
      // que no se vea "adivinando" el rebote antes de que pase por el centro
      if (velX > 0) {
        int centroPaleta2 = paleta2Y + altoPaleta2Actual / 2;
        int centroBola = (int)bolaY + TAM_BOLA / 2;

        if (centroPaleta2 < centroBola - margenError) {
          paleta2Y += velIA;
        } else if (centroPaleta2 > centroBola + margenError) {
          paleta2Y -= velIA;
        }
      }
    }
  }

  paleta2Y = constrain(paleta2Y, Y_JUEGO_MIN, Y_JUEGO_MAX - altoPaleta2Actual);
}

// ==========================================================================
// FISICA DE LA PELOTA
// ==========================================================================

// Calcula, segun en que parte de la paleta pega la pelota, que tan fuerte
// deberia salir disparada verticalmente (pega en el centro = casi recta,
// pega en las puntas = sale con mucho angulo).
float calcularImpacto(int paletaY, int altoActual) {
  float centroPaleta = paletaY + (altoActual / 2.0);
  float centroBola = bolaY + (TAM_BOLA / 2.0);
  float impactoRelativo = (centroBola - centroPaleta) / (altoActual / 2.0);

  if (impactoRelativo > 1.0) {
    impactoRelativo = 1.0;
  }
  if (impactoRelativo < -1.0) {
    impactoRelativo = -1.0;
  }

  return impactoRelativo;
}

// Cada vez que la pelota rebota en una paleta, la aceleramos un poco (con
// un tope) para que los rallies largos se sientan cada vez mas intensos.
void acelerarPelota() {
  factorVel = factorVel * FACTOR_ACELERACION;

  if (factorVel > MAX_FACTOR) {
    factorVel = MAX_FACTOR;
  }
}

void aplicarVelocidadY(int paletaY, int altoActual) {
  velY = calcularImpacto(paletaY, altoActual) * MAX_VEL_Y * factorVel;

  if (velY > LIMITE_VEL_Y) {
    velY = LIMITE_VEL_Y;
  }
  if (velY < -LIMITE_VEL_Y) {
    velY = -LIMITE_VEL_Y;
  }
}

// ==========================================================================
// POWER-UP DE "BALA" (explosion)
// ==========================================================================
void iniciarExplosion(int jugador) {
  jugadorDisparo = jugador;
  sonidoExplosionBuzzer(); // <--- SONIDO EXPLOSION

  // Punto de partida de la animacion: donde estaba la bola al recoger el item
  trayInicioX = bolaX;
  trayInicioY = bolaY;

  // Objetivo: el centro de la paleta rival (la que se "destruye")
  if (jugador == 1) {
    explosionX = PALETA2_X + ANCHO_PALETA / 2;
    explosionY = paleta2Y + altoPaleta2Actual / 2;
  } else {
    explosionX = PALETA1_X + ANCHO_PALETA / 2;
    explosionY = paleta1Y + altoPaleta1Actual / 2;
  }

  trayFinX = explosionX;
  trayFinY = explosionY;

  cambiarEstado(EXPLOSION);
}

// Se ejecuta cuando la pelota toca el item activo. Segun el tipo de item
// hace una cosa distinta; el caso de la bala (tipoItem == 2) corta la
// ejecucion antes porque cambia todo el flujo del juego (pasa a EXPLOSION).
void aplicarItem() {
  if (ultimoGolpeador == 0) {
    // Nadie le pego a la pelota todavia en este rally, asi que no sabemos
    // a quien darle el power-up: lo ignoramos.
    return;
  }

  itemActivo = false;
  tUltimoItem = millis();

  if (tipoItem == 2) {
    iniciarExplosion(ultimoGolpeador);
    return;
  }

  if (ultimoGolpeador == 1) {
    if (tipoItem == 0) {
      altoPaleta1Actual = ALTO_PALETA + BONUS_ALTURA; // agranda su propia paleta
    } else {
      congeladoJ2 = true; // congela al rival
    }
    powerUpJ1Activo = true;
    tInicioPowerUpJ1 = millis();
  } else {
    if (tipoItem == 0) {
      altoPaleta2Actual = ALTO_PALETA + BONUS_ALTURA;
    } else {
      congeladoJ1 = true;
    }
    powerUpJ2Activo = true;
    tInicioPowerUpJ2 = millis();
  }
}

// Actualiza la posicion de la pelota, resuelve colisiones (paredes,
// paletas, item) y detecta cuando alguien anota.
void moverPelota() {
  // Desplazamos la estela un lugar y guardamos la posicion actual como
  // el punto mas nuevo (se usa solo para el efecto visual del rastro)
  for (int i = 2; i > 0; i--) {
    trailX[i] = trailX[i - 1];
    trailY[i] = trailY[i - 1];
  }
  trailX[0] = bolaX;
  trailY[0] = bolaY;

  bolaX += velX;
  bolaY += velY;

  // Rebote contra el limite superior VISIBLE de la cancha
  if (bolaY <= Y_JUEGO_MIN) {
    bolaY = Y_JUEGO_MIN;
    velY = -velY;
    sonidoRebotePared(); // <--- SONIDO PARED
  }

  // Rebote contra el limite inferior VISIBLE de la cancha
  if (bolaY >= Y_JUEGO_MAX - TAM_BOLA) {
    bolaY = Y_JUEGO_MAX - TAM_BOLA;
    velY = -velY;
    sonidoRebotePared(); // <--- SONIDO PARED
  }

  // Rebote Paleta 1 (solo lo chequeamos si la pelota va hacia la izquierda)
  if (velX < 0 &&
      bolaX <= PALETA1_X + ANCHO_PALETA &&
      bolaY + TAM_BOLA >= paleta1Y &&
      bolaY <= paleta1Y + altoPaleta1Actual) {

    acelerarPelota();
    bolaX = PALETA1_X + ANCHO_PALETA; // la "clavamos" justo al borde de la paleta
    velX = INITIAL_VEL_X * factorVel;
    aplicarVelocidadY(paleta1Y, altoPaleta1Actual);
    ultimoGolpeador = 1;
    sonidoRebotePaleta(); // <--- SONIDO PALETA
  }

  // Rebote Paleta 2
  if (velX > 0 &&
      bolaX + TAM_BOLA >= PALETA2_X &&
      bolaY + TAM_BOLA >= paleta2Y &&
      bolaY <= paleta2Y + altoPaleta2Actual) {

    acelerarPelota();
    bolaX = PALETA2_X - TAM_BOLA;
    velX = -INITIAL_VEL_X * factorVel;
    aplicarVelocidadY(paleta2Y, altoPaleta2Actual);
    ultimoGolpeador = 2;
    sonidoRebotePaleta(); // <--- SONIDO PALETA
  }

  // Agarrar Item (colision por cajas simple, AABB)
  if (itemActivo &&
      bolaX + TAM_BOLA >= itemX && bolaX <= itemX + TAM_ITEM &&
      bolaY + TAM_BOLA >= itemY && bolaY <= itemY + TAM_ITEM) {
    sonidoPowerUp(); // <--- SONIDO POWER-UP
    aplicarItem();
  }

  // Anotacion de Puntos: si la pelota se paso de un borde, listo, hay punto
  if (bolaX < 0) {
    puntosJ2++;
    shakeFrames = SHAKE_DURACION;
    sonidoPunto(); // <--- SONIDO PUNTO

    if (puntosJ2 >= maxPuntos) {
      ganador = 2;
      botonSuelto = false; // para que en Game Over el boton no quede "pegado"
      cambiarEstado(GAME_OVER);
    } else {
      resetPelota();
    }
  } else if (bolaX > ANCHO) {
    puntosJ1++;
    shakeFrames = SHAKE_DURACION;
    sonidoPunto(); // <--- SONIDO PUNTO

    if (puntosJ1 >= maxPuntos) {
      ganador = 1;
      botonSuelto = false;
      cambiarEstado(GAME_OVER);
    } else {
      resetPelota();
    }
  }
}

// ==========================================================================
// DIBUJO EN PANTALLA (todo lo que corresponde al estado JUEGO)
// ==========================================================================

// Dibuja el item flotante en la cancha con un simbolo distinto segun su
// tipo, asi el jugador puede reconocer de un vistazo que le conviene ir
// a buscar.
void dibujarItem(int offX, int offY) {
  if (!itemActivo) {
    return;
  }

  int ix = (int)itemX + offX;
  int iy = (int)itemY + offY;

  oled.drawRect(ix, iy, TAM_ITEM, TAM_ITEM, COLOR_ON);

  if (tipoItem == 0) {
    // simbolo tipo "cruz" para el power-up de agrandar
    oled.drawFastHLine(ix + 7, iy + 13, 13, COLOR_ON);
    oled.drawFastVLine(ix + 13, iy + 7, 13, COLOR_ON);
  } else if (tipoItem == 1) {
    // cuadrado solido para el power-up de congelar
    oled.fillRect(ix + 7, iy + 7, 11, 11, COLOR_ON);
  } else {
    // simbolo simple para la bala (dos rayas + una puntita)
    oled.drawFastHLine(ix + 4, iy + 8, 15, COLOR_ON);
    oled.drawFastHLine(ix + 4, iy + 16, 15, COLOR_ON);
    oled.fillCircle(ix + 21, iy + 12, 3, COLOR_ON);
  }
}

// Texto que aparece arriba a la derecha avisando si algun power-up esta
// activo en este momento (quien esta agrandado o quien esta congelado).
void dibujarHUD(int offX, int offY) {
  char texto[10];
  texto[0] = '\0';

  if (congeladoJ1) {
    strcpy(texto, "J1 STOP");
  } else if (congeladoJ2) {
    strcpy(texto, "J2 STOP");
  } else if (altoPaleta1Actual > ALTO_PALETA) {
    strcpy(texto, "J1 BIG");
  } else if (altoPaleta2Actual > ALTO_PALETA) {
    strcpy(texto, "J2 BIG");
  }

  if (texto[0] == '\0') {
    return; // nada que mostrar
  }

  oled.setTextSize(2);
  int x = ANCHO - 12 * (int)strlen(texto) - 10; // alinea el texto a la derecha
  oled.setCursor(x + offX, 10 + offY);
  oled.print(texto);
}

// Dibuja un frame completo de la pantalla de juego: marcador, HUD, red
// central, item, estela, paletas y la pelota. offX/offY son el pequeño
// desplazamiento aleatorio que usamos para el efecto de "shake" cuando
// se anota un punto.
void dibujarJuego() {
  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  int offX = 0;
  int offY = 0;

  if (shakeFrames > 0) {
    offX = random(-SHAKE_AMPLITUD, SHAKE_AMPLITUD + 1);
    offY = random(-SHAKE_AMPLITUD, SHAKE_AMPLITUD + 1);
    shakeFrames--;
  }

  // Marcadores de Puntuacion
  oled.setTextSize(4);
  oled.setCursor(190 + offX, 10 + offY);
  oled.print(puntosJ1);
  oled.setCursor(270 + offX, 10 + offY);
  oled.print(puntosJ2);

  // Multiplicador de Velocidad (Izquierda)
  oled.setTextSize(2);
  oled.setCursor(10 + offX, 10 + offY);
  oled.print("x");
  oled.print(factorVel, 1);

  // Indicador de Dificultad Seleccionada / Nivel (Centro, cerca del borde inferior)
  oled.setTextSize(3);
  oled.setCursor(213 + offX, 278 + offY);
  if (modoActual == MODO_PVE) {
    if (dificultadActual == FACIL) oled.print("FAC");
    else if (dificultadActual == MEDIO) oled.print("MED");
    else oled.print("DIF");
  } else {
    oled.print("PVP");
  }

  dibujarHUD(offX, offY);

  // Limites superior e inferior de la cancha (tambien existen en la fisica)
  oled.drawFastHLine(0, Y_LIMITE_SUP, ANCHO, COLOR_ON);
  oled.drawFastHLine(0, Y_LIMITE_INF, ANCHO, COLOR_ON);

  // Red Central (una linea punteada simulada con segmentos verticales)
  for (int y = Y_JUEGO_MIN + 8; y < Y_JUEGO_MAX - 8; y += 26) {
    oled.drawFastVLine(ANCHO / 2 + offX, y + offY, 13, COLOR_ON);
  }

  dibujarItem(offX, offY);

  // Rastro y Elementos
  oled.drawPixel((int)trailX[2] + offX, (int)trailY[2] + offY, COLOR_ON);
  oled.fillRect((int)trailX[1] + offX, (int)trailY[1] + offY, 6, 6, COLOR_ON);

  oled.fillRect(PALETA1_X + offX, paleta1Y + offY, ANCHO_PALETA, altoPaleta1Actual, COLOR_ON);
  oled.fillRect(PALETA2_X + offX, paleta2Y + offY, ANCHO_PALETA, altoPaleta2Actual, COLOR_ON);
  oled.fillRect((int)bolaX + offX, (int)bolaY + offY, TAM_BOLA, TAM_BOLA, COLOR_ON);

  oled.pushSprite(0, 0);
  aplicarEstiloCanchaColor(offX, offY);
}

// ==========================================================================
// LOOP PRINCIPAL DEL JUEGO (estado JUEGO) Y PAUSA
// ==========================================================================

// Esta es la funcion que se llama en cada frame mientras estamos jugando.
// Primero revisamos si apretaron el boton para pausar (asi evitamos que
// ese mismo frame tambien mueva la pelota); si no, seguimos con el flujo
// normal: musica, power-ups, paletas, fisica y dibujo.
void actualizarJuego() {
  // Verificacion de Pausa mediante el boton del Joystick 1
  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();
    cambiarEstado(PAUSA);
    return;
  }

  actualizarMusica(); // <--- MUSICA DE FONDO (no bloqueante)

  gestionarPowerUps();
  moverPaletas();
  moverPelota();

  // moverPelota() puede haber cambiado el estado (por ejemplo a GAME_OVER
  // o a EXPLOSION), asi que si ya no estamos en JUEGO no tiene sentido
  // dibujar el frame de juego normal
  if (estadoActual != JUEGO) {
    return;
  }

  dibujarJuego();
}

// Estado de pausa: seguimos dibujando el juego "congelado" (no llamamos
// a moverPaletas ni moverPelota, asi que queda estatico) y le superponemos
// un cartelito de PAUSA. Con el mismo boton se reanuda.
void atenderPausa() {
  // Mantener renderizado el estado actual del juego
  dibujarJuego();

  // Superponer cuadro de Pausa centrado
  oled.fillRect(127, 100, 225, 120, COLOR_OFF);
  oled.drawRect(127, 100, 225, 120, COLOR_ON);

  oled.setTextSize(4);
  oled.setTextColor(COLOR_ON);
  oled.setCursor(179, 144);
  oled.print("PAUSA");
  oled.pushSprite(0, 0);

  aplicarEstiloCanchaColor(0, 0);
  // Volvemos a poner el panel por encima de la red coloreada.
  tft.fillRect(127, 100, 225, 120, COLOR_FONDO);
  tft.drawRect(127, 100, 225, 120, COLOR_TEXTO);
  tft.setTextSize(4);
  tft.setTextColor(COLOR_TEXTO, COLOR_FONDO);
  tft.setCursor(179, 144);
  tft.print("PAUSA");

  // Detectar pulsacion del boton para despausar
  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();
    cambiarEstado(JUEGO);
  }
}

// ==========================================================================
// ANIMACION DEL POWER-UP DE BALA (estado EXPLOSION)
// ==========================================================================

// Dibuja la animacion en dos fases, usando el tiempo transcurrido desde
// que entramos al estado (tEntradaEstado) para interpolar posiciones:
//   Fase 1 (0 a DURACION_VUELO_BALA): un pequeño cuadrado (la "bala")
//   viaja en linea recta desde donde estaba la pelota hasta la paleta
//   rival, dejando una trayectoria punteada detras.
//   Fase 2 (resto del tiempo): en el punto de impacto dibujamos dos
//   ondas expansivas mas unos fragmentos dispersandose en circulo,
//   simulando la paleta "explotando".
// Mientras tanto, el resto de la pantalla se dibuja fijo, porque el
// juego esta pausado en este estado.
void dibujarExplosion() {
  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(4);
  oled.setCursor(190, 10);
  oled.print(puntosJ1);
  oled.setCursor(270, 10);
  oled.print(puntosJ2);

  oled.drawFastHLine(0, Y_LIMITE_SUP, ANCHO, COLOR_ON);
  oled.drawFastHLine(0, Y_LIMITE_INF, ANCHO, COLOR_ON);

  for (int y = Y_JUEGO_MIN + 8; y < Y_JUEGO_MAX - 8; y += 26) {
    oled.drawFastVLine(ANCHO / 2, y, 13, COLOR_ON);
  }

  oled.fillRect(PALETA1_X, paleta1Y, ANCHO_PALETA, altoPaleta1Actual, COLOR_ON);
  oled.fillRect(PALETA2_X, paleta2Y, ANCHO_PALETA, altoPaleta2Actual, COLOR_ON);

  uint32_t transcurrido = millis() - tEntradaEstado;

  float fraccionVuelo = (float)transcurrido / (float)DURACION_VUELO_BALA;
  if (fraccionVuelo > 1.0) {
    fraccionVuelo = 1.0;
  }

  // Trayectoria punteada recorrida hasta el momento (interpolacion lineal
  // entre el punto de inicio y el punto de impacto)
  const int PASOS_TRAYECTORIA = 24;
  for (int i = 0; i <= PASOS_TRAYECTORIA; i++) {
    float f = (float)i / PASOS_TRAYECTORIA;
    if (f > fraccionVuelo) {
      break; // no dibujamos mas alla de donde va la "bala" todavia
    }
    if (i % 2 == 0) {
      // salteamos puntos de por medio para que se vea punteada, no solida
      int px = (int)(trayInicioX + (trayFinX - trayInicioX) * f);
      int py = (int)(trayInicioY + (trayFinY - trayInicioY) * f);
      oled.fillRect(px - 2, py - 2, 4, 4, COLOR_ON);
    }
  }

  if (fraccionVuelo < 1.0) {
    // Fase 1: la "bala" sigue viajando hacia la paleta rival
    float actualX = trayInicioX + (trayFinX - trayInicioX) * fraccionVuelo;
    float actualY = trayInicioY + (trayFinY - trayInicioY) * fraccionVuelo;
    oled.fillRect((int)actualX, (int)actualY, TAM_BOLA, TAM_BOLA, COLOR_ON);
  } else {
    // Fase 2: ya impacto, animacion de explosion sobre la paleta rival.
    // Recalculamos un "progreso" propio de esta fase (0 a 1) para no
    // mezclarlo con el progreso de la fase 1.
    uint32_t tExplosion = transcurrido - DURACION_VUELO_BALA;
    uint32_t duracionRestante = DURACION_EXPLOSION - DURACION_VUELO_BALA;

    float progreso = (float)tExplosion / (float)duracionRestante;
    if (progreso > 1.0) {
      progreso = 1.0;
    }

    const int RADIO_MAX = 60;
    const float DURACION_ONDA = 0.7;

    // Dos ondas de choque desfasadas (una arranca despues que la otra)
    // para que se vea como una explosion "con capas"
    float fase1 = progreso;
    if (fase1 <= DURACION_ONDA) {
      int r1 = (int)(RADIO_MAX * (fase1 / DURACION_ONDA));
      if (r1 < 1) {
        r1 = 1;
      }
      oled.drawCircle(explosionX, explosionY, r1, COLOR_ON);
    }

    float fase2 = progreso - 0.2;
    if (fase2 >= 0.0 && fase2 <= DURACION_ONDA) {
      int r2 = (int)(RADIO_MAX * (fase2 / DURACION_ONDA));
      if (r2 < 1) {
        r2 = 1;
      }
      oled.drawCircle(explosionX, explosionY, r2, COLOR_ON);
    }

    // Fragmentos de la paleta dispersandose en circulo, alejandose del
    // centro de la explosion a medida que avanza el progreso
    const int NUM_FRAGMENTOS = 8;
    for (int i = 0; i < NUM_FRAGMENTOS; i++) {
      float angulo = (TWO_PI / NUM_FRAGMENTOS) * i;
      int dist = (int)(16 + progreso * 90);
      int px = explosionX + (int)(cos(angulo) * dist);
      int py = explosionY + (int)(sin(angulo) * dist);
      oled.fillRect(px - 2, py - 2, 4, 4, COLOR_ON);
    }

    // Texto "BOOM" solo durante la primera mitad de la explosion
    if (progreso < 0.5) {
      oled.setTextSize(3);
      int tx = constrain(explosionX - 36, 0, ANCHO - 72);
      int ty = constrain(explosionY - 30, 0, ALTO - 24);
      oled.setCursor(tx, ty);
      oled.print("BOOM");
    }
  }

  oled.pushSprite(0, 0);
  // En explosion no repintamos la red encima, para no tapar la trayectoria.
  aplicarEstiloCanchaColor(0, 0, false);
}

// Mientras estemos en el estado EXPLOSION, el juego esta efectivamente en
// pausa: solo mostramos la animacion. Cuando se cumplen los 3 segundos
// completos, le damos el punto al jugador que disparo la bala, revisamos
// si con eso ya gano la partida, y volvemos al juego normal (o a Game Over).
void atenderExplosion() {
  dibujarExplosion();

  uint32_t transcurrido = millis() - tEntradaEstado;

  if (transcurrido >= DURACION_EXPLOSION) {
    if (jugadorDisparo == 1) {
      puntosJ1++;
    } else {
      puntosJ2++;
    }

    shakeFrames = SHAKE_DURACION;

    bool seAcaboElJuego =
        (jugadorDisparo == 1 && puntosJ1 >= maxPuntos) ||
        (jugadorDisparo == 2 && puntosJ2 >= maxPuntos);

    if (seAcaboElJuego) {
      ganador = jugadorDisparo;
      botonSuelto = false;
      cambiarEstado(GAME_OVER);
    } else {
      resetPelota();
      cambiarEstado(JUEGO);
    }
  }
}

// ==========================================================================
// PANTALLA DE GAME OVER
// ==========================================================================
// Muestra quien gano (o si perdiste, en el caso de PvE) y deja elegir con
// el joystick entre jugar de nuevo con la misma configuracion o volver al
// menu principal.
void atenderGameOver() {
  int direccion = leerDireccionMenu();

  if (direccion < 0) {
    opcionGameOver = 0; // Rejugar
    sonidoNavegacion();
  } else if (direccion > 0) {
    opcionGameOver = 1; // Menu Principal
    sonidoNavegacion();
  }

  oled.fillSprite(COLOR_OFF);
  oled.setTextColor(COLOR_ON);

  oled.setTextSize(5);
  oled.setCursor(105, 20);
  oled.print("GAME OVER");

  oled.drawFastHLine(0, 90, ANCHO, COLOR_ON);

  oled.setTextSize(3);
  if (ganador == 1) {
    oled.setCursor(105, 115);
    oled.print("PLAYER 1 GANA!");
  } else {
    if (modoActual == MODO_PVE) {
      oled.setCursor(168, 115);
      oled.print("PERDISTE");
    } else {
      oled.setCursor(105, 115);
      oled.print("PLAYER 2 GANA!");
    }
  }

  // Opciones de navegacion
  oled.setCursor(114, 180);
  oled.print("Volver a Jugar");
  oled.setCursor(114, 230);
  oled.print("Menu Principal");

  int posY = (opcionGameOver == 0) ? 180 : 230;
  oled.setCursor(50, posY);
  oled.print(">");

  oled.pushSprite(0, 0);

  tft.drawFastHLine(0, 90, ANCHO, COLOR_LINEA);
  aplicarEstiloSelector(50, posY);

  if (ganador == 1) {
    tft.setTextSize(3);
    tft.setTextColor(COLOR_P1, COLOR_FONDO);
    tft.setCursor(105, 115);
    tft.print("PLAYER 1 GANA!");
  } else if (modoActual == MODO_PVP) {
    tft.setTextSize(3);
    tft.setTextColor(COLOR_P2, COLOR_FONDO);
    tft.setCursor(105, 115);
    tft.print("PLAYER 2 GANA!");
  }

  if (digitalRead(PIN_BTN1) == HIGH) {
    botonSuelto = true;
  }

  if (digitalRead(PIN_BTN1) == LOW && botonSuelto) {
    botonSuelto = false;
    sonidoBoton();

    if (opcionGameOver == 0) {
      reiniciarJuego();
      cambiarEstado(JUEGO);
    } else {
      opcionSeleccionada = 0;
      cambiarEstado(MENU);
    }
  }
}

// ==========================================================================
// SETUP Y LOOP
// ==========================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_BTN1, INPUT_PULLUP); // usamos pull-up interno: boton a GND
  pinMode(PIN_BUZZER, OUTPUT);

  // Semilla para random(): mezclamos una lectura analogica (que siempre
  // tiene algo de ruido) con micros(), asi cada partida tira numeros
  // distintos y no siempre el mismo patron de items/comportamiento
  randomSeed(analogRead(PIN_JOY1_Y) ^ micros());

  // Calibramos el centro del joystick 1 con varias muestras.
  // IMPORTANTE: durante el encendido deja el joystick suelto/centrado.
  long sumaCentro = 0;
  for (int i = 0; i < 16; i++) {
    sumaCentro += analogRead(PIN_JOY1_Y);
    delay(2);
  }
  centroJoy1Menu = (int)(sumaCentro / 16);

  filtroJoy1 = centroJoy1Menu;
  filtroJoy2 = analogRead(PIN_JOY2_Y);
  palancaCentrada = true;

  // --- Inicializacion de la pantalla TFT ---
  tft.init();
  tft.setRotation(1); // si la imagen sale rotada o al reves, probar 0, 2 o 3
  tft.fillScreen(TFT_BLACK);

  // --- Creamos el sprite monocromo que usamos como buffer de dibujo ---
  oled.setColorDepth(1); // 1 bit por pixel: blanco/negro, igual que la OLED
  if (!oled.createSprite(ANCHO, ALTO)) {
    // Si no hay memoria para el sprite, avisamos por Serial y nos quedamos
    // trabados a proposito (mismo criterio que antes con la OLED)
    Serial.println("Error: no alcanzo la memoria para el sprite de pantalla");
    while (true) {
      delay(500);
    }
  }
  oled.setBitmapColor(TFT_WHITE, TFT_BLACK); // bit 1 = blanco, bit 0 = negro

  cambiarEstado(BIENVENIDA);
  tUltimoFrame = millis();
}

void loop() {
  uint32_t ahora = millis();

  // Limitamos el loop a ~30 FPS (T_FRAME = 33ms) para que la animacion se
  // vea pareja sin importar que tan rapido corra el resto del codigo.
  if (ahora - tUltimoFrame < T_FRAME) {
    return;
  }
  tUltimoFrame = ahora;

  // El switch es el "corazon" de la maquina de estados: segun en que
  // pantalla estemos, delegamos todo el trabajo de ese frame a la
  // funcion correspondiente.
  switch (estadoActual) {
    case BIENVENIDA:
      mostrarBienvenida();
      break;

    case MENU:
      atenderMenu();
      break;

    case DIFICULTAD:
      atenderMenuDificultad();
      break;

    case PUNTOS:
      atenderMenuPuntos();
      break;

    case JUEGO:
      actualizarJuego();
      break;

    case PAUSA:
      atenderPausa();
      break;

    case EXPLOSION:
      atenderExplosion();
      break;

    case GAME_OVER:
      atenderGameOver();
      break;
  }
}