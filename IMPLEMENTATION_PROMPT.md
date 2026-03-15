# Protocol Module Integration Prompt

## Task Overview
Integrate the existing protocol module (located in `lib/protocol/`) into the USB and Bluetooth communication layers of the MIDIControl firmware. The protocol module provides frame encoding/decoding with proper CRC validation and escape sequences. Your task is to:

1. **Modify USB Communication** (`src/usb.cpp`, `src/usb_descriptors.c`)
2. **Modify Bluetooth Communication** (`src/bluetooth.cpp`)
3. **Refactor Command Handler** (`src/comm.cpp`, `inc/comm.hpp`)
4. **Implement Protocol Command Handlers**


Implement existing messages from https://github.com/WojtaCZ/midicontrol-sw-py. Modify the python accordingly and clear up the unnecessary code, such that it works as a master communication app that controls the downstream devices (this MIDIControl Base and a controller connected over bluetooth).


---

## Current Architecture

### Protocol Module (Already Implemented - `lib/protocol/`)

The protocol module is already fully implemented and provides:

- **Parser** (`protocol::Parser`) - Parses incoming byte streams into protocol frames
  - `bool feed(uint8_t byte)` - Returns true when a complete frame is ready
  - `const Packet &getPacket() const` - Retrieves the decoded packet

- **Codec** - Encodes and decodes frames
  - `uint16_t encode(const Packet &pkt, uint8_t *output)` - Encodes packet to raw bytes
  - `bool decodeFrame(const uint8_t *raw, uint8_t len, Packet &pkt)` - Decodes raw frame

- **Message Types** (`protocol::MsgType`)
  ```
  ACK = 0x01
  NAK = 0x02
  PLAY_SONG = 0x10
  STOP_SONG = 0x11
  RECORD_SONG = 0x12
  GET_SONG_LIST = 0x20
  SONG_LIST_RESP = 0x21
  SIGNAL_PLAYING = 0x30
  SIGNAL_RECORDING = 0x31
  SIGNAL_STOPPED = 0x32
  SET_CURRENT = 0x40
  GET_STATUS = 0x50
  STATUS_RESP = 0x51
  ```

- **Packet Structure** (`protocol::Packet`)
  ```cpp
  uint8_t source;         // ADDR_PC (0x01), ADDR_BASE (0x02), ADDR_REMOTE (0x03), ADDR_BROADCAST (0xFF)
  uint8_t dest;
  uint8_t packetId;
  MsgType msgType;
  uint8_t payload[MAX_PAYLOAD_SIZE];  // MAX_PAYLOAD_SIZE = 128
  uint8_t payloadLen;
  ```

### Current USB Communication (`src/usb.cpp`)

Currently handles:
- USB initialization
- Vendor-specific control transfers with binary commands (not protocol frames)
- CDC/MIDI interfaces already configured

### Current Bluetooth Communication (`src/bluetooth.cpp`)

Currently handles:
- RN4870 BLE module UART communication
- Protocol Parser is already instantiated: `protocol::Parser _protocolParser`
- Protocol packet ready flag: `volatile bool _protocolPacketReady`
- USART2 IRQ handler receives data

### Current Command Structure (`src/comm.cpp`, `inc/comm.hpp`)

Current binary commands:
- `BASE_STATUS`, `BASE_UID`, `BASE_CURRENT`
- `DISPLAY_STATUS`, `DISPLAY_SONG`, `DISPLAY_VERSE`, `DISPLAY_LETTER`, `DISPLAY_LED`

Uses simple SET/GET direction model with USB vendor control requests.

---

## Requirements

### 1. USB Protocol Integration

**Modify `src/usb.cpp`:**
- Add protocol frame handling to USB vendor control transfers
- Incoming protocol frames should be passed to protocol parser
- When a complete protocol frame is received via USB, it should be passed to the unified protocol handler
- Protocol responses should be encoded and sent back via USB
- Maintain backward compatibility with existing CDC/MIDI interfaces

**Integration Points:**
- `tud_vendor_control_xfer_cb()` - Handle incoming protocol frames
- Parse incoming frames byte-by-byte using `protocol::Parser::feed()`
- When frame is complete, call protocol handler

### 2. Bluetooth Protocol Integration

**Modify `src/bluetooth.cpp`:**
- The USART2 IRQ handler already receives data - pass it to the protocol parser
- Protocol parser is already instantiated as `_protocolParser`
- When `_protocolPacketReady` becomes true, handle the protocol message
- Use `sendProtocolPacket()` to send protocol responses back
- Maintain existing Bluetooth command/response handling (ADV_TIMEOUT, CONNECT, etc.)

**Integration Points:**
- In `USART2_IRQHandler()` - Feed received bytes to parser
- After parsing completes - set `_protocolPacketReady = true`
- Create handler function to process protocol packets
- Send protocol frames using existing `sendProtocolPacket()` function

### 3. Unified Command Handler (Refactor `src/comm.cpp`)

**Rewrite the command processing:**
- Create a new function: `bool handleProtocolMessage(const protocol::Packet &packet, protocol::Packet &response)`
- This function should:
  - Decode the incoming protocol message based on `MsgType`
  - Extract payload data
  - Call appropriate subsystem handlers (display, base, etc.)
  - Build response packet with results
  - Handle errors and send NAK messages

**Function Signature:**
```cpp
namespace communication {
    bool handleProtocolMessage(const protocol::Packet &packet, protocol::Packet &response);
}
```

**Response Patterns:**
- For successful commands: Send `ACK` with payload (if applicable)
- For unknown/invalid commands: Send `NAK` with reason (`UNKNOWN_MSG`, `INVALID_PAYLOAD`)
- For errors: Send `NAK` with appropriate reason

### 4. Protocol Command Handlers

Implement handlers for each protocol message type in `comm.cpp`:

#### Song Control Commands
- `PLAY_SONG`: Payload contains song ID (2 bytes, little-endian)
  - Call existing display system to play song
  
- `STOP_SONG`: No payload required
  - Stop current playback
  
- `RECORD_SONG`: Payload contains song ID (2 bytes, little-endian)
  - Start recording to specified song

#### Song List Request
- `GET_SONG_LIST`: No payload
  - Response type: `SONG_LIST_RESP`
  - Payload: Song count (1 byte) + array of song IDs/metadata
  
#### Status Signals (Device → Host)
- `SIGNAL_PLAYING`: Payload contains current song ID (2 bytes)
- `SIGNAL_RECORDING`: Payload contains current song ID (2 bytes)
- `SIGNAL_STOPPED`: No payload

#### Current Item Setting
- `SET_CURRENT`: 
  - Payload format: command_type (1 byte) + data (variable)
  - 0x01: Set verse - data is verse number (1 byte)
  - 0x02: Set letter - data is letter (1 byte)
  - 0x03: Set LED color - data is color value (1 byte)

#### Status Queries
- `GET_STATUS`: No payload
  - Response type: `STATUS_RESP`
  - Payload format:
    - Byte 0: Status flags (connected, recording, playing bits)
    - Byte 1: Current song ID (low)
    - Byte 2: Current song ID (high)
    - Byte 3: Current verse
    - Byte 4: Current letter
    - Byte 5: Current LED color

### 5. Integration with Existing Subsystems

Ensure handlers use existing functions:
- `display::getSong()`, `display::setSong()`
- `display::getVerse()`, `display::setVerse()`
- `display::getLetter()`, `display::setLetter()`
- `display::getLed()`, `display::setLed()`
- `base::current::set()`, `base::current::isEnabled()`
- `display::isConnected()`

---

## Implementation Strategy

### Phase 1: Bluetooth Integration
1. Modify `USART2_IRQHandler()` to feed bytes to protocol parser
2. Create protocol packet handler in Bluetooth namespace
3. Test protocol frame reception via Bluetooth

### Phase 2: USB Integration
1. Modify `tud_vendor_control_xfer_cb()` to handle protocol frames
2. Implement protocol frame sending via USB
3. Test protocol frame reception via USB

### Phase 3: Unified Command Handler
1. Rewrite `processBinaryCommand()` or create new `handleProtocolMessage()`
2. Implement all protocol message type handlers
3. Ensure NAK responses for unsupported/invalid messages

### Phase 4: Testing & Validation
1. Verify protocol frames can be sent/received via both USB and Bluetooth
2. Test all command handlers
3. Verify CRC validation works
4. Test error handling and NAK responses

---

## Key Notes

- **Packet IDs**: Use monotonically increasing packet IDs for tracking
- **ACK/NAK Pattern**: ACK should echo back the packet ID for correlation
- **Addresses**: 
  - `ADDR_PC = 0x01` (Computer)
  - `ADDR_BASE = 0x02` (This device)
  - `ADDR_REMOTE = 0x03` (Remote device)
  - `ADDR_BROADCAST = 0xFF` (Broadcast)
- **Error Handling**: Always respond with NAK if message is invalid or processing fails
- **Backward Compatibility**: The USB vendor interface currently uses simple binary commands - ensure this doesn't break or migrate all commands to use the new protocol format
- **CRC**: Already handled by the protocol module, no additional validation needed
- **Frame Format**: Already defined in protocol module - don't modify frame structure

---

## Files to Modify

1. **`src/usb.cpp`** - Add protocol frame handling
2. **`src/bluetooth.cpp`** - Integrate parser and protocol handling
3. **`src/comm.cpp`** - Rewrite/refactor command handlers
4. **`inc/comm.hpp`** - Update function signatures if needed
5. **`src/usb_descriptors.c`** - May need adjustments if changing USB interface

---

## Success Criteria

- ✅ Protocol frames can be received via Bluetooth and parsed correctly
- ✅ Protocol frames can be received via USB and parsed correctly
- ✅ All protocol message types generate appropriate responses
- ✅ Invalid/unsupported messages trigger NAK responses
- ✅ CRC validation works automatically (via protocol module)
- ✅ Existing subsystem functionality (display, LED, etc.) works correctly through protocol
- ✅ ACK messages include correct packet ID for response correlation
- ✅ Firmware compiles without errors or warnings
