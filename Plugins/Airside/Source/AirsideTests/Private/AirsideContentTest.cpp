#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Content/AirsideContent.h"
#include "Model/RunwayFacts.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * An asset saved before RunwayMaterials existed (issue #105 item 4, PR #137 review): its
 * three deprecated properties still deserialise (the "_DEPRECATED" suffix keeps the tagged
 * name matching - see their own comment), and this asserts PostLoad actually moves them into
 * RunwayMaterials rather than leaving it empty, which is what made every runway render with
 * a null material before this fix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentMigratesDeprecatedRunwayMaterialsTest,
	"Airside.Content.MigratesDeprecatedRunwayMaterials",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentMigratesDeprecatedRunwayMaterialsTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();

	// Simulate what a pre-migration asset deserialises as: only the three deprecated fields
	// set, RunwayMaterials never authored at all.
	Content->RunwayGrassMaterial_DEPRECATED = TSoftObjectPtr<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
	Content->RunwayTarmacMaterial_DEPRECATED = TSoftObjectPtr<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
	Content->RunwayConcreteMaterial_DEPRECATED = TSoftObjectPtr<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Engine/EngineMaterials/DefaultDiffuse.DefaultDiffuse")));

	if (!TestEqual(TEXT("nothing authored into RunwayMaterials yet"),
		Content->RunwayMaterials.Num(), 0))
	{
		return false;
	}

	Content->PostLoad();

	if (!TestEqual(TEXT("PostLoad sizes RunwayMaterials to the slot count"),
		Content->RunwayMaterials.Num(), RunwayMaterialSlotCount))
	{
		return false;
	}

	TestEqual(TEXT("Grass slot resolves from the deprecated grass material"),
		Content->RunwayMaterials[RunwayMaterialSlot(ERunwaySurface::Grass)],
		Content->RunwayGrassMaterial_DEPRECATED);
	TestEqual(TEXT("Tarmac slot resolves from the deprecated tarmac material"),
		Content->RunwayMaterials[RunwayMaterialSlot(ERunwaySurface::Tarmac)],
		Content->RunwayTarmacMaterial_DEPRECATED);
	TestEqual(TEXT("Concrete slot resolves from the deprecated concrete material"),
		Content->RunwayMaterials[RunwayMaterialSlot(ERunwaySurface::Concrete)],
		Content->RunwayConcreteMaterial_DEPRECATED);

	return true;
}

/**
 * A deprecated field left unset (the common case for two of the three, on most real assets)
 * must not stamp a null over a slot RunwayMaterials already has a real answer for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentPostLoadDoesNotClobberAuthoredSlotTest,
	"Airside.Content.PostLoadDoesNotClobberAuthoredSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentPostLoadDoesNotClobberAuthoredSlotTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	Content->RunwayMaterials.SetNum(RunwayMaterialSlotCount);
	Content->RunwayMaterials[RunwayMaterialSlot(ERunwaySurface::Grass)] =
		TSoftObjectPtr<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));

	// The deprecated grass field is unset - PostLoad must leave the already-authored slot
	// alone rather than overwriting it with a null.
	Content->PostLoad();

	TestTrue(TEXT("an already-authored slot survives PostLoad untouched"),
		!Content->RunwayMaterials[RunwayMaterialSlot(ERunwaySurface::Grass)].IsNull());

	return true;
}

#endif
