# lmok serve — the REST server

`lmok serve` keeps one lmok process running and answers HTTP/JSON requests.
Think of it as a web application with every button and drop-down list
present, but no page: each action is a REST endpoint, and every list to choose
from (presets, languages, rule packs, settings) is an endpoint too. There is
no GUI and no login. It binds to `127.0.0.1` by default; anyone who can reach
the port can run lmok commands as the server's user, so keep it on loopback.

```sh
build/lmok serve                      # http://127.0.0.1:7700, preset "default"
build/lmok serve --port 0 --port-file build/port --preset interactive
build/lmok serve --presets examples/serve/presets.json --preset go-review
build/lmok serve --list-presets
```

| Option | Meaning |
| --- | --- |
| `--host ADDR` | IPv4 address to bind (default `127.0.0.1`; `localhost` accepted). |
| `--port N` | TCP port (default `7700`); `0` picks a free port. |
| `--port-file FILE` | Write the bound port to FILE once listening (for scripts). |
| `--preset NAME` | Settings to start with (default `default`). |
| `--presets FILE` | Add or override presets: `{"presets":[{"name","description","settings"}]}`. |
| `--idle-timeout S` | Override the starting preset's `idle_timeout_s`. |
| `--state-dir DIR` | Where uploads live (default: a per-process temp directory, removed on exit). |

The server stops on `POST /v1/shutdown` or once no request has arrived for
`idle_timeout_s` seconds (`0` disables the timeout), printing
`lmok serve: stopped (shutdown requested|idle timeout)` and exiting 0. Requests
are served one at a time, in arrival order.

## The buttons

`GET /` lists every endpoint with a one-line summary, so a client can discover
the whole surface. Bodies are JSON objects; responses are JSON except the raw
`/stdout` and `/stderr` downloads.

| Method and path | Action |
| --- | --- |
| `GET /v1/health` | Liveness, version, preset, uptime, idle countdown, counters. |
| `POST /v1/shutdown` | Stop after answering. |
| `GET /v1/settings` | Current settings and the preset they came from. |
| `PATCH /v1/settings` | Change the named settings (all-or-nothing validation). |
| `PUT /v1/settings` | Replace all settings: defaults overlaid with the body. |
| `POST /v1/settings/reset` | Back to the startup preset. |
| `GET /v1/settings/schema` | Every setting, its type, default and description. |
| `GET /v1/presets`, `GET /v1/presets/{name}` | The presets to choose from. |
| `POST /v1/presets/{name}` | Choose a preset (replaces every setting). |
| `GET /v1/languages`, `GET /v1/packs` | Selection lists for `lang` and `pack`. |
| `GET /v1/commands` | Command buttons and the fields each takes. |
| `POST /v1/run` | Any lmok command: `{"args":["parse","x.json"]}`. |
| `POST /v1/parse`, `/v1/query`, `/v1/lint`, `/v1/facts`, `/v1/metrics`, `/v1/dup`, `/v1/outline`, `/v1/rules` | Typed command buttons. |
| `POST /v1/lmcode` | Run compiled lmcode bytecode sent as base64. |
| `GET /v1/results`, `GET /v1/results/{id}` | Recent results, or one with output. |
| `GET /v1/results/{id}/stdout`, `/stderr` | Raw bytes of a result (exit code in `X-Lmok-Exit-Code`). |
| `DELETE /v1/results`, `DELETE /v1/results/{id}` | Forget results. |
| `GET /v1/cache`, `DELETE /v1/cache`, `DELETE /v1/cache/{key}` | Inspect or empty the cache. |

Typed buttons take their input as `"file"`, `"files"` or inline `"text"`
(with `"lang"` or a file `"name"` so the grammar can be chosen) and fall back
to the `lang`, `pack` and `metadata` settings. `/v1/query` needs `"pattern"`;
`/v1/dup` accepts `"tokens"`; `/v1/outline` accepts `"budget"`. Every command
button also accepts `"cache": false` and `"cwd"`.

`/v1/run` additionally accepts:

- `"files": {"name": {"text": "..."} | {"base64": "..."}}` — uploads; write
  `@{name}` in `args` to refer to the stored file.
- `"stdin"`, `"stdin_base64"` or `"stdin_from": "<result id>"` — feed a
  previous result's stdout into the next command (`lmok parse -`).
- `"invocation_manifest"` — an `LMCODE_INVOCATION_FILE` manifest; `@{name}`
  placeholders in it are replaced by the uploads' absolute paths.

A command result looks like this; `stdout`/`stderr` become
`stdout_base64`/`stderr_base64` when the bytes are not UTF-8:

```json
{"id": "r7", "command": ["lint", "rules/go-tier1.json", "x.go"], "exit_code": 0, "ok": true,
 "cached": false, "cacheable": true, "key": "40c7…", "duration_ms": 3,
 "created": "2026-11-02T16:00:27Z", "stdout": "…", "stderr": ""}
```

The exit code is the CLI's own (0 success, 2 usage, 3 load/run, 4 output),
reported with HTTP 200: the request worked even when the command failed.
Protocol problems are HTTP errors with a JSON body
`{"error":{"status","code","message"}}`: 400 bad body, 404 unknown endpoint,
preset or result, 405 wrong method (with `Allow`), 413 body over
`max_body_bytes`, 422 invalid setting, language or argument.

## Settings and presets

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `idle_timeout_s` | integer | 300 | Exit after this many quiet seconds; 0 = never. |
| `cache` | boolean | true | Reuse results while inputs are unchanged. |
| `cache_max_entries` | integer | 256 | LRU capacity. |
| `cache_lmcode` | boolean | false | Also cache `--lmcode` runs. |
| `results_max` | integer | 64 | Results kept addressable by id. |
| `lang` | string | "" | Default `--lang` for the typed buttons. |
| `pack` | string | "" | Default rule pack for `/v1/lint` and `/v1/rules`. |
| `metadata` | boolean | false | Add `--metadata` to lint and lmcode runs. |
| `cwd` | string | "" | Command working directory; "" = where the server started. |
| `max_body_bytes` | integer | 16 MiB | Largest accepted request body. |

Built-in presets: `default`, `interactive` (no idle timeout, long history),
`ci` (2 minute timeout, short history), `fresh` (no cache), `memo` (cache
lmcode runs too), `json`, `c`, `python` (language plus tier-1 pack) and
`secrets`. Choosing a preset replaces every setting with the defaults overlaid
by the preset; `PATCH` then adjusts single settings. Pack paths are relative
to `cwd`, so the pack presets expect the repository root.

## The intermediate result cache

Side-effect-free commands (`parse`, `query`, `facts`, `metrics`, `dup`,
`rules`, `test`, `outline` and `lint` without DB/fix/SARIF/baseline flags,
`check` without `--sarif`, and the schema/version flags) are cached. The key
covers the lmok version, working directory, arguments, stdin, the manifest,
the SHA-256 of every argument that names a file, and the size and mtime of
every file under an argument that names a directory. Editing an input
therefore misses the cache; nothing is served stale for unchanged arguments.
`--lmcode` programs can read files the key cannot see, so they are cached only
with `cache_lmcode` (the `memo` preset). Every run, cached or not, gets a new
result id; `"cached": true` marks a reused answer.

## lmok remote — the client

`lmok remote` sends one command to `$LMOK_SERVER` (default
`http://127.0.0.1:7700`) and replays the captured stdout, stderr and exit code,
so it behaves like running lmok locally:

```sh
export LMOK_SERVER=http://127.0.0.1:7700
lmok remote parse examples/data/sample.json
echo '[1]' | lmok remote parse --lang json -
lmok remote --api POST /v1/presets/json
lmok remote --api POST /v1/shutdown
```

It sends its working directory with each command, so relative paths resolve
as they would locally (the server must see the same filesystem). Programs
named by `--lmcode/--first/--last`, the bytecode of `check`, and stdin for a
`-` argument are uploaded instead. A `LMCODE_INVOCATION_FILE` manifest is
forwarded with its program paths rewritten to the uploads. That makes it a
drop-in host command for lmscript:

```lms
r = run_lmcode(p, {command:["lmok", "remote", "--lmcode", "$DSLFILE"]});
```

`--no-cache` bypasses the cache for one command, `--json` prints the whole
result object, and `--api METHOD PATH [BODY]` presses any button directly
(exit 0 for a 2xx answer, 1 otherwise). An unreachable server is exit 2.

## Examples and tests

- [examples/serve/text-template.lms](../examples/serve/text-template.lms) —
  render a template on the server (the lmcode sample's `text-template.lms`
  with `lmok remote` as its host command).
- [examples/serve/buttons.lms](../examples/serve/buttons.lms) — choose a
  preset, change settings, lint twice (the second is cached), read history.
- [examples/serve/triage.lms](../examples/serve/triage.lms) — an interactive
  triage: lint one file, locate the warning's function, search the folder for
  its callers, refine with a predicate query on each call site, widen the lint,
  and revisit from the cache. `tests/check-serve-triage.py` runs the same
  session and then fixes the file to prove stale cache entries are not reused.
- [examples/serve/rest-tour.sh](../examples/serve/rest-tour.sh) — a curl
  tour that starts its own server and presses each kind of button.
- [examples/serve/presets.json](../examples/serve/presets.json) — custom presets.

`tests/check-serve.py` checks the REST contract against a live server;
`tests/check-serve-examples.py` runs every `examples/*.lms` locally and again
through the server and requires identical output. The `serve` workflow runs
both, plus the tour, on Linux x64/arm64, macOS arm64/x64 and Windows x64;
`test.sh` (run by `ci.yml` on the same five platforms) includes the REST contract.
