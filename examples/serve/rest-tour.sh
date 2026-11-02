#!/usr/bin/env bash
# A curl tour of `lmok serve`: start, press each kind of button, shut down.
# Run from the repository root after building: examples/serve/rest-tour.sh
# Needs: build/lmok (or LMOK=path), curl.
set -euo pipefail
LMOK="${LMOK:-build/lmok}"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/lmok-tour-XXXXXX")"
trap 'rm -rf "$STATE"' EXIT
"$LMOK" serve --port 0 --port-file "$STATE/port" --preset ci --presets examples/serve/presets.json >"$STATE/server.log" 2>&1 &
for _ in $(seq 100); do [ -s "$STATE/port" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$STATE/port")"
api() { curl -fsS -X "$1" -H 'Content-Type: application/json' ${3:+--data "$3"} "$BASE$2"; }
expect() { grep -- "$1" >/dev/null || { echo "tour: expected $1" >&2; exit 1; }; }

echo "== what can I press?";            api GET / | expect '"path": "/v1/lint"'
echo "== which presets?";                api GET /v1/presets | expect '"go-review"'
echo "== choose go-review";              api POST /v1/presets/go-review | expect '"pack": "rules/go-tier1.json"'
echo "== lint with the preset's pack";   api POST /v1/lint '{"file":"tests/fixtures/go-tier1-violations.go"}' | expect '"cached": false'
echo "== same request: cached";          api POST /v1/lint '{"file":"tests/fixtures/go-tier1-violations.go"}' | expect '"cached": true'
echo "== parse inline text";             api POST /v1/parse '{"text":"[1, {\"a\": true}]","lang":"json"}' | expect '(document (array'
echo "== any command via /v1/run";       api POST /v1/run '{"args":["--version"]}' | expect '"stdout": "lmok '
echo "== feed a result into the next";   id="$(api POST /v1/run '{"args":["--lmcode-schema","--json"]}' | sed -n 's/^  "id": "\(r[0-9]*\)",/\1/p')"
                                         api POST /v1/run "{\"args\":[\"parse\",\"--lang\",\"json\",\"-\"],\"stdin_from\":\"$id\"}" | expect '"ok": true'
echo "== raw stdout of that result";     api GET "/v1/results/$id/stdout" | expect '"name": "lmok"'
echo "== change a setting";              api PATCH /v1/settings '{"cache":false}' | expect '"cache": false'
echo "== rejected setting";              ! api PATCH /v1/settings '{"cache":"maybe"}' 2>/dev/null
echo "== cache statistics";              api GET /v1/cache | expect '"hits": 1'
echo "== back to the startup preset";    api POST /v1/settings/reset | expect '"preset": "ci"'
echo "== the CLI client";                LMOK_SERVER="$BASE" "$LMOK" remote parse examples/data/sample.json | expect '(document (object'
echo "== shut down";                     api POST /v1/shutdown | expect '"stopping": true'
wait
grep -q 'stopped (shutdown requested)' "$STATE/server.log"
echo "tour complete"
