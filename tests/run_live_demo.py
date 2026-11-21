"""Optional real-model repair demo; not part of deterministic CTest."""
import json
import shutil
import subprocess
import time
import urllib.request
from pathlib import Path

from support import APP, EXE
import argparse
parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=11434)
parser.add_argument('--model', default='qwen3:4b-instruct-2507-q4_K_M')
options = parser.parse_args()
MODEL = options.model
PORT = options.port
models = json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/api/tags'))['models']
assert any(model['name'] == MODEL for model in models), 'Run start-model.ps1 first'
root = APP / '.agent_data/demos' / ('repair-live-' + str(time.time_ns()))
shutil.copytree(APP / 'fixtures/repair_project', root)
state = APP / '.agent_data/live-verification'
base = [str(EXE), '--workbench', str(root), '--state', str(state), '--model', MODEL, '--port', str(PORT)]
print('Real model:', MODEL, '\nIsolated project:', root, flush=True)
commands = '/test\n/edit 根据刚才的测试失败结果修复问题，只改相关错误。请读取实际源码并生成修改提案，先不要应用。\n/changes\n/quit\n'
result = subprocess.run(base, input=commands, capture_output=True, text=True, encoding='utf-8', timeout=300)
(APP / 'workbench-live.log').write_text(result.stdout + '\n' + result.stderr, encoding='utf-8')
print(result.stdout[result.stdout.find('pilot> [model'):], flush=True)
if result.stderr: print(result.stderr, flush=True)
workspace = next(path for path in (state / 'workspaces').iterdir() if Path(json.loads((path / 'index.json').read_text(encoding='utf-8'))['root']).resolve() == root.resolve())
changes = [json.loads(path.read_text(encoding='utf-8')) for path in (workspace / 'changes').glob('*.json')]
change = next((item for item in changes if item['status'] == 'pending' and item['path'] == 'calculator.cpp'), None)
assert change, 'No actual patch was proposed; inspect workbench-live.log'
assert 'left + right' in change['after'], change
sessions = [json.loads(path.read_text(encoding='utf-8')) for path in (workspace / 'sessions').glob('*.json')]
session = next(item for item in sessions if item['turns'])
original = (root / 'calculator.cpp').read_bytes()
commands = f"/review {change['id']}\n/apply {change['id']}\n/test\n/undo {change['id']}\n/test\n/report\n/quit\n"
result = subprocess.run(base + ['--resume', session['id']], input=commands, capture_output=True, text=True, encoding='utf-8', timeout=180)
with (APP / 'workbench-live.log').open('a', encoding='utf-8') as output:
    output.write('\n--- APPLY/TEST/UNDO ---\n' + result.stdout + '\n' + result.stderr)
assert (root / 'calculator.cpp').read_bytes() == original
updated = json.loads((workspace / 'sessions' / (session['id'] + '.json')).read_text(encoding='utf-8'))
checks = [event['data']['ok'] for event in updated['events'] if event['kind'] == 'check']
assert checks == [False, True, False], checks
report = next((workspace / 'reports').glob('*.md'))
metadata = {'root': str(root), 'state': str(state), 'change_id': change['id'], 'session_id': session['id'], 'checks': checks, 'model': MODEL, 'report': str(report)}
(APP / '.agent_data/live-verification.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS: real model -> patch -> apply -> tests pass -> undo -> tests fail -> report', flush=True)
print('Log:', APP / 'workbench-live.log', '\nReport:', report, flush=True)
