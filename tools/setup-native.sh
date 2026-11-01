#!/usr/bin/env bash
set -euo pipefail
NEON_TOOLS_DIR="${NEON_TOOLS_DIR:-/workspace/.neon-tools}"
mkdir -p "$NEON_TOOLS_DIR/native-mbedtls"
curl -fsSL https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.6.tar.gz -o "$NEON_TOOLS_DIR/native-mbedtls/source.tar.gz"
curl -fsSL https://github.com/Mbed-TLS/mbedtls-framework/archive/dff9da04438d712f7647fd995bc90fadd0c0e2ce.tar.gz -o "$NEON_TOOLS_DIR/native-mbedtls/framework.tar.gz"
export NEON_TOOLS_DIR
python3 - <<'PY'
import hashlib, os, pathlib, tarfile
root=pathlib.Path(os.environ['NEON_TOOLS_DIR'])/'native-mbedtls'
archive=root/'source.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='82dd9b736624a76577635e8ffae688ac3cc3b16eb3813f0b17201aa8ba3fa76c'
with tarfile.open(archive) as source:
    source.extractall(root,filter='data')
framework=root/'framework.tar.gz'
assert hashlib.sha256(framework.read_bytes()).hexdigest()=='0f2bc024aceb3d3cbfcd8cfb997bc9a5eadfcf81fbbf0ef9a9053236e1d6506c'
with tarfile.open(framework) as source:
    for member in source.getmembers():
        member.name='/'.join(member.name.split('/')[1:])
        if member.name:
            source.extract(member,root/'mbedtls-3.6.6/framework',filter='data')
PY
make -C "$NEON_TOOLS_DIR/native-mbedtls/mbedtls-3.6.6" -j4 SHARED=1 lib
