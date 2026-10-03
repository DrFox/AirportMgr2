#include "CoreMinimal.h"
#include "UI/UiWindow.h"
#include "UI/UiWindowHost.h"
#include "Present/RoadNetworkActor.h"
#include "Present/OpsRuntime.h"
#include "OpsRuntimeResolver.h"
#include "ArrivalViewModels.h"
#include "ArrivalsPanelWidget.h"
#include "Content/AirsideSettings.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestGraph.h"
#include "Blueprint/UserWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/SimClock.h"
#include "Testing/AirsideTestWorld.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The ARRIVALS section (spec 2026-09-28-arrival-queue section 3): every accepted flight until
 * it is airborne, holding first in queue order, then inbound by ETA, then on the ground - and a
 * contract line that counts down from on-blocks and turns into "late" once OffBlocksBy has passed
 * (#398: before on-blocks it says how long the turnaround will be, and counts nothing).
 */
namespace
{
	UFlight* Flight(const TCHAR* Callsign, EFlightPhase Phase)
	{
		UFlight* Out = NewObject<UFlight>(GetTransientPackage());
		Out->Callsign = Callsign;
		Out->TypeName = FText::FromString(TEXT("Saab 340B"));
		Out->SetPhaseForTest(Phase);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsOrderTest, "AirportMgr.Arrivals.OrderHoldingThenInboundThenGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsOrderTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* OnStand = Flight(TEXT("G-STND"), EFlightPhase::Turnaround);
	UFlight* Coming = Flight(TEXT("CU 100"), EFlightPhase::Accepted);
	Coming->ArrivesAt = 500.0;
	UFlight* HeldSecond = Flight(TEXT("CU 200"), EFlightPhase::Inbound);
	HeldSecond->HoldingSince = 10.0;
	UFlight* HeldFirst = Flight(TEXT("CU 300"), EFlightPhase::Inbound);
	HeldFirst->HoldingSince = 5.0;
	for (UFlight* Each : { OnStand, Coming, HeldSecond, HeldFirst }) { Board->AddOffer(*Clock, Each); }

	UArrivalsViewModel* Arrivals = NewObject<UArrivalsViewModel>();
	Arrivals->Refresh(*Board, *Clock);
	const TArray<UArrivalRowViewModel*> Rows = Arrivals->GetRows();
	if (!TestEqual(TEXT("one row per accepted flight"), Rows.Num(), 4)) { return false; }
	TestEqual(TEXT("the first to join the queue is #1"), Rows[0]->GetTitle().ToString(), FString(TEXT("#1 CU 300  Saab 340B")));
	TestEqual(TEXT("then #2"), Rows[1]->GetTitle().ToString(), FString(TEXT("#2 CU 200  Saab 340B")));
	TestEqual(TEXT("then the inbound flight, unnumbered"), Rows[2]->GetTitle().ToString(), FString(TEXT("CU 100  Saab 340B")));
	TestEqual(TEXT("then the one on the ground"), Rows[3]->GetTitle().ToString(), FString(TEXT("G-STND  Saab 340B")));
	TestEqual(TEXT("the heading counts them"), Arrivals->GetCount(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsStatusTest, "AirportMgr.Arrivals.StatusText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsStatusTest::RunTest(const FString& Parameters)
{
	UFlight* F = Flight(TEXT("CU 1"), EFlightPhase::Accepted);
	F->ArrivesAt = 360.0;
	TestEqual(TEXT("inbound says when"), UArrivalRowViewModel::DescribeStatus(*F, 0.0).ToString(), FString(TEXT("in 6 min")));
	const TPair<EFlightPhase, const TCHAR*> Cases[] = {
		{ EFlightPhase::Inbound, TEXT("HOLDING") }, { EFlightPhase::Landing, TEXT("LANDING") },
		{ EFlightPhase::TaxiIn, TEXT("TAXI IN") }, { EFlightPhase::Turnaround, TEXT("ON STAND") },
		{ EFlightPhase::Manoeuvring, TEXT("MANOEUVRING") }, { EFlightPhase::TaxiOut, TEXT("TAXI OUT") },
		{ EFlightPhase::Departing, TEXT("DEPARTING") } };
	for (const TPair<EFlightPhase, const TCHAR*>& Case : Cases)
	{
		F->SetPhaseForTest(Case.Key);
		TestEqual(*FString::Printf(TEXT("status %s"), Case.Value),
			UArrivalRowViewModel::DescribeStatus(*F, 0.0).ToString(), FString(Case.Value));
	}

	// EVERY PHASE THE ENUM HAS (#442 review): DescribeStatus is an exhaustive switch now, so a phase added to EFlightPhase is a build
	// error there; this is the runtime half - a live phase says something, and a phase a live row never shows (an offer, anything
	// finished) is empty, not a stale word from the case above.
	const UEnum* Enum = StaticEnum<EFlightPhase>();
	if (!TestNotNull(TEXT("the enum reflects"), Enum)) { return false; }
	for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
	{
		const EFlightPhase Phase = static_cast<EFlightPhase>(Enum->GetValueByIndex(Index));
		F->SetPhaseForTest(Phase);
		const FString Text = UArrivalRowViewModel::DescribeStatus(*F, 0.0).ToString();
		TestEqual(*FString::Printf(TEXT("%s: a live phase has a status, any other is empty"), *Enum->GetNameStringByIndex(Index)),
			!Text.IsEmpty(), FlightPhase::IsLive(Phase));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsDetailTest, "AirportMgr.Arrivals.ContractDetailAndLate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsDetailTest::RunTest(const FString& Parameters)
{
	UFlight* F = Flight(TEXT("CU 1"), EFlightPhase::Inbound);
	F->AcceptedAt = 0.0;
	F->ContractSeconds = 3600.0;
	F->HoldingSince = 600.0;
	bool bLate = true;
	// #398: HOLDING COSTS THE CONTRACT NOTHING, so the row does not count it down - it says how long the turnaround will be.
	TestEqual(TEXT("holding: how long it has waited, and the turnaround it will get - no countdown before on-blocks"),
		UArrivalRowViewModel::DescribeDetail(*F, 780.0, bLate).ToString(), FString(TEXT("waited 3 min · turnaround 1 h")));
	TestFalse(TEXT("not late"), bLate);
	F->SetPhaseForTest(EFlightPhase::TaxiIn);
	TestEqual(TEXT("taxiing in, hours after the accept: still not started"),
		UArrivalRowViewModel::DescribeDetail(*F, 20000.0, bLate).ToString(), FString(TEXT("turnaround 1 h")));
	TestFalse(TEXT("and not late"), bLate);

	F->SetPhaseForTest(EFlightPhase::Turnaround);
	F->OnBlocksAt = 20000.0;
	TestEqual(TEXT("on the stand: what is left, counted from on-blocks"),
		UArrivalRowViewModel::DescribeDetail(*F, 20000.0 + 780.0, bLate).ToString(), FString(TEXT("47 min left")));

	TestEqual(TEXT("past OffBlocksBy it is late, by how much"),
		UArrivalRowViewModel::DescribeDetail(*F, 20000.0 + 3600.0 + 720.0, bLate).ToString(), FString(TEXT("12 min late")));
	TestTrue(TEXT("and flagged"), bLate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsTurnaroundLineTest, "AirportMgr.Arrivals.TurnaroundLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsTurnaroundLineTest::RunTest(const FString& Parameters)
{
	// THE CONTRACT ON THE AIRCRAFT CARD (2026-09-28): how long the airline gave, and how much of
	// it is left - the same words the ARRIVALS row uses, so the two cannot disagree.
	UFlight* F = Flight(TEXT("CU 1"), EFlightPhase::TaxiIn);
	F->AcceptedAt = 0.0;
	F->ContractSeconds = 2400.0;
	// #398: BEFORE ON-BLOCKS no countdown - the card says the contract starts on the stand.
	TestEqual(TEXT("not started"), UArrivalRowViewModel::DescribeTurnaround(*F, 9000.0).ToString(),
		FString(TEXT("Turnaround 40 min \u00B7 starts on stand")));
	F->SetPhaseForTest(EFlightPhase::Turnaround);
	F->OnBlocksAt = 5000.0;
	TestEqual(TEXT("time left"), UArrivalRowViewModel::DescribeTurnaround(*F, 5000.0 + 2400.0 - 27.0 * 60.0).ToString(),
		FString(TEXT("Turnaround 40 min \u00B7 27 min left")));
	TestEqual(TEXT("past the deadline"), UArrivalRowViewModel::DescribeTurnaround(*F, 5000.0 + 2400.0 + 12.0 * 60.0).ToString(),
		FString(TEXT("Turnaround 40 min \u00B7 12 min late")));
	F->ContractSeconds = 0.0;
	TestTrue(TEXT("no contract (the debug land key), no line"), UArrivalRowViewModel::DescribeTurnaround(*F, 0.0).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsHoldingIsAFactTest, "AirportMgr.UI.Arrivals.HoldingIsAFactNotAWord",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsHoldingIsAFactTest::RunTest(const FString& Parameters)
{
	// THE PANEL TINTS "HOLDING" (the one state the player can act on) and used to find it by comparing the status's LOCALISED TEXT against a copy of
	// the word - a reworded status or a translation would have un-tinted it silently (#447). The row says so itself now, set by the same Refresh that
	// words the status, through the same phase test.
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Holding = Flight(TEXT("CU 1"), EFlightPhase::Inbound);
	Holding->HoldingSince = 5.0;
	UFlight* Landing = Flight(TEXT("CU 2"), EFlightPhase::Landing);
	UFlight* OnStand = Flight(TEXT("G-ABCD"), EFlightPhase::Turnaround);
	for (UFlight* Each : { Holding, Landing, OnStand }) { Board->AddOffer(*Clock, Each); }

	UArrivalsViewModel* Arrivals = NewObject<UArrivalsViewModel>();
	Arrivals->Refresh(*Board, *Clock);
	int32 HoldingRows = 0;
	for (const UArrivalRowViewModel* Row : Arrivals->GetRows())
	{
		const bool bSaysHolding = Row->GetTitle().ToString().Contains(TEXT("CU 1"));
		TestEqual(*FString::Printf(TEXT("'%s': the row is holding exactly when its flight is the holding one"), *Row->GetTitle().ToString()),
			Row->IsHolding(), bSaysHolding);
		HoldingRows += Row->IsHolding() ? 1 : 0;
	}
	TestEqual(TEXT("one holding row of three"), HoldingRows, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsHoldingRowAccentTest, "AirportMgr.UI.Arrivals.HoldingRowIsInAccent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsHoldingRowAccentTest::RunTest(const FString& Parameters)
{
	// THE PANEL'S USE OF IsHolding (#447): HoldingIsAFactNotAWord pins the fact; this pins that the panel READS it - a holding row's status is
	// drawn in Style.Accent, the one state the player can act on, and any other row in InkMuted. Without it the panel could stop tinting and
	// every test above still pass.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	UArrivalsPanelWidget* Panel = CreateWidget<UArrivalsPanelWidget>(TestWorld.World, UArrivalsPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an arrivals panel"), Panel) || !TestNotNull(TEXT("with its style"), Panel->PanelStyleForTest())) { return false; }
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Holding = Flight(TEXT("CU 1"), EFlightPhase::Inbound);
	Holding->HoldingSince = 5.0;
	UFlight* Landing = Flight(TEXT("CU 2"), EFlightPhase::Landing);
	for (UFlight* Each : { Holding, Landing }) { Board->AddOffer(*Clock, Each); }
	Panel->GetArrivals()->Refresh(*Board, *Clock);
	Panel->PaintRowsForTest();
	if (!TestEqual(TEXT("a row each"), Panel->RowCountForTest(), 2)) { return false; }
	const UUIStyle& Style = *Panel->PanelStyleForTest();
	// HOLDING FIRST (the list's own order), then the flight on the ground.
	TestEqual(TEXT("the holding row's status is in accent"), Panel->StatusColourForTest(0), FLinearColor(Style.Accent));
	TestEqual(TEXT("and the other row's is muted"), Panel->StatusColourForTest(1), FLinearColor(Style.InkMuted));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsKeyMovesWithTheTextTest, "AirportMgr.UI.Arrivals.KeyMovesWithTheText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsKeyMovesWithTheTextTest::RunTest(const FString& Parameters)
{
	// #446, THE #480 LESSON: a row is composed again only when its key moves, so the key must move whenever a SENTENCE does. The defect is the
	// quiet one - a key that rounds a minute differently from the text it guards holds a stale sentence for up to a minute - and no single
	// assertion at one instant can see it. So the clock is WALKED across the whole lifetime of a flight in each phase that counts something down,
	// in steps that land on both sides of every rounding boundary, and an unchanged key must never hide a changed sentence. The key is also
	// required to hold still for most steps, or this would pass for a key that changed every call and memoised nothing.
	struct FCase { UFlight* Flight; const TCHAR* Name; };
	UFlight* Coming = Flight(TEXT("CU 1"), EFlightPhase::Accepted);
	Coming->ArrivesAt = 2.0 * 3600.0 + 17.0;
	Coming->AcceptedAt = 0.0;
	Coming->ContractSeconds = 5400.0;
	UFlight* Held = Flight(TEXT("CU 2"), EFlightPhase::Inbound);
	Held->HoldingSince = 600.0;
	Held->AcceptedAt = 0.0;
	Held->ContractSeconds = 3600.0;
	UFlight* OnStand = Flight(TEXT("CU 3"), EFlightPhase::Turnaround);
	OnStand->AcceptedAt = 0.0;
	OnStand->OnBlocksAt = 1.0;           // on blocks at 1 s (0 is "never") - the contract counts from here (#398)
	OnStand->ContractSeconds = 3600.0;   // goes late at 3601 s: the walk crosses left -> late
	const FCase Cases[] = { { Coming, TEXT("accepted") }, { Held, TEXT("holding") }, { OnStand, TEXT("on stand") } };

	for (const FCase& Case : Cases)
	{
		int32 Samples = 0;
		int32 KeyChanges = 0;
		int32 Hidden = 0;
		FString FirstHidden;
		FArrivalRowKey PrevKey;
		FString PrevStatus, PrevDetail;
		for (double Now = 0.0; Now <= 5400.0; Now += 7.3)
		{
			const FArrivalRowKey Key = UArrivalRowViewModel::KeyFor(*Case.Flight, 1, Now);
			bool bLate = false;
			const FString Status = UArrivalRowViewModel::DescribeStatus(*Case.Flight, Now).ToString();
			const FString Detail = UArrivalRowViewModel::DescribeDetail(*Case.Flight, Now, bLate).ToString();
			if (Samples > 0)
			{
				if (Key == PrevKey)
				{
					if (Status != PrevStatus || Detail != PrevDetail)
					{
						++Hidden;
						if (FirstHidden.IsEmpty()) { FirstHidden = FString::Printf(TEXT("at %.1f s '%s' / '%s' after '%s' / '%s'"), Now, *Status, *Detail, *PrevStatus, *PrevDetail); }
					}
				}
				else
				{
					++KeyChanges;
				}
			}
			PrevKey = Key;
			PrevStatus = Status;
			PrevDetail = Detail;
			++Samples;
		}
		TestEqual(*FString::Printf(TEXT("%s: an unchanged key never hides a changed sentence (%s)"), Case.Name, *FirstHidden), Hidden, 0);
		TestTrue(*FString::Printf(TEXT("%s: the key moves as the minutes do (%d changes over %d steps)"), Case.Name, KeyChanges, Samples), KeyChanges > 0);
		TestTrue(*FString::Printf(TEXT("%s: and holds still for most steps - a key that moved every call would memoise nothing (%d changes over %d steps)"), Case.Name, KeyChanges, Samples),
			KeyChanges * 2 < Samples);
	}

	// THE ROW ITSELF: a repeat in the same minute composes nothing, a later minute composes.
	USimClock* Clock = NewObject<USimClock>();
	UArrivalRowViewModel* Row = NewObject<UArrivalRowViewModel>();
	Row->Flight = Coming;
	TestTrue(TEXT("the first refresh composes"), Row->Refresh(*Clock));
	const int32 Stamp = Row->GetRevision();
	TestFalse(TEXT("a repeat with the clock where it was composes nothing"), Row->Refresh(*Clock));
	TestEqual(TEXT("and does not move the stamp a panel repaints on"), Row->GetRevision(), Stamp);
	const double Before = Clock->Now();
	Clock->Advance(10.0);
	if (!TestTrue(TEXT("setup: the clock moved by minutes"), Clock->Now() - Before > 120.0)) { return false; }
	TestTrue(TEXT("a later minute composes again"), Row->Refresh(*Clock));
	TestTrue(TEXT("under a new stamp"), Row->GetRevision() != Stamp);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsFoldedComposesNothingTest, "AirportMgr.UI.Arrivals.FoldedPanelComposesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsFoldedComposesNothingTest::RunTest(const FString& Parameters)
{
	// #446 PIN: a FOLDED window is still ticked by the host, and its rows used to compose every sentence and paint every text every tick for a
	// body nobody could see. Through the real panel, hosted in a real window host, with the world's runtime standing in for play's: folded, the
	// panel composes NOTHING - neither the view model's sentences nor the widget's texts - while time moves and the count still reaches the
	// title bar; unfolded, it composes (the control - otherwise a counter stuck at zero would pass this) and then goes quiet on an idle tick.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world with an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);
	UFlightBoard* Board = Runtime->GetFlightBoard();
	USimClock* Clock = Runtime->GetClock();
	if (!TestTrue(TEXT("setup: the runtime has a board and a clock"), Board != nullptr && Clock != nullptr)) { return false; }

	UFlight* Coming = Flight(TEXT("CU 1"), EFlightPhase::Accepted);
	Coming->ArrivesAt = 3.0 * 3600.0;
	Coming->ContractSeconds = 5400.0;
	UFlight* Also = Flight(TEXT("CU 2"), EFlightPhase::Accepted);
	Also->ArrivesAt = 4.0 * 3600.0 + 31.0;
	Also->ContractSeconds = 5400.0;
	UFlight* Held = Flight(TEXT("CU 3"), EFlightPhase::Inbound);
	Held->HoldingSince = 5.0;
	Held->ContractSeconds = 3600.0;
	UFlight* OnStand = Flight(TEXT("CU 4"), EFlightPhase::Turnaround);
	OnStand->ContractSeconds = 3600.0;
	for (UFlight* Each : { Coming, Also, Held, OnStand }) { Board->AddOffer(*Clock, Each); }

	UUiWindowHost* Host = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	UArrivalsPanelWidget* Panel = CreateWidget<UArrivalsPanelWidget>(TestWorld.World, UArrivalsPanelWidget::StaticClass());
	if (!TestTrue(TEXT("setup: a host and a panel"), Host != nullptr && Panel != nullptr)) { return false; }
	Host->SetViewSizeForTest(FVector2D(1920.0, 1080.0));
	UUiWindow* Window = Host->AddWindow(*Panel);
	if (!TestNotNull(TEXT("setup: the panel is hosted in a window"), Window)) { return false; }
	TestFalse(TEXT("it opens unfolded"), Panel->IsFolded());

	auto Composed = [Panel]() { return Panel->GetArrivals()->ComposeCountForTest() + Panel->PaintCountForTest(); };

	Panel->Refresh();
	TestEqual(TEXT("the first tick composes and paints every row"), Panel->GetArrivals()->ComposeCountForTest(), 4);
	TestEqual(TEXT("one paint a row"), Panel->PaintCountForTest(), 4);
	const int32 Settled = Composed();
	for (int32 Tick = 0; Tick < 5; ++Tick) { Panel->Refresh(); }
	TestEqual(TEXT("unfolded and idle, a tick composes and paints nothing - the keyed rows hold"), Composed(), Settled);

	// FOLDED, WITH TIME MOVING: the accepted rows count down, so an unfolded panel WOULD compose here.
	Host->SetCollapsed(TEXT("arrivals"), true);
	if (!TestTrue(TEXT("setup: the window is folded and the panel knows"), Panel->IsFolded())) { return false; }
	const double Before = Clock->Now();
	Clock->Advance(10.0);
	if (!TestTrue(TEXT("setup: the clock moved by minutes"), Clock->Now() - Before > 120.0)) { return false; }
	for (int32 Tick = 0; Tick < 5; ++Tick) { Panel->Refresh(); }
	TestEqual(TEXT("FOLDED, a tick composes nothing and paints nothing, however far the clock has moved"), Composed(), Settled);

	// THE COUNT STILL READS ON THE TITLE BAR.
	UFlight* Late = Flight(TEXT("CU 5"), EFlightPhase::Accepted);
	Late->ArrivesAt = 5.0 * 3600.0;
	Board->AddOffer(*Clock, Late);
	Panel->Refresh();
	TestEqual(TEXT("a flight accepted while folded reaches the title bar's count"), Window->BadgeForTest(), FString(TEXT("5")));
	TestEqual(TEXT("and the new row is still not composed"), Composed(), Settled);

	// UNFOLDED AGAIN: the control - the counter can see a compose - and then quiet.
	Host->SetCollapsed(TEXT("arrivals"), false);
	Panel->Refresh();
	TestTrue(TEXT("unfolded, the panel composes what it skipped"), Composed() > Settled);
	TestEqual(TEXT("and paints every row - the new one, and the ones the clock moved"), Panel->RowCountForTest(), 5);
	const int32 Caught = Composed();
	for (int32 Tick = 0; Tick < 5; ++Tick) { Panel->Refresh(); }
	TestEqual(TEXT("then an idle tick is quiet again"), Composed(), Caught);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsAcceptingOneFlightComposesOnlyTheNewRowTest, "AirportMgr.UI.Arrivals.AcceptingOneFlightComposesOnlyTheNewRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsAcceptingOneFlightComposesOnlyTheNewRowTest::RunTest(const FString& Parameters)
{
	// #446: a NewObject per row on every board revision threw away each row's memo, so ACCEPTING ONE FLIGHT recomposed every sentence on the board.
	// A flight that stays keeps its row object now, and with it its key - so a board change composes what CHANGED. The clock is held still, so a
	// compose here is not "a minute passed": it is only ever the new row, or a row whose queue place moved.
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Coming = Flight(TEXT("CU 1"), EFlightPhase::Accepted);
	Coming->ArrivesAt = 3600.0;
	Coming->ContractSeconds = 5400.0;
	UFlight* Later = Flight(TEXT("CU 2"), EFlightPhase::Accepted);
	Later->ArrivesAt = 7200.0;
	Later->ContractSeconds = 5400.0;
	UFlight* Held = Flight(TEXT("CU 3"), EFlightPhase::Inbound);
	Held->HoldingSince = 5.0;
	Held->ContractSeconds = 3600.0;
	UFlight* OnStand = Flight(TEXT("CU 4"), EFlightPhase::Turnaround);
	OnStand->ContractSeconds = 3600.0;
	for (UFlight* Each : { Coming, Later, Held, OnStand }) { Board->AddOffer(*Clock, Each); }

	UArrivalsViewModel* Arrivals = NewObject<UArrivalsViewModel>();
	Arrivals->Refresh(*Board, *Clock);
	TMap<const UFlight*, UArrivalRowViewModel*> Before;
	for (UArrivalRowViewModel* Row : Arrivals->GetRows()) { Before.Add(Row->Flight.Get(), Row); }
	if (!TestEqual(TEXT("setup: a row a flight"), Before.Num(), 4)) { return false; }
	const int32 Composed = Arrivals->ComposeCountForTest();
	TestEqual(TEXT("setup: each composed once"), Composed, 4);

	auto KeptTheirObjects = [&](const TCHAR* Case)
	{
		int32 Same = 0;
		for (UArrivalRowViewModel* Row : Arrivals->GetRows())
		{
			UArrivalRowViewModel* Was = Before.FindRef(Row->Flight.Get());
			Same += (Was != nullptr && Was == Row) ? 1 : 0;
		}
		TestEqual(*FString::Printf(TEXT("%s: every flight that was on the board still has THE SAME row object"), Case), Same, 4);
	};

	// ACCEPT ONE MORE, the clock where it was: the new row composes, nothing else does.
	UFlight* Arriving = Flight(TEXT("CU 5"), EFlightPhase::Accepted);
	Arriving->ArrivesAt = 5400.0;
	Arriving->ContractSeconds = 5400.0;
	Board->AddOffer(*Clock, Arriving);
	Arrivals->Refresh(*Board, *Clock);
	TestEqual(TEXT("five rows"), Arrivals->GetCount(), 5);
	TestEqual(TEXT("accepting one flight composes exactly once - the new row, not the four that were there"), Arrivals->ComposeCountForTest() - Composed, 1);
	KeptTheirObjects(TEXT("after the accept"));

	// A NEW HOLDER AHEAD IN THE QUEUE: it composes, and so does the one it moved down (its title carries "#2" now) - and no other row.
	const int32 AfterAccept = Arrivals->ComposeCountForTest();
	UFlight* Ahead = Flight(TEXT("CU 6"), EFlightPhase::Inbound);
	Ahead->HoldingSince = 1.0;
	Ahead->ContractSeconds = 3600.0;
	Board->AddOffer(*Clock, Ahead);
	Arrivals->Refresh(*Board, *Clock);
	TestEqual(TEXT("a flight that joins the queue ahead composes itself and the holder it renumbered - two rows, not six"),
		Arrivals->ComposeCountForTest() - AfterAccept, 2);
	KeptTheirObjects(TEXT("after the queue changed"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsRowNamesItsRunwayTest, "AirportMgr.UI.Arrivals.RowNamesItsRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsRowNamesItsRunwayTest::RunTest(const FString& Parameters)
{
	// 2026-10-01, "show what runway has been chosen": the list's RefreshText hands each row the name RunwayOf gives its flight
	// (the panel's is FlightRunway::Names), the status says it in the phase's words, and the name is in the key - so a row
	// recomposes when its runway changes (a parallel built beside it re-letters 09 to 09L) and holds when nothing moved.
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Holding = Flight(TEXT("CU 1"), EFlightPhase::Inbound);
	Holding->HoldingSince = 5.0;
	UFlight* Landing = Flight(TEXT("CU 2"), EFlightPhase::Landing);
	UFlight* OnStand = Flight(TEXT("CU 3"), EFlightPhase::Turnaround);
	for (UFlight* Each : { Holding, Landing, OnStand }) { Board->AddOffer(*Clock, Each); }
	UArrivalsViewModel* List = NewObject<UArrivalsViewModel>();
	List->SyncRows(*Board);
	FString Name = TEXT("09L");
	const auto RunwayOf = [&Name](const UFlight& F) { return F.GetPhase() == EFlightPhase::Turnaround ? FString() : Name; };
	List->RefreshText(*Clock, RunwayOf);
	auto StatusOf = [List](const UFlight* F)
	{
		for (const UArrivalRowViewModel* Row : List->GetRows()) { if (Row->Flight.Get() == F) { return Row->GetStatus().ToString(); } }
		return FString();
	};
	TestEqual(TEXT("holding names what it waits for"), StatusOf(Holding), FString(TEXT("HOLDING for 09L")));
	TestEqual(TEXT("landing names its runway"), StatusOf(Landing), FString(TEXT("LANDING 09L")));
	TestEqual(TEXT("a phase with no runway keeps its bare word - no dangling 'for'"), StatusOf(OnStand), FString(TEXT("ON STAND")));

	const int32 Settled = List->ComposeCountForTest();
	List->RefreshText(*Clock, RunwayOf);
	TestEqual(TEXT("nothing moved, nothing recomposes"), List->ComposeCountForTest(), Settled);
	Name = TEXT("09R");
	List->RefreshText(*Clock, RunwayOf);
	TestEqual(TEXT("the runway changed: the two rows naming it recompose"), List->ComposeCountForTest(), Settled + 2);
	TestEqual(TEXT("and say the new name"), StatusOf(Landing), FString(TEXT("LANDING 09R")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsInspectSelectsTheAircraftTest, "AirportMgr.UI.Arrivals.InspectSelectsTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsInspectSelectsTheAircraftTest::RunTest(const FString& Parameters)
{
	// A ROW'S INSPECT (2026-10-02, "once the aircraft has been spawned, open the inspection tab from there"): shown on a flight with a
	// live aircraft and on no other, bound to its row's flight - not its slot - and it selects that aircraft and moves the camera to it,
	// so the inspector opens on it. The flight's aircraft leaving takes the button away again.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = TestGraph::Node(Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(Net, 200000.0, 0.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(Net, A, B, Options);
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
		UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	for (int32 Frame = 0; Frame < 30; ++Frame) { Actor->Tick(1.0f / 30.0f); }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);

	UArrivalsPanelWidget* Panel = CreateWidget<UArrivalsPanelWidget>(TestWorld.World, UArrivalsPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an arrivals panel"), Panel)) { return false; }
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Landed = Flight(TEXT("CU 1"), EFlightPhase::TaxiIn);
	Landed->AgentId = Id;
	UFlight* Inbound = Flight(TEXT("CU 2"), EFlightPhase::Accepted);
	for (UFlight* Each : { Landed, Inbound }) { Board->AddOffer(*Clock, Each); }
	Panel->GetArrivals()->Refresh(*Board, *Clock);
	Panel->PaintRowsForTest();
	if (!TestEqual(TEXT("a row each"), Panel->RowCountForTest(), 2)) { return false; }
	const int32 LandedRow = Panel->InspectFlightForTest(0) == Landed ? 0 : 1;
	TestEqual(TEXT("each row's button is bound to its own flight"), Panel->InspectFlightForTest(LandedRow), static_cast<const UFlight*>(Landed));
	TestEqual(TEXT("and the other row's to the other"), Panel->InspectFlightForTest(1 - LandedRow), static_cast<const UFlight*>(Inbound));
	TestTrue(TEXT("a flight with an aircraft can be inspected"), Panel->IsInspectShownForTest(LandedRow));
	TestFalse(TEXT("one not yet spawned cannot"), Panel->IsInspectShownForTest(1 - LandedRow));

	TestTrue(TEXT("Inspect goes"), Panel->Inspect(*Landed, *C));
	const FRoadAgent* Agent = Actor->GetGroundTraffic()->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft"), Agent)) { return false; }
	TestEqual(TEXT("the aircraft is selected, so the inspector opens on it"), C->GetSelection().Kind, ESelectionKind::Aircraft);
	TestEqual(TEXT("that aircraft"), C->GetSelection().Id, Id);
	TestEqual(TEXT("and the camera goes to it"), C->GetViewFocus(), Agent->GroundPosition());
	TestFalse(TEXT("a flight with no aircraft goes nowhere"), Panel->Inspect(*Inbound, *C));

	// GONE: the board clears AgentId, the row recomposes (bHasAircraft is in its key), the button goes.
	Landed->AgentId = INDEX_NONE;
	Panel->GetArrivals()->Refresh(*Board, *Clock);
	Panel->PaintRowsForTest();
	TestFalse(TEXT("an aircraft that has gone takes its Inspect with it"), Panel->IsInspectShownForTest(LandedRow));
	return true;
}

#endif
