#!/usr/bin/env bash
set -euo pipefail
# Keep dependencies outside the checkout. Set NEON_TOOLS_DIR on other machines.
NEON_TOOLS_DIR="${NEON_TOOLS_DIR:-/workspace/.neon-tools}"
mkdir -p "$NEON_TOOLS_DIR/bin" "$NEON_TOOLS_DIR/downloads"
export NEON_TOOLS_DIR
curl -fsSL https://github.com/arduino/arduino-cli/releases/download/v1.3.1/arduino-cli_1.3.1_Linux_64bit.tar.gz -o "$NEON_TOOLS_DIR/downloads/cli.tar.gz"
curl -fsSL https://github.com/arduino/arduino-cli/releases/download/v1.3.1/1.3.1-checksums.txt -o "$NEON_TOOLS_DIR/downloads/checksums.txt"
python3 - <<'PY'
import hashlib, os, pathlib, tarfile
p=pathlib.Path(os.environ['NEON_TOOLS_DIR'])
expected=next(line.split()[0] for line in (p/'downloads/checksums.txt').read_text().splitlines()
              if line.endswith('arduino-cli_1.3.1_Linux_64bit.tar.gz'))
assert hashlib.sha256((p/'downloads/cli.tar.gz').read_bytes()).hexdigest()==expected
with tarfile.open(p/'downloads/cli.tar.gz') as archive:
    archive.extract('arduino-cli',p/'bin',filter='data')
# JSON is a YAML subset and safely quotes paths with spaces.
import json
(p/'arduino-cli.yaml').write_text(json.dumps({
    'board_manager':{'additional_urls':['https://espressif.github.io/arduino-esp32/package_esp32_index.json']},
    'directories':{'data':str(p/'data'),'downloads':str(p/'staging'),'user':str(p/'user')}}))
PY
export ARDUINO_NETWORK_PROXY="${HTTPS_PROXY:-${https_proxy:-}}"
cli() { "$NEON_TOOLS_DIR/bin/arduino-cli" --config-file "$NEON_TOOLS_DIR/arduino-cli.yaml" "$@"; }
cli core update-index
cli core install esp32:esp32@3.3.12
cli lib install 'HomeSpan@2.1.8' 'NimBLE-Arduino@2.3.7' 'Adafruit BusIO@1.17.4' 'Adafruit GFX Library@1.13.0' 'Adafruit SSD1306@2.5.17'
