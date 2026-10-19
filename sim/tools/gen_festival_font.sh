#!/bin/sh
# Regenerate firmware/main/components/78__xiaozhi-fonts/src/font_festival_12.c: a 12 px, 1 bpp subset of
# Source Han Sans SC Regular (SIL OFL, the family the other text fonts come from) holding only the characters
# of the festival names in common/chinese_calendar.cc. Run it again whenever a festival name gets a new character.
#
#   sim/tools/gen_festival_font.sh /path/to/SourceHanSansSC-Regular.otf [size]
#
# Needs node (npx lv_font_conv). Get the font from https://github.com/adobe-fonts/source-han-sans (OTF/SimplifiedChinese).
set -eu
FONT="$1"
SIZE="${2:-12}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SYMBOLS="$(python3 -I - "$ROOT/firmware/main/common/chinese_calendar.cc" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
print("".join(sorted(set("".join(re.findall(r'"([^"\x00-\x7f]+)"', text))))), end="")
PY
)"
OUT="$ROOT/firmware/main/components/78__xiaozhi-fonts/src/font_festival_$SIZE.c"
npx --yes lv_font_conv --force-fast-kern-format --no-compress --no-prefilter --font "$FONT" --format lvgl \
  --lv-include lvgl.h --bpp 1 --size "$SIZE" --symbols "$SYMBOLS" -o "$OUT"
echo "wrote $OUT ($SYMBOLS)"
