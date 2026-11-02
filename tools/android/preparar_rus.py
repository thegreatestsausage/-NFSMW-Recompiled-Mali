#!/usr/bin/env python3
"""
Motor nativo para la PAL inglesa / rusa SIN la PAL Espana.

preparar_nativo.py traduce otra edicion desde la PAL Espana (xex + codigo generado).
Aqui no la hay: se encadena PAL Espana -> USA (tools/ediciones/usa/direcciones.tsv,
ya calculada) -> PAL inglesa (emparejar.py entre los dos XEX que si tenemos).

    python tools/android/preparar_rus.py --tabla     :: solo la tabla y sus comprobaciones
"""
import argparse
import pathlib
import sys

import preparar_nativo as pn

RAIZ = pn.RAIZ
USA_TSV = RAIZ / "tools" / "ediciones" / "usa" / "direcciones.tsv"


def direcciones_de_fuentes(arbol):
    app = arbol / "app"
    todas, en_codigo = set(), set()
    for ruta in (app / "src").rglob("*"):
        if ruta.is_file() and ruta.name.endswith(pn.FUENTES) and not ruta.name.endswith(pn.BINARIOS):
            t = ruta.read_text(encoding="utf-8")
            todas |= pn._direcciones(t)
            en_codigo |= pn._direcciones_en_codigo(t)
    for rel in pn.OTROS:
        todas |= pn._direcciones((app / rel).read_text(encoding="utf-8"))
    todas |= pn._direcciones((arbol / "tools" / "copia_literal.py").read_text(encoding="utf-8"))
    return todas, en_codigo


def tabla_usa():
    t = {}
    for l in USA_TSV.read_text(encoding="utf-8").splitlines()[1:]:
        c = l.split("\t")
        t[int(c[0], 16)] = (c[2], None if c[3] == "-" else int(c[3], 16), c[4])
    return t



USA_XEX = RAIZ / "nativo" / "assets" / "game_root_usa" / "default.xex"
USA_LIB = RAIZ / "nativo" / "out" / "usa" / "nfsmw_shaders.nfsp"


def stub_referencia(arbol):
    """crear_arbol.py parte del codigo generado de la PAL Espana, que no tenemos:
    sin reparto sembrado el codegen reparte las funciones solo."""
    g = arbol / "app" / "generated"
    (g / "default").mkdir(parents=True, exist_ok=True)
    if not (g / "default" / "codegen.partition.json").exists():
        (g / "default" / "codegen.partition.json").write_text('{"assignments": {}}\n', encoding="utf-8")
    if not (g / "rexglue.cmake").exists():
        (g / "rexglue.cmake").write_text("# stub: sin PAL Espana\n", encoding="utf-8")


def referencia_preparada(arbol):
    stub_referencia(arbol)
    return USA_XEX


def tabla_de_edicion(arbol, xex_usa, xex_rus, salida):
    todas, en_codigo = direcciones_de_fuentes(arbol)
    usa = tabla_usa()
    img_u, _ = pn.imagen.cargar(str(xex_usa), str(pn.ediciones.CACHE))
    img_r, _ = pn.imagen.cargar(str(xex_rus), str(pn.ediciones.CACHE))
    e = pn.emparejar.Emparejador(img_u, img_r)
    filas, cuenta, a_usa = [], {}, {}
    for d in sorted(todas):
        if d in usa and usa[d][1] is not None:
            sec, u, est_u = usa[d]
            seg_u = est_u.startswith(pn.SEGURAS)
        elif d in pn.CONSTANTES or d >= pn.FIN_IMAGEN:
            sec, u, est_u, seg_u = ".const", d, "constante", True
        else:
            filas.append("%08X\t?\t-\tsin fila USA" % d)
            cuenta["dudosa"] = cuenta.get("dudosa", 0) + 1
            continue
        a_usa[d] = u
        s2, r, est = e.traducir(u, por_contenido=False)
        if r is None and (u in pn.CONSTANTES or u >= pn.FIN_IMAGEN):
            r, est = u, "constante (misma en las dos)"
        seg = seg_u and est.startswith(pn.SEGURAS)
        filas.append("%08X\t%s\t%s\t%s" % (d, sec, "%08X" % r if r else "-", est if seg or r is None else "dudosa/" + est))
        cuenta["segura" if seg and r else "dudosa"] = cuenta.get("segura" if seg and r else "dudosa", 0) + 1
    salida.parent.mkdir(parents=True, exist_ok=True)
    salida.write_text("pal\tseccion\totra\testado\n" + "\n".join(filas) + "\n", encoding="utf-8")
    print(f"[ok] {len(todas)} direcciones: {cuenta.get('segura', 0)} seguras, {cuenta.get('dudosa', 0)} dudosas -> {salida}")
    return e, {a_usa[d] for d in en_codigo if d in a_usa}


def huellas_shaders(arbol, ficha, biblioteca):
    """Como preparar_nativo.huellas_shaders, con la USA (que si tenemos) en lugar de la PAL."""
    import re
    fuente = (arbol / "app" / "src" / "nfsmw_nativo_dibujos.cpp").read_text(encoding="utf-8")
    usa_map = pn.ediciones.datos()["ediciones"]["usa"]["nativo"]["huellas_shaders"]
    usa = pn._biblioteca(USA_LIB)
    suya = pn._biblioteca(biblioteca)
    cambian = {}
    for nombre in pn.HUELLAS_ESPECIALES:
        m = re.search(rf"constexpr uint64_t {nombre} = 0x([0-9A-Fa-f]{{16}})ull;", fuente)
        pal = "%016X" % int(m.group(1), 16)
        h_usa = int(usa_map.get(pal, pal), 16)
        if h_usa not in usa:
            pn.fallar(f"{nombre}: {h_usa:016X} no esta en la biblioteca USA.")
        b = usa[h_usa]
        ini = int.from_bytes(b[4:8], "big")
        ig = sorted(h for h, u in suya.items() if u[4:8] == b[4:8] and u[ini:] == b[ini:])
        if len(ig) != 1:
            pn.fallar(f"{nombre}: {len(ig)} contenedores con ese microcodigo en la biblioteca de la edicion.")
        if ig[0] != int(pal, 16):
            cambian[pal] = "%016X" % ig[0]
    print("[ok] huellas de shaders:", cambian or "las mismas")
    return cambian


def comprobar_funciones(e, en_codigo):
    malas = []
    for u in sorted(en_codigo):
        if e.traducir(u)[0] != ".text":
            continue
        p = e.comparar_funcion(u) + e.comprobar_parejas(u)[1]
        if p:
            malas.append("%08X: %s" % (u, "; ".join(p[:3])))
    print(f"[{'aviso' if malas else 'ok'}] funciones USA->RUS que toca la app: {len(malas)} distintas")
    for m in malas:
        print("   ", m)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--tabla", action="store_true", help="solo la tabla")
    a, resto = ap.parse_known_args()
    pn.referencia_preparada = referencia_preparada
    pn.tabla_de_edicion = tabla_de_edicion
    pn.huellas_shaders = huellas_shaders
    pn.comprobar_funciones = comprobar_funciones
    if a.tabla:
        arbol = RAIZ / "nativo"
        stub_referencia(arbol)
        rus = RAIZ.parent / "ISO" / "NFS Most Wanted RUS" / "default.xex"
        e, cod = tabla_de_edicion(arbol, USA_XEX, rus, arbol / "out" / "pal_en" / "tabla.tsv")
        comprobar_funciones(e, cod)
    else:
        sys.argv = [sys.argv[0]] + resto
        sys.exit(pn.main())
