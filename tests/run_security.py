#!/usr/bin/env python3
"""Actual updater fault tests using mocked IDF I/O and real Mbed TLS 3.6.6."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release', ROOT / 'tools/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mbedtls-include', type=Path,
                        default=os.environ.get('MBEDTLS_INCLUDE_DIR', '/usr/include'))
    parser.add_argument('--mbedtls-lib-dir', type=Path,
                        default=os.environ.get('MBEDTLS_LIBRARY_DIR'))
    args = parser.parse_args()
    if not (args.mbedtls_include / 'mbedtls/pk.h').exists():
        parser.error('Mbed TLS 3.6.6 headers required; set MBEDTLS_INCLUDE_DIR')
    with tempfile.TemporaryDirectory(prefix='neon-security-') as directory:
        tmp = Path(directory)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                    serialization.NoEncryption())
        private_path = tmp / 'signing.pem'
        private_path.write_bytes(private)
        private_path.chmod(0o600)
        pem = key.public_key().public_bytes(serialization.Encoding.PEM,
                                           serialization.PublicFormat.SubjectPublicKeyInfo)
        next_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        next_pem = next_key.public_key().public_bytes(serialization.Encoding.PEM,
                    serialization.PublicFormat.SubjectPublicKeyInfo)
        (tmp / 'public.pem').write_bytes(pem)
        image = bytearray(4096)
        image[0] = 0xE9
        image[32:36] = bytes.fromhex('3254cdab')
        marker = release.identity(2000001, '2.0.1')
        image[1000:1000+len(marker)] = marker  # Cross the hashing block boundary.
        (tmp / 'image.bin').write_bytes(image)
        subprocess.run(['python3', str(ROOT / 'tools/release.py'), 'sign',
                        '--private-key', str(private_path), '--image', str(tmp / 'image.bin'),
                        '--sequence', '2000001', '--version', '2.0.1',
                        '--github-repo', 'amansanoj/neon-os',
                        '--out', str(tmp / 'release')], check=True)
        subprocess.run(['python3', str(ROOT / 'tools/release.py'), 'verify',
                        '--public-key', str(tmp / 'public.pem'),
                        '--manifest', str(tmp / 'release/manifest.txt'),
                        '--image', str(tmp / 'release/firmware.bin')], check=True)
        # Wrong target/version/slot size must fail before a release is signed.
        for bad_image, seq, version in [(b'bad',2000001,'2.0.1'),
                (image,2000002,'2.0.1'), (image,2000001,'2.0.2'),
                (image+bytes(release.SLOT_SIZE),2000001,'2.0.1')]:
            try:
                release.manifest(bad_image,seq,version,'https://updates.example.com/app.bin')
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid release accepted')
        body = (tmp / 'release/manifest.txt').read_bytes()
        (tmp / 'release/release-next.ota').write_bytes(body+next_key.sign(body,release.PSS,hashes.SHA256()))
        wrong = rsa.generate_private_key(public_exponent=65537,key_size=3072)
        (tmp / 'release/wrong-key.ota').write_bytes(body+wrong.sign(body,release.PSS,hashes.SHA256()))
        command = ['g++' , '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                   '-Wno-unused-function', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                   '-I', str(ROOT / 'tests/ota-mocks'), '-I', str(ROOT),
                   '-I', str(args.mbedtls_include),
                   '-DNEON_OTA_MANIFEST_URL="https://github.com/amansanoj/neon-os/releases/latest/download/release.ota"',
                   '-DNEON_OTA_PUBLIC_KEY_PEM=' + json.dumps(pem.decode()),
                   '-DNEON_OTA_NEXT_PUBLIC_KEY_PEM=' + json.dumps(next_pem.decode()),
                   str(ROOT / 'tests/ota_transport.cpp'), '-l:libmbedcrypto.so.16',
                   '-o', str(tmp / 'ota-tests')]
        if args.mbedtls_lib_dir:
            command += ['-L', str(args.mbedtls_lib_dir),
                        '-Wl,-rpath,' + str(args.mbedtls_lib_dir.resolve())]
        subprocess.run(command, check=True)
        scenarios = ['success', 'bad-signature', 'bad-manifest', 'oversize-signature',
                     'http-redirect', 'truncated-manifest', 'bad-image', 'truncated-image',
                     'journal-fails', 'finish-fails', 'low-heap', 'oversize-image',
                     'same-partition', 'trial-healthy', 'trial-fails', 'quarantine',
                     'missing-trust-journal', 'rotation-next', 'wrong-key',
                     'flash-write-fails', 'boot-select-fails', 'transfer-timeout',
                     'image-redirect', 'trial-journal-fails', 'github-manifest', 'github-image',
                     'redirect-downgrade', 'redirect-loop', 'redirect-relative-host', 'redirect-too-long']
        for scenario in scenarios:
            subprocess.run([str(tmp / 'ota-tests'),str(tmp / 'release'),scenario],
                           check=True,timeout=10)
            print('PASS ota-'+scenario,flush=True)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I', str(ROOT),
                        str(ROOT / 'tests/policy_regression.cpp'), '-o', str(tmp / 'policy')], check=True)
        subprocess.run([str(tmp / 'policy')],check=True)
        print('PASS manifest-policy-and-runtime',flush=True)


if __name__ == '__main__':
    run()
