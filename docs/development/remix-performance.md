# Riverwood performance audit — 2026-09-17/18

> Historical benchmark, not current acceptance. The September21 deployment has
> frame generation off and remains under investigation for post-travel slowdown.
> The figures below describe older binaries/settings and specific cameras; they
> do not establish current performance or completion of the overall Remix goal.

Branch `codex/remix-vulkan`. The goal requires **60 FPS at 1920x1080 output** in
Riverwood. Earlier notes measured 18–24 FPS at 1680x1050. This checkpoint moves
the benchmark to the required 1920x1080 and replaces guesswork about the cost
with per-phase measurements on the host, the runtime and the GPU.

**Historical result: 88-98 presented FPS at 1920x1080 output**, from 18.9 at the start — and
that starting figure was measured at the lower 1680x1050, so the real gain is
larger than the numbers alone show. Rendered frames are 44-48 a second, with one
interpolated frame between each pair; *Where it stands* at the end of this
document sets out both numbers and what moves each. Every number here is from
the running test process. Each change was checked against the fixed river and
town cameras and, for anything touching transforms or retention, against a move
away and back; several changes that looked like large wins were reverted for
rendering incorrectly and are recorded as such.

The sections below are in the order the work happened, so their intermediate
figures (39.7, then 51, then 44-48 rendered) are each true of the build at that
point rather than of the build as it stands.

## Benchmark definition

- Output 1920x1080 (`SkyrimPrefs.ini` `iSize W/H`; previous file backed up to
  `.research/deployment-backups/20260917-ini`). DLSS renders 1280x720 internally
  at the default profile.
- Fixed free camera at `(20480, -45670, 300)`, pitch 0.1, yaw 0.2, Riverwood,
  survival declined, native world suppressed, Remix scene on.
- `tools/remix/RunRiverwoodTest.ps1` launches, loads, parks the camera and
  reports frame rate with the host CPU phases. `tools/remix/SweepSettings.ps1`
  A/B tests runtime options against the same camera.
  `tools/remix/Capture.ps1` parks the camera and copies a screenshot out.

## Where the frame went, and where it goes now

| Stage | Start | Now | Thread |
| --- | ---: | ---: | --- |
| `RemixScene::Submit` gather | 6.9 ms | 0.0 ms (every 4th frame) | game |
| `RemixScene::Submit` capture | 12.5 ms | 5.8 ms | game |
| `RemixScene::Submit` submit | 8.1 ms | 1.0 ms | game |
| Retained replay | 8.1 ms of API calls | 2.2 ms | command stream |
| `prepareSceneData` | 16.5 ms | ~10 ms | command stream |
| GPU: scene stage | 7.5 ms | 0.2 ms | GPU |
| GPU: path tracing | 23.4 ms | 16.7 ms | GPU |
| GPU frame total | 33.5 ms | 20.3 ms | GPU |
| **Observed frame** | **52.8 ms (18.9 FPS)** | **25.2 ms (39.7 FPS)** | |

"Now" is the deployed configuration: the Medium graphics preset with the
features the goal requires restored (below). At Remix's auto-selected Ultra
preset the same build measures 32.2 FPS with a 28.0 ms GPU frame.

At the start the frame was CPU-bound: `rtx.enableSecondaryBounces=False` dropped
the GPU frame to 20.0 ms yet only lifted the frame rate from 21.2 to 23.8 FPS,
the UltraPerformance DLSS profile (640x360 internal) also only reached 25.6 FPS,
and the runtime's `GPUIDLEms` counter read ~20 ms. It is still CPU-bound, but the
floor has moved from ~42 ms to ~22 ms.

**The vanilla game's own work is not the bottleneck.** With the Remix scene
switched off but the game otherwise running, Riverwood renders at **129 FPS**
(7.7 ms/frame), and with vanilla rendering fully restored, 132 FPS. There is no
large win waiting in further game-side suppression of work the game does itself.

Note though that the game thread's frame grows from that 7.7 ms to about 18 ms
once the Remix scene is active, and only ~7 ms of that is the host's own submit
— so roughly 10 ms of it is the game thread waiting inside its own D3D11/DXGI
calls. See *Next*.

## Implemented

### Host: retained instances

New private runtime entry points `csRemixCreateRetainedInstance`,
`csRemixUpdateRetainedInstance`, `csRemixUpdateRetainedInstanceTransform`,
`csRemixDestroyRetainedInstance` and `csRemixDrawRetainedInstances`. The host
registers an instance once and afterwards reports only genuine changes; one call
per frame replays the table. `RemixScene::Submit` submit fell from 8.07 ms to
1.0 ms, describing 422 instances per frame instead of 8,355.

Retained transforms are **absolute world transforms**. Skyrim rebases everything
against `posAdjust`, which tracks the camera, so camera-relative transforms would
have changed every instance every frame and defeated retention entirely. The
runtime applies the camera-origin rebasing itself from the origin passed to
`csRemixDrawRetainedInstances`, keeping Remix's internal world small (NRC bounds,
volumetrics and float precision unaffected).

### Runtime: an unchanged retained instance is refreshed, not re-derived

This removed the CPU wall, and took two failed attempts to find the real
dependency. Skipping `processDrawCallState` for an unchanged instance cut the
replay from 22.4 ms to 1.8 ms — and rendered the scene black, with surfaces
showing only their constant albedo and foliage alpha tests failing into holes.
Skipping only the geometry re-registration had the same symptoms.

**The cause is `SceneManager::m_bufferCache`.** Unlike the texture, sampler and
surface-material caches, which are content-addressed and persistent, the geometry
buffer table is a `BufferRefTable`: `track()` appends to a vector and only dedupes
against the immediately preceding entry, and `onFrameEnd` calls `clear()`.
Geometry bindless indices are therefore **assigned per frame, in submission
order**. An instance that is not resubmitted keeps last frame's
`positionBufferIndex` / `normalBufferIndex` / `texcoordBufferIndex`, which now
address whatever occupies those slots — hence torn geometry, garbage UVs and
zero normals.

A refreshed instance therefore needs exactly this per frame, and nothing else
`updateInstance` writes:

1. `updateBufferCache(blas->modifiedGeometryData)` — re-register the buffers in
   this frame's table.
2. `InstanceManager::processInstanceBuffers` — copy the indices into the surface.
3. `RtxTextureManager::markTextureUsed` for the surface material's textures —
   residency follows `addTexture`, so an unrefreshed texture is demoted to zero
   mips and its surface falls back to constant albedo. Deduped per material per
   frame.
4. Liveness (`setFrameLastUpdated`, camera registration, `frameLastTouched`) and
   the rebased transform via `move()`, which preserves motion vectors.

Replay 21.6 ms → **3.1 ms**, 29 full submits per frame instead of 7,962 (about
300 while the camera moves), `[ProcDCS]` 16 ms → ~0, frame rate 22.0 → 29.0 FPS.

### Runtime: a retained scene stops rebuilding acceleration structures

`frame.scene` was 7.5 ms of GPU with no BLAS rebuilds reported. A GPU sub-split
(new `RtxContext::markGpuStage`, so managers outside `RtxContext` can divide
their own block of the `[CSRemix.GPU]` report) put 6.5 ms of it in
`mergeInstancesIntoBlas`, and a build census showed why: **3,568,804 triangles
were being rebuilt into six merged BLAS buckets every frame.**

Remix routes a mesh into a merged bucket when it is single-instance and has
fewer than `minPrimsInDynamicBLAS` (default 1000) triangles. That threshold
exists because merged geometry is rebuilt every frame — which is the right trade
for a capture-based renderer and the wrong one for a retained scene. Host-retained
instances are now excluded from both the merged-bucket routing and that
threshold, exactly as grass and point-instancer geometry already were.

All 8,356 instances now hold their own reused BLAS, no buckets remain, and no
BLAS is built or refit in a steady-state frame. `frame.scene` GPU: 7.5 ms →
**0.22 ms**.

That initially cost more CPU than it saved, because the unique-BLAS loop asks
`vkGetAccelerationStructureBuildSizesKHR` for every structure every frame just to
confirm nothing needs rebuilding: 1.8 ms → 10.2 ms. The answer depends only on
the build description, so it is now cached on the `BlasEntry` and keyed on the
flags, geometry count, primitive count, vertex count and bound micromap hash.
Back down to 4.7 ms.

### Host: scene discovery on a cadence, capture in scene order

Walking every attached cell, reference and node finds objects *entering and
leaving* the scene, which happens when cells stream, not when objects move. It
now runs every 4th frame; what an object is *doing* is still detected every frame
by probing the retained set. Retention holds `NiPointer` references, so an object
the game unloads stays valid until the next walk notices it. Capture also
iterates a contiguous list built at discovery time rather than the retention map,
which matters because the probe is memory-bound.

An object that has then been still for a second is itself probed on a stagger, so
each frame reads a quarter of the settled set rather than all of it: reading the
probe is the cost, since every input lives in a different game object and is a
cache miss. An object that starts moving is noticed within four frames. The
stagger is a hashed pointer, not the low bits — these objects are cache-line
aligned, so taking the low bits directly put every one of them in the same slot
and the mechanism silently did nothing.

Gather 2.6 ms → 0.0 ms on the 3 frames in 4 that skip it; capture 12.5 → 5.8 ms,
with 4,225 of 5,634 settled objects staggered out of any given frame.

### Host: the retained scene stops re-deriving unchanged geometry

- `RemixSceneGraph::Gather` no longer takes a `NiPointer` reference on every node
  and geometry (tens of thousands of atomic increments per frame), and its
  working buffers persist. Node membership uses an open-addressed pointer set.
- `RemixScene::Capture` caches each geometry's `netimmerse_cast` classification
  (the object's own type never changes; property-derived kinds are revalidated by
  property pointer) and the merged visible LOD segment ranges, rebuilt only when
  a hash of the native segment table changes.
- A cheap identity probe short-circuits plain opaque static shapes whose
  per-frame inputs are unchanged: 5,635 of 10,353 geometries take it.

Remaining full captures are categories that genuinely animate: effects 1,376,
skinned 688, instanced batches 394, distant trees 372.

### Runtime: diagnostics removed from the hot path

The fork ran several per-instance debugging probes in shipped frames: the TLAS
census (two `steady_clock` reads plus a map insert per instance, 1.56 ms/frame),
the `addBlas` first-sight probe (a mutex and a hash insert per instance), the
surface-push tally and the `[BulkPush]` dump. All now follow
`rtx.logSurfaceCoverage`. The stable instance sort is skipped when the table is
already ordered.

### Runtime: trace-optimised acceleration structures for settled geometry

Remix builds every BLAS with `PREFER_FAST_BUILD`, the right trade when geometry
is rebuilt every frame. A retained scene builds once and traces for thousands of
frames, so a BLAS untouched for 30 frames is promoted, once, to a
`PREFER_FAST_TRACE` build, budgeted at 8 promotions per frame.

**This did not measurably change path-tracing time** (23.4 ms before, 23.2 ms
after). BVH quality was not the bottleneck; the change is kept because it is
correct for a retained scene, not because it was the fix.

### Runtime: the host can choose a DLSS profile

`rtx.qualityDLSS` cannot be set by an embedding host. The graphics and DLSS
presets write it into a **derived** option layer at startup, which outranks the
user layer the config API writes to, so every value sent through
`SetConfigVariable` was silently ignored — which is why earlier sweeps of DLSS
quality, bounce counts and upscaler type all appeared to do nothing. This affects
*any* preset-owned option, so past A/B results for such options are not evidence
of anything.

New option `rtx.qualityDLSSOverride` (default `Invalid`) is written by nothing
else and selects the profile when set. Measured after the work above:

| Profile | Internal | GPU frame | Path tracing | FPS |
| --- | --- | ---: | ---: | ---: |
| MaxQuality / default | 1280x720 | 28.3 ms | 23.9 ms | 28.4 |
| Balanced (2) | 1114x626 | 23.5 ms | 19.5 ms | 29.9 |
| MaxPerf (1) | 960x540 | 19.2 ms | 15.7 ms | 31.5 |

### Runtime: the graphics preset is reachable, and keeps the look

`rtx.graphicsPreset` drives bounce counts, denoiser separation, neural-cache
quality and volumetrics, and Remix applies it once during initialization; its
onChange callback is suppressed inside this DLL, so setting the option did
nothing. New export `csRemixApplyGraphicsPreset`, reachable as the DevBench
setting `cs.graphicsPreset`, applies it at runtime.

Remix auto-selects **Ultra** on this GPU. Its lower presets also switch off three
things the goal requires regardless of how much path-tracing quality is traded:
Remix-side post-processing, stochastic alpha blending, and alpha resolve in
indirect rays. `updateGraphicsPresets` now restores those after applying any
preset, which is a no-op for Ultra and High.

| Preset | GPU frame | Path tracing | FPS |
| --- | ---: | ---: | ---: |
| Ultra (auto-selected) | 26.6 ms | 21.8 ms | 32.9 |
| High | 24.2 ms | 20.4 ms | 33.9 |
| **Medium (deployed)** | **19.0 ms** | **15.6 ms** | **36.8** |

Matched captures at the town and river cameras show no difference this scene
reveals: same lighting, shadows, water, foliage and exposure
(`004352-preset-ultra.png` versus `004406-preset-medium.png`, and
`004837-medium-river.png` versus the Ultra `235001-final-river.png`). Medium is
therefore the configured default in `ConfigureRunningTest.ps1`.

Stacking the DLSS profile on top: Balanced 38.6 FPS, MaxPerf 40.6, UltraPerf
45.3 — at which point the GPU is 10.8 ms and the frame is entirely CPU-bound.

## Deployed state

Verified visually at the fixed river and town cameras against their pre-change
captures, and after moving away and back (`.research/captures`). Only the three
owned DLLs were overwritten, after backups to
`.research/deployment-backups/20260917-*` and `20260918-*`; matching symbols are
in `.research/deployed-symbols/`. No files were deleted. The runtime worktree
changes are exported to `tools/remix/runtime.patch`, which passes a reverse-apply
check against the worktree.

- CS `1F138D778C948105F14BBB1C8DC7C337523544DFBBD9F22568C884FF08AB1839`
- d3d11 `1C7747395E245C3A88CBA45C2AF5A71260AB61CA49432AB35A5BEED674EA2E35`
- dxgi `145C734B84B249E59A725B597F0DE7D16F2CF0B588BCFFE0AB2D41AA0248E040`

### Runtime: retained instances stop re-keying the spatial map

`RtInstance::move` recomputes the normal basis (a 3x3 inverse) and re-keys the
BLAS spatial map on every call. For a retained instance neither is needed: the
camera rebasing only moves the translation, and the spatial map exists so a draw
can be matched to an existing instance by position — which a retained draw never
needs, because it names its instance outright (`submitExternalDraw` now passes
it through to `processDrawCallState`). A new `RtInstance::rebase` does the
minimum. Replay 3.1 → **2.2 ms**.

### Runtime: a settled acceleration structure skips its build description

A structure that already exists and is not being refit needs none of the build
description the loop derives for it — only its instances still have to reach the
TLAS. The one input that can change without the geometry changing is the opacity
micromap binding, when a bake finishes, so that is revalidated on a stagger
(1 structure in 8 per frame) rather than every frame. 7,305 of 8,355 structures
take the short path; `dynBlas` 3.7 → 3.4 ms. The remainder is `addBlas` itself —
assembling a surface and a TLAS instance per instance — which is not avoidable
without reducing the instance count.

## Chasing the game thread's missing time

The frame is **CPU-bound at about 22 ms**. Medium plus the MaxPerf DLSS profile
puts the GPU at 15.9 ms — inside the 16.6 ms budget — and the frame rate still
only reaches 41.7 FPS; UltraPerformance takes the GPU to 10.8 ms and it still
only reaches 45.3 FPS.

The two host threads do not overlap as much as they could. The game thread
carries ~15 ms (7.7 ms vanilla game, 6.2 ms capture, 1.0 ms submit, amortized
gather) and the command stream ~15 ms (12 ms `injectRTX`, 3 ms replay), yet the
frame is 25 ms rather than the ~15 ms full overlap would give.

Two candidate explanations were tested and **both are ruled out**:

- *The game thread blocking on the command stream.* DXVK counts exactly this
  (`CsSyncCount`/`CsSyncTicks`), now reported per frame as `[Perf.CsSync]`. It
  reads **zero waits per frame**. `dispatchChunk` never blocks either — the
  chunk queue is unbounded. The handover call itself (`csRemixRender`) measures
  0.014 ms.
- *Frame queue depth.* `dxgi.maxFrameLatency = 3` and three back buffers changed
  nothing (39.2 versus 39.7 FPS).

What is left is that the game thread's own frame grows from 7.7 ms standalone to
~18 ms with the Remix scene active, without blocking on the command stream — so
it is waiting inside its own D3D11/DXGI calls. The next section measures where.

Independently, the total work has to come down by roughly a third:

1. **The vanilla game's 7.7 ms.** Suppression currently removes its GPU
   submission but not its CPU-side render preparation. The goal directs this
   work through game-code hooks, and the shadow-cascade and culling preparation
   is the obvious candidate now that scene capture no longer depends on any
   render pass.
2. **Capture, 5.8 ms**, dominated by 1,375 effect instances taking the full path
   every frame for what is usually only a UV transform change. Distant trees
   (372) and landscape (173) could also become probe-eligible.
3. **`prepareSceneData`, ~10 ms**, now mostly per-instance surface and TLAS
   assembly plus the surface upload. Merging small retained meshes into buckets
   built *once* — baking absolute transforms and rebasing on the bucket's TLAS
   instance, the same trick the retained instances use — would shrink this, the
   TLAS, and the 1.6 ms of path-tracing traversal that giving every mesh its own
   structure cost.

The visual requirements tracked in the other notes are unchanged by this work.
## The game thread was waiting on the GPU, one frame at a time

The unexplained gap above is now measured. `D3D11SwapChain::PresentImage` is
split per call and reported as `[Perf.Present]`, and of the 12 ms the game
thread spent inside `Present`, **11.7 ms was `SyncFrameLatency`** — everything
else (the `injectRTX` hand-off, the flush, the swapchain acquire, the present
submission) was under 0.3 ms combined.

`SyncFrameLatency` waits for the GPU to retire the frame `maxFrameLatency`
frames ago. Skyrim asks D3D11 for a latency of **1**, which the runtime now
logs:

```
[Perf.Present] frame latency now 1 (requested=1 cap=0 buffers=3 override=0)
```

With a latency of one the game thread cannot start building frame N until the
GPU has finished frame N-1, so the three stages that build a frame here — the
game thread, DXVK's command-stream thread, and the GPU — run in series instead
of overlapping. The command-stream thread measured **8 ms idle per 24.8 ms
frame** waiting for work that the game thread was not free to hand it.

`dxgi.maxFrameLatency` cannot fix this, which is why the earlier experiment with
it did nothing: it is a *cap*, applied with `std::min`, and the game had already
asked for less than any cap would impose. The runtime gained
`dxgi.frameLatencyOverride` (env `DXVK_FRAME_LATENCY_OVERRIDE`), which replaces
the negotiated value outright, and `DxvkLoader` sets it to 2 unless the
environment already names a value.

| frame latency | FPS | command stream idle |
| --- | --- | --- |
| 1 (what the game asks for) | 35.4 | 8.0 ms/frame |
| 2 | 45.6 | under 0.1 ms/frame |
| 3 | same as 2 within noise | — |

(Measured with the corrected benchmark below. The first readings of this change
were 39.7 against 48.6 on the older, looser protocol.)

Two is enough to fill the command-stream thread; three measured the same and
only adds input latency.

### The benchmark was measuring a paused world, and a drifting one

Dismissing the "Survival Mode" prompt is what makes the loaded save playable,
and while a message box is up Skyrim pauses: no animation, no skinning, no
acceleration-structure refits. The harness accepted only one box, before the
prompt appears, so every number above this section was measured with the world
paused — consistently, so the comparisons hold, but optimistically.
`RunRiverwoodTest.ps1` now dismisses boxes until none is left (declining rather
than accepting: "Yes" on that prompt changes the save's rules) and re-checks
after the camera settles.

That exposed a second problem. The save loads at whatever hour and weather it
was saved in and then keeps running, so the sun angle and cloud cover drift
while the camera sits still. Successive identical runs varied by +/- 2 FPS,
which is larger than most of the individual changes below -- three single-run
A/B comparisons in this session read backwards because of it, including one that
briefly looked like a regression. The harness now stops the clock, pins noon,
and forces one weather before measuring; the spread across three runs is then
about 0.7 FPS. Noon is a harder frame than the save's own time, so numbers on
this protocol are lower than the earlier ones and are not comparable with them.
Everything below is on the corrected protocol; a change worth less than ~1 FPS
is reported as within noise rather than given a number it cannot support.

## The command stream is now the limiter

With the queue deepened, `[Perf.CsSync]` reports the command-stream thread
**busy 20.5 of every 21 ms** — it is the critical path, and the game thread's
`SyncFrameLatency` wait has fallen to under 0.05 ms. Its two large costs:

| | ms |
| --- | --- |
| `prepareSceneData` (`injectRTX`) | 10-13 |
| &nbsp;&nbsp;`mergeInstancesIntoBlas` | 7-9 |
| &nbsp;&nbsp;garbage collection | 1.0 |
| &nbsp;&nbsp;surface + material upload | 1.1 |
| &nbsp;&nbsp;point-instancer culling | 0.8 |
| &nbsp;&nbsp;bindless tables | 0.6 |
| retained replay (outside `injectRTX`) | 6-7 |
| &nbsp;&nbsp;7,550 refreshes | 2.5 |
| &nbsp;&nbsp;~410 full re-submissions | 3.5 |

The replay's split is new: `[Perf.Retained]` now separates the two populations
and records why each full re-submission happened. Essentially all of them are
instances the host re-described this frame (~380/frame out of 8,360 — particles
and animating characters), not instances the runtime failed to refresh.

### What the measurement itself got wrong

Three of the per-instance sub-timers first added here read zero, which nearly
sent this in the wrong direction twice:

- `duration_cast<microseconds>` on a sub-microsecond interval truncates to 0, so
  8,000 samples of 0.4 µs summed to nothing. The per-instance accumulators are
  nanoseconds now.
- Keying the opacity-micromap eligibility cache on `RtInstance::frameLastUpdated`
  cached nothing, because the retained path stamps that field on every instance
  every frame as a keep-alive. It is an explicit validity flag now, cleared by
  `onInstanceUpdated` and `onInstanceAdded` — the paths that actually rewrite
  the material, geometry and alpha state the decision reads.

### Changes measured after the frame-queue fix

| change | what it removes, measured in the phase timers |
| --- | --- |
| Drop the build-read barrier and buffer tracking for structures not being built | 2.2 ms of `dynBlas` |
| Hoist the five `RtxOption` reads out of the per-instance routing loop and its helpers | ~58,000 global-mutex acquisitions per frame |
| Flat per-frame BLAS grouping (`std::vector<BlasEntry*>` + the instance list on the entry) instead of `unordered_map<BlasEntry*, vector<RtInstance*>>` | the map's clear-walk, 8,350 hash inserts and a node-chasing walk, per frame |
| Cache the BLAS build description on the entry, keyed on its geometry's last update plus the filling instance's flags | 0.6 ms of the routing loop |
| Cache opacity-micromap eligibility per instance | 0.4 ms of the routing loop |

The phase timers are the evidence for each of these; only the barrier/tracking
change is individually larger than the benchmark's noise floor. Together with
the frame-queue fix they take the frame from 28.2 ms to 21.9 ms.

A structure that is not being rebuilt this frame reads none of its source
vertex or index buffers: no build consumes them, and reaching that path means
nothing wrote them this frame either, so there is no hazard to declare. They are
still read by the path tracer, and `BindlessResourceManager::prepareSceneData`
already tracks every buffer in the bindless table for the frame on that account.
Declaring it a second time per structure cost more than everything else in that
loop combined.


## Accounting for the command-stream thread completely

Everything the host submits through the Remix API is deferred onto the
command-stream thread, so that thread's time is not `injectRTX` plus a
remainder. `RemixAPIPrivateAccessor::EmitCs` now times each deferred command and
`[Perf.CsSync]` reports the total, under `CS_REMIX_API_TIMING=1` (it has its own
switch because the wrapper costs about 0.8 ms of the frame it measures, which
the frame-rate runs must not pay):

```
csBusyMs=21.13 csIdleMs=0.25 apiMs=20.92 apiCalls=2240
```

The thread is entirely inside host commands, and they account for:

| | ms |
| --- | --- |
| `injectRTX` (`prepareSceneData` ~8.5 of it) | 10.9 |
| retained replay | 6.4 |
| storing re-described instances | 0.2 |
| **everything else, across ~2,240 calls** | **3.4** |

That last row is the 394 instanced batches -- grass, foliage and distant trees --
which the host draws with `csRemixDrawInstanceSet` / `csRemixDrawGrassInstanceSet`
every frame. Each one runs the whole `submitExternalDraw` path, at roughly 8 µs,
the same cost a re-described single instance pays. Single instances stopped
paying it when they became retained; instanced batches never did.

## More per-frame work that a retained scene does not need

| change | what it removes, from the phase timers |
| --- | --- |
| Opacity-micromap build requests short-circuit when the instance has nothing pending and no cache entry has been evicted since | `regOmm` 1.2 → 0.39 ms |
| The geometry-entry reaper runs on an eight-frame cadence instead of every frame (it only decides whether an entry has gone untouched for longer than that; the instance, structure and light reapers still run every frame) | `gc` 1.0 → 0.4 ms |
| An object that only moved is updated through `csRemixUpdateRetainedInstanceTransform` instead of being re-described | ~100 of ~390 full re-submissions per frame; `fullUs` 3.0 → 2.6 ms |
| The surface-material staging vector is retained instead of allocated and zeroed each frame | `surfMat` 1.0 → 0.81 ms |
| Micromap revalidation interval 8 → 32 frames, and gated on the micromap cache having changed at all | `dynBlas` 2.5 → 2.1 ms; 7,100 → 7,820 of 8,350 structures skip the build description |
| The staging ring is 32 MB instead of 4 MB | see below |
| Instanced batches that are not grass go through the retained table instead of being re-drawn every frame | 394 per-frame batch draws → 22 (grass only) |
| The three invariant bindings and the shader in the point-instancer culling loop are bound once instead of per batch | `piCull` 0.72-4.6 → 0.63-0.66 ms, and it stopped varying |

### Two of those needed a second attempt

Keying the micromap short-circuit on the *cache* generation -- which counts every
change to the bindable set -- cached nothing, because while anything at all is
baking that counter moves every frame. It is keyed on an eviction-only counter
now: an instance whose requests have all completed can only acquire a pending one
again if its own inputs change, which invalidates its record directly, or if its
cache entry goes away. Applying the same (wrong) reasoning to the per-structure
micromap *binding* was worse -- it put all 8,350 structures through the full
build-description path and took `dynBlas` from 2.5 ms to 6 ms. That one keeps a
stagger, widened to 32 frames: binding a freshly baked micromap late costs its
speedup for a fraction of a second, never correctness, because without one the
any-hit shader does the alpha test itself.

### The staging ring, and a stall that moved around

`piCull` was 0.8 ms in most runs and 7 ms in others, and the phase it landed on
moved: an earlier run had the same 4 ms sitting on the TLAS build instead. It is
not that phase's work. DXVK's staging ring is recreated whole -- a fresh
host-visible allocation -- whenever a frame's uploads run past its end, and this
scene uploads a surface table, a surface-material table and an instance buffer
every frame, about three megabytes against a four-megabyte ring. The allocation
landed on whichever phase asked for staging next. At 32 MB the ring is crossed
roughly every tenth frame and `piCull` reads 0.72-0.84 ms consistently.

This is worth remembering when reading any of the phase numbers above: a
multi-millisecond reading that appears and disappears between runs is more likely
to be this than the phase it is attributed to.


## The bindless buffer table stops being a per-frame thing

The table the path tracer reads geometry through was a tape: cleared every
frame, appended to in submission order, so every geometry in the scene had to
re-register its buffers and every instance had to re-copy the resulting indices
into its surface, whether or not anything had changed. That is what made the
retained replay's refresh cost what it did, and it is why the surface data could
never be treated as stable.

It is now a slot table whose indices belong to the geometry that took them, for
as long as that geometry lives. A geometry takes its slots when it is registered
or its buffers change and gives them back when it is destroyed; in between, the
index is a fact about the geometry rather than about this frame. Both calls came
out of the replay's refresh path entirely.

| | before | after |
| --- | --- | --- |
| retained replay | 6.8-7.1 ms | **4.7 ms** |
| — refresh over ~8,000 instances | 3.3-3.8 ms | **1.2-1.4 ms** |
| — of that, re-registering buffers | 1.3-1.5 ms | **0.22-0.32 ms** |
| — of that, copying indices into surfaces | 1.5-1.8 ms | **0.52-0.62 ms** |
| command-stream thread | 19-21 ms busy, 0 idle | **15.6-16.5 ms busy, 3.8-4.6 ms idle** |

The command-stream thread is no longer the limiter.

### Two ways it went wrong first, both fatal

The first version lost the device. Its table grew without bound -- 11k slots,
then 25k, then 38k over a few hundred frames -- and once it outgrew the bindless
descriptor array the writes ran off the end. Two separate causes:

- **Nine slots per geometry instead of two.** A geometry's attribute buffers are
  usually slices of one interleaved buffer. The tape deduplicated them for free
  by comparing each registration against its last entry; the slot table has to do
  it explicitly, and without it every geometry took a slot per buffer.
- **No way to notice a leak.** Slot ownership is recorded in the geometry's own
  index fields, and those travel through several paths that rebuild the geometry
  struct. A path that loses the old indices before reassigning leaks the slots
  they named, silently. `reconcileBufferCache` now sweeps on the same cadence as
  the geometry reaper: the draw-call cache is the complete set of geometry that
  can own a slot, so walking it gives an exact answer rather than a heuristic,
  and anything unclaimed goes back. With the deduplication in place it reclaims
  nothing in a steady scene, which is the point -- it is there so that a leak
  cannot reach the descriptor array.

There is also a sentinel hazard worth naming: the surface format spells "this
geometry has no such buffer" as index 0xFFFF, which is a perfectly ordinary slot
number. The table is told to reserve it.


## Chasing the GPU: what moves it and what does not

With the frame GPU-bound at 17.3-17.9 ms, the question became which part of that
is reducible without changing the look. Four things were measured.

**Merging static meshes into shared structures.** The hypothesis was that a
top-level structure with 8,350 instances in it is expensive to traverse, and
that merging the static ones would pay for itself. `rtx.mergeRetainedInstances`
lets host-retained meshes join the merged buckets so this could be measured; the
routing decision is taken once per geometry and then pinned, so it has to be set
before the cell loads (`tools/remix/MergeSweep.ps1` does a cold launch per case).

It collapses 8,350 top-level instances into **478**, and the frame gets *worse*:
19.8 → 25.3 ms, with the game thread's wait on the GPU going from 5.0-5.9 ms to
11.4-11.7 ms. The merged buckets are rebuilt every frame, and in this scene that
is 3.57 million primitives of acceleration-structure build per frame against the
430 thousand the retained path refits. The experiment cannot separate the
traversal saving from the rebuild cost, because the rebuild is far larger: net
+5.5 ms for a build that is plausibly +9 ms implies the traversal saving is
around 3 ms, which is the right order for what is missing -- but it only arrives
if the bucket is built *once*, which means a retained bucket: absolute
transforms in the per-geometry transform buffer (the merged path already builds
that way, so no vertices have to be rewritten) and the camera-origin rebase
moved onto the bucket's own top-level instance, where the retained instances
already put it. That is the remaining structural item.

**Trace-optimised acceleration structures.** The promotion that rebuilds a
settled structure with `PREFER_FAST_TRACE` instead of the `PREFER_FAST_BUILD` it
was created with had been silently dead since the fast-reuse path went in: a
structure only reaches the promotion code on the full build-description path,
and the fast-reuse early-out skipped exactly the structures that qualified.
`bTracePromote` read 0 in every sample. It is fixed -- a structure that is due
promotion now takes the full path, still budgeted to eight per frame -- and
promotions do happen (the whole cell is converted over about a thousand frames).
It makes no measurable difference here: 49.0-51.3 FPS against 49.8-51.4 before,
indirect integration 10.2-10.3 ms against 9.9. Kept anyway, because a structure
that is traced for thousands of frames should have the BVH built for tracing,
but nobody should read it as a win.

**Path length and the choice of indirect integrator.** Neither matters.
`pathMaxBounces` 4 → 3 moves indirect integration 10.3 → 9.7 ms and the frame
rate 51.9 → 52.2. A clean same-process A/B of the integrator reads 51.6-52.5 FPS
for the neural radiance cache against 51.7-52.0 for ReSTIR GI. DLSS ray
reconstruction changes nothing (51.59 against 51.60).

**Resolution, and one lighting feature.** Internal resolution is the only strong
lever: at the profile the Medium preset picks (Auto, resolving to 1114x626 for a
1920x1080 target) the GPU is 17.9 ms; at MaxPerf (960x540) it is 16.2 ms and
56.3 FPS; at UltraPerformance (640x360) it is 11.7 ms and 68.5 FPS. Turning
RTXDI off is worth 2.8 FPS. Both trade exactly what the goal asks to keep -- the
first is a visible softening at 1080p output, the second makes the game's own
light sources resample far more noisily -- so neither is taken.

That is the honest position on the GPU: at this quality its floor is about
17.4 ms, a 16.67 ms frame needs it near 16, and the only measured route there
that does not trade the look is the retained merged bucket above.

## Building the retained merged bucket

The remaining item from the section above, implemented behind
`rtx.mergeRetainedInstances` (off by default). A retained bucket takes settled
host-retained meshes, bakes their **absolute** placements into the per-geometry
transform buffer the merged path already uses -- so no vertices are rewritten --
and carries the camera-origin rebasing on the bucket's own top-level instance
rather than baking it into the geometry, where it would be wrong the moment the
camera moved. A structure built that way describes the same thing every frame,
so it is built once and looked up afterwards by a signature over its membership,
their placements and the last frame each one's geometry changed.

It runs without corruption and does what it says. The first version assigned
instances to whichever compatible bucket was open when they were reached, so
membership depended on iteration order and a third of the buckets rebuilt every
frame (44.1 FPS). Keying assignment on the instance instead fixed that; what
that revealed is below.

### Four lost devices, and what each taught

Every one of these is a way to end up with a top-level structure pointing at a
bottom-level structure that no longer holds what it was built to hold. The GPU
does not report that; it faults, and DXVK reports `VK_ERROR_DEVICE_LOST`.

- **The pool can hand your structure to someone else.** Taking the bucket's
  structure from the shared pool let a later frame pick the same one for a
  different bucket and rebuild it, while the signature map still named it for
  the old bucket. Retained structures now have their own pool, and taking one
  over retires whatever signature named it.
- **Structures cannot be reused while a previous frame's top-level structure is
  still in flight.** The retained pool's first version reused anything not
  touched *this* frame. The shared pool pads that by one or two frames precisely
  because the previous frame's structure is kept for temporal reprojection. Same
  padding now.
- **A structure built once keeps whatever opacity micromap was bound at build
  time referenced for as long as it lives.** The per-mesh path notices a
  micromap going away and rebuilds; a retained bucket has no such moment, so a
  micromap evicted underneath it leaves the structure pointing at freed memory.
  Alpha-tested geometry is excluded from retained buckets.
- **Unbounded allocation looks like a driver fault too.** A version that
  allocated a fresh structure per changed signature exhausted video memory
  within seconds while the cell streamed in, because membership churns on every
  frame during streaming.

The signature also had to stop being bit-exact on the placement: the absolute
transform is reconstructed by adding the scene origin back to a transform the
origin was subtracted from, so its low bits move as the camera does even when
the object has not. It is quantised to a thousandth of a game unit.

### The traversal hypothesis was wrong

Making bucket assignment deterministic fixed the churn: keyed on what makes two
instances shareable plus the cell of the world they stand in, an instance lands
in the same bucket every frame regardless of the order the loop reaches it in.
Reuse went from 49 of 82 buckets to **1,080 of 1,106**, with 26 rebuilt per
frame instead of a third of them.

With that working, the top-level structure holds **4,479 instances instead of
8,347** — and indirect integration does not move: 9.57 ms against the 9.6-9.9 ms
it measures without any of this. The frame rate does not move either (48.6-49.7
against 50.6 with the path off; the bucket machinery costs about a millisecond
of scene-build time, which is why it is slightly behind).

So the reasoning in the section above -- that a top-level structure with 8,350
instances in it is what the 10 ms of indirect integration is spent on -- is
wrong, and the earlier "+5.5 ms net for a build that is plausibly +9 ms implies
a traversal saving of about 3 ms" was an inference from two numbers that could
not be separated, not a measurement. Halving the instance count directly, with
the rebuild cost removed, buys nothing.

What indirect integration does scale with is pixels: 10.3 ms at 1114x626, 6.0 ms
at 640x360. It is ray work, not structure.

The path is left in, off by default, because it is correct, the measurement is
worth keeping, and it may matter in a scene with a different shape. It is not
the route to a 16.67 ms frame.


## Frame generation was configured on and was not reaching the display

The goal asks for 60 FPS *at 1080p output*. Frame generation raises the
presented frame rate without changing what is rendered, and the runtime reported
it as running:

```
[Upscaler.dlfg] supported=1 optionEnabled=1 failed=0 active=1
                interpolatedFrames=1 maxSupported=1 reason=
```

It was not. Counting presents against rendered frames gave **exactly one present
per rendered frame**:

```
[Perf.CsSync] ... presentsPerFrame=1 presentedFps=51.06
```

The reason was structural. Interpolation is done by `DxvkDLFGPresenter`, a
`vk::Presenter` subclass that takes the rendered frame, produces an interpolated
one between it and the last, and presents both. Upstream Remix installs it from
its **D3D9** swap chain. This fork presents through D3D11, and
`D3D11SwapChain::CreatePresenter` constructed a plain `vk::Presenter` — nothing
in the tree ever constructed `DxvkDLFGPresenter`. Everything downstream behaved
as though frame generation were on: the option enabled, NGX reporting support,
`isDLFGEnabled()` true, the interpolated frame count one, and `dispatchDLFG()`
faithfully handing the submission queue a `DxvkFrameInterpolationInfo` built from
`m_primaryScreenSpaceMotionVector` and `m_primaryDepth` every frame — which the
plain presenter's `presentImage` ignores.

### Where it has to be installed

Not from `CreatePresenter`: that runs while the swap chain is being constructed,
before the RTX common objects that own the NGX context exist, and touching them
that early lost the device.

Not from the start of `PresentImage` either — an attempt there produced no log
line at all, neither success nor failure nor the retry's give-up message, for a
reason never established.

It works from the present loop, immediately after `SynchronizePresent()`: the
previous present has been synchronised and this frame has not emitted its own
yet, so no present is in flight and the presenter can be replaced whole.
Everything below that point in the loop — `info()`, `acquireNextImage`, the blit,
`SubmitPresent` — then picks up the new one on the same frame. It is asked for
the first 600 presents and then left alone, because the game presents for several
seconds while it is still starting and the answer is not stable until the RTX
objects exist.

### Four things had to be fixed before it worked

Installing it was the smallest part. Three defects in code that had never been
exercised, and one in this fork's present path, each lost the device or hung the
game.

**The constructor never sized itself.** `DxvkDLFGPresenter`'s constructor carries
a comment saying that `vk::Presenter`'s constructor creates the swap chain
through the *base* class's `recreateSwapChain` rather than the override, and that
the backbuffers therefore have to be created explicitly — and then calls
`createBackbuffers()` without setting `m_appRequestedImageCount`, which only the
override assigns. Every backbuffer array was sized to zero, `info()` reported
zero images, and `acquireNextImage`'s first statement is a modulus by that count.
The real swap chain was also left at the application's image count instead of the
interpolation-addressable one, which `swapchainAcquire` asserts against. Running
the override once from the constructor's body does both — and dispatch reaches
the override there, because the object is already of its final type by the time
the body runs.

**Presentation was on the wrong queue.** The installer built its
`vk::PresenterDevice` the way `CreatePresenter` does, from the graphics queue.
The interpolating presenter presents from *its own thread*, so that had that
thread calling `vkQueuePresentKHR` on the same `VkQueue` the command-stream
thread submits to, with nothing synchronising them. It survived 125 frames of the
main menu and lost the device within half a second of the path tracer submitting
real work. The queue it wants is the one the device already marks to Reflex as
the out-of-band present queue and that DLFG's own command lists use —
`queues().present`, a compute family. `__DLFG_USE_GRAPHICS_QUEUE` being 0 is
exactly this expectation; the `#if` around `presenter->synchronize()` in the
submission queue is the other half of it.

**`VK_EVENT_SET` is not a failure.** The interpolating presenter's `presentImage`
does not present — it queues the frame for its own thread and returns
`VK_EVENT_SET`, and the submission queue deliberately leaves that in the status
so the DLFG thread can overwrite it with the real result later.
`SynchronizePresent` treated anything that was not `VK_SUCCESS` as a failed
present and rebuilt the swap chain, which meant rebuilding it on every single
present.

**The acquired image index was hardcoded.** `SubmitPresent` called
`m_device->presentImage(0, false, 0, ...)`. The ordinary presenter ignores that
argument and uses the index it recorded itself, so the zero never mattered. The
interpolating presenter cannot: it keeps several acquires in flight, and that
argument is what tells it which backbuffer holds the frame and which in-flight
slot to release afterwards. It is threaded through from the acquire now.

### What it does

Measured where the frames actually leave, one count per `vkQueuePresentKHR` on
the DLFG present thread — interpolated frames included, which is the only place
in the runtime that sees them:

```
[Perf.Dlfg] presented 480 frames in 5002 ms = 95.96 fps out
[Perf.Dlfg] presented 480 frames in 5006 ms = 95.88 fps out
[Perf.Dlfg] presented 482 frames in 5020 ms = 96.02 fps out
```

**88-98 FPS out at 1920x1080**, against 44-48 rendered. Rendered frames cost a
few FPS against the 51 before, which is the interpolation's own GPU time on the
compute queue; the output is a little under twice the rendered rate because the
ratio is exactly two and the rendered rate fell.

Nothing about what is rendered changed: same internal resolution, same DLSS
profile, same path-tracing settings, same look. The interpolated frame is derived
from two frames that are already correct.

## Where it stands

**88-98 FPS at 1920x1080 output** in Riverwood — 96.7, 97.6, 95.9, 96.0, 95.2,
94.6, 94.3, 93.2, 90.4, 88.7, 88.3 across five-second windows in three runs —
from 44-48 rendered frames a second with one interpolated frame between each
pair. The goal's bar is 60 at 1080p output; the worst window measured is 88.

The rendered rate got there in stages: **35.4 FPS** on the build as it was, with
the frame queue the game asks for; **51 FPS** (50.4, 50.5, 50.6, 51.0, 51.5,
51.6, 51.8 across runs) after the work in the sections above; **44.2, 45.5,
47.4, 47.8, 48.1** with interpolation also running, which pays a few rendered
frames a second for twice as many presented ones.

The benchmark also had to settle longer: shader and pipeline compilation,
micromap baking and the radiance cache's training all continue for the best part
of a minute after the camera stops, and measuring at twenty seconds read about
two frames a second low. The harness settles for forty-five now.

**The frame is GPU-bound.** The command-stream thread is idle 3.5-5.7 ms of every
frame and the game thread's own work (7.7 ms vanilla, 6.5-6.9 ms capture, 1.2 ms
submit) is inside budget; what it spends the rest of the frame on is
`SyncFrameLatency`, waiting for the GPU. The GPU measures 17.3-17.9 ms:

| | ms |
| --- | --- |
| path tracing | 14.2-14.5 |
| — integrate indirect | 9.6-10.3 |
| — g-buffer | 1.8-2.3 |
| — RTXDI | 1.1 |
| scene (acceleration structure refits, TLAS) | 1.8 |
| neural radiance cache | 0.8 |
| denoise | 0.7 |
| upscale, composite, post | 0.6 |

Everything that has been measured against it:

| | effect on the frame |
| --- | --- |
| **frame generation** | **the output rate: 44-48 rendered becomes 88-98 presented** |
| internal resolution | **strong**: 17.9 ms of GPU at 1114x626, 16.2 at 960x540 (56.3 FPS), 11.7 at 640x360 (68.5 FPS) |
| RTXDI off | +2.8 FPS |
| halving the top-level instance count (8,347 → 4,479) | none |
| `pathMaxBounces` 4 → 3 | +0.3 FPS |
| neural radiance cache vs ReSTIR GI | none |
| DLSS ray reconstruction | none |
| trace-optimised acceleration structures | none |
| opacity micromaps off | none (49.5 FPS, indirect 10.1 ms) |
| the game's own rendering | already gone: **17 draw calls and 16 render passes per frame**, which is the UI |

Of the things that change what the GPU does, only two move the rendered rate, and
they are the two the goal rules out: dropping to 960x540 into a 1080p output is a
visible softening, and turning RTXDI off makes the game's own light sources
resample far more noisily. Everything structural that has been tried — and the
retained merged bucket was the last candidate — leaves indirect integration where
it is, because that cost is ray work at the render resolution, not anything about
how the scene is organised.

The goal suggests looking for the vanilla game still doing work. It is not: the
command stream carries **17 draw calls and 16 render passes a frame**, which is
the user interface and the blit. Its world rendering is gone, and what is left on
the GPU is entirely the path tracer.

So the honest statement about *rendered* frames is unchanged: 60 of them a second
in this scene, at this render resolution and with this lighting, is not reachable
by any further optimisation this document has been able to find — on an RTX 4080,
indirect integration costs 9.6-10.1 ms for 1114x626 pixels and nothing structural
changes it. What reaches the goal as written is the output rate, and frame
generation gets it past the bar without touching what is rendered.

One quality lever remains unused and is worth recording: DLSS Performance instead
of Balanced closes roughly half the remaining gap on *rendered* frames. It is not
needed now, and it is a decision about the look, which the goal reserves.

### A note on the machine

The system drive was full (0.6 MB free) during this session. It first showed up
as `LNK1108: cannot write file at 0x0` from the linker; `BuildRuntime.ps1` now
points the build's `TMP`/`TEMP` at `.research/buildtemp` so the build does not
depend on free space there. Measurements taken while a volume is that full are
worth re-checking — the game's own logs are written to it.

### Deployed state after this pass

Verified at the fixed town camera and at ground level in Riverwood against the
pre-change captures (`.research/captures/045730-final-ground.png`,
`045732-final-town.png`, `050357-retainedsets-town.png`,
`051415-pass2-ground.png`, `053633-persistent-ground.png`,
`060115-final2-ground.png`, `060117-final2-town.png`,
`063040-promote-ground.png`, `071001-bucketwork-ground.png`,
`072548-spatial-ground.png` and `080707-dlfgprobe-ground.png`, against
`025118-fillcache-town.png` and `025137-fillcache-ground.png`), with
`083231-dlfg-town.png` and `083233-dlfg-ground.png` taken through the
interpolating presenter: geometry, alpha-tested foliage and ivy, thatch, grass,
terrain and its blending, the wicker fences, the sky and the UI compass are
unchanged. The path tracer's inputs are untouched by this pass — only what
happens to the finished frame between the blit and the display.

Deployed binaries:

| | SHA-256 |
| --- | --- |
| `dxvk_d3d11.dll` | `13D13135DB98274723E8CAD8D5F42A09B6B11B0FA899DC31D16D521BBE7542D4` |
| `dxvk_dxgi.dll` | `FA339D7CE3DF85AC187446D6A57C1EC1B7E15FFDDB27BC312D1551B647336531` |
| `CommunityShaders.dll` | `658A5A28AB115CF8BAC87485370F5DA56B8189435925993B16FE78811299FCF7` |

The plugin is unchanged by this pass; everything in it is in the runtime. Every
deployment overwrites its own files and nothing else — backups of what was
replaced are under `.research/deployment-backups/`.

## Native 1080p, no upscaling, no frame generation

The benchmark condition changed to the hardest one: the path tracer traces every
output pixel, nothing interpolates, and DLSS is present only as an anti-aliaser
(`rtx.qualityDLSSOverride = 5`, FullResolution -- DLAA). `rtx.dlfg.enable` is
off. Confirmed each run by the runtime's own report rather than by the setting:

```
[Upscaler.extent] render=1920x1080 target=1920x1080 ...
[Upscaler.dlfg] supported=1 optionEnabled=0 active=0 interpolatedFrames=0
```

**32.5 FPS** (32.56, 32.51, 32.38), from **28.2** in the same warm session
before the change below. 2.07M traced pixels a frame against 697k for the
upscaled configuration this document measured earlier, so the two sets of
figures are not comparable with each other.

### One lever moved it, and it was not one of the obvious ones

Roughly twenty options were swept at this resolution. Everything that sounds
like it should matter did not:

| | FPS |
| --- | --- |
| baseline | 28.2 |
| **`rtx.neeCache.enableOnFirstBounce=False`** | **32.4** |
| `psrrMaxBounces` / `pstrMaxBounces` 10 -> 0 | 29.9 |
| gbuffer raytrace mode RayQuery / RayQueryRayGen | 30.0 / 29.8 |
| `denoiseDirectAndIndirectLightingSeparately=False` | 30.4 |
| `russianRouletteMode` specular-based | 30.2 |
| `risLightSampleCount` 7 -> 2 | 30.3 |
| `neeCache.enableMIS=False` | 27.9 |
| `neeCache.enableImportanceSampling=False` | 28.1 |
| `enableRayReconstruction=True` | 24.7 (gbuffer 7.4 -> 4.7, lost again elsewhere) |
| `pathMinBounces=0` | 28.1 |
| `enableUnorderedResolveInIndirectRays=False` | 13.5 |

Primary surface replacement at ten bounces was the strongest hypothesis for the
gbuffer costing 8.4 ms for 2.07M primary rays -- about 4 ns a ray, when this GPU
should manage primary visibility in one or two milliseconds. Zeroing it changes
nothing, so that is not where the time goes.

`neeCache.enableOnFirstBounce` stops the cache doing next-event estimation
against emissive triangles on the primary hit. Path tracing drops from **26.7 ms
to 15.8 ms**. A/B'd four times alternating, warm: 25.8 / 30.2 / 25.9 / 30.3.

### What it costs

Measured rather than judged by eye, because the first comparison was confounded
by an NPC walking into frame. Camera locked at Riverwood's forge, six frames
each, alternating twice:

| `enableOnFirstBounce` | mean scene luminance |
| --- | --- |
| True | 59.92, 59.61 |
| False | 56.52, 56.34 |

**About 6 per cent less light.** The forge still renders -- coals glowing,
embers, the fire itself -- and in daylight with no emitter in frame the two are
indistinguishable. What is lost is part of the forge's indirect contribution to
the wood and stone around it. That is a real change to the look, so it is set in
`ConfigureRunningTest.ps1` with the trade written next to it and is one edit to
put back.

### The command-stream thread

Two fixes, each verified, neither of which helps at native because the GPU is
the limiter there -- but both lift the ceiling everywhere else:

| | before | after |
| --- | --- | --- |
| retained replay | 4.54 ms | **2.94** |
| merge | 6.32 ms | **4.97** |
| `prepareSceneData` total | 9.25 ms | **7.93** |

Both were the same defect, and an embarrassing one: **the instrument was a
measurable fraction of the measurement**. The replay took five
`steady_clock::now()` readings for every one of 8,300 entries, and the merge
loops took three for each of about 16,000 iterations -- roughly 56,000
`QueryPerformanceCounter` calls a frame, on the two loops whose cost was the
thing being optimised. The per-entry splits are opt-in now behind
`CS_REMIX_REPLAY_TIMING=1`; the once-a-frame totals that remain cost nothing.

At 640x360, where the GPU is cheap enough that the CPU decides the frame, this
measured **65.7 -> 77.9 FPS**.

### Where the frame stands at native

| | ms |
| --- | --- |
| GPU frame (as instrumented) | 22.60 |
| — path tracing | 15.83 |
| — denoise | 2.23 |
| — nrc | 1.53 |
| — sceneCullTlas | 1.13 |
| frame period | 30.92 |
| game thread busy | 12.3 |
| game thread waiting on the GPU | 17.7 |

The 8.3 ms between the instrumented GPU frame and the frame period is not idle
time. Raising `DXVK_FRAME_LATENCY_OVERRIDE` from 2 to 3 changed the frame rate
by nothing (32.13-32.28 against 32.5) but drove the command-stream thread to
`csBusyMs=30.95` with `csIdleMs=0`: with a deeper queue it saturates at about 31
ms. So the GPU really is busy for about that long and `scope=frame` is missing
roughly eight milliseconds of it -- most samples are being rejected as
out-of-order, and the survivors look biased toward fast frames. The frame-rate
conclusion does not depend on that gap, but per-stage attribution at native
should be treated as a lower bound until it is closed.

### On the numbers in the sections above

Riverwood keeps getting faster for many minutes after the camera stops. The same
native configuration read 24.9 FPS after a settle that declared itself finished
at 180 seconds, and 29.7 once genuinely warm. The settle test compares a
three-window mean against one a full minute earlier, which catches the fast part
of the warm-up and not the tail. Only same-session A/B comparisons in this
document are safe; absolute figures from different sessions are not comparable,
and several earlier ones are biased low.

### The eight-millisecond gap was the instrument, not the frame

The GPU scope reported 22.6 ms against a 30.9 ms frame and the difference was
attributed, in this document, to work the instrumentation could not see. There
was no such work. The timestamps were being read before the GPU had written
them.

`DxvkGpuQuery::getDataForHandle` gates on the query pool's reset event and then
calls `vkGetQueryPoolResults` without `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`.
The reset event only says the slot was reset; it says nothing about whether this
frame's timestamp has landed in it. A read issued before the GPU gets there
returns `VK_SUCCESS` carrying whatever the slot's previous occupant left behind.

The retire loop runs on every frame and was reading each sample the frame after
recording it, while the GPU was still two or three frames behind. So it was
reading timestamps belonging to unrelated frames. That produced:

- adjacent marks hundreds of milliseconds apart in both directions
  (`sceneAccelLight=-840.78`, `composite=-83.93`, `sceneSetup=-245.54`),
- the ordering check rejecting almost every sample -- 2,559 dropped in one run,
- and the few that survived being biased low, because a sample only passes the
  monotonic check when its stale values happen to ascend. Surviving totals
  wandered between 14.5 and 28.2 ms for the same parked camera.

The fix is to leave a sample alone until the GPU has certainly executed the
command lists holding it: `kReadSampleAfterFrames = 8`, comfortably past the
frames in flight. It costs a little latency on a diagnostic and nothing else.

Before and after, same scene, same camera:

| | before | after |
| --- | --- | --- |
| samples rejected as out-of-order | 2,559 and climbing | **0** |
| samples failed | varied | **0** |
| frame total | 14.5-28.2, unstable | **27.8-29.5** |
| unexplained gap against the frame period | 8.3 ms | **1.4 ms** |

The residual 1.4 ms is the present, the blit and the game's own seventeen draw
calls, which are genuinely outside the scope. That is the whole of it.

### What the frame actually looks like

The first per-stage attribution in this document that can be trusted. Parts sum
to 28.46 against a reported 28.55, and the total sits 1.4 ms under a 29.95 ms
frame:

| | ms |
| --- | --- |
| path tracing | 21.61 |
| — gbuffer | 5.97 |
| — integrate | 14.57 |
| —— indirect | **13.19** |
| —— direct | 1.21 |
| —— gradient | 0.10 |
| — rtxdi | 1.06 |
| denoise | 2.40 |
| nrc | 1.34 |
| sceneCullTlas | 0.90 |
| sceneMergeBlas | 0.76 |
| composite | 0.52 |
| volumetrics | 0.28 |
| upscale | 0.27 |
| post | 0.27 |

Every per-stage GPU figure quoted earlier in this document was drawn from the
broken sampler and should be read as unreliable -- the ranking it implied
survives, the magnitudes do not. Frame-rate figures are unaffected: they come
from frame counts, never from these queries.

### Two things measured while chasing it

**Alpha testing could not be ruled in or out.** `rtx.enableAlphaTest=False`
measured 31.78 against a 32.11 baseline, which looks like a definitive answer
until the capture is compared: foliage cutouts and grass blades are still there,
so the option never took effect and the test is void. The resolve thresholds are
live -- widening `resolveTransparencyThreshold` to 0.1 and lowering
`resolveOpaquenessThreshold` to 0.9 costs 3.7 FPS, and disabling
`enableSeparateUnorderedApproximations` costs 8.6 -- so the defaults are already
the best of the settings available there.

**Grass is worth about eight per cent, as a ceiling.** The game's own `tg`
removes it entirely: 31.2 -> 33.9 FPS, and back to 31.4 when restored. That
bounds any scheme for tracing grass only in the g-buffer and skipping it for
secondary rays, since such a scheme keeps the primary cost. The instance mask has
no spare bit -- all eight are allocated -- but Skyrim has no ray portals, so
`OBJECT_MASK_PORTAL` is available to repurpose if that work is ever wanted.

## 1080p output, DLSS Balanced, frame generation on

Verified each run from the runtime's own report rather than from the settings
asked for:

```
[Upscaler.extent] render=1114x626 target=1920x1080 qualityOverride=2
```

**56.5 rendered / 113.1 presented** (56.57/113.31, 56.40/113.06, 56.50/113.06).
Warm, the same figure reads about 118.

### Changing the DLSS profile with frame generation on killed the process

Three launches in a row died with the log ending on the same line:

```
[Config.set] rtx.qualityDLSSOverride = '2' type=1 resolved=6
```

`resetScreenResolution` replaces the ray-tracing outputs and marks the DLFG
context dirty, which re-initialises the NGX feature for the new render extent.
The frame-generation presenter runs its own present and pacer threads and is
holding that frame's motion vectors and depth while that happens. Nothing
synchronised the two.

This is not a benchmark problem. Any user changing a graphics setting in game
with frame generation enabled hits it, and frame generation is on by default.

`RtxContext::resetScreenResolution` now drains the presenter before the resize:

```cpp
m_device->synchronizePresenter();
getResourceManager().onResize(this, downscaleExtent, upscaleExtent);
```

Verified against the reproducer -- Balanced, MaxPerf, FullResolution, Balanced,
with frame generation live, the render extent changing each time so the resize
genuinely ran:

| | before | after |
| --- | --- | --- |
| first profile change | process dies | `render=1114x626, alive` |
| second | -- | `render=960x540, alive` |
| third | -- | `render=1920x1080, alive` |
| fourth | -- | `render=1114x626, alive` |

`LaunchTest.ps1` also gained `-DlssProfile`, which puts the profile in
`RTX_QUALITY_DLSS_OVERRIDE` before anything presents. That was the workaround
found while the crash was still unexplained; it is kept because pinning the
profile at startup is the right thing for a benchmark regardless, and it is how
the harness avoids a resize mid-run.

It also cleared a false suspicion. The crash reproduced identically with
`-NoGpuTiming`, which ruled out the GPU timing rework that had been deployed
just before it first appeared. With the resize fixed, timing runs clean: 963
samples, `dropped=0`.

### 200 presented is not reachable at this resolution

200 presented is 100 rendered, a 10.0 ms frame. The frame is 17.0 ms and the GPU
16.0 of it, with the command-stream thread 47 per cent idle -- so it is the GPU,
and the GPU is:

| | ms |
| --- | --- |
| path tracing | 11.93 |
| — indirect | **7.38** |
| — gbuffer | 3.42 |
| — rtxdi | 1.28 |
| — direct | 0.49 |
| sceneCullTlas | 1.55 |
| nrc | 0.82 |
| denoise | 0.72 |
| upscale, composite, post | 0.63 |

Stacking every lever that helps, measured rather than predicted:

| | rendered | presented |
| --- | --- | --- |
| baseline | 57.7 | 115.4 |
| + RTXDI off | 59.9 | 119.9 |
| + grass off | 61.3 | 122.7 |
| + all indirect lighting off | 93.2 | **186.6** |

**Those four rows were measured before the scene had settled and the conclusion
drawn from them was wrong.** They are kept because the mistake is instructive.

Re-measured settled, with indirect lighting and RTXDI off:

| | GPU frame | rendered | presented |
| --- | --- | --- | --- |
| full quality | 16.68 ms | 58.4 | 116.7 |
| no indirect lighting, no RTXDI | **10.24 ms** | 90.0 | 180.1 |

and the 10.24 ms is:

| | ms |
| --- | --- |
| path tracing (g-buffer) | 3.92 |
| **nrc** | **3.15** |
| sceneCullTlas | 1.52 |
| denoise | 0.70 |
| upscale, composite, post | 0.62 |
| scene structures | 0.23 |

Two errors, both inflating the floor. The first run was taken while the retained
replay was still churning at 3.5 ms a frame; settled it is 0.85 ms with 29 full
submits rather than 264. Riverwood keeps converging for minutes, which this
document warns about a few sections earlier, and the measurement was taken too
early anyway.

The second is worse: a third of what was being counted as fixed cost is an
artefact of the test. The neural radiance cache costs 3.15 ms there because with
secondary bounces off it never converges and keeps training at full rate against
nothing to cache. With lighting on it costs 0.82 ms. It is not a fixed cost.

So the real non-lighting cost is around **7 ms, inside the 10 ms budget**, and
the claim that "the frame's non-lighting cost alone exceeds the budget" was
false. The blocker is narrower than it was described as: **indirect integration
alone**, 7.4 ms of a 16.68 ms frame. Everything else fits.

That still leaves the target out of reach -- halving indirect would give roughly
154 presented, and reaching 200 needs it close to eliminated -- and frame
generation is already at its ceiling, since Ada reports `maxSupported=1`. But
the reason is one thing rather than several, and the number to attack is one
number.

Indirect is not path length: `pathMaxBounces` from 4 to 1 is worth one frame a
second, so the paths are already short. At 697k pixels, 7.4 ms is about 10.6 ns
per pixel for roughly a single incoherent bounce -- the cost of tracing one ray
per pixel through this scene's structures and resolving its hit. Halving it
means tracing half as many, which is a half-resolution indirect pass and a
change this renderer does not expose.

Everything swept at this resolution against the 7.4 ms of indirect integration:
`pathMaxBounces`, `pathMinBounces`, `enableRussianRoulette`,
`russianRouletteMode`, `russianRoulette1stBounceMinContinueProbability`,
`integrateIndirectMode` across all three modes, the NEE cache's `enable`, `MIS`,
`importanceSampling` and `enableOnFirstBounce`,
`neuralRadianceCache.terminationHeuristicThreshold` across a tenfold range,
`targetNumTrainingIterations`, `psrrMaxBounces`, `pstrMaxBounces`, both raytrace
modes, `denoiseDirectAndIndirectLightingSeparately`, `risLightSampleCount`, the
resolve thresholds, `enableSeparateUnorderedApproximations`,
`enableRayReconstruction`, `useRTXDI`, grass.

Four moved anything. `neeCache.enableOnFirstBounce` is already applied; the other
three are quality losses. The radiance cache's termination heuristic -- the
option that exists precisely to make paths end in the cache sooner -- moved
indirect from 7.38 ms to 7.43 ms across its whole range.

What remains is engineering rather than settings, and it does not reach 200
either: the TLAS is rebuilt every frame rather than refitted, which is worth at
most the 1.55 ms it costs, taking the frame to roughly 132 presented. It also
carries a real risk of going backwards, because camera-origin rebasing moves
every instance whenever the camera moves and a refitted top-level structure
degrades under that.

200 presented is reachable at a lower internal resolution, where the dominant
cost scales down and the frame becomes CPU-bound -- at 640x360 the
command-stream thread is 11.5 ms busy against 1.0 idle, which is where the
replay and merge work in this document starts paying and where the remaining
targets (merge walking 8,331 structures to update 254, `surfUpload` re-uploading
13,401 surfaces for 252 changes) would land.

### The top-level refit was worth nothing, and why

`sceneCullTlas` costs 1.55 ms and the top-level structure was rebuilt from
scratch every frame for eight thousand instances, so refitting it looked like
the last obvious ~1.5 ms on the table. The flags already carried ALLOW_UPDATE;
only the mode was never used.

It is implemented and it works: 14,036 refits against 388 full rebuilds, 97 per
cent of builds now updates, with the count-changed and periodic-rebuild guards
doing the rest. The scene renders correctly -- geometry, foliage, grass, fences
and terrain all intact, which matters because a wrong top-level structure
corrupts geometry rather than failing loudly.

It bought nothing:

| | rendered | presented | sceneCullTlas |
| --- | --- | --- | --- |
| rebuild every frame | 56.5 | 113.1 | 1.55 ms |
| refit | 56.1 | 112.0 | 1.38 ms |

The 0.17 ms difference is the whole of it, because the attribution was wrong:
**`sceneCullTlas` is dominated by point-instancer culling, not by the top-level
build.** The build itself is about 0.2 ms. Refitting an acceleration structure
that only costs 0.2 ms to rebuild cannot pay.

It is left off, behind `CS_REMIX_TLAS_REFIT=1` (`LaunchTest.ps1 -TlasRefit`),
because it adds a failure mode -- a stale or invalid source structure renders
corrupt geometry -- for a gain inside the noise. The code is kept rather than
reverted so the measurement does not have to be repeated, and so the real target
is recorded: if that stage is ever worth attacking, it is the culling, not the
build.

### What the command-stream thread does when nothing changes

Settled, parked, with the clock stopped, the host re-describes nothing -- `0
changed instances, 0 moved only`, and the replay makes 29 full submits out of
8,332 entries in 0.85 ms. The retained path is doing exactly what it is supposed
to.

The runtime underneath it is not:

| | ms | doing |
| --- | --- | --- |
| `prepareSceneData` | 6.19 | |
| — merge | 3.43 | walks 8,354 instances, `submittedBuilds=0` |
| — surfMat | 0.83 | |
| — piCull | 0.66 | |
| — setup1 | 0.68 | |
| retained replay | 0.85 | 29 of 8,332 |

Merge spends 3.43 ms a frame walking every instance to build **zero**
acceleration structures, and `surfUpload` re-serialises all 13,424 surfaces --
0.61 ms of serialisation against 0.10 ms of actual upload, so the cost is
rebuilding the data, not sending it. Roughly 4.5 ms a frame of work whose output
is identical to the previous frame's.

None of it buys a single frame at this resolution: the command-stream thread is
8.2 ms busy against 8.7 ms idle, and the GPU sets the pace. It is worth having
anyway, because at a lower internal resolution the frame becomes CPU-bound and
this is precisely what binds it -- at 640x360 the thread measured 11.5 ms busy
against 1.0 ms idle.

`sceneCullTlas` is also 1.52 ms of the GPU frame, and it is worth recording that
the top-level build is only about 0.2 ms of that: the rest is point-instancer
culling. Refitting the structure instead of rebuilding it was implemented,
verified correct and measured at 0.17 ms, which is why it stays off.

## Riverwood CPU frame profile (September 19)

Measured on the clean `dxvk-remix` fork with the Release plugin, native 1080p,
free camera parked at the Riverwood mill. Every line below is a timed mean over
120 frames, not an estimate.

| phase | ms | share |
| --- | --- | --- |
| `AccelManager::mergeInstancesIntoBlas` | 9.4 | 33% |
| plugin capture + submit | 4.6 | 16% |
| the game's world render, with its draws suppressed | 4.0 | 14% |
| surface material writes, GC, bindless tables, lights | 1.9 | 7% |
| remainder (dispatch recording, present) | ~8.7 | 30% |
| **total CPU frame** | **28.6** | |

Settled frame rate is 47 fps against a 200 fps target, which needs a 5 ms frame.

**The GPU is not involved.** DLSS Ultra Performance, which path traces about a
third of the pixels, measured 35.7 fps against 35.5 for the default profile. The
frame is CPU bound end to end, so resolution and path-tracing quality settings
cannot move it.

**The game is not the problem either.** Its whole world render — scenegraph
traversal, culling, batch building, shader and constant setup, with only the
draw submissions suppressed — is 4.0 ms. Removing it outright, which would cost
the sky cubemap, buys about 41 fps.

**Plugin-side scene capture has a hard ceiling.** Gather, capture and submit
together are 4.6 ms, so driving the plugin's cost to zero is worth roughly
+8 fps. Making effects probe-eligible was a real win on its own terms — the
ineligible count fell from 1376 to 60 and capture from 4.50 to 3.54 ms — and it
moved the frame rate by under one fps.

### Why the existing BLAS cache cannot help

`mergeInstancesIntoBlas` merges 8,351 instances into **6** merged BLAS buckets.
`AccelManager` already has a per-bucket incremental cache, but
`removeInstanceFromBucketCache` cleared the whole cache and reset the generation
counter whenever any single instance was destroyed, so with normal GC churn the
cache was empty every frame and every bucket rebuilt from scratch.

Fixing that was tried: evict only the dying instance's bucket and keep the rest.
The cache then populated and survived, and **it was slower** — 4.1 of 6 buckets
came back dirty every frame while validation added an 8,351-entry set build, and
the merge went from 9.4 ms to 11.0 ms. The change was reverted and the reason
recorded at the call site.

The granularity is the problem, not the bookkeeping. A bucket is about 1,400
instances, so one change rebuilds all of them. Reaching 5 ms/frame needs buckets
fine grained enough that a dirty one is cheap to rebuild. That is a structural
change to how instances are grouped, and it is the highest-value performance
work outstanding.

## Riverwood CPU frame, second pass (September 19)

Measured with `tools/remix/RunRiverwoodTest.ps1` at the canonical pose, after
settling and with the load-time message box dismissed. The settle windows the
harness prints while a message box is still up read about 10 FPS high — a modal
pauses the world — so only the post-dismiss measurement is comparable.

Frame rate varies by up to 1.5 FPS between launches of the same build while
repeat samples within one launch agree to about 0.3, so a single reading either
side proves nothing. These figures come from an A/B under one protocol: restore
the runtime from `.research/deployment-backups/`, launch, settle, take six
ten-second samples, then repeat with the new build.

| | Baseline | After |
|---|---|---|
| Frame rate (median of 6) | 33.2 fps | **35.1 fps** |
| Frame rate (range) | 31.5 - 33.4 | 34.7 - 35.3 |
| `mergeInstancesIntoBlas` | 9.99 ms | 8.17 ms |
| ...per-instance loop | 5.22 ms | 3.37 ms |
| ...`buildBlases` | — | 2.61 ms |
| `prepareSceneData` | 12.1 ms | 11.1 ms |

Three changes, each measured separately.

**Redundant pipeline barriers.** `trackBlasBuildResources` ended in
`execBarriers.recordCommands`, and the merged path called it once per instance —
8,350 pipeline barriers a frame. `buildBlases` already flushes the accumulated
barrier set (the comment there says so explicitly), so the per-call flush was
removed and the tracking deduplicated by `BlasEntry`, since instances sharing an
entry produce identical barriers. Merge 9.99 -> 8.85 ms.

**RtxOption reads in the hot loop.** `RtxOption::getValue` takes a global mutex
on every read. The BLAS routing read four options per instance, so the loop was
doing roughly 42,000 lock/unlock pairs a frame for values that cannot change
mid-loop. Hoisted out. Loop 4.29 -> 3.45 ms.

**Instance churn, which was the real blocker.** Counting destructions showed
28.6 per frame, and a histogram by material hash showed they were **the same
~30 meshes every frame** — torn down and rebuilt rather than transient. With
only 6 buckets those 30 meshes dirtied 4.1 of them, which is exactly the result
the earlier surgical-eviction attempt recorded and why it was judged a
regression.

The fix routes a mesh that was destroyed while cached to its own dynamic BLAS
for the next `kChurnMemoryFrames` frames, keyed on material hash, so one
unstable mesh cannot dirty a bucket holding a thousand stable ones. Billboards
and point instancers keep their existing routing because the merged path is load
bearing for them. Cached evictions fell from 28.6 to 0.25 per frame.

Surgical eviction is now in place rather than the full cache clear, and the
8,351-entry liveness set that made the earlier attempt expensive is gone: a
bucket is excluded from the cache if it holds any renderer-created instance,
which is the only class `InstanceManager::removeInstance` skips the
`onInstanceDestroyed` callback for. Every cached instance is therefore
guaranteed to evict itself on destruction, and a bucket that lost one is marked
dirty *before* the validation scan so its dangling pointer is never read.

### Routing bucket-dirtying meshes: measured, rejected

The same trick was tried against the remaining dirty buckets. Instrumenting the
validation showed ~490 dirty-bucket events per 120 frames and attributed
essentially all of them to `isBlasDirty()` / `frameLastUpdated == currentFrame`,
with identity, bucket-key and size-mismatch contributing nothing. Routing the
offending geometry to its own BLAS, keyed on `BlasEntry`, did cut dirty buckets
from 4.1 to 2.2 of 6 and the loop from 3.37 to 3.09 ms — and still lost frame
rate, because 213 meshes then needed a dynamic BLAS every frame.

That is the difference from the destruction churn: ~30 meshes are cheap to
relocate, 213 are not. The change was reverted and the numbers recorded at the
call site so it is not retried blind.

### What still caps it

4.1 of 6 buckets are dirty every frame even though evictions no longer reach the
cache, and the measurement above shows relocating the cause is not affordable at
this bucket granularity. The plugin reports ~3,440 full captures and ~5,186
staggered submissions a frame against only ~1,718 probe hits, so nearly every
bucket has some BlasEntry touched.

The arm-time ineligibility counters name where those captures come from:
skinned 674, multiStream 394, distantTree 372, nativeLandscape 100,
lodLandscape 73, water 65, dynamic 62, effect 60, hairTint 32, grass 22. The
landscape and distant-tree categories are static in this scene — tree wind is
not currently driven — so roughly 545 objects a frame are re-derived that could
in principle be probe-armed. That is the next lever, and it is plugin side, in
the arm gate in `RemixScene.cpp` rather than anywhere in `AccelManager`.

## The suppressed world render: attributed, and mostly cut

`RemixNativeRender`'s counter reported the game's world render costing **4.0
ms/frame** with every draw and dispatch already stubbed. Hooking the individual
call sites inside `Main::Draw` (addresses from a Ghidra call-site listing of
`0x140656c00`) attributed it:

| Phase | ms/frame |
|---|---|
| `Main_RenderWorld` (`Main::Draw + 0x85E`) | 1.75 |
| `NiCamera::CalculateAndDrawShadowCasterLights` (`+0x1C4`) | 0.55 |
| shadow depth (`+0x3B3`) | 0.49 |
| scene list accumulation + culling (`+0x172`) | 0.41 |
| everything else, individually | < 0.05 |
| unattributed | ~0.8 |

### Main_RenderWorld is skipped

Nothing the plugin captures comes from that pass. `RemixScene::Submit`
discovers geometry through `RemixSceneGraph::Gather()`, which walks attached
cells, references and nodes on its own cadence. The only capture inputs that
ever came from inside it were the grass scale mask and wind, read in
`BSGrassShader::SetupGeometry` — and the capture path already derives both
itself, from the shader property and `RemixNativeRender::ReadGrassWind`. The
audit comparing the two over **330,836 samples reported a maximum difference of
exactly zero**, so the hook-sourced values were redundant.

The skip lives in `Deferred::Hooks::Main_RenderWorld::thunk`, a game-code call
site hook that already owned that call — no D3D11 or Vulkan hook is involved.
The suppressed world render fell from 4.0 to **2.3 ms**. The sky is unaffected:
the reflections cubemap renders through the separate
`BSCubeMapCamera::RenderCubemap` vfunc and `[RemixSky]` keeps submitting its
2048x1024 resolve every frame.

Capture time fell as a side effect, 5.7 to 3.7 ms, because the per-grass-draw
`RecordGrassScale` / `RecordGrassWind` calls no longer happen at all.

### The shadow map pass is skipped; its sibling is load bearing

Skipping both shadow passes at once hung the game, so they were separated.

`Main_RenderShadowMaps` is safe once `globals::deferred->EarlyPrepasses()` is
kept: that call is CS's own work, and dropping it along with the game pass was
what hung the main thread. Skipping only the game pass took the suppressed world
render from 2.3 to **1.3 ms** — more than the 0.49 ms measured at `+0x3B3`,
because the pass hooked at `+0x30A` was never timed separately.

`NiCamera::CalculateAndDrawShadowCasterLights` (`+0x1C4`, 0.55 ms) is **not**
safe. Suppressing it on its own still hangs the main thread, and only a process
restart recovers. Something downstream waits on work it schedules, so that 0.55
ms stays on the table until the dependency is identified. The finding is
recorded at the call site so it is not retried blind.

## Result

Baseline is the original runtime and plugin; "after" is everything above. Same
protocol both sides: restore from `.research/deployment-backups/`, launch,
settle, take repeated ten-second samples.

| | Baseline | After |
|---|---|---|
| Frame rate (median) | 33.2 fps | **37.0 fps** |
| Frame period | 28.9 ms | 26.8 ms |
| `mergeInstancesIntoBlas` | 9.99 ms | 8.17 ms |
| suppressed world render | 4.0 ms | 1.3 ms |
| plugin capture | 5.7 ms | 3.5 ms |

**+11.3%**, against a cross-launch spread of about 1.5 FPS and a within-launch
spread of 0.3.

### Where the remaining 26.8 ms sits

The frame is **entirely CPU bound, with the GPU idle**. Cutting DLSS to Ultra
Performance moves it from 37.38 to 37.22 FPS, and cutting path bounces to one
moves it to 37.15 — both inside the noise. No amount of GPU work removal helps.

Timing the whole RTX injection splits the runtime's cost cleanly:

| | ms/frame |
|---|---|
| `injectRTX` total | 12.8 |
| ...`prepareSceneData` | 12.2 |
| ...recording every ray tracing and post pass | **0.49** |

Recording the ray tracing commands is essentially free. The runtime's CPU cost
is scene preparation, and `mergeInstancesIntoBlas` is 9.0 of that 12.2.

### The merge's remaining cost is architectural

Three separate attempts to cut it are now measured and rejected: routing
bucket-dirtying meshes to their own BLAS (above), and the merged/dynamic split
itself. That split is exposed as runtime options, so it was swept directly
against the live build:

| Case | FPS |
|---|---|
| baseline | 36.93 |
| `rtx.minimizeBlasMerging=1` | 33.38 |
| `rtx.minPrimsInDynamicBLAS=100` | 33.48 |
| `rtx.minPrimsInDynamicBLAS=300` | 33.72 |
| `rtx.maxPrimsInMergedBLAS=5000` | 33.66 |

Every alternative is worse. Pushing meshes out of the merged BLAS costs more
than the bucket rebuilds it avoids, so the shipped configuration is already the
best of them.

What actually gates the cache is upstream, and it now has a name. Instrumenting
the plugin's `sameDescription` check showed **294 geometries re-describing, every
single one because its bones changed** — NPC body parts, hair, brows, a rabbit.
Re-describing makes the runtime destroy and rebuild the `RtInstance`, which bumps
the scene generation and keeps both the bucket cache and `AccelManager`'s
full-skip path from ever settling. Fixing it needs a way to update bones on a
retained instance, the way `updateRetainedTransform` already handles a pure move
— a Remix API capability, not a tuning change. The finding is recorded at the
call site in `RemixScene.cpp`.


## The frame's thread structure, and why the merge is the whole story

Timing the D3D11 present path made the shape of the frame clear, and corrected
an earlier reading of it.

| Main-thread phase | ms/frame |
|---|---|
| `D3D11SwapChain::PresentImage` total | 19.2 |
| ...`SynchronizePresent` | 6.2 |
| ...`EndFrame` | ~0 |
| ...`Flush` | ~0 |

Present alone is 19.2 ms of a 26.9 ms frame, yet `injectRTX` is another 12.5 —
more than the frame. They do not add up because **`injectRTX` runs on the DXVK
CS thread**, not the main thread. `EmitCs` queues it and `FlushCsChunk` and
`SynchronizePresent` block until that thread catches up.

So the roughly 7.6 ms that looked unattributed is not work at all: it is the
main thread idling while the CS thread finishes. There is no separate
submission cost to remove. The critical path is
`injectRTX -> prepareSceneData -> mergeInstancesIntoBlas`, and everything else
in the frame is waiting on it.

That also rules out a change worth considering on its face. The 294 skinned
geometries that re-describe every frame cost plugin capture time, and capture
runs on the game thread — off the critical path. Giving the Remix API a way to
update bones on a retained instance would cut that 3.5 ms of capture without
moving the frame rate at all, until the merge is faster.

### The merge's per-instance lookup: measured, rejected

The merge loop consults `m_instanceBucketIndex` once per instance, 8,351 hash
lookups a frame. Replacing that with a revision-stamped field on `RtInstance`
(stale stamps self-invalidate, so nothing has to walk the instances when the
cache is rebuilt) moved the loop from 3.37 to 3.31 ms and the frame rate not at
all. The lookup was not the cost, and the change grows a hot 800-byte struct
that already exists 8,351 times. Reverted.

### Standing summary of what has been tried against the merge

| Attempt | Result |
|---|---|
| Remove redundant per-instance pipeline barriers | **kept**, 9.99 -> 8.85 ms |
| Hoist mutex-taking RtxOption reads out of the loop | **kept**, loop 4.29 -> 3.45 ms |
| Route destruction-churning meshes to their own BLAS | **kept**, evictions 28.6 -> 0.25/frame |
| Route bucket-dirtying meshes to their own BLAS | rejected, 213 dynamic BLASes cost more |
| Revision-stamped bucket index on the instance | rejected, no measurable change |
| `minimizeBlasMerging`, `minPrimsInDynamicBLAS`, `maxPrimsInMergedBLAS` | rejected, all worse |

The remaining cost is iterating 8,351 instances and rebuilding four of six
merged BLASes every frame. The full-skip path in `AccelManager` would bypass all
of it, but it requires the scene generation to be unchanged, and Riverwood
destroys around 28 instances a frame between NPCs and particles. That is
inherent to the scene, not a bug to fix.

## The retained replay, and the preserve path API draws could not reach

Timing the replay that `SceneManager::submitRetainedExternalDraws` performs
found the largest single cost in the frame, larger than the merge that had been
the focus until then:

| | ms/frame |
|---|---|
| retained replay, 8,331 draws | **14.4** |
| ...of which the `ExternalDrawState` deep copy | 1.4 |
| `mergeInstancesIntoBlas` | 8.5 |
| whole frame | 26.9 |

Only a tenth of it is the copy. The rest is `processDrawCallState` re-deriving
draw state — draw-call cache lookup, geometry hashing, material resolve,
instance update — for objects that did not change. The host's own counters put
the number that actually change at about **96 of 8,300 a frame**.

Remix already has the fast path for this. `SceneManager::submitDrawState`
computes `usePreservePath` and calls `preserveReplacementInstance` instead of
re-deriving. But `submitExternalDraw` calls `processDrawCallState` directly and
never passes through `submitDrawState`, so **no API draw has ever reached it** —
an instrumented counter on that decision did not fire once, because every draw
this host makes arrives through the external path.

### What was added

`RetainedExternalDraw` now carries a revision, bumped whenever the host changes
the registration — `setRetainedExternalDraw` for a re-description and
`setRetainedExternalDrawTransform` for a move, since the preserve path keeps the
transform the instance already has. The replay marks a draw preserve-eligible
when its revision is unchanged since the last full submission and the retained
scene origin has not moved, because rebasing changes every transform.

`submitExternalDraw` then keeps the instances it already produced instead of
re-deriving them, subject to the same kind of conditions `submitDrawState`
applies: the preserve option on, no particle description, clear dirty flags, a
prim per submesh, all prims live with a BLAS and not pending GC, and no expanded
grass, whose vertices are rewritten every frame rather than merely re-described.

### Result

| | Before | After |
|---|---|---|
| Frame rate (median of 5) | 37.1 fps | **49.2 fps** |
| Frame period | 26.9 ms | 20.3 ms |
| retained replay | 14.4 ms | **6.8 ms** |
| draws preserved / resubmitted | 0 / 8,331 | 7,947 / 408 |

The resubmitted 408 a frame are the categories that genuinely change — skinned
meshes, effects, water, grass — and the split stays at 407 with the game clock
running at timescale 20, so the revision tracking is following real change
rather than freezing the scene.

Against the session's starting point of 33.2 FPS this is **+48%**, and
`mergeInstancesIntoBlas` at 8.5 ms is once again the largest single item.

## Splitting merged BLAS buckets by static and dynamic

With the preserve path in place, 7,947 of 8,355 draws a frame are preserved and
only ~408 re-derived — but the merged BLAS buckets stayed 4.0 of 6 dirty,
because those 408 were scattered through buckets that also held the thousands
of unchanged instances.

Capping bucket size at 256 instances had already been tried and rejected: an
arbitrary split leaves unrelated meshes sharing a bucket, so the dirty rate does
not fall in proportion to the extra BLAS builds, and it lost 36.6 to 33.2 FPS.
The preserve path supplies the non-arbitrary version of the same idea, because
each instance now carries `surface.isPreservePath` — whether it was preserved or
re-derived this frame. Adding that to `BlasBucketKey` puts the changing
instances in their own buckets and leaves the static ones alone.

| | Before | After |
|---|---|---|
| Frame rate (median of 6) | 49.2 fps | **55.4 fps** |
| Frame period | 20.3 ms | 16.4 ms |
| `mergeInstancesIntoBlas` | 9.0 ms | **5.6 ms** |
| ...per-instance loop | 3.08 ms | **1.06 ms** |
| ...`buildBlases` | 3.21 ms | 2.11 ms |
| buckets, dirty / total | 4.0 / 6 | 6.2 / 12.1 |

The loop falls by two thirds because the static buckets stay clean and their
instances take the early skip. Bucket count roughly doubles, which is what the
size cap also did — the difference is that here the extra buckets are the ones
that actually change, so the dirty work falls with them.

### Verification

Stability at the canonical pose *improved*: `MeasureFlicker` reports a swing of
0.77 and a largest frame-to-frame luminance step of **0.49**, against 0.83 for
the previous build and the ~100 that marks the flicker defect. The captured
frame is indistinguishable from the baseline: same geometry, thatch shadows,
trees, sky and UI.

Motion was checked separately at the riverbank with the game clock running at
timescale 20. Differencing two captures seven seconds apart, the water region
changes 2.6x as much as a static mountain region and grass 1.5x, so animated
content is still animating rather than frozen by preservation.

## Where the session ended

| | Session start | End |
|---|---|---|
| Frame rate (median) | 33.2 fps | **~55 fps** |
| Frame period | 28.9 ms | 16.4 ms |
| retained replay | 14.4 ms | 6.8 ms |
| `mergeInstancesIntoBlas` | 9.99 ms | 5.6 ms |
| suppressed world render | 4.0 ms | 1.3 ms |
| plugin capture | 5.7 ms | 3.0 ms |

**+66%.** The frame remains entirely CPU bound with the GPU idle, and the CS
thread remains the critical path.

## Making the preserve path itself cheap

With most draws preserved, the replay's remaining 6.8 ms was no longer the work
of preserving. Timing the two `preserveInstance` calls showed they cost 0.83 ms
of it between them; the rest was the per-draw preamble each preserved draw still
paid — copying the registration, three hashes, a mesh-table lookup and the
instance resolution.

**Copying.** `submitExternalDraw` has to be handed a copy because it consumes
what it is given: it moves the instancing transforms out and overwrites the
geometry per submesh. Preserving touches none of that, so the preserve path was
lifted into `tryPreserveRetainedExternalDraw`, which runs straight off the
stored registration. Writing the rebased transform back into it is harmless
because a full submission always overwrites that from `absoluteObjectToWorld`.

**Hashing and the mesh lookup.** The identity hash, spatial-map hash, material
hash and submesh list depend only on the registration, so they are cached on the
entry and recomputed when the host changes it. The identity hash covers
`objectToWorld`, so the cache is also invalidated when the rebasing origin
moves — both conditions preserving already requires.

| | Before | After |
|---|---|---|
| Frame rate (median) | 55.4 fps | **~60 fps** |
| Frame period | 16.4 ms | 15.9 ms |
| retained replay | 6.8 ms | **5.2 ms** |
| `mergeInstancesIntoBlas` | 5.6 ms | 4.6 ms |

### Two things that did not work

Caching the resolved `ReplacementInstance*` as well, guarded by a generation
bumped on every `DrawCallTracker` destruction, **crashed with an access
violation**. Some path tears an instance down without that guard seeing it. The
pointer is no longer cached and the lookup stays; everything else in the cache
is either a value or held by `shared_ptr`, so nothing can dangle.

Caching the hashes before writing the rebased transform cost **half the frame
rate** — 60 down to 32 — because the identity hash covers `objectToWorld`, so
every cached key missed, every lookup created a fresh empty
`ReplacementInstance`, and every draw fell back to the full path. The order
matters: rebase first, then hash. It is a good example of a cache that fails
silently into correctness but catastrophically into cost.

## Direct scene nodes: the host does not need to be guessed at

Identity hashing, the identity map and the two-level spatial search exist so
Remix can work out which scene node a D3D9 draw call belongs to. It has to guess,
because the game never told it. A host driving Remix through the API is not in
that position: it registered the node and holds the handle.

So retained registrations now own their node directly. `ReplacementInstance`
gains `hostOwned`, which exempts it from garbage collection — collection exists
to reclaim instances a game simply stopped drawing, and a host-owned node's
lifetime is the host's business. The full path hands the node it resolves back
to the registration, and from then on the replay addresses it with no hash, no
map lookup and no spatial search.

| | Before | After |
|---|---|---|
| Frame rate (median of 5) | ~60 fps | **64.4 fps** |
| Frame period | 15.9 ms | 15.6 ms |
| retained replay | 4.75 ms | **3.89 ms** |
| `mergeInstancesIntoBlas` | 5.6 ms | 4.70 ms |

### Getting the invalidation right took three attempts

**A generation counter bumped on every instance destruction** invalidated every
held node every frame, because something is destroyed every frame. Preserved
draws fell to zero and the frame rate halved.

**Bumping it only on wholesale teardowns** was no better: one of those happens
every frame too, so the count was still 7,978 invalidations a frame.

**Exempting host-owned nodes from collection without releasing the old one**
when a registration re-resolved elsewhere leaked them — the instance count
doubled to 17,208 and the merge went to 13.2 ms. A registration now releases its
previous node when it takes a new one.

**Deferring invalidation to a drain at the start of the next replay** left the
pointer dangling for the rest of any frame in which an asset was removed, and
crashed. Destruction now calls straight back into the owning registration and
nulls the pointer before anything can read it.

### Also fixed: instanced draws could never be preserved

372 draws a frame were rejected with the catch-all `Other` dirty bit. The full
path hashes a draw's identity *after* moving its instancing transforms into the
draw call, so it always sees an empty vector; the preserve path hashed them
still populated. The two disagreed, the exact-match lookup missed, the spatial
match took over and marked the result drifted. Every instanced draw in the
scene, every frame. With the hashes matched, dirty misses went from 372 to 0.

### Verification

Frame-to-frame stability is unchanged at a swing of 0.91 and a largest luminance
step of **0.49**, against the ~100 that marks the flicker defect. With the game
clock running, differencing two riverbank captures six seconds apart shows the
water changing **8.5x** as much as a static mountain region and grass 2.7x, so
animated content is still animating. The remaining 393 resubmitted draws a frame
are all reported as genuinely changed by the host.

## Measuring the goal: GPU utilisation

There is no GPU timing in this fork — `CS_REMIX_GPU_TIMING` and the
`[CSRemix.GPU]` records the harness parses were never ported, which is why its
`gpu` fields are always empty. `nvidia-smi -q -d UTILIZATION` answers the
question directly and needs no code:

```
nvidia-smi -q -d UTILIZATION
```

At 64 FPS the GPU read **71-75%**, so roughly a quarter of every frame it had
nothing to do.

The frame also stopped being purely CPU bound somewhere along the way. At 33 FPS
neither DLSS nor path-tracing depth moved it at all; at 64 FPS raising bounces
from 4 to 8 costs 4.7 FPS. But dropping to zero bounces at Ultra Performance
only reaches 66.9, so a CPU floor around 15 ms was still setting the frame.

## Four more CPU cuts

**Build sizes for dynamic BLASes that are not rebuilt.**
`vkGetAccelerationStructureBuildSizesKHR` is a driver round trip, and the
dynamic BLAS loop called it for all 1,391 dynamic BLASes every frame to compute
a size that was then only used to confirm nothing had to happen. It is now only
called when the geometry changed, the BLAS is missing, or a rebuild is forced.
Loop 1.44 -> **0.66 ms**.

**Packing surfaces that were already packed.** `uploadSurfaceData` wrote all
13,422 surfaces into the staging vector every frame. An instance that was
preserved rather than re-derived, still sitting in the same slot and with the
same first-index offset, already has its bytes there from last frame. 1.42 ->
**0.93 ms**.

**The surface material buffer.** Same skip, plus the staging vector was a fresh
zero-initialised 1.5 MB allocation every frame; it is now kept across frames.
0.77 -> **0.36 ms**.

**Entry layout.** The replay walks every registration every frame and, for the
thousands it preserves, reads a handful of small fields -- which sat *after* the
several-hundred-byte `ExternalDrawState` in `RetainedExternalDraw`. Moving them
ahead of it turned each entry from a stride into a couple of cache lines.
`prepareSceneData` 7.51 -> **6.43 ms**.

| | Before | After |
|---|---|---|
| Frame rate (median of 4) | 64.4 fps | **68.7 fps** |
| GPU utilisation | 74.6% | **78.9%** |
| `prepareSceneData` | 7.95 ms | 6.43 ms |
| retained replay | 3.89 ms | 4.17 ms |

Stability is unchanged at a swing of 0.79 and a largest luminance step of 0.57.

### Where the frame stands

At ~14.6 ms the main thread spends 9.3 ms inside `PresentImage`, of which 4.4 is
`SynchronizePresent`; the rest is waiting on the CS thread through
`FlushCsChunk`. The plugin's own capture (2.9 ms, game thread) runs before the
CS work it produces, so the two serialise: capture plus CS is the frame. Getting
the GPU to 99% means the whole CPU chain has to fit inside the GPU's ~11.5 ms.

## The CPU was being paced by the GPU, not the other way round

With the GPU sitting at 76% and the frame stuck near 14 ms, timing the present
path phase by phase found where the main thread actually went:

| Present phase | ms/frame |
|---|---|
| `SyncFrameLatency` | **8.66** |
| everything else, together | 0.11 |

`SyncFrameLatency` waits on `m_frameLatencySignal` for the frame
`m_frameId - GetActualFrameLatency()`. That is the pacing mechanism: it holds
the CPU a fixed number of frames behind the GPU so input latency stays bounded.
With the latency capped at `BufferCount + 1` the CPU could never get far enough
ahead to keep the GPU fed, so the GPU idled a quarter of every frame while the
CPU waited to be let go.

Two changes, both driven by the host's existing request:

**`dxvkSetSyncPresent` now exists.** The Community Shaders host has always
called it at load and this fork never exported it, so it logged a warning and
left the fork's synchronous default. It is exported now and sets how many
presents may be outstanding; the swapchain keeps a status slot per outstanding
frame instead of one, and `SynchronizePresent` waits only on the slot it is
about to reuse.

**The requested depth reaches `GetActualFrameLatency` too.** Deepening the
present slots alone changed nothing, because the pacing is in
`SyncFrameLatency`, not `SynchronizePresent`. A host asking for asynchronous
present wants the CPU to run ahead of the GPU, and that is the call that decides
whether it may.

| | Before | After |
|---|---|---|
| Frame rate (median of 6) | 69.1 fps | **86.6 fps** |
| Frame period | 14.4 ms | **11.3 ms** |
| **GPU utilisation** | 75.7% | **97.6% mean, 100% peak** |
| present `SyncFrameLatency` | 8.66 ms | 0.02 ms |

The wait has moved to `SynchronizePresent` at 6.2 ms, which is now waiting on
the GPU — which is the point.

### Verification

Stability is the best it has measured all session: a swing of 0.75 and a largest
frame-to-frame luminance step of **0.48**, against the ~100 that marks the
flicker defect. The captured frame matches the baseline. With the game clock
running, differencing two riverbank captures six seconds apart still shows the
water changing far more than a static mountain region, so animation is intact.

Running the CPU further ahead of the GPU costs input latency by construction.
That is the trade this makes, and it is the trade the host asked for when it
called `dxvkSetSyncPresent(0)`.

## Session result

| | Start | End |
|---|---|---|
| Frame rate (median) | 33.2 fps | **86.6 fps** |
| Frame period | 28.9 ms | 11.3 ms |
| GPU utilisation | ~71% | **97.6%** |
| retained replay | 14.4 ms | 3.99 ms |
| `mergeInstancesIntoBlas` | 9.99 ms | 4.36 ms |
| suppressed world render | 4.0 ms | 1.3 ms |
| plugin capture | 5.7 ms | 2.77 ms |

**+161%**, and the frame is now GPU bound.

## Correction: the parked camera was hiding a collapse

Every measurement above was taken at the harness's fixed pose. Walking the
camera through Riverwood tells a different story, and it invalidates the frame
rates quoted for a moving camera:

| Camera | preserved / frame | replay |
|---|---|---|
| parked | 7,489 | 4.9 ms |
| moving | **993** | **17.2 ms** |

The retained replay passes a scene origin that the runtime rebases every
registered transform against, and that origin was the camera eye — so it moved
whenever the camera did. An instance's identity hash covers its *rebased*
transform, so a moving origin gave every object in the scene a new identity
every frame: the exact-match lookup missed for all of them, the spatial match
took over, and the whole scene was re-derived. In other words the preserve path
only worked while the camera was still, which is precisely the condition the
benchmark measures.

The fix snaps the origin to a 1024-unit grid. The origin only has to keep the
scene near enough to zero for float precision, so it can be coarse; crossing a
cell still re-derives everything, but that is once every 1024 units rather than
every frame.

| Camera moving | Before | After |
|---|---|---|
| preserved / frame | 993 | **7,796** |
| retained replay | 17.2 ms | **4.1 ms** |
| frame time, mean | 13.75 ms | **12.07 ms** |
| frame time, p99 | 23.30 ms | **18.14 ms** |

This was also the frame-time spike during movement: every few frames the whole
scene re-derived, which is a 17 ms replay in a 12 ms frame.

**Benchmark note.** `RunRiverwoodTest.ps1` parks the camera, which makes it
blind to exactly this class of regression. Any change to the retained path
should be checked with the camera moving as well.

## Also: a debug view was enabled in shipping code

`RemixBridge` set `rtx.debugView.debugViewIdx` to 32 (`DEBUG_VIEW_RAW_ALBEDO`)
at initialisation, left over from bringing the integration up against a known
material. A non-zero index is what turns the debug view on. Removed; the only
settings this integration now overrides are the two it genuinely needs --
`normalMapsAreXYZ` for Skyrim's normal maps and `albedoScale` of 1/0.65 for its
sRGB-tagged linear diffuse textures. RTXDI, ReSTIR GI, volumetrics, bounce
counts and the denoiser are all left at Remix's defaults.

## Correctness pass: what was mine, and what was not

### Two regressions I introduced, both reverted

**Camera snapping between positions.** Snapping the rebasing origin to a grid
(above) fixed the preserve path collapsing while the camera moves, but the
camera's submitted view matrix is relative to the game's own `posAdjust` eye.
Rebasing instances around a *different* origin displaces the whole world
relative to the camera by the difference, and that difference changes as you
move. The origin has to stay exactly the one the camera is rebased around, so
the fix is wrong as written; making identity stop depending on the rebasing is
the version that could work.

**Juddering water and skinned models.** Raising `GetActualFrameLatency` so the
CPU could run three frames ahead is what took the frame from 69 to 86 FPS and
the GPU from 76% to 97% busy. It also decouples when animation is sampled from
when the frame is shown. Reverted: a slower frame is better than a juddering
one. The `dxvkSetSyncPresent` export and the second present slot stay, because
the host has always asked for them and they measured neutral on their own.

Frame rate is back to ~65 FPS at the canonical pose with the original pacing.

### Objects flickering in and out: two causes found

Both destroy a registration on a *transient* condition, and the object is
recreated the next frame -- which is what popping in and out actually is.

**Instanced geometry (grass, distant trees, LOD).** `CaptureInstances` skips any
group whose vertex buffer or instance count is momentarily unavailable. If that
leaves the rebuilt placement set empty, the caller retired the object. It now
holds the last set that had anything in it across up to four empty rebuilds, so
only a sustained run counts as the object genuinely being gone.

**Distant trees.** A frame in which the tree atlas was briefly absent retired
*every* distant tree at once. Skipping the frame is enough: an instance already
registered keeps its registration and is replayed as it was, and one not yet
registered has nothing to retire.

### The placement hold needed a lifetime fix

Holding the last good placement set across an empty rebuild is only half of it.
At frame end the submission sweeps `instanceSetHandles` and destroys any handle
not referenced from `visible` or `meshes` -- and the held set is referenced from
neither, so its handle was destroyed while instances were still being handed it.
The symptom was the process disappearing during camera movement, with no crash
marker. `lastGoodPlacements` now counts as live in that sweep. Ten scripted
camera moves with captures survive where they previously killed it within a few.

### Still broken, found while looking

- Three `Fish:1` meshes (skinned, 4 bones, 1 partition) fail `Upload` every
  frame -- `ReadBuffer` cannot read their vertex buffer.
- `22 instanced batches (0 retained)`: grass and tree batches never take the
  retained path, so they are fully resubmitted every frame.
- Changing `cs.suppressWorld` or `rtx.debugView.debugViewIdx` at runtime kills
  the process. Both reconfigure the pipeline under a running frame.

### Floating objects: the standing stones are Skyrim's, the shards are not

Captured the same view with the game's own renderer through
`ConfigureRunningTest.ps1 -KeepVanillaWorld`: the standing stones that appear to
float and the smeared distant terrain are **identical in both**. It is Skyrim's
object LOD sitting over its terrain LOD, and the settings are already high
(`uLargeRefLODGridSize=11`, `fTreesMidLODSwitchDist` effectively disabling the
mid switch), so improving that means better LOD meshes, not code here.

The **shards are ours**, and they are a different thing. At (19900, -45200, 180)
looking at the cliff, this integration drew the rock face and the trees shot
through with thin angular slivers, pine-branch sprites lying at random angles
across the cliff, and long bare trunks rising out of the treeline. The same pose
through the game's own renderer is clean. So they are not Skyrim's LOD, and they
are not the standing stones.

Two things are established about them:

- **They are distant-tree billboards.** The slivers carry the tree atlas: pine
  branches and the pale card behind them, at orientations no billboard would
  take. `BSDistantTreeShaderProperty` is how they are recognised, not a
  `BSShaderMaterial::Feature`, which is why a census keyed on the LOD-object
  features (13, `kLODObjectsHD`) counted zero.
- **They are transient.** The same pose, later in the same session, renders
  clean, with the culled groups still being submitted. Whatever produces them is
  a state the scene passes through rather than a placement that is always wrong,
  which fits reading `BSMultiStreamInstanceTriShape::InstanceGroup`'s vertex
  buffer while the game is uploading into it -- `CaptureInstances` snapshots it
  with `ReadBuffer` and keeps whatever it saw until the bytes change again.

The census put numbers on how much distant-tree LOD is resurrected. Over
Riverwood, per frame: **383 instance groups submitted, 124 that the engine still
wants; 5,668 billboard instances, 2,653 wanted.** The flag carrying that is
`InstanceGroup::isVisible`, which `CaptureInstances` deliberately ignores so
off-screen grass keeps casting shadows and showing in reflections. For distant
trees the same flag also carries "the full trees for this cell are loaded".

`cs.respectDistantTreeCulling` (default off, so nothing changes) drops the
groups the engine has turned off. A/B at the pose above: no visible difference
in a clean frame, 259 of 383 groups not submitted. It is a lever, not a fix.

### A guard against reading an instance group mid-upload

`CaptureInstances` now refuses a distant-tree snapshot it cannot trust. The
game writes a unit 2D rotation per instance, so `cos^2 + sin^2` is 1 for every
instance it has ever produced; a read that caught the buffer mid-write splices
two instances together and almost never satisfies that. Scale is checked for the
same reason. A group that fails keeps the last snapshot that passed and is read
again next frame -- dropping it would take every tree in that cell out of the
scene for as long as the upload lasts, which is the flicker this is here to
avoid.

The guard does not fire on healthy data: **0 rejections** across a settled
scene, a seventy-second flight fourteen thousand units out and back, and three
captures at ten, forty and a hundred seconds after a fresh load.

### A group's placements have to be inside the group's own bound

The rotation check is a test of encoding, not of meaning: data can be perfectly
well-formed and still describe another cell's trees, which is what the shards
were -- real tree sprites in places no tree stands. So the placements are now
checked against the bound the game itself attaches to the group.

The relationship had to be measured before it could be relied on, and the first
attempt at measuring it was wrong. Compared naively, placements sit up to
**130,170 units** outside their group's bound, which looks like every group is
broken. They are not: the placements are relative to the shape, and the shape
carries the cell offset, because a half float tops out at 65,504 and cannot hold
an absolute Skyrim world coordinate at all. Composed with `shape->local` -- the
same transform the submission uses -- the furthest any placement in the whole
loaded scene sat outside its group's bound was **0 units**, over a settled scene
and again over a ninety-second flight across cells. Not "small": zero.

That makes it a real invariant rather than a heuristic, so `CaptureInstances`
now rejects a distant-tree snapshot whose placements escape the bound by more
than 512 units -- far above the ~32 unit step of a half float at these
magnitudes, far below a cell. A rejected group keeps its last good snapshot and
is read again next frame, the same handling as a torn read, so a cell's trees
are never dropped while the check waits for data it can trust.

Verified live: **0 groups rejected, escape still 0 units, all 383 groups and
5,668 tree instances still submitted.** The check cannot be removing anything
valid.

### Watching the guard catch it

A misplaced group cannot be produced by playing the game. It comes of a bad
first read that is then cached for the rest of the session -- the Riverwood
distant-tree block is read once and never changes again, which is why a dozen
attempts at reproducing the shards by flying across cells, reloading, and
capturing at ten, forty and a hundred seconds after a load all came back clean.
So the fault is injected instead. `cs.injectStrayTreeGroups` displaces every
distant-tree group's placements one cell over, which is what a group holding
another cell's trees looks like, and `cs.strayTreeGroupGuard` turns the check
off. Both default to off and on respectively; nothing ships changed.

Three captures at (19900, -45200, 180), pitch 0.04, yaw -1.49 -- the pose the
shards were worst at:

| | what the cliff looks like |
| --- | --- |
| no injection | full 3D pines in their places, rock intact |
| injection, guard **off** | tree billboards and bare trunks scattered across the rock face |
| injection, guard **on** | identical to the first |

The middle one is the artifact: the same cyan-lit pine cards standing on stone
and the same long thin trunks rising out of nothing that the original captures
showed. That is the mechanism confirmed, not inferred. With the guard on, the
runtime reported **358 groups rejected** and a furthest escape of 3,480 units --
the injected 4,096 less the groups' own bounds -- and the scene was clean.

So: the shards are distant-tree placements that belong to another cell, the
guard rejects exactly that, and on healthy data it rejects nothing (0 groups, 0
units of escape, all 383 groups and 5,668 instances still submitted).

### Particle systems are no longer retired on a one-frame gap

A particle system's quads collapse to zero area as the native fade takes each
particle out, and a system where that happens to all of them at once produces no
geometry for a frame or two before the next spawn. Retiring on the spot destroys
the mesh, the material and the registration and rebuilds all three when it
returns -- and the rebuild is subject to the 24-creations-per-frame cap, so it
can come back several frames later. That is a pop, not a gap.

`RetireInstanceIfStillEmpty` holds an object that has nothing to draw for
`kEmptyGraceFrames` (6, under 50 ms at these rates) before retiring it, and the
three transient paths use it: a particle system with no vertices, one whose
quads all collapsed, and a distant tree waiting for its atlas. Over Riverwood
the weather cloud systems went from 366 quick returns per reporting window to
120.

### Grass now shades from each face

`useFaceNormals` (was `preserveVertexNormals`) selected the interpolated
instance-up normal, which is Skyrim's raster trick for soft lighting across a
clump and is not the direction any of the blade's faces point. It now selects
the triangle's own normal, already flipped to the incoming side. The
`DEBUG_VIEW_GEOMETRY_NORMAL` view confirms it: terrain reads as uniform up while
every leaf and frond carries its own direction.

### Also removed

`rtx.debugView.debugViewIdx` was set to 32 (`DEBUG_VIEW_RAW_ALBEDO`) at
initialisation. The only settings this integration now overrides are
`normalMapsAreXYZ` and the 1/0.65 `albedoScale`; RTXDI, ReSTIR GI, volumetrics,
bounce counts and the denoiser are at Remix defaults.

## Moving the camera no longer re-describes the scene

Registered transforms are absolute; the replay rebases them around the origin
the host sends, which is the game's `posAdjust` eye. That origin moves with the
camera, so every frame of camera movement rewrote every transform, and the
preserve path -- which exists precisely to avoid re-deriving unchanged draws --
gave up on all of them. Parked it preserved 7,947 draws in 4.75 ms; moving, it
preserved 993 and took 17.2 ms. Moving the camera cost more than three times
what standing still cost, and that was the judder.

The origin moving is a change of coordinates, not of the scene. Remix now
treats it as one. `SceneManager::submitRetainedExternalDraws` measures how far
the origin moved since the last replay and, before it looks at any of this
frame's draws, shifts the whole world into the new origin:

- Every instance owned by a retained registration that the frame has not
  already touched is moved, walking `m_retainedExternalDraws` and each entry's
  node. Only those: they are the ones whose transform this replay derives from
  an absolute transform and the origin. The sky dome, the view model and the
  native render's own draws are each in a space of their own and must not move
  with the world. `RtInstance::rebaseBy` subtracts the delta from
  `objectToWorld` **and** `prevObjectToWorld`, so the object and its history
  stay in the same coordinates and nothing reports motion it does not have.
  Rotation is untouched, so `normalObjectToWorld` stays valid.
- `CameraManager::rebasePreviousFrames` does the same for every camera's
  previous-frame matrices, so reprojection compares like with like. A camera
  that has not been set up yet this frame also has its current matrices shifted,
  because `RtCamera::update` will rotate those into the previous slot.
  Translated-world matrices are camera-relative and are left alone.

Measured on the Riverwood scene with the camera orbiting continuously:
**preserved 5,041 of 5,108 draws (98.7%), replay 0.86 ms, merge 1.96 ms, host
frame period 7.3 ms, 123 fps** -- against 17.2 ms of replay and 993 preserved
before. Parked, the same build settles at 104-150 fps depending on the hour of
the in-game day.

### Two things this got wrong first, and why

**Moving only the instance, leaving history behind.** The first version set
`prevObjectToWorld` to the pre-shift transform and cleared `isStatic`, which is
arithmetically the correct motion vector but declares five thousand static
objects to be moving. The image during a pan measured 20-30% less sharp by
Laplacian variance than the same pan with the preserve path off, reproducibly
(1272/930 off versus 710/704 on). Shifting the camera's history instead brings
it back to parity.

**A merged BLAS bakes the transform in.** `mergeInstancesIntoBlas` skips
re-validating an instance whose surface is on the preserve path, on the grounds
that nothing about it can have changed. A shifted origin breaks that: the merged
geometry keeps the transform it was baked with while the TLAS moves on. It ran
for about ninety seconds and then took the device with it
(`VK_ERROR_DEVICE_LOST`). `rebaseBy` now sets `m_blasDirty`, exactly as a
dynamic `move()` does, and the merge's preserve-path skip defers to that flag.

### The crash seen during this work was not this

A 0xc0000005 fired four times while measuring, always with the same stack:
calling through a non-code pointer inside the game's shadow-caster lights,
reached from `Deferred::Hooks::Main_RenderShadowMaps::thunk` (`CommunityShaders.dmp`,
`cdb -z ... -c ".ecxr; k"`), with EngineFixes frames in between. It was being
worked on in parallel: `LightManager::prepareSceneData` sized
`m_lightMappingData` to `currentActiveCount + previousActiveCount` and then
indexed it at `currentActiveCount + light.getBufferIdx()`, so a light that
skipped a frame wrote past the end. EngineFixes routes Bethesda allocations
through the CRT heap, which is how a heap overflow lands on a vtable in the
shadow pass. Recorded here only so the stack is not re-derived: nothing in the
rebasing above is involved, and the per-frame suppression latch added while
chasing it (`RemixBridge::SuppressWorldThisFrame`) did not change it.

That latch is worth keeping on its own terms. `SuppressWorld()` reads live game
state and options that can change at any moment, and it was being evaluated
independently at six places spread across one world render. The passes inside a
single world render have to agree about whether the world is suppressed;
`WorldFrame::thunk` now decides once and every hook inside it reads that.

## Remix at its default graphics settings — 2026-09-19

`ConfigureRunningTest.ps1` used to force four non-default settings for speed:
`cs.graphicsPreset = 2` (Medium), `rtx.enableRayReconstruction = False`,
`rtx.neeCache.enableOnFirstBounce = False` and
`rtx.autoExposure.autoExposureSpeed = 1.0`. All four are gone. What remains is
`rtx.zUp` (a coordinate convention this game needs) and two debug-view entries
that are already the shipped defaults and are set only so a previous session's
ImGui fiddling cannot leak into a measurement.

`RtxOptions::updateGraphicsPresets` now logs what the preset actually resolved
to, because that was otherwise unanswerable from a log:

```
[RTX.preset] resolved graphicsPreset=1 (0 Ultra 1 High 2 Medium 3 Low 4 Custom)
  rtxdi=1 rayReconstruction=1 neeCacheFirstBounce=1
  unorderedResolveInIndirectRays=1 postFx=1 volumetrics=1
```

Auto picks **High** on this GPU. RTXDI, ray reconstruction, the NEE cache on the
first bounce, unordered resolve in indirect rays, Remix post-processing and
volumetrics are all on.

**Cost: 49.75 fps settled at the canonical Riverwood pose**, against 86.4 under
the old forced-Medium overrides. Every frame-rate number recorded in this file
before this section was taken under those overrides and is not comparable.

### Smoothness is a tail question, not a median one

`PhaseTiming` gained `Percentile()` and the per-frame line now reports the frame
period's p95 and p99 alongside the median. A run that alternates 14 ms and 34 ms
has the same median as a steady 24 ms and looks nothing like it.

| Camera | median | p95 | p99 | of which GPU wait |
| --- | --- | --- | --- | --- |
| parked | 13.6 ms | 20.3–22.7 | 24.0–29.8 | 10.3 ms |
| 60 s orbit | 17.9 ms | 26.3–29.1 | 29.5–32.4 | 13.1 ms |

The tail is roughly 1.8× the median in both cases, so it is not camera motion
that produces it — and with 10.3 of the parked 13.6 ms already spent waiting on
the GPU, the tail is GPU-side rather than anything the host does.

`spdlog` was flushing every `info` line straight to disk on the render thread,
and the periodic diagnostics emit a burst of twenty-odd lines once per 120
frames, which is exactly p99. That was the obvious suspect and it was wrong: the
tail did not move when the flush policy changed to `warn` plus a two-second
periodic flush. The change is kept as hygiene and is not a fix for anything.

### The retained preserve path survives camera motion

Recorded because the opposite is written down earlier in this file and is now
stale. With the rebasing done in the runtime rather than the host, an orbit does
not collapse the preserve rate:

```
retained replay 4.84 ms/frame over 8308 draws; preserved 7918/frame,
  resubmitted 415/frame; misses: changed 383 dirty 9 ... noNode 0
```

against 7947 preserved / 373 resubmitted parked. The host registers absolute
transforms and the runtime rebases them, so a camera move no longer changes
every object's identity.

## Instance-set placements reported the camera's motion as their own

A grass batch and a tree batch keep their placements in absolute coordinates --
in the expanded vertices for native grass, in `instancesToObject` otherwise --
so the batch's outer object transform is nothing but the camera-origin
rebasing. It is a pure translation of `-origin`, and it changes by the origin
delta on every frame the camera moves although no blade has moved at all.

`processDrawCallState` copies the current object transform into
`prevObjectToWorld` before writing the new one. Passing the rebased transform
straight through therefore leaves the history exactly one frame of origin
behind, and every placement in all 22 batches -- 54,147 of them -- reports a
motion vector equal to the camera's own movement. The camera meanwhile reports
none, because `rebasePreviousFrames` has already moved its history into the new
origin. With ray reconstruction and RTXDI consuming those vectors, the result is
the world shimmering whenever the camera moves.

The retained replay rebases the history of the draws it owns for exactly this
reason. An instance set is not one of them: `csRemixDrawInstanceSet` and
`csRemixDrawGrassInstanceSet` go straight to `commitExternalGeometryToRT`, and
the replay's rebase loop only walks `m_retainedExternalDraws`. The frame log
confirms none of them are retained: `22 instanced batches (0 retained)`.

The fix writes the new translation into the existing instance's current
transform before `processDrawCallState` runs, so the copy into
`prevObjectToWorld` picks it up and the two agree. Only the translation can
differ, because the host writes an identity rotation for a placement batch.

**The obvious implementation hangs the game.** `RtInstance::rebaseBy` does the
same arithmetic and is the natural thing to reach for, but it also sets
`m_blasDirty`, which for an instance set means asking for a rebuild of every
batch every frame. The loading screen never finished: the process sat at
`Responding=False` with the log frozen and the CPU idle. Assigning the
translation directly does the whole job and touches nothing else.

### The instance-set motion-vector theory, tested and disproved

The reasoning above -- that an instance set's object transform is pure origin
rebasing, so its history lags by the origin delta and reports the camera's own
motion on every placement -- is sound on the face of it and wrong in fact.

`SceneManager` now records, per frame, how much motion the placements would have
reported had the origin change been passed straight through
(`m_instanceSetSpuriousMotion`), alongside how many instance-set draws it saw.
Across 120 frames spanning a 60-second camera orbit, with the origin confirmed
from `[RemixScene] origin` to follow the camera across roughly 4,000 units:

```
instance-set draws 2640, largest origin motion that would have been reported 0
```

2640 over 120 frames is 22 per frame, which matches `22 instanced batches`
exactly, so the measurement is reaching every batch. The existing instance
already carries this frame's translation by the time the draw is reached.

The first attempt at the "fix" also hung the loader, because `RtInstance::rebaseBy`
marks the BLAS dirty and for an instance set that means rebuilding every batch
every frame. Both the hang and the theory are recorded here so neither is
re-derived. **The cause of the shimmer under a moving camera remains unknown.**

## Where the frame actually goes — 2026-09-21

Baseline at the canonical Riverwood pose, default High preset with RTXDI:
**62.98 fps settled**, frame period 14.33 ms, fully accounted for as
`rest 8.84 + capture 3.91 + submit 0.96 + gather 0.42`.

### The GPU is not the bottleneck

Two independent sweeps say so, and they agree.

| case | fps |
| --- | --- |
| baseline | 69.3 |
| `pathMaxBounces=1` | 69.6 |
| `pathMaxBounces=0` | 69.3 |
| `neeCache.enableOnFirstBounce=False` | 70.0 |
| `volumetrics.enable=False` | 68.1 |
| `enableUnorderedResolveInIndirectRays=False` | 70.1 |

Path bounces from 2 to 0 changes nothing. Volumetrics off changes nothing. Then
the stronger test: launching with `RTX_QUALITY_DLSS_OVERRIDE=0`
(UltraPerformance, roughly a ninth of the traced pixels) moves 68 to **73.9**
fps and the frame period to 13.23 ms. Slashing the ray count to its floor is
worth about 8 per cent, so at most ~1.5 ms of the frame is GPU work that
quality settings can reach.

BLAS settings, by contrast, do move it -- `minimizeBlasMerging=True` costs 7 fps,
`minPrimsInDynamicBLAS=10000` gains 2 -- which points at the same place the
timings do.

### It is the DXVK CS thread

From `remix-dxvk.log`, per frame:

| CS-thread cost | ms |
| --- | --- |
| `prepareSceneData` | 5.81–6.07 |
| ⮡ `mergeInstancesIntoBlas` | 2.99 |
| ⮡ `buildBlases` (`uploadSurfaceData` 1.21) | 1.45 |
| ⮡ `surfaceMaterial writeGPUData` (13405 surfaces) | 0.73 |
| ⮡ `garbageCollection` | 0.60 |
| ⮡ `bindlessResourceManager` | 0.47 |
| `retained replay` (8313 draws, 7958 preserved) | 3.39 |
| `present` (`syncPresent` 3.1 + `syncFrameLatency` 3.8) | 6.90 |

That is ~9.2 ms of per-frame bookkeeping that does not scale with ray count,
which is exactly why the quality sweep is flat. `blas buckets dirty 6.37 of
15.58 per frame, frames with any dirty 120/120` is the shape of the problem:
41 per cent of buckets rebuild every frame.

### Two things tried on the replay, neither worth much

`RtxOption` accessors take a **global mutex** on every read (`getValue()` in
`rtx_option.h`). The preserve path read options roughly 24,000 times a frame:
`enablePreservePath()` and `isDrawcallTranslationInvalid()` once per retained
draw, `enableSeparateUnorderedApproximations()` once per preserved instance.
Both are now hoisted -- per frame for the replay guard, per frame cached in
`InstanceManager` for the billboard guard. Worth about 2 per cent, inside noise.

Splitting the replay cost showed why: of 3.39 ms, only **0.87 ms** is in the two
`preserveInstance` calls (SceneManager 0.69, InstanceManager 0.18 over 7937
calls). The other ~2.5 ms is the walk itself over 8313 entries whose
`ExternalDrawState` is several hundred bytes each. Removing a dead transform
write and mirroring `cameraType`/`hasParticleDesc` into the compact head of the
entry did not move the frame rate either.

### Effect captures went from 60 to 1375 per frame

`probeIneligibleReasons` is reset every frame, so `ineligible categories:
effect=1375` means 1375 effect geometries take the full capture path *every
frame*, against `effect=60` two days ago. Effects are ineligible for the
identity probe by design because their UV/colour/alpha animate, so this is a
23x increase in unavoidable per-frame work and a large share of the 3.9 ms host
capture. Cloud geometry is a big part of it and is not excluded from
`RemixScene::Capture` despite `RemixSky` submitting the sky separately.

### Frame generation is unreachable, and why

The largest single lever is not a lever yet. `[Perf]` reports `fg=off
method=FSR-FG mult=1` with `post-FG 0.0 fps`, and `frameGenMultiplier` is
already 2 in saved settings, so engaging it would take ~66 rendered frames to
roughly 130 presented. Three things blocked it, in order:

1. `Upscaling` was force-disabled in the Remix process by the feature gate in
   `State.cpp`. Remix owns world shading, but it does not own presentation --
   the swap-chain hook calls straight into
   `Upscaling::PresentWithFrameGeneration`, so frame generation is unreachable
   while the feature is unloaded. It is now allowed to load there; measured at
   66.1 fps against a 63-68 baseline, so it costs nothing.
2. The Remix DXVK fork was missing the presenter interop the host resolves by
   name. `dxvkGetPresenterSurfaceState`, `dxvkRequestSwapchainRecreate` and
   `dxvkSetSwapchainTornDownCallback` are now ported from the stock DXVK
   submodule into `src/vulkan/vulkan_presenter.cpp` and exported from
   `d3d11.def`. The host now logs `Observed presenter surface serial 2` and
   `Committed presenter surface serial 2 for render frames`, and registers its
   torn-down callback.
3. **Still blocking:** Streamline never binds to the Remix DXVK's Vulkan device.
   `[Streamline] feature support: DLSS=false Reflex=false DLSS-G=false FSR=false
   FSR-G=false XeSS=false (FSR-FG fns missing)`, alongside
   `'kFeatureDLSS' has not been initialized yet. Did you forget to create device,
   swap-chain and or call slSetD3DDevice/slSetVulkanInfo?`

   Streamline normally learns the device through its own `vkCreateDevice` and
   `vkCreateInstance` proxies, and under Remix it never sees them: the fork
   brings Vulkan up without going through the interposer. `slSetVulkanInfo` is
   the documented path for exactly that case -- "only call this API if NOT using
   the vkCreateDevice and vkCreateInstance proxies provided by SL" -- and
   `DXVKInterop` already held every handle it wants. Calling it in
   `Streamline::SetVulkanDevice` before the feature probe turns the whole roster
   on:

   ```
   [Streamline] slSetVulkanInfo result 0 (device 0x17c011400d0, queueFamily 0)
   [Streamline] feature support: DLSS=true Reflex=true DLSS-G=true FSR=true
     FSR-G=true XeSS=true (FSR-FG fns ok)
   ```

   `dxvkSetTearingPreference` is still missing from the fork, which leaves
   present-mode control inactive, but it is not required for frame generation.

### Result: 100 fps at 1080p, met

With frame generation engaged the method resolves to DLSS-G rather than FSR-FG.
Camera parked at the canonical Riverwood pose, output 1920x1080 confirmed from
`[Upscaling] Created upscaled texture (1920x1080, ...)`:

```
rendered 65.4 fps (15.28 ms) | post-FG 130.9 fps (7.64 ms) | fg=on method=DLSS-G
rendered 65.7 fps (15.22 ms) | post-FG 131.4 fps (7.61 ms)
rendered 66.7 fps (15.00 ms) | post-FG 133.3 fps (7.50 ms)
rendered 61.8 fps (16.19 ms) | post-FG 123.5 fps (8.09 ms)
rendered 55.7 fps (17.94 ms) | post-FG 111.5 fps (8.97 ms)
rendered 52.8 fps (18.96 ms) | post-FG 105.5 fps (9.48 ms)
```

Ten of twelve consecutive samples are above 100, median around 120. The two dips
(74.5 and 80.3) track the rendered rate falling to 37-40, not a frame-generation
stall.

**Note on measurement.** `MeasureRunningTest.ps1` samples the game's own frame
counter through DevBench `inspect`, so it counts rendered frames and cannot see
generated ones: it reports ~56 while the display is receiving ~120. Any
frame-generation measurement has to come from the `[Perf] post-FG` field, which
is instrumented at the present path. The harness number is not wrong, it is
answering a different question.

**Not persisted.** `frameGeneration` was enabled at runtime through the DevBench
feature tool; `SettingsUser.json` still has it false. Turning it on in the
Upscaling menu is what makes it stick.

## Remix's own DLSS-G on D3D11 — 2026-09-21

Remix ships a complete DLFG implementation, and on this game it was doing
nothing at all. Four separate things, each hiding the next.

**1. It reported enabled while producing nothing.** `isDLFGEnabled()` is
`supportsDLFG() && rtx.dlfg.enable && !hasDLFGFailed()`, and a *successful* DLFG
support check logs nothing, so silence was indistinguishable from absence. The
state is now logged once and whenever it changes:

```
[RTX.dlfg] supported=1 rtx.dlfg.enable=1 failed=0 maxInterpolatedFrames=1 reason=''
```

All three terms were already true.

**2. `DxvkDLFGPresenter` was only ever constructed in `d3d9_swapchain.cpp`.**
The D3D11 swapchain created a plain `vk::Presenter`, so `dispatchDLFG` ran every
frame, `setupFrameInterpolation` was called every frame, and the interpolated
image was computed and dropped. A present counter added to
`vk::Presenter::presentImage` said so plainly: `64.6 presents/s (0
interpolated/s)`. `CreatePresenter` now builds the DLFG presenter when
`m_context->isDLFGEnabled()`, mirroring the D3D9 path and using the dedicated
present queue family (Graphics 0, Present 2 on this adapter).

**3. Wiring it naively made things three times worse**, 65 to 23 presents/s.
`D3D11SwapChain::SynchronizePresent` pins `m_presentSlot = 0` and waits for the
previous present to complete before acquiring, to serialise the single-acquire
Vulkan presenter. `DxvkDLFGPresenter` owns its acquisition and presents from its
own thread, so that wait does not apply to it -- and with it in place every
rendered frame blocked on the interpolated present that followed it. Skipping it
for the DLFG presenter took 23 to 78.

**4. The swapchain image count.** D3D9 adds one image for DLFG; this path
acquires from the game thread and needed more. Raised to at least four. Note
`DxvkDLFGPresenter::recreateSwapChain` overrides the *real* swapchain to
`interpolatedFrameCount + 1` images and keeps `m_appRequestedImageCount`
backbuffers of its own, so this number sizes the latter.

### Where it lands, and what still limits it

| | rendered | presented |
| --- | --- | --- |
| no frame generation | 65 | 65 |
| Remix DLFG, as landed | ~38 | **~80** |

Doubling is exact, and `rtx.dlfg.enablePresentMetering=False` is worth about 5
presents/s over `True`. But the rendered rate falls from 65 to 38, and the
frame-time breakdown says where it goes:

```
present 16.83 ms/frame (acquire 16.78, syncPresent 0.00, syncFrameLatency 0.00)
```

The game thread spends 16.8 ms of a 25 ms rendered frame inside
`DxvkDLFGPresenter::acquireNextImage`, waiting on
`m_backbufferInFlight[index] == false`. With five backbuffers that is not image
starvation -- the present thread is genuinely five frames behind, so the DLFG
pipeline itself is the throughput limit. Interpolation runs on the present queue
while the graphics queue path-traces, and the two contend.

Raising the frame-latency depth by one under DLFG was tried and changed nothing
(80 either way), which is consistent with the stall being downstream of pacing.

**100 fps is not reached this way.** Presented is ~80. Reaching 100 needs the
rendered rate at 50, which means either halving the DLFG pipeline cost or
cutting the ~9 ms of CS-thread bookkeeping documented above.

**For comparison**, Community Shaders' own frame-generation path -- which needed
`slSetVulkanInfo` to bind Streamline to the Remix DXVK device, plus the presenter
interop exports -- measured 105-133 presented at the same pose. That path is
currently disabled by the Remix feature gate in `State.cpp` by request.

### What actually limits it, measured

With DLFG landed, the present thread is the bottleneck and its job takes exactly
as long as a rendered frame:

```
[RTX.dlfgjob] present-thread job 24.68 ms (mean of 120)
[RTX.present] 81.8 presents/s
```

24.7 ms per job, two presents per job, 12.2 ms per present. The swap chain is
`VK_PRESENT_MODE_IMMEDIATE_KHR` with 3 images, so this is not a display cap and
not image starvation -- widening the real swapchain from
`interpolatedFrameCount + 1` to `+ 2` moved it 80 to 82 and no further. It is
GPU work.

And the GPU work is immune to every quality setting there is:

| case | presents/s |
| --- | --- |
| baseline | 81.1 |
| `enableRayReconstruction=False` | 78.5 |
| `rtxdi.enableRayTracedBiasCorrection=False` | 78.9 |
| `useRTXDI=False` | 79.3 |

Turning RTXDI off entirely does nothing. Neither did path bounces 2 to 0,
volumetrics off, the NEE cache, or dropping DLSS to UltraPerformance (8 per
cent). The GPU is saturated by work that does not scale with rays, resolution or
sampling: the same per-frame geometry rebuild that costs 9.2 ms on the CS thread
costs GPU time in BLAS and TLAS builds. `blas buckets dirty 6.37 of 15.58 per
frame, frames with any dirty 120/120`.

**So 100 fps is a scene-churn problem, not a renderer-settings problem.** The
concrete lead is the effect capture: 1375 per frame against 60 two days ago, a
23x regression, and `RemixSky` resolves the sky into a 2048x1024 lat-long dome
while `RemixScene::Capture` separately imports the cloud quads as effect
geometry -- the sky appears to be represented twice. Establishing whether those
cloud meshes are redundant is the next thing worth doing, and it would cut host
capture and GPU build cost together.

### The present job waits on the GPU, not the display

Timing inside the job settles it. The swapchain acquire that happens *within*
the present job costs **0.0033 ms** (mean of 240) while the job as a whole takes
24.7 ms. The presentation engine hands images back instantly; the job is waiting
for GPU completion (`DxvkDLFGCommandListArray::nextCmdList` fences its command
lists) before it can submit the next one.

So the chain is: the GPU takes ~24 ms of work per rendered frame, the present
thread is paced by that, the game thread is paced by the present thread, and
presented lands at 2x rendered = ~80.

Things measured and ruled out as the GPU cost, each with no effect on presents/s:

- path bounces 2 → 0, volumetrics off, NEE cache on first bounce
- ray reconstruction off, RTXDI bias correction off, **RTXDI off entirely**
- DLSS at UltraPerformance -- a ninth of the traced pixels, 8 per cent
- grass wind deformation frozen, which stops every grass BLAS rebuilding each
  frame (81 vs 82: `CS_REMIX_FREEZE_GRASS=1` lever left in
  `rtx_geometry_utils.cpp`)
- swapchain present mode (already `IMMEDIATE`), image count, frame-latency depth

None of it is shading, sampling, resolution or grass. What is left is the
per-frame scene rebuild itself -- BLAS and TLAS construction and surface upload
for a scene that reports `blas buckets dirty 6.37 of 15.58 per frame, frames
with any dirty 120/120` and 13,405 surfaces. That is the same work that costs
9.2 ms on the CS thread, and it is the only remaining candidate on the GPU.

### Arming the identity probe for effects: measured, rejected

Effects are excluded from the probe because their UV, colour and alpha resolve
after the early-out, and a Riverwood exterior spends 1375 full captures a frame
on them. `[RemixScene.effectAnim]` showed the clouds reporting
`uvOffset=(0.00000, 0.00000)` frame after frame, which made them look like they
should sit still and take the cheap path.

`StaticProbe` was given an `effectState` hash over the whole of what the effect
capture reads -- both UV transforms, the base colour, the four falloff terms,
soft falloff depth, colour scale, property alpha, emittance, clamp mode and the
lighting-influence byte -- and the arming condition dropped `!effect`.

It does not pay. 1361 effects armed and **all 1361 reported changed every
frame**: the sky rotates, so a cloud's world transform moves even when its
material state does not, and `StaticProbe::world` catches that. Host capture went
3.9 to 4.45 ms for zero probe hits. Reverted; the static uvOffset was a true
observation that led to a false conclusion.

### The cost is per instance, and it scales

The decisive measurement, same build, same settings, two cells:

| scene | instances | presented |
| --- | --- | --- |
| Sleeping Giant Inn | 805 | **392 fps** |
| Riverwood exterior | 8,359 | **78 fps** |

A tenfold drop in instance count is a fivefold rise in frame rate. Converting to
rendered frame time (presented is exactly 2x rendered with DLFG):

- 805 instances -> 5.1 ms
- 8,359 instances -> 25.6 ms

That is **2.7 microseconds per instance per frame**, for instances that are 95
per cent preserved and unchanged. Nothing about shading, resolution, sampling or
grass moves it; instance count moves all of it.

100 fps presented needs a 20 ms rendered frame, which at 2.7 us/instance means
shedding roughly **2,000 of the 8,359 instances** -- or making a preserved
instance materially cheaper than 2.7 us.

The 2.7 us is spread across three places that all walk the instance list every
frame: `retained replay` (3.39 ms / 8313 = 0.41 us), `prepareSceneData`
(5.8 ms / 8350 = 0.70 us), and the GPU's TLAS build plus surface upload over
13,555 surfaces. None of those is a setting.

Two candidate directions, neither attempted:

1. **Submit fewer instances.** The ineligible tally says the bulk is not LOD
   (`distantTree=372`, `lodLandscape=73`, `nativeLandscape=100`) but ordinary
   static geometry -- 4,195 staggered plus 1,371 probe hits. Cutting it means a
   distance or size threshold on what reaches Remix at all.
2. **Make a preserved instance free.** It should cost close to nothing to say
   "this object did not change", and today it costs 0.41 us in the replay walk
   alone because the walk visits all 8,313 entries. A dirty list rather than a
   full walk would remove that, and the equivalent argument applies to
   `prepareSceneData`.

Note the merge knobs do not help: `minPrimsInDynamicBLAS` at 10,000 and 100,000
and `maxPrimsInMergedBLAS` at 5,000,000 all measured identically. Fewer, larger
BLASes is not the axis; the per-instance walk is.

### The exchange rate between objects and frame rate

A projected-size cull was added to `RemixScene::Capture` to test the scaling law
directly: retire any instance whose bounding sphere subtends less than a
threshold from the camera, so it leaves the retained replay, `prepareSceneData`
and the TLAS together. Size rather than distance, because a distance cut removes
a mountain and a twig at the same range and it is the twig that was costing the
same as the mountain for nothing.

`CS_REMIX_MIN_PROJECTED_SIZE`, radius/distance, zero disables it:

| threshold | instances | presented |
| --- | --- | --- |
| off | 8,345 | 78 |
| 0.004 | 8,044 | 85 |
| 0.015 | 6,670 | **105** |
| 0.03 | 4,784 | **132** |

The law holds and 100 fps is reachable this way -- 0.015 clears it. **It is also
not acceptable: at 0.015 the scene is visibly missing objects.** The user's
words, looking at it. The lever is left in, defaulting to off, because it
measures the exchange rate exactly: roughly 2.7 microseconds and 0.013 ms of
frame time per instance, whatever that instance is.

So the honest position is that the frame is not paying for detail, it is paying
for *count*, and the only acceptable way to spend less is to make a registered
instance cost less rather than to register fewer. That is the `retained replay`
walk (0.41 us/instance over 8,313 entries to preserve 7,958 of them) and
`prepareSceneData` (0.70 us/instance), neither of which should cost anything for
an object that did not change.

## 2026-09-21 — measured frame attribution (supersedes earlier inference)

Earlier entries in this file inferred where frame time went and were wrong. This
section is measured. Three instruments were added to make it measurable:

- `[RTX.render]` in `DxvkDLFGPresenter::presentImage` — the only site reached
  exactly once per *rendered* frame, so it separates render cost from generated
  cadence. The pre-existing `[RTX.present]` counter could not: it flags every
  present as DLFG-presented, which is why it reported `0 rendered/s`.
- `[RTX.blas]` in `rtx_accel_manager.cpp`, split by build mode.
- `CS_REMIX_FRAME_DEPTH` on `GetActualFrameLatency`.

### Methodology correction

Three-sample spot reads taken right after a runtime option change catch a
post-toggle transient. Measured over 90 s (n=45) the same configuration is
stable to 3-4%, but consecutive 3-sample reads of it ranged 68-91. Every A/B
delta under ~10% reported before this date was inside that noise and should not
be trusted. All numbers below are >=20-sample means with the sd quoted.

### The frame is not GPU-bound

At the canonical Riverwood pose, 1920x1080, default graphics preset:

| Configuration | Rendered | Frame | Presented | GPU busy |
| --- | --- | --- | --- | --- |
| DLFG on (x2) | 26.3/s (3%) | 38.0 ms | 52.6/s | 50-64% |
| DLFG off | 35.6/s (7%) | 28.1 ms | 35.6/s | 64%, 171 W |

The GPU is idle a third to a half of every frame in both cases. This is why no
path-tracing quality knob moved the frame, and it confirms the "GPU utilisation
is low" observation that earlier sessions failed to act on.

DLFG is worth keeping: it costs 10 ms of render time and returns +48% presented
(35.6 -> 52.6). It is not the 2x its name implies, but it is not a loss.

### DLSS scaling shows the cost is mostly fixed

| Profile | Frame | Presented |
| --- | --- | --- |
| MaxQuality (0.444 area) | 49.9 ms | 40.1/s |
| Balanced (0.336) | 42.6 ms | 47.0/s |
| MaxPerf (0.25) | 37.6 ms | 53.2/s |
| Auto | 38.0 ms | 52.6/s |
| UltraPerf (0.111) | 31.2 ms | 64.1/s |

Fitting cost = fixed + area x per-pixel across MaxQuality and UltraPerf gives
**~25 ms fixed, ~6 ms of pixels at UltraPerf**. A ninth of the pixels buys 37%.
The 100 fps target needs 20 ms, which is below the fixed cost alone.

### Acceleration structures are not the fixed cost

Per frame: **28 full builds totalling 3.2k primitives, 67 refits totalling
414k primitives**. Grass wind does force its batches to re-deform every frame
(the wind timer advances, so the hash never matches), but those already take the
cheap `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR` path. Full rebuilds are
negligible. This hypothesis is closed.

### Where the fixed cost actually is: per-frame CPU work over every instance

Measured per frame, DXVK side:

- `prepareSceneData` 8.2 ms, of which `mergeInstancesIntoBlas` **5.2 ms**
- retained replay **4.6 ms** over 8,773 draws
- garbage collection 0.73 ms, bindless 0.50 ms

Plugin side: `gather 0.5 + capture 4.5 + submit 1.0 = 6.0 ms`, with
`rest = 32.75 ms` of a 40 ms period — the game thread blocked.

The retained path is already doing its job: **8,364 of 8,797 instances are
preserved unchanged each frame and only 433 resubmit** (395 of those because
their transform genuinely changed). Yet `mergeInstancesIntoBlas` and the
retained replay both still walk all ~8,800 instances every frame. That O(all)
walk over a set that is 95% unchanged is the ~10 ms of stall, and it is the
reason cost tracks *visible instance count* rather than pixels or registration
count.

This also explains the rejected projected-size cull: it reached 105 fps by
shrinking the instance walk, not by saving GPU work — which is why it cost
visible objects for its gain.

### Ruled out by measurement (not inference)

- **Frame latency depth.** `CS_REMIX_FRAME_DEPTH=2` gives 26.3 vs 26.2
  rendered/s and leaves the GPU at ~55%. The older note on `GetActualFrameLatency`
  claiming 69 -> 86 fps from raising depth does not reproduce in the current
  configuration; depth is not the limiter and raising it buys nothing, so there
  is no judder trade to make here.
- **`rtx.enableRaytracing=False`** is not a usable A/B: it drops the whole
  present path, reporting 20.8/s. It measures nothing about path-tracing cost.
- BLAS merge knobs, grass wind freeze, effect probe eligibility, spdlog flush
  policy, instance-set motion vectors — all previously reported, all inside the
  noise floor established above.

### Next lever

Make the preserved-instance path O(changed) rather than O(all). ~95% of
instances are untouched each frame; both the 5.2 ms merge and the 4.6 ms replay
currently pay for them anyway. This is the only identified route to a large win
that does not remove objects from the scene.

## 2026-09-21 (later) — the serial stage is the DLFG present thread

### Between-launch variance is larger than within-run variance

A `CS_REMIX_FRAME_DEPTH=3` run measured 30.1 rendered/s where the baseline run
measured 27.0, which looked like an 11% win. Sweeping the same parameter
**within one launch** (as a runtime `rtx.csExtraFrameLatency` option) gives
26.7 / 25.9 / 26.6 / 26.3 for extra = 0 / 3 / 6 / 10 — flat. The apparent win was
between-launch drift. **A/B comparisons must be swept inside a single launch.**
Frame latency depth is conclusively not a lever, so the judder trade-off the old
`GetActualFrameLatency` comment describes does not need to be made.

### Where the game thread's frame actually goes

`[CSRemix.CPU] present` breaks the game thread's present down:

```
present 28.98 ms/frame (flush 0.0004, syncPresent 0.00002, acquire 28.89,
                        blit 0.028, onPresent 0.0003, submitPresent 0.009,
                        endFrame 0.00006, syncFrameLatency 0.0004)
```

**The game thread spends the entire present blocked in `acquireNextImage`.**
`DxvkDLFGPresenter::acquireNextImage` waits on `m_backbufferInFlight[idx]`, and a
backbuffer is only released by the scope guard at the *end* of the present-thread
job. One thread processes those jobs serially, so **job duration is the frame
rate**: `[RTX.dlfgjob]` tracks the frame period exactly.

Inside the job, `nextCmdList` is 0.010 ms. Everything else — the DLSS-G
interpolation submit and the two presents — is the remainder and is the serial
bottleneck.

### Also ruled out here

- **Vsync.** The swapchain is `VK_PRESENT_MODE_IMMEDIATE_KHR`. The display is
  59 Hz and presented sits at ~54/s, which is suspiciously close to it, but the
  mode is not FIFO, so 100 presented/s is not blocked by the display.
- **Present metering.** `rtx.dlfg.enablePresentMetering` on vs off, swept within
  one run: 26.9 vs 26.8 rendered/s. No effect.
- **Thread saturation.** No thread in SkyrimSE exceeds 37% of a core (busiest
  two: 36.7% and 27%), while the GPU sits at 40-65%. Nothing is saturated — the
  pipeline is serialised, not starved of capacity.

### Landed: bucket index moved onto RtInstance

`AccelManager::m_instanceBucketIndex` was an `unordered_map<RtInstance*, uint32_t>`
cleared and repopulated for ~8,400 instances every frame and then queried ~8,800
times in the `mergeInstancesIntoBlas` hot loop. It is now a generation-stamped
field pair on `RtInstance` (`setBucketCache` / `hasBucketCache`), so the lookup is
a field read on an object the loop already touches and the repopulate is a direct
write. The generation stamp means no bulk clear is ever needed.

Measured, same launch, means of 120 frames:

| | before | after |
| --- | --- | --- |
| `mergeInstancesIntoBlas` | 5.18 ms | 4.55 ms |
| merge cache populate | 0.444 ms | 0.070 ms |
| `prepareSceneData` | 8.22 ms | 7.55 ms |
| presented | 52.6/s | 54.1/s |

`RtInstance` grew 800 -> 816 bytes; the guarded size assert was updated.
`copyInstanceDataFrom` deliberately does not copy the stamp — a cloned instance
belongs to no cached bucket and must be reprocessed.

### Next step

Split the present-thread job's interpolate+present section further: the DLSS-G
interpolation submit, each `submitPresent`, and any fence wait between them. That
section is now the only unattributed part of the frame, and because the job is
serial its duration is the frame rate one-for-one.

### Landed: per-instance timing removed from the retained replay

`tryPreserveRetainedExternalDraw` made three `steady_clock::now()` calls per
preserved instance to report how the preserve time split between SceneManager and
InstanceManager. On Windows that is `QueryPerformanceCounter`, and the loop runs
for ~8,400 instances every frame, so the measurement cost more than the thing it
measured. It is now behind `CS_REMIX_PRESERVE_SPLIT`.

Measured, means of 120 frames: retained replay **4.57 -> 3.74 ms**,
`prepareSceneData` **7.55 -> 7.00 ms**.

An audit of the other 19 `steady_clock::now()` sites in `rtx_scene_manager.cpp`
found them all per-frame rather than per-instance; only this one was in a hot
loop. The plugin's `RemixScene.cpp` has none.

### Frame rate numbers in this section are not comparable across launches

The settled rate after this change read 49.8 presented against a 54.1 baseline
from the previous launch. That is the between-launch drift documented above, not
a regression: both CPU phase timings are means of 120 frames and moved
decisively in the right direction. **Only compare rates swept inside one launch;
compare code changes by their phase timings.**

### What reaching 100 fps would require

The model that now fits every measurement: ~12 ms of CPU submission serialised
with ~15 ms of GPU work per rendered frame (64% x 28 ms GPU-busy matches the
~15 ms). 100 presented at x2 DLFG needs a 20 ms rendered frame.

Gutting the path tracer does not get there — UltraPerformance DLSS plus
volumetrics, RTXDI, ReSTIR GI and secondary bounces all disabled reaches only
70.7 presented, and that is an unusable image. Of those, only secondary bounces
mattered (+13%); volumetrics and RTXDI were free. Ray bounce counts and Russian
roulette are inside the noise.

The remaining route is the CPU submission path, which is ~12 ms:
`mergeInstancesIntoBlas` 4.55 ms and retained replay 3.74 ms both still iterate
all ~8,800 instances every frame to service the ~400 that changed. `RtInstance`
is 816 bytes, so that walk is ~7 MB of pointer-chased reads per frame and is
dominated by cache misses rather than by the work done. Making it iterate a
maintained dirty list instead of the full set is the structural fix; the
complication is that the loop also builds `m_reorderedSurfaces`, whose ordering
has to stay stable across frames for the cached buckets to remain valid.

### Negative result: the instance walk is not the cost

Prefetching `instances[i + 8]` in the `mergeInstancesIntoBlas` loop measured
**4.48 ms against 4.55 ms without it** — no effect. That falsifies the
cache-miss explanation offered in the previous section: walking the ~8,400 clean
instances is cheap, and the 4.5 ms is the real per-instance work done for the
~400 dirty ones (`fillGeometryInfoFromBlasEntry`, bucket keying, OMM hashing).
The prefetch was reverted rather than left in as dead complexity.

**This means an O(changed) rewrite of the loop would not pay for itself.** The
recommendation in the previous section is withdrawn. Reducing the CPU submission
cost requires making the per-dirty-instance work cheaper, or reducing how many
instances go dirty per frame (~400 of 8,800, almost all from genuine transform
changes), not skipping the clean ones faster.

## Where this leaves 100 fps at 1080p

Measured ceiling on this machine (RTX 4080, 1920x1080, canonical Riverwood pose):

- **~54 presented/s** at default quality with DLFG.
- **~64 presented/s** with UltraPerformance DLSS, quality otherwise intact.
- **~70.7 presented/s** with UltraPerformance *and* volumetrics, RTXDI, ReSTIR GI
  and secondary bounces all disabled — an image that is not shippable.

100 presented needs a 20 ms rendered frame; the best measured is 31 ms. The gap
is not in any single subsystem that can be switched off: path tracing quality is
nearly free (volumetrics and RTXDI measured at zero cost), resolution accounts
for ~6 ms, and the CPU submission path is ~12 ms of which the addressable part
has now been measured and largely spent.

Two real reductions landed here (bucket index on the instance, per-instance
timing removed) totalling ~1.5 ms of CPU per frame. Everything else tried was
either inside the noise or a negative result, and all of it is recorded above so
it is not retried.

### Multi-frame generation is not available on this GPU

The shipped `nvngx_dlssg.dll` is 310.9.1, described as "NVIDIA DLSS-G **MFGLW**",
so the binary supports multi-frame generation. `rtx.dlfg.maxInterpolatedFrames`
defaults to 2, which would give x3 presented. Neither is the constraint:

`NGXFeatureContext` queries `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` and
the query **succeeds**, returning **1**. `dlfgInterpolatedFrameCount()` is
`min(option, thatValue)`, so the effective multiplier is x2 regardless of the
option. DLSS 4 multi-frame generation is Blackwell-only; on Ada (RTX 4080) x2 is
the hardware ceiling. Driver 32.0.16.1088.

This closes the last route to 100 presented that did not require a faster
rendered frame. At x2, 100 presented needs 50 rendered/s (20 ms); the best
measured rendered frame with quality intact is 31 ms.

## GPU phase attribution (the instrument that was missing)

`ScopedGpuProfileZone` routes to Tracy, which needs a connected profiler GUI and
cannot be driven headlessly, so the fork had no way to report GPU time to the
log. `RtxGpuPhaseTimer` in `rtx_context.cpp` now writes `vkCmdWriteTimestamp`
marks into a 4-frame ring and reads the oldest entry back, so it never stalls the
submitting thread. Enable with `CS_REMIX_GPU_PHASES=1`; it logs `[RTX.gpu]`.

Canonical Riverwood pose, 1920x1080, default quality, means of 120 frames:

| Phase | GPU ms |
| --- | --- |
| scenePrep (`prepareSceneData` GPU side) | 5.5 |
| pathTrace | 10.8 |
| volumetrics | 0.15 |
| NRC + RTXDI + ReSTIR GI | ~0.0001 |
| denoise | 0.04 |
| composite | 0.15 |
| upscale (DLSS) | 1.02 |
| **total instrumented** | **17.7** |

The rendered frame is 37.9 ms. DLFG accounts for ~10 ms (measured independently:
28.1 ms with DLFG off vs 38.0 ms on), and it runs on its own queue. That leaves
**~10 ms of bubble**, which matches the ~12 ms of CPU submission
(`prepareSceneData` 7.0 + retained replay 3.7 + GC/bindless 1.2) running
*serially* with GPU execution rather than overlapping it.

This finally explains the whole session's worth of null results. Volumetrics and
RTXDI measured free because they *are* free — 0.15 ms and 0.0001 ms. Denoise and
composite are noise. Ray bounce counts do not matter because path tracing is only
10.8 ms of a 37.9 ms frame. Nothing that can be switched off is large enough to
matter.

### What 100 fps would take

100 presented at x2 (the hardware ceiling, see above) needs a 20 ms rendered
frame. The arithmetic from the table:

- Closing the ~10 ms serialisation bubble: 37.9 -> ~28 ms.
- UltraPerformance DLSS on top: ~-7 ms, mostly out of pathTrace: -> ~21 ms.

That lands at roughly 95 presented, so the target is approachable but only by
pipelining CPU submission against GPU execution. That is an architectural change
to how the Remix host drives DXVK, not a setting. Note that the obvious levers
for it have already been measured and do **not** work: frame latency depth (flat
0-10), DLFG backbuffer count (4 vs 8, flat), present metering, and the pacer
semaphore.

### Also closed here

Freezing grass wind (`CS_REMIX_FREEZE_GRASS`) cuts BLAS refits from **414k to
85k primitives per frame** and the frame is **unchanged** (53.2 vs 53.4
presented). Acceleration-structure work is not the cost, measured directly this
time rather than inferred from build mode.

Disabling *all* present pacing — metering off and `kSkipPacerSemaphoreWait` true
together — leaves each present blocking 18.5/18.7 ms and throughput unchanged.
The per-present block is a real GPU wait, not deliberate spacing, which is why
`nvidia-smi`'s 40-65% utilisation figure should not be trusted here.

### The swapchain image count is not the limiter either

The DLFG presenter has two separate image counts and they are easy to confuse:
`presenterDesc.imageCount` sizes the **backbuffer ring** the game thread acquires
from, while `adjustedDesc.imageCount` (`dlfgInterpolatedFrameCount() + 2`) sizes
the **real VkSwapchain**. The earlier 4-vs-8 test raised only the former, so the
VkSwapchain stayed at 3 images while each job presents 2 — which looked like an
obvious stall.

Raising the VkSwapchain to 6 (`CS_REMIX_DLFG_SWAPCHAIN_IMAGES`, confirmed in the
presenter log as "Image count: 6") gives **52.3 presented against 52.7-53.4**.
No change; presents still block 17.4/17.8 ms.

### The present block is a closed loop, not a cause

Across every experiment, the time each present blocks comes out at almost exactly
half the frame period: 18.9 ms at 37.9 ms, 17.5 ms at 38.3 ms, 15.6 ms at 31 ms
under UltraPerformance. It tracks the frame rather than setting it, under every
combination of pacing off, metering off, deeper backbuffer ring, larger
VkSwapchain and deeper frame latency. Only reducing actual work moves it.

Anyone picking this up should not re-investigate the present path. The work is
16.4-17.7 ms of render GPU plus ~10 ms of DLFG inside a 38 ms frame; the gap is
CPU submission not overlapping GPU execution, and none of the exposed knobs
change that overlap.

## The frame is a pipeline bubble, and the bubble is closable

The GPU phase timer now also reports the gap between the end of one ring frame's
last mark and the start of the next frame's first mark. That is the decisive
number this investigation was missing:

```
scenePrep 5.3 | pathTrace 11.0 | volumetrics 0.14 | denoise 0.04 | composite 0.15
upscale 1.02 | busy 18.0 ms | idle between frames 21.1 ms   (frame 38.9 ms)
```

18.0 + 21.1 = 39.1, which is the frame. **The GPU does nothing for over half of
every frame.** A second run at default settings: busy 22.4 ms, idle 25.5 ms.
This is a 1-deep pipeline, not a GPU-bound frame, and it retires the "real GPU
wait" reading of the present block from the previous section — the presents wait
on semaphores, but what they are ultimately waiting for is the CPU.

The chain per frame, from the measurements:

1. Game thread flushes frame N and blocks in `acquireNextImage`.
2. CS thread runs `injectRTX` -> `prepareSceneData`: ~12 ms of CPU.
3. GPU renders: ~18 ms.
4. DLFG interpolates on its own queue: ~10 ms.
5. Only when the present job completes does the game thread's acquire return,
   and frame N+1's CPU work can begin.

The GPU is idle from the end of step 3 until step 2 runs again for N+1.

### The acquire block is removable — proven

Raising the DLFG **backbuffer ring** (`CS_REMIX_DLFG_IMAGES=8`) takes the game
thread's present from `acquire 29.3 ms` to **`acquire 0.000406 ms`**, and the
present-thread job from ~38 ms to 0.08 ms. The block genuinely disappears; the
game thread runs ahead.

That is the mechanism that would close the bubble. It is not yet a fix:

- Ring 8 **plus** `CS_REMIX_DLFG_SWAPCHAIN_IMAGES=6` **crashes** the game about
  ten seconds into rendering.
- In that run DLFG also reported `x1 presented` with
  `frameInterpolation.valid()` false, so interpolation had silently stopped
  before the crash.

Both levers are environment-gated and default off, so the shipped path is
unaffected; a default-settings run afterwards was stable at x2.

The two counts are distinct and conflating them is easy: `presenterDesc.imageCount`
sizes the backbuffer ring the game thread acquires from, `adjustedDesc.imageCount`
sizes the real VkSwapchain, and `DxvkDLFGPresenter::info()` returns the former.
Making the ring deeper while keeping interpolation valid and the swapchain
consistent is the work.

### The arithmetic, if the bubble closes

Frame becomes the GPU's own 18-22 ms rather than 38-48 ms:

- 18 ms rendered -> ~55 rendered/s -> **~110 presented at x2**.
- Even at the slower 22.4 ms sample: ~45 rendered/s -> ~90 presented.

So 100 fps at 1080p is reachable *and does not require removing objects or
lowering quality* — it requires the CPU submission for frame N+1 to overlap the
GPU execution of frame N. Everything else measured in this document is noise by
comparison.

### Correction: the acquire block was not shown to be removable

The previous section claimed that raising the DLFG backbuffer ring takes
`acquire` from 29.3 ms to 0.000406 ms and therefore proves the bubble can be
closed. **That reading is wrong and is retracted.**

In that run DLFG reported `x1 presented` with `frameInterpolation.valid()` false
— frame generation had already stopped before the process crashed. With no
interpolation there is no second present and the present-thread job collapses to
0.08 ms, so of course nothing waits on it. The acquire block disappeared because
the *work* disappeared, not because the pipeline got deeper.

Tested separately, and both stable:

- Ring alone (`CS_REMIX_DLFG_IMAGES=8`): `acquire` still 29.3 ms, 52.7 presented.
- VkSwapchain alone (`CS_REMIX_DLFG_SWAPCHAIN_IMAGES=6`): presents still block
  17.4/17.8 ms, 52.3 presented.

Neither helps. The combination is the one that crashed, and it is the one whose
numbers were misread. `m_blitCommandLists` was suspected as the out-of-bounds
index, but it is sized `kMaxFramesInFlight * (1 + kDLFGMaxInterpolatedFrames)` in
the constructor, which is large enough for a 6-image swapchain, so that is not
the fault either. The crash is undiagnosed.

**So the standing position is:** the GPU is measurably idle 21-25 ms of every
frame (that measurement is direct and holds), the frame is a 1-deep pipeline, and
no demonstrated way to deepen it exists yet. The arithmetic for what closing it
would buy still applies, but nothing here has closed it.

### Correction to the correction: that run crashed at the main menu

The `x1 presented` in the crashed run was not frame generation stopping. The
process started 13:22:27, logged DLFG init at 13:22:29, produced render lines
13:22:35-39, and died — before the save was ever loaded. `x1` is simply the main
menu, where DLFG has not engaged yet; every launch in this document shows `x1`
there before switching to `x2` once the world renders.

So the combination crashes **during load**, and nothing is known about how it
behaves in a rendered scene. `dispatchDLFG` early-returns on `!isDLFGEnabled()`
and otherwise always populates motion vectors and depth, so
`frameInterpolation.valid()` was never the issue.

### Why buffer depth was the wrong lever anyway

Neither count helps on its own, and the reason is structural rather than a
tuning failure. Deeper buffering lets the *game thread* run ahead, but the game
thread cannot produce frame N+1's scene capture until Skyrim has simulated frame
N+1, and Skyrim's frame is paced by `Present` returning. The dependency that
creates the idle gap is that the CS thread's ~12 ms of `prepareSceneData` for
frame N+1 cannot start until that capture exists, so the GPU has nothing queued
between finishing render N and receiving render N+1.

Closing the gap means overlapping scene preparation for one frame with GPU
execution of the previous one. No swapchain or backbuffer count expresses that;
it is a change to the order the host drives capture, submission and present in.

## The present thread is the serial consumer

Two bugs found and fixed while chasing the idle gap. Neither changed throughput,
but the first was corrupting every measurement of the acquire path.

### The present thread held its mutex across the whole job

`DxvkDLFGPresenter::runPresentThread` took `m_presentThread.mutex` at the top of
its loop and kept it for the entire ~38 ms job — interpolation and both presents.
`acquireNextImage` on the game thread needs that mutex only to read one bool, so
the game thread was blocked on the lock, not on the backbuffer.

It also meant a probe placed inside the lock could only run *after* the job had
finished and freed everything, which is why an earlier ring-occupancy probe
reported "in flight 0, queued 0, blocked 0%". With the lock released during the
work, the same probe reports the truth: **in flight 4, queued jobs 4, blocked
100% of acquires**, and the split is `take mutex 0.00003 ms | wait on flag
29.7 ms`.

The job now unlocks after taking the job off the queue and relocks in its scope
guard for the queue and flag updates. `synchronize()` waits on
`m_presentQueue.empty()` and the job pops only in that guard, so swapchain
recreation still waits for an in-flight job.

### RecreateSwapChain dropped the DLFG ring size

The backbuffer ring is sized in `CreatePresenter` only. `RecreateSwapChain`
recomputed `presenterDesc.imageCount = PickImageCount(BufferCount + 1)` with no
DLFG adjustment, so every swapchain recreation silently shrank the ring back to
the D3D11 buffer count. Both paths now go through `PickDlfgImageCount`.

### Ring depth is not the limiter

With the mutex fix in place and the ring actually applied:

| Ring | State | Presented |
| --- | --- | --- |
| 4 | in flight 4, queued 4, blocked 100% | 51.6 |
| 6 | in flight 6, queued 6, blocked 100% | 51.0 |
| 8 | **crashes ~6 s after launch** | — |

The game thread runs as many frames ahead as the ring allows and throughput does
not move. Each extra slot just adds a queued job. The present thread consumes one
job per ~39 ms, and each job is two `vkQueuePresentKHR` calls measured at
**18.8 ms each** — the driver call itself, not our code around it.

So the shape is: game thread free-running, six frames of work queued and
submitted, GPU busy 18 ms and idle 21 ms, and a present thread that retires one
frame per 39 ms. Why the GPU does not execute the queued frames ahead is the open
question; ring depth, swapchain image count, present metering, the pacer
semaphore, frame latency depth and Reflex mode are all measured and none of them
move it.

### Reflex

`rtx.reflexMode` swept within one run: LowLatency 52.8, None 53.3, Boost 53.1 —
throughput-neutral, as expected for a latency feature. Left at the `LowLatency`
default.

## Present path: fully exhausted

Everything below was measured with the present-thread mutex fix in place, so
none of it is subject to the serialisation that invalidated the earlier round.

| Change | Presented | vkQueuePresentKHR |
| --- | --- | --- |
| baseline | 51.6 | 18.8 ms |
| VkSwapchain images 3 -> 6 | 50.1 | 20.5 ms |
| backbuffer ring 4 -> 6 | 51.0 | — |
| backbuffer ring 4 -> 8 | **crash ~6 s** | — |
| exclusive fullscreen disallowed | 49.9 | 20.0 ms |
| present metering off | 51.7 | 19.3 ms |
| Reflex None / LowLatency / Boost | 53.3 / 52.8 / 53.1 | — |
| opacity micromaps off | 52.9 | — |

Facts established while doing it:

- Both swapchains already use `VK_PRESENT_MODE_IMMEDIATE_KHR`. Only the first
  had been checked before; the frame-generation one is IMMEDIATE too.
- The present queue is **family 2**, the graphics queue **family 0**, distinct
  `VkQueue` handles. Presents do not serialise against renders by sharing a queue.
- `vkQueuePresentKHR` costs 19-20 ms in every configuration and only moves when
  GPU work changes (15.6 ms under UltraPerformance). It is waiting on the frame's
  render to finish, not on the display.

## The shape of the frame, finally

```
game thread capture ~6 ms  ->  CS thread prepare ~13 ms  ->  GPU render ~18 ms  ->  present
```

Sum 37-38 ms, which is the frame. Perfectly serial. The backbuffer ring supplies
latency depth (it fills: in flight 6, queued 6, blocked 100%) but no throughput,
because each stage still waits on the previous frame's completion of the next
stage.

Pipelined, the frame would be `max(6, 13, 18)` = 18 ms -> ~55 rendered/s ->
**~110 presented**. That is the entire remaining win, and nothing exposed as a
setting, image count, queue, pacing mode or lock reaches it.

For 100 presented with perfect overlap the CPU also has to come in under 20 ms;
it is currently ~22 ms (capture 4.5, prepareSceneData 7.3, retained replay 3.7,
GC 0.73, bindless 0.50, plus the rest of the game thread's frame). So both halves
of the plan are needed, not either alone.

### Why the CPU is hard to cut further

`uploadSurfaceData` (1.32 ms) and the surface material `writeGPUData` loop
(0.79 ms) both walk all 13,898 surfaces every frame. They cannot skip unchanged
ones: animated materials read **live parameters through pointers into plugin
memory**, so the runtime has no revision to compare and the hash and index both
stay the same. Narrowing these needs the plugin to signal which materials it
mutated, not a runtime-side change.

Of what is left, `merge per-instance loop` is 1.35 ms for 8,800 instances of
which ~900 are processed and ~7,900 skipped clean, so the bucket cache is already
doing its job. GC 0.73 ms and bindless 0.50 ms are the only items with obvious
slack, worth ~1 ms together.

### Correction: the CPU is not the limiter

The "CPU ~22 ms + GPU ~18 ms serialised" model in the previous section is wrong.
Instrumenting `DxvkCsThread::threadFunc`'s wait for work directly:

```
[RTX.csthread] idle 67.5% of wall, queued chunks 1
```

**The command-stream thread is idle two thirds of the time with one chunk
queued.** It processes a frame in ~12 ms of a 38 ms period, queues the present
job, and then waits. It has ample spare capacity, so cutting `prepareSceneData`,
the retained replay or the surface packing would not shorten the frame at all —
the thread would simply idle longer. That retires the whole CPU-optimisation
plan as a route to frame rate.

The real chain is:

1. Present thread retires one job per ~38 ms.
2. Each job is two `vkQueuePresentKHR` calls at ~19 ms each.
3. Jobs hold backbuffer ring slots until they finish, so the ring stays full
   (in flight 5, queued 5, blocked 100%).
4. The game thread blocks in `acquireNextImage` waiting for a slot.
5. The CS thread starves behind the game thread.

Everything upstream is throttled by step 2. The GPU is idle 21 ms per frame for
the same reason, not because the CPU is late.

### What 19 ms per present most likely is

19 ms is 1.12 refresh intervals on this display, which runs at **59 Hz while
supporting 164 Hz**. Two presents per rendered frame at roughly one refresh each
gives a rendered ceiling of about 29.5/s; measured is 25-26, and presented
50-53 against a 59 Hz display is 85-90% of refresh. Every configuration in this
document lands there.

The one measurement that argues against it is the UltraPerformance sweep, which
reached 64 presented with presents at 15.6 ms — above 59 and below one refresh.
That has not been reconciled and was taken before the present-thread mutex fix.

Testing this needs the display set to its native refresh rate, which is a system
setting and the user's call. It is the only untested hypothesis left that would
explain a 19 ms present with the GPU 55% idle, the CS thread 68% idle, and no
queue, pacing, image-count or lock change moving it.

### The ring-8 crash is in the driver

Reproduced deterministically: ring 8 faults ~6 s after launch, during swapchain
setup. The stack is entirely `nvoglv64.dll` frames, entered from
`dxvk_d3d11.dll`. It is a driver-side limit on how many backbuffers the
frame-generation path accepts, not an out-of-bounds in our code —
`DxvkDLFGPresenter::getImage` correctly returns backbuffers and is sized by the
ring, and `m_blitCommandLists` is `kMaxFramesInFlight * (1 + kDLFGMaxInterpolatedFrames)`,
large enough.

It does not matter for performance: ring 6 is stable and measured 51.0 against
51.6 at ring 4. Ring depth is not the lever.

### Display pacing is also ruled out

52.6 presented on a 59 Hz display is neither 59 nor 29.5, so presents are not
quantised to refresh, and `VK_PRESENT_MODE_IMMEDIATE_KHR` does not sync to
vblank. The earlier suggestion to test at 164 Hz is withdrawn — it would not
explain an unquantised rate.

## Standing summary

Per rendered frame, all measured directly:

| | |
| --- | --- |
| GPU render (graphics queue) | ~18 ms |
| DLFG interpolation (DLFG queue) | ~10 ms |
| CS thread work | ~12 ms, **idle 68% of wall** |
| game thread work | ~6 ms, blocked ~30 ms in acquire |
| present-thread job | ~38 ms, two `vkQueuePresentKHR` at ~19 ms |
| **frame** | **~38 ms -> 26 rendered/s -> 52 presented** |

Nothing is saturated. The GPU is idle 21 ms, the CS thread 68%, no CPU thread
exceeds 37% of a core. The present thread retires one job per 38 ms and
everything upstream queues behind it, but the GPU work its presents wait on
totals 28 ms across two queues and has generally already been submitted.

Why two `vkQueuePresentKHR` calls cost 19 ms each under these conditions is not
resolved. Ruled out by measurement: present mode, exclusive fullscreen, present
metering, the CPU pacer, swapchain image count, backbuffer ring depth, frame
latency depth, Reflex mode, queue sharing (present is family 2, graphics family
0), the present-thread mutex, and display refresh quantisation.

Resolving it needs a GPU/CPU timeline capture (Nsight Systems, not installed on
this machine). Every inference-based approach in this document has been tried and
several produced wrong answers that later measurements retracted.

### Pacing and present queue: both closed

With the present-thread mutex fix in place, both pacing mechanisms disabled
together (`CS_REMIX_NO_PACING=1` plus `rtx.dlfg.enablePresentMetering=False`)
gives 52.0 and 50.4 presented with `vkQueuePresentKHR` at 19.0 and 19.3 ms.
Neither the driver's batch metering nor the CPU pacer is responsible.

The DLFG present queue is chosen in `DxvkAdapter` without a surface, so its
presentation support for the actual surface is never checked — a plausible cause
of a slow present. It cannot be swapped for the graphics family to test:
forcing presents onto family 0 takes the device out with
`VK_ERROR_DEVICE_LOST`, because frame generation requires its own out-of-band
present queue. The separate queue is by design and family 2 is correct.

That closes every hypothesis reachable without a GPU timeline capture.

## Nsight Systems: available, but needs elevation

Nsight Systems 2026.4.1 is installed at `I:\NSight\target-windows-x64\nsys.exe`
(not under `C:\Program Files\NVIDIA Corporation`, which is why an earlier check
here wrongly concluded it was missing). Nsight Graphics 2026.3.1 and RenderDoc
are installed too.

It cannot capture what is needed from an unelevated shell:

```
nsys launch --session-new=X --trace=vulkan,wddm <skse64_loader.exe>
Failed to register Vulkan extension JSON file(s).
This operation requires registry writing permissions.
```

Vulkan tracing registers a layer JSON under the registry, and WDDM queue tracing
and CPU context-switch tracing are ETW consumers; all three need Administrator.
A capture without them produces a report containing none of the relevant data
(no Vulkan API events, no WDDM context data).

Two notes for whoever runs it:

- `--trace` must be passed to `nsys launch`, not `nsys start`, in this version,
  and `osrt` is not a valid value there (`vulkan`, `wddm`, `dx11`, ... are).
- `nsys launch` does not set a working directory, so the loader must be started
  with the game root as the cwd or SKSE cannot find the game files.

Recipe, from an **elevated** shell, with the game root as the working directory:

```
nsys launch --session-new=rmx --trace=vulkan,wddm "<game>\skse64_loader.exe"
# load the save, then run tools/remix/ConfigureRunningTest.ps1 twice
nsys start --session=rmx --sample=none --cpuctxsw=process-tree -o capture -f true
# ~15 s
nsys stop --session=rmx
nsys stats capture.nsys-rep     # vulkan_api_sum, vulkan_marker_sum, wddm_queue_sum
```

What to look for: whether the graphics queue and the DLFG queue overlap across
frames, and what `vkQueuePresentKHR` is actually blocked on for its ~19 ms. Those
are the two questions every other measurement in this document has failed to
answer.

RenderDoc is the wrong tool for this: it serialises queues and generally disables
frame generation, so it cannot show either.

## The presents are display-paced after all

Waiting on the interpolation command list's fence before presenting splits the
job cleanly:

```
[RTX.fencesplit] interp GPU wait 1.39 ms
nextCmdList 0.010 | record+submit interp 1.69 | present#1 16.82 | remaining presents 18.55 | job 37.06
```

**The GPU work each present depends on is already finished — 1.39 ms — and the
present still blocks ~17-19 ms.** That retracts the conclusion two sections above
that `vkQueuePresentKHR` is a genuine GPU wait. It is not.

`present#1` at **16.82 ms** against a 59 Hz refresh interval of **16.95 ms** is a
1% match. Two presents per rendered frame at one refresh each gives a rendered
ceiling of 29.5/s and a presented ceiling of 59; measured is 25.8 rendered /
51.7 presented, 87% of it. Every configuration in this document lands in that
band, which is exactly what a display-paced present produces.

This also retracts the earlier argument that display pacing was ruled out because
52.6 is not quantised to 59. The rendered rate is capped at refresh/2 and
per-frame overhead puts it below the cap, so the presented rate is not expected to
land exactly on a refresh divisor.

`VK_PRESENT_MODE_IMMEDIATE_KHR` is selected and exclusive fullscreen makes no
difference, so the pacing is not coming from the swapchain present mode. DLSS-G
presents out-of-band on its own queue, and NVIDIA's frame-generation present path
paces to scanout regardless of the requested mode.

### Consequence

The display runs at **59 Hz and supports 164 Hz**. At 164 Hz one refresh is
6.1 ms, so two presents cost 12.2 ms and stop being the limit: the frame becomes
the GPU's own ~18 ms, which is ~55 rendered/s and **~110 presented**.

Unexplained: the UltraPerformance sweep reached 64 presented with presents at
15.6 ms, above a 59 Hz ceiling and below one refresh. That measurement predates
the present-thread mutex fix and has not been reproduced since.

### Tested at 164 Hz: display pacing is ruled out

The previous section's conclusion is wrong and is retracted. Requesting a 164 Hz
fullscreen mode (`CS_REMIX_REFRESH_HZ`, an ordinary DXGI mode request in the
app's own exclusive-fullscreen path, reverted on exit) put the display genuinely
at 164 Hz -- confirmed by `Win32_VideoController.CurrentRefreshRate = 164` -- and
changed nothing:

```
164 Hz: 25.2 rendered/s (39.6 ms) -> 50.5 presented
present#1 19.00 ms | remaining presents 19.22 ms | job 38.5 ms
```

One refresh at 164 Hz is 6.1 ms. A refresh-paced present would have dropped to
~6 ms. It stayed at 19. The earlier 16.82 ms vs 16.95 ms agreement was
coincidence, and so was the 59 Hz ceiling arithmetic built on it.

### What the present cost actually tracks

Across every configuration in this document the per-present time is almost
exactly half the frame period: 18.9 at 37.9, 17.5 at 38.3, 19.0 at 39.6, 15.6 at
31. It is not GPU work (the fence split shows 1.39 ms), not the display, not
Remix's metering or CPU pacer (both disabled together still give 19 ms), not the
swapchain image count, ring depth, present mode, fullscreen mode or queue.

That leaves NVIDIA's own frame-generation present pacing, which is applied inside
the driver for out-of-band presents and is not reachable from Remix's options.
It paces presents to the observed frame rate, which makes the measurement an
effect of the frame period rather than its cause -- and leaves the frame period
itself set by a feedback equilibrium rather than by any measurable work. The real
work in the frame is ~18 ms of GPU and ~12 ms of CS-thread CPU that is idle 68%
of the time.

## The nsys capture: what the present is actually doing

Captured with an elevated Nsight Systems (`nsys launch --trace=vulkan
--vulkan-gpu-workload=individual` + `start`/`stop`), 8-10 s of settled Riverwood.
Adding `wddm` to the trace list hangs report generation with the target still
running -- the session sits in `Generation` at zero CPU forever. Vulkan-only
generates fine.

### The present thread is the throughput limit

| call | count | avg | median | min | total |
| --- | --- | --- | --- | --- | --- |
| `vkQueuePresentKHR` | 539 | 18.59 ms | 17.13 | 14.62 | 10.02 s |
| `vkAcquireNextImageKHR` | 539 | 3.8 us | | | 2 ms |
| `vkQueueSubmit` | 3147 | 31 us | | | 98 ms |

All 539 presents are on one thread (`dxvk-dlfg-present`), back to back with
0.24 ms between them: a **98.7% duty cycle**. Two presents per job at 18.59 ms is
37.2 ms, which is the measured frame period. The frame rate is exactly the
present thread's rate, and nothing else.

`vkAcquireNextImageKHR` returning in **3.8 us** ends the swapchain-image theory
for good, and `vkQueueSubmit` at 31 us ends the queue-contention theory: submits
are not blocked behind the presenting thread.

### The block is a timed sleep, not a wait

Scheduler events for the present thread over the capture:

| state | time | share |
| --- | --- | --- |
| off-CPU `DelayExecution` | 9433.7 ms | **93.1%** |
| on-CPU | 691.8 ms | 6.8% |
| `Resource` / `UserRequest` / other | 11.1 ms | 0.1% |

`DelayExecution` is `NtDelayExecution` -- a timed sleep. Not a fence, not a
semaphore, not vblank, not a queue. Meanwhile `Main` is blocked 7.27 s of 10.15 s
and `dxvk-cs` 5.95 s, both on `AlertByThreadId`, waiting on the present thread.

### It is not waiting for the GPU either

Per-workload GPU tracing against the present ranges:

```
GPU busy (union)                 4626 ms = 56.7% of wall   (idle 43.3%)
GPU busy DURING each present     mean 10.96 ms, median 12.17   (present is 19.37 ms)
present_end minus last GPU_end   median 9.15 ms
```

The GPU finishes and the present keeps sleeping for a median **9.15 ms with the
GPU idle** before returning. Two presents per frame is ~18 ms of pure idle sleep
inside a 37.9 ms frame. Remove it and the frame is ~20 ms, which is 50 rendered
and ~100 presented -- the whole gap is this sleep.

### What it is not

Measured, not inferred, each against the phase timers:

| change | presented | present#1 |
| --- | --- | --- |
| baseline | 53.2 | 18.72 ms |
| `enablePresentMetering=False` | 50.0 | 20.59 ms |
| `CS_REMIX_NO_PACING` (pacer semaphore bypassed) | 52.4 | 18.25 ms |
| both pacing paths off together | 54.2 | 18.19 ms |
| + `reflexMode=0` | **56.3** | 16.91 ms |
| early backbuffer release | 54.6 | 17.53 ms |
| all of the above combined | 52.8 | 18.24 ms |

Remix's CPU pacer **spins** (`_mm_pause()`), so it cannot be the source of a
`DelayExecution` sleep. Driver metering off makes things worse, not better. The
swapchain is `VK_PRESENT_MODE_IMMEDIATE_KHR`, 3 images, exclusive fullscreen, on
a display genuinely at 164 Hz -- so a 17-18 ms present is not a refresh interval.
Reflex off is the only lever with a real effect, and it is worth ~3 presented
frames.

The remaining owner is NVIDIA's DLSS-G present path inside the driver, which
sleeps per out-of-band present and is not reachable from any Remix option.

### The redundant CPU gate (fixed, `CS_REMIX_EARLY_BB_RELEASE`)

The acquire side shows the renderer is completely gated:

```
[RTX.acquire] ring 5, in flight 5, queued jobs 5, blocked 100% of acquires
[RTX.acquiresplit] take mutex 0.00003 ms | wait on flag 26.8 ms
```

`m_backbufferInFlight[i]` was released only in the job's exit guard, after both
presents. But the backbuffer's last use is `blitRenderedFrame` into the swapchain
image, and that blit signals `m_backbufferAcquireSemaphores[i]` -- the very
semaphore the next writer of that backbuffer waits on (`sync.acquire`). The CPU
flag was therefore a second, much more conservative gate on top of a GPU ordering
guarantee that already existed.

Releasing the slot once the blit is submitted is correct and lets the game thread
run further ahead (queued jobs 5 -> 6). It is worth about +1.4 presented, which is
inside run-to-run noise, because in steady state throughput equals the consumer's
rate and the consumer is the present thread. It is kept behind
`CS_REMIX_EARLY_BB_RELEASE` rather than made default on that evidence.

## Trading quality for frame rate

The present sleep scales with frame time (frame settles at roughly 1.75x the
GPU's own work), so cutting GPU cost scales the whole frame down rather than
exposing a fixed overhead. Launching at DLSS UltraPerformance with
`CS_REMIX_EARLY_BB_RELEASE`:

```
UltraPerformance, clean launch: 34.9 rendered/s -> 69.9 presented
present#1 13.33 ms | remaining presents 13.61 ms | job 27.18 ms
```

**69.9 presented is the best measured all session**, and present dropping from
18.6 to 13.3 ms confirms the sleep tracks GPU cost rather than being a fixed
per-present latency.

Further quality cuts from there did not help:

| from the UltraPerformance baseline | presented | job |
| --- | --- | --- |
| clean launch | **69.9** | 27.18 ms |
| + reflex off, no secondary bounces, 1 bounce | 66.2 | 32.78 ms |
| + bounces reverted | 63.2 | 30.71 ms |
| + reflex restored | 64.1 | 32.16 ms |

Two things to note for future runs. Disabling `enableSecondaryBounces` makes the
frame *more* expensive, not less -- it evidently falls onto a slower path. And
`reflexMode=0`, which is worth +3 presented at Auto quality, is a loss at
UltraPerformance.

More importantly, **runtime option changes do not return to the clean-launch
number**: after toggling the settings above and reverting them, the same
configuration measures 64.1 instead of 69.9. Quality comparisons have to be made
with launch-time settings and a fresh process, not the config API.
`rtx.enableVolumetricLighting` is rejected by the config API entirely.

To reach 100 presented the frame must be 20 ms, which needs GPU work near 11 ms.
UltraPerformance already puts the internal resolution at roughly 640x360 and
still costs ~15 ms, so the path tracer itself is the remaining floor.

### TLAS refit: a real GPU win the present wall hides

The GPU pass breakdown at UltraPerformance shows where the remaining cost is, and
it is not the path tracer:

```
scenePrep 6.20 ms | volumetrics 0.12 | pathTrace 6.80 | nrc+rtxdi+restir ~0 | denoise 0.02
```

`scenePrep` -- acceleration-structure building -- is ~47% of GPU time and **does
not scale with resolution**, which is why dropping to UltraPerformance stopped
helping. `-TlasRefit` (refit the top-level structure instead of rebuilding it
every frame) cuts it by 35%:

```
scenePrep 3.71-4.36 ms (was 5.2-6.9)   =>  best presented 70.9
```

That is a genuine 2.2 ms of GPU time returned, and geometry renders correctly
with it on. It is worth keeping on its own merits.

It also refutes the "frame is 1.75x GPU work" model from the section above.
Taking 2.2 ms of GPU work out made the job time go **up** (27.18 -> 30.17 ms) and
the present go up with it (13.33 -> 14.81 ms). Present duration does not track
GPU cost reliably run to run; it wanders between about 13 and 19 ms whatever the
frame contains.

### The ceiling, stated exactly

Every measurement this session, across ~25 configurations, lands in one band:
per-present 13-19 ms, two serialised presents per rendered frame, so 26-38 ms per
frame and 52-77 presented. The best observed mean present is 13.3 ms and the
lowest single present in the nsys trace is 14.62 ms.

100 presented requires 50 jobs/s, which is a 20 ms job, which is **10 ms per
present**. That is below anything observed, and the two presents cannot be
overlapped: `vkQueuePresentKHR` to one swapchain must be externally synchronised
and ordered, so they are serial by specification, not by Remix's choice.

**With this driver's DLSS-G present path, ~77 presented is the hard ceiling at
any quality setting.** Reaching 100 needs the per-present sleep addressed in the
driver, which is why the 62 MB trace is the deliverable for NVIDIA rather than
another Remix change.

## The wall was the Reflex out-of-band markers

The "hard ceiling" section above is wrong and is retracted. The driver-side sleep
inside `vkQueuePresentKHR` is real, but it is not unconditional: the driver paces
a present that way only because Remix tells it the present is a frame-generation
present, through the Reflex out-of-band markers (`VK_OUT_OF_BAND_PRESENT_START/END`
around each present, `VK_OUT_OF_BAND_RENDERSUBMIT_*` around the interpolation
submit) and the device-level `NvLL_VK_NotifyOutOfBandQueue(...PRESENT)` on the
DLFG queue.

Every "Reflex off" measurement in this document left those markers firing.
`rtx.reflexMode` only changes the sleep mode passed to `NvLL_VK_SetSleepMode`;
`setMarker` fires whenever Reflex *initialised*, which it does at device creation
regardless of mode. The variable that mattered was never actually tested until
`CS_REMIX_NO_OOB_MARKERS` gated all five call sites in `rtx_reflex.cpp`.

### Result

Same scene, `-TlasRefit`, `CS_REMIX_EARLY_BB_RELEASE`, driver metering left on:

| | rendered | **presented** | present#1 | present#2 | job | acquires blocked |
| --- | --- | --- | --- | --- | --- | --- |
| markers on (all prior sections) | 26.6 | 53.2 | 18.72 ms | 19.00 ms | 37.99 ms | 100% |
| **markers off, Auto quality** | 53.5 | **107.0** | 5.64 ms | 9.09 ms | 14.96 ms | 13% |
| **markers off, UltraPerformance** | 74.7 | **149.4** | 2.98 ms | 5.89 ms | 9.11 ms | 0% |

At Auto quality the GPU is now the limiter (scenePrep 4.86 + pathTrace 10.47 ms),
which is where a path tracer should be limited. Run-to-run sd 4%.

Turning driver metering off on top of this is a **loss** (149.4 -> 125.9 at
UltraPerformance, acquires blocking again) because the CPU pacer spins a core.
With the markers gone the driver's metering spaces the second present about half
a job after the first, which is the pacing you want. Leave `enablePresentMetering`
at its default.

### What this costs

The markers exist so Reflex can account for frame-generation presents in its
latency pipeline. Skipping them does not change what is rendered; it removes
Reflex's latency optimisation around the DLFG presents. That is a latency-versus-
throughput trade, so it is opt-in (`-NoOobMarkers` on `LaunchTest.ps1`) rather
than a default change to a game install other sessions share. Frame-delivery
smoothness was not verified by eye in this run; the present#2 spacing suggests it
is fine, but a frametime capture is the way to confirm.

### Reading the earlier sections

The nsys capture, the fence split, the 164 Hz test, the acquire counters and the
early-backbuffer-release fix were all correct measurements; they located the
sleep and proved what it was *not*. The error was concluding the sleep was
unreachable from Remix because the option that was supposed to disable Reflex did
not touch the markers.
