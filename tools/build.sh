#!/usr/bin/env bash
# Build EduDxe.efi and PlatformDiag.efi with EDK2 (GCC). Usage: tools/build.sh /path/to/edk2 [DEBUG|RELEASE]
set -euo pipefail
EDK2=$(cd "${1:?path to an edk2 checkout}" && pwd)
TARGET=${2:-DEBUG}
REPO=$(cd "$(dirname "$0")/.." && pwd)
export WORKSPACE="$REPO/.edk2-workspace"
export PACKAGES_PATH="$EDK2:$REPO"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
mkdir -p "$WORKSPACE"
[ -x "$EDK2/BaseTools/Source/C/bin/GenFw" ] || make -C "$EDK2/BaseTools" -j"$(nproc 2>/dev/null || echo 2)"
set +u
# shellcheck disable=SC1091
. "$EDK2/edksetup.sh" BaseTools > /dev/null
set -u
build -a X64 -t GCC5 -b "$TARGET" -p HwDiagPkg/HwDiagPkg.dsc -n "$(nproc 2>/dev/null || echo 2)" -q
OUT="$WORKSPACE/Build/HwDiagPkg/${TARGET}_GCC5/X64"
mkdir -p "$REPO/build"
cp "$OUT/EduDxe.efi" "$OUT/PlatformDiag.efi" "$REPO/build/"
ls -l "$REPO/build"
