#include "AnimYardCatalogue.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Content/AirsideSettings.h"
#include "Engine/SkeletalMesh.h"
#include "Entities/AircraftType.h"
#include "Model/Airframe.h"

void AnimYardCatalogue::EveryRig(TArray<FYardRigEntry>& Out)
{
	Out.Reset();

	// THE ONE RIGGED VEHICLE. UAirsideContent declares its mesh and its Animation Blueprint
	// side by side, which is the whole pairing - there is no vehicle equivalent of
	// UAircraftType to enumerate, because there is exactly one.
	const FResolvedAgentView Vehicle = UAirsideSettings::ResolveVehicleView();
	if (Vehicle.Mesh != nullptr && Vehicle.AnimClass != nullptr)
	{
		FYardRigEntry Entry;
		Entry.Mesh = Vehicle.Mesh;
		Entry.Rig.AnimClass = Vehicle.AnimClass;
		Entry.Rig.bIsVehicle = true;

		// GEAR LEFT UNSET. A truck has none, and FGearPerformance::IsSet() being false is what
		// makes FYardMotion::ToAgentMotion leave its gear fractions at the resting pose.
		Entry.Rig.Gear = FGearPerformance();
		Entry.bMeshDeclaredByItsType = true;

		// BoxSizeUu IS LEFT AT ITS DEFAULT and never read: ARoadAgentActor::SetVehicleAirframe
		// uses it only when Mesh is null, and this branch has just established it is not.
		Entry.DeclaredBy = TEXT("UAirsideContent");
		Out.Add(MoveTemp(Entry));
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
		if (View.Mesh == nullptr || View.AnimClass == nullptr)
		{
			continue;
		}

		// ONE ENTRY PER MESH, AND THE EXPLICIT DECLARER WINS IT.
		//
		// FOUND BY MEASUREMENT, 2026-09-21: the first form of this took whichever type the
		// asset registry happened to return first, and the registry returned DA_Aircraft_A320
		// before DA_Aircraft_Plane7. The A320 declares no mesh of its own, so ResolveAgentView
		// hands it UAirsideContent's game-wide AgentMesh - which IS SK_Plane7 - and the
		// catalogue ended up driving the Meridian's model with the A320's gear figures. The
		// bench would have shown a Meridian retracting on an airliner's cycle, and the entry
		// was labelled "SK_Plane7 (DA_Aircraft_A320)" in the rig test's output, which is how it
		// was noticed at all.
		//
		// A type that NAMES this mesh is talking about it; one that fell back to the default
		// is talking about having no mesh. When both exist, the first is the answer.
		const bool bDeclaredHere = !Type->Mesh.IsNull();
		const int32 Existing = Out.IndexOfByPredicate(
			[&View](const FYardRigEntry& Entry) { return Entry.Mesh == View.Mesh; });
		if (Existing != INDEX_NONE)
		{
			if (!bDeclaredHere || Out[Existing].bMeshDeclaredByItsType)
			{
				continue;
			}
			Out.RemoveAt(Existing);
		}

		FYardRigEntry Entry;
		Entry.Mesh = View.Mesh;
		Entry.Rig.AnimClass = View.AnimClass;
		Entry.Rig.bIsVehicle = false;
		Entry.bMeshDeclaredByItsType = bDeclaredHere;

		// THE GEAR FIGURES TRAVEL WITH THE TYPE. plane4's are the fleet's only authored set,
		// and the yard's single normalised gear channel becomes this airframe's own cycle
		// length - see FYardMotion::ToAgentMotion for why the split is here rather than in the
		// channel.
		Entry.Rig.Gear = Frame.Gear;
		Entry.DeclaredBy = Data.AssetName.ToString();
		Out.Add(MoveTemp(Entry));
	}
}

bool AnimYardCatalogue::FindRigFor(USkeletalMesh* Mesh, FYardRig& OutRig)
{
	if (Mesh == nullptr)
	{
		return false;
	}

	// BUILT ON EveryRig rather than scanning for itself. The two would otherwise be a
	// catalogue and a second opinion about it, and the test that walks EveryRig would be
	// checking rigs the yard never actually asks for.
	TArray<FYardRigEntry> Rigs;
	EveryRig(Rigs);

	for (const FYardRigEntry& Entry : Rigs)
	{
		if (Entry.Mesh == Mesh)
		{
			OutRig = Entry.Rig;
			return true;
		}
	}

	return false;
}
