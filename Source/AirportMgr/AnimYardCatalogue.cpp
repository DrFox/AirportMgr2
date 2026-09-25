#include "AnimYardCatalogue.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Content/AirsideSettings.h"
#include "Engine/SkeletalMesh.h"
#include "Entities/AircraftType.h"
#include "Entities/VehicleType.h"
#include "Model/Airframe.h"

void AnimYardCatalogue::EveryRig(TArray<FYardRigEntry>& Out)
{
	Out.Reset();

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();

	// THE SCAN IS FINISHED BEFORE IT IS READ. In the editor the registry is long since
	// populated and this returns at once; in a commandlet, or early in a cooked run, an
	// unwaited registry answers with an empty list - which would look exactly like a project
	// containing no aircraft, and would put the whole fleet in the yard undriven.
	Registry.WaitForCompletion();

	// EVERY VEHICLE TYPE, FIRST. Since 2026-09-25 a vehicle declares its mesh and its
	// Animation Blueprint on a UVehicleType, as an aircraft does on a UAircraftType - so the
	// yard shows a newly authored DA_Vehicle_* with no code change, which is the whole reason
	// this catalogue keeps no table of its own.
	//
	// DIRECT, NOT THROUGH A RESOLVER. A vehicle type has no game-wide fallback mesh the way an
	// aircraft type does (ResolveAgentView's AgentMesh), so there is nothing a resolver would
	// add, and a type with no mesh or no graph is simply not a rig.
	TArray<FAssetData> VehicleAssets;
	Registry.GetAssetsByClass(UVehicleType::StaticClass()->GetClassPathName(), VehicleAssets);
	for (const FAssetData& Data : VehicleAssets)
	{
		const UVehicleType* Type = Cast<UVehicleType>(Data.GetAsset());
		if (Type == nullptr)
		{
			continue;
		}
		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		UClass* AnimClass = Type->AnimClass.LoadSynchronous();
		if (Mesh == nullptr || AnimClass == nullptr)
		{
			continue;
		}

		FYardRigEntry Entry;
		Entry.Mesh = Mesh;
		Entry.Rig.AnimClass = AnimClass;
		Entry.Rig.bIsVehicle = true;
		// GEAR LEFT UNSET. A truck has none, and FGearPerformance::IsSet() being false is what
		// makes FYardMotion::ToAgentMotion leave its gear fractions at the resting pose.
		Entry.Rig.Gear = FGearPerformance();
		Entry.Rig.bSteers = !Type->bTowed;
		Entry.bMeshDeclaredByItsType = true;
		Entry.DeclaredBy = Data.AssetName.ToString();
		Out.Add(MoveTemp(Entry));
	}

	// THE CONTENT SET'S VEHICLE, ONLY IF NO TYPE ALREADY ANSWERED FOR ITS MESH. UAirsideContent
	// still names the dispatched truck's mesh and graph (VehicleSkeletalMesh / VehicleAnimClass)
	// because dispatch reads them; DA_Vehicle_FuelTruck1 names the same pair. The TYPE wins,
	// on the rule the aircraft scan below states - the declarer that names a mesh is the one
	// talking about it - and so the fuel truck is one subject in the yard, not two.
	//
	// KEPT AT ALL, rather than dropped now the type exists, so a project whose content set
	// names a vehicle no type describes still shows it driven.
	const FResolvedAgentView Vehicle = UAirsideSettings::ResolveVehicleView();
	const bool bVehicleTyped = Vehicle.Mesh != nullptr && Out.ContainsByPredicate(
		[&Vehicle](const FYardRigEntry& Entry) { return Entry.Mesh == Vehicle.Mesh; });
	if (Vehicle.Mesh != nullptr && Vehicle.AnimClass != nullptr && !bVehicleTyped)
	{
		FYardRigEntry Entry;
		Entry.Mesh = Vehicle.Mesh;
		Entry.Rig.AnimClass = Vehicle.AnimClass;
		Entry.Rig.bIsVehicle = true;
		Entry.Rig.Gear = FGearPerformance();
		Entry.bMeshDeclaredByItsType = true;

		// BoxSizeUu IS LEFT AT ITS DEFAULT and never read: ARoadAgentActor::SetVehicleAirframe
		// uses it only when Mesh is null, and this branch has just established it is not.
		Entry.DeclaredBy = TEXT("UAirsideContent");
		Out.Add(MoveTemp(Entry));
	}

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
