"""Resolve graphics entry points and startup state in the verified Pokopia SDK."""
from hashlib import sha256
from pathlib import Path
import struct
import sys

BASE = 0x7500000000
INIT = (
    ("POKOPIA_NV_INITIALIZE", "_ZN2nv18InitializeGraphicsEPvm"),
    ("POKOPIA_NV_INITIALIZE_DEFAULT", "_ZN2nv18InitializeGraphicsEv"),
    ("POKOPIA_NV_INITIALIZE_DEVTOOLS", "_ZN2nv26InitializeGraphicsDevtoolsEPvm"),
)
NVN = ("nvnBootstrapLoader", "nvnBootstrapLoaderInternal")
VI = (
    "_ZN2nn2vi10InitializeEv",
    "_ZN2nn2vi8FinalizeEv",
    "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",
    "_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE",
    "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",
    "_ZN2nn2vi12DestroyLayerEPNS0_5LayerE",
    "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",
    "_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb",
    "_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt",
    "_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt",
)
STACK = (
    ("POKOPIA_THREAD_CREATE", 0x149a34, (0xd503237f, 0xa9ba7bfd, 0xf9000bfb, 0xa90267fa, 0xa9035ff8, 0xa90457f6)),
    ("POKOPIA_THREAD_DESTROY", 0x149e90, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4, 0x910003fd, 0x9106a035)),
)


def adrp_target(pc, word):
    immediate = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
    if immediate & 0x100000:
        immediate -= 0x200000
    return (pc & ~0xfff) + (immediate << 12)


def main():
    sdk, recompiler, sites, header = map(Path, sys.argv[1:])
    raw = sdk.read_bytes()
    if sha256(raw).hexdigest() != "7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2":
        raise SystemExit("Unrecognized Pokopia SDK for graphics startup")
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(recompiler.resolve()))
    from nxrecomp.image import Image
    from nxrecomp.nso import nso_info

    info = nso_info(raw)
    code, _, data = info["segments"]
    if not all(segment["sha_ok"] for segment in info["segments"]):
        raise SystemExit("SDK segment integrity check failed")
    exports = {name: value for name, _, _, section, value, _ in Image.from_nso(raw, BASE, "sdk").symbols()
               if section}
    check = exports["_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm"]
    adrp, add, load, compare = struct.unpack_from("<4I", code["data"], check)
    if adrp & 0x9f00001f != 0x90000008 or add & 0xffc003ff != 0x91000108 or (
        load, compare
    ) != (0xb8bfc108, 0x7100051f):
        raise SystemExit("The SDK graphics initialization check changed")
    flag = adrp_target(BASE + check, adrp) + ((add >> 10) & 0xfff)

    nv_check = exports["_ZN2nv9InitCheckEv"]
    words = struct.unpack_from("<23I", code["data"], nv_check)
    pointers = [
        adrp_target(BASE + nv_check + i * 4, first) + (((load >> 10) & 0xfff) << 3)
        for i, (first, load, branch) in enumerate(zip(words, words[1:], words[2:]))
        if first & 0x9f00001f == 0x90000008
        and load & 0xffc003ff == 0xf9400108
        and branch & 0xff00001f == 0xb4000008
    ]
    if len(pointers) != 1:
        raise SystemExit("Could not identify the SDK's checked graphics driver pointer")
    driver = pointers[0]
    data_start = BASE + data["vaddr"]
    data_end = data_start + data["size"] + info["bss"]
    if not (data_start <= flag <= data_end - 4 and data_start <= driver <= data_end - 8):
        raise SystemExit("SDK graphics state is outside its writable segment")

    for name, address, words in STACK:
        if struct.unpack_from(f"<{len(words)}I", code["data"], address) != words:
            raise SystemExit(f"Unexpected thread entry instructions for {name}")

    with sites.open("a", encoding="ascii") as output:
        for name in [name for _, name in INIT] + list(NVN) + list(VI):
            output.write(f"entryhook {BASE + exports[name]:x} # {name}\n")
        for name, address, _ in STACK:
            output.write(f"entryhook {BASE + address:x} # {name}\n")
    constants = {key: BASE + exports[name] for key, name in INIT}
    constants.update({name: BASE + address for name, address, _ in STACK})
    constants.update(POKOPIA_GS_FLAG=flag, POKOPIA_NV_DRIVER_SLOT=driver)
    header.write_text("".join(f"#define {name} UINT64_C({address:#x})\n"
                              for name, address in constants.items()), encoding="ascii")


if __name__ == "__main__":
    main()
