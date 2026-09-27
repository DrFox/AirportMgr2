# Shared pavement: one surface scale, stand admission, priced lines

2026-09-27. Status: spec approved 2026-09-27; open questions answered.

## Problem

1. **A stand admits by span alone.** `IcaoCode::StandAdmits(StandSpan, AircraftSpan)` is the
   whole of stand admission (`ArrivalPlanner.cpp:29`, `:112`). An F-sized grass stand would
   take an A380. The question a stand answers is "which aircraft may USE this", of which "does
   it fit" is one part.
2. **Runways already answer that question for surface, and the answer is runway-shaped.**
   `ERunwaySurface` (Grass < Tarmac < Concrete < Reinforced, strength by ordering - spec
   2026-09-07 §1), `FRunwayRequirements::MinimumSurface` on every aircraft type, and
   `RunwayAdmission::Judge`/`Describe`. A stand check written beside it would copy the
   comparison and the sentence.
3. **Price ignores surface everywhere.** `QuoteForRunway` is `Profile.CostPerMetre x length`;
   `FRunwayFacts` never reaches it, nor `DailyUpkeep`. `feature/road-grass-surface` adds
   `ERoadSurface { Tarmac, Grass }` and `BuildCost::GrassRateFactor` for roads only, applied in
   `ForSegment` and segment upkeep - a second scale and a factor per buildable kind.

## Decisions (user, 2026-09-27)

- **Rename** the runway surface types to pavement types; one scale for runways, roads,
  taxiways and stands.
- **Share the comparison, not the judge.** Runway and stand admission stay separate functions;
  the surface check and its sentence are one value type both hold.
- **Grass stands are cheaper**, by the same mechanism runways and roads use - one factor per
  pavement, applied in one place for every buildable.
- **Factors:** Grass 0.4 (the grass branch's figure), Tarmac 1.0, Concrete 1.4, Reinforced 1.8.
  First guesses; tuned in the one table.
- **Order:** `feature/road-grass-surface` lands first; this branch then folds `ERoadSurface`
  into `EPavement`.
- **Services on grass:** every role works on every pavement today. Leave a consumed hook, not
  a restriction.
- **Existing stands:** none but the one on `M_Starter`, which is deleted. No stand migration.
- **Stand pad upkeep is billed by area and pavement** (was: the definition's flat
  `UpkeepPerDay` only). "Per type" read as per PAVEMENT type - see §4.
- **Code A stands merge into B.** The smallest stand is B's floor, 50 x 39.5 m; what used to be
  the A/B choice is now the surface choice. Code A remains an AIRCRAFT letter (wingspan,
  runway width) - see §3a.
- **HELD:** build proposals (each tool builds an `FXxxProposal` with `Quote()` and the click
  places that same proposal, retiring `QuoteForConnect`/`QuoteForRunway` from
  `IRoadEditTarget`). Not in this change. The priced line below is its prerequisite and
  unblocks the pavement work without it.

## 1. `Model/Pavement.h` - the scale and the comparison

```cpp
UENUM(BlueprintType)
enum class EPavement : uint8 { Grass, Tarmac, Concrete, Reinforced, Count UMETA(Hidden) };

/** One surface comparison with its two figures - FRunwayAdmission's rule for Describe. */
USTRUCT()
struct AIRSIDE_API FPavementCheck
{
    GENERATED_BODY()
    UPROPERTY() EPavement Have = EPavement::Tarmac;   // the ground
    UPROPERTY() EPavement Need = EPavement::Grass;    // the aircraft
    bool Passes() const { return Have >= Need; }
};

namespace Pavement
{
    AIRSIDE_API FPavementCheck Judge(EPavement Have, EPavement Need);
    /** "the surface is grass; this aircraft needs tarmac". Empty when it passes. */
    AIRSIDE_API FString Describe(const FPavementCheck& Check);
    AIRSIDE_API const TCHAR* Name(EPavement P);        // was RunwaySurfaceName
    AIRSIDE_API int32 MaterialSlot(EPavement P);       // was RunwayMaterialSlot
    /** Build AND upkeep multiplier on an authored rate. The one table. */
    AIRSIDE_API double RateFactor(EPavement P);
}
```

- **Pattern: Value Object.** The rule lives on the value both judges hold. A Specification
  chain (one rule object per check) was rejected: each refusal carries different figures for
  its sentence (length vs field length, span vs strip, letter vs letter), USTRUCT plans cannot
  hold polymorphic rules, and two facilities with six rules do not pay for it. Revisit at a
  third facility or when rules become player-visible data; this type becomes its first rule.
- **Tarmac is not the zero value** (Grass is, for ordering). Every field that stores one
  defaults explicitly to `Tarmac`, as `FRunwayFacts::Surface` does today.
- `RunwayMaterialSlotCount` becomes `PavementMaterialSlotCount`.

## 2. Rename and move

| Was | Becomes |
|---|---|
| `ERunwaySurface` | `EPavement` (`+EnumRedirects` in `Config/DefaultEngine.ini` - `FRunwayFacts::Surface` is saved on segments in .umaps) |
| `RunwaySurfaceName`, `RunwayMaterialSlot` | `Pavement::Name`, `Pavement::MaterialSlot` |
| `FRunwayRequirements::MinimumSurface` | `UAircraftType::MinimumPavement`, carried on `FAirframe::MinimumPavement` |
| `ERoadSurface` (grass branch) | deleted; road segments store `EPavement` |
| `RoadSurfacePavement`, `RoadSurfaceName` (grass branch) | deleted - identity now |
| `BuildCost::GrassRateFactor`, `SurfaceRateFactor` (grass branch) | `Pavement::RateFactor` |

- **`MinimumPavement` leaves the runway struct** because it is no longer a runway need; a
  stand check reading a field named for runways is the drift this change removes.
- **Redirects cannot move a property between structs.** `FRunwayRequirements` keeps
  `MinimumSurface_DEPRECATED`; `UAircraftType::PostLoad` copies it to `MinimumPavement` when the
  latter is unset; the nine `DA_Aircraft_*` assets that store it are resaved (force-save - see
  memory on headless saves), then the deprecated field is deleted in the same PR. Verified by
  grepping the resaved .uassets for `MinimumPavement`, with a control.
- **One scale, per-buildable offer.** The grass branch's reason for a two-step enum - the road
  tool must not offer a concrete service road - becomes data: `URoadProfile::AllowedPavements`
  (`TArray<EPavement>`, empty = all four). The tool's surface row steps through that list. Road
  and taxiway profiles author `{Tarmac, Grass}`; runway profiles leave it empty. Stands offer
  all four and carry no list (their definitions are transient, one per letter - nothing to
  author it on; YAGNI until a stand kind needs fewer). All three tools build the row through
  one helper, `Pavement::AppendAxis`. A setter asked for a pavement the
  buildable does not offer refuses and logs, never clamps.

## 3. Stand admission - `Model/StandAdmission.h`

```cpp
UENUM() enum class EStandRefusal : uint8 { None, Surface, TooSmall, Service };

USTRUCT()
struct AIRSIDE_API FStandAdmission
{
    GENERATED_BODY()
    UPROPERTY() EStandRefusal Why = EStandRefusal::None;
    UPROPERTY() FPavementCheck Pavement;
    UPROPERTY() double StandDesignSpan = 0.0;   // what the stand was sized for, uu
    UPROPERTY() double Wingspan = 0.0;          // the aircraft's
    UPROPERTY() EServiceRole RefusedRole = EServiceRole::Aircraft;
    bool IsAdmitted() const { return Why == EStandRefusal::None; }
};

namespace StandAdmission
{
    AIRSIDE_API FStandAdmission Judge(const FEntityInstance& Stand, const FAirframe& Airframe);
    AIRSIDE_API FString Describe(const FStandAdmission& Admission);
    /** May a vehicle of Role work on this pavement? True for every pair today. */
    AIRSIDE_API bool PavementAdmitsRole(EPavement P, EServiceRole Role);
}
```

- **Order, first wins:** Surface, TooSmall, Service. Surface first for runway admission's
  reason - redrawing the stand bigger does not fix it.
- **TooSmall is `IcaoCode::StandAdmits`**, called, not re-implemented. `StandRank` stays where
  it is; ranking is not admission.
- **Service is the hook the user asked for, and it is CONSUMED:** Judge walks the stand's
  `ResolvedAnchors` roles through `PavementAdmitsRole`. It never refuses today. Restricting a
  role on grass later is one function body; the test below goes red when that happens, which
  is the point.
- **Stand fact:** `FEntityInstance::Pavement` (`EPavement`, default `Tarmac`), written at
  placement from the stand tool's surface row, copied into `FStandSummary` for capability.
- **Callers:** `ArrivalPlanner.cpp`'s two `StandAdmits` sites become `StandAdmission::Judge`;
  the plan's refusal text reaches `Describe`. Any other `StandAdmits` caller outside
  `IcaoCode.cpp`/`StandAdmission.cpp` is a lint failure (§6).
- **Stand tool:** gains a surface row on the variant popout (#353's mechanism), stepping
  `AllowedPavements`. The stand's pad draws with `Pavement::MaterialSlot` - the runway slots.
- `RunwayAdmission::Judge`'s Surface branch becomes `Pavement::Judge`, and its `Describe` case
  forwards to `Pavement::Describe`. `ERunwayRefusal` is unchanged.

## 3a. Code A stands merge into B

- **`IcaoCode::StandLetterFor(EIcaoCode Aircraft)`** - the ONE alias: A -> B, every other
  letter unchanged. "The stand letter an aircraft of this letter parks on." Every stand-size
  question an aircraft letter asks goes through it (`StandWidthForLetter`,
  `StandDepthForLetter`, `MaxStandWidthForLetter`, `ResolveStandDefinitionFor`).
- **`LetterForStandSize` never answers A.** Its backwards walk stops at the smallest STAND
  letter, B; below B's floor it answers empty, as below A's did.
- **The size gate** (`RoadEditFacadeSurfaces.cpp:665`, today `EIcaoCode::A`'s floor) reads
  `IcaoCode::SmallestStandLetter()` (= `StandLetterFor(A)`), not a literal letter.
- **Row A's stand-SIZE columns go** (`StandDepth`, `TowLaneWidth`, `AftEdgeAllowance`) -
  figures nothing can read are the drift the table's own header warns about. Omitted from the
  designated initialiser they are 0, and `StandDepth > 0` is the row's "has stands" test
  (`HasStands(Row)`, one helper). `StandTurnRadius`, `WingFwd` and `WingAft` STAY: they describe
  the aircraft's geometry and are read for A aircraft by AnchorLink and the envelope floors. A stand for an A aircraft is
  B's stand, sized and served for the utility tow exactly as B's is today.
- **Admission is unchanged in shape:** an A aircraft on a B stand is a small airframe on a
  larger stand, which is already admitted (`StandAdmits` compares by letter, smaller passes).
- **Tests:** the A-band assertions in `Airside.Solve.StandWidthIsDerivedFromClearance`
  (IcaoCode.h's `ENFORCED BY`) are rewritten to assert the merge: `LetterForStandSize` returns
  B at B's floor and empty just under it, for every width; no rectangle reads as A. Every other
  test naming an A stand (`StandBoxTest`, `StandDesignVehicleTest`, `StandPlotPlacementTest`,
  `StandPlotToolTest`, `ServiceLinkTest`, `AirsideContentTest`, `StarterMapProbeTest`) is moved
  to B or deleted if it only duplicated B's case - each named in the PR.
- `IcaoCode.h`'s "CODE A'S BAND IS EMPTY" paragraph is replaced by the merge's reason and date.

## 4. Priced lines - `Model/BuildPurse.h`

```cpp
enum class EBuildUnit : uint8 { Metre, SquareMetre, Each };

struct FBuildLine
{
    TWeakObjectPtr<const UObject> Source;   // profile / definition; null for a bare apron
    EBuildUnit Unit = EBuildUnit::Each;
    double Quantity = 0.0;                  // m, m², or count
    double RatePerUnit = 0.0;               // authored
    TOptional<EPavement> Pavement;          // unset: not ground
    double Amount() const;                  // Quantity * Rate * RateFactor(Pavement or 1)
};

struct FBuildQuote
{
    TArray<FBuildLine> Lines;
    FText What;
    double BaseAmount() const;              // sum of Amount()
    bool IsFree() const { return BaseAmount() <= 0.0; }
};
```

- **The factor is applied in exactly one function, `FBuildLine::Amount`.** Every buildable is
  lines: road/taxiway/runway = one Metre line with its pavement; stand = one Each line (the
  definition) + one SquareMetre line (the pad) with its pavement; apron = one SquareMetre line.
  A grass runway, road, taxiway or stand is cheaper with no kind-specific code.
- **Only `BuildCost` constructs lines.** `ForSegment(Profile, Length, EPavement)`,
  `ForApron(Outline, Rate, TOptional<EPavement>)`, `ForEntity(Definition)`. uu-to-metres stays
  there, once.
- **Upkeep reads the same factor.** `DailyUpkeep` multiplies each segment's upkeep by
  `Pavement::RateFactor` of its pavement - a runway's `FRunwayFacts::Surface`, a road's
  segment pavement - so build and upkeep cannot disagree (BuildCost.h's rule).
- **A stand's pad enters upkeep:** `PolygonAreaSquareMetres(Stand.Outline) x
  ApronRatePerSquareMetrePerDay x RateFactor(Stand.Pavement)`, beside the definition's flat
  `UpkeepPerDay` (kept - it is the stand's equipment, not its ground). The same area function
  the pad's build line used, so the two cannot measure one pad differently. A bare apron has no
  pavement and bills at factor 1, as today.
- **Pricing stays kind-blind.** `ULedger` prices per line:
  `Sum(Pricing->PriceOfBuild(Line.Amount(), Line.Source))`, and the same for `ScrapValue`.
  `UPricing`'s signature is unchanged. A stand's pad line now reaches pricing with its own
  (null) source rather than being folded into the definition's - `QuoteStand`'s hand-summed
  `BaseAmount` goes.
- **`QuoteForRunway` gains an `EPavement`**; the runway tool passes its surface row's value.
  Retiring it is the HELD proposal work.
- Unchanged for today's content: every existing runway and road is Tarmac (factor 1.0), so no
  existing price moves. A test pins that.

## 5. M_Starter

- Delete the one stand on `M_Starter.umap` (headless level edit - see memory: a locked .umap
  reports success and writes nothing; grep the .umap afterwards, with a control).

## 6. Enforcement (Check-Architecture)

- **No second surface scale:** a `UENUM` whose name ends in `Surface` or `Pavement` outside
  `Model/Pavement.h` fails. Catches `ERoadSurface` coming back.
- **One factor site:** `RateFactor(` called outside the .cpp defining `FBuildLine::Amount`, `Model/Pavement.cpp`
  and `Build/BuildCost.cpp` (upkeep) fails.
- **One stand admission:** `IcaoCode::StandAdmits(` outside `IcaoCode.cpp` and
  `StandAdmission.cpp` fails.

## 7. Tests

- `Airside.Model.Pavement.OrderIsStrength` - `Judge` passes iff Have >= Need, all 16 pairs.
- `Airside.Model.Pavement.DescribeMatchesRunwaySentence` - the runway refusal text is
  byte-identical before and after (captured from main).
- `Airside.Model.StandAdmission.GrassStandRefusesTarmacAircraft` - F-sized grass stand, an
  aircraft needing tarmac: refused for Surface, not admitted by size.
- `Airside.Model.StandAdmission.SurfaceBeatsSize` - too small AND too soft reports Surface.
- `Airside.Model.StandAdmission.EveryRoleWorksOnEveryPavement` - pins today's hook; goes red
  by design when grass restricts a role.
- `Airside.Model.StandAdmission.ArrivalUsesIt` - composition level: an arrival is not assigned
  a grass stand it needs tarmac for, through `ArrivalPlanner`, not the judge directly.
- `Airside.Build.BuildCost.FactorOnEveryKind` - grass segment, grass runway, grass stand pad
  each price at 0.4 of tarmac; a building line ignores pavement.
- `Airside.Build.BuildCost.UpkeepUsesBuildFactor` - grass runway upkeep is 0.4 of tarmac.
- `Airside.Build.BuildCost.StandPadUpkeepByArea` - two stands of one letter, one grass, one
  tarmac, same outline: pad upkeep differs by exactly the factor; doubling the area doubles it.
- `Airside.Build.BuildCost.TarmacPricesUnchanged` - figures captured from main for a road,
  a runway and a stand.
- `AirportOps.Ledger.PricesPerLine` - a two-line quote charges the sum of per-line prices.
- `Airside.Content.AircraftMinimumPavementMigrated` - every `DA_Aircraft_*` has the value its
  old `MinimumSurface` held (list captured from main before the resave).
- Tool: the road tool's surface row offers exactly `{Tarmac, Grass}`; the stand and runway rows
  offer all four.

## Refactor contract

The rename and the line change are refactors inside a feature PR: `UE_LOG(` count and comment
lines in touched files do not fall; `RunwaySurfaceName` etc. keep no forwarders (no Blueprint
references them - checked by grep of Content before deleting; forwarders added if any hit).

## Out of scope

- Build proposals (HELD, above).
- Restricting any service role on grass (hook only).
- A stand pad's UPKEEP: today a stand bills only `UEntityDefinition::UpkeepPerDay`, not its pad
  area, so a grass stand is cheaper to build and the same to run. Unchanged here; see question 1.
- Merging the A and B stand floors (question 2).

## Open questions

None. (2026-09-27: pad upkeep by area and pavement - §4; A merged into B - §3a.)
