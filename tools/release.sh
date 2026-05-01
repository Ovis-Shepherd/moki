#!/bin/bash
# Moki Firmware Release Helper
#
# Workflow:
#   1. Baut firmware.bin via PlatformIO
#   2. Liest aktuelle Version aus letztem GH-Release
#   3. Bumped Patch-Level (oder akzeptiert manuelle Version als Arg)
#   4. Erstellt GitHub-Release mit firmware.bin als Asset
#   5. /releases/latest/download/firmware.bin URL ist sofort live
#
# Voraussetzung:
#   - gh-cli installiert + authentifiziert (`gh auth login`)
#   - Repo hat origin remote auf GitHub
#
# Usage:
#   ./release.sh                # auto-bump patch
#   ./release.sh v0.2.0         # explizite Version
#   ./release.sh "fix LoRa SD-bus race"   # auto-bump + Release-Notes

set -euo pipefail
cd "$(dirname "$0")/.."

FW_BIN="firmware/.pio/build/t5_s3_pro/firmware.bin"

# Build firmware
echo "→ pio run"
(cd firmware && pio run) > /tmp/moki-build.log 2>&1 || {
  echo "✗ pio run failed:"
  tail -20 /tmp/moki-build.log
  exit 1
}
SIZE=$(wc -c < "$FW_BIN")
SHA=$(shasum -a 256 "$FW_BIN" | awk '{print $1}')
echo "✓ built: $FW_BIN ($SIZE bytes, sha=${SHA:0:16}...)"

# Determine version
ARG="${1:-}"
if [[ "$ARG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  TAG="$ARG"
  NOTES="${2:-Firmware $TAG}"
else
  # Auto-bump patch
  LAST=$(gh release list --limit 1 --json tagName --jq '.[0].tagName' 2>/dev/null || echo "")
  if [[ -z "$LAST" ]]; then
    TAG="v0.1.0"
  else
    if [[ "$LAST" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
      TAG="v${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$((${BASH_REMATCH[3]} + 1))"
    else
      echo "✗ couldn't parse last tag '$LAST', specify manually: ./release.sh vN.N.N"
      exit 1
    fi
  fi
  NOTES="${ARG:-Firmware $TAG}"
fi

echo "→ tag: $TAG"
echo "→ notes: $NOTES"

# Create release with firmware.bin attached
gh release create "$TAG" "$FW_BIN" --latest --notes "$NOTES" \
  --title "$TAG"

REPO=$(gh repo view --json owner,name --jq '"\(.owner.login)/\(.name)"')
echo ""
echo "✓ Release published: https://github.com/$REPO/releases/tag/$TAG"
echo ""
echo "Auf Moki via Serial:"
echo "  ota_url https://github.com/$REPO/releases/latest/download/firmware.bin"
echo "  ota_release"
echo ""
echo "(URL nur einmal setzen — bleibt in NVS, dann reicht 'ota_release')"
