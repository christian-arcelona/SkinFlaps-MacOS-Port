# Porting notes

How the Windows build of SkinFlaps maps onto macOS, and the handful of
lessons for future maintainers.

## Backend mapping

| Windows | macOS | Why |
|---|---|---|
| Intel MKL BLAS/LAPACK | Apple Accelerate | Native, AMX-accelerated dense kernels; fastest FP64 dense Cholesky on Apple silicon. |
| MKL PARDISO (`iparm[35]` native Schur mode) | SuiteSparse CHOLMOD + explicit CPU Schur complement | CHOLMOD has no native Schur mode, so the port factors A11 sparsely and forms the dense Schur block itself (see below). |
| Intel AVX intrinsics | SIMDe (AVX→NEON translation) | Header-only translation keeps vector semantics and reduction order matching the Windows build. |
| CUDA collision solver | CPU Schur block-eliminate | No CUDA on Apple silicon; the CPU path mirrors the algebra of the non-CUDA fallback. |
| oneTBB | oneTBB | Unchanged; preserves the original task decomposition. |
| GLFW + OpenGL | GLFW + OpenGL | Unchanged apart from GLSL `#version 130` → `150` for the macOS core profile. |

## The overlay mechanism

`SkinFlaps/src/` is byte-identical to the Windows tree (pinned at
`vendor/original_windows/SkinFlaps`), and so are the `gl3wGraphics/`
sources (its `CMakeLists.txt` excepted — build files are port-owned).
Every macOS-specific source change lives in `overlays/`, and the top-level
`CMakeLists.txt` substitutes those files onto the `SkinFlaps` and
`gl3wGraphics` targets at configure time via
`set_target_properties(... SOURCES ...)`. Keeping the Windows tree pristine
means its updates merge cleanly and the entire port is reviewable in
one directory. `diff -rq SkinFlaps/src vendor/original_windows/SkinFlaps/SkinFlaps/src`
must always produce no output, and the same diff over `gl3wGraphics/`
may differ only in `CMakeLists.txt`.

## Input handling on macOS

The GLFW callbacks in `overlays/skinflaps_src/FacialFlapsGui.h` adapt the
Windows input contract to Mac hardware without changing what any gesture
means:

- **Camera pan.** The Windows build pans exclusively with a middle-button drag.
  Apple trackpads and Magic Mice emit no middle button, so Shift- or
  Ctrl-left-drag pans as well (the mode latches at press; plain left-drag
  still rotates). A physical middle button pans exactly as on Windows. The
  surgical Ctrl/Shift latch that drives T-in/T-out, undermine connection,
  deep-cut open ends, and automatic suture lines is untouched.
- **Surgical modifier latch.** The latch is fed from the key callback,
  which returns before touching it while ImGui holds the keyboard
  (the frames around a toolbox click) or while another window is key, and a
  focus change synthesizes a plain release that clears it — so a Shift held
  through a suture could land as a single. The right press is the very event
  the tools sample, and the OS reports the modifier state on it: the latch is
  set at a right press with Shift/Ctrl held and cleared at a right release
  without them.
- **Key events to ImGui.** The key callback is ImGui's only key source, and
  feeding every event, releases included, as a press leaves a key used in a
  text field held down there: one Backspace in the save dialog would erase
  the whole name. The callback feeds the real press/release state and the
  modifier keys.
- **Delete key.** The primary Delete key on Mac keyboards reports
  `GLFW_KEY_BACKSPACE`, which the tools do not act on; only fn+Delete reports
  `GLFW_KEY_DELETE`. The key callback maps Backspace onto Delete (below
  the ImGui capture branch, so text fields still receive Backspace). Delete
  keeps its clinical meaning for a selected hook or suture; a fence post or
  an undermine/periosteal mark is a forward move that Cmd+Z steps back, and a
  Delete press in those tools says so.
- **HiDPI.** Window and framebuffer coordinates differ on Retina
  displays; all pick and camera routing converts through the framebuffer
  scale, with pick/mouse APIs widened from `unsigned short` to `int` in
  the `overlays/gl3wGraphics/` copies.
- **Automatic suture lines.** Two things kept the Shift/Ctrl suture
  feature from working on macOS, both handled in the overlay: the
  `sutureTets` constructor leaves `_constraintId` uninitialized (a rollback
  then passes an unset id to the physics), and the interactive path ran
  `laySutureLine` on a worker thread — the routine creates suture
  graphics, and OpenGL is main-thread-only on macOS, so the shader-uniform
  lookup faulted. The interactive path now runs the call synchronously on
  the main thread, as the history-replay path always has.
- **Fence wall scaling.** Re-aiming a post rebuilds its wall points from the
  unit normal alone, a fixed 0.75 model units; `addPost` scales them by the
  fence size. On the face model the two are within 15% and the difference
  passes unnoticed; on the cleft model (fence size ~0.05) the wall comes out
  eight times too tall. The overlay scales both the same way.
- **Incision line drawing.** `elementArraySize` is an element count for
  `glDrawElements` but held the byte size, so every draw read
  four times past the index buffer — invisible while the buffer only grows,
  visible as leftover fragments once an undo shrinks it. The incision lines
  are also refreshed when an undo removes every incision.

## Undo, failure handling and sessions

- **Whole-state undo and redo (Cmd+Z / Cmd+Shift+Z).** Every forward move —
  a post placed or re-aimed, an undermine or periosteal mark, a hook placed,
  grabbed, moved or removed, a suture placed or removed, a knife, undermine,
  deep-cut or periosteal commit, an excise — first snapshots the whole
  surgical state: the surface mesh, the lattice, the cutter's incision state
  and its static deep-bed map, the hooks, the sutures, the fence posts, the
  pending marks, the selection and tool, and the history array with its
  replay position. Why whole-state rather than per-action inverses: the
  solver is quasi-static, so the positions plus the constraints are the
  complete dynamic state — there is no velocity to carry — and restoring
  them and re-initializing the physics reproduces the earlier state exactly.
  A forward move clears the redo list; the Edit menu steps back or forward
  several moves at once. A refusal that never changed anything takes no undo
  step.
- **The error hold.** An action that throws, or a topology or physics rebuild
  that fails on the task thread, no longer ends the session. The reason is
  shown under "Action failed - Close reverts", the view stays, and Close
  restores the snapshot taken before the move. A hook drag or suture still in
  flight is recorded into the history first, so the failing action is in the
  saved files. When no snapshot can be restored, the original save-and-exit
  flow ("Program exception thrown") runs as before.
- **Versioned error histories.** Each hold writes the usual `ERROR.hst`
  plus `ERROR_<stamp>_<n>.hst` (the actions still in effect) and
  `ERROR_<stamp>_<n>_full.hst`, the full trail: every action committed this
  session with an "undo", "redo" or "revert" record wherever a restore
  happened, so the trail replays the session including its undos (a
  `noRecord` flag marks the undo of a move that never reached the history —
  a post, a mark, a grab — and a replay skips it). Beside every saved history
  a `<name>.clock.json` sidecar records the wall-clock time and the physics
  solves of each state change, so a replay can be paced like the session;
  the sidecar beside a plain history indexes that history's records, not the
  trail's. The app log `skinflaps_app_<stamp>.log` is kept only when an
  error history was written. This is what a bug report needs to be
  reproduced.
- **History-load feedback.** Loading a second history (or loading after
  any surgical action has been recorded) is refused by the loader, and the
  refusal went unreported; the overlay surfaces it, changes
  the window title only on a successful load, and reports an unreadable
  history file.
- **Failure paths behind the callbacks.** A throw escaping a GLFW callback
  must unwind through Cocoa's event dispatch to reach the main-loop
  handlers, a property of the platform that is not guaranteed; the surgical
  dispatch sites catch at the callback boundary and route to the same
  handler. A physics solve that produces a non-finite right-hand side is
  reported once, into the error hold when the forward move's snapshot is the
  finite state before it, instead of re-arming every frame. History
  recording checks each attach-point lookup before mutating state, so a
  failed lookup is a message rather than a half-recorded action.
- **One session per process.** The scene loader is written for one
  model per process: it appends static objects, collision sets and tet
  subsets to what is already loaded, and the cutter, the deep-bed map and
  the fence size are process-wide. A fresh slate is therefore a fresh
  process: File > New window, File > New session..., and a model or history
  load asked for while a session is active confirm ("Start a new session?")
  and spawn this same executable, which reads `SKINFLAPS_START_HISTORY` /
  `SKINFLAPS_START_MODEL` (an internal parent-to-child handoff) to open what
  was chosen; all but New window then close the current window. Undo, redo,
  the hold and the physics thread begin clean in the child.
- **User data folder.** The installed bundle's Resources are root-owned, so
  histories, error files and logs go to `~/SkinFlaps/History`, created on
  first use and seeded with the bundled example procedures. A dangling
  symlink at that path is removed first; when the folder cannot be created
  or written, `~/Library/Application Support/SkinFlaps/History` is used;
  when neither works the bundle's read-only folder stays. File > Show
  session folder opens the folder; File > Save session files... writes
  `<name>.hst`, `<name>_full.hst`, their clock sidecars and `<name>.log`. A
  build run from the source tree keeps the tree's `History/` directory.

## Cutter guards

- **Knife points and paths.** Before any mutation, every cut point must
  resolve to top skin ("Incision point N is not on skin"): `closestPoint`
  searches all materials, so a point on an existing incision edge can land
  on a wall or bed triangle whose vertices carry no deep-bed record, which
  the cut would then dereference. Mid-cut, a point that resolves outside its
  triangle — `closestPoint` clamps u and v separately but never u+v, and a
  point in a triangle an earlier point of the same cut already split resolves
  to the shrunken parent — is walked across the exceeded edge into the
  triangle that really holds it, or refused ("too close to an earlier point",
  "no longer on skin") instead of reading `vertexCoordinate(-1)`. A surface
  path that reaches an existing incision is refused ("Incision path crosses
  an existing incision. Shift-click to T into it, or route around it-"). A
  refusal raised after points were inserted is treated like a throw: the hold
  reverts.
- **Deep-cut refusals.** Two consecutive posts in one surface triangle can
  never be connected (the commit's top probe throws), so the second click is
  refused at once ("Post too close to the previous one"); a post on an
  elevated flap is refused; post lines of one cut that cross beneath the
  surface are refused — a crossed pair spans a self-intersecting wall — and
  the existing crossover test stays active; a surface path that enters or
  leaves the skin at a border with no crossed edge recorded is refused rather
  than reading past the empty edge list; every fence refusal names the
  posts in the order they were placed.
- **Post-order reversal.** The chain builds walls only when walked from
  the closed post toward the open one; an open FIRST post makes the open
  end's skin line follow a wall-less deep-surface line and the cut fails. A
  cut open at exactly one end is therefore walked from its closed post
  whichever order the surgeon placed the posts, and the history record is
  stored in the walked order so it replays on every platform.
- **Local-normal deep-cut posts.** Each post takes the normal of the surface
  triangle it is clicked on. Inheriting the previous post's normal keeps
  post lines parallel, but on curved anatomy (ear, perioral) an
  inherited direction becomes tangential to the local surface, the post
  grazes a single sheet, and the interpost level stacks mismatch. The
  crossover refusal above covers what inheritance guarded against.
- **Border wedge to wall pair.** Where a cut leaves the skin at the
  periosteum border, the incision wall closes with one material-6
  wedge per side. The undermine rewires only material-3 wall pairs, so a
  wedge whose top vertex the undermine doubles keeps the old bottom vertex
  while the wall beside it takes the new one, and the surface comes back
  open. The overlay gives the last segment the same wall-pair structure as
  every other one.
- **Deep-bed record repairs.** A vertex the deep cutter splits into a skin
  edge, and the opposite skin vertex at the border, receive a deep-bed
  record — a later undermine or cut whose region reaches them
  dereferences a missing map entry — and the flap splitter only updates an
  existing record rather than inserting an uninitialized one through
  `operator[]`.
- **Undermine refusals.** A region that reaches the end of a deep cut at the
  skin border is refused before any change ("Undermine region reaches the
  end of a deep cut at the skin border. Undermine short of that end, or in
  two parts-"): the wedge there cannot be doubled, and the topological
  throw would otherwise come with the mesh already modified.
- **Deep-cut and undermine robustness.** Several searches in the cutter
  are guarded only by `assert`, which release builds compile
  out, so a miss reads uninitialized or out-of-range memory: the ring
  searches that sew a cut line to the surrounding surface, the deep-bed
  lookups behind a skin vertex, the pairing of adjacent posts whose
  surface paths never met, and the end of the intersection-map walk. The
  overlay converts each into a reported failure — the error hold above when
  a snapshot exists, otherwise the save-and-exit flow — and a post
  punched where no interior volume exists is refused with the existing
  "Attempted deepCut failed" message so the session simply continues. A
  suture removal that would otherwise wait forever on a physics flag its
  own caller holds now waits a bounded time and fails loudly.
- **Excise robustness.** Two overlay hardenings around the excise tool:
  (1) a read-only pre-check refuses, before any state or history
  mutation, a seed whose skin patch is not bounded by incisions (uncut
  tissue, or a closure that left an unsevered edge) — the walk would otherwise
  leave a border ring inside `undermineSkin()`; (2) when the physics rebuild
  after an excision does fail, the recoverable dialog names the underlying
  exception instead of a generic wrapper.
- **Physics-state remap after excision.** Rebuilding the lattice can
  produce a virtual-noded fragment containing no surface triangles, which
  otherwise ends the session ("Program error 3 in
  remapTetPhysics") because nothing identifies which prior fragment it
  continues. The overlay resolves it the way the neighboring case already
  does, by proximity, and the session survives; the assigned positions
  relax on the next solve.
- **Periosteal undermining.** The recovery that commits pending periosteal
  marks when the tool changes calls the key handler with the key code for
  the letter I, so the marks stay pending; the overlay sends Enter, which
  is the commit the recovery intends.

## Physics-side holds

- **Severed piece held until excised.** A both-open deep cut, or a
  periosteal release, can leave a piece of tissue with no constraint at all.
  The example histories excise such a piece before any force exists; once
  forces have been applied the cut re-initializes the physics, the system is
  singular, and the solver refuses it. A piece with no shared node, no
  T-junction, no fixed point, hook or suture is held where it is by strong
  hook constraints on a few of its tets until the next lattice rebuild, and
  the log names it; orphan slivers after a periosteal release are pinned the
  same way, once per lattice. Each physics initialization also reports the
  lattice's unanchored components in the log.
- **Runaway node guard.** Lattice nodes that are non-finite or lie farther
  than half the model's extent outside the lattice box mean a runaway solve,
  not a lifted flap. The guard throws "Tissue flew out of the model after
  the last move" into the hold; the solver's own check sees only a
  non-finite right-hand side, by which time the positions have overflowed.
- **Bed-ray self-hit test.** The flap/bed collision counts a bed-ray hit as
  a penetration only when the hit point lies below a bed patch within the
  ray's own length. The flap bottom coincides with the bed at rest, and where
  the bed bends sharply a ray cast into the bed passes under the bend and
  comes out through the flap's own mirror a few patches away; read as a
  penetration at rest and pushed the flap every solve (quivering
  along a deep-cut edge after undermining).
- **Flap wall bottom drawing.** An undermine doubles an incision wall's
  bottom vertices; at a cell the cutter does not split (a T-junction corner,
  the free end of an incision) the flap-side copy stays in the bed's tet and
  is drawn from the bed's nodes, so the flap lifts everywhere except that
  point and the wall stretches down to the wound. Such a vertex's drawn
  position is taken from a neighbouring flap-side tet; the cutter's own
  assignment is left alone because a later undermine assumes a vertex lies
  inside its recorded tet.

## Solver architecture

The Windows build solves the collision-partitioned system through PARDISO's
native Schur-complement mode. On macOS the equivalent is assembled from parts:
the A11 (non-collision) block is factored with CHOLMOD, the dense Schur block
`S = A22 − A21·A11⁻¹·A12` is formed explicitly, and S is factored and solved
with Accelerate LAPACK. An augmented sparse factorization of the whole
system was measured 3.35× slower than this arrangement, so the explicit
Schur formation is load-bearing, not incidental.

Five optimizations ship as the default implementation:

1. **Incremental suture refactorization.** A suture is a small low-rank
   matrix update; the port folds it into the existing factors —
   `cholmod_updown` on the no-collision path, a rank-k Woodbury update of S
   on the collision path — instead of refactoring from scratch. Bit-identical
   to the full refactorization; a suture-heavy history
   (`cleft_FisherRepair.hst`) replays ~4.9× faster.
2. **Parallel Schur formation.** The multi-RHS solve `A11·X = A12` is
   column-independent, so it runs column-batched across threads against the
   one shared, read-only A11 factor.
3. **Parallel coordinate solves.** The x/y/z solves share one factor and are
   independent; they run concurrently with per-thread workspaces.
4. **Collision-fingerprint cache.** When the collision constraint set is
   unchanged from the previous step, the Schur restore/update/refactor cycle
   is skipped — the cached factor is still valid.
5. **No full-system factorization on the Schur path.** With collisions
   active, the solve never reads the full-system CHOLMOD factor, so the
   per-step `cholmod_factorize` of the whole matrix is dead work and is
   skipped; the symbolic analysis stays valid until the next topology change.

## Lessons

- **CHOLMOD factorizes `PAP'`.** A split solve must apply the permutation:
  forward is `P` then `L`, backward is `L'` then `P'`. Omitting `P`/`P'`
  returns a wrong-but-plausible solution that no cross-platform comparison
  will flag as obviously broken.
- **No `-ffast-math`, no FP contraction, anywhere.** The local–global
  iteration sits near its stability edge on post-cut topologies; FP
  reordering can push its spectral radius past 1 and the tissue diverges.
  Both build presets pass `-ffp-contract=off -fno-fast-math`.
- **Keep the solver internals in double.** The dense Schur block is stored,
  factored, and solved in double precision end-to-end. This absorbs the
  reduction-order differences between MKL and Accelerate that a float Schur
  block does not.
- **Force the serial tet-cutter paths.** `std::unordered_*` iteration order
  and TBB container ordering are nondeterministic on macOS/libc++ (they are
  de facto stable under MSVC), which made identical replays produce different
  post-cut topology. The `overlays/vnBccTetCutter_tbb_deterministic.cpp`
  overlay compiles the cutter with its own serial fallback paths
  active.
- **Never "reset" per-element stiffness after subset assignment.** Model
  files assign per-region material overrides (`tetrahedralSubsets`) after the
  uniform defaults; re-initializing the mu arrays destructively silences
  those overrides and shifts the iteration matrix's stability.

## Replay verification

The app can drive a history file to completion without user input:

```
SKINFLAPS_AUTORUN_HISTORY=History/AbbeEstlanderLip.hst \
SKINFLAPS_AUTORUN_STEPS=19 \
  build/gui/SkinFlaps/SkinFlaps.app/Contents/MacOS/SkinFlaps
```

Each action is dispatched only after the physics has settled (equilibrium,
plateau, or step-cap), matching how a surgeon paces the NEXT button; the
program exits after the last action settles.
