#!/usr/bin/env python3
"""Structural regression guard for v0.6.1 TCP communication baseline."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


cfg = text("user/config/gateway_build_config.h")
lw = text("user/config/lwipopts.h")
tcp = text("user/net/gw_tcp_server.c")
rtos = text("user/rtos/rtos_objects.h")
app = text("user/app/gateway_app.c")
main = text("user/main.c")


def macro(name: str) -> int:
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)U?\s*$", cfg, re.M)
    assert m, f"missing numeric macro {name}"
    return int(m.group(1))


assert macro("GW_ETH_ENABLE") == 1
assert macro("GW_TCP_SERVER_ENABLE") == 1
assert macro("GW_TCP_SERVER_PORT") == 5000
assert 1 <= macro("GW_TCP_SERVER_BACKLOG") <= 16
for name in ("GW_TCP_ACCEPT_POLL_MS", "GW_TCP_RECV_POLL_MS",
             "GW_TCP_SEND_TIMEOUT_MS", "GW_TCP_RETRY_MS"):
    assert macro(name) > 0, f"{name} must be bounded and non-zero"
assert macro("GW_TCP_ECHO_ENABLE") == 1

assert re.search(r"#define\s+LWIP_NETCONN\s+1", lw), "Netconn API must be enabled"
assert re.search(r"#define\s+LWIP_SOCKET\s+0", lw), "baseline should not mix socket and Netconn APIs"
assert re.search(r"#define\s+LWIP_TCP\s+1", lw)

assert "EVT_TCP_SERVER_READY" in rtos
assert "EVT_TCP_CLIENT_CONNECTED" in rtos
assert "EVT_NET_IP_READY" in tcp
assert "xEventGroupWaitBits(g_system_events, EVT_NET_IP_READY" in tcp
assert "netconn_new(NETCONN_TCP)" in tcp
assert "netconn_bind(listener, IP_ADDR_ANY" in tcp
assert "netconn_listen_with_backlog" in tcp
assert "netconn_set_recvtimeout(listener" in tcp, "accept must be bounded"
assert "netconn_set_recvtimeout(client" in tcp, "client receive must be bounded"
assert "netconn_set_sendtimeout(client" in tcp, "client send must be bounded"
assert "netconn_accept(listener" in tcp
assert "netconn_recv(client" in tcp
assert "netconn_write_partly" in tcp and "NETCONN_COPY" in tcp
assert "netconn_close(*client)" in tcp and "netconn_delete(*client)" in tcp
assert "netconn_delete(*listener)" in tcp
assert "taskYIELD();" in tcp, "continuous TCP traffic must yield periodically"
assert "GW_TCP_SERVER_RX_BURST" not in cfg, "burst limit is an implementation invariant, not a field tuning knob"
assert "gw_tcp_server_task_create();" in app
assert any(v in main for v in ("v0.6.1-tcp-baseline", "v0.7.0-gui-lvgl-baseline", "v0.8.0-unified-gateway-hmi", "v0.8.1-modbus-direct-control", "v0.9.0-edge-management", "v0.9.1-hmi-screenfix", "v0.9.2-scheduler-runtimefix", "v0.9.3-hmi-mpu-faultdiag", "v0.9.4-hmi-vsync-doublebuffer", "v0.9.5-hmi-ipa-fastpath"))

# TCP server must run below lwIP tcpip thread and below data/CAN/RS485 work.
m = re.search(r"#define\s+TCP_SERVER_TASK_PRIORITY\s+(\d+)U", tcp)
assert m and int(m.group(1)) == 2
m = re.search(r"#define\s+TCPIP_THREAD_PRIO\s+(\d+)", lw)
assert m and int(m.group(1)) >= 3

proj = ET.parse(ROOT / "project/gateway.uvprojx")
paths = [(e.text or "").replace("\\", "/") for e in proj.iter("FilePath")]
assert "../user/net/gw_tcp_server.c" in paths, "Keil project missing TCP server source"

print("TCP v0.6.1 Netconn baseline regression: PASS")
