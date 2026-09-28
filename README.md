# Juego de Pong en ESP32 con Joysticks, Pantalla y Buzzer

Juego clásico de Pong desarrollado para la placa de desarrollo **ESP32** utilizando una arquitectura basada en **Máquina de Estados Finitos (FSM)**. Cuenta con control analógico mediante Joysticks (con filtrado de señal digital), efectos de sonido interactivos vía Buzzer y renderizado gráfico en pantalla.

---

## 🧩 Características

- **Máquina de Estados Finitos (FSM):** Control estructurado de las transiciones del juego (`BIENVENIDA`, `MENU`, `JUEGO`, `GAME OVER`).
- **Control Analógico Suavizado:** Lectura de Joysticks vía ADC optimizada con un filtro digital de media móvil exponencial (EMA) para eliminar el ruido mecánico.
- **Gráficos en Tiempo Real:** Renderizado de paletas, bola, marcador y bordes en pantalla mediante librerías gráficas.
- **Efectos de Sonido:** Feedback auditivo mediante tonos en el Buzzer al rebotar la bola, anotar puntos o perder.
- **Eficiencia e Inspección de Memoria:** Código optimizado con un uso ligero de recursos en hardware (7.5% RAM, 23.9% Flash).
- **Diagrama de Flujo del Sistema:** Documentación técnica de la lógica FSM generada automáticamente en Python con Graphviz.

---

## 📦 Hardware Requerido

- **ESP32** (Placa de desarrollo, ej. NodeMCU-32S / ESP32 Dev Module)
- **Pantalla Gráfica** (OLED SSD1306 o TFT según la configuración de pines)
- **2x Joysticks Analógicos** (Ejes analógicos para control de Jugador 1 y Jugador 2)
- **1x Buzzer Pasivo/Activo**
- **Protoboard y Cables de Conexión Dupont**
- **Cable de comunicación/alimentación Micro-USB o USB-C**

---

## 🔌 Conexiones de Hardware

> *Para ver la distribución gráfica detallada del circuito, consulta el esquema en la carpeta de documentación del proyecto.*

| Componente | Pin / Señal ESP32 | Descripción |
| :--- | :--- | :--- |
| **Pantalla (SDA)** | GPIO 21 | Comunicación I2C / SPI Data |
| **Pantalla (SCL)** | GPIO 22 | Comunicación I2C / SPI Clock |
| **Joystick 1 (VRx/VRy)** | GPIO 34 (ADC) | Entrada analógica Jugador 1 |
| **Joystick 2 (VRx/VRy)** | GPIO 35 (ADC) | Entrada analógica Jugador 2 |
| **Buzzer** | GPIO 25 | Salida de audio/tonos |
| **VCC / GND** | 3.3V / GND | Alimentación del circuito |

---

## 🖥️ Software y Herramientas

- **IDE:** [Visual Studio Code](https://code.visualstudio.com/) con la extensión **PlatformIO**.
- **Entorno de Compilación:** `platformio.exe` (`toolchain-xtensa-esp32`).
- **Bibliotecas Principales:**
  - `Adafruit GFX Library`
  - `Adafruit SSD1306` / `TFT_eSPI`
- **Herramientas Complementarias:** Python + `graphviz` para la generación de diagramas del proyecto.

---

## 📊 Métricas de Rendimiento (PlatformIO)

Resultados de la compilación e inspección de memoria en el ESP32:

- **RAM Usada:** `24,636 bytes` de `327,680 bytes` (**7.5%**)
- **Flash Usada:** `313,205 bytes` de `1,310,720 bytes` (**23.9%**)
- **Tiempo de Compilación:** 4.20 segundos

---

## 🚀 Instalación y Configuración

1. **Clonar el repositorio:**
   ```bash
   git clone [https://github.com/luisdavidebIEEE/retodise-opong.git](https://github.com/luisdavidebIEEE/retodise-opong.git)
   cd retodise-opong
