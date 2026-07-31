#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读取板端 NVS 平衡杠校准，检查 L/C/R 是否单调，并给出 INVERT 建议。

用法（与板同一网段）:
  py -3 camera/tools/dump_servo_cal.py --url http://192.168.x.x
  py -3 camera/tools/dump_servo_cal.py --url http://192.168.4.1
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


def main() -> int:
    ap = argparse.ArgumentParser(description="Dump /api/servo/calib and judge L/C/R order")
    ap.add_argument("--url", default="http://192.168.4.1", help="device base URL")
    args = ap.parse_args()
    base = args.url.rstrip("/")
    api = f"{base}/api/servo/calib"

    try:
        with urllib.request.urlopen(api, timeout=5) as resp:
            data = json.load(resp)
    except urllib.error.URLError as exc:
        print(f"FAIL fetch {api}: {exc}", file=sys.stderr)
        return 1

    print(f"channel={data.get('channel')} nvs_present={data.get('nvs_present')}")
    print(f"current pulse_us={data.get('pulse_us')} angle_deg={data.get('angle_deg')}")

    nvs = data.get("nvs") or {}
    check = data.get("check") or {}
    print(f"check.order={check.get('order')}")
    print(f"check.hint={check.get('hint')}")
    print("--- NVS poses (eff_us = pulse + offset) ---")
    for name in ("left", "center", "right"):
        p = nvs.get(name)
        if not p:
            print(f"  {name}: (missing)")
            continue
        print(
            f"  {name}: valid={p.get('valid')} angle={p.get('angle_deg')} "
            f"offset={p.get('offset_deg')} pulse={p.get('pulse_us')} eff_us={p.get('eff_us')}"
        )

    order = check.get("order")
    if order == "non_monotonic":
        print("\n结论: 数据不统一，请重标左/中/右（物理左端→Left，中→Center，右端→Right）。")
        return 2
    if order == "empty" or order == "incomplete":
        print("\n结论: NVS 不完整，请烧校准固件并写入三姿态。")
        return 2
    if order == "L<C<R":
        print("\n结论: 顺序正常。识别固件先 INVERT=0；若球偏左却更左再改 INVERT=1。")
        return 0
    if order == "L>C>R":
        print("\n结论: 脉宽递减。优先重标成 L<C<R；临时可试 INVERT=1。")
        return 0
    print("\n结论: 见 hint。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
