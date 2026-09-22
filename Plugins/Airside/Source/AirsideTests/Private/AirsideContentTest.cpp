#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Content/FenceKit.h"
#include "Entities/EntityDefinition.h"
#include "Model/RunwayFacts.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"

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

/**
 * An asset saved before Placeables existed (issue #192 item 1) still has its bytes under
 * DefaultStand / DefaultFuelDepot - meta = (DeprecatedProperty) keeps those tagged names
 * matching on load, the way the runway materials test above proves for its own three fields
 * - and this asserts PostLoad actually moves them into Placeables rather than leaving it
 * empty, which is what would make ARoadNetworkActor place nothing for a shipped asset.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentMigratesDeprecatedPlaceablesTest,
	"Airside.Content.MigratesDeprecatedPlaceables",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentMigratesDeprecatedPlaceablesTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UEntityDefinition* Stand = NewObject<UEntityDefinition>();
	UEntityDefinition* Depot = NewObject<UEntityDefinition>();

	// Simulate what a pre-migration asset deserialises as: only the two deprecated fields
	// set, Placeables never authored at all.
	Content->DefaultStand = TSoftObjectPtr<UEntityDefinition>(Stand);
	Content->DefaultFuelDepot = TSoftObjectPtr<UEntityDefinition>(Depot);

	if (!TestEqual(TEXT("nothing authored into Placeables yet"), Content->Placeables.Num(), 0))
	{
		return false;
	}

	Content->PostLoad();

	if (!TestEqual(TEXT("PostLoad maps both kinds from the two deprecated slots"),
		Content->Placeables.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("Stand resolves from the deprecated stand definition"),
		Content->Placeables.FindRef(EPlaceableEntity::Stand), Content->DefaultStand);
	TestEqual(TEXT("FuelDepot resolves from the deprecated fuel depot definition"),
		Content->Placeables.FindRef(EPlaceableEntity::FuelDepot), Content->DefaultFuelDepot);

	return true;
}

/**
 * A deprecated field left unset, or a kind already authored into Placeables (a re-author
 * made after this migration first ran and got saved back), must not be clobbered - the same
 * rule the RunwayMaterials PostLoad test above asserts for its own slots.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentPlaceablesPostLoadDoesNotClobberAuthoredSlotTest,
	"Airside.Content.PlaceablesPostLoadDoesNotClobberAuthoredSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentPlaceablesPostLoadDoesNotClobberAuthoredSlotTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UEntityDefinition* AuthoredStand = NewObject<UEntityDefinition>();
	UEntityDefinition* DeprecatedStand = NewObject<UEntityDefinition>();

	Content->Placeables.Add(EPlaceableEntity::Stand, TSoftObjectPtr<UEntityDefinition>(AuthoredStand));
	Content->DefaultStand = TSoftObjectPtr<UEntityDefinition>(DeprecatedStand);

	Content->PostLoad();

	TestEqual(TEXT("an already-authored kind survives PostLoad untouched"),
		Content->Placeables.FindRef(EPlaceableEntity::Stand),
		TSoftObjectPtr<UEntityDefinition>(AuthoredStand));

	return true;
}

/**
 * THE ONE-LIST CHECK FOR EPlaceableEntity (issue #192 item 1) - the shape
 * AirportMgr.Actions.EveryGestureModeIsHandled uses for EGestureMode, and the reason it is
 * worth stating here rather than only asserting Stand and FuelDepot by name: the kind used
 * to be resolved by a hand-written ternary
 * (ARoadNetworkActor::ResolveEntityDefinition: FuelDepot, or else Stand), a list a third
 * member could join without anything here noticing. Walking every REFLECTED member through
 * UAirsideSettings::ResolvePlaceable, and giving each one a DISTINCT marker object, means a
 * kind that resolved to the WRONG entry - exactly the ternary's failure shape - fails here,
 * and a new kind starts being checked the moment it exists, with no edit to this test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryPlaceableEntityResolvesFromContentTest,
	"Airside.Content.EveryPlaceableEntityResolvesFromContent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryPlaceableEntityResolvesFromContentTest::RunTest(const FString& Parameters)
{
	const UEnum* Enum = StaticEnum<EPlaceableEntity>();
	if (!TestNotNull(TEXT("EPlaceableEntity is reflected, so this test can count it"), Enum))
	{
		return false;
	}

	const int32 Count = Enum->NumEnums() - 1; // drop the generated _MAX sentinel
	if (!TestTrue(TEXT("there is more than one kind to map"), Count >= 2))
	{
		return false;
	}

	UAirsideContent* Content = NewObject<UAirsideContent>();
	TArray<UEntityDefinition*> Markers;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const EPlaceableEntity Kind = static_cast<EPlaceableEntity>(Enum->GetValueByIndex(Index));
		UEntityDefinition* Marker = NewObject<UEntityDefinition>();
		Markers.Add(Marker);
		Content->Placeables.Add(Kind, TSoftObjectPtr<UEntityDefinition>(Marker));
	}

	// SWAP THE CONFIGURED CONTENT SET for the duration of this test only, the same way
	// FFuelDepotPlaceToolTest does to reach its "no depot anywhere" branch - restored on
	// every exit so no other test in the run sees a fake content set.
	UAirsideSettings* Settings = GetMutableDefault<UAirsideSettings>();
	const TSoftObjectPtr<UAirsideContent> ConfiguredContent = Settings->Content;
	Settings->Content = TSoftObjectPtr<UAirsideContent>(Content);
	ON_SCOPE_EXIT { Settings->Content = ConfiguredContent; };

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const EPlaceableEntity Kind = static_cast<EPlaceableEntity>(Enum->GetValueByIndex(Index));
		TestEqual(*FString::Printf(TEXT("%s resolves to its own definition, not another kind's"),
			*Enum->GetNameStringByIndex(Index)),
			UAirsideSettings::ResolvePlaceable(Kind), Markers[Index]);
	}

	return true;
}

/**
 * The project's content set names a whole fence kit, and the fabric is masked and two-sided.
 *
 * AGAINST THE REAL DA_AirsideContent, not a NewObject: the failure this guards is the one a
 * build_*.py writes nothing and reports success, and only the saved asset can show it. A
 * translucent fabric would sort wrongly; a one-sided one vanishes from inside the plot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentFenceKitResolvesTest,
	"Airside.Content.FenceKitResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentFenceKitResolvesTest::RunTest(const FString& Parameters)
{
	const FFenceKit Kit = UAirsideSettings::ResolveFenceKit();
	TestNotNull(TEXT("the line post is authored"), Kit.LinePost);
	TestNotNull(TEXT("the heavy post is authored"), Kit.HeavyPost);
	if (!TestNotNull(TEXT("the fabric material is authored"), Kit.Fabric)) { return false; }
	TestEqual(TEXT("the fabric is Masked, per the asset README"),
		Kit.Fabric->GetBlendMode(), EBlendMode::BLEND_Masked);
	TestTrue(TEXT("and two-sided"), Kit.Fabric->IsTwoSided());
	return true;
}

#endif
