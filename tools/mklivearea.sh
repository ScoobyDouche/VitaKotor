#!/usr/bin/env bash
# Generate the Vita LiveArea asset set from the APK's own artwork.
#
#   icon0.png    128x128   home-screen bubble
#   bg.png       840x500   LiveArea background
#   startup.png  280x158   gate image
#
# Requires ImageMagick. Source art is read from apk/, which is gitignored.
set -euo pipefail

ROOT="${1:-/home/bird/Desktop/VitaKotor}"
OUT="${2:-$ROOT/sce_sys}"
LOGO="$ROOT/apk/res/drawable-hdpi/logo.png"
LAUNCH="$ROOT/apk/res/mipmap-xxhdpi/ic_launcher.png"
# Character art for bg.png: Revan on a white backdrop, Bastila on black.
REVAN="$ROOT/apk/livearea/revan.jpeg"
BASTILA="$ROOT/apk/livearea/bastila.jpeg"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -f "$LOGO" ]   || { echo "missing $LOGO"   >&2; exit 1; }
[ -f "$LAUNCH" ] || { echo "missing $LAUNCH" >&2; exit 1; }
[ -f "$REVAN" ]   || { echo "missing $REVAN"   >&2; exit 1; }
[ -f "$BASTILA" ] || { echo "missing $BASTILA" >&2; exit 1; }

# The Vita package installer rejects the entire VPK with error 0x8010113D if any
# sce_sys PNG is not 8-bit indexed, so everything here is written through PNG8.
# Floyd-Steinberg keeps the 256-colour limit from banding the space gradient.
png8() {  # png8 <in> <out>
  convert "$1" -dither FloydSteinberg -colors 256 -strip "PNG8:$2"
}

mkdir -p "$OUT/livearea/contents"

# ---------------------------------------------------------------- starfield --
# Sparse white points, two passes at different densities so the field has both
# faint background stars and a few bright ones.
# NB: -separate leaves -channel active, so +channel must reset it before
# -threshold or only that one channel gets thresholded and the rest comes
# through as full-intensity noise.
stars() {  # stars <w> <h> <out>
  local w=$1 h=$2 out=$3
  convert -size "${w}x${h}" xc:black +noise Random -channel G -separate +channel \
          -threshold 99.5% -blur 0x0.4 -evaluate multiply 0.5 "$TMP/s1.png"
  convert -size "${w}x${h}" xc:black +noise Random -channel R -separate +channel \
          -threshold 99.94% -blur 0x0.8 -evaluate multiply 0.85 "$TMP/s2.png"
  convert "$TMP/s1.png" "$TMP/s2.png" -compose screen -composite "$out"
}

# --------------------------------------------------------------------- bg.png
# Deep space gradient, warm nebula glow, starfield, Revan and Bastila, vignette.
#
# NB: radial-gradient only reaches its end colour at the *corner* radius, so the
# edge midpoints stay lit and the layer screens on as a visible rectangle. Every
# glow here is a drawn ellipse blurred into black instead, which falls off on
# all sides.
glow() {  # glow <w> <h> <rx> <ry> <colour> <blur> <mul> <out>
  convert -size "${1}x${2}" xc:black -fill "$5" \
          -draw "ellipse $(( $1 / 2 )),$(( $2 / 2 )) ${3},${4} 0,360" \
          -blur "0x${6}" -evaluate multiply "$7" "$8"
}

convert -size 840x500 gradient:'#0d1526'-'#010206' "$TMP/base.png"

glow 840 500 300 170 '#2a1e10' 90 0.9 "$TMP/neb.png"
convert "$TMP/base.png" "$TMP/neb.png" -compose screen -composite "$TMP/bg1.png"

stars 840 500 "$TMP/stars.png"
convert "$TMP/bg1.png" "$TMP/stars.png" -compose screen -composite "$TMP/bg2.png"

# Revan far left, Bastila right. No title: the gate and the bubble carry it.
# Both are composited SOLID, with an alpha mask, after the vignette, so
# nothing of the starfield shows through them and the vignette does not dim
# them.
RX=24
BX=$(( 840 - 238 - 24 ))

# Revan comes on white. Flood-fill the white from the corners only, so light
# patches inside him stay opaque, then pull the edge in a pixel and soften it
# so no light fringe survives on the dark background.
convert "$REVAN" -alpha set -fuzz 14% -fill none \
        -draw "color 0,0 floodfill" -draw "color 334,0 floodfill" \
        -draw "color 0,596 floodfill" -draw "color 334,596 floodfill" \
        -channel A -morphology Erode Disk:1 -blur 0x0.7 +channel \
        -resize x470 "$TMP/revan.png"

# Bastila comes on black (really ~5% grey JPEG noise). Her alpha is her own
# brightness with a hard knee: the body is fully opaque, and only the saber's
# faint halo fades out.
convert "$BASTILA" -level 6%,100% -resize x470 "$TMP/bastila_c.png"
convert "$TMP/bastila_c.png" -colorspace gray -level 1%,7% -blur 0x0.8 "$TMP/balpha.png"
convert -size 238x470 xc:black -fill white -draw "rectangle 10,6 227,463" \
        -blur 0x6 "$TMP/bframe.png"
convert "$TMP/balpha.png" "$TMP/bframe.png" -compose multiply -composite "$TMP/balpha2.png"
convert "$TMP/bastila_c.png" "$TMP/balpha2.png" -alpha off -compose copy_opacity \
        -composite "$TMP/bastila.png"

# A soft glow behind each, cold for Revan, warm for Bastila's saber. Drawn on
# the full canvas and rolled into place: a blurred ellipse on a small canvas
# gets its edges sliced off and shows as a column.
glow 840 500 95 200 '#26304a' 70 0.8 "$TMP/rglow.png"
glow 840 500 95 200 '#4a3010' 70 0.8 "$TMP/bglow.png"
convert "$TMP/rglow.png" -roll +$(( RX + 132 - 420 ))+0 "$TMP/rglow2.png"
convert "$TMP/bglow.png" -roll +$(( BX + 119 - 420 ))+0 "$TMP/bglow2.png"
convert "$TMP/bg2.png" \
        "$TMP/rglow2.png" -compose screen -composite \
        "$TMP/bglow2.png" -compose screen -composite "$TMP/bg3.png"

# Vignette for depth, then the two of them on top of it.
convert -size 840x500 radial-gradient:white-'#5a5a5a' -resize 840x500\! "$TMP/vig.png"
convert "$TMP/bg3.png" "$TMP/vig.png" -compose multiply -composite \
        "$TMP/revan.png"   -gravity none -geometry +$RX+22 -compose over -composite \
        "$TMP/bastila.png" -gravity none -geometry +$BX+22 -compose over -composite \
        "$TMP/bg5.png"

convert "$TMP/bg5.png" \
        -alpha off "$TMP/bg_final.png"
png8 "$TMP/bg_final.png" "$OUT/livearea/contents/bg.png"

# ---------------------------------------------------------------- startup.png
# The gate. Same treatment, tighter crop, logo fills it.
convert -size 280x158 gradient:'#101a2e'-'#010206' "$TMP/g1.png"
stars 280 158 "$TMP/gstars.png"
convert "$TMP/g1.png" "$TMP/gstars.png" -compose screen -composite "$TMP/g2.png"
glow 280 158 78 44 '#7a5a1e' 26 0.6 "$TMP/gbloom.png"
convert "$TMP/g2.png" "$TMP/gbloom.png" -compose screen -composite "$TMP/g3.png"
convert "$LOGO" -resize 214x "$TMP/logo_g.png"
convert "$TMP/g3.png" "$TMP/logo_g.png" -gravity center -compose over -composite \
        -alpha off "$TMP/g_final.png"
png8 "$TMP/g_final.png" "$OUT/livearea/contents/startup.png"

# -------------------------------------------------------------------- icon0.png
# The bubble. The Android launcher icon is already composed for this size and
# carries its own gold rounded border with transparent corners -- masking it
# again just clips that border, so only rescale.
convert "$LAUNCH" -resize 128x128 "$TMP/i_final.png"
png8 "$TMP/i_final.png" "$OUT/icon0.png"

echo "wrote:"
identify -format '  %f  %wx%h  %b\n' \
  "$OUT/icon0.png" \
  "$OUT/livearea/contents/bg.png" \
  "$OUT/livearea/contents/startup.png"
