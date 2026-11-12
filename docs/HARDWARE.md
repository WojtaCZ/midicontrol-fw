# MIDIcontrol base station: hardware

Expected system behaviour: see docs/SYSTEM.md (main copy in midicontrolpy-sw).

Status 2026-11-12. Owner decisions applied: D10 (BLE module), D11 (source of truth), D12 (SSR).

**Source of truth (D11).** The firmware pin map plus the bench are authoritative. `midicontrol-hw` (KiCad, one commit "7f9ad57 Initial commit", 2022 backups, netlist 2020-01-13) is historical reference, used only for parts the firmware does not touch. Schematic-only facts are in section 6.

Citation prefixes:
- `fw:` = `midicontrol-fw/`
- `boot:` = `midicontrol-fw/bootloader/`
- `hw:` = `midicontrol-hw/` (baseboard `MIDIcontrol.*`, frontboard `vnitrni_deska_predni.*`, rearboard `vnitrni_deska_zadni.*`). `U16.12` = U16 pad 12, taken from the `.kicad_pcb` files.

## Firmware pitfalls

Read these before touching the base firmware.

1. **Probe selection.** The base uses the STLINK-V3, serial `004200443335510C35383531`. The remote uses the ST-LINK/V2, serial `37FF70064E59313840161343`. `openocd.cfg` selects no probe, so always pass `-c "adapter serial <sn>"` before `-f openocd.cfg`. The old base serial 39FF71... is obsolete. Do not run `st-info --probe`: it leaves the cores halted. (fw:CLAUDE.md:83; fw:.trail/learnings/2026-11-07-rnbd350-reports-a-vanished-peer-as-dev.md; fw:.trail/learnings/2026-10-25-remote-ble-name-is-matched-exactly-dev-2.md)
2. **No SRST.** The probe reset line is not wired (`reset_config none separate`), so a probe reset shows as PIN | SOFTWARE in `resetcause`. openocd leaves `DHCSR.C_DEBUGEN` set, which turns a real fault into a `bkpt` halt. The IWDG stops while the core is halted. (fw:openocd.cfg:24; fw:CLAUDE.md:61,83)
3. **BOOT0 / JP1.** PB8-BOOT0 is sampled from the pin (option bytes `0xFFEFF8AA`, nSWBOOT0=1). In the wrong JP1 position the ST ROM bootloader starts and talks on USART2 to the BLE module. That looks like a dead BLE init ("$$$ timeout"). Fix the jumper. Do not change option bytes. (fw:.trail/learnings/2026-11-07-rnbd350-reports-a-vanished-peer-as-dev.md; boot:src/main.cpp:34)
4. **Which hex to flash.** Flash `midicontrol-fw-update.hex` or the merged `-full.hex`, never the plain `.hex`: a stale trailer left in flash makes the bootloader refuse the app. Holding the encoder button (PB1) at reset forces the USB bootloader (PID 0x1235). (fw:CLAUDE.md:27,43)
5. **PA4 (organ power) has no pull-down.** The pin floats from reset until the static GPIO init drives it low. Analysis says the PC817 stays off while the pin floats; this has not been measured. Never leave PA4 as an input or analog pin. After any IWDG or fault reset the firmware sends CC 120 + CC 123 on all 16 channels. (fw:src/base.cpp:19; fw:CLAUDE.md:61)
6. **Display wire is inverted, and the base hears its own TX.** The opto stage inverts the wire (PA9 low drives the line high), so the USART runs with no TXINV/RXINV. The MCU's own TX comes back on PA10. The firmware suppresses the echo only by disabling the RX DMA during a frame and re-enabling it on TX complete. ORE is never cleared, so the last echoed byte may sit in RDR (analysis, not verified). (fw:src/display.cpp:16,51-93,123,402; hw U3/U4)
7. **The display bus is shared (multi-master).** Manual controllers in the church also write song numbers to it. The base must observe the bus, not assume it is the only writer. It mirrors a foreign number and leaves playback alone (D9, D13). There is no display on the bench: PC13 stays high (not connected). (fw:.trail/learnings/2026-10-25-bench-has-no-church-display-and-dev.md)
8. **The I2C1 bus is shared.** The OLED (0x3C) shares it with the ATtiny U14 (PB0/PB1) and with BLE module pads P1_2/P1_3, which reach the bus through the adapter footprint. Both are schematic facts. Do not add an I2C device or change the address without considering both. (hw U14, U17.11/12)
9. **Leave PA5/PA6/PA7 unconfigured.** They go to the ATtiny (MIDI port controller, its own fixed firmware). The G431 does not drive them. (fw:CLAUDE.md:100)
10. **USB D+ pull-up.** The firmware keeps the internal DPPU on. The base does not enumerate without it, because R33 1k5 is not effective on the boards in use. (fw:CLAUDE.md:98)
11. **The BLE module is an RNBD350, not an RN4870** (D10). Status events carry a connection handle. A vanished peer is reported as `%ERR_CONN%`, never `%DISCONNECT%`. The module runs command set v1.0. (fw:CLAUDE.md:53-55)
12. **The DIN MIDI link has never worked on the bench.** The USB-MIDI stand-in is an Otraki. (REVIEW.md:446; fw:CLAUDE.md:85)

## 1. Overview (firmware + bench)

| Block | Fact | Source |
|---|---|---|
| MCU | STM32G431CBT6 (Cortex-M4F, 128 KB flash, 32 KB SRAM), LQFP48, SYSCLK 144 MHz | fw:src/base.cpp:37-67; fw:STM32G431CBTx_FLASH.ld |
| BLE | **RNBD350 on an adapter board with the RN4870 footprint**, in all units (D10). UART and RST as on the RN4870. The RNBD350 has no MODE pin, so MODE is not wired (D22). USART2 | D10, D22; fw:src/bluetooth.cpp:449-500 |
| MIDI DIN | USART3, 31250 Bd, one TX + one RX. Port switching (MIDI A / B) is done by the ATtiny U14 | fw:src/midi.cpp:31-35; fw:CLAUDE.md:100 |
| Church display | USART1, 1200 Bd, 9-byte frames, presence sense on PC13 | fw:src/display.cpp |
| Organ power | PA4 → SSR current source. SSR type unknown (D12) | fw:src/base.cpp:19 |
| UI | SH1106 OLED on I2C1, rotary encoder with button, 4 front + 6 rear WS2812B | fw:src/oled.cpp; fw:src/led.cpp |
| USB | Full-speed device, composite CDC (protocol) + MIDI + CDC (log). VID 0xABCD (placeholder), PID 0x1234 app / 0x1235 bootloader | fw:src/usb_descriptors.c:30-31; fw:CLAUDE.md:99 |

## 2. MCU, clocks, memory, programming

| Item | Value | Source |
|---|---|---|
| HSE | 48 MHz crystal on PF0/PF1 | fw:src/base.cpp; hw Y1 |
| PLL | HSE/6 ×36 = 288 MHz VCO. R/2 = 144 MHz SYSCLK. Q/6 = 48 MHz USB (CLK48SEL=PLLQ). P is off | fw:src/base.cpp:41-50,65-67 |
| Flash wait states | 4 WS | fw:src/base.cpp:37 |
| Bus clocks | PCLK1 = PCLK2 = 144 MHz. USART1 kernel clock = HSI16. I2C1 = PCLK1 | fw:src/base.cpp:64-67 |
| USB clock trim | CRS synced to SOF. HSI48 is started as the backup | fw:src/main.cpp; fw:src/base.cpp |
| LSE | Never started (no LSEON) | fw:src/base.cpp |
| Bootloader clocks | Core on HSI16. USB on HSI48 + CRS. Sets PWR_CR2.USV | boot:src/main.cpp |
| IWDG | LSI/32, RLR 2000 (~2 s), started on every boot. Loop watchdog 1000 ms | fw:CLAUDE.md:61 |
| Clocked, no visible use | TIM1, TIM2, TIM4, TIM7, RTCAPB | fw:src/base.cpp initClock |

| Region | Address | Size | Source |
|---|---|---|---|
| Custom USB bootloader | 0x08000000 | 32 KB | boot:STM32G431CBTx_BOOT.ld; fw:CLAUDE.md:39 |
| Application (+24-byte trailer, magic 0x4D43544C, boardId 0x02) | 0x08008000 | 94 KB | fw:STM32G431CBTx_FLASH.ld:65-71 |
| Settings | 0x0801F800 (last page) | 2 KB | fw:CLAUDE.md:41 |
| RAM | 0x20000000 | 30 KB | fw:STM32G431CBTx_FLASH.ld |
| NOINIT (fault record, then log header + ring at 0x20007860) | 0x20007800 | 2 KB | fw:STM32G431CBTx_FLASH.ld; fw:CLAUDE.md:62 |
| ST ROM bootloader | 0x1FFF0000 | — | fw:src/base.cpp:110-128 |

| Programming path | How | Source |
|---|---|---|
| SWD | J6 + STLINK-V3 (serial above). No SRST | fw:CLAUDE.md:83 |
| Custom USB bootloader | `ENTER_BOOTLOADER` (TAMP->BKP0R magic), or PB1 held low at reset. PID 0x1235 | boot:src/main.cpp; fw:CLAUDE.md:43 |
| ST ROM DFU | BOOT0 high via JP1 | boot:src/main.cpp:34 |

## 3. Pin map (firmware, authoritative)

Only pins the firmware configures. The external net is listed where the schematic names it; section 6 has the full circuits.

| Pin | MCU pin | fw name | Peripheral / AF | Electrical | Connects to | Source |
|---|---|---|---|---|---|---|
| 2 | PC13 | `disp_sense` | GPIO in, EXTI both edges | Low = display powered (`connected = !read`) | Display sense opto U18 | fw:src/display.cpp:45,53 |
| 5/6 | PF0/PF1 | — | HSE | 48 MHz | Y1 | fw:src/base.cpp |
| 8 | PA0 | `mode` | GPIO out PP | Driven high, drives nothing | Not connected on the RNBD350 adapter (D22) | fw:src/bluetooth.cpp:455 |
| 9 | PA1 | `rst_n` | GPIO out PP | Active low, 100 ms pulse | BLE RST | fw:src/bluetooth.cpp:456,644,670 |
| 10 | PA2 | `usart_tx` | USART2_TX AF7 | 115200 8N1 | BLE UART RX | fw:src/bluetooth.cpp:451,479 |
| 11 | PA3 | `usart_rx` | USART2_RX AF7 | In | BLE UART TX | fw:src/bluetooth.cpp:452 |
| 12 | PA4 | `current_enable` | GPIO out | High = organ current on. No pull-down | SSR current source | fw:src/base.cpp:19,141-157 |
| 16 | PB0 | `encoder_a` | GPIO in, pull-up | | Encoder A | fw:src/base.cpp:20 |
| 17 | PB1 | `encoder_btn` | GPIO in, pull-up | Low = pressed. Held at reset = bootloader | Encoder switch | fw:src/base.cpp:22 |
| 18 | PB2 | `encoder_b` | GPIO in, pull-up | | Encoder B | fw:src/base.cpp:21 |
| 22 | PB10 | `usart_tx` | USART3_TX AF7 | 31250 Bd | MIDI TX buffer U7 | fw:src/midi.cpp:31,35 |
| 25 | PB11 | `usart_rx` | USART3_RX AF7 | In | MIDI RX opto U13 | fw:src/midi.cpp:32 |
| 28 | PB14 | `ledStripFront` | TIM15_CH1 AF1 | 800 kHz WS2812 | Front LEDs (via BSS138 Q5) | fw:src/led.cpp:54 |
| 31 | PA9 | `usart_tx` | USART1_TX AF7, PP | 1200 Bd, no TXINV | Display TX opto U3 | fw:src/display.cpp:51,93 |
| 32 | PA10 | `usart_rx` | USART1_RX AF7 | No RXINV. Hears own TX | Display RX opto U4 | fw:src/display.cpp:52 |
| 33 | PA11 | `usb_dm` | USB DM | | USB-C via rear board | fw:src/usb.cpp:13 |
| 34 | PA12 | `usb_dp` | USB DP | Internal DPPU kept on | USB-C via rear board | fw:src/usb.cpp:14,24; fw:CLAUDE.md:98 |
| 37/38 | PA13/PA14 | — | SWDIO/SWCLK | | J6 | hw J6 |
| 39 | PA15 | `oledScl` | I2C1_SCL AF4, OD, pull-up | 400 kHz | OLED, ATtiny, BLE pad P1_2 | fw:src/oled.cpp:120,155 |
| 42 | PB5 | `ledStripRear` | TIM17_CH1 AF10 | 800 kHz WS2812 | Rear LEDs (via BSS138 Q4) | fw:src/led.cpp:52 |
| 44 | PB7 | `oledSda` | I2C1_SDA AF4, OD, pull-up | | OLED, ATtiny, BLE pad P1_3 | fw:src/oled.cpp:121 |

The firmware does not touch: PA5, PA6, PA7 (ATtiny lines), PB4/PB6 (UCPD CC, resistor Rd only), PB8-BOOT0, PC14/PC15 (LSE), PB3, PB9, PB12, PB13, PB15, PA8 (unconnected).

## 4. Peripherals (firmware)

### 4.1 BLE module (USART2)

| Item | Value | Source |
|---|---|---|
| Module | RNBD350 on an adapter with the RN4870 footprint (D10). The schematic still shows the RN4870U-V/RM118, which predates the adapter | D10; hw U17 |
| UART | 115200 Bd, no RTS/CTS, module set to `SR,0000` | fw:src/bluetooth.cpp:20-25,479 |
| DMA | TX: DMA1_Channel2 ← DMAMUX1_Channel1 req 27. RX: RXNE interrupt into a ring | fw:src/bluetooth.cpp:465-476 |
| Control | PA0 MODE is driven high but not wired (D22). PA1 RST, 100 ms low pulse, then up to 3 s wait for `%REBOOT%`. The module configures even without `%REBOOT%` | fw:src/bluetooth.cpp:449-500; fw:CLAUDE.md:53 |
| Config | `SN,MIDIControlBase`, `SA,2`, `SR,0000`, `SS,C0`, `SGA/SGC,0F`, `ST,0010,0010,0000,00FA`. Coded PHY (`CSPHY`) and `T` sent per connection | fw:CLAUDE.md:53-55 |
| Failure handling | A failed init is not fatal: one ERROR record, retried every 60 s | fw:CLAUDE.md:61 |

### 4.2 MIDI DIN (USART3)

| Item | Value | Source |
|---|---|---|
| UART | 31250 Bd, PB10 TX / PB11 RX, AF7. RX by RXNE interrupt; DMAR is set | fw:src/midi.cpp:31-35 |
| Port direction | Decided by the ATtiny U14 (fixed firmware). The G431 does not take part | fw:CLAUDE.md:100 |
| Reset safety | After an IWDG/fault reset: CC 120 + CC 123 on all 16 channels | fw:CLAUDE.md:61 |
| Bench | Dead in both directions (2026-10-19). Never verified | REVIEW.md:446 |

### 4.3 Church display (USART1)

| Item | Value | Source |
|---|---|---|
| UART | 1200 Bd from HSI16, separate TX/RX pins (no HDSEL), RTOR 100 bits. 9-byte frames, default `{0xB0, 0xE0×8}`. One frame in flight at a time (~75 ms) | fw:src/display.cpp:51-130 |
| DMA | RX: DMA2_Channel3, circular, DMAMUX1_Channel8 req 24. TX: DMA2_Channel4, DMAMUX1_Channel9 req 25 | fw:src/display.cpp:60-85 |
| Echo handling | RX DMA off for the duration of a TX frame, back on at TX complete. An incomplete RX frame is discarded on the receiver timeout | fw:src/display.cpp:123,370-405 |
| Bus model | Shared with the church's manual controllers (multi-master, D9/D13) | DECISIONS.md |
| Sense | PC13 low = display powered | fw:src/display.cpp:45,53 |

### 4.4 Organ power (PA4)

| Item | Value | Source |
|---|---|---|
| Drive | PA4 high → current source on. Low from the static GPIO init after every reset | fw:src/base.cpp:19; fw:CLAUDE.md:61 |
| SSR | Type unknown (D12). The current source suits only a bare-LED-input SSR (see section 6.4) | D12; REVIEW.md:301 |
| Bench | Drives an LED, not a relay | fw:.trail/learnings/2026-10-25-bench-has-no-church-display-and-dev.md |

### 4.5 OLED and encoder (I2C1)

| Item | Value | Source |
|---|---|---|
| Bus | 400 kHz (TIMINGR 0x30F21D35 at 144 MHz). Bootloader: 0x10320309 at HSI16. Internal pull-ups enabled as well | fw:src/oled.cpp:120-155; boot:src/main.cpp |
| DMA | TX: DMA1_Channel3 ← DMAMUX1_Channel2 req 17 | fw:src/oled.cpp:134 |
| Display | SH1106, address 0x3C, charge pump on (0x8D 0x14) | fw:inc/oled.hpp:16; fw:src/oled.cpp |
| Encoder | PB0/PB2 quadrature, PB1 button, 10 ms scheduler | fw:src/base.cpp:15-22 |

### 4.6 WS2812B strips

| Strip | Pin / timer | DMA | Pixels | Source |
|---|---|---|---|---|
| Rear | PB5 TIM17_CH1 AF10 | DMA2_Channel1 ← DMAMUX1_Channel6 req 84 | 6: 0 USB, 1 Display, 2 Current, 3 MIDI A, 4 MIDI B, 5 Bluetooth | fw:src/led.cpp:52,66-83 |
| Front | PB14 TIM15_CH1 AF1 | DMA2_Channel2 | 4. `signalError` blinks red | fw:src/led.cpp:54,95-111; fw:src/main.cpp:676 |

### 4.7 USB

| Item | Value | Source |
|---|---|---|
| Device | Composite CDC0 protocol + USB-MIDI + CDC1 log. App `ABCD:1234`, bootloader `ABCD:1235` | fw:src/usb_descriptors.c:30-31; fw:CLAUDE.md:99 |
| D+ pull-up | Internal DPPU, enabled by `tusb_init()`, kept on | fw:src/usb.cpp:24; fw:CLAUDE.md:98 |
| Type-C CC | No UCPD use. Resistor Rd only (section 6) | hw R37/R38 |

## 5. Bench facts

| Item | Value | Source |
|---|---|---|
| Probes | Base STLINK-V3 `004200443335510C35383531`. Remote ST-LINK/V2 `37FF70064E59313840161343`. No SRST | fw:CLAUDE.md:83 |
| Logic analyzer | fx2lafw/sigrok. D0 display→MCU, D1 MCU→display, D2 BLE→MCU, D3 MCU→BLE, D4/D5 remote SCL/SDA, D6/D7 remote BLE UART | fw:tools/basetest/la.py:7-18 |
| Organ stand-in | Otraki USB-MIDI. DIN link dead | fw:CLAUDE.md:85; REVIEW.md:446 |
| Display / SSR | No church display. SSR output drives an LED | fw:.trail/learnings/2026-10-25-bench-has-no-church-display-and-dev.md |
| BLE | RNBD350 since 2026-11-04. Open solder joints after the swap once made it silent on UART while it still advertised | fw:.trail/learnings/2026-11-04-rnbd350-silent-on-uart-but-advertising-dev.md |

## 6. From the (historical) schematics

Source: `midicontrol-hw` (D11: historical reference). Use it for parts the firmware does not touch. Re-check on the board before relying on it.

### 6.1 Parts

| Block | Part | Source |
|---|---|---|
| MCU | U16 STM32G431CBT6 | hw U16 |
| BLE footprint | U17 RN4870U-V/RM118 footprint (now carries the RNBD350 adapter, D10), U.FL antenna J9 | hw U17/J9 |
| MIDI DIN | J2 "MIDI A", J1 "MIDI B" (JST XH 5p). TX buffer U7 74LVC1G17, RX opto U13 6N138, port switches 74LVC1G3157 (U1, U2, U9, U10, U12) | hw baseboard |
| Port controller | U14 ATtiny1607 | hw U14 |
| Display link | Opto PC817 U3 (TX), U4 (RX), U18 (sense). J3 JST XH 4p | hw baseboard |
| Front board | SH1106 OLED on 30-pin FPC J1, ALPS EC11E encoder (SW1), 4× WS2812B (SK6812MINI footprint) | hw frontboard |
| Rear board | USB-C J2 (JAE DX07S024), USBLC6-2SC6 U1, fuse F1, 5V6 zener D6, 6× WS2812B, WAGO J3 for the SSR | hw rearboard |
| Crystals | Y1 48 MHz (PF0/PF1). Y2 32 kHz Abracon ABS25 (PC14/PC15), unused by the firmware | hw Y1/Y2 |

### 6.2 Nets behind firmware pins

| MCU pin | Net and parts | Source |
|---|---|---|
| PC13 | DISP_SENSE: U18 collector, R40 10k pull-up. DISP_VCC → R41 4k7 → U18 LED | hw |
| PA0 | BLUETOOTH_MODE → U17.16 (P2_0), R32 10k pull-up | hw |
| PA1 | BLUETOOTH_RST → U17.19 RST_N, R31 10k pull-up | hw |
| PA2 / PA3 | BLUETOOTH_RX → U17.20 / BLUETOOTH_TX ← U17.21 | hw |
| PA4 | CURRENT_SOURCE → R4 330R → U5 PC817 LED. Nothing else on the net (no pull-down) | hw |
| PB0 / PB1 / PB2 | J7.6 / J7.5 / J7.7 → front: R7/R8 10k series (A/B), 10k pull-ups R9/R11/R10, 10n C9/C11/C12. C10 DNP | hw |
| PB10 | MIDI_TX → U7 (+5VL) → port switches. R21 1k → U14.14 (ATtiny PB2) | hw |
| PB11 | MIDI_RX ← U13 pin 6, R17 330R pull-up. R22 1k → U14.13 (PB3) | hw |
| PB14 / PB5 | FRONT/BACK_LED_DIN_3V3 → BSS138 Q5 / Q4 → 5 V (R36 10k) → J7.8 / J8.8 | hw |
| PA9 | DISP_TX → R1 330R → U3 PC817 (anode on +3V3) | hw |
| PA10 | DISP_RX ← U4 collector, R5 10k pull-up | hw |
| PA11 / PA12 | R34 / R35 22R → J8.2 / J8.1. C20/C22 47 pF. R33 1k5 PA12 → +3V3 (not effective on the boards in use) | hw; fw:CLAUDE.md:98 |
| PA15 / PB7 | SCL / SDA: R44 / R45 10k pull-ups; J7.4 / J7.3; U14.16 / U14.15 (ATtiny PB0 / PB1); U17.11 / U17.12 (P1_2 / P1_3) | hw |
| PB8-BOOT0 | R18 10k → JP1.2. JP1.1 = +3V3, JP1.3 = GND | hw JP1/R18 |
| NRST | J6.5, R19 10k pull-up | hw; REVIEW.md:44 |

### 6.3 Pins the firmware does not use

| Pin | Net | Source |
|---|---|---|
| PA5 / PA6 / PA7 | MIDI_CONTROLLER_IO / _ACTIVE / _SEARCHING ↔ U14.11 / .10 / .9 (ATtiny PB5 / PB6 / PB7) | hw |
| PB4 / PB6 | USB_CC+ / USB_CC- → J8.4 / J8.5 → rear J2.B5 / A5. R38 / R37 5k1 1% to GND (Rd). D11 / D10 DNP | hw |
| PB3, PB9, PB12, PB13, PB15, PA8 | Unconnected | hw |
| VBAT | +3V3 | hw |

### 6.4 Circuits

- **Display TX** (derived): PA9 low → U3 LED on → Q1 (PNP) drives DISP_DATA to DISP_VCC through D2 + R8 200R. PA9 high (idle) → line not driven. (hw U3/Q1/R6/D2/R8)
- **Display RX** (derived): DISP_DATA → R9 4k7 → U4 LED → DISP_GND. Line high → PA10 low. (hw U4/R9/R5)
- **Display connector J3**: 1 DISP_DATA, 2 n/c, 3 DISP_GND (isolated), 4 DISP_VCC (supplied by the display). (hw J3)
- **SSR current source**: PA4 → R4 → U5 PC817 → Q2 PNP (emitter from +5V via D3, base R10 10k) → U8 LM317L. VO → R12 12R + RV1 → ADJ → F1 500 mA → CURRENT_OUT → J8.9 → rear J3.1 (WAGO 734-232), J3.2 GND. At most about 104 mA (1.25 V / 12 Ω), set by RV1. Headroom 1.5-1.9 V: suits a bare-LED-input SSR, **not a 3-32 VDC input SSR**. (hw; REVIEW.md:301)
- **MIDI**: TX: PB10 → U7 → R7 → 3157 switches → DIN pins 4/5; pin 2 via R2/R3 330R. RX: port → switches → U13 6N138 (D5, R15) → PB11. The ATtiny drives MIDI_TX_SEL (PA7), MIDI_RX_SEL (PA6), MIDI_TX_VCC_ON (PC0 → U6) and MIDI_RX_SENSE_ON/SAFE (PA5/PA4, Q3). (hw U14; REVIEW.md:285)
- **OLED address**: front J1.15 ADD_SEL → R3 4k7 → JP1 (3-pad, 1 GND / 3 +3V3, open by default). (hw front)
- **SWD J6 (2×3)**: 1 GND, 2 SWDIO, 3 SWCLK, 4 n/c, 5 NRST, 6 → JP2 (open solder jumper) → +3V3. No VTref unless JP2 is bridged. SWO not wired. (hw J6/JP2; REVIEW.md:44)
- **USB**: rear J2 → U1 USBLC6 → J1.1/2 → base J8 → R34/R35. VBUS_I → rear F1 ("xxA") → +5V. D6 5V6 on VBUS_I. Shield → C1 100n ‖ R3 330R. (hw rear)

### 6.5 Connectors

| Connector | Pinout | Source |
|---|---|---|
| J7 (base) ↔ J2 (front), IDC 2×5 | 1/2 GND, 3 SDA, 4 SCL, 5 ENC_SW, 6 ENC_A, 7 ENC_B, 8 FRONT_LED_DIN, 9 +3V3, 10 +5V | hw |
| J8 (base) ↔ J1 (rear), IDC 2×5 | 1 D+, 2 D-, 4 CC+, 5 CC-, 7/10 +5V, 8 BACK_LED_DIN, 9 CURRENT_OUT. Pins 3/6 not traced | hw |
| Front J4, 1×4 | GND, 3V3, SCL, SDA | hw front |
| Rear LED chain | D7 → D1 → D2 → D3 → D4 → D5 | hw rear |

### 6.6 Power tree

| Rail | Source | Loads | Source ref |
|---|---|---|---|
| +5V | USB VBUS via rear F1 | MC34063, WS2812 strips, SSR source (via D3), LED D12 | hw |
| +3V3 | U15 MC34063AD buck: L1 220 µH, D7 1N5819, R25 0R2, FB R26/R29, C26 1000 µF, C18 470 µF | MCU, BLE module, I2C pull-ups, OLED, opto pull-ups, LED D9 | hw |
| +5VL | +5V through D4. Supercap C3 via R39 100R, discharge via D8 | U6/U7, analog switches, U11, Q3 | hw |
| +BATT | U11 AP2112K-3.3 from +5VL | ATtiny U14, J4.6 (UPDI) | hw |

## 7. Open hardware items

Answer each one when known. Decisions D10 (BLE fit) and D11 (which source is authoritative) closed the earlier questions on module fit and schematic currency.

1. ~~BLE adapter, P1_2/P1_3.~~ RESOLVED D28: unconnected on the adapter; no load on I2C.
2. ~~BLE adapter, VDD_IO.~~ RESOLVED D28: unconnected.
3. ~~BLE adapter, MODE.~~ RESOLVED D22: the RNBD350 has no MODE pin; P2_0 (PA0) is not wired.
4. **SSR type (D12).** What SSR is fitted at the organ, and what is RV1 set to? The source gives at most ~104 mA at 1.5-1.9 V headroom.
5. ~~PA4 pull-down.~~ RESOLVED D28: the float is accepted; no pull-down.
6. ~~R33 1k5 D+ pull-up.~~ RESOLVED D28: not fitted; the internal DPPU is the only pull-up.
7. **ATtiny.** Does its firmware drive PA5/PA6/PA7 or the I2C lines, or answer on I2C (collision with 0x3C)? Is its source available?
8. **MIDI A / B.** Which port does the ATtiny make OUT and which IN? What are R21/R22 to ATtiny PB2/PB3 for?
9. **DIN link dead (must be fixed before installation, D31 / SYS-NOTE-05).** Cause not traced (cable, ATtiny direction, MIDI_TX_VCC_ON). TX polarity to the DIN pins not verified.
10. **Display echo and polarity.** Confirm on a real display: the line idles undriven, and the firmware's RX-DMA gating is enough to reject its own echo. "Half-duplex" in fw:CLAUDE.md:56 describes the single wire, not the USART mode.
11. **USB-C CC.** Are R37/R38 fitted? Are D10/D11 (DNP) needed? Is UCPD ever to be used?
12. **Rear fuse F1.** The value is "xxA". What is fitted?
13. **Y2 + supercap C3.** Is Y2 fitted? What is +5VL / C3 for (holding the ATtiny and switch state through USB drop-outs)?
14. **BOOT0 / JP1.** Is the normal position GND? With JP1 open, BOOT0 has no pull (R18 goes only to the jumper).
15. **SWD VTref.** Is JP2 bridged on the boards in use?
16. **OLED address.** Is every front board at 0x3C (JP1 open)?
17. **Rear LED order.** Do the panel labels match pixels 0..5 = USB, Display, Current, MIDI A, MIDI B, Bluetooth on chain D7, D1..D5?
18. **Front J4.** What is the 1×4 I2C header for?

Firmware housekeeping (not hardware): TIM1/TIM2/TIM4/TIM7/RTCAPB are clocked with no visible use; `dfuCheck()` (fw:src/base.cpp:110-128) is never called. Keep them or delete them.
