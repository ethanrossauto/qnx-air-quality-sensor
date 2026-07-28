#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Ethan Ross
"""Generate a QPP (QNX POSIX Publish Subscribe) Signal Catalog for the
air-quality demo from a vss-tools JSON export.

The QPP signal service (github.com/qnx/qnx-posix-publish-subscribe, qpp/catalog.c)
parses a JSON object whose single root key is a VSS branch ("Vehicle") and reads,
per node: "type" (branch|sensor|actuator|attribute), "children" (branches),
"datatype" and optional "default" (leaves). Everything else (description, unit,
min/max) is carried as node metadata. A vss-tools JSON export therefore feeds it
directly; this script prunes a full export down to the signals the demo uses so
the catalog stays reviewable.

Usage:
  python3 make_qpp_catalog.py <vss_export.json> <output_catalog.json>

The input export should be produced from VSS master plus the air-quality overlay
in ../proposal/overlay/ (see ../VALIDATION.md for the exact command).
"""

import json
import sys

# Paths kept in the demo catalog. Dot-separated, relative to the "Vehicle" root.
# "SUBTREE" keeps the whole branch.
KEEP = {
    "Cabin.AirQuality": "SUBTREE",           # the proposed signals (this project)
    "Cabin.HVAC.IsRecirculationActive": "LEAF",   # actuator the CO2/filter alerts would drive
    "Cabin.HVAC.AmbientAirTemperature": "LEAF",   # existing VSS home for temp_c_x10
    "Exterior.AirQuality": "SUBTREE",        # outside-air side of Recirculate-Smart
    "Exterior.Humidity": "LEAF",             # existing VSS home for rh_x10 (exterior only;
                                             # VSS master has no cabin-humidity signal)
}


def prune(node: dict, prefix: str) -> dict | None:
    """Return a pruned copy of a vss-tools JSON branch node, or None if empty."""
    kept_children = {}
    for name, child in node.get("children", {}).items():
        path = f"{prefix}{name}"
        rule = KEEP.get(path)
        if rule == "SUBTREE":
            kept_children[name] = child
        elif rule == "LEAF" and child.get("type") != "branch":
            kept_children[name] = child
        elif child.get("type") == "branch":
            sub = prune(child, path + ".")
            if sub is not None:
                kept_children[name] = sub
    if not kept_children:
        return None
    out = {k: v for k, v in node.items() if k != "children"}
    out["children"] = kept_children
    return out


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    with open(sys.argv[1]) as f:
        full = json.load(f)
    vehicle = full["Vehicle"]
    pruned = prune(vehicle, "")
    if pruned is None:
        print("Nothing matched the KEEP list -- wrong input file?", file=sys.stderr)
        return 1
    catalog = {"Vehicle": pruned}

    def count(n):
        return sum(count(c) if c.get("type") == "branch" else 1
                   for c in n.get("children", {}).values())

    with open(sys.argv[2], "w") as f:
        json.dump(catalog, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"Wrote {sys.argv[2]}: {count(pruned)} signals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
