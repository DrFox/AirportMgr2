#include "Entities/EntityDefinition.h"

#include "Content/AirsideSettings.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/IcaoCode.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	// BUILT FIRST, because the layout is measured FROM it: this used to be set after
	// BuildCodeCStand had already run, which was harmless only for as long as nothing in the
	// layout depended on it.
	//
	// AND BECAUSE THE FIXTURE HAS TO BE THE SHIPPING STAND. A definition with no design
	// aircraft is SUPPORTED - it gets a lane whose crossings are placed off the anchors alone
	// rather than pushed clear of a nose and a tail that are not there, which is what a stand
	// with no envelope wants, and BuildCodeCStandFor's header says so. It is simply a
	// different stand from the one the suite is about: without an envelope the tail clamp
	// that puts the aft crossing at x = -3550 never binds and the fuselage the lane is
	// asserted to clear does not exist, so every figure measured here would be another
	// layout's.
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

namespace
{
	/**
	 * How much straighter than the limit every corner of the template is cut.
	 *
	 * A TENTH, and the reason is the one the lane's own header gave: a corner sized to the
	 * exact limit leaves nothing but a rounding error between passing and failing, and the
	 * oracle then reports a radius 0.6 uu inside the limit, which reads as working and is a
	 * coincidence. Every run below is this times what the arithmetic demands.
	 */
	constexpr double LegSlack = 1.1;

	/**
	 * Turn a polyline of corner VERTICES into the chain of quadratics that rounds it.
	 *
	 * A POLYLINE IS HOW A PATH IS DESCRIBED, NOT HOW IT IS DRIVEN. A vertex where two straights
	 * meet is a heading that changes instantly, and FSpeedProfile calls one of those untakeable
	 * at any speed; the bend carries the turn instead. Both legs' tangents meet AT the vertex,
	 * so the single control point they define IS the vertex - the quadratic case, and what
	 * makes the curve leave each side tangentially rather than at an angle to it.
	 *
	 * IT DOES NOT CLAMP. A corner that cannot be given its run is LOGGED and laid anyway, at
	 * the run it asked for, so the oracle sees a curve tighter than the limit and the test
	 * fails with the figure. Shaving it to fit would produce a layout whose shape is decided by
	 * a disagreement between two numbers, which is what the lane this replaces did, and the
	 * resulting stand passed its tests and crabbed in PIE four times running.
	 */
	void BuildLeg(const TArray<FVector2D>& Vertices, double Radius, const TCHAR* What,
		FStandLeg& OutLeg)
	{
		OutLeg.Points.Reset();
		OutLeg.Controls.Reset();
		if (Vertices.Num() < 2)
		{
			return;
		}

		auto Push = [&OutLeg](const FVector2D& To, const FVector2D& Control)
		{
			// A zero-length segment has no direction, so it has no corner angle either - which
			// is the degenerate the lane's Add() guard existed to stop. Dropped rather than
			// laid.
			if (!OutLeg.Points.Last().Equals(To, UE_DOUBLE_KINDA_SMALL_NUMBER))
			{
				OutLeg.Controls.Add(Control);
				OutLeg.Points.Add(To);
			}
		};

		OutLeg.Points.Add(Vertices[0]);

		for (int32 At = 1; At + 1 < Vertices.Num(); ++At)
		{
			const FVector2D In = (Vertices[At - 1] - Vertices[At]).GetSafeNormal();
			const FVector2D Out = (Vertices[At + 1] - Vertices[At]).GetSafeNormal();
			if (In.IsNearlyZero() || Out.IsNearlyZero())
			{
				continue;
			}

			const double Interior = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(In, Out), -1.0, 1.0));
			if (Interior > UE_DOUBLE_PI - 0.01)
			{
				// Collinear: no corner to round, and CornerRunFor would return nothing useful.
				continue;
			}

			const double Run = LegSlack * GuidelineGeom::CornerRunFor(Radius, Interior);
			const double Shortest = FMath::Min(
				FVector2D::Distance(Vertices[At - 1], Vertices[At]),
				FVector2D::Distance(Vertices[At], Vertices[At + 1]));
			if (Run > Shortest)
			{
				UE_LOG(LogAirside, Warning,
					TEXT("Stand template leg '%s': the %.0f degree corner at (%.0f, %.0f) needs "
					     "%.0f uu of run and its shorter leg is %.0f. Laid at the run it asked "
					     "for, so the drivability test reports the radius rather than a shape "
					     "nobody chose."),
					What, FMath::RadiansToDegrees(UE_DOUBLE_PI - Interior),
					Vertices[At].X, Vertices[At].Y, Run, Shortest);
			}

			const FVector2D Enter = Vertices[At] + Run * In;
			const FVector2D Exit = Vertices[At] + Run * Out;

			// The straight into the bend, then the bend. The straight's control sits on its own
			// midpoint, which is how this graph spells "straight" - see GuidelineGeom::IsStraight.
			Push(Enter, (OutLeg.Points.Last() + Enter) * 0.5);
			Push(Exit, Vertices[At]);
		}

		Push(Vertices.Last(), (OutLeg.Points.Last() + Vertices.Last()) * 0.5);
	}
}

void FStandLeg::Sample(TArray<FVector2D>& OutPoints) const
{
	OutPoints.Reset();
	if (!IsSet())
	{
		return;
	}

	for (int32 At = 0; At + 1 < Points.Num(); ++At)
	{
		// THE SHARED POINT IS DROPPED, exactly as a route does when it welds two edges: Sample
		// appends both endpoints, so without this every joint would carry a duplicate, and a
		// duplicate is a zero-length span that FSpeedProfile has no heading for.
		TArray<FVector2D> Segment;
		GuidelineGeom::Sample(Points[At], Controls[At], Points[At + 1], Segment);
		OutPoints.Append(At == 0 ? Segment : TArrayView<const FVector2D>(Segment).RightChop(1));
	}
}

FRoutePlan FStandLeg::ToPlan() const
{
	FRoutePlan Plan;
	Sample(Plan.Polyline);
	if (Plan.Polyline.Num() < 2)
	{
		return Plan;
	}

	Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

	// FOUND, WITHOUT WHICH EVERY CONSUMER REFUSES IT AT ITS FIRST GUARD. FRoutePlan::IsValid is
	// Result == Found and not "has a polyline", so a plan carrying only geometry reads as the
	// code rejecting a legal input - which cost a session on this branch already.
	Plan.Result = ERouteResult::Found;
	return Plan;
}

void UEntityDefinition::BuildCodeCStandFor(
	UEntityDefinition* Definition, UAircraftType* Aircraft, const FAirframe& Largest)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// SET HERE, not by the caller afterwards - see the header. The layout below is measured
	// against the LETTER rather than against this aeroplane, but the envelope drawing and the
	// stand's own inspect panel both read it, and a definition that names no design aircraft is
	// a stand nothing can be shown parked on.
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

	const FString Letter(TEXT("C"));

	// EVERY FIXTURE IS CLEAR OF THE WING, and that is what places them rather than taste.
	// Ruled 2026-09-17: nothing drives under a wing, so a service position beneath one could
	// never be reached. IcaoCode's keep-out is every wing the letter admits laid over one
	// another - Code C runs -2150 to -950 - and the three starboard fixtures sit forward of its
	// leading edge or aft of its trailing edge, which is also where an airliner's holds
	// actually are: one ahead of the wing box and one behind it.
	const double WingFwd = IcaoCode::WingFwdForLetter(Letter);
	const double WingAft = IcaoCode::WingAftForLetter(Letter);
	constexpr double PlantClearance = 150.0;

	// THE STARBOARD ROW. A hydrant pit sits under the refuel panel on the wing leading edge, so
	// the dispenser stands just forward of it and reaches in; the two holds are where an
	// airliner's holds are.
	constexpr double RowY = 700.0;
	const double PitX = WingFwd + PlantClearance;
	const double HoldFwdX = WingFwd + 600.0;
	// 250 RATHER THAN 450 CLEAR OF THE TRAILING EDGE. The aft hold is the aft-most thing any
	// vehicle drives to, so it is the bay whose serve leg has the least lane to complete its
	// shift in - and 450 left that run 47 uu inside what its two corners need. It is paint
	// clearance from a wing nothing drives under, not structural separation.
	const double HoldAftX = WingAft - 250.0;

	AddFixture(TEXT("HydrantPit"), PitX, RowY, -90.0, EServiceRole::Fuel);
	AddFixture(TEXT("EquipmentFwd"), HoldFwdX, RowY, -90.0, EServiceRole::Baggage);
	AddFixture(TEXT("EquipmentAft"), HoldAftX, RowY, -90.0, EServiceRole::Baggage);

	// HEADING -90, which is INBOARD on the starboard side: a vehicle at its service point faces
	// the aeroplane it is working on. It drives in forwards and reverses out, so this is also
	// the heading its reverse leg starts from - see FServiceBay.

	// Fixed ground power at the bridge, off the port bow, facing inboard for the same reason.
	AddFixture(TEXT("FixedGPU"), WingFwd + 600.0, -RowY, 90.0, EServiceRole::GPU);

	// WHERE THE TUG WAITS, and it gets NO BAY - see BuildStandTemplate. A pushback tug does not
	// service an aeroplane from a parking bay; it couples at the nose gear and pushes, which is
	// FPushbackRun's business and already modelled. Forward and to port, abeam the nose.
	//
	// NOT AHEAD OF THE NOSE, which is where a tug really waits, because that is outside the
	// stand: a Code C stand is 55 m deep measured from the nose and the airframe fills 39.5 m
	// of it with every bit of the slack behind. Recorded rather than hidden - holding the tug
	// on the stand wants either a deeper letter or a tug dispatched from the apron.
	AddFixture(TEXT("TugStand"), 350.0, -1400.0, 180.0, EServiceRole::Tug);

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

	BuildStandTemplate(*Definition, Letter, Largest);
}

void UEntityDefinition::BuildStandTemplate(
	UEntityDefinition& Definition, const FString& Letter, const FAirframe& Largest)
{
	// THE LAYOUT IS BUILT FOR THE FLOOR OF ITS LETTER'S BAND. 53 m to just under 75 is all
	// Code C, and a template authored at a comfortable 60 would fail exactly where a player
	// drew the smallest stand the rules allow. Every figure below therefore comes off IcaoCode,
	// which reports the minimum.
	const double Width = IcaoCode::StandWidthForLetter(Letter);
	const double Depth = IcaoCode::StandDepthForLetter(Letter);
	const double NoseFwd = IcaoCode::MaxNoseFwdForLetter(Letter);
	const double TailAft = IcaoCode::MaxTailAftForLetter(Letter);

	// THE STAND BOX, in the definition's own local space. Depth is measured NOSE to the back of
	// the GSE road, so the back edge is the nose overhang minus the depth and every bit of the
	// slack is behind the aeroplane.
	const double HalfWidth = 0.5 * Width;
	const double BackX = NoseFwd - Depth;

	// The two limits, resolved once. Forward is L/sin(lock) and reverse L/tan(lock) - about 30%
	// tighter, because a reversing vehicle pivots about its FIXED axle.
	const double Radius = Largest.TightestFollowableRadius();
	const double ReverseRadius = Largest.TightestReversibleRadius();

	// THE LANE DOWN EACH SIDE, outboard of the wingtip because nothing may pass under a wing.
	// Its own width is what the stand's minimum width was derived from, so this sits exactly
	// inside the boundary with the letter's wingtip clearance between it and the aeroplane.
	const double LaneY = HalfWidth - 0.5 * IcaoCode::ServiceLaneWidth();

	// WHAT EACH KIND OF CORNER COSTS, resolved once and spent everywhere below. A right angle
	// is 1.414 R of run on EACH arm; 45 degrees is 0.448 R, which is why the parking slots are
	// angled and the turn off the road is not square.
	const double Square = LegSlack * GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_HALF_PI);
	const double Diagonal =
		LegSlack * GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_PI * 0.75);
	const double SquareBack =
		LegSlack * GuidelineGeom::CornerRunFor(ReverseRadius, UE_DOUBLE_HALF_PI);

	// THE PARKING ROW. Slots stand at 45 degrees a short run in from the back edge, so a
	// vehicle turning off the GSE road makes a 45 degree corner rather than a square one.
	constexpr double ParkRun = 500.0;
	const double ParkRowX = BackX + ParkRun;

	// HOW FAR APART THE AFT-EDGE NODES SIT, DERIVED FROM THE ROAD'S OWN FILLET rather than
	// chosen. Every entry and exit splits the road where it joins it, and the fillet either
	// side of that contact wants Diagonal of run along the road; two neighbours therefore need
	// twice that between them, plus the tenth everything else here gets.
	//
	// MEASURED, AND IT IS WHY THIS IS NOT 450. At 450 the six aft-edge nodes left 450 uu
	// segments of road between them, the fillets were clamped to what was left, and BOTH
	// sweeps onto the road came out at 89 and 6 uu against a lock of 699 - not the ruled
	// one-good-one-shunt pair but two unusable ones. The aft edge is 5300 uu long and six
	// nodes at this pitch need 3800, so the room was always there; it was the packing that
	// was wrong.
	const double AftPitch = 2.0 * Diagonal * LegSlack;

	// A BAY PER ANCHOR A VEHICLE SERVICES FROM, which is not the same as every anchor that is
	// not the aeroplane's own pose. The TUG is excluded by name: pushback couples at the nose
	// gear and is FPushbackRun's manoeuvre, so a tug never drives from a parking bay to a
	// service point and giving it the four legs would be geometry nothing walks.
	TArray<const FEntityAnchor*> Serviced;
	for (const FEntityAnchor& Anchor : Definition.Anchors)
	{
		if (TraversalForRole(Anchor.Role) != ETraversalClass::Aircraft
			&& Anchor.Role != EServiceRole::Tug)
		{
			Serviced.Add(&Anchor);
		}
	}

	// STARBOARD BEFORE PORT, AND AFT-MOST FIRST WITHIN A SIDE. The side ordering is what keeps
	// one ladder's nodes in one run rather than interleaved; the aft-most ordering is what
	// gives the bay with the least lane the entry that reaches the lane soonest.
	Serviced.Sort([](const FEntityAnchor& A, const FEntityAnchor& B)
		{
			const bool bStarboardA = A.LocalPosition.Y >= 0.0;
			const bool bStarboardB = B.LocalPosition.Y >= 0.0;
			if (bStarboardA != bStarboardB)
			{
				return bStarboardA;
			}
			return A.LocalPosition.X < B.LocalPosition.X;
		});

	// HOW MANY BAYS EACH SIDE HAS, counted before any is placed, because the EXIT sits inboard
	// of every entry on its side and so cannot be positioned until they are all known.
	int32 PortBays = 0;
	int32 StarboardBays = 0;
	for (const FEntityAnchor* Anchor : Serviced)
	{
		(Anchor->LocalPosition.Y >= 0.0 ? StarboardBays : PortBays)++;
	}

	// HOW FAR THE OUTERMOST AFT-EDGE NODE SITS FROM ITS LANE, and it is squeezed from BOTH
	// sides, which is why it is derived rather than chosen.
	//
	// TOO SMALL AND THE SERVE LEG FOLDS AT THE PARK END. The run from a park pose out to its
	// lane is Diagonalise * sqrt(2) long, and the 45 degree corner where it meets the lane
	// wants Diagonal of that; give it less and the bend starts behind the leg's own first
	// point. Measured at LaneGap 200: a 283 uu diagonal against the 345 needed, and the leg
	// came back 179 degrees on itself.
	//
	// TOO LARGE AND IT FOLDS AT THE OTHER END. The 45 degree run meets the lane at
	// BackX + (Lane - EntryY), so pushing the entries inboard pushes that meeting point
	// FORWARD, and it has to land at least Diagonal + Square short of the aft-most bay's
	// turn-in. Measured at LaneGap 400: 1480 uu of lane against the 1433 those two corners
	// need, which passes by 47 - a margin of the kind this whole piece exists to stop
	// shipping.
	//
	// Diagonal itself sits between the two with room at each end: the diagonal comes out at
	// 488 uu against 345 needed, and the lane run at 1735 against 1433.
	const double LaneGap = Diagonal;

	Definition.ServiceBays.Reset();
	int32 PortSlot = 0;
	int32 StarboardSlot = 0;

	for (const FEntityAnchor* Anchor : Serviced)
	{
		const double Side = Anchor->LocalPosition.Y >= 0.0 ? 1.0 : -1.0;
		const int32 Slot = Side > 0.0 ? StarboardSlot++ : PortSlot++;

		const double Lane = Side * LaneY;

		// THE ENTRIES FILL THE AFT EDGE FROM THE LANE INWARD AND THE EXIT GOES INNERMOST.
		//
		// THAT ORDER IS FORCED, not a preference. The aft-most bay is sorted to slot 0 because
		// its turn-in is furthest aft and so it has the least lane to do its shift in - it
		// therefore needs the entry NEAREST the lane, the one whose 45 degree run meets the
		// lane soonest. Giving that slot to the exit pushed every bay one pitch inboard and
		// folded the aft-most serve leg back on itself.
		//
		// A starboard bay's entry may end up on the port half of the back edge when a side has
		// three of them, which is harmless: everything at ParkRowX is astern of the tail, and
		// the two sides only have to stay apart where they run alongside the aeroplane.
		// EACH SIDE COUNTS INBOARD FROM ITS OWN LANE, and the two ladders therefore approach
		// each other in the middle. WITH THREE BAYS ON ONE SIDE THEY COLLIDE: measured at 175
		// uu between the starboard exit and the port one, against the 759 their road fillets
		// need, which is what StandLinkClearsTheTruckLock reports as two unusable sweeps.
		//
		// ONE GLOBAL LADDER WAS TRIED AND IS WORSE, which is why this is written down rather
		// than left as an obvious improvement. It cannot collide, but it puts a side's exit up
		// to 3121 uu inboard of that side's lane, and the depart leg's 45 degree turn off the
		// lane then lands FORWARD of the aft-most bay's own clear pose - the leg folds back on
		// itself at 179 degrees. Six road contacts at this pitch need 3795 uu of aft edge with
		// the outermost within 845 of its lane, so the real lever is the stand's minimum WIDTH,
		// which is a band this project chooses. 5300 does not hold them; about 5900 does.
		const double EntryY = Lane - Side * (LaneGap + ParkRun + Slot * AftPitch);

		// THE EXIT SITS ONE ParkRun IN FROM ITS LANE, which is the position that makes its
		// depart turn exactly 45 degrees at a vertex ParkRun forward of the back edge.
		//
		// IT IS ONLY LaneGap FROM THE OUTERMOST ENTRY, WHICH IS SHORT OF AftPitch, and that is
		// the one thing in this layout still measured as wrong: two road contacts that close
		// leave the fillet between them clamped, and StandLinkClearsTheTruckLock reports the
		// pair as 212 and 15 uu against a lock of 699. Placing it on the ladder instead moves
		// it far enough inboard that its own depart leg folds. Six contacts at this pitch want
		// 3795 uu of aft edge with the outermost within 845 of its lane; 5300 does not hold
		// them and about 5900 does, so the lever is the minimum WIDTH rather than the packing.
		const double ExitY = Lane - Side * ParkRun;
		const double ParkY = EntryY + Side * ParkRun;
		const FVector2D Service = Anchor->LocalPosition;

		FServiceBay Bay;
		Bay.AnchorId = Anchor->Id;

		// THE SLOT, at 45 degrees, pointing forward and outboard so the vehicle parks already
		// aimed at the lane it will leave along.
		Bay.ParkLocal = FVector2D(ParkRowX, ParkY);
		Bay.ParkHeading = Side * 0.25 * UE_DOUBLE_PI;

		// ITS OWN ENTRY on the back edge, on the same 45 degree line, so the arrive leg is a
		// STRAIGHT and every corner of it belongs to the road junction rather than to the stand.
		Bay.EntryLocal = FVector2D(BackX, EntryY);
		Bay.EntryHeading = Bay.ParkHeading;

		// AND THE EXIT, which the side shares: a vehicle leaves along its lane, and one way out
		// per side is one road junction per side rather than one per service.
		//
		// ANGLED LIKE THE SLOTS, AND FOR THE SAME REASON. It left straight along the lane
		// until 2026-09-17, which meets a road running behind the stand at a RIGHT ANGLE - and
		// a square corner costs CornerRunFor(R, 90) = 1088 uu of run on each arm where the gap
		// to the road is whatever the player left, measured at 400. The merge delivered 309 uu
		// against a lock of 699. Turned 45 degrees aft-and-inboard it is the same corner the
		// entries make, needs 345, and fits in the same 400.
		//
		// INBOARD RATHER THAN OUTBOARD because outboard leaves the stand: the lane already runs
		// at half the width less half a lane, so turning away from the aeroplane puts the exit
		// on the neighbour's ground.
		Bay.ExitLocal = FVector2D(BackX, ExitY);
		Bay.ExitHeading = -Side * 0.75 * UE_DOUBLE_PI;

		BuildLeg({ Bay.EntryLocal, Bay.ParkLocal }, Radius, TEXT("arrive"), Bay.ArriveLeg);

		// SERVE: out along the 45 to the lane, forward to abeam the service point, then square
		// inboard to it. The turn inboard is where the wing would be if the fixture were not
		// placed clear of it - see BuildCodeCStandFor, and the test that measures it.
		// SIGNED, NEVER AN ABSOLUTE. How far the park pose has to move OUTBOARD to reach its
		// lane, measured along its own side's outward direction - which is not |ParkY| the
		// moment a slot sits across the centreline, as the third slot on a side now does.
		const double Diagonalise = (Lane - ParkY) * Side;
		BuildLeg({
			Bay.ParkLocal,
			FVector2D(ParkRowX + Diagonalise, Lane),
			FVector2D(Service.X, Lane),
			Service }, Radius, TEXT("serve"), Bay.ServeLeg);

		// REVERSE: straight out to the lane and square onto it, backwards. The vehicle ends
		// facing the way it came, which is what makes this the turn-round - it drives away
		// forwards without ever retracing this curve, so the curve only has to satisfy the
		// REVERSE limit and the 30% that buys is not spent on a path that works both ways.
		const FVector2D Cleared(Service.X + SquareBack, Lane);
		BuildLeg({ Service, FVector2D(Service.X, Lane), Cleared },
			ReverseRadius, TEXT("reverse"), Bay.ReverseLeg);

		// DEPART: back down the lane, then 45 degrees off it, so the vehicle reaches the aft
		// edge already pointing at the road.
		//
		// THE TURN VERTEX IS ParkRun FORWARD OF THE BACK EDGE, which with the exit ParkRun in
		// from the lane makes the corner exactly 45 degrees.
		BuildLeg({ Cleared, FVector2D(BackX + ParkRun, Lane), Bay.ExitLocal },
			Radius, TEXT("depart"), Bay.DepartLeg);

		Definition.ServiceBays.Add(MoveTemp(Bay));
	}

	// WHAT THE LAYOUT ACTUALLY NEEDS, measured off every point of every leg - never typed, and
	// never off the DESIGN aircraft. Width is the greater of the paint's own reach and the
	// airframe the LETTER admits with its wingtip clearance; depth likewise runs from that
	// airframe's nose to the aft-most thing laid.
	double MinX = -TailAft;
	double MaxX = NoseFwd;
	double MaxAbsY = 0.5 * Width;
	auto Cover = [&MinX, &MaxX, &MaxAbsY](const FVector2D& At)
	{
		MinX = FMath::Min(MinX, At.X);
		MaxX = FMath::Max(MaxX, At.X);
		MaxAbsY = FMath::Max(MaxAbsY, FMath::Abs(At.Y));
	};
	for (const FServiceBay& Bay : Definition.ServiceBays)
	{
		for (const FStandLeg* Leg : { &Bay.ArriveLeg, &Bay.ServeLeg, &Bay.ReverseLeg, &Bay.DepartLeg })
		{
			TArray<FVector2D> Sampled;
			Leg->Sample(Sampled);
			for (const FVector2D& At : Sampled)
			{
				Cover(At);
			}
		}
	}

	Definition.RequiredExtent = FVector2D(2.0 * MaxAbsY, MaxX - MinX);

	// READ THESE RATHER THAN TRUST THEM. The poses are written from the geometry and the
	// figures they imply are what say whether that geometry was right.
	UE_LOG(LogAirside, Log,
		TEXT("Stand template '%s': box %.0f x %.0f (x %.0f..%.0f), radius fwd %.1f rev %.1f, "
		     "corner square %.0f diagonal %.0f back %.0f, lane y %.0f, park row x %.0f, "
		     "%d bay(s), needs %.0f x %.0f"),
		*Letter, Width, Depth, BackX, NoseFwd, Radius, ReverseRadius,
		Square, Diagonal, SquareBack, LaneY, ParkRowX,
		Definition.ServiceBays.Num(), Definition.RequiredExtent.X, Definition.RequiredExtent.Y);

	for (const FServiceBay& Bay : Definition.ServiceBays)
	{
		UE_LOG(LogAirside, Log,
			TEXT("  bay '%s': entry (%.0f, %.0f) park (%.0f, %.0f) hdg %.0f exit (%.0f, %.0f) "
			     "legs %d/%d/%d/%d"),
			*Bay.AnchorId.ToString(), Bay.EntryLocal.X, Bay.EntryLocal.Y,
			Bay.ParkLocal.X, Bay.ParkLocal.Y, FMath::RadiansToDegrees(Bay.ParkHeading),
			Bay.ExitLocal.X, Bay.ExitLocal.Y,
			Bay.ArriveLeg.Points.Num(), Bay.ServeLeg.Points.Num(),
			Bay.ReverseLeg.Points.Num(), Bay.DepartLeg.Points.Num());
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

	// AND NO SERVICE BAYS. A bay is where a vehicle waits to work on an aeroplane; a depot has
	// one pose and nothing parked at it, and its pose IS its road connection. Reset for the
	// same reason Anchors is.
	Definition->ServiceBays.Reset();
	Definition->RequiredExtent = FVector2D::ZeroVector;

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
