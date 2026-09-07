# Runway categories and markings — design

Date: 2026-09-07. Status: written for review. Follows the exit arcs (PRs #55, #57, #58) and
holding positions (#56). Precedes M3.

## 1. What this is

A runway today is a road profile with the continuous flag, an exit length and one of five
ICAO widths, painted by the road material with a taxiway's yellow centreline. This spec
makes a runway state what it IS, makes an aircraft state what it NEEDS, derives the paint
from the first and the admission from the comparison, so that neither the markings nor
M3's offers ever need a second classification.

Decided in conversation (2026-09-07):

- The category is physical facts on the runway, read by the paint now and by the rules
  later (not a gameplay tier, not paint only).
- The aircraft carries its own requirements: the weakest surface it may use, the approach
  it needs, and its published take-off and landing field lengths.
- Pavement strength is the surface scale itself: grass < tarmac < concrete < reinforced.
- Displaced thresholds, stopways, clearways and per-direction declared distances are OUT,
  explicitly (2026-09-07). The admission function is written to take a threshold and a
  length so they can be added per end later without changing its callers.

## 2. Vocabulary

```cpp
/** Ordered: strength as well as look. An aircraft names the weakest it may use. */
UENUM() enum class ERunwaySurface : uint8 { Grass, Tarmac, Concrete, Reinforced };

/** Ordered: what the approach aids support. An aircraft names the least it needs. */
UENUM() enum class ERunwayApproach : uint8 { Visual, NonPrecision, Precision };
```

## 3. Where the facts live

**3.1 The runway.** `FRunwayFacts { ERunwaySurface Surface = Tarmac; ERunwayApproach
Approach = Visual; }` as a `UPROPERTY` on `FRoadSegment`, persisted with the level. Set by
the runway tool at placement; every segment of one strip carries the same facts and
`URoadNetwork::RunwayFactsFor(Seed)` reads them off the chain (any member; the tool writes
them all, and a split copies them). Width stays the profile's; length stays the chain's.

Not on new profile assets: four surfaces by three approaches by five widths is sixty
assets, and a runway with a non-asset profile reloads as a taxiway (PR #54's lesson). The
profile remains the cross-section; the facts are the runway's.

**3.2 The aircraft.** On `UAircraftType`, copied into `FAirframe` at dispatch like the
performance structs (one bundle, PR #54's rule):

```cpp
USTRUCT() struct FRunwayRequirements
{
	UPROPERTY(EditAnywhere) ERunwaySurface  MinimumSurface = ERunwaySurface::Grass;
	UPROPERTY(EditAnywhere) ERunwayApproach ApproachNeeded = ERunwayApproach::Visual;
	/** Published take-off field length, uu. Admission figure, not the physics' roll. */
	UPROPERTY(EditAnywhere) double TakeoffFieldLength = 0.0;
	/** Published landing field length, uu. */
	UPROPERTY(EditAnywhere) double LandingFieldLength = 0.0;
};
```

The Piper fallback: Grass, Visual, take-off 80000, landing 80000 (about the POH's
50-ft figures, generous). The physics keeps its derived rolls for MOTION;
`Airside.Model.FieldLengthsCoverTheRoll` pins `RequiredRoll <= TakeoffFieldLength` and
`RequiredLandingDistance * LandingMargin <= LandingFieldLength` for every aircraft the
content set resolves, so a published figure can never be shorter than what the model
actually needs.

**3.3 Admission.** One function, Model/:

```cpp
UENUM() enum class ERunwayRefusal : uint8 { None, Surface, Approach, TooShort, TooNarrow };

namespace RunwayAdmission
{
	/** Why Airframe may not use this strip for Landing or take-off, or None. */
	AIRSIDE_API ERunwayRefusal Check(const URoadNetwork&, FRoadSegmentId Seed,
		const FAirframe&, bool bLanding);
	AIRSIDE_API FString Describe(ERunwayRefusal);
}
```

Length: the chain's length against the field length for the operation. Width: the
airframe's wingspan against the profile's `MaxWingspan` where declared, else the ICAO
code for the width (18 m: A, 23 m: B, 30 m: C, 45 m: D/E, 60 m: F), so a 36 m airliner is
refused a 23 m strip.

`ArrivalPlanner::Plan` and `DeparturePlanner::Plan` call it first and refuse with a new
reason each (`EArrivalRefusal::NotAdmitted`, `EDepartureRefusal::NotAdmitted`) carrying the
`ERunwayRefusal`; `DescribeRefusal` names it ("the surface is grass; this aircraft needs
tarmac"). Their existing `RunwayTooShort` checks stay as the physics backstop. The Land key
and the Route tool show the text as they show refusals today.

## 4. The paint

**4.1 A runway marking builder** (`FRunwayMarkingBuilder`, Build/) beside the holding-
position one, emitting quads per runway chain into a SECOND marking component drawn with
a dynamic instance of the road material whose `MarkingColor` is white - the same mesh trick
(UV1 = 0 paints the whole quad), a different colour, no new material asset. Everything is
derived from the chain's threshold, direction, length, width, surface and approach:

| Marking | Rule (ICAO Annex 14 vol. I, scaled to the widths we have) | Surface / approach |
|---|---|---|
| Designation | `RunwayDesignator::Designate` at each end, digits 9 m tall from a stroke font of quads (0-9, L, C, R), centred, 6 m past the threshold stripes | every paved runway |
| Threshold stripes | 30 m long, 1.8 m wide, starting 6 m from the threshold; count by width: 18 m: 4, 23 m: 6, 30 m: 8, 45 m: 12, 60 m: 16; symmetric about the centreline, outer stripes 3 m inside the edge | every paved runway |
| Centreline | white dashes 30 m on, 20 m off, 0.45 m wide (0.9 m at 45 m+), from the designation to the far designation | every paved runway |
| Aiming point | two bars 45 m long (30 m below 30 m width), 6 m wide, 400 m from the threshold (300 m below 1200 m of runway), symmetric, 18 m apart inside edges | non-precision and precision |
| Touchdown zone | pairs of 22.5 m by 1.8 m stripes at 150 m, 300 m, 450 m (one pair, then two, then three), omitted where they would meet the far end's set | precision |
| Side stripes | 0.9 m continuous white at each edge | precision |
| Grass | no pavement markings; white edge markers (0.6 m squares) every 60 m along both edges and a 3 m square at each corner | grass only |

**4.2 The yellow line goes.** The road material's centreline mask is driven per profile: a
runway profile sets `CentrelineWidth` to 0 through its material slot's dynamic instance, so
the only line on a runway is the white one the builder paints. Taxiways are untouched.

**4.3 The surface itself.** Three band materials for runways, resolved through the content
set beside `SurfaceMaterial` (`RunwayGrassMaterial`, `RunwayTarmacMaterial`,
`RunwayConcreteMaterial`), authored by `Tools/Python/build_runway_materials.py` (editor
closed) as tinted variants of `M_RoadSurface`; reinforced is concrete with a cooler tint and
a "R" is NOT painted - the difference shows in the details panel and in what lands there.
`URoadMaterialSet` gains the three slot names; `FRoadMeshBuilder` already writes per-band
material ids, so a runway segment's bands select the surface's slot by `FRunwayFacts`.

## 5. The tool

The runway tool (key 6) gains two choices beside width: surface and approach, shown on the
bar as the current selection and cycled with the same keys the width uses. Placement
writes the facts to every segment of the new strip. A change of surface or approach on an
existing runway is an edit on the facade (`SetRunwayFacts(SegmentIndex, Facts)`, applied to
the chain, undoable). Existing levels load as tarmac / visual by the struct's defaults.

## 6. Tests (measured)

1. `Airside.Build.RunwayMarkings.<Width>` for each of the five widths at `Precision`:
   threshold stripe count and spacing, designation glyph bounds at both ends, centreline
   dash count from the length, aiming-point bar positions, touchdown-zone pair count, side
   stripes; every quad in the road plane, UV1 zero, engine-computed normals up (the check
   that caught the holding-position paint facing down).
2. `Airside.Build.RunwayMarkings.ByApproach`: Visual has no aiming point; NonPrecision has
   one and no TDZ; Precision has both and the side stripes. `Grass` paints only markers.
3. `Airside.Model.RunwayAdmission`: the Piper against every surface and approach; a 36 m
   wingspan against 23 m; a 100 m field length against a 90 m strip; each refusal named.
4. `Airside.Model.FieldLengthsCoverTheRoll` (§3.2).
5. `Airside.Model.ArrivalPlanner.NotAdmitted` and `DeparturePlanner.NotAdmitted`: the
   planners refuse through admission before anything else and carry the reason.
6. `Airside.Model.RunwayFactsSurviveSaveAndSplit`: facts persist through duplicate (the PIE
   path) and are copied onto both halves when an exit splits the strip.
7. The saved-map probe logs each runway's facts and marking census.

## 7. Out of scope

Displaced thresholds, stopways, clearways, declared distances per end (§1); runway and
approach lighting; weather and minima (the approach class only becomes a live constraint
when M5's weather exists; until then it refuses only aircraft that declare a need);
pavement classification numbers; taxiway markings beyond the holding positions.

## 8. Open questions for review

- Whether reinforced should read differently on the ground at all, or only in the panel.
- Whether the tool should refuse to place a Precision approach on a runway shorter than
  the touchdown-zone markings need (about 900 m), or paint what fits. The spec says paint
  what fits, omitting pairs that would overlap the far end.
