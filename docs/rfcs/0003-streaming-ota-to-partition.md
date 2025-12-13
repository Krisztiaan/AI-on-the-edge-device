# RFC 0003: Stream OTA Directly to OTA Partition (No SD Staging)

## Status

Draft

## Context / Problem

The current online update flow downloads a ZIP to SD and then installs from SD. This is simple but has downsides:
- Requires SD presence and sufficient free space.
- Slower and more failure-prone (extra write + unzip).
- Power loss during unzip/install can leave partially extracted files on SD.

ESP-IDF supports streaming firmware writes directly to an OTA partition using `esp_ota_begin/write/end` and then setting the boot partition.

## Goals

- Enable a “streaming OTA” path that does not require SD space for the firmware artifact.
- Make updates faster and reduce wear on SD.
- Preserve current SD-staged ZIP flow as a fallback (especially for Web UI assets).

## Non-goals

- Delta updates (binary diff).
- Changing the “web UI assets” update mechanism unless explicitly chosen.
- Solving authenticity (handled by RFC 0001).

## Proposed Design

### 1) Artifact Split

Define two update payload types:

1) **Firmware-only**:
- `firmware.bin` (ESP32 app image)
- installed via streaming OTA to OTA partition

2) **Bundle** (optional):
- `firmware.bin` + `sdcard.zip` (web UI/config defaults/models)
- firmware streamed; sdcard.zip optionally downloaded and applied after reboot or on-demand

Manifest change (backwards compatible):
- Add an optional `firmware_bin` URL + sha256.
- Keep existing ZIP fields for web assets.

### 2) Streaming OTA Flow

Firmware download handler:
- HTTP(S) GET `firmware.bin`
- Verify size within limits and sha256 as it streams (incremental hash)
- Write stream into OTA partition via ESP-IDF OTA APIs
- After download:
  - finalize OTA (`esp_ota_end`)
  - set boot partition (`esp_ota_set_boot_partition`)
  - mark state pending verify (see RFC 0002)
  - reboot

### 3) Resilience / Power Loss

- OTA partition write is transactional in the sense that the boot partition only changes on success.
- If power loss occurs mid-download/write:
  - OTA partition may contain partial image, but boot partition remains the previous valid one.
- Combine with rollback verification in RFC 0002 for first-boot safety.

### 4) Where Web Assets Fit

Options:
- **A. Keep current ZIP-to-SD mechanism** for web assets; firmware streaming is independent.
- **B. Add “sdcard.zip” streaming + atomic extraction** (harder).

Recommendation:
- Option A for phase 1: simplest, less risky.

### 5) Endpoints / UI

Add a new OTA task, e.g.:
- `/ota?task=download_firmware&manifest=...`

UI:
- Add “Firmware update (streaming)” button if manifest contains `firmware_bin`.
- Keep “Full update (ZIP)” for existing devices/configurations.

### 6) Implementation Touch Points

- OTA code: `code/components/jomjol_fileserver_ota/server_ota.cpp`
- HTTP client usage / TLS config
- Partition table must support at least 2 OTA slots + OTA data (see RFC 0004)

## Test Plan

- Valid firmware streaming update; verify reboot into new version.
- Corrupt firmware download (hash mismatch); verify no boot partition switch.
- Interrupt power during streaming; verify device still boots old firmware.
- Run with no SD inserted; verify streaming OTA still works.

## Rollout Plan

Phase 1:
- Implement firmware streaming OTA with hash verification.
Phase 2:
- Extend manifest + UI selection.
Phase 3:
- Consider streaming and applying `sdcard.zip` in a safer/atomic way.

