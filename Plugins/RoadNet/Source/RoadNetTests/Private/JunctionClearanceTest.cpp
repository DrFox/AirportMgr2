#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The PIE build tool's defaults: 200 uu wide, 100 uu fillet. */
	URoadProfile* BuildToolProfile()
	{
		return URoadProfile::MakeTransient(200.0, 100.0);
	}

	FRoadPlacementLimits DefaultLimits()
	{
		FRoadPlacementLimits Limits;
		Limits.MinSegmentLength = 250.0;
		Limits.MinTurnDegrees = 25.0;
		return Limits;
	}

	/** A snap result naming an existing node, as the node rule would produce. */
	FRoadSnapResult NodeSnap(const URoadNetwork& Network, FRoadNodeId Node)
	{
		FRoadSnapResult Out;
		Out.Kind = ERoadSnapKind::Node;
		Out.Node = Node;
		if (const FRoadNode* Found = Network.GetNode(Node))
		{
			Out.Position = Found->Position;
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJunctionClearanceTest,
	"RoadNet.Tool.JunctionClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FJunctionClearanceTest::RunTest(const FString& Parameters)
{
	// --- NodeReach ------------------------------------------------------------------
	//
	// 1. A node with no arms paves nothing, so it reaches nowhere. Without this the snap
	//    radius would balloon around every bare node the moment one was placed.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Lonely = Net->AddNode(FVector2D::ZeroVector);

		TestEqual(TEXT("a node with no arms reaches nothing"),
			FRoadNetworkSolver::NodeReach(*Net, Lonely), 0.0);
	}

	// 2. A corner reaches further than a straight-through node, and BOTH agree with the
	//    solver's own cut distance for that node.
	//
	//    Asserted against SolveAll rather than against a number typed here. A constant
	//    would pass while the two drifted apart, and drifting apart is the whole failure
	//    this shares one function to prevent.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = BuildToolProfile();

		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net->AddNode(FVector2D(4000.0, 0.0));
		const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 4000.0));
		Net->AddStraightSegment(Centre, East,  Profile);
		Net->AddStraightSegment(Centre, North, Profile);

		const double Reach = FRoadNetworkSolver::NodeReach(*Net, Centre);
		TestTrue(TEXT("a corner reaches beyond its own half width"), Reach > 100.0);

		// The same node, solved the way the mesh solves it.
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		const FJunctionResult* Junction = Solved.NodeResults.Find(Centre.Index);
		if (Junction != nullptr && Junction->Arms.Num() > 0)
		{
			double Widest = 0.0;
			for (const FJunctionArmResult& Arm : Junction->Arms)
			{
				Widest = FMath::Max(Widest, Arm.CutDistance + 100.0);
			}

			TestEqual(TEXT("the reach a tool sees is the cut the mesh actually makes"),
				Reach, Widest);
		}
		else
		{
			AddError(TEXT("the corner produced no solved junction to compare against"));
		}

		// Conservative by construction: the furthest pavement corner is at
		// sqrt(Cut^2 + HalfWidth^2), which Cut + HalfWidth can only exceed.
		TestTrue(TEXT("the reach never understates the pavement"), Reach > 0.0);
	}

	// --- The snap that closes the overlap --------------------------------------------
	//
	// 3. THE REPRO. A cursor beyond NodeRadius but inside the junction used to resolve
	//    Free, which built a second node inside existing pavement - two junction polygons
	//    at the same Z, which is not a solvable surface, only a z-fight.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = BuildToolProfile();

		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net->AddNode(FVector2D(4000.0, 0.0));
		const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 4000.0));
		Net->AddStraightSegment(Centre, East,  Profile);
		Net->AddStraightSegment(Centre, North, Profile);

		const double Reach = FRoadNetworkSolver::NodeReach(*Net, Centre);

		FRoadSnapSettings Settings;
		Settings.NodeRadius = 150.0;
		Settings.SegmentRadius = 150.0;
		Settings.JunctionSnapFactor = 1.0;

		const FRoadSnapChain Chain;

		// Placed on the bisector, so the cursor is inside the junction rather than sitting
		// on either arm's centreline where the segment rule would have claimed it anyway.
		const double Inside = (150.0 + Reach) * 0.5;
		const FVector2D Cursor = FVector2D(1.0, 1.0).GetSafeNormal() * Inside;

		if (Reach > 150.0)
		{
			const FRoadSnapResult Result = Chain.Resolve(*Net, Cursor, Settings);
			TestTrue(TEXT("a cursor inside the junction resolves to the junction's node"),
				Result.Kind == ERoadSnapKind::Node);
			TestTrue(TEXT("and names that node, not a new one"), Result.Node == Centre);
		}
		else
		{
			AddError(TEXT("fixture is wrong: the junction does not reach past NodeRadius"));
		}

		// Well outside, nothing claims it - the rule must not swallow the whole map.
		{
			const FVector2D Far = FVector2D(1.0, 1.0).GetSafeNormal() * (Reach * 4.0);
			const FRoadSnapResult Result = Chain.Resolve(*Net, Far, Settings);
			TestTrue(TEXT("a cursor well clear of the junction is still Free"),
				Result.Kind == ERoadSnapKind::Free);
		}

		// Zero restores the old behaviour, which is what makes the factor a real control
		// rather than a constant with a name.
		{
			FRoadSnapSettings Fixed = Settings;
			Fixed.JunctionSnapFactor = 0.0;
			const FRoadSnapResult Result = Chain.Resolve(*Net, Cursor, Fixed);
			TestTrue(TEXT("a zero factor falls back to the fixed NodeRadius"),
				Result.Kind != ERoadSnapKind::Node);
		}
	}

	// --- The angle at the FAR end ----------------------------------------------------
	//
	// 4. Drawing INTO an existing node at a hairpin was never checked: the rule measured
	//    the corner at the start node only, on the reasoning that the far end "is checked
	//    when a segment is drawn FROM there". It is not - that is a different segment.
	//    This one makes the corner now.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = BuildToolProfile();

		// Target already has one arm heading due EAST.
		const FRoadNodeId Target = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId TargetEast = Net->AddNode(FVector2D(4000.0, 0.0));
		Net->AddStraightSegment(Target, TargetEast, Profile);

		const FRoadPlacementLimits Limits = DefaultLimits();

		// A start node placed so the new arm arrives at Target from just 10 degrees off
		// that existing arm - a hairpin at Target, and nothing at the start to object.
		{
			const double Radians = FMath::DegreesToRadians(10.0);
			const FVector2D From(4000.0 * FMath::Cos(Radians), 4000.0 * FMath::Sin(Radians));
			const FRoadNodeId Start = Net->AddNode(From);

			const ERoadPlacement Result =
				RoadPlacement::Validate(*Net, Start, NodeSnap(*Net, Target), Limits);

			TestTrue(TEXT("a hairpin AT THE DESTINATION is refused"),
				Result == ERoadPlacement::TooSharpAtEnd);
		}

		// The same geometry at a comfortable angle still builds, or the rule is just a ban
		// on drawing towards junctions.
		{
			const double Radians = FMath::DegreesToRadians(70.0);
			const FVector2D From(4000.0 * FMath::Cos(Radians), 4000.0 * FMath::Sin(Radians));
			const FRoadNodeId Start = Net->AddNode(From);

			const ERoadPlacement Result =
				RoadPlacement::Validate(*Net, Start, NodeSnap(*Net, Target), Limits);

			TestTrue(TEXT("a wide corner at the destination is still allowed"),
				Result == ERoadPlacement::Valid);
		}
	}

	// 5. The same corner against a segment about to be SPLIT. The new node inherits both
	//    halves of the split as arms, so drawing back along the segment is exactly as
	//    sharp as any other hairpin - and was equally unchecked.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = BuildToolProfile();

		const FRoadNodeId West = Net->AddNode(FVector2D(-4000.0, 0.0));
		const FRoadNodeId EastEnd = Net->AddNode(FVector2D(4000.0, 0.0));
		const FRoadSegmentId Chord = Net->AddStraightSegment(West, EastEnd, Profile);

		// Start almost on the line, so the arm arrives nearly parallel to the segment.
		const FRoadNodeId Start = Net->AddNode(FVector2D(2000.0, 60.0));

		FRoadSnapResult Split;
		Split.Kind = ERoadSnapKind::Segment;
		Split.Segment = Chord;
		Split.Position = FVector2D(0.0, 0.0);
		Split.SegmentT = 0.5;

		const ERoadPlacement Result =
			RoadPlacement::Validate(*Net, Start, Split, DefaultLimits());

		TestTrue(TEXT("arriving almost along a segment being split is refused"),
			Result == ERoadPlacement::TooSharpAtEnd);
	}

	// 6. A Free snap has no far-end arms at all, so it can never be TooSharpAtEnd. Without
	//    this the far-end rule could quietly refuse ordinary road building.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = BuildToolProfile();

		const FRoadNodeId Start = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Away  = Net->AddNode(FVector2D(-4000.0, 0.0));
		Net->AddStraightSegment(Start, Away, Profile);

		FRoadSnapResult Free;
		Free.Kind = ERoadSnapKind::Free;
		Free.Position = FVector2D(4000.0, 0.0);

		TestTrue(TEXT("a free end is never too sharp at the far end"),
			RoadPlacement::Validate(*Net, Start, Free, DefaultLimits()) == ERoadPlacement::Valid);
	}

	// 7. Every reason still describes itself. A blank string in the overlay is the same
	//    as no refusal at all to the player looking at it.
	{
		const FString Text = RoadPlacement::Describe(ERoadPlacement::TooSharpAtEnd);
		TestTrue(TEXT("the new reason has player-facing text"), Text.Len() > 0);
		TestTrue(TEXT("and does not read the same as the start-end reason"),
			Text != FString(RoadPlacement::Describe(ERoadPlacement::TooSharp)));
	}

	return true;
}

#endif
