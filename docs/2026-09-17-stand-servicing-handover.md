# Handover: stand servicing, after four failed attempts and a rethink

Paste this into a fresh session. Everything below was established by measurement in the session
of 2026-09-16/17; nothing is a guess unless it says so.

**Branch:** `feature/lane-entrances`, 28 commits ahead of where this session started (`96424e6`),
HEAD `c10ef20`. Nothing pushed, no PR. Working tree clean.

**Suite:** `366 test(s) run, 1 failed, 0 crashed`. **The one failure is deliberate** — see
"The acceptance test" below. Do not make it pass by weakening it.

---

## Read these two first

1. `docs/superpowers/specs/2026-09-16-stand-servicing-rethink-design.md` — why four attempts
   failed, and the A/B/C/D decomposition.
2. `docs/superpowers/specs/2026-09-17-stand-layout-solver-design.md` — piece C's design.

Then `docs/superpowers/plans/2026-09-17-stand-layout-solver.md`, which is the work.

---

## Why four attempts failed — the one thing to understand

**Every attempt derived a layout and assumed the arithmetic meant it worked**, because nothing
could answer "can a vehicle actually drive this?" So the answer arrived in PIE, as a truck that
crabbed, four times running.

`FSpeedProfile::Build` was always the one authority. It measures `Length / Turn` across every
span of a WHOLE ROUTE and separately flags a vertex whose heading changes instantly. It computed
both verdicts, logged them, and **threw them away into locals** — so it could be read by a human
staring at a log and by nothing else, and every test re-implemented its rule ONE EDGE AT A TIME.

Grep `"the same expression FSpeedProfile::Build uses, written out rather than shared"`: every hit
was a site that should have called it.

**A route of individually-legal edges can still be illegal WHERE TWO MEET, and no per-edge test
sees a join.** The PIE repro showed three warnings 87 uu apart at 653-670 uu against 699.4 — the
signature of a join, not of a corner anyone sized.

That is now fixed (piece A). Keep it fixed: **ask the authority, never restate its judgement.**
Restating *arithmetic* in a test is right and `FAirframe::TightestFollowableRadius` argues for it;
restating a *judgement* is what caused this.

---

## What is built and committed

### Piece A — the drivability oracle (`e2d2982`, `87c45ba`)

`FSpeedProfile` keeps the verdict it was discarding:

- `WasTighterThanLock()`, `GetTightestRadius()`, `GetTightestAt()`
- `HasSharpVertex()`, `GetSharpVertexCount()`, `GetSharpestDegrees()`, `GetSharpestAt()`

Both rules are exposed, and that was not the first attempt — exposing only the radius rule let
the first test pass while its route contained a 175 degree instantaneous reversal. **Half an
authority is still an authority nobody can fully ask.**

The reset sits ABOVE `Build`'s early return, because a rebuilt profile that took the degenerate
path would otherwise keep the previous route's verdict and read as authoritative.

### Piece B — reverse into the bay (`5276ca1`)

- `FAirframe::TightestReversibleRadius()` = `Wheelbase / tan(lock)` = **494.5 uu**, against the
  forward **699.4**. A reversing vehicle turns ~30% tighter because it pivots about its FIXED
  axle. This is why a bay is affordable at all.
- `FSpeedProfile::Build` takes `EDriveDirection` and judges by the right limit.
- `FReverseRun` plays back a pre-solved curve — it does NOT track a line. Tracking in reverse is
  unstable (error grows), and every reverse this game performs is between two known poses.
  `FPushbackRun` is not reusable: it works because a tug couples at the nose gear so the steered
  axle still leads.
- **`FReverseRun::Start` REFUSES a curve the airframe cannot hold**, and refuses a sharp vertex
  separately. Playback has no error term, so an unchecked curve would be crabbed silently. That
  is the no-crabbing guarantee, enforced at arming rather than promised.

---

## The acceptance test — currently RED on purpose

`Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant` (`ServiceLinkTest.cpp`).

It plans the real road-to-hydrant route and asks `FSpeedProfile` about all of it. It fails at:

```
no vertex of the route turns instantly (1 found, sharpest 175 deg at 9489)
```

Point 31 is `(-3236, -600)`, a tail-crossing entry node: the route arrives from the road and
**reverses 175 degrees on the spot**.

**Why:** each crossing has two entry nodes whose merges face OPPOSITE directions, and
`RouteSearch::EdgeCost` is sampled length plus congestion — **no curvature term, no
heading-continuity term** (verified at `RouteSearch.cpp:11-36`). So the search picks the NEARER
entry regardless of which way its merge faces.

**A correct layout can still be handed an undrivable route by a search that does not know what
drivable means.** Piece C's fix is a DIRECTED entry, so the wrong choice stops existing.

It also records that the radius rule passes by **0.6 uu** — 700 against 699.4. That is not a
margin, it is a coincidence, and it is why every attempt felt like it nearly worked.

**This test going green is what says the redesign worked.** Nothing else does.

---

## Piece C — designed and planned, not started

**The property the whole thing exists to get: drivability is a property of the TEMPLATE,
verified once for every vehicle — not of every placement.** Placement becomes a transform and a
yes/no, and a transform preserves curvature.

Decisions taken with the user, all recorded in the spec:

- A stand has a **staging rank**, a **bay per service**, and **one fixed entry that must meet a
  road**. Placement VALIDATES; it does not solve. That removes the last per-placement geometry.
- **Three legs per bay**: approach (forward), reverse (the back-in), exit (forward). The vehicle
  does not retrace the reverse leg, or that curve would have to satisfy the forward limit and
  the 30% reversing buys would be spent.
- Bays are **airframe-independent** — paint does not move when a different type parks. Extent
  resolves the **largest airframe the letter admits**.
- Stand sizes join `IcaoCode`'s one table. **Width is DERIVED** — every figure is the letter's
  existing span band plus twice its ICAO wingtip clearance, all six exact. **Depth is authored.**
- **The code letter needs BOTH dimensions.** 67 x 30 m is a Code B, not a Code D.
- The template is built for the **FLOOR of its band** (45 m for Code C). **Extra width inserts
  STRAIGHT, never reshapes a curve** — that is what lets one verification cover the whole band.
- A small airframe on a large stand is fine; a Cessna 172 could use a Code C.
- Refuse placement with a named reason when it will not fit. A stand that exists is serviceable.

The plan is seven tasks. The user's suggested approach: **inline for Tasks 1-3**, where the
template geometry is and where a human needs to look at the numbers, then decide.

### Two places the plan says it is most likely wrong

1. **Task 2's pose positions are written from geometry and have never been run.** The step says
   to log them and read them. Expect to move them; that is the task working.
2. **Task 4 assumes `RouteSearch` honours `EGuidelineDir`.** `RoadNetwork.cpp:776-778` does
   honour it when walking outgoing edges, but whether the search reaches edges ONLY through that
   function is UNVERIFIED. If not, the one-way entry is not enforced and the test passes while
   the property fails. **Check the consumer before relying on it.**

---

## A live defect, found while specifying and not yet fixed

The stand geometry is sized from the A320's tail at **-3250**, but `DA_Aircraft_B738` is authored
at **-3430** and already parks on the same stand. That is **1.2 m of tail clearance where 3 m was
intended**, and the tail clamp is already the binding constraint. Piece C's "extent resolves the
largest airframe the letter admits" fixes it.

---

## Landmines that cost time this session

- **`FRoutePlan::IsValid()` is `Result == ERouteResult::Found`**, not "has a polyline". A fixture
  that fills only geometry is refused at the first guard and reads as the code rejecting a legal
  input.
- **A `UENUM`/`USTRUCT` must not sit between another type's doc comment and its declaration.**
  UHT says `Found 'UENUM' when expecting struct`. Put reflected types ABOVE the doc comment.
- **A `UE_LOG` Warning does NOT fail an automation test here.** `FAutomationTestBase::bElevate
  LogWarningsToErrors` is false (`Runtime/Core/Private/Misc/AutomationTest.cpp:181`); nothing in
  `Config/` sets the key. There is a same-named flag on `UAutomationControllerSettings` that IS
  true — reading the wrong one misleads. Saved as memory `unreal-warnings-do-not-fail-automation-tests`.
- **Sampling is NOT the reason a radius under-reads.** Sampled `Length/Turn` and analytic
  `TightestRadius` agree to 0.1% at `DefaultSamples = 16`. Checked; do not re-chase it.
- **A corner here is a QUADRATIC, not a circular fillet.** `CornerRunFor(R, θ) = R·cos(θ/2)/sin²(θ/2)`,
  which at a right angle is **1.414 R**, not R. The spec costed it wrongly twice.
- **A crossing between two anti-parallel runs is a 180° U-turn**, so two corners joining them are
  90° each and no diagonal makes them shallower. The floor for ANY 180° turn is `2R` = 1398.8 uu.

---

## The recurring failure mode to watch for in yourself

Seven times on this branch, someone wrote a **precaution up as a mechanism** — a comment
asserting something nobody had measured. A "diagonal at 73.55°" that solved the wrong problem; a
per-corner test where the constraint was per-straight; reach arithmetic with the wrong premise; a
`BlueprintType` justified by a Python requirement that does not exist; a caller list that was
stale in the commit that wrote it.

**No test catches that shape.** Only a reader who checks. When you write "because X", make sure
you measured X.

---

## Open decisions the user has not made

1. **The 16 superseded lane commits** (`5e97b9d`..`082c4aa`). Piece C replaces them. Revert, or
   leave for C to overwrite? Not urgent either way.
2. **Does the staging rank's capacity scale with the code letter**, or is it one figure?
3. **Depth as a band like width** — minimum authored, extra absorbed as straight fore-aft. Written
   that way in the spec; unconfirmed.
4. **The earlier 31 commits on this branch** (before this session) have had no whole-branch
   review. The final review was deliberately scoped to this session's work.

## Not started

**Piece D** — the player-drawn polygon apron. The user has a spike on another branch. C should
not assume a shape D has already ruled out; `RequiredExtent` is what D will test a polygon
against.
