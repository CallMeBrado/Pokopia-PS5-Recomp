"""Extract Pokopia SDK startup through its first Horizon syscall."""

from pathlib import Path
import re

SDK_BASE = 0x7500000000
# Follow nn::init::Start to the first GetInfo call, RandomEntropy (subtype 2).
ENTRY_OFFSETS = (
    0x118E90, 0x118F00, 0x797DE0, 0x6C05D0,
    0x797DB0, 0x151860, 0x151A40, 0x792210, 0x6D7820,
    0x799DB0, 0x152040, 0x798ED0, 0x151E94, 0x180D74,
)
FUNCTION_START = re.compile(r"^(?:NX_OPTNONE )?void (f_[0-9a-f]+)\(Ctx \*__restrict c\) \{", re.M)
DIRECT_CALL = re.compile(r"\b(f_[0-9a-f]+)\(c\)")


def main():
    root = Path(__file__).resolve().parent
    wanted = {f"f_{SDK_BASE + offset:x}" for offset in ENTRY_OFFSETS}
    functions = {}
    for source in sorted((root / "lift").glob("c_*.cpp")):
        text = source.read_text(encoding="utf-8")
        starts = list(FUNCTION_START.finditer(text))
        for index, start in enumerate(starts):
            name = start[1]
            if name not in wanted:
                continue
            if name in functions:
                raise ValueError(f"duplicate SDK function {name}")
            limit = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            functions[name] = text[start.start():limit].strip()

    missing = wanted - functions.keys()
    if missing:
        raise ValueError(f"missing SDK functions: {', '.join(sorted(missing))}")

    trap_refs = {called for body in functions.values() for called in DIRECT_CALL.findall(body)}
    trap_refs -= wanted
    output = root / "sdk_entry.cpp"
    with output.open("w", encoding="ascii", newline="\n") as file:
        file.write("// Generated from the local lift. Recreate with make-entry.py.\n")
        file.write("#include <nxrecomp/ctx.h>\n\n")
        file.write('#define NX_OPTNONE __attribute__((optnone, noinline))\n\n')
        for name in sorted(wanted | trap_refs):
            file.write(f'extern "C" void {name}(Ctx *);\n')
        for name in sorted(wanted):
            file.write(f"\n{functions[name]}\n")
        for name in sorted(trap_refs):
            address = name.removeprefix("f_")
            file.write(f'\nvoid {name}(Ctx *) {{ guest_trap("SDK startup escaped the first-SVC probe", 0x{address}ULL); }}\n')

    print(f"{output}: {len(functions)} SDK functions, {len(trap_refs)} guarded off-path calls")


if __name__ == "__main__":
    main()

