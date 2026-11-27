#!/usr/bin/env bash
#
# Build Spek.app and Spek.dmg against Homebrew's FFmpeg and wxWidgets.
#
# Works with Homebrew on Apple Silicon (/opt/homebrew) and on Intel (/usr/local).
# The app is built for the architecture of the machine (and Homebrew) it runs on,
# so on an Apple Silicon Mac it is a native arm64 app.
#
# Usage: dist/osx/bundle.sh [--no-build]
#
#   --no-build   Package an existing src/spek instead of running autogen and make.
#
# Environment:
#   CODESIGN_IDENTITY   Signing identity, "-" (ad-hoc) by default. Apple Silicon
#                       refuses to run unsigned code, and install_name_tool
#                       invalidates existing signatures, so everything is re-signed.

set -euo pipefail

LANGUAGES="bs ca cs da de el eo es fi fr gl he hr hu id it ja ko lv nb nl nn pl pt_BR ru sk sr@latin sv th tr uk vi zh_CN zh_TW"

BUILD=1
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")/../.."
TOP=$(pwd)

BREW_PREFIX=$(brew --prefix)
CODESIGN_IDENTITY=${CODESIGN_IDENTITY:--}

if [ "$BUILD" = 1 ]; then
    # Homebrew keeps wxwin.m4 inside the wxwidgets keg, off aclocal's search path.
    WXM4=$(find "$(brew --prefix wxwidgets)/" -name wxwin.m4 | head -1)
    if [ -z "$WXM4" ]; then
        echo "wxwin.m4 not found; is wxwidgets installed?" >&2
        exit 1
    fi
    # gettext's m4 macros (AM_GNU_GETTEXT) live in share/gettext/m4 in current
    # gettext releases; without them configure silently disables translations.
    GTM4=$(find "$(brew --prefix gettext)/share/" -name gettext.m4 | head -1)
    if [ -z "$GTM4" ]; then
        echo "gettext.m4 not found; is gettext installed?" >&2
        exit 1
    fi
    export ACLOCAL_PATH="$(dirname "$WXM4"):$(dirname "$GTM4"):$BREW_PREFIX/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
    rm -f src/spek
    # Spek translates through wxWidgets, not libintl, but configure only builds
    # the po/*.gmo catalogs when it finds libintl, which Homebrew keeps off the
    # default search path.
    ./autogen.sh --with-libintl-prefix="$(brew --prefix gettext)"
    make -j"$(sysctl -n hw.ncpu)"
fi

if [ ! -x src/spek ] || [ ! -f dist/osx/Info.plist ]; then
    echo "src/spek or dist/osx/Info.plist is missing; build Spek first." >&2
    exit 1
fi

WX_RELEASE=$(wx-config --release)

cd dist/osx
APP=Spek.app
FW=$APP/Contents/Frameworks
EXE=$APP/Contents/MacOS/Spek

rm -fr "$APP"
mkdir -p "$APP/Contents/MacOS" "$FW" "$APP/Contents/Resources"
cp "$TOP/src/spek" "$EXE"
cp Info.plist "$APP/Contents/"
cp Spek.icns "$APP/Contents/Resources/"
cp *.png "$APP/Contents/Resources/"
cp "$TOP/CREDITS.md" "$TOP/LICENSE" "$TOP/README.md" "$APP/Contents/Resources/"
mkdir "$APP/Contents/Resources/lic"
cp "$TOP"/lic/* "$APP/Contents/Resources/lic/"

for lang in $LANGUAGES; do
    mkdir -p "$APP/Contents/Resources/$lang.lproj"
    cp "$TOP/po/$lang.gmo" "$APP/Contents/Resources/$lang.lproj/spek.mo"
    wxmo="$BREW_PREFIX/share/locale/$lang/LC_MESSAGES/wxstd-$WX_RELEASE.mo"
    if [ -f "$wxmo" ]; then
        cp "$wxmo" "$APP/Contents/Resources/$lang.lproj/"
    else
        echo "No wxWidgets translation for $lang"
    fi
done
mkdir -p "$APP/Contents/Resources/en.lproj"

# Print the LC_RPATH entries of a Mach-O file, one per line.
rpaths() {
    otool -l "$1" | awk '$1 == "cmd" && $2 == "LC_RPATH" { f = 1 } f && $1 == "path" { print $2; f = 0 }'
}

# Resolve a dependency install name to a file on disk, or print nothing if the
# dependency is a system library that must not be bundled.
#   $1: install name, as printed by otool -L
#   $2: original path of the binary that references it (for @loader_path/@rpath)
resolve() {
    local dep=$1 src=$2 dir rp cand
    dir=$(dirname "$src")
    case "$dep" in
        /usr/lib/*|/System/*) return 0 ;;
        @loader_path/*|@executable_path/*)
            cand="$dir/${dep#*/}"
            ;;
        @rpath/*)
            cand=""
            for rp in $(rpaths "$src"); do
                rp=${rp/#@loader_path/$dir}
                rp=${rp/#@executable_path/$dir}
                if [ -f "$rp/${dep#@rpath/}" ]; then
                    cand="$rp/${dep#@rpath/}"
                    break
                fi
            done
            if [ -z "$cand" ]; then
                echo "Cannot resolve $dep for $src" >&2
                return 1
            fi
            ;;
        /*) cand=$dep ;;
        *)
            echo "Unexpected install name $dep in $src" >&2
            return 1
            ;;
    esac
    if [ ! -f "$cand" ]; then
        echo "Dependency $dep of $src not found at $cand" >&2
        return 1
    fi
    realpath "$cand"
}

# Breadth-first walk: each work item is "<file in bundle>|<original path>".
# Every non-system dependency is copied into Contents/Frameworks once and every
# reference to it is rewritten to @executable_path/../Frameworks/<name>.
QUEUE="$EXE|$(realpath "$TOP/src/spek")"
while [ -n "$QUEUE" ]; do
    NEXT=""
    for item in $QUEUE; do
        bin=${item%%|*}
        src=${item#*|}
        echo "Updating dependencies for $bin"
        for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}'); do
            case "$dep" in @executable_path/../Frameworks/*) continue ;; esac
            reallib=$(resolve "$dep" "$src")
            [ -n "$reallib" ] || continue
            libname=$(basename "$reallib")
            # A dylib lists its own install name too; that is fixed with -id below.
            if [ "$libname" = "$(basename "$bin")" ] && [ "$bin" != "$EXE" ]; then
                continue
            fi
            install_name_tool -change "$dep" "@executable_path/../Frameworks/$libname" "$bin" 2>/dev/null
            if [ ! -f "$FW/$libname" ]; then
                echo "    Bundling $reallib"
                cp "$reallib" "$FW/"
                chmod u+w "$FW/$libname"
                install_name_tool -id "@executable_path/../Frameworks/$libname" "$FW/$libname" 2>/dev/null
                NEXT="$NEXT $FW/$libname|$reallib"
            fi
        done
    done
    QUEUE=${NEXT# }
done

# Fail if anything still points outside the bundle or the OS.
LEAKS=0
for bin in "$EXE" "$FW"/*; do
    for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}'); do
        case "$dep" in
            /usr/lib/*|/System/*|@executable_path/../Frameworks/*) ;;
            *) echo "ERROR: $bin still references $dep" >&2; LEAKS=1 ;;
        esac
    done
done
[ "$LEAKS" = 0 ] || exit 1

# The app can't run on a macOS older than the newest deployment target among
# the binaries it carries (Homebrew bottles target the macOS they were built for).
MINOS=$(for bin in "$EXE" "$FW"/*; do
    otool -l "$bin" | awk '$1 == "cmd" && $2 == "LC_BUILD_VERSION" { f = 1 } f && $1 == "minos" { print $2; f = 0 }'
done | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
if [ -n "$MINOS" ]; then
    /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $MINOS" "$APP/Contents/Info.plist"
fi

# install_name_tool invalidated any signatures; Apple Silicon won't load
# unsigned code, so sign every library, then the app itself.
codesign --force --sign "$CODESIGN_IDENTITY" "$FW"/*
codesign --force --sign "$CODESIGN_IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"

echo "Spek.app: $(lipo -archs "$EXE"), macOS ${MINOS:-?} or later, $(ls "$FW" | wc -l | tr -d ' ') bundled libraries"

# Make the DMG image.
DMG_FILE=Spek.dmg
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -Rp "$APP" "$STAGE/"
ln -s /Applications "$STAGE/ "
cp DS_Store "$STAGE/.DS_Store"

rm -f "$DMG_FILE"
echo "Creating $DMG_FILE..."
# hdiutil occasionally fails with "Resource busy" on CI machines; retry.
for attempt in 1 2 3; do
    if hdiutil create -quiet -volname Spek -srcfolder "$STAGE" -fs HFS+ -format UDBZ -ov "$DMG_FILE"; then
        break
    fi
    [ "$attempt" = 3 ] && exit 1
    sleep 5
done

echo "Done."
