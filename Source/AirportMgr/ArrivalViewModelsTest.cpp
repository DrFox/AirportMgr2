#include "CoreMinimal.h"
#include "ArrivalViewModels.h"
#include "ArrivalsPanelWidget.h"
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
 * contract line that turns into "late" once AirborneBy has passed.
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
	TestEqual(TEXT("holding: how long it has waited and what is left of the contract"),
		UArrivalRowViewModel::DescribeDetail(*F, 780.0, bLate).ToString(), FString(TEXT("waited 3 min · 47 min left")));
	TestFalse(TEXT("not late"), bLate);

	F->SetPhaseForTest(EFlightPhase::Turnaround);
	TestEqual(TEXT("on the ground: what is left"),
		UArrivalRowViewModel::DescribeDetail(*F, 780.0, bLate).ToString(), FString(TEXT("47 min left")));

	TestEqual(TEXT("past AirborneBy it is late, by how much"),
		UArrivalRowViewModel::DescribeDetail(*F, 3600.0 + 720.0, bLate).ToString(), FString(TEXT("12 min late")));
	TestTrue(TEXT("and flagged"), bLate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalsTurnaroundLineTest, "AirportMgr.Arrivals.TurnaroundLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalsTurnaroundLineTest::RunTest(const FString& Parameters)
{
	// THE CONTRACT ON THE AIRCRAFT CARD (2026-09-28): how long the airline gave, and how much of
	// it is left - the same words the ARRIVALS row uses, so the two cannot disagree.
	UFlight* F = Flight(TEXT("CU 1"), EFlightPhase::Turnaround);
	F->AcceptedAt = 0.0;
	F->ContractSeconds = 7200.0;
	TestEqual(TEXT("time left"), UArrivalRowViewModel::DescribeTurnaround(*F, 7200.0 - 47.0 * 60.0).ToString(),
		FString(TEXT("Turnaround 2 h \u00B7 47 min left")));
	TestEqual(TEXT("past the deadline"), UArrivalRowViewModel::DescribeTurnaround(*F, 7200.0 + 12.0 * 60.0).ToString(),
		FString(TEXT("Turnaround 2 h \u00B7 12 min late")));
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

#endif
