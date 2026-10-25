# Bootloader robustness: bench report 2026-10-24/25

Plan: `docs/superpowers/plans/2026-10-24-bootloader-robustness-tests.md`. Tests: `tools/basetest/test_boot_base.py`,
`tools/basetest/test_boot_remote.py` (helpers `bootfi.py`, `fwclient.py`, `glitch/glitch.c`). Code under test:
`lib/bootloader` f266dad (both boards), base bootloader `bootloader/src/main.cpp`, remote bootloader
`../midicontroller-fw/bootloader/src/main.cpp`, remote staging `../midicontroller-fw/src/fwupdate.cpp`.
Builds: base `bl-robust` (= main 29ca88d code), remote main 1c042ae.

## Verdict

- **Remote:** the OTA design holds. An interrupted staging (host gone, no END, RN4870 held in reset, base reset,
  remote reset) never touches the running image, and a bad image is never staged. A power loss at any step of the
  bootloader's copy (erase of page n, middle of the copy, any block of the vector page, before or inside clearing the
  pending flag, on 3 boots in a row) always converges to the staged image on the next boot, and a transient SPI read
  error or a failed flash program never loses the image. The same ECC/NMI hang exists in the remote bootloader (F4b),
  but the update flow cannot reach it: with `pending = 1` the bootloader erases the region before it reads it.
  **Separate app-level problem found on the way (F6):** after a remote reboot, which every OTA does, the BLE link can
  come up secured while the remote never receives or sends anything, and neither side recovers until the remote is reset.
- **Base:** every interruption of the USB transfer itself (host gone, reset at any point of the erase or the data
  phase, bad or wrong images, 30 s idle with an IWDG running) ends in the bootloader with USB up, and the PC
  recovers it. **One window bricks it: a power loss while the vector-table page is committed at the end of the
  update (F4).** The base then hangs in the bootloader's NMI handler on every power-up, without USB; only SWD or the
  ROM DFU (BOOT0 jumper JP1) recovers it.

## How power loss was emulated

Real power cycles cannot be automated on this bench: the base stays at 3.44 V with its USB hub port switched off
(it is powered from elsewhere) and the remote runs from its own supply. Three stand-ins, from coarse to exact:

1. Probe reset (SYSRESETREQ) at a point of the transfer (percent of the image).
2. Hardware breakpoint / DWT watchpoint at a named bootloader step, then reset. FPB/DWT survive a system reset, so a
   bp armed while the remote app runs fires in the bootloader after the app's own OTA reboot.
3. **Glitch routine** (`glitch/glitch.c`) loaded into SRAM through the probe: it starts a real page erase or doubleword
   program and fires SYSRESETREQ N loop iterations later, swept over 18 points from 0 to 110 % of the measured
   operation time (the delay loop has the same body as the measuring loop, so the fractions are calibrated). This
   interrupts the flash operation itself, which is what a power cut does.

A system reset aborts an ongoing flash program the way a power cut does: the interrupted doubleword ends up with an
ECC error (measured: ECCR ECCD/ECCC set at the doubleword address).

## Findings, worst first

| # | Severity | Finding | Evidence |
|---|---|---|---|
| F4 | **Critical** | Base: power loss during the page0 (vector table) commit leaves a half-programmed doubleword -> ECC double error -> **NMI in the bootloader** (`Crc32::update` reading the app) -> `Default_Handler` (infinite loop), on every boot. No USB, no IWDG (not running after POR). Recovery only SWD / BOOT0. Window: ~256 doubleword programs (~20-25 ms) at the end of every base update. | Glitch sweep: 6 of 18 delays (every cut at 25-50 % of one doubleword program) -> ECCR 0x80008000, IPSR 2. Watchpoint + reset in `finish()`: 4/4 runs ECCR 0x80008020 -> NMI, persists over 3 further resets. `test_base_glitch_program_vector`, `test_base_bp_commit_page0[0]` (strict xfail). |
| F5 | High | Both boards boot an image **without trailer** on the vector check alone (`BOOT_REQUIRE_TRAILER` not set). If the last page of the app (trailer) is lost but page0 is intact, the bootloader starts a truncated app. | Glitch erase of the base trailer page: 17/17 interrupted erases (5-110 %) -> app started, BusFault in `fault::finish()` (IPSR 5), USB gone (without probe: reset loop). `test_base_glitch_erase_trailer_page` (strict xfail). Not reachable through the update flow (page0 is erased first); needs a stray erase or a bad SWD flash. Base can be forced into the bootloader (encoder held at power-on); the remote has no forced entry. |
| F1 | Medium | No write protection on the bootloader pages (WRP1A/WRP1B empty on both), remote BOR disabled (OPTR 0xfffffeaa, BOREN = 0). A stray flash erase in the app kills the bootloader; on a dying remote battery flash operations run below the guaranteed voltage. | Option bytes read 2026-10-24. Not tested destructively. |
| F2 | Medium (PC) | `midicontrol --cmd "update base ..."` fails when the bootloader's `/dev/ttyACMn` is not openable yet: `updateBase` opens it once (`reconnect(blPort)`), ~185 ms after sysfs and ~10 ms before the node has its group. Container: 3/3 failures; on a host the same race exists between devtmpfs and udev. Recovery path (updater started with the base already in the bootloader) works. | `test_ota.py::test_base_ota_over_cdc` 3/3 "cannot open bootloader port"; tty trace. |
| F3 | Low (build) | `inc/git.hpp` declares `constexpr std::string branch`: a branch name longer than 15 characters (no SSO) does not compile. | Branch `bootloader-robustness` failed, renamed to `bl-robust`. |
| F6 | High (app, BLE) | After a remote reboot (OTA apply or reset) the base can show a secured, stream-open link while the remote has received nothing since boot (`link::s_lastRx = 0`, `s_keepaliveSeen = 0`) and sends nothing. The base logs `W router: remote lost`, its disconnect/reconnect does not help, only a remote reset does. Not a bootloader defect, but it hits right after every OTA. | Final run: 2 occurrences logged during the 25 remote tests (each reboots the remote at least once; `$BASETEST_OUT/f6-silent-remote.log`, detected by `test_boot_remote.linked()`, which resets the remote to continue). Development runs: 1 more logged, 3 more seen by hand. One capture: remote RN4870 driver `_connected = 0`, `_mode = DATA`, rx ring empty. Root cause not isolated (suspect: `%CONNECT%` arriving while the remote app re-initialises the RN4870 after boot). |
| F4b | Medium | Remote: interrupted program of the app vector page **with no pending image** -> ECC double error -> NMI in the bootloader -> `Default_Handler` **with PA1 still high**: the remote stays on, hung, until the battery dies; ON/OFF does nothing. Not reachable by the OTA flow (pending -> erase before read), only by a stray write/erase in the app region. | Glitch sweep: 6 of 18 delays (25-50 %) -> ECCR 0x80000800, IPSR 2, PA1 = 1. `test_remote_glitch_program_vector` (strict xfail). |

Bootloader `NMI_Handler`/`HardFault_Handler` = `Default_Handler` (`b .`) on both boards is what turns F4 (and any
other fault in the bootloader) into a permanent hang.

## Results

Final run 2026-10-25 (both suites, commit 24fc8f5, 1 h 38 min): **50 passed, 4 xfailed (F4 x2, F5, F4b), 1 failed**.
The failure was `test_remote_bp_clear_between`: its breakpoint on `clearStagePending` never fired, so no reset was
injected (a harness miss, not a product failure: the remote was healthy and the next tests passed). Rerun alone 3/3
PASS. Most likely cause: the probe loses FPB during the bootloader's erase stall and re-arms afterwards, and one run
lost that race.

### Base (`test_boot_base.py`)

| Test | What | Result |
|---|---|---|
| `test_base_abort_at[0,1,50,99,100]` | Host stops after N % (100 = no END), then reset | PASS: bootloader `abcd:1235`, PC updater / fwclient recovers |
| `test_base_reset_at[0,50,99]` | Reset while chunks are in flight | PASS |
| `test_base_bad_images[wrong_board, corrupt_body, corrupt_trailer, truncated]` | END NAK + status 5/3/3/5 | PASS, stays in bootloader, good image afterwards OK |
| `test_base_oversize_rejected_at_start` | START size > region | PASS (NAK, BAD_SIZE) |
| `test_base_wrong_target_keeps_app` | START target 0x03 in the base bootloader | PASS: NAK before erasing, app boots after reset |
| `test_base_bootloader_kicks_iwdg` | IWDG started in the bootloader by the probe (~1 s timeout), 30 s idle, then a full update | PASS: no re-enumeration (the bootloader kicks it), update OK. Negative control: with the IWDG debug freeze off and the core halted 3 s the base re-enumerates, so the check can fail |
| `test_base_update_x10` | 10 updates in a row | PASS, 37 s each |
| `test_base_bp_erase[1,2,24,47]` | Reset at the erase of page n | PASS: bootloader (n = 1: app or bootloader, the probe reset lands around the start of the erase). Hit count checked through FLASH_CR.PNB |
| `test_base_bp_commit_page0[1,2,128,255]` | Reset during the page0 commit | PASS (bootloader; k = 255 app). k = 1..128 left correctable single-bit ECC errors (ECCR 0x4000xxxx) |
| `test_base_bp_commit_page0[0]` | idem | **XFAIL F4** (NMI hang) |
| `test_base_bp_after_commit` | Reset right after the commit | PASS: new app boots |
| `test_base_glitch_erase_vector_page` | Real erase of page0 cut at 0..110 % | PASS 18/18: never ECC, app (0 %) or bootloader |
| `test_base_glitch_program_vector` | Real program of doubleword 0 cut at 0..110 % | **XFAIL F4** (6/18: 25-50 % -> NMI) |
| `test_base_glitch_erase_trailer_page` | Real erase of the trailer page cut | **XFAIL F5** (17/17 cuts -> truncated app BusFaults) |

### Remote (`test_boot_remote.py`)

| Test | What | Result |
|---|---|---|
| `test_remote_staging_abort[stop_50, no_end, ble_reset_50, base_reset_50, remote_reset_50]` | OTA interrupted (host, no END, RN4870 held in reset 3 s, base reset, remote reset) | PASS: old image kept, bootloader applies nothing on reset, full OTA afterwards OK |
| `test_remote_bad_images[wrong_board, corrupt_body, corrupt_trailer, truncated]` | END NAK + status 5/3/3/5 | PASS, no reboot, nothing staged |
| `test_remote_bp_erase[1,2,27,55]` | Reset at the erase of page n during the bootloader's copy | PASS: next boot re-applies, staged image installed, pending cleared. Hit count checked through the page address in r0 |
| `test_remote_bp_copy_mid`, `test_remote_bp_vector_copy[0,1,7]` | Reset in the middle of the copy / before vector-page block j | PASS (same) |
| `test_remote_bp_before_clear`, `test_remote_bp_clear_between` | Reset before clearing pending / between header erase and write | PASS: copy repeated, or no magic -> no copy; image installed (`clear_between`: bp missed once in the final run, 3/3 on rerun) |
| `test_remote_reset_every_boot_converges` | Reset mid copy on 3 boots in a row | PASS: 4th boot completes |
| `test_remote_spi_read_error_transient` | SPI read of the staged trailer fails once | PASS: old image kept, pending kept, image applied (same or next boot) |
| `test_remote_bad_staged_image` | Staged image reads back corrupted in the bootloader | PASS: BadImage, pending cleared, old app untouched |
| `test_remote_program_error` | `flashprog::program` fails once during the copy | PASS: converges to the staged image |
| `test_remote_ota_x3` | 3 OTAs in a row | PASS, ~80 s each (incl. reboot and BLE reconnect) |
| `test_remote_glitch_erase_vector_page` | Real erase of the remote vector page cut at 0..110 % (no pending) | PASS 18/18: never ECC; bootloader idles with PA1 released |
| `test_remote_glitch_program_vector` | Real program of doubleword 0 cut | **XFAIL F4b** (6/18: 25-50 %) |

## Recommended fixes (not applied; product code was not changed by this work)

1. **F4, both bootloaders:** an `NMI_Handler` that handles a flash ECC double error: if `FLASH->ECCR & ECCD` and the
   address lies in the app region, clear ECCD, erase the app's page0 (base: makes the region non-bootable -> USB
   bootloader comes up and the PC re-flashes; remote: pending image is re-applied, or the region stays refused) and
   `NVIC_SystemReset()`. Any other NMI/HardFault in the bootloader: `NVIC_SystemReset()` instead of `b .` (remote:
   drop PA1 first so a battery unit switches off instead of draining).
   Regression tests are already in place: the F4 xfails turn into XPASS -> remove the marks.
   F4b is fixed by the same handler (remote: release PA1 before the reset).
2. **F5:** build both bootloaders with `BOOT_REQUIRE_TRAILER=1` once every image in the field carries a trailer (the
   SWD `-update.hex` does); document that a plain `.elf` flash needs the trailer. `test_base_glitch_erase_trailer_page`
   then passes.
3. **F1:** set WRP on the bootloader pages (base pages 0-15, remote 0-7) in production; enable BOR on the remote.
   Irreversible-ish option-byte change: decide deliberately (`stm32-option-bytes`).
4. **F2:** `FirmwareUpdater::updateBase`: retry `reconnect(blPort)` for ~3 s.
5. **F6:** remote: if the RN4870 reports a connection (or the base keeps sending) but no keepalive arrives for ~10 s,
   re-initialise the module (`R,1`); base: if the peer is accepted but no remote packet arrives for ~15 s, `K,1` and
   reconnect. Capture the remote BLE UART (D6/D7) during a reboot to confirm the `%CONNECT%`-during-init theory first.
6. **F3:** `cmake/git.cmake`: generate `constexpr std::string_view` (or `const char*`).

## Not covered / needs a person

- Real power cuts (battery pull, USB unplug) on both boards: the bench cannot switch their supply. Suggested manual
  check for F4: unplug the base in the last ~1 s of `update base` repeatedly (window is ~25 ms per update, so expect
  a low hit rate).
- Base forced entry (encoder held at power-on) and ROM DFU via BOOT0 as the F4 recovery path.
- Remote brown-out (dying battery) during the copy.
- Remote: whether a failing SPI read or flash program is retried 3x within the same boot (documented design) could not
  be forced reliably; the bench only proves the image is never lost (applied the same or the next boot).
- Base `test_ota.py::test_base_ota_over_cdc` (PC updater end to end) fails in this container because of F2.

## Bench notes

- Probe gotchas (also in `.trail/learnings`): FPB/DWT survive reset and openocd exit (stale bp -> HardFault once
  debug is released); `wp` only takes effect when set while halted (hla); `resume` is ignored until openocd polled
  the halt; after a remote reset a running openocd may never read the remote again (restart it).
- The base bootloader is built with LTO: only `flashprog::waitBusy`/`program` survive as symbols; the remote bootloader
  keeps all functions.
