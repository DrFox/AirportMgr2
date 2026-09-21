#include "AnimYardCatalogue.h"

#include "AnimYard.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Content/AirsideSettings.h"
#include "Engine/SkeletalMesh.h"
#include "Entities/AircraftType.h"
#include "Model/Airframe.h"

bool AnimYardCatalogue::FindRigFor(USkeletalMesh* Mesh, FYardRig& OutRig)
{
	if (Mesh == nullptr)
	{
		return false;
	}

	// THE VEHICLE FIRST, because it is one comparison against an already-resolved pair and
	// costs nothing, where the aircraft branch below loads every type asset in the project.
	const FResolvedAgentView Vehicle = UAirsideSettings::ResolveVehicleView();
	if (Vehicle.Mesh == Mesh)
	{
		if (Vehicle.AnimClass == nullptr)
		{
			// Rigged content with no graph to drive it. Reported as undrivable rather than
			// dressed with a null class, so the yard labels it instead of showing a model that
			// stands still for a reason nobody can see.
			return false;
		}

		OutRig.AnimClass = Vehicle.AnimClass;
		OutRig.bIsVehicle = true;

		// GEAR LEFT UNSET. A truck has none, and FGearPerformance::IsSet() being false is what
		// makes FYardMotion::ToAgentMotion leave its gear fractions at the resting pose.
		OutRig.Gear = FGearPerformance();

		// BoxSizeUu IS LEFT AT ITS DEFAULT and never read: ARoadAgentActor::SetVehicleAirframe
		// uses it only when Mesh is null, and this branch has just established it is not.
		return true;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();

	// THE SCAN IS FINISHED BEFORE IT IS READ. In the editor the registry is long since
	// populated and this returns at once; in a commandlet, or early in a cooked run, an
	// unwaited registry answers with an empty list - which would look exactly like a project
	// containing no aircraft, and would put the whole fleet in the yard undriven.
	Registry.WaitForCompletion();

	TArray<FAssetData> TypeAssets;
	Registry.GetAssetsByClass(UAircraftType::StaticClass()->GetClassPathName(), TypeAssets);

	for (const FAssetData& Data : TypeAssets)
	{
		const UAircraftType* Type = Cast<UAircraftType>(Data.GetAsset());
		if (Type == nullptr)
		{
			continue;
		}

		// THROUGH Airframe() AND ResolveAgentView, not by reading Type->Mesh directly. The
		// type's own soft pointers are only half the answer: ResolveAgentView is what applies
		// UAirsideContent's game-wide fallbacks, so a type that declares no mesh of its own
		// resolves to the same asset the airport would put on the taxiway. Comparing against
		// the raw field would make the bench disagree with the game about which model this is.
		const FAirframe Frame = Type->Airframe();
		const FResolvedAgentView View = UAirsideSettings::ResolveAgentView(Frame);
		if (View.Mesh != Mesh || View.AnimClass == nullptr)
		{
			continue;
		}

		OutRig.AnimClass = View.AnimClass;
		OutRig.bIsVehicle = false;

		// THE GEAR FIGURES TRAVEL WITH THE TYPE. plane4's are the fleet's only authored set,
		// and the yard's single normalised gear channel becomes this airframe's own cycle
		// length - see FYardMotion::ToAgentMotion for why the split is here rather than in the
		// channel.
		OutRig.Gear = Frame.Gear;
		return true;
	}

	return false;
}
