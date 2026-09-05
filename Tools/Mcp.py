"""Call the editor's MCP server directly over HTTP.

Claude Code binds MCP servers once, at session start. An editor opened after that is
invisible to the session's tool list for the rest of the session, and restarting the
session throws away its context. This script is the fallback: the same JSON-RPC the
harness would send, from the shell, against the running editor.

    python Tools/Mcp.py toolsets                    # list toolsets
    python Tools/Mcp.py describe EditorToolset      # tools in one toolset
    python Tools/Mcp.py call EditorToolset GetEditorState
    python Tools/Mcp.py call EditorToolset SomeTool '{"arg": 1}'
    python Tools/Mcp.py call - list_toolsets        # top-level tool, no toolset
    python Tools/Mcp.py shot out.png               # level viewport (PIE view when playing)
    python Tools/Mcp.py shot out.png editor        # the whole editor window as the user sees it
    python Tools/Mcp.py log LogRoadBuild [regex] [n]  # tail the running session's log by category

Tool names are the short form (IsPIERunning, not EditorToolset.EditorAppToolset.IsPIERunning);
the fully qualified form is rejected as unknown.

A new MCP session is opened per invocation; the server hands out the id in the
Mcp-Session-Id header on initialize and expects it on every later request.
"""
import json
import sys
import urllib.request

URL = "http://localhost:8000/mcp"


def post(body, session=None):
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    }
    if session:
        headers["Mcp-Session-Id"] = session
    req = urllib.request.Request(URL, json.dumps(body).encode(), headers, method="POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read().decode("utf-8", "replace")
        return r.headers.get("Mcp-Session-Id"), (json.loads(raw) if raw.strip() else None)


def rpc(session, rid, method, params=None):
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    _, res = post(body, session)
    if res is None:
        return None
    if "error" in res:
        sys.exit(f"MCP error: {json.dumps(res['error'], indent=1)}")
    return res["result"]


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    try:
        session, _ = post({
            "jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                       "clientInfo": {"name": "AirportMgr Tools/Mcp.py", "version": "1"}},
        })
    except OSError as e:
        sys.exit(f"Editor MCP server not reachable at {URL}: {e}")
    post({"jsonrpc": "2.0", "method": "notifications/initialized"}, session)

    cmd = argv[1]
    if cmd == "toolsets":
        result = rpc(session, 1, "tools/call", {"name": "list_toolsets", "arguments": {}})
    elif cmd == "describe":
        result = rpc(session, 1, "tools/call",
                     {"name": "describe_toolset", "arguments": {"toolset_name": argv[2]}})
    elif cmd == "call":
        toolset, tool = argv[2], argv[3]
        args = json.loads(argv[4]) if len(argv) > 4 else {}
        payload = {"tool_name": tool, "arguments": args}
        if toolset != "-":
            payload["toolset_name"] = toolset
        result = rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": payload})
    elif cmd == "shot":
        import base64
        out = argv[2]
        whole = len(argv) > 3 and argv[3] == "editor"
        tool = "CaptureEditorImage" if whole else "CaptureViewport"
        args = {}
        if not whole:
            # CaptureViewport rejects a missing captureTransform ("needs a default value")
            # despite the schema calling it optional, so capture from the current camera
            # explicitly. Annotations are the same: all six fields or the call fails.
            cam = rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": {
                "toolset_name": "EditorToolset.EditorAppToolset",
                "tool_name": "GetCameraTransform", "arguments": {}}})
            cam = json.loads("".join(b.get("text", "") for b in cam["content"]))["returnValue"]
            args = {"captureTransform": cam, "bShowUI": True,
                    "annotations": {"gridSpacing": 0, "gridExtent": 0, "gridHeight": 0,
                                    "maxLabelDistance": 0, "classFilter": {"refPath": ""},
                                    "maxLabels": 0}}
        result = rpc(session, 2, "tools/call", {"name": "call_tool", "arguments": {
            "toolset_name": "EditorToolset.EditorAppToolset", "tool_name": tool,
            "arguments": args}})
        text = "".join(b.get("text", "") for b in result.get("content", []))
        if result.get("isError"):
            sys.exit(text)
        payload = json.loads(text)["returnValue"]
        image = payload if whole else payload["image"]
        if not image.get("data"):
            sys.exit("Capture returned no image")
        with open(out, "wb") as f:
            f.write(base64.b64decode(image["data"]))
        print(f"{out}: {image['mimeType']}, {len(image['data']) * 3 // 4} bytes")
        return
    elif cmd == "log":
        args = {"category": argv[2], "pattern": argv[3] if len(argv) > 3 else "",
                "maxEntries": int(argv[4]) if len(argv) > 4 else 50}
        result = rpc(session, 1, "tools/call", {"name": "call_tool", "arguments": {
            "toolset_name": "EditorToolset.LogsToolset", "tool_name": "GetLogEntries",
            "arguments": args}})
        text = "".join(b.get("text", "") for b in result.get("content", []))
        for line in json.loads(text)["returnValue"]:
            print(line)
        return
    else:
        sys.exit(__doc__)

    # Tool results arrive as content blocks; print text blocks plainly, everything else as JSON.
    for block in result.get("content", []):
        if block.get("type") == "text":
            print(block["text"])
        else:
            print(json.dumps(block, indent=1))
    if result.get("isError"):
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv)
