# RFC 0002: OTA Rollback, Boot Health, and Safe Mode

## Status

Draft

## Context / Problem

We support OTA updates, but “bad updates” can brick the device in practice:
- Boot loops after update (crash early, watchdog resets).
- Networking misconfiguration (can’t reach web UI).
- Regression that prevents measurement loop from running.

ESP-IDF provides OTA state tracking and rollback primitives (`esp_ota_*`), but the current flow largely relies on installing a new image and rebooting without a robust “health confirmation” gate.

## Goals

- Make firmware updates **recoverable**:
  - Automatic rollback if the new firmware fails to become healthy.
  - Manual recovery mode that is reachable even if configuration is bad.
- Provide clear, inspectable diagnostics for why a rollback happened.
- Keep behavior deterministic across power loss during update/first boot.

## Non-goals

- Implement signed manifests (handled in RFC 0001).
- Build a full remote telemetry pipeline.
- Guarantee recovery from hardware faults (bad flash, SD corruption).

## Proposed Design

### 1) Use ESP-IDF “Pending Verify” Flow

When booting into a freshly updated image:
- Mark the image as “pending verification” at first boot.
- Only mark it “valid” after passing a health check.
- If health check does not complete in time, rollback to previous image.

Key APIs (ESP-IDF):
- `esp_ota_get_state_partition(...)`
- `esp_ota_mark_app_valid_cancel_rollback()`
- `esp_ota_mark_app_invalid_rollback_and_reboot()`

### 2) Define “Health”

Health criteria should be explicit and minimal:
- The system boots to the point where:
  - Wi-Fi has connected successfully OR AP fallback is running.
  - Web server task is running and responds locally.
  - Critical subsystems initialize without fatal errors (NVS, SD mount attempt).
- Optionally: first successful measurement cycle completes (configurable; might be too strict).

Implementation:
- Add a “health watchdog” task that starts on boot if OTA state is pending verify.
- It tracks milestones and a timeout (e.g. 120s configurable).
- If timeout triggers without “healthy”, mark invalid and rollback.

### 3) Bootloop Detection and Escalation

Even without OTA, repeated crashes should lead to a safe-mode boot:
- Store boot counter in NVS with timestamps.
- If N consecutive boots within T seconds, enter safe mode.

Safe mode behavior:
- Force AP mode with known SSID (derived from MAC) and default password prompt on first connect (or printed on logs/UI).
- Disable non-essential tasks (measurement loop) to reduce crashes.
- Provide a minimal web UI page for:
  - viewing logs
  - selecting OTA channel (if enabled)
  - initiating rollback (if possible) or “factory reset” (see RFC 0004)

### 4) Explicit “Confirm Update” Button

To reduce risk of false-positives/false-negatives in health:
- Expose an endpoint/UI button “Confirm new firmware” that calls `esp_ota_mark_app_valid_cancel_rollback()`.
- Default behavior: auto-confirm after health criteria are met; manual confirm is an additional tool.

### 5) Persisted Update Metadata

Write a small JSON status file to SD (or NVS) to make behavior debuggable:
- previous version
- new version
- update timestamp
- result: success/rollback
- rollback reason (timeout, crash loop, missing subsystem)

Location suggestion:
- `sd-card/log/update_status.json` (or `sd-card/config/update_status.json` depending on existing structure).

### 6) UI/UX

In `sd-card/html/ota_page.html`:
- Show “Current firmware status: valid/pending-verify”.
- If pending: show countdown/health milestones.
- If safe mode: show banner and recovery options.

## Implementation Notes / Likely Touch Points

- Boot code entry: `code/main/main.cpp` or the earliest init module used.
- OTA handler: `code/components/jomjol_fileserver_ota/server_ota.cpp`
- Any config persistence layer / NVS wrapper used in the project.
- Web UI pages under `sd-card/html/`.

## Failure Modes to Handle

- Power loss during download: no install should proceed until full artifact verified.
- Power loss during flash write: rely on ESP-IDF OTA partitioning.
- First boot crash: rollback should occur after repeated resets or health timeout.
- SD missing: safe mode must not depend on SD availability.

## Test Plan

Bench tests:
- Force a “bad image” (intentionally crash on boot) and verify rollback triggers.
- Break Wi-Fi credentials and verify AP fallback + ability to reach UI.
- Simulate boot loop without OTA and verify safe mode triggers.
- Verify update status metadata persists and is accurate.

CI/build:
- Ensure partition table supports rollback (see RFC 0004).

## Rollout Plan

Phase 1:
- Add OTA pending-verify + health task + rollback.
Phase 2:
- Add bootloop detector + safe mode UI.
Phase 3:
- Add richer diagnostics/log export.

