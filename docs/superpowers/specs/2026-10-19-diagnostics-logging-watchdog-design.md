# Diagnostics: logging, fault capture and watchdogs — design

Date: 2026-10-19. Status: approved in conversation ("looks right, go for it, we can modify it later").
Repos: `midicontrol-fw` (base, STM32G431), `midicontroller-fw` (remote, STM32G071), `midicontrol-protocol`,
`midicontrol-bootloader` (`lib/bootloader` in both firmwares), `midicontrolpy-sw` (PC player, C++17).

## Purpose

Mainly **field diagnostics**: after something went wrong in the church, the PC web UI shows what happened —
fault history, reset causes and a readable event log that survived resets. Secondary: a live verbose level for
bench debugging (base: switchable at runtime; remote: not in this iteration). The base must give **detailed fault
information** good enough to track a bug down to a function (`addr2line` on the reported pc/lr).

## Decisions

| Topic | Decision | Why |
|---|---|---|
| Base transport | **Text log on a second USB-CDC interface** ("log CDC"), protocol CDC unchanged | CDC 1 must carry only protocol frames; 8 USB endpoints cover CDC+CDC+MIDI exactly; text is what helps on a bug hunt |
| Remote transport | **Binary 12-byte records over the protocol**, pulled by the PC | BLE (~1.5 KB/s, shared with OTA) and the remote's single link rule out text streaming; pulling keeps load bounded |
| Persistence on the boards | Base: log ring + fault record in **no-init RAM** (survive resets, not power loss). Remote: fault record + last events in a **W25Q32 sector** (a remote reset = power off via PA1) | Each survives the board's own failure mode |
| Persistence on the PC | Append-only **JSONL per node** in the state directory | History survives PC restarts; the boards hold data only until fetched |
| Watchdog | **IWDG on both boards** (backstop) + **software loop watchdog in SysTick** (records where the loop is stuck) | Neither IWDG has an early-warning interrupt (G431/G071 SVD: KR, PR, RLR, SR, WINR only); SysTick still runs when the main loop hangs |
| Bootloaders | Kick the IWDG in `midicontrol-bootloader` (shared) and the per-board `bootloader/src/main.cpp` loops | An IWDG cannot be stopped; `ENTER_BOOTLOADER` resets into the bootloader with it running |

**Exception to the plan's constraint** "do not edit `bootloader/`, `lib/bootloader/`": the IWDG kick is the only
bootloader change this spec allows. Shared code goes to the `midicontrol-bootloader` repo and is bumped into both
firmwares; the per-board `bootloader/src/main.cpp` gets the kick in its main loop, and the base's
`bootloader/STM32G431CBTx_BOOT.ld` reserves the same top-of-RAM no-init block as the app (the bootloader runs on
every boot; its `.bss` and stack must not touch the app's no-init RAM).

**No-init RAM (base):** a `NOINIT` memory region `ORIGIN = 0x20007800, LENGTH = 2K` in both the app and the
bootloader linker scripts (their `RAM` shrinks to 30K, `_estack` = 0x20007800). It holds the log ring header +
ring and the fault record. `sizeBudget.ram` (data+bss) becomes 29184 B so 1.5 KB stays for stack + heap.

## 1. Base logging

- **Module:** `Debug` (src/debug.cpp, inc/debug.hpp) reworked; same `Debug::log(Level, ...)` call shape plus a
  module tag and printf-style formatting: `Debug::log(Level::WARNING, "ble", "cmd failed: %s", cmd)`.
- **Line format:** `[ssssss.mmm] L tag: text\r\n` — uptime seconds.milliseconds, level letter `E W I D`, module tag
  (≤ 6 chars), text. Lines longer than 120 chars are cut and end with `~`.
- **Ring:** ~1.5 KB character ring (replaces the 512 B one) in a `.noinit` section with a header
  `{magic, head, tail, crc-of-header}`. On boot: header valid → keep contents (pre-reset log) and print them first
  under `--- log before reset ---`; invalid → clear.
- **Output:** a second TinyUSB CDC interface, string "MIDIControl log". Lines are written from the main loop when the
  host has the port open (DTR); while closed, the ring keeps them (oldest dropped, a `[dropped N]` marker inserted).
  Never written from ISRs — ISRs only set flags/counters that the main loop turns into lines.
- **Input:** line commands on the log CDC: `level e|w|i|d` (runtime level, default `i`), `dump` (re-print the saved
  fault record), `help`. Anything else answers `?`.
- **Boot banner (level I, always):** firmware name, git revision/date (`inc/git.hpp`), reset cause decoded from
  `RCC->CSR` (pin, power-on/BOR, software, IWDG, WWDG, low-power, option-byte load), then the saved fault record if
  present, then clear `RCC->CSR` (RMVF).
- **What to log (initial set):** BLE module state changes, connect/disconnect/bond/secure, command failures, pairing
  steps; router: PC/remote alive changes, retries exhausted, NAKs sent with reason, dropped frames (CRC, overflow);
  USB mount/unmount, DTR changes; display connect; MIDI parser overruns; settings load/save; OTA/ENTER_BOOTLOADER;
  watchdog/fault records. Detail (packet-level) only at D.
- **RAM:** measure after adding the second CDC (TinyUSB FIFOs are global: 256 B RX + 256 B TX + EP buffers per
  CDC). If data+bss exceeds `sizeBudget.ram` (30720 B), reduce the log CDC's share first (e.g. smaller global FIFO
  if the protocol CDC tolerates it, measured) and only then raise `sizeBudget.ram`, keeping ≥ 1.5 KB for stack+heap
  below 32 KB. Record the final numbers.

## 2. Base fault capture

- HardFault, MemManage, BusFault, UsageFault (and `stmcpp::error::globalFaultHandler`, `signalError()`) call one
  `fault::record(kind, frame)`:
  - stacked r0–r3, r12, lr, pc, xpsr (from the active stack per EXC_RETURN), EXC_RETURN, MSP, PSP, IPSR,
    CFSR, HFSR, MMFAR, BFAR, AFSR, uptime ms, git revision, a kind code (hardfault/memmanage/busfault/usagefault/
    error/loop-stall);
  - stored in `.noinit` with magic + CRC; also logged as a line into the (no-init) log ring.
- Then: debugger attached (`CoreDebug->DHCSR & C_DEBUGEN`) → `bkpt` (bench behaviour unchanged); otherwise
  `NVIC_SystemReset()`.
- `signalError()` records `error` with a code, logs it, blinks the red LED for ~3 s and resets (no infinite loop).
- **Amendment 2026-10-20 (R21):** a failed RN4870 init/configure is *not* such an error: it logs `E ble:`, records
  fault `ERROR` once (no reset), the base keeps running without BLE (USB-MIDI, SSR, display, OLED stay usable) and
  retries BLE init every 60 s (kicking the watchdogs during the attempt). `signalError()` + reset is reserved for
  errors that leave the base unusable. A reset loop because of BLE would take the organ's MIDI with it.
- Next boot prints the record (banner) and keeps it until `dump`/power-off; a second fault overwrites it (the
  record carries a counter of faults since power-on).
- PC decodes CFSR/HFSR bits to names (IACCVIOL, PRECISERR, UNDEFINSTR, INVSTATE, NOCP, DIVBYZERO, UNALIGNED,
  FORCED, VECTTBL, …).

## 3. Watchdogs

- **Loop watchdog (both boards):** the main loop calls `wdg::kick()` once per pass. SysTick (1 ms) counts ms since
  the last kick; at **1000 ms** (base) / **2000 ms** (remote) it captures the interrupted context from its own
  exception frame (the main loop's pc/lr) and calls `fault::record(loop-stall, ...)` → reset. Long legitimate
  operations call `wdg::kick()` inside their loops (BLE init/`$$$` busy-waits, `Oled::flush`, DIN send, the remote's
  W25Q32 erase and OTA staging).
- **IWDG (both boards):** LSI 32 kHz; base **~2 s**, remote **~4 s** (prescaler/reload chosen to match). Started in
  `main()` after clocks, before the blocking init; kicked by `wdg::kick()`. Catches what SysTick cannot see
  (interrupts disabled, stuck in a higher-priority ISR, SysTick itself broken). Reset cause → IWDG in the banner /
  remote record.
- **Bootloaders:** kick the IWDG in every loop that can run longer than ~500 ms (main loop, erase, program, the
  remote's W25Q32 → internal copy). If the app was never started (blank), the bootloader does not start the IWDG.
- **Remote power:** a watchdog/fault reset releases PA1 and turns the remote **off**. Intended: a hung remote must
  not drain the battery. The record is written to the W25Q32 **before** the reset (fault path writes synchronously
  with interrupts off; at most one sector program, no erase in the fault path — the sector is pre-erased at boot).

## 4. Remote records and protocol

- **Record (12 B, little-endian):** `seq u16, code u8, level u8, uptime_ms u32, arg u32`.
- **RAM ring:** 32 records. `seq` increments per record since boot; the boot record carries reset cause.
- **Codes (initial):** `BOOT(reset cause)`, `FAULT(kind, pc)` (+ a fault detail record set: pc, lr, xpsr, ipsr),
  `LOOP_STALL(pc)`, `IWDG_RESET`, `BLE_CONNECT`, `BLE_DISCONNECT`, `BLE_SECURED`, `LINK_LOST`, `RETRIES_EXHAUSTED
  (msgType)`, `CRC_ERRORS(count)`, `CHARGER_FAULT(REG09)`, `LOW_BATTERY(mV)`, `OTA_RESULT(status)`,
  `SHUTDOWN(reason)`. Levels E W I D like the base; only E/W/I recorded.
- **Persistent:** one W25Q32 4 KB sector (outside the OTA staging area): on fault/stall/shutdown-by-error the fault
  detail + the last 16 ring records are programmed into the next free slot (sector holds ~10 slots). A slot counts as
  fetched once a DIAG_GET cursor in the same `boot_id` has passed the last `seq` its records were given after
  loading; the remote then programs a "fetched" byte in the slot (program-only, no erase). The sector is erased at
  boot only when it is full and every slot is fetched; full and unfetched → the oldest slot is lost (counted in the
  BOOT record). Boot loads unfetched slots into the ring first.
- **Messages (protocol v2 additions, `midicontrol-protocol`):**
  - `DIAG_GET` `0x80`, PC → Remote (and later Base if needed), payload `[cursor u16]`, ACKed.
  - `DIAG_RESP` `0x81`, Remote → PC, payload `[boot_id u16, next_cursor u16, n u8, n × 12 B record]`, n ≤ 8; sent
    in addition to the ACK (like STATUS_RESP). `boot_id` changes every boot so the PC can tell a reset from a
    cursor wrap.
  - Old firmware answers NAK UNKNOWN_MSG (0x01); the PC then stops polling that node until reconnect.
- **Polling:** the PC sends DIAG_GET on remote link-up and every 10 s while alive; never during OTA; stops when the
  response says nothing new. Worst case one DIAG_RESP (≤ 101 B payload, ~110–120 B on the wire typically, ≤ ~220 B if every byte
  escaped) plus its ACK per 10 s per direction. DIAG_RESP is ACKed by the requester like STATUS_RESP (R22).
- The base forwards these like any other frame (Dest = PC/Remote) — no base router change.
- PROTOCOL.md documents both messages, the record layout and the code table; the PC's vendored protocol copy and
  `names.cpp` get the same.

## 5. PC software (midicontrolpy-sw)

- **Log reader:** a thread opens the base's log CDC: found as the ttyACM whose USB interface belongs to `ABCD:1234`
  and is the log interface (sysfs `bInterfaceNumber` / `interface` string "MIDIControl log"), config override
  `[DIAG] LogPort=`. Reconnects like the protocol port. Each line → parsed (uptime, level, tag, text) → store +
  `StatusRegistry::event(Base, level, text)` for errors/warnings.
- **Remote records:** `DIAG_RESP` handled in `comm.cpp` → decoded with a code table (`src/diag_codes.cpp`) → store.
- **Store:** `DiagStore` appends JSONL `{"t":<pc unix ms>,"node":"base|remote","boot":..,"up":..,"lvl":"E","tag":..,
  "text":..,"fault":{...}}` to `$XDG_STATE_HOME/midicontrol/diag-<node>.jsonl` (config `[DIAG] Dir=`); rotates at
  1 MB keeping 3 files; loads the tail on start.
- **HTTP:** `GET /api/diag?node=base|remote&level=E|W|I|D&before=<id>&limit=<n>` (JSON, newest first, paged);
  `/api/status` gains per-node `last_fault` and `unseen_faults`; the health dot turns amber while `unseen_faults > 0`
  (cleared by a `POST`-free mechanism: the Diagnostics tab sends `GET /api/diag?ack=<id>`).
- **Web page:** new **Diagnostics** tab: per node the last fault (kind, decoded CFSR/HFSR bits, pc/lr hex, firmware
  revision, uptime), reset-cause history, and a filterable log table (node, level, tag, text search), polling every
  1 s while the tab is open.
- **Console/socket:** `diag` command prints the last fault per node and the last 20 lines.

## 6. Testing

- **Base hardware tests (`tools/basetest`):** log CDC enumerates and is separate from the protocol CDC; banner shows
  git revision and the right reset cause (probe reset → pin; `NVIC_SystemReset` via gdb → software); `level d`
  switches; injected fault through gdb (`set $pc = 0xFFFFFFF0`/call to a bad address with the core running →
  HardFault) → after reset the banner shows the fault record with that pc and the pre-crash log lines; loop stall
  (gdb sets a flag/patches a spin, or `hbreak` replaced by an infinite loop) → loop-stall record with the stuck pc;
  IWDG reset with interrupts disabled → reset cause IWDG; protocol CDC tests from Tasks 3–5 still pass (stray
  empty); ENTER_BOOTLOADER + full base OTA succeeds with the IWDG running (Task 7 re-run).
- **Remote hardware tests:** DIAG_GET over the base returns the boot record; injected fault → record survives the
  power-off and is returned after the user powers it on (ask the user to press ON/OFF); OTA via base still succeeds
  with the IWDG on; BLE traffic stays within budget (count DIAG frames over 60 s).
- **PC unit tests:** log-line parser, fault decoder (CFSR/HFSR names), DIAG_RESP decode, `DiagStore` append/rotate/
  load, `/api/diag` paging/filtering, `/api/status` fault fields, web page contains the Diagnostics tab.
- **Host unit tests (firmware):** record encoding and the no-init header validation in `src/logic/` where they are
  hardware-independent.

## Out of scope (now)

Live verbose streaming from the remote; symbolization on the PC (it shows hex + revision); flash persistence on the
base; remote log levels at runtime; a base DIAG_GET (the log CDC covers it).
