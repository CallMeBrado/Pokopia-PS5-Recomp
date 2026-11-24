// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia 2.0.1 uses the same LP64 inflater fields and decode tables as the
// linked RADV zlib 1.3.1. Stream offsets and diagnostic addresses are specific
// to the generated unit verified by patch-lift.py.
#pragma once

struct z_stream_s;
extern "C" void inflate_fast(z_stream_s *, unsigned);

static bool run_native_inflate(Ctx *context) {
    constexpr unsigned len_mode = 0x3f48;
    constexpr unsigned bad_mode = 0x3f51;
    const uint64_t stream = context->x[0];
    if (context->x[30] != 0x71013648f0ULL || stream == 0)
        return false;
    const uint64_t state = rd64(stream + 56);
    const unsigned start = static_cast<unsigned>(context->x[1]);
    const unsigned available_in = rd32(stream + 8);
    const unsigned available_out = rd32(stream + 32);
    if (!state || rd32(state + 8) != len_mode || rd32(state + 7144) != 1 ||
        available_in < 6 || available_out < 258 || start < available_out)
        return false;

    const uint64_t input = rd64(stream);
    const uint64_t output = rd64(stream + 24);
    const uint64_t message = rd64(stream + 48);
    const uint64_t hold = rd64(state + 80);
    const unsigned bits = rd32(state + 88);
    if (bits >= 8 || !input || !output)
        return false;

    inflate_fast(reinterpret_cast<z_stream_s *>(stream), start);
    if (rd32(state + 8) != bad_mode)
        return true;
    const char *error = reinterpret_cast<const char *>(rd64(stream + 48));
    struct Diagnostic { const char *text; uint64_t guest_address; };
    static constexpr Diagnostic diagnostics[] = {
        {"invalid distance code", 0x7102793990ULL},
        {"invalid distance too far back", 0x71027820f5ULL},
        {"invalid literal/length code", 0x71027dd4e3ULL}
    };
    for (const Diagnostic &diagnostic : diagnostics) {
        if (error && strcmp(error, diagnostic.text) == 0) {
            wr64(stream + 48, diagnostic.guest_address);
            return true;
        }
    }
    // Re-enter the original function at the original read/write positions
    // if a different native library returns an unrecognized diagnostic.
    wr64(stream, input);
    wr32(stream + 8, available_in);
    wr64(stream + 24, output);
    wr32(stream + 32, available_out);
    wr64(stream + 48, message);
    wr32(state + 8, len_mode);
    wr64(state + 80, hold);
    wr32(state + 88, bits);
    return false;
}
