# Toolbar icons

Monochrome line icons for Open, Save, Help and the info bar's Close, drawn for
this fork on a 24-unit grid (16 for Close) with 1.6-unit round strokes.

The SVGs here are the sources. To rebuild the bitmaps:

    node dist/icons/render.js /tmp/icons            # 1x, 2x, 4x PNGs, black on transparent
    cp /tmp/icons/{open,save,help,close}{,@2x}.png dist/osx/
    python3 dist/icons/make-ico.py /tmp/icons dist/win

On macOS the app recolours the PNGs at run time to match light or dark mode
(`spek_artwork_icon` in `src/spek-artwork.cc`). Windows gets dark grey .ico
files. Linux keeps the GTK theme's own icons.
