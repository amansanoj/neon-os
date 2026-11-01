import copy
import hashlib
import importlib
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
publisher = importlib.import_module('github_publish')
signing = publisher.signing


class ProxyTests(unittest.TestCase):
    def test_proxy_selection_never_exports_empty_override(self):
        script = str(Path(__file__).resolve().parents[1] / 'tools/arduino-proxy.sh')
        keys = ['ARDUINO_NETWORK_PROXY','HTTPS_PROXY','https_proxy','HTTP_PROXY','http_proxy']
        cases = [({},'UNSET'), ({'ARDUINO_NETWORK_PROXY':''},'UNSET'),
                 ({'HTTPS_PROXY':'http://upper.example:8080'},'http://upper.example:8080'),
                 ({'https_proxy':'http://lower.example:8080'},'http://lower.example:8080'),
                 ({'HTTP_PROXY':'http://http.example:8080'},'http://http.example:8080'),
                 ({'http_proxy':'http://http-lower.example:8080'},'http://http-lower.example:8080'),
                 ({'HTTPS_PROXY':'','https_proxy':'http://fallback.example:8080'},'http://fallback.example:8080'),
                 ({'ARDUINO_NETWORK_PROXY':'http://explicit.example:8080','HTTPS_PROXY':'http://other.example:8080'},'http://explicit.example:8080')]
        for overrides,expected in cases:
            with self.subTest(keys=list(overrides)):
                environment=os.environ.copy()
                for key in keys:
                    environment.pop(key,None)
                environment.update(overrides)
                output=subprocess.check_output(['bash','-c',
                    'set -euo pipefail; . "$1"; printf "%s" "${ARDUINO_NETWORK_PROXY-UNSET}"',
                    'proxy-test',script],env=environment,text=True)
                self.assertEqual(output,expected)


class FakeApi:
    def __init__(self):
        self.calls = []
        self.release = None
        self.private = False
        self.fail_asset = None
        self.bad_digest = False

    def request(self, method, path, payload=None, binary=None, upload=False, missing_ok=False):
        self.calls.append((method, path, payload, upload))
        if method == 'GET' and path == '/repos/amansanoj/neon-os':
            return {'private': self.private}
        if method == 'GET':
            return copy.deepcopy(self.release)
        if upload:
            name = path.split('name=')[1]
            if name == self.fail_asset:
                raise RuntimeError('Simulated upload failure')
            asset = {'name': name, 'size': len(binary), 'state': 'uploaded',
                     'digest': 'sha256:' + hashlib.sha256(binary).hexdigest()}
            if self.bad_digest:
                asset['digest'] = 'sha256:incorrect'
            self.release['assets'].append(asset)
            return copy.deepcopy(asset)
        if method == 'POST':
            self.release = {'id': 42, 'tag_name': payload['tag_name'], 'draft': True,
                            'prerelease': False, 'assets': [],
                            'html_url': 'https://github.com/amansanoj/neon-os/releases/tag/v2.0.0'}
            return copy.deepcopy(self.release)
        if method == 'PATCH':
            self.release.update(payload)
            return copy.deepcopy(self.release)
        raise AssertionError((method, path))


class PublishingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = signing.rsa.generate_private_key(public_exponent=65537, key_size=3072)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='neon-publish-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / 'dist/release-2000000'
        self.bundle.mkdir(parents=True)
        self.public = self.root / 'public.pem'
        self.public.write_bytes(self.key.public_key().public_bytes(signing.serialization.Encoding.PEM,
                                                                  signing.serialization.PublicFormat.SubjectPublicKeyInfo))
        image = bytearray(4096)
        image[0] = 0xE9
        image[32:36] = bytes.fromhex('3254cdab')
        marker = signing.identity(2000000, '2.0.0')
        image[1000:1000+len(marker)] = marker
        body = signing.manifest(image, 2000000, '2.0.0',
                                'https://github.com/amansanoj/neon-os/releases/download/v2.0.0/firmware.bin')
        signature = self.key.sign(body, signing.PSS, signing.hashes.SHA256())
        files = {'firmware.bin': image, 'manifest.txt': body, 'manifest.txt.sig': signature,
                 'release.ota': body+signature}
        for name, data in files.items():
            (self.bundle/name).write_bytes(data)
        self.data, _, _ = publisher.checked_bundle(self.bundle, self.public, 'amansanoj/neon-os', 'v2.0.0')

    def publish(self, api):
        return publisher.publish(api, 'amansanoj/neon-os', 'v2.0.0', self.data, 'a'*40)

    def test_trigger_and_retry_preserve_canonical_tag(self):
        self.assertEqual(publisher.publication_tag('ota-v2.0.0', '2.0.0'), 'v2.0.0')
        self.assertEqual(publisher.publication_tag('ota-v2.0.0-retry1', '2.0.0'), 'v2.0.0')
        with self.assertRaises(ValueError):
            publisher.publication_tag('ota-v2.0.1', '2.0.0')

    def test_verified_bundle(self):
        self.assertEqual(set(self.data), {'firmware.bin', 'manifest.txt', 'manifest.txt.sig', 'release.ota', 'public-key.txt'})
        self.assertNotIn(b'PRIVATE KEY', self.data['public-key.txt'])

    def test_altered_envelope_rejected(self):
        (self.bundle/'release.ota').write_bytes(b'invalid')
        with self.assertRaises(ValueError):
            publisher.checked_bundle(self.bundle, self.public, 'amansanoj/neon-os', 'v2.0.0')

    def test_wrong_repository_rejected(self):
        with self.assertRaises(ValueError):
            publisher.checked_bundle(self.bundle, self.public, 'other/repo', 'v2.0.0')

    def test_wrong_tag_rejected(self):
        with self.assertRaises(ValueError):
            publisher.checked_bundle(self.bundle, self.public, 'amansanoj/neon-os', 'v2.0.1')

    def test_stage_is_idempotent_and_public_only(self):
        (self.root/'firmware-version.h').write_text('#define NEON_FIRMWARE_SEQUENCE 2000000\n#define NEON_FIRMWARE_VERSION "2.0.0"\n')
        arguments = ['release.py', 'stage', '--public-key', str(self.public), '--github-repo', 'amansanoj/neon-os']
        with patch.object(signing, 'ROOT', self.root), patch.object(sys, 'argv', arguments):
            signing.main()
            signing.main()
            target = self.root/'releases/v2.0.0'
            self.assertEqual({file.name for file in target.iterdir()}, set(self.data))
            (target/'firmware.bin').write_bytes(b'changed')
            with self.assertRaises(ValueError):
                signing.main()

    def test_draft_uploads_both_before_publishing(self):
        api = FakeApi()
        self.publish(api)
        uploaded = [entry for entry in api.calls if entry[3]]
        self.assertEqual(len(uploaded), 2)
        self.assertEqual(api.calls[-1][0], 'PATCH')
        self.assertFalse(api.release['draft'])
        self.assertEqual(api.release['make_latest'], 'true')

    def test_failed_upload_keeps_draft_and_can_resume(self):
        api = FakeApi()
        api.fail_asset = 'release.ota'
        with self.assertRaises(RuntimeError):
            self.publish(api)
        self.assertTrue(api.release['draft'])
        self.assertFalse(any(call[0] == 'PATCH' for call in api.calls))
        api.fail_asset = None
        self.publish(api)
        self.assertEqual(len(api.release['assets']), 2)
        self.assertFalse(api.release['draft'])

    def test_published_rerun_is_read_only(self):
        api = FakeApi()
        self.publish(api)
        api.calls.clear()
        self.publish(api)
        self.assertTrue(all(call[0] == 'GET' for call in api.calls))

    def test_existing_published_assets_never_overwritten(self):
        api = FakeApi()
        self.publish(api)
        api.release['assets'][0]['digest'] = 'sha256:wrong'
        api.calls.clear()
        with self.assertRaises(ValueError):
            self.publish(api)
        self.assertTrue(all(call[0] == 'GET' for call in api.calls))

    def test_private_repo_rejected_before_mutation(self):
        api = FakeApi()
        api.private = True
        with self.assertRaises(ValueError):
            self.publish(api)
        self.assertTrue(all(call[0] == 'GET' for call in api.calls))

    def test_wrong_upload_digest_remains_draft(self):
        api = FakeApi()
        api.bad_digest = True
        with self.assertRaises(ValueError):
            self.publish(api)
        self.assertTrue(api.release['draft'])
        self.assertFalse(any(call[0] == 'PATCH' for call in api.calls))

    def test_api_token_never_follows_redirects(self):
        self.assertIsNone(publisher.NoRedirect().redirect_request(None, None, 302, '', {}, 'https://evil.example/'))
        api = publisher.GitHubApi('not-a-real-token')
        with patch.object(api.opener, 'open', side_effect=HTTPError('https://api.github.com', 403, 'secret response', {}, None)):
            with self.assertRaisesRegex(RuntimeError, 'HTTP 403') as error:
                api.request('GET', '/repos/amansanoj/neon-os')
            self.assertNotIn('not-a-real-token', str(error.exception))
            self.assertNotIn('secret response', str(error.exception))


if __name__ == '__main__':
    unittest.main()
