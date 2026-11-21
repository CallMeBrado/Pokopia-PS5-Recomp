# PS5: path from ~50 fps to 60 fps

State on 2026-11-21 (R79–R81, uncommitted on top of 535b54f): the tutorial room runs at a steady
50–53 fps, with stretches at 59 and one ~1 s dip to 20–35 fps about once a minute.

The game asks for 60 fps (present interval 1, FIFO), so every frame has to fit in 16.6 ms.

## Where a frame goes now

Totals are per 5 s window at ~50 fps (`Load stats`, `Thread busy`, `Graphics lock waits`).

| Thread | Busy | Role |
|---|---|---|
| t999 replay worker | ~89% (holds the graphics lock 4.4 s / 5 s) | Bottleneck. ~1,800 draws a frame at ~5.8 µs each, plus present. |
| t1 main thread | ~68% | ~1.2 s of that waits on the graphics lock in `nvnQueuePresentTexture` (frame pacing behind the worker). |
| t24 render thread | ~58% | Records the next frame. No longer waits on any lock. |

## 1. Asynchronous present (R81, written and built, NOT yet tested)

- **Problem:** present was fully synchronous. `gpu-present/present.c` waited on a fence for the
  acquire, then submitted the blit and waited for the GPU to finish everything queued before it.
  Every frame, the worker sat out the GPU's whole frame: ~27% of worker samples were RADV
  `queue_wait_seq`/`sleep_us` with clock reads.
- **Change:**
  - `present.c` now keeps three frames in flight: per-frame command buffers, fences, and
    acquire/render semaphores.
  - Acquire signals a semaphore, the blit waits on it and signals the present semaphore, and
    nothing waits on the CPU.
  - `live_present.c` records into the frame's own command buffer.
  - Six Vulkan thunks were added to `ps5/compat/vkloader/vkthunks.c`. That file is generated, so
    regenerate it with `gen.py` later.
- **Risk:** whether binary semaphores work end to end on PS5 RADV and the VideoOut WSI. The WSI
  code says it handles present wait semaphores. If presents fail, the log prints
  "Live PS4 VideoOut present failed". In that case try a fence-only variant: keep the per-frame
  rings but drop the semaphores, relying on single-queue ordering.
- **Expected:** removes most of the worker's GPU waits, likely enough for 60 on its own if the
  GPU isn't the limit.

## 2. Cut the worker's per-draw cost (~5.8 µs/draw → under 4 µs)

`General draw timing` and `prof-host.py --thread t999` show where it goes:

- **Descriptors (~0.75 µs/draw):** cache descriptor sets by content (texture views, samplers,
  uniform ranges) and reuse a set when a draw binds the same resources as an earlier one. Many
  draws share materials.
- **Pipeline lookup (~0.5 µs):** keep the last pipeline key and skip the hash when the state
  matches. Cache the hash per program pair.
- **Record (~0.85 µs):** skip `vkCmdSetViewport`/`vkCmdSetScissor`/`vkCmdBindPipeline`/
  `vkCmdBindVertexBuffers` when unchanged from the previous draw in the batch.
- **Upload (~0.5 µs):** `generic_upload` zeroes and refills a support buffer (73 floats) every
  draw. Build it once, and upload uniforms only when the range or contents changed.
- **Resolve in `run_draw_generic` (~2 µs outside the live draw):**
  - `draw_span` calls `nvn_front_resolve_pool`, which walks every memory pool on every
    uniform, stream and index lookup. Add a sorted pool index or a last-hit cache.
  - `generic_resolve_texture` calls the front per texture per draw; cache it by
    (pool, handle) within a job.
- **Smaller items:**
  - `sync_linear` and `pokopia_vk_texel_refresh` show kernel calls per draw; check how often
    they actually copy.
  - Emulated TLS (`__emutls_get_address`): read the thread-locals once per call into a local
    struct.

## 3. Shorten the main thread's present path

`nvnQueuePresentTexture` still takes the graphics lock and waits ~4.5 ms per frame for the
worker's yield. It only queues worker item 2, so it can run under the recording lock alone,
like FenceSync (it already releases the locks when the worker queue is full).
`nvnWindowAcquireTexture` could go the same way once its window access is checked against
`present_nvn_frame_execute`.

## 4. Fix the periodic ~1 s dip (fps 20–35 once a minute)

Not investigated yet. Capture a profile during a dip (`watch-world.py` shows which window) and
check for shader/pipeline creation, texture creation or texel uploads, cold-texture paging (off
today), or a log flush.

## 5. If the worker is still the limit: two workers

Split the replay so the CPU resolve (front lookups, guest reads, descriptor and pipeline choice)
runs on one thread and Vulkan recording on another, joined by a per-draw packet queue. This is
a bigger change. Do it only if 1–3 fall short.

## 6. Measuring

- **Loop:**
  1. `compile-runtime.sh` and `link-title.sh`
  2. `ps5-ctl.py push`
  3. `play-intro.py` (~5 min)
  4. `watch-world.py --max-s 60`
- **What to read:**
  - The FPS counter, top-right.
  - The `Graphics lock waits by call` line.
  - Per-thread `gfx_wait/rec_wait/gfx_hold` (worker `gfx_hold` should drop below ~3.3 s / 5 s).
  - `General draw timing`.
  - `prof-host.py`.
- **Saves:** they still don't persist, so every run replays the intro. Fixing saves would cut
  each test from ~8 min to ~3.

## What R79–R81 changed (for reference)

- **Recording lock:**
  - `nvnCommandBuffer*` recording, queue submit/fence/flush, `nvnSyncWait`, front getters,
    descriptor registration and window getters take a recording lock instead of the graphics
    lock. The worker takes only the graphics lock.
  - Lock order: graphics → recording → global.
- **Unhandled indirect calls:** calls into SDK-internal NVN/VI code that no adapter handles
  return before taking any lock. 12.8k calls had been waiting 6.7 s per 5 s behind the worker.
- **Guest-heap fast path:** guest heap reads and writes skip the memory query and the global
  lock, unless a stack alias overlaps. This was the hidden cap: worker cache misses waited on
  the global lock.
- **Faster texture front:**
  - Front textures are hashed with O(1) unlink. Registration went from 7 µs to 0.7 µs per call.
  - Textures released during a lock-free call are destroyed at the next locked call.
- **Per-draw replay:** each shader's resource list is parsed once, and the index min/max scan
  is skipped for streams bound in place.

## Outdoors (R89, measured 2026-11-21)

Outdoors in the first village area runs at 40–42 fps. It is CPU-bound, not GPU-bound:

| | Per 5 s window |
|---|---|
| Draws | ~540,000 (~2,650 a frame; indoors ~700) |
| Replay worker (t999) | holds the graphics lock ~4.7 s (94%), ~6.5 µs per draw |
| Render thread (t24) | ~3.6 s busy (72%): it would cap near 57 fps even with a free worker |
| GPU (draw batches, timestamped) | ~1.7–2.0 s busy (35–40%) |
| Present ring waits for the GPU | none |

What R89 changed:

- **Shadow readback** (`live_texture_upload.c`): the game reads 1x1/8x8 textures back about once a
  second outdoors (likely auto-exposure). Each read used to copy and then `vkQueueWaitIdle`
  (~110 ms with the lock held). Now a copy is recorded at the end of the draw batch that wrote the
  texture, and the read waits only for that batch (`readbacks shadow=` in the log).
- **GPU timestamps** around every draw batch (`GPU draw batches= busy_ms=`), and present-ring
  wait time (`Present GPU-behind waits=`).
- A 6-slot batch ring was tried: worker fence waits halved but per-draw cost rose ~15%; reverted
  to 2.
- Nested NVN calls (game memory callbacks) no longer charge the outer call's time to the inner
  call's name in `Graphics stats top`.

To reach 60 outdoors, the worker needs ~4.5 µs per draw or less (section 2, then section 5), and
the render thread's per-call cost has to fall too: `nvnBufferGetAddress` alone is ~4,000 calls a
frame (~2.5 ms), then the bind/draw entry points.

## R90: per-draw cost round (2026-11-21)

Outdoors went from ~41 fps to 47–53 fps. The exact figure depends on the scene, and the scene
changes between boots: draws per frame drift from ~2,580 to ~2,650, probably with the real-time
clock. Compare changes inside one session with `debug/flags` (below), not across boots.

| Change | Effect |
|---|---|
| `prepare_sample` remembers textures prepared this guest submit (`live_texture_upload.c`) | skips ~800k checks / 5 s; small |
| Small uploads (≤512 KiB) on the replay worker go through an 8 MiB host ring, recorded into the open draw batch (no submit, no wait) | removed two GPU drains a frame (the per-frame 2x1 RGBA32F parameter texture); prepare 0.9 s → 0.17 s / 5 s |
| A draw batch holding ≥32 draws is submitted at a guest fence sync (`execute_nvn_sync`) | without it, `nvnSyncWait` included whole batches: 48 vs 40 fps |
| Lock-free NVN calls use the lock-free heap check (`read_guest`/`write_guest`) | `nvnBufferGetAddress` 0.63 → 0.18 µs, ~1M calls / 5 s |
| Recording-only calls skip the global kernel lock (recording lock only) | kernel-lock acquisitions ~3M → ~190k / 5 s; render thread 78% → ~60% busy |
| Redundant pipeline / viewport / scissor / descriptor-set binds skipped | record 0.61 → 0.35 s / 5 s; no fps change (A/B) |

Ring bug found on the way: a ring segment last used by the *open* batch was treated as reusable
after a wrap. During loading, one batch stages more than 8 MiB, so later uploads overwrote copies
that hadn't run yet and some textures were wrong (blue trees, stippled rock). Fixed: any segment
entered anew must belong to a retired batch.

`debug/flags` in the mailbox (decimal; read every 0.5 s) for in-session A/B
(`F:\Pokopia-work\ab.py 0 1 0 1`):

| Bit | Value | Effect |
|---|---|---|
| 0 | 1 | no bind skipping |
| 1 | 2 | no batch submit at fences |
| 2 | 4 | submit at every fence regardless of size (measured 40 fps, worse) |
| 3 | 8 | also submit after each guest job with ≥64 draws (neutral/worse) |

Where it stands at ~48 fps:

- The replay worker is ~78% busy and idles ~0.6 s / 5 s waiting for work.
- The game waits ~0.7 s / 5 s in `nvnSyncWait`.
- The GPU is ~40% busy.
- `vkQueueSubmit` costs ~0.16 ms (1,500 batch submits / 5 s).

The frame is now latency-bound: game records → worker replays (~16 ms) → GPU runs (~8.5 ms) → the
fence the game waits on. More submits make it worse, probably because each submission boundary
drains the GPU.

Next:

1. Cut the worker's replay time further (it is on the latency chain): resolve ~1.8 µs/draw outside
   the general draw, descriptors ~0.75 µs, uniform upload ~0.5 µs, `sync_linear`'s 4 MiB compare a
   frame.
2. Find which sync the game waits on (by call site) and whether it is frame N-1's end. If so,
   submitting just that frame's final batch earlier would help, without splitting every fence.
3. Section 5 (two workers) still applies.
