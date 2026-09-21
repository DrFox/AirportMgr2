#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"

#include "OfferViewModels.generated.h"

class UFlight;
class UFlightBoard;
class UGroundTraffic;
class URoadNetwork;
class USimClock;

/**
 * One row of the offer inbox.
 *
 * A VIEWMODEL, NOT THE MODEL. UFlight lives in AirportOps Model/ and must not learn about
 * the MVVM runtime, for the same reason Airside Model/ includes nothing above it: a model
 * that knows about its view cannot be tested without one. This flattens a flight into the
 * handful of display fields a row draws, and every one of them is set through
 * UE_MVVM_SET_PROPERTY_VALUE so that a binding actually hears about the change.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API UOfferViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	/** The flight this row shows. WEAK: the board owns flights and retires them. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlight> Flight;

	/** Pull every field from the flight and the board's refusal. */
	void Refresh(const UFlightBoard& Board, const UGroundTraffic& Traffic,
		const URoadNetwork& Network, const USimClock& Clock);

	FText GetAirline() const { return Airline; }
	FText GetTypeName() const { return TypeName; }
	FText GetEta() const { return Eta; }
	bool IsAcceptable() const { return bAcceptable; }
	FText GetRefusal() const { return Refusal; }

private:
	/**
	 * The three revisions bAcceptable/Refusal were last computed at, and whether they have
	 * been computed at all yet.
	 *
	 * WHY THIS ROW EXISTS (issue #169): WhyNotAcceptable is a full ArrivalPlanner::Plan - a
	 * route search over every stand, then every runway exit - and Refresh used to run it
	 * every tick for every row, whether or not anything it could depend on had moved. The
	 * three things it depends on each publish a cheap revision already: the board itself
	 * (UFlightBoard::Revision - added/accepted/declined/expired), the guideline graph
	 * (URoadNetwork::GetGuidelineRevision - an edit changed the taxiways), and occupancy
	 * (UGroundTraffic::OccupancyRevision - a stand claimed or freed, a runway taken or
	 * cleared). Three integer compares replace the search on every call where none of the
	 * three moved, which is most of them - the whole reason a route search per row per frame
	 * went unnoticed until #169's profiling caught it.
	 *
	 * NOT reflected and not a UPROPERTY: bookkeeping about the last recompute, not state a
	 * save would ever need - the same reasoning UGroundTraffic::LastStepsForTest gives.
	 */
	uint32 BoardRevisionAt = 0;
	uint32 GuidelineRevisionAt = 0;
	uint32 OccupancyRevisionAt = 0;
	bool bWhyComputed = false;

	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetAirline",
		Category = "Offer", meta = (AllowPrivateAccess))
	FText Airline;

	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetTypeName",
		Category = "Offer", meta = (AllowPrivateAccess))
	FText TypeName;

	/** "in 12 min", not a game-second count: the player has no feel for the compressed clock. */
	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetEta",
		Category = "Offer", meta = (AllowPrivateAccess))
	FText Eta;

	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "IsAcceptable",
		Category = "Offer", meta = (AllowPrivateAccess))
	bool bAcceptable = true;

	/**
	 * Why not, in the words ArrivalPlanner::DescribeRefusal uses.
	 *
	 * The SAME sentence the arrival itself would print, because it comes from the same plan -
	 * a second wording here would be a second account of why an aeroplane cannot land.
	 */
	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetRefusal",
		Category = "Offer", meta = (AllowPrivateAccess))
	FText Refusal;
};

/**
 * The inbox: the offers awaiting an answer, and the two commands.
 *
 * NOTHING HERE MUTATES THE MODEL. Accept and Decline call UFlightBoard, which is the one
 * door for undo, save and tests - the rule URoadEditFacade already enforces on the build
 * side. A viewmodel that reached into a UFlight would be a second way to change the game.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API UOfferInboxViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	/**
	 * Rebuild the rows from the board.
	 *
	 * CALLED ON TICK (issue #169 revised this from its old claim of "and on
	 * UFlightBoard::OnChanged" - that delegate had fired from nearly every board method for
	 * years with zero subscribers; see UFlightBoard::Revision, which replaces it). The ROW
	 * SET - which flights have an offer at all - is rebuilt only when UFlightBoard::Revision
	 * has moved since the last call: Board.Offers() allocates a fresh array on every call,
	 * and diffing it against Rows was the same cost again for a tick where nothing happened.
	 * Each row's OWN fields (the ETA text, and the acceptable/refusal pair gated on its own
	 * three revisions - see UOfferViewModel) still refresh every call: bookkeeping about the
	 * OFFER SET is cheap to gate here, but each row already knows how to gate what is
	 * actually expensive.
	 */
	void Refresh(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock);

	/** The list the UListView is given. UObject*s, which is what SetListItems takes. */
	UFUNCTION(BlueprintCallable, Category = "Offers")
	const TArray<UOfferViewModel*>& GetOffers() const { return Offers; }

	int32 GetPendingCount() const { return PendingCount; }

	/** True if the board took it. False leaves the offer in the inbox with its reason shown. */
	bool Accept(UOfferViewModel* Row);
	void Decline(UOfferViewModel* Row);

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UOfferViewModel>> Rows;

	/** The same rows as raw pointers, because the list view and Blueprint want that shape. */
	UPROPERTY(Transient) TArray<UOfferViewModel*> Offers;

	/** UFlightBoard::Revision as of the last time Rows was rebuilt from Board.Offers(); see
	 *  Refresh's own comment. Not a UPROPERTY: bookkeeping, not state a save would ever need. */
	uint32 BoardRevisionAt = 0;
	bool bRowsValid = false;

	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetPendingCount",
		Category = "Offers", meta = (AllowPrivateAccess))
	int32 PendingCount = 0;

	/** What Accept and Decline call. Set by Refresh; weak for the usual lifetime reason. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlightBoard> Board;
	UPROPERTY(Transient) TWeakObjectPtr<UGroundTraffic> Traffic;
	UPROPERTY(Transient) TWeakObjectPtr<URoadNetwork> Network;
	UPROPERTY(Transient) TWeakObjectPtr<USimClock> Clock;
};
