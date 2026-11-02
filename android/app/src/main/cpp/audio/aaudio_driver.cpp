#include "aaudio_driver.h"

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <SDL3/SDL_events.h>
#include <android/log.h>
#include <sys/system_properties.h>

#if NFSMW_MOTOR_NATIVO
// Con el motor nativo (nfsmw-android) su SDK trae lo que su driver de SDL hace
// ademas de sacar el sonido, y aqui se hace igual:
//   - audio_ganancia_pct, el volumen del juego (GetOutputGain);
//   - un limitador con los dos canales enlazados, sobre el pliegue sin recortar;
//   - callar el juego mientras suena la pista propia de una pelicula
//     (IsGameOutputSuppressed), que si no se oye doble;
//   - el subsistema de audio de SDL iniciado: la pista de las peliculas va por
//     SDL (nfsmw_video_wmv3.cpp) y lo da por hecho.
#include <SDL3/SDL_init.h>

#include <rex/audio/downmix.h>
#include <rex/audio/output_limiter.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform/env.h>

REXCVAR_DECLARE(int32_t, audio_ganancia_pct);
REXCVAR_DECLARE(bool, audio_estadisticas);
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define TAG "NFS-AAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace rex::audio::android {

namespace {

constexpr int32_t kSampleRate = 48000;

#if NFSMW_MOTOR_NATIVO
// Diagnostico del nivel: pico y muestras recortadas del plegado ANTES del
// limitador, que es lo que dice si el juego o el pliegue entregan un nivel que
// no cabe. Se activa con el cvar audio_estadisticas, con el entorno
// REXGLUE_AUDIOSTATS o, en el telefono, con "adb shell setprop
// debug.nfsmw.audio 1". Informe cada ~5 s de audio.
struct EstadisticasSalida {
  uint64_t muestras = 0;
  uint64_t recortadas = 0;
  float pico = 0.0f;
  // Picos crudos por canal del huesped (FL FR C LFE BL BR), antes del plegado:
  // dicen si el juego entrega canales a tope o si el exceso lo arma el plegado.
  float canal[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // Hambra del anillo en AudioCallback: eventos y muestras de silencio que el
  // DAC pidio y no habia. El chasquido de underrun no se oye en el pico del
  // plegado — se cuenta aqui.
  std::atomic<uint64_t> hambre_eventos{0};
  std::atomic<uint64_t> hambre_muestras{0};
  // Bloques del huesped repetidos bit por bit (mezclador del juego parado).
  std::atomic<uint64_t> repeticiones{0};
};

EstadisticasSalida& Estadisticas() {
  static EstadisticasSalida e;
  return e;
}

bool SondeoActivado() {
  char valor_prop[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.nfsmw.audio", valor_prop) > 0 && valor_prop[0] != '\0' &&
      valor_prop[0] != '0') {
    return true;
  }
  return REXCVAR_GET(audio_estadisticas) || rex::platform::env::get("REXGLUE_AUDIOSTATS");
}

void MedirSalida(const float* datos, size_t muestras) {
  if (!SondeoActivado()) {
    return;
  }
  auto& e = Estadisticas();
  for (size_t i = 0; i < muestras; ++i) {
    const float v = std::fabs(datos[i]);
    if (v > e.pico) e.pico = v;
    if (v >= 1.0f) ++e.recortadas;
  }
  e.muestras += muestras;
  // 2 canales de salida (kCanalesSalida de AndroidAAudioDriver, aun no
  // declarado aqui): el informe cubre ~5 s de audio estereo.
  if (e.muestras < uint64_t(kSampleRate) * 2 * 5) {
    return;
  }
  REXAPU_INFO("[audio] peak={:.4f} clip={}/{} underrun={}/{} canales=[{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}] (FL FR C LFE BL BR crudos)",
              e.pico, e.recortadas, e.muestras, e.hambre_eventos.load(),
              e.hambre_muestras.load(), e.canal[0], e.canal[1], e.canal[2], e.canal[3],
              e.canal[4], e.canal[5]);
  // Duplicado en logcat: el log del SDK va a --log_file (en release del
  // lanzador, /dev/null), y desde logcat se lee en cualquier lanzamiento.
  LOGI("[audio] peak=%.4f clip=%zu/%zu underrun=%llu/%llu rep=%llu ch=[%.2f %.2f %.2f %.2f %.2f %.2f]",
       e.pico, e.recortadas, e.muestras,
       (unsigned long long)e.hambre_eventos.load(), (unsigned long long)e.hambre_muestras.load(),
       (unsigned long long)e.repeticiones.load(),
       e.canal[0], e.canal[1], e.canal[2], e.canal[3], e.canal[4], e.canal[5]);
  e.pico = 0.0f;
  e.recortadas = 0;
  e.muestras = 0;
  for (float& c : e.canal) c = 0.0f;
  e.hambre_eventos.store(0, std::memory_order_relaxed);
  e.hambre_muestras.store(0, std::memory_order_relaxed);
  e.repeticiones.store(0, std::memory_order_relaxed);
}

// Vuelco de la salida FINAL (lo que oye el DAC) para diagnostico: con
// "setprop debug.nfsmw.audio 2" escribe ~20 s de float32 estereo 48 kHz en
// /storage/emulated/0/Android/data/io.github.nfsmwrecomp/files/audio_dump.raw
// y cierra solo. Con 0 o 1 deja de volcar. Asi el chasquido se VE en la forma
// de onda en el PC en vez de adivinarse por picos.
struct VuelcoSalida {
  FILE* fichero = nullptr;
  uint64_t escritas = 0;
};

VuelcoSalida& Vuelco(const char* sufijo) {
  static VuelcoSalida pre, post;
  return std::string_view(sufijo) == "pre" ? pre : post;
}

void VolcarSalida(const float* datos, size_t muestras, const char* sufijo) {
  char valor_prop[PROP_VALUE_MAX] = {0};
  const bool activado = __system_property_get("debug.nfsmw.audio", valor_prop) > 0 &&
                        valor_prop[0] == '2';
  auto& v = Vuelco(sufijo);
  char ruta[160];
  snprintf(ruta, sizeof(ruta),
           "/storage/emulated/0/Android/data/io.github.nfsmwrecomp/files/audio_dump_%s.raw",
           sufijo);
  if (!activado) {
    if (v.fichero) {
      fclose(v.fichero);
      v.fichero = nullptr;
      LOGI("[audio] vuelco %s cerrado: %llu muestras", sufijo,
           (unsigned long long)v.escritas);
    }
    v.escritas = 0;
    return;
  }
  if (!v.fichero) {
    v.fichero = fopen(ruta, "wb");
    v.escritas = 0;
    if (v.fichero) {
      LOGI("[audio] vuelco %s abierto (20 s max, float32 estereo 48k)", sufijo);
    } else {
      LOGW("[audio] no se pudo abrir %s", ruta);
    }
  }
  if (!v.fichero || v.escritas >= uint64_t(kSampleRate) * 2 * 20) {
    return;
  }
  const size_t restantes = size_t(uint64_t(kSampleRate) * 2 * 20 - v.escritas);
  fwrite(datos, sizeof(float), std::min(muestras, restantes), v.fichero);
  v.escritas += std::min(muestras, restantes);
  if (v.escritas >= uint64_t(kSampleRate) * 2 * 20) {
    fclose(v.fichero);
    v.fichero = nullptr;
    LOGI("[audio] vuelco %s completo: 20 s", sufijo);
  }
}
#endif

// Pesos del plegado de 5.1 a estereo. kFoldScale es la normalizacion del
// plegado (1/(1+0.707) seria la ITU; aqui la de -3 dB, como el driver de SDL en
// Android pone en su fold (sdl_audio_driver.cpp: fold.scale = 0.70710678)). Sin
// ella el pliegue sale +3 dB por encima del SDL y el limitador vive pegado al
// techo: eso era el sonido "recocido".
constexpr float kCenterGain = 0.7071f;
constexpr float kSurroundGain = 0.7071f;
constexpr float kLfeGain = 0.5f;
constexpr float kFoldScale = 0.70710678f;

inline float LoadBEFloat(uint32_t raw_be) {
  const uint32_t le = __builtin_bswap32(raw_be);
  float f;
  std::memcpy(&f, &le, sizeof(float));
  return f;
}

bool SDLCALL AAudioEventWatch(void* userdata, SDL_Event* event) {
  auto* driver = static_cast<AndroidAAudioDriver*>(userdata);
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
    driver->Pause();
  } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
    driver->Resume();
  }
  return true;
}

}  // namespace

AndroidAAudioDriver::AndroidAAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore), anillo_(new float[kCapacidad]()) {}

AndroidAAudioDriver::~AndroidAAudioDriver() {
  Shutdown();
}

bool AndroidAAudioDriver::AbrirFlujo() {
  AAudioStreamBuilder* builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK || !builder) {
    LOGE("Failed to create AAudioStreamBuilder: %s", AAudio_convertResultToText(result));
    return false;
  }

  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(builder, kCanalesSalida);
  AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setDataCallback(builder, AudioCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, this);

  result = AAudioStreamBuilder_openStream(builder, &stream_);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK || !stream_) {
    LOGE("Failed to open AAudioStream: %s", AAudio_convertResultToText(result));
    stream_ = nullptr;
    return false;
  }

  result = AAudioStream_requestStart(stream_);
  if (result != AAUDIO_OK) {
    LOGE("Failed to start AAudioStream: %s", AAudio_convertResultToText(result));
    AAudioStream_close(stream_);
    stream_ = nullptr;
    return false;
  }

  LOGI("AAudio stream started: 48kHz Stereo Float, BufferSize=%d",
       AAudioStream_getBufferSizeInFrames(stream_));
  return true;
}

void AndroidAAudioDriver::CerrarFlujo() {
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }
}

bool AndroidAAudioDriver::Initialize() {
#if NFSMW_MOTOR_NATIVO
  SetOutputGain(float(REXCVAR_GET(audio_ganancia_pct)) / 100.0f);
  sdl_audio_iniciado_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (!sdl_audio_iniciado_) {
    LOGW("SDL audio no inicia (%s): las peliculas iran sin sonido", SDL_GetError());
  }
#endif
  {
    std::lock_guard<std::mutex> lock(control_);
    if (!AbrirFlujo()) {
      return false;
    }
  }
  is_running_.store(true, std::memory_order_release);
  SDL_AddEventWatch(AAudioEventWatch, this);
  return true;
}

void AndroidAAudioDriver::Pause() {
  std::lock_guard<std::mutex> lock(control_);
  if (stream_) {
    const aaudio_result_t r = AAudioStream_requestPause(stream_);
    REXLOG_INFO("[aaudio] pausa: {}", AAudio_convertResultToText(r));
  }
}

void AndroidAAudioDriver::Resume() {
  std::lock_guard<std::mutex> lock(control_);
  if (stream_) {
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    REXLOG_INFO("[aaudio] reanudar: {}", AAudio_convertResultToText(r));
  }
}

void AndroidAAudioDriver::Shutdown() {
  if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  SDL_RemoveEventWatch(AAudioEventWatch, this);

  // Si habia una reconexion en marcha, que termine antes de cerrar: si no,
  // podria reabrir un flujo sobre un objeto que se esta destruyendo. Se espera
  // FUERA del cerrojo, porque ese hilo tambien lo coge.
  std::thread pendiente;
  {
    std::lock_guard<std::mutex> lock(control_);
    pendiente = std::move(reconexion_);
  }
  if (pendiente.joinable()) {
    pendiente.join();
  }

  // Despertar a quien espere en el semaforo del audio.
  if (semaphore_) {
    semaphore_->Release(16, nullptr);
  }

#if NFSMW_MOTOR_NATIVO
  if (sdl_audio_iniciado_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_iniciado_ = false;
  }
#endif

  std::lock_guard<std::mutex> lock(control_);
  CerrarFlujo();
}

void AndroidAAudioDriver::SubmitFrame(uint32_t samples_ptr) {
  if (!is_running_.load(std::memory_order_relaxed)) {
    return;
  }

  // El fotograma de audio de la Xbox 360 vive en memoria virtual del invitado.
  uint8_t* frame_data = nullptr;
  if (memory_) {
    frame_data = memory_->TranslateVirtual<uint8_t*>(samples_ptr);
    if (!frame_data) {
      frame_data = TranslatePhysical(samples_ptr);
    }
  }
  if (!frame_data) {
    return;
  }

  // Sin hueco para un bloque entero: se descarta el NUEVO. El productor no toca
  // nunca el indice de lectura, que es lo que permite ir sin cerrojo. Con el
  // anillo dimensionado a kMaximumQueuedFrames no deberia pasar.
  const size_t w = escritura_.load(std::memory_order_relaxed);
  const size_t r = lectura_.load(std::memory_order_acquire);
  if (kCapacidad - (w - r) < kFloatsPorBloque) {
    return;
  }

  // PLANAR, 6 canales de 256 muestras cada uno (6144 bytes), en big-endian.
  // Orden de canales: 0=FL, 1=FR, 2=C, 3=LFE, 4=BL, 5=BR.
  const uint32_t* src = reinterpret_cast<const uint32_t*>(frame_data);
  const uint32_t* ch_fl = src + 0 * kMuestrasPorCanal;
  const uint32_t* ch_fr = src + 1 * kMuestrasPorCanal;
  const uint32_t* ch_c = src + 2 * kMuestrasPorCanal;
  const uint32_t* ch_lfe = src + 3 * kMuestrasPorCanal;
  const uint32_t* ch_bl = src + 4 * kMuestrasPorCanal;
  const uint32_t* ch_br = src + 5 * kMuestrasPorCanal;

  // Se pliega directamente en el anillo. Un bloque son 512 floats y la
  // capacidad es multiplo de 512, asi que un bloque nunca cruza el final: basta
  // una sola posicion base, sin partir la copia en dos.
  float* dst = anillo_.get() + (w & kMascara);
#if NFSMW_MOTOR_NATIVO
  if (IsGameOutputSuppressed()) {
    // Suena la pista de una pelicula: el juego calla, pero el bloque se
    // publica igual, que el ritmo lo sigue marcando el DAC.
    std::memset(dst, 0, kFloatsPorBloque * sizeof(float));
  } else {
    // Sin recortar: el limitador necesita ver los picos de verdad.
    const float ganancia = GetOutputGain();
    const bool sondear = SondeoActivado();
    for (size_t i = 0; i < kMuestrasPorCanal; ++i) {
      const float fl = LoadBEFloat(ch_fl[i]);
      const float fr = LoadBEFloat(ch_fr[i]);
      const float cc = LoadBEFloat(ch_c[i]);
      const float lf = LoadBEFloat(ch_lfe[i]);
      const float bl = LoadBEFloat(ch_bl[i]);
      const float br = LoadBEFloat(ch_br[i]);
      if (sondear) {
        auto& e = Estadisticas();
        const float crudos[6] = {fl, fr, cc, lf, bl, br};
        for (int k = 0; k < 6; ++k) {
          const float v = std::fabs(crudos[k]);
          if (v > e.canal[k]) e.canal[k] = v;
        }
      }
      const float c = cc * kCenterGain;
      const float lfe = lf * kLfeGain;
      const float mid = c + lfe;
      const float left = fl + mid + bl * kSurroundGain;
      const float right = fr + mid + br * kSurroundGain;
      dst[i * 2 + 0] = left * ganancia * kFoldScale;
      dst[i * 2 + 1] = right * ganancia * kFoldScale;
    }
    MedirSalida(dst, kFloatsPorBloque);
    VolcarSalida(dst, kFloatsPorBloque, "pre");
    // PARCHE LOCAL - bloque repetido: el huesped parado re-emite su ultimo
    // bloque tal cual, y eso suena a disco rayado. Un sonido real nunca
    // repite 5,3 ms bit por bit: si se repite, silencio y contador.
    if (hay_previo_ && std::memcmp(dst, bloque_previo_, sizeof(bloque_previo_)) == 0) {
      std::memset(dst, 0, kFloatsPorBloque * sizeof(float));
      ++Estadisticas().repeticiones;
    } else {
      std::memcpy(bloque_previo_, dst, sizeof(bloque_previo_));
      hay_previo_ = true;
    }
    limitador_.Procesar(dst, kMuestrasPorCanal, kCanalesSalida, kSampleRate);
    VolcarSalida(dst, kFloatsPorBloque, "post");
  }
#else
  for (size_t i = 0; i < kMuestrasPorCanal; ++i) {
    const float c = LoadBEFloat(ch_c[i]) * kCenterGain;
    const float lfe = LoadBEFloat(ch_lfe[i]) * kLfeGain;
    const float mid = c + lfe;
    const float left = LoadBEFloat(ch_fl[i]) + mid + LoadBEFloat(ch_bl[i]) * kSurroundGain;
    const float right = LoadBEFloat(ch_fr[i]) + mid + LoadBEFloat(ch_br[i]) * kSurroundGain;
    dst[i * 2 + 0] = std::clamp(left * kFoldScale, -1.0f, 1.0f);
    dst[i * 2 + 1] = std::clamp(right * kFoldScale, -1.0f, 1.0f);
  }
#endif

  // Publicar el bloque: el release garantiza que el consumidor ve las muestras
  // escritas antes que el indice nuevo.
  escritura_.store(w + kFloatsPorBloque, std::memory_order_release);

  // El semaforo NO se suelta aqui: lo suelta AudioCallback segun el DAC
  // consume, que es lo que marca el ritmo de 48 kHz.
}

aaudio_data_callback_result_t AndroidAAudioDriver::AudioCallback(AAudioStream* /*stream*/,
                                                                 void* userData, void* audioData,
                                                                 int32_t numFrames) {
  auto* self = static_cast<AndroidAAudioDriver*>(userData);
  float* out = static_cast<float*>(audioData);
  if (numFrames <= 0) {
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  const size_t pedidos = size_t(numFrames) * kCanalesSalida;
  const size_t r = self->lectura_.load(std::memory_order_relaxed);
  const size_t w = self->escritura_.load(std::memory_order_acquire);
  const size_t n = std::min(w - r, pedidos);

  // Copiar lo que haya, en uno o dos tramos segun cruce el final del anillo.
  const size_t pos = r & kMascara;
  const size_t primero = std::min(n, kCapacidad - pos);
  std::memcpy(out, self->anillo_.get() + pos, primero * sizeof(float));
  if (n > primero) {
    std::memcpy(out + primero, self->anillo_.get(), (n - primero) * sizeof(float));
  }
  // Lo que falte, silencio. Se cuenta: el underrun suena a chasquido y no se
  // ve en el pico del plegado.
  if (n < pedidos) {
    std::memset(out + n, 0, (pedidos - n) * sizeof(float));
    auto& e = Estadisticas();
    e.hambre_eventos.fetch_add(1, std::memory_order_relaxed);
    e.hambre_muestras.fetch_add(pedidos - n, std::memory_order_relaxed);
  }
  self->lectura_.store(r + n, std::memory_order_release);

  // Un permiso por cada 256 fotogramas que consume el DAC, haya dato o no: eso
  // es lo que clava el hilo de audio del juego a 48 kHz (187,5 bloques/s).
  if (self->semaphore_) {
    self->consumidos_ += size_t(numFrames);
    const size_t soltar = self->consumidos_ / kMuestrasPorCanal;
    if (soltar > 0) {
      self->consumidos_ -= soltar * kMuestrasPorCanal;
      self->semaphore_->Release(int32_t(soltar), nullptr);
    }
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AndroidAAudioDriver::ErrorCallback(AAudioStream* /*stream*/, void* userData,
                                        aaudio_result_t error) {
  LOGW("AAudio stream error: %s", AAudio_convertResultToText(error));
  if (error != AAUDIO_ERROR_DISCONNECTED) {
    return;
  }
  auto* self = static_cast<AndroidAAudioDriver*>(userData);

  // AAudio NO permite cerrar ni reabrir el flujo desde su propio callback de
  // error: puede colgarse. Se hace en un hilo aparte. El anillo sobrevive a la
  // reconexion, asi que no se pierde lo ya plegado.
  //
  // Una reconexion anterior, si la hubo, se espera FUERA del cerrojo: ese hilo
  // tambien lo coge y se bloquearian los dos.
  std::thread anterior;
  {
    std::lock_guard<std::mutex> lock(self->control_);
    if (!self->is_running_.load(std::memory_order_acquire)) {
      return;
    }
    anterior = std::move(self->reconexion_);
  }
  if (anterior.joinable()) {
    anterior.join();
  }

  std::lock_guard<std::mutex> lock(self->control_);
  if (!self->is_running_.load(std::memory_order_acquire)) {
    return;
  }
  self->reconexion_ = std::thread([self]() {
    std::lock_guard<std::mutex> lock(self->control_);
    self->CerrarFlujo();
    if (self->is_running_.load(std::memory_order_acquire)) {
      self->AbrirFlujo();
    }
  });
}

// ---------------------------------------------------------------------------
//  AndroidAAudioSystem
// ---------------------------------------------------------------------------

AndroidAAudioSystem::AndroidAAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

AndroidAAudioSystem::~AndroidAAudioSystem() = default;

X_STATUS AndroidAAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                          AudioDriver** out_driver) {
  auto driver = std::make_unique<AndroidAAudioDriver>(memory_, semaphore);
  if (!driver->Initialize()) {
    LOGE("Failed to initialize AndroidAAudioDriver for client %zu", index);
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}

void AndroidAAudioSystem::DestroyDriver(AudioDriver* driver) {
  delete driver;
}

}  // namespace rex::audio::android
