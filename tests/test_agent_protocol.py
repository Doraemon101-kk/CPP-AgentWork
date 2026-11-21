"""Protocol regression test; this uses a controlled server, not a real model."""
import json
import subprocess
import threading
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler

from support import APP as ROOT, EXE
seen = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        seen.append(request)
        if len(seen) == 1:
            message = {"role": "assistant", "content": "", "tool_calls": [
                {"function": {"name": "read_text", "arguments": {"path": "main.cpp"}}},
                {"function": {"name": "search_text", "arguments": {"path": ".", "keyword": "not implemented"}}}
            ]}
        else:
            message = {"role": "assistant", "content": "protocol test completed"}
        body = json.dumps({"message": message}).encode()
        self.send_response(200)
        self.end_headers()
        self.wfile.write(body)

server = HTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    result = subprocess.run([
        str(EXE), "--agent",
        str(ROOT / "fixtures/sample_project"), "Inspect source", "test-server",
        str(server.server_port)
    ], capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert result.returncode == 0, result.stderr
    assert len(seen) == 2
    messages = seen[1]["messages"]
    assert len(messages[-3]["tool_calls"]) == 2
    assert messages[-2]["role"] == messages[-1]["role"] == "tool"
    read = json.loads(messages[-2]["content"])
    search = json.loads(messages[-1]["content"])
    assert read["ok"] and "6: int main()" in read["content"]
    assert search["ok"] and "main.cpp:5:" in search["content"]
    print("PASS: HTTP, multiple calls, real files, ordered tool results and final response")
finally:
    server.shutdown()
    server.server_close()
