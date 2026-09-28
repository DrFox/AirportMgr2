#pragma once

#include "CoreMinimal.h"

#include "ArrivalViewModels.generated.h"

class UFlight;
class UFlightBoard;
class USimClock;

/**
 * One row of the ARRIVALS section (spec 2026-09-28-arrival-queue section 3): a flight from its
 * accept until it is airborne. Display only - no buttons, so nothing here reaches the board.
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

	void Refresh(const USimClock& Clock);

	FText GetTitle() const { return Title; }
	FText GetStatus() const { return Status; }
	FText GetDetail() const { return Detail; }
	bool IsLate() const { return bLate; }

	/** "HOLDING", "in 6 min", "LANDING", "TAXI IN", "ON STAND", ... - by phase. */
	static FText DescribeStatus(const UFlight& Flight, double Now);

	/**
	 * "waited 3 min - 47 min left" while holding, "47 min left" otherwise, "12 min late" once
	 * AirborneBy has passed (bOutLate then true). Empty for a flight with no contract - the
	 * debug land key's, which was never offered.
	 */
	static FText DescribeDetail(const UFlight& Flight, double Now, bool& bOutLate);

	/**
	 * "Turnaround 2 h - 47 min left", or "... 12 min late" past AirborneBy - the contract line
	 * on the aircraft card. Empty for a flight with no contract (the debug land key's).
	 */
	static FText DescribeTurnaround(const UFlight& Flight, double Now);

private:
	UPROPERTY(Transient) FText Title;
	UPROPERTY(Transient) FText Status;
	UPROPERTY(Transient) FText Detail;
	UPROPERTY(Transient) bool bLate = false;
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
	/** Rebuild the row set when the board has moved; refresh every row's text every call. */
	void Refresh(const UFlightBoard& Board, const USimClock& Clock);

	TArray<UArrivalRowViewModel*> GetRows() const;
	int32 GetCount() const { return Rows.Num(); }

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UArrivalRowViewModel>> Rows;

	/** UFlightBoard::Revision as of the last row-set rebuild. Bookkeeping, not saved. */
	uint32 BoardRevisionAt = 0;
	bool bRowsValid = false;
};
