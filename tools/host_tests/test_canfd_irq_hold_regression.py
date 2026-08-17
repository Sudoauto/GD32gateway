#!/usr/bin/env python3
"""Structural regression guard for CAN mailbox IRQ ownership.

Protects two field-critical invariants:
  * MB0 TX pending must be acknowledged before software-state early exits;
  * RX mailbox must be drained in ISR and must never be masked by can_task.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "user/driver/drv_canfd.c").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    for prefix in ("static void ", "void ", "static bool ", "bool "):
        start = SRC.find(prefix + name + "(")
        if start >= 0:
            break
    else:
        raise AssertionError(f"missing function {name}")
    brace = SRC.find("{", start)
    depth = 0
    for i in range(brace, len(SRC)):
        if SRC[i] == "{":
            depth += 1
        elif SRC[i] == "}":
            depth -= 1
            if depth == 0:
                return SRC[brace + 1:i]
    raise AssertionError(f"unterminated body for {name}")


completion = function_body("tx_completion_service")
clear_pos = completion.find("can_interrupt_flag_clear")
inactive_guard_pos = completion.find("if (!s_tx_active)")
assert clear_pos >= 0 and inactive_guard_pos >= 0
assert clear_pos < inactive_guard_pos, (
    "REGRESSION: software inactive guard occurs before MB0 acknowledge"
)

service = function_body("drv_canfd_service")
assert service.find("tx_completion_service();") < service.find("handle_status_flags();"), (
    "REGRESSION: error policy runs before TX completion"
)
assert "if (!s_tx_hold)" in service, "MB0 IRQ safe-hold guard missing"
assert "CAN_RX_MB" not in service, (
    "REGRESSION: task context must not mask/re-arm RX mailbox; RX is ISR-owned"
)

isr = function_body("drv_canfd_isr_message")
rx_get = isr.find("flag_for_mailbox(CAN_RX_MB)")
rx_read = isr.find("rx_mailbox_read_frame(CAN_RX_MB")
rx_queue = isr.find("xQueueSendFromISR(q_can_rx")
tx_mask = isr.find("can_interrupt_disable(CAN_PERIPH, irq_for_mailbox(CAN_TX_MB))")
assert 0 <= rx_get < rx_read < rx_queue, "RX mailbox is not immediately drained/enqueued in ISR"
assert tx_mask >= 0, "TX MB0 pending source must be masked until task completion"

hold = function_body("tx_enter_hold")
assert "tx_mailbox_quiesce(true);" in hold

quiesce = function_body("tx_mailbox_quiesce")
assert quiesce.find("can_interrupt_disable") < quiesce.rfind("can_interrupt_flag_clear"), (
    "TX mailbox quiesce must mask then acknowledge MB0"
)

print("CAN mailbox IRQ regression: PASS")
