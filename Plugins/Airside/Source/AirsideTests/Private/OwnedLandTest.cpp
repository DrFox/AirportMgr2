#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Components/StaticMeshComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialCachedData.h"
#include "Misc/AutomationTest.h"
#include "Present/AirsideOwnedLandActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE WALLS STAND UNDER THE EDGE, and the level can find its land (2026-10-02). The outer face of
 * each wall must sit exactly on the rectangle's side - that is where the ground clip cuts - or the
 * strata show a gap of void, or poke out past the grass. Measured from the components' transforms
 * rather than their bounds, because a fresh checkout's content set may name no wall mesh yet and
 * an empty mesh has no bounds - and the PLACEMENT is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandWallsStandUnderTheEdge, "Airside.Present.OwnedLand.WallsStandUnderTheEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandWallsStandUnderTheEdge::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	TestNull(TEXT("a level with no owned-land actor finds none - and so owns everything"),
		AAirsideOwnedLandActor::Find(Fixture.World));

	AAirsideOwnedLandActor* Land = Fixture.World->SpawnActor<AAirsideOwnedLandActor>(FVector(0.0, 0.0, 250.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("owned land spawned"), Land))
	{
		return false;
	}
	TestTrue(TEXT("Find returns the level's owned land"), AAirsideOwnedLandActor::Find(Fixture.World) == Land);

	const FVector2D Min(-12000.0, -50000.0), Max(40000.0, 13000.0);
	Land->PlinthDepth = 4000.0;
	Land->WallThickness = 100.0;
	Land->SetOwnedLand(Min, Max);
	const FBox2D Owned = Land->GetOwnedLand();
	TestTrue(TEXT("the rectangle is valid"), Owned.bIsValid);

	const TArray<TObjectPtr<UStaticMeshComponent>>& Walls = Land->GetWalls();
	if (!TestEqual(TEXT("four walls"), Walls.Num(), 4))
	{
		return false;
	}
	// Each wall as a world box: centre +/- half its scaled 100 uu cube.
	auto BoxOf = [](const UStaticMeshComponent* Wall)
	{
		const FVector Half = Wall->GetComponentScale() * 50.0;
		const FVector At = Wall->GetComponentLocation();
		return FBox(At - Half, At + Half);
	};
	const FBox South = BoxOf(Walls[0]), North = BoxOf(Walls[1]), West = BoxOf(Walls[2]), East = BoxOf(Walls[3]);
	TestEqual(TEXT("south wall's outer face is the south edge"), South.Min.Y, Min.Y, 1e-6);
	TestEqual(TEXT("north wall's outer face is the north edge"), North.Max.Y, Max.Y, 1e-6);
	TestEqual(TEXT("west wall's outer face is the west edge"), West.Min.X, Min.X, 1e-6);
	TestEqual(TEXT("east wall's outer face is the east edge"), East.Max.X, Max.X, 1e-6);
	TestEqual(TEXT("south wall spans the whole side"), South.Max.X - South.Min.X, Max.X - Min.X, 1e-6);
	TestEqual(TEXT("west wall spans the whole side"), West.Max.Y - West.Min.Y, Max.Y - Min.Y, 1e-6);
	// The actor's Z is the ground: tops just under it, so the clipped landscape covers them without z-fighting.
	TestEqual(TEXT("wall tops sit 5 uu under the ground"), South.Max.Z, 245.0, 1e-6);
	TestEqual(TEXT("walls reach PlinthDepth under that"), South.Max.Z - South.Min.Z, 4000.0, 1e-6);

	// MOVE THE EDGE - land purchase's path. The walls follow at once, not on the next construction.
	Land->SetOwnedLand(Min, FVector2D(60000.0, Max.Y));
	TestEqual(TEXT("the east wall followed the grown edge"), BoxOf(Walls[3]).Max.X, 60000.0, 1e-6);

	Land->SetOwnedLand(Max, Min);
	TestFalse(TEXT("Min not below Max is no rectangle - owns everything, never a negative one"),
		Land->GetOwnedLand().bIsValid);
	return true;
}

/**
 * THE CLIP IS WIRED IN CONTENT. The ground clip lives entirely in assets - the collection, M_Ground's
 * reading of it, the masked instance - so a build script that wrote nothing would leave the walls
 * standing around an unclipped landscape, and only the saved assets can show that. NAMES, not a count
 * (CLAUDE.md "lists that must agree"): airside_matnodes.OWNED_RECT_PARAMS is the other half of
 * AAirsideOwnedLandActor::CollectionParams.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandWiredTest, "Airside.Content.OwnedLandWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandWiredTest::RunTest(const FString&)
{
	const FOwnedLandKit Kit = UAirsideSettings::ResolveOwnedLandKit();
	if (!TestNotNull(TEXT("the content set names an owned-land collection"), Kit.Collection))
	{
		return false;
	}
	TestNotNull(TEXT("and a plinth wall mesh"), Kit.WallMesh);
	TestNotNull(TEXT("and a plinth wall material"), Kit.WallMaterial);
	for (const FName& Name : AAirsideOwnedLandActor::CollectionParams)
	{
		bool bFound = false;
		const float Default = Kit.Collection->GetScalarParameterDefaultValue(Name, bFound);
		TestTrue(*FString::Printf(TEXT("the collection carries '%s'"), *Name.ToString()), bFound);
		// "Everything" by default: a map with no owned-land actor must clip nothing.
		TestTrue(*FString::Printf(TEXT("'%s' defaults to owning everything (%g)"), *Name.ToString(), Default),
			FMath::Abs(Default) >= 1.0e8f);
	}

	const UMaterialInterface* Ground = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Environment/M_Ground.M_Ground"));
	if (!TestNotNull(TEXT("M_Ground loads"), Ground))
	{
		return false;
	}
	bool bReads = false;
	for (const FMaterialParameterCollectionInfo& Info : Ground->GetCachedExpressionData().ParameterCollectionInfos)
	{
		bReads |= Info.ParameterCollection == Kit.Collection;
	}
	TestTrue(TEXT("M_Ground reads the owned-land collection"), bReads);
	TestEqual(TEXT("M_Ground stays Opaque, so maps that do not opt in pay nothing"),
		Ground->GetBlendMode(), EBlendMode::BLEND_Opaque);
	return true;
}

#endif
