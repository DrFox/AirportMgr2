# Stand far-side entry, per-letter design vehicle - design

2026-09-26. Branch `feature/stand-entry-far-side` (worktree `C:\repos\airportmgr2-stand-entry`).

## Problem

1. **Code A and B stands cannot be built.** `BuildStandTemplate` sizes every letter's service
   bays for ONE vehicle, `UAirsideSettings::ResolveLargestServiceVehicle()` (the fuel truck:
   forward radius 502, reverse 355). The corner runs that implies are fixed per vehicle, not per
   letter, so on A/B's floors the contact band inverts (AirsideTests.log 2026-09-26: A needs
   8027 x 7161 against 3500 x 2000; B needs 4400 x 3452 against 4400 x 3000).
   `StandDefinitionCache` then returns null and `WhyStandRefused` says "Code %s stands cannot be
   built yet".
2. **Service vehicles enter on the taxiway side.** Every bay's road contact sits `Square`
   inside the entrance (tail) edge, and the entrance edge is laid on the taxiway. The service
   road therefore has to squeeze into an unchecked strip between the stand and the taxiway.

## Decisions (user, 2026-09-26)

- A letter's template is designed for the LARGEST vehicle that letter admits. A smaller vehicle
  may still serve a larger stand (inefficient, legal).
- A/B's design vehicle is the utility tow (utility1 + fuelTrailer1,
  `ResolveUtilityTowVehicle`); C-F's is the fuel truck (`ResolveDefaultVehicle`).
- Service vehicles enter and leave ONLY by the far edge - the one opposite the taxiway - on
  ALL letters.
- Bays stay reverse-in. Tows reverse since #354 (`FTowReverseRun`, `VehicleFit::JudgePlan`),
  and reverse-in needs less room than drive-through (Chassis.h, the 2026-09-16 rethink).
- A stand with no service road on its far edge is still placed, and is reported unserviceable
  until one is drawn.

## 1. Stand geometry

- **The box's slack moves ahead of the nose.** The entrance edge stays on the taxiway, but the
  tail now sits just inside it: template `BackX = -MaxTailAft`, front edge `BackX + Depth`.
  `StandBox::PoseFor` puts the stop mark `MaxTailAft` in from the entrance midpoint;
  `BoxAt` inverts it. `StandMarkingBuilder`'s entrance midpoint and `StandPlotTool`'s preview
  lead-in read the same offset through ONE `StandBox` helper (`EntranceSetback`), not their
  own copies of the formula.
- **Bays mirror in X.** Contact poses sit `Square` inside the FRONT edge, entry heading PI
  (facing aft), exit heading 0. Lane runs aft outboard of the wingtip; serve, reverse, depart
  back to the front contact. Same leg topology as today; reverse leg kept.
- **Design vehicle per letter.** `UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode)` is the
  one place it is decided. `BuildStandTemplate` takes an `FVehicle`. Forward radius from the
  chassis; reverse radius is the chassis's `TightestReversibleRadius()` for a rigid vehicle, and
  for a tow the tightest arc whose steady hitch angle (`TowReverse::SteadyHitchRadians`) stays
  inside `CriticalHitchRadians` by a named margin - whichever is larger.
- **Depth docs corrected** in `IcaoCode` to "tail to the far side of the service lane". Floor
  figures unchanged. If a letter still overflows, the `Stand template '%s' ... needs W x D` log
  line is the evidence and the figure goes back to the user; no floor is changed unilaterally.
- **Saved stands** keep their outline; the pose is re-derived from it on load (the letter is
  already re-read by `RebindStandDefinitions`). Old saves park nearer the taxiway - intended.

## 2. Dispatch and serviceability

- **Per-job vehicle.** `UFuelService::TruckVehicle` is replaced by a per-job
  `ResolveStandDesignVehicle(StandLetter)`. With depot fleets still untyped counts, the design
  vehicle is the most efficient that fits. Typed fleets and smaller-into-larger dispatch are out
  of scope; the rule is written as "no larger than the design vehicle" so they slot in later.
- **Size ranking.** `VehicleFit::NoLargerThan(A, B)`: widest body, forward radius, reverse
  radius, chain length all <=. One consumer today: a guard in `ChooseDepot` refusing with new
  `EFuelRefusal::VehicleTooLarge` (own `RefusalText`).
- **Unserviceable flag.** `FStandFacts::bServiceable`, derived like `bReachable`: every bay
  entry node `IsServiceNodeConnected`. Inspector: "Service road: joined / not joined - draw a
  service road along the far edge". No stored state. The existing `AnchorLink` "joins nothing"
  warning stays; a new Warning covers the partial case (some entries joined, others not), which
  is silent today.
- **Placement never refuses for a missing road.** The plot tool's ghost draws the far edge
  with a new `IToolPreviewSink` MEANING (`ServiceEdge`), colour chosen only in `PreviewPalette`.

## 3. Testing

Solve/Model first, failing test before fix.

- Solve: `StandBox.TailAtEntrance`; `RoundTrip`/`TailToEntrance`/`FarSide` updated.
- Entities: `StandLayoutFitsItsLettersFloor` over all six letters; new
  `EveryBayContactIsOnTheFarEdge`; `EveryTemplateLegIsDrivableByEveryVehicle` becomes "by its
  letter's design vehicle and every vehicle no larger", tows through `JudgePlan` (reverse leg
  included); `NoTemplateLegPassesUnderTheWing` kept.
- Content: `ResolveStandDesignVehicle` A/B tow, C-F truck; `NoLargerThan` orders tow < truck.
- Present: `StandPlot.PlacesOtherLetters` includes A/B; `UnfitLetterRefused` retired or turned
  into "every letter builds"; `StandPlotToolTest.cpp` `bBuildable = Letter >= C` becomes all
  letters; new `ServiceRoadOnFarEdgeJoins`, `NoServiceRoadIsUnserviceable`.
- Ops: tow to A/B, truck to C; `VehicleTooLarge` refusal; end-to-end A stand fuelled by the tow,
  aircraft not logged UNFUELLED; a composition-level test that fails if `FuelService` still
  dispatches a single fixed vehicle.
- PIE verification: A, B, C stands with far-edge roads; `Stand template 'A' ... needs` within
  floor; FuelService dispatches `UtilityTow` to A; screenshot of entry from the far edge.

## Risks

- A/B may still overflow with the tow's reverse radius - measured by the template log, not
  assumed.
- The C template is an authored asset (`StandDefinition` on the actor); its bays must be rebuilt
  by `BuildCodeCStandFor`, and any authored overrides in the .uasset re-checked.
