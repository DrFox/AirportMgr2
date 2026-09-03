# Hand-Authored Guidelines — Design

**Status:** approved 2026-09-03.

Lets the player draw a routing connection the derivation never made — joining a stranded
road, or reaching a stand whose lead-in missed. The model already reserved the idea; this
makes it actually hold.

## 1. The defect this starts from

`FGuidelineEdge::bDerived` exists, `bDerived == false` edges survive
`FRoadGuidelineBuilder`'s clear pass, and `FindSparedEdge` makes the builder step aside for
them. That reads as ready. It is not.

`URoadNetwork::AddGuidelineNode` never deduplicates: it always allocates a new slot. So on
the next rebuild — which is every road edit — re-derivation creates FRESH nodes at the same
positions and attaches its edges to those, while the player's edge still points at the old
ones. Measured, by `RoadNet.Model.GuidelineAuthoring`:

```
✓ the player's edge survives the rebuild
✗ the link's near end is still attached to the derived graph
✗ the link's far end is still attached to the derived graph
✗ no two alive guideline nodes share a position — expected 0, was 2
```

Two coincident pairs, one per endpoint. **Surviving is not staying connected.** The link
would draw correctly, route nothing, and break on an edit that had nothing to do with it.

The builder's own comment names this shape already: *"two coincident-but-distinct nodes
would satisfy every position check while leaving the turn paths as separate sticks nothing
can route across."*

## 2. Decisions

| Question | Decision |
|---|---|
| How a player edge stays attached | **It stores the IDENTITY of each endpoint, not a handle, and is re-resolved after every derivation.** |
| Why not dedupe nodes by position | The guideline graph shares by HANDLE by design — *"the surface model shares by POSITION and must be bitwise; the guideline graph shares by HANDLE and needs no such contract"*. Position-matching would put exact float comparison into the one graph deliberately built without it. |
| Why not splice like `FAnchorLink` | It works, and it is how entity anchors survive, but it is the most machinery and a splice must decide WHICH guideline to cut. Provenance needs no such decision because the endpoint already knows what it is. |
| What identity is | `(SegmentId, which end, guideline index)` — the key `FRoadGuidelineBuilder` ALREADY computes as `EndKey`. |
| Editor mode | **Out of scope.** See §7. |

This is the move `FResolvedAnchor` already made: *"carries the id it resolved FROM, so
nothing has to be parallel to anything."* The bug in one line is that a player edge stored a
handle where it should have stored an identity.

## 3. Model

```cpp
/** Which derived endpoint a node IS, so it can be found again after regeneration. */
USTRUCT()
struct FGuidelineEndRef
{
    UPROPERTY() FRoadSegmentId Segment;
    UPROPERTY() bool  bEndA = true;
    UPROPERTY() int32 GuidelineIndex = 0;

    bool IsSet() const { return Segment.IsSet(); }
};
```

- `FGuidelineNode` gains `Origin`, set by the builder when it creates a segment-end node.
  Unset on anchor and pose nodes, which are non-derived and already stable.
- `FGuidelineEdge` gains `EndRefA` / `EndRefB`, filled ONLY for `bDerived == false` edges,
  copied from the nodes the player clicked.

An edge whose refs are unset is left exactly as it is. That covers a player edge drawn
between two anchor nodes, whose handles never move.

## 4. Re-resolution, and an ordering change

After the derivation loop, `Build` walks alive `!bDerived` edges and re-points each endpoint
whose `EndRef` is set at `Ends[EndKey(...)]`. `Ends` already exists and is already keyed
exactly this way; it simply stops being thrown away.

Re-pointing changes incidence, so the network gains:

```cpp
bool RelinkGuidelineEdge(FGuidelineEdgeId Edge, FGuidelineNodeId NewA, FGuidelineNodeId NewB);
```

**The orphan sweep MOVES to after re-resolution.** It currently runs before derivation, which
was correct while nothing could detach a node later. Re-resolution detaches the player edge's
old endpoints, so a sweep that ran earlier leaves them alive forever — a slow leak of dead
coincident nodes, and the overlay would draw every one. Running it last is strictly safer: it
is the only point at which every detachment has happened.

An `EndRef` that resolves to nothing — its segment was deleted — leaves the edge alone and
**kills it**, rather than leaving a link to a road that no longer exists. Reported as a count,
not a log line, so a test can assert it.

## 5. The tool

`FGuidelineDrawTool`, key `5`. Two clicks: pick a node, pick another, get an edge.

- Picks with an UNFILTERED nearest-node search. `RouteSearch::FindNearestNode` filters by
  traversal class and requires incidence, so it cannot see an isolated node — which is
  exactly the node you need to connect.
- Ctrl removes a hand-authored edge under the cursor. Derived edges are NOT removable this
  way: the next rebuild would put them straight back, which reads as the tool ignoring you.
- Right-click drops the pending first node.
- Refusals are typed — `SameNode`, `AlreadyJoined`, `NoStart` — and drawn at the cursor, the
  same vocabulary `RoadPlacement` uses, because "it didn't work" has different fixes.

The created edge is straight (`Control` at the midpoint, as the builder does for a straight
guideline), bidirectional, and admits every traversal class. A hand-drawn link exists because
the graph is missing a connection; guessing a narrower rule would make it silently useless to
whoever needed it.

Edits go through `ARoadNetworkActor` so undo works: `ConnectGuidelines(int32, int32)` and
`DisconnectGuideline(int32)`, both wrapped in `FRoadEditScope`.

## 6. Testing

- **The defect test goes green:** a hand edge's endpoints are still attached to derived
  geometry after a rebuild, and no two alive nodes share a position.
- It survives TWO rebuilds, not one — a re-resolution that worked once and left a stale
  `EndRef` would pass a single-rebuild test.
- Deleting the road under an endpoint kills the link and is counted.
- A link between two ANCHOR nodes (no `EndRef`) is untouched by re-resolution.
- The tool refuses a self-link and a duplicate, and reports which.
- Ctrl removes a hand edge; a derived edge is refused.
- Routing across a hand-drawn link succeeds — the point of the feature, asserted end to end
  rather than assumed from the graph shape.

## 7. Out of scope

- **The editor mode.** A new tool there needs a command, a palette entry, a `RegisterTool`,
  an enum value, a `MakeBuilder` case and a toolkit key mapping — six lists, in the module
  that has already shipped three separate "the list nothing reads" defects. The runtime
  driver is where this is being used; the ed-mode is a follow-up on its own.
- **Curved hand-drawn links.** `Control` is the midpoint. Curvature is a later edit gesture.
- **Per-class link rules.** Every class is admitted; narrowing needs a UI to say which.
