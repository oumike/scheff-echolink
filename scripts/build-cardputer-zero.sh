#!/usr/bin/env bash
# Builds the M5Stack CardputerZero (aarch64 Linux) target in linux/docker's
# Debian trixie arm64 container:
#   scripts/build-cardputer-zero.sh          linux/build/cp0/scheff-echolink
#   scripts/build-cardputer-zero.sh --deb    also dist/scheff-echolink-cardputer-zero-v<VERSION>_arm64.deb
# Options:
#   --clean   start from an empty build directory
# On an x86 machine Docker needs arm64 emulation (binfmt/QEMU); Apple Silicon
# and arm64 Linux run it natively.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../linux"

CLEAN=false
DEB=false
usage() { sed -n '2,9s/^# \{0,1\}//p' "$SCRIPT_DIR/$(basename "$0")"; }
for arg in "$@"; do
	case "$arg" in
		--clean) CLEAN=true ;;
		--deb) DEB=true ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $arg"; usage; exit 1 ;;
	esac
done

BUILD_DIR=build/cp0
$CLEAN && rm -rf "$BUILD_DIR"

IMAGE=scheff-echolink-cp0-build
docker build --quiet --platform linux/arm64 -t "$IMAGE" docker >/dev/null
STEPS="cmake --preset cp0 && cmake --build $BUILD_DIR"
$DEB && STEPS="$STEPS && cd $BUILD_DIR && rm -f ./*.deb && cpack -G DEB"
docker run --rm --platform linux/arm64 \
	--user "$(id -u):$(id -g)" -e HOME=/tmp \
	-v "$PWD/..:/work" -w /work/linux \
	"$IMAGE" sh -c "$STEPS"

echo "Built linux/$BUILD_DIR/scheff-echolink"
if $DEB; then
	mkdir -p ../dist
	for deb in "$BUILD_DIR"/scheff-echolink-cardputer-zero-*_arm64.deb; do
		cp "$deb" ../dist/
		echo "Packaged dist/$(basename "$deb")"
	done
fi
