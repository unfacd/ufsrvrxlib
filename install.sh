#!/usr/bin/env bash
# ufsrvrxlib — one-command installer
#
# Usage:
#   wget -qO- https://<server>/install.sh | bash
#   RELEASE_URL=https://<server>/path/to/ufsrvrxlib.deb bash install.sh
#
# Installs the ufsrvrxlib Debian package (static library + headers + pkg-config
# + CMake config + man page) built by the CPack "DEB" generator.
set -euo pipefail

PKG_NAME="ufsrvrxlib"
RELEASE_URL="${RELEASE_URL:-https://tig.unfacd.io/ufsrvrxlib/releases/latest/download/ufsrvrxlib_amd64.deb}"

echo "=== ufsrvrxlib Installer ==="
echo ""

# ── Preflight ──────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (use sudo)."
    exit 1
fi

for cmd in curl dpkg; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: $cmd is required but not installed."
        exit 1
    fi
done

# ── Download ───────────────────────────────────────────
echo "[1/3] Downloading package..."
curl -fsSL "$RELEASE_URL" -o "/tmp/${PKG_NAME}.deb"
echo "       Downloaded $(du -h "/tmp/${PKG_NAME}.deb" | cut -f1)"

# ── Install ────────────────────────────────────────────
echo "[2/3] Installing..."
if ! dpkg -i "/tmp/${PKG_NAME}.deb"; then
    echo "       Resolving dependencies..."
    apt-get update -qq
    apt-get install -f -y
fi
rm -f "/tmp/${PKG_NAME}.deb"

# ── Verify ─────────────────────────────────────────────
echo "[3/3] Verifying..."
if [ -f /usr/lib/libufsrvrxlib.a ] && [ -d /usr/include/ufsrvrxlib ]; then
    VERSION="$(pkg-config --modversion ufsrvrxlib 2>/dev/null || echo '?')"
    echo "       ${PKG_NAME} ${VERSION} installed."
else
    echo "       WARNING: library or headers not found after install."
fi

echo ""
echo "=== Done ==="
echo "Compile:   pkg-config --cflags --libs ufsrvrxlib"
echo "Docs:      man 7 ufsrvrxlib"
echo "Headers:   /usr/include/ufsrvrxlib/"
