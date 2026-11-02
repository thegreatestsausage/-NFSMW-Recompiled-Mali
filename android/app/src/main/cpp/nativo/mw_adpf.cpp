// NFSMW Recompiled - sesion ADPF PerformanceHint (EXPERIMENTO, apagado)
//
// QUE HACE (cvar nfsmw_adpf, false por defecto)
//
//   1. Un hilo propio ("NFS adpf") busca en /proc/self/task los hilos
//      "Main XThread" y "GPU anillo", como hace el vigilante de afinidad.cpp.
//      Como esta llamada llega antes de que el juego cree sus hilos
//      (nfsmw_app.h la llama en OnPostInitLogging), los busca cada segundo
//      hasta que los dos esten.
//   2. Con los dos encontrados crea una APerformanceHintSession con ellos y
//      33333333 ns de objetivo (30 fps).
//   3. Cada segundo mide el frame time real con el contador de fotogramas del
//      juego (g_nfsmw_fotogramas_juego, el mismo que alimenta
//      GameActivity.nativeMsPorFotograma), lo reporta como duracion real
//      (reportActualWorkDuration) y, si se desvia del objetivo mas de 1 ms
//      tres veces seguidas, ajusta el objetivo (updateTargetWorkDuration).
//
// No toca frecuencias ni nucleos a mano: solo le pide al planificador de
// Android que mantenga el ritmo a esos dos hilos. Lo que hicieran los ADPF
// anteriores (sin efecto, o bajar el reloj de la GPU) se mide aqui con el
// cvar encendido; apagado, ninguna sesion existe.
//
// POR QUE EL GUARD NO ES DE COMPILACION
//
// La app compila con __ANDROID_API__ = 26 (su minSdk): un
// "#if __ANDROID_API__ >= 33" de compilacion dejaria el modulo siempre en
// no-op. El guard va en el arranque, por dlopen/dlsym de libandroid.so: en un
// movil sin API 33 esas entradas no existen y el modulo se queda en no-op,
// sin que libmain.so dependa de los simbolos en la carga (la API la marca
// __INTRODUCED_IN(33) y con minSdk 26 no puede enlazarse directa).

#include "mw_adpf.h"

#include <rex/cvar.h>
#include <rex/logging.h>

// El cvar, con el molde de los nfsmw_nativo_* de nfsmw_nativo_sistema.cpp.
// kInitOnly: solo se lee al arrancar; cambiarlo en caliente no crea ni borra
// la sesion.
REXCVAR_DEFINE_BOOL(nfsmw_adpf, false, "System",
                    "Android 13+ (EXPERIMENTO): sesion ADPF PerformanceHint para 'Main XThread' y "
                    "'GPU anillo', con el frame time real como objetivo (30 fps al limite). Ya fallo "
                    "en otros ports (sin efecto en Snapdragon, reloj de GPU abajo en otro): medir "
                    "antes de dejarlo. false = apagado (por defecto)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__ANDROID__)

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>

#include <android/performance_hint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// El contador de fotogramas presentados (nativo/app/src/nfsmw_d3d_trace.cpp,
// en su gancho del Swap). El mismo que usa MedirFps() de nativo_android.cpp.
extern std::atomic<uint64_t> g_nfsmw_fotogramas_juego;

namespace {

// 30 fps, lo que pide el dueño del proyecto.
constexpr int64_t kObjetivoTreintaFpsNanos = 33333333;
// Busqueda de hilos y latido del informe.
constexpr auto kIntervaloBusqueda = std::chrono::seconds(1);
constexpr auto kIntervaloInforme = std::chrono::seconds(1);
// Un frame time fuera de aqui no es una medida que valga (pausa, cargas...).
constexpr int64_t kFrameNanosMinimo = 2000000;    //   2 ms (500 fps)
constexpr int64_t kFrameNanosMaximo = 200000000;  // 200 ms (  5 fps)
// Ticks seguidos desviados mas de esto antes de mover el objetivo.
constexpr int64_t kDesvioObjetivoNanos = 1000000;  // 1 ms
constexpr int kDesviosParaMover = 3;

// Las entradas de libandroid.so, resueltas en el arranque. En un movil sin
// API 33 no estan y el modulo no hace nada.
struct Api {
  APerformanceHintManager* (*get_manager)() = nullptr;
  APerformanceHintSession* (*create_session)(APerformanceHintManager*, const int32_t*, size_t,
                                             int64_t) = nullptr;
  int64_t (*preferred_update_rate)(APerformanceHintManager*) = nullptr;
  int (*update_target)(APerformanceHintSession*, int64_t) = nullptr;
  int (*report_actual)(APerformanceHintSession*, int64_t) = nullptr;
  bool completa = false;
};

Api g_api;

bool CargarApi() {
  // Ya esta cargada (SDL, el propio sistema): dlopen solo da el handle.
  void* libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (!libandroid) {
    REXLOG_WARN("[adpf] sin libandroid.so ({}): no hay sesion", dlerror());
    return false;
  }
  g_api.get_manager = reinterpret_cast<decltype(g_api.get_manager)>(
      dlsym(libandroid, "APerformanceHint_getManager"));
  g_api.create_session = reinterpret_cast<decltype(g_api.create_session)>(
      dlsym(libandroid, "APerformanceHint_createSession"));
  g_api.preferred_update_rate = reinterpret_cast<decltype(g_api.preferred_update_rate)>(
      dlsym(libandroid, "APerformanceHint_getPreferredUpdateRateNanos"));
  g_api.update_target = reinterpret_cast<decltype(g_api.update_target)>(
      dlsym(libandroid, "APerformanceHint_updateTargetWorkDuration"));
  g_api.report_actual = reinterpret_cast<decltype(g_api.report_actual)>(
      dlsym(libandroid, "APerformanceHint_reportActualWorkDuration"));
  if (!g_api.get_manager || !g_api.create_session || !g_api.update_target || !g_api.report_actual) {
    // Sin API 33 las entradas no existen: es lo esperado en un movil viejo,
    // no un fallo.
    REXLOG_INFO("[adpf] sin APerformanceHint (API < 33): no se hace nada");
    return false;
  }
  g_api.completa = true;
  return true;
}

// Un fichero pequeño de /proc, como el LeerFichero de afinidad.cpp.
bool LeerComm(int tid, char* nombre, size_t tam) {
  char ruta[64];
  std::snprintf(ruta, sizeof(ruta), "/proc/self/task/%d/comm", tid);
  FILE* f = std::fopen(ruta, "re");
  if (!f) {
    return false;  // el hilo acaba de terminar
  }
  const bool ok = std::fgets(nombre, static_cast<int>(tam), f) != nullptr;
  std::fclose(f);
  if (!ok) {
    return false;
  }
  // Sin el salto de linea final.
  for (size_t i = std::strlen(nombre); i > 0 && (nombre[i - 1] == '\n' || nombre[i - 1] == '\r'); --i) {
    nombre[i - 1] = '\0';
  }
  return true;
}

// El primer tid cuyo comm empieza por `prefijo`, o 0. Por el principio y no
// completo: el kernel corta el nombre a 15 letras ("GPU anillo nativo" queda
// "GPU anillo nati"), como ya sabia afinidad.cpp.
int32_t HiloPorPrefijo(const char* prefijo) {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    return 0;
  }
  int32_t tid = 0;
  while (!tid) {
    const dirent* e = readdir(dir);
    if (!e) {
      break;
    }
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    char nombre[32];
    if (!LeerComm(std::atoi(e->d_name), nombre, sizeof(nombre))) {
      continue;
    }
    if (std::strncmp(nombre, prefijo, std::strlen(prefijo)) == 0) {
      tid = static_cast<int32_t>(std::atoi(e->d_name));
    }
  }
  closedir(dir);
  return tid;
}

// Los dos hilos de la sesion, esperando a que el juego los cree.
bool BuscarHilos(int32_t* tids) {
  tids[0] = HiloPorPrefijo("Main XThread");
  tids[1] = HiloPorPrefijo("GPU anillo");
  return tids[0] != 0 && tids[1] != 0;
}

void Bucle() {
  // Si no, heredaria el nombre de quien lo crea ("SDLThread").
  pthread_setname_np(pthread_self(), "NFS adpf");
  if (!CargarApi()) {
    return;
  }

  // Los hilos del juego pueden tardar: nfsmw_android_nucleos_grandes_aplicar
  // corre antes de que existan. Se buscan cada segundo hasta que esten los dos.
  int32_t tids[2] = {0, 0};
  int intentos = 0;
  while (!BuscarHilos(tids)) {
    if (++intentos == 30) {
      REXLOG_WARN("[adpf] medio minuto sin ver 'Main XThread' y 'GPU anillo': se sigue buscando");
    }
    std::this_thread::sleep_for(kIntervaloBusqueda);
  }
  REXLOG_INFO("[adpf] hilos encontrados: 'Main XThread' tid {}, 'GPU anillo' tid {}", tids[0],
              tids[1]);

  auto* manager = g_api.get_manager();
  auto* sesion =
      manager ? g_api.create_session(manager, tids, 2, kObjetivoTreintaFpsNanos) : nullptr;
  if (!sesion) {
    REXLOG_WARN("[adpf] APerformanceHint_createSession fallo: sin sesion");
    return;
  }
  const int64_t preferido =
      g_api.preferred_update_rate ? g_api.preferred_update_rate(manager) : 0;
  REXLOG_INFO("[adpf] sesion creada (target {} ns, 30 fps; tasa de actualizacion del sistema {} ns)",
              kObjetivoTreintaFpsNanos, preferido);

  // Latido: frame time real por segundo, de los fotogramas presentados.
  using reloj = std::chrono::steady_clock;
  auto antes = reloj::now();
  uint64_t fotogramas_antes = g_nfsmw_fotogramas_juego.load(std::memory_order_relaxed);
  int64_t objetivo = kObjetivoTreintaFpsNanos;
  int desvios = 0;
  while (true) {
    std::this_thread::sleep_for(kIntervaloInforme);
    const auto ahora = reloj::now();
    const double segundos = std::chrono::duration<double>(ahora - antes).count();
    antes = ahora;
    const uint64_t fotogramas = g_nfsmw_fotogramas_juego.load(std::memory_order_relaxed);
    if (segundos < 0.25 || fotogramas == fotogramas_antes) {
      continue;  // en pausa o sin fotogramas: nada que reportar
    }
    const double fps = double(fotogramas - fotogramas_antes) / segundos;
    fotogramas_antes = fotogramas;
    if (fps < 5.0 || fps > 240.0) {
      continue;  // medida sin sentido (cargas, desbordes del contador...)
    }
    const int64_t real = std::clamp<int64_t>(int64_t(1e9 / fps), kFrameNanosMinimo,
                                             kFrameNanosMaximo);
    if (g_api.report_actual(sesion, real) != 0) {
      // EPIPE: el servicio se fue. Sin sesion no hay mas que reportar; la
      // dejamos y callamos, que reintentar aqui no arregla nada.
      REXLOG_WARN("[adpf] reportActualWorkDuration rechazo {} ns: se para el informe", real);
      return;
    }
    // El objetivo sigue al frame time real, cuando se desvia de verdad.
    if (real > objetivo + kDesvioObjetivoNanos || real < objetivo - kDesvioObjetivoNanos) {
      if (++desvios >= kDesviosParaMover) {
        if (g_api.update_target(sesion, real) == 0) {
          REXLOG_INFO("[adpf] target {} -> {} ns ({} fps medidos)", objetivo, real, fps);
          objetivo = real;
        }
        desvios = 0;
      }
    } else {
      desvios = 0;
    }
  }
}

}  // namespace

void NfsmwAdpfArrancar() {
  if (!REXCVAR_GET(nfsmw_adpf)) {
    return;  // apagado por defecto: ni hilo ni sesion
  }
  REXLOG_INFO("[adpf] experimento encendido (nfsmw_adpf)");
  std::thread(Bucle).detach();  // vive lo que el proceso: GameActivity mata al salir
}

#else  // sin Android: no hace nada

void NfsmwAdpfArrancar() {}

#endif  // __ANDROID__
