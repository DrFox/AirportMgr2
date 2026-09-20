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
	// the dispenser stands just forward of it and reaches in; the hold is where an airliner's
	// forward hold is.
	//
	// ONE BAGGAGE BAY, NOT TWO, ruled 2026-09-17, and it unlocked the layout rather than merely
	// simplifying it. The aft hold sat at -2400, ASTERN of the wing, which made it the aft-most
	// thing any vehicle drove to - and the aft-most bay is the one whose serve leg has the least
	// lane to finish its shift in, so it alone capped the aft-edge pitch at 879 uu when the road
	// fillets wanted 911. With it gone, NOTHING on this side is astern of the wing: the aft-most
	// service is the hydrant at -800 and the ceiling it leaves goes from 388 uu to about 1760.
	// The constraint is not relieved, it is absent.
	constexpr double RowY = 700.0;
	const double PitX = WingFwd + PlantClearance;
	const double HoldX = WingFwd + 600.0;

	AddFixture(TEXT("HydrantPit"), PitX, RowY, -90.0, EServiceRole::Fuel);
	AddFixture(TEXT("BaggageHold"), HoldX, RowY, -90.0, EServiceRole::Baggage);

	// HEADING -90, which is INBOARD on the starboard side: a vehicle at its service point faces
	// the aeroplane it is working on. It drives in forwards and reverses out, so this is also
	// the heading its reverse leg starts from - see FServiceBay.

	// Fixed ground power at the bridge, off the port bow, facing inboard for the same reason.
	AddFixture(TEXT("FixedGPU"), WingFwd + 600.0, -RowY, 90.0, EServiceRole::GPU);

	// WHERE PASSENGERS BOARD, at the forward port door where a bridge or a set of stairs meets
	// the aeroplane. Added 2026-09-17: the stand had fuel, bags, power and a tug, and no way for
	// anybody to get on or off - a gap in the stand rather than in its geometry.
	//
	// NO BAY, AND THAT FALLS OUT OF ITS ROLE RATHER THAN BEING DECIDED HERE. Passenger is a
	// PEDESTRIAN class (see TraversalForRole) and the bay rule asks for GroundVehicle, so an air
	// bridge - a structure, which drives nowhere - correctly gets none. Model stairs instead and
	// the truck that brings them IS a ground vehicle, gaining a bay by the same rule with
	// nothing here to change.
	AddFixture(TEXT("PassengerDoor"), 200.0, -300.0, 90.0, EServiceRole::Passenger);

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

	// WHERE THE ROAD MEETS THE STAND, and since 2026-09-17 it meets it SQUARE.
	//
	// A 45 DEGREE CONTACT IS ASYMMETRIC, WHICH IS WHY THIS CHANGED. Turning off a road onto a
	// 45 degree pose costs CornerRunFor(R, 135) = 345 uu from one direction and
	// CornerRunFor(R, 45) = 4853 from the other, and a road 4 m behind a stand offers 566. The
	// builder clamped the second fillet to the run it had and laid a 55 uu corner - a 0.55 m
	// turning circle, which is a pirouette and not the shunt the 2026-09-17 ruling imagined -
	// and RouteSearch::EdgeCost has no curvature term, so the search took it BECAUSE it was
	// also the shorter of the pair. Reported from PIE the same day: "very tight hairpins to
	// get onto the stand parking that the vehicles cannot make".
	//
	// 90 DEGREES IS THE ONLY HEADING THAT COSTS THE SAME BOTH WAYS, Square either side, so it
	// is the only one a two-way road can use. What a square corner needs is Square of STRAIGHT
	// on the stand side, and that cannot come from the gap between the road and the back edge
	// because the PLAYER chooses that gap - 4 m is legal. It comes from the spur instead: the
	// contact leads into a straight at least Square long before its first bend, so the corner
	// has its run at any gap, including none.
	//
	// AND SQUARE CONTACTS DO NOT PACK, which is the other half of it. A square fillet reaches
	// Square along the road where a 45 reaches Diagonal, so two neighbours need three times
	// Square between them where the old ladder needed three times Diagonal - 33 m against 10.
	// Five contacts wanted 87 m of a 59 m edge. So there is ONE per side now, entry and exit
	// at the same pose, and the ladder moved inside the stand where the branches are one-way
	// and may stay at 45 degrees.
	const double ContactSpan = 3.0 * Square;

	// HOW FAR APART THE BRANCHES OFF A SPUR SIT. Twice Diagonal, not the three times the aft
	// edge wanted: those were ROAD contacts, and the third Diagonal paid for the SPLIT each one
	// makes in the road, which shortens the segment its neighbour's fillet works in. A branch
	// splits nothing - every bay lays its own copy of the spur as its own edge - so this has
	// only to keep the park poses apart, and at 2 x Diagonal the echelon is 488 uu between
	// neighbours measured across the branches.
	const double BranchPitch = 2.0 * Diagonal;

	// A BAY PER ANCHOR A GROUND VEHICLE SERVICES FROM.
	//
	// ASKED AS "IS THIS A GROUND VEHICLE", not as "is this not the aeroplane". The two differ the
	// moment a stand has a PEDESTRIAN anchor - a boarding door served by an air bridge, which is
	// a structure and drives nowhere - and the loose form would hand it four legs and a road
	// entry that nothing would ever use.
	//
	// THE TUG IS EXCLUDED BY NAME, because it IS a ground vehicle and still wants no bay:
	// pushback couples at the nose gear and is FPushbackRun's manoeuvre, so a tug never drives
	// from a parking bay to a service point.
	TArray<const FEntityAnchor*> Serviced;
	for (const FEntityAnchor& Anchor : Definition.Anchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::GroundVehicle
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

	// HOW FAR OUTBOARD THE TWO CONTACTS SIT, DERIVED FROM THE SERVICES THEY FEED.
	//
	// SQUEEZED FROM BOTH ENDS, like everything else on this stand. Too far inboard and a bay's
	// 45 degree branch meets its lane too far FORWARD: the meeting has to land Diagonal + Square
	// short of that bay's turn-in, or the two corners overlap and neither delivers its radius.
	// Too far outboard and the branch has no room to reach the lane at all.
	//
	// THE MEETING POINT DOES NOT DEPEND ON ParkRun, which is what makes this solvable in one
	// pass rather than two. The park pose sits ParkRun along the branch, and the branch then has
	// (LaneY - ContactMag - ParkRun) of outboard left to run; the two cancel, and the meeting
	// lands at BranchX + (LaneY - ContactMag) wherever along the branch the pose is put.
	//
	// PER SLOT, NEVER PER STAND. A bay further down its side's ladder branches further forward,
	// so it meets its lane further forward too - the FORWARD-most bay on a side is the binding
	// one, not the aft-most, and checking only one of them is a bug this file has already had.
	double ContactFloor = 0.5 * ContactSpan;
	const FEntityAnchor* Binding = nullptr;
	{
		int32 PortAt = 0;
		int32 StarboardAt = 0;
		for (const FEntityAnchor* Anchor : Serviced)
		{
			const int32 At = Anchor->LocalPosition.Y >= 0.0 ? StarboardAt++ : PortAt++;
			const double BranchX = BackX + Square + Diagonal + At * BranchPitch;
			const double Wants =
				LaneY + BranchX - Anchor->LocalPosition.X + Diagonal + Square;
			if (Wants > ContactFloor)
			{
				ContactFloor = Wants;
				Binding = Anchor;
			}
		}
	}

	// AT MOST WHAT LEAVES THE BRANCH A LEGAL RUN: ParkRun is at least its own corner's Diagonal,
	// and the corner onto the lane wants Diagonal of the diagonal that is left over.
	const double ParkRunFloor = Diagonal;
	const double LaneCorner = LegSlack * Diagonal / UE_DOUBLE_SQRT_2;
	const double ContactCeiling = LaneY - ParkRunFloor - LaneCorner;

	if (ContactCeiling < ContactFloor)
	{
		// NOT SILENTLY WRONG. The layout is still laid - a shape somebody has to fix is more use
		// than no shape - but no contact position serves every bay, and the drivability test will
		// report the fold with its figure.
		UE_LOG(LogAirside, Warning,
			TEXT("Stand template '%s': no legal road contact - '%s' at %.0f wants it at least "
			     "%.0f outboard and the branch run allows at most %.0f. That bay's serve leg will "
			     "fold."),
			*Letter,
			Binding != nullptr ? *Binding->Id.ToString() : TEXT("?"),
			Binding != nullptr ? Binding->LocalPosition.X : 0.0, ContactFloor, ContactCeiling);
	}

	// THE MIDPOINT OF EACH BAND, so neither end is the one that fails first.
	const double ContactMag =
		FMath::Max(ContactFloor, 0.5 * (ContactFloor + ContactCeiling));
	const double ParkRunCeiling = (LaneY - ContactMag) - LaneCorner;
	const double ParkRun = FMath::Max(ParkRunFloor, 0.5 * (ParkRunFloor + ParkRunCeiling));

	Definition.ServiceBays.Reset();
	int32 PortSlot = 0;
	int32 StarboardSlot = 0;

	for (const FEntityAnchor* Anchor : Serviced)
	{
		const double Side = Anchor->LocalPosition.Y >= 0.0 ? 1.0 : -1.0;
		const int32 Slot = Side > 0.0 ? StarboardSlot++ : PortSlot++;

		const double Lane = Side * LaneY;

		const FVector2D Service = Anchor->LocalPosition;

		FServiceBay Bay;
		Bay.AnchorId = Anchor->Id;

		// ONE CONTACT PER SIDE, AND ENTRY AND EXIT ARE THE SAME POSE. Two square contacts want
		// ContactSpan between them and the band outboard of the wingtip is not that wide, so they
		// share one. The lane is two-way in consequence - which it already was, since every depart
		// leg on a side ran back down the lane its serve legs had come up - and making two vehicles
		// take turns over it is ClaimServiceBay's job rather than the geometry's.
		//
		// STRAIGHT IN AND STRAIGHT OUT, because the heading is exactly what the road corner is
		// measured against. Square to the back edge is square to a road drawn behind it, and that
		// is the only heading a vehicle can reach from either direction for the same price.
		//
		// AND THE POSE SITS Square INSIDE THE BACK EDGE, not on it, which is the part that makes
		// the heading pay off. FAnchorLink measures the corner's stand-side arm from where the
		// entry's own line crosses the road TO THE ENTRY NODE - see the MeetsAt test in Join -
		// so a pose on the boundary offers that corner nothing but the gap, and the gap belongs
		// to the PLAYER: at the 4 m the fixture draws, a square corner wanting 1088 uu got 400,
		// fell through to the lane-change branch, and delivered a 1 uu sweep. Declaring the pose
		// Square in hands the same corner gap + 1088 whatever the player left, including none.
		//
		// THE LEAD-IN THEREFORE CROSSES STAND GROUND the layout did not lay, which is correct
		// rather than merely tolerable: the apron between the boundary and this pose is the
		// stand's, the vehicle is entering it, and the alternative is a corner whose length
		// depends on how close somebody drew a road.
		const double Contact = Side * ContactMag;

		Bay.EntryLocal = FVector2D(BackX + Square, Contact);
		Bay.EntryHeading = 0.0;
		Bay.ExitLocal = Bay.EntryLocal;
		Bay.ExitHeading = UE_DOUBLE_PI;

		// WHERE THIS BAY LEAVES THE SPUR, Diagonal on from the contact - that being the 45
		// degree corner's own arm, and the only run between the two that belongs to the stand.
		const double BranchX = BackX + Square + Diagonal + Slot * BranchPitch;

		// THE SLOT, at 45 degrees on its branch, pointing forward and outboard so the vehicle
		// parks already aimed at the lane it will leave along.
		Bay.ParkLocal = FVector2D(BranchX + ParkRun, Contact + Side * ParkRun);
		Bay.ParkHeading = Side * 0.25 * UE_DOUBLE_PI;

		// ARRIVE: square in off the road, then 45 degrees onto this bay's branch. The straight
		// between them is what gives the ROAD's corner its run, which is why it is Square long
		// before any of this bends - the gap behind the stand belongs to the player and cannot
		// be relied on for a single uu of it.
		BuildLeg({ Bay.EntryLocal, FVector2D(BranchX, Contact), Bay.ParkLocal },
			Radius, TEXT("arrive"), Bay.ArriveLeg);

		// SERVE: out along the 45 to the lane, forward to abeam the service point, then square
		// inboard to it. The turn inboard is where the wing would be if the fixture were not
		// placed clear of it - see BuildCodeCStandFor, and the test that measures it.
		// SIGNED, NEVER AN ABSOLUTE. How far the park pose has to move OUTBOARD to reach its
		// lane, measured along its own side's outward direction - which is not an absolute the
		// moment a slot sits across the centreline, as one did while the ladder was on the aft
		// edge. It reads the pose rather than recomputing it, so the leg cannot disagree with
		// the node it starts from.
		const double Diagonalise = (Lane - Bay.ParkLocal.Y) * Side;
		BuildLeg({
			Bay.ParkLocal,
			FVector2D(Bay.ParkLocal.X + Diagonalise, Lane),
			FVector2D(Service.X, Lane),
			Service }, Radius, TEXT("serve"), Bay.ServeLeg);

		// REVERSE: straight out to the lane and square onto it, backwards. The vehicle ends
		// facing the way it came, which is what makes this the turn-round - it drives away
		// forwards without ever retracing this curve, so the curve only has to satisfy the
		// REVERSE limit and the 30% that buys is not spent on a path that works both ways.
		const FVector2D Cleared(Service.X + SquareBack, Lane);
		BuildLeg({ Service, FVector2D(Service.X, Lane), Cleared },
			ReverseRadius, TEXT("reverse"), Bay.ReverseLeg);

		// DEPART: back down the lane, then 45 degrees inboard onto the contact spur, so the
		// vehicle reaches the back edge already square to the road and leaves by the same corner
		// it arrived through, mirrored.
		//
		// THE SHIFT VERTEX IS SET, NOT CHOSEN. The final straight has to carry Square for the
		// road's corner plus Diagonal for this one, and a 45 degree shift costs one of x for every
		// one of y - so the vertex sits exactly that far forward, plus the shift itself.
		const double Shift = LaneY - ContactMag;
		const double ShiftVertex = BackX + Square + Diagonal + Shift;
		BuildLeg({ Cleared, FVector2D(ShiftVertex, Lane),
			FVector2D(ShiftVertex - Shift, Contact), Bay.ExitLocal },
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
		// THE RUN IN FRONT OF THE CONTACT IS GROUND THE LAYOUT NEEDS, though no leg is laid
		// across it. The contact pose sits Square inside the back edge precisely so the road's
		// corner has that run, and FAnchorLink lays the lead-in over it - so it is used, it is
		// this stand's, and a stand claiming only as far back as its first leg measures a Code
		// C's depth as a Code B's.
		Cover(FVector2D(Bay.EntryLocal.X - Square, Bay.EntryLocal.Y));

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
		     "corner square %.0f diagonal %.0f back %.0f, lane y %.0f, branch pitch %.0f, "
		     "contact y %.0f (band %.0f..%.0f), park run %.0f (band %.0f..%.0f), %d bay(s), "
		     "needs %.0f x %.0f"),
		*Letter, Width, Depth, BackX, NoseFwd, Radius, ReverseRadius,
		Square, Diagonal, SquareBack, LaneY, BranchPitch,
		ContactMag, ContactFloor, ContactCeiling,
		ParkRun, ParkRunFloor, ParkRunCeiling,
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

	// BANDS, NOT SCATTER. The sampled yard put 23 sheds, 9 tanks and 12 pumps wall to wall
	// across a 45 m plot - 65% coverage, with no ground left for a truck to reach any of it.
	// See the 2026-09-20 layout-strategies design.
	Definition->Layout = EPlotLayout::FuelYardBands;

	// HALF-extents of ONE BAY: 4 m by 8 m. It used to be the whole site - 12 m by 8 m, "a
	// tank, a pump, and room to turn a bowser round" - but a depot is DRAWN now, so the site
	// is whatever the player outlined and the only fixed extent left is the module that
	// fills a bay.
	//
	// The old 12 m was not wrong, and it is where the 4 m bay came from: it is exactly three
	// of them, which is the Tier 1 depot on the concept sheet - one shed, one tank, one
	// pump. See Solve/PlotFit.h.
	Definition->FootprintExtent = FVector2D(200.0, 400.0);

	// ONE truck, and this is now the PLOTLESS default rather than the whole story. A depot
	// DRAWN as a plot takes its count from the sheds the player built in it - see
	// URoadNetwork::PlaceEntity(const FEntityPlacement&), where a non-empty module list
	// overrides this outright. A depot placed without a plot still needs a number from
	// somewhere, and this is it.
	//
	// It was briefly zeroed when the derivation landed, on the reasoning that a definition
	// cannot know how many bays a plot holds. True, but it does not follow: the plotless
	// path never asks about bays, and zeroing this silently gave every pre-plot caller a
	// depot that could not dispatch. Six fuel tests said so immediately.
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
