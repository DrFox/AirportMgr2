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
	 * Called on UFlightBoard::OnChanged and on tick. Rows are rebuilt rather than diffed:
	 * there are a handful of them, and a diff would be a second model of what the inbox
	 * holds - see "lists that must agree are ONE list".
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

	UPROPERTY(BlueprintReadOnly, Transient, FieldNotify, Getter = "GetPendingCount",
		Category = "Offers", meta = (AllowPrivateAccess))
	int32 PendingCount = 0;

	/** What Accept and Decline call. Set by Refresh; weak for the usual lifetime reason. */
	UPROPERTY(Transient) TWeakObjectPtr<UFlightBoard> Board;
	UPROPERTY(Transient) TWeakObjectPtr<UGroundTraffic> Traffic;
	UPROPERTY(Transient) TWeakObjectPtr<URoadNetwork> Network;
	UPROPERTY(Transient) TWeakObjectPtr<USimClock> Clock;
};
