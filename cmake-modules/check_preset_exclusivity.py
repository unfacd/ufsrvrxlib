#!/usr/bin/env python3
"""Preset guardrail: fail if ANY CMake configure preset enables more than one of
UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN} (they are mutually exclusive).

Resolves preset `include` chains and `inherits` so violations introduced through
inheritance are caught too. Shared, byte-identical across every ufsrv repo.

    python3 check_preset_exclusivity.py [CMakePresets.json]

Exit 0 = all presets valid; exit 1 = at least one preset violates.
"""
import json
import os
import sys

SANITIZERS = ("UFSRV_BUILD_WITH_ASAN", "UFSRV_BUILD_WITH_MSAN", "UFSRV_BUILD_WITH_TSAN")


def load_presets(path, seen):
    """Return {name: preset} merged across this file and everything it includes."""
    path = os.path.realpath(path)
    if path in seen or not os.path.exists(path):
        return {}
    seen.add(path)
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    presets = {}
    for inc in data.get("include", []):
        inc_path = inc if os.path.isabs(inc) else os.path.join(os.path.dirname(path), inc)
        presets.update(load_presets(inc_path, seen))
    for preset in data.get("configurePresets", []):
        name = preset.get("name")
        if name:
            presets[name] = preset
    return presets


def resolved_cache(name, presets, stack):
    """Effective cacheVariables for a preset, applying its inherits chain."""
    preset = presets.get(name)
    if preset is None or name in stack:
        return {}
    stack = stack | {name}
    inherits = preset.get("inherits", [])
    if isinstance(inherits, str):
        inherits = [inherits]
    merged = {}
    # CMake: earlier entries in `inherits` win, so apply them last.
    for parent in reversed(inherits):
        merged.update(resolved_cache(parent, presets, stack))
    for key, val in (preset.get("cacheVariables") or {}).items():
        merged[key] = val.get("value") if isinstance(val, dict) else val
    return merged


def is_on(value):
    return str(value).strip().upper() in ("ON", "TRUE", "1", "YES", "Y")


def main(argv):
    root = argv[1] if len(argv) > 1 else "CMakePresets.json"
    if not os.path.exists(root):
        return 0  # nothing to validate
    presets = load_presets(root, set())
    violations = []
    for name in sorted(presets):
        cache = resolved_cache(name, presets, set())
        enabled = [s for s in SANITIZERS if is_on(cache.get(s, "OFF"))]
        if len(enabled) > 1:
            violations.append((name, enabled))
    if violations:
        sys.stderr.write("preset(s) enable mutually-exclusive sanitizers:\n")
        for name, enabled in violations:
            sys.stderr.write("  preset '%s' -> %s\n" % (name, ", ".join(enabled)))
        sys.stderr.write("Enable at most ONE of UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN} per preset.\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
