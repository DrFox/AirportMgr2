#pragma once

#include "CoreMinimal.h"

#include "ArrivalViewModels.generated.h"

class UFlight;
class UFlightBoard;
class USimClock;
enum class EFlightPhase : uint8;

/**
 * EVERY INPUT a row's three sentences read, at the resolution they print it (#446): the row is composed again only when this moves. The
 * status says "in 6 min" (whole minutes to the ETA, while accepted), the detail "47 min left" / "12 min late" / "waited 3 min" (whole
 * minutes of the contract, and of the wait while holding), the title a queue number - so a key of (flight, phase, those minutes, the queue
 * place, late, has-a-contract) moves exactly when a word does. THE MINUTES ARE GameTimeText::WholeMinutes, the rounding Duration prints, not a
 * floor of the row's own: a key that rounded differently would repaint half a minute early or hold a sentence a minute stale, the defect the
 * inspector's turnaround key was walked across a day to catch (#480).
 * ENFORCED BY: AirportMgr.UI.Arrivals.KeyMovesWithTheText (a sweep of the clock: an unchanged key never hides a changed sentence)
 */
struct FArrivalRowKey
{
	TWeakObjectPtr<const UFlight> Flight;
	/** The phase, by value: it is compared for EQUALITY only (Check-Architecture rule 58 bans grouping or ordering phases outside Flight.h). Value-initialised; bKeyValid, not this, says a key was ever set. */
	EFlightPhase Phase{};
	int32 QueuePosition = -1;
	/** The runway the status names (FlightRunway::Names) - a strip built beside it re-letters it, and that moves no minute. */
	FString Runway;
	/** Accepted only: whole minutes to ArrivesAt. 0 in every other phase, whose word does not count down. */
	int32 StatusMinutes = -1;
	/** A flight with no contract (the debug land key's) has no detail line at all. */
	bool bContract = false;
	/** On blocks (UFlight::HasContractStarted, #398): "turnaround 1 h" before, "47 min left" after - and at the moment it starts the
	 *  minutes do not move (the whole contract is left either way), so without this the key would hold the old sentence. */
	bool bContractStarted = false;
	bool bLate = false;
	/** Whole minutes of the contract left - or late by, once bLate. */
	int32 DetailMinutes = -1;
	/** Holding only: whole minutes waited. 0 otherwise. */
	int32 WaitedMinutes = -1;
	/** The flight has a live aircraft (UFlight::AgentId) - what shows the row's Inspect button, so its arrival repaints the row. */
	bool bHasAircraft = false;

	bool operator==(const FArrivalRowKey& Other) const = default;
};

/**
 * One row of the ARRIVALS section (spec 2026-09-28-arrival-queue section 3): a flight from its
 * accept until it is airborne. Nothing here reaches the board: the row's one button, Inspect (2026-10-02), selects the flight's aircraft
 * through the controller, as an alert's Go does - see UArrivalsPanelWidget::Inspect.
 *
 * A VIEWMODEL for the same reason UOfferViewModel is one: UFlight is AirportOps Model/ and must
 * not learn about the UI. Plain UObject, polled by OfferInboxWidget::PaintRows - see
 * UOfferViewModel's header for why no MVVM base.
 */
UCLASS()
class AIRPORTMGR_API UArrivalRowViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** The flight this row shows. WEAK: the board owns flights and retires them. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlight> Flight;

	/** Its place in the holding queue, 1-based, or 0 when it is not holding. Set by the list. */
	int32 QueuePosition = 0;

	/**
	 * Composes the row's sentences - but only when KeyFor has moved since it last did (#446): it ran every tick for every row, folded or not,
	 * about six FText::Format a row. True when it composed again.
	 */
	bool Refresh(const USimClock& Clock, const FString& Runway = FString());

	/** What Refresh keys on, for Flight at Now in QueuePosition naming Runway - public so a test can sweep the clock and ask it. */
	static FArrivalRowKey KeyFor(const UFlight& Flight, int32 QueuePosition, double Now, const FString& Runway = FString());

	/**
	 * The stamp of the row's last compose - UNIQUE ACROSS ROWS, not a per-row count, so a panel that remembers the stamp it painted in a
	 * SLOT repaints when a different row lands in that slot (a flight that left the queue and moved every row up) as well as when its own row
	 * recomposes. 0 before the first compose.
	 */
	int32 GetRevision() const { return Revision; }

	FText GetTitle() const { return Title; }
	FText GetStatus() const { return Status; }
	FText GetDetail() const { return Detail; }
	bool IsLate() const { return bLate; }
	/** The flight has an aircraft in the world to inspect - set by Refresh from FArrivalRowKey::bHasAircraft. */
	bool HasAircraft() const { return bHasAircraft; }

	/**
	 * The flight is holding for a runway (the phase DescribeStatus words "HOLDING"): the one state the player can do something about, so
	 * the panel tints it. A FACT OF THE ROW, set by Refresh - the panel used to recover it by comparing the status's localised TEXT
	 * against its own NSLOCTEXT, which a reworded status or a translation would have broken silently (#447; ULedgerRowViewModel's
	 * bOutgoing rejects exactly this idea).
	 * ENFORCED BY: Check-Architecture rule 68 (no `.EqualTo(` then `NSLOCTEXT(` over a statement), AirportMgr.UI.Arrivals.HoldingIsAFactNotAWord (the fact),
	 * AirportMgr.UI.Arrivals.HoldingRowIsInAccent (the panel reads it)
	 */
	bool IsHolding() const { return bHolding; }

	/** Whether Flight is in the phase DescribeStatus words "HOLDING" - what Refresh stores as IsHolding, public so a test can ask it of a flight. */
	static bool IsHoldingPhase(const UFlight& Flight);

	/** "HOLDING", "in 6 min", "LANDING", "TAXI IN", "ON STAND", ... - by phase; with Runway, "HOLDING for 09L", "LANDING 09L",
	 *  "TAXI IN from 09L", "TAXI OUT to 27R", "DEPARTING 27R" - the phases FlightRunway::For names one in. */
	static FText DescribeStatus(const UFlight& Flight, double Now, const FString& Runway = FString());

	/**
	 * "47 min left" on the stand, "12 min late" once OffBlocksBy has passed (bOutLate then true). BEFORE ON-BLOCKS (#398) no
	 * countdown - the contract has not started: "turnaround 1 h", or "waited 3 min - turnaround 1 h" while holding. Empty for a
	 * flight with no contract - the debug land key's, which was never offered.
	 */
	static FText DescribeDetail(const UFlight& Flight, double Now, bool& bOutLate);

	/**
	 * "Turnaround 40 min - 27 min left", or "... 12 min late" past OffBlocksBy - the contract line
	 * on the aircraft card; "Turnaround 40 min - starts on stand" before on-blocks (#398). Empty for a
	 * flight with no contract (the debug land key's).
	 */
	static FText DescribeTurnaround(const UFlight& Flight, double Now);

private:
	UPROPERTY(Transient) FText Title;
	UPROPERTY(Transient) FText Status;
	UPROPERTY(Transient) FText Detail;
	UPROPERTY(Transient) bool bLate = false;
	UPROPERTY(Transient) bool bHolding = false;
	UPROPERTY(Transient) bool bHasAircraft = false;

	/** What the sentences above were composed from - see FArrivalRowKey. Not saved. */
	FArrivalRowKey Key;
	bool bKeyValid = false;
	int32 Revision = 0;
};

/**
 * The ARRIVALS list: every accepted flight until airborne, holding first (queue order, numbered),
 * then inbound by ETA, then on the ground.
 *
 * THE ORDER COMES FROM THE BOARD: UFlightBoard::Queue() for the holding flights, so the numbers
 * here are the order UArrivalSequencer clears them in - never a second opinion about it.
 */
UCLASS()
class AIRPORTMGR_API UArrivalsViewModel : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Rebuild the row set when the board has moved, then refresh every row's text (which each row does only when what it says would change).
	 * SyncRows and RefreshText are the two halves, apart so a FOLDED window can keep the first - the count on its title bar - and skip the
	 * second (#446).
	 */
	void Refresh(const UFlightBoard& Board, const USimClock& Clock);

	/** The row SET: rebuilt only when the board's revision moved. A flight that stays keeps its row object (and its memo); one that goes loses it. */
	void SyncRows(const UFlightBoard& Board);

	/** Every row's sentences as of Clock - a row composes only when its key moved. Names no runway: see the overload. */
	void RefreshText(const USimClock& Clock);

	/** RefreshText, each row's status naming RunwayOf its flight (the panel passes FlightRunway's names) - part of the key, so
	 *  a row recomposes when its runway changes and not otherwise. */
	void RefreshText(const USimClock& Clock, TFunctionRef<FString(const UFlight&)> RunwayOf);

	/** How many row composes RefreshText has caused, in total - a delta across ticks is the number a pin reads. */
	int32 ComposeCountForTest() const { return Composes; }

	TArray<UArrivalRowViewModel*> GetRows() const;
	int32 GetCount() const { return Rows.Num(); }

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UArrivalRowViewModel>> Rows;

	/** UFlightBoard::Revision as of the last row-set rebuild. Bookkeeping, not saved. */
	uint32 BoardRevisionAt = 0;
	bool bRowsValid = false;

	/** See ComposeCountForTest. */
	int32 Composes = 0;
};
