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
	 * A route for Class from an alive node near From to one near To, trying every pair within
	 * Radius (a dead-end taxiway ends at its cut, 11.5 m short of the node): which lane end
	 * leaves which way is the drive side's business, not this test's. MinCrossings > 0 keeps
	 * only a route whose polyline crosses the line Y = CrossY that many times (the loop, whose
	 * shortest route between two corners need not cross at all). Named for this file because
	 * the tests module is a UNITY build.
	 */
	FRoutePlan DerivedCrossingRoute(const URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, double Radius = 2000.0, int32 MinCrossings = 0, double CrossY = 0.0)
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
				if (!Plan.IsValid()) { continue; }
				int32 Crossings = 0;
				for (int32 Index = 1; Index < Plan.Polyline.Num(); ++Index)
				{
					Crossings += ((Plan.Polyline[Index - 1].Y < CrossY) != (Plan.Polyline[Index].Y < CrossY)) ? 1 : 0;
				}
				if (Crossings >= MinCrossings) { return Plan; }
			}
		}
		return FRoutePlan();
	}

	/** Every point where two polylines cross. */
	TArray<FVector2D> DerivedCrossingPoints(const TArray<FVector2D>& P, const TArray<FVector2D>& Q)
	{
		TArray<FVector2D> Out;
		for (int32 I = 1; I < P.Num(); ++I)
		{
			for (int32 J = 1; J < Q.Num(); ++J)
			{
				const FVector2D R = P[I] - P[I - 1];
				const FVector2D S = Q[J] - Q[J - 1];
				const double Den = FVector2D::CrossProduct(R, S);
				if (FMath::IsNearlyZero(Den)) { continue; }
				const FVector2D QP = Q[J - 1] - P[I - 1];
				const double T = FVector2D::CrossProduct(QP, S) / Den;
				const double U = FVector2D::CrossProduct(QP, R) / Den;
				if (T < 0.0 || T > 1.0 || U < 0.0 || U > 1.0) { continue; }
				const FVector2D At = P[I - 1] + R * T;
				if (!Out.ContainsByPredicate([&At](const FVector2D& Seen) { return FVector2D::Distance(Seen, At) < 10.0; }))
				{
					Out.Add(At);
				}
			}
		}
		return Out;
	}

	/** The whole body of an agent as a footprint: a Length x Width box on its heading. */
	TArray<FVector2D> DerivedCrossingBody(const FRoadAgent& Agent, double Length, double Width = 200.0)
	{
		const FVector2D Dir(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading));
		const FVector2D Side(-Dir.Y, Dir.X);
		const FVector2D C = Agent.LastMotion.Position;
		return { C + Dir * Length * 0.5 + Side * Width * 0.5, C - Dir * Length * 0.5 + Side * Width * 0.5,
			C - Dir * Length * 0.5 - Side * Width * 0.5, C + Dir * Length * 0.5 - Side * Width * 0.5 };
	}

	struct FContestVehicle
	{
		ETraversalClass Class = ETraversalClass::GroundVehicle;
		FVehicle Vehicle;
		/** Seconds after the first vehicle is dispatched. */
		double After = 0.0;
	};

	struct FContestAircraft
	{
		FVector2D From, To;
		/** Seconds after the first vehicle's nose reaches this aircraft's crossing's stop line
		 *  (driving alone) that the aircraft reaches the crossing (driving alone). */
		double Lag = 0.0;
		/** Along the vehicles' route, from the stop line to the crossing point: the strip edge. */
		double LineToCrossing = 0.0;
		/** A taxi speed cap, uu/s, or 0 for the airframe's own. */
		double SpeedCap = 0.0;
	};

	enum class EContestRebuild : uint8
	{
		None = 0,
		/** The first tick the first vehicle waits for an aircraft. */
		WhileWaiting = 1,
		/** The first tick its body is inside a strip. */
		InStrip = 2,
		/** The first tick its centre is RebuildPast.X uu past RebuildAt along its route. */
		PastPoint = 4,
	};

	struct FContestSpec
	{
		URoadNetwork* Net = nullptr;
		FVector2D VehicleFrom, VehicleTo;
		int32 VehicleMinCrossings = 0;
		TArray<FContestVehicle> Vehicles;
		TArray<FContestAircraft> Aircraft;
		uint8 Rebuild = 0;
		FVector2D RebuildAt = FVector2D::ZeroVector;
		/** Rebuild again every this many seconds for as long as the first vehicle waits (0: never). */
		double RebuildPeriodWhileWaiting = 0.0;
		double Seconds = 150.0;
	};

	struct FContestOutcome
	{
		bool bRan = false;
		bool bSharedNode = false;
		bool bAllVehiclesArrived = false;
		bool bAllAircraftArrived = false;
		/** Ticks on which a vehicle's WHOLE BODY touched a taxiway strip while an aircraft was
		 *  within its own half-span of the crossing that vehicle was at. The safety property: 0. */
		int32 Intrusions = 0;
		/** Seconds the first vehicle spent refused by an aircraft, and the aircraft by a vehicle. */
		double VehicleWaited = 0.0;
		double AircraftWaited = 0.0;
		int32 Rebuilds = 0;
		double Clock = 0.0;
		/** How far into a strip the worst intruding body reached, uu - for the log line. */
		double DeepestIntrusion = 0.0;
		FString FirstIntrusion;
		/** WHERE THE VANS STOOD: seconds any vehicle spent stopped (under 10 uu/s) with its body
		 *  in a strip, and where the first such stop was. A crossing is driven, never stood on. */
		double StoodInStripSeconds = 0.0;
		FString FirstStood;
	};

	/**
	 * THE CONTEST, on a DERIVED graph: vehicles along one road route, aircraft along taxiway
	 * routes, dispatched so that - each driving alone - an aircraft would reach its crossing
	 * Lag seconds after the first vehicle's nose reaches that crossing's stop line. Arranged
	 * rather than hoped for, for TruckCrossesTaxiway's reason: agents left to chance cross
	 * seconds apart and test nothing. The safety metric is the vehicle's WHOLE body against
	 * the strips (TaxiwayStrip::WorstIntrusion, the placement tools' own query), not its nose:
	 * a vehicle stopped with its tail in the strip is under the wing as surely as one with its
	 * nose there (final review of stage 4, 2026-09-29).
	 */
	FContestOutcome RunContest(FAutomationTestBase& Test, const FContestSpec& Spec, const TCHAR* Name)
	{
		FContestOutcome Out;
		URoadNetwork* Net = Spec.Net;
		const FAirframe Plane = TestAirframes::PiperType()->Airframe();
		const FTrafficRules Rules;
		const double VehicleLength = Rules.VehicleFootprint;
		const double Dt = 1.0 / 30.0;

		const FContestVehicle& First = Spec.Vehicles[0];
		const FRoutePlan VehiclePlan = DerivedCrossingRoute(*Net, Spec.VehicleFrom, Spec.VehicleTo, First.Class,
			2000.0, Spec.VehicleMinCrossings);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: the vehicles route across"), Name), VehiclePlan.IsValid())) { return Out; }
		// EVERY VEHICLE TAKES THE SHARED ROUTE. A per-vehicle route of its own (a merging vehicle) existed as three
		// fields and a branch here, and no contest ever set them (#462).
		TArray<FRoutePlan> VehiclePlans;
		VehiclePlans.Init(VehiclePlan, Spec.Vehicles.Num());

		TArray<FRoutePlan> AircraftPlans;
		TArray<TArray<FVector2D>> CrossingsOf;
		TArray<FVector2D> CrossingOf;
		TArray<FAirframe> Frames;
		for (const FContestAircraft& A : Spec.Aircraft)
		{
			Frames.Add(Plane);
			if (A.SpeedCap > 0.0) { Frames.Last().Chassis.Ground.Taxi.SpeedCap = A.SpeedCap; }
			AircraftPlans.Add(DerivedCrossingRoute(*Net, A.From, A.To, ETraversalClass::Aircraft));
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: the aircraft routes"), Name), AircraftPlans.Last().IsValid())) { return Out; }
			CrossingsOf.Add(DerivedCrossingPoints(VehiclePlan.Polyline, AircraftPlans.Last().Polyline));
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: the routes cross"), Name), CrossingsOf.Last().Num() > 0)) { return Out; }
			CrossingOf.Add(CrossingsOf.Last()[0]);
			TSet<FGuidelineNodeId> Nodes;
			for (const FRouteStep& Step : AircraftPlans.Last().Steps) { Nodes.Add(Step.To); }
			for (const FRouteStep& Step : VehiclePlan.Steps) { Out.bSharedNode |= Nodes.Contains(Step.To); }
		}

		// SOLO TIMINGS: the first vehicle to each stop line, each aircraft to its crossing.
		TArray<double> LineEta, CrossEta;
		{
			UGroundTraffic* Solo = NewObject<UGroundTraffic>(GetTransientPackage());
			const int32 Id = Solo->DispatchAgent(Net, VehiclePlan, First.Vehicle, First.Class, 0.0);
			LineEta.Init(-1.0, Spec.Aircraft.Num());
			for (int32 Step = 0; Step < 30 * 300; ++Step)
			{
				Solo->Advance(Dt, Net);
				const FRoadAgent* Agent = Solo->FindAgent(Id);
				if (Agent == nullptr || Agent->Phase == EAgentPhase::Parked) { break; }
				const FVector2D Nose = Agent->LastMotion.Position
					+ FVector2D(FMath::Cos(Agent->LastMotion.Heading), FMath::Sin(Agent->LastMotion.Heading)) * VehicleLength * 0.5;
				for (int32 A = 0; A < Spec.Aircraft.Num(); ++A)
				{
					if (LineEta[A] < 0.0 && FVector2D::Distance(Nose, CrossingOf[A]) <= Spec.Aircraft[A].LineToCrossing)
					{
						LineEta[A] = (Step + 1) * Dt;
					}
				}
			}
		}
		for (int32 A = 0; A < Spec.Aircraft.Num(); ++A)
		{
			UGroundTraffic* Solo = NewObject<UGroundTraffic>(GetTransientPackage());
			const int32 Id = Solo->DispatchAgent(Net, AircraftPlans[A], Frames[A], ETraversalClass::Aircraft, 0.0);
			double Eta = -1.0;
			for (int32 Step = 0; Step < 30 * 300 && Eta < 0.0; ++Step)
			{
				Solo->Advance(Dt, Net);
				const FRoadAgent* Agent = Solo->FindAgent(Id);
				if (Agent == nullptr) { break; }
				if (FVector2D::Distance(Agent->LastMotion.Position, CrossingOf[A]) < 100.0) { Eta = (Step + 1) * Dt; }
			}
			CrossEta.Add(Eta);
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: each reaches its crossing alone"), Name), Eta > 0.0 && LineEta[A] > 0.0)) { return Out; }
		}

		// Dispatch times, shifted so none is negative.
		TArray<double> AircraftAt;
		double Shift = 0.0;
		for (int32 A = 0; A < Spec.Aircraft.Num(); ++A)
		{
			AircraftAt.Add(LineEta[A] + Spec.Aircraft[A].Lag - CrossEta[A]);
			Shift = FMath::Max(Shift, -AircraftAt.Last());
		}

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		TArray<int32> AircraftIds, VehicleIds;
		AircraftIds.Init(0, Spec.Aircraft.Num());
		VehicleIds.Init(0, Spec.Vehicles.Num());
		// ARRIVED AND RETIRED: the vans share one goal node, and a parked agent occupies its goal,
		// so a van left parked there would be a queue nobody behind it could ever finish.
		TArray<bool> AircraftDone, VehicleDone;
		AircraftDone.Init(false, Spec.Aircraft.Num());
		VehicleDone.Init(false, Spec.Vehicles.Num());
		uint8 Fired = 0;
		const FVector2D RebuildDir = [&]()
		{
			// The vehicles' direction of travel at RebuildAt, off their own polyline.
			double Best = TNumericLimits<double>::Max();
			FVector2D Dir(0.0, 1.0);
			for (int32 Index = 1; Index < VehiclePlan.Polyline.Num(); ++Index)
			{
				const double D = FVector2D::Distance(VehiclePlan.Polyline[Index], Spec.RebuildAt)
					+ FVector2D::Distance(VehiclePlan.Polyline[Index - 1], Spec.RebuildAt);
				if (D < Best) { Best = D; Dir = (VehiclePlan.Polyline[Index] - VehiclePlan.Polyline[Index - 1]).GetSafeNormal(); }
			}
			return Dir;
		}();

		double LastPeriodicRebuild = -1.0e9;
		int32 Step = 0;
		for (; Step < FMath::CeilToInt32(Spec.Seconds * 30.0); ++Step)
		{
			const double Clock = Step * Dt;
			for (int32 A = 0; A < Spec.Aircraft.Num(); ++A)
			{
				if (AircraftIds[A] == 0 && Clock >= AircraftAt[A] + Shift)
				{
					AircraftIds[A] = Traffic->DispatchAgent(Net, AircraftPlans[A], Frames[A], ETraversalClass::Aircraft, 0.0);
				}
			}
			for (int32 V = 0; V < Spec.Vehicles.Num(); ++V)
			{
				if (VehicleIds[V] == 0 && Clock >= Shift + Spec.Vehicles[V].After)
				{
					VehicleIds[V] = Traffic->DispatchAgent(Net, VehiclePlans[V], Spec.Vehicles[V].Vehicle, Spec.Vehicles[V].Class, 0.0);
				}
			}
			Traffic->Advance(Dt, Net);

			bool bAllVehicles = true;
			bool bAllAircraft = true;
			for (int32 A = 0; A < AircraftIds.Num(); ++A)
			{
				const FRoadAgent* Plan = Traffic->FindAgent(AircraftIds[A]);
				if (Plan != nullptr && Plan->Phase == EAgentPhase::Parked && !AircraftDone[A])
				{
					AircraftDone[A] = true;
					Traffic->RetireAgent(AircraftIds[A]);
					Plan = nullptr;
				}
				bAllAircraft &= AircraftDone[A];
				if (Plan != nullptr && Plan->GetWaitingOn() != 0 && VehicleIds.Contains(Plan->GetWaitingOn()))
				{
					Out.AircraftWaited += Dt;
				}
			}
			for (int32 V = 0; V < VehicleIds.Num(); ++V)
			{
				const FRoadAgent* Van = Traffic->FindAgent(VehicleIds[V]);
				if (Van != nullptr && Van->Phase == EAgentPhase::Parked && !VehicleDone[V])
				{
					VehicleDone[V] = true;
					Traffic->RetireAgent(VehicleIds[V]);
					Van = nullptr;
				}
				bAllVehicles &= VehicleDone[V];
				if (Van == nullptr) { continue; }
				const TOptional<TaxiwayStrip::FIntrusion> Intrusion = TaxiwayStrip::WorstIntrusion(*Net, DerivedCrossingBody(*Van, VehicleLength));
				// STOP PRECISION, NOT A BODY UNDER A WING: the follower stops within a few millimetres
				// of the line it is told to (measured: 1 uu past it, creeping in at 17 uu/s behind the
				// far crossing's queue), and the line IS the strip edge. 5 uu is five centimetres.
				const bool bInStrip = Intrusion.IsSet() && Intrusion->Depth > 5.0;
				bool bUnderWing = false;
				for (int32 A = 0; A < AircraftIds.Num(); ++A)
				{
					const FRoadAgent* Plan = Traffic->FindAgent(AircraftIds[A]);
					if (Plan == nullptr) { continue; }
					for (const FVector2D& P : CrossingsOf[A])
					{
						bUnderWing |= FVector2D::Distance(Plan->LastMotion.Position, P) < Plane.Wingspan * 0.5
							&& FVector2D::Distance(Van->LastMotion.Position, P) < Spec.Aircraft[A].LineToCrossing + VehicleLength;
					}
				}
				if (bInStrip && bUnderWing)
				{
					if (Out.Intrusions++ == 0)
					{
						Out.FirstIntrusion = FString::Printf(TEXT("vehicle %d at %.1f s, centre (%.0f, %.0f), speed %.0f"),
							V, Clock, Van->LastMotion.Position.X, Van->LastMotion.Position.Y, Van->LastMotion.GroundSpeed);
					}
					Out.DeepestIntrusion = FMath::Max(Out.DeepestIntrusion, Intrusion->Depth);
				}
				if (bInStrip && Van->LastMotion.GroundSpeed < 10.0)
				{
					if (Out.StoodInStripSeconds == 0.0)
					{
						Out.FirstStood = FString::Printf(TEXT("vehicle %d at %.1f s, centre (%.0f, %.0f), %.0f uu in"),
							V, Clock, Van->LastMotion.Position.X, Van->LastMotion.Position.Y, Intrusion->Depth);
					}
					Out.StoodInStripSeconds += Dt;
				}
				const bool bWaiting = Van->GetWaitingOn() != 0 && AircraftIds.Contains(Van->GetWaitingOn());
				if (V == 0)
				{
					Out.VehicleWaited += bWaiting ? Dt : 0.0;
					const bool bPast = FVector2D::DotProduct(Van->LastMotion.Position - Spec.RebuildAt, RebuildDir) > 100.0 && bInStrip;
					uint8 Now = 0;
					Now |= ((Spec.Rebuild & uint8(EContestRebuild::WhileWaiting)) && bWaiting) ? uint8(EContestRebuild::WhileWaiting) : 0;
					Now |= ((Spec.Rebuild & uint8(EContestRebuild::InStrip)) && bInStrip) ? uint8(EContestRebuild::InStrip) : 0;
					Now |= ((Spec.Rebuild & uint8(EContestRebuild::PastPoint)) && bPast) ? uint8(EContestRebuild::PastPoint) : 0;
					const bool bPeriodic = Spec.RebuildPeriodWhileWaiting > 0.0 && bWaiting
						&& Clock - LastPeriodicRebuild >= Spec.RebuildPeriodWhileWaiting;
					if (bPeriodic)
					{
						LastPeriodicRebuild = Clock;
						TestGraph::Rebuild(*Net);
						Traffic->OnGraphRebuilt(*Net);
						++Out.Rebuilds;
					}
					else if (Now & ~Fired)
					{
						// A ROAD EDIT: every derived node - conflicts, stop lines - is re-made, and
						// the agents re-resolve by position, exactly as a player's edit does it.
						Fired |= Now;
						TestGraph::Rebuild(*Net);
						Traffic->OnGraphRebuilt(*Net);
						++Out.Rebuilds;
					}
				}
			}
			Out.bAllVehiclesArrived = bAllVehicles;
			Out.bAllAircraftArrived = bAllAircraft;
			if (bAllVehicles && bAllAircraft) { break; }
		}
		Out.Clock = Step * Dt;
		if (Spec.Rebuild != 0)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: every rebuild asked for happened"), Name), Fired, Spec.Rebuild);
		}
		Test.AddInfo(FString::Printf(
			TEXT("%s: %d vehicle(s), %d aircraft, %d rebuild(s): %d intrusion tick(s) (deepest %.0f uu); first vehicle waited %.2f s, ")
			TEXT("aircraft waited %.2f s; stood in a strip %.2f s%s%s; %s after %.1f s%s%s"),
			Name, Spec.Vehicles.Num(), Spec.Aircraft.Num(), Out.Rebuilds, Out.Intrusions, Out.DeepestIntrusion, Out.VehicleWaited,
			Out.AircraftWaited, Out.StoodInStripSeconds, Out.FirstStood.IsEmpty() ? TEXT("") : TEXT(" - first "), *Out.FirstStood,
			(Out.bAllVehiclesArrived && Out.bAllAircraftArrived) ? TEXT("all arrived") : TEXT("NOT all arrived"),
			Out.Clock, Out.FirstIntrusion.IsEmpty() ? TEXT("") : TEXT("; first intrusion: "), *Out.FirstIntrusion));
		Out.bRan = true;
		return Out;
	}

	/** The standard derived crossing: taxiway W-E, service road N-S through one road node. */
	/**
	 * Vans, dispatched VanSpacing apart - never closer than 5 s, or two bodies start on one node
	 * and the table sees two agents standing in one place. SpeedCap slows the FIRST van only:
	 * a slow leader with the rest at full speed is how a convoy closes up to its minimum gap by
	 * the time it reaches the crossing.
	 */
	FContestSpec DerivedCrossingSpec(ETraversalClass Class, double Lag, int32 Vans = 1, double VanSpacing = 5.0,
		double SpeedCap = 0.0)
	{
		FContestSpec Spec;
		Spec.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Spec.Net);
		TestGraph::Rebuild(*Spec.Net);
		Spec.VehicleFrom = FVector2D(0.0, -20000.0);
		Spec.VehicleTo = FVector2D(0.0, 20000.0);
		for (int32 V = 0; V < Vans; ++V)
		{
			FContestVehicle Van;
			Van.Class = Class;
			Van.Vehicle = TestAirframes::Van();
			if (SpeedCap > 0.0 && V == 0) { Van.Vehicle.Chassis.Ground.Taxi.SpeedCap = SpeedCap; }
			Van.After = V * VanSpacing;
			Spec.Vehicles.Add(Van);
		}
		FContestAircraft Plane;
		Plane.From = FVector2D(-20000.0, 0.0);
		Plane.To = FVector2D(20000.0, 0.0);
		Plane.Lag = Lag;
		const URoadProfile* Taxiway = Spec.Net->ProfileFor(*Spec.Net->GetSegment(Crossing.West));
		Plane.LineToCrossing = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Spec.Net, Crossing.West);
		Spec.Aircraft.Add(Plane);
		return Spec;
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
	const FContestOutcome Truck = RunContest(*this, DerivedCrossingSpec(ETraversalClass::GroundVehicle, 0.0), TEXT("truck"));
	if (!Truck.bRan) { return false; }

	// THE MECHANISM, asserted so a regression names itself: before stage 4 the two routes
	// crossed in geometry and shared no node, so nothing yielded.
	TestTrue(TEXT("the truck's route and the aircraft's share a conflict node at the crossing"), Truck.bSharedNode);
	// THE SAFETY PROPERTY, in the spec's own terms: the truck waits clear of a passing wing -
	// its whole body outside the taxiway's strip - while the aircraft is at the crossing.
	TestEqual(TEXT("the truck's body never touches the strip while the aircraft is at the crossing"), Truck.Intrusions, 0);
	TestTrue(TEXT("everyone gets across"), Truck.bAllVehiclesArrived && Truck.bAllAircraftArrived);
	// IT WAITED, AT THE LINE: the aircraft is at the crossing as the truck reaches the line, so
	// a zero here would mean the property above held by timing, not by the arbiter.
	TestTrue(*FString::Printf(TEXT("the truck was held for the aircraft: %.2f s"), Truck.VehicleWaited), Truck.VehicleWaited > 0.0);

	// EVERY ORDER THEY CAN MEET IN: an aircraft arriving just after the truck has passed the
	// line, while it is still inside the strip, is the case a reservation a higher rank may
	// take would get wrong - the truck refused under the wing. Measured before the commit rule
	// in FClaimPass::BuildPending: 40 intrusion ticks at 1-3 s.
	for (const double Lag : { -3.0, -1.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 9.0 })
	{
		const FString Name = FString::Printf(TEXT("truck, aircraft %+.0f s"), Lag);
		const FContestOutcome Swept = RunContest(*this, DerivedCrossingSpec(ETraversalClass::GroundVehicle, Lag), *Name);
		if (!Swept.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Swept.Intrusions, 0);
		TestEqual(*FString::Printf(TEXT("%s: nobody stood in the strip"), *Name), Swept.StoodInStripSeconds, 0.0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Swept.bAllVehiclesArrived && Swept.bAllAircraftArrived);
	}

	// A CONVOY (final review, critical 1): a van following close behind another - a 6 m/s
	// leader, the follower at full speed, so it closes up to its minimum gap by the crossing.
	// The follower is refused by its leader on most passes, and a commit rule that read "refused
	// by anybody" never committed it - it passed the line on a reservation the aircraft then took.
	for (const double Lag : { -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0 })
	{
		const FString Name = FString::Printf(TEXT("two-van convoy, aircraft %+.0f s"), Lag);
		const FContestOutcome Convoy = RunContest(*this, DerivedCrossingSpec(ETraversalClass::GroundVehicle, Lag, 2, 5.0, 600.0), *Name);
		if (!Convoy.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Convoy.Intrusions, 0);
		TestEqual(*FString::Printf(TEXT("%s: nobody stood in the strip"), *Name), Convoy.StoodInStripSeconds, 0.0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Convoy.bAllVehiclesArrived && Convoy.bAllAircraftArrived);
	}

	// A CRAWLING VAN (final review, critical 2): at 1.5 m/s its braking distance is centimetres,
	// so a commit rule read off braking distance let its claim fall back to a reservation with
	// its nose already past the line, stopped or all but.
	for (const double Lag : { 0.5, 1.0, 1.5, 2.0, 3.0 })
	{
		const FString Name = FString::Printf(TEXT("crawling van, aircraft %+.1f s"), Lag);
		FContestSpec Spec = DerivedCrossingSpec(ETraversalClass::GroundVehicle, Lag, 1, 5.0, 150.0);
		Spec.Seconds = 300.0;
		const FContestOutcome Crawl = RunContest(*this, Spec, *Name);
		if (!Crawl.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Crawl.Intrusions, 0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Crawl.bAllVehiclesArrived && Crawl.bAllAircraftArrived);
	}

	// REVIEW FOCUS 3: rebuilds while the truck waits at the line, as its body enters the strip,
	// and with its centre PAST the conflict (the steps behind it then hold dead handles). The
	// aircraft is swept so one of them meets the truck in the strip.
	for (const double Lag : { 0.0, 3.0, 5.0, 7.0 })
	{
		const FString Name = FString::Printf(TEXT("truck, three rebuilds, aircraft %+.0f s"), Lag);
		FContestSpec Spec = DerivedCrossingSpec(ETraversalClass::GroundVehicle, Lag);
		Spec.Rebuild = uint8(EContestRebuild::InStrip) | uint8(EContestRebuild::PastPoint)
			| (Lag == 0.0 ? uint8(EContestRebuild::WhileWaiting) : 0);
		const FContestOutcome Rebuilt = RunContest(*this, Spec, *Name);
		if (!Rebuilt.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Rebuilt.Intrusions, 0);
		TestEqual(*FString::Printf(TEXT("%s: nobody stood in the strip"), *Name), Rebuilt.StoodInStripSeconds, 0.0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Rebuilt.bAllVehiclesArrived && Rebuilt.bAllAircraftArrived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmergencyCrossesDerivedCrossingTest,
	"Airside.Model.Traffic.EmergencyCrossesDerivedCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEmergencyCrossesDerivedCrossingTest::RunTest(const FString& Parameters)
{
	// REVIEW FOCUS 4: an emergency vehicle outranks an aircraft (TraversalPriority), so where a
	// truck would wait it is the AIRCRAFT that gives way. The aircraft two seconds behind the
	// line, not on the crossing: an aircraft already STANDING on the conflict holds it occupied,
	// and nobody - an emergency included - is driven into a body.
	const FContestOutcome Emergency = RunContest(*this, DerivedCrossingSpec(ETraversalClass::Emergency, 2.0), TEXT("emergency"));
	if (!Emergency.bRan) { return false; }
	TestTrue(TEXT("the emergency vehicle's route shares the conflict node"), Emergency.bSharedNode);
	TestTrue(*FString::Printf(TEXT("the emergency vehicle never waits for the aircraft: %.2f s"), Emergency.VehicleWaited),
		Emergency.VehicleWaited <= 0.0);
	TestTrue(*FString::Printf(TEXT("the aircraft gives way to it: %.2f s"), Emergency.AircraftWaited), Emergency.AircraftWaited > 0.0);
	TestTrue(TEXT("everyone gets across"), Emergency.bAllVehiclesArrived && Emergency.bAllAircraftArrived);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckCrossesTwoTaxiwaysAtOneNodeTest,
	"Airside.Model.Traffic.TruckCrossesTwoTaxiwaysAtOneNode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckCrossesTwoTaxiwaysAtOneNodeTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 3: a road through a node where a second taxiway also meets, the
	// aircraft turning onto the diagonal, and the graph rebuilt with the truck BETWEEN the first
	// conflict (the W-E line) and the next (the aircraft's turn). The committed hold must survive
	// the steps behind the truck holding dead handles.
	for (const double Lag : { 0.0, 2.0, 4.0, 6.0 })
	{
		FContestSpec Spec;
		Spec.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Spec.Net);
		Spec.Net->AddStraightSegment(Crossing.Centre, Spec.Net->AddNode(FVector2D(14142.0, 14142.0)), TestProfiles::Taxiway());
		TestGraph::Rebuild(*Spec.Net);
		Spec.VehicleFrom = FVector2D(0.0, -20000.0);
		Spec.VehicleTo = FVector2D(0.0, 20000.0);
		FContestVehicle Van;
		Van.Vehicle = TestAirframes::Van();
		Spec.Vehicles.Add(Van);
		FContestAircraft Plane;
		Plane.From = FVector2D(-20000.0, 0.0);
		Plane.To = FVector2D(14142.0, 14142.0);
		Plane.Lag = Lag;
		const URoadProfile* Taxiway = Spec.Net->ProfileFor(*Spec.Net->GetSegment(Crossing.West));
		Plane.LineToCrossing = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Spec.Net, Crossing.West);
		Spec.Aircraft.Add(Plane);
		// Past the W-E centreline, which the truck crosses first.
		Spec.Rebuild = uint8(EContestRebuild::PastPoint);
		Spec.RebuildAt = FVector2D(0.0, 0.0);
		const FString Name = FString::Printf(TEXT("two taxiways, rebuilt between conflicts, aircraft %+.0f s"), Lag);
		const FContestOutcome Out = RunContest(*this, Spec, *Name);
		if (!Out.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Out.Intrusions, 0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Out.bAllVehiclesArrived && Out.bAllAircraftArrived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanQueueBetweenTwoTaxiwaysTest,
	"Airside.Model.Traffic.VanQueueBetweenTwoTaxiways",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanQueueBetweenTwoTaxiwaysTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 4 AND CRITICAL 2: a road crossing two parallel code B taxiways
	// 70 m apart. An aircraft on the far one holds its crossing, so vans queue at its stop line,
	// and the queue backs up towards the NEAR crossing - where a van that passed the line with
	// no room beyond stops in the strip, STOPPED, as a second aircraft arrives on the near one.
	// A van may pass a line only if it can clear the far strip edge.
	//
	//     T2 ========+======== (y = 7000)
	//                |
	//     T1 ========+======== (y = 0)
	//                |  vans northbound
	for (const double Lag : { 4.0, 8.0, 12.0, 16.0, 20.0 })
	{
		FContestSpec Spec;
		Spec.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *Spec.Net;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(1200.0, 1500.0, 120.0);
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		Net.DefaultProfile = Taxiway;
		const FRoadNodeId X1 = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId X2 = Net.AddNode(FVector2D(0.0, 7000.0));
		const FRoadSegmentId T1 = Net.AddStraightSegment(Net.AddNode(FVector2D(-20000.0, 0.0)), X1, Taxiway);
		Net.AddStraightSegment(X1, Net.AddNode(FVector2D(20000.0, 0.0)), Taxiway);
		const FRoadSegmentId T2 = Net.AddStraightSegment(Net.AddNode(FVector2D(-20000.0, 7000.0)), X2, Taxiway);
		Net.AddStraightSegment(X2, Net.AddNode(FVector2D(20000.0, 7000.0)), Taxiway);
		Net.AddStraightSegment(Net.AddNode(FVector2D(0.0, -15000.0)), X1, Road);
		Net.AddStraightSegment(X1, X2, Road);
		Net.AddStraightSegment(X2, Net.AddNode(FVector2D(0.0, 22000.0)), Road);
		TestGraph::Rebuild(Net);
		Spec.VehicleFrom = FVector2D(0.0, -15000.0);
		Spec.VehicleTo = FVector2D(0.0, 22000.0);
		for (int32 V = 0; V < 6; ++V)
		{
			FContestVehicle Van;
			Van.Vehicle = TestAirframes::Van();
			Van.After = V * 5.0;
			Spec.Vehicles.Add(Van);
		}
		const double Edge1 = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(Net, T1);
		// The far aircraft is at its crossing as the first van reaches that line, taxiing at 2 m/s
		// (TruckCrossesTaxiway's figure) so it holds the crossing while the queue forms.
		FContestAircraft Far;
		Far.SpeedCap = 200.0;
		Far.From = FVector2D(-20000.0, 7000.0);
		Far.To = FVector2D(20000.0, 7000.0);
		Far.Lag = 0.0;
		Far.LineToCrossing = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(Net, T2);
		FContestAircraft Near;
		Near.From = FVector2D(-20000.0, 0.0);
		Near.To = FVector2D(20000.0, 0.0);
		Near.Lag = Lag;
		Near.LineToCrossing = Edge1;
		Spec.Aircraft = { Far, Near };
		Spec.Seconds = 240.0;
		const FString Name = FString::Printf(TEXT("queue between taxiways, near aircraft %+.0f s"), Lag);
		const FContestOutcome Out = RunContest(*this, Spec, *Name);
		if (!Out.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Out.Intrusions, 0);
		// RE-REVIEW, IMPORTANT 2: WHERE THE VANS STOOD. The queue for the far crossing backs up to
		// the near one; a van with no room beyond the near strip must wait at its line, never
		// roll in behind the queue and stand in the strip holding the conflict.
		TestEqual(*FString::Printf(TEXT("%s: no van ever stood in a strip"), *Name), Out.StoodInStripSeconds, 0.0);
		// AND THE AIRCRAFT'S WAIT: at most the vans already committed when it asked - each is
		// about 5 s over the near crossing - never a van parked in the strip behind the queue.
		TestTrue(*FString::Printf(TEXT("%s: the near aircraft waited %.2f s, under 10"), *Name, Out.AircraftWaited),
			Out.AircraftWaited < 10.0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across - no deadlock"), *Name), Out.bAllVehiclesArrived && Out.bAllAircraftArrived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanLoopCrossesTaxiwayTwiceTest,
	"Airside.Model.Traffic.VanLoopCrossesTaxiwayTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanLoopCrossesTaxiwayTwiceTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 4: a service-road loop crossing the same taxiway twice, vans
	// driving the lap and an aircraft along the taxiway through both crossings. A van holding
	// one crossing while it queues for the other, with the aircraft between, is the cycle the
	// room check exists to prevent: everybody must finish.
	//
	//   NW +-----------+ NE      (y = 10000)
	//      |           |
	//  ====+===========+====     taxiway, y = 0
	//      |           |
	//   SW +-----------+ SE      (y = -10000)
	for (const double Lag : { 0.0, 3.0, 8.0 })
	{
		FContestSpec Spec;
		Spec.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *Spec.Net;
		URoadProfile* Taxiway = TestProfiles::Taxiway();
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		Net.DefaultProfile = Taxiway;
		const FRoadNodeId XW = Net.AddNode(FVector2D(-10000.0, 0.0));
		const FRoadNodeId XE = Net.AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId West = Net.AddStraightSegment(Net.AddNode(FVector2D(-30000.0, 0.0)), XW, Taxiway);
		Net.AddStraightSegment(XW, XE, Taxiway);
		Net.AddStraightSegment(XE, Net.AddNode(FVector2D(30000.0, 0.0)), Taxiway);
		const FRoadNodeId SW = Net.AddNode(FVector2D(-10000.0, -10000.0));
		const FRoadNodeId NW = Net.AddNode(FVector2D(-10000.0, 10000.0));
		const FRoadNodeId NE = Net.AddNode(FVector2D(10000.0, 10000.0));
		const FRoadNodeId SE = Net.AddNode(FVector2D(10000.0, -10000.0));
		Net.AddStraightSegment(SW, XW, Road);
		Net.AddStraightSegment(XW, NW, Road);
		Net.AddStraightSegment(NW, NE, Road);
		Net.AddStraightSegment(NE, XE, Road);
		Net.AddStraightSegment(XE, SE, Road);
		Net.AddStraightSegment(SE, SW, Road);
		TestGraph::Rebuild(Net);
		Spec.VehicleFrom = FVector2D(-10000.0, -10000.0);
		Spec.VehicleTo = FVector2D(10000.0, -10000.0);
		Spec.VehicleMinCrossings = 2;
		for (int32 V = 0; V < 4; ++V)
		{
			FContestVehicle Van;
			Van.Vehicle = TestAirframes::Van();
			Van.After = V * 5.0;
			Spec.Vehicles.Add(Van);
		}
		FContestAircraft Plane;
		Plane.From = FVector2D(-30000.0, 0.0);
		Plane.To = FVector2D(30000.0, 0.0);
		Plane.Lag = Lag;
		Plane.LineToCrossing = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(Net, West);
		Spec.Aircraft.Add(Plane);
		Spec.Seconds = 240.0;
		const FString Name = FString::Printf(TEXT("loop, aircraft %+.0f s"), Lag);
		const FContestOutcome Out = RunContest(*this, Spec, *Name);
		if (!Out.bRan) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Out.Intrusions, 0);
		TestTrue(*FString::Printf(TEXT("%s: everyone finishes - no deadlock"), *Name), Out.bAllVehiclesArrived && Out.bAllAircraftArrived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanStoppedAcrossTheLineTest,
	"Airside.Model.Traffic.VanStoppedAcrossTheLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanStoppedAcrossTheLineTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, CRITICAL 2: a van STANDING with its nose past the stop line and its centre
	// short of it - where a queue or a rig's length leaves one. Its speed is zero, so its braking
	// distance is zero, and a commit rule read off braking distance handed its claim back to a
	// reservation the aircraft then took, with the van's nose in the strip. Built directly: the
	// approach lane is split 2 m short of the line and the van starts there, stopped, with the
	// aircraft already holding the conflict.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
	TestGraph::Rebuild(*Net);
	const FAirframe Plane = TestAirframes::PiperType()->Airframe();
	const FTrafficRules Rules;

	// The south arm's arriving lane end: the stop line, and the lane edge that runs into it.
	FGuidelineNodeId Line;
	FGuidelineEdgeId Approach;
	{
		const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num() && !Line.IsSet(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].HoldingPosition == EHoldingPositionKind::TaxiwayCrossing
				&& Nodes[Index].Origin.Segment == Crossing.South)
			{
				Line = Net->GuidelineNodeIdAt(Index);
			}
		}
		if (!TestTrue(TEXT("the south arm has a stop line"), Line.IsSet())) { return false; }
		for (const FGuidelineEdgeId Id : Net->GetGuidelineNode(Line)->Incident)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
			if (Edge != nullptr && Edge->B == Line && Edge->DerivedFrom == Crossing.South) { Approach = Id; }
		}
		if (!TestTrue(TEXT("and a lane running into it"), Approach.IsSet())) { return false; }
	}
	const FGuidelineEdge* Lane = Net->GetGuidelineEdge(Approach);
	const double LaneLength = FVector2D::Distance(Net->GetGuidelineNode(Lane->A)->Position, Net->GetGuidelineNode(Lane->B)->Position);
	FGuidelineNodeId Start;
	FGuidelineEdgeId Head, Tail;
	if (!TestTrue(TEXT("the lane splits 2 m short of the line"),
		Net->SplitGuidelineEdge(Approach, 1.0 - 200.0 / LaneLength, 0.0, Start, Head, Tail))) { return false; }
	TestTrue(TEXT("so the van's nose is past the line where it starts"), Rules.VehicleFootprint * 0.5 > 200.0);

	const FRoutePlan AircraftPlan = DerivedCrossingRoute(*Net, FVector2D(-20000.0, 0.0), FVector2D(20000.0, 0.0), ETraversalClass::Aircraft);
	const FRoutePlan Across = DerivedCrossingRoute(*Net, FVector2D(0.0, -20000.0), FVector2D(0.0, 20000.0), ETraversalClass::GroundVehicle);
	const FRoutePlan VanPlan = Across.IsValid()
		? TestGraph::Probe(*Net, Start, Across.Steps.Last().To, ETraversalClass::GroundVehicle) : FRoutePlan();
	if (!TestTrue(TEXT("both route"), AircraftPlan.IsValid() && VanPlan.IsValid())) { return false; }
	const TArray<FVector2D> Crossings = DerivedCrossingPoints(VanPlan.Polyline, AircraftPlan.Polyline);
	if (!TestTrue(TEXT("and cross"), Crossings.Num() > 0)) { return false; }
	const TArray<FGuidelineNodeId>& Conflicts = Net->GetGuidelineNode(Line)->ProtectsConflicts;
	if (!TestTrue(TEXT("the line protects a conflict"), Conflicts.Num() > 0)) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Aircraft = Traffic->DispatchAgent(Net, AircraftPlan, Plane, ETraversalClass::Aircraft, 0.0);
	const bool bHeld = RunUntil(*Traffic, *Net, 60.0, [&]()
	{
		int32 Holder = 0;
		return Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Conflicts[0]), 0, &Holder) && Holder == Aircraft;
	}, 1.0 / 30.0);
	if (!TestTrue(TEXT("the aircraft comes to hold the conflict"), bHeld)) { return false; }

	const int32 Van = Traffic->DispatchAgent(Net, VanPlan, TestAirframes::Van(), ETraversalClass::GroundVehicle, 0.0);
	int32 Intrusions = 0;
	bool bVanArrived = false;
	bool bAircraftArrived = false;
	for (int32 Step = 0; Step < 30 * 120 && !(bVanArrived && bAircraftArrived); ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* Plan = Traffic->FindAgent(Aircraft);
		const FRoadAgent* Truck = Traffic->FindAgent(Van);
		bAircraftArrived |= Plan == nullptr || Plan->Phase == EAgentPhase::Parked;
		bVanArrived |= Truck == nullptr || Truck->Phase == EAgentPhase::Parked;
		if (Plan == nullptr || Truck == nullptr || Truck->Phase == EAgentPhase::Parked) { continue; }
		const TOptional<TaxiwayStrip::FIntrusion> In = TaxiwayStrip::WorstIntrusion(*Net, DerivedCrossingBody(*Truck, Rules.VehicleFootprint));
		bool bNear = false;
		for (const FVector2D& P : Crossings)
		{
			bNear |= FVector2D::Distance(Plan->LastMotion.Position, P) < Plane.Wingspan * 0.5;
		}
		// A van standing across the line is IN the strip from the start; what must not happen is
		// the aircraft passing it there.
		Intrusions += (In.IsSet() && In->Depth > 5.0 && bNear) ? 1 : 0;
	}
	TestEqual(TEXT("the aircraft never passes the van while any of it is in the strip"), Intrusions, 0);
	TestTrue(TEXT("and both finish"), bVanArrived && bAircraftArrived);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanHeldAtLineThroughRebuildsTest,
	"Airside.Model.Traffic.VanHeldAtLineThroughRebuilds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanHeldAtLineThroughRebuildsTest::RunTest(const FString& Parameters)
{
	// RE-REVIEW, IMPORTANT 1: a van HELD at the line has its nose parked exactly on it, and the
	// commit rule's "nose past the line" was decided by rounding - a rebuild re-projecting the van
	// a hair forward turned a waiting van into a committed one, whose occupancy then beat the
	// aircraft's reservation. A slow aircraft (2 m/s) keeps the van waiting, and the graph is
	// rebuilt every half second while it does. Measured: where the van stood, and whether the
	// AIRCRAFT ever waited for it - it must not, the van never had the crossing.
	for (const double Lag : { 0.0, 2.0 })
	{
		FContestSpec Spec = DerivedCrossingSpec(ETraversalClass::GroundVehicle, Lag);
		Spec.Aircraft[0].SpeedCap = 200.0;
		Spec.Rebuild = uint8(EContestRebuild::WhileWaiting);
		Spec.RebuildPeriodWhileWaiting = 0.5;
		Spec.Seconds = 240.0;
		const FString Name = FString::Printf(TEXT("van held through rebuilds, aircraft %+.0f s"), Lag);
		const FContestOutcome Out = RunContest(*this, Spec, *Name);
		if (!Out.bRan) { return false; }
		TestTrue(*FString::Printf(TEXT("%s: rebuilt while it waited (%d)"), *Name, Out.Rebuilds), Out.Rebuilds > 3);
		TestTrue(*FString::Printf(TEXT("%s: the van waited for the aircraft"), *Name), Out.VehicleWaited > 0.0);
		TestEqual(*FString::Printf(TEXT("%s: the aircraft never waited for the van (%.2f s)"), *Name, Out.AircraftWaited),
			Out.AircraftWaited, 0.0);
		TestEqual(*FString::Printf(TEXT("%s: the van never stood in the strip"), *Name), Out.StoodInStripSeconds, 0.0);
		TestEqual(*FString::Printf(TEXT("%s: no intrusion"), *Name), Out.Intrusions, 0);
		TestTrue(*FString::Printf(TEXT("%s: everyone gets across"), *Name), Out.bAllVehiclesArrived && Out.bAllAircraftArrived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanHoldsRoomUntilClearTest,
	"Airside.Model.Traffic.VanHoldsRoomUntilClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanHoldsRoomUntilClearTest::RunTest(const FString& Parameters)
{
	// RE-REVIEW, IMPORTANT 3: the room past the far strip edge - the far lane, a footprint and a
	// gap of it - is what lets a committed van promise to leave the strip. It was claimed on the
	// approach and given back halfway across, once no step ahead ended at a conflict node, for
	// anything merging onto the far arm to take. Measured on the table itself, every tick the
	// van's TAIL is still in the strip beyond the last conflict: the far lane's first edge is
	// claimed by the van, over at least the room it needs.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
	TestGraph::Rebuild(*Net);
	const FTrafficRules Rules;
	const FRoutePlan Plan = DerivedCrossingRoute(*Net, FVector2D(0.0, -20000.0), FVector2D(0.0, 20000.0), ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the van routes across"), Plan.IsValid())) { return false; }

	// The far arm's end: the first node with an Origin after the last conflict node.
	int32 LastConflict = INDEX_NONE;
	for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
	{
		const FGuidelineNode* To = Net->GetGuidelineNode(Plan.Steps[Index].To);
		LastConflict = (To != nullptr && To->bCrossingConflict) ? Index : LastConflict;
	}
	if (!TestTrue(TEXT("the route crosses a conflict"), LastConflict != INDEX_NONE && Plan.Steps.IsValidIndex(LastConflict + 2))) { return false; }
	// Northbound: positions along Y are distances along the lane.
	const double ConflictY = Net->GetGuidelineNode(Plan.Steps[LastConflict].To)->Position.Y;
	const double FarEndY = Net->GetGuidelineNode(Plan.Steps[LastConflict + 1].To)->Position.Y;
	const FGuidelineEdgeId FarLane = Plan.Steps[LastConflict + 2].Edge;
	const double Room = Rules.VehicleFootprint + Rules.GapFor(ETraversalClass::GroundVehicle);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, Plan, TestAirframes::Van(), ETraversalClass::GroundVehicle, 0.0);
	int32 Measured = 0, Held = 0;
	double Shortest = TNumericLimits<double>::Max();
	for (int32 Step = 0; Step < 30 * 120; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Van);
		if (Agent == nullptr || Agent->Phase == EAgentPhase::Parked) { break; }
		const double Centre = Agent->LastMotion.Position.Y;
		if (Centre <= ConflictY || Centre - Rules.VehicleFootprint * 0.5 >= FarEndY) { continue; }
		++Measured;
		const FTrafficClaim* Claim = Traffic->GetOccupancy().FindClaim(Van, FTrafficResource::OfEdge(FarLane));
		if (Claim != nullptr)
		{
			++Held;
			Shortest = FMath::Min(Shortest, FMath::Abs(Claim->To - Claim->From));
		}
	}
	TestTrue(*FString::Printf(TEXT("the van was measured between the last conflict and clearing the strip (%d ticks)"), Measured), Measured > 10);
	TestEqual(TEXT("every one of those ticks, the van holds the far lane"), Held, Measured);
	TestTrue(*FString::Printf(TEXT("over at least the room it needs to clear: %.0f of %.0f uu"), Shortest, Room),
		Held == 0 || Shortest >= Room - 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVanCrossesPastAircraftQueueTest,
	"Airside.Model.Traffic.VanCrossesPastAircraftQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVanCrossesPastAircraftQueueTest::RunTest(const FString& Parameters)
{
	// RE-REVIEW, IMPORTANT 4: one box is all or nothing. A2, queued a gap short of the crossing
	// behind A1 (parked on the far arm's end), was granted both road lanes' conflicts - they
	// come before its refusal in route order - and held them for as long as the queue lasted,
	// with the crossing empty. A van then waits at the line for ever: if the queue waits for a
	// stand that van is needed at, nobody moves. Measured: how long the van waited, where A2
	// stood, and that the van crossed.
	//
	// WHERE A2 STANDS IS ALSO FINAL REVIEW, IMPORTANT 7: splitting the taxiway's through-turn at the conflicts
	// made its first short piece the "box" whose entry the claim pass guards, so an aircraft queued behind one
	// standing on the far arm drove into the junction and stopped ACROSS THE ROAD; on main the whole turn was
	// the box and it waited a gap short of it, behind the junction. A2's nose is asserted clear of the road
	// BEFORE the van is dispatched, so that check is a behaviour change for an aircraft that never meets a
	// vehicle. A test of just that (Airside.Model.Traffic.AircraftQueueWaitsClearOfRoad, removed in #462) had
	// this fixture, these routes and this assertion with a 90 s settle where this one settles for 60 s.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
	TestGraph::Rebuild(*Net);
	const FAirframe Plane = TestAirframes::PiperType()->Airframe();
	const FTrafficRules Rules;

	const FGuidelineNodeId EastEnd = TestGraph::NodeFor(*Net, Crossing.East, /*bEndA=*/true);
	const FRoutePlan Through = DerivedCrossingRoute(*Net, FVector2D(-20000.0, 0.0), FVector2D(20000.0, 0.0), ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the taxiway routes"), Through.IsValid())) { return false; }
	const FRoutePlan ToEnd = TestGraph::Probe(*Net, Through.Start, EastEnd, ETraversalClass::Aircraft);
	const FRoutePlan VanPlan = DerivedCrossingRoute(*Net, FVector2D(0.0, -20000.0), FVector2D(0.0, 20000.0), ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("A1 routes to the east arm's end, the van across"), ToEnd.IsValid() && VanPlan.IsValid())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 A1 = Traffic->DispatchAgent(Net, ToEnd, Plane, ETraversalClass::Aircraft, 0.0);
	if (!TestTrue(TEXT("A1 parks on the far arm's end"), RunUntil(*Traffic, *Net, 120.0, [&]()
	{
		const FRoadAgent* A = Traffic->FindAgent(A1);
		return A != nullptr && A->Phase == EAgentPhase::Parked;
	}, 1.0 / 30.0))) { return false; }

	const int32 A2 = Traffic->DispatchAgent(Net, Through, Plane, ETraversalClass::Aircraft, 0.0);
	// A2 settles into its queue position behind the crossing.
	TickUntil(*Traffic, *Net, 60.0, [](int32) { return true; }, 1.0 / 30.0);
	const FRoadAgent* Queued = Traffic->FindAgent(A2);
	if (!TestNotNull(TEXT("A2 exists"), Queued)) { return false; }
	const double A2At = Queued->LastMotion.Position.X;
	const URoadProfile* Road = Net->ProfileFor(*Net->GetSegment(Crossing.South));
	TestTrue(*FString::Printf(TEXT("A2 queues clear of the road: nose at x = %.0f"), A2At + Rules.AircraftFootprint * 0.5),
		A2At + Rules.AircraftFootprint * 0.5 < -Road->GetTotalWidth() * 0.5);

	const int32 VanId = Traffic->DispatchAgent(Net, VanPlan, TestAirframes::Van(), ETraversalClass::GroundVehicle, 0.0);
	double VanWaited = 0.0;
	bool bVanArrived = false;
	for (int32 Step = 0; Step < 30 * 90 && !bVanArrived; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* Van = Traffic->FindAgent(VanId);
		bVanArrived = Van == nullptr || Van->Phase == EAgentPhase::Parked;
		VanWaited += (Van != nullptr && (Van->GetWaitingOn() == A1 || Van->GetWaitingOn() == A2)) ? 1.0 / 30.0 : 0.0;
	}
	AddInfo(FString::Printf(TEXT("the van waited %.2f s for the queue"), VanWaited));
	TestTrue(TEXT("the van crosses past the queue"), bVanArrived);
	TestEqual(TEXT("without waiting for aircraft that are not moving"), VanWaited, 0.0);
	return true;
}

#endif
