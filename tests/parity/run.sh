#!/usr/bin/env bash
# Parity of the Rust backend with backend/server.mjs (see tests/parity/README.md).
#   tests/parity/run.sh [compare.mjs options] [corpus dirs]   Node vs Rust ($RN_RUST_BIN, else backend-rs/out/radarnite-backend-linux-aarch64)
#   tests/parity/run.sh --node [options] [corpus dirs]        Node vs Node: the harness alone (must give 0 differences)
# Corpus dirs default to tests/parity/synthetic and tests/parity/corpus (real sessions, not in git).
# Light work (one backend at a time on 127.0.0.1): fine on the Linux box.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
command -v node >/dev/null || { echo "node not found" >&2; exit 2; }
case "${1:-}" in
  --node) shift; exec node "$here/compare.mjs" --a node --b node "$@" ;;
esac
bin=${RN_RUST_BIN:-$root/backend-rs/out/radarnite-backend-linux-aarch64}
if [ ! -x "$bin" ]; then
  echo "Rust backend not found: $bin" >&2
  echo "build it (backend-rs) or point RN_RUST_BIN at it; 'tests/parity/run.sh --node' checks the harness alone" >&2
  exit 2
fi
exec node "$here/compare.mjs" --a node --b "rust:$bin" "$@"
