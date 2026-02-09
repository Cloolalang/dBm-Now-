# ESP32 2.4 GHz RF Probe & Path Loss Analyzer (v5.5)

## Table of contents

- [Overview](#overview) — purpose, ESP-NOW summary, firmware summary
- [Hardware](#hardware) — wiring, antennas, thermal/power
- [Software setup](#software-setup-arduino-ide) — Arduino IDE, board, upload
- [Dependencies](#dependencies) — ESP32 core, Bridge libraries
- [Quick start](#quick-start) — get a link in a few steps
- [Usage and testing](#usage-and-testing) — roles, link test, Serial commands, 1-way RF, CSV, tips
- [Quick reference](#quick-reference) — GPIO, RF, Serial, build
- [Default settings on first run](#default-settings-on-first-run) — Master, Transponder, Bridge
- [Links](#links) — Android apps, ESP-NOW docs
- [Planned features](#planned-features)
- [Legal and compliance](#legal-and-compliance)
- [License](#license)

---

## Overview

ESP-NOW based RF link tester using **two ESP32 devices**: a **Master** sends pings and measures path loss / RSSI; a **Transponder** replies and syncs to the master. The **same firmware** can also run as a **Serial–MQTT bridge** (pull **GPIO 12 to GND** at boot): WiFi Manager, Serial1 RX @ 9600 → MQTT, for 1-way transponder data to the cloud.

**Purpose:** Low-cost RF tester for: ESP-NOW/link characterisation; comparing boards (internal vs external antennas); 2.4 GHz propagation (path loss, symmetry, interference); passive components (antennas, cables) without a VNA; RF enclosures; 2.4 GHz channel conditions and coexistence with WiFi/Bluetooth. **Not a VNA** — link performance (RSSI, path loss) only, no S11/S21.

### Technical summary: ESP-NOW

**What it is:** ESP-NOW is a **connectionless, peer-to-peer Wi‑Fi protocol** defined by Espressif. Devices send and receive short frames **without joining an access point (AP)** or maintaining a TCP/IP stack. It is aimed at low-latency, low-overhead links (sensors, remotes, device-to-device control).

**Frame format:** User data is carried in **802.11 management frames**: vendor-specific **action frames** (category 127, OUI **0x18fe34**). The payload is opaque to standard Wi‑Fi gear; other stations see management/vendor traffic and may defer (CSMA/CA) but do not decode the content. Every 802.11 frame includes a **FCS (Frame Check Sequence)** at the end; the receiver validates the FCS and **discards** the frame if it fails (no payload is passed up). In **unicast**, a failed FCS means no ACK is sent, so the sender retries. ESP-NOW does not add its own FEC or ARQ — error handling is detection (FCS) plus 802.11 retransmission for unicast only. **On the ESP32**, the receive path exposes FCS-related info in **`wifi_pkt_rx_ctrl_t`** (the `rx_ctrl` in the ESP-NOW receive callback and in promiscuous RX): **`sig_len`** is the packet length including the FCS (4 bytes); **`rx_state`** is 0 when the frame passed (no error, FCS OK) and non-zero on error (error codes are not public, but typically include CRC/FCS failure). So you can infer FCS pass/fail from `rx_state` in application code.

**Protocol versions:**

| Version | Max payload | Interop |
|---------|-------------|---------|
| v1.0 | 250 bytes | v1.0 only (or v2.0 when packet ≤ 250 bytes) |
| v2.0 | 1470 bytes | Receives from v1.0 and v2.0 |

**PHY and band:** Operates in the **2.4 GHz ISM band** on a single **20 MHz** channel (1–14). Standard mode uses **802.11 b/g/n** (DSSS for 11b, OFDM for 11g/n); default PHY rate is **1 Mbps**. Optional **Long Range (LR)** mode uses Espressif’s proprietary LR modulation (e.g. 250 or 500 kbps) for better sensitivity and range at lower throughput.

**On-air time (basic ESP-NOW packet):** At **1 Mbps** (standard mode), a typical frame is PLCP preamble (~120 µs for 802.11b short) + PLCP header + 802.11/ESP-NOW MAC and headers (~52 bytes) + payload + FCS. For a **25-byte payload**, total air time is about **0.5–0.6 ms** per packet; for a 250-byte payload, roughly **2–2.5 ms**. In **unicast**, the receiver sends an 802.11 ACK (~30 µs at 1 Mbps), so total transaction is packet + ACK. **Long Range 250 kbps** has longer symbol times; a 25-byte payload is on the order of **1–2 ms** on air (chip-dependent preamble/header).

**Peers and addressing:** Up to **20 peers** per device (e.g. 6 with encryption enabled by default in ESP-IDF). Frames can be sent **unicast** (to a peer MAC) or **broadcast** (e.g. FF:FF:FF:FF:FF:FF). **Unicast** typically uses **802.11 link-layer ACK** (receiver sends an ACK frame); **broadcast** has no link-layer ACK. Applications can add their own request/response or retries.

**802.11 retries (unicast):** When a unicast frame is not ACKed, the 802.11 MAC **retransmits** it: the sender waits **DIFS** (DCF inter-frame spacing), then a **random backoff** (slot time × random value in the contention window), and sends again. The contention window typically **increases** (e.g. doubles) up to a maximum on each retry to reduce collisions under load. The number of retries is **limited** (e.g. 4 or 7 attempts, implementation-dependent); after that the frame is discarded and the upper layer may be notified. **Broadcast** frames are not retried at the link layer.

**Security:** Optional **encryption** using **CCMP** with a Primary Master Key (PMK) and per-peer Local Master Keys (LMK). If disabled, all payloads are sent in the clear.

**Stack and API:** Implemented in **ESP-IDF** and exposed in the **ESP32 Arduino core**. The Wi‑Fi interface must be initialised (STA or AP) but does not need to be connected to an AP; ESP-NOW runs alongside Wi‑Fi and shares the same radio and channel.

**Coexistence:** Because the PHY is standard 802.11, ESP-NOW traffic is **visible to other Wi‑Fi devices** as channel activity (carrier sense). It does not use IP; it does not appear as normal Wi‑Fi data traffic to routers or phones. Suitable for dedicated links, test setups, or low-rate telemetry in the 2.4 GHz band.

---

### This firmware

Uses a **27-byte** payload (compatible with ESP-NOW v1.0 and v2.0; accepts 25–26 byte payloads from older firmware). Default PHY rate **1 Mbps** (standard mode); **Long Range** 250 kbps and 500 kbps are supported. Encryption is **disabled** (`peerInfo.encrypt = false`).

**RSSI dynamic range:** The reportable RSSI range is roughly **-100 dBm to -30 dBm** (weaker to stronger); **-127** (or sometimes 0) means no signal / invalid. Receiver sensitivity (minimum usable signal) depends on mode: **Long Range 250 kbps** is best (often about -100 to -105 dBm), then **standard 1 Mbps** (about -98 dBm), then LR 500k, then higher 802.11 rates. Usable dynamic range is about **70–75 dB** (sensitivity to saturation). Exact values are chip/board dependent; strong signals can saturate near the upper end. **Initial test findings (ESP32-WROOM):** Measured dynamic range **-15 dBm to -95 dBm** (lowest TX power, all modes STD/LR 250k/LR 500k, ping rate 50 ms). **Expected measurement standard deviation:** In stable conditions (fixed geometry, little multipath), RSSI repeatability is often on the order of **1–3 dB** (σ). With movement, multipath, or changing environment, standard deviation can be **several dB** (e.g. 3–6 dB or more). Averaging over multiple pings or using metal enclosures and fixed antenna positions reduces observed variance.

**Improving low RSSI measurement (measuring below about -95 dBm):** If the lowest readings you see are around -95 dBm, the following can help reach lower levels. (1) **Use Long Range 250 kbps** (`l` until 250k): LR 250k has better receiver sensitivity (~-100 to -105 dBm) than standard mode (~-98 dBm), typically gaining several dB. (2) **Quiet channel:** Use promiscuous scan (`P`) to pick a channel with low occupancy; interference raises the effective noise floor and limits how low you can measure. (3) **Reduce TX power on the far end:** To measure low RSSI at a given receiver, lower the TX power of the device that is transmitting *to* that receiver (or increase distance/attenuation) so the receiver still decodes the packet but at a weaker level — e.g. to see low RSSI at the master (pong path), reduce transponder target power (`t`); to see low RSSI at the transponder (ping path), reduce master power (`p`). (4) **Environment:** Metal enclosures and fixed antenna positions reduce multipath and variance so you can run closer to the sensitivity limit without the link dropping. (5) **Ping interval:** At very low SNR, a slightly longer interval (e.g. 50–100 ms) can sometimes help the receiver settle. The absolute floor is set by receiver sensitivity (LR 250k best) and by the WiFi stack’s reported RSSI range (chip-dependent).

**MAC / protocol (this firmware):** The link uses **ESP-NOW broadcast mode**. The master sends **one packet per ping** (no retries) to the broadcast address; the transponder sends **one packet in reply**. There is **no link-layer ACK** — if a packet is lost, the master reports *NO REPLY* and continues with the next ping. **Transmit timing** includes 1–17 ms prime jitter (values 1, 2, 3, 5, 7, 11, 13, 17 ms chosen at random) on each ping interval to avoid periodic alignment with WiFi beacons (e.g. 100 ms TU).

**Packet structure:** Both **ping** (Master → Transponder) and **pong** (Transponder → Master) use the same **Payload** struct (27 bytes; 26-byte payloads from older firmware are accepted). The over-the-air frame includes 802.11 and ESP-NOW headers before the payload.

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
| `missedCount` | uint8_t | 0 (not used) | Number of packets the transponder missed before this nonce (gap in sequence); 0 = none |
| `oneWayRF` | uint8_t | 0=normal, 1=request transponder **1-way RF** (reply via JSON on Serial only; no pong) | 0 (not used in pong) |
| `zeroed`, `symmetry`, `pathLossSD` | float | Master: Z (delta from ref RSSI), fwd−bwd symmetry, path loss SD (last 10); for 1-way JSON. In 1-way mode sent as 0. | Transponder echoes in 1-way JSON only |

On **ping**, the master fills nonce, its TX power, target power for the transponder, ping interval, time, channel (current or pending target), rfMode (current or pending target), and **oneWayRF** (1 when master has requested 1-way RF). On **pong**, the transponder echoes nonce, targetPower, pingInterval, and time; sets its own `txPower`, `channel`, and `rfMode`; fills `measuredRSSI` with the RSSI of the ping it received (used for path loss and symmetry); and sets **`missedCount`** when the received nonce is not consecutive with the previous one (e.g. received 7 after 5 → missed 6, so missedCount = 1). The transponder logs **Missed packet(s): nonce(s) X–Y** on Serial when it detects a gap (not in 1-way RF mode); the master prints **Transponder missed N packet(s) (nonce(s) X–Y)** when the pong reports missedCount > 0. All payloads are sent in the clear; do not use for sensitive data.

---

## Hardware

**2x ESP32** (any variant with WiFi, e.g. ESP32-WROOM-32).

| Pin | State | Mode / role |
|-----|-------|-------------|
| **GPIO 12** (BRIDGE_PIN) | GND | Serial–MQTT Bridge (WiFi Manager, Serial1 RX → MQTT) |
| **GPIO 12** | Floating / 3.3V | Master or Transponder (see GPIO 13) |
| **GPIO 13** (ROLE_PIN) | GND | **Master** (only when not Bridge) |
| **GPIO 13** | Floating / 3.3V | **Transponder** |
| **GPIO 2** | — | LED (on-board on most dev boards) |

**External antennas (u.fl cables):** If you use ESP32 dev boards with **u.fl cables and external antennas**, **do not connect the two devices directly** without at least **60 dB of attenuation** between them (e.g. attenuators or sufficient physical separation). Direct connection at full TX power can overload the receiver and damage hardware.

**U.FL port — do not leave open:** The RF output is designed to drive a **50 Ω load** (antenna or dummy load). If **nothing is connected** to the U.FL connector (no antenna, no cable, no load), the port is effectively **open**. Reflected power and high VSWR can **damage the power amplifier (PA)**. Always connect an **antenna** or a **50 Ω load/attenuator** to the U.FL before transmitting at significant power. Boards that offer both U.FL and PCB antenna should have the RF path connected to one or the other (e.g. 0 Ω resistor or switch); do not leave the active RF output open.

**Hardware considerations:**

| Topic | Notes |
|-------|--------|
| **Thermal** | High TX power and short ping intervals heat the chip. ESP32 can auto-reduce TX power (~80°C). Ensure ventilation; thermal throttling is reported on Serial and in status (`h`). Chip temp on pong lines and in master status when supported. |
| **Power supply** | Stable 3.3 V; avoid weak USB/batteries. Board can draw ~250–500 mA at high TX. Supply at least **500 mA** for reliable operation. |
| **Repeatability** | Metal enclosures and U.FL→SMA cables (antennas outside) improve RSSI/path-loss consistency. |

---

## Software setup (Arduino IDE)

| Step | Action |
|------|--------|
| 1. **Board Manager URL** | File → Preferences → Additional Board Manager URLs: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` |
| 2. **Install ESP32 core** | Tools → Board → Boards Manager → search **"esp32"** → Install **"esp32 by Espressif Systems"** |
| 3. **Board** | Tools → Board → **ESP32 Arduino** → **ESP32 Dev Module** (or your board) |
| 4. **Port** | Tools → Port → select COM port for the plugged-in ESP32 |
| 5. **Open sketch** | Open `dBmNow/dBmNow.ino` (folder must be `dBmNow`; contains `bridge_mode.ino` — both compile) |
| 6. **Upload** | Same sketch for all devices. Set GPIO 12/13 for role after upload (see [Quick start](#quick-start)). |

---

## Dependencies

| Mode | Libraries |
|------|-----------|
| **Master / Transponder** | ESP32 Arduino core only (no extra libraries) |
| **Bridge** (GPIO 12 = GND) | **WiFiManager** (tzapu), **PubSubClient** (knolleary) — Sketch → Include Library → Manage Libraries |

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

**RSSI and core version:** Per-packet RSSI in the ESP-NOW receive callback is only available with **Arduino-ESP32 3.x** (ESP-IDF 5.x). With **Arduino-ESP32 2.x** (ESP-IDF 4.x) the callback uses the older API and does **not** provide RSSI; RSSI and path loss will show as **-127** (invalid). Use **ESP32 core 3.x** (or a 2.x build that exposes `esp_now_recv_info_t`) for correct RSSI and path loss.

**Master on Android:** The master has been tested with an Android phone and USB OTG cable at max TX power using **[Serial USB Terminal](https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal) 1.57** for Serial commands and monitoring; it works well.

---

## Quick start

| Step | Action |
|------|--------|
| 1 | **Upload** the sketch to both ESP32s (same sketch for all) |
| 2 | **Set roles:** GPIO 12 floating. GPIO 13 → GND = **Master**, GPIO 13 floating = **Transponder** |
| 3 | **Power** both boards; open Serial Monitor on the **Master** (115200 baud or `SERIAL_BAUD`) |
| 4 | You should see pong lines: `[HH:MM:SS] N:... | TX aa:bb:cc:... | FWD Loss:... | BWD Loss:... | Sym:... | Z:... | plSD:...` |

**Bridge:** GPIO 12 → GND at boot. **1-way RF** (JSON on Serial, no pong): press **`W`** — see [1-way RF mode](#5-1-way-rf-mode).

---

## Usage and testing

### 1. Assign roles

| Role | GPIO 12 | GPIO 13 | Notes |
|------|---------|---------|--------|
| **Bridge** (Serial–MQTT) | GND | — | First run → AP **SerialMQTTBridge** at 192.168.4.1. WiFiManager + PubSubClient. |
| **Master** | Floating | GND | Open Serial on Master (115200 or `SERIAL_BAUD`). |
| **Transponder** | Floating | Floating / 3.3V | No Serial needed for link; use for 1-way JSON or commands. |

### 2. Basic link test

1. Power both boards (USB or 3.3V).
2. Open Serial Monitor on the **Master** (115200 baud).
3. You should see periodic status and incoming pong lines like:
   - `[HH:MM:SS] N:... | TX aa:bb:cc:dd:ee:ff | FWD Loss:... | BWD Loss:... | Sym:... | Z:...`
   - **TX** is the MAC address of the replying transponder (so you can identify which unit replied). Press **`h`** for full status; the status table shows each device’s **MAC address** and **ESP-NOW mode** (master: TX broadcast, RX unicast; transponder: RX broadcast, TX unicast) for both boards.
4. **LED on GPIO 2**: blinks on TX (and on transponder on RX).

If you see `[NO REPLY]` with **1-way mode**, the transponder is replying via JSON on Serial (no pong over the air); this is normal when 1-way RF is requested. If the **master** was power-cycled but the **transponder** was left in 1-way mode (persisted in NVS), the master will also see no reply; after the 3rd timeout the master prints a tip: press **W** on the master to request 1-way (so both are in sync), or connect Serial to the transponder and press **W** to turn 1-way OFF. For other no-reply cases: **INTERFERENCE** (last RSSI was high → likely collision) or **SIGNAL TOO LOW** (last RSSI was low → likely range) — check distance, antennas, and power (see below).

**Serial output format:**

| Source | Format | Key fields |
|--------|--------|------------|
| **Master** (pongs) | `[HH:MM:SS] N:... \| TX aa:bb:cc:... \| FWD Loss:... \| BWD Loss:... \| Sym:... \| Z:... \| T:XX.X C \| Link%:XX Lavg:Y.Y \| plSD:X.X` | **TX** = transponder MAC; **T** = chip temp; **Link%** = rolling quality (last 10); **Lavg** = avg missed; **plSD** = path loss SD. Thermal throttling may print a second line. |
| **Transponder** (pings) | `[HH:MM:SS] RX N=... \| Mstr aa:bb:cc:... \| mode \| RSSI:... \| Mstr Pwr:... \| Path Loss:... \| TX Pwr:...` | **Mstr** = master MAC; **TX Pwr** = transponder power (set by master). |
| **Status** (`h`) | RF protocol, channel, MAC, ESP-NOW mode, CHIP TEMP (master), THERMAL (both), role settings | — |

### 3. Master serial commands (115200 baud)

| Key | Action |
|-----|--------|
| `l` | Cycle RF mode: STD (802.11) -> 250k -> 500k; master sends new mode in payload for 3 pings before switching (transponder switches first, no restart) |
| `W` | Toggle **1-way RF request**: master sends request in next ping; transponder switches to 1-way RF (JSON on Serial, no pong) or back to normal (pong over ESP-NOW). |
| `p` + number | Set master TX power (dBm), e.g. `p14`. **Valid range:** -1 to +20 dBm (typical ESP32; board-dependent). This firmware maps to discrete steps. |
| `t` + number | Set remote (transponder) target power (dBm), e.g. `t8`. **Valid range:** same as master (-1 to +20 dBm). |
| `s` | Set remote target power = current master power |
| `v` | Toggle plot mode (CSV-style output: channel, fwdLoss, bwdLoss, symmetry, zeroed, linkPct, lavg, plSD) |
| `r` + number | Set ping interval (ms), e.g. `r500`. **Valid range:** 10 ms minimum (STD and Long Range); **max** not enforced by firmware (e.g. 100–86400000 ms). LR mode can work at 10 ms. |
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

**RF channel:** Both devices **boot on channel 1** and on **ESP-NOW standard rate** (802.11, not Long Range) so they sync quickly. On the **master**, use **`n`** + channel number (e.g. **`n6`**) to set the 2.4 GHz WiFi channel (1-14). The master **sends the new channel in the payload for 3 pings** while still on the current channel; the **transponder** receives those pings and switches to the new channel. Then the master switches. **RF mode** works the same way: use **`l`** to cycle STD → LR 250k → LR 500k; the master sends the new **rfMode** in the payload for 3 pings before switching, so the transponder switches first and the link re-establishes quickly (no restart). The channel is saved to NVS when set; the RF mode is saved to NVS when switched. **Hunt on timeout** (transponder): When the transponder gets **no packet for several consecutive timeouts** it can **hunt** (cycle channels 1–14 and RF modes) to find the master again. This is **OFF by default** so open-air / lossy links do not de-sync (many lost packets would otherwise cause the transponder to keep switching channel/mode). Press **`H`** on the transponder Serial to toggle; when ON, use for lab resync after master channel/mode change; setting is saved to NVS. **Long Range mode can run at 10 ms ping interval** (same minimum as STD). Use this for channel-specific propagation tests or to avoid busy channels.

**Maximum range (shoot-out mode):** For the longest radio range, set **Long Range 250 kbps** (**`l`** until you see 250k), **max TX power** on both master (**`p`** + 20 or highest) and transponder (**`t`** + 20 or highest via master target), and a ping interval (**`r`** + value; 10 ms minimum for STD and LR). **Theoretical open-ground range with panel antennas:** Using a simple free-space link budget (e.g. 20 dBm TX, LR 250k receiver sensitivity on the order of −100 dBm, 10–12 dBi gain per side, 15 dB margin for fading/terrain), **theoretical line-of-sight range is on the order of 20–30 km** with directional panels pointed at each other. Real-world open ground will be less due to multipath, ground reflection, and obstacles; actual range depends on antenna gain, feed loss, and local conditions.

**CSV file logging:** **`f`** = start/stop, **`d`** = dump to Serial, **`e`** = erase, **`m`** + seconds = max recording time (0 = no limit). Path: SPIFFS `/log.csv`. Requires SPIFFS partition (default 4MB usually has it).

| Where | Columns |
|-------|---------|
| **Master** (each pong) | timestamp, nonce, fwdLoss, bwdLoss, symmetry, zeroed, masterRSSI, remoteRSSI, linkPct, lavg, chipTempC, plSD |
| **Transponder** (each received ping) | timestamp, nonce, rfMode, rssi, masterPwr, pathLoss, transponderPwr |

**Transponder one-way link test:** Start logging on transponder (**`f`**), set **`m`** + seconds if desired, then walk away with the master. Transponder keeps logging every packet it receives (master may show NO REPLY). Return, connect Serial to transponder, **`d`** to dump, copy to PC.

### 4. Transponder behavior

| Key / feature | Action / description |
|---------------|----------------------|
| **Pings** | Listens; replies with RSSI and power (or in 1-way RF: JSON on Serial only — see §5). |
| **`W`** / **`w`** | Toggle 1-way RF (transponder). Or **`W`** on master: request in next ping; transponder switches to 1-way (9600, JSON only, no pong). |
| **`H`** | Toggle **hunt on timeout** (cycle channel/mode when no ping). OFF by default; NVS. |
| **`0`** | Force RF to STD and restart (resync). |
| **Serial RX** | Each ping: timestamp, nonce, master MAC, mode, RSSI, path loss, TX Pwr. In 1-way: JSON only (+ LED); no status. |
| **Sync** | Follows master channel and RF mode from payload; syncs time. Hunt (if ON) cycles channel/mode after timeouts. |

### 5. 1-way RF mode

Enable with **`W`** on transponder or master. Transponder switches Serial to **9600 baud** (for Serial–MQTT bridges); prints **one JSON object per line** (no other output). Turn OFF with **`W`** again to restore pong over ESP-NOW. **1-way mode is stored in NVS** (Preferences) on both devices: after a power cycle the **transponder** comes back up in 1-way mode with Serial at 9600, and the **master** restores its last 1-way request state (so if the master was in 1-way mode when power-cycled, it boots requesting 1-way again). When **no packet is received when expected**, the transponder sends JSON with **rssi:-127, pl:-127** at the **same rate** as the master was sending (transponder stores the master ping interval from the payload). Before the first packet, it sends every 10 s. So MQTT keeps getting messages at the last rate when out of range.

**Example (packet JSON):**  
`{"pl":45.2,"rssi":-72,"mp":14,"tp":14,"n":1234,"ch":6,"m":"STD","ts":"12:34:56","missed":0,"linkPct":100,"lavg":0.0,"temp":42.5,"z":0.0,"plSD":0.8,"interval_ms":1000}`

**1-way JSON fields**

| Field | Description |
|-------|-------------|
| **pl** | Path loss (dB) |
| **rssi** | RSSI (dBm) at transponder |
| **mp**, **tp** | Master / transponder TX power (dBm) |
| **n**, **ch**, **m** | Nonce, channel, mode (STD / LR 250k / LR 500k) |
| **ts** | Timestamp (from master) |
| **missed**, **linkPct**, **lavg** | Missed packets, rolling link % (last 10), rolling avg missed |
| **temp** | Transponder chip temp (°C; -999 if unsupported) |
| **z** | In 1-way mode: **transponder-computed** (RSSI − reference from first ping in 1-way) so **z updates** on MQTT. In 2-way: master zeroed (delta from ref RSSI). |
| **plSD** | Path loss SD (dB): in 1-way mode computed on the **transponder** (SD of last 10 path losses) so it **updates** on MQTT; in 2-way the master sends it (0 when master has no pongs). |
| **interval_ms** | Master ping interval (ms) from payload; transponder stores this and sends missed JSON at this rate when no packet received. |

**Missed packet (rssi/pl -127):** The transponder stores the **master ping interval** from each received packet. When a packet is **not received when expected** (same interval + small jitter tolerance), it sends one JSON line with **rssi:-127, pl:-127** and **hb:1** at the **same rate** as the master was sending (e.g. 1/s if master was 1/s). **Before the first packet**, it sends every **10 s**. Example: `{"hb":1,"rssi":-127,"pl":-127,"ch":6,"m":"STD","ts":"12:34:56","temp":42.5,"lastN":1234,"hunt":0,"tp":14.0,"oneWay":1,"interval_ms":1000}`. **interval_ms** = stored master interval (or 10000 before first packet). Bridge publishes to the configured topic so MQTT keeps getting messages at the last rate when out of range.

**Interpretation**

| Metric | Meaning |
|--------|---------|
| **Zeroed (z)** | Master sets reference RSSI on first pong (or `z`). **z** = current backward RSSI − reference. Track change from baseline (drift, cable/antenna, movement). |
| **Path loss SD (plSD)** | In 1-way mode the **transponder** computes plSD (SD of last 10 path losses) so it **updates** on MQTT. **plSD** > ~3 dB suggests one or both devices mobile or near moving objects (RF fading). |

LED still flashes on each received ping. Master shows `[NO REPLY] | 1-way mode`.

**1-way wiring (ESP32 dev boards)** — Transponder UART0: **GPIO1 (TX)**, **GPIO3 (RX)**. For bridge you need transponder → bridge only:

| From (transponder) | To (bridge)   |
|--------------------|---------------|
| **GPIO1 (TX)**     | **GPIO21 (RX)** (e.g. [ESP32-Serial-Bridge](https://github.com/Lumy88/ESP32-Serial-Bridge)) |
| **GND**            | **GND**         |

**Built-in Bridge:** GPIO 12 → GND at boot = Serial–MQTT bridge (Serial1 RX GPIO 21 @ 9600 → MQTT). Same firmware. WiFiManager + PubSubClient. First run → AP **SerialMQTTBridge** at 192.168.4.1. Reconfigure: BOOT at boot or **http://\<IP\>/reconfigure**.

**Note (MQTT bridge and path loss):** The Serial–MQTT bridge uses WiFi on a 2.4 GHz channel. If the bridge uses the **same channel** as the ESP-NOW Master/Transponder link, WiFi traffic from the bridge can interfere with path loss measurements. Best **avoid that channel** for the RF probe link when the bridge is nearby.

**Viewing on Android:** [IoT MQTT Panel](https://play.google.com/store/apps/details?id=com.iot.mqtt.panel) — subscribe to bridge topics; use a cloud Mosquitto broker and same credentials in the app.

### 6. Development tips

| Tip | Description |
|-----|-------------|
| **Single board** | Run one as Master (GPIO 13 → GND); use Serial to verify commands. Link tests need a second board as Transponder. |
| **RSSI / path loss** | Vary distance and obstacles; watch FWD Loss, BWD Loss, Symmetry in Serial. |
| **Plot mode** (`v`) | CSV line: channel, fwdLoss, bwdLoss, symmetry, zeroed, linkPct, lavg, plSD. Use for spreadsheets. |
| **Fast ping rate** | At 115200, full lines ~8–10 ms; at 10 ms interval Serial can delay pings. Use plot mode, higher baud (e.g. 460800), or slower interval. **Baud** = `SERIAL_BAUD` in sketch. |
| **Power** | Master `p`, remote `t` set link budget; lower power helps test sensitivity. |

### 7. ESP32 optimisations

| Setting | Effect |
|---------|--------|
| **HT40 off** | `WIFI_SECOND_CHAN_NONE` — 20 MHz only (no 40 MHz). Compatible with all ESP-NOW modes. |
| **Modem sleep off** | `esp_wifi_set_ps(WIFI_PS_NONE)` — modem awake; consistent ping/pong latency. For battery, use `WIFI_PS_MIN_MODEM` in sketch; expect higher/variable latency. |
| **WiFi only** | `WiFi.mode(WIFI_STA)`; no AP. Bluetooth unused; disable in board options to free RAM and avoid radio overlap. |

---

## Quick reference

| Item | Value |
|------|--------|
| **Mode at boot** | GPIO 12 = GND → **Bridge** (Serial1 RX @ 9600 → MQTT). GPIO 12 floating → Master or Transponder. |
| **Role** | GPIO 13 = GND → **Master**; GPIO 13 floating → **Transponder**. |
| **RF** | STD or Long Range 250k/500k; channel 1–14. Boot: channel 1, standard rate. **`n`** = channel, **`l`** = cycle mode. |
| **Serial** | 115200 (or `SERIAL_BAUD`) on Master. **`h`** = full status (MAC, ESP-NOW mode). |
| **Build** | Arduino IDE: open `dBmNow.ino`, Upload. **Flash all 3:** [Arduino CLI](https://arduino.github.io/arduino-cli/) + **`.\flash_all.ps1`** (edit COM ports in script). |

---

## Default settings on first run

Values below apply when nothing has been configured by the user (no NVS/config yet). Master does not load channel or RF mode from NVS on boot; transponder loads only **hunt on timeout** from NVS (default OFF). Bridge uses the MQTT defaults until configured via the AP.

**Master** (GPIO 12 floating, GPIO 13 → GND)

| Setting | Default |
|---------|---------|
| RF mode | STD (802.11 b/g/n) |
| RF channel | 1 |
| Master TX power | -1 dBm |
| Remote (transponder) target power | -1 dBm |
| Ping interval | 1000 ms |
| 1-way RF request | OFF |
| Zero cal (Z) | Not calibrated |
| Plot mode | OFF |
| CSV file logging | OFF |
| CSV max record time | 0 (no limit) |
| Promiscuous mode | OFF |
| Serial baud | 115200 (or `SERIAL_BAUD`) |

**Transponder** (GPIO 12 floating, GPIO 13 floating)

| Setting | Default |
|---------|---------|
| RF mode | STD (802.11 b/g/n) |
| RF channel | 1 (follows master from payload) |
| TX power | -1 dBm (until first ping; then follows master target) |
| Hunt on timeout | OFF |
| 1-way RF mode | OFF |
| Serial baud | 115200 (or `SERIAL_BAUD`; 9600 when 1-way RF is ON) |
| CSV file logging | OFF |
| CSV max record time | 0 (no limit) |

**Bridge** (GPIO 12 → GND at boot)

| Setting | Default (first run, no WiFi/MQTT config) |
|---------|------------------------------------------|
| Serial1 (RX) | GPIO 21 @ 9600 baud |
| MQTT broker | 192.168.1.100 |
| MQTT port | 1883 |
| MQTT topic | Esp32/result |
| MQTT user / pass | (empty) |
| WiFi | Not configured → AP **SerialMQTTBridge** at 192.168.4.1 |
| USB Serial (debug) | 9600 (`SERIAL_BAUD_BRIDGE_USB`) |

---

## Links

| Resource | Description |
|----------|-------------|
| **[IoT MQTT Panel](https://play.google.com/store/apps/details?id=com.iot.mqtt.panel)** (Android) | MQTT client for 1-way path loss, RSSI, etc. with cloud Mosquitto + bridge. |
| **[Serial USB Terminal](https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal)** (Android) | Serial over USB OTG; tested with master at max TX (v1.57). |
| **[ESP-NOW source](https://github.com/espressif/esp-now)** | Official ESP-NOW component and examples. |
| **[ESP-NOW API (ESP-IDF)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)** | ESP-NOW API reference. |
| **[ESP-IDF espnow example](https://github.com/espressif/esp-idf/tree/master/examples/wifi/espnow)** | Example usage in ESP-IDF. |

---

## Planned features



- **Commands via MQTT** — Allow sending some commands to the device over MQTT (e.g. set channel, toggle 1-way RF, start/stop logging) so the master or transponder can be controlled from the cloud or from an app.
- **Transponder command acknowledgment** — The pong already carries the transponder’s current channel, rfMode, and txPower. Add on the master: (1) show transponder-reported channel and mode (e.g. in the status line or “TX ch X mode Y”) so the user can see what the transponder says it’s using; (2) when the master has sent a channel or RF-mode change, verify pong.channel / pong.rfMode against what was requested and print a confirmation or mismatch warning (e.g. “>> Transponder confirmed ch X” or “>> Transponder reports ch X (expected Y)”).
- **TRX relay controller** -- Antenna switching (e.g. for T/R or diversity setups).
- **RF measurement calibration** -- Calibration for different ESP-NOW modes (STD, LR 250k, LR 500k) for more accurate dBm/path-loss readings.
- **RF characterisation** -- Characterise the RF behaviour of the device (e.g. TX power vs setting, RSSI linearity) as a reference for calibration.
- **Unicast mode with 802.11 PHY statistics** -- Unicast operation with PHY stats (retries, etc.) for link analysis; support for operating multiple transponders.
- **RF test examples** -- Examples described in the README (e.g. path loss, symmetry, channel sweep), including a one-port test using a directional coupler.
- **Max payload mode** -- Master and transponder fill all free space in the packet so each packet is maximum size (e.g. for throughput or air-time testing).
- **Firmware version check** -- Check that master and transponder are running the same firmware version and alert the master (e.g. via payload or status) when they differ.
- **Save Promiscuous mode results to .csv** -- Option to save promiscuous scan measurements (channel, packet count, avg/min RSSI, busy %) to a .csv file.
- **Android-based master controller** -- Master UI/control from an Android device via OTG cable.
- **Android app: master over OTG + MQTT + real-time chart** — Develop an Android app that connects to the master via USB OTG (Serial) and to MQTT to receive 1-way mode measurements (path loss, RSSI, etc.) and display them on a real-time chart. Aimed at **testing DAS uplink performance in RFoFibre (RF-over-Fibre) DAS systems** — master at the test point, transponder at the remote antenna unit or head end, 1-way JSON over MQTT to the app for live uplink path-loss monitoring.
- **RF overload detection** -- Detect when the receiver is overloaded (e.g. antennas too close or insufficient attenuation) and warn or protect the link.
- **Auto TX power** -- When path loss is below a set point (RX level too high), the master automatically reduces TX power on both master and transponder so that RX level does not exceed a maximum (e.g. -20 dBm) to avoid receiver overload. Conversely, when RX level is too low (path loss above a set point), the master increases TX power on both devices to improve the link.
- **Configurable PHY rate for standard mode** -- Option to set ESP-NOW PHY rate above the default 1 Mbps (e.g. 24M, 54M, or 802.11n MCS rates) for higher throughput at short range, with tradeoff of range/reliability.
- **Real-time clock** -- Hardware RTC (e.g. external RTC module or battery-backed RTC) for accurate timestamps across power cycles and when WiFi sync is not available.
- **Hardware design with rechargeable battery** -- Reference or suggested hardware design for a battery-powered unit with rechargeable battery and charging circuitry.
- **SD card** -- Hardware and firmware support for recording results to SD card (e.g. in addition to or instead of SPIFFS).

---

## Legal and compliance

### Development and dependencies

This project was developed using [Cursor](https://cursor.com/) (AI-assisted IDE) and the Arduino IDE for Windows v2.3.2. The firmware uses the [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) (and Espressif SDKs); ensure your use and distribution comply with GPL v2 and their terms. ESP32, Arduino, and related names are trademarks of their respective owners; this project is not affiliated with or endorsed by them.

### RF use and EIRP

Use of this firmware in devices that transmit in the 2.4 GHz band may be subject to local regulations (FCC, CE, radio licensing). You are responsible for compliance; avoid causing interference to other users.

**External antennas:** Limits are usually expressed as **EIRP**, which applies to **transmit** and **transceive** antennas only. In a dual-antenna system with a GPIO-controlled switch, a separate high-gain **receive** antenna could be used without counting toward EIRP limits.

| Term | Formula / note |
|------|-----------------|
| **EIRP (dBm)** | P_TX (dBm) + G_antenna (dBi) − L_cable (dB) |
| **Example** | 20 dBm + 5 dBi − 1 dB = **24 dBm EIRP** |
| **Typical limit** | Many regions: **20 dBm EIRP** for 2.4 GHz ISM. Reduce TX (`p`, `t`) or use low-gain antennas. EIRP applies to **transmit** only. Check local rules (FCC, ETSI, UK IR2030). |

### Duty cycle and UK

In some regions (e.g. EU under ETSI EN 300 328), non-adaptive 2.4 GHz use may be subject to **duty cycle / medium-utilisation limits**. Choose ping interval (`r`) and TX power accordingly.

**UK:** 2.4 GHz licence-exempt use is governed by **Ofcom** and **UK IR2030**. Comply with IR2030 and **ETSI EN 300 328**. Operation is non-protected, non-interference. See [Ofcom licence-exempt devices](https://www.ofcom.org.uk/spectrum/radio-equipment/licence-exempt-devices) and [IR2030](https://www.ofcom.org.uk/spectrum/radio-equipment/regulations-technical-reference). In the UK (and EU) only **channels 1–13** are allowed for **radiated** use; **channel 14** is not permitted over the air. For **conducted / cabled use only** (e.g. RF in cables, no antenna, or screened environment), use of channel 14 is generally acceptable because restrictions apply to radiated emissions; confirm with IR2030 and your compliance advisor if in doubt.

### Radio amateurs

Licensed amateurs may have higher power privileges on 2.4 GHz (e.g. FCC Part 97) when operating in the amateur service. The ESP32 is limited to ~20 dBm; for higher power use an external PA and comply with national amateur regulations.

---

## License

**dBm-Now** is free software; you can redistribute it and/or modify it under the terms of the **GNU General Public License version 2** (or, at your option, any later version). See the [LICENSE](LICENSE) file in this repository for the full text.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
