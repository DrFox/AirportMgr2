#include "CoreMinimal.h"
#include "BuildActions.h"
#include "BuildHudLayer.h"
#include "Entities/AircraftType.h"
#include "LandAircraftPanelWidget.h"
#include "LandChoices.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Testing/AirsideTestGraph.h"
#include "Model/OpsAlerts.h"
#include "Model/RunwayFacts.h"
#include "Content/AirsideSettings.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** A straight runway W -> E of Length and TotalWidth - RunwayAdmissionTest's shape. */
	void LandPanelMakeRunway(URoadNetwork& Net, double Length, double TotalWidth)
	{
		URoadProfile* Runway = URoadProfile::MakeTransient(TotalWidth, 1500.0, TotalWidth * 0.1);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId A = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId B = Net.AddNode(FVector2D(Length, 0.0));
		Net.AddStraightSegment(A, B, Runway);
	}

	const FLandChoice* LandPanelFind(const TArray<FLandChoice>& Choices, const TCHAR* AssetName)
	{
		return Choices.FindByPredicate([AssetName](const FLandChoice& Choice)
		{
			return Choice.Type != nullptr && Choice.Type->GetName() == AssetName;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandChoicesListEveryMeshedTypeTest,
	"AirportMgr.UI.LandChoicesListEveryMeshedType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandChoicesListEveryMeshedTypeTest::RunTest(const FString& Parameters)
{
	// THE PANEL'S LIST IS THE CONTENT'S, not a table here. Eighteen meshed types on
	// 2026-09-27; a floor rather than an exact count, so adding plane19 does not turn this red.
	const TArray<UAircraftType*> Types = LandChoices::EveryMeshedType();
	TestTrue(FString::Printf(TEXT("every meshed aircraft type is offered (%d found)"), Types.Num()),
		Types.Num() >= 18);
	TestTrue(TEXT("and no paper type - the paper A320 and 737 have no model to watch land"),
		!Types.ContainsByPredicate([](const UAircraftType* Type) { return Type->Mesh.IsNull(); }));

	const TArray<FLandChoice> Choices = LandChoices::Build(nullptr, FVector2D::ZeroVector, Types);
	TestEqual(TEXT("one choice per type"), Choices.Num(), Types.Num());

	// SORTED BY LETTER, THEN NAME, so the list reads small to large - the order a player
	// scanning for "something that fits my strip" wants.
	for (int32 Index = 1; Index < Choices.Num(); ++Index)
	{
		const FString Prev = Choices[Index - 1].Type->Code.ToString();
		const FString Here = Choices[Index].Type->Code.ToString();
		TestTrue(FString::Printf(TEXT("row %d's letter %s does not come before row %d's %s"),
			Index, *Here, Index - 1, *Prev), Prev <= Here);
	}

	if (const FLandChoice* A320 = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane9")))
	{
		TestEqual(TEXT("a row names its letter and its aeroplane"),
			A320->Label.ToString(), FString(TEXT("C · A320-200")));
	}
	else
	{
		AddError(TEXT("the A320 (DA_Aircraft_Plane9) is in the list"));
	}

	// NO NETWORK, NOTHING LANDS - and the row says why rather than just greying.
	for (const FLandChoice& Choice : Choices)
	{
		TestFalse(*FString::Printf(TEXT("%s is not landable with no runway"), *Choice.Label.ToString()),
			Choice.bAdmitted);
	}
	if (Choices.Num() > 0)
	{
		TestFalse(TEXT("and carries a reason"), Choices[0].Refusal.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandChoicesGreyWhatTheRunwayRefusesTest,
	"AirportMgr.UI.LandChoicesGreyWhatTheRunwayRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandChoicesGreyWhatTheRunwayRefusesTest::RunTest(const FString& Parameters)
{
	// A 600 m, 45 m strip: long enough for the Meridian (510 / 470), far too short for the
	// A380 (3000 / 2050). Asked through RunwayAdmission - the check the arrival itself makes -
	// so a greyed row is exactly a click that would have been refused.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LandPanelMakeRunway(*Net, 60000.0, 4500.0);

	const TArray<FLandChoice> Choices =
		LandChoices::Build(Net, FVector2D::ZeroVector, LandChoices::EveryMeshedType());
	const FLandChoice* Meridian = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane7"));
	const FLandChoice* A380 = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane8"));
	if (!TestNotNull(TEXT("the Meridian is listed"), Meridian) || !TestNotNull(TEXT("the A380 is listed"), A380))
	{
		return false;
	}

	TestTrue(TEXT("the Meridian can land on 600 m"), Meridian->bAdmitted);
	TestTrue(TEXT("with no refusal"), Meridian->Refusal.IsEmpty());
	TestFalse(TEXT("the A380 cannot"), A380->bAdmitted);
	TestTrue(FString::Printf(TEXT("and the row says it is the length, in metres: \"%s\""), *A380->Refusal),
		A380->Refusal.Contains(TEXT("short")) && A380->Refusal.Contains(TEXT(" m")));

	// THE NEAREST RUNWAY, NOT ANY RUNWAY. A 3.5 km strip 5 km north: the arrival planner lands
	// at the threshold nearest the view focus and does not fall back, so while the view is on
	// the short strip the A380 stays grey - and looking at the long one un-greys it.
	const FRoadNodeId FarA = Net->AddNode(FVector2D(0.0, 500000.0));
	const FRoadNodeId FarB = Net->AddNode(FVector2D(350000.0, 500000.0));
	URoadProfile* Long = URoadProfile::MakeTransient(6000.0, 1500.0, 600.0);
	Long->bContinuousThroughJunctions = true;
	Net->AddStraightSegment(FarA, FarB, Long);

	const TArray<FLandChoice> AtShort = LandChoices::Build(Net, FVector2D::ZeroVector, LandChoices::EveryMeshedType());
	const TArray<FLandChoice> AtLong = LandChoices::Build(Net, FVector2D(0.0, 500000.0), LandChoices::EveryMeshedType());
	const FLandChoice* A380AtShort = LandPanelFind(AtShort, TEXT("DA_Aircraft_Plane8"));
	const FLandChoice* A380AtLong = LandPanelFind(AtLong, TEXT("DA_Aircraft_Plane8"));
	if (TestNotNull(TEXT("listed at both"), A380AtShort) && TestNotNull(TEXT("listed at both "), A380AtLong))
	{
		TestFalse(TEXT("a long runway elsewhere does not un-grey it while the view is on the short one"),
			A380AtShort->bAdmitted);
		TestTrue(FString::Printf(TEXT("but looking at the long one does (%s)"), *A380AtLong->Refusal),
			A380AtLong->bAdmitted);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandChoicesGreyWhatCannotLeaveTest,
	"AirportMgr.UI.LandChoicesGreyWhatCannotLeave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandChoicesGreyWhatCannotLeaveTest::RunTest(const FString& Parameters)
{
	// THE REPORTED STRIP, 404 m: the SR22 lands in 390 m but needs 430 m to leave (2026-09-27).
	// Greyed, with the departure named - not offered as a click that strands the aircraft.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LandPanelMakeRunway(*Net, 40366.0, 2300.0);
	const TArray<FLandChoice> Choices =
		LandChoices::Build(Net, FVector2D::ZeroVector, LandChoices::EveryMeshedType());
	const FLandChoice* Sr22 = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane15"));
	if (!TestNotNull(TEXT("the SR22 is listed"), Sr22)) { return false; }
	TestFalse(TEXT("the SR22 cannot land where it cannot take off again"), Sr22->bAdmitted);
	TestTrue(FString::Printf(TEXT("and the row says it is the take-off: \"%s\""), *Sr22->Refusal),
		Sr22->Refusal.Contains(TEXT("take off")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelRowLandsItsTypeTest,
	"AirportMgr.UI.LandPanelRowLandsItsType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelRowLandsItsTypeTest::RunTest(const FString& Parameters)
{
	// THE SEAM, AT THE COMPOSITION: a real controller and the real panel, a click on a row,
	// and the controller asked to land THAT type. A panel whose buttons were built and never
	// bound would pass every list test above.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// Against the WORLD: CreateWidget refuses a controller with no local player, and this one
	// is a bare SpawnActor - see ClickRowForTest for how the click still reaches it.
	ULandAircraftPanelWidget* Panel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel builds with no asset"), Panel)) { return false; }

	TestFalse(TEXT("hidden until asked for"), Panel->IsShowing());
	Panel->Toggle();
	TestTrue(TEXT("open after one toggle"), Panel->IsShowing());
	TestEqual(TEXT("one row widget per meshed type"), Panel->RowWidgetCountForTest(),
		LandChoices::EveryMeshedType().Num());

	const int32 Row = Panel->RowIndexOfForTest(TEXT("DA_Aircraft_Plane8"));
	if (!TestTrue(TEXT("the A380 has a row"), Row != INDEX_NONE)) { return false; }
	// BOTH HALVES OF THE CLICK: the button is bound to its row, and the row lands its type.
	// The first alone passes a row that lands the wrong aeroplane; the second alone passes a
	// button nothing is listening to.
	TestTrue(TEXT("the A380 row's button is bound to its row"), Panel->IsRowBoundForTest(Row));
	Panel->ClickRowForTest(Row, *C);

	const UAircraftType* Asked = C->GetLastLandRequestForTest();
	TestTrue(FString::Printf(TEXT("the click asked the controller to land the A380 (asked: %s)"),
		Asked != nullptr ? *Asked->GetName() : TEXT("nothing")),
		Asked != nullptr && Asked->GetName() == TEXT("DA_Aircraft_Plane8"));
	TestTrue(TEXT("and the panel stays open, so several can be queued"), Panel->IsShowing());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelIsKeySevenTest,
	"AirportMgr.Actions.LandPanelIsKeySeven",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelIsKeySevenTest::RunTest(const FString& Parameters)
{
	// THROUGH THE ONE ACTION LIST, as the ledger is: the bar's Land button and key 7 are one
	// row, so they cannot disagree about whether Land opens the panel or lands a default.
	const FBuildAction* Land = FindAction(FName(TEXT("aircraft.land")));
	if (!TestNotNull(TEXT("Land is in the one action list"), Land)) { return false; }
	TestEqual(TEXT("on 7"), Land->Key, EKeys::Seven);

	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHudForTest()))
	{
		return false;
	}
	// A headless controller has no local player, so BeginPlay never created the HUD's
	// widgets; the panel is put where CreateAll would have put it.
	C->GetHudForTest()->LandPanel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());

	FBuildActionContext Ctx(*C);
	Land->Execute(Ctx);
	TestTrue(TEXT("pressing Land opens the panel rather than landing a default"), C->IsLandPanelShowing());
	TestTrue(TEXT("and the bar button lights while it is open"), Land->IsActive(Ctx));
	Land->Execute(Ctx);
	TestFalse(TEXT("pressing it again closes it"), C->IsLandPanelShowing());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelBuildsOnlyOnChangeTest,
	"AirportMgr.UI.LandPanelBuildsOnlyOnChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelBuildsOnlyOnChangeTest::RunTest(const FString& Parameters)
{
	// OPS BATCH 3 PR E: the rows are judged again only when something LandChoices::Build reads moves - one step per
	// input of FLandChoicesKey, each alone. It judged every type every frame while open.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	const FTestTwoRunways Field = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe(), Actor->Network);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	ULandAircraftPanelWidget* Panel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }

	auto FocusAt = [C](const FVector2D& At)
	{
		FAlertFocus Focus;
		Focus.Kind = EAlertFocusKind::Point;
		Focus.Point = At;
		C->SelectAndFocus(Focus);
	};
	auto Builds = [&](int32 Frames)
	{
		const int32 Before = Panel->BuildCountForTest();
		for (int32 Frame = 0; Frame < Frames; ++Frame) { Panel->RefreshFor(C); }
		return Panel->BuildCountForTest() - Before;
	};

	FocusAt(FVector2D(1000.0, 0.0));
	TestEqual(TEXT("opened on runway A: judged once over 30 frames"), Builds(30), 1);
	FocusAt(FVector2D(1100.0, 50.0));
	TestEqual(TEXT("the camera moves along A: nothing Build reads moved"), Builds(5), 0);
	FocusAt(FVector2D(1000.0, -40000.0));
	TestEqual(TEXT("the camera onto runway B: judged again, once"), Builds(5), 1);

	const FRoadSegment* Piece = Actor->Network->GetSegment(Field.B);
	if (!TestNotNull(TEXT("runway B"), Piece)) { return false; }
	const uint32 Guideline = Actor->Network->GetGuidelineRevision();
	Actor->Network->SetNodePosition(Piece->A, Actor->Network->GetNode(Piece->A)->Position - FVector2D(20000.0, 0.0));
	TestEqual(TEXT("(a drag moves no guideline revision)"), Actor->Network->GetGuidelineRevision(), Guideline);
	TestEqual(TEXT("B dragged longer (EditRevision): judged again, once"), Builds(5), 1);

	const uint32 Edit = Actor->Network->GetEditRevision();
	FRunwayFacts Facts = Actor->Network->RunwayFactsFor(Field.B);
	Facts.Surface = EPavement::Grass;
	if (!TestTrue(TEXT("the facade takes B's facts"), Actor->SetRunwayFacts(Field.B.Index, Facts))) { return false; }
	TestEqual(TEXT("(facts move no edit revision)"), Actor->Network->GetEditRevision(), Edit);
	TestEqual(TEXT("B turned to grass (GuidelineRevision): judged again, once"), Builds(5), 1);

	Actor->ClearNetwork();
	TestEqual(TEXT("a new network: judged again, once"), Builds(5), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelJudgesWhenTypesArriveTest,
	"AirportMgr.UI.LandPanelJudgesWhenTypesArrive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelJudgesWhenTypesArriveTest::RunTest(const FString& Parameters)
{
	// THE TYPES ARE AN INPUT TOO (PR E review): read again every Refresh while there are none, they can arrive with
	// nothing FLandChoicesKey reads moving - and the rows must appear when they do.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	ULandAircraftPanelWidget* Panel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	TArray<UAircraftType*> Two = LandChoices::EveryMeshedType();
	if (!TestTrue(TEXT("content has at least two meshed types"), Two.Num() >= 2)) { return false; }
	Two.SetNum(2);
	bool bArrived = false;
	Panel->SetTypeSourceForTest([&bArrived, Two]() { return bArrived ? Two : TArray<UAircraftType*>(); });

	for (int32 Frame = 0; Frame < 3; ++Frame) { Panel->RefreshFor(C); }
	TestEqual(TEXT("no types: judged once"), Panel->BuildCountForTest(), 1);
	TestEqual(TEXT("and no rows"), Panel->RowWidgetCountForTest(), 0);
	bArrived = true;
	Panel->RefreshFor(C);
	TestEqual(TEXT("the types arrive: judged again"), Panel->BuildCountForTest(), 2);
	TestEqual(TEXT("and their rows are there"), Panel->RowWidgetCountForTest(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandChoicesKeyNamesTheNetworkTest,
	"AirportMgr.UI.LandChoicesKeyNamesTheNetwork",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandChoicesKeyNamesTheNetworkTest::RunTest(const FString& Parameters)
{
	// TWO NETWORKS, EVERY REVISION EQUAL: two deterministic builds of one field count their revisions identically, so
	// only the network's identity tells a key on one from a key on the other - a load's new network looked like this.
	const FTestTwoRunways First = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe());
	const FTestTwoRunways Second = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe());
	if (!TestTrue(TEXT("the same revisions - the case under test"),
		First.Net->GetEditRevision() == Second.Net->GetEditRevision()
		&& First.Net->GetGuidelineRevision() == Second.Net->GetGuidelineRevision())) { return false; }
	const FVector2D Near(1000.0, 0.0);
	const FLandChoicesKey A = LandChoices::KeyFor(First.Net, Near);
	const FLandChoicesKey B = LandChoices::KeyFor(Second.Net, Near);
	TestTrue(TEXT("the same runway seed on each"), A.bHasRunway && B.bHasRunway && A.Seed == B.Seed);
	TestTrue(TEXT("and still two keys: the network is on it"), A != B);
	TestTrue(TEXT("one network is one key"), A == LandChoices::KeyFor(First.Net, Near));
	return true;
}

#endif
