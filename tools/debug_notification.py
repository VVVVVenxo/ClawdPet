#!/usr/bin/env python3
"""调试: 把所有 Notification hook 的 stdin JSON 写到日志文件, 用于排查 matcher 类型"""
import sys, json, time, os

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notif_debug.log")

raw = ""
try:
    raw = sys.stdin.read()
except Exception:
    pass

with open(LOG, "a", encoding="utf-8") as f:
    f.write(f"\n--- {time.strftime('%H:%M:%S')} ---\n")
    f.write(f"argv: {sys.argv}\n")
    f.write(f"stdin: {raw[:2000]}\n")

sys.exit(0)
