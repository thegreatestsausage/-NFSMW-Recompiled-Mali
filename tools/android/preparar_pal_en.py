#!/usr/bin/env python3
"""
Motor nativo para la PAL inglesa / rusa SIN la PAL Espana: la cadena de
preparar_rus.py (usa -> emparejar USA<->RUS), con red de seguridad identidad.

La nota del autor en tools/editions/rus/parejas_corregidas.json dice que con el
emparejamiento hecho "los 131 ficheros salen iguales que en el espanol": las
funciones que toca la app estarian en las mismas direcciones. Donde la cadena
no llega, se prueba identidad y lo dira el codegen / el movil.

    python tools/android/preparar_pal_en.py --juego <carpeta del juego RUS>
    python tools/android/preparar_pal_en.py --juego ... --rexglue <rexglue del arbol>
"""
import sys

import preparar_rus as pr
import preparar_nativo as pn

RAIZ = pn.RAIZ


def tabla_de_edicion(arbol, xex_ref, xex_otra, salida):
    """pal -> usa (tabla del Xenos, verificada) -> rus (emparejar por contenido);
    donde la cadena no llega, identidad marcada 'misma direccion'."""
    todas, en_codigo = pr.direcciones_de_fuentes(arbol)
    usa = pr.tabla_usa()
    img_u, _ = pn.imagen.cargar(str(xex_ref), str(pn.ediciones.CACHE))
    img_r, _ = pn.imagen.cargar(str(xex_otra), str(pn.ediciones.CACHE))
    e = pn.emparejar.Emparejador(img_u, img_r)
    filas, cuenta, a_rus = [], {}, {}
    for d in sorted(todas):
        fila = None
        if d in usa and usa[d][1] is not None:
            sec, u, est_u = usa[d]
            if est_u.startswith(pn.SEGURAS):
                s2, r, est = e.traducir(u, por_contenido=False)
                if r is None and (u in pn.CONSTANTES or u >= pn.FIN_IMAGEN):
                    r, est = u, "constante"
                if r is not None and est.startswith(pn.SEGURAS):
                    fila = "%08X\t%s\t%08X\t%s" % (d, sec, r, est)
                    a_rus[d] = r
                    cuenta["cadena " + est] = cuenta.get("cadena " + est, 0) + 1
        if fila is None:
            # Sin ancla PAL<->USA: identidad. La nota del autor dice que para la
            # PAL inglesa los ficheros salen iguales que en la espanola.
            fila = "%08X\t?\t%08X\tmisma direccion" % (d, d)
            a_rus[d] = d
            cuenta["identidad"] = cuenta.get("identidad", 0) + 1
        filas.append(fila)
    salida.parent.mkdir(parents=True, exist_ok=True)
    salida.write_text("pal\tseccion\totra\testado\n" + "\n".join(filas) + "\n", encoding="utf-8")
    print(f"[ok] {len(todas)} direcciones: {cuenta} -> {salida}")
    return e, {a_rus[d] for d in en_codigo}


def comprobar_funciones(e, en_codigo):
    malas = []
    for u in sorted(en_codigo):
        if e.traducir(u)[0] != ".text":
            continue
        p = e.comparar_funcion(u) + e.comprobar_parejas(u)[1]
        if p:
            malas.append("%08X: %s" % (u, "; ".join(p[:3])))
    print(f"[{'aviso' if malas else 'ok'}] funciones USA->RUS que toca la app: {len(malas)} distintas")
    for m in malas[:12]:
        print("   ", m)
    return malas


if __name__ == "__main__":
    pn.referencia_preparada = pr.referencia_preparada
    pn.tabla_de_edicion = tabla_de_edicion
    pn.huellas_shaders = pr.huellas_shaders
    pn.comprobar_funciones = comprobar_funciones
    sys.exit(pn.main())
