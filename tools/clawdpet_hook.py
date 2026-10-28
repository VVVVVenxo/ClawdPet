#!/usr/bin/env python3
# ============================================================================
# clawdpet_hook.py - 极轻量 hook 桥接 (被 Claude Code hooks 调用)
#
# 行为:
#   - 从 stdin 读取 Claude Code 传入的 hook JSON (含 session_id)
#   - 取事件名 (argv[1] 优先, 否则用 JSON 的 hook_event_name)
#   - 把 {event, session_id, cwd} 发给本地守护进程
#   - 若守护进程没在跑, 自动后台拉起它, 再重试一次
#
# 设计要点: 必须快速返回、绝不阻塞 Claude Code, 任何异常都静默退出 0。
#
# Claude Code settings.json 里这样调用:
#   "command": "python G:/xueluoCode/ClawdPet/tools/clawdpet_hook.py"
# 事件名自动从 stdin 的 hook_event_name 读取, 无需写死。
#
# 特例: Notification(idle_prompt) 无法从 stdin JSON 可靠区分通知种类,
#       故在 settings.json 用 matcher=idle_prompt 过滤, 并显式传 argv:
#   "command": "python .../clawdpet_hook.py NotificationIdle"
# 这样 daemon 只在收到明确的 NotificationIdle 事件时回落空闲。
# ============================================================================

import json
import os
import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
PORT = 8787
DAEMON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "clawdpet_daemon.py")
_HEX = set("0123456789abcdefABCDEF")


def read_peacock_color(cwd):
    """读 <cwd>/.vscode/settings.json 里的 peacock.color (#rrggbb), 返回 6 位小写 hex 或 ""。"""
    if not cwd:
        return ""
    try:
        with open(os.path.join(cwd, ".vscode", "settings.json"),
                  "r", encoding="utf-8") as f:
            cfg = json.load(f)
        c = (cfg.get("peacock.color") or "").lstrip("#").strip()
        if len(c) == 6 and all(ch in _HEX for ch in c):
            return c.lower()
    except Exception:
        pass
    return ""


def send(payload, timeout=0.5):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    s.sendall(json.dumps(payload).encode())
    try:
        s.recv(64)
    except Exception:
        pass
    s.close()


def spawn_daemon():
    flags = 0
    if os.name == "nt":
        # DETACHED_PROCESS | CREATE_NO_WINDOW, 让守护脱离当前 terminal
        flags = 0x00000008 | 0x08000000
    subprocess.Popen([sys.executable, DAEMON],
                     stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL,
                     creationflags=flags,
                     close_fds=True)


def detect_agent(data):
    """自动识别事件来自哪个 AI 工具: claude / codex / pi"""
    explicit = (data.get("agent") or os.environ.get("CLAWDPET_AGENT") or "").strip().lower()
    if explicit in ("claude", "codex", "pi"):
        return explicit
    transcript = str(data.get("transcript_path") or "").replace("\\", "/").lower()
    if (os.environ.get("CODEX_THREAD_ID")
            or os.environ.get("CODEX_SANDBOX")
            or os.environ.get("CODEX_HOME")
            or "/.codex/" in transcript):
        return "codex"
    if (os.environ.get("PI_SESSION_ID")
            or os.environ.get("PI_AGENT_DIR")
            or "/.pi/" in transcript):
        return "pi"
    return "claude"


def extract_tool(data):
    """从 PreToolUse / PostToolUse 负载中提取简短工具名"""
    raw = (data.get("tool_name")
           or data.get("tool")
           or (data.get("tool_input") or {}).get("tool")
           or "")
    if not isinstance(raw, str):
        return ""
    raw = raw.strip()
    # MCP 工具名通常形如 mcp__server__tool, 简化显示
    if raw.startswith("mcp__"):
        parts = raw.split("__")
        return ("mcp:" + parts[-1])[:10] if len(parts) > 1 else "mcp"
    return raw[:10]


FLASH_LOCK = os.path.expanduser("~/.clawdpet_flashing.lock")


def main():
    if os.path.exists(FLASH_LOCK):
        try:
            if time.time() - os.path.getmtime(FLASH_LOCK) < 120:
                sys.exit(0)
        except Exception:
            pass
    # 读 stdin JSON (hook 负载); 读不到就给空 dict
    raw = ""
    try:
        raw = sys.stdin.buffer.read().decode("utf-8", errors="replace")
    except Exception:
        try:
            raw = sys.stdin.read()
        except Exception:
            pass
    data = {}
    if raw.strip():
        try:
            data = json.loads(raw)
        except Exception:
            data = {}

    event = sys.argv[1] if len(sys.argv) > 1 else data.get("hook_event_name", "")
    cwd = data.get("cwd", "") or os.getcwd()
    agent = detect_agent(data)
    sid = (data.get("session_id")
           or data.get("thread_id")
           or os.environ.get("CODEX_THREAD_ID")
           or os.environ.get("CLAUDE_CODE_SESSION_ID")
           or "")
    # 终端显示名: CLAWDPET_NAME 优先, 兼容旧变量名, 否则回落 cwd 目录名
    name = (os.environ.get("CLAWDPET_NAME")
            or os.environ.get("NOAPET_NAME")
            or os.environ.get("NONOPET_NAME")
            or "").strip()
    if not name and cwd:
        name = os.path.basename(cwd.rstrip("/\\"))
    color = read_peacock_color(cwd)
    tool = extract_tool(data)
    payload = {
        "event": event,
        "session_id": sid,
        "cwd": cwd,
        "name": name,
        "color": color,
        "agent": agent,
        "tool": tool,
    }

    try:
        send(payload)
    except (ConnectionRefusedError, OSError):
        # 守护没起 -> 拉起来再试一次
        try:
            spawn_daemon()
            time.sleep(1.2)
            send(payload)
        except Exception:
            pass
    except Exception:
        pass

    sys.exit(0)  # 永远不阻塞 Claude Code


if __name__ == "__main__":
    main()
