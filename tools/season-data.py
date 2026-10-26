#!/usr/bin/env python3
"""season-data.py - backend/seasons/season<N>.json from a build's season asset (just season-data-pc).

  tools/season-data.py <cosmetics-gen --season dump> <the build's offline/cosmetics.json> <out.json>

In: the CUE4Parse JSON of AthenaSeason<N> (battle pass reward schedules per level, offer ids) and of the data tables
its XP curves point to. Out, for backend/progress.mjs:
  { "season", "levelXp": [XP from level i+1 to i+2], "free": {level: [{templateId, quantity}]}, "paid": {...},
    "battlePassOfferIds": [...], "levelOfferIds": [...] }
Rewards are named as the game's profile names them ("AthenaCharacter:cid_..."): cosmetics through the build's own
cosmetics list, V-bucks as Currency:MtxGiveaway, anything else "<asset class or folder>:<asset name>" (kept, ignored by
the backend).
"""
import json
import os
import re
import sys


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def walk(node):
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)


def asset_name(ref):
    """'/Game/Athena/Items/Cosmetics/Characters/CID_A.CID_A' (or a dict holding it) -> 'cid_a'"""
    if isinstance(ref, dict):
        ref = ref.get("AssetPathName") or ref.get("ObjectPath") or ref.get("ObjectName") or ""
    ref = str(ref)
    m = re.search(r"([A-Za-z0-9_]+)(?:'|\.[A-Za-z0-9_]+)?\s*'?$", ref.split(".")[-1] if "." in ref else ref)
    return (m.group(1) if m else ref).lower()


def main():
    dump, cosmetics_path, out_path = sys.argv[1:4]
    d = load(dump)
    by_name = {}
    for it in load(cosmetics_path).get("items", []):
        by_name[it["templateId"].split(":", 1)[1].lower()] = it["templateId"]

    season = None
    for key, val in d.items():
        if key in ("SeasonXpCurve", "BookXpCurve", "SeasonXpOnlyExtendedCurve"):
            continue
        for ex in val if isinstance(val, list) else []:
            if ex.get("Type") == "AthenaSeasonItemDefinition":
                season = ex
    if not season:
        sys.exit("no AthenaSeasonItemDefinition in the dump")
    props = season.get("Properties", season)

    def template(reward):
        tid = reward.get("TemplateId") or ""
        if tid:
            return tid
        ref = reward.get("ItemDefinition") or {}
        path = ref.get("AssetPathName", "") if isinstance(ref, dict) else str(ref)
        name = asset_name(path)
        if name in by_name:
            return by_name[name]
        if "mtx" in name.lower() or "currency" in path.lower():
            return "Currency:MtxGiveaway"
        folder = path.rsplit("/", 2)[-2] if path.count("/") >= 2 else "Item"
        return f"{folder}:{name}"

    def schedule(name):
        out = {}
        levels = (props.get(name) or {}).get("Levels") or []
        for i, level in enumerate(levels):
            rewards = [{"templateId": template(r), "quantity": r.get("Quantity", 1) or 1} for r in level.get("Rewards") or []]
            if rewards:
                out[str(i)] = rewards   # Levels[i] = the rewards of reaching level i (index 0: level 0, unused)
        return out

    # XP per level from the season XP curve: rows with the level and the XP it takes to the next (names vary by season)
    level_xp = []
    curve = d.get("SeasonXpCurve")
    rows = []
    for node in walk(curve or {}):
        if isinstance(node.get("Rows"), dict):
            rows = list(node["Rows"].values())
            break
    for r in rows:
        nums = {k: v for k, v in r.items() if isinstance(v, (int, float))}
        lvl = nums.get("Level")
        xp = next((nums[k] for k in ("XpToNextLevel", "XPToNextLevel", "XpPerLevel") if k in nums), None)
        if lvl is not None and xp is not None:
            level_xp.append((int(lvl), int(xp)))
    level_xp = [xp for _, xp in sorted(level_xp)]

    res = {
        "season": props.get("SeasonNumber"),
        "source": season.get("Name"),
        "levelXp": level_xp,
        "free": schedule("BookXpScheduleFree"),
        "paid": schedule("BookXpSchedulePaid"),
        "battlePassOfferIds": [x for x in (props.get("BattlePassOfferIds") or [props.get("BattlePassOfferId")]) if x],
        "levelOfferIds": [x for x in (props.get("BattlePassLevelOfferIDs") or [props.get("BattlePassLevelOfferID")]) if x],
    }
    unknown = sorted({r["templateId"] for s in ("free", "paid") for lv in res[s].values() for r in lv if not r["templateId"].startswith(("Athena", "Currency"))})
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
        f.write("\n")
    print(f"season {res['season']}: {len(level_xp)} XP levels, {len(res['free'])} free and {len(res['paid'])} paid reward levels, "
          f"offers {res['battlePassOfferIds']} / {res['levelOfferIds']}; other rewards: {', '.join(unknown[:12]) or 'none'}")


if __name__ == "__main__":
    main()
