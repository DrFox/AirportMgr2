#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Entities/EntityDefinition.h"
#include "Entities/PlotModuleKit.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The shipped fuel depot matches the code that is supposed to author it.
 *
 * DA_FuelDepot IS BUILT BY BuildFuelDepot, through the commandlet, so the asset on disk and
 * MakeFuelDepotTransient should be the same object twice. They drift the moment somebody adds
 * a field to the definition and does not re-run the authoring script - and the asset keeps
 * whatever the property DEFAULTS to, silently.
 *
 * THAT IS NOT HYPOTHETICAL. EPlotLayout arrived on 2026-09-20 defaulting to Scatter so an
 * un-migrated definition would keep its old behaviour. DA_FuelDepot was never re-authored, so
 * the tool previewed a banded yard from its own hardcoded kind while the BUILT depot read
 * Scatter off the asset and scattered - the exact preview-versus-built split the whole
 * reservation design exists to prevent, shipped by the very default meant to be safe.
 *
 * IT COMPARES THE WHOLE FIELD, not just the layout: a test that named the field that bit us
 * would catch that field and no other, which is the failure AircraftLookTest exists for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelDepotAssetMatchesItsBuilderTest,
	"AirportMgr.Content.FuelDepotAssetMatchesItsBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelDepotAssetMatchesItsBuilderTest::RunTest(const FString& Parameters)
{
	const UEntityDefinition* Shipped = Cast<UEntityDefinition>(
		StaticLoadObject(UEntityDefinition::StaticClass(), nullptr,
			TEXT("/Game/Entities/DA_FuelDepot")));
	if (!TestNotNull(TEXT("DA_FuelDepot is on disk"), Shipped)) { return false; }

	const UEntityDefinition* Fresh = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("and BuildFuelDepot still builds one"), Fresh)) { return false; }

	TestEqual(TEXT("the shipped depot lays out the way BuildFuelDepot says"),
		static_cast<int32>(Shipped->Layout), static_cast<int32>(Fresh->Layout));
	TestEqual(TEXT("and takes the same pose role"),
		static_cast<int32>(Shipped->PoseRole), static_cast<int32>(Fresh->PoseRole));
	TestEqual(TEXT("and the same footprint"),
		Shipped->FootprintExtent, Fresh->FootprintExtent);
	TestEqual(TEXT("and the same anchor count"),
		Shipped->Anchors.Num(), Fresh->Anchors.Num());
	TestEqual(TEXT("and the same service bay count"),
		Shipped->ServiceBays.Num(), Fresh->ServiceBays.Num());

	return true;
}

/**
 * Every module has a kit, no two kits share a mesh, and a baked kit's variants are dense.
 *
 * IT WALKS THE ENUM AND THE REGISTRY, not a list written here - the lesson AircraftLookTest
 * paid for. Its first version compared a hand-written PAIR and passed while the A320 and the
 * 737 both still wore the default mesh, because a test that names its subjects can only ever
 * catch the subjects somebody remembered. CLAUDE.md names that failure three times over:
 * check where a list is CONSUMED, not where it is declared.
 *
 * In the game module because these are /Game assets - Airside may not reach them, and
 * Check-Architecture enforces that direction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotKitContentTest,
	"AirportMgr.Content.EveryDepotModuleHasItsOwnKit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotKitContentTest::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Registry.Get().SearchAllAssets(true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UAirsideContent::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Found;
	Registry.Get().GetAssets(Filter, Found);

	// A CONTENT ASSET MUST EXIST. Zero of them would make every loop below vacuous and this
	// test would pass on a project with no depot content at all - which is precisely the
	// state it is here to notice.
	if (!TestTrue(TEXT("the project has an Airside content asset"), Found.Num() > 0))
	{
		return false;
	}

	for (const FAssetData& Data : Found)
	{
		const UAirsideContent* Content = Cast<UAirsideContent>(Data.GetAsset());
		if (Content == nullptr)
		{
			continue;
		}

		const FString Where = Data.AssetName.ToString();
		TSet<FString> MeshPaths;

		// EVERY VALUE OF THE ENUM, so a module added without a kit fails here rather than
		// silently falling back to a grey box in a shipped build.
		for (int32 Raw = 0; Raw <= static_cast<int32>(EDepotModule::Pump); ++Raw)
		{
			const EDepotModule Module = static_cast<EDepotModule>(Raw);
			const TObjectPtr<UPlotModuleKit>* Slot = Content->DepotKits.Find(Module);

			if (!TestTrue(*FString::Printf(TEXT("%s maps module %d to a kit"), *Where, Raw),
				Slot != nullptr && *Slot != nullptr))
			{
				continue;
			}

			const UPlotModuleKit* Kit = *Slot;

			TestTrue(*FString::Printf(TEXT("module %d's kit has a footprint"), Raw),
				Kit->Footprint.X > 0.0 && Kit->Footprint.Y > 0.0);
			TestTrue(*FString::Printf(TEXT("module %d's run cap is at least one"), Raw),
				Kit->RunCap >= 1);
			TestTrue(*FString::Printf(TEXT("module %d's weight is at least one"), Raw),
				Kit->ReserveWeight >= 1);

			if (Kit->Assembly != EKitAssembly::Baked)
			{
				continue;
			}

			// EMPTY IS LEGAL and means grey box - the meshes are a later slice, and the
			// runtime falls back rather than drawing nothing.
			if (Kit->BakedMeshes.Num() == 0)
			{
				continue;
			}

			// DENSE UP TO RunCap: the presenter indexes BakedMeshes by bay count, so a hole
			// is a run the player can buy and cannot see.
			TestEqual(*FString::Printf(TEXT("module %d has one mesh per bay count"), Raw),
				Kit->BakedMeshes.Num(), Kit->RunCap);

			for (const TSoftObjectPtr<UStaticMesh>& Mesh : Kit->BakedMeshes)
			{
				const FString Path = Mesh.ToString();
				if (!TestFalse(
					*FString::Printf(TEXT("module %d's baked variant is not null"), Raw),
					Path.IsEmpty()))
				{
					continue;
				}
				TestFalse(*FString::Printf(TEXT("no two kits share mesh %s"), *Path),
					MeshPaths.Contains(Path));
				MeshPaths.Add(Path);
			}
		}
	}

	return true;
}

/**
 * Every kit that has meshes has a footprint its meshes actually fill - the drift
 * UPlotModuleKit's own comment names: the footprint and the mesh are two statements of one
 * dimension, and they disagree the first time a model is rescaled.
 *
 * MEASURED FROM THE MESH'S BOUNDS turned into the kit's frame by MeshYawDeg, exactly the frame
 * UPlotPresenter lays pieces out in, so a wrong yaw fails here as a footprint swapped end for
 * end rather than in PIE as a shed standing sideways.
 *
 * FOUR UNITS OF SLACK, not zero, and measured rather than chosen: GetBoundingBox on an asset
 * loaded from disk returns its RENDER bounds, which on the curved tank read 223.2 x 480.9 uu
 * against vertices of exactly 220 x 480 (2026-09-22) - while the flat shed pieces read exact.
 * The presenter lays out with the same render bounds, so that is what is measured here; the
 * import script checks the vertices themselves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitMeshesMatchTheirFootprintsTest,
	"AirportMgr.Content.DepotKitMeshesMatchTheirFootprints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitMeshesMatchTheirFootprintsTest::RunTest(const FString& Parameters)
{
	const UAirsideContent* Content = Cast<UAirsideContent>(StaticLoadObject(
		UAirsideContent::StaticClass(), nullptr, TEXT("/Game/DA_AirsideContent")));
	if (!TestNotNull(TEXT("DA_AirsideContent is on disk"), Content)) { return false; }

	constexpr double SlackUu = 4.0;

	// The mesh's plan extent in the kit's frame: a quarter turn swaps its X and Y.
	auto KitExtent = [](const UStaticMesh& Mesh, double YawDeg)
	{
		const FVector Size = Mesh.GetBoundingBox().GetSize();
		const bool bQuarter = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(YawDeg))) > 0.5;
		return bQuarter ? FVector2D(Size.Y, Size.X) : FVector2D(Size.X, Size.Y);
	};

	int32 Measured = 0;
	for (const TPair<EDepotModule, TObjectPtr<UPlotModuleKit>>& Entry : Content->DepotKits)
	{
		const UPlotModuleKit* Kit = Entry.Value;
		if (Kit == nullptr)
		{
			continue;
		}
		const FString Name = Kit->GetName();

		TestTrue(*FString::Printf(TEXT("%s's mesh yaw is a quarter turn, so its bounds stay a box in the kit's frame"), *Name),
			FMath::IsNearlyZero(FMath::Fmod(Kit->MeshYawDeg, 90.0)));

		if (Kit->Assembly == EKitAssembly::Parts)
		{
			const UStaticMesh* Bay = Kit->PartBayMesh.LoadSynchronous();
			const UStaticMesh* Cap = Kit->PartCapMesh.LoadSynchronous();
			if (!TestTrue(*FString::Printf(TEXT("%s is Parts and names both a bay and a cap"), *Name),
				Bay != nullptr && Cap != nullptr))
			{
				continue;
			}
			const FVector2D BayExtent = KitExtent(*Bay, Kit->MeshYawDeg);
			const FVector2D CapExtent = KitExtent(*Cap, Kit->MeshYawDeg);
			TestNearlyEqual(*FString::Printf(TEXT("%s's bay is as deep as its footprint"), *Name),
				BayExtent.X, Kit->Footprint.X, SlackUu);
			TestNearlyEqual(*FString::Printf(TEXT("%s's bay is as wide as one module - the pitch the run steps by"), *Name),
				BayExtent.Y, Kit->Footprint.Y, SlackUu);
			TestNearlyEqual(*FString::Printf(TEXT("%s's cap reaches PartCapUu, the ground the solver reserves for it"), *Name),
				CapExtent.Y, Kit->PartCapUu, SlackUu);
			// AS DEEP AS THE BAY, not merely no deeper: the far cap is this one turned half a
			// turn and centred on its own depth, which lines up with the bay only when the two
			// span the same depth - the bounds half of "symmetric front to back" (SPEC.md).
			TestNearlyEqual(*FString::Printf(TEXT("%s's cap spans the bay's whole depth, so it turns into place"), *Name),
				CapExtent.X, BayExtent.X, SlackUu);
			++Measured;
			continue;
		}

		for (int32 Index = 0; Index < Kit->BakedMeshes.Num(); ++Index)
		{
			const UStaticMesh* Mesh = Kit->BakedMeshes[Index].LoadSynchronous();
			if (!TestNotNull(*FString::Printf(TEXT("%s's baked mesh %d loads"), *Name, Index), Mesh))
			{
				continue;
			}
			const FVector2D Extent = KitExtent(*Mesh, Kit->MeshYawDeg);
			TestNearlyEqual(*FString::Printf(TEXT("%s's %d-bay mesh is as deep as its footprint"), *Name, Index + 1),
				Extent.X, Kit->Footprint.X, SlackUu);
			TestNearlyEqual(*FString::Printf(TEXT("%s's %d-bay mesh is as wide as that many modules"), *Name, Index + 1),
				Extent.Y, Kit->Footprint.Y * (Index + 1), SlackUu);
			++Measured;
		}
	}

	// NOT VACUOUS: the shed and the tank ship meshes since 2026-09-22, and a content set
	// that lost them would pass every check above by having nothing to check.
	TestTrue(TEXT("at least two kits have meshes to measure"), Measured >= 2);
	return true;
}

/**
 * A depot placed through the real actors draws the shipped kit's meshes, and every built
 * piece stands inside the ground its module reserved.
 *
 * THE SEAM TEST for AAirsideBuildingsActor handing UAirsideSettings::ResolveDepotLooks to
 * the presenter. Airside.Present.PlotPresenterAssemblesAShedFromParts drives the presenter
 * with stand-ins and would pass with that call deleted; this is the one that would not.
 *
 * INSIDE ITS ENVELOPE is what catches a wrong MeshYawDeg: the shed is 9.4 m deep and 5 m per
 * bay, so a quarter turn too few stands it across the ground it reserved and its corners
 * leave the envelope by metres.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotDrawsItsShippedMeshesTest,
	"AirportMgr.Content.DepotDrawsItsShippedMeshes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotDrawsItsShippedMeshesTest::RunTest(const FString& Parameters)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestNotNull(TEXT("the project content set"), Content)) { return false; }
	const TObjectPtr<UPlotModuleKit>* ShedSlot = Content->DepotKits.Find(EDepotModule::Shed);
	const UPlotModuleKit* ShedKit = ShedSlot != nullptr ? ShedSlot->Get() : nullptr;
	if (!TestTrue(TEXT("the shed kit is assembled from parts"),
		ShedKit != nullptr && ShedKit->Assembly == EKitAssembly::Parts)) { return false; }
	UStaticMesh* Bay = ShedKit->PartBayMesh.LoadSynchronous();
	UStaticMesh* Cap = ShedKit->PartCapMesh.LoadSynchronous();
	if (!TestTrue(TEXT("and names both pieces"), Bay != nullptr && Cap != nullptr)) { return false; }

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestTrue(TEXT("a road network, its presenter and a depot"),
		Actor != nullptr && Plots != nullptr && Depot != nullptr)) { return false; }

	// A 30 x 30 m plot: room for a whole shed run at the real 9.4 x 5 m bay, plus the rest.
	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D(1500.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0),
	                      FVector2D(3000.0, 3000.0), FVector2D(0.0, 3000.0) };
	Placement.Modules = { EDepotModule::Shed };
	Actor->ClearNetwork();
	Actor->Network->PlaceEntity(Placement);
	Actor->RebuildMesh();

	if (!TestEqual(TEXT("the shed bay was built"), Plots->GetModuleCount(), 1)) { return false; }
	TestEqual(TEXT("drawn as the kit's bay"), Plots->GetMeshInstanceCountForTest(Bay, false), 1);
	TestEqual(TEXT("closed by the kit's cap at both ends"), Plots->GetMeshInstanceCountForTest(Cap, false), 2);

	FTransform Envelope;
	if (!TestTrue(TEXT("the envelope reads back"), Plots->GetInstanceTransformForTest(0, Envelope)))
	{
		return false;
	}

	// Every corner of every built piece, in the envelope's unit-cube space: the grey box is the
	// engine cube, 100 uu and centred, scaled to the footprint, so inside is |x|,|y| <= 50.
	constexpr double SlackUu = 2.0;
	const FVector EnvelopeScale = Envelope.GetScale3D();
	auto Inside = [&](UStaticMesh* Mesh, int32 Index)
	{
		FTransform Piece;
		if (!Plots->GetMeshInstanceTransformForTest(Mesh, false, Index, Piece))
		{
			return false;
		}
		const FBox Bounds = Mesh->GetBoundingBox();
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local((Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
				(Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
			const FVector InEnvelope = Envelope.InverseTransformPosition(Piece.TransformPosition(Local));
			if (FMath::Abs(InEnvelope.X) > 50.0 + SlackUu / EnvelopeScale.X
				|| FMath::Abs(InEnvelope.Y) > 50.0 + SlackUu / EnvelopeScale.Y)
			{
				AddInfo(FString::Printf(TEXT("%s instance %d corner %d at envelope-local (%.1f, %.1f)"),
					*Mesh->GetName(), Index, Corner, InEnvelope.X, InEnvelope.Y));
				return false;
			}
		}
		return true;
	};
	TestTrue(TEXT("the bay stands inside the ground its module reserved"), Inside(Bay, 0));
	TestTrue(TEXT("so does the near cap"), Inside(Cap, 0));
	TestTrue(TEXT("and the far cap, turned half a turn"), Inside(Cap, 1));
	return true;
}

#endif
