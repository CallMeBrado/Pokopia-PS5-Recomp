"""Gate the isolated SDK worker stack hook against the tested NSO."""
from hashlib import sha256
from pathlib import Path
import struct
import sys


def main():
    sdk, recompiler, output = map(Path, sys.argv[1:])
    source = sdk.read_bytes()
    expected_hash = "7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2"
    if sha256(source).hexdigest() != expected_hash:
        raise SystemExit("Unrecognized Pokopia SDK for isolated worker stack")

    sys.dont_write_bytecode = True
    sys.path.insert(0, str(recompiler.resolve()))
    from nxrecomp.nso import nso_info

    segment = nso_info(source)["segments"][0]
    expected = (0x91400403, 0xaa0003e2, 0x52820004, 0x2a1f03e5, 0x2a1603e6, 0x94192433)
    if not segment["sha_ok"] or struct.unpack_from("<6I", segment["data"], 0x14bf70) != expected:
        raise SystemExit("The SDK worker's stack argument or call instruction changed")
    output.write_text("inline 750014bf84 # Isolate the worker's dedicated 4 KiB stack from SDK globals\n",
                      encoding="ascii")


if __name__ == "__main__":
    main()
