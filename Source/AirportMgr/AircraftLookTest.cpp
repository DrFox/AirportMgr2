#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * No two aircraft an airline can OFFER wear the same mesh.
 *
 * THE BUG THIS EXISTS FOR, reported from play: "I just had a Twin Otter offered, it was a
 * Piper Meridian that spawned." The offer, the name, the performance figures and the refusal
 * reasons were all correctly the Twin Otter's - only the aeroplane on the runway was not,
 * because UAircraftType carried no mesh and UAirsideTraffic handed every agent
 * UAirsideContent::AgentMesh, one skeletal mesh for the whole game.
 *
 * IT WALKS THE ASSET REGISTRY rather than a list written here, and that is the second
 * lesson from the same report. The first version of this test compared the Piper against
 * plane2 - a hand-written PAIR - and passed while the A320 and the 737 both still wore the
 * default, which is exactly the defect it was written to prevent. A test that names its
 * subjects can only ever catch the subjects somebody remembered, and CLAUDE.md names that
 * failure three times over: check where a list is CONSUMED, not where it is declared.
 *
 * In the game module because these are /Game assets - Airside may not reach them, and
 * Check-Architecture enforces that direction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAircraftLookTest,
	"AirportMgr.Content.AircraftTypesDoNotShareOneMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAircraftLookTest::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Registry.Get().SearchAllAssets(true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UAirlineDefinition::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Found;
	Registry.Get().GetAssets(Filter, Found);

	if (Found.Num() == 0)
	{
		// A fresh checkout that has not run the authoring scripts has none; failing then
		// would fail the suite for want of content rather than for a defect.
		AddInfo(TEXT("No UAirlineDefinition assets found; look not checked"));
		return true;
	}

	// THE FLEETS, not every UAircraftType asset on disk. A type nothing flies cannot arrive,
	// so it cannot land looking like the wrong aeroplane - and the A320 and 737 are exactly
	// that today: authored, modelless, and deliberately in no fleet until a jet model
	// exists. Scoping to what can actually be OFFERED is what makes this assertion mean
	// "the player will never see the wrong aircraft" rather than "every asset is tidy".
	TSet<UAircraftType*> Offerable;
	for (const FAssetData& Data : Found)
	{
		const UAirlineDefinition* Airline = Cast<UAirlineDefinition>(Data.GetAsset());
		if (Airline == nullptr)
		{
			continue;
		}
		for (const TObjectPtr<UAircraftType>& Type : Airline->Fleet)
		{
			if (Type != nullptr)
			{
				Offerable.Add(Type);
			}
		}
	}

	if (Offerable.Num() == 0)
	{
		AddInfo(TEXT("No airline flies anything; look not checked"));
		return true;
	}

	// Mesh path -> the types wearing it. Built from what each type's AIRFRAME resolves to,
	// not from the type's own property: UFlight flattens the type into an FAirframe and the
	// agent carries no pointer back, so the airframe is what actually reaches the spawn. A
	// check of the property would pass while Airframe() forgot to copy it.
	TMap<FString, TArray<FString>> ByMesh;
	TArray<FString> Meshless;

	for (UAircraftType* Type : Offerable)
	{
		const FString Name = Type->GetName();
		const FAirframe Airframe = Type->Airframe();

		TestFalse(*FString::Printf(TEXT("%s publishes a short code, so it can be named"), *Name),
			Airframe.TypeCode.IsNone());

		if (Airframe.Mesh.IsNull())
		{
			Meshless.Add(Name);
			continue;
		}
		ByMesh.FindOrAdd(Airframe.Mesh.ToString()).Add(Name);
	}

	// THE ASSERTION THAT MATTERS. Two types resolving to one mesh is the defect itself, and
	// it is invisible from any single type: each looks correctly configured on its own.
	for (const TPair<FString, TArray<FString>>& Pair : ByMesh)
	{
		TestTrue(*FString::Printf(TEXT("%s is worn by exactly one type, not by %s"),
			*FPaths::GetBaseFilename(Pair.Key), *FString::Join(Pair.Value, TEXT(", "))),
			Pair.Value.Num() == 1);
	}

	// A type with no mesh falls back to UAirsideContent::AgentMesh - so two of THOSE are two
	// aircraft that will land looking identical, by the same mechanism, just one level down.
	TestTrue(*FString::Printf(
		TEXT("at most one type falls back to the game-wide mesh; these do: %s"),
		Meshless.Num() > 0 ? *FString::Join(Meshless, TEXT(", ")) : TEXT("none")),
		Meshless.Num() <= 1);

	AddInfo(FString::Printf(
		TEXT("%d offerable type(s) across %d airline(s): %d with their own mesh, %d falling back"),
		Offerable.Num(), Found.Num(), ByMesh.Num(), Meshless.Num()));
	return true;
}

#endif
