#!/usr/bin/env python3
# ============================================================================
# clawdpet_daemon.py - 常驻守护进程
#
# 职责:
#   - 独占串口 (Windows 下 COM 口同一时刻只能被一个进程打开)
#   - 监听 localhost TCP, 接收各 Claude Code session 上报的 hook 事件
#   - 把每个 session_id 当作一个 task, 复用 TaskStateManager 聚合多窗口状态
#
# 聚合规则 (由 TaskStateManager 实现):
#   - 任一 session 在跑            -> 设备 working
#   - 某 session 完成但还有别的在跑 -> 发 done(瞬时), 随后回到 working
#   - 最后一个 session 完成         -> all_done, 空闲 N 秒后回落 idle
#   - 手动/异常                     -> error
#
# 依赖: pyserial  (pip install pyserial)
# 运行: python clawdpet_daemon.py           # 自动探测 303A: 设备
#       python clawdpet_daemon.py --port COM4
# ============================================================================

import argparse
import json
import os
import socket
import sys
import threading
import time

import serial
from serial.tools import list_ports

# 同目录导入聚合逻辑
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from task_state_manager import TaskStateManager, IDLE, DONE, ALL_DONE, CHOOSING  # noqa: E402,F401

HOST = "127.0.0.1"
PORT = 8787                 # 本地 TCP 端口 (hook <-> daemon)
BAUD = 115200
ESP_VID = 0x303A           # Espressif USB VID (StickS3)

DONE_PULSE_SEC = 2.5       # done 提示停留时间, 之后回到聚合状态
IDLE_AFTER_SEC = 15.0      # all_done 庆祝后自动归档并回落 idle
RECONNECT_POLL_SEC = 1.0   # 串口断开后, 轮询设备重新出现的间隔
STALE_SESSION_SEC = 90.0   # session 超过此时间无新事件则自动回落 idle
WRITE_TIMEOUT_SEC = 2.0    # 写串口超时 (锁屏挂起 USB 时防止 write 永久阻塞)
MAX_HISTORY = 8            # 最多保留的完成历史记录条数

AGENT_CODE_MAP = {
    "claude": "C",
    "codex": "X",
    "pi": "P",
    "c": "C",
    "x": "X",
    "p": "P",
}


def _clean_tok(val, max_len=12):
    if not val:
        return ""
    return str(val).replace("|", "-").replace(":", "-").strip()[:max_len]


def find_port(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == ESP_VID:
            return p.device
    return None


def _daemon_already_running():
    # 单例探测: 8787 已有 daemon 在监听则返回 True (本进程应放弃启动)。
    # 必须在开串口之前调用 —— 否则第二个进程会先抢 COM 口再发现自己多余。
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect((HOST, PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


class Daemon:
    def __init__(self, port):
        self.lock = threading.RLock()
        self.ser_lock = threading.Lock()      # 串口写互斥 (状态/计数/校时多线程)
        self.timer = None
        self.last_stat = None                 # 上次发出的计数, 去重用
        self.names = {}                       # session_id -> 终端显示名
        self.colors = {}                      # session_id -> peacock hex "rrggbb"
        self.agents = {}                      # session_id -> 'C' | 'X' | 'P'
        self.tools = {}                       # session_id -> 当前执行的工具短名
        self.renamed = set()                  # 手动 rename 过的 sid, hook 不覆盖
        self.last_work = None                 # 上次发出的工作名列表, 去重用
        self.last_wait = None                 # 上次发出的等待名列表, 去重用
        self.last_sess = None                 # 上次发出的 #sess 行, 去重用
        self.last_hist = None                 # 上次发出的 #hist 行, 去重用
        self.state_timestamps = {}            # session_id -> time.time() 进入当前状态
        self.history = []                     # [{name, completed_at}] 最近完成记录
        self.explicit_port = port             # 用户指定端口 (None=自动探测 303A:)
        self.ser = None
        self.reconnecting = False             # 是否已有重连线程在跑

        # 今日统计
        self.daily_date = time.strftime("%Y-%m-%d")
        self.daily_done = 0
        self.last_today = None                # 去重用

        # 成就系统
        self.achievements = self._load_achievements()
        self.last_work_date = self.achievements.get("last_work_date", "")
        self.streak_days = self.achievements.get("streak_days", 0)

        # 节日彩蛋
        self.festival_sent_today = False

        # Token 用量
        self.last_tok = None                  # 去重用

        # 工具连击计数 (Combo)
        self.combo_count = 0
        self.last_combo_ts = 0.0

        # 聚合逻辑: 串口写入作为 emit 注入; done/all_done 由定时器回落
        self.mgr = TaskStateManager(emit=self._on_state)

        ser, actual = self._open_serial()
        if ser is not None:
            self.ser = ser
            self.log(f"serial open: {actual} @ {BAUD}")
        else:
            self.log("device not present at startup; polling for it...")
            self._start_reconnect()

    # --- 探测并打开串口; 成功返回 (ser, port), 失败返回 (None, None) ---
    def _open_serial(self):
        tried = []
        if self.explicit_port:
            tried.append(self.explicit_port)
        auto = find_port(None)                # 优先重新探测 303A: 真实端口号
        if auto and auto not in tried:
            tried.append(auto)
        for port in tried:
            try:
                ser = serial.Serial(port, BAUD, timeout=0.3,
                                    write_timeout=WRITE_TIMEOUT_SEC)
                time.sleep(0.4)               # 等 CDC 稳定
                ser.reset_input_buffer()
                return ser, port
            except (serial.SerialException, OSError):
                continue
        return None, None

    def log(self, msg):
        print(f"[daemon] {time.strftime('%H:%M:%S')} {msg}", flush=True)

    # --- 串口写一行 (加锁; 任何线程都经此) ---
    # 写失败时关闭句柄并触发后台重连, 不抛给调用方。失败包括:
    #   - 设备拔出/RESET 致句柄失效 (SerialException/OSError)
    #   - 锁屏挂起 USB 致 write 超时 (SerialTimeoutException, SerialException 子类)
    #     —— 没 write_timeout 时这里会永久阻塞并连带卡死所有写线程, 务必保留超时。
    def _write_line(self, line):
        with self.ser_lock:
            if self.ser is None:
                return False
            try:
                self.ser.write((line + "\n").encode())
                self.ser.flush()
                return True
            except (serial.SerialException, OSError) as e:
                self.log(f"serial write failed ({e}); will reconnect")
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
        self._start_reconnect()
        return False

    # --- 后台重连: 轮询设备重新出现, 重开串口后重发当前状态 ---
    def _start_reconnect(self):
        with self.lock:
            if self.reconnecting:
                return
            self.reconnecting = True
        threading.Thread(target=self._reconnect_loop, daemon=True).start()

    def _reconnect_loop(self):
        self.log("serial lost; polling for device...")
        while True:
            ser, actual = self._open_serial()
            if ser is not None:
                with self.ser_lock:
                    self.ser = ser
                self.log(f"serial reopened: {actual} @ {BAUD}")
                with self.lock:
                    self.reconnecting = False
                self._resync()
                return
            time.sleep(RECONNECT_POLL_SEC)

    # --- 重连后把设备刷成当前真实状态 (重启后设备从 idle 开始) ---
    def _resync(self):
        with self.lock:
            self.last_stat = None
            self.last_work = None
            self.last_wait = None
            self.last_sess = None
            self.last_hist = None
            self.last_today = None
            state = self.mgr.device_state or IDLE
            self._write_line(state)
            self.log(f"-> {state} (resync)")
            self.push_stat()
            self.push_work()
            self.push_wait()
            self.push_sess()
            self.push_hist()
            self.push_today()
            self.last_tok = None
        try:
            self.push_time()
            self.push_tok()
        except Exception as e:
            self.log(f"resync time/tok error: {e}")

    # --- 计数行: 仅在变化时发 #stat T=.. W=.. D=.. C=.. X=.. P=.. ---
    def push_stat(self):
        c = self.mgr.counts()
        snap = self.mgr.snapshot()
        c_act = sum(1 for tid, st in snap.items()
                    if st in ("running", "waiting") and self.agents.get(tid, "C") == "C")
        x_act = sum(1 for tid, st in snap.items()
                    if st in ("running", "waiting") and self.agents.get(tid) == "X")
        p_act = sum(1 for tid, st in snap.items()
                    if st in ("running", "waiting") and self.agents.get(tid) == "P")
        key = (c["total"], c["work"], c["done"], c_act, x_act, p_act)
        if key == self.last_stat:
            return
        self.last_stat = key
        line = (f"#stat T={c['total']} W={c['work']} D={c['done']} "
                f"C={c_act} X={x_act} P={p_act}")
        self._write_line(line)
        self.log(f"-> {line}")

    # --- 工作终端名行: 仅在变化时发 #work n1|n2|.. (running 的 session 名) ---
    def push_work(self):
        running = [tid for tid, st in self.mgr.snapshot().items()
                   if st == "running"]
        names = [_clean_tok(self.names.get(tid, "")) for tid in running]
        names = [n for n in names if n]        # 去掉空名
        key = tuple(sorted(names))
        if key == self.last_work:
            return
        self.last_work = key
        self._write_line("#work " + "|".join(names))
        self.log(f"-> #work {'|'.join(names) or '(none)'}")

    # --- 等待终端名行: 仅在变化时发 #wait n1|n2|.. (waiting 的 session 名) ---
    def push_wait(self):
        waiting = [tid for tid, st in self.mgr.snapshot().items()
                   if st == "waiting"]
        names = [_clean_tok(self.names.get(tid, "")) for tid in waiting]
        names = [n for n in names if n]        # 去掉空名
        key = tuple(sorted(names))
        if key == self.last_wait:
            return
        self.last_wait = key
        self._write_line("#wait " + "|".join(names))
        self.log(f"-> #wait {'|'.join(names) or '(none)'}")

    # --- per-session 详情: #sess name:state:dur:color:agent:tool|... ---
    def push_sess(self):
        snap = self.mgr.snapshot()
        if not snap:
            line = "#sess"
        else:
            now = time.time()
            parts = []
            state_map = {"running": "R", "waiting": "W", "done": "D",
                         "pending": "P", "error": "E"}
            # 排序优先级: waiting > running > error > done > pending
            prio = {"waiting": 0, "running": 1, "error": 2, "done": 3, "pending": 4}
            ordered = sorted(snap.items(), key=lambda kv: (prio.get(kv[1], 9), kv[0]))
            for tid, st in ordered:
                name = _clean_tok(self.names.get(tid, tid[:8]), 12) or tid[:8]
                code = state_map.get(st, "?")
                ts = self.state_timestamps.get(tid, now)
                dur = max(0, int(now - ts))
                col = self.colors.get(tid, "")
                ag = self.agents.get(tid, "C")
                tool = _clean_tok(self.tools.get(tid, ""), 8) if st == "running" else ""
                tok = f"{name}:{code}:{dur}:{col}:{ag}:{tool}"
                parts.append(tok)
            line = "#sess " + "|".join(parts)
        if line == self.last_sess:
            return
        self.last_sess = line
        self._write_line(line)
        self.log(f"-> {line}")

    # --- 完成历史: #hist [agent]name:info|... ---
    def push_hist(self):
        if not self.history:
            line = "#hist"
        else:
            now = time.time()
            parts = []
            for entry in reversed(self.history[-MAX_HISTORY:]):
                ago = max(0, int(now - entry["completed_at"]))
                if ago < 60:
                    ago_s = f"{ago}s"
                elif ago < 3600:
                    ago_s = f"{ago // 60}m"
                else:
                    ago_s = f"{ago // 3600}h"
                ag = entry.get("agent", "C")
                nm = _clean_tok(entry.get("name", ""), 10)
                parts.append(f"{ag}~{nm}:{ago_s}")
            line = "#hist " + "|".join(parts)
        if line == self.last_hist:
            return
        self.last_hist = line
        self._write_line(line)
        self.log(f"-> {line}")

    # --- 校时行: #time HH:MM (电脑本地时间) ---
    def push_time(self):
        hhmm = time.strftime("%H:%M")
        self._write_line(f"#time {hhmm}")
        self.log(f"-> #time {hhmm}")

    # --- 今日统计: #today D=count S=streak ---
    def push_today(self):
        today = time.strftime("%Y-%m-%d")
        if today != self.daily_date:
            self.daily_date = today
            self.daily_done = 0
            self.festival_sent_today = False
        key = (self.daily_done, self.streak_days)
        if key == self.last_today:
            return
        self.last_today = key
        self._write_line(f"#today D={self.daily_done} S={self.streak_days}")
        self.log(f"-> #today D={self.daily_done} S={self.streak_days}")

    # --- Token 用量扫描: 联合扫描 Claude / Codex / Pi 今日所有 session ---
    def _scan_token_usage(self):
        today_str = time.strftime("%Y-%m-%d")
        today_start = time.mktime(time.strptime(today_str, "%Y-%m-%d"))

        total_in = 0
        total_out = 0
        total_cache = 0
        claude_tok = 0
        codex_tok = 0
        pi_tok = 0

        # 1. Claude Code (~/.claude/projects/**/*.jsonl)
        projects_dir = os.path.expanduser("~/.claude/projects")
        if os.path.isdir(projects_dir):
            for root, _dirs, files in os.walk(projects_dir):
                for fname in files:
                    if not fname.endswith(".jsonl"):
                        continue
                    fpath = os.path.join(root, fname)
                    try:
                        if os.path.getmtime(fpath) < today_start:
                            continue
                    except OSError:
                        continue
                    try:
                        with open(fpath, "r", encoding="utf-8",
                                  errors="replace") as f:
                            for line in f:
                                if '"assistant"' not in line:
                                    continue
                                try:
                                    obj = json.loads(line)
                                except (json.JSONDecodeError, ValueError):
                                    continue
                                if obj.get("type") != "assistant":
                                    continue
                                usage = obj.get("message", {}).get("usage")
                                if not usage:
                                    continue
                                i = usage.get("input_tokens", 0)
                                o = usage.get("output_tokens", 0)
                                ca = (usage.get("cache_creation_input_tokens", 0)
                                      + usage.get("cache_read_input_tokens", 0))
                                total_in += i
                                total_out += o
                                total_cache += ca
                                claude_tok += i + o
                    except (OSError, IOError):
                        continue

        # 2. Codex (~/.codex/sessions/**/*.jsonl)
        codex_dir = os.path.expanduser("~/.codex/sessions")
        if os.path.isdir(codex_dir):
            for root, _dirs, files in os.walk(codex_dir):
                for fname in files:
                    if not fname.endswith(".jsonl"):
                        continue
                    fpath = os.path.join(root, fname)
                    try:
                        if os.path.getmtime(fpath) < today_start:
                            continue
                    except OSError:
                        continue
                    try:
                        last_tot = None
                        with open(fpath, "r", encoding="utf-8",
                                  errors="replace") as f:
                            for line in f:
                                if '"token_count"' not in line:
                                    continue
                                try:
                                    obj = json.loads(line)
                                except (json.JSONDecodeError, ValueError):
                                    continue
                                if (obj.get("type") == "event_msg"
                                        and obj.get("payload", {}).get("type") == "token_count"):
                                    info = obj.get("payload", {}).get("info") or {}
                                    tot = info.get("total_token_usage")
                                    if tot:
                                        last_tot = tot
                        if last_tot:
                            raw_i = last_tot.get("input_tokens", 0)
                            cached_i = last_tot.get("cached_input_tokens", 0)
                            ca = cached_i + last_tot.get("cache_write_input_tokens", 0)
                            i = max(0, raw_i - cached_i)
                            o = last_tot.get("output_tokens", 0)
                            total_in += i
                            total_out += o
                            total_cache += ca
                            codex_tok += i + o
                    except (OSError, IOError):
                        continue

        # 3. Pi (~/.pi/agent/sessions/**/*.jsonl)
        pi_dir = os.path.expanduser("~/.pi/agent/sessions")
        if os.path.isdir(pi_dir):
            for root, _dirs, files in os.walk(pi_dir):
                for fname in files:
                    if not fname.endswith(".jsonl"):
                        continue
                    fpath = os.path.join(root, fname)
                    try:
                        if os.path.getmtime(fpath) < today_start:
                            continue
                    except OSError:
                        continue
                    try:
                        with open(fpath, "r", encoding="utf-8",
                                  errors="replace") as f:
                            for line in f:
                                if '"usage"' not in line:
                                    continue
                                try:
                                    obj = json.loads(line)
                                except (json.JSONDecodeError, ValueError):
                                    continue
                                if obj.get("type") != "message":
                                    continue
                                msg = obj.get("message") or {}
                                if msg.get("role") != "assistant":
                                    continue
                                usage = msg.get("usage")
                                if not usage:
                                    continue
                                i = usage.get("input", 0)
                                o = usage.get("output", 0)
                                ca = usage.get("cacheRead", 0) + usage.get("cacheWrite", 0)
                                total_in += i
                                total_out += o
                                total_cache += ca
                                pi_tok += i + o
                    except (OSError, IOError):
                        continue

        return (total_in, total_out, total_cache, claude_tok, codex_tok, pi_tok)

    def push_tok(self):
        try:
            in_tok, out_tok, cache_tok, c_tok, x_tok, p_tok = self._scan_token_usage()
        except Exception as e:
            self.log(f"tok scan error: {e}")
            return
        key = (in_tok, out_tok, cache_tok, c_tok, x_tok, p_tok)
        if key == self.last_tok:
            return
        self.last_tok = key
        line = (f"#tok IN={in_tok} OUT={out_tok} CACHE={cache_tok} "
                f"C={c_tok} X={x_tok} P={p_tok}")
        self._write_line(line)
        self.log(f"-> {line}")

    # --- 成就系统 ---
    def _achievements_path(self):
        return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "achievements.json")

    def _load_achievements(self):
        path = self._achievements_path()
        if os.path.exists(path):
            try:
                with open(path, "r") as f:
                    return json.load(f)
            except Exception:
                pass
        return {"unlocked": [], "streak_days": 0, "last_work_date": ""}

    def _save_achievements(self):
        self.achievements["streak_days"] = self.streak_days
        self.achievements["last_work_date"] = self.last_work_date
        path = self._achievements_path()
        try:
            with open(path, "w") as f:
                json.dump(self.achievements, f, indent=2)
        except Exception as e:
            self.log(f"achievements save error: {e}")

    def _unlock(self, name):
        if name in self.achievements.get("unlocked", []):
            return
        self.achievements.setdefault("unlocked", []).append(name)
        self._save_achievements()
        self._write_line(f"#achv {name}")
        self.log(f"-> #achv {name}")

    def _check_achievements(self, event_type):
        if event_type == "done":
            if not self.achievements.get("unlocked") or \
               "FIRST!" not in self.achievements["unlocked"]:
                self._unlock("FIRST!")
            if self.daily_done >= 3:
                self._unlock("HAT TRICK")
            if self.daily_done >= 10:
                self._unlock("ON FIRE")
        if event_type == "streak":
            if self.streak_days >= 3:
                self._unlock("STREAK x3")
        if event_type == "marathon":
            self._unlock("MARATHON")

    # --- 节日彩蛋 ---
    FESTIVALS = {
        (1, 1): "new-year", (2, 14): "valentine",
        (10, 31): "halloween", (12, 25): "christmas",
    }

    def _check_festival(self):
        if self.festival_sent_today:
            return
        now = time.localtime()
        name = self.FESTIVALS.get((now.tm_mon, now.tm_mday))
        if name:
            self.festival_sent_today = True
            self._write_line(f"#festival {name}")
            self.log(f"-> #festival {name}")

    def time_loop(self):
        self.push_time()
        self._check_festival()
        self.push_tok()
        while True:
            now = time.time()
            time.sleep(60 - (now % 60) + 0.2)
            try:
                self.push_time()
                self._check_festival()
                self._reap_stale_sessions()
                # 每分钟刷新 #sess 避免详情页时长漂移
                with self.lock:
                    self.last_sess = None
                    self.push_sess()
                self.push_tok()
            except Exception as e:
                self.log(f"time push error: {e}")

    def _reap_stale_sessions(self):
        now = time.time()
        with self.lock:
            snap = self.mgr.snapshot()
            for sid, st in list(snap.items()):
                ts = self.state_timestamps.get(sid, now)
                if st in ("running", "waiting") and (now - ts) >= STALE_SESSION_SEC:
                    self.log(f"stale session {sid[:8]} ({st} for "
                             f"{int(now - ts)}s), marking idle")
                    self.mgr.idle_task(sid)
                    self.state_timestamps.pop(sid, None)
                    self.tools.pop(sid, None)
                elif st == "done" and (now - ts) >= IDLE_AFTER_SEC:
                    # Codex / Pi 没有 NotificationIdle, 完成超过 15s 自动归档移除
                    self.mgr.remove_task(sid)
                    self.state_timestamps.pop(sid, None)
                    self.tools.pop(sid, None)

    def _record_history(self, sid):
        hist_name = self.names.get(sid, sid[:8])
        ag = self.agents.get(sid, "C")
        self.history = [h for h in self.history if h.get("sid") != sid]
        self.history.append({
            "name": hist_name,
            "sid": sid,
            "agent": ag,
            "completed_at": time.time(),
        })
        if len(self.history) > MAX_HISTORY:
            self.history = self.history[-MAX_HISTORY:]

    # --- TaskStateManager 状态变化回调: 写串口 + 安排回落定时器 ---
    def _on_state(self, state):
        self._write_line(state)
        self.log(f"-> {state}")
        if state == DONE:
            # done 脉冲: 停留片刻后回到聚合稳态并清理已完成的单任务
            self.schedule(DONE_PULSE_SEC, self._settle_and_clean_done)
        elif state == ALL_DONE:
            # 全部完成: 空闲 N 秒后清理已完成任务并回落 idle
            self.schedule(IDLE_AFTER_SEC, self._idle_down)
        else:
            self.cancel_timer()

    def _settle_and_clean_done(self):
        for tid, st in list(self.mgr.snapshot().items()):
            if st == "done":
                self.mgr.remove_task(tid)
                self.state_timestamps.pop(tid, None)
                self.tools.pop(tid, None)
        self.mgr.settle()

    def _idle_down(self):
        # all_done 停留后: 移除所有已完成任务, 无任务 -> settle 得到 idle
        for tid, st in list(self.mgr.snapshot().items()):
            if st == "done":
                self.mgr.remove_task(tid)
                self.state_timestamps.pop(tid, None)
                self.tools.pop(tid, None)

    def cancel_timer(self):
        if self.timer:
            self.timer.cancel()
            self.timer = None

    def schedule(self, delay, fn):
        self.cancel_timer()
        self.timer = threading.Timer(delay, self._run_locked, args=(fn,))
        self.timer.daemon = True
        self.timer.start()

    def _run_locked(self, fn):
        with self.lock:
            fn()
            self.push_stat()
            self.push_work()
            self.push_wait()
            self.push_sess()
            self.push_hist()
            self.push_today()

    # --- hook 事件 -> TaskStateManager API ---
    def on_event(self, event, sid, name="", color="", agent="", tool=""):
        with self.lock:
            if not sid and event != "manual":
                sid = f"{agent or 'claude'}-{name or 'default'}"
            if event == "rename":
                # 手动 rename: 最高优先, 标记不被 hook 覆盖
                if name:
                    self.names[sid] = name
                    self.renamed.add(sid)
            elif name and sid not in self.renamed:
                self.names[sid] = name
            if color:
                self.colors[sid] = color       # peacock 颜色 (空值不覆盖, 保留缓存)
            if agent:
                self.agents[sid] = AGENT_CODE_MAP.get(agent.lower(), "C")
            elif sid not in self.agents:
                self.agents[sid] = "C"

            if event == "UserPromptSubmit":
                self.combo_count = 1
                self.last_combo_ts = time.time()
                self.tools[sid] = tool or "think"
                self.mgr.start_task(sid)                 # -> working
                self.state_timestamps[sid] = time.time()
                self._write_line(f"#combo {self.combo_count}")

            elif event == "SessionStart":
                self.mgr.add_task(sid)                   # pending (视为 idle)
                self.state_timestamps[sid] = time.time()

            elif event == "SessionEnd":
                self.mgr.remove_task(sid)                # 无任务 -> idle
                self.state_timestamps.pop(sid, None)
                self.names.pop(sid, None)
                self.colors.pop(sid, None)
                self.agents.pop(sid, None)
                self.tools.pop(sid, None)
                self.renamed.discard(sid)

            elif event == "SubagentStop":
                # 子任务完成: 瞬时 done 脉冲, 不改变 session 忙碌态
                self.mgr._send(DONE, force=True)
                self.schedule(DONE_PULSE_SEC, self.mgr.settle)

            elif event == "Stop":
                self.combo_count = 0
                self._write_line("#combo 0")
                # 成就: 检查单次工作时长是否 >= 2h
                start_ts = self.state_timestamps.get(sid)
                if start_ts:
                    work_sec = int(time.time() - start_ts)
                    if work_sec >= 7200:
                        self._check_achievements("marathon")
                self.daily_done += 1
                # 连续天数
                today = time.strftime("%Y-%m-%d")
                if today != self.last_work_date:
                    yesterday = time.strftime(
                        "%Y-%m-%d",
                        time.localtime(time.time() - 86400))
                    if self.last_work_date == yesterday:
                        self.streak_days += 1
                    else:
                        self.streak_days = 1
                    self.last_work_date = today
                    self._save_achievements()
                    self._check_achievements("streak")
                self._check_achievements("done")
                self.tools.pop(sid, None)
                self._record_history(sid)
                self.mgr.finish_task(sid)                # done(仍有在跑) / all_done
                self.state_timestamps[sid] = time.time()

            elif event == "NotificationIdle":
                self.cancel_timer()
                # 成就: ESC 中断也检查单次工作时长是否 >= 2h
                start_ts = self.state_timestamps.get(sid)
                if start_ts and (time.time() - start_ts) >= 7200:
                    self._check_achievements("marathon")
                # session 真正回到空闲: 记录一次完成历史 (按 sid 去重)
                if self.mgr.snapshot().get(sid) in ("running", "waiting", "done"):
                    self._record_history(sid)
                self.tools.pop(sid, None)
                self.mgr.idle_task(sid)
                self.state_timestamps.pop(sid, None)

            elif event == "NotificationChoosing":
                self.cancel_timer()
                self.tools[sid] = "wait"
                self.mgr.wait_task(sid)
                self.state_timestamps[sid] = time.time()

            elif event in ("PreToolUse", "PostToolUse"):
                old_st = self.mgr.snapshot().get(sid)
                if old_st is None or old_st in ("pending", "done"):
                    self.mgr.start_task(sid)
                    old_st = "running"
                else:
                    self.mgr.unwait_task(sid)
                if tool:
                    self.tools[sid] = tool
                elif event == "PostToolUse" and not self.tools.get(sid):
                    self.tools[sid] = "code"
                if event == "PreToolUse":
                    now_t = time.time()
                    if now_t - self.last_combo_ts <= 45.0:
                        self.combo_count += 1
                    else:
                        self.combo_count = 1
                    self.last_combo_ts = now_t
                    self._write_line(f"#combo {self.combo_count}")
                    self.log(f"-> #combo {self.combo_count} ({tool})")
                # 工具仍在执行 -> 该 session 仍活跃, 刷新时间戳避免被 90s reaper 误判空闲。
                if old_st in ("running", "waiting"):
                    self.state_timestamps[sid] = time.time()

            elif event == "manual":
                self.cancel_timer()
                self.mgr._send(sid, force=True)

            elif event == "quit":
                self.log("received quit command; closing serial and exiting.")
                with self.ser_lock:
                    if self.ser is not None:
                        try:
                            self.ser.close()
                        except Exception:
                            pass
                        self.ser = None
                os._exit(0)

            else:
                self.log(f"ignored event: {event}")

            self.push_stat()        # 每个事件处理后, 计数变化才发 #stat
            self.push_work()        # 工作中的终端名变化才发 #work
            self.push_wait()        # 等待中的终端名变化才发 #wait
            self.push_sess()        # per-session 详情
            self.push_hist()        # 完成历史
            self.push_today()       # 今日统计

    def serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Windows 上 SO_REUSEADDR 允许多进程绑同一端口 -> 多个 daemon 共存抢 accept,
        # 正是多实例混乱的根因, 故 Windows 不设; 非 Windows 保留以便快速重启。
        if os.name != "nt":
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((HOST, PORT))
        except OSError:
            # 单例探测与 bind 之间的竞态兜底: 端口被别的 daemon 抢先, 本进程退出。
            # (此时本进程已开的串口句柄随退出释放, 让赢家独占。)
            self.log(f"port {PORT} busy; another daemon won the race; exiting.")
            sys.exit(0)
        srv.listen(16)
        self.log(f"listening on {HOST}:{PORT}")
        with self.lock:
            self.mgr.clear()        # 启动即 idle
            self.push_stat()
            self.push_work()
            self.push_wait()
            self.push_sess()
            self.push_hist()
            self.push_today()       # 初始今日统计
        # 校时线程: 启动即发 + 每整分钟发 #time
        threading.Thread(target=self.time_loop, daemon=True).start()
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self.handle_conn, args=(conn,),
                             daemon=True).start()

    def handle_conn(self, conn):
        try:
            data = conn.recv(4096).decode(errors="replace").strip()
            if not data:
                return
            msg = json.loads(data)
            event = msg.get("event", "")
            sid = msg.get("session_id", "")
            name = msg.get("name", "")
            color = msg.get("color", "")
            agent = msg.get("agent", "")
            tool = msg.get("tool", "")
            self.on_event(event, sid, name, color, agent, tool)
            conn.sendall(b"ok")
        except Exception as e:
            self.log(f"conn error: {e}")
        finally:
            conn.close()


FLASH_LOCK = os.path.expanduser("~/.clawdpet_flashing.lock")


def main():
    if os.path.exists(FLASH_LOCK):
        try:
            if time.time() - os.path.getmtime(FLASH_LOCK) < 120:
                sys.exit(0)
        except Exception:
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM4 (default: auto 303A:)")
    args = ap.parse_args()

    # 单例: 已有 daemon 在 8787 监听则直接退出, 不开串口、不抢 COM 口。
    # hook 在连不上时会自动 spawn 本进程, 没有这道闸就会堆成多实例互抢串口。
    if _daemon_already_running():
        print("[daemon] already running on "
              f"{HOST}:{PORT}; exiting.", file=sys.stderr)
        sys.exit(0)

    port = find_port(args.port)
    if not port and not args.port:
        # 启动时设备不在也照常起: daemon 会后台轮询, 设备插上自动连。
        print("[daemon] no Espressif (303A:) device yet; starting anyway "
              "and polling for it.", file=sys.stderr)

    d = Daemon(args.port)        # explicit_port=None 时由 _open_serial 自动探测
    try:
        d.serve()
    except KeyboardInterrupt:
        d.log("bye")


if __name__ == "__main__":
    main()
