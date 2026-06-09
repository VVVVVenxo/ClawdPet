#!/usr/bin/env bash
# ============================================================================
# tools/flash.sh - ClawdPet 一键烧录
#
# 解决的核心问题:
#   Claude Code hooks 会不断把 clawdpet_daemon 拉起来独占 COM 口,
#   导致 esptool 报 "port is busy"。本脚本在 upload 期间于后台
#   持续精准杀掉 clawdpet_daemon 进程, 保证串口空闲。
#
# 用法:
#   tools/flash.sh            # 自动探测 303A: 设备端口, 构建并烧录
#   tools/flash.sh COM4       # 指定端口
# ============================================================================
set -u

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIO="$HOME/.platformio/penv/Scripts/pio"
ENV_NAME="stick_s3"

cd "$PROJ_DIR"

# --- 探测端口 ---
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT="$("$PIO" device list --json-output 2>/dev/null \
    | "$HOME/.platformio/penv/Scripts/python" -c \
      'import sys,json;[print(d["port"]) or sys.exit(0) for d in json.load(sys.stdin) if "303A" in (d.get("hwid") or "").upper()]')"
fi
if [ -z "$PORT" ]; then
  echo "[flash] ERROR: 未找到 303A: 设备, 请手动指定: tools/flash.sh COMx" >&2
  exit 1
fi
echo "[flash] 目标端口: $PORT"

# --- 精准杀 clawdpet_daemon (不影响其他 python 进程) ---
kill_daemons() {
  powershell.exe -NoProfile -Command "
    Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" |
      Where-Object { \$_.CommandLine -match 'clawdpet_daemon' } |
      ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }
  " 2>/dev/null
}

# --- 后台清场: 每 0.5 秒精准清杀 daemon ---
( while true; do kill_daemons; sleep 0.5; done ) &
KILLER=$!
trap 'kill "$KILLER" 2>/dev/null; wait "$KILLER" 2>/dev/null' EXIT INT TERM

# 先杀一波确保串口释放
kill_daemons
sleep 2

echo "[flash] 后台清场已启动 (仅杀 clawdpet_daemon), 开始构建并烧录..."
"$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT"
RC=${PIPESTATUS[0]:-$?}

kill "$KILLER" 2>/dev/null
trap - EXIT INT TERM

if [ "$RC" -eq 0 ]; then
  echo "[flash] 烧录成功。daemon 会在下次 hook 触发时自动重启。"
else
  echo "[flash] 烧录失败 (rc=$RC)。"
  echo "        若反复失败, 手动进下载模式: 按住 BOOT + 点 RESET → 松 RESET → 松 BOOT" >&2
fi
exit "$RC"
