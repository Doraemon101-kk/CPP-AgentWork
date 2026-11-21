"""Behavior-level tests. Mock model only controls requests; tools/builds are real."""
import json
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler

from support import APP, EXE, SCRATCH

class FakeModel:
    def __init__(self, messages):
        self.requests = []
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append(request)
                answer = messages(request, len(owner.requests))
                data = json.dumps({'message': answer}).encode()
                self.send_response(200)
                self.end_headers()
                self.wfile.write(data)
        self.server = HTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
    def close(self):
        self.server.shutdown()
        self.server.server_close()


def calls(*items):
    return {'role': 'assistant', 'content': '', 'tool_calls': [
        {'function': {'name': name, 'arguments': args}} for name, args in items
    ]}

class WorkbenchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / 'project with spaces'
        shutil.copytree(APP / 'fixtures/repair_project', self.root)
        self.state = self.base / 'state'
        self.servers = []
    def tearDown(self):
        for server in self.servers: server.close()
        self.temp.cleanup()
    def run_app(self, command, resume=None, model=None, expected=0, stdin=None):
        args = [str(EXE), '--workbench', str(self.root), '--state', str(self.state)]
        if resume: args += ['--resume', resume]
        if model: args += ['--port', str(model.server.server_port), '--model', 'test-model']
        if command is not None: args += ['--exec', command]
        result = subprocess.run(args, input=stdin, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result
    def workspace(self):
        return next((self.state / 'workspaces').iterdir())
    def proposal(self, create=False, old='return left - right;', new='return left + right;'):
        def respond(request, number):
            if number == 1:
                edit = ('propose_file', {'path': 'new_file.cpp', 'content': new}) if create else (
                    'propose_edit', {'path': 'calculator.cpp', 'old_text': old, 'new_text': new})
                return calls(('read_text', {'path': 'calculator.cpp'}), edit)
            return {'role': 'assistant', 'content': 'A change was proposed. Use /review and /apply.'}
        model = FakeModel(respond); self.servers.append(model)
        self.run_app('/edit Repair the addition function', model=model)
        changes = list((self.workspace() / 'changes').glob('*.json'))
        self.assertEqual(len(changes), 1)
        change = json.loads(changes[0].read_text(encoding='utf-8'))
        sessions = [json.loads(path.read_text(encoding='utf-8')) for path in (self.workspace() / 'sessions').glob('*.json')]
        session = next(item for item in sessions if item['turns'])
        return change, session, model
    def test_map_and_lexical_index(self):
        result = self.run_app('/map')
        self.assertIn('calculator.cpp', result.stdout)
        index = json.loads((self.workspace() / 'index.json').read_text(encoding='utf-8'))
        self.assertTrue(any(item['name'] == 'add' for item in index['symbols']))
        self.assertTrue(any(item['name'] == 'main' for item in index['symbols']))
    def test_stage_apply_undo_and_real_tests(self):
        failed = self.run_app('/test', expected=2)
        self.assertIn('FAIL: add must perform addition', failed.stdout)
        before = (self.root / 'calculator.cpp').read_bytes()
        change, session, model = self.proposal()
        self.assertEqual((self.root / 'calculator.cpp').read_bytes(), before)
        self.assertEqual(change['status'], 'pending')
        self.assertIn('+ return left + right;', self.run_app('/review ' + change['id']).stdout)
        self.run_app('/apply ' + change['id'])
        passed = self.run_app('/test')
        self.assertIn('100% tests passed', passed.stdout)
        self.run_app('/undo ' + change['id'])
        self.assertEqual((self.root / 'calculator.cpp').read_bytes(), before)
        self.run_app('/test', expected=2)
    def test_apply_conflict_preserves_newer_work(self):
        change, _, _ = self.proposal()
        target = self.root / 'calculator.cpp'
        target.write_text('newer user work\n', encoding='utf-8')
        result = self.run_app('/apply ' + change['id'], expected=2)
        self.assertIn('Conflict', result.stderr)
        self.assertEqual(target.read_text(), 'newer user work\n')
    def test_undo_conflict_preserves_newer_work(self):
        change, _, _ = self.proposal()
        self.run_app('/apply ' + change['id'])
        target = self.root / 'calculator.cpp'
        target.write_text('changed after apply\n', encoding='utf-8')
        self.run_app('/undo ' + change['id'], expected=2)
        self.assertEqual(target.read_text(), 'changed after apply\n')
    def test_new_file_and_undo(self):
        change, _, _ = self.proposal(create=True, new='int extra() { return 1; }\n')
        target = self.root / 'new_file.cpp'
        self.assertFalse(target.exists())
        self.run_app('/apply ' + change['id'])
        self.assertTrue(target.exists())
        self.run_app('/undo ' + change['id'])
        self.assertFalse(target.exists())
    def test_resume_context_and_export(self):
        _, session, _ = self.proposal()
        def respond(request, number):
            text = json.dumps(request['messages'])
            self.assertIn('Repair the addition function', text)
            self.assertIn('A change was proposed', text)
            return {'role': 'assistant', 'content': 'Continuing the same saved conversation.'}
        model = FakeModel(respond); self.servers.append(model)
        self.run_app('Continue from the earlier proposal', resume=session['id'], model=model)
        result = self.run_app('/report', resume=session['id'])
        reports = list((self.workspace() / 'reports').glob('*.md'))
        self.assertEqual(len(reports), 1)
        self.assertIn('Continuing the same saved conversation', reports[0].read_text())
        self.assertIn('Report:', result.stdout)
    def test_paths_and_ambiguous_replacements(self):
        self.run_app('/read ../outside.txt', expected=2)
        target = self.root / 'calculator.cpp'
        target.write_text('return x;\nreturn x;\n', encoding='utf-8')
        def respond(request, number):
            if number == 1:
                return calls(('read_text', {'path': 'calculator.cpp'}), ('propose_edit', {
                    'path': 'calculator.cpp', 'old_text': 'return x;', 'new_text': 'return 0;'}))
            data = json.loads(request['messages'][-1]['content'])
            self.assertFalse(data['ok'])
            self.assertIn('ambiguous', data['detail'])
            return {'role': 'assistant', 'content': 'The exact replacement is ambiguous.'}
        model = FakeModel(respond); self.servers.append(model)
        self.run_app('/edit Inspect duplicate text', model=model, expected=2)
        self.assertEqual(list((self.workspace() / 'changes').glob('*.json')), [])
    def test_read_before_edit_and_schema_error_recovery(self):
        def respond(request, number):
            if number == 1:
                return calls(('propose_edit', {'path': 'calculator.cpp', 'old_text': 'return left - right;', 'new_text': 'return left + right;'}))
            if number == 2:
                previous = json.loads(request['messages'][-1]['content'])
                self.assertFalse(previous['ok'])
                self.assertIn('Read this file', previous['detail'])
                return calls(('read_text', {'path': 'calculator.cpp', 'keyword': 'add'}))
            if number == 3:
                previous = json.loads(request['messages'][-1]['content'])
                self.assertFalse(previous['ok'])
                self.assertEqual(list(previous['expected_parameters']['properties']), ['path'])
                return calls(('read_text', {'path': 'calculator.cpp'}), ('propose_edit', {'path': 'calculator.cpp', 'old_text': 'return left - right;', 'new_text': 'return left + right;'}))
            return {'role': 'assistant', 'content': 'A valid proposal is now ready.'}
        model = FakeModel(respond); self.servers.append(model)
        self.run_app('/edit Repair after correcting tool arguments', model=model)
        self.assertEqual(len(list((self.workspace() / 'changes').glob('*.json'))), 1)
        self.assertIn('left - right', (self.root / 'calculator.cpp').read_text())
    def test_search_accepts_single_file_and_directory(self):
        def respond(request, number):
            if number == 1:
                return calls(('search_text', {'path': 'calculator.cpp', 'keyword': 'return left'}), ('read_text', {'path': 'calculator.cpp'}))
            data = json.loads(request['messages'][-2]['content'])
            self.assertTrue(data['ok'])
            self.assertIn('calculator.cpp:3:', data['content'])
            return {'role': 'assistant', 'content': 'The single-file search returned the real source line.'}
        model = FakeModel(respond); self.servers.append(model)
        self.run_app('Search inside one file', model=model)
    def test_default_search_excludes_generated_state(self):
        hidden = self.root / '.agent_data'
        hidden.mkdir()
        (hidden / 'hidden.cpp').write_text('private_generated_needle', encoding='utf-8')
        result = self.run_app('/search private_generated_needle')
        self.assertNotIn('hidden.cpp', result.stdout)
        index = json.loads((self.workspace() / 'index.json').read_text(encoding='utf-8'))
        self.assertFalse(any('hidden.cpp' in item['path'] for item in index['files']))
    def test_unicode_file_queries(self):
        unicode_root = self.base / '项目 空格'
        self.root.rename(unicode_root)
        self.root = unicode_root
        result = self.run_app('/read calculator.cpp')
        self.assertIn('return left - right', result.stdout)
        self.run_app('/map')
    def test_no_tests_is_failure_and_timeout(self):
        cmake = self.root / 'CMakeLists.txt'
        cmake.write_text('cmake_minimum_required(VERSION 3.16)\nproject(empty NONE)\nenable_testing()\n', encoding='utf-8')
        result = self.run_app('/test', expected=2)
        self.assertIn('No tests were found', result.stdout)
        # Reconfigure the same build tree with a slow registered test.
        cmake.write_text('cmake_minimum_required(VERSION 3.16)\nproject(empty NONE)\nenable_testing()\nadd_test(NAME slow COMMAND "${CMAKE_COMMAND}" -E sleep 5)\n', encoding='utf-8')
        self.run_app('/configure')
        config_file = self.workspace() / 'config.json'
        config = json.loads(config_file.read_text()); config['check_timeout'] = 1
        config_file.write_text(json.dumps(config))
        result = self.run_app('/test', expected=2)
        self.assertIn('[timeout]', result.stdout)

if __name__ == '__main__': unittest.main(verbosity=2)
