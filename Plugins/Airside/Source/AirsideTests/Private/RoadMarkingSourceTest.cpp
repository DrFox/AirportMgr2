#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMarkingSourceTest,
	"Airside.Model.MarkingSources",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
	/** A profile of one full-width guideline of the given class. */
	URoadProfile* MakeGuidedProfile(ETraversalClass Class, EGuidelineDir Direction, double Width)
	{
		URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

		FProfileBand Band;
		Band.Width = Width;
		Band.Type = ERoadBandType::Lane;
		Profile->Bands.Add(Band);

		FProfileGuideline Line;
		Line.CentreOffset = 0.0;
		Line.Class = Class;
		Line.Direction = Direction;
		Line.Width = (Class == ETraversalClass::Aircraft) ? 0.0 : Width;
		Profile->Guidelines.Add(Line);

		return Profile;
	}
}

bool FRoadMarkingSourceTest::RunTest(const FString& Parameters)
{
	// Spec section 6 is a falsification test for the MODEL. Every marking in the reference
	// images must have a source in the data; where one does not, the model is missing
	// something and this is where that shows up - long before anything tries to draw it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// The concrete everything sits on.
	{
		FApronSurface Slab;
		Slab.Outline = {
			FVector2D(-20000.0, -20000.0),
			FVector2D( 40000.0, -20000.0),
			FVector2D( 40000.0,  40000.0),
			FVector2D(-20000.0,  40000.0) };
		Slab.SurfaceMaterialSlot = TEXT("Concrete");
		TestTrue(TEXT("the apron is placed"), Net->AddApron(MoveTemp(Slab)).IsSet());
	}

	URoadProfile* Taxiway = MakeGuidedProfile(ETraversalClass::Aircraft, EGuidelineDir::Bidirectional, 2300.0);
	URoadProfile* Service = MakeGuidedProfile(ETraversalClass::GroundVehicle, EGuidelineDir::Bidirectional, 400.0);
	URoadProfile* Walkway = MakeGuidedProfile(ETraversalClass::Pedestrian, EGuidelineDir::Bidirectional, 300.0);

	// A taxiway with a junction, a service road crossing the apron, and a walkway.
	const FRoadNodeId TaxiA = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId TaxiB = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId TaxiC = Net->AddNode(FVector2D(0.0, 20000.0));
	Net->AddStraightSegment(TaxiA, TaxiB, Taxiway);
	Net->AddStraightSegment(TaxiA, TaxiC, Taxiway);

	const FRoadNodeId RoadA = Net->AddNode(FVector2D(10000.0, -10000.0));
	const FRoadNodeId RoadB = Net->AddNode(FVector2D(10000.0,  10000.0));
	const FRoadNodeId RoadC = Net->AddNode(FVector2D(25000.0,  10000.0));
	Net->AddStraightSegment(RoadA, RoadB, Service);
	Net->AddStraightSegment(RoadB, RoadC, Service);

	const FRoadNodeId WalkA = Net->AddNode(FVector2D(30000.0, -5000.0));
	const FRoadNodeId WalkB = Net->AddNode(FVector2D(30000.0, 15000.0));
	Net->AddStraightSegment(WalkA, WalkB, Walkway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("the marking network solved"), Solved.FailedNodes, 0);

	FRoadGuidelineBuilder::Build(*Net, Solved);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Gate = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(25000.0, 25000.0), UE_DOUBLE_PI);
	TestTrue(TEXT("the stand is placed"), Gate.IsSet());

	// --- Now walk spec section 6's table, row by row. -------------------------------

	// Yellow taxi centreline <- a guideline edge with Aircraft access.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft))
			{
				++Sources;
			}
		}
		TestTrue(TEXT("a taxi centreline has a source"), Sources > 0);
	}

	// Service road edge lines <- a GroundVehicle edge, drawn at +/- Width/2. So the edge
	// must carry a NON-ZERO width, or there is nothing to offset the two lines by and the
	// marking cannot be derived at all.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive &&
				Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) &&
				!Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) &&
				Edge.Width > 0.0)
			{
				++Sources;
			}
		}
		TestTrue(TEXT("service road edge lines have a source, with a width to offset by"),
			Sources > 0);
	}

	// Pedestrian walkway edging <- a Pedestrian edge, likewise needing a width.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive &&
				Edge.AllowedTraffic.Allows(ETraversalClass::Pedestrian) &&
				Edge.Width > 0.0)
			{
				++Sources;
			}
		}
		TestTrue(TEXT("walkway edging has a source"), Sources > 0);
	}

	// Stand number and stop position <- the entity's Aircraft anchor. That row has a real
	// source and is asserted below.
	//
	// Stand lead-in line <- "the guideline into an Aircraft anchor" - which does NOT have a
	// source. Nothing emits an edge terminating on an anchor node: the derivation only
	// produces edges between a segment's two cut-line ends, or between arm ends at a
	// junction. So this block pins the ENDPOINT a lead-in would terminate on, and nothing
	// about the lead-in itself. Recorded in spec section 6 beside the zebra row.
	{
		const FEntityInstance* Instance = Net->GetEntity(Gate);
		if (TestNotNull(TEXT("the stand resolves"), Instance))
		{
			// Spec section 6's stop-position marking is derived from the stand's OWN POSE,
			// not from an anchor. That changed when stand and aircraft were split: the mark
			// painted on the apron is where the stand is, and where the nose gear stops is
			// the same point by construction. An "Aircraft" fixture would have been a
			// second copy of the stand's own position, free to disagree with it.
			TestTrue(TEXT("the stand declares it can take an aircraft"),
				Stand->Provides(EServiceRole::Aircraft));
			TestTrue(TEXT("and its pose is the stop position the marking derives from"),
				Instance->Position.Equals(FVector2D(25000.0, 25000.0), 0.01));

			// The ground fixtures still resolve, by id.
			TestNotNull(TEXT("the hydrant pit has a source, a live node"),
				Net->GetAnchorNode(Gate, FName(TEXT("HydrantPit"))));

			TestNull(TEXT("an id the stand does not declare resolves to nothing"),
				Net->GetAnchorNode(Gate, FName(TEXT("NoSuchAnchor"))));
		}
	}

	// Runway / taxiway edge treatment <- the surface profile's outermost band.
	{
		TestTrue(TEXT("the taxiway profile has a band to derive its edge from"),
			Taxiway->Bands.Num() > 0);
		TestTrue(TEXT("with a real width"), Taxiway->GetTotalWidth() > 0.0);
	}

	// Hold bar <- a guideline node flagged hold-short.
	//
	// Nothing WRITES HoldShortFor yet - that is the build tool's job - so what section 6
	// needs from the model here is that the node can CARRY the source. Asserting only that
	// a node can be created would establish nothing about hold bars at all; this sets the
	// field and reads it back through the network, which is the actual claim.
	{
		const FGuidelineNodeId Marked =
			Net->AddGuidelineNode(FVector2D(1.0, 1.0), /*bDerived=*/false);
		if (TestTrue(TEXT("a hold-short node can be created"), Marked.IsSet()))
		{
			// Any live segment will do as the thing being protected; a runway is the real
			// case, and the field is a plain FRoadSegmentId either way.
			FRoadSegmentId Protected;
			for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
			{
				if (Net->GetSegments()[Index].bAlive)
				{
					Protected.Index = Index;
					Protected.Generation = Net->GetSegments()[Index].Generation;
					break;
				}
			}
			TestTrue(TEXT("there is a surface for it to protect"), Protected.IsSet());

			if (FGuidelineNode* Mutable = Net->GetGuidelineNodeMutable(Marked))
			{
				Mutable->HoldShortFor = Protected;
			}

			const FGuidelineNode* ReadBack = Net->GetGuidelineNode(Marked);
			if (TestNotNull(TEXT("the hold-short node resolves"), ReadBack))
			{
				TestTrue(TEXT("and carries the surface it protects"),
					ReadBack->HoldShortFor == Protected);
			}
		}
	}

	// Road centre line <- two adjacent lane guidelines of ONE surface.
	//
	// "Surface" is the load-bearing word, and it is the one a profile-only assertion never
	// touches: asserting that a profile declares two guidelines only restates the fixture
	// this block just built. The claim is that a single SEGMENT derives both of them, both
	// naming that segment as their source - which is what a marking pass has to walk to
	// find the pair to paint between.
	//
	// Built on its own network so the fixture above, and the zebra count that reads it, are
	// left undisturbed.
	{
		URoadProfile* TwoLane = NewObject<URoadProfile>(GetTransientPackage());
		FProfileBand Lane;
		Lane.Width = 350.0;
		Lane.Type = ERoadBandType::Lane;
		TwoLane->Bands.Add(Lane);
		TwoLane->Bands.Add(Lane);

		FProfileGuideline Left;
		Left.CentreOffset = 175.0;
		Left.Class = ETraversalClass::GroundVehicle;
		Left.Direction = EGuidelineDir::AToB;
		Left.Width = 350.0;
		FProfileGuideline Right = Left;
		Right.CentreOffset = -175.0;
		Right.Direction = EGuidelineDir::BToA;
		TwoLane->Guidelines.Add(Left);
		TwoLane->Guidelines.Add(Right);

		URoadNetwork* Divided = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId DualA = Divided->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId DualB = Divided->AddNode(FVector2D(15000.0, 0.0));
		const FRoadSegmentId Dual = Divided->AddStraightSegment(DualA, DualB, TwoLane);
		TestTrue(TEXT("the two-lane surface is placed"), Dual.IsSet());

		const FRoadSolveResult DualSolved = FRoadNetworkSolver::SolveAll(*Divided);
		TestEqual(TEXT("the two-lane surface solved"), DualSolved.FailedNodes, 0);
		FRoadGuidelineBuilder::Build(*Divided, DualSolved);

		int32 FromThisSurface = 0;
		int32 Forward = 0;
		int32 Backward = 0;
		for (const FGuidelineEdge& Edge : Divided->GetGuidelineEdges())
		{
			if (!Edge.bAlive || !(Edge.DerivedFrom == Dual))
			{
				continue;
			}

			++FromThisSurface;
			if (Edge.Direction == EGuidelineDir::AToB) { ++Forward; }
			if (Edge.Direction == EGuidelineDir::BToA) { ++Backward; }
		}

		TestEqual(TEXT("one surface derived exactly two guidelines"), FromThisSurface, 2);

		// Opposed, which is what makes the line between them a CENTRE line rather than a
		// lane divider: two guidelines running the same way would be one carriageway.
		TestEqual(TEXT("one of them running A to B"), Forward, 1);
		TestEqual(TEXT("and one running B to A"), Backward, 1);
	}

	// Zebra crossing <- a node where a Pedestrian edge meets a GroundVehicle edge.
	//
	// THIS ROW HAS NO SOURCE, and that is the finding, not a failure of this test. The
	// derivation never creates such a node: a pedestrian guideline and a vehicle guideline
	// derived from two different segments terminate on their own cut lines and are joined
	// only if a junction exists between those SURFACES. A walkway painted across a service
	// road on an apron shares no surface junction with it at all - which is precisely the
	// case spec section 1 says the two-graph split exists to represent.
	//
	// So a guideline CROSSING - two guidelines meeting where no surface junction does - is
	// a thing the model can hold but nothing can currently produce. It needs either a
	// hand-drawn guideline API or a crossing-detection pass. Recorded as a plan gap; the
	// assertion below states what IS true today so the gap is visible rather than implied.
	{
		int32 MixedClassNodes = 0;
		for (int32 Index = 0; Index < Net->GetGuidelineNodes().Num(); ++Index)
		{
			const FGuidelineNode& Node = Net->GetGuidelineNodes()[Index];
			if (!Node.bAlive)
			{
				continue;
			}

			bool bWalk = false;
			bool bDrive = false;
			for (const FGuidelineEdgeId EdgeId : Node.Incident)
			{
				if (const FGuidelineEdge* Edge = Net->GetGuidelineEdge(EdgeId))
				{
					if (Edge->AllowedTraffic.Allows(ETraversalClass::Pedestrian))    { bWalk = true; }
					if (Edge->AllowedTraffic.Allows(ETraversalClass::GroundVehicle)) { bDrive = true; }
				}
			}
			if (bWalk && bDrive)
			{
				++MixedClassNodes;
			}
		}

		TestEqual(
			TEXT("no zebra source exists yet - guideline crossings are not derivable"),
			MixedClassNodes, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
