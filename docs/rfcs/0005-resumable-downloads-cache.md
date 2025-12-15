# RFC 0005: Resumable Downloads, Cache, and Atomic Writes (Models + Updates)

## Status

Draft

## Context / Problem

We download update ZIPs and model files over the network. Failure modes:
- Wi-Fi drop mid-download → wasted bandwidth/time.
- Power loss during download → partial files with unclear state.
- Re-downloading the same model/update repeatedly.

Also, integrity verification happens after download; if we can combine verification with atomic file replacement, we can avoid leaving corrupt artifacts in-place.

## Goals

- Support resumable downloads where the server supports HTTP range requests.
- Make file writes atomic (or as-close-as-possible on FAT/SD) to avoid half-written artifacts being treated as valid.
- Avoid re-downloading known-good models/updates (cache by hash/version).
- Improve UX with progress + clear errors.

## Non-goals

- Content distribution network design.
- Peer-to-peer model distribution.
- Replacing GitHub Pages transport entirely.

## Proposed Design

### 1) Download to Temp + Atomic Rename

When downloading an artifact to SD:
- Write to `*.part` (temporary file).
- Only after the full download and hash verification succeeds, rename to final name.

Examples:
- `github_update.zip.part` → `github_update.zip`
- `<model>.tflite.part` → `<model>.tflite`

Notes:
- FAT rename is not fully atomic in all cases, but this is the best available pattern.
- On boot/startup, clean up stale `*.part` if desired (with caution).

### 2) Resume with HTTP Range

If a `.part` file exists:
- Determine its current size `N`.
- Attempt HTTP request with `Range: bytes=N-`.
- If server replies `206 Partial Content`, append to the `.part` file.
- If server replies `200 OK`, restart from scratch (server doesn’t support range or ignores it).

Integrity:
- For hashing, support incremental hashing:
  - If resuming, either:
    - recompute hash from file start (simpler; more IO), or
    - persist hash state (complex; probably not worth it on ESP32).

Recommendation:
- Recompute SHA-256 from the final `.part` once download completes (simple, robust).

### 3) Cache / De-duplication by Hash

For models:
- Store models under a cache directory keyed by hash:
  - `sd-card/models-cache/<sha256>/<original_name>`
- Then link/copy into `sd-card/config/<model>/...` as needed.

If symlinks are not viable on FAT:
- Use copy-once semantics:
  - If the target file already exists and its hash matches, skip.

For firmware ZIP:
- Keep the most recent N downloaded update artifacts by version/tag and hash:
  - `sd-card/firmware/cache/<version>/github_update.zip`
- Avoid overwriting the “current” update until install is scheduled.

### 4) UX and Progress

Expose progress:
- If the HTTP client provides content-length, show percentage.
- Otherwise show bytes downloaded.

Endpoints:
- Current `/ota` tasks can return structured JSON with:
  - `state`, `bytes_downloaded`, `bytes_total`, `hash_ok`, `error`

### 5) Cleanup and Storage Limits

Define policies:
- Max cache size (MB) and/or max number of versions.
- LRU cleanup when exceeding limit.
- User-triggered “clear cache” button.

### 6) Implementation Touch Points

- Download implementation: `code/components/jomjol_fileserver_ota/server_ota.cpp` (and/or shared download helper).
- SD file handling utilities.
- Web UI: `sd-card/html/ota_page.html`

## Test Plan

- Simulate disconnect mid-download; retry and verify resume works when server supports range.
- Power-cycle mid-download; verify `.part` file is not treated as valid and download can resume/restart.
- Corrupt a cached file; verify hash mismatch causes re-download.
- Verify cache eviction works and does not delete active/configured models.

## Rollout Plan

Phase 1:
- `.part` temp writes + post-download hash + rename.
Phase 2:
- HTTP range resume support.
Phase 3:
- Hash-keyed cache + eviction UI.

