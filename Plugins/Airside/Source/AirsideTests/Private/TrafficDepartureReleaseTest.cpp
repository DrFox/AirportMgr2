#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TakeoffRun.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogM2DepTest, Log, All);

namespace
{
	// Prefixed against the unity build.
	FGuidelineNodeId M2DepNode(URoadNetwork& Net, double X, double Y)
	{
		return Net.AddGuidelineNode(FVector2D(X, Y), /*bDerived=*/false);
	}

	FAirframe M2DepPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}
}

/**
 * THE SECOND PIE REPORT OF 2026-09-06: "an aircraft taking off never releases the runway".
 * The log had every 7 pressed in the 56 s between "rolling for departure" and "Departure
 * complete" refused as "the runway is in use". The strip was held until Gone - the top of
 * the climb, 300 m up - when it is clear the moment the aircraft is airborne.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficDepartureReleasesWhenAirborneTest,
	"Airside.Model.Traffic.DepartureReleasesWhenAirborne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDepartureReleasesWhenAirborneTest::RunTest(const FString& Parameters)
{
	// A runway split at a road node, so the chain has two segments, and a taxiway guideline
	// ending ON the runway at the split - which is what arms a departure (see
	// UGroundTraffic::ArmDepartureIfRunway).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Near = Net->AddStraightSegment(RA, RM, Runway);
	const FRoadSegmentId Far = Net->AddStraightSegment(RM, RB, Runway);

	const FGuidelineNodeId A = M2DepNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId B = M2DepNode(*Net, 0.0, 0.0);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Net->AddGuidelineEdge(MoveTemp(Edge));
	}

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	const int32 Plane = Traffic->DispatchAgent(Net, RouteSearch::Find(*Net, Q), M2DepPiper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
	if (!TestTrue(TEXT("the route ends on the runway, so the departure is armed"), Traffic->FindAgent(Plane)->bDepartureArmed)) { return false; }

	const FAirframe Airframe = M2DepPiper();
	auto StripHeld = [&]()
	{
		return Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Near), 0)
			|| Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Far), 0);
	};

	double RollStart = -1.0, AirborneAt = -1.0, ReleasedAt = -1.0, GoneAt = -1.0;
	bool bHeldWhileOnWheels = true;
	bool bStillDepartingAtRelease = false;
	double Clock = 0.0;
	for (; Clock < 300.0; Clock += 0.05)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		if (P == nullptr) { GoneAt = Clock; break; }
		if (P->Phase != EAgentPhase::Departing) { continue; }
		if (RollStart < 0.0) { RollStart = Clock; }

		const bool bOnWheels = P->Departure.Phase != ETakeoffPhase::Climb && P->Departure.Phase != ETakeoffPhase::Clear;
		if (bOnWheels)
		{
			// Every tick on the wheels, the strip must be held: a landing cleared onto a
			// rolling aircraft is the one thing this table exists to prevent.
			bHeldWhileOnWheels = bHeldWhileOnWheels && StripHeld();
		}
		else
		{
			if (AirborneAt < 0.0) { AirborneAt = Clock; }
			if (ReleasedAt < 0.0 && !StripHeld())
			{
				ReleasedAt = Clock;
				bStillDepartingAtRelease = (P->Phase == EAgentPhase::Departing);
			}
		}
	}

	UE_LOG(LogM2DepTest, Log,
		TEXT("DepartureReleasesWhenAirborne measured: rolling at %.2f s, airborne at %.2f s, strip released at %.2f s, gone at %.2f s"),
		RollStart, AirborneAt, ReleasedAt, GoneAt);

	TestTrue(TEXT("it rolled"), RollStart >= 0.0);
	TestTrue(TEXT("the strip was held on every tick the wheels were on it"), bHeldWhileOnWheels);
	TestTrue(TEXT("it got airborne"), AirborneAt >= 0.0);
	TestTrue(FString::Printf(TEXT("the strip was released within one tick of lift-off (airborne %.2f, released %.2f)"), AirborneAt, ReleasedAt),
		ReleasedAt >= 0.0 && ReleasedAt - AirborneAt <= 0.05 + 1e-9);
	TestTrue(TEXT("released while still Departing - long before Gone"), bStillDepartingAtRelease);
	TestTrue(TEXT("it eventually went"), GoneAt >= 0.0);
	TestTrue(FString::Printf(TEXT("the release is well before Gone (%.1f s earlier)"), GoneAt - ReleasedAt), GoneAt - ReleasedAt > 5.0);

	// What the player pressed 7 for: an arrival asked after lift-off is not refused for the
	// runway. (The planner may refuse for a LATER reason - this bare graph has no exit that
	// reaches a stand - which is exactly the point: the refusal is not RunwayOccupied.)
	const FArrivalPlan After = ArrivalPlanner::Plan(*Net, FVector2D(-60000.0, 0.0), Airframe, &Traffic->GetOccupancy());
	TestNotEqual(TEXT("after the departure the runway is not 'in use'"), After.Why, EArrivalRefusal::RunwayOccupied);
	return true;
}

#endif
