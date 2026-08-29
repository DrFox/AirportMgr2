#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEntityTest,
	"RoadNet.Model.Entity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEntityTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TestTrue(TEXT("a stand declares several anchors"), Stand->Anchors.Num() >= 4);

	// Placed at 5000,3000 facing +Y (90 degrees), so a local offset along local +X lands
	// along world +Y. The rotation matters: without it every anchor of every stand would
	// sit in the same relative direction regardless of how the stand faces.
	const FVector2D Where(5000.0, 3000.0);
	const double Facing = UE_DOUBLE_PI * 0.5;

	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Where, Facing);
	TestTrue(TEXT("the entity handle is set"), Placed.IsSet());

	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the entity resolves"), Instance))
		{
			return false;
		}

		TestEqual(TEXT("one resolved anchor per declared anchor"),
			Instance->ResolvedAnchors.Num(), Stand->Anchors.Num());

		// Every anchor became a real guideline node, at the anchor's WORLD pose.
		for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
		{
			const FGuidelineNodeId NodeId = Instance->ResolvedAnchors[Index];
			const FGuidelineNode* Node = Net->GetGuidelineNode(NodeId);
			if (!TestNotNull(TEXT("an anchor resolved to a live node"), Node))
			{
				continue;
			}

			const FEntityAnchor& Anchor = Stand->Anchors[Index];
			const double Cos = FMath::Cos(Facing);
			const double Sin = FMath::Sin(Facing);
			const FVector2D Expected(
				Where.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
				Where.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);

			TestTrue(TEXT("the node sits at the anchor's world pose"),
				Node->Position.Equals(Expected, 0.01));

			// THE PROPERTY THAT MAKES ANCHORS USABLE. A derived node is swept by the very
			// next rebuild once it is idle, and an anchor has no incident edges until
			// somebody draws a guideline to it - so a derived anchor node would not
			// survive its own placement.
			TestFalse(TEXT("an anchor node is NOT owned by the derivation"), Node->bDerived);
		}
	}

	// The block above recomputes the expected pose with the SAME trigonometry as the
	// implementation, so it restates the transform rather than verifying it: a transposed
	// rotation matrix - a rotation by minus the heading, invisible at heading 0 and wrong
	// everywhere else - would be reproduced identically in both and pass.
	//
	// This pins the rotation DIRECTION with plain arithmetic and no trig at all. The tug
	// anchor sits at local (1500, 0): straight ahead of the aircraft, nothing lateral. At
	// heading pi/2 the stand faces world +Y, so straight ahead must land 1500 uu NORTH of
	// it - where a transposed matrix would put it 1500 uu east.
	{
		const FEntityInstance* Rotated = Net->GetEntity(Placed);
		if (TestNotNull(TEXT("the entity resolves for the rotation check"), Rotated))
		{
			int32 TugIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
			{
				if (Stand->Anchors[Index].Role == EServiceRole::Tug)
				{
					TugIndex = Index;
				}
			}

			if (TestTrue(TEXT("the stand has a tug anchor"), TugIndex != INDEX_NONE))
			{
				// Straight ahead in local space, so rotation is the only thing under test.
				TestEqual(TEXT("the tug anchor is straight ahead, nothing lateral"),
					Stand->Anchors[TugIndex].LocalPosition.Y, 0.0);

				const FGuidelineNode* TugNode =
					Net->GetGuidelineNode(Rotated->ResolvedAnchors[TugIndex]);
				if (TestNotNull(TEXT("the tug anchor resolved"), TugNode))
				{
					const double Ahead = Stand->Anchors[TugIndex].LocalPosition.X;
					TestTrue(TEXT("facing +Y, straight ahead lands due north of the stand"),
						TugNode->Position.Equals(FVector2D(Where.X, Where.Y + Ahead), 0.01));
				}
			}
		}
	}

	// Anchors must be distinguishable by role, or "drive to the fuel position" has no
	// answer. Asserted on the definition and on the instance's parallel array together,
	// because the pairing is what callers rely on.
	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (TestNotNull(TEXT("the entity still resolves"), Instance))
		{
			int32 AircraftAnchors = 0;
			int32 FuelAnchors = 0;
			for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
			{
				if (Stand->Anchors[Index].Role == EServiceRole::Aircraft) { ++AircraftAnchors; }
				if (Stand->Anchors[Index].Role == EServiceRole::Fuel)     { ++FuelAnchors; }
			}
			TestEqual(TEXT("a stand has exactly one aircraft stop position"), AircraftAnchors, 1);
			TestEqual(TEXT("and one fuel position"), FuelAnchors, 1);
		}
	}

	// Removing the entity takes its anchor nodes with it, or the graph fills with nodes
	// nothing references and no route can reach.
	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		TArray<FGuidelineNodeId> Orphaned;
		if (TestNotNull(TEXT("the entity resolves before removal"), Instance))
		{
			Orphaned = Instance->ResolvedAnchors;
		}

		TestTrue(TEXT("the entity removes"), Net->RemoveEntity(Placed));
		TestNull(TEXT("a removed entity no longer resolves"), Net->GetEntity(Placed));

		for (const FGuidelineNodeId NodeId : Orphaned)
		{
			TestNull(TEXT("its anchor nodes went with it"), Net->GetGuidelineNode(NodeId));
		}
	}

	// SPEC RISK 4, asserted. FRoadGuidelineBuilder::Build destroys and re-adds every
	// DERIVED node on each run, advancing its generation - so anything holding a derived
	// node's handle across a rebuild finds it dangling. An entity's anchors are exactly
	// that: handles, stored, and expected to outlive edits elsewhere on the airport.
	//
	// The answer is provenance, not a rewrite. Anchor nodes are created non-derived, the
	// orphan sweep requires bDerived, so Build cannot touch them.
	{
		URoadNetwork* Live = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(800.0, 200.0);
		UEntityDefinition* Gate = UEntityDefinition::MakeStandTransient();

		// A taxiway network the builder will churn, and a stand parked well away from it.
		const FRoadNodeId Hub  = Live->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Live->AddNode(FVector2D(12000.0, 0.0));
		const FRoadNodeId Nrth = Live->AddNode(FVector2D(0.0, 12000.0));
		Live->AddStraightSegment(Hub, East, Taxi);
		Live->AddStraightSegment(Hub, Nrth, Taxi);

		const FEntityInstanceId Gate12 =
			Live->PlaceEntity(Gate, FVector2D(30000.0, 30000.0), 0.0);

		TArray<FGuidelineNodeId> Before;
		if (const FEntityInstance* Instance = Live->GetEntity(Gate12))
		{
			Before = Instance->ResolvedAnchors;
		}
		TestTrue(TEXT("the stand resolved its anchors"), Before.Num() > 0);

		// Now churn the graph. Twice, because the first Build has nothing to clear.
		const FRoadSolveResult LiveSolved = FRoadNetworkSolver::SolveAll(*Live);
		FRoadGuidelineBuilder::Build(*Live, LiveSolved);
		FRoadGuidelineBuilder::Build(*Live, LiveSolved);

		const FEntityInstance* After = Live->GetEntity(Gate12);
		if (TestNotNull(TEXT("the stand survives a rebuild"), After))
		{
			TestEqual(TEXT("with the same anchor count"), After->ResolvedAnchors.Num(), Before.Num());

			for (int32 Index = 0; Index < Before.Num(); ++Index)
			{
				// Handle identity, generation included - not just "a node is still there".
				// A swept-and-recreated node would have the same index and a DIFFERENT
				// generation, which is exactly the dangle this guards against.
				TestTrue(TEXT("the anchor handle is unchanged, generation included"),
					After->ResolvedAnchors[Index] == Before[Index]);

				TestNotNull(TEXT("and still resolves to a live node"),
					Live->GetGuidelineNode(Before[Index]));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
