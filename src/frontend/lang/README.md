# Traducciones

Los textos se escriben en español en el código y cada idioma tiene aquí su `.po`
(`en.po`, `fr.po`, `pt.po`), que se mete dentro del ejecutable al compilar.

## Al añadir o cambiar un texto

1. Envuélvelo en el código:
   - `tr("Guardar estado")` para un texto que se muestra tal cual (también vale con
     `"Texto##id"`: solo se traduce lo de antes de `##`).
   - `trf("Ranura %d", n)` cuando lleva `%d`, `%s`...
   - `N_("...")` en tablas o constantes que se traducen más tarde con `tr(...)`.
2. Ejecuta `python3 tools/i18n.py --update`: añade los textos nuevos a los `.po` (con la
   traducción vacía) y quita los que ya no están.
3. Rellena el `msgstr` en los tres `.po` (`python3 tools/i18n.py --missing` lista lo
   que falta). Mantén los `%d`/`%s` en el mismo orden.
4. `python3 tools/i18n.py --check` (la prueba `i18n` de `ctest`) falla mientras falte
   algo.

Los `.po` van ordenados, así que dos ramas que añaden textos distintos rara vez chocan.

## Añadir un idioma

Añade el código en `PLUMSG_LANGUAGES` (CMakeLists.txt), en `LANGUAGES` (tools/i18n.py)
y en `kLanguages` (src/frontend/languages.cpp) con su nombre, y crea el `.po` con
`tools/i18n.py --update`.
