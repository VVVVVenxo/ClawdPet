#!/usr/bin/env bash
# ============================================================================
# tools/flash.sh - ClawdPet 一键烧录
#
# 解决的核心问题:
#   其他 Claude Code 窗口的 hook 会不断把 clawdpet_daemon 拉起来独占 COM 口,
#   导致 esptool 打开串口时报 "port is busy"。本脚本在 upload 期间于后台
#   持续清场 (杀掉任何重生的 daemon), 保证 esptool 开口那一刻串口空闲。
#
# 用法:
#   tools/flash.sh            # 自动探测 303A: 设备端口, 构建并烧录
#   tools/flash.sh COM4       # 指定端口
#
# 依赖: PlatformIO (penv 自带), 设备已连接。
# ============================================================================
set -u

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIO="$HOME/.platformio/penv/Scripts/pio"
ENV_NAME="stick_s3"

cd "$PROJ_DIR"

# --- 探测端口 (优先命令行参数, 否则找 303A: Espressif 设备) ---
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT="$("$PIO" device list --json-output 2>/dev/null \
    | "$HOME/.platformio/penv/Scripts/python" -c \
      'import sys,json;[print(d["port"]) or sys.exit(0) for d in json.load(sys.stdin) if "303A" in (d.get("hwid") or "").upper()]')"
fi
if [ -z "$PORT" ]; then
  echo "[flash] ERROR: 未找到 303A: 设备, 请确认已连接, 或手动指定: tools/flash.sh COMx" >&2
  exit 1
fi
echo "[flash] 目标端口: $PORT"

# --- 后台清场: 持续杀掉重生的 clawdpet_daemon, 让串口保持空闲 ---
kill_daemons() {
  for pid in $(wmic process where "name='python.exe'" get ProcessId,CommandLine 2>/dev/null \
               | grep -i clawdpet_daemon | grep -oE '[0-9]+ *$' | tr -d ' '); do
    taskkill //PID "$pid" //F >/dev/null 2>&1
  done
}

( while true; do kill_daemons; sleep 0.2; done ) &
KILLER=$!
# 确保脚本退出时关掉后台清场进程
trap 'kill "$KILLER" 2>/dev/null' EXIT INT TERM

echo "[flash] 后台清场已启动 (PID $KILLER), 开始构建并烧录..."
"$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT"
RC=${PIPESTATUS[0]:-$?}

kill "$KILLER" 2>/dev/null
trap - EXIT INT TERM

if [ "$RC" -eq 0 ]; then
  echo "[flash] ✓ 烧录成功。daemon 会在下次 Claude Code hook 触发时自动重启。"
else
  echo "[flash] ✗ 烧录失败 (rc=$RC)。若反复失败, 手动进下载模式: 按住 BOOT + 点 RESET → 松 RESET → 松 BOOT, 再重试。" >&2
fi
exit "$RC"
