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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
