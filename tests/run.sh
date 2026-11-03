#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
pid=
cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; rm -rf "$tmp"; }
trap cleanup EXIT INT TERM

cat > "$tmp/service" <<X
#!/bin/sh
if [ "\$1" = check ]; then echo "Service SurfaceFlinger: found"; exit 0; fi
echo "\$*" >> "$tmp/service.log"
X

cat > "$tmp/logcat" <<'X'
#!/bin/sh
echo "setScreenEffect displayId=0 mode=0 value=0"
echo "PccData value 0.9, 0.8, 0.7"
sleep 0.3
echo "setScreenEffect displayId=0 mode=3 value=70"
echo "PccData value 0.5, 0.5, 0.5"
sleep 0.3
echo "setScreenEffect displayId=0 mode=3 value=0"
sleep 0.3
echo "PccData value 0.6, 0.5, 0.4"
sleep 30
X
chmod +x "$tmp/service" "$tmp/logcat"
: > "$tmp/service.log"

${CC:-gcc} -std=gnu11 -O2 -Wall -Wextra -pthread \
    -DSERVICE_BIN="\"$tmp/service\"" -DLOGCAT_BIN="\"$tmp/logcat\"" \
    -DLOCK_PATH="\"$tmp/lock\"" \
    -o "$tmp/relay_test" "$root/src/df_pcc_relay.c" -ldl

"$tmp/relay_test" --strength 50 -v > "$tmp/out.log" 2>&1 &
pid=$!

i=0
while ! grep -q "MATRIX SENT: 0.600000" "$tmp/out.log" && [ $i -lt 100 ]; do
    sleep 0.1; i=$((i + 1))
done
kill -TERM "$pid"
wait "$pid" || true
pid=

expect() {
    if ! grep -q -- "$1" "$tmp/service.log"; then
        echo "FAIL: missing matrix: $1"; cat "$tmp/service.log"; cat "$tmp/out.log"; exit 1
    fi
}

expect "f 0.900000 f 0 f 0 f 0 f 0 f 0.800000 f 0 f 0 f 0 f 0 f 0.700000 f 0 f 0 f 0 f 0 f 1"
expect "f 1.000000 f 0 f 0 f 0 f 0 f 0.777352 f 0 f 0 f 0 f 0 f 0.608307 f 0 f 0 f 0 f 0 f 1"
expect "f 0.500000 f 0 f 0 f 0 f 0 f 0.500000 f 0 f 0 f 0 f 0 f 0.500000 f 0 f 0 f 0 f 0 f 1"
expect "f 1.000000 f 0 f 0 f 0 f 0 f 1.000000 f 0 f 0 f 0 f 0 f 1.000000 f 0 f 0 f 0 f 0 f 1"
expect "f 0.600000 f 0 f 0 f 0 f 0 f 0.500000 f 0 f 0 f 0 f 0 f 0.400000 f 0 f 0 f 0 f 0 f 1"

grep -q "received signal 15" "$tmp/out.log" || { echo "FAIL: no clean shutdown"; cat "$tmp/out.log"; exit 1; }

echo "OK: matrices parsed correctly, clean shutdown"
