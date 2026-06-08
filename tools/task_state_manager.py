#!/usr/bin/env python3
# ============================================================================
# task_state_manager.py - 多任务状态管理器
#
# 维护多个任务的状态: pending / running / waiting / done / error
# 根据聚合规则输出设备状态: idle / working / choosing / done / all_done / error
#
# 规则:
#   - 任一任务 error            -> error    (优先级最高)
#   - 任一任务 waiting          -> choosing (等用户选择/输入, 高于 working)
#   - 任一任务 running          -> working
#   - 某任务"刚"完成(且仍有在跑) -> done (瞬时提示)
#   - 全部任务完成(无 running)   -> all_done
#   - 没有任何任务              -> idle
#
# 设计: 与串口解耦。构造时注入 emit(state:str) 回调, 仅在设备状态
#       变化时调用; done 为瞬时脉冲, 由调用方决定停留多久后 settle()。
# ============================================================================

from enum import Enum
from threading import RLock
from typing import Callable, Dict, Optional


class TaskStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    WAITING = "waiting"     # 等用户选择/输入 (权限确认/选项/计划审批)
    DONE = "done"
    ERROR = "error"


# 设备(屏幕)状态
IDLE = "idle"
WORKING = "working"
CHOOSING = "choosing"      # 等用户选择/输入
DONE = "done"
ALL_DONE = "all_done"
ERROR = "error"


class TaskStateManager:
    def __init__(self, emit: Optional[Callable[[str], None]] = None):
        """
        emit: 设备状态变化时的回调, 形如 emit("working")。
              瞬时的 done 脉冲也通过它发出 (force, 见下)。
              不传则默认打印到 stdout。
        """
        self._tasks: Dict[str, TaskStatus] = {}
        self._device_state: Optional[str] = None
        self._lock = RLock()
        self._emit = emit if emit is not None else (lambda s: print(f"[emit] {s}"))

    # ---- 内部: 发送设备状态 (去抖; force 用于瞬时脉冲如 done) ----
    def _send(self, state: str, force: bool = False) -> None:
        if state == self._device_state and not force:
            return
        self._device_state = state
        self._emit(state)

    # ---- 计数 ----
    def _count(self, status: TaskStatus) -> int:
        return sum(1 for s in self._tasks.values() if s == status)

    def _any_running(self) -> bool:
        return self._count(TaskStatus.RUNNING) > 0

    def _any_waiting(self) -> bool:
        return self._count(TaskStatus.WAITING) > 0

    def _any_error(self) -> bool:
        return self._count(TaskStatus.ERROR) > 0

    def _any_pending(self) -> bool:
        return self._count(TaskStatus.PENDING) > 0

    # ---- 聚合稳态: 计算当前应显示的设备状态 ----
    def _settled_state(self) -> str:
        if self._any_error():
            return ERROR
        if not self._tasks:
            return IDLE
        if self._any_waiting():
            # 有 session 在等用户选择/输入 -> choosing (高于 working)
            return CHOOSING
        if self._any_running():
            return WORKING
        if self._any_pending():
            # 仅排队、尚未开始 -> 视为空闲
            return IDLE
        # 有任务且全部 done (无 pending/running/error) -> 全部完成
        return ALL_DONE

    def settle(self) -> str:
        """回到聚合稳态 (用于 done 脉冲结束后)。返回当前设备状态。"""
        with self._lock:
            st = self._settled_state()
            self._send(st)
            return st

    @property
    def device_state(self) -> Optional[str]:
        return self._device_state

    def snapshot(self) -> Dict[str, str]:
        """返回各任务状态副本, 便于调试。"""
        with self._lock:
            return {k: v.value for k, v in self._tasks.items()}

    def counts(self) -> Dict[str, int]:
        """屏幕计数: total=任务总数, work=running+waiting, done=done。"""
        with self._lock:
            return {
                "total": len(self._tasks),
                "work": self._count(TaskStatus.RUNNING) + self._count(TaskStatus.WAITING),
                "done": self._count(TaskStatus.DONE),
            }

    # ---- 任务生命周期 API ----
    def add_task(self, task_id: str) -> None:
        """登记一个待处理任务 (pending)。"""
        with self._lock:
            self._tasks[task_id] = TaskStatus.PENDING
            self.settle()

    def start_task(self, task_id: str) -> None:
        """任务开始运行 -> running, 设备进入 working。"""
        with self._lock:
            self._tasks[task_id] = TaskStatus.RUNNING
            self._send(self._settled_state())  # -> working

    def finish_task(self, task_id: str) -> None:
        """
        任务完成 -> done。
          - 若仍有别的任务在跑: 发瞬时 done 脉冲 (设备随后由 settle() 回到 working)
          - 若这是最后一个: 直接进入 all_done
        """
        with self._lock:
            self._tasks[task_id] = TaskStatus.DONE
            if self._any_error():
                self._send(ERROR)
            elif self._any_running():
                self._send(DONE, force=True)   # 单个完成, 瞬时提示
            else:
                self._send(ALL_DONE)           # 全部完成

    def fail_task(self, task_id: str) -> None:
        """任务异常 -> error, 设备进入 error (优先级最高)。"""
        with self._lock:
            self._tasks[task_id] = TaskStatus.ERROR
            self._send(ERROR)

    def remove_task(self, task_id: str) -> None:
        """移除任务 (例如清理已完成的)。无任务时回到 idle。"""
        with self._lock:
            self._tasks.pop(task_id, None)
            self.settle()

    def idle_task(self, task_id: str) -> None:
        """
        session 回到空闲 (idle_prompt 通知): 这一轮彻底结束。
          - 无论正常结束还是被 ESC 中断, 都把该任务从 running 中清掉,
            让设备回落到聚合稳态 (无别的任务在跑 -> idle)。
          - 边界: 任务不存在则忽略; 已是 done 则不改动 (保留 all_done/done 语义)。
        """
        with self._lock:
            st = self._tasks.get(task_id)
            if st is None:
                return                       # 未知 task, 忽略
            if st == TaskStatus.DONE:
                return                       # 已完成, 不打扰 done/all_done 提示
            self._tasks.pop(task_id, None)   # 移除该 session 的忙碌态
            self.settle()                    # 回到聚合稳态

    def clear(self) -> None:
        """清空所有任务 -> idle。"""
        with self._lock:
            self._tasks.clear()
            self._send(IDLE)

    def wait_task(self, task_id: str) -> None:
        """
        session 进入"等用户选择/输入"(权限确认/选项/计划审批) -> waiting,
        设备进入 choosing。
          - 未知 task 也登记为 waiting: 权限弹窗出现时该 session 一定活跃,
            但可能 daemon 没收到过它的 start (例如 daemon 重启后)。
          - 已 done 的任务不改动 (不打扰 done/all_done 提示)。
        """
        with self._lock:
            if self._tasks.get(task_id) == TaskStatus.DONE:
                return
            self._tasks[task_id] = TaskStatus.WAITING
            self._send(self._settled_state())   # -> choosing

    def unwait_task(self, task_id: str) -> None:
        """
        用户做完选择/授权, 工具开始执行 (PostToolUse) -> 该 session 回到 running。
          - 仅当该 session 当前是 waiting 才转回 running 并 settle (-> working)。
          - 否则忽略: PostToolUse 高频触发, 不在等待态时不应刷屏。
        """
        with self._lock:
            if self._tasks.get(task_id) != TaskStatus.WAITING:
                return
            self._tasks[task_id] = TaskStatus.RUNNING
            self.settle()                        # -> working


# --- 自测 ---
if __name__ == "__main__":
    log = []
    m = TaskStateManager(emit=lambda s: log.append(s))

    m.add_task("a")          # idle (pending 不算 running)
    m.start_task("a")        # working
    m.add_task("b")
    m.start_task("b")        # working (去抖, 不重复)
    m.finish_task("a")       # done (b 还在跑)
    m.settle()               # working (回到稳态)
    m.finish_task("b")       # all_done (最后一个)
    m.clear()                # idle

    print("emit sequence:", log)
    expected = ["idle", "working", "done", "working", "all_done", "idle"]
    assert log == expected, f"FAIL: {log} != {expected}"
    print("OK: matches", expected)

    # error 路径
    log.clear()
    m2 = TaskStateManager(emit=lambda s: log.append(s))
    m2.start_task("x")       # working
    m2.fail_task("x")        # error
    print("error sequence:", log)
    assert log == ["working", "error"], log
    print("OK error path")

    # idle_task 路径: ESC 中断后由 idle_prompt 回落
    log.clear()
    m3 = TaskStateManager(emit=lambda s: log.append(s))
    m3.start_task("s1")          # working (假装在跑, 被 ESC 中断)
    m3.idle_task("s1")           # idle (回到空闲)
    m3.idle_task("unknown")      # 未知 task: 忽略, 无输出
    print("idle sequence:", log)
    assert log == ["working", "idle"], log
    print("OK idle_task path")

    # idle_task 多 session: 只回落当前 session, 别的仍在跑 -> 保持 working
    log.clear()
    m4 = TaskStateManager(emit=lambda s: log.append(s))
    m4.start_task("a")           # working
    m4.start_task("b")           # working (去抖)
    m4.idle_task("a")            # b 还在跑 -> 仍 working (去抖, 不重复)
    print("idle multi sequence:", log)
    assert log == ["working"], log
    assert m4.device_state == "working", m4.device_state
    m4.idle_task("b")            # 最后一个空闲 -> idle
    assert m4.device_state == "idle", m4.device_state
    print("OK idle_task multi-session path")

    # idle_task 不打扰已完成的 done 任务
    log.clear()
    m5 = TaskStateManager(emit=lambda s: log.append(s))
    m5.start_task("a")
    m5.start_task("b")
    m5.finish_task("a")          # a=done, b 在跑 -> done 脉冲
    m5.settle()                  # working
    m5.idle_task("a")            # a 已 done -> 忽略, 不改动
    print("idle done-guard sequence:", log)
    assert log == ["working", "done", "working"], log
    print("OK idle_task done-guard path")

    # wait_task / unwait_task 路径: 权限确认进 choosing, PostToolUse 回 working
    log.clear()
    m6 = TaskStateManager(emit=lambda s: log.append(s))
    m6.start_task("a")           # working
    m6.wait_task("a")            # choosing (等用户授权/选择)
    m6.unwait_task("a")          # working (用户选完, 工具执行)
    print("choosing sequence:", log)
    assert log == ["working", "choosing", "working"], log
    print("OK wait/unwait path")

    # unwait 守卫: 非 waiting 态的 PostToolUse 不刷屏
    log.clear()
    m7 = TaskStateManager(emit=lambda s: log.append(s))
    m7.start_task("a")           # working
    m7.unwait_task("a")          # a 是 running 非 waiting -> 忽略
    print("unwait guard sequence:", log)
    assert log == ["working"], log
    assert m7.device_state == "working", m7.device_state
    print("OK unwait guard path")

    # choosing 优先级高于 working: 多 session, 一个 waiting 即 choosing
    log.clear()
    m8 = TaskStateManager(emit=lambda s: log.append(s))
    m8.start_task("a")           # working
    m8.start_task("b")           # working (去抖)
    m8.wait_task("b")            # b 等输入 -> choosing (盖过 a 在跑)
    assert m8.device_state == "choosing", m8.device_state
    c = m8.counts()              # waiting 仍算活跃 -> work=2
    assert c["work"] == 2, c
    m8.unwait_task("b")          # b 回 running -> working
    assert m8.device_state == "working", m8.device_state
    print("choosing-priority sequence:", log)
    assert log == ["working", "choosing", "working"], log
    print("OK choosing-priority path")
