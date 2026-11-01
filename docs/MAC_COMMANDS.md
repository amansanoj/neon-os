# Mac: first installation and future releases

Run commands from the updated `neon-os` project folder. This uses GitHub Releases, not a custom server. The initial firmware is `2.0.0`, sequence `2000000`.

## 1. Get the source and tools once

If you already have a clone, use `git pull --ff-only origin main` from that folder. Keep any local edits safe; do not reset/discard them to make a pull succeed. Otherwise, from a directory where you want the project:

```bash
git clone https://github.com/amansanoj/neon-os.git
cd neon-os
```

With Homebrew installed:

```bash
brew install python@3.12 arduino-cli gh
python3.12 -m venv "$HOME/.venvs/neon-os"
source "$HOME/.venvs/neon-os/bin/activate"
python -m pip install -r tools/requirements.txt

git config user.name "Aman Sanoj"
git config user.email "72658023+amansanoj@users.noreply.github.com"
gh auth login
```

Choose GitHub.com, HTTPS and browser login. Allow Git authentication when prompted. Credentials stay on your Mac; none go into the ESP32.

Install the pinned board package and libraries:

```bash
NEON_BOARD_INDEX='https://espressif.github.io/arduino-esp32/package_esp32_index.json'
arduino-cli core update-index --additional-urls "$NEON_BOARD_INDEX"
arduino-cli core install esp32:esp32@3.3.12 --additional-urls "$NEON_BOARD_INDEX"
arduino-cli lib install 'HomeSpan@2.1.8' 'NimBLE-Arduino@2.3.7' 'Adafruit BusIO@1.17.4' 'Adafruit GFX Library@1.13.0' 'Adafruit SSD1306@2.5.17'
```

The Linux/cloud `tools/setup.sh` is not the Mac installer. The direct Arduino CLI commands above use the appropriate Mac toolchain. The build/export/upload flags were checked against the installed Arduino CLI; macOS/Homebrew installation itself has not run in the Linux cloud workspace.

## 2. Create your signing key once

Create a directory outside the repository:

```bash
mkdir -p "$HOME/.config/neon-os"
```

Run the next line **on its own**, type a strong signing-key password (input is hidden), and press Enter:

```bash
read -r -s NEON_SIGNING_KEY_PASSWORD
```

Then:

```bash
export NEON_SIGNING_KEY_PASSWORD
python tools/release.py keygen \
  --private-key "$HOME/.config/neon-os/signing.pem" \
  --public-key "$HOME/.config/neon-os/public.pem"
python tools/release.py configure \
  --public-key "$HOME/.config/neon-os/public.pem" \
  --github-repo amansanoj/neon-os
```

These commands refuse to replace existing keys/configuration. If you have already created yours, reuse them instead of generating replacements. Back up the private key, password and public configuration securely. Never commit/upload the private key. `ota-config.local.h` is generated, ignored by Git and needed in every build.

## 3. Rebuild and sign

In each fresh Terminal session, enter the project folder and activate the virtual environment. Set the board variable:

```bash
source "$HOME/.venvs/neon-os/bin/activate"
NEON_BOARD='esp32:esp32:esp32:FlashSize=4M,PSRAM=disabled,PartitionScheme=no_fs'
```

If the password is not already exported in this session, run the hidden `read` line from step 2 by itself again, then `export NEON_SIGNING_KEY_PASSWORD`.

Build and sign:

```bash
arduino-cli compile --fqbn "$NEON_BOARD" --jobs 4 --export-binaries --output-dir dist .
python tools/release.py sign \
  --private-key "$HOME/.config/neon-os/signing.pem" \
  --image dist/neon-os.ino.bin \
  --github-repo amansanoj/neon-os
```

Read version/sequence directly from the source so subsequent commands cannot accidentally use a stale release folder:

```bash
NEON_VERSION=$(python -c 'from tools.release import source_version; print(source_version()[1])')
NEON_SEQUENCE=$(python -c 'from tools.release import source_version; print(source_version()[0])')
NEON_TAG="v$NEON_VERSION"
python tools/release.py verify \
  --public-key "$HOME/.config/neon-os/public.pem" \
  --manifest "dist/release-$NEON_SEQUENCE/manifest.txt" \
  --image "dist/release-$NEON_SEQUENCE/firmware.bin"
```

The signer refuses an existing release output folder. Preserve earlier signed outputs; choose a new version/sequence for a different binary. Only remove a disposable local output deliberately when rebuilding a release that has never been approved/published.

## 4. Commit/push code and publish a release

For the first `2.0.0` release, the firmware code is already committed/pushed by Codex. For later edited versions, review and push your source first:

```bash
git diff --check
git diff --stat
git add -A
git commit -m "Release $NEON_TAG"
git push origin main
```

A Git push runs validation; it does not update the device. Check the GitHub Actions result for the source you're releasing:

```bash
gh run list --repo amansanoj/neon-os --workflow validate.yml --branch main --limit 1
```

After tests pass, tag the exact local commit used for your build. Do not reuse a published tag:

```bash
git tag -a "$NEON_TAG" -m "NeonOS $NEON_VERSION"
git push origin "$NEON_TAG"
```

Create a **draft** release with both assets, then publish it as latest:

```bash
gh release create "$NEON_TAG" \
  "dist/release-$NEON_SEQUENCE/firmware.bin" \
  "dist/release-$NEON_SEQUENCE/release.ota" \
  --repo amansanoj/neon-os --verify-tag --draft \
  --title "NeonOS $NEON_VERSION" --notes "Signed OTA firmware release."
gh release edit "$NEON_TAG" --repo amansanoj/neon-os --draft=false --latest
```

This requires a public repository and your GitHub login's permission to create releases. It uploads only the public application and signed metadata. The ESP32's stable URL is:

```text
https://github.com/amansanoj/neon-os/releases/latest/download/release.ota
```

## 5. First USB installation only

The existing pre-v2 firmware cannot install its own first OTA-enabled replacement. Publish the initial signed release above, connect the ESP32 to USB and find its port:

```bash
arduino-cli board list
```

Replace `/dev/cu.YOUR_PORT` below with the actual port shown:

```bash
arduino-cli upload --port /dev/cu.YOUR_PORT --fqbn "$NEON_BOARD" --input-dir dist .
```

This uploads the already built image. Keep flash erasure disabled. Use Arduino IDE Serial Monitor at 115200 baud to provision Wi-Fi/HomeKit if necessary and check `[OTA] Application health confirmed`. Existing pairing/settings are intended to survive; hardware acceptance is still required.

## 6. Every later update

1. Make your firmware edits.
2. Change **both** release fields in `firmware-version.h`, for example:

```cpp
#define NEON_FIRMWARE_VERSION "2.0.1"
#define NEON_FIRMWARE_SEQUENCE 2000001
```

Always increase the sequence; use a fresh version/tag for the next approved binary. A deliberate return to older source also needs a new sequence/tag.

3. Repeat steps 3 and 4: build, sign, verify, commit/push, check CI, tag, create/upload draft, publish latest.
4. The running device normally checks within about 15 minutes, downloads the signed image, verifies it and reboots. GitHub caching can add delay. No USB upload is needed for these updates.

Confirm the actual new version/health before permanent mounting. See [OTA.md](OTA.md#hardware-acceptance) for rollback, interruption, memory and soak tests. No release was automatically published by Codex; these commands are run on your Mac using your real key.
