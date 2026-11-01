# Signed OTA deployment and recovery

## Architecture and prerequisites

NeonOS keeps two application slots. The running image is never overwritten during an update. A trusted release contains the exact image's SHA-256 hash, monotonically increasing sequence, version, hardware ID, channel and immutable HTTPS image URL. Its RSA-3072 PSS/SHA-256 signature is verified before any firmware download. The device hashes the actual written partition and checks the embedded release identity before asking ESP-IDF to validate the application image and change the boot selector.

The updater is independent of HomeSpan. It uses the ESP-IDF HTTPS client and OTA partition primitives already supplied by Arduino-ESP32 3.3.12. Using the primitives directly keeps download length, redirect policy and boot selection explicit. TLS root/hostname verification is mandatory on every connection. The downloader follows at most four HTTPS redirects, including GitHub's latest-release and temporary asset URLs, with a bounded connection phase. It rejects HTTP downgrades, credentials, explicit ports, fragments and oversized redirect headers. Initial signed URLs are limited to 768 bytes; temporary redirect URLs may be up to 4096 bytes and are never saved in the release metadata. Relative redirects must start with a single slash. Use a trusted public CA. Valid device time is needed for TLS; SNTP starts non-blockingly once the updater task is launched.

The software currently accepts exactly the hardware ID `neon-esp32-wroom-4m` and channel `stable`. These are release-contract constants, not remote configuration. Keep `firmware-version.h`, the signing tool and the partition layout consistent.

**Not provisioned here:** an actual signing key, a published GitHub release, device secrets, a secure-boot root of trust, or an enrolled device. The example host and test keys are never a production deployment.

## GitHub Releases

Use the public repository **amansanoj/neon-os**. No custom domain, cloud account, bucket or always-running server is required. The stable endpoint is:

```text
https://github.com/amansanoj/neon-os/releases/latest/download/release.ota
```

Each published stable release must contain exactly named assets:

- `firmware.bin`: the signed manifest's application image, downloaded through a URL pinned to this release tag.
- `release.ota`: signed release metadata plus its signature, discovered through the latest-release link above.

Create a **draft** release, upload both assets, and publish it with **Set as latest release** enabled. Avoid prerelease/draft-only publication when deploying to the stable device. Keep published assets unchanged; use a new release tag and higher sequence for each approved image. GitHub may cache latest/download redirects, so discovery is not instantaneous.

The device uses public HTTPS download URLs, not the GitHub API, and contains no GitHub access token. Private release assets require a separate authentication design and are not supported by this configuration. Release signing remains independent of GitHub: hosting/write access alone cannot forge a valid new image without your signing key.

## Initial setup and first USB upload

Install the pinned dependencies from the README and install the signer dependencies on your release workstation:

```bash
python3 -m pip install -r tools/requirements.txt
mkdir -p "$HOME/.config/neon-os"
python3 tools/release.py keygen \
  --private-key "$HOME/.config/neon-os/signing.pem" \
  --public-key "$HOME/.config/neon-os/public.pem"
```

The private key is created exclusively with mode 0600 and is rejected if its path is inside the repository. Back it up securely. `NEON_SIGNING_KEY_PASSWORD`, when supplied through your password manager/environment, enables encrypted private-key generation and loading. No key/password should be pasted into chat, Git, CI logs or the firmware. For a larger deployment, move signing to a protected signing service/HSM; its signatures must use RSA-3072 PSS/SHA-256 with MGF1-SHA256 and a 32-byte salt.

Configure only the **public** key and your actual stable URL:

```bash
python3 tools/release.py configure \
  --public-key "$HOME/.config/neon-os/public.pem" \
  --github-repo amansanoj/neon-os
```

This selects the stable GitHub Releases endpoint and creates ignored `ota-config.local.h`; it refuses to overwrite a prior trust configuration. Every future build needs this configuration. Keep a backup of the public configuration with your deployment records. `ota-config.example.h` documents the format for Arduino IDE users.

Set the version/sequence in `firmware-version.h`. The initial rewrite uses version `2.0.0`, sequence `2000000`. Build on Linux:

```bash
python3 tools/build.py --production
```

Arduino IDE users can build/upload the same sketch with the pinned libraries, local configuration, 4 MB flash, PSRAM disabled and **No FS 4MB (2MB APP x2)**. The checked-in `partitions.csv` is authoritative. Keep **Erase All Flash: Disabled**. This layout retains the existing NVS addresses.

Sign the **application image only**:

```bash
python3 tools/release.py sign \
  --private-key "$HOME/.config/neon-os/signing.pem" \
  --image dist/neon-os.ino.bin \
  --github-repo amansanoj/neon-os
python3 tools/release.py verify \
  --public-key "$HOME/.config/neon-os/public.pem" \
  --manifest dist/release-2000000/manifest.txt \
  --image dist/release-2000000/firmware.bin
```

The signer rejects reused output directories, non-ESP32 images, images with mismatched release identity and images larger than the slot. It does not replace the ESP-IDF image verifier or the need for hardware testing.

The tool reads sequence/version from `firmware-version.h`, defaults the tag to `vVERSION` (initially `v2.0.0`), and writes `dist/release-SEQUENCE/`. Optional `--tag`, `--sequence`, `--version` and `--out` support explicit release preparation; mismatched binary identity is rejected.

Commit and push the tested source before creating its release tag; the tag should point to the source used for the build. Open GitHub → amansanoj/neon-os → Releases → Draft a new release. Select/create tag **v2.0.0**, attach **firmware.bin** and **release.ota** from `dist/release-2000000/`, select **Set as latest release**, and publish. The image URL in the signed envelope points to this exact tag. The other output files are useful for local auditing and do not need to be attached. **The local signing commands do not publish a GitHub release.**

Then upload the configured sketch once over USB. Confirm serial output shows the intended version/identity and OTA health confirmation after successful endpoint verification and at least 60 seconds of healthy operation. This initial server release can have the same sequence as the USB firmware: it proves endpoint/trust continuity without triggering a download.

## Subsequent releases

1. Change the application and increment the sequence; never reuse it for different binaries. Set an appropriate version string.
2. Run all native tests, build the configured production image, inspect flash/RAM usage and test on a spare or accessible device.
3. Sign the application using `--github-repo amansanoj/neon-os`; verify it offline. The tool derives a version-specific GitHub image URL.
4. Draft a new GitHub Release with the matching tag, attach both generated assets, then publish it as latest after testing and approval.
5. The device discovers the release on its next check, downloads to the inactive slot, verifies it and reboots. Controls are briefly unavailable during reboot; flash writes/TLS can also affect latency and must be measured on hardware.
6. Confirm the advertised HomeSpan sketch version and serial health/OTA logs. Never equate a completed upload with an accepted, healthy new boot.

Normal checks are about 15 minutes apart with jitter. Initial/retry checks use a 10-second starting delay and backoff. There is no inbound Arduino IDE OTA server; selecting a network port in the IDE is not this release workflow. A Git push alone does not deploy firmware. CI builds/tests, and a separate release operator controls signing/promotion.

A deliberate source rollback should be built and approved with a **new, higher sequence**. The application keeps a software version floor in NVS; this is not eFuse anti-rollback and is not protected against physical flash erasure.

## Trial boots, power failure and recovery

- An interrupted/failed download leaves the currently running slot selected. The inactive slot can be retried on a later check.
- The pending sequence is journaled before the boot selector is changed. Power loss in that narrow interval can conservatively quarantine a release that had not yet booted; publishing a new higher sequence permits recovery.
- `SpanRollback.h` defers Arduino's automatic early image validation. An OTA trial must pass 60 continuous seconds of application health and verify a signed release envelope from the configured endpoint.
- The trial deadline is five minutes. Failure invokes rollback. A main-loop hang is watched by HomeSpan's 20-second watchdog; BLE and update workers have 120-second progress limits. Reboots before trial acceptance let the rollback-enabled bootloader select the previous image.
- Missing OLED hardware and a disconnected light do not themselves fail the trial. Network/server outages during a trial can cause a conservative rollback; investigate the endpoint/certificate/time path before republishing.
- If a candidate is rolled back, the old application quarantines its pending sequence instead of repeatedly downloading and booting it. Publish a corrected higher-sequence release, or recover over USB after diagnosing the failure.
- Once an image is accepted, later hangs trigger a restart of that accepted image; this is **not** an automatic rollback of every future runtime failure. A fallback slot also does not protect against loss of both valid images, severe flash corruption, an overwritten partition table or a wrong bootloader.
- Before v2, the old firmware does not know this health protocol. Install the first v2 build over USB, then test an actual OTA between v2 builds before relying on rollback.

Preserve an accessible USB/programming connection. This system cannot guarantee recovery from arbitrary replacement firmware that removes its updater or bypasses its health protocol. Approved releases must retain both.

## Signing key and TLS trust rotation

The normal configuration trusts one RSA-3072 public key. `tools/release.py configure --next-public-key NEXT.pem ...` can additionally enroll the next key as `NEON_OTA_NEXT_PUBLIC_KEY_PEM`.

Rotate in stages: first publish a release signed by the old key that trusts both old and new keys; confirm its acceptance. Then promote releases signed by the new key. Finally publish a new-key-signed release that removes the old key. Keep sequence numbers increasing. Test the sequence on an accessible device. Removing the old key before the endpoint is signed with the new key causes the trial to fail endpoint verification and roll back.

TLS trust uses the bundled public roots and certificate hostname/expiry checks. Maintain time synchronization and certificate renewal. Test endpoint/root changes while the old release still works; a permanently unreachable endpoint cannot deliver its own fix. Never use insecure TLS options to bypass an expired or mismatched certificate.

## Hardware security

Signed manifests authenticate the image bytes over HTTPS; **Secure Boot and flash encryption are not enabled by this rewrite**. The application cannot protect its verifier from arbitrary physical reflashing. The reported ESP32 revision 3.01 can support Secure Boot V2 when built/provisioned appropriately. That requires a separately controlled bootloader/build/signing process, an eFuse/provisioning plan and tested recovery access. Do not burn eFuses or disable UART recovery as part of an ordinary update. Security-version anti-rollback also constrains which older images can be recovered; it is different from normal application version sequencing.

## Hardware acceptance

Do not call the device ready for permanent mounting until these are exercised on the actual board:

- Two successful OTA upgrades between distinct sequences, plus a deliberate newer-sequence source rollback. Verify HomeKit pairing, exact RGB state, brightness, orientation and sleep preferences across boots.
- Rejection of wrong-key, altered, wrong-target, oversized and incomplete images while the current application keeps operating.
- Power cuts and Wi-Fi loss at multiple download points, immediately before/after boot selection, and during trial boot. Confirm the selected slot and actual rollback with serial partition/boot-state evidence.
- A trial that crashes, a trial that deadlocks the main task, a stalled BLE worker and an unreachable OTA endpoint. Verify recovery behavior and failed-release quarantine, without a perpetual reboot/update loop.
- Measure free/minimum heap, largest free block, main/OTA/BLE stack headroom, control latency and TLS failures while Apple Home and BLE are busy during download. The low-heap guard must defer gracefully. Static RAM figures alone do not establish TLS headroom.
- At least a 72-hour soak with rapid HomeKit changes, BLE power cycling, Wi-Fi/AP interruption, OLED sleep/wake/rotation and intermittent I²C failure. Extend the soak for the deployment's risk. Host rollover tests do not prove physical multi-week operation.
- Verify stored-state recovery after hard power loss, NVS-full/write-failure behavior, and backwards-compatible settings migrations.
- Confirm stable power, mechanically secured connections, antenna clearance, certificate/clock recovery and USB accessibility in the final mounted location.
