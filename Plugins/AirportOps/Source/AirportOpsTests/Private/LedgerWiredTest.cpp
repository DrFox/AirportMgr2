#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/OpsSave.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "OpsSaveTestHelpers.h"
#include "Present/OpsRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerIsComposedByTheRuntimeTest,
	"AirportOps.Runtime.LedgerIsComposedByTheRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerIsComposedByTheRuntimeTest::RunTest(const FString& Parameters)
{
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();

	// THE SEAM TEST the refactor contract asks for: the runtime grew two subobjects, and a
	// constructor that silently failed to make one would leave every fee posting to nothing at
	// all, with no error anywhere - the money would simply never move.
	TestNotNull(TEXT("the runtime composes a ledger, as it does every other subobject"),
		Runtime->GetLedger());
	TestNotNull(TEXT("and a pricing resolver"), Runtime->GetPricing());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerRoundTripTest,
	"AirportOps.Model.LedgerRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerRoundTripTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFuelService* Fuel = NewObject<UFuelService>();
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	URoadNetwork* Network = NewObject<URoadNetwork>();

	Ledger->Open(500000.0);
	Ledger->Post(12.0, ELedgerCategory::LandingFee, 1200.0, FText::FromString(TEXT("G-ABCD")));
	Pricing->LandingFeeMultiplier = 1.75;

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing),
		*Network, Snapshot);

	USimClock* LoadedClock = NewObject<USimClock>();
	UFlightBoard* LoadedBoard = NewObject<UFlightBoard>();
	UFuelService* LoadedFuel = NewObject<UFuelService>();
	ULedger* Loaded = NewObject<ULedger>();
	UPricing* LoadedPricing = NewObject<UPricing>();
	URoadNetwork* LoadedNetwork = NewObject<URoadNetwork>();

	if (!TestTrue(TEXT("the snapshot restores"),
		OpsSave::Restore(Snapshot,
			OpsSaveTest::Persistents(*LoadedClock, *LoadedBoard, *LoadedFuel, *Loaded, *LoadedPricing),
			*LoadedNetwork)))
	{
		return false;
	}

	TestEqual(TEXT("the balance came back, opening float and entries both"),
		Loaded->Balance(), 501200.0, 1e-6);
	TestEqual(TEXT("and so did the entry itself - the entries ARE the record, not a log of it"),
		Loaded->Entries().Num(), 1);
	TestEqual(TEXT("the player's own fee setting is saved: it is theirs, not the scenario's"),
		LoadedPricing->LandingFeeMultiplier, 1.75, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerAbsentBlobTest,
	"AirportOps.Model.LedgerAbsentBlob",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerAbsentBlobTest::RunTest(const FString& Parameters)
{
	// A SAVE FROM BEFORE THE LEDGER EXISTED. No version bump was needed for this slice
	// precisely because a missing blob already means "that system starts fresh" - and this is
	// the test that says so, rather than the comment on FOpsSnapshot::Version claiming it.
	FOpsSnapshot Snapshot;
	Snapshot.Version = 4;

	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFuelService* Fuel = NewObject<UFuelService>();
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	URoadNetwork* Network = NewObject<URoadNetwork>();

	Ledger->Open(500000.0);

	TestTrue(TEXT("a save from before the ledger still opens"),
		OpsSave::Restore(Snapshot,
			OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing), *Network));
	TestEqual(TEXT("and opens at the balance it was given rather than at zero, which is what a "
		"blank blob deserialised over the top would have left"),
		Ledger->Balance(), 500000.0, 1e-6);
	return true;
}

#endif
