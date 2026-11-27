# Building the macOS bundle

`bundle.sh` builds `Spek.app` and `Spek.dmg` against [Homebrew](https://brew.sh)'s
FFmpeg and wxWidgets. It works with Homebrew on Apple Silicon (`/opt/homebrew`)
and on Intel (`/usr/local`), and builds for the machine's own architecture, so
on an Apple Silicon Mac the result is a native arm64 app.

Install the build dependencies:

    brew install autoconf automake libtool gettext pkgconf coreutils
    brew install ffmpeg wxwidgets

Build and bundle Spek:

    ./dist/osx/bundle.sh

If you have already built Spek with `./autogen.sh && make`, package that build
without rebuilding:

    ./dist/osx/bundle.sh --no-build

The script finds `wxwin.m4` inside the wxwidgets keg itself, so no symlinks are
needed. It copies every non-system library Spek uses (FFmpeg, wxWidgets and
their dependencies, including ones Homebrew links via `@rpath` or
`@loader_path`) into `Spek.app/Contents/Frameworks`, rewrites their install
names, and fails if anything still points into Homebrew. It then sets
`LSMinimumSystemVersion` to the newest deployment target among the bundled
binaries, because Homebrew bottles target the macOS version they were built on.

## Signing

Apple Silicon refuses to run unsigned code, and rewriting install names
invalidates existing signatures, so the script re-signs every library and the
app. By default it uses an ad-hoc signature (`-`), which is enough to run the app
on the Mac that built it. A downloaded ad-hoc-signed app is quarantined by
Gatekeeper; right-click and choose Open, or run
`xattr -dr com.apple.quarantine /Applications/Spek.app`.

To sign with a Developer ID instead:

    CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" ./dist/osx/bundle.sh

Notarization is not set up yet.
