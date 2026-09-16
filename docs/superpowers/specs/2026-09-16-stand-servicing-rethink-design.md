# The stand is solved, not threaded — a rethink after four attempts

> Supersedes the layout half of `2026-09-16-stand-routing-design.md`. That document's
> measurements stand; its shape does not.

## Why four attempts failed, in one line

Every attempt **derived a layout and assumed the arithmetic meant it worked**, because nothing
in the codebase could answer "can a vehicle actually drive this?" — so the answer arrived in
PIE, as a truck that crabbed.

That is not a geometry problem. `FSpeedProfile::Build` is and always was the one authority on
whether a line is drivable: it measures `Length / Turn` across every span of a whole route and
warns when a span asks for a radius the steering lock cannot hold. It computed that verdict,
logged it, and **threw it away** into three locals. So it could be read by a human staring at a
log and by nothing else, and every test re-implemented its rule one edge at a time. Grep for
"the same expression `FSpeedProfile::Build` uses, written out rather than shared": every hit is
a site that should have called it.

A route of individually-legal edges can still be illegal **where two meet**, and no per-edge
test can see that. Reported from play 2026-09-16: four warnings on one journey, three of them
87 uu apart at 653-670 uu against the 699.4 the lock allows — the signature of a join, not of a
corner anyone sized.

**The missing piece was never geometry. It was a verdict anything could ask for.**

## What the ground should look like

Real aprons do not thread a lane past the aeroplane. They have a **designated staging area**
for ground vehicles and **marked bays** at each service point.

- A vehicle routes from the service road to the stand's **staging area** — one arrival point,
  so there is ONE sweeping arc off the road to get right instead of five.
- From staging it takes a **short leg to its own bay** at the hydrant, the hold door, the GPU.
- Each service has its **own bay**, so a vehicle already working does not block anyone. This is
  the direct answer to the defect the lane design could not answer: with anchors ON a shared
  through-lane, a truck routing past an occupied box drives through whatever is parked there,
  and a single cycle gives it no way round.

### Three consequences, taken deliberately

**1. Vehicles drive to their service point.** Not "park at staging and work from there", which
would kill the geometry problem outright but put a fuel truck 30 m from the hydrant.

**2. A bay is a dead end, so the vehicle REVERSES INTO IT.** There is no third option: leaving a
dead end forwards is impossible, so either every bay is a drive-through with two 699 uu corners
— the apron-eating loop that failed twice — or the vehicle backs in. It drives forward from
staging past its bay, stops, backs in, and drives out forwards when done. That is what a
dispenser does at a hydrant and a loader at a hold door.

Reversing therefore stops being "a later stage" and becomes the thing that makes the layout
affordable. It is also the EASY form: a short leg between two KNOWN poses, pre-computable per
bay, not a search. `FPushbackRun` is not reusable — it works precisely because a tug couples at
the nose gear so the STEERED axle still leads.

**3. The layout is DERIVED, but derivation must be CHECKABLE.** The end goal (below) is a
player-drawn apron, which forces derivation — authored poses on an asset are a dead end for it.
So the answer to four failed derivations is not to stop deriving; it is to make a derivation
able to check itself and refuse out loud.

## The end goal, and why this is not it yet

**The player draws a polygon apron and the game solves where parking and service points go, how
they connect, and where the roads join — at place time.** Same pillar as buildings being drawn
rather than stamped.

That is the right goal and it is achievable. It is also **strictly harder** than what has just
failed four times: we could not reliably derive a drivable layout for ONE known rectangle with
five fixed service points, and an arbitrary polygon is that problem with the constraints
removed.

What makes it tractable is the same thing that was missing all along. Once a solver can ask "can
the largest admitted vehicle drive this, and if not, where and by how much", the failure mode
becomes a FEATURE: the player draws too tight an apron and the game says so at draw time, the
way a city builder refuses a placement and tells you why. That is a better game than a truck
that silently crabs.

## The decomposition

Four pieces, each useful alone, in this order. Each gets its own spec and plan.

### A — the drivability oracle

One authority that answers: can this vehicle drive this line, where is the worst point, and by
how much does it miss. Everything consumes it — tests, the solver, the player-facing refusal.

Half-built already: `FSpeedProfile` now keeps the verdict it was discarding
(`WasTighterThanLock`, `GetTightestRadius`, `GetTightestAt`), and one test asks it about a whole
route rather than re-deriving its rule on one edge.

**This is the piece that would have caught all four failures.** It is days, not weeks.

### B — the reverse-into-bay manoeuvre

A short leg between two known poses: reverse kinematics in `FRouteFollower` with the fixed axle
on the line and steering inverted. Bounded, because both poses are known in advance.

### C — the stand layout solver

Given a region and an aircraft: place staging, place bays, connect them, **check every
connection with A**, and refuse with a named reason when it cannot. Proved on the Code C stand
we already have, where the result can still be checked by eye.

C is where we find out whether the solver approach is sound, while the problem is small.

### D — the player-drawn apron

The polygon tool, place-time solving, and the feedback when it will not fit. Consumes C.

## What this supersedes

The lane-and-entries design and its implementation (16 commits on `feature/lane-entrances`) are
superseded by C. The measurements in `2026-09-16-stand-routing-design.md` remain true and worth
keeping — the corner-cost formula, the 2R floor for a 180 degree turn, the derived-vs-delivered
distinction — but the shape they describe threads a lane past an aeroplane, and that shape is
what we are abandoning.

## Unresolved questions

1. When the solver cannot fit a stand in a drawn apron, should that REFUSE the placement, or
   place it and FLAG it as unserviceable? Shapes C and D; not needed for A.
2. Does a route want its curve computed ONCE over the whole path, rather than as a jigsaw of
   quadratic Bezier edges that must each be legal and agree pairwise at their joins? Every
   guideline edge today is a single-control-point quadratic, so it cannot inflect, so every S
   is two edges and every corner its own. The joins are where the crabbing was. Deferred:
   A will say whether joins remain the problem once there are fewer of them.
3. Does the existing `feature/lane-entrances` work get reverted, or left for C to replace?
