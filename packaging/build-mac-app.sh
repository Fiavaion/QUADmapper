#!/bin/bash
# Builds QUADmapper.app and QUADmapper-<version>-macOS-arm64.dmg (ADR-062).
#
#   packaging/build-mac-app.sh              build, sign, make the disk image
#   packaging/build-mac-app.sh --notarize   the same, then notarise with Apple and staple
#
# 1. cmake --build the Release app (build/mac-release must be configured:
#    cmake --preset mac-release).
# 2. cmake --install it into .scratch/mac-app/stage.
# 3. Lay out packaging/out/QUADmapper.app. The debug symbols are split off into
#    packaging/out/QUADmapper-<version>.dSYM, which stays with the developer like the
#    Windows PDB: it is what makes a user's crash report readable.
# 4. Sign with the hardened runtime. QM_SIGN_IDENTITY names the certificate; without it
#    the first "Developer ID Application" certificate in the keychain is used. With no
#    such certificate the app is signed ad hoc and runs on this Mac only.
# 5. Make and sign the disk image.
# 6. --notarize: send the app, then the disk image, to Apple and staple both tickets, so
#    a first start needs no internet. It needs a notarytool keychain profile, made once:
#      xcrun notarytool store-credentials quadmapper-notary
#    QM_NOTARY_PROFILE names a different profile.
# The version is the one in the top-level CMakeLists.txt project(VERSION).
set -euo pipefail

notarize=false
for argument in "$@"; do
    case "$argument" in
        --notarize) notarize=true ;;
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/mac-release"
stage="$repo/.scratch/mac-app/stage"
out="$repo/packaging/out"
app="$out/QUADmapper.app"
profile="${QM_NOTARY_PROFILE:-quadmapper-notary}"

version="$(sed -n 's/^ *VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "$repo/CMakeLists.txt" | head -1)"
[ -n "$version" ] || { echo "No project(QUADmapper VERSION x.y.z) in CMakeLists.txt" >&2; exit 1; }
[ -f "$build/CMakeCache.txt" ] || { echo "build/mac-release is not configured: cmake --preset mac-release" >&2; exit 1; }

identity="${QM_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk '/Developer ID Application/ { print $2; exit }')}"
if [ -z "$identity" ]; then
    if $notarize; then
        echo "No Developer ID Application certificate in the keychain: Apple notarises nothing else." >&2
        exit 1
    fi
    echo "No Developer ID Application certificate: signing ad hoc (this Mac only)."
    identity="-"
fi

# $1: what to sign. Apple's timestamp is required for notarisation; ad hoc has none.
sign() {
    if [ "$identity" = "-" ]; then
        codesign --force --options runtime --sign - "$1"
    else
        codesign --force --options runtime --timestamp --sign "$identity" "$1"
    fi
}

# $1: the file to send to Apple. $2: what to staple the ticket to.
notarise() {
    echo "Notarising $(basename "$1") (this takes a few minutes)"
    local result
    result="$(xcrun notarytool submit "$1" --keychain-profile "$profile" --wait)" || {
        echo "$result"
        echo "notarytool failed. Is the keychain profile '$profile' stored? See the top of this script." >&2
        exit 1
    }
    echo "$result"
    if ! grep -q "status: Accepted" <<<"$result"; then
        echo "Apple did not accept it. Read the reasons with:" >&2
        echo "  xcrun notarytool log <the id above> --keychain-profile $profile" >&2
        exit 1
    fi
    xcrun stapler staple "$2"
}

echo "QUADmapper $version: building Release"
cmake --build "$build" --target QUADmapper

echo "Staging into $stage"
rm -rf "$stage"
cmake --install "$build" --prefix "$stage" > /dev/null
[ -x "$stage/QUADmapper" ] || { echo "The staged folder has no QUADmapper" >&2; exit 1; }

echo "Laying out $app"
rm -rf "$app" "$out/QUADmapper-$version.dSYM"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$stage/QUADmapper" "$app/Contents/MacOS/QUADmapper"
cp -R "$stage/resources" "$app/Contents/Resources/resources"
cp "$stage/THIRD-PARTY-NOTICES.txt" "$stage/LICENSE.txt" "$app/Contents/Resources/"

min_macos="$(vtool -show-build "$app/Contents/MacOS/QUADmapper" | awk '$1 == "minos" { print $2; exit }')"
sed -e "s/@VERSION@/$version/g" -e "s/@MIN_MACOS@/$min_macos/g" \
    "$repo/packaging/macos/Info.plist.in" > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist" > /dev/null

# The icon is the Windows one: its largest picture is 256 pixels, which covers every
# size macOS asks for below 256@2x.
iconset="$repo/.scratch/mac-app/QUADmapper.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
sips -s format png "$repo/src/app/QUADmapper.ico" --out "$iconset/icon_256x256.png" > /dev/null
for size in 16 32 128; do
    sips -z $size $size "$iconset/icon_256x256.png" --out "$iconset/icon_${size}x${size}.png" > /dev/null
    sips -z $((size * 2)) $((size * 2)) "$iconset/icon_256x256.png" --out "$iconset/icon_${size}x${size}@2x.png" > /dev/null
done
iconutil --convert icns "$iconset" --output "$app/Contents/Resources/QUADmapper.icns"

echo "Splitting off the debug symbols"
dsymutil "$app/Contents/MacOS/QUADmapper" -o "$out/QUADmapper-$version.dSYM"
strip -S "$app/Contents/MacOS/QUADmapper"

echo "Signing"
sign "$app"
codesign --verify --strict "$app"
if $notarize; then
    zip="$repo/.scratch/mac-app/QUADmapper.zip"
    rm -f "$zip"
    ditto -c -k --keepParent "$app" "$zip"
    notarise "$zip" "$app"
fi

echo "Building the disk image"
dmg="$out/QUADmapper-$version-macOS-arm64.dmg"
dmgroot="$repo/.scratch/mac-app/dmgroot"
rm -rf "$dmgroot" "$dmg"
mkdir -p "$dmgroot"
cp -R "$app" "$dmgroot/"
ln -s /Applications "$dmgroot/Applications"
hdiutil create -volname "QUADmapper $version" -srcfolder "$dmgroot" -ov -format UDZO "$dmg" > /dev/null
sign "$dmg"
if $notarize; then
    notarise "$dmg" "$dmg"
fi

echo "Done: $dmg ($(du -h "$dmg" | cut -f1 | tr -d ' '))"
echo "SHA-256: $(shasum -a 256 "$dmg" | cut -d' ' -f1)"
if ! $notarize; then
    echo "Not notarised: another Mac will refuse to open it. Run again with --notarize for a release."
fi
