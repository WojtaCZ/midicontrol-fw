import usb.core
import usb.util
import time
import rtmidi

# Cílové VID a PID
PID = 0x4000
VID = 0xCAFE

# 1) Najít USB zařízení
dev = usb.core.find(idVendor=VID, idProduct=PID)
if not dev:
    print("USB zařízení nenalezeno.")
    exit(1)
print("USB zařízení nalezeno.")

# 2) Ověřit MIDI interface
has_midi = False
for cfg in dev:
    for intf in cfg:
        if intf.bInterfaceClass == 1 and intf.bInterfaceSubClass == 3:
            has_midi = True
            break
    if has_midi:
        break

if not has_midi:
    print("Zařízení nemá MIDI interface.")
    exit(1)
print("MIDI interface potvrzeno.")

# 3) Najít odpovídající MIDI port
midi_out = rtmidi.MidiOut()
ports = midi_out.get_ports()
print("Dostupné MIDI porty:")
for i, name in enumerate(ports):
    print(f" {i}: {name}")

target = None
for i, name in enumerate(ports):
    if "MIDI Interface" in name:
        target = i
        print("Zvolený port podle VID/PID:", name)
        break

if target is None:
    print("Nepodařilo se najít port patřící USB zařízení.")
    exit(1)

midi_out.open_port(target)
print("Otevřen port:", ports[target])

# 4) MIDI zprávy
messages = {
    "Note On": [0x90, 0x3C, 0x7F],
    "Note Off": [0x80, 0x3C, 0x00],
    "Control Change": [0xB0, 0x07, 0x64],
    "Program Change": [0xC0, 0x05],
    "Pitch Bend": [0xE0, 0x00, 0x40],
    "SysEx Example": [
        0xF0, 0x7D,                # Start, non-commercial ID (0x7D)
        0x10, 0x20, 0x30, 0x40,    # arbitrary data
        0x50, 0x60, 0x70, 0x11,    # more data
        0x22, 0x33, 0x44, 0x55,    # more data
        0x66, 0x77,
        0xF7                       # End
    ]
}

for name, msg in messages.items():
    print(f"Odesílám {name}: {' '.join([hex(i) for i in msg])}")
    midi_out.send_message(msg)
    time.sleep(0.2)

midi_out.close_port()
print("Hotovo.")
