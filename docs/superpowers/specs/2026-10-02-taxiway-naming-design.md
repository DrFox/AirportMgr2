# Taxiway naming - design

2026-10-02. Approved in conversation section by section; owner AFK for the spec review and asked
for implementation to continue.

## Why

Two consumers need a taxiway to have a name the player can read:

- **The space-time taxi planner** (next spec) states every plan as an ICAO-style clearance -
  "taxi to stand 14 via A, A3, B, hold short of C" - and shows what a holding aircraft waits for.
  Segment ids are unreadable.
- **Today's diagnostics**: the deadlock alert says "2 aircraft deadlocked - the layout needs
  another way round" without saying where. On a 40-stand airport (`M_ScaleGatwick`) the player
  cannot find it.

Naming is useful on its own, so it ships first and the planner uses names from day one.

## Real-world convention (ICAO Annex 14, Aerodrome Design Manual)

- Main taxiways are LETTERS (A, B, C...). I, O, X are avoided (read as 1, 0, closed-runway).
- Letter + number names the CONNECTORS off a lettered taxiway, in order along it (A1, A2...) -
  runway entries/exits and links. Not junctions.
- A junction is named by the taxiways meeting there ("A/B intersection").
- Stands are numbered (already done here: `FRoadEntity::StandNumber`).

## Rulings (owner, 2026-10-02)

1. Names are ASSIGNED AUTOMATICALLY AT DRAW TIME, STORED, STABLE, and RENAMEABLE - not
   re-derived from geometry (that would reshuffle letters on every edit, the reason stand numbers
   are never re-derived).
2. Map display v1 is SCREEN-SPACE LABELS; ground-painted names follow soon after as a look pass.
3. A taxiway is a SINGLE CHAIN. An edit that breaks that renames the split-off part (toast).
4. Naming ships before the planner, as its own spec.

## Data (Model/, saved with URoadNetwork)

```
FTaxiway
  int32   Id                    stable handle (slot list, like entities)
  FString Name                  "A", "AB", or an explicit override ("K7"); empty for a connector
                                that has not been renamed (its display name is derived)
  int32   ParentId              INDEX_NONE for a lettered taxiway; the parent for a connector
  int32   ConnectorNumber       1.. for a connector, 0 otherwise
  int32   NextConnectorNumber   counter for this taxiway's connectors, never reused while alive
FRoadSegment
  int32   TaxiwayId             INDEX_NONE on service roads and runways
```

- Runways keep their derived designators ("09L"); service roads are unnamed.
- A connector's DISPLAY name is `Parent.DisplayName + ConnectorNumber` unless it has its own
  `Name` override. So renaming A to K makes A1..A4 read K1..K4.
- `URoadNetwork::TaxiwayDisplayName(TaxiwayId)` is the ONE function that turns an id into text.

## Assignment rules - `URoadNetwork::AssignTaxiway`, called for every new taxiway segment

| Gesture | Result |
|---|---|
| Continue in line (within `ExitGeometry::InLineEndDegrees`, 10 deg) from the END of a named taxiway | inherits that taxiway |
| A new chain shorter than `ConnectorMaxLength` (~300 m, a UPROPERTY knob) with BOTH ends on a named taxiway or a runway | connector of the taxiway it leaves (first end); number = parent's `NextConnectorNumber++`. If it leaves a runway and lands on a taxiway, the taxiway is the parent |
| Anything else | next free letter |
| Insert a node / split a segment | both halves keep the taxiway |
| Two taxiways meet / run in line | each keeps its own; the junction reads "A/B" |
| A drag moves a connector's end off its parent | keeps its name (names never move on a drag) |

Letters: A..Z skipping I, O, X; then AA, AB, ... A letter returns to the pool only when NO
taxiway displays a name derived from it - a deleted parent with surviving connectors keeps its
letter reserved, or a reused "A" would mint a second "A1".

## The single-chain invariant - `URoadNetwork::NormaliseTaxiways`

Runs after EVERY topology change (draw, delete, merge, insert, load), in one place. Mutators do
not patch names themselves (issue #255: fixes at sites regress; fixes at shapes hold).

Invariant: every taxiway's segments form ONE connected chain with at most 2 of its segments at
any node (a closed loop is allowed). Names (display) are unique.

- **Branch** (3+ segments of one taxiway at a node, e.g. a merge of A's end onto A's middle):
  the shortest branch (by length, ties by lowest segment id) is split off as a new taxiway with
  the next free letter.
- **Disconnected** (a delete or merge leaves two pieces): the longer piece keeps the taxiway; each
  other piece gets the next free letter.
- **Empty** (last segment collapsed/deleted): the taxiway is removed; its letter is freed per the
  rule above.
- Each rename emits ONE event the game turns into a toast: "C split off from A".
  Deterministic: same network in, same names out.

## Backfill on load

A network saved before names (every map today, including `M_ScaleGatwick`) has segments with
`TaxiwayId == INDEX_NONE`. PostLoad groups unnamed taxiway segments into straight-through chains
(in-line within the same 10 deg window at nodes of degree 2, or across a junction when one
continuation is in line), then names them by the assignment rules, longest chain first, so the
long parallels get the early letters. A one-off, like `EnsureStandNumbers`; logs
`TaxiwayNames: backfilled N taxiway(s), M connector(s)`.

## UI

- **Labels**: `Model/TaxiwayLabels` computes anchors - one per taxiway at the middle of its
  longest segment, repeated every ~500 m (knob) on long ones - as MEANINGS (name + road-plane
  point), never colours (Tool/ rule). Both drivers draw them as fixed-pixel yellow-on-black tags:
  the PIE HUD (`ARoadBuildController` side) and the editor mode's `DrawHUD`. Shown while a build
  tool is lit; while watching, toggled with the existing Guidelines key (G). No new key.
- **Rename**: the Select tool also picks a taxiway segment. Inspector card "Taxiway A": length,
  connectors, Rename field. Valid: 1-3 characters, letters/digits, unique, upper-cased; refusals
  say why ("B is taken", "I, O and X are avoided: they read as 1, 0 and closed"). PIE only in v1.
- **First consumers**: the deadlock alert/log says where ("2 aircraft deadlocked on A3"); the
  aircraft inspector gains "On: A3".
- Undo/save: names are network state, so the existing network snapshot (undo Memento) and the
  save blob carry them. Verify, do not assume - a test undoes a rename and a split.

## Testing

World-free model tests `Airside.Model.TaxiwayNames.*`, one per rule above (assign, inherit,
connector, split/insert, collapse + letter reservation, branch, disconnect, in-line A/B, rename
propagation + connector override, I/O/X skip + Z->AA, undo, backfill determinism on a
Gatwick-shaped fixture).

- **Invariant test**: a seeded random edit sequence; after every mutator, each taxiway is one
  connected chain with <= 2 own segments per node and display names are unique. Measures the
  contract, not the sites.
- **Seam tests** (actor level): spawn `ARoadNetworkActor`, draw, assert the label list matches
  the names; Select-pick a taxiway and assert the card's title.
- **Lint**: Check-Architecture rule - `TaxiwayId` is written only inside `AssignTaxiway` /
  `NormaliseTaxiways` / the backfill.

## Delivery

Three stacked PRs, each usable alone:

1. Model: data, assignment, normalise, backfill, display name; deadlock + inspector text.
2. Labels in both drivers, G toggle.
3. Rename card + Select on taxiways.

Then (separate) ground-painted names.

**PIE check**: open `M_ScaleGatwick`; labels appear; the parallels read as letters and the stand
links as connectors; rename one; delete a middle segment and see the split toast.

## Out of scope

Holding-point names derived from connectors ("holding point A1"), ground paint, rename in the
editor mode, naming service roads.
