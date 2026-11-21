from http.client import HTTPConnection
from pathlib import Path
from time import sleep
import sys

from playwright.sync_api import sync_playwright

project = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(project))
from viewer import CAPTURE_PORT, FRAME_LIMIT, load_settings

client_ip, token = load_settings()
playwright = sync_playwright().start()
browser = playwright.chromium.launch_persistent_context(
    str(project / "build" / "apollo-controller-browser"),
    executable_path="C:/Program Files/Google/Chrome/Application/chrome.exe",
    headless=True,
)
page = browser.pages[0]
origin = "http://127.0.0.1:65248"
page.route(origin + "/", lambda route: route.fulfill(content_type="text/html", body="<!doctype html><title>PS4 controller</title>"))
page.goto(origin + "/")
page.evaluate('''async () => {
    const peer = new RTCPeerConnection({iceServers: []});
    const control = peer.createDataChannel('control', {negotiated: true, id: 0});
    const input = peer.createDataChannel('input', {negotiated: true, id: 1, ordered: false, maxRetransmits: 0});
    const pending = new Map();
    let id = 0, sequence = 0, pad = null, buttons = 0, r2 = 0, timer = null;
    control.onmessage = event => {
        const message = JSON.parse(event.data);
        if (!pending.has(message.id)) return;
        pending.get(message.id)(message);
        pending.delete(message.id);
    };
    function request(method, params = []) {
        return new Promise((resolve, reject) => {
            const index = ++id;
            const timeout = setTimeout(() => { pending.delete(index); reject(new Error(method + ' timed out')); }, 5000);
            pending.set(index, response => {
                clearTimeout(timeout);
                if (response.error) reject(new Error(JSON.stringify(response.error)));
                else resolve(response.result);
            });
            control.send(JSON.stringify({id: index, method, params}));
        });
    }
    function send() {
        if (pad === null || input.readyState !== 'open') return;
        input.send(JSON.stringify({method: 'i', params: [pad, ++sequence, buttons, 128, 128, 128, 128, 0, r2, 0, 0]}));
    }
    await peer.setLocalDescription(await peer.createOffer());
    const response = await fetch('/rtc', {method: 'POST', headers: {'Content-Type': 'text/plain'}, body: peer.localDescription.sdp});
    if (!response.ok) throw new Error('RTC offer refused: ' + response.status);
    await peer.setRemoteDescription({type: 'answer', sdp: await response.text()});
    await Promise.all([control, input].map(channel => new Promise((resolve, reject) => {
        if (channel.readyState === 'open') return resolve();
        const timeout = setTimeout(() => reject(new Error('RTC channel did not open')), 8000);
        channel.onopen = () => { clearTimeout(timeout); resolve(); };
    })));
    window.padSession = {
        request,
        async claim(index) {
            const result = await request('claim', [index]);
            if (!result.pads[index].mine) throw new Error('Controller was not assigned to this session');
            pad = index;
            timer = setInterval(send, 450);
            return result.pads[index].state;
        },
        async tap([mask, trigger, ms]) {
            if (pad === null) throw new Error('Claim a controller first');
            buttons = mask; r2 = trigger; send();
            await new Promise(resolve => setTimeout(resolve, ms));
            buttons = 0; r2 = 0; send();
        },
        async close() {
            clearInterval(timer);
            if (pad !== null) await request('claim', []);
            peer.close();
        },
    };
}''')

BUTTONS = dict(up=0x10, right=0x20, down=0x40, left=0x80, cross=0x4000, circle=0x2000,
               square=0x8000, triangle=0x1000, ps=0x10000, options=0x8, r2=0x200)


def status():
    pads = page.evaluate("padSession.request('status')")["pads"]
    return [{key: pad.get(key) for key in ("state", "mine", "clients")} for pad in pads]


def claim():
    index = next(i for i, pad in enumerate(status()) if pad["state"] == "free" and not pad["clients"])
    return page.evaluate("index => padSession.claim(index)", index)


def tap(key, wait=.45, hold=120):
    page.evaluate("input => padSession.tap(input)", [BUTTONS[key], 255 if key == "r2" else 0, hold])
    sleep(wait)


def capture():
    connection = HTTPConnection("172.30.0.99", CAPTURE_PORT, timeout=2, source_address=(str(client_ip), 0))
    try:
        connection.request("GET", "/frame.jpg", headers={"Authorization": "Bearer " + token})
        response = connection.getresponse()
        image = response.read(FRAME_LIMIT + 1)
        if response.status == 200 and image.startswith(b"\xff\xd8") and image.endswith(b"\xff\xd9"):
            (project / "build" / "ps4-apollo-view.jpg").write_bytes(image)
            print("PS4 capture updated")
        else:
            print("Capture HTTP", response.status)
    finally:
        connection.close()


def close():
    try:
        page.evaluate("padSession.close()")
    finally:
        browser.close()
        playwright.stop()


print("PS4 controller ready:", status(), flush=True)
