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
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/MemoryReader.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadEditFacade.h"
#include "Model/RoadNetwork.h"
#include "Model/LandGrid.h"

#if WITH_DEV_AUTOMATION_TESTS


namespace
{
	FLandGrid OlStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D(-100000.0, -300000.0), 60000.0, 8, 8, Start);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandUndoKeepsLand, "Airside.Present.OwnedLand.UndoKeepsLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandUndoKeepsLand::RunTest(const FString&)
{
	// SPEC 3.1: undo is a Memento of the whole network, so without the carry an undo would hand back land
	// the player paid for while keeping the money (or, refunded, make land free to try).
	FAirsideTestWorld Fixture;
	URoadEditFacade* Facade = Fixture.Actor->GetEditFacade();
	int32 Heard = 0;
	Facade->OnOwnedLandChanged.AddLambda([&Heard](const FLandGrid&) { ++Heard; });
	Facade->AuthorOwnedLand(OlStart());
	TestEqual(TEXT("authoring announces"), Heard, 1);

	const int32 A = Fixture.Actor->PlaceNode(FVector2D(-90000.0, -100000.0));
	const int32 B = Fixture.Actor->PlaceNode(FVector2D(-90000.0, -60000.0));
	TestTrue(TEXT("a road is drawn"), Fixture.Actor->ConnectNodes(A, B, ERoadKind::Taxiway));

	FLandGrid Grown = Fixture.Actor->Network->GetOwnedLand();
	Grown.SetTileOwned(FIntPoint(1, 3), true);
	Facade->AuthorOwnedLand(Grown);   // stands in for a purchase until Task 10

	TestTrue(TEXT("undo of the road succeeds"), Facade->Undo());
	TestTrue(TEXT("the tile bought after the road is STILL owned"), Fixture.Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	TestTrue(TEXT("redo too"), Facade->Redo());
	TestTrue(TEXT("still owned after redo"), Fixture.Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	Facade->ClearNetwork();
	TestEqual(TEXT("clearing the airport keeps the land"), Fixture.Actor->Network->GetOwnedLand().NumOwned(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandSaveRoundTrips, "Airside.Present.OwnedLand.SaveRoundTripsLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandSaveRoundTrips::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(OlStart());
	// THROUGH THE ARCHIVE THE SAVE USES (OpsSave::SerializeObject's base): a bare FMemoryWriter cannot write the
	// network's object references and crashes - it is not what a save does.
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
	FObjectAndNameAsStringProxyArchive Out(Writer, /*bInLoadIfFindFails*/ false);
	Fixture.Actor->Network->Serialize(Out);

	URoadNetwork* Loaded = NewObject<URoadNetwork>();
	FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
	FObjectAndNameAsStringProxyArchive In(Reader, /*bInLoadIfFindFails*/ true);
	Loaded->Serialize(In);
	TestEqual(TEXT("the mask survives the network's own serialisation (the save's Network blob)"),
		Loaded->GetOwnedLand().Owned, OlStart().Owned);
	TestEqual(TEXT("and the origin"), Loaded->GetOwnedLand().Origin, OlStart().Origin);
	return true;
}


/**
 * THE WALLS FOLLOW THE TILES, AT ONCE (land purchase spec 4). THE SEAM: the actor must hear OnOwnedLandChanged -
 * unwired, the walls stay on the old outline, a cut through owned ground and a void where land was bought. Each
 * wall's outer face is measured on its run from the component's transform, not its bounds: a fresh checkout's
 * content set may name no wall mesh, and the PLACEMENT is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandWallsFollowAPurchase, "Airside.Present.OwnedLand.WallsFollowAPurchase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandWallsFollowAPurchase::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	TestNull(TEXT("a level with no owned-land actor finds none"), AAirsideOwnedLandActor::Find(Fixture.World));
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(OlStart());
	AAirsideOwnedLandActor* Edge = Fixture.World->SpawnActor<AAirsideOwnedLandActor>(FVector(0.0, 0.0, 250.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("edge spawned"), Edge))
	{
		return false;
	}
	TestTrue(TEXT("Find returns the level's edge"), AAirsideOwnedLandActor::Find(Fixture.World) == Edge);
	Edge->DispatchBeginPlay();   // binds to the airport, as play does
	TestEqual(TEXT("a 1x2 has four walls"), Edge->NumWalls(), 4);

	FLandGrid Grown = Fixture.Actor->Network->GetOwnedLand();
	Grown.SetTileOwned(FIntPoint(1, 3), true);
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(Grown);
	if (!TestEqual(TEXT("an L has six walls, at once"), Edge->NumWalls(), 6))
	{
		return false;
	}
	const TArray<FLandEdgeRun> Runs = Grown.Outline();
	for (int32 I = 0; I < Edge->NumWalls(); ++I)
	{
		const UStaticMeshComponent* Wall = Edge->GetWalls()[I];
		const FVector2D Centre(Wall->GetComponentLocation());
		// Half the default WallThickness (100) out along the run's normal is the outer face.
		const FVector2D Outer = Centre + Runs[I].Outward * 50.0;
		const FVector2D Mid = (Runs[I].A + Runs[I].B) * 0.5;
		TestTrue(*FString::Printf(TEXT("wall %d's outer face is on its run"), I), Outer.Equals(Mid, 1e-6));
		// The actor's Z is the ground: tops 5 uu under it, so the clipped landscape covers them without z-fighting.
		const double Top = Wall->GetComponentLocation().Z + Wall->GetComponentScale().Z * 50.0;
		TestEqual(*FString::Printf(TEXT("wall %d's top sits 5 uu under the ground"), I), Top, 245.0, 1e-6);
	}
	return true;
}

/**
 * THE CLIP IS WIRED IN CONTENT. The ground clip lives entirely in assets - the collection, M_Ground's
 * reading of it, the masked instance - so a build script that wrote nothing would leave the walls
 * standing around an unclipped landscape, and only the saved assets can show that. NAMES, not a count
 * (CLAUDE.md "lists that must agree"): airside_matnodes.OWNED_LAND_PARAMS is the other half of
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
		// "Everything" by default: LandValid 0 clips nothing - a map whose airport owns no grid, or a frame before
		// any edge has written the collection.
		if (Name == AAirsideOwnedLandActor::CollectionParams[0])
		{
			TestEqual(TEXT("LandValid defaults to 0 - owns everything"), Default, 0.0f);
		}
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
