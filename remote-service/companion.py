"""Windows connection manager for the PS4 LAN Service."""

import sys
import tkinter as tk
from ipaddress import IPv4Address, IPv4Network
from pathlib import Path
from queue import Empty, SimpleQueue
from socket import AF_INET, SO_BROADCAST, SOCK_DGRAM, SOL_SOCKET, create_connection, socket
from subprocess import CREATE_NO_WINDOW, Popen, SubprocessError, run
from threading import Thread
from time import monotonic
from tkinter import filedialog, messagebox, ttk
from webbrowser import open as open_browser

from viewer import CAPTURE_PORT, CONTROL_PORT, SETTINGS, Viewer, load_settings, route_ip

DISCOVER = b"P4LAN-DISCOVER/1"
DISCOVERY_REPLY = b"P4LAN-SERVICE/1"
DEFAULT_HOST = "172.30.0.99"
SETTINGS_DIR = Path(sys.executable).resolve().parent / "settings" if getattr(sys, "frozen", False) else SETTINGS
CHIAKI_EXECUTABLES = ("chiaki.exe", "chiaki-ng.exe")


def adapter_network(client_ip):
    command = (
        f"(Get-NetIPAddress -AddressFamily IPv4 -IPAddress '{client_ip}' "
        "-ErrorAction Stop | Select-Object -First 1).PrefixLength"
    )
    try:
        result = run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, check=True, timeout=3, creationflags=CREATE_NO_WINDOW,
        )
        return IPv4Network(f"{client_ip}/{int(result.stdout.strip())}", strict=False)
    except (OSError, SubprocessError, ValueError):
        return None


def discover(client_ip, network, timeout=1.6, port=CAPTURE_PORT):
    broadcasts = {IPv4Address("255.255.255.255")}
    if network and network.prefixlen <= 30:
        broadcasts.add(network.broadcast_address)
    results = set()
    with socket(AF_INET, SOCK_DGRAM) as connection:
        connection.setsockopt(SOL_SOCKET, SO_BROADCAST, 1)
        connection.bind((str(client_ip), 0))
        sent = False
        for address in broadcasts:
            try:
                connection.sendto(DISCOVER, (str(address), port))
                sent = True
            except OSError:
                continue
        if not sent:
            raise OSError("Discovery could not broadcast from the configured PC address.")
        deadline = monotonic() + timeout
        while (remaining := deadline - monotonic()) > 0:
            connection.settimeout(remaining)
            try:
                message, (sender, sender_port) = connection.recvfrom(65535)
            except TimeoutError:
                break
            address = IPv4Address(sender)
            if (
                message == DISCOVERY_REPLY and sender_port == port and address != client_ip
                and not (address.is_multicast or address.is_unspecified)
                and (network is None or address in network)
            ):
                results.add(address)
    return sorted(results)


def checked_host(value, client_ip, network):
    try:
        host = IPv4Address(value.strip())
    except ValueError as error:
        raise ValueError("Enter the PS4's IPv4 address, for example 172.30.0.99.") from error
    if (
        host == client_ip or host.is_unspecified or host.is_multicast or host.is_loopback
        or (network and host not in network)
        or (network and network.prefixlen <= 30 and host in (network.network_address, network.broadcast_address))
        or (network is None and host.is_global)
    ):
        raise ValueError("Use the PS4's address on the configured PC adapter's local network.")
    if route_ip(host) != client_ip:
        raise ValueError(f"Windows routes to that address through another adapter. This package uses {client_ip}.")
    return host


def reachable_host(value, client_ip):
    host = checked_host(value, client_ip, adapter_network(client_ip))
    for port in (CAPTURE_PORT, CONTROL_PORT):
        try:
            with create_connection((str(host), port), timeout=0.8, source_address=(str(client_ip), 0)):
                return host
        except OSError:
            continue
    raise OSError(f"No PS4 LAN Service answered at {host}. Open the service on the console and retry.")


def remote_play_client(settings_dir):
    saved = settings_dir / "remote-play-client"
    try:
        path = Path(saved.read_text(encoding="utf-8").strip())
        if path.is_absolute() and path.name.lower() in CHIAKI_EXECUTABLES and path.is_file():
            return path
    except (OSError, ValueError):
        pass
    for name in CHIAKI_EXECUTABLES:
        path = settings_dir.parent / "chiaki-ng" / name
        if path.is_file():
            return path
    return None


class Companion:
    def __init__(self, root, settings):
        self.root = root
        self.settings = settings
        self.results = SimpleQueue()
        self.server = None
        self.discovery_addresses = []
        self.address = tk.StringVar(value=DEFAULT_HOST)
        self.status = tk.StringVar(value="Enter the PS4 address or discover a console.")

        root.title("PS4 Companion")
        root.minsize(520, 405)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        panel = ttk.Frame(root, padding=22)
        panel.grid(sticky="nsew")
        panel.columnconfigure(0, weight=1)
        panel.rowconfigure(4, weight=1)
        ttk.Label(panel, text="PS4 Companion", font=("Segoe UI", 19, "bold")).grid(sticky="w")
        ttk.Label(panel, text=f"LAN service · This PC: {settings[0]}").grid(sticky="w", pady=(2, 16))
        ttk.Label(panel, text="PS4 IPv4 address").grid(sticky="w")
        connection = ttk.Frame(panel)
        connection.grid(sticky="ew", pady=(4, 14))
        connection.columnconfigure(0, weight=1)
        self.entry = ttk.Entry(connection, textvariable=self.address)
        self.entry.grid(row=0, column=0, sticky="ew")
        self.entry.bind("<Return>", lambda _event: self.connect())
        self.connect_button = ttk.Button(connection, text="Connect", command=self.connect)
        self.connect_button.grid(row=0, column=1, padx=(8, 0))

        discovered = ttk.LabelFrame(panel, text="Discovered consoles", padding=8)
        discovered.grid(sticky="nsew")
        discovered.columnconfigure(0, weight=1)
        discovered.rowconfigure(0, weight=1)
        self.list = tk.Listbox(discovered, height=5, exportselection=False)
        self.list.grid(row=0, column=0, sticky="nsew")
        self.list.bind("<<ListboxSelect>>", self.pick)
        self.list.bind("<Double-Button-1>", self.pick_and_connect)
        self.discover_button = ttk.Button(discovered, text="Discover", command=self.find_consoles)
        self.discover_button.grid(row=0, column=1, sticky="n", padx=(8, 0))
        ttk.Label(panel, textvariable=self.status, wraplength=465).grid(sticky="w", pady=(12, 8))
        actions = ttk.Frame(panel)
        actions.grid(sticky="ew")
        actions.columnconfigure(0, weight=1)
        ttk.Button(actions, text="Remote Play…", command=self.open_remote_play).grid(row=0, column=0, sticky="w")
        self.disconnect_button = ttk.Button(actions, text="Disconnect", command=self.disconnect, state="disabled")
        self.disconnect_button.grid(row=0, column=1, sticky="e")
        root.after(75, self.poll)
        self.entry.focus_set()

    def run_background(self, operation, task):
        self.connect_button.state(["disabled"])
        self.discover_button.state(["disabled"])

        def worker():
            try:
                self.results.put((operation, task(), None))
            except (OSError, ValueError) as error:
                self.results.put((operation, None, str(error)))

        Thread(target=worker, daemon=True).start()

    def find_consoles(self):
        self.status.set("Looking for PS4 LAN Service on this network…")
        client_ip, _ = self.settings
        self.run_background("discovery", lambda: discover(client_ip, adapter_network(client_ip)))

    def connect(self):
        if "disabled" in self.connect_button.state():
            return
        value = self.address.get()
        client_ip, _ = self.settings
        self.status.set("Checking the address…")
        self.run_background("connection", lambda: reachable_host(value, client_ip))

    def pick(self, _event):
        selected = self.list.curselection()
        if selected:
            self.address.set(str(self.discovery_addresses[selected[0]]))

    def pick_and_connect(self, event):
        self.pick(event)
        if self.list.curselection():
            self.connect()

    def poll(self):
        try:
            operation, value, error = self.results.get_nowait()
        except Empty:
            self.root.after(75, self.poll)
            return
        self.connect_button.state(["!disabled"])
        self.discover_button.state(["!disabled"])
        if error:
            self.status.set(error)
        elif operation == "discovery":
            self.discovery_addresses = value
            self.list.delete(0, "end")
            for host in value:
                self.list.insert("end", str(host))
            self.status.set(
                "Select a console and click Connect." if value else
                "No console replied. Start the updated PS4 LAN Service or enter the address manually."
            )
        else:
            self.open_viewer(value)
        self.root.after(75, self.poll)

    def open_viewer(self, host):
        if not self.server or self.server.console != str(host):
            self.disconnect()
            try:
                self.server = Viewer(host, 0, self.settings)
            except OSError as error:
                self.status.set(f"Could not start the PC viewer: {error}")
                return
            Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
        url = f"http://{self.server.address}/"
        self.disconnect_button.state(["!disabled"])
        if open_browser(url):
            self.status.set(f"Viewer opened for {host}. It will show the dashboard and controller when ready.")
        else:
            self.status.set(f"Open {url} in a browser to connect to {host}.")

    def open_remote_play(self):
        client = remote_play_client(SETTINGS_DIR)
        selected = client is None
        if selected:
            path = filedialog.askopenfilename(
                parent=self.root, title="Choose the Chiaki-ng Remote Play client",
                initialdir=SETTINGS_DIR.parent,
                filetypes=[("Chiaki-ng executable", CHIAKI_EXECUTABLES)],
            )
            if not path:
                return
            client = Path(path)
            if not client.is_absolute() or client.name.lower() not in CHIAKI_EXECUTABLES or not client.is_file():
                self.status.set("Choose chiaki.exe from the official Chiaki-ng Windows package.")
                return

        try:
            Popen([str(client)], cwd=client.parent, creationflags=CREATE_NO_WINDOW)
        except OSError as error:
            self.status.set(f"Could not open Chiaki-ng. {error}")
            return
        if selected:
            try:
                (SETTINGS_DIR / "remote-play-client").write_text(str(client), encoding="utf-8")
            except OSError:
                self.status.set("Chiaki-ng opened. Windows could not save the client path.")
                return
        self.status.set(f"Chiaki-ng opened. Pair there or select your PS4 at {self.address.get().strip()}.")

    def disconnect(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server = None
        self.disconnect_button.state(["disabled"])
        self.status.set("Disconnected.")

    def close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    root.withdraw()
    try:
        settings = load_settings(SETTINGS_DIR)
    except (OSError, UnicodeError, ValueError) as error:
        messagebox.showerror("PS4 Companion", f"Could not read the PS4 LAN Service pairing files.\n\n{error}")
        root.destroy()
        return
    Companion(root, settings)
    root.deiconify()
    root.mainloop()


if __name__ == "__main__":
    main()
