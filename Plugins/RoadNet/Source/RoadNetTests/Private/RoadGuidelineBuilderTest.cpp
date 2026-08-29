#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGuidelineBuilderTest,
	"RoadNet.Build.GuidelineBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGuidelineBuilderTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(800.0, 200.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadGuidelineBuilder::Build(*Net, Solved);

	// One edge per segment per declared guideline. The taxiway profile declares one, and
	// there are two segments, so exactly two edges carry a DerivedFrom naming a segment.
	{
		int32 SegmentEdges = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				++SegmentEdges;
			}
		}
		TestEqual(TEXT("one guideline edge per segment"), SegmentEdges, 2);
	}

	// The edge must inherit the profile's access and direction, or the guideline exists
	// and admits nobody - which looks like a pathfinding bug, not a derivation bug.
	{
		bool bFound = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			bFound = true;
			TestTrue(TEXT("the derived edge admits aircraft"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft));
			TestFalse(TEXT("and not pedestrians"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Pedestrian));
			TestEqual(TEXT("bidirectional, as the profile declares"),
				Edge.Direction, EGuidelineDir::Bidirectional);
			TestTrue(TEXT("and is marked derived"), Edge.bDerived);
		}
		TestTrue(TEXT("the east segment produced an edge"), bFound);
	}

	// Endpoints sit ON the segment's stored cut lines, not at its node positions. A
	// guideline that ran node-to-node would overlap the junction it should hand off to.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FVector2D ExpectedA = FMath::Lerp(Seg->RightCutA, Seg->LeftCutA, 0.5);

		bool bOnCutLine = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			const FGuidelineNode* NodeA = Net->GetGuidelineNode(Edge.A);
			const FGuidelineNode* NodeB = Net->GetGuidelineNode(Edge.B);
			if (NodeA != nullptr && NodeB != nullptr)
			{
				bOnCutLine =
					NodeA->Position.Equals(ExpectedA, 0.01) ||
					NodeB->Position.Equals(ExpectedA, 0.01);
			}
		}
		TestTrue(TEXT("an endpoint sits on the A-end cut line"), bOnCutLine);
	}

	// The B end's cut line is authored from B's point of view, so its left is the segment's
	// RIGHT walking A to B - which is why the builder passes the B-end cut vertices
	// swapped. With a CENTRED guideline alpha is exactly 0.5, and Lerp(X,Y,0.5) equals
	// Lerp(Y,X,0.5), so that swap is mathematically invisible: deleting it leaves every
	// assertion above still passing.
	//
	// An off-centre guideline is what makes it visible. Offset +175 on a 700-wide profile
	// gives alpha 0.75, so both ends must land on the SAME side of the segment axis.
	// Without the swap the B end lands across it and the guideline cuts diagonally over
	// the road.
	{
		URoadNetwork* Offset = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* OffsetProfile = NewObject<URoadProfile>(GetTransientPackage());

		FProfileBand OffsetLane;
		OffsetLane.Width = 350.0;
		OffsetLane.Type = ERoadBandType::Lane;
		OffsetProfile->Bands.Add(OffsetLane);
		OffsetProfile->Bands.Add(OffsetLane);

		FProfileGuideline LeftOfCentre;
		LeftOfCentre.CentreOffset = 175.0;          // positive is left; gives alpha 0.75
		LeftOfCentre.Class = ETraversalClass::GroundVehicle;
		LeftOfCentre.Direction = EGuidelineDir::AToB;
		OffsetProfile->Guidelines.Add(LeftOfCentre);

		const FRoadNodeId Hub  = Offset->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Away = Offset->AddNode(FVector2D(12000.0, 0.0));
		const FRoadNodeId Side = Offset->AddNode(FVector2D(0.0, 12000.0));
		const FRoadSegmentId Eastward = Offset->AddStraightSegment(Hub, Away, OffsetProfile);
		Offset->AddStraightSegment(Hub, Side, OffsetProfile);

		const FRoadSolveResult OffsetSolved = FRoadNetworkSolver::SolveAll(*Offset);
		TestEqual(TEXT("the off-centre network solved"), OffsetSolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*Offset, OffsetSolved);

		// Eastward runs +X, so the segment's left is +Y. A guideline 175uu left of centre
		// sits at roughly y = +175 at BOTH ends.
		bool bChecked = false;
		for (const FGuidelineEdge& Edge : Offset->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != Eastward)
			{
				continue;
			}

			const FGuidelineNode* OffsetA = Offset->GetGuidelineNode(Edge.A);
			const FGuidelineNode* OffsetB = Offset->GetGuidelineNode(Edge.B);
			if (TestNotNull(TEXT("the off-centre edge's A node resolves"), OffsetA) &&
				TestNotNull(TEXT("the off-centre edge's B node resolves"), OffsetB))
			{
				bChecked = true;
				TestTrue(TEXT("the A end sits left of the segment axis"),
					OffsetA->Position.Y > 100.0);
				TestTrue(TEXT("and the B end sits on the SAME side, not across it"),
					OffsetB->Position.Y > 100.0);
			}
		}
		TestTrue(TEXT("the off-centre segment produced an edge to check"), bChecked);
	}

	// Turn paths. The centre node has two arms, so two ordered pairs, so two turn edges.
	// A turn edge is recognised by carrying no DerivedFrom - it belongs to a junction
	// rather than to any one segment.
	{
		int32 TurnEdges = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet())
			{
				++TurnEdges;
				TestEqual(TEXT("a turn path runs one way"), Edge.Direction, EGuidelineDir::AToB);
			}
		}
		TestEqual(TEXT("two arms give two ordered turn paths"), TurnEdges, 2);
	}

	// THE CONNECTIVITY PROPERTY. A turn path must reuse the SAME node handles its segments
	// end on, or the graph is a heap of disconnected sticks that each look fine on their
	// own and that nothing can route across. Handles, not positions - this graph has no
	// weld contract, so two coincident-but-distinct nodes would be invisible to any
	// position check while being fatal to pathfinding.
	{
		TSet<FGuidelineNodeId> SegmentEnds;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				SegmentEnds.Add(Edge.A);
				SegmentEnds.Add(Edge.B);
			}
		}

		int32 Connected = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet() &&
				SegmentEnds.Contains(Edge.A) && SegmentEnds.Contains(Edge.B))
			{
				++Connected;
			}
		}
		TestEqual(TEXT("every turn path joins two segment endpoints"), Connected, 2);
	}

	// A three-arm node gives six ordered pairs. Asserted separately because two arms
	// cannot distinguish "ordered pairs" from "pairs" - both give two.
	{
		URoadNetwork* Tee = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* TeeProfile = URoadProfile::MakeTransient(800.0, 200.0);

		const FRoadNodeId Hub = Tee->AddNode(FVector2D(0.0, 0.0));
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D( 12000.0,     0.0)), TeeProfile);
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D(-12000.0,     0.0)), TeeProfile);
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D(     0.0, 12000.0)), TeeProfile);

		const FRoadSolveResult TeeSolved = FRoadNetworkSolver::SolveAll(*Tee);
		TestEqual(TEXT("the tee solved"), TeeSolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*Tee, TeeSolved);

		int32 TeeTurns = 0;
		for (const FGuidelineEdge& Edge : Tee->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet())
			{
				++TeeTurns;
			}
		}
		TestEqual(TEXT("three arms give six ordered turn paths"), TeeTurns, 6);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
