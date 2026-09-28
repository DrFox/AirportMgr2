#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TaxiwayStrip.h"
#include "Model/TrafficOccupancy.h"
#include "Entities/AircraftType.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckCrossesTaxiwayTest,
	"Airside.Model.Traffic.TruckCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	// A hand-authored crossing: a taxiway east-west through the origin and a road
	// north-south through it, sharing the CENTRE node. Hand-authored rather than solved,
	// like every M2 traffic fixture, because what is under test is the ARBITER and a fixture
	// that had to pave a junction first would fail for reasons that are not this test's.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId Centre = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId West   = Net->AddGuidelineNode(FVector2D(-30000.0, 0.0), false);
	const FGuidelineNodeId East   = Net->AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	const FGuidelineNodeId South  = Net->AddGuidelineNode(FVector2D(0.0, -8000.0), false);
	const FGuidelineNodeId North  = Net->AddGuidelineNode(FVector2D(0.0, 30000.0), false);

	auto Link = [Net](FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = false;
		Net->AddGuidelineEdge(MoveTemp(Edge));
	};
	Link(West, Centre, ETraversalClass::Aircraft);
	Link(Centre, East, ETraversalClass::Aircraft);
	Link(South, Centre, ETraversalClass::GroundVehicle);
	Link(Centre, North, ETraversalClass::GroundVehicle);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	auto Route = [Net](FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class)
	{
		return TestGraph::Probe(*Net, Start, Goal, Class);
	};

	const FRoutePlan AircraftPlan = Route(West, East, ETraversalClass::Aircraft);
	const FRoutePlan TruckPlan = Route(South, North, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the aircraft routes along the taxiway"), AircraftPlan.IsValid())) { return false; }
	if (!TestTrue(TEXT("the truck routes across it"), TruckPlan.IsValid())) { return false; }

	// EACH ONLY EVER GETS ITS OWN CLASS OF LINE. Pinned here as well as in
	// Airside.Build.RoadCrossesTaxiway, because that test measures what the BUILDER derives
	// and this one measures what the SEARCH will actually hand an agent.
	TestFalse(TEXT("a truck cannot route down the taxiway"),
		Route(West, East, ETraversalClass::GroundVehicle).IsValid());
	TestFalse(TEXT("and an aircraft cannot route down the road"),
		Route(South, North, ETraversalClass::Aircraft).IsValid());

	// A SLOW TAXI, and the reason is the whole design of this fixture.
	//
	// Dispatching both at once and hoping they meet does NOT test priority: measured on the
	// first attempt, the two crossed 788 uu apart with NEITHER ever stopping, because their
	// claim windows on the shared node were about three seconds each and simply missed. That
	// test would have passed a build in which class priority did nothing at all.
	//
	// So the contest is ARRANGED rather than hoped for: the aircraft taxis at 2 m/s (a real
	// figure - a heavy on a tight apron does about this), which widens the window it holds
	// the crossing for to something a truck can arrive inside, and the truck is not
	// dispatched until the table SAYS the aircraft holds the node. What is then measured is
	// who gives way, which is the only thing this test is about.
	FAirframe SlowPlane = UAirsideSettings::ResolveDefaultAirframe();
	SlowPlane.Chassis.Ground.Taxi.SpeedCap = 200.0;

	const int32 Aircraft = Traffic->DispatchAgent(Net, AircraftPlan, SlowPlane,
		ETraversalClass::Aircraft, 0.0);
	if (!TestTrue(TEXT("the aircraft is under way"), Aircraft != 0)) { return false; }

	// Run it up to the crossing and stop when the TABLE says it holds the node - not when a
	// stopwatch says it ought to. The claim is the thing the truck will be refused by, so
	// the claim is what this waits for.
	bool bPlaneHoldsCrossing = false;
	for (int32 Step = 0; Step < 6000 && !bPlaneHoldsCrossing; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		int32 Holder = 0;
		bPlaneHoldsCrossing =
			Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Centre), 0, &Holder)
			&& Holder == Aircraft;
	}
	if (!TestTrue(TEXT("the aircraft comes to hold the crossing"), bPlaneHoldsCrossing)) { return false; }

	const int32 Truck = Traffic->DispatchAgent(Net, TruckPlan,
		UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle, 0.0);
	if (!TestTrue(TEXT("and the truck sets off into it"), Truck != 0)) { return false; }

	// Drive them both and watch the closest they ever get. AIRCRAFT OVER VEHICLE is a fact
	// about the CLASSES (TraversalPriority, spec 5.4) and needs no authoring at the junction
	// - which is exactly the property worth measuring, because a crossing that happened to
	// work by timing would pass a test that only checked arrival.
	const FVector2D CrossingAt = Net->GetGuidelineNode(Centre)->Position;

	double Closest = TNumericLimits<double>::Max();

	// THE SAFETY PROPERTY: how near the truck ever got to the crossing WHILE THE AIRCRAFT
	// HELD IT. That is what "gives way" means mechanically, and unlike a plain separation it
	// is something the arbiter actually promises.
	double NearestWhileHeld = TNumericLimits<double>::Max();

	bool bTruckWaited = false;
	bool bTruckArrived = false;
	int32 TruckStoppedSteps = 0;

	for (int32 Step = 0; Step < 6000; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);

		const FRoadAgent* Plane = Traffic->FindAgent(Aircraft);
		const FRoadAgent* Van = Traffic->FindAgent(Truck);
		if (Van == nullptr)
		{
			break;
		}
		bTruckArrived |= Van->Phase == EAgentPhase::Parked;
		TruckStoppedSteps += (Van->Phase == EAgentPhase::Taxiing
			&& Van->LastMotion.GroundSpeed <= 0.0) ? 1 : 0;

		int32 Holder = 0;
		if (Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Centre), 0, &Holder)
			&& Holder == Aircraft)
		{
			NearestWhileHeld = FMath::Min(NearestWhileHeld,
				FVector2D::Distance(Van->LastMotion.Position, CrossingAt));
		}

		if (Plane != nullptr)
		{
			Closest = FMath::Min(Closest,
				FVector2D::Distance(Plane->LastMotion.Position, Van->LastMotion.Position));

			// A YIELD IS A SPEED DROP WHILE REFUSED, NOT A DEAD STOP - the same correction
			// Airside.Model.Traffic.NodeYield already carries, and for the same arithmetic.
			// The yielder is refused at its own braking distance and told to stop at
			// End - Gap, so it reaches a standstill only if
			// Gap + Footprint_blocker/2 >= v^2/(2 Decel). Here that is 300 + 500 = 800 uu
			// against 2500, so it cannot.
			//
			// IT USED TO REACH ZERO, and that was a BUG WEARING A PASS. The truck arrived at
			// the crossing crawling at MinTaxiSpeed, because FSpeedProfile dropped it there
			// at every corner tighter than the steering lock - at 50 uu/s the braking
			// distance is 6 uu and anything stops dead. Fixing that on 2026-09-15 let the
			// truck arrive at speed, and this assertion started failing on a truck behaving
			// better than before. Measured after: refused, slowed, 512 uu clear of the node.
			//
			// WaitingOn names who refused it, so this still cannot be satisfied by the truck
			// slowing for any other reason - a corner, or its own destination.
			bTruckWaited |= Van->Phase == EAgentPhase::Taxiing
				&& Van->GetWaitingOn() == Aircraft
				&& Van->LastMotion.GroundSpeed < 0.5 * Van->Chassis().Ground.Taxi.SpeedCap;
		}
	}

	// MIN SEPARATION, MEASURED (spec §8) and reported rather than asserted against a
	// Euclidean threshold.
	//
	// A threshold of half-footprint-plus-half-footprint was tried and is WRONG PHYSICS for
	// this model: FTrafficRules footprints are lengths ALONG A ROUTE, not radii, and two
	// agents on perpendicular lines legitimately pass with their centres closer than the sum
	// of their half-lengths - measured at 698 uu against a 750 uu "touching" figure, with the
	// truck correctly waiting throughout. Asserting it would have failed a build that was
	// behaving exactly as designed.
	AddInfo(FString::Printf(
		TEXT("min separation %.0f uu; truck came no nearer than %.0f uu to the crossing while "
			 "the aircraft held it; truck stopped for %d step(s)"),
		Closest, NearestWhileHeld, TruckStoppedSteps));

	TestTrue(TEXT("the truck gave way to the aircraft at the crossing"), bTruckWaited);

	// AND IT STAYED OFF THE CROSSING WHILE THE AIRCRAFT OWNED IT. This is the property the
	// arbiter actually promises, and the one a truck driving straight through would break:
	// the truck's centre never came within its own half-footprint of the node while the
	// table said the aircraft held it.
	TestTrue(*FString::Printf(
		TEXT("the truck stayed off the crossing while the aircraft held it: nearest %.0f uu"),
		NearestWhileHeld),
		NearestWhileHeld > Traffic->Rules.VehicleFootprint * 0.5);

	// AND THE TRUCK STILL GETS THERE. A yield that never released would satisfy every
	// assertion above and be exactly the starvation the resolver exists to prevent.
	TestTrue(TEXT("the truck reaches the far side"), bTruckArrived);
	return true;
}

namespace
{
	/**
	 * A route for Class from the alive node nearest From to the one nearest To, trying every
	 * pair within Radius (a dead-end taxiway ends at its cut, 11.5 m short of
	 * the node): which lane end leaves which way is the drive side's business, not
	 * this test's. Named for this file because the tests module is a UNITY build.
	 */
	FRoutePlan DerivedCrossingRoute(const URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, double Radius = 2000.0)
	{
		TArray<FGuidelineNodeId> Starts, Goals;
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			if (FVector2D::Distance(Nodes[Index].Position, From) < Radius) { Starts.Add(Net.GuidelineNodeIdAt(Index)); }
			if (FVector2D::Distance(Nodes[Index].Position, To) < Radius) { Goals.Add(Net.GuidelineNodeIdAt(Index)); }
		}
		for (const FGuidelineNodeId Start : Starts)
		{
			for (const FGuidelineNodeId Goal : Goals)
			{
				FRoutePlan Plan = TestGraph::Probe(Net, Start, Goal, Class);
				if (Plan.IsValid()) { return Plan; }
			}
		}
		return FRoutePlan();
	}

	/** Where a plan's polyline first crosses the line Y = 0 - the vehicle's lane on the
	 *  taxiway centreline, which is where the conflict is. */
	bool DerivedCrossingPoint(const FRoutePlan& Plan, FVector2D& Out)
	{
		for (int32 Index = 1; Index < Plan.Polyline.Num(); ++Index)
		{
			const FVector2D P = Plan.Polyline[Index - 1];
			const FVector2D Q = Plan.Polyline[Index];
			if ((P.Y <= 0.0) != (Q.Y <= 0.0) && !FMath::IsNearlyEqual(P.Y, Q.Y))
			{
				Out = P + (Q - P) * (-P.Y / (Q.Y - P.Y));
				return true;
			}
		}
		return false;
	}

	/** Seconds from dispatch until the agent's centre is within 100 uu of At, driving ALONE. */
	double DerivedCrossingEta(const URoadNetwork& Net, const FRoutePlan& Plan, const FAirframe* Airframe,
		const FVehicle* Vehicle, ETraversalClass Class, const FVector2D& At)
	{
		UGroundTraffic* Solo = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Airframe != nullptr
			? Solo->DispatchAgent(&Net, Plan, *Airframe, Class, 0.0)
			: Solo->DispatchAgent(&Net, Plan, *Vehicle, Class, 0.0);
		const double Dt = 1.0 / 30.0;
		for (int32 Step = 0; Step < 30 * 120; ++Step)
		{
			Solo->Advance(Dt, &Net);
			const FRoadAgent* Agent = Solo->FindAgent(Id);
			if (Agent == nullptr) { break; }
			if (FVector2D::Distance(Agent->LastMotion.Position, At) < 100.0)
			{
				return (Step + 1) * Dt;
			}
		}
		return -1.0;
	}

	/** What one contest at the derived crossing measured. */
	struct FDerivedCrossingOutcome
	{
		bool bRan = false;
		bool bSharedNode = false;
		bool bVehicleArrived = false;
		/** Ticks on which the vehicle's nose was inside the strip while the aircraft was within
		 *  its own half-span of the crossing point. The safety property: zero. */
		int32 Intrusions = 0;
		/** Seconds the vehicle spent refused by the aircraft. */
		double WaitedSeconds = 0.0;
		/** Seconds the aircraft spent refused by the vehicle. */
		double AircraftWaitedSeconds = 0.0;
		double NearestNoseToCentreline = TNumericLimits<double>::Max();
	};

	/**
	 * THE CONTEST, on a DERIVED graph: an aircraft along the taxiway and a vehicle of Class
	 * along the road, dispatched so that - each driving alone - the aircraft would reach the
	 * crossing point at the instant the vehicle's nose reaches the stop line, the moment the
	 * vehicle has to decide. Arranged rather than hoped for, for TruckCrossesTaxiway's reason:
	 * two agents dispatched together and left to chance cross seconds apart and test nothing.
	 * AircraftLag shifts the aircraft's arrival that many seconds LATER (negative: earlier), so
	 * a sweep covers every order the two can meet in. bRebuild
	 * re-derives the graph twice (Review Focus 3): the first tick the vehicle waits at the stop
	 * line, and the first tick its nose is inside the strip.
	 */
	FDerivedCrossingOutcome RunDerivedCrossingContest(FAutomationTestBase& Test, ETraversalClass VehicleClass,
		bool bRebuild, double AircraftLag = 0.0)
	{
		FDerivedCrossingOutcome Out;
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
		TestGraph::Rebuild(*Net);

		// THE AUTHORED TYPE'S airframe, not TestAirframes::Piper(): that one leaves Wingspan 0,
		// and the half-span is what the safety property below is measured against.
		const FAirframe Plane = TestAirframes::PiperType()->Airframe();
		const FVehicle Van = TestAirframes::Van();

		const FRoutePlan AircraftPlan = DerivedCrossingRoute(*Net, FVector2D(-20000.0, 0.0), FVector2D(20000.0, 0.0),
			ETraversalClass::Aircraft);
		const FRoutePlan VehiclePlan = DerivedCrossingRoute(*Net, FVector2D(0.0, -20000.0), FVector2D(0.0, 20000.0),
			VehicleClass);
		if (!Test.TestTrue(TEXT("the aircraft routes along the taxiway"), AircraftPlan.IsValid())
			|| !Test.TestTrue(TEXT("the vehicle routes across it"), VehiclePlan.IsValid()))
		{
			return Out;
		}

		// WHY IT FAILED BEFORE STAGE 4: at a derived crossing every segment end derived its own
		// node, so the road's through-turn and the taxiway's crossed in geometry and shared no
		// FGuidelineNodeId - and the claim table conflicts by resource identity only. With no
		// shared node there is nothing for the rank rule to decide.
		TSet<FGuidelineNodeId> AircraftNodes;
		for (const FRouteStep& Step : AircraftPlan.Steps) { AircraftNodes.Add(Step.To); }
		for (const FRouteStep& Step : VehiclePlan.Steps) { Out.bSharedNode |= AircraftNodes.Contains(Step.To); }

		FVector2D CrossingAt;
		if (!Test.TestTrue(TEXT("the vehicle's route crosses the taxiway centreline"),
			DerivedCrossingPoint(VehiclePlan, CrossingAt)))
		{
			return Out;
		}

		const URoadProfile* TaxiwayProfile = Net->ProfileFor(*Net->GetSegment(Crossing.West));
		const double StripEdge = TaxiwayProfile->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, Crossing.West);
		const double HalfSpan = Plane.Wingspan * 0.5;

		const double AircraftEta = DerivedCrossingEta(*Net, AircraftPlan, &Plane, nullptr, ETraversalClass::Aircraft, CrossingAt);
		// The vehicle's CENTRE when its nose is on the stop line: half a footprint short of it.
		const FVector2D AtStopLine = CrossingAt - FVector2D(0.0, StripEdge + FTrafficRules().VehicleFootprint * 0.5);
		const double VehicleEta = DerivedCrossingEta(*Net, VehiclePlan, nullptr, &Van, VehicleClass, AtStopLine);
		if (!Test.TestTrue(TEXT("each reaches the crossing driving alone"), AircraftEta > 0.0 && VehicleEta > 0.0))
		{
			return Out;
		}

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const double Dt = 1.0 / 30.0;
		// Dispatch times that put the aircraft at the crossing AircraftLag after the vehicle.
		const double AircraftAt = FMath::Max(0.0, (VehicleEta + AircraftLag) - AircraftEta);
		const double VehicleAt = FMath::Max(0.0, AircraftEta - (VehicleEta + AircraftLag));
		int32 Aircraft = 0;
		int32 Vehicle = 0;

		int32 Rebuilds = 0;
		bool bRebuiltWaiting = false;
		bool bRebuiltInStrip = false;
		for (int32 Step = 0; Step < 30 * 120; ++Step)
		{
			const double Clock = Step * Dt;
			if (Aircraft == 0 && Clock >= AircraftAt)
			{
				Aircraft = Traffic->DispatchAgent(Net, AircraftPlan, Plane, ETraversalClass::Aircraft, 0.0);
			}
			if (Vehicle == 0 && Clock >= VehicleAt)
			{
				Vehicle = Traffic->DispatchAgent(Net, VehiclePlan, Van, VehicleClass, 0.0);
			}
			Traffic->Advance(Dt, Net);

			const FRoadAgent* Plan = Traffic->FindAgent(Aircraft);
			const FRoadAgent* Truck = Traffic->FindAgent(Vehicle);
			if (Plan != nullptr && Vehicle != 0 && Plan->GetWaitingOn() == Vehicle)
			{
				Out.AircraftWaitedSeconds += Dt;
			}
			if (Truck == nullptr)
			{
				if (Vehicle != 0) { break; }
				continue;
			}
			Out.bVehicleArrived |= Truck->Phase == EAgentPhase::Parked;
			if (Out.bVehicleArrived) { break; }

			const FVector2D Nose = Truck->LastMotion.Position
				+ FVector2D(FMath::Cos(Truck->LastMotion.Heading), FMath::Sin(Truck->LastMotion.Heading))
				* Traffic->Rules.VehicleFootprint * 0.5;
			Out.NearestNoseToCentreline = FMath::Min(Out.NearestNoseToCentreline, FMath::Abs(Nose.Y));
			const bool bNoseInStrip = FMath::Abs(Nose.Y) < StripEdge - TaxiwayStrip::ToleranceUu;
			const bool bAircraftAtCrossing = Plan != nullptr
				&& FVector2D::Distance(Plan->LastMotion.Position, CrossingAt) < HalfSpan;
			Out.Intrusions += (bNoseInStrip && bAircraftAtCrossing) ? 1 : 0;
			Out.WaitedSeconds += (Aircraft != 0 && Truck->GetWaitingOn() == Aircraft) ? Dt : 0.0;

			// A ROAD EDIT while the vehicle waits, and again mid-crossing: every derived node, the
			// conflicts and the stop line included, is re-made, and the agents re-resolve by
			// position (UGroundTraffic::OnGraphRebuilt), exactly as a player's edit does it.
			const bool bWaiting = Aircraft != 0 && Truck->GetWaitingOn() == Aircraft;
			if (bRebuild && ((!bRebuiltWaiting && bWaiting) || (!bRebuiltInStrip && bNoseInStrip)))
			{
				bRebuiltWaiting |= bWaiting;
				bRebuiltInStrip |= bNoseInStrip;
				TestGraph::Rebuild(*Net);
				Traffic->OnGraphRebuilt(*Net);
				++Rebuilds;
			}
		}
		if (bRebuild)
		{
			Test.TestTrue(TEXT("the graph was rebuilt while the vehicle waited at the stop line"), bRebuiltWaiting);
			Test.TestTrue(TEXT("and again with the vehicle inside the strip"), bRebuiltInStrip);
		}
		Test.AddInfo(FString::Printf(
			TEXT("%s, aircraft lag %.1f s, %d rebuild(s): aircraft eta %.2f s, vehicle eta to the stop line %.2f s; crossing at (%.0f,%.0f), strip edge %.0f uu, "
				 "half-span %.0f uu; %d intrusion tick(s); vehicle waited %.2f s, aircraft waited %.2f s; "
				 "nose nearest the centreline %.0f uu"),
			VehicleClass == ETraversalClass::Emergency ? TEXT("emergency") : TEXT("truck"), AircraftLag, Rebuilds,
			AircraftEta, VehicleEta, CrossingAt.X, CrossingAt.Y, StripEdge, HalfSpan, Out.Intrusions,
			Out.WaitedSeconds, Out.AircraftWaitedSeconds, Out.NearestNoseToCentreline));
		Out.bRan = true;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckYieldsAtDerivedCrossingTest,
	"Airside.Model.Traffic.TruckYieldsAtDerivedCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckYieldsAtDerivedCrossingTest::RunTest(const FString& Parameters)
{
	// TruckCrossesTaxiway above passes only because it HAND-AUTHORS a shared centre node. This
	// is the same contest on the graph the builder actually derives from a drawn crossing -
	// taxiway strip spec, "Holds sit at the strip edge", stage 4.
	const FDerivedCrossingOutcome Truck = RunDerivedCrossingContest(*this, ETraversalClass::GroundVehicle, false);
	if (!Truck.bRan) { return false; }

	// THE MECHANISM, asserted so a regression names itself: before stage 4 the two routes
	// crossed in geometry and shared no node, so nothing yielded.
	TestTrue(TEXT("the truck's route and the aircraft's share a conflict node at the crossing"), Truck.bSharedNode);

	// THE SAFETY PROPERTY, in the spec's own terms: the truck waits clear of a passing wing -
	// its nose outside the taxiway's strip - for as long as the aircraft's body is within its
	// half-span of the point where the truck's lane crosses the centreline.
	TestEqual(TEXT("the truck's nose never enters the strip while the aircraft is at the crossing"),
		Truck.Intrusions, 0);

	// AND IT STILL GETS ACROSS: a yield that never released would pass every line above.
	TestTrue(TEXT("the truck reaches the far side"), Truck.bVehicleArrived);

	// IT WAITED, AT THE LINE: the aircraft is at the crossing as the truck reaches the line, so
	// a zero here would mean the property above held by timing, not by the arbiter.
	TestTrue(*FString::Printf(TEXT("the truck was held for the aircraft: %.2f s"), Truck.WaitedSeconds),
		Truck.WaitedSeconds > 0.0);

	// EVERY ORDER THEY CAN MEET IN, not just the one: an aircraft arriving just after the truck
	// has passed the line, while it is still inside the strip, is the case a reservation that a
	// higher rank may take would get wrong - the truck refused under the wing. Measured before
	// the commit rule in FClaimPass::BuildPending: 40 intrusion ticks at 1-3 s.
	for (const double Lag : { -3.0, -1.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 9.0 })
	{
		const FDerivedCrossingOutcome Swept = RunDerivedCrossingContest(*this, ETraversalClass::GroundVehicle, false, Lag);
		if (!Swept.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("aircraft %.0f s after the truck: no intrusion"), Lag), Swept.Intrusions, 0);
		TestTrue(*FString::Printf(TEXT("aircraft %.0f s after the truck: the truck gets across"), Lag), Swept.bVehicleArrived);
	}

	// REVIEW FOCUS 3: a rebuild while the truck waits at the line, and again mid-crossing. The
	// conflicts and the stop line are re-derived; the truck re-resolves and still behaves.
	const FDerivedCrossingOutcome Rebuilt = RunDerivedCrossingContest(*this, ETraversalClass::GroundVehicle, true);
	if (!Rebuilt.bRan) { return false; }
	TestEqual(TEXT("across two rebuilds: no intrusion"), Rebuilt.Intrusions, 0);
	TestTrue(TEXT("across two rebuilds: the truck still waited for the aircraft"), Rebuilt.WaitedSeconds > 0.0);
	TestTrue(TEXT("across two rebuilds: the truck still gets across"), Rebuilt.bVehicleArrived);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmergencyCrossesDerivedCrossingTest,
	"Airside.Model.Traffic.EmergencyCrossesDerivedCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEmergencyCrossesDerivedCrossingTest::RunTest(const FString& Parameters)
{
	// REVIEW FOCUS 4: an emergency vehicle outranks an aircraft (TraversalPriority), so where a
	// truck would wait it is the AIRCRAFT that gives way: the stop line's reservation is taken at
	// the emergency's rank and the aircraft's own node claim is refused by it. The aircraft two
	// seconds behind the line, not on the crossing: an aircraft already STANDING on the conflict
	// holds it occupied, and nobody - an emergency included - is driven into a body.
	const FDerivedCrossingOutcome Emergency = RunDerivedCrossingContest(*this, ETraversalClass::Emergency, false, 2.0);
	if (!Emergency.bRan) { return false; }
	TestTrue(TEXT("the emergency vehicle's route shares the conflict node"), Emergency.bSharedNode);
	TestTrue(*FString::Printf(TEXT("the emergency vehicle never waits for the aircraft: %.2f s"), Emergency.WaitedSeconds),
		Emergency.WaitedSeconds <= 0.0);
	TestTrue(*FString::Printf(TEXT("the aircraft gives way to it: %.2f s"), Emergency.AircraftWaitedSeconds),
		Emergency.AircraftWaitedSeconds > 0.0);
	TestTrue(TEXT("the emergency vehicle gets across"), Emergency.bVehicleArrived);
	return true;
}

#endif
