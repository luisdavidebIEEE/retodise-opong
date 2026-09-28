import os
import sys
import subprocess
import graphviz

def buscar_dot():
    # 1. Comprobar rutas comunes de instalación en Windows
    rutas_comunes = [
        r"C:\Program Files\Graphviz\bin",
        r"C:\Program Files (x86)\Graphviz\bin",
        os.path.expandvars(r"%LOCALAPPDATA%\Programs\Graphviz\bin"),
        os.path.expandvars(r"%USERPROFILE%\AppData\Local\Programs\Graphviz\bin")
    ]
    
    for ruta in rutas_comunes:
        if os.path.exists(os.path.join(ruta, "dot.exe")):
            return ruta

    # 2. Si no está en las rutas comunes, buscar dinámicamente en C:\
    print("Buscando ejecutable 'dot.exe' en el sistema...")
    try:
        resultado = subprocess.run(
            ['powershell', '-Command', 'Get-ChildItem -Path "C:\\Program Files*", "$env:LOCALAPPDATA" -Filter "dot.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty DirectoryName'],
            capture_output=True, text=True
        )
        ruta_hallada = resultado.stdout.strip()
        if ruta_hallada and os.path.exists(os.path.join(ruta_hallada, "dot.exe")):
            return ruta_hallada
    except Exception:
        pass
        
    return None

# Configurar PATH dinámicamente
ruta_graphviz = buscar_dot()
if ruta_graphviz:
    os.environ["PATH"] += os.pathsep + ruta_graphviz
    print(f"Graphviz detectado en: {ruta_graphviz}")
else:
    print("No se encontró 'dot.exe'. Revisa la instalación.")

def generar_diagrama_pong():
    dot = graphviz.Digraph('Pong_ESP32_Flowchart', comment='Diagrama de Flujo del Juego Pong ESP32')
    
    dot.attr(rankdir='TB', size='12,20', compound='true', nodesep='0.4', ranksep='0.5')
    dot.attr('node', shape='box', style='filled,rounded', fontname='Helvetica', fontsize='10')
    dot.attr('edge', fontname='Helvetica', fontsize='9')

    c_inicio, c_setup, c_input, c_estado = '#2ECC71', '#3498DB', '#F39C12', '#9B59B6'
    c_decide, c_juego, c_render = '#E67E22', '#1ABC9C', '#E74C3C'

    dot.node('Start', 'Inicio (Power On / Reset)', shape='ellipse', fillcolor=c_inicio, fontcolor='white')
    dot.node('Setup', 'setup()\n• TFT (480x320) & Búfer 1-bit\n• GPIOs (Joysticks, Botón, Buzzer)\n• Estado = BIENVENIDA', fillcolor=c_setup, fontcolor='white')
    dot.node('Loop', 'loop() [Bucle Principal]', shape='ellipse', fillcolor='#34495E', fontcolor='white')
    dot.node('ReadInputs', 'Procesar Entradas & Audio:\n• Filtro EMA + Zona Muerta\n• Lectura Botón\n• Audio Adaptativo', fillcolor=c_input, fontcolor='white')
    dot.node('FSM_Switch', 'Evaluación de Estado (EstadoJuego)', shape='diamond', fillcolor=c_decide, fontcolor='white')

    dot.node('St_Bienvenida', 'BIENVENIDA\n• Título, Créditos, Carga\n• ¿Carga Completa? -> MENU', fillcolor=c_estado, fontcolor='white')
    dot.node('St_Menu', 'MENU\n• Selección Modo (1P / 2P)', fillcolor=c_estado, fontcolor='white')
    dot.node('Dec_Modo', '¿Modo Elegido?', shape='diamond', fillcolor=c_decide, fontcolor='white')
    dot.node('St_Dificultad', 'DIFICULTAD (PvE)\n• Fácil, Medio, Difícil', fillcolor=c_estado, fontcolor='white')
    dot.node('St_Puntos', 'PUNTOS\n• Meta: 5, 10 o 15', fillcolor=c_estado, fontcolor='white')

    dot.node('St_Juego', 'JUEGO\n• Mover Raquetas (Humano/IA)\n• Pelota + Estela\n• Colisiones y Power-Ups', fillcolor=c_juego, fontcolor='white')
    dot.node('Dec_Pausa', '¿Botón Pausa?', shape='diamond', fillcolor=c_decide, fontcolor='white')
    dot.node('St_Pausa', 'PAUSA\n• Congelar Partida', fillcolor=c_estado, fontcolor='white')

    dot.node('Dec_Bala', '¿Power-Up Bala?', shape='diamond', fillcolor=c_decide, fontcolor='white')
    dot.node('St_Explosion', 'EXPLOSION\n• Trayectoria + Animación "BOOM"\n• Otorgar Punto', fillcolor=c_estado, fontcolor='white')

    dot.node('Dec_Punto', '¿Pelota Fuera?', shape='diamond', fillcolor=c_decide, fontcolor='white')
    dot.node('Dec_Ganador', '¿Puntaje >= Meta?', shape='diamond', fillcolor=c_decide, fontcolor='white')
    dot.node('ResetRound', 'Reiniciar Ronda\n• Screen Shake + Sonido', fillcolor=c_juego, fontcolor='white')
    dot.node('St_GameOver', 'GAME_OVER\n• Ganador Final\n• Botón -> Volver a MENU', fillcolor=c_estado, fontcolor='white')

    dot.node('Render', 'Renderizado Híbrido:\n1. Búfer 1-bit RAM\n2. Transferir a TFT\n3. Capas de Color', fillcolor=c_render, fontcolor='white')

    # Enlaces
    dot.edge('Start', 'Setup')
    dot.edge('Setup', 'Loop')
    dot.edge('Loop', 'ReadInputs')
    dot.edge('ReadInputs', 'FSM_Switch')

    dot.edge('FSM_Switch', 'St_Bienvenida', label=' BIENVENIDA')
    dot.edge('St_Bienvenida', 'St_Menu', label=' Carga Finalizada')
    dot.edge('FSM_Switch', 'St_Menu', label=' MENU')
    dot.edge('St_Menu', 'Dec_Modo', label=' Confirmar')
    dot.edge('Dec_Modo', 'St_Dificultad', label=' 1P (PvE)')
    dot.edge('Dec_Modo', 'St_Puntos', label=' 2P (PvP)')
    dot.edge('St_Dificultad', 'St_Puntos', label=' Confirmar')

    dot.edge('FSM_Switch', 'St_Puntos', label=' PUNTOS')
    dot.edge('St_Puntos', 'St_Juego', label=' Iniciar Partida')

    dot.edge('FSM_Switch', 'St_Juego', label=' JUEGO')
    dot.edge('St_Juego', 'Dec_Pausa')
    dot.edge('Dec_Pausa', 'St_Pausa', label=' Sí')
    dot.edge('FSM_Switch', 'St_Pausa', label=' PAUSA')
    dot.edge('St_Pausa', 'St_Juego', label=' Botón Presionado')

    dot.edge('Dec_Pausa', 'Dec_Bala', label=' No')
    dot.edge('Dec_Bala', 'St_Explosion', label=' Sí')
    dot.edge('FSM_Switch', 'St_Explosion', label=' EXPLOSION')
    dot.edge('Dec_Bala', 'Dec_Punto', label=' No')

    dot.edge('St_Explosion', 'Dec_Ganador', label=' Punto concedido')
    dot.edge('Dec_Punto', 'Dec_Ganador', label=' Sí')
    dot.edge('Dec_Ganador', 'St_GameOver', label=' Sí')
    dot.edge('Dec_Ganador', 'ResetRound', label=' No')
    dot.edge('ResetRound', 'Render')
    dot.edge('Dec_Punto', 'Render', label=' No')

    dot.edge('FSM_Switch', 'St_GameOver', label=' GAME_OVER')
    dot.edge('St_GameOver', 'St_Menu', label=' Reiniciar')
    dot.edge('Render', 'Loop')

    dot.render('diagrama_flujo_pong', format='png', cleanup=True)
    print("¡Éxito! Se ha generado el archivo 'diagrama_flujo_pong.png'.")

if __name__ == '__main__':
    generar_diagrama_pong()