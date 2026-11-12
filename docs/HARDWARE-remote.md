<!-- COPY of midicontroller-fw/docs/HARDWARE.md. Do not edit here: edit the main copy, then run midicontrolpy-sw/tools/sync-system-doc.sh -->

# MIDIcontrol remote ("midicontroller", "ovladac"): hardware

Expected system behaviour: see docs/SYSTEM.md (main copy in midicontrolpy-sw).

Status 2026-11-12. Owner decisions applied: D10 (BLE module), D11 (source of truth), D17 (power).

**Source of truth (D11).** The firmware pin map plus the bench are authoritative. `midicontroller-hw` (`ovladac.xml` netlist and `ovladac.pdf`, 2020-01; one commit; PCB edited until 2026-10-27) and `midicontroller-fw/schematic.pdf` (2020-04-06) are historical reference, used only for parts the firmware does not touch. Schematic-only facts are in section 6.

Citation prefixes:
- paths with no prefix = `midicontroller-fw/`
- `CL:<line>` = `midicontroller-fw/CLAUDE.md:<line>`
- `sch` = `midicontroller-hw/ovladac.xml` / `ovladac.pdf`, by reference designator

## Firmware pitfalls

Read these before touching the remote firmware.

1. **A reset powers the remote off.** `SystemInit` drives PA1 (`MCU_PWR_ON`) high to hold the 3V3 latch. Any reset releases PA1. This applies to a fault, the IWDG, a stall, `NVIC_SystemReset` and `fault::testInject`. Someone must then press ON/OFF. Since the 2026-10-19 change, probe and software resets during flashing have not powered the bench board off. If one does, hold ON/OFF. The bootloader also latches PA1 first, and releases it when no app is bootable. (CL:43,62,92; `src/base.cpp:25,44`; `bootloader/src/main.cpp:18-25`)
2. **Probe selection.** The remote uses the ST-LINK/V2, serial `37FF70064E59313840161343`. The base uses the STLINK-V3, serial `004200443335510C35383531`. `openocd.cfg` selects no probe and uses `reset_config none separate` (no SRST), so pass `-c "adapter serial <sn>"`. Back up to `../backups/` before every flash. `mcu flash` writes the `-full.hex` (bootloader + app). (CL:87; `openocd.cfg:24`; `midicontrol-fw/CLAUDE.md:83`)
3. **BOOT0 / JP1.** PA14 is both SWCLK and BOOT0. The strap is R13 10k → JP1 (3-pad solder jumper, +3V3 / GND). Whether the pin is sampled at all depends on the G0 option bytes (nBOOT_SEL), which have not been read. Do not change option bytes. (sch R13, JP1)
4. **The I2C1 bus is shared.** OLED SH1106 (0x3C) and charger BQ24195 (0x6B) share PB6/PB7. Schematic-only: the RN4870-footprint pads P1_2/P1_3 (U4.13/14), now under the RNBD350 adapter, are on the bus too. A hung charger transaction blocks the display. The driver does a 9-clock bus recovery. (`src/i2cbus.cpp:10-11`; `src/bq24195.cpp:10`; sch U4)
5. **No USB data.** PA11/PA12 are not connected. USB-C only charges the battery (BQ24195), and UCPD1 reads CC for the input limit. Firmware updates go over BLE through the base into the W25Q32 (OTA staging), or over SWD. (sch J2; CL:57; `src/usbc.cpp`)
6. **The BLE module is an RNBD350 on an RN4870-footprint adapter** (D10). Status events carry a connection handle. The module has no MODE pin, so MODE is not wired (D22): PB9 is driven high and drives nothing. The remote advertises extended on Coded PHY, so phones cannot see it. (`src/bluetooth.cpp:24-26`; CL:47,49)
7. **Power hold and button.** PB13 is ON/OFF, active high since the 2026-10-19 hardware change. Holding it 5 s shuts down. The shutdown path writes the diag slot with interrupts off before it releases PA1. Auto-off after 20 min idle never fires on VBUS, so it does not fire on the bench. (CL:59,61,92)
8. **Unfiltered ADC.** The PA0/PA5 dividers are 4.7M/8.2M with no capacitor and pick up hum when the remote is held. The cut-off uses the unsmoothed 1 s mean. (CL:53; `src/battery.cpp:12`)
9. **USB D+/D- are crossed into the BQ24195, and R18 is removed on the bench board.** The BQ's own source detection can report "unknown" (100 mA). The firmware overrides IINLIM from the Type-C CC advertisement. (CL:99)

## 1. Overview (firmware + bench)

| Item | Fact | Source |
|---|---|---|
| Function | Handheld BLE remote for the base; BLE peripheral, Transparent UART | CL:7 |
| MCU | STM32G071CBT6 (Cortex-M0+, 128 KB flash, 36 KB SRAM, no FPU), LQFP48 | CL:7; linker |
| BLE | **RNBD350 on an adapter board with the RN4870 footprint**, in all units (D10). UART and RST as on the RN4870. The RNBD350 has no MODE pin, so MODE is not wired (D22). USART2 | D10, D22; `src/bluetooth.cpp:22-26` |
| Display | SH1106 128×64 OLED, I2C 0x3C | `inc/oled.hpp:16-21` |
| Keys | 5×3 matrix + OK + ON/OFF | `src/base.cpp` |
| Storage | W25Q32 SPI NOR (4 MB) on SPI1: OTA staging + diag slots | `lib/bootloader/src/w25q.cpp`; CL:57,61 |
| Charger | BQ24195 on I2C1 (0x6B), INT on PB4, OTG on PB5 | `src/bq24195.cpp` |
| Battery | 2P Panasonic NCR18650B, 7 Ah, 4.20 V | CL:54 |
| USB | USB-C, power and charging only; UCPD1 sink for CC detection | CL:55,98 |
| LEDs, buzzer | None. All feedback is on the OLED | sch; fw |

## 2. MCU, clocks, memory

| Item | Value | Source |
|---|---|---|
| Clock | HSI16 → PLL (M=1, N=8, R=2) = 64 MHz SYSCLK, 2 WS | `src/base.cpp:56-63` |
| Bootloader clock | HSI16, no PLL | `bootloader/src/main.cpp` |
| HSE / LSE | None. IWDG on LSI | sch; `src/wdg.cpp` |
| Peripheral clocks | USART2, I2C1, TIM3 (enabled, unused), SYSCFG, ADC, DMA1 | `src/base.cpp:88` |
| DMA | DMA1 ch1 ← DMAMUX ch0 req 11 (I2C1_TX). DMA1 ch2 ← DMAMUX ch1 req 53 (USART2_TX) | CL:101 |
| IWDG | ~4 s (LSI/64, RLR 2000), `DBG_IWDG_STOP`. Loop watchdog 2000 ms | `src/wdg.cpp:25-34`; CL:62 |
| Main loop | WFI between scheduler passes, no stop mode | CL:43 |
| Fault handling | Fault → FAULT records + W25Q slot → `NVIC_SystemReset()` (= power off). `fault::haltOnFault = 1` (set by the probe) gives a `bkpt` | CL:62 |

| Region | Address | Size | Source |
|---|---|---|---|
| Bootloader | 0x08000000 | 16 KB | `STM32G071CBTx_BOOT.ld` |
| Application (+24-byte trailer, magic 0x4D43544C, boardId 0x03) | 0x08004000-0x0801F800 | 110 KB | `STM32G071CBTx_FLASH.ld`; `bootloader/CMakeLists.txt:14` |
| Settings | 0x0801F800 | 2 KB | `bootloader/CMakeLists.txt:14` |
| RAM | 0x20000000 (heap 0x200, stack 0x400) | 36 KB | `STM32G071CBTx_FLASH.ld` |
| W25Q diag sector | 0x3FF000 (16 × 256 B) | 4 KB | CL:61 |

## 3. Pin map (firmware, authoritative)

Only pins the firmware configures. Section 6 has the external circuits.

| Pin | MCU pin | fw function | Peripheral / AF | Electrical | Connects to | Source |
|---|---|---|---|---|---|---|
| 11 | PA0 | battery voltage | ADC_IN0 | Ratio 0.636 | Battery divider | `src/battery.cpp:12` |
| 12 | PA1 | `power_hold` | GPIO out PP | High = stay on. Set first in SystemInit | 3V3 latch (MCU_PWR_ON) | `src/base.cpp:25,44` |
| 13 | PA2 | BLE TX | USART2_TX AF1 | 115200 8N1 | BLE UART RX | `src/bluetooth.cpp:22` |
| 14 | PA3 | BLE RX | USART2_RX AF1, pull-up | | BLE UART TX | `src/bluetooth.cpp:23` |
| 15 | PA4 | `spi_cs` | GPIO out | Active low | W25Q32 CS | `lib/bootloader/src/w25q.cpp:21` |
| 16 | PA5 | USB 5 V sense | ADC_IN5 | Ratio 0.636. VBUS present at ≥ 4000 mV | VBUS divider | `src/battery.cpp:12,26-32` |
| 17 | PA6 | SPI1 MISO | AF0 | | W25Q32 | `lib/bootloader/src/w25q.cpp` |
| 18 | PA7 | SPI1 MOSI | AF0 | | W25Q32 | `lib/bootloader/src/w25q.cpp` |
| 20-24 | PB1, PB2, PB10, PB11, PB12 | key rows R1-R5 | GPIO out PP | Driven low to scan | Matrix rows (via diodes) | `src/base.cpp` |
| 25 | PB13 | `on_off_btn` | GPIO in, no pull | Active high (since 2026-10-19). Press = no low sample for 20 ms | Power button via level shifter | `src/base.cpp:27`; CL:92 |
| 26 | PB14 | `ok_btn` | GPIO in, pull-up | Active low | OK / ENTER key | `src/base.cpp:42` |
| 27 | PB15 | USB CC2 | UCPD1_CC2 | Sink Rd, detection only | USB-C B5 | `src/usbc.cpp`; CL:98 |
| 28 | PA8 | USB CC1 | UCPD1_CC1 | Sink Rd, detection only | USB-C A5 | `src/usbc.cpp`; CL:98 |
| 29 / 32 | PA9 / PA10 | DBCC1 / DBCC2 | UCPD1 dead-battery Rd | Dropped (UCPD1_STROBE) once UCPD takes over | Tied to PA8 / PB15 | `src/usbc.cpp:31-34`; CL:98 |
| 35 / 36 | PA13 / PA14-BOOT0 | SWDIO / SWCLK | SWD | | J3 | sch J3 |
| 38-40 | PD0, PD1, PD2 | key columns C3, C2, C1 | GPIO in, pull-up | UCPD2 dead-battery pull-down disabled (UCPD2_STROBE) | Matrix columns | `src/base.cpp:103` |
| 42 | PB3 | SPI1 SCK | AF0 | | W25Q32 | `lib/bootloader/src/w25q.cpp` |
| 43 | PB4 | BQ_CHRG_INT | GPIO in, pull-up, EXTI4 falling | 256 µs active-low pulse | BQ24195 /INT | `src/bq24195.cpp:12,67` |
| 44 | PB5 | BQ_CHRG_OTG | GPIO out | Driven high | BQ24195 OTG | `src/bq24195.cpp:63` |
| 45 | PB6 | I2C1 SCL | AF6, OD, pull-up | ~120 kHz | OLED, BQ24195 (+ BLE pad P1_2, sch) | `src/i2cbus.cpp:10-11` |
| 46 | PB7 | I2C1 SDA | AF6, OD | | OLED, BQ24195 (+ BLE pad P1_3, sch) | `src/i2cbus.cpp:10` |
| 47 | PB8 | BLE reset | GPIO out | Active low, 100 ms, then up to 3 s for `%REBOOT%` | BLE RST (RNBD350 NMCLR) | `src/bluetooth.cpp:24` |
| 48 | PB9 | BLE mode | GPIO out | Driven high, drives nothing | Not connected on the RNBD350 adapter (D22) | `src/bluetooth.cpp:25-26` |

The firmware does not touch: PC13-PC15, PF0/PF1, PB0, PC6/PC7, PA11/PA12, PA15, PD3 (all unconnected per sch).

Keymap (`src/base.cpp`, checked switch by switch against the netlist):

| Row | C1 (PD2) | C2 (PD1) | C3 (PD0) |
|---|---|---|---|
| R1 (PB1) | 1 | 2 | 3 |
| R2 (PB2) | 4 | 5 | 6 |
| R3 (PB10) | 7 | 8 | 9 |
| R4 (PB11) | MF | 0 | UP |
| R5 (PB12) | DOWN | LEFT | RIGHT |

- Indexes 0-14 row-major; OK = 15.
- GUI mapping: MF → Left, LEFT → Back, RIGHT → Right, OK → Enter.
- Debounce in the 1 ms SysTick: a press counts on the first low sample; a release needs 20 ms high. (CL:46)

## 4. Peripherals (firmware)

### 4.1 BLE (USART2)

| Item | Value | Source |
|---|---|---|
| Module | RNBD350 on an RN4870-footprint adapter (D10). The schematic still shows the RN4870-V_RM118 (U4), which predates the adapter | D10; sch U4 |
| UART | 115200 8N1, no RTS/CTS. TX via DMA1 ch2. RX IRQ at priority 0 | `src/bluetooth.cpp:364` |
| Config | `SN,MIDIControlRemote`, `SS,C0`, `SR,0000`, `SA,2`, `SGA,0F`, `SGC,0F`, `ST,0010,0010,0000,00FA`. Extended advertising on Coded PHY. `CSPHY` (Coded S8) after every `%SECURED%` | CL:47,49 |
| Module replacement | A new module has a new MAC: pair it once from the base menu | CL:95 |

### 4.2 Display (I2C1)

| Item | Value | Source |
|---|---|---|
| Controller | SH1106, 132-column RAM, `OLED_XOFFSET 2` | `inc/oled.hpp:16-21` |
| Bus | ~120 kHz (TIMINGR 0x0070215B from HSI16), DMA1 ch1, 9-clock recovery | `src/i2cbus.cpp:11` |
| Address | 0x3C fixed, no fallback for 0x3D | `inc/oled.hpp:16` |
| Init | Charge pump on (0x8D 0x14), contrast 0xFF, remap 0xA1, COM scan 0xC8 | `src/oled.cpp` |
| Sleep | Off after 30 s without input | `inc/oled.hpp:24` |

### 4.3 Charger and battery

| Item | Value | Source |
|---|---|---|
| BQ24195 settings | REG00 = 0x32 (IINLIM 500 mA, VINDPM 4.36 V). REG02 ICHG 2048 mA. REG05 = 0x8C (watchdog off, termination on, 12 h timer). Termination 256 mA | `src/bq24195.cpp:83-88`; CL:54 |
| IINLIM from Type-C | Rp 3 A → 2 A. Rp 1.5 A → 1.5 A. Otherwise the BQ D+/D- result | CL:53 |
| ADC | HSI16/64, 160.5 cycles. Channels 0, 5, VREFINT 13 (calibration at 0x1FFF75AA, 3.0 V) | `src/battery.cpp:39,89-91` |
| Thresholds | Warning 3450 mV. Shutdown 3300 mV (5 consecutive 1 s means). 4180 mV = 100 %, 3300 mV = 0 % | `src/battery.cpp:26-32` |

### 4.4 Power on/off

- On: SW1 (or VBUS through R27) raises 3V3_ON; the LDO starts; SystemInit sets PA1 high to hold it.
- Off: `shutdown()`: message → OLED sleep → BLE held in reset (PB8 low) → diag slot → PA1 low → wait for a press → `NVIC_SystemReset`. (`src/base.cpp:332`)
- Auto-off: 20 min idle; never while VBUS is present or a song plays. (CL:59)
- Power optimisation: easy wins only, no stop-mode redesign (D17).

## 5. Bench facts

- Probe: ST-LINK/V2 `37FF70064E59313840161343`, openocd `stm32g0x.cfg`, hla_swd, SVD STM32G071xx. (CL:87; `.mcu-profile.json`; `openocd.cfg`)
- Bench board modifications (CL:92-99):
  - R18 removed (2026-10-19)
  - R24/R25 not fitted
  - PB13 hardware change (2026-10-19)
  - RNBD350 on the adapter (2026-10-26)
- The battery reading jumps when the remote is touched.
- Logic analyzer channels D6/D7 carry the remote's BLE UART; D4/D5 its SCL/SDA. (`midicontrol-fw/tools/basetest/la.py:7-18`)
- Remote OTA through the PC took 2 m 32 s (progress.md, 2026-11-07).

## 6. From the (historical) schematics

Source: `midicontroller-hw/ovladac.xml` + `ovladac.pdf` (D11: historical reference). `midicontroller-fw/schematic.pdf` (2020-04-06) is newer and has not been diffed against them. Neither shows the 2022-2026 PCB edits.

### 6.1 Parts

| Block | Part | Source |
|---|---|---|
| MCU | IC1 STM32G071CBT6 | sch IC1 |
| BLE footprint | U4 RN4870-V_RM118 (now carries the RNBD350 adapter, D10). The PCB references the 3D model `RN48720_to_RNBD350.step` | sch U4; `midicontroller-hw/ovladac.kicad_pcb:35759` |
| Display | 30-pin FPC J5 | sch J5 |
| Flash | U5 W25Q32JVSS | sch U5 |
| Charger | U1 BQ24195, L1 2.2 µH | sch U1 |
| Battery | J1 (JST XH) → F3 ("xxA") → +BATT | sch J1 |
| LDO | U2 AP2112K-3.3 behind PMOS Q2, latch Q1 BC817 | sch U2/Q1/Q2 |
| USB | J2 USB-C, U3 USBLC6-2SC6 | sch J2/U3 |
| Keys | SW2-SW16 matrix with diodes D9-D23, SW17 OK, SW1 power | sch |

### 6.2 Nets behind firmware pins

| MCU pin | Net and parts | Source |
|---|---|---|
| PA0 | MCU_BATT_VOLTAGE: R6 4M7 / R7 8M2, D3 3V6 zener, no capacitor | sch |
| PA1 | MCU_PWR_ON → R22 → D6 → 3V3_ON | sch |
| PA2 / PA3 | BT_RX → U4.22 / BT_TX ← U4.23 (RNBD350 PB6 / PA5 per CL:95) | sch U4; CL:95 |
| PA4, PA6, PA7, PB3 | CS → U5.1, MISO ← U5.2, MOSI → U5.5, SCK → U5.6 | sch |
| PA5 | MCU_5V: R39 4M7 / R40 8M2, D25 3V6 zener | sch |
| PB1-PB12 rows | KEYBOARD_R1..R5 → D9-D23 | sch |
| PD0-PD2 columns | KEYBOARD_C3/C2/C1, pull-ups R30/R29/R28 | sch |
| PB13 | MCU_PWR_SWITCH: Q3 BSS138 level shifter, R26 10k to 3V3. Pre-2026-10-19 polarity | sch Q3/R26 |
| PB14 | KEYBOARD_ENTER: SW17 to GND, R31 pull-up | sch |
| PA8 / PB15 | USB_CC- / USB_CC+ → J2.A5 / B5. R24 / R25 5k1 (not fitted). D7 / D8 DNP | sch; CL:98 |
| PB4 / PB5 | U1.7 /INT (R38 pull-up) / U1.8 OTG | sch |
| PB6 / PB7 | SCL: J5.18, U1.5, R36 10k, U4.13 (P1_2). SDA: J5.19, J5.20, U1.6, R37 10k, U4.14 (P1_3) | sch |
| PB8 | BT_RST → U4.21 RST_N, R42 pull-up | sch |
| PB9 | BT_MODE → U4.18 P2_0, R41 pull-up | sch |
| PA14-BOOT0 | J3.2, R13 10k → JP1 (+3V3 / GND) | sch |
| NRST | J3.4, R12 10k pull-up | sch |

### 6.3 Circuits

Power latch:

```
SW1 ──R21──┐
+5V ──R27──┤ PWR_SWITCH ──R23──GND ; ──Q3 BSS138── PB13
           └─D5──┐
PA1 ─R22─D6──────┴─ 3V3_ON ─R15 330R─ Q1 BC817 base (R17 10k pull-down; R16 DNP)
Q1 collector pulls the gate of Q2 (PMOS) → VS → U2 AP2112K-3.3 (EN tied to VIN) → +3V3
VS: +BATT ─JP3─ VS ─F1 200 mA─ SYS (BQ SYS / L1)
```

- **Debug header J3** (5 pins): 1 GND, 2 SWCLK/BOOT0, 3 SWDIO, 4 NRST, 5 +3V3.
- **Charger**: ILIM R4 330R. /CE tied low by R2 (charging enabled). STAT not connected. D1 DNP. No NTC: TS1/TS2 sit on a fixed divider (R5 5k49 from REGN, R3 10k, R44 31.23k).
- **USB**: J2 → F2 ("xxA"; CL:53 says 2.6 A hold) → +5V → BQ VBUS. D4 5V6, C11 100n. D+/D- → U3 → R19/R20 22R → BQ D+/D-, **crossed**. R18 1k5 +3V3 → BQ_D- (removed on the bench board). DNP: R43, R1, D7, D8, D1.
- **Display panel**: 3V3 on J5.6/9. VCC on J5.14 from R33/C15/D24. Charge-pump caps C18/C19. IREF J5.26 via R35 560k. J5.11 via R34 ("910k / 4k7"). No reset line. Address strap J5.15 ADD_SEL → R32 4k7 → JP4 (GND = 0x3C, 3V3 = 0x3D).
- **BLE footprint**: U4 VBAT = +3V3. VDD_IO (U4.6/7) has no other connection. CTS U4.15, RTS U4.17 and LED U4.30 are not connected.

## 7. Open hardware items

Answer each one when known. D10 closed the question of whether the RNBD350 adapter is the production fit. D11 closed the question of which schematic is authoritative.

1. **BLE adapter, P1_2/P1_3.** What does the adapter connect to the RN4870 pads P1_2/P1_3 (U4.13/14, on the I2C bus)? Can they load the bus?
2. **BLE adapter, VDD_IO.** U4.6/7 have no net. What does the adapter do with VDD_IO?
3. **BLE adapter, TX pull-up.** (MODE resolved D22: not wired.) Does the adapter have the recommended 10k pull-up on the RNBD350 TX (PA5)?
4. **PB13 change.** What exactly changed on 2026-10-19 (R27, Q3, R26)? Is it on every board?
5. **R18.** Removed or DNP on every board?
6. **R24/R25.** Keep them unfitted on future boards (the firmware relies on the dead-battery Rd + UCPD)?
7. **USB D+/D- crossing.** Fix it in a revision, or accept it (the firmware compensates via CC)?
8. **F2/F3.** The schematic says "xxA". What is fitted? F2 has a 2.6 A hold per CL:53; F3 is unknown.
9. **JP3 / F1.** Which way is JP3 set? Is F1 really 200 mA in the BQ SYS path?
10. **No NTC.** Is charging without temperature monitoring intended?
11. **OTG pin.** PB5 is held high (500 mA limit / boost enable). Intended? Boost is never used.
12. **OLED strap JP4.** Always GND (0x3C)? The firmware has no 0x3D fallback.
13. **R34.** "910k / 4k7": which value is fitted?
14. **ADC filter.** Add a capacitor on PA0/PA5 in a revision?
15. **Charger variant.** BQ24195 or BQ24195L (different VINDPM/IINLIM defaults)?
16. **BOOT0.** Which side is JP1 bridged to? What are the G0 option bytes (nBOOT_SEL, nBOOT0), i.e. is the pin sampled at all?
17. **Cells.** Are the two NCR18650B really 2P on J1? With one protection circuit, or none?

Firmware housekeeping (not hardware): TIM3 is clocked but unused (`src/base.cpp:88`). `.mcu-profile.json` sizeBudget uses 32768 B RAM; the device has 36 KB.
