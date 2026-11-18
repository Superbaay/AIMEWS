// Cliente WebSocket hacia /ws de la API, compartido por el Home del panel y Control_dron.
// Depende de sesion_panel.js (salir_del_panel). Cada página pone sus textos de registro y su estado del enlace
// en las funciones al_*; aquí solo vive el socket, la reconexión y el rechazo de la sesión.
//
// El navegador no permite cabeceras en WebSocket: el JWT va en el primer mensaje ({"token": "..."}), no en la URL,
// así no queda escrito en los registros del servidor ni en el historial.
//
// crear_enlace_ws({ url, token, al_conectar, al_abrir, al_mensaje, al_mensaje_invalido, al_cerrar, al_rechazo, al_reintento })
//   al_conectar()          justo después de crear un socket nuevo
//   al_abrir()             socket abierto y token ya enviado
//   al_mensaje(mensaje)    cada mensaje de texto ya convertido de JSON
//   al_mensaje_invalido()  el mensaje no era JSON (o al_mensaje lanzó un error)
//   al_cerrar(evento)      el socket se cerró, antes de decidir si se reintenta
//   al_rechazo()           la API rechazó la sesión: en 1200 ms se sale del panel
//   al_reintento(espera_ms) se volverá a conectar tras espera_ms
// Devuelve { conectar(), enviar(objeto) → bool, marcar_rechazada(), estado, rechazada }
//   estado es socket.readyState (WebSocket.CONNECTING, OPEN, CLOSING o CLOSED) o null si aún no hay socket.
function crear_enlace_ws({ url, token, al_conectar, al_abrir, al_mensaje, al_mensaje_invalido, al_cerrar, al_rechazo, al_reintento }) {
  let socket = null;
  let sesion_rechazada = false;
  let intentos_reconexion = 0;
  let temporizador_reconexion = null;

  function conectar() {
    clearTimeout(temporizador_reconexion);
    if (sesion_rechazada) return;
    if (socket && socket.readyState <= WebSocket.OPEN) return;

    const socket_nuevo = new WebSocket(url);
    socket = socket_nuevo;
    al_conectar?.();

    socket_nuevo.addEventListener('open', () => {
      intentos_reconexion = 0;
      socket_nuevo.send(JSON.stringify({ token }));
      al_abrir?.();
    });

    socket_nuevo.addEventListener('message', (evento) => {
      try {
        al_mensaje(JSON.parse(evento.data));
      } catch (error) {
        al_mensaje_invalido?.();
      }
    });

    socket_nuevo.addEventListener('close', (evento) => {
      al_cerrar?.(evento);
      // 1008 (policy violation) o un "Unauthorized" previo: la API rechazó el token, reconectar no sirve
      if (sesion_rechazada || evento.code === 1008) {
        sesion_rechazada = true;
        al_rechazo?.();
        setTimeout(salir_del_panel, 1200);
        return;
      }
      const espera_ms = Math.min(30000, 1000 * 2 ** intentos_reconexion);
      intentos_reconexion += 1;
      al_reintento?.(espera_ms);
      temporizador_reconexion = setTimeout(conectar, espera_ms);
    });
  }

  function enviar(mensaje) {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify(mensaje));
    return true;
  }

  return {
    conectar,
    enviar,
    // La API respondió "Unauthorized" o "WebSocket error…": el próximo cierre no reintenta
    marcar_rechazada() { sesion_rechazada = true; },
    get estado() { return socket ? socket.readyState : null; },
    get rechazada() { return sesion_rechazada; },
  };
}
