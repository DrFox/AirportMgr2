#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkTest,
	"Airside.Model.Network",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(10000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 10000.0));
	const FRoadNodeId West   = Net->AddNode(FVector2D(-10000.0, 0.0));

	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToWest  = Net->AddStraightSegment(Centre, West,  Profile);

	TestTrue(TEXT("segments created"), ToNorth.IsSet() && ToEast.IsSet() && ToWest.IsSet());

	// Outgoing tangents at the centre node.
	const FVector2D TanEast = Net->GetOutgoingTangent(ToEast, Centre);
	TestTrue(TEXT("east tangent"), TanEast.Equals(FVector2D(1.0, 0.0), 1e-6));

	const FVector2D TanNorth = Net->GetOutgoingTangent(ToNorth, Centre);
	TestTrue(TEXT("north tangent"), TanNorth.Equals(FVector2D(0.0, 1.0), 1e-6));

	// Tangent at the far end points back toward the centre.
	const FVector2D TanBack = Net->GetOutgoingTangent(ToEast, East);
	TestTrue(TEXT("reverse tangent"), TanBack.Equals(FVector2D(-1.0, 0.0), 1e-6));

	// Incident list sorted by bearing ascending: east(0), north(UE_DOUBLE_PI/2), west(UE_DOUBLE_PI).
	const FRoadNode* CentreNode = Net->GetNode(Centre);
	TestEqual(TEXT("incident count"), CentreNode->Incident.Num(), 3);
	TestTrue(TEXT("order[0] east"),  CentreNode->Incident[0] == ToEast);
	TestTrue(TEXT("order[1] north"), CentreNode->Incident[1] == ToNorth);
	TestTrue(TEXT("order[2] west"),  CentreNode->Incident[2] == ToWest);

	// Removing a segment updates both endpoints' incident lists.
	TestTrue(TEXT("remove"), Net->RemoveSegment(ToNorth));
	TestEqual(TEXT("incident after remove"), Net->GetNode(Centre)->Incident.Num(), 2);
	TestEqual(TEXT("far node emptied"), Net->GetNode(North)->Incident.Num(), 0);
	TestNull(TEXT("segment gone"), Net->GetSegment(ToNorth));

	// Removing a node cascades to its segments.
	TestTrue(TEXT("remove centre"), Net->RemoveNode(Centre));
	TestNull(TEXT("centre gone"), Net->GetNode(Centre));
	TestNull(TEXT("east segment cascaded"), Net->GetSegment(ToEast));
	TestEqual(TEXT("east node emptied"), Net->GetNode(East)->Incident.Num(), 0);

	// Self-loops and invalid handles are rejected.
	TestFalse(TEXT("self loop rejected"), Net->AddStraightSegment(East, East, Profile).IsSet());
	TestFalse(TEXT("stale handle rejected"), Net->AddStraightSegment(Centre, East, Profile).IsSet());

	// IsSet() reports assignment, not liveness: a handle to a slot that has since been
	// removed still reads as set. Liveness is RoadSlot::IsValid's job, and the network
	// enforces it - which is why the stale-handle AddStraightSegment above was rejected.
	TestTrue(TEXT("stale handle still reads as set"), Centre.IsSet());
	TestNull(TEXT("stale handle is not live"), Net->GetNode(Centre));

	// --- GetOutgoingTangent must never return a zero vector ---
	//
	// Only A == B is rejected, so two DISTINCT nodes may sit at the same position. The
	// degenerate-control fallback then computes a zero chord, and an unguarded
	// GetSafeNormal hands back (0,0) - which collapses every edge ray and makes the node
	// silently fail to solve rather than fail loudly.
	{
		URoadNetwork* Degenerate = NewObject<URoadNetwork>(GetTransientPackage());

		// Case 1: Control sits exactly on the node, so the chord fallback is taken.
		const FRoadNodeId Origin = Degenerate->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Far    = Degenerate->AddNode(FVector2D(0.0, 5000.0));
		const FRoadSegmentId Straight =
			Degenerate->AddSegment(Origin, Far, FVector2D(0.0, 0.0), Profile);
		TestTrue(TEXT("degenerate-control segment created"), Straight.IsSet());

		const FVector2D FromControlFallback = Degenerate->GetOutgoingTangent(Straight, Origin);
		TestTrue(TEXT("control-on-node falls back to the chord"),
			FromControlFallback.Equals(FVector2D(0.0, 1.0), 1e-6));

		// Case 2: two distinct nodes at the same position - the chord is zero too.
		const FRoadNodeId Twin = Degenerate->AddNode(FVector2D(0.0, 0.0));
		const FRoadSegmentId Coincident =
			Degenerate->AddSegment(Origin, Twin, FVector2D(0.0, 0.0), Profile);
		TestTrue(TEXT("coincident segment created"), Coincident.IsSet());

		const FVector2D BothEnds[] = {
			Degenerate->GetOutgoingTangent(Coincident, Origin),
			Degenerate->GetOutgoingTangent(Coincident, Twin)
		};
		for (const FVector2D& Tangent : BothEnds)
		{
			TestTrue(TEXT("coincident-node tangent is unit length"),
				FMath::IsNearlyEqual(Tangent.Length(), 1.0, 1e-9));
		}

		// And the earlier fallbacks are unit length too, by the same contract.
		TestTrue(TEXT("chord fallback is unit length"),
			FMath::IsNearlyEqual(FromControlFallback.Length(), 1.0, 1e-9));
	}

	// --- Cut vertices are part of the model, not something callers recompute (K2) ---
	{
		URoadNetwork* CutNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* CutProfile = URoadProfile::MakeTransient(2300.0, 1500.0);

		const FRoadNodeId P = CutNet->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Q = CutNet->AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId Seg = CutNet->AddStraightSegment(P, Q, CutProfile);

		const FRoadSegment* Fresh = CutNet->GetSegment(Seg);
		TestFalse(TEXT("a new segment's A end is not yet solved"), Fresh->bSolvedA);
		TestFalse(TEXT("a new segment's B end is not yet solved"), Fresh->bSolvedB);
		TestTrue(TEXT("cut vertices start at zero"),
			Fresh->LeftCutA.IsZero() && Fresh->RightCutA.IsZero() &&
			Fresh->LeftCutB.IsZero() && Fresh->RightCutB.IsZero());

		// Only the solver writes these; the test stands in for it here.
		FRoadSegment* Mutable = CutNet->GetSegmentMutable(Seg);
		Mutable->LeftCutA  = FVector2D(1150.0, 1150.0);
		Mutable->RightCutA = FVector2D(1150.0, -1150.0);
		Mutable->LeftCutB  = FVector2D(8850.0, -1150.0);
		Mutable->RightCutB = FVector2D(8850.0, 1150.0);
		Mutable->bSolvedA = true;
		Mutable->bSolvedB = true;

		const FRoadSegment* Solved = CutNet->GetSegment(Seg);
		TestTrue(TEXT("solved flags survive"), Solved->bSolvedA && Solved->bSolvedB);
		// Bitwise, not Equals(). These values are the shared truth.
		TestTrue(TEXT("left cut A stored exactly"),
			Solved->LeftCutA.X == 1150.0 && Solved->LeftCutA.Y == 1150.0);
		TestTrue(TEXT("right cut B stored exactly"),
			Solved->RightCutB.X == 8850.0 && Solved->RightCutB.Y == 1150.0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
