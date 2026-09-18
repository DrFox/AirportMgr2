#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Every value of ESource, so a test can walk the enum rather than list it per assertion. */
	const TArray<SnapGuide::ESource>& EverySource()
	{
		static const TArray<SnapGuide::ESource> All = {
			SnapGuide::ESource::Extending, SnapGuide::ESource::PointAlign,
			SnapGuide::ESource::Aligned,   SnapGuide::ESource::Collinear,
			SnapGuide::ESource::Parallel,  SnapGuide::ESource::Runway,
			SnapGuide::ESource::World,     SnapGuide::ESource::Offset };
		return All;
	}
}

/**
 * EVERY SOURCE HAS A FLAG, AND NO TWO SHARE ONE. A switch with a case missing returns false
 * for that source, which reads on screen as a guide that simply never fires - and a case
 * copied from its neighbour makes one button control two guides. Both are silent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSettingsGiveEverySourceItsOwnFlagTest,
	"Airside.Tool.GuideSettingsGiveEverySourceItsOwnFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSettingsGiveEverySourceItsOwnFlagTest::RunTest(const FString& Parameters)
{
	// THE ENUM'S OWN SIZE, checked against this file's list, so a source added to ESource is
	// caught here rather than quietly skipped by every loop below.
	TestEqual(TEXT("this test's table covers every ESource value"),
		EverySource().Num(), static_cast<int32>(SnapGuide::ESource::Offset) + 1);

	for (const SnapGuide::ESource Source : EverySource())
	{
		// A FRESH STRUCT EACH TIME, so the defaults - whatever they are - cannot mask a
		// shared flag.
		FSnapGuideSettings Settings;
		const bool Before = Settings.IsEnabled(Source);

		Settings.Toggle(Source);
		TestNotEqual(
			*FString::Printf(TEXT("source %d has a flag Toggle actually moves"),
				static_cast<int32>(Source)),
			Settings.IsEnabled(Source), Before);

		for (const SnapGuide::ESource Other : EverySource())
		{
			if (Other == Source) { continue; }
			TestEqual(
				*FString::Printf(TEXT("and toggling %d leaves %d alone"),
					static_cast<int32>(Source), static_cast<int32>(Other)),
				Settings.IsEnabled(Other), FSnapGuideSettings().IsEnabled(Other));
		}
	}

	// THE DEFAULTS THE DESIGN ASKED FOR (section 7, amended for PointAlign in stage 3): the
	// ones that fire most often are on, and a player meeting every guide at once learns
	// nothing.
	const FSnapGuideSettings Defaults;
	TestTrue(TEXT("Extending is on by default"),
		Defaults.IsEnabled(SnapGuide::ESource::Extending));
	TestTrue(TEXT("PointAlign is on, being the gesture's own geometry"),
		Defaults.IsEnabled(SnapGuide::ESource::PointAlign));
	TestTrue(TEXT("Parallel is on"), Defaults.IsEnabled(SnapGuide::ESource::Parallel));
	TestTrue(TEXT("World is on"), Defaults.IsEnabled(SnapGuide::ESource::World));
	TestFalse(TEXT("Aligned is off"), Defaults.IsEnabled(SnapGuide::ESource::Aligned));
	TestFalse(TEXT("Collinear is off"), Defaults.IsEnabled(SnapGuide::ESource::Collinear));
	TestFalse(TEXT("Runway is off"), Defaults.IsEnabled(SnapGuide::ESource::Runway));
	TestFalse(TEXT("and Offset is off, since nothing proposes it yet"),
		Defaults.IsEnabled(SnapGuide::ESource::Offset));

	return true;
}

/**
 * A SOURCE THAT IS OFF PROPOSES NOTHING, and the guide the player was getting from it stops.
 * Driven through the CHAIN rather than the settings, because the settings agreeing with
 * themselves proves nothing about whether anything reads them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainSkipsADisabledSourceTest,
	"Airside.Tool.GuideChainSkipsADisabledSource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainSkipsADisabledSourceTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// An east-west taxiway, and a drag beside it running almost along it.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 2000.0);
	const FVector2D Cursor(3000.0, 2100.0);

	const FSnapGuideChain Chain;

	// PARALLEL ON, everything else off: the road is the only thing that can answer.
	FSnapGuideSettings Settings;
	Settings.bParallel = true;
	Settings.bWorld = false;
	Settings.bExtending = false;
	Settings.bPointAlign = false;

	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with Parallel on, the nearest road offers a guide"), On.bActive))
	{
		return false;
	}
	TestEqual(TEXT("and it is Parallel that offered it"),
		static_cast<int32>(On.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::Parallel));

	// PARALLEL OFF: nothing else is on, so nothing answers at all. That is the assertion the
	// whole toggle exists for.
	Settings.bParallel = false;
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with Parallel off, the same drag is offered nothing"), Off.bActive);

	// CONTROL LEG: the drag itself was fine - switch World on and a guide returns. Without
	// this, a Resolve that had simply broken would pass the assertion above.
	Settings.bWorld = true;
	const SnapGuide::FResult World = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestTrue(TEXT("and another source switched on still answers"), World.bActive);
	TestEqual(TEXT("from the world grid this time"),
		static_cast<int32>(World.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

/**
 * ALT MEANS NOT THIS TIME. Toggles are for "I never want this"; without a hold the player
 * fights the guide for a position it will not give them.
 *
 * Driven through FBuildSession::MakeContext, because that is where both drivers meet and the
 * only place the flag can be shown to reach the resolution at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSuspendsOnHoldTest,
	"Airside.Tool.GuideSuspendsOnHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSuspendsOnHoldTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(20000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);

	// The depot tool BY ID, never by a literal index - the next tool added moves every index.
	int32 Depot = INDEX_NONE;
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		if (Registry[Index].Id == FName(TEXT("FuelDepot"))) { Depot = Index; }
	}
	if (!TestTrue(TEXT("the registry lists a fuel-depot tool"), Depot != INDEX_NONE))
	{
		return false;
	}

	FBuildSession Session;
	const FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);
	Session.SelectTool(Depot);
	IBuildTool* Tool = Session.GetActiveTool();
	if (!TestNotNull(TEXT("the depot tool is active"), Tool)) { return false; }

	// Anchor beside the road and run the frontage east, so a back corner has a guide to get.
	Tool->OnClick(Session.MakeContext(Actor, FVector2D(0.0, 1000.0), Tunables, false, false));
	Tool->OnClick(Session.MakeContext(Actor, FVector2D(6000.0, 1000.0), Tunables, false, false));

	TArray<FVector2D> Frontage;
	static_cast<FPlotPlaceTool*>(Tool)->Quad(
		Session.MakeContext(Actor, FVector2D(6000.0, 3000.0), Tunables, false, false), Frontage);
	if (!TestTrue(TEXT("two corners are pinned"), Frontage.Num() >= 2)) { return false; }

	// A corner dragged near square: with Alt up this is exactly the case stage 1 guides.
	const FVector2D NearSquare = Frontage[1] + FVector2D(60.0, 2000.0);

	const FToolContext Free = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, false);
	if (!TestTrue(TEXT("with Alt up, the corner is guided"), Free.Guide.bActive))
	{
		return false;
	}

	const FToolContext Held = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, true);
	TestFalse(TEXT("with Alt held, the same drag is offered nothing"), Held.Guide.bActive);
	TestTrue(TEXT("and the raw cursor is what the tool would use"),
		Held.GuidedCursor().Equals(NearSquare, 1.0e-6));

	// RELEASING ALT STARTS AFRESH rather than resuming the winner it was holding: the suspended
	// frame cleared LastGuide, so this is the hysteresis rule being handed an empty previous.
	const FToolContext Released = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, false);
	TestTrue(TEXT("and releasing Alt gives the guide back"), Released.Guide.bActive);

	return true;
}

#endif
