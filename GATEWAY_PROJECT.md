# v0.9.5 HMI IPA fast-path / latency hotfix

v0.9.5 keeps the v0.9.4 **25 MHz / ~54.3 Hz TLI timing and VSYNC page flip**, but removes the main source of UI lag: LVGL no longer performs software rasterization directly into the full-screen **non-cacheable external SDRAM** framebuffer.

The display pipeline is now:

```text
LVGL 9.2.2 RGB565
      │
      │ PARTIAL render, 2 x 800x40 buffers
      ▼
internal AXI SRAM (cacheable, fast CPU drawing)
      │
      │ D-cache clean for the rendered rectangle
      ▼
GD32H759 IPA RGB565 rectangle copy
      │
      ▼
off-screen SDRAM framebuffer
      │
      │ final invalid area only
      ▼
TLI frame-blank / VSYNC page flip
      │
      └── IPA mirrors the changed rectangles into the old off-screen FB
          so both full framebuffers remain coherent without a full-frame CPU copy.
```

The two scan-out buffers remain non-cacheable for simple TLI/IPA coherency, but the CPU-only LVGL TLSF heap at `0xC0200000` is overlaid by MPU region 2 as **Normal cacheable** memory. This removes another high-latency SDRAM path for label/style/object allocation while keeping DMA-visible framebuffer memory uncached.

Memory / rendering layout:

```text
Internal AXI SRAM
  draw0 800x40 RGB565 = 64 KiB
  draw1 800x40 RGB565 = 64 KiB

External SDRAM
  0xC0000000  FB0 800x480 RGB565  (non-cacheable)
  0xC0100000  FB1 800x480 RGB565  (non-cacheable)
  0xC0200000  LVGL heap 512 KiB    (cacheable MPU overlay)
```

Additional latency reductions:

- LVGL default refresh period is `18 ms`, matching the ~54.3 Hz panel instead of limiting redraw to ~30 FPS.
- GUI task priority is raised from 1 to 2, still below CAN/RS485/data/poll real-time work.
- Periodic refresh only updates the **currently visible page**; hidden device/data/network/service pages are not invalidated every 250 ms.
- Repeated label text is compared with the existing label text before calling `lv_label_set_text`, avoiding redraws when values did not change.
- IPA/TLI diagnostics expose partial blit count, back-buffer sync count, transferred bytes, timeout/error counters and full-resync count.

Expected startup breadcrumbs:

```text
[I][LCD] TLI ready 800x480 RGB565; PCLK=25.0MHz refresh=54.3Hz; IPA partial-blit + VSYNC double-FB; backlight ON
[I][GUI] LVGL display PASS partial-IPA draw0=0x240..... draw1=0x240..... 800x40 (64000B each) FB0=0xC0000000 FB1=0xC0100000
[I][GUI] HMI ready 800x480 LVGL 9.2.2 partial-IPA 2x40-line touch=YES
```

The v0.9.4 double-framebuffer architecture remains as a safety property; v0.9.5 changes **where LVGL renders and how dirty pixels reach SDRAM**, not the tear-free scan-out mechanism.

---

# v0.9.4 HMI VSYNC / double-buffer flicker hotfix

This release fixes continuous visible flicker observed after the v0.9.3 HMI became functional. Two display-path causes were corrected together:

1. The TLI clock recipe used `PLL2R / 8`, which produced about **12.5 MHz** pixel clock with the actual 25 MHz HXTAL and PLL2 settings. With the 887×519 total timing that is only about **27.15 Hz** panel refresh. v0.9.4 uses `PLL2R / 4`, producing **25.0 MHz / ~54.3 Hz**.
2. The previous LVGL path used one FULL render buffer and IPA copied the complete frame into the framebuffer while TLI was actively scanning that same framebuffer. v0.9.4 uses **two full RGB565 framebuffers**, `LV_DISPLAY_RENDER_MODE_DIRECT`, and switches the TLI framebuffer only during **frame blank (VSYNC-safe page flip)**. Only the final LVGL flush area triggers a page flip.

Memory map remains inside the first 4 MiB Normal Non-cacheable SDRAM MPU region:

```text
0xC0000000  FB0  800x480 RGB565
0xC0100000  FB1  800x480 RGB565
0xC0200000  LVGL heap 512 KiB
```

Expected startup breadcrumb:

```text
[I][LCD] TLI ready 800x480 RGB565; PCLK=25.0MHz refresh=54.3Hz double-buffer VSYNC; backlight ON
[I][GUI] LVGL display PASS direct-double FB0=0xC0000000 FB1=0xC0100000 bytes=768000
```

The LCD statistics retain `flush/timeout/error` and now also track TLI FIFO/transfer errors and the active framebuffer. A nonzero page-flip timeout or TLI error count after the fix is actionable evidence of a remaining TLI/SDRAM timing problem rather than ordinary LVGL redraw activity.

---

# v0.9.3 HMI MPU / HardFault diagnostic hotfix

This release fixes a real-board HardFault observed immediately after `TLI ready 800x480 RGB565; backlight ON`. The scheduler, SDRAM self-test and TLI were healthy; the fault occurred while entering the LVGL bring-up path.

The v0.9.1 external-LVGL-heap change had configured the first 4 MiB of SDRAM with Cortex-M7 MPU attributes `TEX=0,C=0,B=1`. That encoding is Device memory, not ordinary non-cacheable RAM. Framebuffer-style accesses can appear healthy, while a general-purpose allocator such as LVGL TLSF can perform accesses that are illegal or unsuitable for Device memory. The SDRAM window is now Normal non-cacheable memory using `TEX=1,C=0,B=0`, which remains coherent for TLI/IPA while preserving normal RAM semantics for the LVGL heap.

The GUI bring-up now logs explicit stages around `lv_init`, display creation and touch initialization. The HardFault path also prints stacked PC/LR plus CFSR/HFSR/MMFAR/BFAR and stack pointers over the polling debug UART. This makes any remaining board-specific fault directly actionable from UART logs.

Expected startup sequence after the fix:

```text
[I][LCD] SDRAM self-test PASS
[I][LCD] TLI ready 800x480 RGB565; backlight ON
[I][GUI] LVGL core init begin heap=0xC0200000 size=524288
[I][GUI] LVGL core init PASS
[I][GUI] LVGL display create begin
[I][GUI] LVGL display PASS ...
[I][GUI] touch init begin
[I][GUI] touch init PASS|DEGRADED
[I][GUI] creating HMI object tree
...
```

---

# GD32H759 Industrial Gateway — Unified Project Manual

> **Current authoritative firmware:** `v0.9.5-hmi-ipa-fastpath`  
> **Board:** GD32H759 industrial gateway  
> **Southbound:** CAN-FD 500 kbit/s, BRS OFF + RS485/Modbus RTU  
> **Network:** LAN8720A RMII + lwIP 2.1.2 + TCP management/uplink  
> **HMI:** 5-inch 800×480 capacitive touch + LVGL 9.2.2  
> **Configuration:** runtime CSV model + internal dual-slot Flash + optional SD/OSPI hooks  
> **Time:** SNTP UTC synchronization; PTP intentionally not enabled  
> **Operations:** watchdog, Syslog, compact read-only SNMP, diagnostics, offline replay  
> **Security baseline:** authenticated HMI/TCP management, fail-closed OTA framework, production-lock hook

This is the **only Markdown file in the project**. Everything below the v0.9.0 section is consolidated historical reference; where older behavior conflicts with the hotfix sections above, the **newest version wins**.

---

# 1. v0.9.0 release scope and status

v0.9.0 changes the gateway from a fixed demonstration configuration into a configurable edge gateway. The current runtime path is:

```text
CAN-FD / Modbus RTU / generic RS485
             │
             ├── protocol decode ──> Device Manager + Point DB
             │                              │
             │                              ├── alarm engine
             │                              └── local linkage rules
             │
             └── normalized raw event envelope
                            │
                            ▼
               persistent offline spool
                            │
                            ▼
             authenticated GW-JSONL :5001
                            │
              ┌─────────────┴─────────────┐
              ▼                           ▼
          upper host                    HMI
              │                           │
              └────── command router ─────┘
                         │
                    CAN / Modbus
```

The following requested functions are implemented in application software:

- runtime network/RS485/device/point/CAN-map/poll/alarm/rule configuration;
- persistent internal-Flash configuration and offline black-box queue;
- optional external SD/OSPI configuration hooks;
- remote point-map CRUD through authenticated TCP `:5001`;
- HMI factory reset and a board hook for a physical 3-second reset button;
- SNTP synchronization and UTC timestamps;
- high/low/rate alarms and high-priority alarm uplink;
- local CAN or Modbus-FC06 linkage rules;
- business-level hardware watchdog supervision;
- UDP Syslog and a compact read-only SNMP GET service;
- offline event/time-series replay with `history=true`;
- HMI/TCP credential authentication and lockout;
- OTA management/state/hash/signature integration framework that **fails closed** without board security backends;
- 120-second HMI diagnostic trends and controller self-test framework;
- bounded Goodix I2C access, recovery, reprobe, and GUI degraded mode to address intermittent touch freezes.

## v0.9.1 HMI black-screen hotfix

The v0.9.0 HMI grew substantially (authentication overlay, diagnostics trends, maintenance controls) while LVGL still used a 128 KiB internal-RAM heap. Because the panel backlight was enabled only after the first successful LVGL flush, an allocation assert before that first flush could look exactly like a dead LCD. v0.9.1 moves the LVGL allocator to a dedicated 512 KiB SDRAM pool at `0xC0200000`, keeps framebuffer/draw-buffer/heap regions non-overlapping, brings up a visible boot surface immediately after TLI initialization, and logs SDRAM/TLI/LVGL heap stages. The GUI then forces the first refresh immediately.

Expected UART sequence includes `SDRAM self-test PASS`, `TLI ready ... backlight ON`, `creating HMI object tree`, an `LVGL heap=...` line, and finally `HMI ready ...`.

## 1.1 Features intentionally not represented as hardware-complete

Several functions require hardware or a separate trusted boot image and are therefore exposed as explicit integration points rather than being falsely reported as production-certified:

- **PTP / IEEE 1588:** not enabled in v0.9.0. SNTP is the active time baseline. Hardware-timestamp PTP requires a validated PHY/MAC timestamp path.
- **SD/OSPI file backend:** internal MCU Flash persistence is active. Weak hooks are present for `/config.csv` on SD/OSPI, but the exact storage hardware was not specified.
- **Secure boot / encrypted signed OTA:** the application has OTA commands, streaming hash verification and fail-closed signature/staging hooks. A protected bootloader, key provisioning, decryption backend, rollback/anti-rollback policy, and board-specific staging storage must still be integrated and validated.
- **SWD/JTAG production protection:** optional Level-1 read-protection logic is disabled by default. Irreversible/high protection is not automatically applied by this development build and belongs in controlled manufacturing provisioning.
- **Active CAN/RS485 physical loopback:** software self-test exists, plus weak fixture hooks. Never short CAN_H to CAN_L or RS485 A to B. Active physical-layer tests require a proper fixture/second transceiver/node or board-specific safe loopback arrangement.
- **Internal-Flash wear/power-loss qualification:** dual-slot config and ring spool are implemented, but erase/program endurance and power-fail behavior still need target hardware qualification before production release.

---

# 2. Runtime configuration system

The old `gateway_build_config.h` values remain **factory defaults and compile-time feature gates**. Operational configuration is loaded at boot and may be changed without reflashing.

Boot priority:

```text
optional external config.csv hook
        ↓ if absent/not supported
internal Flash dual-slot config
        ↓ if absent/invalid
compiled factory defaults
```

A full CSV import is transactional: the current model is exported to a rollback snapshot first; if any replacement row fails, the previous device/point/map/poll/alarm/rule model is restored instead of leaving a half-applied configuration.

Runtime NET and RS485 changes are consumed by the network and RS485 owner tasks. Network changes reconfigure DHCP/static IPv4, while RS485 changes reinitialize the UART line configuration without requiring a firmware rebuild.

## 2.1 Supported CSV rows

```text
NET,dhcp,ip0,ip1,ip2,ip3,mask0,mask1,mask2,mask3,gw0,gw1,gw2,gw3
RS485,baud,data_bits,parity,stop_bits,timeout_ms,retry
SERVICE,sntp_server,syslog_server,syslog_port,min_level,snmp_community,reserved
AUTH,username,salt_hex,password_hash_hex
DEVICE,id,name,protocol,interface,address,timeout_ms,retry,enabled
POINT,id,device_id,name,type,scale,offset
CANMAP,id,device_id,point_id,can_id,byte_offset,encoding,endian,extended,require_fd,enabled
POLL,id,device_id,point_id,function,start_address,quantity,register_offset,interval_ms,encoding,enabled
ALARM,id,point_id,kind,threshold,hysteresis,priority,enabled
RULE,id,point_id,op,threshold,hysteresis,required_device,action,cooldown_ms,can_id,extended,fd,hex_payload,mb_slave,mb_register,mb_value,enabled
```

Reference example: `tools/sample_config.csv`.

`AUTH` is exported by `CFGGET`, but normal password changes should use `PASS,<user>,<new-password>` so operators do not need to manipulate salted hashes manually.

## 2.2 Remote configuration through TCP :5001

Authentication is required before management commands when `GW_AUTH_ENABLE=1`.

```text
AUTH,admin,<password>
CFGGET
CFGSET,POINT,2001,2,temperature,5,0.1,0
CFGSET,CANMAP,1,2,2001,0x301,0,2,0,0,1,1
CFGDEL,POINT,2001
CFGSAVE
```

`CFGSET` is an upsert of one CSV row. `CFGDEL` supports `DEVICE`, `POINT`, `CANMAP`, `POLL`, `ALARM`, and `RULE`. Deleting a point also removes dependent CAN maps, poll jobs, alarms and linkage rules; deleting a device cascades its points and dependents.

Autosave is enabled with a 1.5-second debounce. `CFGSAVE` requests an immediate persistent save cycle.

## 2.3 Internal persistent layout

The Keil application image is capped at `0x08000000 + 0x00380000`, leaving `0x08380000..0x083BFFFF` for gateway NVM:

```text
0x08380000  config slot A   32 KiB
0x08388000  config slot B   32 KiB
0x08390000  offline spool  192 KiB
0x083C0000  end
```

Config slots use generation numbers, CRC, and commit-last metadata. The offline spool uses fixed 1024-byte records with up to 976 JSON bytes per record.

Optional board backends can override:

```c
gw_config_external_load_csv()
gw_config_external_save_csv()
gw_config_external_factory_reset()
```

so the same config service can use SD/OSPI later without changing Point/Device/CAN/Modbus code.

## 2.4 Factory reset

HMI Service page provides a guarded factory-reset action. A board may also implement:

```c
bool gw_factory_button_pressed(void);
```

Holding the physical hook for at least 3 seconds requests reset. Factory reset removes external configuration through its hook, erases internal config, optionally clears the offline spool, then resets the MCU.

---

# 3. UTC time synchronization

`gw_time` separates monotonic MCU time from wall-clock UTC. Protocol/Point timestamps use UTC after synchronization and retain a monotonic fallback before time is available.

SNTP is automatically started after `EVT_NET_IP_READY`, uses the configured `SERVICE` server, and periodically resynchronizes. The default is:

```text
server   pool.ntp.org
period   3600000 ms
```

`EVT_TIME_SYNCED` marks valid UTC. HMI displays synchronized UTC when available.

**PTP is disabled (`GW_PTP_ENABLE=0`) in this release.** If future process requirements need microsecond-class synchronization, add a separately validated hardware-timestamp PTP path rather than treating software SNTP as equivalent.

---

# 4. Alarm and local linkage engine

Alarms are evaluated after committed Point DB updates and are queued ahead of ordinary telemetry.

Alarm kinds:

```text
0  HIGH
1  LOW
2  RATE_HIGH
```

Each alarm has threshold, hysteresis, priority and enable state. Raise/clear transitions are published as `kind:"alarm"` high-priority events.

Example:

```text
ALARM,1,2001,0,80,2,7,1
```

means Point 2001 high alarm at 80 with 2 units hysteresis and priority 7.

Local rules support `GT/GE/LT/LE`, hysteresis, optional required-device-online state, cooldown and latch behavior. Actions use the **same command router as HMI/TCP**, so automation cannot bypass CAN/RS485 ownership rules.

Actions:

```text
0  CAN command
1  Modbus RTU FC06 command
```

Example: temperature Point 2001 > 80 while Device 2 is online, then send CAN ID `0x302`:

```text
RULE,1,2001,0,80,2,2,0,5000,0x302,0,1,475700000001,0,0,0,1
```

The production CAN invariant still forces **BRS OFF**.

---

# 5. Runtime watchdog and fault containment

The watchdog is not a single unconditional `FWDGT` refresh in `main()`. A high-priority supervisor feeds the hardware only if every enabled required service has reported progress within its deadline.

Supervised channels include configuration, RS485, data processing, CAN, poll scheduler, network, uplink and GUI according to feature enablement. Default policy:

```text
startup grace     8 s
business stale    4 s
supervisor poll   250 ms
hardware timeout  ~6 s
```

Long RS485 transactions are waited in bounded slices so a legitimate Modbus timeout does not falsely trip the system watchdog. If GUI initialization fails, the GUI task enters degraded mode and continues its watchdog heartbeat instead of reboot-looping the entire gateway because of a bad display/touch peripheral.

---

# 6. Remote operations: Syslog and SNMP

## 6.1 Syslog

Warning/Error logs are still printed to UART2 and are additionally queued for UDP Syslog on the configured server/port (default UDP 514).

`SERVICE` field `min_level`:

```text
0  Error only
1  Warning + Error
```

Syslog is best-effort; a network outage must not block real-time CAN/RS485 tasks.

## 6.2 Compact SNMP GET service

A small read-only community-based SNMP GET service listens on UDP 161. It intentionally implements a small management subset rather than adding a large management stack.

Private prefix:

```text
1.3.6.1.4.1.55555.1.<metric>
```

Metrics:

| Metric | Value |
|---:|---|
| 1 | uptime seconds |
| 2 | CPU load, permille |
| 3 | FreeRTOS free heap bytes |
| 10 | Ethernet RX frames |
| 11 | Ethernet TX frames |
| 20 | CAN load, permille |
| 21 | CAN error total |
| 30 | RS485 load, permille |
| 31 | RS485 loss, permille |
| 32 | RS485 error total |

The community is runtime-configurable. This service is for simple plant monitoring; it is **not SNMPv3 security**.

---

# 7. Offline black-box spool and replay

When there is no **authenticated** :5001 uplink client, normalized raw frames, Point data and alarm/event JSON records are persisted into the internal ring spool.

When the network/client returns:

```text
current snapshot
→ high-priority live alarms
→ oldest offline records (bounded replay burst)
→ live raw frames / dirty points
```

Replayed records carry:

```json
"history":true
```

so the host can distinguish historical recovery from current live events. Point dirty acknowledgements are revision-aware: an ACK for an older snapshot cannot accidentally clear a newer Point update that arrived while the record was being transmitted.

The default replay burst is 4 records per scheduling pass so historical backlog does not permanently starve live control/telemetry.

---

# 8. Authentication and security baseline

## 8.1 TCP :5001 authentication

Factory development credential:

```text
user      admin
password  ChangeMe123!
```

**Change this before deployment.** Passwords are stored as salted SHA-256 hashes in persistent configuration. Authentication uses failure counting and a 60-second lockout after 5 failed attempts.

Typical sequence:

```text
AUTH,admin,ChangeMe123!
PASS,admin,<new-password>
CFGGET
```

The HMI has the same operator credential/session model and a 15-minute idle session timeout.

Important limitation: **TCP :5001 authentication is credential authentication, not transport encryption.** Credentials and data are not protected against network sniffing until a TLS/VPN/segmented trusted network layer is added.

## 8.2 OTA command framework

Supported authenticated commands:

```text
OTA_BEGIN,<version>,<size>,<sha256_hex>,<signature_hex>,<encrypted:0|1>
OTA_DATA,<offset>,<hex_data>        # up to 256 bytes per command
OTA_END
OTA_ABORT
OTA_STATUS
```

The OTA layer tracks offsets, re-reads staged data, verifies SHA-256 and requires a signature backend before marking an image pending.

The default board hooks deliberately return `NOT_SUPPORTED` / signature failure. Therefore a development build **cannot accept an unverifiable image**. Product integration must provide protected key verification, staging read/write, optional decryption, a secure bootloader and rollback/anti-rollback behavior.

## 8.3 Production debug protection

`GW_PRODUCTION_LOCK_ENABLE` is `0` by default and a separate confirmation value is required before the application applies Level-1 read protection. High/irreversible protection is intentionally not automated in this project. Provision and verify debug protection in the manufacturing process only after recovery/programming requirements are settled.

---

# 9. Diagnostics, trends and maintenance

The diagnostics task samples once per second and keeps 120 samples. HMI Service displays compact visual 120-second bars for:

```text
CPU load
CAN load
RS485 load
RS485 loss
```

It also exposes Ethernet counters, CAN errors, RS485 errors/loss, watchdog stale mask, storage/spool state and authentication failures.

`SELFTEST` or the HMI Service action runs controller/software baseline checks. Optional board fixture hooks are:

```c
gw_diag_fixture_can_loopback()
gw_diag_fixture_rs485_loopback()
```

Their default result is `NOT_SUPPORTED`, shown as `FIXTURE` rather than pretending a physical test passed.

**Safety:** never perform a test by directly shorting CAN_H to CAN_L or RS485 A to B. Use a proper field-bus test fixture, another transceiver/node, or a board-specific safe loopback topology.

---

# 10. Touch-screen freeze hardening

The intermittent Goodix-touch freeze path was redesigned so I2C/touch faults cannot indefinitely block the LVGL task:

- each touch I2C operation has a 4 ms bound;
- NACK, bus error, lost arbitration, overrun, PEC and timeout states are checked;
- recovery sends STOP/clear, deinitializes the controller, clocks up to 9 SCL recovery pulses, releases a passive STOP condition, and reinitializes I2C2;
- after 3 consecutive errors the Goodix controller is reset/reprobed with a 250 ms backoff;
- a touch device not detected during boot is periodically reprobed instead of being disabled forever;
- the LVGL pointer device is registered even if the initial probe fails, allowing later recovery;
- input errors report RELEASE to LVGL and never wait indefinitely;
- if GUI initialization itself fails, the task remains alive in degraded mode so the system watchdog does not turn an LCD failure into an endless whole-device reboot loop.

This is a software robustness fix. Final validation still requires prolonged operation on the actual 5-inch panel, including repeated touch, hot/noisy power, cable manipulation and Ethernet/CAN/RS485 concurrent load.

---

# 11. Current HMI information architecture

The HMI remains a restrained industrial dark interface rather than a demo dashboard:

```text
Overview   overall health / latest traffic
Devices    device online/offline/error state
Data       normalized Point DB + raw traffic
Control    authenticated CAN / Modbus operator control
Network    link, IPv4, Ethernet and TCP/uplink state
Service    health trends, watchdog, storage, auth, self-test, lock/reset
```

Control actions and maintenance actions use the same service APIs as remote commands; the GUI does not directly manipulate CAN or UART registers.

The historical PA8 hardware constraint still applies: the 5-inch LCD uses PA8 for TLI_R6. Ethernet and LCD together therefore require an independent/external 50 MHz LAN8720A RMII reference rather than PA8 MCO.

---

# 12. TCP :5001 management command reference

After authentication:

```text
PING
CAN,<id>,<len>,<hex>
MBR,<slave>,<register>,<quantity>
MBW,<slave>,<register>,<value>
MBRD,<device_id>,<register>,<quantity>
MBWD,<device_id>,<register>,<value>
CFGGET
CFGSET,<complete CSV row>
CFGDEL,<DEVICE|POINT|CANMAP|POLL|ALARM|RULE>,<id>
CFGSAVE
PASS,<user>,<new-password>
SELFTEST
FACTORY,YES
OTA_BEGIN,...
OTA_DATA,...
OTA_END
OTA_ABORT
OTA_STATUS
```

`MBR/MBW` address a real Modbus Slave ID directly and do not require a Device Manager entry. `MBRD/MBWD` are the configured-device variants.

PC helper:

```text
tools/gateway_uplink_client.py
```

Example:

```powershell
py .\tools\gateway_uplink_client.py --host 192.168.103.213 --ask-password --command CFGGET --seconds 2
```

---

# 13. Build-size / compile-time status

The earlier GUI optimization remains in place. Current v0.9.0 adds management services but still stays far below the original full demo/vendor build:

```text
Keil C translation units   179
LVGL C translation units    81
lwIP C translation units    31
Markdown files               1
```

The v0.7 development tree had about 314 C translation units and 223 LVGL units. Keep using **Build Target** for normal code/UI changes and reserve **Rebuild All** for compiler/linker/vendor/config composition changes.

---

# 14. v0.9.0 automated verification status

The release package was checked with the project host regression suite and ARM-target syntax configuration:

```text
modbus regression                                      PASS
can decoder regression                                 PASS
point/device regression                                PASS
CAN mailbox IRQ regression                             PASS
CAN-FD 500k BRS-OFF stable baseline regression        PASS
project stable-baseline regression                     PASS
Ethernet/lwIP baseline regression                      PASS
TCP v0.6.1 Netconn baseline regression                PASS
GUI/LVGL optimized HMI regression (81 LVGL sources)   PASS
v0.9 edge-management regression (179 C / 81 LVGL)     PASS
Modbus direct FC03/FC06 regression                     PASS
all user C + 31 lwIP + LVGL + required SPL syntax     PASS
high-risk changed-file Clang static analysis           PASS
```

These checks do not replace Keil ArmClang linking or target hardware validation.

## 14.1 Required target validation before treating v0.9.0 as production-ready

At minimum run:

1. Keil `Clean Target` + `Rebuild All` and verify Flash/SRAM/SDRAM margins.
2. 24-hour CAN-FD 500k BRS-OFF + RS485 + Ethernet + HMI concurrent soak.
3. Touch stress with repeated taps/drags and deliberate I2C/panel interruption; confirm recovery without whole-system reboot.
4. SNTP sync/re-sync and network-loss/recovery tests.
5. Dynamic config CRUD, invalid full-document rollback, reboot persistence and factory reset.
6. Offline spool by disconnecting authenticated :5001, generating traffic, reconnecting and checking ordered `history=true` replay.
7. Alarm/rule actuation against safe test loads only.
8. Watchdog fault injection by intentionally stalling each supervised task in a test build.
9. Syslog/SNMP validation under link loss and high southbound traffic.
10. Internal-Flash erase/program/power-loss/endurance qualification, or replace the weak external-storage hooks with the actual SD/OSPI medium.
11. Before OTA deployment, integrate and independently validate the secure bootloader/key/staging/decryption/rollback chain.
12. Provision production debug protection only after manufacturing recovery procedures are finalized.

---

# Consolidated historical reference — v0.8.1 and earlier

The remaining sections are preserved for design history and older validation records. They are **not authoritative when they conflict with the v0.9.0 section above**.


## Historical v0.8.1 Modbus RTU manual-control correction

This section supersedes older historical notes where the HMI/uplink `MBR` and `MBW` commands were described as taking an internal Device ID. Production defaults intentionally keep `GW_DEMO_MODBUS_CONFIG_ENABLE=0`, so manual control must not depend on a demo Device Manager entry.

- HMI **Slave ID** is the real Modbus RTU address `1..247`.
- `MBR,<slave_id>,<holding_register>,<quantity>` performs FC03 directly.
- `MBW,<slave_id>,<holding_register>,<value>` performs FC06 directly.
- If the slave is already configured in Device Manager, its device ID / timeout / retry policy is attached automatically. Otherwise the transaction runs as an ad-hoc operator command with the production default timeout/retry policy.
- `MBRD,<device_id>,<register>,<quantity>` and `MBWD,<device_id>,<register>,<value>` retain explicit configured-device addressing for automation/integration code.
- Modbus exception frames (`FC|0x80`) are accepted as 5-byte terminal responses even when the normal FC03/FC06 response would be longer.
- Manual ad-hoc transactions use `device=0` in the unified uplink envelope when no configured device matches; the real slave address remains in `addr`.

Example:

```text
MBR,1,0,2
MBW,1,10,123
```

The screen Control page now labels this field **Slave ID**, not Device.


> **Firmware baseline:** v0.8.1-unified-gateway-hmi  
> **Board:** GD32H759 industrial gateway  
> **Southbound:** CAN-FD 500 kbit/s, BRS OFF + RS485/Modbus RTU + generic RS485 frame envelope  
> **Network:** LAN8720A RMII + lwIP 2.1.2 + TCP  
> **HMI:** 5-inch 800×480 capacitive touch + LVGL 9.2.2  
> **Unified uplink:** TCP :5001, GW-JSONL-1

This is the **only Markdown document in the project**. Historical `.md` files from v0.2.x through v0.7.0 are consolidated verbatim in the archive section at the end of this document.

---

## 1. v0.8.0 purpose

v0.8.0 changes the project from a collection of board-validation branches into one gateway-oriented architecture:

```text
CAN-FD 500k BRS-OFF ─┐
                      ├─> protocol adapters ─> Point DB / Device Manager ─┐
RS485 / Modbus RTU ───┘                                                   │
                                                                          ├─> GW-JSONL uplink :5001 ─> upper host
raw CAN/Modbus/RS485 traffic ─> common gw_uplink_event_t envelope ─────────┘

Touch HMI ──> gw_command_router ──> CAN / Modbus drivers ──> field devices
Upper host ─> command parser :5001 ─┘
```

The important boundary is that **southbound drivers do not know MQTT/HTTP/SCADA formats**. They produce normalized Point data plus a common raw-frame envelope. Northbound protocols consume those objects later.

---

## 2. Build-time and project-size optimization

The largest source of the long rebuild time was LVGL: the v0.7.0 Keil project compiled almost the entire LVGL source tree although the gateway HMI uses only a small subset.

### 2.1 Verified project reduction

| Item | v0.7.0 | v0.8.0 | Reduction |
|---|---:|---:|---:|
| Keil C translation units | 314 | 165 | 47.5% |
| LVGL C translation units | 223 | 81 | 63.7% |
| lwIP C translation units | 39 | 30 | 23.1% |
| Project working-tree size in this environment | ~39 MiB | ~19 MiB | ~51% |
| LVGL tree size in this environment | ~27 MiB | ~6.6 MiB | ~76% |

A repeatable parallel host syntax compile of the LVGL project set changed from **9.57 s to 4.07 s** in one A/B run in this environment (about 57% faster). That is only a relative engineering measurement; the exact Keil rebuild time depends on PC, antivirus, disk, ArmClang version and whether a full rebuild is requested.

### 2.2 What was removed from the active build

Only HMI-required LVGL features remain enabled:

- base objects/core
- label
- button
- button matrix (keyboard dependency)
- textarea
- keyboard
- RGB565 software rendering
- Montserrat 14/16/20/24 fonts

Large unused widget families such as table, tabview, chart, menu, list, calendar and demo code are disabled. Inactive LVGL `.c` files and the bundled examples directory are removed from the deliverable. lwIP source files that preprocess to empty objects under the current `lwipopts.h` are removed from the Keil source group.

### 2.3 Incremental-build guidance

For normal development use **Build Target**, not **Rebuild all target files**. Use a full clean/rebuild only after changing compiler options, `lv_conf.h`, `lwipopts.h`, linker/MPU settings or vendor library composition.

---

## 3. Stable hardware / transport baseline

### CAN-FD

```text
CAN-FD      ON
BRS         OFF
Bit rate    500 kbit/s for the whole frame
TDC         OFF
Payload     up to 64 bytes subject to legal CAN-FD DLC lengths
```

The BRS-OFF baseline is deliberately frozen because hardware tests showed stable TX/RX with TEC/REC at zero, while higher-rate BRS operation had physical-layer margin problems.

### Ethernet

```text
PHY         LAN8720A
Interface   RMII
IPv4        192.168.103.213/24 (static baseline)
TCP echo    :5000
GW-JSONL    :5001
```

**Hardware constraint:** the 5-inch LCD uses PA8 as TLI_R6. The combined LCD + Ethernet build therefore requires the LAN8720A RMII 50 MHz reference clock to come from an external/independent source; PA8 cannot simultaneously output CKOUT0.

### HMI

```text
LCD         800×480 RGB565
Controller  GD32H759 TLI + IPA
Touch       Goodix capacitive touch over I2C2
GUI         LVGL 9.2.2 minimal configuration
```

---

## 4. Unified southbound data model

There are two complementary representations.

### 4.1 Parsed process data: Point DB

Protocol decoders convert configured device data into `gw_point_t` values. Point DB provides value type, quality, timestamp, revision and dirty state. CAN and Modbus therefore become protocol-independent before upper-layer publication.

Example normalized point record sent to the upper host:

```json
{"v":1,"kind":"point","mode":"delta","if":"canfd_0","proto":"canfd","device":2,"point":2001,"name":"temperature","ts":123456,"quality":"good","type":"f32","value":25.000,"rev":42}
```

The same shape is used for a Modbus-derived point; only `if`, `proto`, device/point metadata and value differ.

### 4.2 Raw/diagnostic traffic: `gw_uplink_event_t`

Every supported southbound adapter can publish the same envelope:

- interface ID
- protocol
- RX/TX direction
- device ID
- timestamp
- bus address / CAN ID
- function/code
- flags
- result/error code
- byte length
- raw bytes

CAN RX/TX, Modbus RTU TX/RX and generic RS485 transaction traffic use this path. Modbus CRC/protocol-error frames preserve received wire bytes; timeout/error completions are represented even when no response bytes exist. Generic RS485 uses `proto=rs485_raw` instead of an ambiguous `none` protocol tag.

Example:

```json
{"v":1,"kind":"frame","seq":77,"dir":"rx","if":"rs485_0","proto":"modbus_rtu","device":1,"ts":123500,"addr":1,"code":3,"flags":0,"result":0,"len":9,"data":"01030400C8012Dxxxx"}
```

`gw_uplink_publish_event()` is the generic ingress API for future southbound protocols, so adding another field interface should not require inventing another upper-host format.

---

## 5. Upper-host unified uplink — TCP 5001

The gateway exposes a single-client newline-delimited JSON stream on TCP port **5001**.

On connection it sends:

1. hello record
2. current Point DB snapshot
3. subsequent dirty point deltas
4. live CAN/Modbus/generic-RS485 frame events
5. command ACK records

This is intentionally a transport/normalization baseline rather than the final MQTT or HTTP schema. It gives us one verified internal model before adding higher-level northbound protocols.

### PC monitor tool

```powershell
py .\tools\gateway_uplink_client.py --host 192.168.103.213 --port 5001
```

Send a command while monitoring:

```powershell
py .\tools\gateway_uplink_client.py --command PING
py .\tools\gateway_uplink_client.py --command "CAN,0x302,6,475700000001"
py .\tools\gateway_uplink_client.py --command "MBR,1,0,2"
py .\tools\gateway_uplink_client.py --command "MBW,1,0,123"
```

Command grammar (one ASCII line per command):

```text
PING
CAN,<can_id>,<length>,<hex_without_spaces>
MBR,<slave_id>,<holding_register>,<quantity>
MBW,<slave_id>,<holding_register>,<value>
```

CAN commands always keep BRS disabled in the production router. Modbus commands resolve `device_id` through Device Manager; arbitrary unconfigured slaves are not silently accessed.

---

## 6. Touch HMI

The v0.8 HMI is deliberately restrained: dark graphite surfaces, consistent spacing, a restrained teal interaction accent and semantic green/amber/red status colors. It avoids dashboard gradients, oversized cards and decorative effects that do not help an operator.

Pages:

1. **Overview** — device/point/CAN/network KPIs, health and traffic
2. **Devices** — configured endpoints and runtime state
3. **Data** — normalized Point DB values plus the latest southbound bus frames
4. **Control** — field-device command console
5. **Network** — Ethernet, TCP :5000 and unified uplink :5001 status/counters

### HMI command capabilities

**CAN-FD:** enter standard/extended CAN ID and hexadecimal payload, then send. The router forces BRS OFF.

**Modbus RTU:** choose Slave ID (1..247) and register. The HMI supports:

- FC03 read holding registers (`quantity` 1..125)
- FC06 write single holding register

Both HMI and upper-host commands go through `gw_command_router`; they do not bypass the existing bus ownership, timeout, retry or stable CAN protections.

---

## 7. Concurrency / reliability changes in v0.8

- HMI and upper-host command sequence/transaction counters are protected against concurrent callers.
- RS485/Modbus traffic is captured at the bus-owner task, so successful responses, exceptions, CRC/protocol failures, retries and terminal timeout/I/O failures share the same normalized event path without double-publishing in `task_data`.
- The normalized uplink does not accumulate stale raw traffic while no upper-host client is connected.
- On uplink connect, Point DB sends a current snapshot; dirty ACK uses point revision so a newer update cannot be accidentally cleared by an old ACK.
- GUI never calls CAN/RS485 low-level register code directly; it submits through the command router.
- GUI is low priority and refreshes business data without moving protocol decoding into the LVGL task.
- Recent bus history shown on the HMI is bounded.

---

## 8. Build configuration highlights

Production defaults remain conservative. Relevant macros live in `user/config/gateway_build_config.h`:

```c
#define GW_CANFD_BRS_ENABLE               0U
#define GW_GUI_ENABLE                     1U
#define GW_ETH_ENABLE                     1U
#define GW_TCP_SERVER_ENABLE              1U
#define GW_TCP_SERVER_PORT                5000U
#define GW_UPLINK_ENABLE                  1U
#define GW_UPLINK_PORT                    5001U
#define GW_ETH_RMII_REFCLK_PA8_MCO        0U
#define GW_ETH_RMII_REFCLK_EXTERNAL_50M   1U
```

Compile-time guards reject a TCP/uplink port collision and the LCD/PA8 Ethernet-clock conflict.

---

## 9. Validation status and next hardware tests

Automated host checks for the v0.8 source tree include:

- Modbus regression
- CAN decoder regression
- Point/Device regression
- CAN IRQ/safe-hold structural regression
- CAN-FD 500k BRS-OFF configuration regression
- Ethernet/lwIP structural regression
- TCP Netconn regression
- optimized LVGL/HMI regression
- v0.8 unified data/command architecture regression
- protocol/interface binding regression (CAN/CAN-FD, Modbus/RS485, generic RS485)
- Clang static analysis of modified gateway/HMI/transport modules
- syntax checking of all project-owned C sources
- syntax checking of active lwIP/LVGL sources and ENET/I2C/EXMC/TLI/IPA SPL modules

The following still require the actual GD32H759 board and Keil/ArmClang final link:

1. LCD + touch + Ethernet concurrent operation with external RMII 50 MHz
2. HMI CAN and Modbus command transmission to real devices
3. TCP :5001 snapshot/delta/frame stream against the PC client
4. CAN + RS485 + Ethernet + TCP + GUI concurrent soak test
5. stack/heap high-water marks and long-duration memory-leak observation

---

## 10. Recommended next northbound milestone

After v0.8 board validation, build MQTT/Modbus-TCP/HTTP adapters **on top of Point DB + `gw_uplink_event_t`**, rather than parsing CAN/RS485 again. The internal normalized model should remain the stable contract.

---

# Consolidated historical documents

The sections below preserve the previous Markdown documentation in one file. Paths are retained so historical references remain traceable.


---

## Archived source: `PROJECT_MANUAL.md`

# GD32H759 Industrial Gateway — Project Manual

> Current engineering baseline: **v0.8.0-unified-gateway-hmi**  
> This is the **only Markdown document** retained in the project. Historical `.md` files are preserved verbatim in the archive section below.

## 1. Current baseline

The current baseline consolidates the validated southbound, network and HMI work into one gateway architecture:

- **CAN-FD:** 500 kbit/s, FD payload enabled, **BRS OFF**, TDC OFF.
- **RS485:** UART4, TX DMA + RX DMA/IDLE, Modbus RTU master/polling.
- **Data model:** Device Manager + Point DB with quality, timestamp, revision and dirty-ACK semantics.
- **Ethernet:** ENET0 + LAN8720A RMII + lwIP 2.1.2.
- **TCP:** validated echo service on **port 5000** for transport regression.
- **Unified northbound stream:** **GW-JSONL on port 5001**.
- **GUI:** 5-inch 800x480 RGB565, TLI + IPA + SDRAM, LVGL 9.2.2, Goodix capacitive touch.
- **HMI command path:** screen and upper-host commands share one `gw_command_router`.

### Hardware constraint: LCD + Ethernet

The official 5-inch LCD pin map uses **PA8 = TLI_R6**. The earlier Ethernet bring-up used PA8/CKOUT0 as the LAN8720A 50 MHz RMII reference. Full LCD + Ethernet operation therefore requires an **external/independent 50 MHz RMII reference clock** for the PHY. The build configuration contains a compile-time guard for this conflict.

## 2. Build / project-size optimization

The v0.7.0 Keil project carried **223 LVGL C translation units** and about **314 C files** in the target. The v0.8.0 target is trimmed to the actual product feature set:

- LVGL project sources: **81** (223 -> 81, about 63.7% fewer).
- Total Keil C sources: **165** (about 47.5% fewer than the v0.7.0 project list).
- Disabled/unused LVGL widgets, themes, Flex layout and Montserrat 28 are not compiled.
- The vendored LVGL tree was trimmed to the source set needed by this product baseline; examples/demos and unused generated font sources are removed.
- Build outputs and Python `__pycache__` are not part of the release package.

For normal development use **Build Target** instead of **Rebuild All**. Rebuild All necessarily recompiles vendor stacks and remains slower; ordinary application/HMI edits should only rebuild changed translation units and affected dependents.

## 3. Unified gateway data path

```text
CAN-FD --------> CAN decoder -----> Point DB ----+
      \---- raw frame --------------------------+----> GW-JSONL :5001 ----> Upper host
                                                  |
RS485/Modbus --> RTU parser/poll -> Point DB ----+
      \---- raw request/response ---------------+

Upper host :5001 command ----+
                              +--> gw_command_router --> CAN / Modbus RTU
Touch HMI command ------------+
```

### 3.1 Common raw-event envelope

All currently supported southbound frame adapters publish through `gw_uplink_event_t`. New interfaces should use `gw_uplink_publish_event()` instead of inventing a separate northbound schema.

Common fields include:

- sequence number
- interface (`canfd_0`, `rs485_0`, ...)
- protocol (`canfd`, `modbus_rtu`, ...)
- direction (`rx` / `tx`)
- device ID
- timestamp
- address / CAN ID / slave address
- protocol code / function code
- flags
- result
- payload length and raw bytes

Example frame record:

```json
{"v":1,"kind":"frame","seq":42,"dir":"rx","if":"canfd_0","proto":"canfd","device":0,"ts":123456,"addr":769,"code":0,"flags":2,"result":0,"len":12,"data":"00FA00000000000000000000"}
```

### 3.2 Normalized Point DB records

Protocol-specific decoders map values to Point DB. The uplink sends initial point snapshots and dirty deltas using the same common identity/timestamp/protocol context.

```json
{"v":1,"kind":"point","mode":"delta","if":"canfd_0","proto":"canfd","device":10,"point":2001,"name":"temperature","ts":123456,"quality":"good","type":"f32","value":25.000,"rev":18}
```

Point `revision` is used when clearing dirty state so an ACK for an older snapshot cannot erase a value that changed during transmission.

## 4. Upper-host command protocol

Connect to **192.168.103.213:5001**. Commands are ASCII lines terminated with `\n`.

```text
PING
CAN,0x302,6,475700000001
MBR,1,0,2
MBW,1,0,123
```

- `CAN,<id>,<len>,<hex>`: send a CAN-FD frame. Production driver forces BRS OFF.
- `MBR,<slave_id>,<register>,<quantity>`: Modbus RTU FC03.
- `MBW,<slave_id>,<register>,<value>`: Modbus RTU FC06.

Responses use JSONL ACK records:

```json
{"v":1,"kind":"ack","cmd":"MBR","ok":1,"err":0}
```

Use `tools/gateway_uplink_client.py` to monitor the stream and send the same commands from a PC.

## 5. HMI design

The HMI is intentionally styled as a compact industrial console rather than a demo/dashboard template:

- dark neutral background with restrained teal status accent
- fixed top status bar and left navigation
- **Overview:** service health + recent live bus activity
- **Devices:** configured endpoint state and counters
- **Data:** normalized Point DB plus recent CAN/RS485 frames
- **Control:** CAN-FD transmit and Modbus RTU FC03/FC06 controls
- **Network:** Ethernet, TCP and normalized uplink status/counters
- one shared touch keyboard; LVGL is called only from the GUI task

The screen uses the same `gw_command_router` as the upper host, so local and remote control do not bypass device validation or create parallel driver ownership paths.

## 6. Current validation commands

Host regression:

```bash
bash tools/host_tests/run_host_tests.sh
```

TCP echo regression:

```powershell
py .\tools\tcp_echo_test.py --host 192.168.103.213 --port 5000 --rounds 20 --size 1024
```

Unified uplink monitor / command client:

```powershell
py .\tools\gateway_uplink_client.py --host 192.168.103.213 --port 5001
py .\tools\gateway_uplink_client.py --command PING --seconds 3
py .\tools\gateway_uplink_client.py --command "CAN,0x302,6,475700000001" --seconds 3
py .\tools\gateway_uplink_client.py --command "MBR,1,0,2" --seconds 3
```

## 7. Production defaults and next protocol work

Production/demo switches should remain off unless doing board validation. The unified event and command layers are the intended boundary for subsequent MQTT, HTTP or Modbus TCP work; those protocols should consume Point DB / `gw_uplink_event_t` and `gw_command_router`, not access CAN/RS485 drivers directly.

---

# Historical documentation archive

The sections below are the contents of every Markdown file that existed before v0.8.0 consolidation. Their original paths are retained as headings. They are historical unless a statement is repeated in the authoritative sections above.


---

## Archived file: `AUDIT_FIX_REPORT_V0.4.5.md`

# Gateway v0.4.5 Software Audit & Fix Report

## Scope

The active Keil project (`project/gateway.uvprojx`) and its user-layer gateway code were audited end-to-end, with emphasis on ownership/lifetime, FreeRTOS concurrency, RS485 DMA/IDLE, Modbus RTU correlation, Device/Point state transitions, CAN-FD mailbox/DLC handling, CAN signal decoding and numeric conversion.

The uploaded baseline already contained a Keil build log with **0 errors / 77 warnings**. Most recorded warnings were in vendor FreeRTOS/GD32 code or warning-policy noise; functional correctness was therefore checked beyond simple build success.

## Fixed defects

1. **CAN-FD mailbox/API payload mismatch (high)**
   - Public frames allowed up to 64 bytes while hardware mailboxes were configured for 16 bytes.
   - GD32 SPL derives DLC from the requested length but copies only the configured mailbox payload, so long frames could be transmitted truncated while advertising a larger DLC.
   - Mailbox storage is now 64 bytes.

2. **Illegal CAN-FD payload lengths accepted (high)**
   - Lengths such as 9, 10, 11, 13, 14 and 15 were accepted even though ISO CAN-FD DLC can represent only 0..8, 12, 16, 20, 24, 32, 48 and 64 bytes.
   - GD32 SPL rounds such lengths upward, which can expose unintended tail bytes.
   - TX now rejects non-representable lengths and rejects BRS/ESI on Classic CAN frames.

3. **CAN U32/I32 precision loss and unsafe integer conversion (high)**
   - Raw 32-bit CAN values were converted through `float`, losing integer precision (for example `0xFFFFFFFF`).
   - NaN/out-of-range float-to-integer conversion could also produce invalid/undefined results.
   - Decoding now uses `double`, validates finite/range constraints, and only then converts to the configured Point type.

4. **CAN signal-map schema was insufficiently validated (medium/high)**
   - Registration could accept missing/mismatched devices or points, invalid endian values, and mappings whose offset+width exceeded the 64-byte CAN-FD payload.
   - Registration now verifies Device protocol/interface, Point ownership, endian, ID range and signal bounds before activation.
   - CAN timeout watcher capacity now follows `GW_MAX_DEVICES` rather than an unrelated hard-coded 16-device limit.

5. **CAN TX safe-hold queue race (medium/high)**
   - A producer blocked in `xQueueSend()` could complete after CAN safe-hold had reset the TX queue, leaving a frame stranded during hold.
   - Submit now rechecks hold state after enqueue and discards queued work if hold was asserted concurrently.

6. **Unexpected CAN TX mailbox terminal states were silently ignored (medium)**
   - Non-INACTIVE/non-ABORT terminal states could clear the software active flag without a failure count or cleanup.
   - They are now counted as failed and the mailbox is aborted/inactivated defensively.

7. **Modbus FC01/FC02 response correlation gap (medium)**
   - Request-aware validation checked read byte counts for FC03/FC04 but not FC01/FC02.
   - A CRC-valid bit-read response with the wrong byte count could be accepted.
   - FC01/FC02 now derive expected bytes from requested quantity (1..2000 bits) and require an exact match. RS485 DMA expected-length inference was extended accordingly.

8. **RS485 transaction parameter holes (medium)**
   - Zero `device_id` could be submitted.
   - A Modbus transaction could specify an impossible non-zero expected response shorter than the 5-byte minimum exception frame.
   - Both are now rejected at the bus-manager boundary.

9. **Device-wide Point quality changes kept stale timestamps (medium)**
   - OFFLINE/STALE transitions changed quality and dirty state but not the Point timestamp.
   - Device-wide quality updates now carry the transition time, keeping value-quality-time tuples coherent.

10. **Disabled devices could be revived/mutated by late results (medium)**
    - `device_manager_report_success/failure()` modified counters and state even after a device was disabled.
    - Late transport events are now ignored while `DEVICE_DISABLED` is active.

11. **NaN/Inf could enter the Point database or poll conversion path (medium)**
    - Point registration/update accepted non-finite float/double values and non-finite scale/offset.
    - Poll decoding could cast NaN/Inf or overflow a target F32.
    - Point DB and poll conversion now reject non-finite/out-of-range values.

12. **Logging macro portability / format checking (low)**
    - GNU `, ##__VA_ARGS__` caused repeated ArmClang extension warnings.
    - Logging macros now use standard variadic forwarding, and the logger carries a printf-format attribute for caller-side format checking.

13. **Current README timing information was stale (documentation)**
    - README mixed historical 500k/5M/TDC settings with the current Stage2A default.
    - It now identifies Stage2A as 500k/2M, BRS on, TDC off; Stage2B/Stage3 remains the 5M+TDC gate.

## Verification performed

- Host regression suite: `tools/host_tests/run_host_tests.sh`
  - Modbus CRC, FC03 byte-count correlation, FC01 bit-count correlation, write echo and exception handling.
  - CAN U32 full-range decode, NaN rejection, invalid endian and out-of-payload signal mapping.
  - Point DB non-finite rejection, device-wide quality timestamping, offline/recovery transitions and disabled-device immutability.
- Tests compile with Clang `-Wall -Wextra -Werror` and pass.
- Clang static analyzer run on Modbus, CAN decoder, Point DB and Device Manager: no diagnostics.
- Keil project XML parses successfully.
- Python utility scripts compile with Python syntax checking.

## Hardware/toolchain gate still required

This audit environment does **not** contain the licensed Keil/ArmClang embedded toolchain or the GD32H759 target board/CAN/RS485 instruments. Therefore the modified firmware cannot be truthfully claimed as hardware-qualified here.

Before production release, rebuild `project/gateway.uvprojx` in the original Keil environment and run the existing RS485/Modbus and `BOARD_VALIDATION_M3.md` physical-bus gates, including Stage2A 500k/2M BRS and the later 5M+TDC gate if required by the product configuration.

No software audit can prove the absence of every possible defect; this package contains the defects found by source review, static checks and executable host regressions, with hardware-dependent behavior explicitly left to the board validation gate.


---

## Archived file: `BOARD_VALIDATION_M123.md`

# M1.3 / M1.4 / M2 实板闭环验收 — v0.3.1

本版本是专门用于第一轮实板闭环验收的固件配置。它不会改变 RS485/Modbus/Poll 的工作路径，只增加板上统计与 PASS/WAIT 日志。

## 1. 验收目标

第一轮硬件 Gate 必须证明以下三条链路在真实 UART/DMA/RS485 物理层上成立：

1. **M1.3**：TX 只走 DMA，并且只有 UART TC（最后一个 stop bit 完成）后才释放 DE；RX 由 DMA + IDLE/FTF 完成。
2. **M1.4**：Modbus RTU request/response 正常闭环；超时会 retry；连续失败能进入 OFFLINE；从站恢复后能回到 ONLINE。
3. **M2**：Poll Scheduler 周期发起 transaction，结果经 Device Manager 写入 Point DB；Point timestamp 持续推进；断线时 Point 最终为 OFFLINE，恢复后回 GOOD。

## 2. 默认测试参数

固件默认配置：

- UART4 RS485：9600-8-N-1
- Slave ID：1
- FC03
- Start address：0
- Quantity：2
- Poll period：1000 ms
- Response timeout：300 ms（从 UART TC 后开始）
- Retry：1
- Point 1001：取第 1 个寄存器，U16 -> F32，scale=0.1
- Debug：USART2 / PC10 TX / PC11 RX / 115200-8-N-1

默认请求应为：

```text
01 03 00 00 00 02 C4 0B
```

## 3. 接线

### Debug 串口

- PC10 -> USB-UART RX
- GND -> USB-UART GND
- 串口终端：115200-8-N-1

### RS485

- 网关 RS485 A -> 从站/USB-RS485 A
- 网关 RS485 B -> 从站/USB-RS485 B
- 建议共地
- 若无真实仪表，可使用 `tools/modbus_rtu_slave.py` + USB-RS485 适配器作为物理从站

逻辑分析仪建议至少接：

- CH1：PB6 / UART4 TX
- CH2：PB4 / DE-/RE
- 可选 CH3：PB12 / UART4 RX

## 4. 编译与烧录

使用 `project/gateway.uvprojx`，Keil ARM Compiler 6 clean rebuild 后烧录。

启动日志必须包含：

```text
GD32H759 industrial gateway v0.3.1-m123-validation
[I][SYS] RS485 transport: TX DMA + RX DMA/IDLE
[I][SYS] M1.3/M1.4/M2 board validation: ENABLED
[I][RS485] UART4 ready, 9600 baud
[I][POLL] scheduler started, jobs=1
[I][VAL] M1.3/M1.4/M2 board validation monitor started
```

如果这些基础日志不完整，本轮验收停止在启动层，不进入 M1.3 判定。

## 5. Phase A — 正常在线闭环

保持 Slave 1 在线，寄存器 0、1 可读。等待约 4–6 秒。

预期 TX 日志持续出现：

```text
[D][RS485] TX(DMA): 01 03 00 00 00 02 C4 0B
```

预期 RX 为 9 字节 FC03 正常响应：

```text
01 03 04 xx xx yy yy CRC_LO CRC_HI
```

Validation 统计应满足：

- `DMA start >= 3`
- `DMA ftf >= 3`
- `DMA tc >= 3`
- `rxIdle + rxFtf >= 3`
- `RTU ok >= 3`
- Device = ONLINE
- Point = GOOD
- Point timestamp 在相邻报告间递增

随后必须看到：

```text
[I][VAL] M1.3 PASS: TX DMA -> UART TC -> RX DMA event observed
[I][VAL] M1.4 normal PASS: request/response transactions valid
[I][VAL] M2 PASS: Poll Scheduler -> Device -> Point DB timestamp advances
```

### M1.3 逻辑分析仪判定

9600-8-N-1 下：

- 1 字节 = 10 bit = 1.0417 ms
- 8 字节请求线速时间约 8.33 ms
- 9 字节正常响应线速时间约 9.38 ms
- t3.5 理论约 3.65 ms；固件 tick 向上取整为约 4 ms

PB4/DE 必须：

1. 在第一个 TX start bit 之前拉到 TX；
2. 整个 8 字节期间保持 TX；
3. **不能在 DMA FTF 时提前下降**；
4. 只允许在第 8 字节最后一个 stop bit 完成之后下降到 RX。

若首字节缺失/重复、DE 提前下降、DMA FTF 后立即切 RX，则 M1.3 FAIL。

## 6. Phase B — timeout / retry / OFFLINE 故障注入

在已经看到 Phase A 的三个 PASS/normal PASS 后，断开 Slave 的 A/B 或关闭从站电源，保持 **至少 5 秒**。

每个 Poll transaction 配置 `retry=1`，因此一次最终 timeout 应产生两次 TX attempt。

预期看到类似：

```text
[W][RS485] txn=... retry err=-1 left=0
[W][MODBUS] dev=1 timeout
```

Validation 必须看到：

```text
[I][VAL] M1.4 retry observed: PASS sub-gate
[I][VAL] M1.4 OFFLINE observed: PASS sub-gate; reconnect slave now
```

同时统计/状态应符合：

- `retries > 0`
- `tout > 0`
- Device `consecutive_error` 达到 3
- Device = OFFLINE
- Point = OFFLINE

Point 如果在设备 OFFLINE 后仍显示 TIMEOUT，而不是 OFFLINE，则 M2 FAIL。本版已修复该覆盖问题。

## 7. Phase C — 从站恢复

重新接通 A/B 或给从站上电，保持原来的 Slave 1 / 9600-8-N-1 配置。

下一次成功 transaction 后应看到：

- Device：OFFLINE -> ONLINE
- `consecutive_error -> 0`
- Point：OFFLINE/STALE -> GOOD
- Point timestamp 再次推进

Validation 最终必须打印：

```text
[I][VAL] M1.4 recovery observed: ONLINE + GOOD
[I][VAL] M1.4 PASS: transaction/retry/offline/recovery closed loop
[I][VAL] FINAL PASS: M1.3=PASS M1.4=PASS M2=PASS
```

出现 `FINAL PASS` 才认为本次第一轮 M1.3/M1.4/M2 实板闭环 Gate 完成。

## 8. 第一轮 PASS / FAIL 标准

| 项目 | PASS 条件 | FAIL 条件 |
|---|---|---|
| M1.3 TX DMA | txStart/FTF/TC 持续增长，PB4 到最终 stop bit 后才释放 | polling 路径、首字节异常、DE 在 TC 前释放 |
| M1.3 RX DMA | RX IDLE 或 DMA FTF 能冻结完整帧并解析 | 长度漂移、丢尾字节、频繁 0-byte snapshot |
| M1.4 normal | 连续至少 3 次 FC03 transaction OK | CRC/长度/slave/fc 匹配不稳定 |
| M1.4 retry | 断线后 `retry_count` 增长 | timeout 后不 retry 或总线卡死 |
| M1.4 state | 3 次最终失败进入 OFFLINE，重连恢复 ONLINE | 状态不迁移或无法恢复 |
| M2 Poll | 周期 1s 发起 transaction | 只靠 smoke-test 或一次性采集 |
| M2 Point DB | GOOD + timestamp 持续推进 | 值不更新或 timestamp 不变 |
| M2 quality | 断线最终 OFFLINE，恢复 GOOD | OFFLINE 被 TIMEOUT 覆盖或恢复后仍坏 |

## 9. 这次 Gate 不覆盖的项目

这只是进入 M3 前的第一轮硬件 Gate，不等于最终稳定性认证。以下放到后续专项：

- CRC 故障注入覆盖率
- Modbus exception 全码测试
- 多从站、多 Poll Job 压力测试
- 24/72 小时长稳
- EMI/ESD/拔插抖动
- Watchdog/Fault 注入
- M3 CAN-FD 及之后的网络功能


---

## Archived file: `BOARD_VALIDATION_M3.md`

# M3 board validation — v0.4.3 staged vendor baseline

Start with `GW_CANFD_BRINGUP_STAGE=0`.

## Wiring

- Gateway CANH <-> USBCAN CANH
- Gateway CANL <-> USBCAN CANL
- Gateway GND  <-> USBCAN GND
- Termination: normally 120 ohm at each end; power-off CANH-CANL should read
  approximately 60 ohm.

## Stage 0 — Classic CAN 500K

PC CAN settings:

- Nominal bitrate: 500000 bit/s
- Normal/active mode, not listen-only
- Classic CAN frames; CAN-FD data bitrate is irrelevant for this stage

Immediately after boot the PC should receive once per second:

- EXT ID `0x1314`
- DLC 8
- `A1 A2 A3 A4 A5 A6 A7 A8`

This intentionally matches the supplied working board example.

Then PC sends every 200-500 ms:

- STD ID `0x301`
- Classic CAN, DLC 8
- `00 FA 00 00 00 00 00 00`

Expected UART:

`[I][CANRX] id=0x00000301 STD FD=0 BRS=0 len=8 data=00 FA ...`

Expected M3VAL:

- `txOK` grows
- `rx` grows
- `DEC seen/match/sig` grow
- `DEV=ONLINE`
- `POINT=GOOD value_x10=250`
- `TX PASS`
- `RX/Point PASS`

Stop PC 0x301 for >=5 seconds, then resume. Expect OFFLINE then ONLINE+GOOD.
Stage0 final message:

`STAGE0 PASS: Classic CAN TX/RX + Point DB baseline`

## Stage 1 — FD, BRS OFF

After Stage0 passes, set `GW_CANFD_BRINGUP_STAGE=1`, rebuild and flash.

PC sends:

- STD `0x301`
- FD=ON
- BRS=OFF
- 12 bytes: `00 FA 00 00 00 00 00 00 00 00 00 00`

Expected raw RX has `FD=1 BRS=0 len=12`. Gateway TX is STD `0x302`, FD=1,
BRS=0, len=12.

Stage1 final message:

`STAGE1 PASS: CAN-FD BRS-off TX/RX + Point DB`

## Stage 2A — FD + BRS 500K/1M (current default)

After Stage1 passes, set `GW_CANFD_BRINGUP_STAGE=2`, rebuild and flash.

PC:

- Nominal: 500000 bit/s, target sample ~80%
- Data: 1000000 bit/s, target sample ~80%
- FD=ON, BRS=ON

Send the same 12-byte STD 0x301 frame. Gateway TX is STD 0x302 FD+BRS.
This lower-rate gate is intentionally used to test physical-layer margin before
returning to higher data rates.

Expected final message after TX/RX/PointDB/offline/recovery:

`STAGE2A-LR PASS: M3 CAN-FD 500k/1M BRS + Point DB closed loop`

---

## BRS safe-gate / low-rate diagnostic update

The previous direct 500k/5M Stage-2 test produced rising TEC/fdTEC and could
appear to freeze after several TX attempts. Do not use the old "TX PASS" line
from v0.4.3 as proof of a successful ACK; it only reflected mailbox terminal
state.

### Stage 2A low-rate diagnostic (current default)

Configure PSCAN:

- Arbitration: 500 kbit/s
- Data: 1 Mbit/s
- FD: enabled
- BRS: enabled
- STD ID: 0x301
- Length: 12
- Data: `00 FA 00 00 00 00 00 00 00 00 00 00`

The gateway is RX-first and sends no 0x302 until at least three valid BRS RX
frames have been accepted. Expected sequence:

1. Three `[CANRX] ... FD=1 BRS=1 len=12` lines.
2. `BRS RX gate PASS (3 frames); gateway TX released`.
3. PC begins receiving STD 0x302 FD+BRS heartbeat.
4. `txGood` grows while `txFail=0`, `hold=0`, `TEC=0`, `fdTEC=0`.

If `hold=1`, stop the test. The driver deliberately suppresses further TX
before error-passive/bus-off.

### Stage 2B final 5 Mbit/s gate

After Stage 2A passes, set:

`GW_CANFD_BRINGUP_STAGE = 3U`

Then configure PSCAN for 500 kbit/s arbitration / 5 Mbit/s data / FD+BRS.
Stage 3 also remains RX-first. The gateway data timing is 5 Mbit/s with a
76.67% sample point, intentionally closer to PSCAN's 75% option.


---

## Archived file: `BOARD_VALIDATION_STABLE.md`

# v0.5.0 Stable Baseline — Board Validation

## 1. Bus setup

Use correct CAN termination at the two physical ends of the bus. Configure every
node for ISO CAN-FD with BRS disabled.

Gateway expected startup:

```text
GD32H759 industrial gateway v0.5.0-canfd500-stable
[I][CAN] CAN2 stable: CAN-FD 500k, BRS=OFF, TDC=OFF, CK_CAN=APB2
```

Linux SocketCAN example:

```bash
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 500000 dbitrate 500000 fd on
sudo ip link set can0 up
python3 tools/canfd_stable_socketcan.py --channel can0 --raw 250 --length 12
```

`bitrate_switch` must remain false.

## 2. Basic RX test

For the built-in demo validation only, temporarily set:

```c
GW_DEMO_CAN_CONFIG_ENABLE       1U
GW_M3_BOARD_VALIDATION_ENABLE   1U
```

Send standard ID 0x301, FD=1, BRS=0, LEN=12:

```text
00 FA 00 00 00 00 00 00 00 00 00 00
```

Expected Point 2001: 25.0 / GOOD.

## 3. Basic TX test

With validation enabled, gateway sends standard ID 0x302, FD=1, BRS=0,
LEN=12. Require at least three successful TX completions with:

```text
txFail=0
spurTX=0
TEC=0
REC=0
fdTEC=0
fdREC=0
```

## 4. Offline/recovery

Stop ID 0x301 for >=5 seconds. Confirm device becomes OFFLINE and Point quality
becomes OFFLINE. Resume BRS-OFF frames and confirm ONLINE + GOOD.

## 5. Long soak

Return production defaults (demo/validation/trace OFF), then run representative
application traffic:

- CAN-FD BRS-OFF, 500k, representative 12..64-byte frames.
- concurrent RS485/Modbus traffic.
- at least 1 hour initial soak; 24 hours before product freeze is recommended.

Fail the gate on any unexplained:

- CAN RX overrun or software queue drop
- spurious TX interrupt
- bus-off
- nonzero persistent TEC/REC on a healthy bus
- task starvation / frozen periodic activity
- RS485 transport regression
- stack overflow / HardFault / MemManage / BusFault / UsageFault

## 6. After PASS

Restore:

```c
GW_DEMO_CAN_CONFIG_ENABLE       0U
GW_M3_BOARD_VALIDATION_ENABLE   0U
GW_CANFD_RX_TRACE_ENABLE        0U
```

Then freeze v0.5.0 as the southbound base for northbound development.


---

## Archived file: `claude.md`

# GD32H759 Industrial Gateway

## Project Goal

Develop an industrial gateway based on GD32H759IMK6.

Hardware interfaces:

- 1 x RS485
- 1 x CAN-FD
- 1 x 10/100 Ethernet
- Debug UART
- External Flash
- SD Card
- RTC

RTOS: FreeRTOS
TCP/IP: LwIP
Filesystem: FatFs

Target IDE:
Keil MDK / ARM Compiler 6.

MCU:
GD32H759IMK6.

---

# Hardware Mapping

## Debug UART

Peripheral: USART2

TX:
PC10

RX:
PC11

Baud:
115200

Format:
8-N-1

Debug USART must never be used by RS485.

---

## RS485

Peripheral:
UART4

TX:
PB6

RX:
PB12

DE:
/RE:
PB4

Transceiver:
SIT3088EESA

PB4 controls DE and /RE together.

PB4 = 1:
Transmit mode.

PB4 = 0:
Receive mode.

Important:

DMA TX completion is NOT equivalent to UART transmission complete.

RS485 DE must only return to RX after USART TC is asserted.

---

## CAN-FD

Peripheral:
CAN2

TX:
PD13

RX:
PD12

Transceiver:
SIT1042AQT/3

CAN-FD support is required.

---

## Ethernet

One physical Ethernet port.

PHY:
LAN8720A-CP-TR

Interface:
RMII

Speed:
10/100 Mbps

Network stack:
LwIP.

The same Ethernet interface carries:

- MQTT
- HTTP/HTTPS
- Modbus TCP
- NTP
- OTA

---

# Architecture

Strict dependency direction:

Application
    ↓
Service
    ↓
Protocol
    ↓
Driver
    ↓
BSP

Never introduce dependencies in the opposite direction.

Examples of forbidden dependencies:

- Modbus calling MQTT
- CAN calling MQTT
- Web directly accessing UART
- MQTT directly accessing RS485
- ISR modifying Point Database
- Protocol modules directly accessing Flash

All field data must pass through Point Database.

---

# Core Data Path

RS485:
UART4
→ RS485 Task
→ Modbus RTU
→ Data Task
→ Point Database

CAN-FD:
CAN2
→ CAN Task
→ CAN Decoder
→ Point Update Queue
→ Data Task
→ Point Database

Northbound:
Point Database
→ MQTT / Modbus TCP / Web / Storage

---

# FreeRTOS Rules

ISR must be short.

ISR may only:

- clear interrupt flags
- move minimal data
- notify a task
- give an ISR-safe semaphore
- write to an ISR-safe queue when required

ISR must never:

- malloc/free
- printf
- parse Modbus
- parse CAN application data
- access filesystem
- call MQTT
- update Point Database

Prefer:

ISR → Direct-to-Task Notification

Use queues for actual messages.

Do not use portMAX_DELAY without explicit justification.

All important waits must have timeouts.

---

# Memory Rules

This is Cortex-M7 with D-Cache.

DMA cache coherency must always be considered.

Ethernet DMA, UART DMA and SDIO DMA buffers must follow the project's
cache coherency strategy.

High-frequency communication code must not call malloc/free.

Use:

- static allocation
- fixed pools
- queues
- ring buffers

where practical.

---

# RS485 Rules

Only rs485_task owns UART4.

Only one RS485 transaction may be active at a time.

Transaction state machine:

IDLE
→ TX
→ WAIT_TX_COMPLETE
→ WAIT_RESPONSE
→ COMPLETE / TIMEOUT

Use UART RX DMA plus UART IDLE detection.

Modbus frame completion should consider:

- expected frame length
- UART IDLE
- CRC
- response timeout

---

# CAN Rules

CAN ISR only wakes can_task.

can_task drains RX FIFO.

CAN Decoder converts CAN signals into Point Update messages.

Do not expose raw CAN frames to MQTT directly.

---

# Point Database

Point Database is the single source of truth for process values.

Every Point must contain:

- Point ID
- Device ID
- Data type
- Value
- Quality
- Timestamp

Supported quality:

GOOD
STALE
TIMEOUT
BAD
OFFLINE
INVALID

---

# Error Handling

Never return generic -1 from new modules.

Use gw_err_t error codes.

All communication failures must be recoverable.

A single subsystem failure must not stop unrelated subsystems.

Examples:

Ethernet failure must not stop RS485/CAN.

MQTT failure must not stop data acquisition.

SD failure must not stop communication.

---

# Development Order

Do not implement all features simultaneously.

Development phases:

M0:
Board bring-up
Debug UART
FreeRTOS
Watchdog
Fault handling

M1:
UART4 RS485
Modbus RTU FC03
Real device test

M2:
Device Manager
Register Mapping
Point Database
Poll Scheduler

M3:
CAN2 CAN-FD

M4:
Ethernet
LwIP
Ping

M5:
MQTT

M6:
Web
Storage
Offline cache

M7:
TLS
OTA
Secure Boot

Do not move to the next phase until the current phase builds and passes its
defined tests.

---

# Coding Style

Language:
C11-compatible embedded C.

Prefer:

module_init()
module_start()
module_stop()
module_get_status()

Internal module state should normally be static.

Public headers must expose the minimum necessary interface.

Avoid global variables.

Do not silently modify hardware mappings.

Do not invent peripheral pins.

If hardware information is missing, stop and report the missing information.

---

# Build Rule

Every meaningful code change must:

1. compile with zero errors
2. avoid introducing new warnings
3. report which files changed
4. explain the reason for each change
5. state what hardware test is required

Never claim hardware functionality was verified unless physical hardware
testing actually occurred.

---

# Git Rule

Before major changes:

inspect git status.

Make changes in small logical units.

Do not rewrite unrelated code.

Do not delete working code merely to simplify implementation.

Do not commit automatically unless explicitly requested.


---

## Archived file: `ETHERNET_BASELINE_V0.6.0.md`

# Ethernet Baseline v0.6.0

## Scope

This milestone integrates the supplied GD32H759 FreeRTOS TCP client example into
our existing industrial-gateway architecture without importing the demo's
application traffic. The result is a reusable ENET0/lwIP transport layer for
later MQTT, HTTP and Modbus TCP work.

The southbound v0.5.0 baseline is intentionally unchanged:

- CAN2: ISO CAN-FD, 500 kbit/s, BRS OFF
- RS485: UART4, TX DMA + RX DMA/IDLE
- Point DB / Device Manager remain the normalized data model

## Hardware mapping

The Ethernet port follows the supplied LAN8720A RMII example:

| Signal | GD32H759 pin |
|---|---|
| RMII_REF_CLK | PA1 |
| MDIO | PA2 |
| CRS_DV | PA7 |
| TX_EN | PB11 |
| MDC | PC1 |
| RXD0 | PC4 |
| RXD1 | PC5 |
| TXD0 | PG13 |
| TXD1 | PG14 |
| PHY RESET | PF6 |
| PHY 50 MHz clock | PA8 CKOUT0, PLL0P/12 |

PHY address is 0. The GD32 SPL `LAN8700` status-register layout is used for the
LAN8720A, matching the supplied example.

## Memory / Cortex-M7 cache policy

The GD32 ENET SPL places DMA objects at fixed addresses:

```text
0x30000000  RX descriptors
0x30000160  TX descriptors
0x30000300  RX buffers
0x30002100  TX buffers
```

The complete `0x30000000..0x30003FFF` 16 KiB window is configured MPU
non-cacheable before D-cache is enabled. This is mandatory for deterministic DMA
ownership/status visibility on Cortex-M7.

lwIP's heap is placed in the separately reserved SRAM window at `0x30004000`,
with `MEM_SIZE = 15 KiB`.

## Network configuration

Production bring-up defaults are intentionally deterministic:

```text
MAC      02:47:44:32:48:01   (locally administered bring-up address)
IPv4     192.168.103.213
Mask     255.255.255.0
Gateway  192.168.103.254
DHCP     OFF
```

Before product deployment, assign a unique MAC per gateway and move IP settings
to persistent configuration. DHCP is compiled into lwIP and can be enabled with
`GW_ETH_DHCP_ENABLE`.

## RTOS / interrupt architecture

```text
ENET0 IRQ (priority 4)
        |
        +-- clear RX DMA IRQ + give binary semaphore
        |
        v
eth-rx task (priority 4)
        |
        +-- bounded DMA-ring drain (max 8 descriptors/wake)
        +-- copy into lwIP pbuf
        v
tcpip thread (priority 3)
        |
        v
ARP / ICMP / TCP / UDP / future northbound protocols

net task (priority 2)
        +-- PHY reset / ID
        +-- wait for cable without blocking system boot
        +-- auto-negotiation
        +-- static IP or DHCP
        +-- link up/down monitoring
```

CAN and RS485 service tasks retain their existing higher priorities, so an
Ethernet RX burst cannot take ownership of the gateway scheduler indefinitely.

## Defensive changes vs the supplied demo

The official demo is useful as the board/PHY reference, but several behaviors
were deliberately hardened for a continuously-running gateway:

- no `while(1)` fatal loop on Ethernet initialization failure;
- boot succeeds with Ethernet cable unplugged;
- TX descriptor wait is bounded to 100 ms rather than infinite;
- RX processing is bounded and handles invalid/pbuf-failure descriptors without
  stranding later frames in the ring;
- link state is monitored instead of unconditionally forcing `NETIF_FLAG_LINK_UP`;
- Ethernet DMA memory is explicitly made non-cacheable before D-cache enable;
- official TCP client demo traffic is not started automatically;
- IP-ready and link-ready EventGroup bits are exported for later northbound
  protocol tasks.

## lwIP feature baseline

Enabled:

- IPv4 / ARP / ICMP
- TCP / UDP / DNS
- DHCP (compiled, runtime disabled by default)
- netconn API
- netif API
- hardware IP/TCP/UDP/ICMP checksum offload

Disabled for this first baseline:

- IPv6
- BSD sockets API
- TLS
- application TCP client demo

The next northbound layer should use `EVT_NET_IP_READY` as its dependency instead
of touching PHY/ENET internals directly.

## lwIP platform hooks

The copied GD32 lwIP port's original `arch/cc.h` did not provide `LWIP_RAND` and
compiled platform assertions to a no-op. v0.6.0 overrides it from
`user/config/arch/cc.h`:

- `LWIP_RAND()` uses a lightweight gateway PRNG for DNS transaction IDs, DHCP
  XIDs and ephemeral ports;
- `LWIP_PLATFORM_ASSERT()` logs and stops instead of silently continuing after
  an internal lwIP invariant failure.

The lightweight PRNG is explicitly **not** a cryptographic RNG. Future TLS key
material/nonces must use the GD32H759 hardware TRNG through the TLS library's RNG
integration.


---

## Archived file: `ETHERNET_VALIDATION_V0.6.0.md`

# Ethernet Validation v0.6.0

## 1. Build and flash

Open `project/gateway.uvprojx` in Keil/ArmClang 6, perform **Clean Target** and
**Rebuild all target files**, then flash the board.

Expected boot banner:

```text
GD32H759 industrial gateway v0.6.0-eth-lwip-baseline
[I][SYS] Ethernet: ENET0 RMII + LAN8720A + lwIP 2.1.2
```

If the cable is unplugged, the gateway must continue running CAN/RS485 and print
only a network wait indication rather than blocking boot:

```text
[I][NET] Ethernet cable/link DOWN; waiting for link
```

## 2. PC Ethernet setup

For direct/lab testing, configure the PC NIC in the same subnet, for example:

```text
PC IPv4     192.168.103.19
Mask        255.255.255.0
Gateway     optional for direct subnet test
Gateway MCU 192.168.103.213
```

Connect the LAN8720A Ethernet port to the PC/switch.

Expected UART information after link negotiation:

```text
[I][ETH] LAN8720A/ENET0 RMII PHY id=....:....
[I][ETH] link negotiated: 100M full-duplex
[I][NET] link UP IP=192.168.103.213 mask=255.255.255.0 gw=192.168.103.254
```

The exact PHY ID and negotiated 10/100M/full/half mode depend on the connected
peer; 100M full-duplex is the expected normal lab result.

## 3. Ping / ARP test

From the PC:

```text
ping 192.168.103.213
```

Success criteria:

- ping replies continuously;
- `rxIRQ` and `rx` increase;
- `tx` increases;
- `rxAlloc=0`;
- `rxDrop=0` during normal traffic;
- `txFail=0`;
- `txWait=0` under normal traffic.

Example diagnostic line:

```text
[I][NET] ETH link=UP ip=READY rxIRQ=... rx=... rxAlloc=0 rxDrop=0 tx=... txFail=0 txWait=0
```

## 4. Cable hot-plug

While pinging:

1. unplug Ethernet;
2. verify UART reports `Ethernet link DOWN`;
3. verify CAN/RS485/log tasks continue;
4. reconnect Ethernet;
5. verify link becomes UP and ping recovers without MCU reset.

For the static-IP baseline the address remains 192.168.103.213 after reconnection.

## 5. Southbound concurrency test

Run Ethernet ping while simultaneously:

- sending CAN-FD 500k BRS-OFF traffic to the gateway;
- running normal RS485/Modbus polling.

Acceptance:

```text
CAN: txFail=0, TEC/REC=0, rx_overrun=0, rx_queue_drop=0, spurTX=0
ETH: rxAlloc=0, txFail=0, no persistent txWait growth
RS485: no new DMA/IDLE/CRC/timeout anomalies beyond actual device behavior
RTOS: no stack overflow, malloc failure, HardFault or task starvation
```

## 6. Soak

After the short functional test passes, run at least 1 hour of concurrent
CAN/RS485/Ethernet traffic, then a 24-hour soak before freezing v0.6.x as the
network transport baseline.

## Not yet part of this milestone

The supplied TCP Client application's automatic client connection is
intentionally not started. TCP is compiled and ready, but the first acceptance
criterion is a stable PHY/DMA/lwIP/ICMP foundation. A controlled TCP smoke test
or the first northbound protocol can be added after this validation passes.


---

## Archived file: `FIX_REPORT.md`

# V0.4.0 M3 CAN-FD Change Report

## Baseline

Started from the V0.3.1 M1.3/M1.4/M2 board-validation source after the user completed real-board normal, timeout/retry/offline and recovery tests.

## M1/M2 freeze cleanup

- Disabled M1.3/M1.4/M2 validation task by default.
- Disabled verbose RS485 TX/RX diagnostic logs.
- Kept RX and TX DMA-only behavior unchanged.
- Hardened RX DMA FTF accounting: only treat FTF as a real full RX when the receive window is armed and DMA remaining count is zero; track rejected observations separately.

## M3 implementation

- Added CAN2 BSP definitions: PD12 RX, PD13 TX, AF5.
- Added bundled GD32H7xx CAN SPL source/header to project build.
- Added `drv_canfd` with:
  - ISO CAN-FD;
  - 64-byte mailboxes;
  - 500 kbit/s nominal / 5 Mbit/s data phase;
  - BRS;
  - standard and extended receive mailboxes;
  - software RX/TX queues;
  - TX completion, overrun, bus-off and error statistics.
- Added `can_task`; all CAN2 ISRs only wake this task.
- Added CAN Decoder with signal maps, endian handling and Point scale/offset.
- CAN Decoder emits `point_update_t` into the existing M2 data pipeline.
- Added CAN Device timeout/offline/recovery integration with Device Manager and Point DB.
- Added M3 validation Device 2 / Point 2001 / ID 0x301 mapping.
- Added periodic gateway TX validation frame ID 0x302.
- Added M3 validation monitor and SocketCAN helper.

## Verification completed in this environment

- All 25 user C source files pass local Clang syntax parsing with project headers/macros.
- `project/gateway.uvprojx` parses as valid XML.
- `tools/canfd_m3_socketcan.py` passes Python syntax compile.

## Hardware gate still required

This environment cannot perform the physical CAN-FD test. Follow `BOARD_VALIDATION_M3.md` and require the final board log:

`FINAL PASS: M3 CAN-FD + Point DB closed loop`


---

## Archived file: `GUI_BASELINE_V0.7.0.md`

# v0.7.0 — 5-inch LVGL GUI baseline

## Scope

This milestone integrates the supplied official `TLI_LCD_IPA_LVGL` example into
the already validated gateway v0.6.1 architecture. It does **not** replace the
existing FreeRTOS startup, CAN/RS485 tasks, Ethernet/lwIP or TCP server.

The GUI baseline is intentionally a presentation/diagnostic layer over the
existing Device Manager, Point DB and communications statistics.

## Display stack

- Panel: 5-inch RGB capacitive touch panel from the supplied board example
- Resolution: 800 x 480
- Pixel format: RGB565
- Display controller: GD32H759 TLI
- Frame copy acceleration: IPA
- GUI: LVGL 9.2.2
- Touch: Goodix GTxxx family over I2C2, polled by LVGL
- SDRAM: EXMC SDRAM from the official example

5-inch TLI timing copied from the supplied example:

```text
HSW 1   HBP 46   HFP 40
VSW 3   VBP 23   VFP 13
800 x 480
PLL2 PSC/N/P/Q/R = 25/300/3/3/3
TLI clock divider = PLL2R / 8
```

## Memory layout

```text
0xC0000000  TLI RGB565 framebuffer, 768000 bytes
0xC0100000  LVGL full-screen RGB565 render buffer, 768000 bytes
0xC03FF000  SDRAM startup self-test scratch
```

The first 4 MiB at `0xC0000000` is configured MPU non-cacheable before D-cache
is enabled. This prevents TLI/IPA/CPU cache-coherency problems. The Ethernet DMA
window at `0x30000000..0x30003FFF` remains independently non-cacheable.

LVGL's internal heap is 128 KiB in MCU RAM. LVGL is configured with `LV_OS_NONE`
because **only the GUI task calls LVGL APIs**; other tasks expose data through
thread-safe snapshot/statistics APIs instead of touching widgets.

## Task priority

```text
CAN / RS485          priority 5
Ethernet RX / data   priority 4
lwIP tcpip           priority 3
TCP server           priority 2
GUI                   priority 1
```

The GUI refreshes model data every 500 ms. `lv_timer_handler()` is serviced with
5..20 ms bounded sleeps. Screen drawing is therefore kept below the gateway's
southbound real-time work.

## GUI pages

### Overview

Shows registered device/point counts and current CAN, Ethernet and TCP state.

### Devices

Displays up to the first 12 configured devices with ID, name, protocol, state,
success and error counters. It uses a new non-destructive Device Manager
snapshot API.

### Points

Displays up to the first 16 configured Point DB items with ID, name, value,
quality and revision. It uses `point_db_snapshot()` and never clears dirty state.

### Network

Shows Ethernet link/IP state, ENET RX/TX counters and TCP listener/client
statistics.

### Diagnostics

Shows CAN error/counter state, RS485 DMA statistics, LCD/IPA flush statistics
and touch-controller status.

## Driver hardening versus the demo

The integration intentionally does not copy demo blocking behavior unchanged.

- IPA FTF/TAE/WCF flags are cleared before a new transfer and completion is
  acknowledged after each frame. This avoids a stale FTF flag making a later
  flush look complete before IPA actually finished.
- IPA full-frame transfer has a 100 ms timeout instead of an unbounded wait.
- Goodix I2C read/write paths use bounded waits and bus recovery; a missing or
  stalled touch controller cannot permanently block the GUI task.
- Touch failure is non-fatal: display GUI remains usable and the gateway keeps
  running.
- SDRAM is self-tested before TLI/LVGL is started.
- LCD backlight is enabled only after the first successful LVGL frame flush.

## Ethernet PA8 requirement

The LCD uses PA8 as TLI_R6, while gateway v0.6.x used PA8/CKOUT0 for the
LAN8720A 50 MHz reference clock. Full Ethernet + full-color GUI therefore
requires an **external 50 MHz RMII PHY clock source**. See
`HARDWARE_NOTE_ETH_LCD_PA8.md` before flashing the combined build.

## Power

The supplied LCD example warns that the development board may need both USB
power inputs when the LCD is fitted. An under-powered board can show flicker,
corruption or other display instability. Use a stable supply during GUI tests.


---

## Archived file: `GUI_VALIDATION_V0.7.0.md`

# v0.7.0 GUI real-board validation

## 0. Hardware prerequisite

Before testing Ethernet and LCD simultaneously, read
`HARDWARE_NOTE_ETH_LCD_PA8.md`.

For an unchanged board with no external PHY 50 MHz oscillator, first validate
the LCD with Ethernet temporarily disabled. For the full gateway build, provide
the independent 50 MHz RMII PHY reference clock.

Use a stable board/LCD supply. The supplied LCD example recommends powering both
USB inputs when needed to prevent LCD power instability.

## 1. Build and boot

Keil:

1. Clean Target
2. Rebuild All
3. program the MCU
4. reset the board

Expected early UART output:

```text
GD32H759 industrial gateway v0.7.0-gui-lvgl-baseline
[I][SYS] GUI: 5-inch 800x480 RGB + LVGL 9.2.2 + Goodix touch
[I][SYS] HW: Ethernet PHY RMII 50MHz must be externally supplied; PA8 is LCD R6
Starting FreeRTOS...
```

Expected GUI initialization:

```text
[I][GUI] touch ready product=.... max=5
[I][GUI] LVGL 9.2.2 ready 800x480 RGB565 TLI+IPA touch=YES
```

A different recognized Goodix product/max-point value is acceptable. If touch is
not found, the expected behavior is a warning and a working display, not a
system hang.

## 2. Display acceptance

Verify:

- backlight turns on after the first rendered frame;
- image fills exactly 800x480 with no wrap/shift;
- no persistent tearing/flicker/color noise;
- Overview/Devices/Points/Network/Diagnostics tabs are visible;
- tabs can be switched repeatedly using touch;
- display remains stable for at least 30 minutes.

UART diagnostics should show increasing flush count and no errors:

```text
[I][GUI] ready=1 touch=1 refresh=... flush=... timeout=0 err=0
```

## 3. Touch acceptance

Touch all four corners and the center, then select each tab repeatedly.

Pass criteria:

- no mirrored X/Y axis;
- no 90-degree rotation mismatch;
- no stuck pressed state;
- no I2C-related system freeze.

If coordinates are mirrored/rotated on the exact panel revision, adjust only the
coordinate transform in `gw_touch_read()`; do not change TLI timing to fix touch
orientation.

## 4. Ethernet + GUI coexistence

With the external PHY 50 MHz reference source present, Ethernet must retain the
previous v0.6.1 behavior:

```text
[I][ETH] LAN8720A/ENET0 RMII PHY id=0007:C0F1
[I][ETH] link negotiated: 100M full-duplex
[I][NET] link UP IP=192.168.103.213 ...
[I][TCP] server listening on port 5000 (echo enabled)
```

PC:

```powershell
ping 192.168.103.213
py .\tools\tcp_echo_test.py --host 192.168.103.213 --port 5000 --rounds 100 --size 1024
```

Pass criteria:

```text
TCP all rounds PASS
ETH rxAlloc=0
ETH rxDrop=0
ETH txFail=0
ETH txWait=0
TCP rxErr=0
TCP txErr=0
GUI timeout=0
GUI err=0
```

## 5. Southbound concurrency

Run CAN-FD 500k BRS-OFF traffic and RS485 polling while TCP echo and touch/UI
updates are active.

Pass criteria include:

```text
CAN rx_overrun=0
CAN rx_queue_drop=0
CAN tx_spurious_irq=0
CAN busoff=0
normal TEC/REC=0
RS485 no unexpected DMA/UART error growth
TCP/Ethernet errors=0
GUI flush timeout/error=0
no HardFault
no FreeRTOS stack-overflow/malloc-failure hook
no stopped task/log stream
```

## 6. Long soak

After functional acceptance, run at least 1 hour with Ethernet/TCP + CAN/RS485
+ GUI active, then a 24-hour soak before freezing the GUI hardware baseline.


---

## Archived file: `HARDWARE_NOTE_ETH_LCD_PA8.md`

# Hardware note — LCD TLI_R6 and Ethernet RMII reference clock share PA8

## Critical conflict

The supplied 5-inch TLI/LVGL example assigns **PA8 = TLI_R6**. The Ethernet
baseline used by gateway v0.6.x generated the LAN8720A 50 MHz RMII reference on
**PA8 = CKOUT0 (PLL0P / 12)**. One pin cannot provide both signals.

The vendor Ethernet example also contains an RMII note saying that the user can
provide the 50 MHz clock by soldering a 50 MHz oscillator. For a full-color LCD
and Ethernet running at the same time, that external PHY reference-clock option
is required on this board design.

## v0.7.0 full gateway configuration

```c
GW_GUI_ENABLE                     1U
GW_ETH_ENABLE                     1U
GW_ETH_RMII_REFCLK_PA8_MCO        0U
GW_ETH_RMII_REFCLK_EXTERNAL_50M   1U
```

With this configuration:

- PA8 belongs to TLI_R6.
- firmware does **not** configure CKOUT0 on PA8;
- LAN8720A must receive a valid external 50 MHz RMII reference clock;
- the build has a compile-time guard that rejects GUI + PA8-MCO mode.

If the board does not have the external 50 MHz source populated, the expected
failure is Ethernet PHY/link operation after the LCD claims PA8. This is a
hardware clock-source issue, not an lwIP issue.

## Safe bring-up choices

### A. Validate LCD/touch first on an unmodified board

Temporarily set:

```c
GW_ETH_ENABLE 0U
```

The GUI, SDRAM, TLI, IPA and Goodix touch can then be validated without the
Ethernet clock conflict. Restore Ethernet only after the PHY has an independent
50 MHz reference source.

### B. Full gateway (recommended final hardware)

Populate/provide the board's external 50 MHz RMII PHY reference source and use
the v0.7.0 defaults shown above.

### C. Headless Ethernet build

For a build without LCD, PA8 can still generate the PHY clock:

```c
GW_GUI_ENABLE                     0U
GW_ETH_RMII_REFCLK_PA8_MCO        1U
GW_ETH_RMII_REFCLK_EXTERNAL_50M   0U
```

## Why not simply omit LCD R6?

Leaving the LCD R6 pin connected while driving PA8 with an asynchronous 50 MHz
clock would inject a non-pixel data signal into the red channel. That is not a
reliable display mode and is intentionally not supported by this baseline.


---

## Archived file: `HOTFIX2_BRS_TX_TDC.md`

# v0.4.5-m3-stage2a-tdc-hotfix2

## Evidence from hotfix1 bench log

- BRS RX remained clean: fdREC=0 and Point updates continued.
- First gateway BRS TX: TEC=7 and fdTEC=7.
- Second gateway BRS TX: TEC=14 and fdTEC=14, then safe-hold entered.
- M3VAL/Modbus/CANRX logging continued in safe-hold, proving the IRQ-storm fix worked.

The simultaneous rise of the normal TX error counter and the FD data-phase TX
error counter localizes the remaining fault to the gateway transmit data phase.
Stage2A had TDC intentionally disabled as an A/B diagnostic, so this revision
enables transmitter delay compensation at 2 Mbit/s.

## Changes

- Stage2A (500k/2M FD+BRS): TDC enabled.
- Initial TDCO = 2.
- Boot/M3 validation text updated to make TDC state explicit.
- Existing TX safe-hold / MB0 IRQ-storm protection remains unchanged.
- `FDERR ... tdc=<TDCV> oor=<count>` is the tuning evidence.

## Acceptance

PC adapter must be in normal active CAN-FD mode at nominal 500 kbit/s and data
2 Mbit/s, BRS enabled. After three valid RX frames release TX. Pass requires:

- `txGood` increases;
- `txFail=0`;
- `TEC=0`;
- `fdTEC=0`;
- `tdcOOR=0`;
- periodic M3VAL/Modbus logs continue.

If `fdTEC` still rises, keep the safe-hold and use the measured `tdc` value plus
an oscilloscope/CAN-FD analyzer to tune TDCO / data sample point rather than
raising the error thresholds.


---

## Archived file: `HOTFIX3_BRS_TX_TDCO_SAMPLE_POINT.md`

# v0.4.5-m3-stage2a-tdc-hotfix3

## Bench evidence from hotfix2

The IRQ-storm fix remains good: `spurTX=0`, M3VAL/Modbus/RX logging continues in hold.
With TDC enabled and TDCO=2, gateway BRS TX improved from immediate failure to a mix of successes and failures: the first three TX attempts completed successfully, a later TX raised TEC/fdTEC by 7, subsequent successful TX reduced the counters, and another failure eventually triggered safe-hold.

The same log shows TDCV around 16 during the gateway TX window with no TDC out-of-range flag.  This is consistent with insufficient secondary-sample-point margin rather than a fixed bitrate/ACK/mailbox failure.

## Fix

TDCO is no longer a hand-written bring-up constant.  It is derived from the configured data-phase regular sample point:

```c
GW_CANFD_TDC_OFFSET = 1 + GW_CANFD_DATA_PROP_SEG + GW_CANFD_DATA_SEG1
```

Therefore:

- Stage2A 500k/2M: TDCO = 1 + 11 + 8 = **20** (80% data sample point).
- Stage2B 500k/5M: TDCO = 1 + 14 + 8 = **23** (76.67% data sample point).

This also restores the same TDCO convention that the earlier 5M configuration used, while preventing the timing fields and TDCO from drifting apart in future edits.

## Safety behavior retained

- RX-first gate remains 3 valid BRS RX frames.
- fdTEC safe-hold threshold remains 8.
- MB0 pending-IRQ quiesce/ack hotfix remains unchanged.
- RX, Modbus, and periodic diagnostics continue while TX is held.

## Board acceptance

For Stage2A the boot log must report `TDC=ON TDCO=20`.  After the BRS RX gate releases TX, the desired result is `txGood` increasing with `txFail=0`, TEC/fdTEC remaining zero, `oor=0`, `hold=0`, and `spurTX=0`.

If errors remain, do not raise the hold threshold.  Capture the first failed TX together with `tdc`, TEC/fdTEC and a CAN-FD analyzer/oscilloscope trace; the next tuning target is data-phase timing/sample-point/physical TX path, not IRQ handling.


---

## Archived file: `HOTFIX4_LOW_RATE_500K_1M.md`

# v0.4.5-m3-stage2a-1m-hotfix4

## Purpose

Field logs from hotfix2 showed that BRS TX could succeed several times but still fail intermittently at a 2 Mbit/s data phase, while RX and RTOS operation stayed healthy. To test the hypothesis that termination, harness impedance, topology, connector stubs, or transceiver margin is the remaining limiter, Stage2A is intentionally slowed down.

## Current CAN-FD timing

- Nominal/arbitration phase: **500 kbit/s**
- Data phase with BRS: **1 Mbit/s**
- ISO CAN-FD: enabled
- BRS: enabled
- TDC: enabled
- CAN kernel clock: 150 MHz
- Data timing: prescaler=6, PROP=11, SEG1=8, SEG2=5, SJW=2
- Data sample point: 80%
- TDCO: 20

The previous Stage2A timing was prescaler=3 with the same 25 TQ layout, giving 2 Mbit/s. Hotfix4 doubles only the data prescaler to 6. This preserves the segment ratios and sample point while halving the data rate, which makes the A/B result easier to interpret.

## Expected test

Configure the PC adapter to **500k nominal / 1M data / FD on / BRS on** and send STD 0x301, 12-byte FD+BRS frames. After 3 valid BRS RX frames the gateway releases its 0x302 TX.

Desired result:

- `txGood` continuously increases
- `txFail=0`
- `TEC=0`, `fdTEC=0`
- `oor=0`
- `hold=0`
- `spurTX=0`

If 1 Mbit/s is stable while the same hardware remains intermittent at 2 Mbit/s, that strongly supports a physical-layer signal-integrity/termination margin issue. The previously fixed TX safe-hold/MB0 IRQ-storm protection remains active in this build.


---

## Archived file: `HOTFIX_BRS_TX_IRQ_STORM.md`

# v0.4.5-m3-stage2a-tdc-ab-hotfix1

## 现场现象

设备/PC 先向网关发送 CAN-FD+BRS 帧后，M3 RX-first gate 释放网关 TX。网关主动发送若干帧后若 `fdTEC` 达到 safe-hold 门槛，UART2 的周期 `M3VAL`/Modbus 日志停止，但新的 CAN RX 帧仍能输出 `CANRX`。

这说明 CPU、FreeRTOS、CAN RX 与 UART2 并未整体死机，问题集中在 CAN TX 错误后的任务调度/中断路径。

## 根因

旧版 `drv_canfd_service()` 先运行 `handle_status_flags()`，safe-hold 可能先把 `s_tx_active=false`。随后 `tx_completion_service()` 因 `!s_tx_active` 直接返回，没有清除 MB0 TX pending flag。服务函数末尾又无条件重新开启 MB0 IRQ，造成 MB0 IRQ -> `can_task` 唤醒 -> 再开 MB0 IRQ的高频循环，使低优先级周期任务长期得不到运行机会。

## 修复

1. `tx_completion_service()` 以**硬件 MB0 pending flag**为准先确认/清中断，再根据软件 TX 状态处理；不允许 `!s_tx_active` 提前跳过硬件 ACK。
2. `drv_canfd_service()` 调整为先处理 TX completion，再执行错误计数/safe-hold 策略，保留终态 TX 事件。
3. 新增 `tx_mailbox_quiesce()`：统一执行 MB0 IRQ mask、abort/inactive、状态收敛和最终 pending ACK。
4. safe-hold 和 Bus-Off 都使用该收敛路径。
5. safe-hold 为锁存状态，hold 后不再重新开启 MB0 TX IRQ；MB1 RX IRQ继续开启，因此接收链路不受影响。
6. 新增 `tx_spurious_irq_count` / `spurTX` 日志，用于观察无活动 TX 时出现的陈旧 MB0 pending。
7. 初始化 MB0 后先清一次 stale flag，再开启 mailbox IRQ。
8. 增加 `tools/host_tests/test_canfd_irq_hold_regression.py`，防止后续修改重新引入上述顺序/ACK问题。

## 预期实板行为

如果 BRS TX 物理层/时序问题仍使 `fdTEC >= 8`：

- `hold` 增加到 1；
- 网关停止继续 TX；
- `M3VAL` 周期日志应继续刷新；
- `CANRX` 仍能持续接收并解码；
- `CAN irq` / `txIRQ` 不应在无新事件时高速增长；
- 不应再出现“发送约 8 帧后周期任务像卡死”的现象。

本 hotfix 解决的是 **TX error -> safe-hold 后的 IRQ storm / task starvation**。造成 BRS TX 自身 `fdTEC` 上升的物理层/数据相位问题是独立问题，仍需结合 PC 适配器 ACK、500k/2M timing、采样点、TDC、收发器与总线拓扑继续定位。


---

## Archived file: `M3_REAUDIT_V043.md`

# M3 re-audit v0.4.3 — vendor-baseline staged bring-up

## Why this revision exists

A supplied `FD_MODE_DUAL_NODE_COMMUNICATION` project is confirmed to transmit a
frame that the PC/USBCAN can receive on the same GD32H759 board. It is therefore
used as the hardware/software reference baseline.

Important: although the example is named `FD_MODE`, its `CAN_SetMsg()` actually
sets `fdf=0`, `brs=0`, `data_bytes=8` and transmits extended ID `0x1314` with
`A1 A2 A3 A4 A5 A6 A7 A8`. It proves the 500 kbit/s Classic CAN TX path, not yet
5 Mbit/s BRS CAN-FD.

## What was verified against the supplied known-good project

- Same GD32H7 CAN SPL source/header files (byte-identical).
- Same RCU source and `system_gd32h7xx.c` (byte-identical).
- 600 MHz SYSCLK profile, AHB 300 MHz, APB2 300 MHz.
- CAN2 clock source APB2/2 => CK_CAN = 150 MHz.
- CAN2 pins: PD12 RX, PD13 TX, AF5.
- Known-good nominal timing: prescaler=30, SJW=1, PROP=2, SEG1=5, SEG2=2.
- Known-good mailbox RAM: 32 units.
- Known-good RX model: mailbox 1, ID=0, IDE/RTR filtered, public mask=0.

## Problems/risk in v0.4.2

1. It started from a different 500 kbit/s timing (88% sample point) instead of
   the already proven vendor 80% timing.
2. It used two RX mailboxes with IDE compared instead of the vendor wildcard
   mailbox. This may be valid, but it added an unnecessary bring-up variable.
3. The CAN message ISR only notified `can_task` while leaving the mailbox
   interrupt source enabled. A pending mailbox source can retrigger before task
   context drains/clears the mailbox. v0.4.3 masks the pending MB source in ISR,
   drains in task context, then re-enables it.
4. v0.4.2 RX-first intentionally suppressed gateway TX before the first RX. That
   made it impossible to compare gateway TX directly with the known-good board
   example.
5. M3 validation was tied directly to CAN-FD/BRS. Physical/Classic CAN, FD format,
   and BRS data-phase timing were not isolated into independent gates.
6. TDCO=23 was a calculated bring-up assumption rather than a value established
   from this board's proven example/real TDC evidence.

## v0.4.3 staged gate

Set `GW_CANFD_BRINGUP_STAGE` in `user/config/gateway_build_config.h`.

### Stage 0 (default) — known-good Classic CAN baseline

Gateway TX every second:

- EXT ID: `0x1314`
- Classic CAN (`FDF=0`, `BRS=0`)
- DLC 8
- Data: `A1 A2 A3 A4 A5 A6 A7 A8`

This is intentionally the same transmitted frame as the supplied working
example.

PC -> gateway RX/Point DB test:

- STD ID: `0x301`
- Classic CAN
- DLC 8
- Data: `00 FA 00 00 00 00 00 00`

Expected raw log:

`[I][CANRX] id=0x00000301 STD FD=0 BRS=0 len=8 data=00 FA ...`

Expected Point DB:

- Device 2 = ONLINE
- Point 2001 = 25.0
- Point quality = GOOD

Stage0 is passed only after TX, RX->PointDB, OFFLINE, and recovery gates pass.

### Stage 1 — CAN-FD without BRS

Change:

`#define GW_CANFD_BRINGUP_STAGE 1U`

Use 500 kbit/s nominal. Send ID `0x301`, FD=1, BRS=0, 12 bytes. No data-rate
switch occurs, so 5 Mbit/s timing/TDC are not part of this gate.

### Stage 2 — CAN-FD + BRS

Change:

`#define GW_CANFD_BRINGUP_STAGE 2U`

- Nominal = 500 kbit/s, 80% sample point.
- Data = 5 Mbit/s, 80% sample point.
- BRS = ON.
- TDC = ON.
- Initial TDCO = 2; tune only from actual TDCV / FD error evidence if needed.

Only Stage2 completion closes M3 CAN-FD Hardware Gate.

## ISR architecture after re-audit

`CAN2_Message_IRQHandler` does not decode frames or touch Point DB. It masks a
pending mailbox interrupt and wakes `can_task`. `can_task` drains/clears MB0/MB1,
queues RX frames, starts queued TX, and then re-enables the mailbox sources.
This prevents a pending mailbox interrupt from repeatedly firing while keeping
protocol/database work in task context.

## M1/M2 scope

RS485 TX/RX remain DMA-based. Modbus transaction/retry, Device Manager, Point DB,
and Poll Scheduler are not rolled back by this M3 revision.


---

## Archived file: `MILESTONE_STATUS.md`

# Milestone Status — v0.7.0 5-inch LVGL GUI Baseline

## Hardware-validated frozen foundations

- RS485 UART4: TX DMA + RX DMA/IDLE, Modbus RTU master baseline
- CAN2: ISO CAN-FD 500 kbit/s, BRS OFF, TDC OFF stable baseline
- Device Manager + Point DB + Poll Scheduler
- ENET0 + LAN8720A RMII, lwIP 2.1.2
- static IPv4 baseline `192.168.103.213/24`
- PHY negotiation + ICMP ping: real-board PASS
- lwIP Netconn TCP server `192.168.103.213:5000`: real-board PASS
- TCP byte-exact echo: 20 x 1024-byte rounds / 20480 bytes PASS, zero TCP RX/TX errors in the supplied validation run

## Current milestone — local HMI / LVGL

```text
Panel       5-inch RGB, 800x480
Pixel       RGB565
Controller  GD32H759 TLI
Blit        IPA
GUI         LVGL 9.2.2
Touch       Goodix GTxxx capacitive touch over I2C2
Task        one low-priority GUI owner task
Pages       Overview / Devices / Points / Network / Diagnostics
```

The GUI is a presentation layer over thread-safe snapshots/statistics. It does not call CAN, RS485, Ethernet or TCP internals directly and does not make LVGL calls from those tasks.

## Mandatory combined Ethernet + LCD hardware prerequisite

The official 5-inch LCD pinout uses `PA8` as `TLI_R6`. The v0.6.x Ethernet baseline used `PA8/CKOUT0` to generate the LAN8720A 50 MHz RMII reference clock. Those two functions cannot coexist on one pin.

For the full gateway build, provide the Ethernet PHY with an independent/external 50 MHz RMII reference and keep:

```c
GW_ETH_RMII_REFCLK_PA8_MCO        0U
GW_ETH_RMII_REFCLK_EXTERNAL_50M   1U
```

For an LCD-only first bring-up on unmodified hardware, temporarily set `GW_ETH_ENABLE 0U`.

## Automated gate

`tools/host_tests/run_host_tests.sh` currently passes:

- Modbus regression
- CAN decoder / Point / Device regressions
- CAN mailbox IRQ and CAN-FD 500k BRS-OFF guards
- Ethernet/lwIP baseline guard
- TCP v0.6.1 Netconn guard
- GUI/LVGL baseline guard
- syntax checks for all project-owned C, 39 enabled lwIP files, 223 LVGL sources and ENET/I2C/EXMC/TLI/IPA SPL sources

Changed GUI/model files also pass `-Wall -Wextra -Werror` syntax checks and Clang static analysis.

## Real-board gate

Follow `GUI_VALIDATION_V0.7.0.md`:

1. verify SDRAM + stable 800x480 image;
2. verify Goodix touch coordinates/corners;
3. verify GUI refresh and diagnostics;
4. with external PHY 50 MHz reference, verify Ethernet link + ping + TCP echo while GUI is running;
5. verify CAN-FD 500k BRS-OFF and RS485 concurrently;
6. run a 1-hour soak, then a 24-hour production gate.

## Next milestone

After the GUI and simultaneous Ethernet/CAN/RS485 real-board gate passes, freeze the display/touch port and continue northbound protocol development. The next logical protocol milestone remains Modbus TCP or MQTT, both consuming Point DB / Device Manager APIs rather than GUI internals.


---

## Archived file: `NORTHBOUND_READINESS.md`

> v0.6.1 update: the Ethernet baseline has passed real-board ping and a bounded lwIP Netconn TCP server/echo validation layer is now present on port 5000. Future protocols should reuse the network/IP lifecycle but implement their own TCP stream framing.

# Northbound Readiness

This file defines the interface boundary for the next development phase. The v0.6.0 branch now provides ENET0/LAN8720A + lwIP 2.1.2 as the network transport baseline, but it intentionally does not select or auto-start a northbound application protocol yet.

## Southbound state that northbound may consume

Northbound code should consume normalized gateway objects, never raw CAN/RS485
frames directly:

```text
CAN / Modbus -> decoder/poll -> Point DB -> northbound adapter -> protocol
                               Device Manager ----^             
```

Primary inputs:

- `gw_point_t`: value, type, quality, timestamp, revision
- `gw_device_t`: state, last_seen, error counters

## Publish contract

For at-least-once/retry-capable protocols:

1. snapshot dirty points with `point_db_collect_dirty(out, n, false)`;
2. serialize from the snapshot only;
3. submit to the protocol transport;
4. after remote/protocol success, call
   `point_db_ack_dirty(point.id, point.revision)`;
5. on `GW_ERR_STATE`, a newer revision exists: keep it for the next publish.

This prevents an in-flight publish from clearing data that changed after the
snapshot was created.

## Recommended northbound layering

```text
northbound_task
   |
   +-- northbound_adapter      Point/Device -> protocol-neutral telemetry
   |
   +-- protocol client         MQTT / HTTP / Modbus TCP / other
   |
   +-- network transport       TCP/UDP now available; TLS later
   |
   `-- ENET0 + LAN8720A + lwIP 2.1.2 (v0.6.0 baseline)
```

Keep protocol-specific JSON/topic/register mapping out of Point DB and CAN/
Modbus drivers.

## Network dependency contract

Northbound tasks should wait for `EVT_NET_IP_READY` before creating outbound connections. They should not directly reset the PHY, manipulate ENET descriptors, or call raw lwIP APIs from arbitrary tasks. Prefer the netconn API for the first MQTT/HTTP client implementation unless there is a measured reason to use the raw API.

The current MAC/IP are bring-up defaults; fleet identity and persistent network configuration still need to be designed.

## Configuration work needed next

Before implementing a specific protocol, define:

- device identity and gateway identity
- publish/reporting mode (change/event/periodic)
- point selection and naming/address mapping
- server/broker address and reconnect policy
- time source / wall-clock timestamps
- offline buffer depth and persistence requirement
- authentication/TLS requirement
- command/downlink model, if any

## Suggested next decision

Choose the first northbound protocol and deployment constraints. Typical options
for this project are MQTT, Modbus TCP server, HTTP/REST, or a combination, but
the stable southbound core does not assume one.


---

## Archived file: `PROJECT_AUDIT_V0.5.0.md`

# Project Audit — v0.5.0 Stable Baseline

## Scope

Reviewed the complete application-owned gateway path:

- BSP/debug and interrupt ownership
- FreeRTOS application tasks/static objects
- CAN-FD driver and decoder
- RS485 DMA/IDLE transport and bus manager
- Modbus RTU master
- Device Manager
- Point DB
- Poll Scheduler
- application/demo configuration
- Keil project source inventory
- host regression suite and Python tools

Vendor SPL/FreeRTOS source was not rewritten; relevant CAN SPL behavior was
reviewed where it affects driver workarounds.

## Fixes in this baseline

### 1. Remove BRS from the active production configuration

CAN-FD remains enabled but the complete frame stays at 500 kbit/s. Hardware BRS
is disabled and `drv_canfd_submit()` rejects a frame whose `brs` field is true.

### 2. Remove automatic demo traffic from production defaults

The previous M3 branch registered demo Modbus/CAN devices and could run automatic
validation traffic. Demo object registration and the validation task are now
compile-time gated and disabled by default.

### 3. CAN clock / timing correction

CK_CAN changed from APB2/2 to APB2. With the current system profile this is
300 MHz, so the 500k prescaler changed to 60 while retaining 10 TQ and an 80%
sample point.

### 4. RX mailbox latency / unlock path

The old design deferred receive-mailbox reading until `can_task`. The stable
baseline reads the complete mailbox immediately in CAN2 message ISR and only
queues the raw frame. On SPL RX read failure, the code explicitly performs the
global mailbox unlock before clearing/re-arming the mailbox.

### 5. Bus-off recovery TEC handling

After bus-off recovery the driver enters CAN inactive mode, clears TEC, returns
to normal mode, then acknowledges recovery.

### 6. TX mailbox initialization ordering

MB0 TX mailbox configuration now occurs only after CAN normal mode is entered.

### 7. Retain MB0 stale-pending IRQ-storm fix

TX completion acknowledges the hardware pending flag before any software
`s_tx_active` early return. Quiesce paths mask, settle and acknowledge MB0. This
prevents the previously reproduced high-priority CAN task starvation failure.

### 8. Point DB northbound-safe dirty ACK

`gw_point_t` now has a `revision`. Every value or quality change increments it.
`point_db_ack_dirty(point_id, revision)` clears dirty only if no newer update
arrived after the published snapshot.

### 9. Do not silently lose a Point update on transient mutex contention

`task_data` retries one transient `GW_ERR_BUSY` result and logs any final Point
DB update failure instead of silently discarding it.

### 10. Cleanup / operator-safety changes

- Stable SocketCAN helper always uses BRS OFF.
- The old M3 helper name is a compatibility wrapper to the stable helper, so an
  outdated command cannot accidentally inject BRS traffic.
- Active startup/version logs identify `v0.5.0-canfd500-stable`.
- Obsolete high-speed structural tests are retained under test history only.

## Automated verification performed

- Modbus regression: PASS
- CAN decoder regression: PASS
- Point/Device regression including revision ACK: PASS
- CAN mailbox IRQ structural regression: PASS
- CAN-FD 500k BRS-OFF configuration/errata guard: PASS
- project baseline guard: PASS
- all active project user C files: `-Wall -Wextra -Werror` syntax PASS
- core user modules: Clang static analyzer PASS
- optional stable board-validation code syntax: PASS
- Python helper/tests syntax: PASS
- Keil `gateway.uvprojx` XML parse: PASS

## Residual validation boundary

This environment cannot replace Keil ARM Compiler 6 linking, GD32H759 real-board
execution, oscilloscope/CAN analyzer measurements, transceiver/termination
validation, or long-duration dual-bus soak testing. Those remain the release
gates in `STABLE_BASELINE_V0.5.0.md`.


---

## Archived file: `PROJECT_AUDIT_V0.6.0.md`

# Project Audit v0.6.0 — Ethernet/LwIP Baseline

## Source basis

Ethernet hardware mapping and lwIP port were derived from the user-supplied
GD32H759 FreeRTOS TCP client example (`tcp_client_FreeRTOS.zip`). The gateway's
existing FreeRTOS, CAN, RS485, Point DB and Device Manager architecture was kept
instead of replacing `main.c` with the demo.

## Changes

- integrated lwIP 2.1.2 and GD32H7 FreeRTOS `sys_arch` port;
- integrated ENET0 + LAN8720A RMII hardware initialization;
- PHY address 0, PA8 50 MHz CKOUT0, official RMII pin map;
- added Ethernet RX task, bounded TX wait and driver statistics;
- added PHY link monitor and 10/100M duplex diagnostics;
- added static IPv4 baseline and optional DHCP path;
- added `EVT_ETH_LINK_UP` and `EVT_NET_IP_READY` synchronization bits;
- added M7 MPU non-cacheable region for ENET DMA descriptors/buffers;
- added Keil source groups/include paths for ENET and lwIP;
- did not enable the official demo TCP client automatically;
- preserved CAN-FD 500k BRS-OFF stable baseline.

## Bugs / risks found and handled during integration

1. **Cortex-M7 DMA/cache coherency** — ENET descriptor/buffer area is now
   non-cacheable before D-cache is enabled.
2. **Boot dependency on cable** — network task waits/retries instead of blocking
   all gateway startup when PHY link is absent.
3. **Infinite TX descriptor wait in demo** — replaced by 100 ms bounded wait and
   diagnostic counter.
4. **Potential scheduler starvation** — Ethernet RX drain has a fixed per-wake
   budget and task/IRQ priorities are below the southbound critical paths.
5. **Invalid RX descriptor can strand later ring entries** — invalid/allocation-
   failed descriptors are released and the drain continues until DMA ownership is
   reached.
6. **Link state correctness** — link is monitored and propagated through lwIP
   instead of setting it permanently UP.
7. **Demo traffic contamination** — TCP client demo is excluded from automatic
   startup so Ethernet transport can be validated independently.
8. **lwIP port incompleteness** — the supplied `arch/cc.h` leaves `LWIP_PLATFORM_ASSERT` as a no-op and does not provide `LWIP_RAND`, which breaks DNS compilation/use. The gateway now supplies a project `arch/cc.h`, a non-cryptographic lwIP transaction-ID/ephemeral-port PRNG, and a fatal platform assertion hook. TLS will use hardware TRNG separately.
9. **Fleet MAC collision risk** — current locally-administered MAC is explicitly
   marked bring-up only; unique device MAC assignment is required before shipping.

## Automated validation performed in this environment

- Modbus regression: PASS
- CAN decoder regression: PASS
- Point/Device regression: PASS
- CAN mailbox IRQ regression: PASS
- CAN-FD 500k BRS-OFF baseline regression: PASS
- project baseline regression: PASS
- Ethernet/lwIP structural regression: PASS
- all 39 lwIP sources included by the Keil project: host syntax PASS
- changed gateway C files: Clang `-Wall -Wextra -Werror` syntax PASS using the
  project headers and test-only host stubs for ARM barrier intrinsics
- Ethernet manager/port/interface: Clang static analyzer PASS
- GD32 ENET SPL and lwIP FreeRTOS `sys_arch`/`tcpip.c`: syntax PASS (host-width
  MMIO cast warnings suppressed only for the vendor SPL host check)
- Keil project XML parse: PASS

## Remaining hardware gates

This environment cannot replace the ArmClang/Keil linker placement check or a
real LAN8720A/ENET DMA test. Before declaring Ethernet production-ready:

- Clean/Rebuild in Keil and verify no linker overlap around `0x30000000` and
  `0x30004000`;
- verify PHY ID/link/negotiation on the actual board;
- ping and ARP test;
- cable hot-plug test;
- concurrent CAN + RS485 + Ethernet soak;
- inspect FreeRTOS high-water marks/heap during the soak.


---

## Archived file: `PROJECT_AUDIT_V0.6.1.md`

# Project Audit v0.6.1 — TCP Communication Layer

## Basis

v0.6.1 starts from the real-board-tested v0.6.0 Ethernet baseline where the
LAN8720A PHY negotiated 100M full-duplex and `192.168.103.213` successfully
responded to ICMP ping.  The existing Ethernet hardware/driver path is kept
unchanged except for adding a TCP service task above lwIP.

## Added

- `user/net/gw_tcp_server.c/.h`;
- lwIP Netconn single-client TCP server on port 5000;
- exact byte-stream echo for acceptance testing;
- bounded accept/receive/send waits;
- link/IP loss unwind and listener restart;
- TCP connection and byte/error statistics;
- `EVT_TCP_SERVER_READY` and `EVT_TCP_CLIENT_CONNECTED`;
- PC-side `tools/tcp_echo_test.py`;
- Keil project source registration;
- structural TCP regression guard.

## Design risks handled

1. **Blocking forever in accept/recv/send** — all service waits are bounded so
   cable/peer failure cannot permanently trap the TCP task.
2. **Using lwIP raw API from arbitrary RTOS tasks** — v0.6.1 uses Netconn API,
   keeping raw-core thread ownership out of gateway application tasks.
3. **TCP traffic starving southbound work** — server priority is 2 and a
   continuous receive burst yields periodically; CAN/RS485 remain priority 5.
4. **Use-after-free on echo data** — writes use `NETCONN_COPY` before the
   received netbuf is deleted.
5. **Listener surviving invalid network state** — IP-ready loss destroys the
   listener and recreates it only after network readiness returns.
6. **Assuming TCP packet boundaries equal application messages** — the baseline
   deliberately exposes stream semantics and leaves framing to later protocols.
7. **Resource leakage across client reconnects** — every accepted Netconn has a
   close/delete path; listeners are deleted on restart.

## Automated validation

- Modbus regression: PASS
- CAN decoder regression: PASS
- Point/Device regression: PASS
- CAN mailbox IRQ regression: PASS
- CAN-FD 500k BRS-OFF regression: PASS
- project stable-baseline regression: PASS
- Ethernet/lwIP regression: PASS
- TCP v0.6.1 structural regression: PASS
- all application C + 39 selected lwIP files + ENET SPL syntax: PASS
- TCP server / network manager / Ethernet interface Clang static analysis: PASS
- Python TCP test syntax: PASS
- Keil project XML parse/source presence: PASS

## Remaining hardware gate

The server cannot be fully validated without the physical board.  Run
`TCP_VALIDATION_V0.6.1.md`.  A successful ping from v0.6.0 proves the underlying
network path, but TCP listen/accept/echo/reconnect still require the v0.6.1
firmware to be compiled, flashed and exercised on hardware.


---

## Archived file: `PROJECT_AUDIT_V0.7.0.md`

# Project audit — v0.7.0 GUI/LVGL baseline

## Basis

- gateway v0.6.1 TCP baseline
- supplied official `TLI_LCD_IPA_LVGL` example
- supplied 5-inch panel timing/pin mapping, SDRAM, TLI/IPA and Goodix references

## Integrated components

- LVGL 9.2.2 (223 source files selected exactly from the vendor Keil example)
- TLI RGB 800x480 layer 0, RGB565
- IPA accelerated full-frame copy
- EXMC SDRAM driver
- Goodix GTxxx I2C2 touch port
- low-priority FreeRTOS GUI task
- Device Manager and Point DB snapshot APIs for presentation/northbound readers

## Bugs/risks addressed during integration

1. **PA8 hard conflict** — LCD R6 conflicts with v0.6.x Ethernet CKOUT0.
   Full combined mode now uses a compile-time external-RMII-clock configuration
   and rejects PA8 MCO + GUI builds.
2. **IPA stale completion flag risk** — flush path clears FTF/error flags before
   each transfer and acknowledges completion afterward.
3. **Unbounded touch I2C waits** — Goodix accesses now have finite waits and bus
   recovery so a failed controller cannot wedge the task forever.
4. **M7 DMA/display cache coherency** — first 4 MiB of LCD SDRAM is MPU
   non-cacheable while the existing Ethernet DMA non-cache window is retained.
5. **GUI reading mutable databases** — new non-destructive snapshot APIs copy
   Point/Device data while their existing mutex is held; LVGL never traverses
   private mutable arrays directly.
6. **GUI interfering with realtime communications** — GUI runs at priority 1;
   refresh cadence is 500 ms and only the GUI task calls LVGL.
7. **Display failure affecting gateway boot** — LCD/touch initialization happens
   after the scheduler starts in its own low-priority task; failure logs and
   disables GUI instead of halting CAN/RS485/network tasks.
8. **SDRAM allocation overlap** — compile-time assertions protect framebuffer,
   LVGL draw buffer and self-test ranges inside the non-cacheable SDRAM window.
9. **GUI numeric conversion overflow** — large finite F32/F64 Point values are
   range-checked before fixed-point formatting.

## Automated validation performed

- Modbus regression: PASS
- CAN decoder regression: PASS
- Point/Device regression, including snapshot APIs: PASS
- CAN mailbox IRQ regression: PASS
- CAN-FD 500k BRS-OFF baseline guard: PASS
- project stable-baseline guard: PASS
- Ethernet/lwIP baseline guard: PASS
- TCP Netconn baseline guard: PASS
- GUI/LVGL structural regression: PASS (223 LVGL sources)
- changed application sources `-Wall -Wextra -Werror` syntax: PASS
- all user sources + 39 lwIP + 223 LVGL + ENET/I2C/EXMC/TLI/IPA SPL syntax: PASS
- GUI/LVGL port static analysis: PASS
- Keil project XML and all FilePath entries: PASS

## Not validated in this environment

This environment cannot replace real GD32H759/5-inch-panel testing. The final
hardware gates are:

- exact RGB timing/electrical behavior on the attached panel;
- Goodix touch product/orientation on that panel revision;
- external LAN8720A 50 MHz RMII clock hardware after freeing PA8;
- final Keil ArmClang link/flash image size;
- long-duration GUI + Ethernet/TCP + CAN/RS485 concurrency soak.


---

## Archived file: `README.md`

# GD32H759 Industrial Gateway — v0.7.0 5-inch LVGL GUI Baseline

This branch keeps the real-board-validated CAN-FD 500k BRS-OFF, ENET0/LAN8720A/lwIP and v0.6.1 TCP Netconn baselines, then integrates the supplied official 5-inch `TLI_LCD_IPA_LVGL` example as a low-priority presentation layer. The GUI uses LVGL 9.2.2, TLI RGB565, IPA, EXMC SDRAM and Goodix capacitive touch without replacing the existing FreeRTOS/CAN/RS485/Ethernet architecture.

## Frozen baseline

- MCU: GD32H759
- RTOS: FreeRTOS, static application tasks/queues/mutexes
- Debug: USART2, 115200-8-N-1
- RS485: UART4, TX DMA + RX DMA/IDLE, Modbus RTU master
- CAN: CAN2, ISO CAN-FD, **500 kbit/s for the complete frame, BRS OFF**
- CAN payload: legal ISO CAN-FD lengths up to 64 bytes
- TDC: OFF (no data-rate switch)
- Ethernet: ENET0 RMII + LAN8720A, lwIP 2.1.2, static IPv4 baseline
- TCP: lwIP Netconn server, port 5000, single active client, echo acceptance service
- GUI: 5-inch 800x480 RGB565, TLI + IPA + LVGL 9.2.2
- Touch: Goodix GTxxx over I2C2, polling input device
- GUI task: priority 1, model refresh every 500 ms
- Demo objects / automatic poll traffic: OFF by default
- Board validation traffic: OFF by default
- Per-frame CAN trace: OFF by default

Boot banner:

```text
GD32H759 industrial gateway v0.7.0-gui-lvgl-baseline
[I][SYS] Ethernet: ENET0 RMII + LAN8720A + lwIP 2.1.2
[I][SYS] TCP: Netconn server port 5000, single-client echo baseline
[I][SYS] GUI: 5-inch 800x480 RGB + LVGL 9.2.2 + Goodix touch
[I][SYS] HW: Ethernet PHY RMII 50MHz must be externally supplied; PA8 is LCD R6
[I][CAN] CAN2 stable: CAN-FD 500k, BRS=OFF, TDC=OFF, CK_CAN=APB2
```

## GUI v0.7.0 baseline

The display integration is based on the supplied official `TLI_LCD_IPA_LVGL` example, but all LVGL calls are owned by one low-priority GUI task. The UI contains Overview, Devices, Points, Network and Diagnostics pages and reads the existing gateway model through mutex-protected snapshot/statistics APIs.

Display memory uses two RGB565 full-screen buffers in external SDRAM: TLI framebuffer at `0xC0000000` and LVGL render buffer at `0xC0100000`. The first 4 MiB of SDRAM is MPU non-cacheable for CPU/TLI/IPA coherency. The official 5-inch timing (800x480, HSW/HBP/HFP 1/46/40, VSW/VBP/VFP 3/23/13) is retained.

**Hardware prerequisite:** LCD R6 uses PA8, which v0.6.x used as CKOUT0 for the LAN8720A 50 MHz RMII reference. Full Ethernet + full-color LCD therefore requires an independent external 50 MHz PHY clock. The project has a compile-time guard against PA8-MCO + GUI. Read `HARDWARE_NOTE_ETH_LCD_PA8.md` before flashing the combined build.

The supplied LCD example also recommends a stable power source / both USB power inputs when necessary; weak USB hub power can cause flicker or corrupted display.

See `GUI_BASELINE_V0.7.0.md` and `GUI_VALIDATION_V0.7.0.md`.

## Ethernet baseline

The Ethernet implementation is based on the supplied GD32H759 FreeRTOS TCP-client example, but is integrated into the existing gateway RTOS architecture instead of replacing it. Current defaults are:

```text
PHY       LAN8720A, address 0
Interface ENET0 RMII
Link      10/100M auto-negotiation
IPv4      192.168.103.213/24
Gateway   192.168.103.254
DHCP      OFF (compiled and optional)
MAC       02:47:44:32:48:01 (bring-up only; make unique before fleet deployment)
```

The ENET DMA descriptor/buffer window at `0x30000000..0x30003FFF` is MPU non-cacheable before D-cache is enabled. lwIP heap uses the reserved SRAM window at `0x30004000`. Boot does not block if the cable is unplugged, TX descriptor waits are bounded, and the network manager exports `EVT_ETH_LINK_UP` / `EVT_NET_IP_READY` for later northbound tasks.

See `ETHERNET_BASELINE_V0.6.0.md` for the frozen physical/link/IP layer.

## TCP v0.6.1 baseline

The v0.6.1 service waits on `EVT_NET_IP_READY`, then listens on TCP port 5000 using the lwIP Netconn API. Accept, receive and send paths all have finite timeouts. Only one client is actively serviced at a time; received bytes are echoed exactly for acceptance testing. Link/IP loss closes the client and listener, then the server resumes automatically when IP readiness returns.

The TCP service runs at priority 2, below the lwIP tcpip thread and below CAN/RS485/data tasks. Continuous receive traffic yields periodically to avoid turning the validation service into a scheduler hog.

See `TCP_BASELINE_V0.6.1.md` and `TCP_VALIDATION_V0.6.1.md`.

## Why BRS is disabled

The previous BRS experiments were useful for exposing TX data-phase and IRQ
corner cases, but the stable gateway baseline prioritizes deterministic field
reliability. CAN-FD remains enabled, so 12/16/20/24/32/48/64-byte payloads are
still available; only the data-rate switch is disabled.

All nodes on the test bus must use compatible CAN-FD settings and must not emit
BRS frames while validating this baseline.

## CAN timing

The driver selects CK_CAN = CK_APB2. With the current 600 MHz system profile,
AHB is 300 MHz and APB2 is 300 MHz. The CAN nominal timing is:

```text
CK_CAN      300 MHz
Prescaler   60
SYNC        1 TQ
PROP        2 TQ
SEG1        5 TQ
SEG2        2 TQ
Total       10 TQ
Bitrate     500 kbit/s
Sample      80%
```

The FD data-timing fields are deliberately configured to the same values as the
nominal timing. BRS requests are rejected by `drv_canfd_submit()` in this build.

## CAN RX/TX architecture

```text
CAN2 message IRQ
  |-- RX MB1: read complete mailbox immediately -> q_can_rx
  |                                      |
  |                                      v
  |                                  can_task
  |                                      |
  |                                      v
  |                                  CAN Decoder
  |                                      |
  |                                      v
  |                               q_point_update
  |                                      |
  |                                      v
  |                                  task_data
  |                                      |
  |                                      v
  |                                   Point DB
  |
  `-- TX MB0: mask pending source -> notify can_task
                                      |
                                      v
                              completion / errors
```

Protocol decoding, Device Manager state and Point DB writes remain outside ISR
context. Only the complete raw CAN frame is copied to the static RX queue in the
mailbox ISR.

## GD32H7 CAN defensive fixes retained

The driver contains workarounds/defensive handling for the GD32H73x/H75x CAN
limitations relevant to this project:

- TX mailbox is initialized after entering normal mode.
- RX mailbox is drained promptly in the message ISR.
- RX read failure performs an explicit global mailbox unlock before re-arming.
- Bus-off recovery explicitly clears TEC after entering inactive mode.
- CK_CAN uses APB2 instead of APB2/2.
- MB0 TX pending is acknowledged independently of the software TX-active flag,
  preventing the previously observed interrupt-storm/task-starvation failure.

See `PROJECT_AUDIT_V0.5.0.md` and `STABLE_BASELINE_V0.5.0.md`.

## Production defaults

`user/config/gateway_build_config.h` defaults to:

```c
GW_CANFD_ENABLE                 1
GW_CANFD_BRS_ENABLE             0
GW_CANFD_TDC_ACTIVE             0
GW_DEMO_MODBUS_CONFIG_ENABLE    0
GW_DEMO_CAN_CONFIG_ENABLE       0
GW_M3_BOARD_VALIDATION_ENABLE   0
GW_CANFD_RX_TRACE_ENABLE        0
GW_GUI_ENABLE                    1
GW_ETH_RMII_REFCLK_PA8_MCO       0
GW_ETH_RMII_REFCLK_EXTERNAL_50M  1
```

This means a normal build does **not** create demo devices, does not auto-poll a
test Modbus slave, and does not transmit CAN validation frames.

For CAN board validation only, enable `GW_DEMO_CAN_CONFIG_ENABLE` and
`GW_M3_BOARD_VALIDATION_ENABLE`; Modbus demo polling remains independently off.
The validation task in this branch uses CAN-FD BRS-OFF only.

## Data model / northbound preparation

Point DB remains the single source of truth for southbound data. `gw_point_t`
now contains a monotonic `revision` counter. A northbound publisher should:

1. call `point_db_collect_dirty(..., false)` to snapshot dirty points;
2. publish/submit the snapshot;
3. only after protocol-level success call
   `point_db_ack_dirty(point_id, snapshot.revision)`.

If a newer point update arrived while the publish was in flight, the ACK returns
`GW_ERR_STATE` and keeps the point dirty, preventing data loss.

Do not use `point_db_collect_dirty(..., true)` for a reliable northbound
protocol that can fail asynchronously; that legacy option is retained only for
compatibility.

## Host validation

Run:

```bash
tools/host_tests/run_host_tests.sh
```

It covers:

- Modbus RTU request/response regression
- CAN decoder regression
- Point / Device Manager regression
- CAN mailbox IRQ regression
- CAN-FD 500k BRS-OFF configuration/errata guard
- project stable-baseline / northbound dirty-ACK guard
- Ethernet/TCP structural guards
- GUI/LVGL structural guard and embedded syntax checks

Linux SocketCAN helper:

```bash
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 500000 dbitrate 500000 fd on
sudo ip link set can0 up
python3 tools/canfd_stable_socketcan.py --channel can0 --raw 250
```

The helper always sends `is_fd=True, bitrate_switch=False`.

## Hardware acceptance before northbound work

A stable baseline should pass a long-duration CAN/RS485 soak with:

```text
CAN rx/tx counters continue increasing
rx_overrun       = 0
rx_queue_drop    = 0
tx_spurious_irq  = 0
busoff           = 0
TEC/REC          = 0 during normal operation
fdTEC/fdREC      = 0
FreeRTOS stack overflow / malloc failure = none
RS485 timeout/CRC behavior matches connected devices
```

The v0.6.0 PHY/RMII/DMA/lwIP baseline has passed real-board ping. For v0.6.1, complete `TCP_VALIDATION_V0.6.1.md`; after TCP echo, reconnect and concurrency soak pass, the network transport can be frozen for Modbus TCP/MQTT/HTTP work.

## v0.6.1 validation

Run `tools/host_tests/run_host_tests.sh`, then flash and follow `TCP_VALIDATION_V0.6.1.md`. `tools/tcp_echo_test.py` validates repeated byte-exact TCP round trips from a PC. The official vendor raw-API TCP client demo is still not auto-started.


## v0.7.0 GUI validation

Run `tools/host_tests/run_host_tests.sh`, flash the board, then follow `GUI_VALIDATION_V0.7.0.md`. Before combined Ethernet+LCD testing, satisfy the PA8/RMII clock requirement in `HARDWARE_NOTE_ETH_LCD_PA8.md`. The first real-board gate is a stable 800x480 display and touch; the second is simultaneous Ping/TCP echo while the GUI, CAN-FD 500k BRS-OFF and RS485 remain healthy.

## Historical files

Documents named `V0.4.x_*`, `M3_*`, `HOTFIX*` and the old board-validation
reports are retained as engineering history. They describe BRS bring-up and
must not be treated as current v0.5.0 runtime configuration.


---

## Archived file: `RX_DIAG_v0.2.3.md`

# v0.2.3 RX diagnostic build

> **Legacy diagnostic note:** this document describes a pre-V0.3 bring-up revision. The current default RS485 transport is TX DMA + RX DMA/IDLE; no TX polling path remains.


- UART4 TX remains polling mode (TX DMA bypassed).
- UART4 RX remains DMA driven.
- Every RX DMA snapshot is printed before Modbus validation.
- Log format:

```text
[D][RS485] TX(POLL): 01 03 00 00 00 02 C4 0B
[D][RS485] RX(DMA) len=9 expected=9
[D][RS485] RXHEX 0000: 01 03 04 08 9B 00 64 XX XX
[D][RS485] RX validate=0 exception=0
```

If UART IDLE splits a response, more than one RX snapshot can appear. The dump is cumulative because RX DMA resumes at the previous offset.


---

## Archived file: `RX_DMA_LENGTH_FIX_v0.2.4.md`

# v0.2.4 RS485 RX DMA 有效长度修复

> **Legacy diagnostic note:** this document describes a pre-V0.3 bring-up revision. The current default RS485 transport is TX DMA + RX DMA/IDLE; no TX polling path remains.


问题现象：从站实际 Modbus FC03 响应只有 9 字节，但日志出现：

```text
[D][RS485] RX(DMA) len=256 expected=9
```

导致 9 字节有效帧之后的 RX DMA 空 buffer 一起进入协议层。

## 修复

1. `drv_rs485_rx_arm()` 增加 `max_len` 参数，本次 DMA 只接收当前事务允许的最大有效响应长度。
   - FC03/FC04 根据请求 quantity 预计算：`5 + quantity * 2`。
   - 当前 smoke test `quantity=2`，因此 RX DMA count 直接配置为 9，而不是 256。
   - RAW 协议使用事务传入的 `expected_rx_length`。
   - 无法预先判断长度的 Modbus 功能码才回退到 256 字节缓冲区。
2. 新增 `s_rx_dma_limit`，IDLE、FTF、resume、read 全部以本次 DMA limit 为边界，不再固定使用 256 计算累计长度。
3. UART IDLE ISR 不再“先关闭 DMA channel，再读取剩余计数”。现在先关闭 UART RX DMA request，等待 DMA 剩余计数稳定后读取 `CHCNT`，最后再关闭 DMA channel，避免关闭动作影响有效长度采样。
4. 诊断任务调用点同步适配新接口；传 `0` 表示允许使用完整 RX buffer。

## 预期结果

对于请求：

```text
01 03 00 00 00 02 C4 0B
```

正常 9 字节响应应输出：

```text
[D][RS485] RX(DMA) len=9 expected=9
[D][RS485] RX validate=0 exception=0
```

不会再把第 10..256 字节空 buffer 当成接收数据。


---

## Archived file: `STABLE_BASELINE_V0.5.0.md`

# v0.5.0 Stable Baseline — CAN-FD 500k / BRS OFF

## Objective

Freeze a low-risk southbound baseline before Ethernet/northbound protocol work.
The baseline keeps CAN-FD payload capacity but removes data-rate switching.

## Active CAN contract

- ISO CAN-FD: enabled
- Arbitration and whole-frame rate: 500 kbit/s
- BRS: disabled
- TDC: disabled
- CK_CAN: APB2
- RX mailbox: MB1 wildcard, 64-byte mailbox storage
- TX mailbox: MB0
- application TX rejects `brs=true`
- legal FD payload lengths: 0..8, 12, 16, 20, 24, 32, 48, 64

## Production behavior

The firmware does not register demo devices or schedule demo Modbus polling by
default. It also does not transmit validation CAN frames. This is deliberate:
the northbound phase should start from an idle, configuration-driven gateway,
not a board-demo workload.

## CAN silicon-defensive behavior

1. CK_CAN uses APB2 and the 500k timing is recalculated for 300 MHz.
2. RX mailbox is read immediately in CAN2 message ISR and copied to a static
   FreeRTOS queue. Decode/data-model work remains in `can_task`/`task_data`.
3. An RX BUSY/read failure explicitly unlocks the global mailbox before re-arm.
4. TX mailbox is created only after CAN normal mode is entered.
5. Bus-off recovery enters inactive mode and clears TEC before normal mode.
6. TX pending interrupt is always acknowledged from hardware state, preventing
   stale MB0 interrupt storms.

## Board soak acceptance

Recommended minimum gate before starting northbound integration:

- 2-node CAN-FD BRS-OFF test at 500k, correct two-end termination.
- Continuous 12-byte FD traffic in both directions for at least 1 hour.
- Repeat with representative 64-byte payloads and expected production bus load.
- `rx_overrun=0`, `drop=0`, `spurTX=0`, `boff=0`.
- TEC/REC and fdTEC/fdREC remain zero in a healthy bus.
- Concurrent RS485 polling remains stable and no task-starvation symptom occurs.
- Exercise CAN device timeout -> OFFLINE -> recovery.
- Exercise Modbus timeout / CRC / exception paths.

A 24-hour combined CAN + RS485 soak is recommended before declaring the
southbound baseline frozen for product use.


---

## Archived file: `TCP_BASELINE_V0.6.1.md`

# TCP Communication Baseline v0.6.1

## Goal

v0.6.1 adds a bounded, FreeRTOS-friendly TCP communication layer on top of the
real-board-verified v0.6.0 ENET0/LAN8720A/lwIP baseline.  Ethernet PHY, RMII,
DMA, MPU/cache policy and the stable CAN/RS485 southbound paths are not changed.

The first application-facing acceptance service is a single-client TCP echo
server.  It exists to validate TCP connection lifecycle and byte-stream I/O
before Modbus TCP, MQTT or HTTP are layered on top.

## Runtime defaults

```text
Gateway IP       192.168.103.213/24
TCP API          lwIP Netconn
Listen address   0.0.0.0
Listen port      5000
Backlog          2
Active clients   1 at a time
Echo             enabled
Accept timeout   500 ms
Receive timeout  500 ms
Send timeout     1000 ms
TCP task prio    2
TCP task stack   1024 words
```

The lwIP tcpip thread remains priority 3, Ethernet RX task priority 4, data task
priority 4, and CAN/RS485 service tasks priority 5.  Continuous TCP traffic is
therefore not permitted to become the highest-priority gateway workload.

## Lifecycle

```text
EVT_NET_IP_READY
       |
       v
create Netconn TCP listener
       |
       v
bind :5000 -> listen(backlog=2)
       |
       +---- EVT_TCP_SERVER_READY
       |
       v
accept with 500 ms timeout
       |
       v
one active client
       +---- EVT_TCP_CLIENT_CONNECTED
       |
       v
recv with 500 ms timeout
       |
       +---- echo exact TCP byte stream using NETCONN_COPY
       |
       v
peer close / reset / link loss / IP loss / send error
       |
       v
close + delete client
       |
       v
return to accept, or destroy listener and wait for IP-ready again
```

No accept, receive or send operation is intentionally allowed to block forever.
This is important for cable hot-plug, peer failure and future northbound service
restart behavior.

## TCP stream semantics

TCP is a byte stream.  A `send()` call at the PC does **not** imply one matching
`recv()` at the gateway.  v0.6.1 deliberately echoes each received stream chunk
without inventing packet boundaries.

Future application protocols must implement their own framing:

- Modbus TCP: MBAP length field;
- MQTT: fixed header + Remaining Length;
- HTTP: headers/content-length/chunked framing.

Do not build future northbound code on the assumption that one Netconn receive
is one application message.

## Diagnostics

Connection events are logged as:

```text
[I][TCP] server listening on port 5000 (echo enabled)
[I][TCP] client connected 192.168.103.19:xxxxx
[I][TCP] listen=1 client=1 acc=1 disc=0 rx=.../...B tx=.../...B rxErr=0 txErr=0
[I][TCP] client disconnected
```

Event bits exported in `rtos_objects.h`:

```text
EVT_NET_IP_READY
EVT_TCP_SERVER_READY
EVT_TCP_CLIENT_CONNECTED
```

## Deliberate limitations

- one actively serviced TCP client at a time;
- no TLS yet;
- no authentication yet;
- no application protocol framing yet;
- no generic cross-task send API yet;
- echo service is a validation service, not a production northbound protocol.

Those constraints keep the first TCP milestone measurable and avoid premature
abstractions before protocol requirements are fixed.


---

## Archived file: `TCP_VALIDATION_V0.6.1.md`

# TCP v0.6.1 Real-board Validation

## 1. Build and boot

Keil: Clean Target -> Rebuild All -> flash.

Expected boot excerpt:

```text
GD32H759 industrial gateway v0.6.1-tcp-baseline
[I][SYS] Ethernet: ENET0 RMII + LAN8720A + lwIP 2.1.2
[I][SYS] TCP: Netconn server port 5000, single-client echo baseline
[I][ETH] link negotiated: 100M full-duplex
[I][NET] link UP IP=192.168.103.213 mask=255.255.255.0 gw=192.168.103.254
[I][TCP] server listening on port 5000 (echo enabled)
```

Keep the PC adapter in the same subnet, for example `192.168.103.19/24`.

## 2. Keep ICMP baseline green

```text
ping 192.168.103.213
```

Ping must remain stable before diagnosing TCP.

## 3. Port-open check

Windows PowerShell:

```powershell
Test-NetConnection 192.168.103.213 -Port 5000
```

Expected: `TcpTestSucceeded : True`.

UART should report a client connect and disconnect.

## 4. Echo correctness

From a PC with Python 3:

```text
python tools/tcp_echo_test.py --host 192.168.103.213 --port 5000 --rounds 20 --size 1024
```

Expected final line:

```text
TCP echo PASS: ...
```

UART counters should show `rxErr=0 txErr=0` and matching growth in RX/TX bytes.

## 5. Connection lifecycle

Repeat the echo test several times.  `acc` and `disc` must grow and the listener
must remain available after every disconnect.

While a client is connected, unplug Ethernet.  Within the bounded receive/link
poll interval the client must close and UART should show the server paused while
waiting for IP-ready.  Reconnect the cable; after link/IP recovery the server
must listen on port 5000 again without MCU reset.

## 6. Concurrency

Run TCP echo traffic while CAN-FD 500k BRS-OFF and RS485/Modbus traffic are
active.  Requirements:

```text
TCP rxErr / txErr        0 in normal operation
Ethernet rxAlloc/rxDrop  0
Ethernet txFail/txWait   0 in normal operation
CAN rx_overrun/drop      0
CAN TEC/REC              0 in normal operation
No task stops refreshing
No HardFault / stack overflow / malloc failure
```

## 7. Soak

After short functional validation, run a 1-hour TCP+CAN+RS485 concurrency soak.
The later production gate should include a 24-hour soak.


---

## Archived file: `user/docs/KEIL_PROJECT_SETUP.md`

# Keil MDK Project Setup — GD32H759 Gateway V0.2.1

## 1. RTE components
Select only:
- CMSIS -> CORE
- Device -> Startup

Do not select Keil Network/File System/USB middleware for this phase.

## 2. Target
- Device: GD32H759IMK6 / GD32H759IM (depending on DFP naming)
- Compiler: Arm Compiler 6
- Preprocessor define: `GD32H7XX`

Use the device-pack/startup memory setup, or match the GigaDevice demo:
- IROM1: start 0x08000000, size 0x003C0000
- IRAM1: start 0x24000000, size 0x000E0000

Do not manually add a second startup file or second `system_gd32h7xx.c` if RTE Startup already added them.

## 3. Keil groups

### 00_RTE_Startup
Managed by RTE:
- startup_gd32h7xx.s
- system_gd32h7xx.c

### 01_GD32_SPL
Add from `GD32H7xx_Firmware_Library/GD32H7xx_standard_peripheral/Source`:
- gd32h7xx_rcu.c
- gd32h7xx_gpio.c
- gd32h7xx_usart.c
- gd32h7xx_dma.c
- gd32h7xx_misc.c

### 02_FreeRTOS_Kernel
From the same GigaDevice Demo Suites V2.1.0 used by the project, initially keep the vendor demo version/port:
- tasks.c
- queue.c
- list.c
- timers.c
- event_groups.c
- heap_4.c
- portable/.../port.c (use the exact port used by the vendor FreeRTOS demo first)

`stream_buffer.c` can be added now for future use but is not required by the current V0.2.1 code.
`croutine.c` is not needed because `configUSE_CO_ROUTINES == 0`.

### 03_Config
- config/FreeRTOSConfig.h
- config/gateway_build_config.h
- config/gd32h7xx_libopt.h

### 04_BSP
- bsp/bsp_cache.c
- bsp/bsp_debug_uart.c

### 05_Common
- common/gw_message.c

### 06_Driver
- driver/drv_rs485.c

### 07_Protocol_Modbus
- protocol/modbus/modbus_rtu_master.c

### 08_Service
- service/rs485/rs485_bus_manager.c
- service/device/device_manager.c
- service/point/point_db.c

### 09_RTOS_Objects
- rtos/rtos_objects.c

### 10_Tasks
- task/task_rs485.c
- task/task_data.c

### 11_Interrupt
- interrupt/gateway_irq.c

### 12_Application
- main.c
- app/gateway_app.c
- app/rs485_smoke_test.c
- app/freertos_hooks.c

Headers do not need to be added to a Keil group to compile, but adding them for navigation is fine.

## 4. Include paths
Use paths relative to your own project root. Required categories:

- gateway/config
- gateway/bsp
- gateway/common
- gateway/driver
- gateway/protocol/modbus
- gateway/service/rs485
- gateway/service/device
- gateway/service/point
- gateway/rtos
- gateway/task
- gateway/app
- GD32H7xx_Firmware_Library/CMSIS
- GD32H7xx_Firmware_Library/CMSIS/GD/GD32H7xx/Include
- GD32H7xx_Firmware_Library/GD32H7xx_standard_peripheral/Include
- FreeRTOS-10.3.1/include
- the include directory corresponding to the selected FreeRTOS portable port

Important: make sure `gateway/config` is in the include paths so the project's `FreeRTOSConfig.h` and `gd32h7xx_libopt.h` are found.

## 5. Current hardware map
- Debug USART2: TX PC10, RX PC11, 115200-8-N-1
- RS485 UART4: TX PB6, RX PB12, DE+/RE PB4, SIT3088EESA
- CAN-FD reserved for next milestone: CAN2 TX PD13, RX PD12
- Ethernet reserved for later milestone: one RMII port + LAN8720A

## 6. Do not add yet
For M0/M1 do not add:
- gd32h7xx_can.c
- gd32h7xx_enet.c
- LwIP
- FatFs
- TLS
- Keil Network middleware
- Keil File System middleware

They are introduced only after RS485/Modbus bring-up is stable.

## 7. First build checklist
1. Build after RTE Startup + SPL + FreeRTOS only.
2. Resolve all include/link errors.
3. Add gateway source groups.
4. Build with zero errors.
5. Confirm USART2 startup log on PC10/PC11.
6. Confirm RS485 PB4 defaults low (receive mode).
7. Run the smoke test against a Modbus slave ID 1, FC03, address 0, quantity 2.

## 8. Common duplicate-symbol mistakes
If you get duplicate definitions of Reset_Handler/SystemInit/SysTick_Handler/PendSV_Handler/SVC_Handler:
- ensure only one startup file exists;
- ensure only one `system_gd32h7xx.c` exists;
- do not add a vendor `gd32h7xx_it.c` that defines RTOS handlers already mapped by FreeRTOS;
- keep `interrupt/gateway_irq.c` for UART4/DMA IRQ handlers.


---

## Archived file: `user/docs/PORTING_CHECKLIST.md`

# V0.2 Keil bring-up checklist

- [ ] Select GD32H759IMK6 device/startup.
- [ ] Use ARM Compiler 6 or your frozen compiler.
- [ ] Add `config/FreeRTOSConfig.h` before any other FreeRTOSConfig in include order.
- [ ] Confirm `SystemCoreClock` is correct after startup.
- [ ] Confirm PC10/PC11 are physically routed to the chosen debug adapter.
- [ ] Confirm UART4 PB6/PB12 and PB4 match the BTB baseboard revision.
- [ ] Confirm DMA0 CH0/CH1 are not used by another module in this first build.
- [ ] Confirm `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY == 2` or adjust `GW_RTOS_IRQ_PREEMPT_PRIORITY` accordingly.
- [ ] Remove duplicate UART4/DMA0 CH0/CH1 handlers from example `gd32h7xx_it.c`.
- [ ] Build with warnings enabled.
- [ ] First run with no RS485 slave: expect timeout logs, no crash.
- [ ] Connect Modbus slave and confirm TX/RX on logic analyzer.
- [ ] Verify PB4 goes high for TX and returns low only after UART TC.
- [ ] Verify CRC-valid FC03 response reaches `task_data`.


---

## Archived file: `V0.4.1_CANFD_500K_5M_TDC.md`

# v0.4.1 CAN-FD 500K/5M + BRS + TDC

This update adapts M3 to the PSCAN/USBCAN bench that supports 5 Mbit/s CAN-FD data phase.

- CAN2 kernel clock: APB2/2 = 150 MHz (current SYSCLK=600 MHz profile)
- Nominal: 500 kbit/s, 88.0% sample point
- Data: 5 Mbit/s, 76.67% sample point
- ISO CAN-FD: enabled
- BRS: enabled
- TDC: enabled, TDCO=23
- Validation log exposes `tdc` and `tdcOOR`; `tdcOOR` must remain 0 for TX gate PASS.

PC PSCAN target: 500000 / 87.5%, 5000000 / 75%, normal (not monitor/listen-only) mode.


---

## Archived file: `V0.4.2_M3_RX_FIRST.md`

# v0.4.2 M3 CAN-FD RX-first bring-up

Purpose: isolate PC/USBCAN -> GD32H759 receive before enabling the gateway's
periodic FD+BRS transmit test.

## Why this build exists

v0.4.1 starts the 0x302 FD+BRS transmitter immediately. If the PC channel is
not yet open, the bus is not ACKing, or the 5 Mbit/s data phase is mismatched,
TX errors can dominate bring-up and hide the RX path. v0.4.2 waits until at
least one CAN frame has been received before it starts 0x302 transmission.

The receive path also prints every received frame in task context as CANRX.

## CAN configuration

- CAN2 PD12 RX / PD13 TX, AF5
- CAN kernel: APB2/2 = 150 MHz
- nominal: 500 kbit/s
- data: 5 Mbit/s
- ISO CAN-FD enabled
- BRS capability enabled
- TDC enabled
- CAN message memory: 32 units during bring-up
- mailbox payload size: 16 bytes during this 12-byte validation
- error IRQs disabled during bring-up; errors are polled every 20 ms

## Phase A1: receive without BRS first

Configure PSCAN:

- nominal 500 kbit/s
- data 5 Mbit/s (still configured, but not used when BRS is off)
- normal/active mode, not listen-only

Send repeatedly every 200-500 ms:

- ID: 0x301 standard
- FD: ON
- BRS: OFF
- payload length: 12 bytes
- data: `00 FA 00 00 00 00 00 00 00 00 00 00`

Expected UART line immediately for every accepted frame:

`[I][CANRX] id=0x00000301 STD FD=1 BRS=0 len=12 data=00 FA ...`

The decoder should set Device 2 ONLINE and Point 2001 GOOD = 25.0.

## Phase A2: enable BRS

Once A1 works, enable BRS on the PC frame and continue sending the same frame.
Expected CANRX now shows `BRS=1`.

After the first received frame, the gateway begins its own 0x302 FD+BRS frame
once per second. PC should receive it when the 5 Mbit/s transmit path is good.

## Diagnostic counters

Every second M3VAL prints:

- CAN RX/TX queue and completion counters
- error state: 0 active, 1 passive, 2 bus-off
- TEC/REC
- FD TEC/REC
- ACK/stuff/form/CRC/bit error counts
- FD-phase error counts
- TDC value and out-of-range count

Interpretation examples:

- `rx=0, REC=0` after PC transmission: usually physical link, adapter send
  settings, ID/frame type, or nominal timing issue.
- `REC`/`fdREC` and FD error counters rise only with BRS ON: data-phase timing
  mismatch.
- `ack`/`TEC` rise after gateway starts 0x302: PC is not ACKing or gateway TX
  is not being decoded.
- `boff>0`: controller entered bus-off; auto recovery remains enabled.


---

## Archived file: `V0.4.4_M3_BRS_SAFE.md`

# v0.4.4 M3 BRS-safe bring-up

This revision addresses the Stage-2 failure where TEC/fdTEC rose quickly and the
board could appear to freeze after several bidirectional CAN-FD+BRS frames.

## What the v0.4.3 log proved

- Stage 0 Classic CAN 500 kbit/s: PASS.
- Stage 1 CAN-FD, BRS off, 12-byte frame: PASS.
- Stage 2 500k/5M+BRS: FAIL before Point DB. RX stayed at 0 while TEC/fdTEC grew.
- The old `TX PASS` was a false positive because it treated an inactive TX
  mailbox as proof of a successful/ACKed frame.

## Changes

1. Split BRS bring-up:
   - Stage 2A (default): 500k nominal / 2M data, FD+BRS, RX-first.
   - Stage 2B: 500k nominal / 5M data, FD+BRS, RX-first.
2. Gateway sends no BRS heartbeat until at least 3 valid BRS RX frames have
   arrived from the PC. This prevents the gateway from driving TEC to
   error-passive/bus-off while the PC timing is still being adjusted.
3. TX safe-hold stops new CAN TX when TEC/fdTEC rises beyond conservative
   thresholds or the controller leaves Error Active.
4. `txGood` and `txFail` distinguish mailbox terminal status from real TX
   health. Validation uses `txGood`, not the old mailbox-done count.
5. Stage 2A uses exact 2 Mbit/s at 80% sample point.
6. Stage 2B uses exact 5 Mbit/s at 76.67% sample point, closer to PSCAN's 75%
   selectable point.
7. Defensive CAN2 TEC/REC/WKUP handlers and HardFault/MemManage/BusFault/
   UsageFault diagnostics were added so an unexpected vector/fault no longer
   looks like a silent whole-gateway hang.

## Stage 2A PC settings

- Nominal/arbitration: 500 kbit/s
- Data: 2 Mbit/s
- CAN-FD: on
- BRS: on
- Standard ID: 0x301
- Length: 12 bytes
- Data: `00 FA 00 00 00 00 00 00 00 00 00 00`

The gateway should first print 3 `CANRX` frames and then:

`BRS RX gate PASS (3 frames); gateway TX released`

Only after that will it start the 0x302 BRS heartbeat.

If Stage 2A passes, set `GW_CANFD_BRINGUP_STAGE` to `3U` and use PSCAN 5 Mbit/s
data phase for the final Stage 2B gate.


---

## Archived file: `V0.4.5_M3_STAGE2A_TDC_AB.md`

# v0.4.5 M3 Stage2A TDC follow-up

Purpose: isolate the v0.4.4 TX-only failure. 500k/2M FD+BRS RX was clean, while gateway BRS TX increased TEC/fdTEC.

The TDC-OFF A/B run has now produced decisive hardware evidence: gateway BRS RX is clean, but every gateway BRS TX increases both TEC and the FD data-phase TX error counter. Stage2A therefore enables TDC again at 2 Mbit/s. Hotfix3 sets TDCO to the configured data sample point (20 at 2M) after hotfix2 showed intermittent TX errors with TDCO=2.

Expected diagnostic result:
- If 2M TX becomes clean (`txGood` grows, `txFail=0`, TEC/fdTEC stay 0), the failure is in TDC configuration/secondary sampling, not CAN mailbox/queue/decoder.
- If 2M TX still fails with TDC off, investigate gateway TX data timing / physical TX waveform / adapter receive timing next.

Safety changes:
- TX hold thresholds reduced to TEC>=32 or fdTEC>=8.
- Raw CANRX logging: first 6 frames, then every 20th frame, to avoid saturating the 115200 debug UART.


## hotfix3 result expected

- Boot log must show `TDC=ON TDCO=20` for Stage2A.
- With PC/gateway both at 500k/2M FD+BRS, `txGood` should increase and `txFail`, TEC and fdTEC should remain 0.
- `tdcOOR` must remain 0. `tdc` now reports the measured TDC value for tuning.
- If TX still fails, safe-hold remains active and the gateway must continue RX/Modbus/M3VAL logging; use the reported TDCV to tune TDCO and inspect the physical TX waveform.


---

## Archived file: `vendor/lvgl-9.2.2/lvgl/src/drivers/README.md`

High level drivers for display controllers, frame buffers, etc


---

## Archived file: `vendor/lvgl-9.2.2/lvgl/src/others/fragment/README.md`


# v0.9.2 Scheduler/Runtime-Stats Hotfix

## Root cause of boot stopping after `Starting FreeRTOS...`

`configGENERATE_RUN_TIME_STATS` was enabled in v0.9.x and `portGET_RUN_TIME_COUNTER_VALUE()` called `gw_time_ms()`. FreeRTOS invokes this hook from `vTaskSwitchContext()` inside PendSV. `gw_time_ms()` uses `taskENTER_CRITICAL()`, which is a task-context API; the Cortex-M7 port asserts when it is called from an exception. The first task therefore ran until its first block/yield, then PendSV entered the assert loop and no CAN/RS485/LCD task ever ran.

The run-time counter is now `gw_time_runtime_counter32()`, a single atomic read of the monotonic 1-ms tick low word. It contains no FreeRTOS API and is safe from PendSV. CPU-load diagnostics retain millisecond resolution. A watchdog startup breadcrumb is also printed before the first watchdog delay.

Expected startup sequence now includes:

```text
Starting FreeRTOS...
[I][WD] supervisor started ...
[I][RS485] UART4 ready, 9600 baud
[I][CAN] CAN2 stable: CAN-FD 500k, BRS=OFF, TDC=OFF, CK_CAN=APB2
[I][POLL] scheduler started, jobs=...
[I][LCD] initializing EXMC SDRAM
...
```
