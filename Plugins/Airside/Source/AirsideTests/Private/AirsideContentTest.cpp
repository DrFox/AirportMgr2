#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Content/FenceKit.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Solve/IcaoCode.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialCachedData.h"
#include "Engine/StaticMesh.h"
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
		Content->RunwayMaterials.Num(), PavementMaterialSlotCount))
	{
		return false;
	}

	TestEqual(TEXT("Grass slot resolves from the deprecated grass material"),
		Content->RunwayMaterials[Pavement::MaterialSlot(EPavement::Grass)],
		Content->RunwayGrassMaterial_DEPRECATED);
	TestEqual(TEXT("Tarmac slot resolves from the deprecated tarmac material"),
		Content->RunwayMaterials[Pavement::MaterialSlot(EPavement::Tarmac)],
		Content->RunwayTarmacMaterial_DEPRECATED);
	TestEqual(TEXT("Concrete slot resolves from the deprecated concrete material"),
		Content->RunwayMaterials[Pavement::MaterialSlot(EPavement::Concrete)],
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
	Content->RunwayMaterials.SetNum(PavementMaterialSlotCount);
	Content->RunwayMaterials[Pavement::MaterialSlot(EPavement::Grass)] =
		TSoftObjectPtr<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));

	// The deprecated grass field is unset - PostLoad must leave the already-authored slot
	// alone rather than overwriting it with a null.
	Content->PostLoad();

	TestTrue(TEXT("an already-authored slot survives PostLoad untouched"),
		!Content->RunwayMaterials[Pavement::MaterialSlot(EPavement::Grass)].IsNull());

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
	UEntityDefinition* Depot = NewObject<UEntityDefinition>();

	// Simulate what a pre-migration asset deserialises as: only the deprecated field set,
	// Placeables never authored at all. ONE FIELD since 2026-10-03: DefaultStand went with the
	// retired DA_Stand_CodeC (stands are built at runtime), so only the depot migrates.
	Content->DefaultFuelDepot = TSoftObjectPtr<UEntityDefinition>(Depot);

	if (!TestEqual(TEXT("nothing authored into Placeables yet"), Content->Placeables.Num(), 0))
	{
		return false;
	}

	Content->PostLoad();

	if (!TestEqual(TEXT("PostLoad maps the depot from its deprecated slot, and nothing for a stand"),
		Content->Placeables.Num(), 1))
	{
		return false;
	}

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
	UEntityDefinition* AuthoredDepot = NewObject<UEntityDefinition>();
	UEntityDefinition* DeprecatedDepot = NewObject<UEntityDefinition>();

	// THE DEPOT since 2026-10-03 (was the stand, whose deprecated field went with DA_Stand_CodeC).
	Content->Placeables.Add(EPlaceableEntity::FuelDepot, TSoftObjectPtr<UEntityDefinition>(AuthoredDepot));
	Content->DefaultFuelDepot = TSoftObjectPtr<UEntityDefinition>(DeprecatedDepot);

	Content->PostLoad();

	TestEqual(TEXT("an already-authored kind survives PostLoad untouched"),
		Content->Placeables.FindRef(EPlaceableEntity::FuelDepot),
		TSoftObjectPtr<UEntityDefinition>(AuthoredDepot));

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
 * The fence's distance fade is WIRED: the fabric and both posts' materials read one parameter
 * collection, the posts are masked (an opaque post cannot fade), and the collection's figures
 * are ordered so the fade can happen at all.
 *
 * WHY: at build-camera range the wire and the posts are sub-pixel (build_fence_content.py's
 * docstring has the figures), and the fade is the only thing standing between the player and
 * a band of crawling noise. It lives entirely in content, so a rerun of the script that writes
 * nothing, or an import that resets the posts' slot to chainlink_post, would drop it silently.
 *
 * NAMES, not a count, per CLAUDE.md "lists that must agree": the script's FADE list is the
 * other half of this one, and a parameter it renamed would otherwise read as 0 in HLSL.
 *
 * ALSO THE KIT'S SHAPE (#462, merge M26, from Airside.Content.FenceKitResolves): against the REAL DA_AirsideContent, not a
 * NewObject - the failure this guards is the one where a build_*.py writes nothing and reports success, and only the saved
 * asset can show it. A translucent fabric would sort wrongly; a one-sided one vanishes from inside the plot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentFenceFadeWiredTest,
	"Airside.Content.FenceFadeWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentFenceFadeWiredTest::RunTest(const FString& Parameters)
{
	const FFenceKit Kit = UAirsideSettings::ResolveFenceKit();
	if (!TestNotNull(TEXT("the fabric material is authored"), Kit.Fabric)
		|| !TestNotNull(TEXT("the line post is authored"), Kit.LinePost)
		|| !TestNotNull(TEXT("the heavy post is authored"), Kit.HeavyPost))
	{
		return false;
	}

	TestEqual(TEXT("the fabric is Masked, per the asset README"), Kit.Fabric->GetBlendMode(), EBlendMode::BLEND_Masked);
	TestTrue(TEXT("and two-sided"), Kit.Fabric->IsTwoSided());

	auto CollectionOf = [this](const UMaterialInterface* Material, const TCHAR* What)
		-> const UMaterialParameterCollection*
	{
		if (!TestNotNull(*FString::Printf(TEXT("%s has a material"), What), Material)) { return nullptr; }
		const TArray<FMaterialParameterCollectionInfo>& Infos =
			Material->GetCachedExpressionData().ParameterCollectionInfos;
		if (!TestEqual(*FString::Printf(TEXT("%s reads exactly one parameter collection"), What), Infos.Num(), 1))
		{
			return nullptr;
		}
		return Infos[0].ParameterCollection;
	};

	const UMaterialParameterCollection* Fade = CollectionOf(Kit.Fabric, TEXT("the fabric"));
	const UMaterialInterface* LineMat = Kit.LinePost->GetMaterial(0);
	const UMaterialInterface* HeavyMat = Kit.HeavyPost->GetMaterial(0);
	const UMaterialParameterCollection* LineFade = CollectionOf(LineMat, TEXT("the line post"));
	const UMaterialParameterCollection* HeavyFade = CollectionOf(HeavyMat, TEXT("the heavy post"));
	if (!TestNotNull(TEXT("the fabric's collection loaded"), Fade)) { return false; }
	TestTrue(TEXT("the line post fades by the fabric's collection"), LineFade == Fade);
	TestTrue(TEXT("the heavy post fades by the fabric's collection"), HeavyFade == Fade);
	if (LineMat != nullptr)
	{
		TestEqual(TEXT("the line post is Masked - an opaque one cannot dither out"),
			LineMat->GetBlendMode(), EBlendMode::BLEND_Masked);
	}
	if (HeavyMat != nullptr)
	{
		TestEqual(TEXT("the heavy post is Masked"), HeavyMat->GetBlendMode(), EBlendMode::BLEND_Masked);
	}

	auto Get = [this, Fade](const TCHAR* Name) -> float
	{
		bool bFound = false;
		const float Value = Fade->GetScalarParameterDefaultValue(FName(Name), bFound);
		TestTrue(*FString::Printf(TEXT("the collection has %s"), Name), bFound);
		return Value;
	};
	const float VeilBelowPx = Get(TEXT("VeilBelowPx"));
	const float WireAbovePx = Get(TEXT("WireAbovePx"));
	const float VeilOpacity = Get(TEXT("VeilOpacity"));
	const float PostFadeStart = Get(TEXT("PostFadeStart"));
	const float PostFadeEnd = Get(TEXT("PostFadeEnd"));
	const float FadeStart = Get(TEXT("FadeStart"));
	const float FadeEnd = Get(TEXT("FadeEnd"));
	TestTrue(TEXT("the veil takes over below the size the wire is whole at"), VeilBelowPx < WireAbovePx);
	TestTrue(TEXT("the veil is partly see-through, not solid and not gone"), VeilOpacity > 0.f && VeilOpacity < 1.f);
	TestTrue(TEXT("the posts' fade runs forwards"), PostFadeStart < PostFadeEnd);
	TestTrue(TEXT("the fence's fade runs forwards"), FadeStart < FadeEnd);
	TestTrue(TEXT("the posts are gone before the fabric starts to go"), PostFadeEnd <= FadeStart);

	// THE CULL READS THE SAME FIGURE: DA_AirsideContent names a collection of its own, and it
	// must be the one the materials fade by, or the posts cull where the material says nothing.
	TestEqual(TEXT("the kit's post fade end is the materials' collection's"), Kit.PostFadeEndUu,
		static_cast<double>(PostFadeEnd));

	// NANITE OFF: a post is a few hundred triangles in a HISM, where Nanite buys nothing, and
	// the masked fade would put every post on Nanite's programmable raster path.
	TestFalse(TEXT("the line post is not Nanite"), Kit.LinePost->IsNaniteEnabled());
	TestFalse(TEXT("the heavy post is not Nanite"), Kit.HeavyPost->IsNaniteEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLetterEnvelopeRaisedByAFleetTypeTest,
	"Airside.Content.LetterEnvelope.RaisedByAFleetType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLetterEnvelopeRaisedByAFleetTypeTest::RunTest(const FString& Parameters)
{
	// THE PIN #292 EXISTS FOR: adding a UAircraftType with a longer tail raises its letter's
	// envelope with NO C++ EDIT - the whole point of splitting MaxTailAft/MaxNoseFwd out of
	// IcaoCode::Rows, where raising them meant retyping a row AND a Python MAX_TAIL_AFT_<letter>
	// by hand (issue #292's own evidence, seven times over).
	//
	// AGAINST EnvelopeFromFleet DIRECTLY, NOT ResolveLetterEnvelope - the test seam
	// EnvelopeFromFleet's own header names itself for: a synthetic in-memory UAircraftType
	// (NewObject, never saved) exercises the raising rule with no .uasset for the
	// AssetRegistry to discover, so this test is deterministic regardless of what the project's
	// real DA_Aircraft_* assets happen to measure this week.
	const EIcaoCode Letter = EIcaoCode::C;
	const double Floor = IcaoCode::FloorEnvelopeForLetter(Letter).MaxTailAft;

	// NOTHING TO RAISE IT: an empty fleet keeps the floor exactly - the letter this asset
	// registry has never heard of yet.
	{
		const FLetterEnvelope Empty =
			UAirsideSettings::EnvelopeFromFleet(Letter, TArrayView<UAircraftType* const>());
		TestEqual(TEXT("with no fleet, the envelope is exactly the floor"), Empty.MaxTailAft, Floor, 0.01);
	}

	// A TYPE 1 UU LONGER THAN THE FLOOR RAISES IT BY EXACTLY THAT 1 UU - pinning the raising
	// RULE (a max, not a replace, not a round-up), not merely "it changed".
	UAircraftType* Longer = NewObject<UAircraftType>(GetTransientPackage());
	Longer->Code = FName(IcaoCode::ToLetter(Letter));
	Longer->SteerAxleX = 0.0;   // origin ON the nose-gear stop mark - see FChassis::SteerAxleX
	Longer->Footprint.TailX = -(Floor + 1.0);   // 1 uu further aft than the floor admits

	TArray<UAircraftType*> Fleet = { Longer };
	const FLetterEnvelope Raised = UAirsideSettings::EnvelopeFromFleet(Letter, Fleet);
	TestEqual(TEXT("the envelope rises by exactly the 1 uu the type exceeds the floor by"),
		Raised.MaxTailAft, Floor + 1.0, 0.01);
	TestEqual(TEXT("MaxNoseFwd is untouched - this type's nose does not exceed the floor"),
		Raised.MaxNoseFwd, IcaoCode::FloorEnvelopeForLetter(Letter).MaxNoseFwd, 0.01);

	// A SHORTER TYPE ALONGSIDE IT DOES NOT LOWER THE ENVELOPE - a MAX over the fleet, never an
	// average or a last-write-wins.
	UAircraftType* Shorter = NewObject<UAircraftType>(GetTransientPackage());
	Shorter->Code = FName(IcaoCode::ToLetter(Letter));
	Shorter->SteerAxleX = 0.0;
	Shorter->Footprint.TailX = -(Floor - 500.0);
	Fleet.Add(Shorter);
	TestEqual(TEXT("a shorter type alongside the longer one does not lower the envelope"),
		UAirsideSettings::EnvelopeFromFleet(Letter, Fleet).MaxTailAft, Floor + 1.0, 0.01);

	// A DIFFERENT LETTER'S TYPE, HOWEVER LONG, NEVER TOUCHES THIS ONE'S ENVELOPE - the fleet is
	// filtered by Code before it is compared, not blended across letters.
	UAircraftType* WrongLetter = NewObject<UAircraftType>(GetTransientPackage());
	WrongLetter->Code = FName(IcaoCode::ToLetter(EIcaoCode::F));
	WrongLetter->Footprint.TailX = -50000.0;
	Fleet.Add(WrongLetter);
	TestEqual(TEXT("a Code F type does not raise Code C's envelope"),
		UAirsideSettings::EnvelopeFromFleet(Letter, Fleet).MaxTailAft, Floor + 1.0, 0.01);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLetterEnvelopeInvalidatesOnAssetChangeTest,
	"Airside.Content.LetterEnvelope.InvalidatesOnAssetChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLetterEnvelopeInvalidatesOnAssetChangeTest::RunTest(const FString& Parameters)
{
	// THE GAP #292's REVIEW FOUND: ResolveLetterEnvelopeTable's cache had no invalidation in
	// production at all - a UAircraftType authored or re-measured after the FIRST resolve of a
	// session stayed invisible until the editor restarted, exactly the class of stale-cache bug
	// "a green test may measure nothing" warns about (nothing here would have failed without
	// content changing mid-session, which no other test does). This drives the AssetRegistry
	// the way the editor itself does on import - AssetCreated, not a direct cache call - so it
	// proves BindLetterEnvelopeInvalidation's delegate actually fires, not merely that the cache
	// CAN be cleared.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();

	const EIcaoCode Letter = EIcaoCode::A;
	UAirsideSettings::ResetLetterEnvelopeCacheForTest();
	const double FloorBefore = UAirsideSettings::ResolveLetterEnvelope(Letter).MaxTailAft;

	// A REAL ASSET, NOT TRANSIENT: UObject::IsAsset() (what AssetCreated itself gates on)
	// requires RF_Public and a package outer that is not the transient package - see that
	// function's own implementation. GetTransientPackage() is what every OTHER UAircraftType
	// fixture in this module uses precisely because it is NOT registered as an asset; this one
	// test needs the opposite.
	UPackage* ProbePackage = CreatePackage(TEXT("/Temp/AirsideLetterEnvelopeInvalidationTest/DA_Aircraft_EnvelopeProbe"));
	UAircraftType* Probe = NewObject<UAircraftType>(ProbePackage,
		TEXT("DA_Aircraft_EnvelopeProbe"), RF_Public | RF_Standalone);
	Probe->Code = FName(IcaoCode::ToLetter(Letter));
	Probe->SteerAxleX = 0.0;
	Probe->Footprint.TailX = -(FloorBefore + 5000.0);   // absurdly further aft than any floor

	// TEARDOWN RUNS REGARDLESS - a stray probe left registered, or a cache still pinned to its
	// inflated figure, would corrupt every other test in this session that asks for Code A's
	// envelope, this test's own early-outs included.
	ON_SCOPE_EXIT
	{
		if (Probe->IsAsset())
		{
			Registry.AssetDeleted(Probe);
		}
		Probe->ClearFlags(RF_Public | RF_Standalone);
		Probe->MarkAsGarbage();
		UAirsideSettings::ResetLetterEnvelopeCacheForTest();
	};

	if (!TestTrue(TEXT("the probe registers as a real asset, or this test proves nothing"),
		Probe->IsAsset()))
	{
		return false;
	}

	Registry.AssetCreated(Probe);

	const double AfterAdd = UAirsideSettings::ResolveLetterEnvelope(Letter).MaxTailAft;
	TestTrue(FString::Printf(TEXT("adding the probe raised Code A's envelope to %.0f with no "
		"ResetLetterEnvelopeCacheForTest call (was %.0f)"), AfterAdd, FloorBefore),
		AfterAdd >= FloorBefore + 5000.0 - 0.5);

	return true;
}

/**
 * PINS THE MIGRATION (2026-09-27): MinimumSurface moved off FRunwayRequirements onto
 * UAircraftType::MinimumPavement (spec 2026-09-27-shared-pavement, Task 3). The Before map is
 * every value the fleet held BEFORE the move, captured from main at f20add83 (Task 0's
 * BASELINE log). A migration that silently reset every type to grass would admit an airliner
 * to a grass strip and pass every test that only checks the Piper - so all 18 are pinned here,
 * grass ones included: they pin that the migration did not INVENT a need either.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAircraftMinimumPavementMigratedTest, "Airside.Content.AircraftMinimumPavementMigrated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAircraftMinimumPavementMigratedTest::RunTest(const FString&)
{
	// THE VALUES EACH ASSET HELD BEFORE THE FIELD MOVED, captured from main 2026-09-27
	// (Task 0). A migration that silently reset every type to grass would admit an airliner
	// to a grass strip and pass every test that only checks the Piper.
	const TMap<FString, EPavement> Before = {
		{ TEXT("DA_Aircraft_A320"), EPavement::Grass },
		{ TEXT("DA_Aircraft_B738"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane1"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane2"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane3"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane4"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane5"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane6"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane7"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane8"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane9"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane10"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane11"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane12"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane13"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane14"), EPavement::Tarmac },
		{ TEXT("DA_Aircraft_Plane15"), EPavement::Grass },
		{ TEXT("DA_Aircraft_Plane16"), EPavement::Grass },
	};
	int32 Seen = 0;
	for (const TPair<FString, EPavement>& Row : Before)
	{
		const UAircraftType* Type = LoadObject<UAircraftType>(nullptr,
			*FString::Printf(TEXT("/Game/Entities/%s.%s"), *Row.Key, *Row.Key));
		if (const EPavement* Expected = Type != nullptr ? &Row.Value : nullptr)
		{
			++Seen;
			TestEqual(FString::Printf(TEXT("%s keeps the pavement it needed"), *Type->GetName()),
				Type->MinimumPavement, *Expected);
			TestEqual(TEXT("and carries it into the airframe admission reads"), Type->Airframe().MinimumPavement, *Expected);
		}
	}
	TestEqual(TEXT("every baseline type loaded - a moved asset would otherwise skip silently"), Seen, Before.Num());
	return true;
}

/**
 * EVERY ROAD AND TAXIWAY PROFILE THE CONTENT SET RESOLVES OFFERS { Tarmac, Grass }, in that
 * order - the fact RoadProfile.h's AllowedPavements, RoadDrawTool.h's Surface row and
 * RouteSearch's TaxiwayPavementCeiling all state. AGAINST THE REAL DA_AirsideContent: the
 * lists are authored data (build_road_profiles.py), and an asset re-authored with concrete, or
 * left empty (empty means ALL FOUR), would pass every test that builds a transient profile.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfilesOfferTarmacAndGrassTest,
	"Airside.Content.RoadProfilesOfferTarmacAndGrass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadProfilesOfferTarmacAndGrassTest::RunTest(const FString& Parameters)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestNotNull(TEXT("the project's content set loads"), Content)) { return false; }

	const TArray<EPavement> Expected = { EPavement::Tarmac, EPavement::Grass };
	auto CheckList = [this, &Expected](const TArray<TSoftObjectPtr<URoadProfile>>& List, const TCHAR* Kind)
	{
		int32 Seen = 0;
		for (const TSoftObjectPtr<URoadProfile>& Soft : List)
		{
			const URoadProfile* Profile = Soft.LoadSynchronous();
			if (!TestNotNull(*FString::Printf(TEXT("%s profile %s loads"), Kind, *Soft.ToString()), Profile)) { continue; }
			++Seen;
			TestTrue(*FString::Printf(TEXT("%s offers exactly tarmac then grass"), *Profile->GetName()),
				Profile->AllowedPavements == Expected);
		}
		// A list that resolved nothing would pass the loop above vacuously.
		TestTrue(*FString::Printf(TEXT("the content set names at least one %s profile"), Kind), Seen > 0);
	};
	CheckList(Content->TaxiwayProfiles, TEXT("taxiway"));
	CheckList(Content->ServiceRoadProfiles, TEXT("service road"));
	return true;
}

namespace
{
	/**
	 * Compare two FAirframes property by property, by reflection, and say which one differs. A field
	 * added to FAirframe next month is compared without anyone adding it here - the whole point of
	 * walking the struct rather than naming fields (#449). Returns how many properties were compared,
	 * so a caller can floor it and a struct that stopped reflecting anything cannot pass vacuously.
	 */
	int32 CompareEveryAirframeProperty(FAutomationTestBase& Test, const TCHAR* Who,
		const FAirframe& Actual, const FAirframe& Expected)
	{
		int32 Compared = 0;
		for (TFieldIterator<FProperty> It(FAirframe::StaticStruct()); It; ++It)
		{
			++Compared;
			Test.TestTrue(FString::Printf(TEXT("%s: FAirframe::%s is the Meridian's own"), Who, *It->GetName()),
				It->Identical_InContainer(&Actual, &Expected));
		}
		// A FLOOR, NOT A COUNT: 14 on 2026-09-30, and a field added later is compared without this changing.
		Test.TestTrue(FString::Printf(TEXT("%s: every property was compared (%d)"), Who, Compared), Compared >= 14);
		return Compared;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDefaultAirframeIsTheMeridiansOwnTest,
	"Airside.Content.DefaultAirframeIsTheMeridiansOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDefaultAirframeIsTheMeridiansOwnTest::RunTest(const FString& Parameters)
{
	// #449: THE CONTENT-LESS DEFAULT IS THE MERIDIAN READ THE WAY AN ASSET IS READ - BuildPiperMeridian, then
	// UAircraftType::Airframe() - on EVERY property of FAirframe, walked by reflection so a field added next month is
	// compared without anyone adding it here. The hand copy this replaced set 8 of 20 and drove on the wrong steer law.
	//
	// AGAINST THE FALLBACK ITSELF (#479), not through ResolveDefaultAirframe: this used to compare the resolver and
	// return early - AddInfo, pass - when the content set named a DefaultAircraft, so the day #30 authored
	// DA_PiperMeridian it would have gone vacuous without a line changing. ContentlessDefaultAirframe is the branch
	// ResolveDefaultAirframe falls back to, public for exactly this, so the pin holds whatever content is present.
	UAircraftType* Meridian = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildPiperMeridian(Meridian);
	const FAirframe Expected = Meridian->Airframe();
	const FAirframe Fallback = UAirsideSettings::ContentlessDefaultAirframe();

	CompareEveryAirframeProperty(*this, TEXT("the content-less fallback"), Fallback, Expected);

	// THE TWO FIGURES THE HAND COPY GOT WRONG, named so a failure says what a fleet test would feel.
	TestEqual(TEXT("it steers like every modelled aeroplane - RollingSteer, not the FChassis default Pivot"),
		Fallback.Chassis.SteerLaw, ESteerLaw::RollingSteer);
	TestEqual(TEXT("with the Meridian's measured wheelbase"), Fallback.Chassis.FixedAxleX, -237.8, 1e-9);

	// THE DOOR: with no DefaultAircraft named, ResolveDefaultAirframe IS the fallback. When the content set DOES name
	// one it wins by design and is the same mapping by construction (Airframe() of that asset), so there is nothing
	// to compare - but the comparison above no longer depends on which of the two worlds this run is in.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (Content != nullptr && !Content->DefaultAircraft.IsNull())
	{
		AddInfo(TEXT("The content set names a DefaultAircraft; ResolveDefaultAirframe returns that asset's airframe, not the fallback"));
	}
	else
	{
		CompareEveryAirframeProperty(*this, TEXT("ResolveDefaultAirframe with no DefaultAircraft"),
			UAirsideSettings::ResolveDefaultAirframe(), Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFixturePiperIsTheMeridiansOwnTest,
	"Airside.Content.FixturePiperIsTheMeridiansOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFixturePiperIsTheMeridiansOwnTest::RunTest(const FString& Parameters)
{
	// #479: TestAirframes::Piper() WAS A HAND COPY OF THE MERIDIAN, AND A WRONG ONE - the same drift #449 removed from
	// production, left behind in the fixture 132 call sites lean on (git grep of TestAirframes::Piper(), 2026-10-01; it was 122 when this was
	// written). It set the four performance structs and nothing
	// else: the FChassis default Pivot steer law where every modelled aeroplane rolls on its mains, no wheelbase, a
	// steered final turn, no body centre, no wingspan or TypeCode. Every traffic test that took it measured a vehicle no
	// flight in the game is (#477 found six measuring the nose where the body centre was meant).
	//
	// AGAINST THE FALLBACK, NOT ResolveDefaultAirframe: the fixture must stay the Meridian whatever DefaultAircraft a
	// content set names (a content change should not silently change what those 132 call sites measure), so it is pinned to the
	// content-less branch directly. Every property, by reflection, so a field added to FAirframe is compared without
	// anyone remembering this file.
	const FAirframe Fixture = TestAirframes::Piper();
	const FAirframe Fallback = UAirsideSettings::ContentlessDefaultAirframe();
	CompareEveryAirframeProperty(*this, TEXT("TestAirframes::Piper()"), Fixture, Fallback);

	// THE TWO FIGURES, named, so a red run says what a fleet test would feel (the same two the pin above names).
	TestEqual(TEXT("the fixture steers like every modelled aeroplane - RollingSteer, not the FChassis default Pivot"),
		Fixture.Chassis.SteerLaw, ESteerLaw::RollingSteer);
	TestEqual(TEXT("with the Meridian's measured wheelbase"), Fixture.Chassis.FixedAxleX, -237.8, 1e-9);
	return true;
}

#endif
