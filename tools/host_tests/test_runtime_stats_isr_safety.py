from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
hooks = (ROOT / "user/app/freertos_hooks.c").read_text()
timec = (ROOT / "user/common/gw_time.c").read_text()

def strip_comments(s: str) -> str:
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//.*', '', s)
    return s

start = hooks.index("uint32_t gw_runtime_stats_counter")
body = strip_comments(hooks[start: hooks.index("void vApplicationTickHook", start)])
assert "gw_time_runtime_counter32" in body
for forbidden in ("gw_time_ms(", "taskENTER_CRITICAL", "xTask", "vTask", "xSemaphore", "xQueue"):
    assert forbidden not in body, f"runtime stats hook is not PendSV-safe: {forbidden}"

start = timec.index("uint32_t gw_time_runtime_counter32")
body = strip_comments(timec[start: timec.index("uint64_t gw_time_ms", start)])
assert "return s_tick_low;" in body
for forbidden in ("taskENTER_CRITICAL", "taskEXIT_CRITICAL", "xTask", "vTask"):
    assert forbidden not in body, f"runtime counter uses task API: {forbidden}"

print("FreeRTOS runtime-stats PendSV safety regression: PASS")
