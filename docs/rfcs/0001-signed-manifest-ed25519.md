# RFC 0001: Signed Manifest Verification (Ed25519)

## Status

Draft

## Context / Problem

We currently support “online update” and “on-demand model downloads” by fetching a JSON manifest over HTTPS and verifying SHA-256 hashes of the referenced artifacts. This protects integrity *assuming* the manifest itself is trustworthy.

The missing piece is **authenticity** of the manifest: if an attacker can serve a different manifest (or compromise the hosting account/repo), SHA-256 verification does not help because the attacker can provide matching hashes for malicious artifacts.

## Goals

- Add cryptographic authenticity for the update/model manifest.
- Keep the runtime implementation small and robust on ESP32.
- Keep the release publishing process reproducible and automated.
- Support changing “update channel” / repo owner (forks) without recompiling when possible.

## Non-goals

- Full secure-boot chain design (ESP32 Secure Boot / Flash Encryption).
- Preventing a malicious actor with physical access (SD replacement, etc.).
- Differential updates or transport changes (handled in separate RFCs).

## Proposed Design

### 1) Manifest Signature Format

- Continue publishing `manifest.json` (as today).
- Publish a detached signature file `manifest.json.sig` (or `manifest.sig`) created with **Ed25519** over the exact bytes of the manifest.
- The firmware verifies:
  1. It fetched the manifest over HTTPS (as now).
  2. The Ed25519 signature is valid for `manifest.json` under a configured public key.
  3. The referenced artifact hashes match (as now).

Rationale:
- Detached signature keeps JSON unchanged and avoids canonicalization pitfalls.
- Ed25519 is fast, small, and widely supported.

### 2) Canonicalization / Determinism

To avoid JSON canonicalization complexity:
- The signature is computed over the exact file bytes published as `manifest.json`.
- The publishing workflow generates `manifest.json` in a deterministic way (stable key order, stable whitespace).

Implementation requirement:
- The generator script must write `manifest.json` deterministically (e.g. Python `json.dumps(..., sort_keys=True, separators=(',', ':'))` and newline policy).

### 3) Key Management

Firmware needs at least one pinned public key.

Options:
- **A. Compile-time pinned key** (simplest): public key in firmware image.
- **B. Configurable key on SD** (more flexible): `sd-card/config/ota_public_key.txt` (or similar), loaded at boot.
- **C. Both**: compile-time default key, optionally overridden by SD config (useful for forks/testing).

Recommended:
- Option C: compile-time default + optional SD override gated by a config flag to reduce accidental downgrade in security posture.

### 4) Multi-Channel Support (Forks / Alternate Repos)

Introduce a concept of “channel”:
- A base URL for manifests (e.g. GitHub Pages URL).
- An associated Ed25519 public key.

Storage:
- `sd-card/config/ota_channels.json` with entries like:
  - `name`
  - `manifest_base_url`
  - `public_key_ed25519` (base64)
  - `enabled`

UI:
- OTA page displays current channel and allows switching (with warnings).

### 5) Firmware Verification Path

Add verification to the existing online update/model fetch flow:
- After downloading `manifest.json`, download the signature file next to it.
- Verify signature before accepting manifest contents.
- If verification fails, abort and surface a clear error.

Target code areas:
- `code/components/jomjol_fileserver_ota/server_ota.cpp` (manifest fetch handling).
- Any manifest parsing utility used by `/ota?task=download_update` and `/ota?task=download_model`.

### 6) Publishing Workflow Changes

Update the GitHub Pages workflow to:
- Generate deterministic `manifest.json`.
- Create `manifest.json.sig` with a private signing key stored in GitHub Actions secrets.
- Publish both to Pages.

Key handling in Actions:
- Store Ed25519 private key as a secret (base64 or armored).
- Ensure logs do not print key material.
- Ensure the workflow only signs when building from trusted refs (e.g. tags/releases).

### 7) Backward Compatibility

Add a configuration flag:
- `require_signed_manifest = true` by default for new installs.
- If set to false, allow legacy behavior (hash-only) for development.

UI must clearly indicate the security mode and default to safe settings.

## Security Considerations

- Signature verification prevents manifest tampering and unauthorized manifests.
- Artifact hash verification continues to protect integrity for ZIP/model downloads.
- If SD-based public key override exists, document that physical SD access can bypass it (unless secure boot + encrypted storage).
- Mitigate downgrade attacks by versioning the manifest schema and disallowing older schema versions unless explicitly enabled.

## Implementation Steps (Suggested)

1. Decide signature file name and encoding (raw 64-byte, base64, or hex).
2. Update `tools/github-pages/generate_manifest.py` to output deterministic JSON.
3. Add signing step + publish `manifest.json.sig` in `.github/workflows/publish-pages-manifest.yaml`.
4. Add Ed25519 verify code on-device (library choice: `mbedtls` if available, or a small Ed25519 implementation).
5. Add config + UI for channel/key selection (optional in phase 2).
6. Add regression tests for:
   - valid signature accepted
   - invalid signature rejected
   - modified manifest rejected

## Test Plan

- Unit-level (host) tests for signature verification if feasible in repo tooling.
- On-device manual validation:
  - Verify update and model download succeed with valid signature.
  - Corrupt signature or manifest and verify device rejects with clear error.

## Rollout Plan

- Phase 1: Sign manifests for `main` release channel; device requires signed manifests by default.
- Phase 2: Add multi-channel support for forks/testing.
- Phase 3: Consider key rotation support (multiple public keys accepted with explicit expiration).

