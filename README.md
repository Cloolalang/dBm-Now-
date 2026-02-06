# ESP32 2.4 GHz RF Probe & Path Loss Analyzer (v3.3)

ESP-NOW based RF link tester using **two ESP32 devices**: a **Master** sends pings and measures path loss / RSSI; a **Transponder** replies and syncs to the master.

**Why this approach?** Can we use a **well-evolved comms protocol** to build **low-cost test equipment** and learn about the RF channel? Established methods (VNAs, spectrum analyzers) are accurate but expensive. The idea here: **reverse-engineering the protocol and hardware** (ESP-NOW, 802.11, commodity ESP32) is very cheap and yields a **novel test method** — RSSI, path loss, link behaviour at a fraction of the cost.

**Purpose:** This project uses the ESP-NOW RSSI measurement capability to provide a **low-cost RF tester** for:
- Characterising **ESP-NOW protocol performance** and link behaviour
- Comparing **ESP32 boards** with internal vs external antennas
- Basic **2.4 GHz propagation testing** (path loss, symmetry, interference)
- Analysing **passive components** (antennas, cables, connectors) without a VNA or tracking generator
- Testing **RF shielded enclosures** (e.g. put one unit inside, one outside; compare RSSI/path loss with and without the enclosure)

It can also help explore **2.4 GHz ISM band** channel conditions and **device interoperability** with WiFi, Bluetooth, drone controllers, and other 2.4 GHz systems.

**Comparison with a low-cost VNA (e.g. TinyVNA):** This project is a **link tester** (RSSI, path loss between two devices), not a VNA. The ESP32 probe gives **link performance** at low cost but no S11/S21 or swept response.

---

### Technical summary: ESP-NOW

**What it is:** ESP-NOW is a **connectionless, peer-to-peer Wi‑Fi protocol** defined by Espressif. Devices send and receive short frames **without joining an access point (AP)** or maintaining a TCP/IP stack. It is aimed at low-latency, low-overhead links (sensors, remotes, device-to-device control).

**Frame format:** User data is carried in **802.11 management frames**: vendor-specific **action frames** (category 127, OUI **0x18fe34**). The payload is opaque to standard Wi‑Fi gear; other stations see management/vendor traffic and may defer (CSMA/CA) but do not decode the content.

**Protocol versions:**
- **v1.0:** Max payload **250 bytes**. Only interoperates with other v1.0 devices (or v2.0 when packet ≤ 250 bytes).
- **v2.0:** Max payload **1470 bytes**. Can receive from both v1.0 and v2.0 senders.

**PHY and band:** Operates in the **2.4 GHz ISM band** on a single **20 MHz** channel (1–14). Standard mode uses **802.11 b/g/n** (DSSS for 11b, OFDM for 11g/n); default PHY rate is **1 Mbps**. Optional **Long Range (LR)** mode uses Espressif’s proprietary LR modulation (e.g. 250 or 500 kbps) for better sensitivity and range at lower throughput.

**On-air time (basic ESP-NOW packet):** At **1 Mbps** (standard mode), a typical frame is PLCP preamble (~120 µs for 802.11b short) + PLCP header + 802.11/ESP-NOW MAC and headers (~52 bytes) + payload + FCS. For a **25-byte payload**, total air time is about **0.5–0.6 ms** per packet; for a 250-byte payload, roughly **2–2.5 ms**. In **unicast**, the receiver sends an 802.11 ACK (~30 µs at 1 Mbps), so total transaction is packet + ACK. **Long Range 250 kbps** has longer symbol times; a 25-byte payload is on the order of **1–2 ms** on air (chip-dependent preamble/header).

**Peers and addressing:** Up to **20 peers** per device (e.g. 6 with encryption enabled by default in ESP-IDF). Frames can be sent **unicast** (to a peer MAC) or **broadcast** (e.g. FF:FF:FF:FF:FF:FF). **Unicast** typically uses **802.11 link-layer ACK** (receiver sends an ACK frame); **broadcast** has no link-layer ACK. Applications can add their own request/response or retries.

**802.11 retries (unicast):** When a unicast frame is not ACKed, the 802.11 MAC **retransmits** it: the sender waits **DIFS** (DCF inter-frame spacing), then a **random backoff** (slot time × random value in the contention window), and sends again. The contention window typically **increases** (e.g. doubles) up to a maximum on each retry to reduce collisions under load. The number of retries is **limited** (e.g. 4 or 7 attempts, implementation-dependent); after that the frame is discarded and the upper layer may be notified. **Broadcast** frames are not retried at the link layer.

**Security:** Optional **encryption** using **CCMP** with a Primary Master Key (PMK) and per-peer Local Master Keys (LMK). If disabled, all payloads are sent in the clear.

**Stack and API:** Implemented in **ESP-IDF** and exposed in the **ESP32 Arduino core**. The Wi‑Fi interface must be initialised (STA or AP) but does not need to be connected to an AP; ESP-NOW runs alongside Wi‑Fi and shares the same radio and channel.

**Coexistence:** Because the PHY is standard 802.11, ESP-NOW traffic is **visible to other Wi‑Fi devices** as channel activity (carrier sense). It does not use IP; it does not appear as normal Wi‑Fi data traffic to routers or phones. Suitable for dedicated links, test setups, or low-rate telemetry in the 2.4 GHz band.

---

The following describes **this firmware’s** usage of the stack: payload, link behaviour, and measurement details.

**This firmware:** Uses a **25-byte** payload (compatible with ESP-NOW v1.0 and v2.0). Default PHY rate **1 Mbps** (standard mode); **Long Range** 250 kbps and 500 kbps are supported. Encryption is **disabled** (`peerInfo.encrypt = false`).

**RSSI dynamic range:** The reportable RSSI range is roughly **-100 dBm to -30 dBm** (weaker to stronger); **-127** (or sometimes 0) means no signal / invalid. Receiver sensitivity (minimum usable signal) depends on mode: **Long Range 250 kbps** is best (often about -100 to -105 dBm), then **standard 1 Mbps** (about -98 dBm), then LR 500k, then higher 802.11 rates. Usable dynamic range is about **70–75 dB** (sensitivity to saturation). Exact values are chip/board dependent; strong signals can saturate near the upper end. **Expected measurement standard deviation:** In stable conditions (fixed geometry, little multipath), RSSI repeatability is often on the order of **1–3 dB** (σ). With movement, multipath, or changing environment, standard deviation can be **several dB** (e.g. 3–6 dB or more). Averaging over multiple pings or using metal enclosures and fixed antenna positions reduces observed variance.

**MAC / protocol (this firmware):** The link uses **ESP-NOW broadcast mode**. The master sends **one packet per ping** (no retries) to the broadcast address; the transponder sends **one packet in reply**. There is **no link-layer ACK** — if a packet is lost, the master reports *NO REPLY* and continues with the next ping. **Transmit timing** includes 0–49 ms random jitter on each ping interval to avoid syncing with WiFi beacons.

**Packet structure:** Both **ping** (Master → Transponder) and **pong** (Transponder → Master) use the same **Payload** struct (25 bytes). The over-the-air frame includes 802.11 and ESP-NOW headers before the payload.

| Field | Type | Ping (master sends) | Pong (transponder sends) |
|-------|------|---------------------|--------------------------|
| `nonce` | uint32_t | Sequence number (incremented each ping) | Echo of received nonce |
| `txPower` | float | Master TX power (dBm) | Transponder TX power (dBm) |
| `measuredRSSI` | float | Not used (0) | RSSI of the received ping (dBm) |
| `targetPower` | float | Desired transponder TX power (dBm) | Echo of received targetPower |
| `pingInterval` | uint32_t | Master ping interval (ms) | Echo of received pingInterval |
| `hour`, `minute`, `second` | uint8_t | Master RTC time | Echo of received time (transponder syncs its clock) |
| `channel` | uint8_t | RF channel 1–14: current channel, or **target channel** during a channel change (sent for 3 pings before master switches so transponder can switch first) | Transponder’s current RF channel |
| `rfMode` | uint8_t | 0=STD, 1=LR 250k, 2=LR 500k: current mode, or **target mode** during an RF mode change (sent for 3 pings before master switches so transponder can switch first) | Transponder’s current RF mode |

On **ping**, the master fills nonce, its TX power, target power for the transponder, ping interval, time, channel (current or pending target), and rfMode (current or pending target). On **pong**, the transponder echoes nonce, targetPower, pingInterval, and time; sets its own `txPower`, `channel`, and `rfMode`; and fills `measuredRSSI` with the RSSI of the ping it received (used for path loss and symmetry). All payloads are sent in the clear; do not use for sensitive data.

---

## Hardware

- **2x ESP32** (any variant with WiFi, e.g. ESP32-WROOM-32).
- **Wiring**
  - **ROLE_PIN (GPIO 13)**  
    - **GND** -> **Master**  
    - **Leave floating or 3.3V** -> **Transponder**
  - **LED** on **GPIO 2** (on-board LED on most dev boards).

**External antennas (u.fl cables):** If you use ESP32 dev boards with **u.fl cables and external antennas**, **do not connect the two devices directly** without at least **60 dB of attenuation** between them (e.g. attenuators or sufficient physical separation). Direct connection at full TX power can overload the receiver and damage hardware.

**Hardware considerations (thermal, power):**
- **Thermal:** Transmitting at high TX power and high rates (e.g. short ping intervals) increases chip temperature. The ESP32 uses built-in thermal management and can **automatically reduce TX power** at high temperature (e.g. around 80°C). For sustained high-power or high-rate testing, ensure adequate **ventilation** and avoid enclosing the board in a sealed box without cooling. Very short ping intervals (e.g. under 100 ms) with high power can heat the device over time; allow airflow or reduce power/rate for long runs.
- **Power supply:** RF performance can become unstable if the supply voltage is too low (e.g. below about 2.8 V). Use a stable **3.3 V** supply and avoid marginal USB cables or weak batteries when running at high power. A typical ESP32 dev board can draw **up to about 250-500 mA** when the WiFi radio is transmitting at high power (chip TX alone is often ~240 mA at max TX power; the board adds LEDs, regulator, and other loads). Ensure your USB port, cable, or external supply can deliver at least **500 mA** for reliable operation at high TX power.
- **Receiver sensitivity and measurement repeatability:** For better RSSI/path-loss repeatability and less influence from the environment, house the dev boards in **metal enclosures** and use **U.FL to SMA adapter cables** so antennas (or attenuators) are outside the enclosure. This reduces coupling to nearby objects and improves consistency when comparing runs or boards.

---

## Software setup (Arduino IDE)

1. **Install ESP32 core**
   - File -> Preferences -> Additional Board Manager URLs:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Tools -> Board -> Boards Manager -> search **"esp32"** -> Install **"esp32 by Espressif Systems"**.

2. **Board**
   - Tools -> Board -> **ESP32 Arduino** -> **ESP32 Dev Module** (or your exact board).

3. **Port**
   - Tools -> Port -> select the COM port for the plugged-in ESP32.

4. **Open the sketch**
   - Open `sketch_jan4b/sketch_jan4b.ino` (the folder name must stay `sketch_jan4b` so the .ino is in a folder of the same name).

5. **Upload**
   - **The same sketch is uploaded to both devices.** Master vs Transponder is determined by hardware (ROLE_PIN): connect one ESP32, set GPIO 13 for the role you want, then Sketch -> Upload. Repeat for the second ESP32 with the other role.

---

## Dependencies

This project does **not** use any third-party libraries. All code relies on the **ESP32 Arduino core** (by Espressif) and the C/POSIX toolchain it provides.

| Include / feature | Provided by | Purpose |
|------------------|-------------|---------|
| `esp_now.h` | ESP32 Arduino (esp_wifi) | ESP-NOW init, send, recv, peers |
| `WiFi.h` | ESP32 Arduino | `WiFi.mode(WIFI_STA)` |
| `esp_wifi.h` | ESP32 Arduino (esp_wifi) | Channel, protocol, TX power, promiscuous mode |
| `Preferences.h` | ESP32 Arduino | NVS: save RF mode and channel across reboots |
| `FS.h` | ESP32 Arduino | Filesystem abstraction |
| `SPIFFS.h` | ESP32 Arduino | SPIFFS for CSV log file (`/log.csv`) |
| `time.h` / `sys/time.h` | Toolchain (C/POSIX) | RTC/time for timestamps |

**Tested with:** Arduino core for **ESP32 by Espressif** **3.x** (ESP-IDF v5.5), on **ESP32 Dev Module** (ESP32-WROOM-32). Other 3.x versions with ESP-IDF v5.x should be compatible; if you use a different core or IDF version, API changes (e.g. promiscuous callback signature) may require small code updates. Check *Tools → Board → Boards Manager* for your installed core version.

---

## Testing and development

### 1. Assign roles

- **Master**: GPIO 13 **LOW** (e.g. wire to GND). Upload, then open Serial at **115200**.
- **Transponder**: GPIO 13 **HIGH** or floating. Upload.

### 2. Basic link test

1. Power both boards (USB or 3.3V).
2. Open Serial Monitor on the **Master** (115200 baud).
3. You should see periodic status and incoming pong lines like:
   - `[HH:MM:SS] N:... | TX aa:bb:cc:dd:ee:ff | FWD Loss:... | BWD Loss:... | Sym:... | Z:...`
   - **TX** is the MAC address of the replying transponder (so you can identify which unit replied). Press **`h`** for full status; the status table shows each device’s **MAC address** and **ESP-NOW mode** (master: TX broadcast, RX unicast; transponder: RX broadcast, TX unicast) for both boards.
4. **LED on GPIO 2**: blinks on TX (and on transponder on RX).

If you see `[NO REPLY]` with **INTERFERENCE** (last RSSI was high → likely collision) or **SIGNAL TOO LOW** (last RSSI was low → likely range), check distance, antennas, and power (see below).

**Serial output format:**
- **Master** (incoming pongs): `[HH:MM:SS] N:... | TX aa:bb:cc:dd:ee:ff | FWD Loss:... | BWD Loss:... | Sym:... | Z:...` — **TX** is the transponder’s MAC address.
- **Transponder** (incoming pings): `[HH:MM:SS] RX N=... | Mstr aa:bb:cc:dd:ee:ff | mode | RSSI:... dBm | Mstr Pwr:... | Path Loss:... | TX Pwr:...` — **Mstr** is the master’s MAC; **TX Pwr** is the transponder’s transmit power (set by master).
- **Status** (`h` on either device): Shows RF protocol, channel, **MAC address**, **ESP-NOW mode** (TX/RX broadcast vs unicast), and role-specific settings.

### 3. Master serial commands (115200 baud)

| Key | Action |
|-----|--------|
| `l` | Cycle RF mode: STD (802.11) -> 250k -> 500k; master sends new mode in payload for 3 pings before switching (transponder switches first, no restart) |
| `p` + number | Set master TX power (dBm), e.g. `p14` |
| `t` + number | Set remote (transponder) target power (dBm), e.g. `t8` |
| `s` | Set remote target power = current master power |
| `v` | Toggle plot mode (CSV-style output: channel, fwdLoss, bwdLoss, symmetry, zeroed) |
| `r` + number | Set ping interval (ms), e.g. `r500` |
| `h` | Print detailed status |
| `k` + number | Set time (HHMM), e.g. `k1430` = 14:30 |
| `z` | Zero cal: set reference from last RSSI so **Z** = delta from that point (or on next pong if none yet) |
| `c` | Reset minute counters (interference / signal-too-low stats) |
| `f` | Toggle CSV file logging to SPIFFS (`/log.csv`) |
| `d` | Dump log file to Serial (copy to save on PC) |
| `e` | Erase log file for fresh start |
| `m` + number | Set max recording time in seconds (0 = no limit), e.g. `m300` = 5 min; logging auto-stops when limit reached |
| `n` + number | Set RF channel 1-14, e.g. `n6`; master sends new channel in payload for 3 pings before switching so transponder can switch first (quicker link re-establishment); saved to NVS |
| `P` | **Start promiscuous scan** (master only): sweep channels 1–14 repeatedly; report packet count, avg/min RSSI, busy % per channel |
| `E` or `e` | **Exit** promiscuous scan; resumes ESP-NOW |

**Promiscuous test mode (master only):** Send **`P`** (uppercase) to stop ESP-NOW and enter WiFi promiscuous mode. The master sweeps channels 1–14, dwelling ~2.1 s per channel, and prints one line per channel: **Ch \| Pkts \| AvgRSSI \| MinRSSI \| Busy%**. It keeps scanning (1→14, then 1→14 again) until you send **`E`** or **`e`** to exit; then ESP-NOW is restored. Use this to see which channels are busy and typical signal levels. Promiscuous mode and ESP-NOW cannot run at the same time, so the link is paused during the scan.

**RF channel:** Both devices **boot on channel 1** and on **ESP-NOW standard rate** (802.11, not Long Range) so they sync quickly. On the **master**, use **`n`** + channel number (e.g. **`n6`**) to set the 2.4 GHz WiFi channel (1-14). The master **sends the new channel in the payload for 3 pings** while still on the current channel; the **transponder** receives those pings and switches to the new channel. Then the master switches. **RF mode** works the same way: use **`l`** to cycle STD → LR 250k → LR 500k; the master sends the new **rfMode** in the payload for 3 pings before switching, so the transponder switches first and the link re-establishes quickly (no restart). The channel is saved to NVS when set; the RF mode is saved to NVS when switched. The transponder also **hunts** (cycles channels 1-14 and RF modes) when it gets **no packet for several consecutive timeouts** (default 3 x timeout). **For Long Range modes, keep the master sending rate (ping interval, `r`) at least 25 ms.** Use this for channel-specific propagation tests or to avoid busy channels.

**Maximum range (shoot-out mode):** For the longest radio range, set **Long Range 250 kbps** (**`l`** until you see 250k), **max TX power** on both master (**`p`** + 20 or highest) and transponder (**`t`** + 20 or highest via master target), and a **ping interval of at least 25 ms** (**`r`** + value). **Theoretical open-ground range with panel antennas:** Using a simple free-space link budget (e.g. 20 dBm TX, LR 250k receiver sensitivity on the order of −100 dBm, 10–12 dBi gain per side, 15 dB margin for fading/terrain), **theoretical line-of-sight range is on the order of 20–30 km** with directional panels pointed at each other. Real-world open ground will be less due to multipath, ground reflection, and obstacles; actual range depends on antenna gain, feed loss, and local conditions.

**CSV file logging (master):** Press **`f`** to start logging each pong to a `.csv` file on the ESP32 flash (SPIFFS, path `/log.csv`). Columns: timestamp, nonce, fwdLoss, bwdLoss, symmetry, zeroed, masterRSSI, remoteRSSI. Press **`f`** again to stop. Use **`d`** to print the file to Serial so you can copy it to a file on your PC. Use **`e`** to delete the log file. Use **`m`** + seconds (e.g. **`m300`** = 5 min) to set a max recording time; logging auto-stops when the limit is reached (0 = no limit). Requires a partition with SPIFFS (default 4MB partition usually has it).

**CSV file logging (transponder) -- one-way link testing:** On the **transponder**, connect Serial and press **`f`** to start logging each **received ping** (master->transponder) to `/log.csv`. Columns: timestamp, nonce, rfMode, rssi, masterPwr, pathLoss, transponderPwr. Optionally set **`m`** + seconds (e.g. **`m300`**) so logging auto-stops after that time (0 = no limit). Then **walk away with the master**; even when the master gets NO REPLY (one-way link), the transponder keeps logging every packet it receives until you stop or the max time is reached. When you return, connect Serial to the transponder and press **`d`** to dump the log, then copy to your PC. Press **`f`** again to stop logging, or **`e`** to erase the file. Transponder commands: **`f`** toggle log, **`d`** dump, **`e`** erase, **`m`** max time (seconds), **`h`** status.

### 4. Transponder behavior

- Listens for master's pings; replies with RSSI and power.
- **Serial RX lines** show each received ping: timestamp, nonce, **master MAC** (Mstr aa:bb:cc:dd:ee:ff), mode, RSSI, Mstr Pwr, Path Loss, **TX Pwr** (transponder transmit power set by master). So you can identify which master sent the ping and see transponder power.
- **Follows master's RF channel** (from payload); sets its channel to match.
- If no packet for a while, it **cycles RF mode** (STD -> 250k -> 500k) to match the master.
- Syncs its time from the master.

### 5. Development tips

- **Single board**: Run one as Master (ROLE_PIN to GND) and use Serial to verify commands and status; link tests need a second board as Transponder.
- **RSSI / path loss**: Vary distance and obstacles; watch FWD Loss, BWD Loss, and Symmetry in Serial.
- **Plot mode** (`v`): Use for logging into a spreadsheet. Each line is: **channel**, fwdLoss, bwdLoss, symmetry, zeroed (comma-separated).
- **Power**: Master `p` and remote `t` affect link budget; lower power helps test sensitivity.

### 6. ESP32 optimisations

The sketch applies a few RF/radio settings for consistent behaviour:

- **Second channel (HT40) off** -- The channel is set with `WIFI_SECOND_CHAN_NONE`, so the radio uses 20 MHz only (no 40 MHz wide channel). This avoids extra complexity and keeps operation compatible with all ESP-NOW modes.
- **WiFi modem sleep off** -- `esp_wifi_set_ps(WIFI_PS_NONE)` is called in `setup()`. The WiFi modem stays awake so ping/pong latency is consistent and round-trip times are predictable. For **battery operation** you can change this to `WIFI_PS_MIN_MODEM` (or `WIFI_PS_MAX_MODEM`) in the sketch to allow modem sleep when idle; expect higher and more variable latency.
- **WiFi only** -- ESP-NOW needs the WiFi radio; the sketch uses `WiFi.mode(WIFI_STA)` and does not connect to an AP. Bluetooth is not used; if your board supports it, you can disable Bluetooth in the Arduino IDE board options (e.g. "Bluetooth" -> Disabled) to free RAM and avoid any radio scheduling overlap.

---

## Quick reference

- **RF**: STD (802.11 b/g/n) or Long Range 250k/500k; channel 1-14. Both boot on **channel 1** and **standard rate** for quick sync; set channel with **`n`**, switch to LR with **`l`** (session only; reboot = STD again).
- **Role**: GPIO 13 = GND -> Master; floating/3.3V -> Transponder.
- **Serial**: 115200, on Master for commands and output. Incoming lines show the other device’s **MAC address** (master: TX = transponder MAC; transponder: Mstr = master MAC). Press **`h`** for full status (MAC, ESP-NOW mode, etc.).

Once both boards are flashed and roles are set, power them and open the Master's Serial Monitor to test and develop.

---

## Links

- **[ESP-NOW source (Espressif)](https://github.com/espressif/esp-now)** — Official ESP-NOW component and examples.
- **[ESP-NOW API (ESP-IDF)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)** — ESP-NOW API reference in the ESP-IDF programming guide.
- **[ESP-IDF Wi-Fi / espnow example](https://github.com/espressif/esp-idf/tree/master/examples/wifi/espnow)** — Example usage of ESP-NOW in ESP-IDF.

---

## Planned features

- **TRX relay controller** -- Antenna switching (e.g. for T/R or diversity setups).
- **RF measurement calibration** -- Calibration for different ESP-NOW modes (STD, LR 250k, LR 500k) for more accurate dBm/path-loss readings.
- **RF characterisation** -- Characterise the RF behaviour of the device (e.g. TX power vs setting, RSSI linearity) as a reference for calibration.
- **Unicast mode with 802.11 PHY statistics** -- Unicast operation with PHY stats (retries, etc.) for link analysis; support for operating multiple transponders.
- **RF test examples** -- Examples described in the README (e.g. path loss, symmetry, channel sweep), including a one-port test using a directional coupler.
- **Save Promiscuous mode results to .csv** -- Option to save promiscuous scan measurements (channel, packet count, avg/min RSSI, busy %) to a .csv file.
- **Android-based master controller** -- Master UI/control from an Android device via OTG cable.
- **RF overload detection** -- Detect when the receiver is overloaded (e.g. antennas too close or insufficient attenuation) and warn or protect the link.
- **Configurable PHY rate for standard mode** -- Option to set ESP-NOW PHY rate above the default 1 Mbps (e.g. 24M, 54M, or 802.11n MCS rates) for higher throughput at short range, with tradeoff of range/reliability.
- **Hardware design with rechargeable battery** -- Reference or suggested hardware design for a battery-powered unit with rechargeable battery and charging circuitry.
- **SD card** -- Hardware and firmware support for recording results to SD card (e.g. in addition to or instead of SPIFFS).

---

## Legal and compliance

- **Development:** This project was developed using [Cursor](https://cursor.com/) (AI-assisted IDE).
- **Dependencies:** This firmware uses the [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) (and thus Espressif SDKs). Those projects have their own licenses; ensure your use and distribution comply with GPL v2 and with their terms.
- **Trademarks:** ESP32, Arduino, and related names are trademarks of their respective owners. This project is not affiliated with or endorsed by them.
- **RF use:** Use of this firmware in devices that transmit in the 2.4 GHz band may be subject to local regulations (e.g. FCC, CE, radio licensing). You are responsible for compliance in your jurisdiction. Please take care not to cause RF interference to other users of the band.
- **Duty cycle / channel utilisation:** In some regions (e.g. EU under ETSI EN 300 328), 2.4 GHz devices that do not use Listen-Before-Talk or similar adaptivity may be subject to **duty cycle or medium-utilisation limits** (e.g. 10% non-adaptive utilisation in EU). Choose **ping interval** (`r`) and **TX power** (`p`, `t`) with these limits in mind; longer intervals and lower power help stay within typical requirements.
- **Radio amateurs:** In many countries, **licensed radio amateurs** have higher power privileges on 2.4 GHz (e.g. under FCC Part 97 in the US) when operating in the amateur service with callsign and local rules. The ESP32 itself is limited to about **20 dBm (100 mW)**; to run at higher power you must use an **external power amplifier** and comply with your national amateur regulations (identification, band plan, minimum power necessary, etc.).

---

## License

**dBm-Now** is free software; you can redistribute it and/or modify it under the terms of the **GNU General Public License version 2** (or, at your option, any later version). See the [LICENSE](LICENSE) file in this repository for the full text.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
