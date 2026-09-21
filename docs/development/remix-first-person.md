# First-person camera and model space

## 2026-09-21 01:24 — native-phase first-person pose fix deployed

HostB0415D2B3D5F244FEC7C21974DAC96C051DCBDBC8FB548B359D39C15BFEAE61B,
build89940 exit0, backup20260921-native-viewmodel-pose preserves9776E361.
NormalPID34868 started01:19:54; runtime016AD3F5 unchanged.139 CPU/source tests
pass. CaptureViewModelPose runs at the verified native first-person camera hook,
freezes bone sources, rigid attachment local chains and fallback rigid worlds.
Submit reuses those values with the SAME translation used to restore the native
camera. Current-frame/native-root guard enforced; meshes discovered after the
hook wait for a native sample. Third-person entry snapshot is unchanged.

Two movement captures, full64-frame integrity and pose coverage:
- first-person-native-pose-fixed-1789950048,15533..15596
- first-person-native-pose-fixed-repeat-1789950096,17373..17436
Both hands and rigid axe have ZERO held rotations in both runs. Each hand has
63/63 native-phase transition coverage, also zero holds. Axe maximum
camera-relative origin step0.295376 and0.288850, versus6.330171 in the preceding
native-phase diagnostic run with three shared holds. Runs are not pixel-matched
or identical phases. Full first frame and both all-frame weapon sheets inspected;
axe position looks plausible, no numerical native silhouette/hand-grip parity.

CheckViewModel -Rounds1 passed12 camera/category ownership samples. Audit-off
first-person-native-pose-audit-off-1789950164,19945..20008 passes integrity and
weapon sheet inspected; route enters water and contains waterline/occlusion
artifacts, so do not use it as water or whole-image acceptance. No pose data in
that run. Original Save1 restored, first person/axe drawn, freeCamfalse,
auditfalse, normal Remix scene. All jobs terminal; no crash observed.

This fixes the measured late-sampling holds in these tests; all first-person
judder, display pacing, attacks, swimming, arbitrary equipment and native parity
remain unqualified. User confirmation pending; full goal ACTIVE/incomplete.

## 2026-09-21 01:18 — user confirms remaining axe judder; native-phase cause

First-person animation remains OPEN. The rigid attachment fix alone is not
sufficient: user still sees judder, and repeat capture confirms held hand/axe
poses. Eyes remain parked; do not mistake an improved first run for acceptance.

Host84BE1562CE7C7F20E152328DB45841D8C46FD0573A36CE0220693237122F8FF0
reconstructed rigid view-model attachments from the shared bone snapshot plus
local ancestor chain (NPC R Hand). Candidate run
first-person-attachment-candidate-1789948927 had64-frame coverage: old maximum
camera-relative axe step9.25054 versus candidate0.298996 units. Stationary
candidate-1789949019 difference median0.0331/max0.1368, unlike walking median6.05.
Build40729 exit0, backup20260921-rigid-viewmodel-snapshot,137 tests pass.
First fixed capture first-person-attachment-fixed-1789949255,1681..1744:
zero held hand/axe rotations, axe relative maximum0.295801. Repeat
first-person-attachment-fixed-repeat-1789949481,10079..10142: integrity passes,
but axe has11 held rotations, shared with hand holds; relative max3.08279.
Camera/location changed between runs; NOT matched A/B. Both weapon sheets
inspected. POV ownership check passed12 samples, not animation acceptance.

Diagnostic host9776E361D915CDF0718466C73E57CF6988AC909974000149CAC9E98E4C038BF8,
build12846 exit0, backup20260921-native-viewmodel-pose-audit preserves84BE1562.
Normal PID39552 started01:14:58, runtime016AD3F5 unchanged. Opt-in native camera
hook records copied bone rotations, frame-tagged, before later submission.
first-person-native-phase-1789949781:64 captured frames/full pose coverage.
Both hands have native coverage63/63 transitions, ZERO native held rotations,
but submitted hands AND axe hold54380,54389,54393. Native hand rotation changes
on those frames range0.00439..0.01770 while submitted change is0. Thus there is
direct evidence of a later pose-sampling defect, not merely display pacing.

Next candidate freezes first-person bones and local-chain attachments at the
native camera hook and translates both by the EXACT same restoration offset
as that camera. Current-frame/root camera guards remain. Newly discovered
view-model meshes without a native snapshot wait until next frame rather than
mixing phases. No interpolation, movement thresholds or lighting changes.
Candidate build/validation pending; do not claim deployed until confirmed.

## 2026-09-21 00:57 — rigid weapon cadence remains

After entry-bone snapshot fix, first-person walking still has an independently
measurable rigid-weapon hold. No production first-person fix this turn.
CapturePlayerMovement supports -Pov first (default third). Initial audit-off
first-person-entry-bones-1789948235, PID49876 frames13050..13113, includes weapon
draw animation; integrity passes, full first frame and all-frame weapon sheet
inspected. Not a settled locomotion comparison.

Expanded opt-in audit to union playerBody and viewModelGeometry, tagging each
pose viewModel and recording submitted first-person eye/camera validity. Records
view-model entry world/rotation before discovery to compare with submitted
transform. Analyzer handles rigid geometry and uses the appropriate camera;
group output by geometry identity, NOT name (world and view-model axes share
WarAxe:0). Body/hand bone sampling production path unchanged.
Build73489 exit0; host1A546792F1E4AA83AD5DFED5C71A40FE28798ABF158295369D5EE4D0394E4082
deployed with backup20260921-viewmodel-pose-audit preserving4506FB59.136 tests
pass. Normal PID47036 started00:54:03, runtime016AD3F5 unchanged.

Settled drawn-axe forward run first-person-pose-audit-1789948494,1296..1359,
64frame integrity/full pose coverage, confirmed locomotion. Weapon sheet inspected.
Two submitted skinned hand meshes have ZERO repeated rotations; maximum
camera-relative bind-origin steps1.218/.304units. First-person rigid axe
geometry1719523523840 repeats both translation and rotation at1310, while
camera moves3.932units. At1311 weapon catches up11.548units (camera-relative
5.904). Maximum camera-relative weapon step6.595units. These are host transforms,
not vertex or display timing proof. Entry-to-submitted weapon position changes
by up to4.683units in the run; skinned object roots by4.589, cancelled in skinning
through the inverse-world palette. The rigid axe has no such cancellation.

Source lead: Capture still initializes rigid instance.world from geometry->world
after scene discovery/material work. First-person axe ancestry is WarAxe:0 ->
Weapon(00013790) -> WEAPON -> NPC R Hand -> forearm/... . NiSkinInstance has
bone-node pointers and separate boneWorldTransforms; flattened skeletons can
store matrices separately from node.world. Potential next fix is derive rigid
attachment from the SAME sampled skeleton pose plus local attachment chain,
not independently timed cached world. Verify anchor mappings and transform
composition against good frames before claiming this fixes the observed hold.
Merely sampling node.world earlier may still preserve a stale cached pose.

Save1 restored after tests; normal rendering, audit off. First-person smoothness,
hand/weapon alignment, native silhouette/depth parity remain OPEN.

User validation2026-09-20: first-person model is still buggy while moving and
animating; third-person character animation is also jittery. BOTH remain open.
The camera/ownership checks below do not establish visual motion correctness.

Exact-build audit: Skyrim 1.7.99, image SHA256
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`.
Read-only decompilation and instructions are archived in
`.research/first-person-native-audit.json` and `first-person-camera-audit.json`.

Main::Draw conditionally renders first-person geometry after the world pass.
RVA1537200 shifts the first-person scene recursively using153abe0, which adds
the same translation to every node's world transform.1537450 reverses it after
rendering. The separate camera pointer is at3436220. Its accumulator render
function1514fd0 calls SetCameraData at1514ff7 (target101c400).

The plugin verifies this call before patching. Its hook invokes the original,
then snapshots camera data, origin and first-person root translation only when
the supplied camera matches that native pointer. WorldFrame clears the snapshot;
the getter rejects another frame or a replaced player root. The world camera's
existing independent hook at656fd4 is unchanged.

At import time the model and bone transforms have been restored. The camera
origin is therefore adjusted by `restoredRoot - capturedRoot`. For a row-vector
view matrix V, native vertex p, captured origin o, and restored displacement d:

`(p - o) V = (p + d) [T(-(o + d)) V]`.

This reuses RestoreWorldViewTranslation; no guessed player-height or camera
offset is needed. The projection retains the native first-person FOV and depth
terms, with Skyrim's two TAA jitter terms removed as for the world camera.
Meshes use Remix's VIEW_MODEL category and separate VIEW_MODEL camera. Remix's
view-model support is enabled with scale1; its own perspective correction maps
them into the traced world-camera projection. World geometry remains unchanged.

First-person geometry is imported only while first-person POV and its current
native camera are both valid. Otherwise its retained registrations are explicitly
retired. Rigid weapons bypass the staggered static probe because they follow the
camera. `Inspect(remixScene,camera).viewModel` exposes camera validity, counts and
restored eye position. Runtime and native visual parity still require validation.

## Initial validation, 2026-09-20

Optimized CS build C645A2C68D8A9A3383643C47E02CA8FBDD5648EDC48A517800D9718448ED5093
rendered the first-person axe in the inn and Riverwood. Runtime logged external
camera type1, near5 (world near15). World and restored view-model eye positions
agreed within float precision at both indoor and outdoor coordinates.
CheckWorldViewMath passed1080 coordinate comparisons, max error0.02411 game
units; this is a matrix test, not a renderer test.

PID49580 passed CheckViewModel twice (24 samples each in inn and Riverwood):
first-person camera valid with4 submitted meshes, third-person camera invalid
with0 view-model registrations. Native exit0x13419 and entry0x13424 activation
succeeded. Archived reports:20260920-viewmodel-{inn,riverwood}.json. Separate
parked world-camera tests on PID16036 passed first12 and third8 samples.

Inspected captures:042627-first-person-initial.png (Remix inn),
042748-first-person-sheathed.png (weapon no longer visible),
043258-first-person-native-reference.png (separate native-rendering process),
043436-first-person-riverwood.png. Axe projection/placement is plausible but
the native and Remix captures have different idle-animation phases. No exact
silhouette or depth parity is claimed. Remix axe shading is much darker.

Runtime inspection found WarAxeBloodLighting submitted despite its own
AppCulled flag. First-person capture now honors the geometry's own AppCulled
flag to hide these authored overlays; world scene culling policy is unchanged.
The visibility correction was validated separately below.

Final visibility build B25E9E1F with runtime retained-identity4DC4243E passed
24 inn POV samples on PID51112. First-person submitted3 meshes (instead of4),
including the axe but excluding its AppCulled WarAxeBloodLighting overlay.
Other actors' similarly named world overlays were unchanged. Inspected capture
045214-first-person-final-axe.png confirms axe/HUD. Temporarily unequipping the
iron axe0x13790 and drawing fists produced2 view-model meshes; inspected capture
045249-first-person-bare-hands.png confirms both hands. Axe was re-equipped in
finally. Hands and axe remain very dark; appearance is not qualified.

Three deliberate runtime scene resets subsequently recovered in the same PID.
On later first-person cell transitions the ownership audit recorded3 unowned
GC-marked instances on each of2 frames (11535,13715), with no bad ownership
pointers. Single-pass InstanceManager GC can mark a derived clone after that
clone's vector position has already been visited. The full BLAS merge skips
marked instances, but cached-bucket behavior and that residual lifetime need
further validation. Do not label the full first-person ownership audit clean.

Follow-up runtime861C8158 (clone-retirement) repairs this collector ordering.
The real-manager regression fails before the fix and passes all120 source/copy
orderings afterward, plus direct-copy and empty-GC cases. Existing persistent
bucket caching already excludes renderer-created copies. Diagnostic PID32920
then passed24 POV samples, native inn entry/exit and8 first-person console cell
transitions;8624 runtime censuses had no GC-marked or ownership violations.
All8 route results submitted3 first-person meshes. Evidence is archived under
20260920-clone-retirement in testlogs. This supersedes the pending CPU-lifetime
finding, not the older failing evidence or unqualified appearance/depth parity.

Harness caveat: Skyrim's existing idle-vanity countdown overrides forced POVs
after prolonged idle. Increasing fAutoVanityModeDelay:Camera through Utility
did not reset the existing countdown; its original120 value was restored in
finally. Computer-use movement and Tab input did not reliably leave vanity.
Fresh sessions completed the tests before that countdown expired. Earlier
attempts failed on actual POV changes, not on a missing model assertion. The
first exterior visit also raises the Survival prompt, which must be dismissed
before testing. CheckViewModel now rejects modal/loading start states.
