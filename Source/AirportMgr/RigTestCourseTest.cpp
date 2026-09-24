#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigTestCourse.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS: the module is a unity build.
namespace RigCourseTest
{
	/**
	 * Every WARNING line in the categories the course and the tow guard log to. Unbuffered
	 * (CanBeUsedOnMultipleThreads), or the log thread delivers lines after the spy has gone
	 * (memory: log spies must be unbuffered).
	 */
	struct FWarningSpy : public FOutputDevice
	{
		TArray<FString> Lines;

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Warning
				&& (Category == FName(TEXT("LogRoadBuild")) || Category == FName(TEXT("LogAirside"))))
			{
				Lines.Add(FString(V));
			}
		}

		int32 Containing(const TCHAR* Text) const
		{
			int32 Count = 0;
			for (const FString& Line : Lines) { Count += Line.Contains(Text) ? 1 : 0; }
			return Count;
		}
	};

	/** Plans a leg exactly as a route search would with the vehicle's body - independently of the course. */
	bool Fits(const URoadNetwork& Net, const FRigCourseWaypoint& From, const FRigCourseWaypoint& To, const FVehicle& Vehicle)
	{
		const FGuidelineNodeId Start = ARigTestCourse::ResolveWaypoint(Net, From);
		const FGuidelineNodeId Goal = ARigTestCourse::ResolveWaypoint(Net, To);
		if (!Start.IsSet() || !Goal.IsSet()) { return false; }
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Vehicle);
		return RouteSearch::Find(Net, Query).IsValid();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseOneLoopHeadlessTest,
	"AirportMgr.RigCourse.OneLoopHeadless",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseOneLoopHeadlessTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }

	Course->BuildCourseForTest(*Actor);
	const URoadNetwork& Net = *Actor->Network;

	// THE LAYOUT: every feature at every tier, so one loop says which tier each vehicle fits.
	TestEqual(TEXT("3 tiers x 5 features (straight, right 90, left 90, T both ways, dead end)"),
		Course->FeatureCountForTest(), 15);
	const TArray<FRigCourseWaypoint>& Waypoints = Course->GetWaypoints();
	const int32 Legs = Course->LegCountForTest();
	TestEqual(TEXT("one leg per waypoint, the last wrapping to the first"), Legs, Waypoints.Num());
	for (int32 I = 0; I < Waypoints.Num(); ++I)
	{
		const FRoadNode* Node = Net.GetNode(Waypoints[I].Node);
		TestTrue(FString::Printf(TEXT("waypoint %d (%s) is a live node"), I, *Waypoints[I].Label),
			Node != nullptr && Node->bAlive);
		TestTrue(FString::Printf(TEXT("and names a lane end in the derived graph (%s)"), *Waypoints[I].Label),
			ARigTestCourse::ResolveWaypoint(Net, Waypoints[I]).IsSet());
	}

	// WHAT EACH VEHICLE SHOULD FIT, asked of the router independently of the course's driver.
	const TArray<FVehicle>& Vehicles = Course->GetVehicles();
	if (!TestEqual(TEXT("two vehicles: the rig, then the utility + trailer"), Vehicles.Num(), 2)) { return false; }
	TestTrue(TEXT("both vehicles tow something - the course is for chains"),
		Vehicles[0].HasTrailer() && Vehicles[1].HasTrailer());
	TArray<bool> Expected;
	for (int32 L = 0; L < Legs; ++L)
	{
		for (int32 V = 0; V < Vehicles.Num(); ++V)
		{
			Expected.Add(Fits(Net, Waypoints[L], Waypoints[(L + 1) % Legs], Vehicles[V]));
		}
	}

	// ONE LOOP of both vehicles at a fixed step, bounded so a hang fails rather than spins.
	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	constexpr float Step = 0.05f;
	constexpr int32 MaxTicks = 200000;   // 10 000 s of sim time: ~20x a loop's real length
	int32 Ticks = 0;
	for (; Ticks < MaxTicks && Course->LoopsCompletedForTest() < 1; ++Ticks)
	{
		Actor->Tick(Step);
		Course->Tick(Step);
	}
	GLog->RemoveOutputDevice(&Spy);
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: loop took %d ticks (%.0f s sim)"), Ticks, Ticks * Step);
	if (!TestEqual(TEXT("one loop completed within the tick bound"), Course->LoopsCompletedForTest(), 1)) { return false; }

	TestEqual(TEXT("no tow jack-knifed - driving forwards within the lock never reaches the guard"),
		Spy.Containing(TEXT("jack-knifed")), 0);
	TestEqual(TEXT("no leg entered Reversing - the course has no reverse legs"), Spy.Containing(TEXT("Reversing")), 0);
	TestEqual(TEXT("no leg timed out stuck"), Spy.Containing(TEXT("stuck")), 0);

	const TArray<FRigLegResult>& Results = Course->LastLoopResultsForTest();
	if (!TestEqual(TEXT("a result per leg per vehicle"), Results.Num(), Legs * Vehicles.Num())) { return false; }
	int32 Refusals = 0;
	for (int32 L = 0; L < Legs; ++L)
	{
		for (int32 V = 0; V < Vehicles.Num(); ++V)
		{
			const FRigLegResult& R = Results[L * Vehicles.Num() + V];
			const FString What = FString::Printf(TEXT("leg %d (%s), vehicle %d"), L, *Waypoints[(L + 1) % Legs].Label, V);
			TestTrue(What + TEXT(" was driven or refused"),
				R.Outcome == ERigLegOutcome::Driven || R.Outcome == ERigLegOutcome::Refused);
			TestEqual(What + TEXT(": refused exactly when the router refuses it with this body"),
				R.Outcome == ERigLegOutcome::Refused, !Expected[L * Vehicles.Num() + V]);
			TestFalse(What + TEXT(" never reversed"), R.bReversed);
			if (R.Outcome == ERigLegOutcome::Driven)
			{
				// The steered axle walks the line, so arrival is measured along it; the chassis
				// origin then sits one wheelbase back from the lane end, and no further.
				const double Wheelbase = Vehicles[V].Chassis.Wheelbase();
				TestTrue(What + TEXT(" drove its whole line to the waypoint's lane end"), R.DistanceLeft < 50.0);
				TestTrue(What + TEXT(" and stopped with its fixed axle a wheelbase behind that lane end"),
					FMath::Abs(FVector2D::Distance(R.EndPosition, R.GoalPosition) - Wheelbase) < 50.0);
			}
			Refusals += R.Outcome == ERigLegOutcome::Refused ? 1 : 0;
		}
	}
	TestEqual(TEXT("each refusal was logged once in the loop"), Spy.Containing(TEXT("refused:")), Refusals);
	return true;
}

#endif
