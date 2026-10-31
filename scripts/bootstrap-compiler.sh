#!/usr/bin/env bash
# Build the lmscript compiler workspace (lmctl-mono) at the SHA pinned in
# compiler.lock and print the resulting lmscript binary path on stdout.
#
# Why this exists: fixture compilation imports the compiler library
# (dist/dsl.js) next to the compiler binary, upstream removed the public
# bytecode-export CLI route, and the published npm CLI package does not ship
# the library. The only working compiler for the full gate is a source build.
#
# Builds are cached per SHA under ${LMOK_COMPILER_CACHE:-$XDG_CACHE_HOME|~/.cache}/lmok/compiler/.
# All build noise goes to stderr; stdout carries ONLY the binary path.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

sha="$(sed -n 's/^\([0-9a-f]\{40\}\).*/\1/p' compiler.lock | head -1)"
[ -n "$sha" ] || { echo "error: compiler.lock pins no 40-char sha" >&2; exit 2; }

cache="${LMOK_COMPILER_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/lmok/compiler}"
dest="$cache/$sha"
if [ ! -x "$dest/lmscript/bin/lmscript" ]; then
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/lmok-compiler-XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT
  echo "cloning lmctl-mono@$sha ..." >&2
  mkdir -p "$tmp/mono"
  git -C "$tmp/mono" init -q
  git -C "$tmp/mono" remote add origin https://github.com/lmctlhq/lmctl-mono.git
  git -C "$tmp/mono" fetch -q --depth 1 origin "$sha" >&2 2>&1 ||
    git -C "$tmp/mono" fetch -q origin >&2   # fallback: full history
  git -C "$tmp/mono" checkout -q FETCH_HEAD || { echo "error: compiler sha $sha not fetchable; bump compiler.lock" >&2; exit 2; }
  (cd "$tmp/mono" && npm ci --ignore-scripts --no-audit --no-fund >&2 && npm run build >&2)
  [ -x "$tmp/mono/lmscript/bin/lmscript" ] || { echo "error: mono build produced no lmscript binary" >&2; exit 2; }
  mkdir -p "$cache"
  rm -rf "${dest:?}.partial.$$"
  mv "$tmp/mono" "$dest.partial.$$"
  # A stale/incomplete $dest (interrupted earlier build) must be replaced, not
  # descended into: `mv a b` with an existing directory b moves a INSIDE b and
  # the published path stays broken. A complete $dest is another clone's win.
  [ -x "$dest/lmscript/bin/lmscript" ] || rm -rf "${dest:?}"
  mv "$dest.partial.$$" "$dest" 2>/dev/null || rm -rf "${dest:?}.partial.$$"   # lost a race; winner's copy is fine
  [ -x "$dest/lmscript/bin/lmscript" ] || { echo "error: compiler publish failed: $dest has no lmscript binary" >&2; exit 2; }
  rm -rf "$tmp"; trap - EXIT
fi
[ -x "$dest/lmscript/bin/lmscript" ] || { echo "error: compiler cache at $dest is incomplete; remove it and retry" >&2; exit 2; }
printf '%s\n' "$dest/lmscript/bin/lmscript"
