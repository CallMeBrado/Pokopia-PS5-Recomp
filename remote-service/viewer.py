"""Open the PS4 LAN service on this PC."""

from argparse import ArgumentParser
from hashlib import blake2s
from http.client import HTTPConnection, HTTPException
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import IPv4Address
from pathlib import Path
from secrets import token_hex
from socket import AF_INET, SOCK_DGRAM, socket
from urllib.parse import urlsplit
from webbrowser import open as open_browser

ROOT = Path(__file__).resolve().parent
SETTINGS = ROOT / "build" / "settings"
CAPTURE_PORT = 4265
CONTROL_PORT = 4264
FRAME_LIMIT = 512 * 1024
SDP_LIMIT = 8192
CONTROL_ASSETS = {
    "/controls": "/",
    "/manifest.webmanifest": "/manifest.webmanifest",
    "/icon-192.png": "/icon-192.png",
}

PAGE = b"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PS4 LAN Service</title>
<style>
  :root { color-scheme: dark; font: 16px system-ui; background: #101722; color: #e9f0fa }
  body { max-width: 1360px; margin: auto; padding: 24px }
  h1 { margin: 0 0 18px; font-size: 1.45rem }
  main { display: grid; grid-template-columns: minmax(0, 3fr) minmax(340px, 2fr); gap: 20px }
  img, iframe { width: 100%; border: 1px solid #33445a; border-radius: 10px; background: #050912 }
  img { aspect-ratio: 16 / 9; object-fit: contain }
  img:not([src]) { visibility: hidden }
  iframe { height: 74vh; min-height: 450px }
  p { color: #a7b8cf }
  @media (max-width: 850px) { main { display: block } }
</style>
<h1>PS4 LAN Service</h1>
<main>
  <section aria-label="Live video">
    <img id="screen" alt="Live PS4 dashboard">
    <p id="status" aria-live="polite">Waiting for the console frame</p>
  </section>
  <section aria-label="Virtual controller">
    <iframe id="controller" title="PS4 virtual controller" allow="gamepad"></iframe>
    <p>Click the controller panel to give it keyboard focus.</p>
  </section>
</main>
<script>
  const display = document.getElementById("screen");
  const status = document.getElementById("status");
  const controller = document.getElementById("controller");
  const frameInterval = 50;
  const retryInterval = 150;
  let previous;
  let previousTag;
  let controlReady = false;
  function showFrameStatus(message) {
    if (status.textContent !== message) status.textContent = message;
  }
  async function checkController() {
    try {
      const reply = await fetch("/controller-ready", {cache:"no-store", signal:AbortSignal.timeout(2500)});
      if (reply.ok && !controlReady) {
        controller.src = "/controls";
        controlReady = true;
      } else if (!reply.ok && controlReady) {
        controller.removeAttribute("src");
        controlReady = false;
      }
    } catch {
      controller.removeAttribute("src");
      controlReady = false;
    }
    setTimeout(checkController, controlReady ? 3500 : 1000);
  }
  async function refresh() {
    const started = performance.now();
    let delay = retryInterval;
    let pending;
    try {
      const headers = previousTag ? {"If-None-Match": previousTag} : {};
      const response = await fetch("/frame.jpg", {cache:"no-store", headers, signal:AbortSignal.timeout(1500)});
      if (response.status !== 304) {
        if (!response.ok) throw new Error();
        pending = URL.createObjectURL(await response.blob());
        const image = new Image();
        image.src = pending;
        await image.decode();
        display.src = pending;
        if (previous) URL.revokeObjectURL(previous);
        previous = pending;
        previousTag = response.headers.get("ETag");
        pending = undefined;
      }
      showFrameStatus("Live dashboard");
      delay = Math.max(0, frameInterval - (performance.now() - started));
    } catch {
      if (pending) URL.revokeObjectURL(pending);
      showFrameStatus(previous ? "Waiting for a new frame" : "Waiting for the console frame");
    }
    setTimeout(refresh, delay);
  }
  refresh();
  checkController();
</script>
</html>
"""


def route_ip(host):
    with socket(AF_INET, SOCK_DGRAM) as route:
        route.connect((str(host), CAPTURE_PORT))
        return IPv4Address(route.getsockname()[0])


def prepare(host):
    SETTINGS.mkdir(parents=True, exist_ok=True)
    (SETTINGS / "client-ip").write_text(f"{route_ip(host)}\n", encoding="ascii")
    try:
        with (SETTINGS / "view-token").open("x", encoding="ascii") as output:
            output.write(token_hex(32) + "\n")
    except FileExistsError:
        pass


def load_settings(directory=SETTINGS):
    client_ip = IPv4Address((directory / "client-ip").read_text(encoding="ascii").strip())
    token = (directory / "view-token").read_text(encoding="ascii").strip()
    if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
        raise ValueError("Invalid capture token in the service settings")
    return client_ip, token


class Viewer(ThreadingHTTPServer):
    def __init__(self, host, port, settings=None):
        self.console = str(host)
        client_ip, self.token = settings or load_settings(SETTINGS)
        self.client_ip = str(client_ip)
        super().__init__(("127.0.0.1", port), RequestHandler)
        self.address = f"127.0.0.1:{self.server_port}"

    def fetch(self, port, path, limit, method="GET", body=None, headers=None):
        connection = HTTPConnection(self.console, port, timeout=2, source_address=(self.client_ip, 0))
        try:
            connection.request(method, path, body, headers or {})
            response = connection.getresponse()
            data = response.read(limit + 1)
            if len(data) > limit:
                raise ValueError("Console response exceeds its limit")
            return response.status, response.headers, data
        finally:
            connection.close()

    def controller_ready(self):
        status, headers, _ = self.fetch(CONTROL_PORT, "/", 2 * 1024 * 1024)
        return status == 200 and headers.get_content_type() == "text/html" and headers.get("Content-Encoding") == "gzip"


class RequestHandler(BaseHTTPRequestHandler):
    def send(self, status, data=b"", content_type="text/plain; charset=utf-8", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if status != 304:
            self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        for name, value in (extra or {}).items():
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def valid_host(self):
        if self.headers.get("Host") != self.server.address:
            self.send(403)
            return False
        return True

    def do_GET(self):
        if not self.valid_host():
            return
        path = urlsplit(self.path).path
        if path == "/":
            return self.send(200, PAGE, "text/html; charset=utf-8", {"X-Frame-Options": "DENY"})
        try:
            if path == "/frame.jpg":
                status, _, frame = self.server.fetch(
                    CAPTURE_PORT, "/frame.jpg", FRAME_LIMIT,
                    headers={"Authorization": f"Bearer {self.server.token}"},
                )
                if status != 200 or not frame.startswith(b"\xff\xd8") or not frame.endswith(b"\xff\xd9"):
                    return self.send(503)
                tag = f'"{blake2s(frame, digest_size=16).hexdigest()}"'
                if self.headers.get("If-None-Match") == tag:
                    return self.send(304, content_type="image/jpeg", extra={"ETag": tag})
                return self.send(200, frame, "image/jpeg", {"ETag": tag})
            if path == "/controller-ready":
                return self.send(204 if self.server.controller_ready() else 503)
            if path in CONTROL_ASSETS:
                status, headers, data = self.server.fetch(CONTROL_PORT, CONTROL_ASSETS[path], 2 * 1024 * 1024)
                if status != 200:
                    return self.send(503, b"Waiting for the PS4 controller service")
                extra = {}
                if encoding := headers.get("Content-Encoding"):
                    extra["Content-Encoding"] = encoding
                if policy := headers.get("Content-Security-Policy"):
                    extra["Content-Security-Policy"] = policy.replace("frame-ancestors 'none'", "frame-ancestors 'self'")
                return self.send(200, data, headers.get("Content-Type", "application/octet-stream"), extra)
        except (OSError, HTTPException, ValueError):
            return self.send(503, b"Waiting for the PS4 service")
        self.send(404)

    def do_POST(self):
        if not self.valid_host():
            return
        if self.path != "/rtc" or self.headers.get("Origin") != f"http://{self.server.address}":
            return self.send(403)
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return self.send(400)
        if not 0 < length <= SDP_LIMIT or self.headers.get_content_type() != "text/plain":
            return self.send(400)
        try:
            self.connection.settimeout(2)
            offer = self.rfile.read(length)
            if len(offer) != length:
                return self.send(400)
            status, headers, answer = self.server.fetch(
                CONTROL_PORT, "/rtc", SDP_LIMIT, "POST", offer, {"Content-Type": "text/plain"},
            )
            return self.send(status, answer, headers.get("Content-Type", "text/plain"))
        except (OSError, HTTPException, ValueError):
            self.send(503, b"Waiting for the PS4 controller service")

    def log_message(self, *_args):
        pass


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=IPv4Address, default="172.30.0.99")
    parser.add_argument("--port", type=int, default=4266)
    parser.add_argument("--prepare", action="store_true", help="generate this PC's package configuration")
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()
    if args.prepare:
        prepare(args.host)
        print(f"Prepared package configuration in {SETTINGS}")
        return
    with Viewer(args.host, args.port) as server:
        url = f"http://{server.address}/"
        print(f"PS4 viewer at {url}", flush=True)
        if not args.no_browser:
            open_browser(url)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
