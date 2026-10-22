#!/usr/bin/env python3
import argparse, json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("case_json"); p.add_argument("-o", "--output", required=True)
a = p.parse_args()
case = json.loads(Path(a.case_json).read_text())
files = case["files"]
lines = []
def add(k, v):
    if v: lines.append(f"{k} {v}")
add("device", files.get("1:0:0"))
add("qualifier", files.get("6:0:0"))
add("other_config", files.get("7:0:0"))
add("bos", files.get("15:0:0"))
for i in range(16): add(f"string{i}", files.get(f"3:{i}:0"))
for i in range(16): add(f"config{i}", files.get(f"2:{i}:0"))
for address, endpoint in case.get("endpoints", {}).items():
    number = int(address, 16) & 0x0F
    transfer = {"control":0,"isochronous":1,"bulk":2,"interrupt":3}[endpoint["type"]]
    attrs = transfer | (0x04 if transfer == 1 else 0)
    lines.append(f"endpoint {number} {endpoint['direction']} {attrs} {endpoint['max_packet_size']} 0")
Path(a.output).write_text("\n".join(lines)+"\n")
print(f"Written: {a.output}")
