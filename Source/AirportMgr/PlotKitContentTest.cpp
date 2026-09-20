#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Content/AirsideContent.h"
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Entities/EntityDefinition.h"
#include "Entities/PlotModuleKit.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

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

#endif
