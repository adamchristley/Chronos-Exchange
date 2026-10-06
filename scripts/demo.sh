#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j >/dev/null
rm -f "$ROOT/chronos.events"

"$BUILD/chronos_subscriber" 9100 & SUB=$!
"$BUILD/chronos_server" --log "$ROOT/chronos.events" & SRV=$!
trap 'kill $SRV $SUB 2>/dev/null || true' EXIT
sleep 0.4
"$BUILD/chronos_client" 127.0.0.1 9000 BUY AAPL 10025 10 3
"$BUILD/chronos_client" 127.0.0.1 9000 SELL AAPL 10000 5 4
sleep 0.5
kill -INT $SRV
wait $SRV || true
kill $SUB 2>/dev/null || true
"$BUILD/chronos_replay" "$ROOT/chronos.events"
