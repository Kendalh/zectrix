#!/usr/bin/env bash
# Publish a built merged-binary.bin to the Alibaba Cloud nginx host and print the download link.
#
#   firmware/scripts/upload_firmware.sh [path/to/merged-binary.bin]
#
# Result: <REMOTE_DIR>/firmware/merged-binary_<YYYYMMDD-HHMM>.bin (+ .sha256), served at <URL_BASE>/<name>.
# The timestamp is the file's mtime (when merge-bin produced it) in UTC. Existing files are never overwritten.
#
# Host, key and directory are NOT stored in this repo. They are read from the environment, or else from
# the aifin project's sync script (the same server, see CLAUDE.md):
#   REMOTE_HOST  e.g. user@host        SSH_KEY  private key        REMOTE_DIR  nginx root for /fin
#   URL_BASE     public URL of the firmware directory (default: the /fin URL printed by that script + /firmware)
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AIFIN_SCRIPT="${AIFIN_SCRIPT:-$HOME/workspace/aifin/sync_reports_to_cloud.sh}"

from_aifin() {  # from_aifin VAR -> value of VAR="..." in the aifin script
  [[ -f "$AIFIN_SCRIPT" ]] || return 0
  sed -n "s/^$1=\"\(.*\)\"/\1/p" "$AIFIN_SCRIPT" | head -1 | sed "s|\$HOME|$HOME|"
}
REMOTE_HOST="${REMOTE_HOST:-$(from_aifin REMOTE_HOST)}"
SSH_KEY="${SSH_KEY:-$(from_aifin SSH_KEY)}"
REMOTE_DIR="${REMOTE_DIR:-$(from_aifin REMOTE_DIR)}"
if [[ -z "${URL_BASE:-}" && -f "$AIFIN_SCRIPT" ]]; then
  fin_url="$(grep -o 'https\?://[^"]*' "$AIFIN_SCRIPT" | head -1 || true)"
  [[ -n "$fin_url" ]] && URL_BASE="${fin_url%/}/firmware"
fi
for v in REMOTE_HOST SSH_KEY REMOTE_DIR URL_BASE; do
  [[ -n "${!v:-}" ]] || { echo "[ERROR] $v is not set and could not be read from $AIFIN_SCRIPT" >&2; exit 1; }
done

BIN="${1:-$(ls -1t "$FIRMWARE_DIR"/releases/v*/merged-binary.bin 2>/dev/null | head -1)}"
[[ -f "$BIN" ]] || { echo "[ERROR] no merged-binary.bin found (build first: firmware/scripts/build_local.sh)" >&2; exit 1; }

STAMP="$(TZ=UTC date -r "$BIN" +%Y%m%d-%H%M)"  # UTC, so it does not depend on the machine time zone
NAME="merged-binary_${STAMP}.bin"
LOCAL_SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"
SSH_OPTS="-i $SSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
DEST="$REMOTE_DIR/firmware"

# Never overwrite: a link that was handed out must keep pointing at the same bytes.
if ssh $SSH_OPTS "$REMOTE_HOST" "test -e '$DEST/$NAME'"; then
  echo "[ERROR] $DEST/$NAME already exists on the server; rebuild or touch the file to get a new timestamp" >&2
  exit 1
fi

ssh $SSH_OPTS "$REMOTE_HOST" "mkdir -p '$DEST'"
rsync -t -e "ssh $SSH_OPTS" "$BIN" "$REMOTE_HOST:$DEST/$NAME"
echo "$LOCAL_SHA  $NAME" | ssh $SSH_OPTS "$REMOTE_HOST" "cat > '$DEST/$NAME.sha256'"
ssh $SSH_OPTS "$REMOTE_HOST" "chown nginx:nginx '$DEST' '$DEST/$NAME' '$DEST/$NAME.sha256' && chmod 755 '$DEST' && chmod 644 '$DEST/$NAME' '$DEST/$NAME.sha256'"

# Verify on the server itself. The dev machine's network blocks the public domain, so checking the public
# URL from here is not possible; this still goes through nginx (loopback + Host header) without bypassing anything.
HOST_HEADER="$(echo "$URL_BASE" | sed 's|^[a-z]*://||; s|/.*||')"
URL_PATH="$(echo "$URL_BASE" | sed 's|^[a-z]*://[^/]*||')"
REMOTE_CHECK="$(ssh $SSH_OPTS "$REMOTE_HOST" "curl -sk -o /tmp/fw_upload_check.bin -w '%{http_code}' -H 'Host: $HOST_HEADER' 'https://127.0.0.1$URL_PATH/$NAME'; echo -n ' '; sha256sum /tmp/fw_upload_check.bin | cut -d' ' -f1; rm -f /tmp/fw_upload_check.bin")"
CODE="${REMOTE_CHECK%% *}"; REMOTE_SHA="${REMOTE_CHECK##* }"
if [[ "$CODE" != "200" || "$REMOTE_SHA" != "$LOCAL_SHA" ]]; then
  echo "[ERROR] verification through nginx failed: http=$CODE sha=$REMOTE_SHA (expected $LOCAL_SHA)" >&2
  exit 1
fi

echo "[OK] uploaded and verified through nginx (http 200, sha256 matches)"
echo "Link:   $URL_BASE/$NAME"
echo "SHA256: $LOCAL_SHA"
