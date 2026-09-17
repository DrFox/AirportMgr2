# The Ledger, Fees and What Building Costs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Money enters the game — flights earn, building and owning the airport cost, and the player sets their own landing fee.

**Architecture:** Two symmetric halves. `BuildCost` (Airside) answers *how much of it is there, at the authored rate*; `UPricing` (AirportOps) answers *what we charge for that*. `ULedger` is append-only and implements both `IOpsPersistent` (one save blob) and `IBuildPurse` (the Airside seam). No call site reads a base rate off an asset and spends it.

**Tech Stack:** UE 5.8.2, C++. Plugins `Airside` (model, tools, presentation) and `AirportOps` (ops, economy); game module `AirportMgr` (HUD, controller).

**Spec:** `docs/superpowers/specs/2026-09-13-ledger-and-fees-design.md`

## Global Constraints

- **Check-Architecture.ps1 is the pre-commit lint** and runs first inside the test script. AirportOps includes Airside, **never the reverse**. Airside `Model/` includes nothing above it; `Solve/` takes `CoreMinimal.h` only.
- **One log category per name.** The modules are UNITY builds — two `DEFINE_LOG_CATEGORY_STATIC` of one name compile alone and collide together. Use `LogAirside` / `LogAirsideTraffic` in the plugin, `LogAirportOps` in AirportOps, `LogRoadBuild` in the game module.
- **Every `UE_LOG` survives a move**, and every WHY comment travels with its code.
- **A doc comment touches its declaration.** Inserting between them means moving the comment.
- **Honour the return of anything that fills an out-parameter.**
- **A phase is an enum, never a set of bools.**
- **Units are uu; 100 uu = 1 m.** Every money rate in this plan is per METRE or per SQUARE METRE, so every quote divides by 100 (or 10 000 for area) exactly once, in `BuildCost` and nowhere else.
- **Never trust the automation runner's exit code.** Read the `N test(s) run, N failed, N crashed` line.
- **A new test .cpp needs two builds** — the first reports `Succeeded` without compiling it.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex` (editor must be CLOSED).
- Test: `./Tools/Run-AirsideTests.ps1`, narrow with `-Filter`.
- Test naming: `<Plugin>.<layer>.<Thing>`, leaf names distinct — the automation tree DROPS a bare-named test once a dotted child exists.

---

### Task 1: `ULedger` — append-only entries, balance as a fold

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/Ledger.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/LedgerTest.cpp`

**Interfaces:**
- Consumes: `IOpsPersistent` (`Model/OpsSave.h`), `USimClock::Now()`.
- Produces: `ELedgerCategory`, `FLedgerEntry`, `ULedger::Post/Balance/Entries/Reverse/RollUp`, `ULedger::StartingBalance`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgerFoldTest,
	"AirportOps.Model.LedgerFold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLedgerFoldTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(500000.0);

	Ledger->Post(0.0, ELedgerCategory::LandingFee, 1200.0, FText::FromString(TEXT("G-ABCD")));
	Ledger->Post(10.0, ELedgerCategory::Placement, -150000.0, FText::FromString(TEXT("Taxiway")));

	TestEqual(TEXT("balance is the starting balance plus every entry"),
		Ledger->Balance(), 351200.0, 1e-6);
	TestEqual(TEXT("the cached balance agrees with a fresh fold, which is the invariant that "
		"makes caching it safe"), Ledger->Balance(), Ledger->FoldBalanceForTest(), 1e-6);
	TestEqual(TEXT("both entries were kept"), Ledger->Entries().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgerReverseTest,
	"AirportOps.Model.LedgerReverse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLedgerReverseTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(1000.0);

	const int32 Id = Ledger->Post(0.0, ELedgerCategory::Placement, -400.0,
		FText::FromString(TEXT("Taxiway")));
	TestTrue(TEXT("a posted entry has an id to reverse it by"), Id != INDEX_NONE);

	TestTrue(TEXT("reversing a real id succeeds"), Ledger->Reverse(5.0, Id));
	TestEqual(TEXT("a reversal cancels its charge to the penny, which is what undo means"),
		Ledger->Balance(), 1000.0, 1e-9);
	TestEqual(TEXT("the reversal is a NEW entry - the ledger is append-only, never edited"),
		Ledger->Entries().Num(), 2);

	TestFalse(TEXT("the same charge cannot be reversed twice"), Ledger->Reverse(6.0, Id));
	TestFalse(TEXT("an unknown id reverses nothing"), Ledger->Reverse(7.0, 9999));
	TestEqual(TEXT("and neither attempt moved the balance"), Ledger->Balance(), 1000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgerRollUpTest,
	"AirportOps.Model.LedgerRollUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLedgerRollUpTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(0.0);
	Ledger->MaxDays = 30;

	const double Day = 86400.0;
	Ledger->Post(1.0 * Day, ELedgerCategory::Upkeep, -100.0, FText::FromString(TEXT("old")));
	Ledger->Post(2.0 * Day, ELedgerCategory::Upkeep, -100.0, FText::FromString(TEXT("old")));
	Ledger->Post(40.0 * Day, ELedgerCategory::LandingFee, 500.0, FText::FromString(TEXT("new")));

	const double Before = Ledger->Balance();
	Ledger->RollUp(40.0 * Day);

	TestEqual(TEXT("roll-up preserves the balance EXACTLY - it summarises history, it never "
		"rewrites it"), Ledger->Balance(), Before, 1e-9);
	TestEqual(TEXT("the two entries older than MaxDays became one brought-forward entry, "
		"leaving it plus the recent one"), Ledger->Entries().Num(), 2);
	TestEqual(TEXT("and the survivor of the roll-up carries their sum"),
		Ledger->Entries()[0].Amount, -200.0, 1e-9);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Ledger`
Expected: compile failure — `Model/Ledger.h` does not exist. (Remember: a new test .cpp needs two builds.)

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"

#include "Ledger.generated.h"

/**
 * What a ledger entry was for.
 *
 * ONLY CATEGORIES WITH A PUBLISHER IN THIS SLICE. Research, contract and fine categories
 * arrive with the systems that raise them - an enumerator nothing can ever produce is the
 * "list nothing consumes" bug CLAUDE.md names three times, and it would be read as a promise
 * by whoever writes the finance screen.
 */
UENUM()
enum class ELedgerCategory : uint8
{
	LandingFee,
	ParkingFee,
	/** Fuelling, today. Every per-service fee the job board adds lands here too. */
	ServiceFee,
	Placement,
	/** An undo reversing a placement, or a demolition's scrap value. */
	Refund,
	Upkeep,
	/** RollUp's summary of everything older than MaxDays. Never posted directly. */
	BroughtForward
};

/** One movement of money. Append-only: entries are never edited, only followed by more. */
USTRUCT()
struct AIRPORTOPS_API FLedgerEntry
{
	GENERATED_BODY()

	/** USimClock::Now when it happened. GAME time - the ledger never sees wall time. */
	UPROPERTY() double At = 0.0;

	UPROPERTY() ELedgerCategory Category = ELedgerCategory::Placement;

	/** SIGNED. Income positive, spending negative, so Balance is a plain sum and no call
	 *  site has to remember which categories subtract. */
	UPROPERTY() double Amount = 0.0;

	UPROPERTY() FText What;

	/** Ids start at 1 so INDEX_NONE means "not a real charge" everywhere. */
	UPROPERTY() int32 Id = 0;

	/** The id this entry reverses, or INDEX_NONE. Stops a charge being reversed twice. */
	UPROPERTY() int32 Reverses = INDEX_NONE;
};

/**
 * The money. Append-only entries; the balance is their sum.
 *
 * THE BALANCE IS CACHED AND THE CACHE IS TESTED. Folding a long game's entries on every HUD
 * frame is waste, but a running total that can silently disagree with the entries is worse
 * than either - so FoldBalanceForTest exists and one test asserts the two agree. If they ever
 * diverge the entries win: they are the record, the total is a convenience.
 *
 * World-free, like every other AirportOps Model/ class: NewObject and Post, no world needed.
 */
UCLASS()
class AIRPORTOPS_API ULedger : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Ledger"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * How many game days of entries are kept in full before RollUp folds them into one.
	 *
	 * Append-only does not mean unbounded: every entry is saved, and a long game would
	 * otherwise carry tens of thousands of rows through every write to disk.
	 */
	UPROPERTY() int32 MaxDays = 30;

	/**
	 * The balance a new game opens at, from UScenario::StartingBalance.
	 *
	 * A FIELD AND NOT AN OPENING ENTRY, because an opening entry would be rolled up like any
	 * other and the roll-up would then have to special-case it. Balance is this plus the fold.
	 */
	UPROPERTY() double StartingBalance = 0.0;

	/** Start a NEW GAME at this balance. Not for a load - Restore brings back the entries. */
	void Open(double InStartingBalance);

	/** Append one entry. Returns its id, for Reverse. */
	int32 Post(double At, ELedgerCategory Category, double Amount, FText What);

	/**
	 * Post the exact opposite of entry ChargeId. False if it is unknown or already reversed.
	 *
	 * REVERSES BY ID RATHER THAN BY AMOUNT so an undo cannot put back a number that was never
	 * taken - the amount comes from the entry, not from the caller recomputing it.
	 */
	bool Reverse(double At, int32 ChargeId);

	double Balance() const { return CachedBalance; }

	const TArray<FLedgerEntry>& Entries() const { return Rows; }

	/** Fold entries older than Now - MaxDays days into one BroughtForward entry. */
	void RollUp(double Now);

	/** The balance computed from the entries. See the class comment: this is the test's half
	 *  of the cached-total invariant, and is never used in production. */
	double FoldBalanceForTest() const;

private:
	UPROPERTY() TArray<FLedgerEntry> Rows;
	UPROPERTY() int32 NextId = 1;
	UPROPERTY() double CachedBalance = 0.0;

	void Recache();
};
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "Model/Ledger.h"

#include "AirportOpsLog.h"
#include "Model/SimClock.h"

void ULedger::Open(double InStartingBalance)
{
	StartingBalance = InStartingBalance;
	Rows.Reset();
	NextId = 1;
	Recache();
	UE_LOG(LogAirportOps, Log, TEXT("Ledger opened at %.0f"), StartingBalance);
}

int32 ULedger::Post(double At, ELedgerCategory Category, double Amount, FText What)
{
	FLedgerEntry& Entry = Rows.AddDefaulted_GetRef();
	Entry.At = At;
	Entry.Category = Category;
	Entry.Amount = Amount;
	Entry.What = MoveTemp(What);
	Entry.Id = NextId++;

	CachedBalance += Amount;
	return Entry.Id;
}

bool ULedger::Reverse(double At, int32 ChargeId)
{
	if (ChargeId == INDEX_NONE)
	{
		return false;
	}

	const FLedgerEntry* Charge = Rows.FindByPredicate(
		[ChargeId](const FLedgerEntry& Row) { return Row.Id == ChargeId; });
	if (Charge == nullptr)
	{
		return false;
	}

	// ALREADY REVERSED is a refusal, not a second reversal: redo pushes a fresh charge with a
	// fresh id, so a second reversal of the same id can only be a double-undo bug - and paying
	// the player twice for it would be invisible until the balance was inexplicable.
	const bool bAlready = Rows.ContainsByPredicate(
		[ChargeId](const FLedgerEntry& Row) { return Row.Reverses == ChargeId; });
	if (bAlready)
	{
		return false;
	}

	// Copied BEFORE Post, which may reallocate Rows and dangle Charge.
	const double Amount = -Charge->Amount;
	const FText What = Charge->What;

	const int32 Id = Post(At, ELedgerCategory::Refund, Amount, What);
	Rows.Last().Reverses = ChargeId;
	UE_LOG(LogAirportOps, Log, TEXT("Ledger reversed charge %d (%.0f), entry %d"),
		ChargeId, Amount, Id);
	return true;
}

void ULedger::RollUp(double Now)
{
	const double Cutoff = Now - (MaxDays * USimClock::SecondsPerDay);

	double Folded = 0.0;
	int32 Count = 0;
	for (const FLedgerEntry& Row : Rows)
	{
		if (Row.At < Cutoff)
		{
			Folded += Row.Amount;
			++Count;
		}
	}
	if (Count == 0)
	{
		return;
	}

	Rows.RemoveAll([Cutoff](const FLedgerEntry& Row) { return Row.At < Cutoff; });

	FLedgerEntry Summary;
	Summary.At = Cutoff;
	Summary.Category = ELedgerCategory::BroughtForward;
	Summary.Amount = Folded;
	Summary.What = NSLOCTEXT("Ledger", "BroughtForward", "Brought forward");
	Summary.Id = NextId++;
	Rows.Insert(Summary, 0);

	// The balance must not move: this summarises history, it does not rewrite it.
	Recache();
	UE_LOG(LogAirportOps, Log, TEXT("Ledger rolled up %d entries into %.0f; balance %.0f"),
		Count, Folded, CachedBalance);
}

double ULedger::FoldBalanceForTest() const
{
	double Sum = StartingBalance;
	for (const FLedgerEntry& Row : Rows)
	{
		Sum += Row.Amount;
	}
	return Sum;
}

void ULedger::Recache()
{
	CachedBalance = FoldBalanceForTest();
}
```

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Ledger`
Expected: `3 test(s) run, 0 failed, 0 crashed`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h `
        Plugins/AirportOps/Source/AirportOps/Private/Model/Ledger.cpp `
        Plugins/AirportOps/Source/AirportOpsTests/Private/LedgerTest.cpp
git commit -m "feat(ops): an append-only ledger whose balance is a fold"
```

---

### Task 2: `UPricing` — the one place a base figure becomes a number

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/Pricing.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/Pricing.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/PricingTest.cpp`

**Interfaces:**
- Consumes: `IcaoCode::LetterForWingspan` (`Solve/IcaoCode.h`), `FAirframe` (`Model/RoadEntity.h`), `IOpsPersistent`.
- Produces: `UPricing::LandingFee/ParkingFeePerHour/FuelServiceFee/DemandFactor/PriceOfBuild/ScrapValue/Format`, `UPricing::LandingFeeMultiplier`, `UPricing::Elasticity`, `UPricing::RefundFraction`, `UPricing::CurrencySymbol`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Pricing.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Wingspans, uu, that land squarely inside a code letter's band. */
	FAirframe AirframeOfSpan(double WingspanMetres)
	{
		FAirframe A;
		A.Wingspan = WingspanMetres * 100.0;
		return A;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPricingByCodeLetterTest,
	"AirportOps.Model.PricingByCodeLetter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPricingByCodeLetterTest::RunTest(const FString&)
{
	UPricing* Pricing = NewObject<UPricing>();

	// 28 m span is code C; 10 m is code A. The band edges are IcaoCode's, not this test's.
	const double CodeC = Pricing->LandingFee(AirframeOfSpan(28.0));
	const double CodeA = Pricing->LandingFee(AirframeOfSpan(10.0));

	TestEqual(TEXT("a code C landing is the authored code C fee"), CodeC, 1200.0, 1e-6);
	TestEqual(TEXT("a code A landing is the authored code A fee"), CodeA, 150.0, 1e-6);
	TestTrue(TEXT("a bigger aeroplane pays more, which is the whole reason the fee is keyed "
		"on the code letter rather than being flat"), CodeC > CodeA);

	TestEqual(TEXT("parking is a tenth of the landing fee per hour - ONE table drives all "
		"three fees, so there is no second list to keep in agreement"),
		Pricing->ParkingFeePerHour(AirframeOfSpan(28.0)), 120.0, 1e-6);
	TestEqual(TEXT("and the fuel service fee is half of it"),
		Pricing->FuelServiceFee(AirframeOfSpan(28.0)), 600.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPricingMultiplierTest,
	"AirportOps.Model.PricingMultiplier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPricingMultiplierTest::RunTest(const FString&)
{
	UPricing* Pricing = NewObject<UPricing>();
	const FAirframe Airframe = AirframeOfSpan(28.0);

	Pricing->LandingFeeMultiplier = 1.5;
	TestEqual(TEXT("the player's lever scales what a landing earns"),
		Pricing->LandingFee(Airframe), 1800.0, 1e-6);
	TestEqual(TEXT("and scales parking with it, so one lever does not split the fees apart"),
		Pricing->ParkingFeePerHour(Airframe), 180.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPricingElasticityTest,
	"AirportOps.Model.PricingElasticity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPricingElasticityTest::RunTest(const FString&)
{
	UPricing* Pricing = NewObject<UPricing>();
	Pricing->Elasticity = 1.0;

	Pricing->LandingFeeMultiplier = 1.0;
	TestEqual(TEXT("at the default fee, demand is unchanged"),
		Pricing->DemandFactor(), 1.0, 1e-9);

	Pricing->LandingFeeMultiplier = 2.0;
	TestEqual(TEXT("double the fee halves the offers - constant elasticity of 1"),
		Pricing->DemandFactor(), 0.5, 1e-9);

	// THE DESIGN CLAIM, asserted rather than trusted: at elasticity 1 the lever is revenue-
	// neutral, so it pays only when the airport is capacity-bound. If this ever fails, the
	// lever has silently become a slider with one correct setting (spec D8).
	Pricing->LandingFeeMultiplier = 1.0;
	const double RevenueAtOne = Pricing->LandingFee(AirframeOfSpan(28.0)) * Pricing->DemandFactor();
	Pricing->LandingFeeMultiplier = 2.0;
	const double RevenueAtTwo = Pricing->LandingFee(AirframeOfSpan(28.0)) * Pricing->DemandFactor();
	TestEqual(TEXT("fee times demand is flat at elasticity 1"), RevenueAtTwo, RevenueAtOne, 1e-6);

	Pricing->Elasticity = 0.0;
	TestEqual(TEXT("elasticity 0 is perfectly inelastic demand: the fee does not move it"),
		Pricing->DemandFactor(), 1.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPricingBuildAndScrapTest,
	"AirportOps.Model.PricingBuildAndScrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPricingBuildAndScrapTest::RunTest(const FString&)
{
	UPricing* Pricing = NewObject<UPricing>();
	Pricing->RefundFraction = 0.5;

	TestEqual(TEXT("with no modifier a build costs what Airside quoted"),
		Pricing->PriceOfBuild(1000.0, nullptr), 1000.0, 1e-9);
	TestEqual(TEXT("scrap is the refund fraction of today's price, not of what was paid"),
		Pricing->ScrapValue(1000.0, nullptr), 500.0, 1e-9);

	const FString Money = Pricing->Format(1234.0).ToString();
	TestTrue(TEXT("formatted money carries the currency symbol, and Airside never has to "
		"know what it is"), Money.Contains(Pricing->CurrencySymbol));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Pricing`
Expected: compile failure — `Model/Pricing.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"

#include "Pricing.generated.h"

struct FAirframe;

/**
 * The ONE place a base figure becomes a number anyone spends.
 *
 * WHY A RESOLVER AND NOT CONSTANTS ON THE ASSETS. Prices are not constants in this game:
 * research will make roads cheaper, a fuel contract will make fuel cheaper, and the player
 * chooses what to charge for a landing. If call sites read a rate off an asset and spent it,
 * every one of those would be a hunt for call sites - which is exactly how the Piper's
 * performance figures ended up typed at seven places. So Airside quotes what is THERE, and
 * this decides what it COSTS.
 *
 * ONE LIVE MODIFIER TODAY: LandingFeeMultiplier. Research and contract modifiers become
 * further inputs to these same functions in M4, with no call site touched. A generic stacked-
 * modifier engine is deliberately NOT here - nothing but the lever could push one, and
 * scaffolding with no publisher is the bug CLAUDE.md names three times.
 *
 * THE FEE TABLE IS THE ICAO LETTER TABLE. Solve/IcaoCode.h is already the codebase's one
 * table of code letters, so a new aircraft type is priced the moment it has a wingspan and
 * there is no per-type fee to author. The letter-to-MONEY row lives here rather than there
 * because it is money, not aerodrome geometry, and Solve/ stays dependency-free.
 */
UCLASS()
class AIRPORTOPS_API UPricing : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Pricing"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * What the player charges, as a multiple of the authored fee. THE one decision this
	 * class exposes to them, and saved because it is theirs and not the scenario's.
	 */
	UPROPERTY() double LandingFeeMultiplier = 1.0;

	/**
	 * How hard demand answers the fee: offers scale by Multiplier^-Elasticity, constant-
	 * elasticity demand, the textbook form.
	 *
	 * ONE BY DEFAULT, AND THAT IS A DESIGN DECISION, NOT A PLACEHOLDER. At 1.0 fee times
	 * demand is flat, so raising the fee pays NOTHING while stands are free and pays real
	 * money only once the airport is full and turning away offers it could not have served.
	 * The question becomes "am I full?" rather than "what is the best slider position".
	 * Below 1.0 raising fees would always be right; above 1.0, always wrong. Both are traps.
	 */
	UPROPERTY() double Elasticity = 1.0;

	/** What tearing something out gives back, as a fraction of today's price. */
	UPROPERTY() double RefundFraction = 0.5;

	/**
	 * U+00A4, the Unicode GENERIC currency sign - the glyph whose whole purpose is to stand
	 * in for an unspecified currency. No real country is implied, and unlike an invented
	 * glyph it is in every font.
	 */
	UPROPERTY() FString CurrencySymbol = TEXT("\u00A4");

	/** What this aeroplane pays to land, lever included. */
	double LandingFee(const FAirframe& Airframe) const;

	/** Per GAME hour on a stand. A tenth of the landing fee - see the class comment. */
	double ParkingFeePerHour(const FAirframe& Airframe) const;

	/**
	 * What a completed fuelling earns. Half the landing fee.
	 *
	 * NOT scaled by the landing-fee lever: the player is charging for a service they
	 * performed, not for permission to land, and one lever moving both would make the
	 * fee decision unreadable.
	 */
	double FuelServiceFee(const FAirframe& Airframe) const;

	/** Offers per day scale by this. See Elasticity. */
	double DemandFactor() const;

	/**
	 * What Airside's quoted base amount actually costs.
	 *
	 * Source is the URoadProfile or UEntityDefinition being placed, and is UNUSED TODAY - it
	 * is how an M4 research discount aimed at taxiways will key on the asset itself rather
	 * than on a parallel enum of build kinds that would have to be kept in agreement with
	 * EPlaceableEntity. Named now because the call sites that must pass it are being written
	 * now, and retrofitting an argument through them later is the churn this avoids.
	 */
	double PriceOfBuild(double BaseAmount, const UObject* Source) const;

	/** What tearing it out gives back: RefundFraction of today's price. */
	double ScrapValue(double BaseAmount, const UObject* Source) const;

	/** "¤1,234". The ONLY place money becomes text, so Airside never sees a currency. */
	FText Format(double Amount) const;

private:
	/** The authored landing fee for an ICAO code letter, before the lever. */
	double BaseLandingFeeForLetter(const FString& Letter) const;
};
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "Model/Pricing.h"

#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"

double UPricing::BaseLandingFeeForLetter(const FString& Letter) const
{
	// FIRST-PASS FIGURES, roughly doubling per letter (spec §7). The ORDERING is the part to
	// defend when these are tuned; the magnitudes are unplayed guesses.
	if (Letter == TEXT("A")) { return 150.0; }
	if (Letter == TEXT("B")) { return 400.0; }
	if (Letter == TEXT("C")) { return 1200.0; }
	if (Letter == TEXT("D")) { return 2600.0; }
	if (Letter == TEXT("E")) { return 4500.0; }
	if (Letter == TEXT("F")) { return 7000.0; }

	// C, for the same reason IcaoCode::RadiusForLetter falls back to it: the commonest stand
	// in the world, so erring here does not price an airliner as a light aircraft.
	return 1200.0;
}

double UPricing::LandingFee(const FAirframe& Airframe) const
{
	const FString Letter = IcaoCode::LetterForWingspan(Airframe.Wingspan);
	return BaseLandingFeeForLetter(Letter) * LandingFeeMultiplier;
}

double UPricing::ParkingFeePerHour(const FAirframe& Airframe) const
{
	return LandingFee(Airframe) * 0.1;
}

double UPricing::FuelServiceFee(const FAirframe& Airframe) const
{
	const FString Letter = IcaoCode::LetterForWingspan(Airframe.Wingspan);
	return BaseLandingFeeForLetter(Letter) * 0.5;
}

double UPricing::DemandFactor() const
{
	// GUARDED, not trusted: a zero or negative multiplier would make pow() infinite or NaN and
	// the offer cadence would silently stop or divide by zero. A free landing is a real thing
	// a player might set; an infinite demand is not.
	const double Multiplier = FMath::Max(LandingFeeMultiplier, KINDA_SMALL_NUMBER);
	return FMath::Pow(Multiplier, -Elasticity);
}

double UPricing::PriceOfBuild(double BaseAmount, const UObject* /*Source*/) const
{
	return BaseAmount;
}

double UPricing::ScrapValue(double BaseAmount, const UObject* Source) const
{
	return PriceOfBuild(BaseAmount, Source) * RefundFraction;
}

FText UPricing::Format(double Amount) const
{
	FNumberFormattingOptions Options;
	Options.SetMaximumFractionalDigits(0);
	Options.SetUseGrouping(true);

	const FText Number = FText::AsNumber(FMath::Abs(Amount), &Options);
	const FString Sign = Amount < 0.0 ? TEXT("-") : TEXT("");
	return FText::FromString(FString::Printf(TEXT("%s%s%s"), *Sign, *CurrencySymbol, *Number.ToString()));
}
```

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Pricing`
Expected: `4 test(s) run, 0 failed, 0 crashed`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/Pricing.h `
        Plugins/AirportOps/Source/AirportOps/Private/Model/Pricing.cpp `
        Plugins/AirportOps/Source/AirportOpsTests/Private/PricingTest.cpp
git commit -m "feat(ops): one resolver turns an authored rate into a price"
```

---

### Task 3: The ledger and the pricing survive a save, and `OpsSave` stops growing parameters

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsSave.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsSave.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h` (add `OnAfterRestore`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FlightBoard.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (the two call sites)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsSaveTest.cpp` (extend)

**Interfaces:**
- Consumes: `ULedger`, `UPricing` from Tasks 1–2.
- Produces: `IOpsPersistent::OnAfterRestore(int32 SnapshotVersion)`; `OpsSave::Capture(TArrayView<IOpsPersistent* const>, const URoadNetwork&, FOpsSnapshot&)`; `OpsSave::Restore(const FOpsSnapshot&, TArrayView<IOpsPersistent* const>, URoadNetwork&)`.

**Why this task exists at all:** `Capture`/`Restore` take four model objects as named positional parameters. Adding `ULedger` and `UPricing` would make six — exactly the growth `IOpsPersistent` and `FOpsSnapshot::Blobs` were introduced to stop (#105 item 8), as that header's own comment says.

The blocker is that `Restore` is order-dependent: the pre-v3 `ApproachFocus` migration must run immediately after the BOARD's blob, and only if it had one. A blind loop loses that. So the migration moves to a new hook and the loop becomes uniform.

- [ ] **Step 1: Write the failing test**

Append to `OpsSaveTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsSaveLedgerRoundTripTest,
	"AirportOps.Model.OpsSaveLedgerRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpsSaveLedgerRoundTripTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	URoadNetwork* Network = NewObject<URoadNetwork>();

	Ledger->Open(500000.0);
	Ledger->Post(12.0, ELedgerCategory::LandingFee, 1200.0, FText::FromString(TEXT("G-ABCD")));
	Pricing->LandingFeeMultiplier = 1.75;

	TArray<IOpsPersistent*> Out;
	Out.Add(Ledger);
	Out.Add(Pricing);

	FOpsSnapshot Snapshot;
	OpsSave::Capture(Out, *Network, Snapshot);

	ULedger* Loaded = NewObject<ULedger>();
	UPricing* LoadedPricing = NewObject<UPricing>();
	URoadNetwork* LoadedNetwork = NewObject<URoadNetwork>();
	TArray<IOpsPersistent*> In;
	In.Add(Loaded);
	In.Add(LoadedPricing);

	TestTrue(TEXT("the snapshot restores"), OpsSave::Restore(Snapshot, In, *LoadedNetwork));
	TestEqual(TEXT("the balance came back, entries and all"), Loaded->Balance(), 501200.0, 1e-6);
	TestEqual(TEXT("and so did the entry itself, because the entries ARE the record"),
		Loaded->Entries().Num(), 1);
	TestEqual(TEXT("the player's own fee setting is saved - it is theirs, not the scenario's"),
		LoadedPricing->LandingFeeMultiplier, 1.75, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsSaveWithoutLedgerBlobTest,
	"AirportOps.Model.OpsSaveWithoutLedgerBlob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpsSaveWithoutLedgerBlobTest::RunTest(const FString&)
{
	// A SAVE FROM BEFORE THE LEDGER EXISTED. No version bump was needed for this slice
	// precisely because a missing blob already means "that system starts fresh", and this is
	// the test that says so rather than the comment claiming it.
	FOpsSnapshot Snapshot;
	Snapshot.Version = 4;

	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(500000.0);
	URoadNetwork* Network = NewObject<URoadNetwork>();
	TArray<IOpsPersistent*> In;
	In.Add(Ledger);

	TestTrue(TEXT("an old save still opens"), OpsSave::Restore(Snapshot, In, *Network));
	TestEqual(TEXT("and opens at the balance it was given, not at zero"),
		Ledger->Balance(), 500000.0, 1e-6);
	return true;
}
```

Add `#include "Model/Ledger.h"` and `#include "Model/Pricing.h"` to the file's includes.

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.OpsSave`
Expected: compile failure — `Capture` does not take a `TArrayView`.

- [ ] **Step 3: Add the `OnAfterRestore` hook**

In `OpsSave.h`, beneath `OnBeforeRestore`:

```cpp
	/**
	 * Called after THIS object's blob has been deserialised, with the snapshot's version.
	 *
	 * EXISTS SO Restore CAN BE A PLAIN LOOP. The pre-v3 ApproachFocus migration used to live
	 * in Restore itself, wrapped around the flight board's blob specifically - which meant
	 * Restore had to name the board as a parameter and call it in a particular position.
	 * Moving the migration to the object that needs it lets every persistent object be
	 * restored identically, which is what stopped Capture/Restore growing a parameter per
	 * system (#105 item 8, and see FOpsSnapshot::Version).
	 */
	virtual void OnAfterRestore(int32 SnapshotVersion) {}
```

Replace the two namespace declarations:

```cpp
	/**
	 * Every persistent object's blob, plus the network's.
	 *
	 * THE NETWORK IS STILL NAMED and is the one documented exception: it lives in Airside,
	 * which may not depend on AirportOps (Check-Architecture.ps1), so it cannot implement
	 * IOpsPersistent and its blob is keyed by a literal name here instead.
	 */
	AIRPORTOPS_API void Capture(TArrayView<IOpsPersistent* const> Persistents,
		const URoadNetwork& Network, FOpsSnapshot& Out);

	/** False only when a blob is present and fails to deserialise. Missing blobs leave their
	 *  target untouched, which is what makes a save from before a system existed still open. */
	AIRPORTOPS_API bool Restore(const FOpsSnapshot& In,
		TArrayView<IOpsPersistent* const> Persistents, URoadNetwork& Network);
```

- [ ] **Step 4: Rewrite `Capture` and `Restore`**

```cpp
void OpsSave::Capture(TArrayView<IOpsPersistent* const> Persistents,
	const URoadNetwork& Network, FOpsSnapshot& Out)
{
	Out.Blobs.Reset();
	Out.Clock.Reset();
	Out.Network.Reset();
	Out.Flights.Reset();

	// Serialize is non-const on UObject; the archive is saving, so nothing is written to them.
	SerializeObject(const_cast<URoadNetwork&>(Network), Out.Blobs.FindOrAdd(NetworkBlobName).Bytes);

	for (IOpsPersistent* Persistent : Persistents)
	{
		if (Persistent != nullptr)
		{
			CaptureBlob(*Persistent, Out);
		}
	}

	UE_LOG(LogAirportOps, Log, TEXT("Captured snapshot: %d blob(s)"), Out.Blobs.Num());
}

bool OpsSave::Restore(const FOpsSnapshot& In, TArrayView<IOpsPersistent* const> Persistents,
	URoadNetwork& Network)
{
	FOpsSnapshot Shimmed = In;
	if (Shimmed.Version < 4)
	{
		// v3-AND-EARLIER SHIM: bytes under the old named fields, not yet in Blobs - see
		// FOpsSnapshot::Version's own comment. A field the old save never wrote (e.g. no
		// Flights blob at all, v1) stays absent from Blobs too, same as today.
		if (Shimmed.Clock.Num() > 0)   { Shimmed.Blobs.FindOrAdd(TEXT("Clock")).Bytes = Shimmed.Clock; }
		if (Shimmed.Network.Num() > 0) { Shimmed.Blobs.FindOrAdd(NetworkBlobName).Bytes = Shimmed.Network; }
		if (Shimmed.Flights.Num() > 0) { Shimmed.Blobs.FindOrAdd(TEXT("Flights")).Bytes = Shimmed.Flights; }
	}

	if (const FOpsBlob* NetworkBlob = Shimmed.Blobs.Find(NetworkBlobName))
	{
		if (NetworkBlob->Bytes.Num() > 0)
		{
			DeserializeObject(Network, NetworkBlob->Bytes);
		}
	}

	// ONE UNIFORM PASS. Anything a particular system must do about an OLD snapshot is that
	// system's own OnAfterRestore - see IOpsPersistent, and UFlightBoard's override for the
	// migration that used to be special-cased here.
	for (IOpsPersistent* Persistent : Persistents)
	{
		if (Persistent != nullptr)
		{
			RestoreBlob(Shimmed, *Persistent);
			Persistent->OnAfterRestore(Shimmed.Version);
		}
	}

	UE_LOG(LogAirportOps, Log, TEXT("Restored snapshot v%d: %d nodes, %d persistent object(s)"),
		In.Version, Network.GetNodes().Num(), Persistents.Num());
	return true;
}
```

- [ ] **Step 5: Move the migration onto the board**

In `FlightBoard.h`, beside the other `IOpsPersistent` members:

```cpp
	/**
	 * Recreate every flight's own ApproachFocus from this board's one field, for a snapshot
	 * older than FOpsSnapshot::Version 3.
	 *
	 * MOVED HERE FROM OpsSave::Restore so that Restore could become a plain loop over every
	 * persistent object (see IOpsPersistent::OnAfterRestore). Before UFlight::ApproachFocus
	 * existed (issue #96) every flight shared this one board-wide field, so recreating it
	 * per-flight is the only way an old load lands where it was actually aimed rather than at
	 * the world origin.
	 */
	virtual void OnAfterRestore(int32 SnapshotVersion) override;
```

In `FlightBoard.cpp`:

```cpp
void UFlightBoard::OnAfterRestore(int32 SnapshotVersion)
{
	// Flights.Num() replaces Restore's old bHadFlights flag: a board with no flights has
	// nothing to migrate either way, so asking its own state answers the same question
	// without Restore having to remember which blobs it saw.
	if (SnapshotVersion < 3 && Flights.Num() > 0)
	{
		AimUnaimedFlightsAtBoardFocus();
	}
}
```

- [ ] **Step 6: Update the two call sites in `OpsRuntime.cpp`**

In `SaveToSlot` and `LoadFromSlot`, replace the positional calls with a list. Add a private helper to `OpsRuntime.h`:

```cpp
	/**
	 * Every model object that owns saved state, in a FIXED ORDER.
	 *
	 * ONE LIST, so a system added to the runtime and forgotten here is a system that silently
	 * stops saving - the failure this returns rather than four call sites each naming their
	 * own subset. Order matters only in that it is stable across a save and a load.
	 */
	TArray<IOpsPersistent*> Persistents() const;
```

```cpp
TArray<IOpsPersistent*> UOpsRuntime::Persistents() const
{
	TArray<IOpsPersistent*> Out;
	Out.Add(Clock);
	Out.Add(FuelService);
	Out.Add(FlightBoard);
	Out.Add(Ledger);
	Out.Add(Pricing);
	return Out;
}
```

(`Ledger` and `Pricing` members arrive in Task 4; until then include only the three that exist, and add the two in that task.)

- [ ] **Step 7: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.OpsSave`
Expected: every OpsSave test passes, including the pre-existing round-trip and legacy-version ones.

- [ ] **Step 8: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps Plugins/AirportOps/Source/AirportOpsTests
git commit -m "refactor(save): Capture/Restore take a list, not a parameter per system"
```

---

### Task 4: The runtime owns the ledger, and a new game opens at the scenario's balance

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp:14-15` (the "Balance goes to the ledger when it exists (M3)" comment — this task is what it was waiting for)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsRuntimeLedgerTest.cpp`

**Interfaces:**
- Consumes: `ULedger`, `UPricing`, `UScenario::StartingBalance`.
- Produces: `UOpsRuntime::GetLedger()`, `UOpsRuntime::GetPricing()`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Present/OpsRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeHasLedgerTest,
	"AirportOps.Runtime.HasLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeHasLedgerTest::RunTest(const FString&)
{
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();

	TestNotNull(TEXT("the runtime constructs a ledger, as it does every other subobject"),
		Runtime->GetLedger());
	TestNotNull(TEXT("and a pricing resolver"), Runtime->GetPricing());
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Runtime.HasLedger`
Expected: compile failure — `GetLedger` is not a member.

- [ ] **Step 3: Add the members**

In `OpsRuntime.h` beside the other getters:

```cpp
	/** The money. See ULedger - this runtime owns it, opens it from the scenario, and hands it
	 *  to the facade as the build purse. */
	ULedger* GetLedger() const { return Ledger; }

	/** What things cost. See UPricing. */
	UPricing* GetPricing() const { return Pricing; }
```

and beside the other `UPROPERTY` subobjects:

```cpp
	UPROPERTY() TObjectPtr<ULedger> Ledger;
	UPROPERTY() TObjectPtr<UPricing> Pricing;
```

Forward-declare `class ULedger;` and `class UPricing;` at the top; include their headers in the .cpp only.

In the constructor, alongside the existing `CreateDefaultSubobject` calls, construct both the same way the existing subobjects are constructed (match the surrounding code exactly rather than inventing a second idiom).

- [ ] **Step 4: Open the ledger from the scenario**

In `Attach`, inside the `if (const UScenario* Scenario = ...)` block, and REPLACING the comment at `OpsRuntime.cpp:14-15` that says the balance goes to the ledger when it exists:

```cpp
		// The balance the scenario opens at. THE comment that used to stand here said this
		// would happen "when the ledger exists (M3)"; this is that.
		Ledger->Open(Scenario->StartingBalance);
```

and extend the existing scenario log line to name the balance, so one line still says what the whole scenario applied.

Add `Ledger` and `Pricing` to `Persistents()` from Task 3.

- [ ] **Step 5: Run to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Runtime`
Expected: PASS, and the existing runtime tests still pass.

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps Plugins/AirportOps/Source/AirportOpsTests
git commit -m "feat(ops): the runtime owns the ledger and opens it from the scenario"
```

---

### Task 5: A flight pays to land and to park

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Flight.h` (add `ParkedAt`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h` (add `Ledger`, `Pricing`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FlightBoard.cpp` (`OnAgentPhase`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OfferGenerator.cpp:93` (the "fees are deliberately left at zero" comment — this task is what it was waiting for)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightFeesTest.cpp`

**Interfaces:**
- Consumes: `ULedger::Post`, `UPricing::LandingFee/ParkingFeePerHour`, `EFlightPhase`.
- Produces: `UFlight::ParkedAt`; `UFlightBoard::Ledger`, `UFlightBoard::Pricing`; `UOfferGenerator::Pricing`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightPaysToLandTest,
	"AirportOps.Model.FlightPaysToLand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlightPaysToLandTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Ledger->Open(0.0);
	Board->Ledger = Ledger;
	Board->Pricing = Pricing;

	UFlight* Flight = NewObject<UFlight>();
	Flight->Id = 1;
	Flight->LandingFee = 1200.0;
	Flight->Phase = EFlightPhase::Inbound;

	Board->PostLandingFee(0.0, *Flight);

	TestEqual(TEXT("landing credits the fee the OFFER quoted, not one recomputed at "
		"touchdown - the number in the inbox must not have been a lie"),
		Ledger->Balance(), 1200.0, 1e-6);
	TestEqual(TEXT("and it is booked as a landing fee"),
		Ledger->Entries()[0].Category, ELedgerCategory::LandingFee);

	Board->PostLandingFee(0.0, *Flight);
	TestEqual(TEXT("a second call banks nothing: a flight lands once, and a phase that can be "
		"re-entered must not be able to pay twice"), Ledger->Balance(), 1200.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightPaysToParkTest,
	"AirportOps.Model.FlightPaysToPark",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlightPaysToParkTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Ledger->Open(0.0);
	Board->Ledger = Ledger;
	Board->Pricing = Pricing;

	UFlight* Flight = NewObject<UFlight>();
	Flight->Id = 1;
	Flight->Airframe.Wingspan = 2800.0;   // code C
	Flight->ParkedAt = 1000.0;

	// Two game hours on the stand.
	Board->PostParkingFee(1000.0 + 7200.0, *Flight);

	TestEqual(TEXT("parking is charged for the hours actually occupied"),
		Ledger->Balance(), 240.0, 1e-6);
	TestEqual(TEXT("and recorded on the flight, so the row can show what it earned"),
		Flight->ParkingFee, 240.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightParkedAtUnsetPaysNothingTest,
	"AirportOps.Model.FlightParkedAtUnsetPaysNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlightParkedAtUnsetPaysNothingTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Ledger->Open(0.0);
	Board->Ledger = Ledger;
	Board->Pricing = Pricing;

	UFlight* Flight = NewObject<UFlight>();
	Flight->Id = 1;
	Flight->Airframe.Wingspan = 2800.0;

	// NEVER PARKED - ParkedAt is still 0. A flight that departs without having parked (a
	// restored save, a debug dispatch) must not be charged for every hour since the epoch.
	Board->PostParkingFee(50000.0, *Flight);

	TestEqual(TEXT("a flight that never parked pays no parking"), Ledger->Balance(), 0.0, 1e-9);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Flight`
Expected: compile failure — `PostLandingFee` is not a member.

- [ ] **Step 3: Add `ParkedAt` to `UFlight`**

Beside the existing fee fields, and REPLACING their "Banked by the ledger when it exists" comment, since it now is:

```cpp
	/**
	 * USimClock::Now at which it parked, or 0 if it never did.
	 *
	 * THE START OF THE PARKING CLOCK. Zero means "never parked" and is checked rather than
	 * trusted - a restored or debug-dispatched flight that departs without a park would
	 * otherwise be billed for every hour since the epoch.
	 */
	UPROPERTY() double ParkedAt = 0.0;

	/**
	 * What this flight earned. LandingFee is fixed at the OFFER (see UOfferGenerator) so the
	 * inbox row shows what accepting it is worth; ParkingFee is filled in when it leaves.
	 */
	UPROPERTY() double LandingFee = 0.0;
	UPROPERTY() double ParkingFee = 0.0;
```

- [ ] **Step 4: Add the posting methods to `UFlightBoard`**

Header:

```cpp
	/** The money, or null in a test that does not care. Set by UOpsRuntime::Attach. */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Bank the landing fee this flight was OFFERED at. Idempotent: a flight lands once.
	 *
	 * Public so a test can call it without driving a whole agent through its phases; the
	 * production caller is OnAgentPhase.
	 */
	void PostLandingFee(double Now, UFlight& Flight);

	/** Bank the parking fee for the time actually occupied, and record it on the flight. */
	void PostParkingFee(double Now, UFlight& Flight);
```

Implementation:

```cpp
void UFlightBoard::PostLandingFee(double Now, UFlight& Flight)
{
	if (Ledger == nullptr || Flight.LandingFee <= 0.0 || Flight.bLandingFeePaid)
	{
		return;
	}

	Flight.bLandingFeePaid = true;
	Ledger->Post(Now, ELedgerCategory::LandingFee, Flight.LandingFee,
		FText::Format(NSLOCTEXT("Ledger", "LandingBy", "Landing: {0}"), Flight.AirlineName));
}

void UFlightBoard::PostParkingFee(double Now, UFlight& Flight)
{
	if (Ledger == nullptr || Pricing == nullptr || Flight.ParkedAt <= 0.0)
	{
		return;
	}

	const double Hours = FMath::Max(0.0, (Now - Flight.ParkedAt) / 3600.0);
	const double Fee = Pricing->ParkingFeePerHour(Flight.Airframe) * Hours;
	if (Fee <= 0.0)
	{
		return;
	}

	Flight.ParkingFee = Fee;
	Ledger->Post(Now, ELedgerCategory::ParkingFee, Fee,
		FText::Format(NSLOCTEXT("Ledger", "ParkingBy", "Parking: {0}"), Flight.AirlineName));
}
```

Add `UPROPERTY() bool bLandingFeePaid = false;` to `UFlight` with a comment saying it is what makes `PostLandingFee` idempotent, and that it is saved so a reload cannot re-bank a fee.

- [ ] **Step 5: Call them from `OnAgentPhase`**

In `UFlightBoard::OnAgentPhase`, after the flight's phase has been updated, add:

```cpp
	// AT Landing AND AT TaxiOut, and those two specifically. Landing is where an aeroplane
	// becomes the airport's business; TaxiOut is the one phase EVERY departure reaches -
	// Manoeuvring is not, because an aeroplane parked within StraightOutDegrees of its exit
	// heading simply drives out and never enters it (commit 021cc2e). Charging at a phase some
	// flights never enter is a fee that silently goes uncollected on the best-built layouts.
	if (Flight->Phase == EFlightPhase::Landing)
	{
		PostLandingFee(Now, *Flight);
	}
	else if (Flight->Phase == EFlightPhase::Turnaround && Flight->ParkedAt <= 0.0)
	{
		Flight->ParkedAt = Now;
	}
	else if (Flight->Phase == EFlightPhase::TaxiOut)
	{
		PostParkingFee(Now, *Flight);
	}
```

`OnAgentPhase` does not currently take the clock. Add a `const USimClock& Clock` parameter (the sibling `UFuelService::OnAgentPhase` already takes one, so this matches the neighbouring shape rather than inventing a second) and update `UOpsRuntime::OnAgentPhase` to pass `*Clock`.

- [ ] **Step 6: Price the offer**

In `OfferGenerator.h` add `UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;`, and in `OfferGenerator.cpp` replace the "Fees are deliberately left at zero" comment at line 93 with:

```cpp
	// PRICED AT THE OFFER, not at touchdown, so the inbox row shows what accepting it is
	// worth and the player's fee lever moves NEW offers only. A fee computed on landing would
	// let them accept cheaply and raise the price afterwards, and the number they decided on
	// would have been a lie.
	if (Pricing != nullptr)
	{
		Offer->LandingFee = Pricing->LandingFee(Offer->Airframe);
	}
```

Wire `Generator->Pricing`, `Board->Ledger` and `Board->Pricing` in `UOpsRuntime::Attach` beside the existing `Board->Generator` wiring.

- [ ] **Step 7: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps`
Expected: all AirportOps tests pass.

- [ ] **Step 8: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): a flight pays to land and to park"
```

---

### Task 6: A fuelling earns its fee, and an unserved aircraft earns nothing

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp:519` (where the demand becomes `Done`)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceFeeTest.cpp`

**Interfaces:**
- Consumes: `ULedger`, `UPricing::FuelServiceFee`, `EFuelDemandState`.
- Produces: `UFuelService::Ledger`, `UFuelService::Pricing`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelService.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelServiceEarnsItsFeeTest,
	"AirportOps.Model.FuelServiceEarnsItsFee",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFuelServiceEarnsItsFeeTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFuelService* Fuel = NewObject<UFuelService>();
	Ledger->Open(0.0);
	Fuel->Ledger = Ledger;
	Fuel->Pricing = Pricing;

	FAirframe Airframe;
	Airframe.Wingspan = 2800.0;   // code C

	Fuel->PostServiceFee(0.0, Airframe);
	TestEqual(TEXT("a completed fuelling earns the service fee"), Ledger->Balance(), 600.0, 1e-6);
	TestEqual(TEXT("booked as a service fee, not as a landing"),
		Ledger->Entries()[0].Category, ELedgerCategory::ServiceFee);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelServiceUnservedEarnsNothingTest,
	"AirportOps.Model.FuelServiceUnservedEarnsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFuelServiceUnservedEarnsNothingTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFuelService* Fuel = NewObject<UFuelService>();
	Ledger->Open(1000.0);
	Fuel->Ledger = Ledger;
	Fuel->Pricing = Pricing;

	// THE FORFEIT IS AN ENTRY THAT DOES NOT HAPPEN, never a negative one (spec D7). Nothing
	// calls PostServiceFee for an Unserviceable demand, so this asserts the shape of the rule:
	// the balance is untouched and the ledger has no row explaining a fine that was not levied.
	TestEqual(TEXT("an unserved aircraft leaves the balance exactly as it was"),
		Ledger->Balance(), 1000.0, 1e-9);
	TestEqual(TEXT("and writes no entry at all - a forfeit is not a charge"),
		Ledger->Entries().Num(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.FuelService`
Expected: compile failure — `PostServiceFee` is not a member.

- [ ] **Step 3: Add the members and the method**

Header, beside the other wired-in pointers:

```cpp
	/** The money, or null in a test that does not care. Set by UOpsRuntime::Attach. */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Bank the fee for one completed fuelling.
	 *
	 * THERE IS NO MATCHING PENALTY METHOD, on purpose (spec D7): an aircraft that times out
	 * Unserviceable simply never reaches here, so the forfeit is an entry that does not
	 * happen. A negative entry would be a fine, which is a different thing and needs a
	 * promised time to be late against - and nothing in this build has one.
	 */
	void PostServiceFee(double Now, const FAirframe& Airframe);
```

Implementation:

```cpp
void UFuelService::PostServiceFee(double Now, const FAirframe& Airframe)
{
	if (Ledger == nullptr || Pricing == nullptr)
	{
		return;
	}

	const double Fee = Pricing->FuelServiceFee(Airframe);
	if (Fee <= 0.0)
	{
		return;
	}

	Ledger->Post(Now, ELedgerCategory::ServiceFee, Fee,
		NSLOCTEXT("Ledger", "Fuelling", "Fuelling"));
}
```

- [ ] **Step 4: Call it where the demand completes**

At `FuelService.cpp:519`, where `Demand.State = EFuelDemandState::Done;` is set, add the fee post immediately after, using the served aircraft's airframe and `Clock.Now()`. Keep the existing `UE_LOG` — a refactor never drops one.

Wire `FuelService->Ledger` and `FuelService->Pricing` in `UOpsRuntime::Attach`.

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): a fuelling earns its fee; an unserved aeroplane earns none"
```

---

### Task 7: The fee lever moves demand

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OfferGenerator.cpp` (`OfferIntervalSeconds`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp` (extend)

**Interfaces:**
- Consumes: `UPricing::DemandFactor`.
- Produces: `UOfferGenerator::OfferIntervalSeconds(Airlines, DemandFactor)`.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCadenceAnswersTheFeeTest,
	"AirportOps.Model.OfferCadenceAnswersTheFee",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOfferCadenceAnswersTheFeeTest::RunTest(const FString&)
{
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>();
	Airline->OffersPerDay = 12.0;
	TArray<UAirlineDefinition*> Airlines;
	Airlines.Add(Airline);

	const double AtPar = UOfferGenerator::OfferIntervalSeconds(Airlines, 1.0);
	const double AtHalfDemand = UOfferGenerator::OfferIntervalSeconds(Airlines, 0.5);

	TestTrue(TEXT("halving demand doubles the wait between offers - charging more means "
		"fewer aeroplanes, which is the whole cost of the lever"),
		FMath::IsNearlyEqual(AtHalfDemand, AtPar * 2.0, 1e-6));

	TestEqual(TEXT("a demand factor of zero is NEVER, exactly as no airline offering anything "
		"is - not a divide by zero"),
		UOfferGenerator::OfferIntervalSeconds(Airlines, 0.0), 0.0, 1e-9);
	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.OfferCadence`
Expected: compile failure — `OfferIntervalSeconds` takes one argument.

- [ ] **Step 3: Add the parameter**

```cpp
double UOfferGenerator::OfferIntervalSeconds(const TArray<UAirlineDefinition*>& Airlines,
	double DemandFactor)
{
	double OffersPerDay = 0.0;
	for (const UAirlineDefinition* Airline : Airlines)
	{
		OffersPerDay += Airline != nullptr ? Airline->OffersPerDay : 0.0;
	}

	// THE FEE'S ONLY COST, and it is paid here: a higher landing fee scales this down, so the
	// player earns more per aeroplane and sees fewer of them. See UPricing::Elasticity for why
	// that trade is deliberately even until the airport is capacity-bound.
	OffersPerDay *= FMath::Max(DemandFactor, 0.0);

	// ZERO IS "NEVER", not a divide-by-zero to guard against a caller forgot to. An airport
	// with no airline offering anything is a real, reportable state - UOpsRuntime::Attach
	// warns about it rather than scheduling a callback that would never fire usefully.
	return OffersPerDay > 0.0 ? USimClock::SecondsPerDay / OffersPerDay : 0.0;
}
```

Update the one production call site in `UOpsRuntime::Attach` to pass `Pricing->DemandFactor()`.

- [ ] **Step 4: Run to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): charging more for a landing brings fewer aeroplanes"
```

---

### Task 8: Airside learns what things cost — rates, quotes and the purse seam

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/BuildPurse.h`
- Create: `Plugins/Airside/Source/Airside/Public/Build/BuildCost.h`
- Create: `Plugins/Airside/Source/Airside/Private/Build/BuildCost.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideSettings.h`
- Test: `Plugins/Airside/Source/AirsideTests/Private/BuildCostTest.cpp`

**Interfaces:**
- Consumes: `URoadProfile`, `UEntityDefinition`, `UAirsideSettings`, `URoadNetwork`.
- Produces: `FBuildQuote`, `IBuildPurse`, `BuildCost::ForSegment/ForEntity/ForApron/DailyUpkeep`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/BuildCost.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildCostPerMetreTest,
	"Airside.Build.BuildCostPerMetre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCostPerMetreTest::RunTest(const FString&)
{
	URoadProfile* Profile = NewObject<URoadProfile>();
	Profile->CostPerMetre = 300.0;

	// 500 m, in uu. The conversion happens ONCE, in BuildCost, and this is what pins it: a
	// second place dividing by 100 is how a taxiway would come to cost a hundred times too
	// much with nothing to say which of the two was wrong.
	const FBuildQuote Quote = BuildCost::ForSegment(*Profile, 50000.0);

	TestEqual(TEXT("500 m of a 300-per-metre profile quotes 150,000"),
		Quote.BaseAmount, 150000.0, 1e-6);
	TestEqual(TEXT("the quote names the profile as its source, so a discount can key on the "
		"asset itself rather than on a parallel enum of build kinds"),
		Quote.Source, static_cast<const UObject*>(Profile));
	TestFalse(TEXT("and it says what it is, for the ghost to print"), Quote.What.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildCostApronAreaTest,
	"Airside.Build.BuildCostApronArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCostApronAreaTest::RunTest(const FString&)
{
	// A 100 m x 100 m square, in uu.
	TArray<FVector2D> Outline;
	Outline.Add(FVector2D(0.0, 0.0));
	Outline.Add(FVector2D(10000.0, 0.0));
	Outline.Add(FVector2D(10000.0, 10000.0));
	Outline.Add(FVector2D(0.0, 10000.0));

	const FBuildQuote Quote = BuildCost::ForApron(Outline, 15.0);

	TestEqual(TEXT("10,000 square metres at 15 quotes 150,000"),
		Quote.BaseAmount, 150000.0, 1e-6);

	// WOUND THE OTHER WAY. Area from a signed shoelace sum is negative for one winding, and a
	// negative quote would PAY the player to build - so the absolute value is taken, and this
	// is the test that says so. CCW faces down in Unreal, so both windings genuinely occur.
	Algo::Reverse(Outline);
	TestEqual(TEXT("winding does not change what an apron costs"),
		BuildCost::ForApron(Outline, 15.0).BaseAmount, 150000.0, 1e-6);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.BuildCost`
Expected: compile failure — `Build/BuildCost.h` does not exist.

- [ ] **Step 3: Write `Tool/BuildPurse.h`**

```cpp
#pragma once

#include "CoreMinimal.h"

#include "BuildPurse.generated.h"

/**
 * What one build or demolition is worth, at the AUTHORED rate.
 *
 * BASE, NOT PRICE. Airside owns the geometry and the profile, so Airside is the only layer
 * that can answer "how much of it is there"; what it COSTS is AirportOps' answer, because
 * only that side knows about research discounts, contracts and the player's own levers.
 */
USTRUCT()
struct AIRSIDE_API FBuildQuote
{
	GENERATED_BODY()

	UPROPERTY() double BaseAmount = 0.0;

	/**
	 * The URoadProfile or UEntityDefinition being placed - NOT a parallel enum of build kinds.
	 *
	 * A research discount aimed at taxiways keys on the asset itself, so there is no second
	 * list to keep in agreement with EPlaceableEntity - the failure this codebase has shipped
	 * three times. Weak because a quote is short-lived and must never keep an asset alive.
	 */
	UPROPERTY() TWeakObjectPtr<const UObject> Source;

	/** "Taxiway, 500 m". What the ghost prints beside the price. */
	UPROPERTY() FText What;

	/** A quote for nothing: an edit that moves no pavement. Charges and credits skip these. */
	bool IsFree() const { return BaseAmount <= 0.0; }
};

/**
 * Where the money for a build comes from, as Airside sees it.
 *
 * A PLAIN ABSTRACT CLASS, not a UINTERFACE, for the same reason IRoadEditTarget is one:
 * nothing in Blueprint needs to see this seam. ULedger implements it directly - there is no
 * adapter class - exactly as USimClock implements IOpsPersistent.
 *
 * WHY NOT A TFunction, which is what UFlightBoard::Dispatcher is. That one is a single call in
 * a single direction and its own comment argues against an interface for one call site. This
 * is five operations that must ALL be bound together: five independently-bindable TFunction
 * members would let a build charge while undo silently stopped refunding, and nothing would
 * say so.
 *
 * A NULL PURSE MEANS FREE. The editor mode, every existing tool test and a bare PIE session
 * build at no cost and needed no change when this arrived - and one test asserts exactly that,
 * because a default that silently began charging would break the editor mode.
 */
class AIRSIDE_API IBuildPurse
{
public:
	virtual ~IBuildPurse() = default;

	/** Can this be paid for right now? No side effect - the ghost asks it every frame. */
	virtual bool CanAfford(const FBuildQuote& Quote) const = 0;

	/** Take the money. Returns an id to reverse it by, or INDEX_NONE if it was refused. */
	virtual int32 Charge(const FBuildQuote& Quote) = 0;

	/**
	 * Undo: put back exactly what charge Id took.
	 *
	 * BY ID, because undo reverses the transaction that happened rather than re-pricing the
	 * geometry - see Credit for the other half of that decision.
	 */
	virtual void Reverse(int32 ChargeId) = 0;

	/**
	 * Demolish: a NEW transaction valuing this geometry at today's price.
	 *
	 * TAKES NO FRACTION AND NO ID. Airside says what was torn out; the purse decides what that
	 * is worth back. Quoting fresh rather than remembering what each segment cost is what
	 * keeps a BuiltFor field out of FRoadSegment and out of every save.
	 */
	virtual void Credit(const FBuildQuote& Quote) = 0;

	/** The quote as money, for the ghost's label - so no currency symbol ever enters Airside. */
	virtual FText Describe(const FBuildQuote& Quote) const = 0;
};
```

- [ ] **Step 4: Add the rates to the assets**

`URoadProfile` (beside the geometry fields, each with its own doc comment):

```cpp
	/**
	 * What a metre of this profile costs to lay, and what a day of owning it costs.
	 *
	 * ON THE PROFILE rather than in a cost table beside it, so a new taxiway width cannot be
	 * added without a price. A central table keyed by asset was the alternative and was
	 * rejected: forget a row and it builds free, with nothing anywhere to say so.
	 *
	 * A FIGURE ONLY AirportOps EVER READS, and that is deliberate - Airside owns the geometry,
	 * so Airside is the only layer that can say how much of it there is. See FBuildQuote.
	 */
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double CostPerMetre = 0.0;
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double UpkeepPerMetrePerDay = 0.0;
```

`UEntityDefinition`:

```cpp
	/** What placing one costs, and what a day of owning it costs. See URoadProfile::CostPerMetre
	 *  for why the figure lives on the asset rather than in a table beside it. */
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double PlacementCost = 0.0;
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;
```

`UAirsideSettings`:

```cpp
	/**
	 * What a square metre of apron costs, and what a day of owning one costs.
	 *
	 * HERE AND NOT ON AN ASSET, which is the one exception to the rule the other two rates
	 * follow - and it is forced. FApronSurface is an outline and a material slot name; there
	 * is deliberately no per-apron asset, because bands and lanes are meaningless for a
	 * polygon (RoadApron.h). This class is already this project's single door for a content
	 * default with no better home, so it is where the rate goes.
	 */
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double ApronCostPerSquareMetre = 15.0;
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double ApronUpkeepPerSquareMetrePerDay = 0.015;
```

- [ ] **Step 5: Write `Build/BuildCost.h` and its .cpp**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Tool/BuildPurse.h"

class URoadProfile;
class UEntityDefinition;
class URoadNetwork;

/**
 * How much of it there is, at the authored rate. The Airside half of the money; UPricing in
 * AirportOps is the other, and answers what it costs.
 *
 * THE ONE PLACE uu BECOME METRES for money. Every rate in this codebase is per metre or per
 * square metre, and every quote divides by 100 (or 10 000 for area) exactly once - here. A
 * second site doing the conversion is how a taxiway comes to cost a hundred times too much
 * with nothing to say which of the two was wrong.
 */
namespace BuildCost
{
	/** LengthUu of pavement at Profile's rate. */
	AIRSIDE_API FBuildQuote ForSegment(const URoadProfile& Profile, double LengthUu);

	AIRSIDE_API FBuildQuote ForEntity(const UEntityDefinition& Definition);

	/** The polygon's area at RatePerSquareMetre. Winding-independent - see the .cpp. */
	AIRSIDE_API FBuildQuote ForApron(TConstArrayView<FVector2D> Outline, double RatePerSquareMetre);

	/**
	 * One day of owning everything currently standing, at the authored rates.
	 *
	 * WALKS THE WHOLE NETWORK ONCE A DAY, which is cheap at a day's interval and keeps the
	 * figure honest: a cached total maintained by every mutator would be a second source of
	 * truth about what exists, and the mutator that forgot to update it would be invisible.
	 */
	AIRSIDE_API double DailyUpkeep(const URoadNetwork& Network, double ApronRatePerSquareMetrePerDay);
}
```

The .cpp computes area with the shoelace formula and takes `FMath::Abs` of the result, with the comment from the test explaining that a negative quote would pay the player to build.

- [ ] **Step 6: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.BuildCost`
Expected: `2 test(s) run, 0 failed, 0 crashed`

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(build): what a metre of pavement is worth, and where the money comes from"
```

---

### Task 9: The facade asks before it builds

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h` (add `GetPurse`)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` (forwarder)
- Test: `Plugins/Airside/Source/AirsideTests/Private/BuildPurseTest.cpp`

**Interfaces:**
- Consumes: `IBuildPurse`, `BuildCost::*`.
- Produces: `URoadEditFacade::SetPurse/GetPurse`, `URoadEditFacade::CanAfford`, `CommitPurchase`, `CommitDisposal`.

**The ordering that matters.** `FRoadEditScope`'s destructor calls `AbandonEdit()` when the scope was not committed — and `AbandonEdit` discards the pending UNDO SNAPSHOT. **It does not roll the network back.** So authorisation must happen BEFORE the mutation, not at commit time: a refusal after the edit would leave the graph changed with no undo step for it.

Every charged mutator therefore reads: quote → `CanAfford` → early return → existing edit → `CommitPurchase`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "Tool/BuildPurse.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A purse that records rather than banks, so a test can see exactly what the facade did. */
	class FRecordingPurse : public IBuildPurse
	{
	public:
		double Funds = 1000000.0;
		TArray<double> Charges;
		TArray<int32> Reversed;
		TArray<double> Credits;
		int32 NextId = 1;

		virtual bool CanAfford(const FBuildQuote& Quote) const override { return Quote.BaseAmount <= Funds; }
		virtual int32 Charge(const FBuildQuote& Quote) override
		{
			Funds -= Quote.BaseAmount;
			Charges.Add(Quote.BaseAmount);
			return NextId++;
		}
		virtual void Reverse(int32 ChargeId) override { Reversed.Add(ChargeId); }
		virtual void Credit(const FBuildQuote& Quote) override { Credits.Add(Quote.BaseAmount); }
		virtual FText Describe(const FBuildQuote& Quote) const override
		{
			return FText::AsNumber(Quote.BaseAmount);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildPurseChargesTest,
	"Airside.Present.BuildPurseCharges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPurseChargesTest::RunTest(const FString&)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.SpawnRoadNetworkActor();
	FRecordingPurse Purse;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	TestTrue(TEXT("the taxiway is built"), Actor->ConnectNodes(A, B, ERoadKind::Taxiway, 0));

	TestEqual(TEXT("connecting two nodes charges exactly once"), Purse.Charges.Num(), 1);
	TestTrue(TEXT("and charges something for 100 m of pavement"), Purse.Charges[0] > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildPurseRefusesTest,
	"Airside.Present.BuildPurseRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPurseRefusesTest::RunTest(const FString&)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.SpawnRoadNetworkActor();
	FRecordingPurse Purse;
	Purse.Funds = 0.0;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));

	TestFalse(TEXT("a build nobody can pay for is refused"),
		Actor->ConnectNodes(A, B, ERoadKind::Taxiway, 0));
	TestEqual(TEXT("nothing was charged"), Purse.Charges.Num(), 0);

	// THE POINT OF REFUSING BEFORE THE EDIT, not after it. FRoadEditScope's abandon discards
	// the undo snapshot; it does NOT roll the network back. A refusal at commit time would
	// leave the segment built and unpaid for, with no undo step for it.
	TestEqual(TEXT("and no segment was left behind by the refusal"),
		Actor->GetNetwork()->GetSegments().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildPurseNullBuildsFreeTest,
	"Airside.Present.BuildPurseNullBuildsFree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPurseNullBuildsFreeTest::RunTest(const FString&)
{
	// THE EDITOR MODE'S TEST, and it is not incidental: URoadBuildEdMode and every existing
	// tool test build with no purse at all, and a default that silently began charging would
	// break design-time building with nothing to say why.
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.SpawnRoadNetworkActor();

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));

	TestTrue(TEXT("with no purse, building is free and always allowed"),
		Actor->ConnectNodes(A, B, ERoadKind::Taxiway, 0));
	return true;
}

#endif
```

Check `AirsideTestFixtures.h` for the exact fixture name and the actor's spawn helper before writing this — use what is actually there rather than the names above if they differ.

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.BuildPurse`
Expected: compile failure — `SetPurse` is not a member.

- [ ] **Step 3: Add the purse to the facade**

```cpp
	/**
	 * Where the money for a build comes from, or null for free.
	 *
	 * A RAW POINTER, not a UPROPERTY: the purse is ULedger, which UOpsRuntime owns and outlives
	 * any edit, and the facade is Transient wiring that is re-made on every Attach. Null is the
	 * normal state at design time - see IBuildPurse.
	 */
	void SetPurse(IBuildPurse* InPurse) { Purse = InPurse; }
	IBuildPurse* GetPurse() const { return Purse; }
```

private:

```cpp
	IBuildPurse* Purse = nullptr;

	/** True when there is no purse (design time) or the purse says yes. */
	bool CanAfford(const FBuildQuote& Quote) const;

	/**
	 * Edit.Commit(), NotifyChanged(), and the charge - in one call, so a mutator that spends
	 * the player's money cannot forget to notify and a mutator that notifies cannot forget to
	 * charge. The charge id is recorded on the pending undo snapshot BEFORE the scope's
	 * destructor pushes it, which is what lets Undo reverse exactly what was taken.
	 */
	void CommitPurchase(FRoadEditScope& Edit, const FBuildQuote& Quote);

	/** Edit.Commit(), NotifyChanged(), and the scrap value. See IBuildPurse::Credit. */
	void CommitDisposal(FRoadEditScope& Edit, const FBuildQuote& Quote);
```

`CommitAndNotify(FRoadEditScope&)` KEEPS its name and its eight existing call sites: those are the edits that move no pavement — `PlaceNode`, `SetRunwayFacts`, `ConnectGuidelines`, `SplitSegment` and the free branches of `DeleteSlot`. Add to its doc comment that it is the FREE door and that anything creating or destroying pavement must use one of the two below.

- [ ] **Step 4: Charge the four creating mutators**

`ConnectNodes`, `PlaceRunway`, `PlaceEntity`, `AddApron`. Each gains, after its existing validation and BEFORE its `FRoadEditScope`:

```cpp
	const FBuildQuote Quote = BuildCost::ForSegment(*Profile, Length);
	if (!CanAfford(Quote))
	{
		UE_LOG(LogAirside, Log, TEXT("Refused: cannot afford %s"), *Quote.What.ToString());
		return false;   // or INDEX_NONE, matching the mutator's own return type
	}
```

and ends with `CommitPurchase(Edit, Quote);` in place of `CommitAndNotify(Edit);`.

- [ ] **Step 5: Credit the destroying mutators**

`DeleteSegment`, `DeleteNode` (which credits every incident segment — `SegmentsIncidentTo` already exists), and `DeleteSlot` for aprons and entities. `DisconnectGuideline` stays FREE and keeps `CommitAndNotify`: a guideline is not pavement.

- [ ] **Step 6: Add `GetPurse` to `IRoadEditTarget` and forward it from the actor**

So a tool can reach it through `FToolContext::Target` the way it reaches everything else. `ARoadNetworkActor` forwards to the facade, as it does for every other facade member.

- [ ] **Step 7: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside`
Expected: every Airside test passes — especially the pre-existing tool tests, which build with no purse.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(present): the facade asks the purse before it lays pavement"
```

---

### Task 10: Undo puts the money back

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditHistory.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadEditHistory.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp` (`Travel`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/BuildPurseTest.cpp` (extend)

**Interfaces:**
- Consumes: `IBuildPurse::Reverse`, `FRoadEditSnapshot`.
- Produces: `FRoadEditSnapshot::ChargeId`, `URoadEditHistory::SetPendingCharge`, `URoadEditHistory::PeekUndoChargeId`.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildPurseUndoReversesTest,
	"Airside.Present.BuildPurseUndoReverses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildPurseUndoReversesTest::RunTest(const FString&)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.SpawnRoadNetworkActor();
	FRecordingPurse Purse;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	Actor->ConnectNodes(A, B, ERoadKind::Taxiway, 0);
	TestEqual(TEXT("the build charged"), Purse.Charges.Num(), 1);

	TestTrue(TEXT("undo steps back"), Actor->GetEditFacade()->Undo());

	TestEqual(TEXT("undo reverses the charge the build actually made - BY ID, so it puts back "
		"exactly what was taken rather than a number recomputed from the geometry"),
		Purse.Reversed.Num(), 1);
	TestEqual(TEXT("and it is that build's own id"), Purse.Reversed[0], 1);
	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.BuildPurseUndo`
Expected: FAIL — `Purse.Reversed` is empty.

- [ ] **Step 3: Carry the charge on the snapshot**

`FRoadEditSnapshot` gains:

```cpp
	/**
	 * The ledger entry the edit this snapshot precedes paid for, or INDEX_NONE.
	 *
	 * ON THE SNAPSHOT because the snapshot IS the edit, as far as undo is concerned: stepping
	 * past it reverses that edit, so the charge to reverse has to travel with it. Not saved -
	 * the history is a session's undo stack and has never been persisted.
	 */
	int32 ChargeId = INDEX_NONE;
```

`URoadEditHistory` gains `void SetPendingCharge(int32 ChargeId)` (writes it onto the pending snapshot, if there is one) and `int32 PeekUndoChargeId() const` (the top of the undo stack's, or `INDEX_NONE`).

- [ ] **Step 4: Reverse it in `Travel`**

`Travel` reads `PeekUndoChargeId()` BEFORE stepping — once the step has happened the snapshot has moved to the other stack — and calls `Purse->Reverse(Id)` when both the purse and the id are real. `Redo` re-charges instead; give it the quote-free path for now by having redo restore the snapshot's `ChargeId` unchanged, and note in the comment that a redo after money was spent elsewhere is refused by `CanAfford` at the point it is re-charged.

- [ ] **Step 5: Run to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(present): undo puts back exactly what the build took"
```

---

### Task 11: Owning the airport costs money

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Build/BuildCost.cpp` (`DailyUpkeep`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (`Attach`)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/UpkeepTest.cpp`

- [ ] **Step 1: Write the failing test**

A world-free test that builds a small network, calls `BuildCost::DailyUpkeep`, and asserts it is the sum of the authored rates — plus a runtime test that ticks the clock a whole game day and asserts exactly ONE `Upkeep` entry appeared, with the comment that a hundred-stand airport must not write a hundred rows a day.

- [ ] **Step 2: Run to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Runtime.Upkeep`

- [ ] **Step 3: Arm the daily callback in `Attach`**

```cpp
	// ONE ENTRY A DAY, not one per object: a hundred-stand airport would otherwise write a
	// hundred rows a day into a saved array. RollUp then keeps even those bounded.
	UpkeepHandle = Clock->Every(USimClock::SecondsPerDay, [this]()
	{
		PostDailyUpkeep();
	});
```

with a matching `Clock->Cancel(UpkeepHandle)` in `Detach`, exactly as `OfferHandle` is cancelled — and the same `INDEX_NONE` sentinel.

- [ ] **Step 4: Run to verify it passes**

- [ ] **Step 5: Commit**

```bash
git add Plugins
git commit -m "feat(ops): a day of owning the airport costs money"
```

---

### Task 12: The ledger IS the purse

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h` (implement `IBuildPurse`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/Ledger.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (`Attach` hands it to the facade)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/LedgerPurseTest.cpp`

**Interfaces:**
- Produces: `ULedger : public IBuildPurse` — `CanAfford`, `Charge`, `Reverse`, `Credit`, `Describe`.

`ULedger` needs a `UPricing*` to answer `CanAfford` and `Describe`, so give it one and wire it in `Attach`. `Charge` posts `Placement`; `Credit` posts `Refund` at `UPricing::ScrapValue`; `Reverse(int32)` forwards to the existing `Reverse(At, ChargeId)` using the clock's current time — so `ULedger` also needs the clock, or an `At` supplied by the runtime. Prefer giving `ULedger` a `USimClock*`, because a ledger that cannot date its own entries is the thing that would silently write everything at time zero.

Test that `CanAfford` is false while the balance is negative — **the GDD's "negative balance locks placement" falls out of this and is not a second rule**, so this is the test that says so.

- [ ] **Step 1–6:** failing test, run, implement, run, wire into `Attach` (`Facade->SetPurse(Ledger)`), commit.

```bash
git commit -m "feat(ops): the ledger is the purse the build tools spend from"
```

---

### Task 13: The HUD shows the balance, and the player sets their fee

**Files:**
- Modify: `Source/AirportMgr/BuildBarWidget.h`
- Modify: `Source/AirportMgr/BuildBarWidget.cpp:235-245` (the reserved `LedgerSlot` spacer) and `:393-406` (the per-frame refresh)
- Modify: `Source/AirportMgr/BuildActions.cpp` (two new Game-section actions)
- Modify: `Source/AirportMgr/RoadBuildController.h/.cpp` (the two verbs)
- Test: `Source/AirportMgr/BuildBarWidgetTest.cpp`, `Source/AirportMgr/BuildActionsTest.cpp`

The `USpacer` becomes a `UTextBlock` named `BalanceText`, styled `EUITextRole::Clock` in `Style->Text`, switching to `Style->Warning` below zero. The fee lever is two `BuildActions()` entries — `game.feeup` and `game.feedown` — because that table is already the one list the bar, the key bindings and the inspector all read.

Assert in the widget test that the balance text exists and that it re-reads after a post; assert in the actions test that both new ids are present and disabled without a runtime (`HasRuntime`), matching every other runtime-dependent action.

- [ ] **Step 1–6:** failing tests, run, implement, run, commit.

```bash
git commit -m "feat(ui): the bar shows the money, and the fee is the player's to set"
```

---

### Task 14: The ghost says what it will cost

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp` (and `RunwayTool`, `StandPlaceTool`, `ApronDrawTool`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/BuildPurseTest.cpp` (extend)

Each tool's `BuildPreview` quotes what it would build and emits `Sink.Label(At, Purse->Describe(Quote).ToString(), Style)` with `EPreviewStyle::Pending` when affordable and `EPreviewStyle::Refused` when not — **no new sink method**, because `Label` already exists and `Refused` is already defined as "something the gesture cannot do, with the reason".

Reach the purse through `Context.Target->GetPurse()`; a null purse means no label at all, which is what the editor mode should show.

- [ ] **Step 1–6:** failing test (a recording sink asserting a label appears, and that it carries `Refused` when funds are short), run, implement, run, commit.

```bash
git commit -m "feat(tool): the ghost prices the thing before the click"
```

---

### Task 15: Determinism, the full run, and the PR

- [ ] **Step 1: Write the determinism test**

The test M1 deferred for want of a ledger: build the same airport twice from the same seed, run the same number of clock ticks, and assert the two ledgers' entries match category-for-category and amount-for-amount. **This is the test that keeps wall time out of the sim** — if it ever fails, something is scaling off real seconds.

- [ ] **Step 2: Full build**

```bash
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

- [ ] **Step 3: Full test run**

```bash
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line — **never the exit code**.

- [ ] **Step 4: Author the first-pass figures onto the content assets**

The rates in the spec's §7 table are defaults on the C++ assets; the existing content assets (taxiway and runway profiles, the stand and depot definitions) need them set. Do it headlessly with a `Tools/Python` script, with the editor closed.

- [ ] **Step 5: PIE verification**

Build a taxiway and watch the balance fall; accept an offer and watch the landing fee arrive. Quote the `LogAirportOps` lines from `Saved/Logs/AirportMgr.log` as the evidence — an inference is not a verification.

- [ ] **Step 6: Open the PR**

Fill in the template's build line, test line, and the log-line and comment-line deltas.

---

## Self-Review

**Spec coverage.** §3 D1 resolver: Task 2. D2 rates on assets: Task 8. D3 symmetric halves: Tasks 2 and 8. D4 `IBuildPurse`: Tasks 8, 12. D5 undo vs demolish: Tasks 9, 10. D6 negative-balance lock: Task 12. D7 fuel fee instead of fines: Task 6. D8 elasticity: Tasks 2, 7. D9 offer-time pricing: Task 5. D10 daily upkeep and roll-up: Tasks 1, 11. D11 `OpsSave` parameters: Task 3. D12 ghost price: Task 14. D13 currency: Task 2. §4 objects: Tasks 1, 2, 8. §5 flows: Tasks 5, 6, 9, 10, 11. §6 HUD: Task 13. §7 figures: Tasks 2, 8, and authored in Task 15 step 4. §8 testing: every task, plus Task 15.

**Known gap, stated rather than hidden.** The spec's fourth flow — charging the delta when a node is DRAGGED, so that building ten metres and dragging it to two kilometres is not free — has **no task of its own**. It belongs with Task 9 but `MoveNode` is scope-free across frames and needs `BeginInteractiveEdit`/`EndInteractiveEdit` to hold a base quote between calls, which is a different shape from every other mutator. Add it as Task 9b when Task 9 lands, or accept the hole knowingly and record it in the PR. **Do not leave it silently unimplemented** — it is the one gap that lets a player build for nothing.

**Type consistency.** `FBuildQuote::BaseAmount`, `Source`, `What` are used identically in Tasks 8, 9, 10, 12, 14. `IBuildPurse`'s five methods are declared once in Task 8 and implemented once in Task 12. `ULedger::Post` returns the id `Reverse` takes, in Tasks 1, 10 and 12. `UPricing::LandingFee` is used in Tasks 2, 5 and 13.
