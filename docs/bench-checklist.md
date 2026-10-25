# Bench checklist: tests that need a person

State on 2026-10-24: base `1ff673b` (firmware `251285b`), remote `1c042ae`. Everything the bench can run on its own
passed (600 s soak twice; pairing, BLE security, relay, CDC, BLE-down, remote diagnostics). What is left needs someone
at the bench, a change to the bench, or a setup that would break the base–remote bond. Results go into `../REVIEW.md`
("Hardware test log"). Tick them off here as they pass.

Commands run from `tools/basetest` in this repo unless stated otherwise. The remote-side tests need the remote's DWARF
ELF: `export BASETEST_REMOTE_ELF=<-g build>/midicontroller-fw.elf` (recipe in `tools/basetest/README.md`, "Remote symbols").

## 1. Look and press (no commands)

- [x] **Base OLED:** the main menu and the pairing screens draw correctly, Czech text without diacritics, nothing cut off.
- [x] **Base hint without a stored remote:** the main-menu title shows `Sparuj ovladac` (left of the PC icon). To get
      there: clear the pairing (or start from a v1-settings upgrade); after "Nastaveni > Sparovat ovladac" succeeds,
      the hint goes away. *2026-10-25: shown on the "Odpojeno" overlay without a PC (6b2e9ef).*
- [x] **Encoder:** turning moves the selection one item per detent, push selects.
- [x] **Pairing from the menu:** "Nastaveni > Sparovat ovladac" with the real remote ends paired; the remote works
      afterwards. *2026-10-25: first try after 2a5c1f3 and remote 771682a.*
- [x] **Remote during a base reconnect:** reset or power-cycle the base; the remote never shows "Sparovat?" and its
      link comes back by itself within ~10 s. *2026-10-25: link back in 6.2 s.*
- [ ] **Church display hot-plug:** unplug and replug the display; the PC status shows `displayConnected` follow it
      (PC13 sense). *2026-10-25: no display on the bench.*
- [x] **Remote ON/OFF:** a short press does nothing; a 5 s hold shows the progress screen and powers off (20 ms debounce).
- [x] **SSR:** with the relay fitted, organ power on/off from the menu ("Napajeni Varhan") and from the PC switches the relay. *2026-10-25: LED on the SSR output.*

## 2. Tests that switch the remote off (someone must switch it on again)

```bash
../../.venv/bin/python -m pytest -q -m needs_user -s test_remote_diag.py -k "remote_iwdg or remote_loop_stall or fault_survives"
```

- [x] `test_remote_fault_survives_power_off`: fault slot persisted and delivered after the next power-on. *2026-10-25: warm-reset path; a real power loss was not reached on the bench.*
- [x] `test_remote_loop_stall`: LOOP_STALL + 4 FAULT_DETAIL; the PC shows it as the remote's last fault.
- [x] `test_remote_iwdg`: off for 3–7 s, no new FAULT/LOOP_STALL record. *2026-10-25: reset after 3.9 s.*
- [x] Remote BOOT record end to end (needs a remote reboot): `test_remote_diag.py` skips it while the ring has wrapped.
- [x] Base log CDC physical replug: unplug and replug the base's USB; the log port comes back and the PC reconnects. *2026-10-25: log back 8.3 s, BLE 5.3 s.*

## 3. Needs a bench change first

- [ ] **DIN link** (dead in both directions since 2026-10-19): fix the Otraki to base MIDI IN/OUT link (cables,
      optocoupler supply, ATtiny U14 direction), then `../../.venv/bin/python -m pytest -m hw -s test_midi.py`, the
      USB-MIDI to DIN tests and the Otraki parts of the headless-player check.
- [ ] **1k5 USB pull-up (R33):** check whether it is fitted on the boards in use; the firmware keeps the internal pull-up
      on because the external one had no effect.

## 4. Automated, not run yet (runs alone, but changes the bench state)

- [x] **Base OTA power loss** (puts the base in its bootloader, the fixture recovers it):
      `../../.venv/bin/python -m pytest -q -m hw -s test_ota.py -k power_loss` *2026-10-25: covered by the physical cuts at 11/47/90 %; the test needs the shim-built updater.*
- [x] **Remote `test_outbound`** (`../midicontroller-fw/tools/bletest`): the PC's BLE adapter plays the base and
      re-pairs the remote, so the base loses its bond. Re-pair from the base menu afterwards (or run `test_pairing.py`). *2026-10-25: 14/14 with the base halted.*

## 5. Bootloader recovery (added 2026-10-25)

The automated bootloader suites (`test_boot_base.py`, `test_boot_remote.py`) passed on 2026-10-25, apart from the known
findings. Report: `docs/bootloader-robustness-report.md`. The bench cannot switch either board's supply, so these are
left for a person. Before you start, have the probe ready to recover the base, because F4 can hang it for good.

- [ ] **Base real power cut during an update (F4):** run `update base` and unplug the base USB in its last ~1 s, many
      times. The danger window is ~25 ms per update, so most tries land outside it. Expected: it always comes back in the
      bootloader (`abcd:1235`) or the app. A dark, non-enumerating base is F4: note how many tries it took, then recover
      it with the probe. *2026-10-25: fewer than 5 tries, never dark: inconclusive.*
- [x] **Base real power cut mid-transfer:** unplug at ~10 %, ~50 % and ~90 % of `update base`, then plug it back in.
      Expected: the bootloader enumerates, and a second `update base` finishes and boots the app.
- [x] **Base forced entry:** hold the encoder while plugging in the base. Expected: the bootloader, even with a good app.
- [ ] **Base ROM DFU (BOOT0) as the last resort for F4:** with BOOT0 held high at power-on, `dfu-util -l` lists the
      STM32 ROM DFU. Then flash the bootloader and the app over it and check both boot. *2026-10-25: the ROM selects USART2 (RN4870 chatter), USB never starts; not needed, the ST-Link is the last resort.*
- [x] **Remote battery pull during the image copy:** start a remote OTA and pull the battery right after "apply" (the
      copy takes a few seconds). Put the battery back and power on. Expected: the copy finishes, and the remote runs the new
      app or the old one. It must never stay dark. *2026-10-25: pulled at "Instalace", copy finished, image B ran.*
- [ ] **Remote brown-out:** run a remote OTA apply on a nearly flat battery (or a bench supply ramped down through
      ~2.0 V). Remote BOR is off (F1), so watch for a corrupt or half-copied app. Expected: the next power-on on a good
      battery finishes the copy. *2026-10-25: skipped.*
- [x] **Remote stays reachable after OTA (F6):** after every remote OTA or reset done by hand, check the remote buttons
      reach the base within ~10 s. A link that the base calls secured but that carries nothing is F6: note it, then reset the remote. *2026-10-25: after 3 OTAs a key press reached the PC.*
- [ ] **PC updater end to end (F2):** run `test_ota.py::test_base_ota_over_cdc` on a host outside this container. It
      fails here because the updater races for the port. *2026-10-25: not run (needs a host).*

## If something odd shows up

- A soak "base reset" with no USB re-enumeration is a harness read problem, not a reset (trail learning, `tools/basetest/dbg.py`).
- If `test_pairing_timeout_no_remote` fails at "base disconnected", capture the base BLE UART (D2/D3) and the base log
  before re-pairing or running `U,Z`: see the trail learning on the base RN4870 ignoring `$$$`.
