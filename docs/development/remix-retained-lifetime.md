# Explicit retained-instance removal

The Skyrim importer registers scene objects with the retained external draw API.
Unlike a legacy game's missing draw call, an explicit removal is authoritative:
the instance must stop participating in rendering at the next scene build.

`SceneManager::removeRetainedExternalDraw` calls
`ReplacementInstance::releaseHost`. This drops ownership and clears the
node's prims. `clear()` marks mesh/light prims for garbage collection, removes graph
instances, detaches their back-pointers and invalidates cached bounds. The normal
scene collection before acceleration-structure build then retires these prims;
the unseen-draw age and anti-culling policy cannot extend their lifetime.

The tracker owns the node until normal collection. Retained nodes are not entered
in heuristic identity/spatial maps. No Vulkan allocation is freed directly by the
host API.

The dedicated `test_retained_retirement` test covers same-frame mesh retirement,
empty prim slots, ownership/bounds reset, repeated removal and reuse. It does not
exercise Vulkan, light/graph prims, scene transitions or output images.

Verified 2026-09-20: Release build passed; dedicated retirement/effect/refraction
tests passed 3/3. Eight console transitions between Riverwood, Sleeping Giant Inn
and Riverwood Trader passed load and settled plugin-membership checks. Above-inn
capture showed black background without exterior clouds/LOD. This does not verify
every transition frame or establish native-image parity.

## Scope of the interior investigation

The importer already excludes the global WorldRoot for interiors and refreshes
membership on a cell change. Actual exterior shader classes/features are checked
by `tools/remix/CheckInteriorMembership.ps1`. Do not reject arbitrary names:
`ObjectLODRoot` also parents legitimate indoor geometry, and indoor dust beams use
Cloud textures. The global temporary-node root remains a possible source to audit;
the inspected Trader scene had no geometry beneath it.

The removal bug is real, but is not yet proved to be the reported interior leak.
In particular, leaving-cell mesh destruction also calls the runtime's
`removeReplacementInstancesWithSpatialMapHash`, which already retires those nodes.
This fix matters when a retained instance is retired or reassociated without
destroying its mesh, including visibility/placement changes. Do not claim that a
passing settled membership check proves every transition frame is correct.

## Runtime ownership census — 2026-09-20

LaunchTest.ps1 -RetainedAudit sets CS_REMIX_AUDIT_RETAINED=1 on the test process.
SceneManager scans the live replacement-node, retained-registration and
ray-tracing-instance tables after garbage collection, before graph overrides
and acceleration-structure preparation. It checks reciprocal retained ownership,
valid primitive back-pointers and absence of GC-marked live instances. Unowned
instances are counted separately; those attached to a node without a current
draw are reported as stale. Each executed census emits a numbered log record.
No filtering, rendering or lifetime policy is changed. Disabled by default;
enabled census allocation/logging overhead makes this unsuitable for benchmarks.

CheckRuntimeOwnership.ps1 requires a minimum record count, detects malformed or
missing/out-of-order records and rejects any ownership violation or stale
unowned instance. The parser's two fixtures cover a clean log and a single bad
frame followed by a clean frame. These are parser tests, not runtime fault
injection. Dedicated retirement/material unit tests passed2/2 after rebuilding.

Diagnostic PID42164, runtime20260920-runtime-ownership, covered27,008 censuses
(runtime frames417..27594; menus/loading account for gaps). No ownership
violations or stale unowned instances. Eight console cell transitions passed;
host audit across them found2041 attempts/1219 interior, zero suspects and zero
camera-ready failures, with22 no-current-camera attempts. A subsequent native
entry door0x13424 returned true and reached the inn. Runtime settled counts
included371 in Trader,808/809 in inn, and up to8350 retained outdoor draws.
Ordinary exterior non-retained grass draws were current, not stale.

Full evidence: `.research/testlogs/20260920-runtime-ownership-full.log` and
`20260920-runtime-ownership-full-summary.json`; route/host reports have the same
runtime-ownership prefix. The diagnostic process was quit normally and the
game restarted without the audit to remove measurement overhead.

This is runtime CPU ownership evidence, not Vulkan buffer readback or final
pixels. It does not validate GPU culling, cached BLAS/TLAS contents, graph
overrides after the audit point, sky activation, or frame composition. The exact
intermittent interior clouds/LOD report is still not reproduced or fixed.

## Bulk invalidation and explicit identity — 2026-09-20

PID51248 crashed while initially loading the inn with CS first-person-visibility
build B25E9E1F and runtime ownership build C42DD380. Matching PDBs locate the
fault at `ReplacementInstance::clear`, reached from the old-node release in
`submitExternalDraw`. The node at 0x11f2f71c0a0 contains invalid/reused data,
including null prim storage and nonsensical ownership fields. The mini-dump
does not record the earlier allocation/free history.

One definite dangling-pointer path was found: `DrawCallTracker::clear` freed its
nodes without the destruction callback, while SceneManager kept registrations
and their raw node pointers. Bulk clear now notifies every retained owner before
destroying nodes. Individual mesh invalidation already used that callback.

A separate ownership hazard existed in dynamic replay: L1 identity and L2 spatial
matching could return another registration's node. Dynamic retained replay now
uses the registration's explicit node, allocating a distinct tracker-owned node
when absent. It never enters heuristic lookup maps. Key drift still refreshes
dirty flags; mesh changes retire old prims without changing registration identity.
Mesh invalidation and GC still visit these nodes in the tracker-owned vector.

The extended unit regression covers two identical independent registrations,
stable/moving updates, bulk destruction callbacks, repeated clear, recreation,
mesh-specific removal and release. `CheckSceneReset.ps1` temporarily disables
raytracing and restores it in finally. With RetainedAudit enabled, the runtime
logs actual nonempty bulk clears as retainedClear events. These tests do not
establish that either lifetime defect caused the reported exterior leak.

## Same-frame derived-instance collection — 2026-09-20

The first-person diagnostic run exposed three unowned GC-marked instances after
collection on each of two transition frames. InstanceManager scans a swap-erased
vector. Destroying a reference marks its persistent derived instances, but a
derived instance may occupy a slot already visited in that scan. A single forward
pass therefore left some marked copies until the next frame.

The real-manager regression reproduced this before the fix: ordering
source/view/player/survivor/virtual (0,1,3,4,2) left two instances where only the
unrelated survivor should remain. Negative log:
`.research/testlogs/20260920-clone-retirement-negative.txt` (exit -1).

Persistent-map removal now returns the earliest index it marks. Collection
continues at the minimum of that index and its current swap slot. This handles
source-to-view-to-virtual chains in the same collection call without introducing
an unconditional second scene scan. Each deletion still invalidates the scene
generation; renderer-created copies still skip unbalanced material callbacks.

The dedicated test passes all120 permutations of source, view copy, virtual copy,
player copy and unrelated survivor. It checks exact survivor identity/index,
empty persistent maps, balanced callbacks, generation invalidation and repeated
collection. Direct copy removal also preserves its source. Positive log:
20260920-clone-retirement-positive.txt; retirement/material tests passed2/2.

Existing acceleration code excludes renderer-created instances from persistent
bucket caching and skips GC-marked instances in a full merge. This inspection
limits the suspected stale-bucket path; it is not proof of correct output pixels.
The patch repairs authoritative same-frame CPU lifetime, not materials, shading,
camera projection or animation. In-game transition validation is recorded in the
handover; original failing logs remain evidence rather than being filtered out.
