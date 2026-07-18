#!/usr/bin/env bash
# End-to-end wiring check: browser -> Studio (Vite) -> /api proxy -> aero-runtime daemon -> back.
# Starts the daemon + the Vite dev server, then drives the same requests the Studio's api.ts client
# makes (deploy / status / list / undeploy) THROUGH the Vite proxy — proving the full loop works with
# no CORS (the browser only ever hits the same-origin /api, Vite forwards server-side).
#
# Usage: studio/scripts/e2e.sh   (from anywhere; paths are resolved from this script)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUDIO="$(dirname "$HERE")"
ROOT="$(dirname "$STUDIO")"
API_PORT="${API_PORT:-8090}"
WEB_PORT="${WEB_PORT:-5174}"
DAEMON="$ROOT/build/aero-runtime"
APP="$ROOT/examples/hello_flow.json"

fail() { echo "E2E FAIL: $*"; exit 1; }
[ -x "$DAEMON" ] || fail "daemon not built at $DAEMON (run: cmake --build build)"
[ -f "$APP" ] || fail "missing $APP"

daemon_pid=""; web_pid=""
cleanup() {
  [ -n "$web_pid" ] && kill "$web_pid" 2>/dev/null
  [ -n "$daemon_pid" ] && kill "$daemon_pid" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

echo "== starting daemon on :$API_PORT =="
taskset -c 0-3 "$DAEMON" --port "$API_PORT" >/tmp/aero-e2e-daemon.log 2>&1 &
daemon_pid=$!
for i in $(seq 1 60); do curl -fsS "http://127.0.0.1:$API_PORT/health" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fsS "http://127.0.0.1:$API_PORT/health" >/dev/null 2>&1 || fail "daemon did not become ready"
echo "daemon ready (pid $daemon_pid)"

echo "== starting Vite dev server on :$WEB_PORT (proxy /api -> :$API_PORT) =="
( cd "$STUDIO" && VITE_API_URL="http://127.0.0.1:$API_PORT" \
    npm run dev -- --port "$WEB_PORT" --strictPort --host 127.0.0.1 >/tmp/aero-e2e-web.log 2>&1 ) &
web_pid=$!
for i in $(seq 1 100); do curl -fsS "http://127.0.0.1:$WEB_PORT/api/health" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fsS "http://127.0.0.1:$WEB_PORT/api/health" >/dev/null 2>&1 || fail "Studio proxy did not become ready (see /tmp/aero-e2e-web.log)"
echo "Studio dev server ready (pid $web_pid)"

BASE="http://127.0.0.1:$WEB_PORT/api"   # exactly what the browser's api.ts uses (base '/api')

echo "== 1) health through the proxy =="
[ "$(curl -fsS "$BASE/health")" = "ok" ] || fail "health via proxy != ok"

echo "== 2) deploy hello_flow through the proxy (POST /api/apps) =="
dep="$(curl -fsS -X POST "$BASE/apps" -H 'Content-Type: application/json' --data-binary @"$APP")"
echo "  deploy -> $dep"
echo "$dep" | grep -q '"deployed":true' || fail "deploy did not report deployed"

echo "== 3) status through the proxy (GET /api/status) =="
st="$(curl -fsS "$BASE/status")"
echo "  status -> $st"
echo "$st" | grep -q '"name":"hello_flow"' || fail "status missing hello_flow"
echo "$st" | grep -q '"frames_processed":100' || fail "flow did not process 100 frames"

echo "== 4) list apps (GET /api/apps) =="
curl -fsS "$BASE/apps" | grep -q 'hello_flow' || fail "app list missing hello_flow"

echo "== 5) undeploy (DELETE /api/apps/hello_flow) =="
curl -fsS -X DELETE "$BASE/apps/hello_flow" | grep -q 'undeployed' || fail "undeploy failed"

echo
echo "E2E OK — browser -> Studio -> /api proxy -> daemon verified end-to-end"
