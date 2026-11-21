# Pokopia PS5 port plan

Baseline: PS4 build at run r64 (commit 942a5a0). Target: jailbroken PS5, FW 13.60 (Relapse + etaHEN).

## Why this is tractable

The PS4 port's layers mostly carry over:

| Layer | PS4 today | PS5 | Effort |
|---|---|---|---|
| Lifted game code (`nxrecomp lift`) | Orbis libc++, `-march=btver2` | Rebuild with PS5 SDK, `-march=znver2` | Low |
| Horizon HLE (`horizon/`) | Portable, small Orbis bits | Same code; swap the Orbis bits | Low–medium |
| NVN → Vulkan bridge (`gfx-bridge/`) | RADV (Mesa) on Liverpool via GnmDriver | RADV (Mesa) on PS5 via AGC: [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | Low (same driver family) |
| Presentation | `VK_EXT_headless_surface` + `wsi_orbis` → VideoOut | `VK_KHR_display` → VideoOut (built into PS5 RADV) | Medium |
| Memory | 4.5 GiB, so sparse heap, cold-page spill and `ps4-memory.sites` patches | ~12 GiB direct memory from a PPSA app | Removes a lot of code |
| Packaging | fself + PKG to `/data/pkg` over FTP | PPSA app folder in `/data/homebrew` via ShadowMountPlus | Medium |

Every GPU-heavy PS5 homebrew (Burnout 3 / Max Payne 3 recomps, RecompStore, EglantinePS5) ships as a PPSA native app on the PS5 RADV driver, so that is the path we follow.

## Phases

### 0. Toolchain + "hello Vulkan" on the PS5
- WSL Ubuntu-22.04: install the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) (clang/lld 18), plus the [Mihawk SDK fork](https://github.com/mihawk-99/PS5_PayloadSDK) the RADV titles use.
- Get the PS5_Vulkan RADV static archive. Look at how [PS5_Burnout3](https://github.com/griches/PS5_Burnout3) links it into a PPSA title.
- Start from [ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate) (PPSA layout, param.json).
- Goal: a PPSA app that clears the screen with Vulkan and reads the pad. This proves the toolchain, signing, ShadowMountPlus install and VideoOut.

### 1. Platform layer: `platform/prospero`
Replace the Orbis-only code, keeping the `_WIN32` / Orbis split style:
- **Memory:** `horizon/heap.c`, `live_shared.c`, `stack_backing.h`, `thread.c` stacks. These use direct memory and are mostly API-compatible. Drop `POKOPIA_SPARSE_HEAP`, cold-page spill and `orbis_memory.c` census at first. **The libc malloc heap in PS5 apps is only ~16 MiB**, so route malloc to a direct-memory allocator.
- **Budget:** `boot_budget.c`. Remove the 4608 MiB / 1302 MiB reserve math and size from `sceKernelGetDirectMemorySize`. Build the lifted `main` **without** `ps4-memory.sites` (stock arenas).
- **Input:** `hid_ps4.c` → `hid_ps5.c` (scePad API is near-identical; DualSense).
- **Audio:** `ps4-pcm-native.cpp` → sceAudioOut on PS5 (48 kHz).
- **Misc:** remove the `sceGnmSubmitDone` heartbeat and CE-34878-0 idle-loop workarounds. Check `kern.arandom` and the timer APIs. Move `/data/pokopia/...` paths into one config header.

### 2. Graphics
- Link the PS5 RADV archive instead of `libvulkan_radeon.a` + `liborbis-compat` + vkloader.
- `gpu-present/present.c` + `gfx-bridge/vk_adapter.c`: headless surface → `VK_KHR_display` swapchain. Drop the `ORBIS_*` env knobs.
- Note: AGC only works through linked imports (`dlopen` of libSceAgc fails), so link statically like Burnout 3.

### 3. Build + packaging
- New `build-ps5.ps1` / `build-ps5.sh` modeled on `toolchain/build-probe.ps1`, with PS5 SDK triple/CRT/linker script, `-march=znver2`.
- Output a `PPSAxxxxx/` folder (eboot.bin, sce_sys/param.json, modules/, shaders/). Upload with FTP and install through ShadowMountPlus.
- Keep `monitor-log.py` / `debug-console.py` working against PS5 paths.

### 4. Reach r64 parity, then push forward
- Boot to the same point as PS4 r64 (game graphics init + frames).
- Then continue the shared NVN coverage work that blocks both ports: the unsupported NVN call at the r64 stop, skipped draws (bindless/shadow/buffer textures, driver constant buffers cb0–cb2, >16 vertex attributes / >8 streams, polygon offset).

## Risks / unknowns
- **Fixed guest addresses** (0x71/0x75/0x76/0x77 followed by eight zeros) must be mappable in a PS5 app's address space. Check this first in Phase 0.
- Is 12 GiB shared CPU+GPU? EglantinePS5 reports OOM on heavy games, so keep memory tracing.
- The PS5 RADV driver isn't formally conformant and has no real async compute. That's fine for a start.
- PS5 FW 13.60 exploit is not persistent: rerun Relapse + etaHEN after each reboot. Don't update to 14.00.
