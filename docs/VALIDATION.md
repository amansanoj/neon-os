# Validation record

Validated in the cloud workspace on 2026-11-01. No hardware was connected, no production endpoint was provisioned, and no firmware was flashed or pushed.

## Host checks

- `python3 tests/run_native.py`: **23 scenarios passed** (controls, rendering, HomeKit input validation, BLE worker/retry simulations and state persistence/migration).
- `python3 tests/run_security.py` with locally built, checksum-pinned Mbed TLS 3.6.6: **24 updater fault scenarios plus the manifest/runtime policy suite passed**. The tests execute the actual updater source with mocked IDF transport/flash/NVS and actual RSA-PSS/SHA-256 verification. Coverage includes trust-key rotation, failed validation journal writes, transfer deadlines and boot-selection failures.
- The compiled ESP32 application was signed and verified through the offline release CLI with a disposable key. Invalid release identities/sizes and an example endpoint in a production build are rejected.
- Pinned Arduino/core/library setup and native Mbed TLS setup both ran successfully; repeat setup succeeded. Python compilation, shell syntax, repository diff whitespace and local documentation links were checked.
- GitHub Actions workflow was written and its pinned setup action resolved, but the workflow itself has not run on GitHub.

## ESP32 builds

Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5, ESP32 Dev Module, 240 MHz defaults, 4 MB flash, no PSRAM, checked-in No FS two-slot partition layout. Each slot is 2,031,616 bytes.

| Profile | Program storage reported | Exported application image | Static RAM reported |
| --- | --- | --- | --- |
| Configured disposable OTA test profile | 1,912,911 bytes (94%) | 1,913,056 bytes; 118,560 bytes remaining | 74,272 bytes (22%) |
| Default profile, no trust configuration | 1,905,483 bytes (93%) | 1,905,632 bytes; 125,984 bytes remaining | 74,264 bytes (22%) |

The disposable trust header was removed after testing. The default checkout disables network updates until a real endpoint/public key is configured. Build timestamps can cause small artifact differences. Static RAM and host tests do not establish real TLS peak memory, stack sufficiency, RF behavior, flash power-loss safety or prolonged runtime stability.

Complete the hardware acceptance procedure in [OTA.md](OTA.md#hardware-acceptance) before permanent installation. Hardware Secure Boot/flash-encryption provisioning and a cloud release account/domain remain outside this validation.

## GitHub Releases adaptation

The endpoint now uses `https://github.com/amansanoj/neon-os/releases/latest/download/release.ota`. The downloader follows at most four verified HTTPS redirects and supports long temporary asset URLs without persisting them. A live public GitHub release-asset HEAD request confirmed a redirect to `release-assets.githubusercontent.com` with a 956-byte URL and final HTTP 200; this was a representative Arduino CLI asset, not a published NeonOS release or a device OTA test.

The security suite now includes six additional GitHub redirect scenarios: metadata and image success, HTTP downgrade rejection, loop limit, protocol-relative rejection and oversized Location rejection. Together with the unchanged controls/persistence checks, **54 host checks pass**. The GitHub signing helper inferred the version/sequence/tag and signed/verified the actual configured ESP32 application offline. No release was published.

The configured GitHub test build reports 1,913,787 bytes of program storage (94%), an exported application of 1,913,936 bytes (117,680 bytes remaining), and 74,272 bytes static RAM (22%). The disposable trust header was removed after compilation.

## Git-only release publication

Added thirteen passing host tests for trigger/retry mapping, staged signed-bundle verification, tag/repository matching, public-only copying, idempotent staging, draft-before-publish ordering, interrupted-upload recovery, immutable published assets, private-repository rejection, digest mismatch and API credential redirect/error handling. Existing signing/updater security tests also pass through `uv run`. No production release was created in the cloud workspace.

Both Actions workflows passed actionlint 1.7.7 after correcting an invalid job-level runner context in the earlier validation workflow. The new publisher uses an `ota-vVERSION` trigger and a built-in GitHub Actions token; it does not require a personal token on the Mac. Live publication remains to be exercised by pushing the user's staged signed bundle.

## Hosted-runner proxy correction

The first release run reached the toolchain installation step after all native/security/publication tests passed. The runner then failed because an empty `ARDUINO_NETWORK_PROXY` was exported: Arduino CLI interpreted it as a proxy at `:0`. Setup now leaves that override unset for direct networking and preserves/reuses a configured session proxy when present. A regression test covers eight proxy-environment combinations; fourteen publication/setup unit tests pass in total. No firmware or signed release bytes changed.
