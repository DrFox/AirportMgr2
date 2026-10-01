#include "CoreMinimal.h"
#include "BuildActions.h"
#include "BuildHudLayer.h"
#include "Entities/AircraftType.h"
#include "LandAircraftPanelWidget.h"
#include "LandChoices.h"
#include "OpsRuntimeResolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "HAL/PlatformTime.h"
#include "Testing/AirsideTestWorld.h"
#include "Testing/AirsideTestGraph.h"
#include "Model/OpsAlerts.h"
#include "Model/RunwayFacts.h"
#include "Content/AirsideSettings.h"
#include "Present/RoadNetworkActor.h"
#include "Present/OpsRuntime.h"
#include "Model/Airport.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadTraffic.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"

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

	UAircraftType* LandPanelType(const TCHAR* AssetName)
	{
		for (UAircraftType* Type : LandChoices::EveryMeshedType())
		{
			if (Type->GetName() == AssetName) { return Type; }
		}
		return nullptr;
	}

	/** A runtime attached to Actor and settled - what the widget quotes from in play. */
	UOpsRuntime* LandPanelRuntime(ARoadNetworkActor& Actor)
	{
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(&Actor);
		Runtime->Tick(0.0);
		return Runtime;
	}

	/** Every type's row, quoted by Runtime at Near - the widget's own lambda, minus the widget. */
	TArray<FLandChoice> LandPanelQuoted(const UOpsRuntime& Runtime, const FVector2D& Near)
	{
		return LandChoices::Build(LandChoices::EveryMeshedType(),
			[&Runtime, &Near](const FAirframe& Airframe) { return Runtime.QuoteLanding(Airframe, Near); });
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

	// NO NETWORK: a runtime attached to nothing, which QuoteLanding refuses as LandNear does.
	const UOpsRuntime* Unattached = NewObject<UOpsRuntime>();
	const TArray<FLandChoice> Choices = LandChoices::Build(Types,
		[Unattached](const FAirframe& Airframe) { return Unattached->QuoteLanding(Airframe, FVector2D::ZeroVector); });
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
	// A FIELD SIZED FOR THE MERIDIAN - runway, exit, taxiway, stand - so it lands, and the A380 (3000 / 2050) is far too
	// long for the strip. Quoted through UOpsRuntime::QuoteLanding, the check the click's own accept makes (#432), so a
	// greyed row is exactly a click that would have been refused.
	//
	// "THE NEAREST RUNWAY, NOT ANY RUNWAY" WAS ASSERTED HERE UNTIL #432 - a long strip elsewhere did not un-grey the
	// A380 while the view was on the short one. Stale since #412: the planner lands on whichever runway takes the
	// arrival, so the panel greyed a click the game would take. AirportMgr.UI.LandChoicesAgreeWithThePlanner replaces it.
	UAircraftType* MeridianType = LandPanelType(TEXT("DA_Aircraft_Plane7"));
	if (!TestNotNull(TEXT("the Meridian is content"), MeridianType)) { return false; }
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	const FTestAirport Field = FTestAirport::Build(MeridianType->Airframe(), FTestAirportOptions(), Actor->Network);
	const UOpsRuntime* Runtime = LandPanelRuntime(*Actor);

	const TArray<FLandChoice> Choices = LandPanelQuoted(*Runtime, Field.Threshold);
	const FLandChoice* Meridian = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane7"));
	const FLandChoice* A380 = LandPanelFind(Choices, TEXT("DA_Aircraft_Plane8"));
	if (!TestNotNull(TEXT("the Meridian is listed"), Meridian) || !TestNotNull(TEXT("the A380 is listed"), A380))
	{
		return false;
	}

	TestTrue(FString::Printf(TEXT("the Meridian can land on the field built for it ('%s')"), *Meridian->Refusal), Meridian->bAdmitted);
	TestTrue(TEXT("with no refusal"), Meridian->Refusal.IsEmpty());
	TestFalse(TEXT("the A380 cannot"), A380->bAdmitted);
	// THE PLANNER'S OWN WORDS, figures and all - the runway's admission, which for this strip is its width before its
	// length (measured 2026-09-30: "the runway admits a 6500 uu wingspan; this aircraft's is 7940").
	UAircraftType* A380Type = LandPanelType(TEXT("DA_Aircraft_Plane8"));
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Actor->Network, Field.Threshold, A380Type->Airframe(),
		&Actor->GetGroundTraffic()->GetOccupancy(), ERunwayBusy::Queue);
	TestEqual(TEXT("and the row says why in the planner's words"), A380->Refusal, ArrivalPlanner::DescribeRefusal(Plan));
	TestTrue(FString::Printf(TEXT("naming the runway: \"%s\""), *A380->Refusal), A380->Refusal.Contains(TEXT("runway")));
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
	// THROUGH THE RUNTIME'S QUOTE (#432): the admission is the planner's first question of a runway, so a bare strip is
	// enough - the SR22 is refused before any exit or stand is asked for.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	LandPanelMakeRunway(*Actor->Network, 40366.0, 2300.0);
	// DERIVED, as the facade's rebuild would: a guideline graph behind the road refuses every plan as mid-edit.
	TestGraph::Derive(*Actor->Network);
	const TArray<FLandChoice> Choices = LandPanelQuoted(*LandPanelRuntime(*Actor), FVector2D::ZeroVector);
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

	TestFalse(TEXT("hidden until asked for"), Panel->IsShown());
	Panel->Toggle();
	TestTrue(TEXT("open after one toggle"), Panel->IsShown());
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
	TestTrue(TEXT("and the panel stays open, so several can be queued"), Panel->IsShown());
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
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHud()))
	{
		return false;
	}
	// A headless controller has no local player, so BeginPlay never created the HUD's
	// widgets; the panel is put where CreateAll would have put it.
	C->GetHud()->LandPanel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());

	FBuildActionContext Ctx(*C);
	Land->Execute(Ctx);
	TestTrue(TEXT("pressing Land opens the panel rather than landing a default"), C->GetHud()->IsWindowShowing(EHudWindow::Land));
	TestTrue(TEXT("and the bar button lights while it is open"), Land->IsActive(Ctx));
	Land->Execute(Ctx);
	TestFalse(TEXT("pressing it again closes it"), C->GetHud()->IsWindowShowing(EHudWindow::Land));
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

	// A STAND HELD (#432): each row is a whole plan now, and a plan's NoFreeStand moves with the occupancy table.
	UGroundTraffic* Traffic = Actor->GetGroundTraffic();
	if (!TestNotNull(TEXT("traffic"), Traffic)) { return false; }
	const uint32 Occupancy = Traffic->OccupancyRevision();
	TestTrue(TEXT("a stand held for a flight"), Traffic->HoldStand(-99, Field.Pose(0)));
	TestNotEqual(TEXT("(a hold moves the occupancy revision)"), Traffic->OccupancyRevision(), Occupancy);
	TestEqual(TEXT("a stand held (OccupancyRevision): judged again, once"), Builds(5), 1);

	// A RUNTIME TO QUOTE FROM: with none every row is refused, so its arrival must re-quote them.
	UOpsRuntime* Runtime = LandPanelRuntime(*Actor);
	auto BuildsWith = [&](int32 Frames)
	{
		const int32 Before = Panel->BuildCountForTest();
		for (int32 Frame = 0; Frame < Frames; ++Frame) { Panel->RefreshFor(C, Runtime); }
		return Panel->BuildCountForTest() - Before;
	};
	TestEqual(TEXT("the runtime arrives: judged again, once"), BuildsWith(5), 1);

	Actor->ClearNetwork();
	TestEqual(TEXT("a new network: judged again, once"), BuildsWith(5), 1);
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
	const FLandChoicesKey A = LandChoices::KeyFor(First.Net, nullptr, Near, true, true);
	const FLandChoicesKey B = LandChoices::KeyFor(Second.Net, nullptr, Near, true, true);
	TestTrue(TEXT("the same first runway on each"), A.FirstRunway != INDEX_NONE && A.FirstRunway == B.FirstRunway);
	TestTrue(TEXT("and still two keys: the network is on it"), A != B);
	TestTrue(TEXT("one network is one key"), A == LandChoices::KeyFor(First.Net, nullptr, Near, true, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelGreysWhileClosedTest,
	"AirportMgr.UI.LandPanelGreysWhileClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelGreysWhileClosedTest::RunTest(const FString& Parameters)
{
	// A CLOSED AIRPORT ADMITS NO ARRIVALS, the debug Land included (PR B ruling I1) - so every row is a click the game
	// would refuse, and PaintRows's rule (a greyed row is exactly that) greys them all (whole-stack review M1). The
	// status is on FLandChoicesKey, so the close is seen with nothing else moving.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe(), Actor->Network);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("open"), Runtime->GetAirport()->Status(), EAirportStatus::Open)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Point;
	Focus.Point = FVector2D(1000.0, 0.0);
	C->SelectAndFocus(Focus);
	ULandAircraftPanelWidget* Panel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	auto Enabled = [Panel]()
	{
		int32 Count = 0;
		for (int32 Row = 0; Row < Panel->RowWidgetCountForTest(); ++Row) { Count += Panel->IsRowEnabledForTest(Row) ? 1 : 0; }
		return Count;
	};

	Panel->RefreshFor(C, Runtime);
	if (!TestTrue(FString::Printf(TEXT("open: some type can land (%d of %d rows)"), Enabled(), Panel->RowWidgetCountForTest()), Enabled() > 0)) { return false; }
	TArray<int32> Landable;
	for (int32 Row = 0; Row < Panel->RowWidgetCountForTest(); ++Row) { if (Panel->IsRowEnabledForTest(Row)) { Landable.Add(Row); } }
	const int32 Built = Panel->BuildCountForTest();
	Runtime->SetAirportClosed(true);
	Panel->RefreshFor(C, Runtime);
	TestEqual(TEXT("the close alone re-judges the rows"), Panel->BuildCountForTest(), Built + 1);
	TestEqual(TEXT("closed: every row greyed"), Enabled(), 0);
	// THE GATE'S WORDS on every row the plan would take - the airport's closure, not a plan's refusal (a row the plan
	// refuses keeps its own reason, which is still true when the airport reopens).
	for (const int32 Row : Landable)
	{
		TestTrue(FString::Printf(TEXT("row %d says why ('%s')"), Row, *Panel->RowRefusalForTest(Row)), Panel->RowRefusalForTest(Row).Contains(TEXT("closed")));
	}
	Runtime->SetAirportClosed(false);
	Panel->RefreshFor(C, Runtime);
	TestTrue(TEXT("reopened: they are back"), Enabled() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandChoicesAgreeWithThePlannerTest,
	"AirportMgr.UI.LandChoicesAgreeWithThePlanner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandChoicesAgreeWithThePlannerTest::RunTest(const FString& Parameters)
{
	// #432's PIN: TWO RUNWAYS, THE ONE UNDER THE VIEW DEPARTURES ONLY. The panel judged the runway nearest the focus by
	// itself - departures only, so it greyed every row - while the planner lands on the other one, and the click would
	// have been taken. Each row is now the model's quote; this asks the planner directly, type by type, at the same
	// focus, and the two must agree on the verdict AND the words.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	const FTestTwoRunways Field = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe(), Actor->Network);
	auto UseOf = [&](FRoadSegmentId Seed, ERunwayUse Use)
	{
		// THROUGH THE FACADE, as the player's toggle is - its Topology notify moves the guideline revision the key reads.
		FRunwayFacts Facts = Actor->Network->RunwayFactsFor(Seed);
		Facts.Use = Use;
		return Actor->SetRunwayFacts(Seed.Index, Facts);
	};
	if (!TestTrue(TEXT("A is departures only"), UseOf(Field.A, ERunwayUse::DeparturesOnly))) { return false; }
	UOpsRuntime* Runtime = LandPanelRuntime(*Actor);
	UGroundTraffic* Traffic = Actor->GetGroundTraffic();
	if (!TestNotNull(TEXT("traffic"), Traffic)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	const FVector2D Focus(1000.0, 0.0);
	FAlertFocus OnA;
	OnA.Kind = EAlertFocusKind::Point;
	OnA.Point = Focus;
	C->SelectAndFocus(OnA);
	FRunwayEnd Nearest;
	if (!TestTrue(TEXT("the view is on A - the case under test"),
		Actor->Network->NearestRunwayThreshold(Focus, Nearest) && FTestTwoRunways::IsA(Nearest))) { return false; }
	ULandAircraftPanelWidget* Panel =
		CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }

	auto AgreeRowByRow = [&](const TCHAR* When)
	{
		int32 Lit = 0;
		for (const UAircraftType* Type : LandChoices::EveryMeshedType())
		{
			const int32 Row = Panel->RowIndexOfForTest(*Type->GetName());
			if (!TestTrue(FString::Printf(TEXT("%s: %s has a row"), When, *Type->GetName()), Row != INDEX_NONE)) { continue; }
			const FArrivalPlan Plan = ArrivalPlanner::Plan(*Actor->Network, Focus, Type->Airframe(),
				&Traffic->GetOccupancy(), ERunwayBusy::Queue);
			const bool bLit = Panel->IsRowEnabledForTest(Row);
			Lit += bLit ? 1 : 0;
			TestEqual(FString::Printf(TEXT("%s: %s's row is the planner's verdict"), When, *Type->GetName()), bLit, Plan.IsValid());
			TestEqual(FString::Printf(TEXT("%s: %s's row gives the planner's reason"), When, *Type->GetName()),
				Panel->RowRefusalForTest(Row), Plan.IsValid() ? FString() : ArrivalPlanner::DescribeRefusal(Plan));
		}
		return Lit;
	};

	Panel->RefreshFor(C, Runtime);
	TestTrue(TEXT("B takes arrivals, so something the view's own runway would refuse still lands"), AgreeRowByRow(TEXT("A departures only")) > 0);

	if (!TestTrue(TEXT("B is departures only too"), UseOf(Field.B, ERunwayUse::DeparturesOnly))) { return false; }
	Panel->RefreshFor(C, Runtime);
	TestEqual(TEXT("no runway takes arrivals: every row greyed, each with the planner's words"), AgreeRowByRow(TEXT("both departures only")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandWithNoRuntimeLandsNothingTest,
	"AirportMgr.Actions.LandWithNoRuntimeLandsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandWithNoRuntimeLandsNothingTest::RunTest(const FString& Parameters)
{
	// #431: THE BOARD-LESS FALLBACK IS GONE. With no runtime the controller dispatched straight onto the traffic model -
	// "for the editor mode", which never creates this controller - an aeroplane belonging to no flight, past the board
	// and the closure rule. A field that would take the landing, and no runtime: refused, said, nothing on the field.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Actor->Network);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	if (!TestTrue(TEXT("a headless world has no runtime - the case under test"), OpsRuntimeResolver::Resolve(TestWorld.World) == nullptr)) { return false; }
	UGroundTraffic* Traffic = Actor->GetGroundTraffic();
	if (!TestNotNull(TEXT("traffic"), Traffic)) { return false; }

	AddExpectedMessagePlain(TEXT("Land refused: no ops runtime"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	C->LandAircraftNearViewFocus(nullptr);
	TestEqual(TEXT("nothing was dispatched onto the field"), Traffic->GetAgents().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelCostOnAScaleFieldTest,
	"AirportMgr.UI.LandPanelCostOnAScaleField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelCostOnAScaleFieldTest::RunTest(const FString& Parameters)
{
	// #471 ITEM 5: WHAT ONE RE-QUOTE COSTS on a built-out field - every meshed type's whole arrival plan (UOpsRuntime::
	// QuoteLanding, the panel's own lambda), which the open panel pays each time FLandChoicesKey moves, an occupancy change
	// included. FTestAirport::BuildScale (#256's fixture: two runways, an 8x20 taxiway grid, 30 stands, 4 depots), twice:
	// sized for the type with the SHORTEST landing field length (the small types plan to a stand, the big ones are refused
	// at the runway) and for the LONGEST (no runway refuses on length, so every type searches the taxiways). Each type timed
	// alone, so the log says which refusals cost what.
	TArray<UAircraftType*> Types = LandChoices::EveryMeshedType();
	if (!TestTrue(TEXT("meshed types to quote"), Types.Num() > 0)) { return false; }
	UAircraftType* Shortest = Types[0];
	UAircraftType* Longest = Types[0];
	for (UAircraftType* Type : Types)
	{
		const double Field = Type->Airframe().Requirements.LandingFieldLength;
		Shortest = Field < Shortest->Airframe().Requirements.LandingFieldLength ? Type : Shortest;
		Longest = Field > Longest->Airframe().Requirements.LandingFieldLength ? Type : Longest;
	}
	// THE THIRD FIELD IS ONE THAT ADMITS: BuildScale's stands sit in its taxiways' strips, so on it every type that passes
	// the runway is refused at the stand (NoStandClearOfStrip) and no whole successful plan is ever timed. FTestAirport's
	// line of 30 stands beside one taxiway, sized for the shortest type, is.
	for (int32 Variant = 0; Variant < 3; ++Variant)
	{
		UAircraftType* SizedFor = Variant == 1 ? Longest : Shortest;
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(-900000.0, -900000.0));
		FTestAirportOptions ThirtyStands;
		ThirtyStands.StandCount = 30;
		ThirtyStands.bDerived = false;
		const FTestAirport Field = Variant < 2
			? FTestAirport::BuildScale(SizedFor->Airframe(), 20260921, /*bDerived=*/false, Actor->Network.Get())
			: FTestAirport::Build(SizedFor->Airframe(), ThirtyStands, Actor->Network.Get());
		Actor->RebuildMesh();
		const UOpsRuntime* Runtime = LandPanelRuntime(*Actor);
		if (!TestNotNull(TEXT("a runtime"), Runtime)) { return false; }

		// THREE ROUNDS, the slowest kept per type: the frame a player feels is the worst one.
		TMap<EArrivalRefusal, double> MsByWhy;
		TMap<EArrivalRefusal, int32> CountByWhy;
		double Total = 0.0;
		for (UAircraftType* Type : Types)
		{
			double Worst = 0.0;
			EArrivalRefusal Why = EArrivalRefusal::None;
			for (int32 Round = 0; Round < 3; ++Round)
			{
				const double Start = FPlatformTime::Seconds();
				Why = Runtime->QuoteLanding(Type->Airframe(), Field.Threshold).Why;
				Worst = FMath::Max(Worst, (FPlatformTime::Seconds() - Start) * 1000.0);
			}
			MsByWhy.FindOrAdd(Why) += Worst;
			++CountByWhy.FindOrAdd(Why);
			Total += Worst;
		}
		FString ByWhy;
		for (const TPair<EArrivalRefusal, int32>& Each : CountByWhy)
		{
			ByWhy += FString::Printf(TEXT(" %s x%d %.1f ms;"), *UEnum::GetValueAsString(Each.Key), Each.Value, MsByWhy[Each.Key]);
		}
		// AND WHAT THE GATE LEAVES OF IT: the occupancy-only re-quote (LandChoices::RequoteForOccupancy) over the same rows.
		TArray<FLandChoice> Rows = LandPanelQuoted(*Runtime, Field.Threshold);
		const double GateStart = FPlatformTime::Seconds();
		const int32 Requoted = LandChoices::RequoteForOccupancy(Rows,
			[Runtime, &Field](const FAirframe& Airframe) { return Runtime->QuoteLanding(Airframe, Field.Threshold); });
		const double GateMs = (FPlatformTime::Seconds() - GateStart) * 1000.0;
		const int32 Soft = Rows.FilterByPredicate([](const FLandChoice& Row) { return !ArrivalPlanner::IsPermanentRefusal(Row.Why); }).Num();
		TestEqual(TEXT("an occupancy move re-quotes exactly the rows occupancy can change"), Requoted, Soft);

		const TCHAR* FieldName = Variant < 2 ? TEXT("scale field") : TEXT("30-stand line");
		UE_LOG(LogRoadBuild, Log, TEXT("LandPanelCost: %s sized for %s - %d types, %.1f ms per re-quote (worst of 3 per type):%s - an occupancy move re-quotes %d row(s), %.1f ms"),
			FieldName, *SizedFor->GetName(), Types.Num(), Total, *ByWhy, Requoted, GateMs);
		AddInfo(FString::Printf(TEXT("Land panel re-quote on the %s sized for %s: %.1f ms whole, %.1f ms on an occupancy move"),
			FieldName, *SizedFor->GetName(), Total, GateMs));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelRequotesOnlyWhatOccupancyCanChangeTest,
	"AirportMgr.UI.LandPanelRequotesOnlyWhatOccupancyCanChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelRequotesOnlyWhatOccupancyCanChangeTest::RunTest(const FString& Parameters)
{
	// #471 ITEM 5's GATE, through the widget: an occupancy move re-quotes only the rows it can change, and those rows still
	// TRACK it - a row greyed is still exactly a click the game would refuse. Two stands: both held turns every admitted
	// row to NoFreeStand; both freed admits them again. The permanently refused rows (a type the strips do not admit) are
	// never re-quoted by either.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	const FTestTwoRunways Field = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe(), Actor->Network);
	if (!TestEqual(TEXT("two stands"), Field.Stands.Num(), 2)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	ULandAircraftPanelWidget* Panel = CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	UOpsRuntime* Runtime = LandPanelRuntime(*Actor);
	UGroundTraffic* Traffic = Actor->GetGroundTraffic();
	if (!TestNotNull(TEXT("traffic"), Traffic)) { return false; }

	Panel->RefreshFor(C, Runtime);
	const int32 Rows = Panel->RowWidgetCountForTest();
	if (!TestTrue(TEXT("rows"), Rows > 0)) { return false; }
	TArray<int32> Admitted;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		if (Panel->IsRowEnabledForTest(Row)) { Admitted.Add(Row); }
	}
	if (!TestTrue(TEXT("PRECONDITION: some rows admitted and some refused - the case under test"), Admitted.Num() > 0 && Admitted.Num() < Rows)) { return false; }
	TestEqual(TEXT("CONTROL: the first judgement quotes every row"), Panel->RowQuoteCountForTest(), Rows);

	auto QuotesFor = [&](TFunctionRef<void()> Change)
	{
		const int32 Before = Panel->RowQuoteCountForTest();
		Change();
		Panel->RefreshFor(C, Runtime);
		return Panel->RowQuoteCountForTest() - Before;
	};
	const int32 BothHeld = QuotesFor([&]() { Traffic->HoldStand(-98, Field.Pose(0)); Traffic->HoldStand(-99, Field.Pose(1)); });
	TestTrue(FString::Printf(TEXT("both stands held: only the rows occupancy can change are re-quoted (%d of %d)"), BothHeld, Rows),
		BothHeld > 0 && BothHeld < Rows);
	bool bAllGreyed = true;
	for (const int32 Row : Admitted) { bAllGreyed &= !Panel->IsRowEnabledForTest(Row); }
	TestTrue(TEXT("and every admitted row now greys - nowhere to park"), bAllGreyed);

	const int32 Freed = QuotesFor([&]() { Traffic->ReleaseHold(-98); Traffic->ReleaseHold(-99); });
	TestEqual(TEXT("both freed: the same rows, and no more, are re-quoted"), Freed, BothHeld);
	bool bAllBack = true;
	for (const int32 Row : Admitted) { bAllBack &= Panel->IsRowEnabledForTest(Row); }
	TestTrue(TEXT("and they are admitted again - NoFreeStand cleared on its own"), bAllBack);

	// CONTROL: ANYTHING ELSE IN THE KEY STILL JUDGES EVERY ROW - here a new network.
	Actor->ClearNetwork();
	TestEqual(TEXT("a new network: every row quoted again"), QuotesFor([]() {}), Rows);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelGatedWordingTest,
	"AirportMgr.UI.LandPanelGatedWordingMatchesAFreshBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelGatedWordingTest::RunTest(const FString& Parameters)
{
	// #497 REVIEW: RequoteForOccupancy skips a PERMANENTLY refused row on an occupancy change, which is exact only if nothing
	// about that row moves with occupancy. Its WORDING did: NoRouteToStand became TaxiwayTooNarrow only while the stand the
	// span-blind probe found was free, so a row judged while it was held kept "no route" after it freed. The field: a Code C
	// 737, a C stand, and the one taxiway to it restricted to Code B by a service road at its edge (the Airside
	// TaxiwayTooNarrow test's field). Judged with the stand HELD, re-quoted for the occupancy when it frees, then compared
	// with a fresh Build.
	UAircraftType* Type = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(Type);
	const FAirframe Airframe = Type->Airframe();
	if (!TestTrue(TEXT("PRECONDITION: the 737 is a Code C span"), Airframe.Wingspan > IcaoCode::MaxWingspanForLetter(EIcaoCode::B)
		&& Airframe.Wingspan <= IcaoCode::MaxWingspanForLetter(EIcaoCode::C))) { return false; }
	const double Needed = FMath::Max3(FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach)
		* FLandingRun::LandingMargin, Airframe.Requirements.LandingFieldLength, Airframe.Requirements.TakeoffFieldLength);
	const double RunwayLength = Needed * 1.5;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	const FVector2D ThresholdAt(0.0, 0.0);
	const FVector2D ExitAt(RunwayLength * 0.8, 0.0);
	const FRoadNodeId Exit = Net->AddNode(ExitAt);
	Net->AddStraightSegment(Net->AddNode(ThresholdAt), Exit, TestProfiles::Runway());
	Net->AddStraightSegment(Exit, Net->AddNode(FVector2D(RunwayLength, 0.0)), TestProfiles::Runway());
	Net->AddStraightSegment(Exit, Net->AddNode(ExitAt + FVector2D(0.0, -20000.0)), Taxiway);
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, ExitAt + FVector2D(9000.0, -10000.0), 0.0,
		IcaoCode::DesignSpanForLetter(EIcaoCode::C), StandDef->PoseRole, StandDef->Trucks);
	const double ReachB = Taxiway->GetMaxHalfWidth() + IcaoCode::TaxiwayStripFor(EIcaoCode::B, Taxiway->GetTotalWidth());
	const double ReachC = Taxiway->GetMaxHalfWidth() + IcaoCode::TaxiwayStripFor(EIcaoCode::C, Taxiway->GetTotalWidth());
	const double RoadX = ExitAt.X - (0.5 * (ReachB + ReachC) + Road->GetMaxHalfWidth());
	Net->AddStraightSegment(Net->AddNode(FVector2D(RoadX, -8000.0)), Net->AddNode(FVector2D(RoadX, -12000.0)), Road);
	TestGraph::Rebuild(*Net);
	const FEntityInstance* StandAt = Net->GetEntity(Stand);
	if (!TestTrue(TEXT("the stand has a pose"), StandAt != nullptr && StandAt->PoseNode.IsSet())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	// THE PANEL'S QUOTE WITHOUT THE RUNTIME: the board's PlanQuote, a busy runway queued.
	const auto Quote = [&](const FAirframe& Frame)
	{
		const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, ThresholdAt, Frame, &Traffic->GetOccupancy(), ERunwayBusy::Queue);
		FArrivalQuote Out;
		Out.Why = Plan.Why;
		Out.Sentence = Plan.IsValid() ? FString() : ArrivalPlanner::DescribeRefusal(Plan);
		return Out;
	};
	const TArray<UAircraftType*> Types{ Type };
	const TArray<FLandChoice> Free = LandChoices::Build(Types, Quote);
	if (!TestTrue(FString::Printf(TEXT("PRECONDITION: the stand free, the row is TaxiwayTooNarrow ('%s')"), Free.Num() == 1 ? *Free[0].Refusal : TEXT("")),
		Free.Num() == 1 && Free[0].Why == EArrivalRefusal::TaxiwayTooNarrow)) { return false; }

	Traffic->HoldStand(-5, StandAt->PoseNode);
	TArray<FLandChoice> Gated = LandChoices::Build(Types, Quote);
	Traffic->ReleaseHold(-5);
	TestEqual(TEXT("a permanent refusal is not re-quoted for an occupancy change"), LandChoices::RequoteForOccupancy(Gated, Quote), 0);
	const TArray<FLandChoice> Fresh = LandChoices::Build(Types, Quote);
	TestEqual(TEXT("the gated row's reason is a fresh build's"), Gated[0].Why, Fresh[0].Why);
	TestEqual(TEXT("and so is its sentence - it did not move with the held stand"), Gated[0].Refusal, Fresh[0].Refusal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandPanelPanBackTest,
	"AirportMgr.UI.LandPanelPanBackQuotesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandPanelPanBackTest::RunTest(const FString& Parameters)
{
	// #497 REVIEW: A WHOLE RE-QUOTE IS STILL 78-125 ms ON A BUILT-OUT FIELD, and FLandChoicesKey's FirstRunway moves with the
	// camera - a pan between two runways paid it on every crossing. The rows are kept per runway now: back onto a runway judged
	// on this very graph and traffic quotes nothing, and one judged before a stand was held re-quotes only the rows occupancy
	// can change.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	const FTestTwoRunways Field = FTestTwoRunways::Build(UAirsideSettings::ResolveDefaultAirframe(), Actor->Network);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	ULandAircraftPanelWidget* Panel = CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	UOpsRuntime* Runtime = LandPanelRuntime(*Actor);
	UGroundTraffic* Traffic = Actor->GetGroundTraffic();
	if (!TestNotNull(TEXT("traffic"), Traffic)) { return false; }

	auto QuotesFor = [&](TFunctionRef<void()> Change)
	{
		const int32 Before = Panel->RowQuoteCountForTest();
		Change();
		Panel->RefreshFor(C, Runtime);
		return Panel->RowQuoteCountForTest() - Before;
	};
	auto FocusAt = [C](const FVector2D& At)
	{
		FAlertFocus Focus;
		Focus.Kind = EAlertFocusKind::Point;
		Focus.Point = At;
		C->SelectAndFocus(Focus);
	};
	const FVector2D OnA(1000.0, 0.0);
	const FVector2D OnB(1000.0, -40000.0);
	const int32 Rows = QuotesFor([&]() { FocusAt(OnA); });
	if (!TestTrue(TEXT("opened on runway A: every row quoted"), Rows > 0 && Rows == Panel->RowWidgetCountForTest())) { return false; }
	TestEqual(TEXT("onto runway B: every row quoted for it"), QuotesFor([&]() { FocusAt(OnB); }), Rows);
	TestEqual(TEXT("BACK ONTO A, nothing having moved: nothing quoted"), QuotesFor([&]() { FocusAt(OnA); }), 0);
	TestEqual(TEXT("and back onto B: nothing"), QuotesFor([&]() { FocusAt(OnB); }), 0);

	const int32 Held = QuotesFor([&]() { Traffic->HoldStand(-97, Field.Pose(0)); });
	TestTrue(FString::Printf(TEXT("a stand held while on B: only the rows it can change (%d of %d)"), Held, Rows), Held > 0 && Held < Rows);
	const int32 BackToA = QuotesFor([&]() { FocusAt(OnA); });
	TestTrue(FString::Printf(TEXT("onto A, judged before the hold: its occupancy rows only, not every row (%d of %d)"), BackToA, Rows),
		BackToA > 0 && BackToA < Rows);

	// CONTROL: AN EDIT judges every row - here a new network.
	TestEqual(TEXT("a new network: every row quoted again"), QuotesFor([&]() { Actor->ClearNetwork(); }), Rows);
	return true;
}

#endif
