# App icon

`spek.svg` is the source: a spectrogram with a cyan cutoff line flattening
the middle bars, the mark this fork draws on lossy files. It sits on a
macOS-style rounded tile (824 of 1024 units, with a shadow).

To rebuild every platform's icon:

    node dist/appicon/render.js dist/appicon/spek.svg /tmp/appicon
    for s in 16 22 24 32 48; do cp /tmp/appicon/spek-$s.png data/icons/${s}x${s}/spek.png; done
    cp dist/appicon/spek.svg data/icons/scalable/spek.svg
    python3 -c "from PIL import Image; Image.open('/tmp/appicon/spek-1024.png').save('dist/osx/Spek.icns')"
    python3 -c "from PIL import Image; s=(16,24,32,48,64,96,128,256); i=[Image.open(f'/tmp/appicon/spek-{n}.png') for n in s]; i[-1].save('dist/win/spek.ico', sizes=[(n,n) for n in s], append_images=i[:-1])"
