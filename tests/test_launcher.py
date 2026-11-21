"""Exercise the console launcher against a local fake model service."""
import json
import os
import shutil
import subprocess
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory

from support import APP, EXE


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory(prefix='pilot-launcher-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / '便携包 with spaces'
        self.root.mkdir()
        shutil.copytree(APP / 'launcher', self.root / 'launcher')
        shutil.copytree(APP / 'fixtures/repair_project', self.root / 'fixtures/repair_project')
        shutil.copy2(APP / 'start-model.ps1', self.root)
        (self.root / 'bin').mkdir()
        shutil.copy2(EXE, self.root / 'bin/cpp_agent.exe')
        self.requests = []
        self.names = ['launcher-model']
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append(('GET', self.path))
                data = json.dumps({'models': [{'name': name} for name in owner.names]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                owner.requests.append(('POST', self.path))
                self.send_error(500, 'Launcher must not make unconfirmed changes')

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def launch(self, *extra, stdin=''):
        return subprocess.run(['powershell.exe', '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                               '-File', str(self.root / 'launcher/Start.ps1'),
                               '-Port', str(self.server.server_port), '-Model', 'launcher-model',
                               '-NoGuide', *extra], input=stdin, capture_output=True,
                              text=True, encoding='utf-8', timeout=30)

    def test_check_only_does_not_launch_or_download(self):
        result = self.launch('-CheckOnly')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        status = json.loads(result.stdout[result.stdout.index('{'):])
        self.assertTrue(status['serverReady'])
        self.assertTrue(status['modelReady'])
        self.assertFalse((self.root / '.agent_data').exists())
        self.assertEqual(self.requests, [('GET', '/api/tags')])

    def test_ready_launch_with_unicode_path_and_clean_exit(self):
        result = self.launch('-Root', str(self.root / 'fixtures/repair_project'), '-Execute', '/map')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('calculator.cpp', result.stdout)
        self.assertTrue((self.root / '.agent_data').is_dir())
        self.assertTrue(all(method == 'GET' for method, _ in self.requests))

    def test_model_download_requires_confirmation(self):
        self.names = []
        result = self.launch(stdin='\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / '.agent_data').exists())
        self.assertEqual(self.requests, [('GET', '/api/tags')])

    def test_missing_binary_fails_before_network(self):
        (self.root / 'bin/cpp_agent.exe').unlink()
        result = self.launch('-CheckOnly')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.requests, [])

    def test_invalid_project_is_rejected(self):
        result = self.launch('-Root', str(self.root / 'missing'))
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.root / '.agent_data').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
