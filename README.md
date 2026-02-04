# ESP32 RF Probe & Path Loss Analyzer (v4.18)

ESP-NOW based RF link tester: **Master** sends pings and measures path loss / RSSI; **Transponder** replies and syncs to the master. Uses Japan region, Channel 14, with 802.11b or Long Range (250k/500k) modes.

---

## Hardware

- **2× ESP32** (any variant with WiFi, e.g. ESP32-WROOM-32).
- **Wiring**
  - **ROLE_PIN (GPIO 13)**  
    - **GND** → **Master**  
    - **Leave floating or 3.3V** → **Transponder**
  - **LED** on **GPIO 2** (on-board LED on most dev boards).

---

## Software setup

### Option A: Arduino IDE 2.x

1. **Install ESP32 core**
   - File → Preferences → Additional Board Manager URLs:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Tools → Board → Boards Manager → search **“esp32”** → Install **“esp32 by Espressif Systems”**.

2. **Board**
   - Tools → Board → **ESP32 Arduino** → **ESP32 Dev Module** (or your exact board).

3. **Port**
   - Tools → Port → select the COM port for the plugged-in ESP32.

4. **Open the sketch**
   - Open `sketch_jan4b/sketch_jan4b.ino` (the folder name must stay `sketch_jan4b` so the .ino is in a folder of the same name).

5. **Upload**
   - Connect one ESP32, set role with ROLE_PIN, then Sketch → Upload. Repeat for the second ESP32 with the other role.

### Option B: PlatformIO (CLI / VS Code)

*Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).*

1. **Open project**
   - In VS Code: Open Folder → `sketch_jan4b` (the folder that contains `platformio.ini`).
   - Or in terminal: `cd sketch_jan4b` then `pio run` / `pio run -t upload`.

2. **Build**
   - `pio run`

3. **Upload**
   - `pio run -t upload` (set role via ROLE_PIN before uploading each device).

4. **Serial monitor**
   - `pio device monitor -b 115200`

---

## Testing and development

### 1. Assign roles

- **Master**: GPIO 13 **LOW** (e.g. wire to GND). Upload, then open Serial at **115200**.
- **Transponder**: GPIO 13 **HIGH** or floating. Upload.

### 2. Basic link test

1. Power both boards (USB or 3.3V).
2. Open Serial Monitor on the **Master** (115200 baud).
3. You should see periodic status and lines like:
   - `[HH:MM:SS] N:... | FWD Loss:... | BWD Loss:... | Sym:... | Z:...`
4. **LED on GPIO 2**: blinks on TX (and on transponder on RX).

If you see `[NO REPLY]` with **INTERFERENCE** or **RANGE LIMIT**, check distance, antennas, and power (see below).

### 3. Master serial commands (115200 baud)

| Key | Action |
|-----|--------|
| `l` | Cycle RF mode: STD (802.11b) → 250k → 500k (saves to NVS, restarts) |
| `p` + number | Set master TX power (dBm), e.g. `p14` |
| `t` + number | Set remote (transponder) target power (dBm), e.g. `t8` |
| `s` | Set remote target power = current master power |
| `v` | Toggle plot mode (CSV-style output) |
| `r` + number | Set ping interval (ms), e.g. `r500` |
| `h` | Print detailed status |
| `k` + number | Set time (HHMM), e.g. `k1430` = 14:30 |
| `z` | Reset calibration (zero reference) |
| `c` | Reset minute counters (interference/range stats) |

### 4. Transponder behavior

- Listens for master’s pings; replies with RSSI and power.
- If no packet for a while, it **cycles RF mode** (STD → 250k → 500k) to match the master.
- Syncs its time from the master.

### 5. Development tips

- **Single board**: Run one as Master (ROLE_PIN to GND) and use Serial to verify commands and status; link tests need a second board as Transponder.
- **RSSI / path loss**: Vary distance and obstacles; watch FWD Loss, BWD Loss, and Symmetry in Serial.
- **Plot mode** (`v`): Use for logging into a spreadsheet (e.g. fwd/bwd/symmetry/zeroed).
- **Power**: Master `p` and remote `t` affect link budget; lower power helps test sensitivity.

---

## Quick reference

- **RF**: Japan, **Channel 14**; 802.11b or LR 250k/500k.
- **Role**: GPIO 13 = GND → Master; floating/3.3V → Transponder.
- **Serial**: 115200, on Master for commands and output.

Once both boards are flashed and roles are set, power them and open the Master’s Serial Monitor to test and develop.
