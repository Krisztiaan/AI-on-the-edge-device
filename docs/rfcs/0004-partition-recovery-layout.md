# RFC 0004: Partition Table and Recovery Strategy (A/B OTA, Factory, and Data)

## Status

Draft

## Context / Problem

Reliable OTA depends on the flash layout:
- Need at least two app slots (A/B) for safe updates.
- Need OTA data partition for ESP-IDF OTA state/rollback.
- Prefer a “factory” image to recover from extreme failure.

This repo already builds artifacts including `partitions.bin`, but we should explicitly define a partitioning strategy that supports:
- rollback (RFC 0002)
- streaming OTA (RFC 0003)
- long-term recoverability

## Goals

- Standardize a partition table for supported boards/environments that enables:
  - A/B OTA with rollback
  - optional factory partition
  - robust NVS usage
- Document size constraints and tradeoffs (app size vs. filesystem size).

## Non-goals

- Full Secure Boot + Flash Encryption enablement (can be a follow-up RFC).
- Guaranteeing compatibility with all third-party partition tables.

## Proposed Design

### 1) Baseline Partition Layout

Minimum recommended partitions:
- `nvs` (for config + counters)
- `otadata` (ESP-IDF OTA state)
- `ota_0` + `ota_1` (two app slots)

Optional:
- `factory` app slot (known-good recovery image)
- `coredump` (for crash diagnostics)
- `spiffs`/`littlefs` (if needed for small persistent data; SD is primary here)

### 2) Sizing Principles

- App slots must be >= the largest firmware image size + margin.
- If web assets are primarily on SD, filesystem partition can be minimized.
- If TFLite models ever move to flash, that must be explicitly planned (likely not desired).

### 3) Factory Recovery Mode

If a `factory` partition is included:
- Provide a UI/endpoint to “Boot factory firmware next reboot”.
- Factory firmware should be minimal but functional:
  - AP mode
  - web UI for OTA and config restore
  - optional SD formatting/repair tools (with strong warnings)

### 4) Compatibility With PlatformIO / Build Outputs

Document the expected build artifacts:
- `firmware.bin`
- `bootloader.bin`
- `partitions.bin`

And how the partition CSV is chosen per environment.

### 5) Migration / Backward Compatibility

Changing partition tables can brick devices if not handled carefully.

Recommended migration strategy:
- Only change partition layout in a major release.
- Provide clear instructions and a “manual flash all” fallback.
- Consider a staged approach:
  - First release includes a “migration firmware” that can prepare NVS and warn users.
  - Next release actually changes partitions.

### 6) Interaction With Rollback / Safe Mode

Rollback requires:
- `otadata` present and correct
- two OTA app slots

Safe mode (RFC 0002) must:
- avoid dependence on large filesystem partitions
- rely on NVS and minimal SD usage

## Implementation Steps (Suggested)

1. Locate existing partition CSV(s) used by `esp32cam` environment.
2. Define the recommended “default” CSV and document its intent.
3. Update build docs / release artifacts to reflect the default partitioning.
4. Add sanity checks (where feasible) to CI/build to ensure partitions are produced and app size fits.

## Test Plan

- Confirm firmware fits both OTA slots with margin.
- Perform streaming OTA update (RFC 0003) and rollback test (RFC 0002).
- If factory partition exists: verify switching boot partition to factory works and can then update back.

