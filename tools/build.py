#!/usr/bin/env python3
"""Reproducible ESP32 build, slot budget and release identity checks."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools-dir', type=Path,
                        default=os.environ.get('NEON_TOOLS_DIR', '/workspace/.neon-tools'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--production', action='store_true')
    args = parser.parse_args()
    if args.production:
        config = ROOT / 'ota-config.local.h'
        if not config.exists() or 'example.com' in config.read_text():
            parser.error('Configure your real HTTPS endpoint and public key before a production build')
    cli = args.tools_dir / 'bin/arduino-cli'
    command = [str(cli), '--config-file', str(args.tools_dir / 'arduino-cli.yaml'),
               'compile', '--fqbn', 'esp32:esp32:esp32:FlashSize=4M,PSRAM=disabled,PartitionScheme=no_fs',
               '--jobs', str(min(4, os.cpu_count() or 1)), '--build-path', str(args.build_dir.resolve()),
               '--export-binaries', '--output-dir', str(ROOT / 'dist'), str(ROOT)]
    subprocess.run(command, cwd=ROOT, check=True)
    image = (ROOT / 'dist/neon-os.ino.bin').read_bytes()
    header = (ROOT / 'firmware-version.h').read_text()
    version = re.search(r'#define NEON_FIRMWARE_VERSION "([^"]+)"', header).group(1)
    sequence = int(re.search(r'#define NEON_FIRMWARE_SEQUENCE (\d+)', header).group(1))
    marker = f'NEON_RELEASE_ID|neon-esp32-wroom-4m|{sequence}|{version}|stable|'.encode()
    if marker not in image or len(image) > 0x1F0000:
        sys.exit('Image identity is missing or the image exceeds its OTA slot')
    print(f'Verified firmware {version}, sequence {sequence}, {len(image)} bytes; '
          f'{0x1F0000-len(image)} bytes free in each slot')


if __name__ == '__main__':
    main()
