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

	// Idempotence. Build runs on every edit in the build tool, so a Build that accumulates
	// is a leak that grows with every mouse move - and one that looks like nothing at all
	// until the graph is large.
	//
	// Counting EDGES alone leaves the orphan sweep completely unpinned: deleting the whole
	// sweep leaks four nodes per rebuild and every edge assertion stays green. So the node
	// count is asserted alongside.
	{
		int32 Before = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive) { ++Before; }
		}

		int32 NodesBefore = 0;
		for (const FGuidelineNode& Node : Net->GetGuidelineNodes())
		{
			if (Node.bAlive) { ++NodesBefore; }
		}

		FRoadGuidelineBuilder::Build(*Net, Solved);

		int32 After = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive) { ++After; }
		}

		int32 NodesAfter = 0;
		for (const FGuidelineNode& Node : Net->GetGuidelineNodes())
		{
			if (Node.bAlive) { ++NodesAfter; }
		}

		TestEqual(TEXT("rebuilding does not accumulate edges"), After, Before);
		TestEqual(TEXT("rebuilding does not accumulate nodes"), NodesAfter, NodesBefore);
	}

	// An edited guideline survives regeneration. Without this, a player who redraws a
	// taxi line loses it the next time anything touches the pavement - silently, because
	// the replacement looks exactly like a correct derived guideline.
	{
		FGuidelineEdgeId Edited;
		for (int32 Index = 0; Index < Net->GetGuidelineEdges().Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Net->GetGuidelineEdges()[Index];
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				Edited.Index = Index;
				Edited.Generation = Edge.Generation;
				break;
			}
		}

		if (TestTrue(TEXT("found a derived edge to edit"), Edited.IsSet()))
		{
			FGuidelineEdge* Mutable = Net->GetGuidelineEdgeMutable(Edited);
			if (TestNotNull(TEXT("the edge is mutable"), Mutable))
			{
				Mutable->bDerived = false;
				Mutable->MaxWingspan = 6543.0;   // a value derivation would never produce
			}

			FRoadGuidelineBuilder::Build(*Net, Solved);

			const FGuidelineEdge* Survivor = Net->GetGuidelineEdge(Edited);
			if (TestNotNull(TEXT("the edited edge survives regeneration"), Survivor))
			{
				TestFalse(TEXT("still marked as edited"), Survivor->bDerived);
				TestEqual(TEXT("and keeps its edited value"), Survivor->MaxWingspan, 6543.0);

				// SURVIVING IS NOT ENOUGH. Deriving over a spared edge leaves the player's
				// line AND a fresh derived one on the same segment - and every turn path
				// and every route uses the derived one, so the edit does nothing while the
				// graph grows a coincident pair per edit. Only the count catches that:
				// every assertion above passes with the duplicate sitting right there.
				const FRoadSegmentId EditedSegment = Survivor->DerivedFrom;
				int32 OnThatSegment = 0;
				for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
				{
					if (Edge.bAlive && Edge.DerivedFrom == EditedSegment)
					{
						++OnThatSegment;
					}
				}
				TestEqual(TEXT("and is not duplicated by a fresh derived edge"),
					OnThatSegment, 1);
			}
		}
	}

	// The wingspan sentinel. 0 means UNLIMITED, so a turn between an unlimited arm and a
	// 5200 arm must be 5200 - a naive Min gives 0 and waves a 747 onto a turn that cannot
	// take it. Every other profile in this file leaves MaxWingspan at 0, so without a real
	// value on one arm the three-way conditional is never actually evaluated and swapping
	// it for that Min leaves the whole suite green.
	{
		URoadNetwork* Span = NewObject<URoadNetwork>(GetTransientPackage());

		// Built by hand: MakeTransient always produces MaxWingspan == 0 on both arms.
		auto MakeSpanProfile = [](double MaxWingspan) -> URoadProfile*
		{
			URoadProfile* Made = NewObject<URoadProfile>(GetTransientPackage());

			FProfileBand Lane;
			Lane.Width = 350.0;
			Lane.Type = ERoadBandType::Lane;
			Made->Bands.Add(Lane);
			Made->Bands.Add(Lane);

			FProfileGuideline Centre;
			Centre.CentreOffset = 0.0;
			Centre.Class = ETraversalClass::Aircraft;      // same class on both arms, so the
			Centre.Direction = EGuidelineDir::Bidirectional; // direction filter keeps the turns
			Centre.Width = 400.0;
			Centre.MaxWingspan = MaxWingspan;
			Made->Guidelines.Add(Centre);

			return Made;
		};

		URoadProfile* Unlimited = MakeSpanProfile(0.0);
		URoadProfile* Limited   = MakeSpanProfile(5200.0);

		const FRoadNodeId SpanHub  = Span->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId SpanEast = Span->AddNode(FVector2D(12000.0, 0.0));
		const FRoadNodeId SpanNorth = Span->AddNode(FVector2D(0.0, 12000.0));
		Span->AddStraightSegment(SpanHub, SpanEast,  Unlimited);
		Span->AddStraightSegment(SpanHub, SpanNorth, Limited);

		const FRoadSolveResult SpanSolved = FRoadNetworkSolver::SolveAll(*Span);
		TestEqual(TEXT("the wingspan network solved"), SpanSolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*Span, SpanSolved);

		int32 SpanTurns = 0;
		for (const FGuidelineEdge& Edge : Span->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet())
			{
				++SpanTurns;
				TestEqual(TEXT("an unlimited arm does not widen a limited one"),
					Edge.MaxWingspan, 5200.0);
			}
		}
		TestEqual(TEXT("both turn paths at the mixed junction were checked"), SpanTurns, 2);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
