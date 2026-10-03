#!/usr/bin/env bash
set -euo pipefail

REPO_URL="https://unfacd.github.io/ufsrvrxlib"
KEY_URL="$REPO_URL/apt-unfacd.pub.asc"
KEYRING="/etc/apt/keyrings/unfacd-apt.asc"
SOURCES="/etc/apt/sources.list.d/unfacd.sources"
PACKAGE="${PACKAGE:-ufsrvrxlib-dev}"
SUITE="${SUITE:-stable}"

EXPECTED_SUBKEY="D808EE4147CC41952C9CBA7B6178496040D97E06"

step() { printf '[%s] %s\n' "$1" "$2"; }
die()  { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

echo "=== ufsrvrxlib-dev installer ==="
echo ""

[ "$(id -u)" -eq 0 ] || die "must run as root — pipe into 'sudo bash', not 'bash'"

command -v apt-get >/dev/null 2>&1 || die "apt-get not found — this is not a Debian/Ubuntu system"

ARCH="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
[ "$ARCH" = "amd64" ] || die "this repository publishes amd64 only; this host is '$ARCH'"

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "       host: ${PRETTY_NAME:-unknown} ($ARCH)"
fi

for cmd in curl gpg; do
    command -v "$cmd" >/dev/null 2>&1 || die "$cmd is required but not installed"
done

step 1/4 "Fetching the repository signing key"
KEY_TMP="$(mktemp)"
trap 'rm -f "$KEY_TMP"' EXIT

curl -fsSL --max-time 30 "$KEY_URL" -o "$KEY_TMP" \
    || die "could not fetch $KEY_URL"

if ! gpg --show-keys --with-colons "$KEY_TMP" 2>/dev/null \
     | awk -F: '/^fpr:/{print $10}' | grep -qx "$EXPECTED_SUBKEY"; then
    echo "       key file fingerprints:" >&2
    gpg --show-keys --with-colons "$KEY_TMP" 2>/dev/null | awk -F: '/^fpr:/{print "         "$10}' >&2
    die "signing subkey $EXPECTED_SUBKEY is NOT in the downloaded key — refusing"
fi
echo "       verified signing subkey $EXPECTED_SUBKEY"

step 2/4 "Installing the keyring"
install -d -m 0755 /etc/apt/keyrings
install -m 0644 "$KEY_TMP" "$KEYRING"
echo "       $KEYRING"

step 3/4 "Adding the repository"
cat > "$SOURCES" <<EOF
Types: deb
URIs: $REPO_URL
Suites: $SUITE
Components: main
Signed-By: $KEYRING
EOF
echo "       $SOURCES"

step 4/4 "Installing $PACKAGE"
DEBIAN_FRONTEND=noninteractive apt-get update -qq </dev/null \
    || die "apt-get update failed — is the repository reachable?"
DEBIAN_FRONTEND=noninteractive apt-get install -y "$PACKAGE" </dev/null \
    || die "install failed"

INSTALLED="$(dpkg-query -W -f='${Version}' "$PACKAGE" 2>/dev/null || true)"
echo ""

if [ -z "$INSTALLED" ]; then
    echo "WARNING: $PACKAGE is not registered with dpkg after install." >&2
    exit 1
fi

echo "=== Done — $PACKAGE $INSTALLED ==="
echo ""
echo "Headers:    /usr/include/ufsrvrxlib/"
echo "pkg-config: pkg-config --cflags --libs ufsrvrxlib"
echo "CMake:      find_package(ufsrvrxlib REQUIRED)  ->  target_link_libraries(app PRIVATE ufsrvrxlib::ufsrvrxlib)"
echo "Man page:   man 7 ufsrvrxlib"
echo ""
echo "Repository: $REPO_URL"
