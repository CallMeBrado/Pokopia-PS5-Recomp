"""Build-time patches to the locally generated lift (never committed).

The game's frame limiter (main module, function at 0x710089df20) computes its
target rate as 60.0f divided by a helper that returns 1.0. The port's frame cap
replaces that 60.0f constant (0x42700000, loaded into x8 right after the call
that returns to 0x710089e1d0) with pokopia_frame_limit_bits(), so the game's own
limiter, and the real-time delta it derives, runs at 30, 60 or uncapped.

The site is found by that return address and constant alone.

    python3 patch-lift.py <lift dir>        (idempotent; prints what it changed)
"""
import pathlib
import re
import sys

UNIT = "c_050.cpp"
RETURN_ADDRESS = "0x710089e1d0ULL"
CONSTANT = "0x42700000ULL"
MARKER = "/* R96: port frame cap */"
DECLARATION = 'extern "C" uint32_t pokopia_frame_limit_bits(void); /* R96 */\n'


def main():
    path = pathlib.Path(sys.argv[1]) / UNIT
    lines = path.read_text(encoding="utf-8").split("\n")
    if any(MARKER in line for line in lines):
        print(f"patch-lift: {UNIT} already patched")
        return
    sites = [i for i, line in enumerate(lines[:-1])
             if RETURN_ADDRESS in line and CONSTANT in lines[i + 1] and "c->x[8] =" in lines[i + 1]]
    if len(sites) != 1:
        sys.exit(f"patch-lift: {UNIT}: expected one frame-limiter site, found {len(sites)}")
    i = sites[0] + 1
    lines[i] = re.sub(r"\(\s*" + re.escape(CONSTANT) + r"\s*\)", "(pokopia_frame_limit_bits())", lines[i]) + " " + MARKER
    includes = [j for j, line in enumerate(lines) if line.startswith("#include")]
    if not includes:
        sys.exit(f"patch-lift: {UNIT}: no include to declare after")
    lines.insert(includes[-1] + 1, DECLARATION.rstrip("\n"))
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"patch-lift: {UNIT} frame limiter now uses pokopia_frame_limit_bits()")


if __name__ == "__main__":
    main()
