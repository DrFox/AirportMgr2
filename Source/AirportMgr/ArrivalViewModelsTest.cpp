#include "CoreMinimal.h"
#include "ArrivalViewModels.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/SimClock.h"

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
		Out->Phase = Phase;
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
		F->Phase = Case.Key;
		TestEqual(*FString::Printf(TEXT("status %s"), Case.Value),
			UArrivalRowViewModel::DescribeStatus(*F, 0.0).ToString(), FString(Case.Value));
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

	F->Phase = EFlightPhase::Turnaround;
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

#endif
