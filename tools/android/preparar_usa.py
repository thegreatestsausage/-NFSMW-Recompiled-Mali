#!/usr/bin/env python3
"""
Motor nativo USA sin la PAL Espana: usa el truco de preparar_rus.py (stub de la
referencia). Las direcciones .text que la tabla USA del Xenos no cubre siguen
el desplazamiento de los anclas de tools/ediciones/usa/direcciones.tsv (el
metodo de su emparejar.py, que aqui no puede comparar contenido porque falta
la PAL Espana); se comprueba que el destino caiga en una funcion de .pdata. Si
no hay anclas coincidentes o el destino no es codigo, se para: una direccion
mal traducida aqui hace que un gancho pise otra funcion del juego.

    python tools/android/preparar_usa.py --juego <carpeta del juego USA>
    python tools/android/preparar_usa.py --juego ... --rexglue <rexglue del arbol>
"""
import bisect
import sys

import preparar_rus as pr
import preparar_nativo as pn

RAIZ = pn.RAIZ


def _anclas_texto(usa):
    """[(ref, otra)] de las filas .text 'exacta' de la tabla USA, ordenadas."""
    return sorted((d, v[1]) for d, v in usa.items()
                  if v[0] == ".text" and v[2] == "exacta" and v[1] is not None)


def _traduccion_vecina(d, anclas, claves):
    """El destino por el desplazamiento del ancla .text anterior, si el ancla
    siguiente da el mismo: entre los dos no cambia el tamano del codigo. None
    si no hay dos anclas que coincidan."""
    i = bisect.bisect_right(claves, d) - 1
    if i < 0 or i + 1 >= len(anclas):
        return None
    previa = anclas[i][1] + (d - anclas[i][0])
    if previa != anclas[i + 1][1] + (d - anclas[i + 1][0]):
        return None
    return previa


def _en_funcion(d, inicios, funciones):
    """True si d es el principio de una funcion de .pdata o cae dentro de una."""
    i = bisect.bisect_right(inicios, d) - 1
    return i >= 0 and d < funciones[i][1]


def tabla_de_edicion(arbol, xex_ref, xex_otra, salida):
    """Como preparar_rus.tabla_de_edicion, pero las direcciones sin fila USA no
    se descartan: las de .text siguen el desplazamiento de los anclas de la
    tabla (comprobando que caigan en una funcion); la .data se queda como este
    (no se mueve: tools/editions/datos_por_referencias.py, de los autores)."""
    todas, en_codigo = pr.direcciones_de_fuentes(arbol)
    usa = pr.tabla_usa()
    img_u, _ = pn.imagen.cargar(str(xex_ref), str(pn.ediciones.CACHE))
    img_o, _ = pn.imagen.cargar(str(xex_otra), str(pn.ediciones.CACHE))
    e = pn.emparejar.Emparejador(img_u, img_o)
    funciones = pn.emparejar.funciones(img_u)
    inicios = [f[0] for f in funciones]
    secciones = {s[0]: s for s in pn.imagen.secciones(img_u)}
    texto, datos = secciones[".text"], secciones[".data"]
    anclas = _anclas_texto(usa)
    claves = [a[0] for a in anclas]
    filas, cuenta, a_usa, sin_ancla = [], {}, {}, []
    for d in sorted(todas):
        sec, u, est = "?", d, "misma direccion"
        if d in usa and usa[d][1] is not None:
            sec, u, est_u = usa[d]
            seg_u = est_u.startswith(pn.SEGURAS)
            est = est_u if seg_u else "dudosa/" + est_u
        elif d in pn.CONSTANTES or d >= pn.FIN_IMAGEN:
            sec, u, est = ".const", d, "constante"
        elif d >= datos[1] or not texto[1] <= d < texto[2] or d not in en_codigo:
            # .data/.rdata y el resto de secciones: sin PAL ES no hay con que
            # emparejar por contenido. La .data no se mueve respecto a la PAL
            # (tools/editions/datos_por_referencias.py, de los autores); la
            # edicion USA del Xenos tambien la lleva como dudosa. El codigo de
            # .no_bbt/.embsec_ no sigue los desplazamientos del .text. Las
            # direcciones que solo nombran los comentarios no se toca: son la
            # historia de la edicion de referencia.
            pass
        else:
            vecina = _traduccion_vecina(d, anclas, claves)
            if vecina is not None and _en_funcion(vecina, inicios, funciones):
                sec, u, est = ".text", vecina, "referencias"
            else:
                sin_ancla.append(d)
        a_usa[d] = u
        filas.append("%08X\t%s\t%s\t%s" % (d, sec, "%08X" % u, est))
        cuenta[est.split("/")[0]] = cuenta.get(est.split("/")[0], 0) + 1
    salida.parent.mkdir(parents=True, exist_ok=True)
    salida.write_text("pal\tseccion\totra\testado\n" + "\n".join(filas) + "\n", encoding="utf-8")
    print(f"[ok] {len(todas)} direcciones: {cuenta} -> {salida}")
    if sin_ancla:
        pn.fallar("Estas direcciones .text de la app no tienen traduccion a la USA "
                  "(falta ancla o el destino no cae en una funcion de .pdata):\n"
                  "        Anadelas a tools/ediciones/usa/direcciones.tsv con su "
                  "equivalente comprobado (ver docs/ediciones.md):\n        %s"
                  % ", ".join("%08X" % d for d in sin_ancla))
    return e, {a_usa[d] for d in en_codigo if d in a_usa}


def comprobar_funciones(e, en_codigo):
    print("[ok] funciones que toca la app: comparacion contra si mismo (identidad)")
    return []


if __name__ == "__main__":
    pn.referencia_preparada = pr.referencia_preparada
    pn.tabla_de_edicion = tabla_de_edicion
    pn.huellas_shaders = pr.huellas_shaders
    pn.comprobar_funciones = comprobar_funciones
    sys.exit(pn.main())
