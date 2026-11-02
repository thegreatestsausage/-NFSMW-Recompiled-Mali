// mw4droid: __imp__-originales que el descubrimiento no crea en esta edicion.
// Lista: union de llvm-nm sobre objetos + undefined de lld (el log corta a 44).
// STUBS DE SONDEO: vacios; REXGLUE_SURVEY=1 registra las direcciones reales.
// Solo __imp__: sub_ las definen los REX_HOOK_RAW de la app.
//
// mw4droid (usa): se queda vacio. Hasta ahora tapaba con stubs vacios los
// __imp__sub_ que el descubrimiento no creaba porque la tabla de direcciones
// dejaba en identidad direcciones .text de la app sin fila USA: los ganchos
// del video ni se enganchaban y el render entero iba con el placeholder. Con
// la tabla completa (tools/ediciones/usa/direcciones.tsv), el codegen define
// cada __imp__sub_ que la app referencia y un stub aqui seria un simbolo
// duplicado. Si el enlazador vuelve a pedir un __imp__sub_, anade su stub
// aqui (y revisa esa direccion en la tabla, no el simbolo).
#include <rex/ppc/context.h>
#include <rex/logging.h>

namespace {
[[maybe_unused]] void mw_stub(PPCContext&, uint8_t*) {}
}
