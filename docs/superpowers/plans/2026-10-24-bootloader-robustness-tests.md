# Bootloader Robustness Tests (base + remote) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove on the bench, with automated hardware tests, that the base (STM32G431, USB bootloader) and the remote
(STM32G071, W25Q staging + copy bootloader) always end in a working or recoverable state when a firmware update is
interrupted at any point (power loss, reset, link loss, bad image, flash errors), and name the worst-case threats.

**Architecture:** New pytest modules in `tools/basetest` (same `hw` marker, same `dbg.Target` probes). Power loss is
emulated three ways, from coarse to precise: (1) probe reset at a transfer percentage, (2) hardware breakpoint /
data watchpoint inside the bootloader at a named step, then reset (deterministic "power lost exactly here"), (3) a
RAM-resident glitch routine loaded through the probe that starts a real flash erase/program and fires SYSRESETREQ
after N cycles (interrupts the flash operation itself, the closest thing to a power cut the bench can do). A Python
FW_UPDATE client gives byte-exact control over what the host sends.

**Tech Stack:** Python 3 + pytest + pyserial (existing `.venv`), openocd via `dbg.Target`, arm-none-eabi-gcc for the
glitch routine, PC updater `midicontrol` built against the ALSA RtMidi shim (`BASETEST_UPDATER`).

**Spec:** the user request (2026-10-24) plus the threat model below. Code under test: `lib/bootloader` (f266dad),
`bootloader/src/main.cpp` (base), `../midicontroller-fw/bootloader/src/main.cpp` (remote),
`../midicontroller-fw/src/fwupdate.cpp` (remote staging), `../midicontrolpy-sw/src/firmware_update.cpp` (PC).

## Threat model (what can go wrong, worst first)

| # | Threat | Where | Expected (design) | Worst case if design is wrong |
|---|---|---|---|---|
| T1 | Power lost while a flash page erase or doubleword program is in progress -> half-erased/half-programmed doubleword -> ECC double error on the next read -> **NMI**; bootloader NMI = `Default_Handler` (infinite loop), no IWDG after POR | base `appValid`/`usedLength`/`crc32` over app region; remote same + copy | not covered by design | base: hangs before USB comes up -> only BOOT0/SWD recovers; remote: hangs with PA1 latched -> battery drained, ON/OFF dead |
| T2 | Power lost during the bootloader's copy W25Q -> internal flash (remote) | `applyStagedImageIfPending` | vector page erased first, written last; pending kept -> retried next boot | remote never boots |
| T3 | Power lost during base USB update (START erase, chunks, page0 commit, before reset) | base `FlashSink` | region has no vector table until page0 commit; stays in bootloader `abcd:1235`; PC retries | base bricked without SWD |
| T4 | Power lost / link lost during remote staging (BLE drop, base reset, PC killed, remote reset) | remote app `SpiFlashSink` | header pending=1 only after verified image; old app keeps running | old app destroyed or a half image applied |
| T5 | Bad image accepted: wrong board, bad CRC, broken trailer, oversize, truncated | `FwReceiver`, `stagedImageGood`, `appBootable` | rejected before commit; START with wrong target rejected before erase | bricks or boots garbage |
| T6 | SPI read error / internal flash write error during apply | remote | WriteError -> 3 tries, pending kept; app with no vector page -> power hold released | boot loop that wears flash, or hang with power on |
| T7 | IWDG running when the bootloader is entered (app -> ENTER_BOOTLOADER / OTA reboot) | both | bootloader kicks in every long loop | reset loop in the bootloader mid-update |
| T8 | Base stuck in bootloader (PC vanished after ENTER_BOOTLOADER) | base | magic cleared on read; next reset boots the app | needs a person |
| T9 | Bootloader region not write-protected (WRP empty on both, measured 2026-10-24) | both | not covered | an app bug or a stray erase kills the bootloader -> SWD only |
| T10 | Remote BOR disabled (OPTR BOREN = 0, measured) -> flash ops at undervoltage on a dying battery | remote | CRC verify + retry | silent corruption that verifies? (CRC makes this unlikely) |

T9/T10 are configuration findings: reported, not tested destructively.

## Global Constraints

- Never erase or write the bootloader region (base 0x08000000-0x08007FFF, remote 0x08000000-0x08003FFF) or option bytes.
- Before the first test, dump both full flashes to `/workspaces/MIDICONTROL-b81bba4a/backups/*-2026-10-24-pre-bootrobust.bin`.
- Every test ends with the board back on the build-under-test app (`-update.hex` via SWD as the last resort) and the
  remote paired + linked; a test that cannot restore that fails loudly (`BLOCKED`) instead of continuing.
- A remote reset may switch the remote off (CLAUDE.md of the remote): tests that do a probe reset of the remote check
  the target voltage/`core_state` afterwards and skip with "needs_user: switch the remote on" if it is gone.
- Only one openocd per probe (`dbg.Target` owns it); never `st-info --probe`.
- Product code is not changed by this plan. Findings go into the report; fixes are a separate decision.

## Review Focus

1. Reset *during* a page erase of the app vector page (T1): expect the bootloader to reach USB/app; likely NMI hang. -> Task 4 `test_base_glitch_*`, Task 6 `test_remote_glitch_*`.
2. Reset during the page0 commit after k doublewords (base) / vector page copy (remote): partly written vector table that passes `appValid` but fails the CRC must keep the bootloader resident. -> Task 3 `test_base_bp_commit_page0`, Task 5 `test_remote_bp_vector_copy`.
3. A START with the wrong target must not erase the running app (base app survives). -> Task 3 `test_base_wrong_target_keeps_app`.
4. Repeated interruptions (reset during apply on 3+ consecutive boots) must still converge. -> Task 5 `test_remote_reset_every_boot_converges`.
5. Bootloader sitting idle with the app's IWDG running must not reset (60 s). -> Task 3 `test_base_bootloader_idle_with_iwdg`.

---

### Task 0: Bench baseline

**Files:** none (results into the run log `$BASETEST_OUT/bootrobust/`).

- [ ] Step 1: build base (`mcu build` in midicontrol-fw) and remote (`mcu build` in midicontroller-fw); `fw_trailer.py --check` both `-update.bin`.
- [ ] Step 2: dump both flashes to `backups/` (Global Constraints); compare the app region with the `-update.bin` (prefix) to know whether the board runs the build under test; if not, flash `-update.hex`.
- [ ] Step 3: run the existing happy paths: `pytest -m hw -s test_ota.py -k "base_ota_over_cdc or remote_ota_via_base"`. Expected: 2 passed. These are the reference durations.
- [ ] Step 4: try a real base power cycle: `echo 1 | sudo tee /sys/bus/usb/devices/8-1.3:1.0/8-1.3-port2/disable` and read the base target voltage through the probe; `0` re-enables. Record whether VBUS is really switched (if not: real power cycles are `needs_user`).

### Task 1: Harness helpers `bootfi.py`, `fwclient.py`, glitch routine

**Files:**
- Create: `tools/basetest/bootfi.py`, `tools/basetest/fwclient.py`, `tools/basetest/glitch/glitch.c`, `tools/basetest/test_bootfi_offline.py`

**Interfaces (Produces):**
- `fwclient.Client(port: str)` (serial, protocol frames via `proto`); `start(target:int, size:int, crc:int, timeout=15) -> (acked:bool, status:int|None)`; `data(off:int, chunk:bytes, timeout=2) -> (acked, status)`; `end(timeout=10) -> (acked, status)`; `send_image(target, img:bytes, stop_after:int|None=None, mutate=None) -> str` ("ok" | "stopped" | failure text). `fwclient.bootloader_port() -> str|None` (sysfs `abcd:1235` if00), `fwclient.make_image(src:bytes, *, board=None, corrupt_trailer=False, corrupt_body=False, truncate=None) -> bytes` (re-signs with `../../lib/bootloader/tools/fw_trailer.py` logic).
- `bootfi.Board(name: "base"|"remote")` wrapping `dbg.Target` + bootloader ELF symbols: `bl_sym(name) -> int`; `break_then_reset(addr:int, *, hits:int=1, watch_write:bool=False, timeout=30) -> bool` (hw bp or DWT write watchpoint, count hits, then `reset run`; False if never hit); `glitch(op:"erase"|"program", addr:int, delay_cycles:int)` (loads `glitch.bin` to SRAM, runs it from a halted core; the routine resets the MCU itself); `eccr() -> int`; `flash_read(addr, n) -> bytes`; `app_region_state() -> dict(vector_ok, trailer, crc_ok)`; `where() -> "app"|"bootloader"|"hang"|"off"` (pc range + USB ids for base, pc + DHCSR lockup for remote, voltage for off); `swd_restore()` (program `-update.hex`).
- `glitch.c`: freestanding Thumb-1 (`-mcpu=cortex-m0plus`, runs on both): params at a fixed SRAM struct `{flash_base, op, addr, delay, word0, word1}`; unlock, start erase (PER|PNB|STRT) or PG + 2 word stores, busy-loop `delay` iterations, `AIRCR = 0x05FA0004`.

- [ ] Step 1: offline test `test_bootfi_offline.py`: `make_image` round trip (`fw_trailer.py --check` accepts the re-signed image, rejects `corrupt_trailer`, rejects `board=` mismatch), `fwclient` encodes FW_UPDATE_START/DATA like `firmware_update.cpp` (target, LE32 size, LE32 crc; offset LE32 + <=120 B).
- [ ] Step 2: run it, FAIL (no module). Implement. Run, PASS.
- [ ] Step 3: hardware smoke: `Board("base").where() == "app"`, `Board("remote").where() == "app"`; build `glitch.bin`, size < 256 B.
- [ ] Step 4: commit.

### Task 2: Base - interrupted USB update (coarse)

**Files:** Create `tools/basetest/test_boot_base.py` (fixture `base_bl_recover`: if left in the bootloader, PC updater; if hung, `swd_restore()` and record as FAIL).

- [ ] `test_base_abort_at[pct]` for pct in 0 (after START), 1, 50, 99, 100 (all chunks, no END): stop the host (fwclient `stop_after`), then probe reset. Expected: `abcd:1235` again within 5 s, `abcd:1234` absent; then a full PC update passes.
- [ ] `test_base_reset_at[pct]` for 0, 50, 99: probe reset while chunks are in flight. Same expectation.
- [ ] `test_base_bad_images[kind]` kind in wrong_board, corrupt_body, corrupt_trailer, truncated, oversize: expected NAK/`BAD_SIZE` at START (oversize, wrong target) or `CRC_FAIL`/`BAD_SIZE` at END; bootloader stays; good update afterwards passes.
- [ ] `test_base_wrong_target_keeps_app`: from the app, ENTER_BOOTLOADER, START with target 0x03: NAK; probe reset -> app (`abcd:1234`) boots (region not erased).
- [ ] `test_base_bootloader_idle_with_iwdg`: ENTER_BOOTLOADER from the app (IWDG running), wait 60 s, the USB device number of `abcd:1235` must not change (no reset); then probe reset -> app (T8: magic cleared).
- [ ] `test_base_update_x10`: 10 consecutive PC updates, all pass, banner revision each time.
- [ ] Run, record, commit.

### Task 3: Base - power loss at exact bootloader steps (breakpoint / watchpoint)

**Files:** extend `test_boot_base.py`.

- [ ] `test_base_bp_erase[n]`: bp on `boot::flashprog::erasePageAt` hit n in {1 (vector page), 2, 24, 47 (last)}: reset -> bootloader.
- [ ] `test_base_bp_commit_page0[k]`: DWT write watchpoint on `0x08008000 + 8k` for k in {0, 1, 128, 255} during `finish()`: reset -> bootloader (vector words present for k>=1 but CRC must fail), then good update.
- [ ] `test_base_bp_after_commit`: bp on `NVIC_SystemReset` call site / the apply loop: reset -> new app boots (commit complete).
- [ ] Run, commit.

### Task 4: Base - interrupted flash operations (glitch, T1)

**Files:** extend `test_boot_base.py`.

- [ ] `test_base_glitch_erase_vector_page`: app valid; glitch erase of page 0x08008000 with delays sweeping 0..~22 ms in 10 steps; after each: read `FLASH_ECCR`, `where()`; restore with `swd_restore()`. Pass = never "hang". Record ECC flags per delay.
- [ ] `test_base_glitch_program`: erase app vector page fully, glitch a doubleword program at 0x08008000 with delays 0..100 iterations; same checks.
- [ ] `test_base_glitch_last_page`: same for the last used page (trailer page).
- [ ] Run; if a hang is reproduced, capture pc/xPSR (expect NMI = exception 2), write it up as the T1 finding.

### Task 5: Remote - interrupted staging, apply at exact steps

**Files:** Create `tools/basetest/test_boot_remote.py` (fixture: remote back on the build-under-test app, paired, `remote_alive`; else SWD restore + re-pair; BLOCKED if the remote is off).

- [ ] `test_remote_staging_abort[how]` how in kill_pc_50, ble_reset_50 (`hold_ble_reset`), base_reset_50, remote_reset_50, no_end: remote keeps running the old app (same flash CRC), W25Q header `pending != 1` (read via app `boot::readStageHeader`? no: via bootloader-free read: halt + gdb call or DIAG), then a full OTA passes.
- [ ] `test_remote_bad_images[kind]` wrong_board, corrupt_body, corrupt_trailer, truncated: `CRC_FAIL`/`BAD_SIZE`, remote does not reboot (boot_id unchanged).
- [ ] Remote bootloader apply cases (stage a good image with the OTA up to END, then the remote reboots into the bootloader; the bp is armed before by halting at `boot::applyStagedImageIfPending` with `reset halt`... the remote must not be power-cycled, so the bp is set while the app runs and fires after its own reset):
  - `test_remote_bp_erase[n]` n in {1, 2, 30, 56}: reset -> after the next boot the new image is applied, pending cleared, app runs.
  - `test_remote_bp_copy_mid`: bp on `boot::copyRange` + watchpoint 50 % through: reset -> converges.
  - `test_remote_bp_vector_copy[k]`: write watchpoint on `0x08004000 + 8k`, k in {0, 1, 255}: reset -> converges.
  - `test_remote_bp_before_clear`: bp on `boot::clearStagePending`: reset -> copy repeated, converges.
  - `test_remote_bp_clear_between`: bp on `boot::w25q::writeData` inside clearStagePending (after the header erase): reset -> app boots, no re-copy (magic gone).
  - `test_remote_reset_every_boot_converges`: reset during apply on 3 consecutive boots, 4th completes.
- [ ] `test_remote_spi_read_error`: force `w25q::read` false once (bp at its return, `r0 = 0`) during verify: WriteError, old app boots, pending kept, next reset applies.
- [ ] `test_remote_program_error_3x`: force `flashprog::program` false on all 3 attempts: no bootable app -> PA1 released (GPIOA ODR bit 1 = 0), core in the idle kick loop; then reset without the fault -> applies, boots.
- [ ] `test_remote_bad_staged_image`: flip a byte of `s_buf` at the first `Crc32::update` in `stagedImageGood`: BadImage -> pending cleared, OLD app boots untouched.
- [ ] `test_remote_ota_x3`: three consecutive OTAs pass.
- [ ] Run, commit.

### Task 6: Remote - glitch (T1)

- [ ] `test_remote_glitch_erase_vector_page` / `test_remote_glitch_program`: as Task 4 on 0x08004000. Pass = never "hang". After a hang, SWD restore.

### Task 7: Report

**Files:** Create `docs/bootloader-robustness-report.md` (results table per test, measured ECC behaviour, threats ranked with evidence, fix proposals), link it from `REVIEW.md`; update the basetest README (new modules, env, how to run); `trail note` for the non-obvious bench findings.

- [ ] Write, `trail check`, commit.
