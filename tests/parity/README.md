# Backend parity

Checks that the Rust backend (`backend-rs/`) answers like `backend/server.mjs`: recorded game sessions are replayed
against both, and every difference is reported.

- `recorder.mjs` records a session (every HTTP exchange and WebSocket frame, with timing, plus the backend's
  inputs) from inside the backend's node process: it takes the public port and pipes bytes unchanged to server.mjs.
- `compare.mjs` replays sessions against two backends, each in its own temp build dir and config. It diffs responses,
  WebSocket frames and timing, `offline/state.json`, `unknown-routes.log`, `cloud/` and CLI exit codes, and exits
  1 on any difference.
- `synthetic/` is a committed corpus with made-up data: every route (canonical, mixed case, wrong method), MCP styles,
  the matchmaker, XMPP, odd paths, and BOM or broken JSON inputs. `gen-synthetic.mjs` re-records it.
- `corpus/` holds real sessions. It is git-ignored because it contains Epic data, so it lives only in the private
  `Radarnite/radarnite-launcher-tests` repo.
- `selftest.mjs` checks the harness itself: the recorder round trip, and 21 mutations of server.mjs that must each
  be caught.

## Run

```sh
tests/parity/run.sh                    # Node vs Rust (RN_RUST_BIN or backend-rs/out/radarnite-backend-linux-aarch64)
tests/parity/run.sh --node             # Node vs Node: 0 differences expected
tests/parity/run.sh path/to/corpus     # other corpora; --only <name>, --coverage, --json <file>: see compare.mjs --help
tests/parity/run.sh --selftest
```

Only volatile values are normalized:

- timestamps near now, and only in the exact `YYYY-MM-DDTHH:MM:SS.sssZ` form;
- tokens and random hex ids, aliased so that equal values stay equal;
- epoch milliseconds;
- matchmaker timers (±150 ms) and the 1 s `Queued` poll count (±1);
- the exception text in server.mjs's 500.

JSON key order is a note, not a failure, unless `--strict-order` is set.

## Record a real session (Windows PC)

1. Copy `recorder.mjs` and `wire.mjs` into one folder, e.g. `%LOCALAPPDATA%\rn-parity`.
2. Quit the launcher completely: an instance that is already running does not see the variables below.
3. In PowerShell, start the launcher (installed, or the dev tree) with the recorder loaded into its backend:

   ```powershell
   $d = "$env:LOCALAPPDATA\rn-parity"
   $env:RN_PARITY_RECORD = "$d\corpus"
   $env:NODE_OPTIONS = "--import=" + ([Uri]"$d\recorder.mjs").AbsoluteUri
   & "$env:LOCALAPPDATA\Programs\Radarnite Launcher\RadarniteLauncher.exe"
   ```

4. Play the build: lobby, locker and item shop with styles, a match, then exit. Each backend start writes one session
   to `corpus\<build>-<date>-<time>\`, made of `session.jsonl`, `inputs\` and `final\`. It is written as it goes, so
   killing the backend loses nothing. Record 3.6, 14.30 and 17.50.
5. Close that PowerShell window, so that `NODE_OPTIONS` is gone. Copy the session folders to `tests/parity/corpus/`
   on the dev box, or to the private repo, and run `tests/parity/run.sh tests/parity/corpus`.

To record without the launcher, run
`node recorder.mjs --out <dir> --name <session> backend/server.mjs --build <dir> --config <file> --port 3551`.

## Limits

This runs on Linux, but the target is Windows. Two behaviours differ by platform:

- the `readdir` order of `hotfix/*.ini` and `cloud/`;
- `path.basename` on backslashes.

The synthetic corpus avoids both.
