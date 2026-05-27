#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build-deploy.sh — cross-compile RmAnki and deploy to reMarkable Paper Pro
#
# Prerequisites:
#   - reMarkable Yocto toolchain at ~/Tools/remarkable-toolchain/
#   - CMake >= 3.18, rsync, ssh
#   - Device connected via USB at root@10.11.99.1
#
# Usage:
#   ./build-deploy.sh
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TC_ROOT="${HOME}/Tools/remarkable-toolchain"
TC_ENV="${TC_ROOT}/environment-setup-cortexa53-crypto-remarkable-linux"
TC_SYSROOT="${TC_ROOT}/sysroots/cortexa53-crypto-remarkable-linux"

BINARY="${BUILD_DIR}/entry"
MANIFEST="${SCRIPT_DIR}/external.manifest.json"

REMOTE_HOST="root@10.11.99.1"
REMOTE_DIR="/home/root/xovi/exthome/appload/anki"

# ---------------------------------------------------------------------------
# 1. Source toolchain
# ---------------------------------------------------------------------------
if [[ ! -f "${TC_ENV}" ]]; then
    echo "ERROR: Toolchain not found at: ${TC_ENV}" >&2
    exit 1
fi
# shellcheck source=/dev/null
source "${TC_ENV}"

# ---------------------------------------------------------------------------
# 2. Configure
# ---------------------------------------------------------------------------
cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/toolchain-aarch64-remarkable.cmake" \
    -DCMAKE_BUILD_TYPE=Release

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
echo ""
echo "Build complete: ${BINARY}"

# ---------------------------------------------------------------------------
# 4. Collect shared libraries from sysroot
# ---------------------------------------------------------------------------
STAGING="${BUILD_DIR}/deploy-staging"
LIBS_DIR="${STAGING}/backend/libs"
PLAT_DIR="${STAGING}/backend/platforms"
rm -rf "${STAGING}"
mkdir -p "${LIBS_DIR}" "${PLAT_DIR}"

SKIP_PATTERN="^(libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libgcc_s\.so|ld-linux)"

collect_libs() {
    local name
    name="$(basename "$1")"
    [[ -f "${LIBS_DIR}/${name}" ]] && return
    [[ "${name}" =~ ${SKIP_PATTERN} ]] && return

    local src
    src="$(find "${TC_SYSROOT}/usr/lib" "${TC_SYSROOT}/lib" \
               -maxdepth 2 -name "${name}" 2>/dev/null | head -1)"
    if [[ -z "${src}" ]]; then
        echo "  WARNING: ${name} not found in sysroot, skipping" >&2
        return
    fi

    echo "  + ${name}"
    cp -L "${src}" "${LIBS_DIR}/${name}"

    local dep
    while IFS= read -r dep; do
        [[ -z "${dep}" ]] && continue
        collect_libs "${dep}"
    done < <(readelf -d "${src}" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/')
}

echo ""
echo "Collecting shared libraries..."
while IFS= read -r dep; do
    [[ -z "${dep}" ]] && continue
    collect_libs "${dep}"
done < <(readelf -d "${BINARY}" | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/')

OFFSCREEN="$(find "${TC_SYSROOT}" -name "libqoffscreen.so" 2>/dev/null | head -1)"
if [[ -n "${OFFSCREEN}" ]]; then
    echo "  + platforms/libqoffscreen.so"
    cp -L "${OFFSCREEN}" "${PLAT_DIR}/libqoffscreen.so"
    while IFS= read -r dep; do
        [[ -z "${dep}" ]] && continue
        collect_libs "${dep}"
    done < <(readelf -d "${OFFSCREEN}" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/')
fi

cp "${BINARY}"                     "${STAGING}/backend/entry"
cp "${SCRIPT_DIR}/backend/anki.sh" "${STAGING}/backend/anki.sh"
cp "${MANIFEST}"                   "${STAGING}/external.manifest.json"

# ---------------------------------------------------------------------------
# 5. Deploy
# ---------------------------------------------------------------------------
echo ""
echo "Deploying to ${REMOTE_HOST}:${REMOTE_DIR} ..."

ssh "${REMOTE_HOST}" "mkdir -p '${REMOTE_DIR}/backend/libs' '${REMOTE_DIR}/backend/platforms'"
rsync -az --progress "${STAGING}/" "${REMOTE_HOST}:${REMOTE_DIR}/"
ssh "${REMOTE_HOST}" "chmod +x '${REMOTE_DIR}/backend/entry' '${REMOTE_DIR}/backend/anki.sh'"

echo ""
echo "Done. Launch Anki from AppLoad."
