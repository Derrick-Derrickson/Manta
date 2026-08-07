#!/usr/bin/env python3
"""Validates emitted artifacts against the published JSON Schemas (spec 15.4).

Skips cleanly when jsonschema is not installed, so the suite stays runnable on a
machine with only a compiler.
"""
import glob
import json
import sys

try:
    import jsonschema
except ImportError:
    print("jsonschema not installed; skipping schema validation")
    sys.exit(0)

schema_dir, work_dir = sys.argv[1], sys.argv[2]
object_schema = json.load(open(f"{schema_dir}/mantaO.schema.json"))
netlist_schema = json.load(open(f"{schema_dir}/mantaNets.schema.json"))

failures = 0
checked = 0
for path in sorted(glob.glob(f"{work_dir}/build/*.mantaO")):
    try:
        jsonschema.validate(json.load(open(path)), object_schema)
        checked += 1
    except jsonschema.ValidationError as error:
        failures += 1
        print(f"{path}: {error}", file=sys.stderr)

for path in sorted(glob.glob(f"{work_dir}/*.mantaNets")):
    try:
        jsonschema.validate(json.load(open(path)), netlist_schema)
        checked += 1
    except jsonschema.ValidationError as error:
        failures += 1
        print(f"{path}: {error}", file=sys.stderr)

if checked == 0:
    print("no artifacts found to validate", file=sys.stderr)
    sys.exit(1)
print(f"{checked} artifact(s) validate against the published schemas")
sys.exit(1 if failures else 0)
