#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("case_json")
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()

    case = json.loads(Path(args.case_json).read_text())
    files = case["files"]

    lines = []

    def add(key, value):
        if value:
            lines.append(f"{key} {value}")

    add("device", files.get("1:0:0"))
    add("qualifier", files.get("6:0:0"))
    add("other_config", files.get("7:0:0"))
    add("bos", files.get("15:0:0"))

    for index in range(16):
        add(f"string{index}", files.get(f"3:{index}:0"))

    for index in range(16):
        add(f"config{index}", files.get(f"2:{index}:0"))

    for address, endpoint in case.get("endpoints", {}).items():
        ep_number = int(address, 16) & 0x0F
        direction = endpoint["direction"]
        transfer = {
            "control": 0,
            "isochronous": 1,
            "bulk": 2,
            "interrupt": 3,
        }[endpoint["type"]]

        attributes = transfer
        if transfer == 1:
            attributes |= 0x04

        lines.append(
            "endpoint "
            f"{ep_number} "
            f"{direction} "
            f"{attributes} "
            f"{endpoint['max_packet_size']} "
            f"0"
        )

    Path(args.output).write_text("\n".join(lines) + "\n")
    print(f"Written: {args.output}")


if __name__ == "__main__":
    main()