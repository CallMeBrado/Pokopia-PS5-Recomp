# Third-party code and notices

The project as a whole is licensed under the GNU General Public License, version 3 or later (see
`LICENSE`). It includes or links the following, each under its own license:

| Component | Where | License |
|---|---|---|
| Pokopia Recomp (`drakolordx7/pokopia-recomp`): the Switch 2 recompiler, runtime and shader tools this port builds on | fetched into `upstream/` (not tracked); parts adapted throughout | GPL-3.0 |
| Vulkan loader shim | `ps5/compat/vkloader/` | MIT / Apache-2.0 (see file headers) |
| AMD FidelityFX Super Resolution 1 (`ffx_a.h`, `ffx_fsr1.h`) | `gfx-bridge/shaders/ffx/` | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. |
| DejaVu Sans 2.37 glyphs, rasterised into `gfx-bridge/osk_font.h` | `gfx-bridge/osk_font.h` | Bitstream Vera Fonts license; DejaVu changes are public domain (full notice in the header) |
| Mesa RADV Vulkan driver (PS5 port), linked into the title | built separately (`PS5_Vulkan`) | MIT |
| Opus audio codec | fetched into `upstream/` (not tracked) | BSD-3-Clause |

Sony's PS5 system libraries are used through the console's own system interfaces; they are not
part of this repository.
