#!/usr/bin/env python3
# ============================================================================
# noapet_send.py - 手动发送状态 (调试 / 异常上报)
#
# 用法:
#   python noapet_send.py working        # 通过守护进程发 (推荐, 多窗口安全)
#   python noapet_send.py error
#   python noapet_send.py done --direct  # 绕过守护直接开串口 (守护没跑时)
#   python noapet_send.py rename <name> [--sid SESSION_ID]
#                                         # 给某终端(默认本会话)改显示名
#
# 状态: idle | working | done | all_done | error
# ============================================================================

import argparse
import json
import os
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 8787
VALID = {"idle", "working", "done", "all_done", "error"}


def _send(payload):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.0)
    s.connect((HOST, PORT))
    s.sendall(json.dumps(payload).encode())
    try:
        s.recv(64)
    except Exception:
        pass
    s.close()


def via_daemon(state):
    # 复用 daemon 的 "manual" 事件: session_id 字段传目标状态
    _send({"event": "manual", "session_id": state})


def rename(name, sid):
    # rename 事件: 只改 sid 的终端显示名, 不动状态
    _send({"event": "rename", "session_id": sid, "name": name})


def direct(state):
    import serial
    from serial.tools import list_ports
    port = None
    for p in list_ports.comports():
        if p.vid == 0x303A:
            port = p.device
            break
    if not port:
        print("ERROR: no 303A: device", file=sys.stderr)
        sys.exit(1)
    with serial.Serial(port, 115200, timeout=0.3) as ser:
        time.sleep(0.3)
        ser.write((state + "\n").encode())
        ser.flush()
    print(f"sent {state} directly to {port}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", help="state (%s) 或 'rename'" % "|".join(sorted(VALID)))
    ap.add_argument("name", nargs="?", help="rename 时的新显示名")
    ap.add_argument("--sid", help="rename 目标 session_id (默认取 $CLAUDE_CODE_SESSION_ID)")
    ap.add_argument("--direct", action="store_true",
                    help="bypass daemon, open serial directly")
    args = ap.parse_args()

    if args.action == "rename":
        if not args.name:
            print("ERROR: rename 需要新名字: rename <name>", file=sys.stderr)
            sys.exit(2)
        sid = args.sid or os.environ.get("CLAUDE_CODE_SESSION_ID", "")
        if not sid:
            print("ERROR: 无 session_id; 用 --sid 指定", file=sys.stderr)
            sys.exit(2)
        try:
            rename(args.name, sid)
            print(f"renamed {sid[:8]} -> {args.name}")
        except (ConnectionRefusedError, OSError):
            print("daemon not running", file=sys.stderr)
            sys.exit(1)
        return

    if args.action not in VALID:
        print(f"ERROR: 未知动作 '{args.action}'", file=sys.stderr)
        sys.exit(2)

    if args.direct:
        direct(args.action)
    else:
        try:
            via_daemon(args.action)
            print(f"sent {args.action} via daemon")
        except (ConnectionRefusedError, OSError):
            print("daemon not running; retry with --direct", file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
