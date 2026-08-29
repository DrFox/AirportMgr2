# Ground Movement Model — Design

**Status:** design only. Defines data and rules. Defines no generation, no pathfinding, no simulation.

**Parent:** `2026-08-28-procedural-road-system-design.md`. This spec **amends R3 and R9** of that document; see §3.

---

## 1. The problem this fixes

The parent design has one graph. A segment is pavement, and lanes are derived from its profile, so the thing an aircraft follows and the thing it drives on are the same object seen two ways.

A satellite image of Birmingham T2 (`samples/taxiwayBhx.png`) shows that this is false. A single sheet of concrete carries a dozen yellow guidelines that fan from the taxiway to the stands, branching and merging in a pattern with no relationship to any pavement edge. Where two of those cross there is no junction of pavement — no corner, no fillet, nothing for the junction solver to do — but there is absolutely a junction of *routes*.

**The surface an agent may occupy and the line it is told to follow are different things, and they form different graphs.** On a taxiway they coincide, which is why the conflation survived this long. On an apron they come apart completely.

Everything below follows from separating them.

---

## 2. Terms

| Term | Meaning |
|---|---|
| **Surface** | Pavement. Where an agent can physically be. A ribbon (profile swept along a centreline) or a polygon (apron). |
| **Guideline** | A line an agent is told to follow. Yellow taxi centreline, road lane, service route, pedestrian walkway. |
| **Traversal class** | How a thing moves: `Aircraft`, `GroundVehicle`, `Pedestrian`, `Emergency`. |
| **Service role** | What a thing does at an anchor: `Fuel`, `Baggage`, `Tug`, `GPU`, `Passenger`, `Crew`, … |
| **Entity** | A placed installation — a stand, hangar, de-icing pad — binding into the guideline graph through anchors. |
| **Anchor** | A typed connection point between an entity and the guideline graph. |

Traversal class and service role are **deliberately separate**. A refuel truck and a baggage cart obey identical movement rules and differ only in job. Merging them would produce a traversal enum that grows with every vehicle type in the game and is consulted by pathfinding for no reason.

---

## 3. Amendments to the parent spec

**R3 — Navigation.** Reads "Lane-level modelling from the start — aircraft taxiing needs per-lane turn paths through junctions."

Wrong for aircraft. A taxiway has **one** guideline, not lanes; an aircraft occupies the full width with its nose wheel on the line, and separation from the aircraft ahead is longitudinal. R3 becomes **guideline-level modelling**, where a road profile generating one guideline per lane is the special case that recovers the original meaning.

**R9 — Segment subdivision.** Reads "A long drag auto-subdivides into multiple segments at a maximum length, giving finer granularity for AI pathing and per-segment logic."

The AI justification is void. Pathfinding runs on guidelines; subdividing *pavement* to gain pathing resolution buys nothing, while costing extra junction solves, extra welds, and making §5's `theta → pi` degenerate case the common path. R9 survives or falls on **editing ergonomics alone** — selecting, deleting or moving part of a run — and if kept, its maximum length should be chosen for that, not for pathing.

Guideline granularity is set by meaning, not by interval: nodes exist at junctions, crossings, hold-short positions and entity anchors, because those are where an agent decides or stops. A node every N metres has nothing to say to anybody.

---

## 4. Model

Three new collections in `URoadNetwork`, in slot maps beside the existing nodes and segments, so all of it inherits generation-checked handles and the command/undo machinery.

Rationale for one object rather than a separate `UGuidelineNetwork`: the build tool must make "draw a taxiway" a single atomic undo step spanning pavement *and* its derived guideline, and `Revert` must restore handles identically including generation counters. Splitting the graphs across two objects makes every composite command a two-phase commit for no gain. The conceptual separation is preserved by directory and header layout, exactly as `Solve/` is isolated today without being its own object.

### 4.1 Surfaces

```cpp
FRoadSegment    // unchanged - ribbon between two nodes, profile, persisted cut vertices
FApronSurface   // NEW
{
    TArray<FVector2D> Outline;                  // simple polygon, CCW
    FName             SurfaceMaterialSlot;      // concrete, asphalt, ...
};
```

An apron has no cross-section, so it cannot carry a `URoadProfile` — bands and lanes are meaningless for a polygon. It names a material slot directly. This overlaps with the material role `URoadProfile` acquires in the per-band materials slice; factoring that role into something both can share is a decision for **that** slice, and is raised here only so the duplication is deliberate rather than discovered.

Only segments participate in the junction solve. An apron has no arms, no fillets and nothing to trim. This is the one place polymorphism is justified — ribbon and polygon genuinely differ in behaviour (geometry generation, editing affordance, junction semantics) rather than in field values. Downstream consumers deal in "a surface".

### 4.2 Guidelines

```cpp
enum class ETraversalClass : uint8 { Aircraft, GroundVehicle, Pedestrian, Emergency };
using FTrafficMask = TFlags<ETraversalClass>;      // bitmask

enum class EGuidelineDir : uint8 { Bidirectional, AToB, BToA };

FGuidelineNode
{
    FVector2D Position;
    TArray<FGuidelineEdgeId> Incident;

    /** Non-none when this node requires clearance; names the surface it protects (§5.5). */
    FRoadSegmentId HoldShortFor;

    /**
     * Overrides the class priority order at this node (§5.4). Empty means the default
     * order applies, which is the case nearly everywhere - authoring is the exception.
     */
    TArray<ETraversalClass> PriorityOverride;
};

FGuidelineEdge
{
    FGuidelineNodeId A, B;
    FVector2D        Control;          // quadratic Bezier, as FRoadSegment
    FTrafficMask     AllowedTraffic;   // bitmask of traversal classes
    EGuidelineDir    Direction;        // Bidirectional | AToB | BToA
    double           Width;            // physical extent; see 5.3
    double           MaxWingspan;      // 0 == unlimited
    FRoadSegmentId   DerivedFrom;      // invalid when hand-drawn
    bool             bDerived;         // false once manually edited
};
```

**Generation.** A segment generates **one edge per guideline its profile declares** - one for a taxiway, one per lane for a road - each spanning the whole segment end to end. It does not generate interior nodes; per §3, guideline nodes exist only where something happens. A **junction generates one edge per ordered arm pair** — these are the parent spec's §5.8 turn paths, expressed as ordinary guideline edges so that pathfinding never special-cases a junction. An apron generates nothing; its guidelines are drawn by hand.

**Derived by default, independent in the data.** Guidelines are modelled as first-class edges rather than as a view over segments, so that a future feature letting a player redraw a taxiway's guideline is not blocked. `bDerived` starts true and flips to false on first manual edit. Derived guidelines are regenerated when their surface moves; edited ones are not, because regenerating would discard the edit. A move affecting an edited guideline is therefore a decision the build tool surfaces, not something the model resolves silently.

**No weld contract here.** The guideline graph is connected by handles, not by coincident positions, so it has no analogue of the seam problem that dominates the surface model. Guideline geometry may be recomputed freely.

### 4.3 Entities

Flyweight, matching `URoadProfile`.

```cpp
FEntityAnchor            // on the definition, local space
{
    FVector2D    LocalPosition;
    double       LocalHeading;
    EServiceRole Role;
};

UEntityDefinition : UDataAsset
{
    TArray<FEntityAnchor> Anchors;
    // footprint, visuals, per-type data
};

FEntityInstance          // in the network's slot map
{
    FVector2D  Position;
    double     Heading;
    TObjectPtr<UEntityDefinition> Definition;
    TArray<FGuidelineNodeId> ResolvedAnchors;   // parallel to Definition->Anchors
};
```

Placing an entity resolves each anchor to a guideline node at its world pose. A stand definition declares an `Aircraft` anchor (stop position and heading), plus `Fuel`, `Baggage`, `Tug`, `GPU` and `Passenger` anchors. "A baggage cart drives to stand 12's cart position" is then an ordinary path query to an ordinary node.

Adding an entity type is a new data asset, not new code. This is the property the anchor mechanism exists for, and the first new requirement to arrive — passengers walking to a remote stand — cost one enum value.

---

## 5. Rules

### 5.1 Access

`AllowedTraffic` is a bitmask of traversal classes.

| Guideline | Mask |
|---|---|
| Aircraft centreline | `Aircraft \| Emergency` |
| Service road | `GroundVehicle \| Emergency` |
| Pedestrian walkway | `Pedestrian \| Emergency` |
| Shared apron crossing | all |

`Emergency` appears nearly everywhere; that is the point of it.

### 5.2 Direction

`Bidirectional | AToB | BToA`, defaulted from the profile's lane direction. Unavoidable rather than speculative: a road profile generating one guideline per lane produces inherently directional edges. Taxiways default bidirectional, which is how they work in reality — opposing traffic is resolved by clearance, not by a property of the pavement.

### 5.3 Width — and what it is not

`Width` is the **physical extent of the path**. It drives marking geometry and clearance. It is *not* a capacity.

There is no capacity field. Two distinct arguments retire it:

- **For single-file paths, capacity duplicates structure.** Two lanes are two guidelines; a number saying "2 abreast" on one guideline states the same fact twice and invites the two representations to disagree. This is the same failure class as recomputing a welded vertex: do not represent one thing two ways.
- **Flow versus single file is a property of the traversal class, not the edge.** Pedestrians spread across a walkway and pass each other; aircraft and ground vehicles queue single file. A four-metre service road could physically hold two vans abreast and never does, so capacity is not derivable from width either.

Width therefore has exactly one job. The case that proves the separation: an apron service road is single file *and* painted as two edge lines with nothing between them, so it needs a width for its marking while being single-occupancy for its use.

### 5.4 Priority

Priority is a property of the **traversal class**, not of the edge or the crossing:

```
Emergency  >  Aircraft  >  Pedestrian  >  GroundVehicle
```

A crossing resolves by comparing whoever is actually contending, so the common case needs no authoring at all. A per-node override exists for real-world exceptions.

The order is total, and stays total because access does the other half of the work: pedestrians are not permitted on a service road except at a marked crossing, so "who gives way mid-road" never arises. Crew at a marked crossing outrank a baggage tug; nobody outranks a taxiing aircraft except an emergency vehicle.

Authoring priority per crossing was rejected: laborious, and wrong the moment a fire truck arrives.

### 5.5 Hold-short

A flag on a guideline **node**, naming the surface whose clearance it protects. Parent R10 guarantees a runway crossing produces a real node, so the topology already exists.

### 5.6 Size limits

`MaxWingspan` on the edge, defaulted from the profile and overridable. This is what stops a Code F aircraft being routed down a Code C taxiway, and it is the constraint that makes an airport layout a real design problem rather than a drawing.

### 5.7 Occupancy — deliberately absent

Exclusivity is **lateral, not longitudinal**. An aircraft takes the full width, so nothing passes it; the length holds a queue, which is why ATC instruct crews to line up behind the aircraft ahead.

The model expresses width, direction and semantically-placed nodes. It does not choose between:

- **interval occupancy** — agents hold a stretch of an edge, separated by distance. Faithful to how traffic is actually separated, needs no extra nodes.
- **block occupancy** — guidelines subdivided, one agent per block. Railway signalling: cruder, far simpler to implement and debug.

Both are expressible, because a block is just a guideline node and the model already has those. The choice belongs to the simulation slice. Reservation and deadlock avoidance — two aircraft nose to nose on a bidirectional taxiway — likewise.

---

## 6. Markings are derived

Every marking must have a source in the data. Where one does not, the model is missing something. This table is a falsification test for the model, not a feature list, and it is cheap to run now and expensive later.

| Marking | Derived from |
|---|---|
| Yellow taxi centreline | guideline edge, `Aircraft` access |
| Stand lead-in line | the guideline into an `Aircraft` anchor |
| Hold bar | guideline node flagged hold-short |
| Stand number and stop position | entity instance's `Aircraft` anchor |
| Service road edge lines | guideline edge, `GroundVehicle` access, at ±`Width`/2 |
| Pedestrian walkway edging | guideline edge, `Pedestrian` access, at ±`Width`/2 |
| Zebra crossing | node where a `Pedestrian` edge meets a `GroundVehicle` edge |
| Runway / taxiway edge treatment | the surface profile's outermost band |
| Road centre line | two adjacent lane guidelines of one surface |

**Marking style follows the traversal class**, not an authored field:

| Class | Painted as |
|---|---|
| `Aircraft` | one centreline; the agent's nose wheel rides it |
| `GroundVehicle` | two edge lines; the agent travels between them |
| `Pedestrian` | two edge lines, plus a zebra at a crossing node |

Note that markings derive from **three** sources, not one — the guideline, the surface profile, and the adjacency of two guidelines. That is deliberate, and recorded here so it does not later read as an oversight.

Rendering is out of scope (parent slice 6). Derivability is in scope, because it constrains the model now.

---

## 7. Validation rules

For the build tool's `FPlacementValidator` (parent §7.5), a Composite of small rules. Recorded here while the reasoning is fresh:

- A `Pedestrian` guideline may not cross an `Aircraft` guideline. Real airports escort passengers or route them around; they do not paint a zebra across a live taxiway.
- A guideline must lie on a surface. A route across grass is a modelling error.
- An entity anchor must resolve to a reachable guideline node, or the entity is unusable and silently so.
- A guideline edge's `MaxWingspan` may not exceed what its surface's width physically permits.

---

## 8. What this changes in existing code

- `URoadNetwork` gains three slot maps: apron surfaces, guideline nodes and edges, entity instances.
- `FProfileLane` is **absorbed**. A profile declares the guidelines its cross-section generates — offset, traversal class, direction, width — of which "one guideline, `Aircraft`, bidirectional" is a taxiway and "one per lane, `GroundVehicle`, directional" is a road.
- **No change to `Solve/`**, to `FRoadNetworkSolver`, or to the mesh builder. The surface pipeline and its bitwise weld contract are untouched.
- No change to the existing profile band work; per-band materials remain a separate slice and are unaffected.

---

## 9. Out of scope

- Apron mesh generation (polygon triangulation, edge treatment).
- The pathfinding algorithm.
- Agent simulation: reservation, deadlock avoidance, queueing, overtaking, crowd flow.
- Marking rendering (parent slice 6).
- ATC, clearances and scheduling.

Each consumes this model. None of them constrains it, provided the model expresses width, access, direction, priority and semantic nodes — which §4 and §5 do.

---

## 10. Testing

The model is data, so its tests are about invariants rather than geometry:

- Guideline derivation from a segment is **idempotent** — regenerating twice yields identical edges.
- `bDerived` flips to false on manual edit and survives a regeneration pass, which then leaves that edge alone.
- A junction generates exactly one edge per ordered arm pair, and each names live arm nodes.
- An access mask excludes as expected: a `GroundVehicle` query never returns an `Aircraft`-only edge.
- The priority order is total, and a crossing between any two classes resolves deterministically.
- Anchor resolution round-trips: placing, moving and undoing an entity restores identical `ResolvedAnchors` handles, generation counters included.
- Every row of §6's marking table has a live source in a small network built to exercise it: a taxiway into a stand, a service road crossing it, a walkway crossing the service road. Not a reproduction of BHX — the point is coverage of the table, not of the airport.

---

## 11. Risks

1. **The two-graph split doubles the editing surface.** Every operation now potentially touches both graphs, and the build tool must keep them consistent within one undo step. This is the cost of the split and it is paid in the tool, not the model.
2. **Derived-then-edited guidelines are a stale-data hazard.** `bDerived` records the state but does not resolve what a pavement move should do to an edited guideline. That decision belongs to the build tool and must be made explicitly rather than defaulted.
3. **`FProfileLane`'s absorption is free today and will not stay free.** Verified 2026-08-29: no `URoadProfile` assets exist under `Content/`, and `FProfileLane` is written by `MakeTransient` and read by **nothing**. Absorbing it costs one struct change and no migration. The moment a profile asset is authored, it costs a migration instead.
4. **Pedestrians are the first flow-type class.** A simulation slice that assumes every class queues like an aircraft will be wrong for them, and wrong in a way that looks like a bug rather than a design gap.
