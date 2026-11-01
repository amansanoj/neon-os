#!/usr/bin/env python3
"""Offline release signer. Private keys never belong on devices or in Git."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

ROOT = Path(__file__).resolve().parents[1]
HARDWARE = 'neon-esp32-wroom-4m'
CHANNEL = 'stable'
SLOT_SIZE = 0x1F0000
PSS = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32)


def https_url(value):
    if len(value) > 768 or not value.startswith('https://'):
        raise ValueError('A direct HTTPS URL is required')
    parsed = urlsplit(value)
    if (not parsed.netloc or not re.fullmatch(r'[A-Za-z0-9.-]+', parsed.netloc)
            or parsed.netloc.startswith('.') or parsed.netloc.endswith('.')
            or not parsed.path or parsed.fragment
            or any(ord(c) <= 32 or ord(c) >= 127 or c in '\\"' for c in value)):
        raise ValueError('URL must use a DNS hostname, no credentials/port/fragment, and a path')
    return value


def github_repo(value):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', value):
        raise argparse.ArgumentTypeError('Expected GitHub owner/repository')
    return value


def source_version():
    header = (ROOT / 'firmware-version.h').read_text()
    sequence = int(re.search(r'#define NEON_FIRMWARE_SEQUENCE (\d+)', header).group(1))
    version = re.search(r'#define NEON_FIRMWARE_VERSION "([^"]+)"', header).group(1)
    return sequence, version


def identity(sequence, version):
    if not 0 < sequence <= 0xFFFFFFFF:
        raise ValueError('Sequence must be a nonzero uint32')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', version):
        raise ValueError('Version must be 1-31 safe ASCII characters')
    return f'NEON_RELEASE_ID|{HARDWARE}|{sequence}|{version}|{CHANNEL}|'.encode()


def manifest(image, sequence, version, url):
    marker = identity(sequence, version)
    https_url(url)
    if not image or len(image) > SLOT_SIZE:
        raise ValueError('Image does not fit an OTA slot')
    # ESP32 application image, not bootloader, merged flash, or another chip.
    if len(image) < 288 or image[0] != 0xE9 or image[12:14] != b'\0\0':
        raise ValueError('Expected an ESP32 application .bin')
    if image[32:36] != bytes.fromhex('3254cdab') or marker not in image:
        raise ValueError('Image metadata/NeonOS identity does not match the requested release')
    text = (f'NEON-OTA-1\n{HARDWARE}\n{CHANNEL}\n{sequence}\n{version}\n'
            f'{len(image)}\n{hashlib.sha256(image).hexdigest()}\n{url}\n')
    data = text.encode('ascii')
    if len(data) > 1536:
        raise ValueError('Manifest is too large')
    return data


def public_key(path):
    key = serialization.load_pem_public_key(Path(path).read_bytes())
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        raise ValueError('Only RSA-3072 public keys are supported')
    return key


def private_key(path):
    resolved = Path(path).resolve()
    if resolved.is_relative_to(ROOT):
        raise ValueError('Keep the signing private key outside the repository')
    secret = os.environ.get('NEON_SIGNING_KEY_PASSWORD')
    key = serialization.load_pem_private_key(resolved.read_bytes(),
                                            password=secret.encode() if secret else None)
    if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 3072:
        raise ValueError('Only RSA-3072 private keys are supported')
    return key


def exclusive_write(path, data, mode=0o644):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, mode)
    with os.fdopen(fd, 'wb') as output:
        output.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    keys = sub.add_parser('keygen')
    keys.add_argument('--private-key', type=Path, required=True)
    keys.add_argument('--public-key', type=Path, required=True)
    config = sub.add_parser('configure')
    config.add_argument('--public-key', type=Path, required=True)
    target = config.add_mutually_exclusive_group(required=True)
    target.add_argument('--manifest-url', type=https_url)
    target.add_argument('--github-repo', type=github_repo)
    config.add_argument('--next-public-key', type=Path)
    sign = sub.add_parser('sign')
    sign.add_argument('--private-key', type=Path, required=True)
    sign.add_argument('--image', type=Path, required=True)
    sign.add_argument('--sequence', type=int)
    sign.add_argument('--version')
    destination = sign.add_mutually_exclusive_group(required=True)
    destination.add_argument('--image-url', type=https_url)
    destination.add_argument('--github-repo', type=github_repo)
    sign.add_argument('--tag', help='GitHub release tag; defaults to vVERSION')
    sign.add_argument('--out', type=Path)
    verify = sub.add_parser('verify')
    verify.add_argument('--public-key', type=Path, required=True)
    verify.add_argument('--manifest', type=Path, required=True)
    verify.add_argument('--image', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'keygen':
        if args.private_key.resolve().is_relative_to(ROOT):
            raise ValueError('Private key must be outside the repository')
        if args.private_key.exists() or args.public_key.exists():
            raise ValueError('Refusing to replace an existing key')
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        secret = os.environ.get('NEON_SIGNING_KEY_PASSWORD')
        encryption = (serialization.BestAvailableEncryption(secret.encode()) if secret
                      else serialization.NoEncryption())
        exclusive_write(args.private_key, key.private_bytes(serialization.Encoding.PEM,
                        serialization.PrivateFormat.PKCS8, encryption), 0o600)
        exclusive_write(args.public_key, key.public_key().public_bytes(
                        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    elif args.command == 'configure':
        if args.github_repo:
            args.manifest_url = f'https://github.com/{args.github_repo}/releases/latest/download/release.ota'
        key = public_key(args.public_key).public_bytes(serialization.Encoding.PEM,
                                                      serialization.PublicFormat.SubjectPublicKeyInfo).decode()
        header = ('#pragma once\n#define NEON_OTA_MANIFEST_URL "' + args.manifest_url + '"\n'
                  '#define NEON_OTA_PUBLIC_KEY_PEM R"KEY(' + key + ')KEY"\n')
        # No overwrite: replacement of device trust must be deliberate.
        if args.next_public_key:
            next_pem = public_key(args.next_public_key).public_bytes(serialization.Encoding.PEM,
                        serialization.PublicFormat.SubjectPublicKeyInfo).decode()
            header += '#define NEON_OTA_NEXT_PUBLIC_KEY_PEM R"KEY(' + next_pem + ')KEY"\n'
        exclusive_write(ROOT / 'ota-config.local.h', header.encode())
    elif args.command == 'sign':
        default_sequence, default_version = source_version()
        args.sequence = default_sequence if args.sequence is None else args.sequence
        args.version = args.version or default_version
        args.out = args.out or ROOT / 'dist' / f'release-{args.sequence}'
        if args.github_repo:
            tag = args.tag or f'v{args.version}'
            if not re.fullmatch(r'[A-Za-z0-9._-]+', tag):
                raise ValueError('GitHub tag must contain only letters, numbers, dot, dash or underscore')
            args.image_url = f'https://github.com/{args.github_repo}/releases/download/{tag}/firmware.bin'
        elif args.tag:
            raise ValueError('--tag requires --github-repo')
        data = args.image.read_bytes()
        signed = manifest(data, args.sequence, args.version, args.image_url)
        key = private_key(args.private_key)
        signature = key.sign(signed, PSS, hashes.SHA256())
        args.out.mkdir(parents=True, exist_ok=False)
        exclusive_write(args.out / 'firmware.bin', data)
        exclusive_write(args.out / 'manifest.txt', signed)
        exclusive_write(args.out / 'manifest.txt.sig', signature)
        exclusive_write(args.out / 'release.ota', signed + signature)
        print(f'Ready: {args.out}/firmware.bin and {args.out}/release.ota')
    else:
        signed = args.manifest.read_bytes()
        signature = args.manifest.with_name(args.manifest.name + '.sig').read_bytes()
        public_key(args.public_key).verify(signature, signed, PSS, hashes.SHA256())
        fields = signed.decode('ascii').splitlines()
        if len(fields) != 8:
            raise ValueError('Invalid manifest')
        expected = manifest(args.image.read_bytes(), int(fields[3]), fields[4], fields[7])
        if expected != signed:
            raise ValueError('Manifest/image mismatch')
        print('Signature, image hash and identity verified')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Avoid including data/keys in logs or CLI errors.
        print(f'Release operation failed: {type(error).__name__}: {error}', file=sys.stderr)
        sys.exit(1)
