#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BuildActions.h"
#include "RoadBuildHUD.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PANEL'S CONTENT, with no Canvas and no PIE.
 *
 * The drawing cannot be tested headlessly; what CAN go wrong silently is what it says - a
 * panel that never mentions Build, one that offers it before the shape is finished, or one
 * that quietly drops the warning that would have changed the player's mind.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPanelShowsProgressAndBuildTest,
	"AirportMgr.HUD.PlotPanelShowsProgressAndBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPanelShowsProgressAndBuildTest::RunTest(const FString& Parameters)
{
	// NOTHING TO SAY, NOTHING DRAWN. A panel hanging over an idle cursor is clutter the
	// player cannot dismiss.
	TestEqual(TEXT("an empty readout draws no panel"),
		ARoadBuildHUD::PanelLines(FToolReadout()).Num(), 0);

	FToolReadout Mid;
	Mid.Facts.Emplace(TEXT("Plot Points"), TEXT("2/4"));
	Mid.Facts.Emplace(TEXT("Frontage"), TEXT("20 m"));
	Mid.bCommittable = false;

	const TArray<FString> MidLines = ARoadBuildHUD::PanelLines(Mid);
	TestTrue(TEXT("the panel reports progress through the gesture"),
		MidLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("2/4")); }));
	TestFalse(TEXT("and does not offer Build before the shape is finished"),
		MidLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("Build")); }));

	FToolReadout Ready = Mid;
	Ready.bCommittable = true;
	const TArray<FString> ReadyLines = ARoadBuildHUD::PanelLines(Ready);

	TestTrue(TEXT("a committable gesture is offered Build, by name"),
		ReadyLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("Build")); }));

	// THE KEY COMES FROM THE REGISTRY, through CommitPromptText - so a rebound Build cannot
	// leave the panel advertising a key that does nothing.
	const FBuildAction* Build = FindAction(FName(TEXT("edit.build")));
	if (!TestNotNull(TEXT("a Build action"), Build)) { return false; }
	TestTrue(TEXT("and the key it names is the one the registry bound"),
		ReadyLines.ContainsByPredicate([Build](const FString& L)
		{
			return L.Contains(Build->Key.GetDisplayName().ToString());
		}));

	// WARNINGS SURVIVE. "No room to grow" is the one line that changes a decision, and a
	// panel that dropped it would be a readout which only ever reports good news.
	FToolReadout Warned = Ready;
	Warned.Warnings.Add(TEXT("No room to grow"));
	TestTrue(TEXT("a warning reaches the panel"),
		ARoadBuildHUD::PanelLines(Warned).ContainsByPredicate(
			[](const FString& L) { return L.Contains(TEXT("No room to grow")); }));

	return true;
}


/**
 * PINNED AND PROVISIONAL MUST READ APART, because that is their whole job: one edge of the
 * plot has stopped moving and the other has not, and the player counts corners by the
 * difference. Identical looks would leave a dashed boundary reading as decoration - which is
 * the verdict the bay marks this replaces actually earned in PIE.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPinnedAndProvisionalReadApartTest,
	"AirportMgr.HUD.PinnedAndProvisionalReadApart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPinnedAndProvisionalReadApartTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildHUD* Hud = TestWorld.World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	const FPreviewLook& Pinned = Hud->LookForTest(EPreviewStyle::Pinned);
	const FPreviewLook& Provisional = Hud->LookForTest(EPreviewStyle::Provisional);

	TestTrue(TEXT("pinned has a positive thickness"), Pinned.ThicknessScale > 0.0f);
	TestTrue(TEXT("provisional has a positive thickness"), Provisional.ThicknessScale > 0.0f);

	// SAME WEIGHT, so the DASH is what tells them apart rather than a thickness the player
	// would have to compare against some other line elsewhere on screen.
	TestEqual(TEXT("both are drawn at the same weight"),
		Pinned.ThicknessScale, Provisional.ThicknessScale);

	TestTrue(TEXT("and the hud dashes one of them and not the other"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Provisional)
			&& !ARoadBuildHUD::IsDashed(EPreviewStyle::Pinned));

	return true;
}


/**
 * THE COMMIT PROMPT'S ONE DECISION, with no Canvas and no PIE.
 *
 * The drawing itself cannot be tested headlessly, so the part that CAN go wrong silently is
 * split out: whether the prompt appears at all, and whether it names the key the registry
 * actually bound. A prompt reading "Build [Enter]" beside an unbound Enter is this project's
 * most-repeated bug - see CLAUDE.md on the startup banner that advertised four routes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCommitPromptNamesTheBoundKeyTest,
	"AirportMgr.HUD.CommitPromptNamesTheBoundKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCommitPromptNamesTheBoundKeyTest::RunTest(const FString& Parameters)
{
	// NOTHING TO COMMIT, NOTHING OFFERED. bCommittable is false by default for every tool
	// but one, so a prompt that ignored it would hang over every gesture in the game.
	TestTrue(TEXT("an empty readout offers nothing"),
		ARoadBuildHUD::CommitPromptText(FToolReadout()).IsEmpty());

	FToolReadout Ready;
	Ready.bCommittable = true;
	const FString Prompt = ARoadBuildHUD::CommitPromptText(Ready);

	TestFalse(TEXT("a committable readout offers something"), Prompt.IsEmpty());
	TestTrue(TEXT("and it is the registry's own label"), Prompt.Contains(TEXT("Build")));

	// THE KEY IS REAL, not decoration. FindAction(Key) is what SetupInputComponent binds
	// from, so asking it back is asking whether the key in the prompt actually does anything.
	const FBuildAction* Build = FindAction(FName(TEXT("edit.build")));
	if (!TestNotNull(TEXT("a Build action"), Build)) { return false; }
	if (!TestTrue(TEXT("Build has a key bound"), Build->Key.IsValid())) { return false; }

	TestTrue(TEXT("the prompt names that key"),
		Prompt.Contains(Build->Key.GetDisplayName().ToString()));
	TestEqual(TEXT("and that key reaches the same action when pressed"),
		FindAction(Build->Key, Build->bRequiresCtrl), Build);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildHUDLooksTest,
	"AirportMgr.HUD.LooksCoverEveryStyle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildHUDLooksTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK for Looks (#104). A style Marker/Line/CrossMark/Label draws with no
	// matching entry used to fall through StyleColour's checkNoEntry() crash; now it is
	// LookFor's, and this is what catches a style left out of the constructor's seeding list
	// before a player does, at the first frame that draws it.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildHUD* Hud = TestWorld.World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	// COUNTED THROUGH REFLECTION since 2026-09-26, as FGestureModesAreExclusiveTest counts
	// EGestureMode. This used to walk up to a literal last member (Handle) and its own comment
	// called that bound "the fifth list a new style has to appear in, and the only one nothing
	// else would have caught" - adding ReverseRoute/ReverseGuideline would have meant moving it,
	// or leaving both untested in silence. Now a new style is checked the day it exists.
	const UEnum* Enum = StaticEnum<EPreviewStyle>();
	if (!TestNotNull(TEXT("EPreviewStyle is reflected, so this test can count it"), Enum)) { return false; }
	// NumEnums() includes the generated _MAX sentinel, which is not a style.
	for (int32 S = 0; S < Enum->NumEnums() - 1; ++S)
	{
		const EPreviewStyle Style = static_cast<EPreviewStyle>(Enum->GetValueByIndex(S));
		const FPreviewLook& Look = Hud->LookForTest(Style);
		TestTrue(*FString::Printf(TEXT("style %d has a positive radius scale"), S), Look.RadiusScale > 0.0f);
		TestTrue(*FString::Printf(TEXT("style %d has a positive thickness scale"), S), Look.ThicknessScale > 0.0f);
	}
	return true;
}

/**
 * A GUIDE MUST NOT READ AS A PLOT EDGE. It is drawn from the very corner a Provisional edge
 * ends at, in the same frame, so if the two shared a look the player would see a five-sided
 * plot rather than a four-sided one with an aid attached.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideReadsApartFromProvisionalTest,
	"AirportMgr.HUD.GuideReadsApartFromProvisional",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideReadsApartFromProvisionalTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildHUD* Hud = TestWorld.World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	const FPreviewLook& Guide = Hud->LookForTest(EPreviewStyle::Guide);
	const FPreviewLook& Provisional = Hud->LookForTest(EPreviewStyle::Provisional);

	// SEEDED AT ALL. LookFor falls back rather than crashing, so a style left out of the
	// constructor's list would otherwise pass every assertion below by accident.
	TestTrue(TEXT("the guide style is seeded with a positive thickness"),
		Guide.ThicknessScale > 0.0f);

	TestFalse(TEXT("a guide is not drawn in the plot boundary's colour"),
		Guide.Colour.Equals(Provisional.Colour));
	TestTrue(TEXT("and it is lighter than the edge it helps draw"),
		Guide.ThicknessScale < Provisional.ThicknessScale);

	// DASHED, BOTH - and that is deliberate: the dash says "not settled", which is true of
	// both. The colour is what separates them.
	TestTrue(TEXT("a guide line is dashed"), ARoadBuildHUD::IsDashed(EPreviewStyle::Guide));
	TestTrue(TEXT("as is a provisional edge"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Provisional));
	TestFalse(TEXT("while a pinned edge is solid"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Pinned));

	return true;
}

#endif
