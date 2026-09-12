#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "FlightBoard.generated.h"

class UFlight;
class UGroundTraffic;
class UOfferGenerator;
class URoadNetwork;
class UStandAllocator;
class USimClock;
enum class EAgentPhase : uint8;

/**
 * Every live flight, and the ONLY thing that dispatches an arrival.
 *
 * ONE DOOR. The inbox and key 7 both come through here, because two doors onto arrival is
 * how this codebase has shipped three lists-that-must-agree bugs - see CLAUDE.md, "Check
 * where a list is CONSUMED". A second caller of DispatchArrival would put an aeroplane on
 * the field that no flight owns, and nothing would ever take its stand back.
 *
 * ARoadNetworkActor::DispatchArrival is reached through the Dispatcher seam rather than
 * called directly, so the schedule can be proved to fire without a UWorld. Everything in
 * this class is world-free by construction; if a change here needs a world, it belongs in
 * Present/.
 */
UCLASS()
class AIRPORTOPS_API UFlightBoard : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * What actually puts an aeroplane in the world. UOpsRuntime::Attach points this at
	 * ARoadNetworkActor::DispatchArrival; tests substitute a recorder.
	 *
	 * A TFunction and not an interface, because there is exactly one production
	 * implementation and it is a forwarder on an actor - an interface would be a class per
	 * call site, for one call site.
	 *
	 * Not a UPROPERTY and deliberately not saved: it is wiring, re-made on every Attach.
	 */
	TFunction<bool(const FVector2D& Near, const FAirframe& Airframe)> Dispatcher;

	/** Raised whenever anything a viewmodel displays has changed. */
	FSimpleMulticastDelegate OnChanged;

	UPROPERTY() TObjectPtr<UStandAllocator> Allocator = nullptr;
	UPROPERTY() TObjectPtr<UOfferGenerator> Generator = nullptr;

	/**
	 * Where an arrival is aimed. ArrivalPlanner chooses the runway by nearest threshold to
	 * this point, so it is the same "focus" the land key already computes from the view.
	 */
	UPROPERTY() FVector2D ApproachFocus = FVector2D::ZeroVector;

	/** Takes ownership of an offer and gives it the next id if it has none. */
	void AddOffer(UFlight* Offer);

	/** The id the next offer should carry. The board owns numbering; see UOfferGenerator. */
	int32 TakeNextId();

	/**
	 * Hold a stand and put the arrival on the clock.
	 *
	 * False when no stand admits it, which is the refusal the inbox shows - and the whole of
	 * "the player cannot over-commit": an accepted flight always has somewhere to go.
	 */
	bool Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	void Decline(UFlight& Flight);

	/**
	 * Why this offer could not be accepted this instant, or EArrivalRefusal::None.
	 *
	 * The REAL ArrivalPlanner::Plan against the live occupancy, so the inbox's greyed-out
	 * reason is the same sentence the arrival itself would print - ArrivalPlanner::
	 * DescribeRefusal renders it. A second, cheaper guess here would be a second source of
	 * truth about whether an aeroplane can land.
	 */
	EArrivalRefusal WhyNotAcceptable(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/** Expiry, and nothing else on the fast path: the ETA is the clock's job, not a poll. */
	void Tick(USimClock& Clock);

	void OnAgentPhase(const UGroundTraffic& Traffic, const URoadNetwork& Network, int32 AgentId,
		EAgentPhase From, EAgentPhase To);

	/** Re-make every accepted flight's stand hold. See UStandAllocator::Reapply. */
	void OnGraphRebuilt(UGroundTraffic& Traffic, const URoadNetwork& Network);

	/**
	 * Re-arm the clock for every Accepted flight.
	 *
	 * CALLED AFTER A LOAD, and it is not optional: USimClock deliberately does not save its
	 * callback queue, so a restored flight has an ETA and nothing armed. Without this it
	 * never arrives, and nothing anywhere says so.
	 */
	void RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock);

	/** Offers still awaiting an answer, in the order they arrived. */
	TArray<UFlight*> Offers() const;

	/** Everything accepted and not yet departed. */
	TArray<UFlight*> Live() const;

	int32 PendingOfferCount() const { return Offers().Num(); }

private:
	UPROPERTY() TArray<TObjectPtr<UFlight>> Flights;
	UPROPERTY() int32 NextFlightId = 1;

	/**
	 * Clock handles by flight id, so an accepted flight can be un-scheduled.
	 *
	 * NOT a UPROPERTY and NOT saved, on purpose: USimClock does not save its queue either,
	 * and a handle restored against a queue that no longer holds it would cancel somebody
	 * else's callback. UFlight::ArrivesAt is the saved truth; RearmSchedules rebuilds this.
	 */
	TMap<int32, int32> ArrivalHandles;

	void DispatchNow(UGroundTraffic& Traffic, UFlight& Flight);
	void Schedule(UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight);
	UFlight* FindByAgent(int32 AgentId);
	UFlight* FindById(int32 Id);
};
