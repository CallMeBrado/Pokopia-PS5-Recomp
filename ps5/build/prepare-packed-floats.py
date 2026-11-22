#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare a private copy of Pokopia Recomp's headers for the PS5 main-module build."""

import hashlib
from pathlib import Path
import sys


# nxrecomp/ctx.h and cxx.h use relative includes. Copy their siblings together so
# only the selected translation units see the replacement, without editing the SDK.
PACKED = r"""#if defined(POKOPIA_PACKED_FLOAT32) && defined(__x86_64__) && defined(__clang__)
typedef float NxFloat4 __attribute__((vector_size(16)));
static __attribute__((noinline)) void v_repair_nan4(V128 *out, const V128 *a, const V128 *b, unsigned mask) {
    for (int i = 0; i < 4; ++i)
        if (mask & (1u << i))
            out->f[i] = fp_nan_result32(2, a->f[i], b->f[i], 0);
}

__attribute__((always_inline)) static inline void v_f3v(V128 *d, const V128 *pn, const V128 *pm, int op, int dbl, int n) {
    if (!dbl && n == 4 && (unsigned)op <= F_MUL) {
        const V128 a = *pn, b = *pm;
        V128 r;
        NxFloat4 left, right, value;
        __builtin_memcpy(&left, &a, sizeof left);
        __builtin_memcpy(&right, &b, sizeof right);
        if (op == F_ADD) value = left + right;
        else if (op == F_SUB) value = left - right;
        else value = left * right;
        __builtin_memcpy(&r, &value, sizeof r);
        /* Check the arithmetic result as the scalar helper does. A floating
         * comparison also preserves its MXCSR denormal-operand flag. */
        const NxFloat4 nan_lanes = __builtin_ia32_cmpps(value, value, 3);
        const unsigned mask = (unsigned)__builtin_ia32_movmskps(nan_lanes);
        if (mask) v_repair_nan4(&r, &a, &b, mask);
        *d = r;
        return;
    }
    v_f3v_scalar(d, pn, pm, op, dbl, n);
}
#else
#define v_f3v v_f3v_scalar
#endif"""


def prepare(source, destination, flags):
    headers = {path.name: path.read_bytes() for path in source.glob("*.h")}
    original = headers["neon.h"].replace(b"\r\n", b"\n")
    # The splice relies on the scalar operation ordering and NaN rules of this version.
    if hashlib.sha256(original).hexdigest() != "cc667aceafa6db6e9c6977a64b91f9926e68b08c58d1d07535ac41fe81813b9b":
        raise ValueError("Unsupported nxrecomp/neon.h for the PS5 packed-float helper")
    signature = b"static inline void v_f3v("
    start = original.index(signature)
    end = original.index(b"\n}", start) + 2
    scalar = original[start:end].replace(b"v_f3v(", b"v_f3v_scalar(", 1)
    headers["neon.h"] = original[:start] + scalar + b"\n\n" + PACKED.encode() + original[end:]

    digest = hashlib.sha256(Path(__file__).with_name("compile-game.sh").read_bytes())
    for flag in flags:
        digest.update(flag.encode() + b"\0")
    for name, contents in sorted(headers.items()):
        digest.update(name.encode() + b"\0" + hashlib.sha256(contents).digest())
    directory = destination / digest.hexdigest()
    include = directory / "include" / "nxrecomp"
    include.mkdir(parents=True, exist_ok=True)
    for name, contents in headers.items():
        path = include / name
        if not path.exists() or path.read_bytes() != contents:
            path.write_bytes(contents)
    return directory


if __name__ == "__main__":
    print(prepare(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]))
