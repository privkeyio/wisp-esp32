# wisp-esp32

<img src="assets/wisp-logo.jpeg" alt="Wisp Logo" width="300">

A minimal [Nostr](https://github.com/nostr-protocol/nostr) relay for ESP32-S3.

## Overview

Wisp-ESP32 brings the Nostr protocol to embedded hardware. An ephemeral relay with 21-day TTL storage, designed for privacy-first local relay use cases.

**Target specs:**
- 5-10 concurrent WebSocket connections
- Sub-100ms latency
- 6MB flash storage (~6,000 events)
- Schnorr signature verification via libnostr-c/noscrypt

**Supported NIPs:**
- NIP-01 (protocol)
- NIP-11 (relay info)
- NIP-40 (expiration)

## Prerequisites

**ESP-IDF v5.3.4**

```bash
git clone -b v5.3.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
source export.sh
```

**Clone repositories as siblings:**

```bash
cd ~/projects  # or your preferred directory
git clone -b esp-idf-support https://github.com/privkeyio/secp256k1-frost
git clone https://github.com/privkeyio/wisp-esp32
git clone -b esp-idf-support https://github.com/privkeyio/noscrypt
git clone https://github.com/privkeyio/libnostr-c
```

Your directory structure should look like:
```
~/projects/
├── wisp-esp32/        # This repo
├── libnostr-c/        # Nostr library (symlinked in components/)
├── noscrypt/          # NIP-44 crypto (symlinked in components/)
└── secp256k1-frost/   # Schnorr signatures
```

## Build

```bash
cd wisp-esp32
source ~/path/to/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Hardware

- ESP32-S3 with 8MB Flash (PSRAM optional, recommended)
- Tested on M5Stack AtomS3 Lite, ESP32-S3-DevKitC-1-N8R8

## Testing

### Hardware tests (requires device)

```bash
pip install websockets
RELAY_URL=ws://<relay-ip>:4869 python test/hardware/test_relay.py
```

### Native tests (no device)

```bash
cd test/native
mkdir -p ../../managed_components/espressif__cjson
git clone https://github.com/DaveGamble/cJSON.git ../../managed_components/espressif__cjson/cJSON
mkdir -p build && cd build
cmake .. && make
./test_router
```

## License

AGPL-3.0
