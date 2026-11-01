#!/usr/bin/env python3
"""Publish verified, locally signed bundles inside GitHub Actions only."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import HTTPRedirectHandler, Request, build_opener

import release as signing


FILES = ('firmware.bin', 'release.ota')


def checked_bundle(folder, public_key_path, repository, tag):
    signing.github_repo(repository)
    if not re.fullmatch(r'v[A-Za-z0-9._-]{1,31}', tag):
        raise ValueError('Invalid canonical release tag')
    folder = Path(folder)
    limits = {'firmware.bin': signing.SLOT_SIZE, 'manifest.txt': 1536,
              'manifest.txt.sig': 384, 'release.ota': 1920}
    data = {}
    for name, limit in limits.items():
        path = folder / name
        if not path.is_file() or not 0 < path.stat().st_size <= limit:
            raise ValueError(f'Invalid bundle file: {name}')
        data[name] = path.read_bytes()
    key_path = Path(public_key_path)
    if not key_path.is_file() or key_path.stat().st_size > 4096:
        raise ValueError('Invalid public key file')
    key = signing.public_key(key_path)
    body, signature = data['manifest.txt'], data['manifest.txt.sig']
    key.verify(signature, body, signing.PSS, signing.hashes.SHA256())
    fields = body.decode('ascii').splitlines()
    if len(fields) != 8 or tag != 'v' + fields[4]:
        raise ValueError('Release tag does not match the signed version')
    expected_url = f'https://github.com/{repository}/releases/download/{tag}/firmware.bin'
    if fields[7] != expected_url:
        raise ValueError('Signed image URL does not match repository/tag')
    expected = signing.manifest(data['firmware.bin'], int(fields[3]), fields[4], expected_url)
    if expected != body or data['release.ota'] != body + signature:
        raise ValueError('Bundle image/envelope does not match its signature')
    data['public-key.txt'] = key.public_bytes(signing.serialization.Encoding.PEM,
                                             signing.serialization.PublicFormat.SubjectPublicKeyInfo)
    return data, int(fields[3]), fields[4]


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Never forward the Actions token through a server-supplied redirect.
        return None


class GitHubApi:
    def __init__(self, token):
        if not token:
            raise ValueError('GitHub Actions token unavailable')
        self.token = token
        self.opener = build_opener(NoRedirect())

    def request(self, method, path, payload=None, binary=None, upload=False, missing_ok=False):
        if not path.startswith('/repos/') or '\n' in path or '\r' in path:
            raise ValueError('Invalid GitHub API path')
        host = 'uploads.github.com' if upload else 'api.github.com'
        data = binary if binary is not None else (json.dumps(payload).encode() if payload is not None else None)
        headers = {'Authorization': f'Bearer {self.token}', 'Accept': 'application/vnd.github+json',
                   'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'NeonOS-release-publisher',
                   'Content-Type': 'application/octet-stream' if upload else 'application/json'}
        request = Request('https://' + host + path, data=data, headers=headers, method=method)
        try:
            with self.opener.open(request, timeout=90) as response:
                return json.load(response)
        except HTTPError as error:
            if missing_ok and error.code == 404:
                return None
            # Do not echo response bodies, headers or tokens into workflow logs.
            raise RuntimeError(f'GitHub {method} failed (HTTP {error.code})') from None


def asset_matches(asset, data):
    return (asset.get('state') == 'uploaded' and asset.get('size') == len(data)
            and asset.get('digest') == 'sha256:' + hashlib.sha256(data).hexdigest())


def publish(api, repository, tag, data, commit):
    prefix = f'/repos/{repository}'
    if api.request('GET', prefix).get('private', True):
        raise ValueError('Public GitHub Releases are required for the device download URLs')
    existing = api.request('GET', prefix + '/releases/tags/' + quote(tag, safe=''), missing_ok=True)
    if existing and existing.get('tag_name') != tag:
        raise ValueError('GitHub returned an unexpected release tag')
    if existing and not existing.get('draft'):
        assets = {item['name']: item for item in existing.get('assets', [])}
        if all(name in assets and asset_matches(assets[name], data[name]) for name in FILES):
            return existing['html_url']  # Safe rerun; do not re-promote an older release.
        raise ValueError('Published release differs; refusing to overwrite its assets')
    release = existing or api.request('POST', prefix + '/releases', payload={
        'tag_name': tag, 'target_commitish': commit, 'name': f'NeonOS {tag}',
        'body': 'Locally signed OTA firmware.\n\nBoth assets verified before publication.',
        'draft': True, 'prerelease': False})
    release_id = int(release['id'])
    assets = {item['name']: item for item in release.get('assets', [])}
    for name in FILES:
        if name in assets:
            if asset_matches(assets[name], data[name]):
                continue
            raise ValueError('Existing draft asset differs; leaving the draft unchanged')
        uploaded = api.request('POST', prefix + f'/releases/{release_id}/assets?name=' + name,
                               binary=data[name], upload=True)
        if (uploaded.get('name') != name or uploaded.get('size') != len(data[name])
                or uploaded.get('state') != 'uploaded'):
            raise ValueError('Asset upload incomplete; release remains a draft')
        if uploaded.get('digest') and not asset_matches(uploaded, data[name]):
            raise ValueError('Asset digest mismatch; release remains a draft')
    published = api.request('PATCH', prefix + f'/releases/{release_id}', payload={
        'draft': False, 'prerelease': False, 'make_latest': 'true'})
    if published.get('draft') or published.get('prerelease'):
        raise ValueError('GitHub did not publish the stable release')
    return published['html_url']


def publication_tag(trigger, version):
    tag = 'v' + version
    if not re.fullmatch(re.escape('ota-' + tag) + r'(?:-retry[0-9]+)?', trigger):
        raise ValueError('Publication trigger must match the current source version')
    return tag


def main():
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise ValueError('This publisher runs in GitHub Actions; locally use release.py stage and git push')
    repository = signing.github_repo(os.environ['GITHUB_REPOSITORY'])
    ref = os.environ['GITHUB_REF_NAME']
    source_sequence, source_version = signing.source_version()
    tag = publication_tag(ref, source_version)
    folder = signing.ROOT / 'releases' / tag
    data, sequence, version = checked_bundle(folder, folder / 'public-key.txt', repository, tag)
    if (sequence, version) != (source_sequence, source_version):
        raise ValueError('Source version/sequence differs from staged firmware')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD^{commit}'], text=True).strip()
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Invalid source commit')
    subprocess.run(['git', 'merge-base', '--is-ancestor', commit, 'origin/main'], check=True)
    # Existing v2.0.0 may predate this workflow. Preserve it when app source is
    # identical; never move a release tag or silently point it at different code.
    # Installer/network fixes may change without invalidating the existing image.
    canonical_ref = f'refs/tags/{tag}'
    found = subprocess.run(['git', 'rev-parse', '--verify', canonical_ref], capture_output=True)
    if found.returncode == 0:
        subprocess.run(['git', 'diff', '--exit-code', canonical_ref, commit, '--',
                        '*.ino', '*.h', '*.cpp', 'partitions.csv'], check=True)
    api = GitHubApi(os.environ.get('GITHUB_TOKEN'))
    print(publish(api, repository, tag, data, commit))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'Release publication stopped: {type(error).__name__}: {error}')
        raise SystemExit(1)
