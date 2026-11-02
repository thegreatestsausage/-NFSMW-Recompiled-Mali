// NFSMW Recompiled - sesion ADPF PerformanceHint (EXPERIMENTO, apagado)
//
// Que hace (cvar nfsmw_adpf, false por defecto): en Android 13 (API 33) o
// mas, una sesion de APerformanceHintSession para los hilos "Main XThread" y
// "GPU anillo", con el frame time real del juego como duracion objetivo.
//
// Por que apagado por defecto: ADPF ya fallo dos veces en proyectos parecidos
// (NFSMW en Snapdragon: sin efecto medible; otro port: ADPF bajaba el reloj
// de la GPU Mali). Aqui es solo un experimento que se mide con el cvar
// encendido: apagado no se crea ninguna sesion y el modulo no hace nada.
//
// Solo entra en la build de Android (nativo.cmake). En cualquier otro sitio
// el arranque no hace nada.

#ifndef NFSMW_MW_ADPF_H
#define NFSMW_MW_ADPF_H

#if defined(__ANDROID__)
// Lo llama nativo_android.cpp en nfsmw_android_nucleos_grandes_aplicar. No
// bloquea a quien lo llama: si los hilos del juego todavia no existen, el
// hilo propio del modulo los espera (los busca cada segundo). Los cvars ya
// estan leidos cuando llega esa llamada.
void NfsmwAdpfArrancar();
#else
// Sin Android (iOS compila este fichero): no hace nada.
inline void NfsmwAdpfArrancar() {}
#endif

#endif  // NFSMW_MW_ADPF_H
