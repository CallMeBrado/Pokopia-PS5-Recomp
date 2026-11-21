from http.client import HTTPConnection
from subprocess import Popen
from time import sleep

def get(token=None, source="127.0.0.1"):
    conn = HTTPConnection("127.0.0.1", 4285, timeout=2, source_address=(source, 0))
    try:
        conn.request("GET", "/frame.jpg", headers={"Authorization": f"Bearer {token}"} if token else {})
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()

token = "a" * 64
with open("client-ip", "w") as output:
    output.write("127.0.0.1\n")
with open("view-token", "w") as output:
    output.write(token + "\n")
app = Popen(["./capture-test"])
try:
    for _ in range(50):
        try:
            assert get()[0] == 403
            break
        except ConnectionRefusedError:
            if app.poll() is not None:
                raise RuntimeError(f"Stream start failed ({app.returncode})")
            sleep(0.02)
    else:
        raise RuntimeError("Stream did not open")
    assert get("b" * 64)[0] == 403
    try:
        get(token, "127.0.0.2")
        raise AssertionError("Foreign source was admitted")
    except (ConnectionError, OSError):
        pass
    assert get(token)[0] == 503
    for _ in range(20):
        sleep(0.1)
        status, frame = get(token)
        if status == 200:
            break
    assert status == 200 and frame.startswith(b"\xff\xd8") and frame.endswith(b"\xff\xd9")
    assert b"\xff\xc0\x00\x11\x08\x02\x1c\x03\xc0" in frame
    print(f"PASS source allowlist, token, initial 503, and 960x540 JPEG ({len(frame)} bytes)")
finally:
    assert app.wait(timeout=10) == 0
