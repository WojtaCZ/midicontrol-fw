# Firmware-Update Progress Bars Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While new firmware is uploaded, the base and the remote both show a framed progress bar with a title and a hint, drawn exactly like the remote's shutdown screen (`PowerHoldScreen`).

**Architecture:** One drawing function (`ui::drawProgress`, lib/ui) is the single source of the look; app screens wrap it in `ui::ProgressScreen`, the two bootloaders draw it into a 1 KB framebuffer flushed by a small shared blocking SH1106 driver (`boot::oled`, midicontrol-bootloader). A header-only tracker (`boot::FwProgress`) turns observed `FW_UPDATE_*` packets into state + percent; the base app feeds it the packets it relays to the remote, the remote app the packets it receives and the statuses it sends.

**Tech Stack:** C++23 bare metal (STM32G431 base, STM32G071 remote), lib/gfx (`Font_7x10`), lib/ui, midicontrol-bootloader host tests (plain CHECK), remote GoogleTest host tests (`/opt/embedded/cmake/host-test.cmake`).

**Spec:** No separate spec — the user skipped the design questions ("skip the questions and implement the progress bars for updating both the base and the remote"). The design below is Claude's call and is the spec.

## Design (the spec)

What each screen shows, all with title `Aktualizace` and the shutdown screen's bar (frame x=4 y=26 w=width-8 h=12, fill inset 2 px, fill width `(w-4)*percent/100`), title at y=8, hint at y=48, both centred:

| Where | When | Hint | Bar |
|---|---|---|---|
| Base bootloader | resident, no transfer yet | `Cekam na PC` | 0 % |
| Base bootloader | FW_UPDATE_START accepted .. reset | `Nevypinejte` | received / size |
| Base bootloader | transfer rejected (Failed action) | `Chyba, opakujte` | stays |
| Base app | relaying an update to the remote | `Prenos do ovladace` | relayed / size |
| Base app | END relayed or remote reported CRC_OK | `Instalace ovladace` | 100 % |
| Base app | remote reported CRC_FAIL / WRITE_ERROR / BAD_SIZE, or 20 s without FW packets | `Chyba aktualizace` | stays |
| Remote app | receiving (staging into W25Q) | `Prijem dat` | received / size |
| Remote app | image verified, reboot armed | `Instalace` | 100 % |
| Remote app | failed / stalled | `Chyba aktualizace` | stays |
| Remote bootloader | copying the staged image | `Instalace` | copy progress |

Done and Failed screens on the apps disappear by themselves 10 s later; keys do nothing while the screen is up (like `PowerHoldScreen`). The display is woken when the screen appears and on each change.

## Global Constraints

- User-visible text: Czech without diacritics, plain ASCII, ≤ 18 chars at 7x10 (all hints above fit).
- Style: tabs (width 4), 160 columns, braces attached, `int *p`; comments in Czech in the firmware repos (midicontrol-fw, midicontroller-fw), English in midicontrol-bootloader (follow the file you are in).
- C++: no exceptions, no heap, no RTTI. Bootloaders: blocking code is fine, every wait loop has a guard counter and `boot::wdg::kick()` where it can last > 100 ms.
- Flash budgets: remote bootloader ≤ 16 KB (now 5.9 KB), base bootloader ≤ 32 KB (now 14.6 KB); report the new sizes.
- SH1106 column address 0 (as both apps use), page addressing, I2C address 0x3C.
- Timeouts in `boot::FwProgress`: stall 20000 ms (> the PC's 15 s START wait), Done and Failed linger 10000 ms; wrap-safe `uint32_t` ms arithmetic.
- Branch `fw-progress` in every repo touched (midicontrol-fw from `bl-robust`, the others from their current HEAD). Do not push. Leave unrelated dirty files (`.gitignore`, `inc/git.hpp`, `.claude/`, `.trail/`) out of commits.
- Bench: back up the current flash to `../backups/` before flashing (ST-Link serials in midicontrol-fw/CLAUDE.md).

## Review Focus

1. PC aborts mid-transfer (no END, no status): apps must not show the bar forever — stall → Failed after 20 s, Idle 10 s later (Task 2 test `StallFailsThenClears`).
2. BLE retransmits a chunk (lower offset): the bar must not jump backwards (Task 2 test `RetransmitDoesNotGoBack`).
3. A new update right after a failed one: START restarts at 0 % from Failed/Done (Task 2 test `StartRestartsAfterFailure`).
4. Base-targeted START/DATA seen by a tracker for the remote (and STATUS from another node) are ignored (Task 2 test `IgnoresOtherTarget`).
5. Systick ms counter wraps during an update (Task 2 test `WrapSafeTimeouts`).

---

### Task 1: `ui::drawProgress` + `ui::ProgressScreen`, shutdown screen uses it

**Files:**
- Create: `midicontroller-fw/lib/ui/inc/ui/progress.hpp`, `midicontroller-fw/lib/ui/src/progress.cpp`, `midicontroller-fw/lib/ui/inc/ui/progress_screen.hpp`
- Modify: `midicontroller-fw/src/menu.cpp:209-251` (`PowerHoldScreen` → `ui::ProgressScreen`), `midicontroller-fw/CMakeLists.txt` (firmware sources + host suite)
- Test: `midicontroller-fw/test/host/test_progress.cpp`

**Interfaces:**
- Produces:
  - `void ui::drawProgress(gfx::IFrameBuffer<uint8_t> &fb, const gfx::text::font &font, int width, const char *title, const char *hint, uint8_t percent);` — clears fb, draws the layout from the Design; percent clamped to 100; `nullptr` title/hint draws nothing for that line. Depends on gfx only (bootloaders link it).
  - `class ui::ProgressScreen : public ui::Screen` — ctor `ProgressScreen(const char *title, const char *hint)`; `void setPercent(uint8_t)`, `void setHint(const char *)` (each marks dirty only on change); `handleInput` swallows everything; `render` calls `drawProgress(fb, *theme.smallFont, theme.displayWidth, ...)`.

- [ ] **Step 1: Write the failing tests** in `test/host/test_progress.cpp` with a 128x64 test framebuffer (vector of bytes, `IFrameBuffer` subclass):
  - `Progress.FrameAtZero`: percent 0 → pixels (4,26), (123,26), (4,37), (123,37) set; (6,28) clear.
  - `Progress.HalfFill`: percent 50 → fill = (120-4)*50/100 = 58 → (6,28) and (63,35) set, (64,28) clear.
  - `Progress.FullAndClamp`: percent 100 → (121,35) set; percent 200 draws the same pixels as 100.
  - `Progress.TitleCentred`: title "Vypnuti" → leftmost set pixel in rows 8..17 has x ≥ (128-7*7)/2 = 39 and rightmost < 39+49; nothing set in rows 0..7.
  - `Progress.NullHint`: hint nullptr → rows 48..63 empty.
- [ ] **Step 2:** Add the test + `lib/ui/src/progress.cpp` + `lib/gfx/src/font_7x10.cpp` to the `HOST_TESTS` suite (INCLUDES `lib/ui/inc lib/gfx/inc`); run `cmake -S . -B build-host -G Ninja -DHOST_TESTS=ON && cmake --build build-host` → fails (no `ui/progress.hpp`).
- [ ] **Step 3:** Implement `drawProgress` and `ProgressScreen` (layout constants as in the old `PowerHoldScreen::render`, centring with `strlen(text) * font.width()`).
- [ ] **Step 4:** `ctest --test-dir build-host` → all pass.
- [ ] **Step 5:** Replace `PowerHoldScreen` in `src/menu.cpp` by `static ui::ProgressScreen powerHoldScreen("Vypnuti", "Drzte 5 s");` (same pixels as before), add `progress.cpp` to the firmware sources, `mcu build` → links.
- [ ] **Step 6:** Commit in `lib/ui` (branch `fw-progress`): "Add drawProgress and ProgressScreen"; commit in midicontroller-fw: test, CMake, menu.cpp, submodule pointer.

### Task 2: `boot::FwProgress` tracker

**Files:**
- Create: `midicontrol-bootloader/inc/boot/fw_progress.hpp` (header-only)
- Test: `midicontrol-bootloader/tests/test_main.cpp`

**Interfaces:**
- Produces (namespace `boot`):
  ```cpp
  class FwProgress {
  public:
  	enum class State : uint8_t { Idle, Transfer, Done, Failed };
  	explicit FwProgress(uint8_t target);
  	void observe(const protocol::Packet &pkt, uint32_t nowMs); // any direction, any packet
  	void tick(uint32_t nowMs);
  	State state() const;
  	uint8_t percent() const;  // 0..100
  	bool takeChange();        // true once after state or percent changed
  };
  ```
  Rules: START (payload `target(1) size(4 LE) ...`) with `pkt.dest == target && payload[0] == target && size > 0` → Transfer, 0 %, from any state. DATA (dest == target, payload `offset(4 LE) chunk`) in Transfer → done = max(done, offset + payloadLen - 4), percent = min(100, done*100/size) in 64-bit. END (dest == target) in Transfer → Done, 100 %. STATUS (`source == target`): CRC_OK → Done 100 %; CRC_FAIL / WRITE_ERROR / BAD_SIZE → Failed; others ignored. Every accepted FW packet refreshes the activity time. `tick`: Transfer idle ≥ 20000 ms → Failed; Done/Failed for ≥ 10000 ms → Idle.

- [ ] **Step 1: Write failing tests** (CHECK style, build packets with `protocol::Packet` fields directly):
  - `StartDataEnd`: START size 1000 → Transfer 0; DATA offset 0 len 120 → 12 %; DATA offset 880 len 120 → 100; END → Done.
  - `RetransmitDoesNotGoBack`: after DATA to 600, DATA offset 480 → percent stays 60.
  - `StatusFailure`: STATUS CRC_FAIL from target → Failed; STATUS CRC_OK from target → Done.
  - `IgnoresOtherTarget`: tracker(ADDR_REMOTE): START dest ADDR_BASE ignored; STATUS CRC_FAIL from ADDR_BASE ignored.
  - `StallFailsThenClears`: START at t=0, tick(19999) Transfer, tick(20000) Failed, tick(29999) Failed, tick(30000) Idle.
  - `StartRestartsAfterFailure`: Failed → START → Transfer 0 %.
  - `WrapSafeTimeouts`: START at 0xFFFFF000, tick(0x00003000) (≈ 16 s later) still Transfer.
  - `TakeChangeOnce`: takeChange true after START, false on second call, false after a DATA that does not change the integer percent.
- [ ] **Step 2:** `cmake -S tests -B build-host && cmake --build build-host` → fails (no header).
- [ ] **Step 3:** Implement `fw_progress.hpp`.
- [ ] **Step 4:** `ctest --test-dir build-host` → pass.
- [ ] **Step 5:** Commit in midicontrol-bootloader (branch `fw-progress`): "Add FwProgress: update progress from observed FW_UPDATE packets".

### Task 3: shared bootloader display `boot::oled`

**Files:**
- Create: `midicontrol-bootloader/inc/boot/oled.hpp`, `midicontrol-bootloader/src/oled.cpp`
- Modify: `midicontrol-bootloader/cmake/bootloader.cmake` (`BOOTLIB_OLED_SOURCES`, `BOOTLIB_OLED_INC`), `midicontrol-bootloader/tests/CMakeLists.txt`, `README.md` (one paragraph)
- Test: `midicontrol-bootloader/tests/test_main.cpp`

**Interfaces:**
- Consumes: `ui::drawProgress` (Task 1).
- Produces (namespace `boot::oled`, I2C1 register-level, identical on G0/G4 — the caller has enabled the I2C1 clock, configured SCL/SDA and its kernel clock):
  - `uint8_t dirtyPages(const uint8_t *frame, const uint8_t *shown);` — bit p set when page p (128 bytes) differs. MCU-independent, host-tested.
  - `void init(uint32_t timingr);` — TIMINGR, PE, SH1106 init sequence (copy of the apps' sequence, display on), clears the panel.
  - `void show(const char *title, const char *hint, uint8_t percent);` — `ui::drawProgress` into the internal 1 KB frame, then writes only the dirty pages (column 0) and updates the shadow copy.
  - `BOOTLIB_OLED_SOURCES` = `src/oled.cpp`, `../ui/src/progress.cpp`, `../gfx/src/font_7x10.cpp`; `BOOTLIB_OLED_INC` = `../ui/inc`, `../gfx/inc` (siblings in a firmware repo's `lib/`).
  - I2C writes: as the remote's old `bootoled::write` (AUTOEND, NBYTES ≤ 255, NACK → give up, guard counters) — a missing panel must never hang the bootloader.
- [ ] **Step 1: Failing test** `DirtyPages`: identical buffers → 0; byte 0 differs → 0x01; byte 1023 → 0x80; bytes 300 and 700 → 0x24.
- [ ] **Step 2:** Add `src/oled.cpp` to the test build behind a host guard (only `dirtyPages` compiled on host, e.g. register code under `#ifndef BOOTLIB_HOST_TEST`), run → fails.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Tests pass.
- [ ] **Step 5:** Commit in midicontrol-bootloader: "Add shared SH1106 progress display for the bootloaders".

### Task 4: remote bootloader uses `boot::oled`

**Files:**
- Delete: `midicontroller-fw/bootloader/src/bootoled.cpp`, `bootoled.hpp`
- Modify: `midicontroller-fw/bootloader/src/main.cpp`, `midicontroller-fw/bootloader/CMakeLists.txt`, `midicontroller-fw/lib/bootloader` (pointer → Task 2/3 commit), `midicontroller-fw/CLAUDE.md` (bootloader line)

**Interfaces:** Consumes `boot::oled::init/show`.

- [ ] **Step 1:** Board init in `main.cpp` (from the old `bootoled::init`: GPIOB clock, I2C1 clock, I2C1SEL = HSI16, PB6/PB7 AF6 open-drain pull-up, ~200000-loop settle), then `boot::oled::init(0x0070215B)`; progress callback `[](uint8_t p) { boot::oled::show("Aktualizace", "Instalace", p); }` (a captureless lambda converts to `void(*)(uint8_t)`).
- [ ] **Step 2:** `cd midicontroller-fw && mcu build` → builds; `arm-none-eabi-size build/bootloader/remote-bootloader.elf` text < 16384. Record the size.
- [ ] **Step 3:** Commit in midicontroller-fw: "Bootloader: framed install bar with text via the shared display".

### Task 5: base bootloader shows the update

**Files:**
- Modify: `midicontrol-fw/bootloader/src/main.cpp`, `midicontrol-fw/bootloader/CMakeLists.txt`, `midicontrol-fw/lib/bootloader` + `lib/ui` (pointers), `midicontrol-fw/CLAUDE.md` (flash-layout/bootloader text)

**Interfaces:** Consumes `boot::oled`, `FwReceiver::active()/received()/expectedSize()`.

- [ ] **Step 1:** `displayInit()`: GPIOA/GPIOB clocks, I2C1 clock (APB1ENR1), I2C1SEL = PCLK1 (reset value, 16 MHz HSI16), PA15 SCL + PB7 SDA AF4 open-drain pull-up, `boot::oled::init(0x10320309)` (400 kHz @ 16 MHz, RM0440 table). Call it once the bootloader decided to stay resident, before USB comes up; show `Cekam na PC`, 0 %.
- [ ] **Step 2:** In `onPacket()`: after `handle()`, Failed → hint `Chyba, opakujte`; otherwise while `s_rx.active()` → hint `Nevypinejte`, percent `received*100/expectedSize` (64-bit); Apply → 100 % before the reset. `show()` is cheap when nothing changed (no dirty page), so calling it per packet is fine.
- [ ] **Step 3:** `cd midicontrol-fw && mcu build` → builds; bootloader text < 32768, record the size.
- [ ] **Step 4:** Commit in midicontrol-fw: "Bootloader: progress bar on the OLED during a USB update".

### Task 6: remote app shows reception

**Files:**
- Modify: `midicontroller-fw/src/fwupdate.cpp`, `inc/fwupdate.hpp`, `src/menu.cpp`, `inc/menu.hpp`, `src/main.cpp` (loop)

**Interfaces:**
- Consumes `boot::FwProgress`, `ui::ProgressScreen`.
- Produces: `void fwupdate::process();` (tick with `stmcpp::systick::getTicks()`, on `takeChange()` call the GUI); `void GUI::showFwProgress(const char *hint, uint8_t percent);` (push once — not over the splash, like `showPowerHold` — else update; wake the OLED) and `void GUI::hideFwProgress();` (pop if current, else remove nothing).
- [ ] **Step 1:** `s_progress(protocol::ADDR_REMOTE)` in fwupdate.cpp: observe every incoming packet in `handlePacket` and every outgoing `status` packet; `process()` maps Transfer → `Prijem dat`, Done → `Instalace`, Failed → `Chyba aktualizace`, Idle → hide. Call `fwupdate::process()` in the main loop next to `link::process()`.
- [ ] **Step 2:** `mcu build` → builds; host tests still pass.
- [ ] **Step 3:** Commit: "OTA: progress screen while the remote receives an image".

### Task 7: base app shows the relay to the remote

**Files:**
- Modify: `midicontrol-fw/src/router.cpp`, `src/menu.cpp`, `inc/menu.hpp`, `midicontrol-fw/CMakeLists.txt` (add `lib/ui/src/progress.cpp`, `${BOOTLIB_INC}` if not on the include path)

**Interfaces:** Consumes `boot::FwProgress`, `ui::ProgressScreen`; produces `GUI::showFwProgress(const char *hint, uint8_t percent)` / `GUI::hideFwProgress()` with the same semantics as Task 6 (base OLED wake: `Oled::wakeupCallback()`).

- [ ] **Step 1:** `static boot::FwProgress _remoteFw(protocol::ADDR_REMOTE);` in router.cpp, `observe()` in the `ADDR_REMOTE` (PC → remote) and `ADDR_PC` (remote → PC) cases; in `router::process()` tick and on `takeChange()` call the GUI with `Prenos do ovladace` / `Instalace ovladace` / `Chyba aktualizace` / hide.
- [ ] **Step 2:** `mcu build` → builds within the 96256 B budget (`mcu size`).
- [ ] **Step 3:** Commit: "Router: progress screen while relaying a remote update".

### Task 8: bench verification

- [ ] **Step 1:** Back up both boards' flash to `../backups/`, flash base full hex and remote full hex (`mcu flash` in each repo, by ST-Link serial).
- [ ] **Step 2:** Base update over USB with the PC updater (`midicontrolpy-sw`); while it runs, read the base bootloader's frame buffer over SWD (openocd `read_memory`, symbol from `base-bootloader.map`) at least twice and render it as ASCII: expect title, frame and a growing fill; the app comes back afterwards.
- [ ] **Step 3:** Remote update through the base; sample the base app frame buffer (`frameBuffer`) and the remote app framebuffer the same way during the transfer; capture the remote I2C (LA CH5/CH6) across the reboot to confirm the bootloader writes pages 3/4 during the copy.
- [ ] **Step 4:** Both boards end on the new app; report sizes, test output and what a person should still look at on the real panels.
