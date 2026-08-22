# iSpatial IoT Sensor Node

An ESP32-based IoT sensor node that collects sensor readings, creates one-minute telemetry records, and sends them to an MQTT broker.

The main focus of this project is **reliable data delivery during network failures**. When Wi-Fi or MQTT is unavailable, telemetry records are stored in the ESP32 flash using LittleFS. When the connection returns, stored records are sent oldest first.

## Features

- ESP32-based sensor node
- ADC sensor reading every 10 seconds
- One-minute telemetry aggregation
- Average, minimum, maximum, and sample count
- NTP time synchronization
- MQTT communication using HiveMQ public broker
- MQTT QoS 1
- PUBACK verification using Packet ID
- LittleFS local storage
- Store-and-forward during network failure
- Oldest-first queued record transmission
- Record deletion only after successful matching PUBACK
- Queued records remain available after ESP32 restart
- Wi-Fi reconnect with bounded exponential backoff
- MQTT reconnect with bounded exponential backoff
- ESP32 watchdog for firmware recovery

---

## Hardware

| Component | Purpose |
|---|---|
| ESP32 | Main controller |
| 10 kΩ potentiometer | Sensor input / ADC test source |
| USB cable | Power and programming |

The potentiometer is used as the sensor input for this assignment.

### Wiring

The potentiometer is connected as a voltage divider:

```text
ESP32 3.3V
   |
 [ POT ]
   |
   +-------- GPIO 34 (ADC)
   |
 [ POT ]
   |
  GND
```

- Potentiometer VCC → ESP32 3.3V
- Potentiometer GND → ESP32 GND
- Potentiometer wiper → ESP32 GPIO 34

GPIO 34 is used as the ADC input.

---

## Software

- Arduino IDE
- ESP32 Arduino Core
- AsyncMqttClient
- LittleFS
- WiFi
- NTP / `time.h`

### MQTT Broker

The project uses the HiveMQ public MQTT broker:

```text
broker.hivemq.com
Port: 1883
```

MQTT was selected because it is lightweight and suitable for telemetry from embedded devices.

---

## MQTT Topic

Each device creates a device-specific topic using its ESP32 MAC address.

Example:

```text
devices/ESP32_FCE8C07BB6DC/telemetry
```

This allows multiple devices to use separate telemetry topics.

---

## Data Sampling

The ESP32 reads the ADC input every 10 seconds.

After 60 seconds, the collected readings are used to calculate:

- Average
- Minimum
- Maximum
- Sample count

The sampling and aggregation logic uses `millis()` timing instead of `delay()` in the main loop.

The configured intervals are:

```text
Sampling interval: 10 seconds
Aggregation window: 60 seconds
```

---

## JSON Format

Each one-minute telemetry record is created as JSON.

Example:

```json
{
  "device": "ESP32_FCE8C07BB6DC",
  "timestamp": "2026-08-21 23:10:43",
  "average": 1345.667,
  "minimum": 379,
  "maximum": 2487,
  "count": 6
}
```

### Fields

| Field | Description |
|---|---|
| `device` | Unique ESP32 device ID |
| `timestamp` | Timestamp of the telemetry record |
| `average` | Average ADC reading |
| `minimum` | Minimum reading in the window |
| `maximum` | Maximum reading in the window |
| `count` | Number of readings in the window |

---

## Timestamp

The ESP32 synchronizes its clock using NTP during startup.

The timestamp is stored inside each telemetry record.

Example:

```text
2026-08-21 23:10:43
```

The ESP32 continues using its system clock while offline, so records created during a network outage keep their original timestamps.

---

# Store-and-Forward

Store-and-forward is the main reliability feature of this project.

Every completed telemetry record is stored in LittleFS before transmission.

Example:

```text
/record_001.json
/record_002.json
/record_003.json
```

This means that a network failure does not immediately remove the only local copy of a telemetry record.

### Normal operation

```text
Sensor
   ↓
10-second readings
   ↓
60-second aggregation
   ↓
Create JSON record
   ↓
Store in LittleFS
   ↓
Publish using MQTT
   ↓
PUBACK
   ↓
Delete stored record
```

### Network failure

```text
Network unavailable
        ↓
Continue sensor sampling
        ↓
Create telemetry record
        ↓
Store in LittleFS
        ↓
Keep record in queue
```

### Network recovery

```text
Network returns
      ↓
Reconnect to MQTT
      ↓
Find oldest queued record
      ↓
Publish using QoS 1
      ↓
Receive PUBACK
      ↓
Verify Packet ID
      ↓
Delete acknowledged record
      ↓
Send next queued record
```

---

## MQTT QoS 1 and PUBACK

The project uses MQTT QoS 1 for telemetry.

When a record is published, MQTT provides a Packet ID.

The ESP32 keeps track of:

- Packet ID
- Filename of the queued record

The record is deleted only when the received PUBACK Packet ID matches the pending Packet ID.

Example:

```text
Packet ID: 2
       ↓
Publish record_001.json
       ↓
PUBACK Packet ID: 2
       ↓
Packet IDs match
       ↓
Delete record_001.json
```

This prevents a queued record from being deleted merely because a publish was attempted.

---

## Queue Order

The ESP32 scans the LittleFS records and determines:

- The lowest record number → oldest record
- The highest record number → newest record
- The next available number → new record filename

For example:

```text
record_001.json
record_002.json
record_003.json
```

The device sends:

```text
record_001
      ↓
record_002
      ↓
record_003
```

This provides oldest-first recovery.

---

# Network Recovery

Wi-Fi and MQTT recovery are handled separately so that network failures do not stop the telemetry loop.

The reconnect logic uses **bounded exponential backoff** rather than continuously retrying in a tight loop.

The retry delay increases approximately as:

```text
1 second
     ↓
2 seconds
     ↓
4 seconds
     ↓
8 seconds
     ↓
16 seconds
     ↓
30 seconds maximum
```

After a successful connection, the retry delay is reset.

This reduces unnecessary connection attempts during long outages while allowing the device to continue sampling and storing telemetry.

> The current implementation uses bounded exponential backoff. Random jitter has not been added.

---

# Watchdog

An ESP32 task watchdog is enabled with a 10-second timeout.

The main loop feeds the watchdog while the firmware is operating normally.

The watchdog is a last-resort recovery mechanism if the firmware becomes stuck.

Network failure itself does **not** intentionally reset the device. The device continues sampling, buffering records, and retrying the connection.

Example startup output:

```text
Watchdog enabled (10 seconds)
```

---

## Wi-Fi Failure Test

The system was tested by disconnecting the network while the ESP32 was running.

During the outage:

- Sensor readings continued.
- One-minute telemetry records continued to be generated.
- Records were stored in LittleFS.
- MQTT transmission was unavailable.
- Wi-Fi reconnection continued using backoff.

After Wi-Fi returned:

- The ESP32 reconnected.
- MQTT reconnected.
- Stored records were detected.
- The oldest record was transmitted first.
- PUBACK was received.
- The acknowledged record was deleted.
- The next queued record was transmitted.

Example:

```text
record_001 → PUBACK → deleted
record_002 → PUBACK → deleted
```

The records arrived at the MQTT subscriber with their original timestamps and in the correct order during testing.

---

## ESP32 Restart Test

Queued records were also tested across an ESP32 restart.

Because the records are stored in LittleFS flash rather than only in RAM, queued records remain available after reboot.

After reconnecting to MQTT, the ESP32 sends the stored records.

---

# Build and Flash

### 1. Install Arduino IDE

Install Arduino IDE and add ESP32 board support.

### 2. Install required libraries

Install:

```text
AsyncMqttClient
```

The following libraries are provided by the ESP32 Arduino environment:

```text
WiFi
LittleFS
time
```

### 3. Configure Wi-Fi

Update the Wi-Fi configuration in the firmware:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

### 4. Configure MQTT

The MQTT server is configured as:

```cpp
const char* MQTT_SERVER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
```

### 5. Select the ESP32 board

In Arduino IDE:

```text
Tools → Board → ESP32 → Your ESP32 Board
```

Select the correct COM port.

### 6. Upload

Compile and upload the firmware to the ESP32.

Open Serial Monitor at:

```text
115200 baud
```

The device should display its connection, recovery, and telemetry status.

---

## Example Serial Output

```text
================================
iSpatial IoT Sensor Node
================================

Device ID: ESP32_FCE8C07BB6DC
MQTT Topic: devices/ESP32_FCE8C07BB6DC/telemetry

LittleFS mounted

Wi-Fi connected
NTP synchronized!

Watchdog enabled (10 seconds)

MQTT connected

----- TELEMETRY RECORD -----
Timestamp: 2026-08-22 07:53:15
Average: 715.00
Minimum: 304
Maximum: 1459
Count: 6

Stored: /record_001.json
```

When a queued record is transmitted:

```text
----- SENDING QUEUED RECORD -----

File: /record_001.json
Packet ID: 2
Waiting for PUBACK...

Publish acknowledged!
ACK Packet ID: 2
ACK matches: /record_001.json

Record deleted from LittleFS.
```

During reconnect:

```text
Wi-Fi reconnect attempt (delay 1000 ms)
Wi-Fi reconnect attempt (delay 2000 ms)
Wi-Fi reconnect attempt (delay 4000 ms)
```

---

# Design Decisions

## Why MQTT?

MQTT is lightweight and commonly used for IoT telemetry. It also provides QoS levels and acknowledgments that can be used to improve delivery reliability.

## Why LittleFS?

LittleFS provides persistent storage in ESP32 flash. It allows telemetry records to remain available when Wi-Fi or MQTT is unavailable.

## Why store before publishing?

The telemetry record is stored first so that a network failure does not remove the only local copy of the record.

## Why QoS 1?

QoS 1 provides an acknowledgment from the MQTT broker. The project uses the PUBACK Packet ID to decide when a local record can safely be deleted.

## Why oldest-first?

Sending the oldest queued record first preserves chronological recovery order. Original timestamps remain inside the records.

## Why exponential backoff?

Repeated immediate connection attempts waste resources and can create unnecessary network traffic. Bounded exponential backoff increases the retry interval during a long outage while keeping the device responsive when connectivity returns.

## Why a watchdog?

The watchdog provides a last-resort recovery mechanism if firmware execution becomes stuck. It is not used as the normal response to network failure.

## Why device-specific topics?

The device ID is generated from the ESP32 MAC address:

```text
ESP32_<MAC_ADDRESS>
```

The MQTT topic is then created from the device ID.

This allows multiple sensor nodes to publish independently.

---

# Known Limitations

The following items are not fully implemented and would need additional work for a production deployment:

1. There is currently no explicit storage-full handling policy in the firmware. The project documents this as a limitation rather than silently deleting queued data.
2. The current backoff is bounded exponential backoff; random jitter has not been added.
3. MQTT currently uses an unsecured connection on port 1883. TLS should be used for production.
4. Wi-Fi credentials are currently configured in the firmware and should be moved to a safer provisioning mechanism.
5. Flash wear management should be considered for long-term deployments.
6. The current queue uses individual JSON files and would need stronger protection against storage corruption or unexpected power loss during a write.
7. Production deployment would benefit from health telemetry such as uptime, reset reason, Wi-Fi signal strength, free storage, queue depth, firmware version, and last successful publish time.
8. OTA firmware updates and rollback are not implemented.

These limitations are intentionally documented rather than hidden.

---

# Testing Summary

| Test | Result |
|---|---|
| ESP32 startup | Passed |
| Wi-Fi connection | Passed |
| NTP synchronization | Passed |
| ADC sampling | Passed |
| 60-second aggregation | Passed |
| JSON generation | Passed |
| MQTT publishing | Passed |
| MQTT QoS 1 | Passed |
| PUBACK verification | Passed |
| LittleFS storage | Passed |
| Wi-Fi failure | Passed |
| Continued sampling during network failure | Passed |
| Multiple queued records | Passed |
| Oldest-first recovery | Passed |
| Delete after matching PUBACK | Passed |
| Queued records after ESP32 restart | Passed |
| Wi-Fi reconnect backoff | Passed |
| MQTT reconnect backoff | Passed |
| Watchdog initialization | Passed |

---

# AI and External Resources

AI assistance was used during development to help with:

- Understanding MQTT QoS 1 and PUBACK behavior
- Reviewing the store-and-forward design
- Debugging ESP32 and LittleFS behavior
- Reviewing reconnect and watchdog logic
- Improving code structure and documentation

External libraries used include:

- ESP32 Arduino Core
- AsyncMqttClient
- LittleFS

The final implementation was tested on the ESP32. The developer is responsible for understanding and explaining the implemented code.

---

# Project Status

**Core store-and-forward telemetry system: Completed and tested.**

The system successfully collects sensor data, creates timestamped telemetry records, stores records locally when required, and sends queued records after network recovery using MQTT QoS 1 and PUBACK verification.

The firmware also includes watchdog protection and bounded exponential backoff for Wi-Fi and MQTT recovery.

