#include "Profiles/RoadMaterialSet.h"

#include "Materials/Material.h"

int32 URoadMaterialSet::IndexOf(FName Slot) const
{
	// A linear scan, not a cached map. Slots is a handful of entries walked once per
	// profile per rebuild, and a map would need invalidating whenever the asset is edited -
	// a second source of truth for the ordering that IS the material id.
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].Name == Slot)
		{
			return Index;
		}
	}

	// INDEX_NONE, deliberately never 0. Falling back here would make a misspelled slot
	// indistinguishable from a genuine Asphalt band at every caller. The band table decides
	// the fallback, and reports it.
	return INDEX_NONE;
}

void URoadMaterialSet::ResolveMaterials(TArray<UMaterialInterface*>& Out) const
{
	Out.Reset();
	Out.Reserve(Slots.Num());

	for (const FRoadMaterialSlot& Slot : Slots)
	{
		// An empty slot still gets a material. ConfigureMaterialSet matches purely by
		// index, and FDynamicMeshSceneProxy discards any triangle whose id is >= the
		// material count - so a hole in this array is a hole in the road, drawn with no
		// error. The engine default is ugly and visible, which is the correct failure.
		Out.Add(Slot.Material != nullptr
			? Slot.Material.Get()
			: UMaterial::GetDefaultMaterial(MD_Surface));
	}
}

URoadMaterialSet* URoadMaterialSet::MakeTransient(const TArray<FName>& Names)
{
	URoadMaterialSet* Set = NewObject<URoadMaterialSet>(GetTransientPackage());
	Set->Slots.Reserve(Names.Num());

	for (FName Name : Names)
	{
		FRoadMaterialSlot Slot;
		Slot.Name = Name;
		Set->Slots.Add(Slot);
	}

	return Set;
}
