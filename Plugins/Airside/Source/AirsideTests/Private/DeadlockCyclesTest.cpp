#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/DeadlockResolver.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

// ONE DEFINITION OF A WAIT CYCLE (ops alerts spec 2026-09-29 §1): FDeadlockResolver::FindCycles is what
// Resolve acts on AND what the Deadlock alert reports, so the two cannot disagree about what a jam is.
// Staged exactly as Airside.Model.Traffic.DeadlockResolverStandalone stages it - Refuse + AccrueStall,
// no UGroundTraffic.

namespace
{
	/** Agents Id 1..N; each waits on Waits[i] (0 = on nobody) and has stalled for Stalled seconds. */
	TArray<FRoadAgent> DeadlockCyclesAgents(const TArray<int32>& Waits, double Stalled)
	{
		TArray<FRoadAgent> Agents;
		Agents.SetNum(Waits.Num());
		for (int32 Index = 0; Index < Waits.Num(); ++Index)
		{
			FGuidelineNodeId Node;
			Node.Index = 100 + Index;
			Agents[Index].Id = Index + 1;
			if (Waits[Index] != 0)
			{
				Agents[Index].Refuse(INDEX_NONE, FTrafficResource::OfNode(Node), TNumericLimits<double>::Max(), Waits[Index]);
				Agents[Index].AccrueStall(Stalled);
			}
		}
		return Agents;
	}

	TMap<int32, int32> DeadlockCyclesIndex(const TArray<FRoadAgent>& Agents)
	{
		TMap<int32, int32> Index;
		for (int32 At = 0; At < Agents.Num(); ++At)
		{
			Index.Add(Agents[At].Id, At);
		}
		return Index;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesAllAircraftTest, "Airside.Model.Traffic.Deadlock.AllAircraftCycleReported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesAllAircraftTest::RunTest(const FString&)
{
	// 1 waits on 2, 2 on 1; 3 queues behind 1. Both members default to Aircraft.
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1, 1 }, 10.0);
	FTrafficRules Rules;
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::AllAircraftCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Cycles);
	if (!TestEqual(TEXT("one all-aircraft cycle"), Cycles.Num(), 1)) { return false; }
	TArray<int32> Members = Cycles[0];
	Members.Sort();
	TestEqual(TEXT("its members are the two waiting on each other - not the one queued behind them"),
		Members, TArray<int32>{ 1, 2 });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesMixedTest, "Airside.Model.Traffic.Deadlock.MixedCycleIsNotAnAlert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesMixedTest::RunTest(const FString&)
{
	TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	Agents[1].Class = ETraversalClass::GroundVehicle;
	FTrafficRules Rules;
	FDeadlockResolver::FCycleScratch Scratch;
	TArray<TArray<int32>> Every;
	FDeadlockResolver::FindCycles(Agents, Rules, Scratch, Every);
	TestEqual(TEXT("the walk still finds the cycle - a van can go round, so the resolver acts on it"), Every.Num(), 1);
	TArray<TArray<int32>> Alerts;
	FDeadlockResolver::AllAircraftCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Alerts);
	TestEqual(TEXT("but only an ALL-aircraft cycle is a layout problem for the player"), Alerts.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesThresholdTest, "Airside.Model.Traffic.Deadlock.ShortStallIsNoCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesThresholdTest::RunTest(const FString&)
{
	FTrafficRules Rules;
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1 }, Rules.StallSeconds * 0.5);
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::AllAircraftCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Cycles);
	TestEqual(TEXT("two aircraft that have waited less than StallSeconds are traffic, not a jam"), Cycles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesQueueTest, "Airside.Model.Traffic.Deadlock.QueueIsNoCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesQueueTest::RunTest(const FString&)
{
	// 1 waits on 2, 2 on 3, and 3 waits on nobody (moving): a queue.
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 3, 0 }, 10.0);
	FTrafficRules Rules;
	FDeadlockResolver::FCycleScratch Scratch;
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::FindCycles(Agents, Rules, Scratch, Cycles);
	TestEqual(TEXT("a queue ending at a mover is no cycle"), Cycles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesTrafficTest, "Airside.Model.Traffic.Deadlock.QuietTrafficReportsNone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesTrafficTest::RunTest(const FString&)
{
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	TArray<TArray<int32>> Cycles;
	Cycles.AddDefaulted();   // a stale entry must be cleared, not appended to
	Traffic->CurrentDeadlocks(Cycles);
	TestEqual(TEXT("an empty traffic model reports no deadlock, and the out array is reset"), Cycles.Num(), 0);
	return true;
}

#endif
