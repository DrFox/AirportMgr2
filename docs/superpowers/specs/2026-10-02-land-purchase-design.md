# Land purchase: owned tiles, the Land tool, and an edge that grows

Date: 2026-10-02. Status: agreed 2026-10-02. Builds on #529 (the diorama edge, main 28b96d99),
which made one rectangle - `AAirsideOwnedLandActor` - clip the ground, lay the strata walls,
bound the build camera and keep grass off the void. This replaces the rectangle with owned TILES
the player buys.

## 1. Rulings (user, 2026-10-02)

| # | Ruling |
|---|---|
| R1 | Land is bought in **grid tiles of 600 m**. 250 m was "meaningless": a 500 m runway is the least anything useful lands on, and a step must be worth taking. |
| R2 | The map is **8 x 8 tiles (4.8 km)**. Room for a 3.3 km (Gatwick-length) runway in any orientation. |
| R3 | A new game owns **1 x 2 tiles (600 x 1200 m) at the bottom centre** of the grid. |
| R4 | A tile is **buyable when unowned and edge-adjacent to owned land** (not diagonal). |
| R5 | **Price rises per tile owned**: `Base x (1 + K x TilesOwned)`. Base and K are content. |
| R6 | Bought with a **tool on the bar**: buyable tiles show as ghosts past the edge, with their price; click buys. |
| R7 | **Building past owned land is refused, and says why** ("Outside your land"). |
| R8 | **Approach paths are unconstrained.** Only ground geometry needs owned land; aircraft fly over the void. |
| R9 | **One off-map road link**, fixed on the start tiles' bottom edge; the player can move it and add more. |
| R10 | The purchase **goes on the ops event bus**. |

## 2. Shape

```
AirportOps (depends on Airside)            Airside (cannot see AirportOps)
---------------------------------          -----------------------------------------
UOpsRuntime::BuyLandTile(Tile)             URoadNetwork::OwnedLand  (FLandGrid, saved)
  refuse? -> FLandRefusedTileEvent           IsOwned / IsBuyable / Outline / TilePrice
  charge Ledger                                         |
  Network->BuyTile(Tile) ------------------>  URoadEditFacade::BuyTile -> OnOwnedLandChanged
  publish FLandPurchasedEvent                           |
    -> toast, ledger row (Presentation)       AAirsideOwnedLandActor (walls + MPC)
                                              AAirsideGroundCoverActor (mask)
                                              UBuildCameraComponent (focus clamp; game module)
                                              WhyXRefused guards (R7)
```

**THE TILES LIVE IN AIRSIDE'S MODEL, NOT IN OPS.** The refusal (R7) is an Airside tool's preview
and an Airside mutator's guard, and Airside may not include AirportOps. Ops owns the MONEY and the
announcement, never the tiles - the same split as building a road: the facade mutates, Ops charges
and publishes. Rejected: an `IOpsPersistent` land object with its own save blob. It saves without a
version bump, but Airside could not read it, so R7 would need a second copy pushed across.

**THE BUS IS NOT THE AIRSIDE LISTENERS' CHANNEL.** Airside cannot subscribe to `FOpsEventBus`. Its
listeners (walls, clip, grass, and the camera via the same delegate) hear
`URoadEditFacade::OnOwnedLandChanged` - a native multicast, like `ARoadNetworkActor::OnNetworkChanged`.
The bus carries the ops fact (`FLandPurchasedEvent`: tile, price, balance) for the toast, the ledger
and anything ops-side later (airline reactions, a "land" alert). One publisher each; the delegate
fires in the mutator, the bus event in the command that called it.

## 3. Model: `FLandGrid` (Airside `Model/`, world-free, tested first)

```cpp
USTRUCT() struct FLandGrid
{
    UPROPERTY() FVector2D Origin;        // south-west corner, uu
    UPROPERTY() double TileSize = 60000; // R1
    UPROPERTY() int32 Columns = 8, Rows = 8;  // R2
    UPROPERTY() uint64 Owned = 0;        // bit (Row * Columns + Column); 64 = R2's 8x8 exactly
    bool IsValid() const;                // Columns*Rows in 1..64, TileSize > 0
    bool IsOwned(FVector2D Point) const; // edge points: owned only if BOTH sides are (inclusive-covered rule)
    bool IsTileOwned(FIntPoint) const;
    bool IsBuyable(FIntPoint) const;     // R4
    int32 NumOwned() const;
    FBox2D TileBox(FIntPoint) const;
    TArray<FLandEdgeRun> Outline() const;   // boundary edges merged into straight runs, outward normal
    FVector2D ClampToOwned(FVector2D) const; // nearest owned point - camera (an L must not let the view sit over the void)
};
```

- **An INVALID grid owns everything** - every map before this, and M_Test & co. Nothing clips,
  bounds, walls or refuses. This keeps #529's "no actor, no edge" contract.
- **`uint64` and 8x8 are one fact.** A bigger grid is a different mask type; `IsValid` refuses
  `Columns * Rows > 64` and a `static_assert` ties the bit count to it.
- **Saved on `URoadNetwork`** beside `Nodes`/`Segments` (RoadNetwork.h:1229-1307): it rides the
  existing "Network" blob, so save/load costs nothing. No migration (no player saves yet).

### 3.1 Undo must not un-buy land

`URoadEditHistory` is a Memento of the whole network. A purchase inside it would let Ctrl+Z take the
land back and keep the money gone, or with a refund, make land free to "try". **The owned mask is
excluded from the memento**: restore copies every field but `OwnedLand`. A test buys a tile, undoes
an earlier road, and asserts the tile is still owned. (Rejected: purchase clears the undo stack -
punishes buying land mid-layout for no reason.)

## 4. The edge follows the tiles (`AAirsideOwnedLandActor`)

The actor stops holding a rectangle. It holds the **starting grid** for its level (origin, size,
start tiles) and, at BeginPlay, seeds `URoadNetwork::OwnedLand` from it **only if the network's grid
is invalid** - a loaded save's tiles win. Then it listens to `OnOwnedLandChanged`.

- **Walls**: one per `Outline()` run, pooled components (≤ 8x8 tiles => at most 4 x 32 runs; ~10 in
  practice). Placement test from #529 generalises: every run's outer face lies on its edge.
- **Clip**: `MPC_OwnedLand` becomes Origin.xy, TileSize, and the mask as **four 16-bit floats**
  (Mask0..Mask3; a float holds integers to 2^24 exactly). The material finds the pixel's tile,
  picks the float, `fmod(floor(m / 2^bit), 2)`. Outside the grid: clipped. No render target, no
  texture to keep in sync. "Everything" = an invalid flag scalar `GridValid = 0`.
- **Camera**: `FBuildCameraRig::FocusBounds` (a box) becomes a clamp function the component sets
  from `FLandGrid::ClampToOwned`, re-read on `OnOwnedLandChanged`.
- **Grass**: `FGroundCoverMask::SetLand(FLandGrid)`; mask rebuilt on `OnOwnedLandChanged`.

The rectangle API #529 added (`OwnedMin/Max`, `SetOwnedLand`, `GetOwnedLand`) is REPLACED, not kept
beside: two descriptions of owned land is the drift #529 existed to prevent. `M_Diorama` is re-authored
by `build_diorama_prototype.py` to an 8x8 grid with the R3 start.

## 5. Refusal (R7)

`EBuildRefusal` gains `OutsideOwnedLand` (BuildPurse.h:100 - its comment asks for exactly this).

- **Preview**: each `Why*Refused` adds "Outside your land" when any footprint point of the proposed
  geometry is not owned: `WhySegmentRefused` (roads, taxiways), `WhyStandSiteRefused`,
  `WhyPlotRefused`, `WhyUpgradeSiteRefused`. **Runways and aprons have no `Why*` today**; this adds
  `WhyRunwayRefused` and `WhyApronRefused` so the ghost can go red before the click, which is the
  rule everywhere else.
- **Commit**: the mutators (`ConnectNodes`, `PlaceRunway`, `AddApron`, `PlaceEntity*`,
  `PlaceStandInPlot`, `PlaceNode`) refuse through one `OwnedOrRefuse` beside `AffordOrRefuse`,
  broadcasting `OnRefused(OutsideOwnedLand)` -> `FBuildRefusedEvent` -> toast.
- **Footprint, not centreline.** A taxiway whose centre is inside but whose shoulder crosses the cut
  would hang over the void. The test is on the surface polygon the solver produces, sampled at its
  vertices plus edge midpoints. Check-Architecture: extend rule 32 so a mutator calling
  `AffordOrRefuse` without `OwnedOrRefuse` fails.

## 6. The Land tool (R6)

- Registered in `ToolRegistry()` (BuildSession.cpp:18) as **"Buy land"** - not "Land", which is
  already the `aircraft.land` row's label (BuildActions.cpp:355). **Key: `T`** (free; 0-9 are all
  taken). Rows needed: ToolRegistryRulings.h, `fetch_ui_icons.py` (`tool.buy land`).
- While lit: every buyable tile draws as a ghost slab past the edge (meaning `Buyable`, via
  `IToolPreviewSink` - the plugin names meanings, not colours), its price as a label at its centre;
  the hovered tile highlights; an unaffordable one shows `CannotAfford`. Click = `BuyLandTile`.
- Unowned, unbuyable tiles are not drawn: the void stays void.
- A destructive-gesture concern does not apply (buying adds); still a deliberate mode per R6.

## 7. Price (R5)

`UScenario::LandTileBase` and `LandTileGrowth` (K), resolved in the one Ops resolver.
`TilePrice = Base x (1 + K x NumOwned())`. Starting guesses, to tune in play: Base ¤150,000,
K 0.5 - the first purchase ¤300,000 with 2 owned (StartingBalance is ¤500,000), the 10th (11 owned) ¤975,000. Corrected 2026-10-02: the first draft said ¤450,000, an arithmetic slip.
`FLandPurchasedEvent { Tile, Price, Balance }`; the ledger posts it under a new `Land` category.

## 8. The off-map road link (R9) - deferred, recommendation

**Nothing consumes it yet.** The GDD's link exists for contract deliveries (GDD:195-245), and no
delivery traffic exists in code. A link node with no reader is the "declared, never consumed" shape
CLAUDE.md warns about. Recommendation: record R9 here, and build the link WITH deliveries - it is
then a road node flagged `MapLink`, pinned to the owned boundary, seeded on the start tiles' bottom
edge (R3 puts that edge on the map's bottom, which is why bottom centre works), movable along the
boundary and addable. Land purchase makes nothing about it harder: a link on a boundary that grows
outward simply stops being on the boundary, which the deliveries spec must rule on (follow the edge,
or stay put as an inner road).

## 9. Tests (written first where the logic is in Model/)

- `Airside.Model.LandGrid.*`: IsOwned incl. edges and corners; IsBuyable adjacency (not diagonal,
  not off-grid); Outline of an L and of a hole-free U is the expected runs; ClampToOwned on an L.
- `Airside.Model.LandGrid.SurvivesUndo` (3.1), `.SavedWithNetwork` (round-trip through the blob).
- Refusal: each `Why*` says "Outside your land" for a shoulder crossing the cut; each mutator
  refuses and broadcasts. Rule 32 extension red without `OwnedOrRefuse`.
- Seams, at the composition: buy a tile -> walls re-laid, MPC written, grass mask rebuilt, camera
  clamp moved (one test each, per the refactor contract's "every seam gets a test").
- Ops: `BuyLandTile` refusals (not adjacent, owned, can't afford), charge, event published once.
- Content: `OwnedLandWired` grows to the new MPC names.

## 10. Out of scope

Selling land; non-rectangular maps; terrain under the void; the off-map link (§8); land the player
starts NOT at bottom centre (scenario choice later).

## 11. Resolved (user, 2026-10-02)

1. Off-map link deferred to the deliveries feature (section 8).
2. Tool "Buy land", key `T`.
3. Price starts at Base 150,000, K 0.5.
