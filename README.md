# Pokopia-PS5-Recomp

Unofficial, fan-made port of Pokémon Pokopia (Nintendo Switch 2) to a jailbroken PS5, built on
the static recompilation from [Pokopia Recomp](https://github.com/drakolordx7/pokopia-recomp).
Not affiliated with Nintendo, The Pokémon Company, Creatures, GAME FREAK, KOEI TECMO or Sony.
See [LEGAL.md](LEGAL.md).

**This repository contains no game code or data, and no builds are distributed.** You need your
own copy of the game (version 2.0.1a), which you dump and recompile yourself.

## What it does

- **Recompiled game code** runs natively on the PS5's CPU (the recompiler output is generated
  locally from your dump and never committed).
- **Horizon OS emulation** (`horizon/`): the Switch services the game uses (filesystem and saves,
  HID mapped to the DualSense, audio, applets including a software keyboard).
- **NVN → Vulkan bridge** (`gfx-bridge/`): the game's NVN command buffers are replayed into Vulkan
  on the PS5's RADV driver ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), with FSR 1
  upscaling and PlayStation button prompts.
- **Packaging** (`ps5/`): a PPSA native app (title `PPSA94010`) run through etaHEN / ShadowMountPlus.

## Status

Boots to the title screen, loads and writes saves, and plays in the world. Around 55-60 fps
indoors and 47-53 fps outdoors. Button prompts, including the title screen's, show PlayStation
glyphs. Performance notes and plans are in
[docs/PS5_60FPS_PLAN.md](docs/PS5_60FPS_PLAN.md); the original port plan is in
[docs/PS5_PORT_PLAN.md](docs/PS5_PORT_PLAN.md).

## Port settings

Hold the touch pad for about a second, anywhere in the game, to open the port's own settings
(tapping it still opens the game's Settings as usual):

- FPS counter and debug info overlays
- Button prompts: PlayStation or the original Nintendo glyphs
- Confirm button: Circle (Japanese / Nintendo layout) or Cross (Western)
- Frame rate cap: 60 or 30
- Upscaler: FSR 1 or bilinear, and FSR sharpness

They are saved to `/data/pokopia/port-settings.ini` on the console.

## Building (outline)

The build runs in a WSL Ubuntu environment with the PS5 payload SDK and the PS5_Vulkan RADV
archive (`ps5/scripts/setup-build-host.sh`, `ps5/build/env.sh`):

1. Recompile your game dump with the Pokopia Recomp tools and build its shader cache.
2. `ps5/build/compile-game.sh`, `ps5/build/compile-runtime.sh`, then `ps5/build/link-title.sh`
   produce the title folder in `ps5/dist/PPSA94010`.
3. Copy the title and game data to the console and launch it (`ps5/scripts/deploy.py`,
   `ps5/scripts/ps5-ctl.py`). Set `PS5_HOST` to your console's IP address.

Expect to adapt paths: this is a research codebase, and much of `boot/` is experiment history.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). Third-party components and their licenses are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
