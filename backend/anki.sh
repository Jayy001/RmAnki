#!/bin/sh
ANKI_DIR="${0%/*}"
cd "${ANKI_DIR}" || exit 1

export LD_LIBRARY_PATH="${ANKI_DIR}/libs"
export QT_QPA_PLATFORM=offscreen
export QT_QPA_PLATFORM_PLUGIN_PATH="${ANKI_DIR}/platforms"

exec ./entry
