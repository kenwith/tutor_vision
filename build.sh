#!/bin/bash
# Build the tutor vision agent with the staged static libcurl dependencies.
set -euo pipefail

STAGE=/tmp/opencode/stage

gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -o agent agent.c \
    -I"$STAGE/include" \
    "$STAGE/lib/libcurl.a" \
    "$STAGE/lib64/libssl.a" \
    "$STAGE/lib64/libcrypto.a" \
    "$STAGE/lib/libz.a" \
    -ldl -lpthread

echo "built: $(ls -la agent | awk '{print $5}') bytes"
