import json, socket, time, sys

def hook(event, sid):
    s = socket.socket(); s.settimeout(1.0); s.connect(("127.0.0.1", 8787))
    s.sendall(json.dumps({"event": event, "session_id": sid, "cwd": "x"}).encode())
    try: s.recv(64)
    except Exception: pass
    s.close()
    print(f"  hook: {event:16s} sid={sid}")

A, B = "sessA", "sessB"
print("T1: A starts"); hook("UserPromptSubmit", A); time.sleep(1)
print("T2: B starts"); hook("UserPromptSubmit", B); time.sleep(1)
print("T3: A finishes (B still working -> expect done, then back to working)")
hook("Stop", A); time.sleep(3)
print("T4: B finishes (last one -> expect all_done)")
hook("Stop", B); time.sleep(3)
print("done")
