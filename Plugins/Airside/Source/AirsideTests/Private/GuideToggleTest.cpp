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
	/** Every ERelation, so a test can walk the enum rather than list it per assertion. */
	const TArray<SnapGuide::ERelation>& EveryRelation()
	{
		static const TArray<SnapGuide::ERelation> All = {
			SnapGuide::ERelation::Extending, SnapGuide::ERelation::LevelWith,
			SnapGuide::ERelation::Parallel,  SnapGuide::ERelation::Collinear,
			SnapGuide::ERelation::MatchingGap };
		return All;
	}

	/** Every EReference that HAS a flag - ThisGesture has none, deliberately. Design section 7. */
	const TArray<SnapGuide::EReference>& EverySwitchableReference()
	{
		static const TArray<SnapGuide::EReference> All = {
			SnapGuide::EReference::Road,  SnapGuide::EReference::Runway,
			SnapGuide::EReference::Apron, SnapGuide::EReference::Stand,
			SnapGuide::EReference::World };
		return All;
	}
}

/**
 * EVERY AXIS HAS A FLAG, AND NO TWO SHARE ONE. A switch with a case missing returns false for
 * that row or column, which reads on screen as a guide that simply never fires - and a case
 * copied from its neighbour makes one button control two. Both are silent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSettingsGiveEveryAxisItsOwnFlagTest,
	"Airside.Tool.GuideSettingsGiveEveryAxisItsOwnFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSettingsGiveEveryAxisItsOwnFlagTest::RunTest(const FString& Parameters)
{
	// EACH ENUM'S OWN SIZE, checked against this file's lists, so a row or column added is
	// caught here rather than quietly skipped by every loop below. The reference list is one
	// SHORT of its enum, because ThisGesture has no flag - see FSnapGuideSettings::IsReferenceOn.
	TestEqual(TEXT("this test's table covers every ERelation value"),
		EveryRelation().Num(), static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1);
	TestEqual(TEXT("and every EReference but ThisGesture"),
		EverySwitchableReference().Num(), static_cast<int32>(SnapGuide::EReference::World));

	for (const SnapGuide::ERelation Relation : EveryRelation())
	{
		// A FRESH STRUCT EACH TIME, so the defaults - whatever they are - cannot mask a
		// shared flag.
		FSnapGuideSettings Settings;
		const bool Before = Settings.IsRelationOn(Relation);

		Settings.ToggleRelation(Relation);
		TestNotEqual(
			*FString::Printf(TEXT("relation %d has a flag ToggleRelation actually moves"),
				static_cast<int32>(Relation)),
			Settings.IsRelationOn(Relation), Before);

		for (const SnapGuide::ERelation Other : EveryRelation())
		{
			if (Other == Relation) { continue; }
			TestEqual(
				*FString::Printf(TEXT("and toggling relation %d leaves %d alone"),
					static_cast<int32>(Relation), static_cast<int32>(Other)),
				Settings.IsRelationOn(Other), FSnapGuideSettings().IsRelationOn(Other));
		}

		// AND NEITHER AXIS REACHES THE OTHER. One switch statement per axis is two chances to
		// paste a case from the wrong list, and nothing else would notice.
		for (const SnapGuide::EReference Column : EverySwitchableReference())
		{
			TestEqual(
				*FString::Printf(TEXT("toggling relation %d leaves column %d alone"),
					static_cast<int32>(Relation), static_cast<int32>(Column)),
				Settings.IsReferenceOn(Column), FSnapGuideSettings().IsReferenceOn(Column));
		}
	}

	for (const SnapGuide::EReference Reference : EverySwitchableReference())
	{
		FSnapGuideSettings Settings;
		const bool Before = Settings.IsReferenceOn(Reference);

		Settings.ToggleReference(Reference);
		TestNotEqual(
			*FString::Printf(TEXT("reference %d has a flag ToggleReference actually moves"),
				static_cast<int32>(Reference)),
			Settings.IsReferenceOn(Reference), Before);

		for (const SnapGuide::EReference Other : EverySwitchableReference())
		{
			if (Other == Reference) { continue; }
			TestEqual(
				*FString::Printf(TEXT("and toggling reference %d leaves %d alone"),
					static_cast<int32>(Reference), static_cast<int32>(Other)),
				Settings.IsReferenceOn(Other), FSnapGuideSettings().IsReferenceOn(Other));
		}
	}

	// THISGESTURE IS ALWAYS ON AND CANNOT BE MOVED. Extending is the only cell in its row, so
	// a button for this column would switch off exactly what the Extending button does.
	FSnapGuideSettings Fixed;
	Fixed.ToggleReference(SnapGuide::EReference::ThisGesture);
	TestTrue(TEXT("ThisGesture stays on however hard it is toggled"),
		Fixed.IsReferenceOn(SnapGuide::EReference::ThisGesture));

	// THE DEFAULTS THE DESIGN ASKED FOR: the ones that fire most often are on, and a player
	// meeting every guide at once learns nothing.
	const FSnapGuideSettings Defaults;
	TestTrue(TEXT("Extending is on by default"),
		Defaults.IsRelationOn(SnapGuide::ERelation::Extending));
	TestTrue(TEXT("LevelWith is on, being the gesture's own geometry"),
		Defaults.IsRelationOn(SnapGuide::ERelation::LevelWith));
	TestTrue(TEXT("Parallel is on"), Defaults.IsRelationOn(SnapGuide::ERelation::Parallel));
	TestFalse(TEXT("Collinear is off"), Defaults.IsRelationOn(SnapGuide::ERelation::Collinear));
	TestFalse(TEXT("and MatchingGap is off, being the least familiar"),
		Defaults.IsRelationOn(SnapGuide::ERelation::MatchingGap));

	TestTrue(TEXT("Road is on: it is what a taxiway is usually drawn against"),
		Defaults.IsReferenceOn(SnapGuide::EReference::Road));
	TestTrue(TEXT("World is on"), Defaults.IsReferenceOn(SnapGuide::EReference::World));
	TestFalse(TEXT("Runway is off"), Defaults.IsReferenceOn(SnapGuide::EReference::Runway));
	TestFalse(TEXT("Apron is off"), Defaults.IsReferenceOn(SnapGuide::EReference::Apron));
	TestFalse(TEXT("and Stand is off"), Defaults.IsReferenceOn(SnapGuide::EReference::Stand));

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
	Settings.bLevelWith = false;

	// THE ROAD COLUMN STAYS ON. Since 2026-09-20 a relation alone does not offer anything -
	// without this the road is gated off by its column and the test would pass for the wrong
	// reason, proving only that two switches beat one.
	Settings.bRoad = true;

	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with Parallel on, the nearest road offers a guide"), On.bActive))
	{
		return false;
	}
	TestEqual(TEXT("and it is Parallel that offered it"),
		static_cast<int32>(On.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Parallel));
	TestEqual(TEXT("against the road, which is the only column with anything in it"),
		static_cast<int32>(On.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Road));

	// THE ROW OFF: nothing else is on, so nothing answers at all.
	Settings.bParallel = false;
	const SnapGuide::FResult NoRow = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Parallel row off, the same drag is offered nothing"),
		NoRow.bActive);

	// THE COLUMN OFF, THE ROW BACK ON: also nothing, and this is the half that did not exist
	// before 2026-09-20. The road is still the nearest thing to the drag and Parallel is still
	// switched on - what stops it is that the player asked not to be lined up with roads.
	Settings.bParallel = true;
	Settings.bRoad = false;
	const SnapGuide::FResult NoColumn = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Road column off, the road offers nothing either"),
		NoColumn.bActive);

	// CONTROL LEG: the drag itself was fine - switch a different COLUMN on and a guide
	// returns. Without this, a Resolve that had simply broken would pass both assertions above.
	//
	// THE ROW HAS TO BE ON FOR IT. A world axis is a Parallel guide - it gives a direction and
	// nothing else - so switching the Parallel row off takes the world grid with it. That is
	// not a quirk of the gate: it is what makes World a column rather than a source, and the
	// first draft of this leg left the row off and read Winners[0] off the end of an empty
	// array for it.
	Settings.bWorld = true;
	const SnapGuide::FResult World = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("and another column switched on still answers"), World.bActive))
	{
		return false;
	}
	TestEqual(TEXT("from the world grid this time"),
		static_cast<int32>(World.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::World));

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

/**
 * THE REPORT, AS A TEST. 2026-09-20: "i have parallel on and runway off [and] the guide will
 * still show me that my taxiway is parallel to a runway". A column switched off must silence
 * every relation in it, not just the one relation that happened to be named after it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayColumnOffSilencesEveryRelationTest,
	"Airside.Tool.RunwayColumnOffSilencesEveryRelation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayColumnOffSilencesEveryRelationTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A RUNWAY AS THE ONLY THING ON THE FIELD, so anything the chain offers must reference it.
	//
	// NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a runway is not a
	// road kind - it is a segment laid through PlaceRunway with a profile that is continuous
	// through junctions, which is what IsRunwaySegment then recognises.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-20000.0, 0.0), FVector2D(20000.0, 0.0))))
	{
		// HONOURED, NOT ASSUMED. PlaceRunway can refuse, and every assertion below would then
		// be measuring an empty field while looking like a gating bug.
		return false;
	}
	if (!TestTrue(TEXT("and the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 3000.0);
	const FVector2D Cursor(4000.0, 3100.0);
	const FSnapGuideChain Chain;

	// EVERY RELATION ON, THE RUNWAY COLUMN OFF. Road stays on so the test cannot pass merely
	// by everything being switched off.
	FSnapGuideSettings Settings;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bMatchingGap = true;
	Settings.bRoad = true;
	Settings.bRunway = false;
	Settings.bWorld = false;

	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	for (const SnapGuide::FCandidate& Winner : Off.Winners)
	{
		TestNotEqual(TEXT("with Runway off, no relation may reference a runway"),
			static_cast<int32>(Winner.Reference),
			static_cast<int32>(SnapGuide::EReference::Runway));
	}

	// CONTROL LEG: the drag itself was fine. Without this, a Resolve that had simply broken
	// would pass the assertion above.
	Settings.bRunway = true;
	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with Runway on, the runway answers"), On.bActive)) { return false; }
	TestEqual(TEXT("and what it offers is the runway"),
		static_cast<int32>(On.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Runway));

	return true;
}

#endif
