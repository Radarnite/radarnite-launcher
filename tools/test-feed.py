#!/usr/bin/env python3
"""The test channel's feed (native/core/update.h): feed.json + the installers it names, in one folder served as-is
by a static HTTP server on a private network. `just publish-test <version>` runs `publish`.

  test-feed.py publish <dir> <installer.exe> <version> [notes]   copy the installer, then swap feed.json in
  test-feed.py current <dir>                                     the feed's version ("" = none)
  test-feed.py newer <a> <b>                                     exit 0 when a > b (the launcher's semver order)

publish refuses a version that is not newer than the feed's current one: the launcher only installs a newer build, so
an older one would be published and never arrive. Pre-release identifiers compare as the launcher does
(update::compare, Semantic Versioning 2.0.0): numbers by value, words in ASCII order, so t120 > t106 but t99 > t106.
The two newest installers are kept, older ones are deleted.
"""
import functools, hashlib, json, os, re, shutil, sys, tempfile

KEEP = 2


def parse(v):
    v = v.strip()
    if v[:1] in "vV":
        v = v[1:]
    v = v.split("+", 1)[0]
    core, _, pre = v.partition("-")
    parts = core.split(".")
    if len(parts) not in (2, 3) or not all(p.isdigit() for p in parts):
        raise ValueError(f"not a version: {v!r}")
    nums = [int(p) for p in parts] + [0] * (3 - len(parts))
    return nums, pre.split(".") if pre else []


def ident_cmp(a, b):
    na, nb = a.isdigit(), b.isdigit()
    if na != nb:
        return -1 if na else 1
    if na:
        a, b = int(a), int(b)
    return (a > b) - (a < b)


def compare(a, b):
    (x, xp), (y, yp) = parse(a), parse(b)
    if x != y:
        return -1 if x < y else 1
    if not xp or not yp:
        return 0 if not xp and not yp else (1 if not xp else -1)
    for i, j in zip(xp, yp):
        c = ident_cmp(i, j)
        if c:
            return c
    return (len(xp) > len(yp)) - (len(xp) < len(yp))


def current(d):
    try:
        with open(os.path.join(d, "feed.json"), encoding="utf-8") as f:
            return json.load(f).get("version", "")
    except FileNotFoundError:
        return ""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def publish(d, installer, version, notes=""):
    parse(version)
    name = os.path.basename(installer)
    if not re.fullmatch(r"RadarniteLauncherSetup-[0-9A-Za-z.+-]+\.exe", name):
        sys.exit(f"test-feed: unexpected installer name {name}")
    cur = current(d)
    if cur and compare(version, cur) <= 0:
        sys.exit(f"test-feed: {version} is not newer than the feed's {cur}; the launcher would never install it. "
                 f"Publish a higher version (e.g. {cur}.1 sorts after {cur}).")
    os.makedirs(d, exist_ok=True)
    digest = sha256(installer)
    part = os.path.join(d, name + ".part")
    shutil.copyfile(installer, part)
    if sha256(part) != digest:
        sys.exit("test-feed: the copy's sha256 differs")
    os.replace(part, os.path.join(d, name))
    feed = {"version": version, "installer": name, "sha256": digest, "notes": notes}
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".feed-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(feed, f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o644)
    os.replace(tmp, os.path.join(d, "feed.json"))   # the new build is offered only once its installer is complete
    # prune: the newest KEEP installers by version stay (the current one always)
    setups = [f for f in os.listdir(d) if f.startswith("RadarniteLauncherSetup-") and f.endswith(".exe")]

    def ver(f):
        return f[len("RadarniteLauncherSetup-"):-4]

    setups.sort(key=functools.cmp_to_key(lambda a, b: compare(ver(a), ver(b))), reverse=True)
    for old in setups[KEEP:]:
        if old != name:
            os.remove(os.path.join(d, old))
    print(json.dumps(feed))


def main(a):
    if len(a) >= 4 and a[0] == "publish":
        publish(a[1], a[2], a[3], a[4] if len(a) > 4 else "")
    elif len(a) == 2 and a[0] == "current":
        print(current(a[1]))
    elif len(a) == 3 and a[0] == "newer":
        sys.exit(0 if compare(a[1], a[2]) > 0 else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
