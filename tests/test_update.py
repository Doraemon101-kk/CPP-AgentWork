"""0.2 task modes, versioned memory, streamed transport and cancellation."""
import json
import subprocess
import threading
import time
import unittest
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import test_workbench as helpers
from support import EXE

class StreamModel:
    def __init__(self, respond):
        self.requests=[];self.errors=[]
        owner=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_POST(self):
                request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append(request)
                self.send_response(200);self.send_header('Content-Type','application/x-ndjson');self.end_headers()
                try:
                    for chunk in respond(request,len(owner.requests)):
                        raw=chunk if isinstance(chunk,bytes) else (json.dumps(chunk,ensure_ascii=False)+'\n').encode()
                        # Force byte boundaries inside UTF-8 and JSON; not one write per record.
                        for start in range(0,len(raw),7): self.wfile.write(raw[start:start+7]);self.wfile.flush()
                except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError): pass
                except Exception as error: owner.errors.append(error)
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.server.daemon_threads=True
        threading.Thread(target=self.server.serve_forever,daemon=True).start()
    def close(self):self.server.shutdown();self.server.server_close()

def chunk(text='',done=False,calls=None):
    message={'role':'assistant','content':text}
    if calls is not None:message['tool_calls']=calls
    return {'message':message,'done':done}

class UpdateTests(unittest.TestCase):
    setUp=helpers.WorkbenchTests.setUp
    tearDown=helpers.WorkbenchTests.tearDown
    run_app=helpers.WorkbenchTests.run_app
    workspace=helpers.WorkbenchTests.workspace
    def session(self):
        values=[json.loads(f.read_text(encoding='utf-8')) for f in (self.workspace()/'sessions').glob('*.json')]
        return max(values,key=lambda value:(len(value['turns']),len(value['events'])))
    def spawn(self,command,model):
        return subprocess.Popen([str(EXE),'--workbench',str(self.root),'--state',str(self.state),
            '--port',str(model.server.server_port),'--model','test-model','--exec',command],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    def test_ask_mode_cannot_stage_edits_even_when_question_mentions_fix(self):
        def reply(request,n):
            names=[t['function']['name'] for t in request['tools']]
            self.assertNotIn('propose_edit',names);self.assertNotIn('run_check',names)
            if n==1:return helpers.calls(('read_text',{'path':'calculator.cpp'}),('propose_edit',{'path':'calculator.cpp','old_text':'left - right','new_text':'left + right'}))
            self.assertFalse(json.loads(request['messages'][-1]['content'])['ok'])
            return {'role':'assistant','content':'This is an explanation only.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        # A rejected edit does not make an ask-mode explanation require a proposal.
        result=self.run_app('/ask Explain how to fix the bug',model=model)
        self.assertEqual(list((self.workspace()/'changes').glob('*.json')),[])
    def test_sticky_mode_and_one_turn_override(self):
        def reply(request,n):
            names=[t['function']['name'] for t in request['tools']]
            self.assertNotIn('propose_edit',names)
            if n==1:return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            return {'role':'assistant','content':'Read-only explanation of the implementation.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app(None,model=model,stdin='/mode edit\n/ask Explain the fix\n/quit\n')
        self.assertEqual(self.session()['mode'],'edit')
        self.assertEqual(self.session()['turns'][0]['mode'],'ask')
    def test_check_mode_exposes_checks_but_no_edits(self):
        def reply(request,n):
            names=[t['function']['name'] for t in request['tools']]
            self.assertIn('run_check',names);self.assertNotIn('propose_file',names)
            if n==1:return helpers.calls(('run_check',{'kind':'test'}))
            self.assertFalse(json.loads(request['messages'][-1]['content'])['ok'])
            return {'role':'assistant','content':'The actual arithmetic test failed.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/check Run the tests',model=model)
    def test_external_edit_invalidates_read_and_test_facts(self):
        self.run_app(None,stdin='/read calculator.cpp\n/test\n/quit\n')
        session=self.session();sid=session['id']
        self.assertTrue(session['memory']['reads']['calculator.cpp']['current'])
        target=self.root/'calculator.cpp';target.write_text(target.read_text()+'\n// external edit\n',encoding='utf-8')
        result=self.run_app('/memory',resume=sid)
        memory=json.loads(result.stdout)
        self.assertFalse(memory['reads']['calculator.cpp']['current'])
        self.assertFalse(memory['checks'][-1]['current'])
        def reply(request,n):
            if n==1:return {'role':'assistant','content':'An unsupported stale answer'}
            if n==2:return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            return {'role':'assistant','content':'Re-read current source.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/ask Explain current code',resume=sid,model=model)
        self.assertEqual(len(model.requests),3)
    def test_compaction_keeps_pinned_goal_and_separates_facts(self):
        self.run_app('/note Preserve GOAL_COIN and public interfaces')
        session=self.session();sid=session['id']
        session['turns']=[{'question':'GOAL_COIN task '+str(i),'answer':'Decision '+str(i),'status':'completed'} for i in range(10)]
        path=self.workspace()/'sessions'/(sid+'.json');path.write_text(json.dumps(session),encoding='utf-8')
        def reply(request,n):
            if request['messages'][0]['content'].startswith('Summarize prior'):
                return {'role':'assistant','content':'Goal GOAL_COIN; decisions recorded; implementation remains pending.'}
            text=json.dumps(request['messages'])
            self.assertIn('Preserve GOAL_COIN',text);self.assertIn('unverified',text)
            if request['messages'][-1]['role']=='user':return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            return {'role':'assistant','content':'Continuing GOAL_COIN with source evidence.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/ask Continue the goal',resume=sid,model=model)
        memory=json.loads(path.read_text())['memory']
        self.assertGreater(memory['compacted_through'],0)
        self.assertEqual(memory['summary_method'],'model_unverified')
        self.assertIn('GOAL_COIN',memory['summary']);self.assertIn('calculator.cpp',memory['reads'])
    def test_stream_arrives_before_response_finishes_and_utf8_is_valid(self):
        gate=threading.Event();expired=threading.Event()
        def reply(request,n):
            if n==1:
                yield chunk(calls=[{'function':{'name':'read_text','arguments':{'path':'calculator.cpp'}}}]);yield chunk(done=True)
            else:
                yield chunk('你好 FIRST_PART\n')
                if not gate.wait(5):expired.set()
                yield chunk('END',True)
        model=StreamModel(reply);self.servers.append(model)
        process=self.spawn('/ask Inspect source',model)
        try:
            output=''
            while 'FIRST_PART' not in output:
                line=process.stdout.readline();self.assertTrue(line,'Process ended before streamed text');output+=line
            self.assertFalse(expired.is_set());self.assertIsNone(process.poll())
            gate.set();tail,error=process.communicate(timeout=15)
            self.assertEqual(process.returncode,0,error)
            self.assertIn('你好',output)
            self.assertEqual(self.session()['turns'][-1]['answer'],'你好 FIRST_PART\nEND')
        finally:
            gate.set()
            if process.poll() is None:process.kill();process.communicate()
    def test_fragmented_tool_arguments_execute_only_after_done(self):
        def reply(request,n):
            if n==1:
                yield chunk(calls=[{'function':{'index':0,'name':'read_text','arguments':'{"path":"calcu'}}])
                yield chunk(calls=[{'function':{'index':0,'arguments':'lator.cpp"}'}}]);yield chunk(done=True)
            else:
                self.assertIn('return left - right',request['messages'][-1]['content'])
                yield chunk('File received.',True)
        model=StreamModel(reply);self.servers.append(model)
        self.run_app('/ask Inspect source',model=model)
        self.assertEqual(self.session()['turns'][-1]['tools'],1)
        self.assertFalse(model.errors)
    def test_truncated_stream_never_executes_partial_tool_call(self):
        def reply(request,n):
            yield chunk(calls=[{'function':{'name':'read_text','arguments':{'path':'calculator.cpp'}}}])
        model=StreamModel(reply);self.servers.append(model)
        self.run_app('/ask Inspect source',model=model,expected=2)
        self.assertEqual(self.session()['turns'][-1]['status'],'failed')
        self.assertEqual(self.session()['memory']['reads'],{})
    def test_pending_http_cancel_preserves_session_and_partial_text(self):
        gate=threading.Event()
        def reply(request,n):
            yield chunk('PARTIAL\n');gate.wait(8);yield chunk('SHOULD_NOT_COMPLETE',True)
        model=StreamModel(reply);self.servers.append(model)
        process=self.spawn('/ask Inspect source',model)
        try:
            output=''
            while 'PARTIAL' not in output:
                line=process.stdout.readline();self.assertTrue(line);output+=line
            started=time.monotonic();self.run_app('/cancel')
            tail,error=process.communicate(timeout=10)
            self.assertEqual(process.returncode,130,error)
            self.assertLess(time.monotonic()-started,5)
            turn=self.session()['turns'][-1]
            self.assertEqual(turn['status'],'interrupted');self.assertIn('PARTIAL',turn['partial_answer'])
            self.assertEqual(list((self.workspace()/'changes').glob('*.json')),[])
        finally:
            gate.set()
            if process.poll() is None:process.kill();process.communicate()
    def test_context_budget_shortens_large_tool_output_without_orphans(self):
        (self.root/'large.cpp').write_text('int value = 1; // ' + 'x'*18000, encoding='utf-8')
        def reply(request,n):
            if n==1:return helpers.calls(('read_text',{'path':'large.cpp'}))
            messages=request['messages']
            self.assertEqual(messages[-2]['role'],'assistant')
            self.assertEqual(messages[-1]['role'],'tool')
            data=json.loads(messages[-1]['content'])
            self.assertTrue(data['context_shortened'])
            self.assertLess(len(json.dumps(messages,ensure_ascii=False).encode()),24000)
            return {'role':'assistant','content':'Large output was bounded without breaking tool messages.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/ask Read large source',model=model)
    def test_summary_failure_preserves_original_turns_and_pinned_note(self):
        self.run_app('/note Pinned goal survives summary failure')
        session=self.session();sid=session['id']
        session['turns']=[{'question':'old '+str(i),'answer':'answer '+str(i),'status':'completed'} for i in range(8)]
        path=self.workspace()/'sessions'/(sid+'.json');path.write_text(json.dumps(session),encoding='utf-8')
        def reply(request,n):
            if request['messages'][0]['content'].startswith('Summarize prior'):
                return {'role':'assistant','content':''}
            if request['messages'][-1]['role']=='user':return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            return {'role':'assistant','content':'Continued after bounded extractive fallback.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/ask Continue',resume=sid,model=model)
        stored=json.loads(path.read_text())
        self.assertEqual(len(stored['turns']),9)
        self.assertEqual(stored['turns'][0]['question'],'old 0')
        self.assertEqual(stored['memory']['summary_method'],'extractive_fallback')
        self.assertIn('Pinned goal',stored['memory']['user_note'])
    def test_external_change_between_read_and_edit_is_rejected(self):
        def reply(request,n):
            if n==1:return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            if n==2:
                target=self.root/'calculator.cpp';target.write_text(target.read_text()+'// newer work',encoding='utf-8')
                return helpers.calls(('propose_edit',{'path':'calculator.cpp','old_text':'return left - right;','new_text':'return left + right;'}))
            self.assertIn('File changed since reading',request['messages'][-1]['content'])
            return {'role':'assistant','content':'Need to re-read changed source.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/edit Repair addition',model=model,expected=2)
        self.assertEqual(list((self.workspace()/'changes').glob('*.json')),[])
        self.assertFalse(self.session()['memory']['reads']['calculator.cpp']['current'])
    def test_reread_after_external_change_does_not_reuse_stale_cache(self):
        def reply(request,n):
            if n==1:return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            if n==2:
                target=self.root/'calculator.cpp';target.write_text(target.read_text()+'// newer work',encoding='utf-8')
                return helpers.calls(('propose_edit',{'path':'calculator.cpp','old_text':'return left - right;','new_text':'return left + right;'}))
            if n==3:return helpers.calls(('read_text',{'path':'calculator.cpp'}))
            if n==4:
                self.assertIn('newer work',request['messages'][-1]['content'])
                return helpers.calls(('propose_edit',{'path':'calculator.cpp','old_text':'return left - right;','new_text':'return left + right;'}))
            return {'role':'assistant','content':'Valid proposal preserves the newer work.'}
        model=helpers.FakeModel(reply);self.servers.append(model)
        self.run_app('/edit Repair after re-reading',model=model)
        change=json.loads(next((self.workspace()/'changes').glob('*.json')).read_text())
        self.assertIn('newer work',change['after'])
    def test_process_tree_cancellation_is_distinct_from_timeout(self):
        cmake=self.root/'CMakeLists.txt'
        cmake.write_text('cmake_minimum_required(VERSION 3.26)\nproject(slow NONE)\nenable_testing()\nadd_test(NAME slow COMMAND "${CMAKE_COMMAND}" -E sleep 20)\n',encoding='utf-8')
        self.run_app('/configure')
        process=subprocess.Popen([str(EXE),'--workbench',str(self.root),'--state',str(self.state),'--exec','/test'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
        try:
            time.sleep(0.6);self.assertIsNone(process.poll())
            self.run_app('/cancel');output,error=process.communicate(timeout=8)
            self.assertEqual(process.returncode,130,error)
            self.assertIn('[cancelled]',output)
            task=json.loads((self.workspace()/'active-task.json').read_text())
            self.assertEqual(task['status'],'interrupted')
        finally:
            if process.poll() is None:process.kill();process.communicate()

if __name__=='__main__':unittest.main(verbosity=2)
