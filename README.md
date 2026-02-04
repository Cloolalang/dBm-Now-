# ESP32 RF Probe & Path Loss Analyzer (v4.21)

ESP-NOW based RF link tester: **Master** sends pings and measures path loss / RSSI; **Transponder** replies and syncs to the master. STD mode uses 802.11 (b/g/n); optional Long Range 250k/500k.

**Protocol:** The link uses **ESP-NOW broadcast mode**. The master sends **one packet per ping** (no retries) to the broadcast address; the transponder sends **one packet in reply**. There is **no link-layer ACK** — if a packet is lost, the master reports *NO REPLY* and continues with the next ping.

---

## Hardware

- **2× ESP32** (any variant with WiFi, e.g. ESP32-WROOM-32).
- **Wiring**
  - **ROLE_PIN (GPIO 13)**  
    - **GND** → **Master**  
    - **Leave floating or 3.3V** → **Transponder**
  - **LED** on **GPIO 2** (on-board LED on most dev boards).

---

## Software setup (Arduino IDE)

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
   - **The same sketch is uploaded to both devices.** Master vs Transponder is determined by hardware (ROLE_PIN): connect one ESP32, set GPIO 13 for the role you want, then Sketch → Upload. Repeat for the second ESP32 with the other role.

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
| `l` | Cycle RF mode: STD (802.11) → 250k → 500k (saves to NVS, restarts) |
| `p` + number | Set master TX power (dBm), e.g. `p14` |
| `t` + number | Set remote (transponder) target power (dBm), e.g. `t8` |
| `s` | Set remote target power = current master power |
| `v` | Toggle plot mode (CSV-style output) |
| `r` + number | Set ping interval (ms), e.g. `r500` |
| `h` | Print detailed status |
| `k` + number | Set time (HHMM), e.g. `k1430` = 14:30 |
| `z` | Reset calibration (zero reference) |
| `c` | Reset minute counters (interference/range stats) |
| `f` | Toggle CSV file logging to SPIFFS (`/log.csv`) |
| `d` | Dump log file to Serial (copy to save on PC) |
| `e` | Erase log file for fresh start |
| `m` + number | Set max recording time in seconds (0 = no limit), e.g. `m300` = 5 min; logging auto-stops when limit reached |

**CSV file logging (master):** Press **`f`** to start logging each pong to a `.csv` file on the ESP32 flash (SPIFFS, path `/log.csv`). Columns: timestamp, nonce, fwdLoss, bwdLoss, symmetry, zeroed, masterRSSI, remoteRSSI. Press **`f`** again to stop. Use **`d`** to print the file to Serial so you can copy it to a file on your PC. Use **`e`** to delete the log file. Use **`m`** + seconds (e.g. **`m300`** = 5 min) to set a max recording time; logging auto-stops when the limit is reached (0 = no limit). Requires a partition with SPIFFS (default 4MB partition usually has it).

**CSV file logging (transponder) — one-way link testing:** On the **transponder**, connect Serial and press **`f`** to start logging each **received ping** (master→transponder) to `/log.csv`. Columns: timestamp, nonce, rfMode, rssi, masterPwr, pathLoss, transponderPwr. Optionally set **`m`** + seconds (e.g. **`m300`**) so logging auto-stops after that time (0 = no limit). Then **walk away with the master**; even when the master gets NO REPLY (one-way link), the transponder keeps logging every packet it receives until you stop or the max time is reached. When you return, connect Serial to the transponder and press **`d`** to dump the log, then copy to your PC. Press **`f`** again to stop logging, or **`e`** to erase the file. Transponder commands: **`f`** toggle log, **`d`** dump, **`e`** erase, **`m`** max time (seconds), **`h`** status.

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

- **RF**: STD (802.11 b/g/n) or Long Range 250k/500k; channel auto.
- **Role**: GPIO 13 = GND → Master; floating/3.3V → Transponder.
- **Serial**: 115200, on Master for commands and output.

Once both boards are flashed and roles are set, power them and open the Master’s Serial Monitor to test and develop.
