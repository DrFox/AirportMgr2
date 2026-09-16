#include "Entities/EntityDefinition.h"

#include "Content/AirsideSettings.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	// A stand with no design aircraft draws no envelope, offers no service positions and -
	// since the service lane is laid out along the aeroplane - gets its crossings placed off
	// the anchors alone, which in a test reads as "the feature is broken" rather than "the
	// fixture is thin".
	//
	// BUILT FIRST now, because the layout is measured FROM it: this used to be set after
	// BuildCodeCStand had already run, which was harmless only for as long as nothing in the
	// layout depended on it.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildCodeCStand(Definition, A320);

	return Definition;
}

void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* Aircraft)
{
	// ONE RESOLUTION SITE for the vehicle the ground is sized for, which is the whole reason
	// the split exists: every shipping caller - the Python asset author, Blueprint, the tests
	// that want the real stand - goes through here and gets the SAME vehicle, and only a test
	// that is deliberately measuring the derivation names a different one.
	BuildCodeCStandFor(Definition, Aircraft, UAirsideSettings::ResolveLargestServiceVehicle());
}

void UEntityDefinition::BuildCodeCStandFor(
	UEntityDefinition* Definition, UAircraftType* Aircraft, const FAirframe& Largest)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// SET HERE, not by the caller afterwards - see the header. The service lane below is
	// measured along this aeroplane, so the builder that lays the ground round it has to know
	// which aeroplane that is.
	Definition->DesignAircraft = Aircraft;

	// A Code C contact stand: the ground half of a turnaround.
	//
	// ORIGIN is the NOSE GEAR STOP POSITION - the mark painted on the apron that a docking
	// guidance system stops the aircraft at. An aircraft parked here shares this pose
	// exactly, which is why composing a stand with an aircraft needs no offset.
	//
	// WHAT IS HERE is ground-fixed only: plant dug into the concrete and boxes painted on
	// it. Where a service connects to the AIRCRAFT is on UAircraftType, because an A320 and
	// a 737-800 both park here and their doors are metres apart.
	//
	// +X faces the terminal, +Y is starboard of a parked aircraft. Units are uu, and a uu
	// here is a centimetre.
	auto AddFixture = [Definition](const TCHAR* Id, double X, double Y, double HeadingDegrees,
		EServiceRole Role)
	{
		FEntityAnchor Fixture;
		Fixture.Id = FName(Id);
		Fixture.LocalPosition = FVector2D(X, Y);
		Fixture.LocalHeading = FMath::DegreesToRadians(HeadingDegrees);
		Fixture.Role = Role;
		Definition->Anchors.Add(Fixture);
	};

	// WHAT THE GROUND HAS TO GIVE A DRIVER, resolved once and spent everywhere below. Sized for
	// the largest vehicle ADMITTED and never for the one driving now - the same rule the
	// taxiway geometry follows, and the reason a bigger dispenser needs no edit here.
	const double Radius = Largest.TightestFollowableRadius();

	// EVERY STRAIGHT CARRIES BOTH CUTS THAT MEET ON IT, PLUS A TENTH.
	//
	// THE SUM AND NOT THE LARGER, which is the correction that reshaped this layout. Two
	// corners cut back into the same straight from opposite ends, so a straight shorter than
	// Run(a) + Run(b) has the two curves overlapping and NEITHER delivers its radius - exactly
	// the clamp FServiceLoopBuild applies one level down. The first draft of this layout costed
	// each corner against the larger of its two legs alone and put a square 90 degree crossing
	// on a 1700 uu straight that needs 1978 for its two ends.
	//
	// THE TENTH IS MEASURED, not chosen: a straight sized to the exact sum leaves the two
	// curves meeting at a single point, with no straight at all between them and nothing but a
	// rounding error between passing and failing. A tenth is the most the tightest place on the
	// lane can afford - the 1700 uu between the two runs allows 1.12 - so it is set by the
	// geometry rather than by taste.
	constexpr double LegSlack = 1.1;

	// WHERE THE GROUND PLANT IS. The pit and the two lane runs are FIXED: a hydrant is plant
	// dug into concrete under the wing root, the box row is painted where the holds are, and
	// the port run is the line the bridge's ground power and the tug's box already sit on. The
	// paint is arranged around them, not the other way about.
	constexpr double PitX = -1200.0;
	constexpr double PitY = 700.0;
	constexpr double BoxY = 1100.0;
	constexpr double PortY = -600.0;
	constexpr double GpuX = 300.0;
	constexpr double TugX = 1400.0;

	// THE HYDRANT DIP. The pit sits 400 uu inboard of the box row, on a FLAT long enough to
	// hold the cut of the corner at each of its ends, with a leg rising to the row at
	// DipLegAngle and a corner where it meets the row.
	//
	// 40.5 DEGREES IS THE MINIMUM OF THE DIP'S X EXTENT, not a taste: steeper legs shorten the
	// diagonal but lengthen the corner runs, shallower ones the reverse. The expression
	// 2*Run(angle) + Depth/tan(angle) bottoms out at 1018 uu anywhere between about 40 and 42
	// degrees at the shipping truck, and costs within 7% of that anywhere from 30 to 50 - the
	// worst of that range is 30 degrees, at 1081 uu. The figure is stated so the arithmetic
	// below has one input rather than a search.
	constexpr double DipLegAngleDegrees = 40.5;
	constexpr double DipDepth = BoxY - PitY;
	const double DipLeg = FMath::DegreesToRadians(DipLegAngleDegrees);
	const double DipRun = GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_PI - DipLeg);

	// THE FLAT holds two cuts, one from each end, so it is 2*LegSlack*Run long and the pit sits
	// in the middle of it - which is what "the pit is driven through" means: a vehicle stopped
	// there is on straight ground, not mid-curve.
	const double DipFlatHalf = LegSlack * DipRun;

	// WHERE THE LEG MEETS THE ROW, and it is a CORNER rather than a box. The first draft put
	// EquipmentFwd here and made the box itself the corner; that is a truck turning INTO an
	// anchor, which is the exact thing this redesign exists to remove. The diagonal between
	// this corner and the flat's is 616 uu against the 550 its two cuts need.
	const double DipRowX = DipFlatHalf + DipDepth / FMath::Tan(DipLeg);

	// AND THE BOXES SIT A CUT FURTHER OUT, on the straight run, where a vehicle reaches them
	// pointing along the row and stops without turning.
	const double DipHalfExtent = DipRowX + LegSlack * DipRun;

	AddFixture(TEXT("HydrantPit"), PitX, PitY, 180.0, EServiceRole::Fuel);

	// THE BOXES SIT WHERE THE DIP LETS THEM, not where they were typed. They were at -300 and
	// -2100, which is 900 uu from the pit against the 1018 the dip needs with no slack at all -
	// short by 118, and short by a different amount for any other vehicle. So the figure is
	// derived. Airside.Entities.StandBoxesMoveWithTheLargestVehicle asserts the derivation by
	// building the same stand around a longer vehicle, because asserting the OUTPUT would pass
	// just as well against a hand-typed number.
	//
	// HEADING 180, re-authored 2026-09-16. They faced -90 because the old spur arrived square
	// on from outboard; a truck driving along the row finishes pointing along the aircraft, so
	// that is how the box is painted. Slightly wrong for a belt loader, which really does
	// square up to a hold door - accepted until the reverse leg exists to do it properly. The
	// pit's own heading moved for the same reason, and from the same -90.
	const double BoxFwdX = PitX + DipHalfExtent;
	const double BoxAftX = PitX - DipHalfExtent;
	AddFixture(TEXT("EquipmentFwd"), BoxFwdX, BoxY, 180.0, EServiceRole::Baggage);
	AddFixture(TEXT("EquipmentAft"), BoxAftX, BoxY, 180.0, EServiceRole::Baggage);

	// Fixed ground power at the bridge, off the port bow.
	AddFixture(TEXT("FixedGPU"), GpuX, PortY, 90.0, EServiceRole::GPU);

	// Where the tug waits before pushback, clear of the nose.
	AddFixture(TEXT("TugStand"), TugX, PortY, 180.0, EServiceRole::Tug);

	// What this stand can provide at all. A contact stand does the lot.
	Definition->AvailableServices = {
		EServiceRole::Aircraft, EServiceRole::Fuel, EServiceRole::Baggage,
		EServiceRole::Tug, EServiceRole::GPU, EServiceRole::Passenger, EServiceRole::Crew };

	// EVERY FIELD THIS BUILDER OWNS, SET, including the three that happen to want the
	// constructor default. Not decoration: build_stand_asset.py re-authors an EXISTING asset
	// in place (it must - deleting one that a level and another asset reference fails), so a
	// field the builder leaves alone keeps whatever was last saved into it. Stating them is
	// what makes "re-run the script" mean the same thing as "make it from scratch".
	Definition->PoseRole = EServiceRole::Aircraft;   // the nose gear stop mark
	Definition->FootprintExtent = FVector2D::ZeroVector;   // the stand's extent IS its aircraft's
	Definition->Trucks = 0;                          // nothing is based here; a depot has the fleet

	// THE CROSSINGS, which join the two runs into one cycle.
	//
	// AN OCTAGON'S CORNER FOUR TIMES, and that is forced rather than chosen. A U-turn is 180
	// degrees however it is cut up, and two SQUARE corners want 2 * CornerRunFor(R, 90deg) =
	// 1978 uu on the straight between the runs against the 1700 there is between y=1100 and
	// y=-600. Three corners of 60 degrees fit, but with 50 uu to spare on a 950 uu leg, which
	// is a layout that breaks on the next re-measure. Four corners of 45 spread the same turn
	// over two diagonals and a straight and leave a tenth on each.
	//
	// THE FLOOR IS 2R WHATEVER THE CUT, because that is a semicircle - 1399 uu at the shipping
	// truck against the 1700 available. There is room, and not much of it; the warning below is
	// what says so out loud when a bigger vehicle takes it away.
	constexpr double CrossDeflectDegrees = 45.0;
	const double CrossDeflect = FMath::DegreesToRadians(CrossDeflectDegrees);
	const double CrossRun = GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_PI - CrossDeflect);

	// THE DIAGONALS get exactly their slack; the straight between them takes what is left of
	// the 1700, because the two runs' y are fixed by the plant on them and cannot move.
	const double CrossDiagonal = LegSlack * 2.0 * CrossRun;
	const double CrossBulge = CrossDiagonal * FMath::Sin(CrossDeflect);
	const double CrossStraight = (BoxY - PortY) - 2.0 * CrossBulge;
	if (CrossStraight < 2.0 * CrossRun)
	{
		// NOT SILENTLY WRONG. The lane is still emitted - a shape that is too tight to drive is
		// more use to whoever has to fix it than no shape at all - but the crossings will have
		// corners no vehicle of this size can take, and the log is what gets that noticed
		// rather than lived with.
		UE_LOG(LogAirside, Warning,
			TEXT("Stand lane crossings are too tight for a vehicle needing %.0f uu of radius: "
			     "the two runs are %.0f uu apart and the crossing needs %.0f. A U-turn cannot "
			     "be cut into less than twice the radius."),
			Radius, BoxY - PortY, 2.0 * CrossBulge + 2.0 * CrossRun);
	}

	// HOW FAR OUT THE CROSSINGS SIT. Far enough that the corner where a crossing leaves a run
	// has its cut clear of the last anchor on that run, and - the binding one at the tail -
	// clear of the aeroplane itself.
	//
	// The clearance is a CONSTANT and not a UPROPERTY, deliberately: it is a fact about how
	// this stand type is laid out, decided at authoring time beside the anchors. A
	// level-authored version would be a knob that silently reshaped stands already placed.
	constexpr double EndClearance = 300.0;
	const double EntryLeg = LegSlack * CrossRun;
	double NoseX = FMath::Max(TugX, BoxFwdX) + EntryLeg;
	double TailX = FMath::Min(GpuX, BoxAftX) - EntryLeg;
	if (Aircraft != nullptr && Aircraft->Footprint.IsSet())
	{
		// AHEAD OF THE NOSE AND ASTERN OF THE TAIL, which is what keeps the old ring's one real
		// promise: a crossing is the only part of this lane that spans the fuselage centreline,
		// so it is the only part that could cross the aeroplane. The tailplane is the reason
		// this is the tail's binding constraint and not the aft box's - it is 12 m across, and a
		// crossing level with it would run under it.
		NoseX = FMath::Max(NoseX, Aircraft->Footprint.NoseX + EndClearance);
		TailX = FMath::Min(TailX, Aircraft->Footprint.TailX - EndClearance);
	}

	Definition->ServiceLane.Reset();
	auto Coincident = [](const FStandWaypoint& A, const FStandWaypoint& B)
	{
		return A.Local.Equals(B.Local, UE_DOUBLE_KINDA_SMALL_NUMBER);
	};

	auto Add = [Definition, &Coincident](
		double X, double Y, EStandWaypointKind Kind, const TCHAR* Id = nullptr)
	{
		FStandWaypoint Point;
		Point.Local = FVector2D(X, Y);
		Point.Kind = Kind;
		Point.AnchorId = Id != nullptr ? FName(Id) : NAME_None;

		// A COINCIDENT POINT IS MERGED, and only a PIVOTING vehicle produces one: its corners
		// need no run at all, so EntryLeg is zero, NoseX lands exactly on TugX, and each
		// crossing's two shaping points collapse onto its corners. Merging is what leaves a
		// square U-turn rather than a polyline with zero-length sides, which has no direction
		// and so no corner angle either.
		//
		// AND THE ANCHOR ALWAYS WINS, whichever of the two arrived first. Dropping the LATER
		// point unconditionally - which is what this guard did when it was written - silently
		// loses the TugStand anchor to the entry that landed on top of it, and a lane missing an
		// anchor is the one thing StandLaneCarriesItsAnchors exists to catch. Only a Plain or an
		// Entry is ever thrown away.
		if (Definition->ServiceLane.Num() > 0 && Coincident(Definition->ServiceLane.Last(), Point))
		{
			if (Point.Kind == EStandWaypointKind::Anchor
				&& Definition->ServiceLane.Last().Kind != EStandWaypointKind::Anchor)
			{
				Definition->ServiceLane.Last() = Point;
			}
			return;
		}

		Definition->ServiceLane.Add(Point);
	};

	// THE CYCLE, laid out starboard-forward, across the nose, port-aft, across the tail. Order
	// matters - it is a closed polyline and must not fold through itself.
	//
	// EVERY ANCHOR IS A STRAIGHT-THROUGH POINT. Not one of the five is a corner: the pit is in
	// the middle of the dip's flat, the boxes are a cut clear of the row corners, and the GPU
	// and the tug sit mid-run on the port side. That is the whole claim of the redesign, and
	// Airside.Entities.StandLaneCornersClearTheTruckLock is where it is measured.
	Add(BoxAftX,              BoxY,  EStandWaypointKind::Anchor, TEXT("EquipmentAft"));
	Add(PitX - DipRowX,       BoxY,  EStandWaypointKind::Plain);
	Add(PitX - DipFlatHalf,   PitY,  EStandWaypointKind::Plain);
	Add(PitX,                 PitY,  EStandWaypointKind::Anchor, TEXT("HydrantPit"));
	Add(PitX + DipFlatHalf,   PitY,  EStandWaypointKind::Plain);
	Add(PitX + DipRowX,       BoxY,  EStandWaypointKind::Plain);
	Add(BoxFwdX,              BoxY,  EStandWaypointKind::Anchor, TEXT("EquipmentFwd"));

	// ACROSS THE NOSE. The entries are the two corners where the crossing meets a run, because
	// those are the points a road outside the stand can actually reach: the shaping points
	// between them are mid-turn, and a road joining one would arrive at a heading the lane does
	// not have. An entry ON the cycle, never abeam it - a stub is a dead end, and reverse does
	// not exist until a later stage.
	Add(NoseX,                BoxY,                EStandWaypointKind::Entry);
	Add(NoseX + CrossBulge,   BoxY - CrossBulge,   EStandWaypointKind::Plain);
	Add(NoseX + CrossBulge,   PortY + CrossBulge,  EStandWaypointKind::Plain);
	Add(NoseX,                PortY,               EStandWaypointKind::Entry);

	// Port-aft, through the tug's box and the bridge's ground power.
	Add(TugX,                 PortY, EStandWaypointKind::Anchor, TEXT("TugStand"));
	Add(GpuX,                 PortY, EStandWaypointKind::Anchor, TEXT("FixedGPU"));

	// Across the tail, and back onto the starboard run - the close is implicit.
	Add(TailX,                PortY,               EStandWaypointKind::Entry);
	Add(TailX - CrossBulge,   PortY + CrossBulge,  EStandWaypointKind::Plain);
	Add(TailX - CrossBulge,   BoxY - CrossBulge,   EStandWaypointKind::Plain);
	Add(TailX,                BoxY,                EStandWaypointKind::Entry);

	// AND THE WRAP, which the guard inside Add cannot see. The cycle CLOSES IMPLICITLY, so the
	// last waypoint is as much a neighbour of the first as of the one before it, and a
	// coincident pair across that join is the same defect one place further round. It is the
	// same pivoting vehicle that causes it: with no corner run, TailX lands exactly on
	// EquipmentAft's X and the tail crossing's last entry sits on top of the first waypoint.
	//
	// A LOOP rather than one comparison, because each pop exposes a new last - and it
	// terminates on every pass, since it only ever shortens the array.
	while (Definition->ServiceLane.Num() > 1
		&& Coincident(Definition->ServiceLane.Last(), Definition->ServiceLane[0]))
	{
		if (Definition->ServiceLane.Last().Kind == EStandWaypointKind::Anchor
			&& Definition->ServiceLane[0].Kind != EStandWaypointKind::Anchor)
		{
			Definition->ServiceLane[0] = Definition->ServiceLane.Last();
		}
		Definition->ServiceLane.Pop();
	}
}

UEntityDefinition* UEntityDefinition::MakeFuelDepotTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildFuelDepot(Definition);
	return Definition;
}

void UEntityDefinition::BuildFuelDepot(UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return;
	}

	// NO ANCHORS. A depot has no plant an aircraft connects to - see the header for why the
	// pose alone is its road connection. Reset rather than left alone so re-authoring an
	// asset that once had some really does clear them.
	Definition->Anchors.Reset();

	// AND NO SERVICE LANE. A lane threads the boxes an aeroplane is serviced from; a depot has
	// one pose and nothing parked at it, and its pose IS its road connection. Reset for the
	// same reason Anchors is.
	Definition->ServiceLane.Reset();

	// ORIGIN IS THE TRUCK BAY - where a truck stands when it is home, and the node it is
	// dispatched from and back to.
	//
	// +X FACES AWAY FROM THE ROAD, exactly as a stand's +X faces the terminal: the pose
	// lead-in leaves along heading PLUS 180 (see FAnchorLink), so it runs out of the BACK of
	// the installation to the movement area. A depot is therefore aimed away from the road it
	// serves, and the truck drives out behind it.
	//
	// Stated the wrong way round when this was first written ("+X faces the road"), which is
	// self-contradictory given the +180 in the same sentence - and it is the sentence a
	// player placing one would have followed.
	Definition->PoseRole = EServiceRole::Fuel;

	// HALF-extents: 12 m by 8 m overall. A tank, a pump, and room to turn a bowser round.
	// A placeholder box, and the only geometry the depot has this slice.
	Definition->FootprintExtent = FVector2D(600.0, 400.0);

	// ONE truck. The number UFuelService counts trucks-out against; M3's job board replaces
	// the counting, not the number.
	Definition->Trucks = 1;

	// What this installation can provide. Fuel and nothing else, which is the whole slice.
	Definition->AvailableServices = { EServiceRole::Fuel };

	// NO DesignAircraft, deliberately: nothing parks here, so there is no envelope to draw
	// and no ICAO code letter to size a lead-in sweep by. FAnchorLink falls back to Code C's
	// 2500 uu radius, which is generous for a van and costs nothing.
}

bool UEntityDefinition::HasUsableAnchorIds(const UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return false;
	}

	TSet<FName> Seen;
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		if (Anchor.Id.IsNone() || Seen.Contains(Anchor.Id))
		{
			return false;
		}
		Seen.Add(Anchor.Id);
	}
	return true;
}

int32 UEntityDefinition::RefreshResolvedAnchors(URoadNetwork& Network)
{
	int32 ChangedCount = 0;

	// Gathered by index, like FAnchorLink::Build: nothing here adds or removes an entity,
	// so holding this reference across RefreshResolvedAnchor calls is safe, and the id
	// still needs building by hand from Index and Generation - the array elements have no
	// stable handle of their own to hand back.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		// THE INSTANCE'S OWN POSE ROLE, for the same reason the anchors' snapshots are
		// refreshed here: an entity placed and saved before FEntityInstance::PoseRole existed
		// loads with the UPROPERTY default (Aircraft) and nothing else ever corrects it.
		// Harmless for every entity that COULD have been saved then - they were all stands -
		// and wrong for a depot whose definition is re-authored after placement, which would
		// otherwise be stuck casting its lead-in at a taxiway for ever.
		if (Instance.PoseRole != Instance.Definition->PoseRole
			&& Network.SetEntityPoseRole(EntityId, Instance.Definition->PoseRole))
		{
			++ChangedCount;
		}

		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			// The role lives on the definition, addressed by id - never by position in the
			// array, which is the invariant FResolvedAnchor exists to remove.
			const FEntityAnchor* Declared = Instance.Definition->Anchors.FindByPredicate(
				[&Resolved](const FEntityAnchor& Candidate) { return Candidate.Id == Resolved.Id; });
			if (Declared == nullptr)
			{
				continue;
			}

			if (Declared->LocalHeading == Resolved.LocalHeading && Declared->Role == Resolved.Role)
			{
				continue;
			}

			if (Network.RefreshResolvedAnchor(EntityId, Resolved.Id, Declared->LocalHeading, Declared->Role))
			{
				++ChangedCount;
			}
		}
	}

	return ChangedCount;
}
