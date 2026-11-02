#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rex::audio {

// Limitador feed-forward con lookahead: ~5 ms de retraso dejan ver el
// transitorio ANTES de que suene, de modo que la ganancia ya bajo cuando
// llega. El limitador de muestra a muestra anterior atacaba con el pico ya
// dentro (la recuperacion de 80 ms llega tarde a un transitorio), asi que lo
// que sobresalia lo remataba el clamp — y ese recorte era el sonido
// "recocido". Con lookahead el techo no se pisa nunca y el clamp sobra.
// Canales enlazados (una ganancia para el frame entero): preserva el balance
// estereo. Estado persistente entre bloques: una instancia por driver y solo
// el hilo de audio la toca.
class Limitador {
 public:
  Limitador() = default;
  Limitador(const Limitador&) = delete;
  Limitador& operator=(const Limitador&) = delete;

  // Procesa frames*channels muestras float intercaladas, en el mismo sitio.
  void Procesar(float* data, size_t frames, size_t channels, int sample_rate) {
    if (frames == 0 || channels == 0 || sample_rate <= 0) {
      return;
    }
    const size_t latencia = size_t(double(sample_rate) * 0.005) * channels;
    if (linea_.size() != latencia) {
      linea_.assign(latencia, 0.0f);
      entrada_.assign(channels, 0.0f);
      ocupadas_ = 0;
      lectura_ = 0;
      escritura_ = 0;
      ganancia_ = 1.0f;
    }
    // Constante de tiempo de la subida de ganancia: 80 ms, como antes.
    const float recuperacion = 1.0f / (0.080f * float(sample_rate));
    // Ataque suave (0,5 ms): un escalon instantaneo de ganancia sobre el audio
    // que todavia esta sonando es un chasquido audible. Con 5 ms de lookahead
    // la ganancia llega igualmente antes del transitorio (10 constantes de
    // tiempo).
    const float ataque = 1.0f / (0.0005f * float(sample_rate));
    for (size_t i = 0; i < frames; ++i) {
      float* frame = data + i * channels;
      // In-place: la salida se escribe sobre la entrada, asi que el frame de
      // entrada se salva ANTES de tocar data. (Sin esto el anillo recirculaba
      // la salida retrasada y el audio se apagaba en cero.)
      for (size_t ch = 0; ch < channels; ++ch) {
        if (!std::isfinite(frame[ch])) frame[ch] = 0.0f;
        entrada_[ch] = frame[ch];
      }
      float pico_entrada = 0.0f;
      for (size_t ch = 0; ch < channels; ++ch) {
        pico_entrada = std::max(pico_entrada, std::fabs(entrada_[ch]));
      }
      // La ganancia mira el maximo de la ventana que todavia no sono: el
      // frame entrante mas todo lo ya retrasado en la linea.
      float pico_ventana = pico_entrada;
      for (size_t k = 0; k < linea_.size(); ++k) {
        const float v = std::fabs(linea_[k]);
        if (v > pico_ventana) pico_ventana = v;
      }
      const float objetivo = pico_ventana > techo ? techo / pico_ventana : 1.0f;
      ganancia_ = objetivo < ganancia_
                      ? ganancia_ + (objetivo - ganancia_) * ataque
                      : ganancia_ + (objetivo - ganancia_) * recuperacion;

      if (ocupadas_ == linea_.size()) {
        // El frame mas viejo del retraso sale ahora, con la ganancia ya
        // ajustada a lo que viene detras de el.
        for (size_t ch = 0; ch < channels; ++ch) {
          frame[ch] = linea_[lectura_ + ch] * ganancia_;
        }
      } else {
        // Arranque: aun no hay 5 ms acumulados; sale silencio y el frame
        // entra al retraso como los demas.
        for (size_t ch = 0; ch < channels; ++ch) {
          frame[ch] = 0.0f;
        }
      }
      for (size_t ch = 0; ch < channels; ++ch) {
        linea_[escritura_ + ch] = entrada_[ch];
      }
      escritura_ += channels;
      if (escritura_ == linea_.size()) escritura_ = 0;
      if (ocupadas_ < linea_.size()) {
        ocupadas_ += channels;
      } else {
        lectura_ += channels;
        if (lectura_ == linea_.size()) lectura_ = 0;
      }
    }
  }

 private:
  static constexpr float techo = 0.97f;
  // Anillo de retraso: latencia*channels muestras intercaladas.
  std::vector<float> linea_;
  // Copia del frame de entrada: data se procesa in-place y la salida lo
  // pisa antes de que el frame entre al anillo.
  std::vector<float> entrada_;
  size_t ocupadas_ = 0;
  size_t lectura_ = 0;
  size_t escritura_ = 0;
  float ganancia_ = 1.0f;
};

}  // namespace rex::audio
