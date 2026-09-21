#include "Content/AirsideContent.h"

#include "Model/RunwayFacts.h"

void UAirsideContent::PostLoad()
{
	Super::PostLoad();

	// MIGRATED, NOT RESAVED (issue #105 item 4, PR #137 review): an asset authored before
	// RunwayMaterials existed still has its bytes under the three deprecated properties -
	// see their own comment for why the tagged names still match on load. Each deprecated
	// slot only overwrites RunwayMaterials at its own index when it is actually SET, so an
	// asset that already has some RunwayMaterials authored (a partial re-author, or one made
	// after this migration first ran and got saved back) is never clobbered by a leftover
	// deprecated value nothing ever cleared.
	if (RunwayMaterials.Num() < RunwayMaterialSlotCount)
	{
		RunwayMaterials.SetNum(RunwayMaterialSlotCount);
	}

	auto Migrate = [this](ERunwaySurface Surface, const TSoftObjectPtr<UMaterialInterface>& Deprecated)
	{
		if (!Deprecated.IsNull())
		{
			RunwayMaterials[RunwayMaterialSlot(Surface)] = Deprecated;
		}
	};
	Migrate(ERunwaySurface::Grass, RunwayGrassMaterial_DEPRECATED);
	Migrate(ERunwaySurface::Tarmac, RunwayTarmacMaterial_DEPRECATED);
	Migrate(ERunwaySurface::Concrete, RunwayConcreteMaterial_DEPRECATED);

	// MIGRATED, NOT RESAVED (issue #192 item 1): an asset authored before Placeables existed
	// still has its bytes under DefaultStand / DefaultFuelDepot - meta = (DeprecatedProperty)
	// keeps those tagged names matching on load without an "_DEPRECATED" rename, since
	// nothing else is claiming the old names. Each deprecated slot only fills its OWN map
	// entry when Placeables does not already have one there, so a set authored (or re-saved)
	// after this migration first ran is never clobbered by a leftover deprecated value
	// nothing ever cleared - the same rule the RunwayMaterials migration above follows.
	auto MigratePlaceable = [this](EPlaceableEntity Kind, const TSoftObjectPtr<UEntityDefinition>& Deprecated)
	{
		if (!Deprecated.IsNull() && !Placeables.Contains(Kind))
		{
			Placeables.Add(Kind, Deprecated);
		}
	};
	MigratePlaceable(EPlaceableEntity::Stand, DefaultStand);
	MigratePlaceable(EPlaceableEntity::FuelDepot, DefaultFuelDepot);
}
