#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineEdgeId M2OccEdge(int32 Index) { FGuidelineEdgeId Id; Id.Index = Index; Id.Generation = 1; return Id; }
	FGuidelineNodeId M2OccNode(int32 Index) { FGuidelineNodeId Id; Id.Index = Index; Id.Generation = 1; return Id; }

	FTrafficClaim M2OccEdgeClaim(int32 Agent, int32 Edge, double From, double To, bool bOccupied, int32 Rank)
	{
		FTrafficClaim C;
		C.AgentId = Agent; C.Resource = FTrafficResource::OfEdge(M2OccEdge(Edge));
		C.From = From; C.To = To; C.bOccupied = bOccupied; C.Rank = Rank;
		return C;
	}
	FRoadSegmentId M2OccSurface(int32 Index) { FRoadSegmentId Id; Id.Index = Index; Id.Generation = 1; return Id; }

	FTrafficClaim M2OccSurfaceClaim(int32 Agent, int32 Segment, bool bOccupied, int32 Rank)
	{
		FTrafficClaim C;
		C.AgentId = Agent; C.Resource = FTrafficResource::OfSurface(M2OccSurface(Segment));
		C.bOccupied = bOccupied; C.Rank = Rank;
		return C;
	}
	FTrafficClaim M2OccNodeClaim(int32 Agent, int32 Node, bool bOccupied, int32 Rank)
	{
		FTrafficClaim C;
		C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(M2OccNode(Node));
		C.bOccupied = bOccupied; C.Rank = Rank;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyClaimsTest,
	"Airside.Model.Occupancy.Claims",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOccupancyClaimsTest::RunTest(const FString& Parameters)
{
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;

	// 1. Disjoint intervals on one edge coexist - that is a queue.
	TestEqual(TEXT("agent 1 reserves [0,1000)"), Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1000.0, true, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("agent 2 reserves [1000,2000) behind it - touching is not overlapping"), Table.TryClaim(M2OccEdgeClaim(2, 7, 1000.0, 2000.0, false, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("two claims held"), Table.GetClaims().Num(), 2);

	// 2. Overlap is refused and names the holder.
	TestEqual(TEXT("agent 3 wants [500,1500) - held"), Table.TryClaim(M2OccEdgeClaim(3, 7, 500.0, 1500.0, false, 2), Blocker), EClaimResult::Held);
	TestEqual(TEXT("the blocker named is agent 1, the occupied one found first"), Blocker.AgentId, 1);
	TestEqual(TEXT("a refused claim adds nothing"), Table.GetClaims().Num(), 2);

	// 3. Rank preempts a RESERVATION, never an OCCUPANCY.
	TestEqual(TEXT("rank 3 preempts agent 2's reservation"), Table.TryClaim(M2OccEdgeClaim(4, 7, 1200.0, 1800.0, false, 3), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("rank 3 cannot preempt agent 1, who is standing there"), Table.TryClaim(M2OccEdgeClaim(4, 7, 200.0, 400.0, false, 3), Blocker), EClaimResult::Held);
	{
		const TSet<int32> Preempted = Table.TakePreempted();
		TestTrue(TEXT("agent 2 is reported preempted, once"), Preempted.Num() == 1 && Preempted.Contains(2));
		TestEqual(TEXT("and the report clears on read"), Table.TakePreempted().Num(), 0);
	}

	// 4. Equal rank: the holder keeps it (first-to-reserve).
	TestEqual(TEXT("agent 5 at rank 3 cannot take agent 4's rank-3 reservation"), Table.TryClaim(M2OccEdgeClaim(5, 7, 1200.0, 1300.0, false, 3), Blocker), EClaimResult::Held);
	TestEqual(TEXT("blocker is agent 4"), Blocker.AgentId, 4);

	// 5. Re-claiming your own resource updates it rather than conflicting with yourself.
	TestEqual(TEXT("agent 1 extends its own interval"), Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1100.0, true, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("still one claim for agent 1 on that edge"), Table.GetClaims().FilterByPredicate([](const FTrafficClaim& C) { return C.AgentId == 1; }).Num(), 1);

	// 6. Nodes are exclusive whatever the numbers say.
	TestEqual(TEXT("agent 1 takes node 9"), Table.TryClaim(M2OccNodeClaim(1, 9, false, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("agent 2 cannot"), Table.TryClaim(M2OccNodeClaim(2, 9, false, 2), Blocker), EClaimResult::Held);
	int32 Holder = 0;
	TestTrue(TEXT("IsHeld sees node 9 held by someone other than agent 2"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 2, &Holder) && Holder == 1);
	TestFalse(TEXT("but not held by anyone other than agent 1"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 1));

	// 7. HeldLengthOn sums OTHER agents' intervals - the routing cost's input.
	TestEqual(TEXT("edge 7 as seen by a stranger: 1100 + 600"), Table.HeldLengthOn(M2OccEdge(7), 0), 1700.0, 1e-9);
	TestEqual(TEXT("edge 7 as seen by agent 1 excludes its own 1100"), Table.HeldLengthOn(M2OccEdge(7), 1), 600.0, 1e-9);

	// 8. Release.
	Table.ReleaseExcept(1, { FTrafficResource::OfNode(M2OccNode(9)) });
	TestEqual(TEXT("agent 1 keeps only the node"), Table.GetClaims().FilterByPredicate([](const FTrafficClaim& C) { return C.AgentId == 1; }).Num(), 1);
	Table.ReleaseAll(1);
	TestFalse(TEXT("after ReleaseAll node 9 is free"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 0));
	Table.Clear();
	TestEqual(TEXT("Clear empties the table"), Table.GetClaims().Num(), 0);

	// 9. PHYSICAL PRESENCE BEATS A RESERVATION, whatever the ranks say. Spec §3.3 promises
	//    nobody is evicted from a node they are standing on - which means a RESERVATION on a
	//    node somebody is already standing on was never a valid claim in the first place, and
	//    leaving it in the table let the reserver drive into the occupant while the occupant,
	//    refused its own ground, abandoned the rest of its claim pass.
	TestEqual(TEXT("a rank-9 stranger reserves node 3"), Table.TryClaim(M2OccNodeClaim(6, 3, false, 9), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("a rank-0 agent STANDING on node 3 takes it anyway"), Table.TryClaim(M2OccNodeClaim(7, 3, true, 0), Blocker), EClaimResult::Granted);
	{
		const TSet<int32> Preempted = Table.TakePreempted();
		TestTrue(TEXT("and the reserver is reported, so it re-claims this same tick"), Preempted.Num() == 1 && Preempted.Contains(6));
	}
	TestTrue(TEXT("the occupant holds node 3"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(3)), 6, &Holder) && Holder == 7);

	// 10. Two OCCUPANTS of one node is the case that stays refused: presence beats a
	//     reservation, but it cannot evict another body.
	TestEqual(TEXT("a second agent standing on node 3 is Held"), Table.TryClaim(M2OccNodeClaim(8, 3, true, 9), Blocker), EClaimResult::Held);
	TestEqual(TEXT("named by the occupant"), Blocker.AgentId, 7);
	TestEqual(TEXT("nothing was preempted by a refused claim"), Table.TakePreempted().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyReleaseTest,
	"Airside.Model.Occupancy.Release",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOccupancyReleaseTest::RunTest(const FString& Parameters)
{
	// ONE RESOURCE, not the agent. ReleaseAll and ReleaseExcept were the only two ways to
	// drop a claim, and both are wrong for a handover that gives back exactly one thing -
	// the runway an arrival has vacated - while the agent goes on holding the ground it is
	// standing on.
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;
	Table.TryClaim(M2OccNodeClaim(1, 3, true, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(1, 4, true, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(2, 5, true, 2), Blocker);

	Table.Release(1, FTrafficResource::OfNode(M2OccNode(3)));

	TestFalse(TEXT("the named resource is released"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(3)), 0));
	TestTrue(TEXT("the same agent's OTHER claim survives"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(4)), 0));
	TestTrue(TEXT("and so does another agent's"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(5)), 0));
	TestEqual(TEXT("exactly one claim went"), Table.GetClaims().Num(), 2);

	// Releasing something nobody holds is not an error - a handover may run twice.
	Table.Release(1, FTrafficResource::OfNode(M2OccNode(3)));
	TestEqual(TEXT("releasing an unheld resource changes nothing"), Table.GetClaims().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyReleaseReservationsTest,
	"Airside.Model.Occupancy.ReleaseReservations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOccupancyReleaseReservationsTest::RunTest(const FString& Parameters)
{
	// WHAT A REPLAN GIVES BACK: the line ahead, which belongs to a journey nobody is making
	// any more - and NOT the ground the agent is standing on, which is a fact about where its
	// body is and no plan can change it. UGroundTraffic::ReplanAt called ReleaseAll, so a
	// replanned aircraft standing on a runway showed the strip free to ArrivalPlanner for the
	// frame before its next claim pass, and a landing could be cleared onto it.
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;
	Table.TryClaim(M2OccNodeClaim(1, 3, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1000.0, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(1, 4, /*bOccupied=*/false, 2), Blocker);
	Table.TryClaim(M2OccEdgeClaim(1, 8, 0.0, 1000.0, /*bOccupied=*/false, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(2, 5, /*bOccupied=*/false, 2), Blocker);

	Table.ReleaseReservations(1);

	TestTrue(TEXT("the node the agent stands on survives"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(3)), 0));
	TestTrue(TEXT("and the line under its body"), Table.IsHeld(FTrafficResource::OfEdge(M2OccEdge(7)), 0));
	TestFalse(TEXT("the node it had merely reserved is given back"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(4)), 0));
	TestFalse(TEXT("and so is the edge ahead"), Table.IsHeld(FTrafficResource::OfEdge(M2OccEdge(8)), 0));
	TestTrue(TEXT("another agent's reservation is untouched"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(5)), 0));
	TestEqual(TEXT("exactly the two reservations went"), Table.GetClaims().Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyReleaseGuidelineClaimsTest,
	"Airside.Model.Occupancy.ReleaseGuidelineClaims",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOccupancyReleaseGuidelineClaimsTest::RunTest(const FString& Parameters)
{
	// WHAT A GRAPH REBUILD GIVES BACK: every guideline handle, because the builder has just
	// freed the nodes and edges those claims name, and NOT the runway segments, because a
	// guideline rebuild does not touch the road model - the strip under an aeroplane is as
	// real afterwards as before. Clear() was used here first and was wrong: it handed back an
	// aircraft's runway hold, and ArrivalPlanner::Plan reads this table directly at
	// DispatchArrival, BETWEEN ticks, so a landing could be cleared onto a crossing.
	//
	// EVERYBODY'S, not one agent's: a rebuild is resources ceasing to exist, not an agent
	// giving something up, so two agents are held here to say so.
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;
	Table.TryClaim(M2OccNodeClaim(1, 3, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1000.0, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccEdgeClaim(1, 8, 0.0, 1000.0, /*bOccupied=*/false, 2), Blocker);
	Table.TryClaim(M2OccSurfaceClaim(1, 2, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(5, 9, /*bOccupied=*/false, 2), Blocker);
	Table.TryClaim(M2OccSurfaceClaim(5, 4, /*bOccupied=*/false, 2), Blocker);

	Table.ReleaseGuidelineClaims();

	TestFalse(TEXT("the node an agent was STANDING on goes: the node itself no longer exists"),
		Table.IsHeld(FTrafficResource::OfNode(M2OccNode(3)), 0));
	TestFalse(TEXT("and the edge under its body"), Table.IsHeld(FTrafficResource::OfEdge(M2OccEdge(7)), 0));
	TestFalse(TEXT("and the edge it had reserved"), Table.IsHeld(FTrafficResource::OfEdge(M2OccEdge(8)), 0));
	TestFalse(TEXT("and another agent's node with them - this is not one agent releasing"),
		Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 0));
	TestTrue(TEXT("the runway an aircraft is OCCUPYING survives: a rebuild does not move it off the strip"),
		Table.IsHeld(FTrafficResource::OfSurface(M2OccSurface(2)), 0));
	TestTrue(TEXT("and so does a runway merely RESERVED, whoever holds it"),
		Table.IsHeld(FTrafficResource::OfSurface(M2OccSurface(4)), 0));
	TestEqual(TEXT("exactly the four guideline claims went"), Table.GetClaims().Num(), 2);

	// AND Clear() STILL MEANS CLEAR. The two are different calls for different events, and a
	// reader who found only one of them would reasonably assume it was the only one.
	Table.Clear();
	TestEqual(TEXT("Clear takes the surfaces too"), Table.GetClaims().Num(), 0);

	// AND THE PER-AGENT FORM, which is what a STRANDING gives back: one agent's guidelines,
	// nobody else's, and not its runway. ReleaseAll was called at both stranding sites and
	// took the strip claim of an aircraft standing on the asphalt with it - a landing could
	// then be cleared onto it, the same window ReleaseGuidelineClaims exists to keep shut.
	Table.TryClaim(M2OccNodeClaim(1, 3, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1000.0, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccSurfaceClaim(1, 2, /*bOccupied=*/true, 2), Blocker);
	Table.TryClaim(M2OccNodeClaim(5, 9, /*bOccupied=*/false, 2), Blocker);
	Table.TryClaim(M2OccSurfaceClaim(5, 4, /*bOccupied=*/false, 2), Blocker);

	Table.ReleaseGuidelineClaimsOf(1);

	TestFalse(TEXT("the stranded agent's node goes"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(3)), 0));
	TestFalse(TEXT("and the edge under it: it will never drive that line"),
		Table.IsHeld(FTrafficResource::OfEdge(M2OccEdge(7)), 0));
	TestTrue(TEXT("but the runway it is STANDING on stays held: a dead plan does not move a body"),
		Table.IsHeld(FTrafficResource::OfSurface(M2OccSurface(2)), 0));
	TestTrue(TEXT("and another agent's node is untouched - this is ONE agent, not a rebuild"),
		Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 0));
	TestTrue(TEXT("nor is another agent's runway"), Table.IsHeld(FTrafficResource::OfSurface(M2OccSurface(4)), 0));
	TestEqual(TEXT("exactly the two guideline claims of agent 1 went"), Table.GetClaims().Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyIndexedCostTest,
	"Airside.Model.Occupancy.IndexedCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * ISSUE #168: TryClaim must compare against the claims on ONE resource, not every claim in
 * the table. A flat scan grows with the WHOLE table (agents x claims each); this pins the
 * fix by building a table where one edge's queue is short but the table around it is not,
 * and reading GetLastTryClaimComparesForTest() rather than trusting a walkthrough of the
 * code - the whole "measure, don't infer" rule this project runs on.
 */
bool FTrafficOccupancyIndexedCostTest::RunTest(const FString& Parameters)
{
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;

	// THE BACKGROUND: 50 agents x 8 claims each, none of them on the edge under test, so the
	// table is exactly the scale the issue's O(A^2 k^2) figure is about - 400 claims - but
	// every one of them lands in its OWN node's bucket (nodes are exclusive, so a bucket of
	// more than one claim on the same node is a contradiction).
	const int32 Agents = 50;
	const int32 ClaimsPerAgent = 8;
	for (int32 Agent = 0; Agent < Agents; ++Agent)
	{
		for (int32 Claim = 0; Claim < ClaimsPerAgent; ++Claim)
		{
			// AgentId 0 is reserved (ExcludingAgent's "nobody" elsewhere in this file), so
			// background agents start at 1000 - well clear of the queue's 1..8 below.
			const int32 AgentId = 1000 + Agent;
			const int32 NodeIndex = Agent * ClaimsPerAgent + Claim;
			TestEqual(TEXT("background claim granted - the fixture must actually reach 400 claims"),
				Table.TryClaim(M2OccNodeClaim(AgentId, NodeIndex, false, 2), Blocker), EClaimResult::Granted);
		}
	}
	TestEqual(TEXT("the fixture holds A*k claims before the measured call"), Table.GetClaims().Num(), Agents * ClaimsPerAgent);

	// THE QUEUE UNDER TEST: 8 agents packed end to end on ONE edge - a queue, not a block,
	// exactly what FTrafficClaim::Conflicts' half-open rule exists to allow.
	const int32 QueueLen = 8;
	for (int32 Agent = 0; Agent < QueueLen; ++Agent)
	{
		TestEqual(TEXT("queue claim granted"),
			Table.TryClaim(M2OccEdgeClaim(Agent + 1, 7, Agent * 1000.0, Agent * 1000.0 + 900.0, false, 2), Blocker),
			EClaimResult::Granted);
	}

	// THE MEASURED CALL: one more claim on the SAME edge, past the back of the queue - it
	// conflicts with nothing, so every compare TryClaim makes is one this test can count and
	// name in advance: exactly the QueueLen claims already on edge 7, and NONE of the 400
	// background claims on other resources.
	const EClaimResult Result = Table.TryClaim(
		M2OccEdgeClaim(9999, 7, QueueLen * 1000.0, QueueLen * 1000.0 + 900.0, false, 2), Blocker);
	TestEqual(TEXT("the new tail of the queue is granted"), Result, EClaimResult::Granted);
	TestEqual(TEXT("TryClaim compared against exactly the queue on edge 7, not the whole table"),
		Table.GetLastTryClaimComparesForTest(), QueueLen);
	TestTrue(TEXT("and that is far below what an un-indexed scan of A*k claims would have cost"),
		Table.GetLastTryClaimComparesForTest() < Agents * ClaimsPerAgent);
	return true;
}

namespace
{
	/**
	 * THE PRE-#168 TABLE, kept verbatim so FTrafficOccupancyIndexedEquivalenceTest can drive
	 * it and the indexed FTrafficOccupancy with the identical random sequence and diff the
	 * result. Only the calls the driver below exercises are reproduced - this is a reference
	 * for TryClaim's arbitration and the per-agent Release* family, not a second production
	 * implementation.
	 */
	struct FLegacyOccupancy
	{
		TArray<FTrafficClaim> Claims;

		EClaimResult TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker)
		{
			int32 ExistingIndex = INDEX_NONE;
			for (int32 i = 0; i < Claims.Num(); ++i)
			{
				if (Claims[i].AgentId == Claim.AgentId && Claims[i].Resource == Claim.Resource)
				{
					ExistingIndex = i;
					break;
				}
			}

			TArray<int32> ToPreempt;
			for (int32 Index = 0; Index < Claims.Num(); ++Index)
			{
				if (Claims[Index].AgentId == Claim.AgentId) { continue; }
				if (!Claims[Index].Conflicts(Claim)) { continue; }
				if (Claims[Index].bOccupied)
				{
					OutBlocker = Claims[Index];
					return EClaimResult::Held;
				}
				if (!Claim.bOccupied && Claims[Index].Rank >= Claim.Rank)
				{
					OutBlocker = Claims[Index];
					return EClaimResult::Held;
				}
				ToPreempt.Add(Index);
			}

			if (ExistingIndex != INDEX_NONE) { Claims[ExistingIndex] = Claim; }
			else { Claims.Add(Claim); }

			for (int32 At = ToPreempt.Num() - 1; At >= 0; --At)
			{
				Claims.RemoveAtSwap(ToPreempt[At]);
			}
			return EClaimResult::Granted;
		}

		void ReleaseWhere(TFunctionRef<bool(const FTrafficClaim&)> Predicate)
		{
			Claims.RemoveAllSwap([&Predicate](const FTrafficClaim& C) { return Predicate(C); });
		}
		void ReleaseAll(int32 AgentId)
		{
			ReleaseWhere([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId; });
		}
		void Release(int32 AgentId, const FTrafficResource& Resource)
		{
			ReleaseWhere([AgentId, &Resource](const FTrafficClaim& C)
			{
				return C.AgentId == AgentId && C.Resource == Resource;
			});
		}
		void ReleaseReservations(int32 AgentId)
		{
			ReleaseWhere([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId && !C.bOccupied; });
		}
		void ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep)
		{
			ReleaseWhere([AgentId, &Keep](const FTrafficClaim& C)
			{
				return C.AgentId == AgentId && !Keep.Contains(C.Resource);
			});
		}
	};

	/** A total order over claims by IDENTITY (agent + resource), not by table position -
	 *  table position is allowed to differ between the legacy and the indexed table (both
	 *  use swap-removal, and issue #168 changed WHERE the swaps happen), so this is what
	 *  "identical GetClaims() contents" is measured by. */
	FString M2OccCanonKey(const FTrafficClaim& C)
	{
		return FString::Printf(TEXT("%d|%d|%d|%d|%d|%.1f|%.1f|%d|%d"),
			C.AgentId, (int32)C.Resource.Kind,
			C.Resource.Edge.Index, C.Resource.Node.Index, C.Resource.Surface.Index,
			C.From, C.To, C.bOccupied ? 1 : 0, C.Rank);
	}

	TArray<FString> M2OccSortedKeys(const TArray<FTrafficClaim>& Claims)
	{
		TArray<FString> Keys;
		Keys.Reserve(Claims.Num());
		for (const FTrafficClaim& C : Claims) { Keys.Add(M2OccCanonKey(C)); }
		Keys.Sort();
		return Keys;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyIndexedEquivalenceTest,
	"Airside.Model.Occupancy.IndexedEquivalence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * ISSUE #168: the index changes HOW a claim is found, never WHAT TryClaim or a Release* call
 * decides. Fixed-seed random claims and releases are driven, step for step, into the OLD
 * flat table (FLegacyOccupancy, reproduced above) and the NEW indexed one, and the two must
 * hold the same set of claims after every single step - a stronger check than reusing the
 * hand-picked scenarios above, because it exercises orderings and overlaps nobody hand-wrote.
 */
bool FTrafficOccupancyIndexedEquivalenceTest::RunTest(const FString& Parameters)
{
	FLegacyOccupancy Old;
	FTrafficOccupancy New;
	FRandomStream Rng(20260921);

	const int32 NumAgents = 6;
	const int32 NumHandles = 3;

	for (int32 Step = 0; Step < 500; ++Step)
	{
		const int32 AgentId = 1 + Rng.RandRange(0, NumAgents - 1);
		const int32 Op = Rng.RandRange(0, 99);

		if (Op < 60)
		{
			// A random claim: kind, handle, and - for an edge - a random non-degenerate
			// interval. Rank and occupancy are random too, so both tables see the full
			// spread of TryClaim's three outcomes.
			const int32 KindRoll = Rng.RandRange(0, 2);
			const int32 HandleIndex = Rng.RandRange(0, NumHandles - 1);
			const bool bOccupied = Rng.RandRange(0, 1) == 1;
			const int32 Rank = Rng.RandRange(0, 3);

			FTrafficClaim Claim;
			Claim.AgentId = AgentId;
			Claim.bOccupied = bOccupied;
			Claim.Rank = Rank;
			if (KindRoll == 0)
			{
				Claim.Resource = FTrafficResource::OfEdge(M2OccEdge(HandleIndex));
				const double From = Rng.RandRange(0, 9) * 100.0;
				Claim.From = From;
				Claim.To = From + 100.0 + Rng.RandRange(0, 8) * 100.0;
			}
			else if (KindRoll == 1)
			{
				Claim.Resource = FTrafficResource::OfNode(M2OccNode(HandleIndex));
			}
			else
			{
				Claim.Resource = FTrafficResource::OfSurface(M2OccSurface(HandleIndex));
			}

			FTrafficClaim OldBlocker;
			FTrafficClaim NewBlocker;
			const EClaimResult OldResult = Old.TryClaim(Claim, OldBlocker);
			const EClaimResult NewResult = New.TryClaim(Claim, NewBlocker);
			TestEqual(*FString::Printf(TEXT("step %d: TryClaim's Held/Granted decision agrees"), Step),
				NewResult, OldResult);
		}
		else if (Op < 75)
		{
			Old.ReleaseAll(AgentId);
			New.ReleaseAll(AgentId);
		}
		else if (Op < 85)
		{
			const int32 KindRoll = Rng.RandRange(0, 2);
			const int32 HandleIndex = Rng.RandRange(0, NumHandles - 1);
			const FTrafficResource Resource = KindRoll == 0 ? FTrafficResource::OfEdge(M2OccEdge(HandleIndex))
				: KindRoll == 1 ? FTrafficResource::OfNode(M2OccNode(HandleIndex))
				: FTrafficResource::OfSurface(M2OccSurface(HandleIndex));
			Old.Release(AgentId, Resource);
			New.Release(AgentId, Resource);
		}
		else if (Op < 95)
		{
			Old.ReleaseReservations(AgentId);
			New.ReleaseReservations(AgentId);
		}
		else
		{
			// Keep a random subset of this agent's own resources - reads them off the NEW
			// table, which by this point in the run is exactly what production code does
			// (ApplyClaims builds Wanted from what it just claimed, not from a fixture).
			TArray<FTrafficResource> Keep;
			for (const FTrafficClaim& C : New.GetClaims())
			{
				if (C.AgentId == AgentId && Rng.RandRange(0, 1) == 1)
				{
					Keep.Add(C.Resource);
				}
			}
			Old.ReleaseExcept(AgentId, Keep);
			New.ReleaseExcept(AgentId, Keep);
		}

		const TArray<FString> OldKeys = M2OccSortedKeys(Old.Claims);
		const TArray<FString> NewKeys = M2OccSortedKeys(New.GetClaims());
		if (OldKeys != NewKeys)
		{
			AddError(*FString::Printf(TEXT("step %d: indexed table diverged from the legacy one (%d claims vs %d)"),
				Step, NewKeys.Num(), OldKeys.Num()));
			return false;
		}
	}
	return true;
}

#endif
