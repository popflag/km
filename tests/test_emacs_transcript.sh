#!/bin/sh
set -eu

if ! command -v emacs >/dev/null 2>&1; then
    echo "[INFO] GNU Emacs unavailable; transcript comparison skipped"
    exit 0
fi

LC_ALL=C ./build/test_transcript >build/km-transcript.txt
LC_ALL=C emacs -Q --batch -l tests/emacs_transcript.el \
    >build/emacs-transcript.txt
diff -u build/emacs-transcript.txt build/km-transcript.txt
