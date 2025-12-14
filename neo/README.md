# AI on the Edge Device — NEO

This is an experimental “ground-up” firmware track intended to keep **ESP32‑CAM (4MB flash)** as a first‑class target while improving:

- local-first setup and management UX
- HA-native MQTT integration (discovery, `update` entity, etc.)
- deterministic model/package storage
- robust OTA + recovery
- performance and flash/RAM efficiency

NEO is built to be **feature-gated by capability** (flash size, PSRAM availability, target family) so we can keep “niceties” on ESP32‑CAM where possible, and scale up on stronger hardware without redesigning the stack.

## Building

From repo root:

```sh
platformio run -d neo -e esp32cam-neo
```

## Notes

- This is intentionally minimal at first to establish a clean baseline and measure flash usage.
- Feature gates live in Kconfig under `neo/components/*/Kconfig` as they are introduced.

