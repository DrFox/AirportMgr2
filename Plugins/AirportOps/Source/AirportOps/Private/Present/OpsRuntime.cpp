#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Build/BuildCost.h"
#include "Build/DepotKit.h"
#include "Content/AirportOpsSettings.h"
#include "Content/AirsideSettings.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirlineRoster.h"
#include "Model/Airport.h"
#include "Model/OpsAlerts.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/StandAllocator.h"
#include "Model/OpsEvents.h"
#include "Model/Pricing.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

UOpsRuntime::UOpsRuntime()
{
	Clock = CreateDefaultSubobject<USimClock>(TEXT("Clock"));
	Events = CreateDefaultSubobject<UOpsEvents>(TEXT("Events"));
	Catalog = CreateDefaultSubobject<UOpsCatalog>(TEXT("Catalog"));
	JobBoard = CreateDefaultSubobject<UJobBoard>(TEXT("JobBoard"));

	// GROWS BY FORWARDING, as UJobBoard established: the board and the generator are
	// subobjects this class feeds and ticks, and it holds no logic of theirs.
	FlightBoard = CreateDefaultSubobject<UFlightBoard>(TEXT("FlightBoard"));
	FlightBoard->Allocator = CreateDefaultSubobject<UStandAllocator>(TEXT("StandAllocator"));
	FlightBoard->Sequencer = CreateDefaultSubobject<UArrivalSequencer>(TEXT("ArrivalSequencer"));
	OfferGenerator = CreateDefaultSubobject<UOfferGenerator>(TEXT("OfferGenerator"));
	FlightBoard->Generator = OfferGenerator;

	// The money, and the same forwarding shape: this class gains two pointers and a line in
	// Attach, and every decision about what things cost lives in UPricing, not here.
	Ledger = CreateDefaultSubobject<ULedger>(TEXT("Ledger"));
	Pricing = CreateDefaultSubobject<UPricing>(TEXT("Pricing"));

	// The airlines' mood - the bus's first Reaction (spec 2026-09-29 §3), forwarded like the rest.
	Airlines = CreateDefaultSubobject<UAirlineRoster>(TEXT("Airlines"));

	// The standing alerts - derived from the boards, owned here like them (spec 2026-09-29-ops-alerts).
	Alerts = CreateDefaultSubobject<UOpsAlerts>(TEXT("Alerts"));

	// THE AIRPORT'S STATUS (spec 2026-09-29-ops-batch3 §3), the same forwarding shape - and read, not subscribed
	// to, by the generator, which is handed it here once for the runtime's life.
	Airport = CreateDefaultSubobject<UAirport>(TEXT("Airport"));
	OfferGenerator->Airport = Airport;
	// AND BY THE BOARD'S Accept, the one door every accept comes through - a closed airport admits nothing (ruling
	// I1). Weak, for the dispatcher's reason in Attach.
	// ENFORCED BY: AirportOps.Present.Airport.AcceptRefusedWhileClosed, AirportOps.Present.Airport.LandRefusedWhileClosed
	TWeakObjectPtr<const UAirport> WeakAirport = Airport;
	FlightBoard->AdmitsArrivals = [WeakAirport]()
	{
		const UAirport* Live = WeakAirport.Get();
		return Live == nullptr || Live->Status() == EAirportStatus::Open;
	};

	// The Unstick menu, the same shape again: a pointer, and the two boards it composes.
	AgentRescue = CreateDefaultSubobject<UAgentRescue>(TEXT("AgentRescue"));
	AgentRescue->JobBoard = JobBoard;
	AgentRescue->FlightBoard = FlightBoard;

	// PURCHASES, the same shape: a pointer, and the owners it commands - the board owns the fleet, the
	// ledger the money. The world hooks are Attach's (they need the actor).
	FacilityPurchases = CreateDefaultSubobject<UFacilityPurchases>(TEXT("FacilityPurchases"));
	FacilityPurchases->JobBoard = JobBoard;
	FacilityPurchases->Ledger = Ledger;
	FacilityPurchases->Pricing = Pricing;
	FacilityPurchases->Clock = Clock;
	FacilityPurchases->Bus = &Bus;
}

void UOpsRuntime::SeedAirlines()
{
	for (const FAirlineOffers& Each : AirlineOffers)
	{
		if (Each.Airline != nullptr)
		{
			Airlines->Ensure(Each.Airline->GetFName());
		}
	}
}

bool UOpsRuntime::SetAirportClosed(bool bClosed)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Airport %s refused: no network attached"), bClosed ? TEXT("close") : TEXT("open"));
		return false;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Airport: the player %s it (%d flight(s) not yet arrived, %d on the ground)"),
		bClosed ? TEXT("closes") : TEXT("reopens"), FlightBoard->UnarrivedCount(), FlightBoard->OnGroundCount());
	// RE-DERIVED NOW, published by the airport on a change; the cancellation is that event's Sim handler (WireBus),
	// so a close from the bar and a runway deleted under an open airport take the one path.
	Airport->SetClosedByPlayer(bClosed, *Target->Network);
	return true;
}

FUnstickVerdict UOpsRuntime::CanUnstick(int32 AgentId, EUnstickAction Action) const
{
	const UGroundTraffic* Model = Target != nullptr && Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr;
	if (Model == nullptr)
	{
		return FUnstickVerdict::No(NSLOCTEXT("AgentRescue", "NoAirport", "No airport attached"));
	}
	return AgentRescue->CanUnstick(*Model, AgentId, Action);
}

FUnstickVerdict UOpsRuntime::Unstick(int32 AgentId, EUnstickAction Action)
{
	UGroundTraffic* Model = Target != nullptr && Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr;
	if (Model == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Unstick: agent %d %s -> refused: no airport attached"),
			AgentId, *UEnum::GetValueAsString(Action));
		return FUnstickVerdict::No(NSLOCTEXT("AgentRescue", "NoAirport", "No airport attached"));
	}
	const FUnstickVerdict Verdict = AgentRescue->Unstick(*Model, *Target->Network, *Clock, AgentId, Action);
	// A PLAYER COMMAND THAT MAY HAVE CHANGED THE JOB BOARD (jobs released, a vehicle recalled): its
	// pass runs on events (stage 3), and every rescue path today also raises a phase event - but a
	// command is not an event, so it says so itself rather than leaning on that.
	Bus.MarkDirty(TEXT("JobBoard"));
	return Verdict;
}

FFacilityQuote UOpsRuntime::QuoteFacility(FEntityInstanceId Entity) const
{
	return Target != nullptr && Target->Network != nullptr ? FacilityPurchases->Quote(*Target->Network, Entity) : FFacilityQuote();
}

FPurchaseResult UOpsRuntime::BuyModule(FEntityInstanceId Entity, EDepotModule Module)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d - no airport attached"), Entity.Index);
		return FPurchaseResult{ EPurchaseRefusal::NotAFacility };
	}
	return FacilityPurchases->BuyModule(*Target->Network, Entity, Module);
}

FPurchaseResult UOpsRuntime::BuyVehicle(FEntityInstanceId Entity, FName TypeCode)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d - no airport attached"), Entity.Index);
		return FPurchaseResult{ EPurchaseRefusal::NotAFacility };
	}
	return FacilityPurchases->BuyVehicle(*Target->Network, Entity, TypeCode);
}

FPurchaseResult UOpsRuntime::SellVehicle(int32 VehicleId)
{
	return FacilityPurchases->SellVehicle(VehicleId);
}

int32 UOpsRuntime::ReservedSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)
{
	if (Target == nullptr)
	{
		return 0;
	}
	if (ReservationMemoNetwork.Get() != Target->Network)
	{
		ReservationMemo.Reset();
		ReservationMemoNetwork = Target->Network;
	}
	TArray<int32>* Ceilings = ReservationMemo.Find(Id);
	if (Ceilings == nullptr)
	{
		++ReservationSolves;
		Ceilings = &ReservationMemo.Add(Id);
		const TArray<PlotYard::FKitSpec> Specs = Target->ResolveDepotKits();
		const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(Depot, Specs);
		// INDEXED BY EDepotModule: DepotKitSpecs walks the enum, so a spec's index IS its module (its header).
		for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
		{
			Ceilings->Add(Reserved.IsSet() ? Reserved->CeilingFor(Kit) : 0);
		}
	}
	const int32 Kit = static_cast<int32>(Module);
	return Ceilings->IsValidIndex(Kit) ? (*Ceilings)[Kit] : 0;
}

TArray<FAirlineOffers> UOpsRuntime::AirlineOffersFromCatalog() const
{
	// THE CROSSING. Model/ may not read a UAircraftType, so the definitions are flattened
	// into airframes here, where Entities/ is legal - the same division of labour
	// URoadNetwork::PlaceEntity uses for a design wingspan.
	TArray<FAirlineOffers> Out;
	for (const UAirlineDefinition* Airline : Catalog->All<UAirlineDefinition>())
	{
		if (Airline == nullptr)
		{
			continue;
		}
		FAirlineOffers& Offering = Out.AddDefaulted_GetRef();
		Offering.Airline = Airline;
		for (const TSoftObjectPtr<UAircraftType>& SoftType : Airline->Fleet)
		{
			// LoadSynchronous, not a bare Get(): Fleet is now a SOFT reference (issue #191 -
			// Model/AirlineDefinition.h may not own a hard pointer to an Entities/ type), and
			// this is exactly the crossing point named below - Entities/ is legal to resolve
			// HERE, in Present/, same as UAirsideSettings::ResolveDefaultVehicle resolves its
			// own TSoftObjectPtrs.
			UAircraftType* Type = SoftType.LoadSynchronous();
			if (Type == nullptr)
			{
				continue;
			}
			FOfferCandidate Candidate;
			Candidate.Airframe = Type->Airframe();
			Candidate.AirlineName = Airline->DisplayName;
			Candidate.TypeName = Type->DisplayName;
			Offering.Fleet.Add(Candidate);
		}
	}
	return Out;
}

void UOpsRuntime::OfferTick()
{
	++OfferTicks;
	// AN AIRLINE'S "CAN IT COME?" is re-judged inside TickMinute below, which announces nothing - so the
	// alerts pass looks again every offer minute (spec 2026-09-29-ops-alerts §1).
	Bus.MarkDirty(TEXT("Alerts"));
	if (Target == nullptr || Target->Network == nullptr)
	{
		return;
	}
	if (AirlineOffers.Num() == 0)
	{
		// No airlines loaded. Almost always the missing PrimaryAssetTypesToScan line rather
		// than an empty world - see UAirlineDefinition's header. Attach has already warned.
		return;
	}

	// FORWARDED: choosing a runway is UFlightBoard's decision now, not this class's - see
	// UFlightBoard::DefaultApproachFocus (issue #98). Left untouched when the airport has no
	// runway yet, same as before.
	FVector2D Focus;
	if (UFlightBoard::DefaultApproachFocus(*Target->Network, Focus))
	{
		FlightBoard->ApproachFocus = Focus;
	}

	const TArray<UFlight*> Made = OfferGenerator->TickMinute(*Target->Network,
		FlightBoard->ApproachFocus, AirlineOffers, *Clock, FlightBoard->PendingOfferCount(),
		[this]() { return FlightBoard->TakeNextId(); });
	for (UFlight* Offer : Made)
	{
		FlightBoard->AddOffer(*Clock, Offer);
		UE_LOG(LogAirportOps, Log, TEXT("Offer %d: %s %s, %s, %.0f s to answer"),
			Offer->Id, *Offer->Callsign, *Offer->AirlineName.ToString(), *Offer->TypeName.ToString(),
			Offer->OfferSecondsLeft);
	}
}

FVehicle UOpsRuntime::StandDesignVehicleOf(const FEntityInstance& Stand)
{
	return UAirsideSettings::ResolveStandDesignVehicleOf(Stand.Definition.Get(), UJobBoard::LetterOfStand(Stand));
}

UGroundTraffic* UOpsRuntime::LiveModel() const
{
	if (Target == nullptr || Target->Network == nullptr || Target->GetTraffic() == nullptr)
	{
		return nullptr;
	}
	return Target->GetTraffic()->GetModel();
}

void UOpsRuntime::WireBus()
{
	// THE WHOLE SUBSCRIPTION MAP, in one function - see FOpsEventBus. Reset first: Attach runs
	// again on a level change, and a second wiring on top of the first would handle every
	// event twice.
	// ENFORCED BY: AirportOps.Present.Bus.ReattachDoesNotDouble
	Bus.ResetWiring();
	Bus.BeginWiring();

	// SIM: the boards, job board first - the order OnAgentPhase kept by hand before the bus.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("JobBoard"), [this](const FAgentPhaseEvent& E)
	{
		if (UGroundTraffic* Model = LiveModel())
		{
			JobBoard->OnAgentPhase(*Model, *Target->Network, *Clock, E.AgentId, E.From, E.To);
		}
		// ANY PHASE CHANGE may be the board's business - an aircraft parked, a vehicle arrived or lost
		// its agent - and deciding which here would be a second copy of OnAgentPhase's own rules.
		Bus.MarkDirty(TEXT("JobBoard"));
	});
	// THE PLAYER DREW SOMETHING: a new depot seeds its fleet, a refused job may be servable now.
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	// A VEHICLE BOUGHT OR SOLD, A MODULE BOUGHT: the board's candidates changed, and a job waiting on an
	// empty depot meets the new vehicle on this drain's pass - nothing polls (facility-upgrades spec §3).
	// ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFleetChangedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFacilityUpgradedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });

	// THE JOB BOARD'S WHOLE SEQUENCE, as one pass (stage 3 - see UJobBoard::Step for why it stays one
	// sequence). It runs when something above marked it, when its deadline comes due, or - while it
	// says something is unresolved - once each frame, which is exactly what Tick used to do always.
	// ENFORCED BY: AirportOps.Present.Bus.QuietBoardDoesNoWork
	Bus.RegisterPass(TEXT("JobBoard"), [this]()
	{
		UGroundTraffic* Model = LiveModel();
		if (Model == nullptr)
		{
			return;
		}
		const bool bUnresolved = JobBoard->Step(*Model, *Target->Network, *Clock);
		ArmJobBoardDeadline();
		// A STEP MAY HAVE MADE A JOB UNSERVICEABLE, or servable again - the alerts pass reads the result.
		Bus.MarkDirty(TEXT("Alerts"));
		if (bUnresolved)
		{
			Bus.MarkDirtyNextDrain(TEXT("JobBoard"));
		}
	});
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAgentPhaseEvent& E)
	{
		if (UGroundTraffic* Model = LiveModel())
		{
			FlightBoard->OnAgentPhase(*Model, *Target->Network, *Clock, E.AgentId, E.From, E.To);
		}
	});

	// THE ARRIVAL QUEUE, as a pass (ops batch 3 §5) - see RunArrivalQueue. After the job board's pass and before the
	// alerts', so a dispatch and the alerts that read it land in the same round. Every dirtier below is an event that
	// can let a holding flight land; each goes through DirtyArrivalQueue, which is how a safety run knows it was not
	// asked for. AFTER THE CLOCK in Tick, still: a flight that came due this frame publishes FlightInbound from the
	// clock's callback, so it is holding and can be cleared this frame if its runway is free.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.EachEventDirtiesIt
	Bus.RegisterPass(TEXT("ArrivalQueue"), [this]() { RunArrivalQueue(); });
	// A RUNWAY OR A STAND FREED: what a holding flight waits for (Airside's diff, bridged in Attach).
	Bus.Subscribe<FRunwayFreedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FRunwayFreedEvent&) { DirtyArrivalQueue(); });
	Bus.Subscribe<FStandsFreedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FStandsFreedEvent&) { DirtyArrivalQueue(); });
	// AN ACCEPT: a zero-lead accept (key 7's AcceptImmediate) is due at once, and the queue re-reserves.
	Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FOfferAcceptedEvent&) { DirtyArrivalQueue(); });
	// A FLIGHT JOINS THE QUEUE - its ETA came (UFlightBoard::Enqueue).
	Bus.Subscribe<FFlightInboundEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FFlightInboundEvent&) { DirtyArrivalQueue(); });
	// THE PLAYER BUILT OR DELETED something: a new stand, exit or runway may be the one a flight was refused for.
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FNetworkChangedEvent&) { DirtyArrivalQueue(); });
	// A REOPEN admits the queue again (a closure cancels it through the flight board's handler above).
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FAirportStatusChangedEvent&) { DirtyArrivalQueue(); });
	// A RESUME: TickQueue clears nobody while paused, and a pass run then has consumed its dirt - so the speed change
	// that un-pauses is itself a dirtier. Not in the spec's list; its omission would hold the queue until the net.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.EachEventDirtiesIt ("a resume")
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FSpeedChangedEvent&) { DirtyArrivalQueue(); });
	// THE NEW AGENT'S ARRIVING, the other half of ONE CLEARANCE A FRAME: after a dispatch the pass does not re-dirty
	// itself; this does, and RunArrivalQueue defers it to the next frame - so a second runway gets its flight then.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FAgentPhaseEvent& E)
	{
		if (E.To == EAgentPhase::Arriving)
		{
			DirtyArrivalQueue();
		}
	});

	// A REOPEN FORGETS EVERY AIRLINE'S VERDICT (review M1): while closed no airline was judged, so a verdict from
	// before the closure is stale - the player may have built what it wanted. Sim, so the alerts pass this change
	// dirties reads the forgotten state; the next offer minute judges afresh.
	// ENFORCED BY: AirportOps.Present.Airport.ReopenForgetsAirlineVerdicts
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("Offers"), [this](const FAirportStatusChangedEvent& E)
	{
		if (E.New == EAirportStatus::Open)
		{
			OfferGenerator->ForgetAirlineVerdicts();
		}
	});

	// THE AIRPORT'S STATUS, re-derived when the network changes - a runway built or deleted (spec 2026-09-29-ops-
	// batch3 §3). Sim: the status is state the flight board's handler below acts on in the next round.
	// ENFORCED BY: AirportOps.Present.Airport.RunwayComesAndGoes
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("Airport"), [this](const FNetworkChangedEvent&)
	{
		if (Target != nullptr && Target->Network != nullptr)
		{
			Airport->Refresh(*Target->Network);
		}
	});
	// ENTERING A CLOSED STATUS CANCELS WHAT HAS NOT ARRIVED - whichever closed it, the player or the last runway.
	// Every non-Open New, not only a change from Open: NoRunway -> ClosedByPlayer finds nothing left, and costs
	// one scan. Mapped here so the board never learns the airport.
	// ENFORCED BY: AirportOps.Present.Airport.CloseCancelsThroughTheBus
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAirportStatusChangedEvent& E)
	{
		UGroundTraffic* Model = LiveModel();
		if (E.New == EAirportStatus::Open)
		{
			return;
		}
		if (Model == nullptr)
		{
			// SAID, not skipped (review M4): an accepted flight nothing cancels would keep coming to a closed airport.
			// ENFORCED BY: AirportOps.Present.Airport.ClosureWithNoTrafficWarns
			UE_LOG(LogAirportOps, Warning, TEXT("Airport %s cancels nothing: no traffic model attached"), *UEnum::GetValueAsString(E.New));
			return;
		}
		FlightBoard->CancelUnarrived(*Model, *Clock,
			E.New == EAirportStatus::NoRunway ? ECancelReason::NoRunway : ECancelReason::AirportClosed);
	});

	// REACTION: the airlines, reading what the boards have already settled.
	Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FFlightAirborneEvent& E) { Airlines->OnFlightAirborne(E); });
	Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FOfferExpiredEvent& E) { Airlines->OnOfferExpired(E); });
	Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FOfferDeclinedEvent& E) { Airlines->OnOfferDeclined(E); });
	Bus.Subscribe<FDayEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FDayEndedEvent& E) { Airlines->OnDayEnded(E); });
	// THE FLIGHT BOARD IS HANDED IN, so the job board that published this never learns flights: the roster
	// resolves the airline through the agent the event names. WHAT KEEPS THE FLIGHT FINDABLE IS PUBLISH ORDER
	// WITHIN THE DRAIN, not the tier (review M1): the event is published by the job board's Sim handler for the
	// aircraft leaving Parked, and the flight board unhooks the agent only on its Gone - a later phase change,
	// published later, so dispatched in a later round than this.
	// ENFORCED BY: AirportOps.Present.Bus.TurnaroundShortfallReachesAirline, AirportOps.Present.Bus.UnfuelledDepartureLowersAirline
	Bus.Subscribe<FTurnaroundEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FTurnaroundEndedEvent& E) { Airlines->OnTurnaroundEnded(E, FlightBoard); });
	// A CANCELLED FLIGHT, from either publisher (a closure, an Unstick despawn); the roster charges only a closure.
	// ENFORCED BY: AirportOps.Present.Airport.CloseCancelsThroughTheBus ("its airline charged the closure penalty")
	Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Reaction, TEXT("Airlines"),
		[this](const FFlightCancelledEvent& E) { Airlines->OnFlightCancelled(E); });

	// PRESENTATION: the line the PIE check greps for (spec §4) - a satisfaction change as the bus
	// delivered it, which is what proves the chain end to end rather than the roster's own log line.
	Bus.Subscribe<FAirlineSatisfactionEvent>(EOpsTier::Presentation, TEXT("Log"), [](const FAirlineSatisfactionEvent& E)
	{
		UE_LOG(LogOpsBus, Log, TEXT("Airline %s satisfaction %.2f -> %.2f: %s"),
			*E.AirlineId.ToString(), E.Old, E.New, *E.Cause);
	});

	// PRESENTATION: UOpsEvents, the BP/UMG face of the bus. Its Notify* functions keep their
	// UE_LOG lines, so the log is unchanged.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAgentPhaseEvent& E) { Events->NotifyAgentPhaseChanged(E.AgentId, E.From, E.To); });
	Bus.Subscribe<FArrivalRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FArrivalRefusedEvent& E) { Events->NotifyArrivalRefused(E.Why); });
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FSpeedChangedEvent& E) { Events->NotifySpeedChanged(E.Speed); });
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FNotificationEvent& E) { Events->NotifyNotification(E.Text); });

	// THE ALERTS PASS (spec 2026-09-29-ops-alerts §1) - registered AFTER the job board's, so a Step and the
	// alerts that follow from it land in the same round. Dirtied, in the Reaction tier, by the events below,
	// by the job board pass, and by the OFFER MINUTE (OfferTick). The offer minute is also the catch-all: a
	// condition that starts or ends with no event of its own - a deadlock's stall time passing the
	// threshold, an airline's admission re-judged, a balance crossing zero before stage 3's MoneyPosted -
	// is seen within one game minute, ~1 real second at x1 (2026-09-29). A separate stall backstop on a
	// 10 game s timer was cut in review: at the day compression it fired about seven times a real second
	// whenever any agent queued, recomputing every alert.
	// ENFORCED BY: AirportOps.Present.Alerts.PassRaisesThroughTheRuntime
	Bus.RegisterPass(TEXT("Alerts"), [this]() { RecomputeAlerts(); });
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FAgentPhaseEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferExpiredEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferDeclinedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FFlightAirborneEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// OVERDRAWN, as soon as money moves - no longer waiting for the offer minute (stage 3). Every post, not
	// only a sign change: the pass is coalesced, and only Overdrawn reads the balance.
	Bus.Subscribe<FMoneyPostedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FMoneyPostedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// AN ACCEPT CHANGES BOARD STATE, so the pass that reads the board runs (batch 3 §2) - for the pass's own
	// correctness, not for any alert today: no alert kind can be raised or cleared BY an accept (Reserve holds
	// only a stand with a pose, and HeldStandLost needs one without - review M2). It is here so the next
	// condition about accepted flights is right without anyone remembering this line; the arrival queue's pass
	// joins this event in PR D.
	// ENFORCED BY: AirportOps.Present.Alerts.AcceptDirtiesAlerts
	Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferAcceptedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// THE STATUS IS READ BY THE PASS (NoRunway; airlines judged only while Open), and a closure changes it with no
	// network change of its own.
	// ENFORCED BY: AirportOps.Present.Airport.StatusChangeDirtiesAlerts
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FAirportStatusChangedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });

	// PRESENTATION: the new UOpsEvents faces.
	Bus.Subscribe<FAlertRaisedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertRaisedEvent& E) { Events->OnAlertRaised.Broadcast(E.Alert); });
	Bus.Subscribe<FAlertClearedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertClearedEvent& E) { Events->OnAlertCleared.Broadcast(E.Key); });
	Bus.Subscribe<FAlertsResetEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertsResetEvent&) { Events->OnAlertsReset.Broadcast(); });
	Bus.Subscribe<FBuildRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FBuildRefusedEvent& E) { Events->OnBuildRefused.Broadcast(E.What, E.Price, E.Balance); });
	Bus.Subscribe<FLandRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FLandRefusedEvent& E) { Events->OnLandRefused.Broadcast(E.Why); });
	Bus.Subscribe<FBalanceSignChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FBalanceSignChangedEvent& E) { Events->OnBalanceSignChanged.Broadcast(E.bOverdrawn); });

	// THE PURCHASE TOASTS - through the notification face every other toast uses, not a delegate of their
	// own (§6 deviation 4: nothing would bind one; the inspector re-reads the quote anyway).
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFleetChangedEvent& E)
	{
		const FFuelVehicleSpec Spec = JobBoard->SpecFor(E.TypeCode);
		const FString Name = Spec.DisplayName.IsEmpty() ? E.TypeCode.ToString() : Spec.DisplayName.ToString();
		Events->NotifyNotification(FString::Printf(TEXT("%s %s \u2014 %s"),
			E.Change == EFleetChange::Bought ? TEXT("Bought") : TEXT("Sold"), *Name, *Pricing->Format(E.Amount).ToString()));
	});
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFacilityUpgradedEvent& E)
	{
		const FModuleOffer* Offer = FacilityPurchases->ModuleOffers.Find(E.Module);
		const FString Name = Offer != nullptr ? Offer->DisplayName.ToString() : UEnum::GetValueAsString(E.Module);
		Events->NotifyNotification(FString::Printf(TEXT("Bought %s \u2014 %s"), *Name, *Pricing->Format(E.Amount).ToString()));
	});

	Bus.EndWiring();
}

void UOpsRuntime::RecomputeAlerts()
{
	UGroundTraffic* Model = LiveModel();
	FOpsAlertSources Sources;
	Sources.Flights = FlightBoard;
	Sources.Jobs = JobBoard;
	Sources.Traffic = Model;
	Sources.Network = Target != nullptr ? Target->Network.Get() : nullptr;
	Sources.Offers = OfferGenerator;
	Sources.Ledger = Ledger;
	Sources.Airport = Airport;
	Sources.Airlines = AirlineOffers;
	Alerts->Recompute(Sources, Clock->Now());
}

void UOpsRuntime::OnBuildRefused(const FBuildQuote& Quote, EBuildRefusal Why)
{
	// PRICED BY THE PURSE, which is the ledger: Airside knows only the base amount (FBuildQuote's own
	// comment - "what it COSTS is AirportOps' answer").
	Bus.Publish(FBuildRefusedEvent{ Quote.What.ToString(), Why, Ledger->Describe(Quote).ToString(),
		Pricing->Format(Ledger->Balance()).ToString() });
}

void UOpsRuntime::DirtyArrivalQueue()
{
	bQueueCovered = true;
	Bus.MarkDirty(TEXT("ArrivalQueue"));
}

void UOpsRuntime::RunArrivalQueue()
{
	UGroundTraffic* Model = LiveModel();
	if (Model == nullptr)
	{
		return;
	}
	// ONE CLEARANCE A FRAME, ACROSS ROUNDS: the Arriving event of the flight just cleared is dispatched in this same
	// drain's next round and dirties the pass again; a second clearance now would be decided in the frame the first
	// was, which TickQueue's own rule exists to prevent. Deferred, not dropped - and bQueueCovered is left set, so
	// the deferred run is still one an event asked for.
	if (QueueClearedFrame == DrainFrame)
	{
		Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue"));
		return;
	}
	const bool bSafetyOnly = bQueueSafetyDue && !bQueueCovered;
	bQueueSafetyDue = false;
	bQueueCovered = false;

	const FQueueTick Result = FlightBoard->TickQueue(*Model, *Target->Network, *Clock);
	if (Result.Cleared != nullptr)
	{
		QueueClearedFrame = DrainFrame;
		if (bSafetyOnly)
		{
			// THE DEFECT, NAMED: nothing published said this flight could land, yet it could. Whatever freed its
			// runway or stand needs an event (or a DirtyArrivalQueue) of its own.
			UE_LOG(LogOpsBus, Warning, TEXT("safety pass dispatched flight %d (%s) - no event covered it"),
				Result.Cleared->Id, *Result.Cleared->Callsign);
		}
	}
	if (Result.bRetry)
	{
		// A DISPATCH REFUSED in a same-frame race (UFlightBoard::DispatchNow): retried next frame, never dropped -
		// and COVERED (review M1): the retry is the tail of a run something asked for, not the net's own find.
		// ENFORCED BY: AirportOps.Present.ArrivalQueue.RetryStaysCovered
		bQueueCovered = true;
		Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue"));
	}
	ArmQueueSafetyNet(Result.Waiting > (Result.Cleared != nullptr ? 1 : 0));
}

void UOpsRuntime::ArmQueueSafetyNet(bool bWaiting)
{
	if (bWaiting && QueueSafetyHandle == INDEX_NONE)
	{
		QueueSafetyHandle = Clock->Every(QueueSafetySeconds, [this]()
		{
			bQueueSafetyDue = true;
			Bus.MarkDirty(TEXT("ArrivalQueue"));
		});
	}
	else if (!bWaiting)
	{
		if (QueueSafetyHandle != INDEX_NONE)
		{
			Clock->Cancel(QueueSafetyHandle);
			QueueSafetyHandle = INDEX_NONE;
		}
		// BOTH FLAGS GO WITH THE NET (review M1): a safety-due or a covered left over from a disarmed net - a load,
		// a detach, an emptied queue - would misattribute the next run, one way or the other.
		bQueueSafetyDue = false;
		bQueueCovered = false;
	}
}

void UOpsRuntime::OnRunwayFreed(FRoadSegmentId Seed)
{
	// PUBLISHED, NOT HANDLED - OnAgentPhase's reason: this runs inside UGroundTraffic's Advance.
	Bus.Publish(FRunwayFreedEvent{ Seed });
}

void UOpsRuntime::OnStandsFreed(const TArray<FGuidelineNodeId>& PoseNodes)
{
	Bus.Publish(FStandsFreedEvent{ PoseNodes });
}

void UOpsRuntime::ArmJobBoardDeadline()
{
	if (JobBoardDeadlineHandle != INDEX_NONE)
	{
		Clock->Cancel(JobBoardDeadlineHandle);
		JobBoardDeadlineHandle = INDEX_NONE;
	}
	const double Next = JobBoard->NextDeadline(Clock->Now());
	if (Next < TNumericLimits<double>::Max())
	{
		JobBoardDeadlineHandle = Clock->At(Next, [this]()
		{
			JobBoardDeadlineHandle = INDEX_NONE;
			Bus.MarkDirty(TEXT("JobBoard"));
		});
	}
}

void UOpsRuntime::Attach(ARoadNetworkActor* Actor)
{
	Detach();
	Target = Actor;
	if (Target == nullptr || Target->GetTraffic() == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("OpsRuntime attached to nothing: no ARoadNetworkActor"));
		return;
	}
	// WIRED BEFORE the Airside delegates are bound, so nothing can be published into a bus with
	// no handlers for it.
	WireBus();
	UAirsideTraffic* Traffic = Target->GetTraffic();
	PhaseHandle = Traffic->OnAgentPhaseChanged.AddUObject(this, &UOpsRuntime::OnAgentPhase);
	RefusalHandle = Traffic->OnArrivalRefused.AddUObject(this, &UOpsRuntime::OnArrivalRefused);
	// AIRSIDE'S DERIVED FREEDOM (ops batch 3 §5) - Airside never learns ops exists; it fires native delegates and
	// this bridges them, like the two above.
	// ENFORCED BY: Check-Architecture rule 1b (cross-plugin) for "never learns"; AirportOps.Present.Bus.FreedIsBridged for the bridge
	RunwayFreedHandle = Traffic->OnRunwayFreed.AddUObject(this, &UOpsRuntime::OnRunwayFreed);
	StandsFreedHandle = Traffic->OnStandsFreed.AddUObject(this, &UOpsRuntime::OnStandsFreed);

	// Content is resolved ONCE, here, and applied to the clock and the ledger.
	if (Catalog->Num() == 0)
	{
		Catalog->LoadFromAssetManager();
	}
	// Never null - it falls back to UScenario's CDO, so the defaults are actually applied
	// rather than skipped. See ResolveDefaultScenario.
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Catalog))
	{
		Clock->RealSecondsDaylight = Scenario->RealSecondsDaylight;
		Clock->RealSecondsNight = Scenario->RealSecondsNight;
		Clock->DawnHour = Scenario->DawnHour;
		Clock->DuskHour = Scenario->DuskHour;

		// BEFORE THE OFFER SCHEDULE BELOW, and that ordering is load-bearing: Every() books
		// its first firing at Now() + Interval, so moving the clock after scheduling would
		// leave the first offer due at a time that no longer means what it did.
		Clock->StartAtHour(Scenario->StartHour);

		// The designer figures set from the same asset in the same breath, so none of them
		// is the one somebody forgot to copy.
		JobBoard->VehicleSpecs = Scenario->FuelVehicles;
		JobBoard->RefillLitresPerMinutePerPump = Scenario->DepotRefillLitresPerMinutePerPump;
		FacilityPurchases->ModuleOffers = Scenario->ModuleOffers;
		OfferGenerator->MaxPendingOffers = Scenario->MaxPendingOffers;
		Airlines->Tuning = Scenario->AirlineSatisfaction;
		// A NEW GAME, like the ledger's Open below: an airport attached afresh starts every airline at
		// the tuning's start. A load overwrites it from the snapshot moments later.
		Airlines->ResetForNewGame();

		// THE BALANCE A NEW GAME OPENS AT. The comment that used to stand at the top of this
		// block said this would happen "when the ledger exists (M3)"; this is that. A LOAD
		// overwrites it moments later from the saved entries, which is why Open is safe here:
		// it is the new-game path, and OpsSave::Restore is the other one.
		Ledger->Open(Scenario->StartingBalance);

		UE_LOG(LogAirportOps, Log,
			TEXT("Scenario '%s': %.0f/%.0f real s day/night (%02.0f-%02.0f), starts %02.0f:00, %d fuel vehicle kind(s), refill %.0f L/min/pump, opens at %.0f"),
			*Scenario->GetName(), Scenario->RealSecondsDaylight, Scenario->RealSecondsNight,
			Scenario->DawnHour, Scenario->DuskHour, Scenario->StartHour,
			Scenario->FuelVehicles.Num(), Scenario->DepotRefillLitresPerMinutePerPump, Scenario->StartingBalance);
	}

	// EVERY LETTER'S FUEL VEHICLE resolved HERE, once, into FuelService's table - not by
	// FuelService at every dispatch (#104): this is Present/, where every other content default
	// gets resolved, and Model/ has no business reaching Content/ for it. One table and not one
	// truck since 2026-09-26 (far-side-entry spec): A and B stands are sized for the utility tow,
	// C to F for the fuel truck, so a single vehicle would send a truck onto an A stand's lane
	// that only the tow was proven to drive.
	// ENFORCED BY: AirportOps.Fuel.RuntimeResolvesPerStand
	JobBoard->ResolveVehicles([](EIcaoCode Letter) { return UAirsideSettings::ResolveStandDesignVehicle(Letter); });
	// AND WHAT EACH STAND WAS BUILT FOR, read off its own definition when the guard asks - see
	// UJobBoard::DesignVehicleOf for why the read is handed down rather than made there.
	// ENFORCED BY: AirportOps.Fuel.RuntimeResolvesPerStand (A sent the truck still reads as tow-built)
	JobBoard->DesignVehicleOf = &UOpsRuntime::StandDesignVehicleOf;

	// THE PURCHASE SERVICE'S TWO WORLD HOOKS (facility-upgrades spec §3; UJobBoard::DesignVehicleOf's
	// pattern): the plot's ceiling from Airside's one solve, and the module write through the facade's one
	// door - which rebuilds the yard and checkpoints undo. `this` for the ceiling, which the runtime memoises
	// and outlives nothing; weak for the actor, the dispatcher's reason below.
	// ENFORCED BY: AirportOps.Present.Facility.ShedPurchaseRelightsASlot
	FacilityPurchases->ReservedSlotsOf = [this](FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)
	{
		return ReservedSlotsOf(Id, Depot, Module);
	};
	{
		TWeakObjectPtr<ARoadNetworkActor> WeakActor = Target;
		FacilityPurchases->ApplyModulePurchase = [WeakActor](FEntityInstanceId Id, EDepotModule Module)
		{
			ARoadNetworkActor* Actor = WeakActor.Get();
			URoadEditFacade* Facade = Actor != nullptr ? Actor->GetEditFacade() : nullptr;
			return Facade != nullptr && Facade->AddEntityModule(Id, Module);
		};
	}

	// THE LITRES A FLIGHT WAS OFFERED AT reach its fuel demand through the board - see
	// UJobBoard::LitresOwedFor. Weak, for the dispatcher's reason below.
	TWeakObjectPtr<UFlightBoard> WeakBoard = FlightBoard;
	JobBoard->LitresOwedFor = [WeakBoard](int32 AgentId, const FAirframe& Airframe)
	{
		const UFlightBoard* Board = WeakBoard.Get();
		const UFlight* Flight = Board != nullptr ? Board->FlightForAgent(AgentId) : nullptr;
		return Flight != nullptr ? Flight->FuelLitres : UJobBoard::DefaultLitres(Airframe);
	};
	{
		// READ BACK OFF THE TABLE, every letter, rather than a banner typed beside the resolve:
		// the line then says what dispatch will actually send.
		FString PerLetter;
		for (int32 Index = 0; Index < UJobBoard::LetterCount; ++Index)
		{
			const EIcaoCode Letter = static_cast<EIcaoCode>(Index);
			PerLetter += FString::Printf(TEXT("%s %s  "), IcaoCode::ToLetter(Letter),
				*JobBoard->VehiclesFor(Letter).TypeCode.ToString());
		}
		UE_LOG(LogAirportOps, Log, TEXT("Fuel vehicles by stand letter: %s"), *PerLetter.TrimEnd());
	}

	// THE MONEY, wired in one breath like the scenario figures above, so none of these is the
	// one somebody forgot to connect. Each of the three posts to the ledger for its own part of
	// a flight: the generator prices the offer, the board banks landing and parking, the fuel
	// service banks a completed fuelling.
	OfferGenerator->Pricing = Pricing;
	FlightBoard->Ledger = Ledger;
	FlightBoard->Pricing = Pricing;
	FlightBoard->Fuel = JobBoard;
	FlightBoard->Bus = &Bus;
	Alerts->Bus = &Bus;
	Ledger->Bus = &Bus;
	// A NEW AIRPORT, A NEW SET: the old actor's alerts name its flights and agents.
	Alerts->Reset();
	Airlines->Bus = &Bus;
	// A NEW GAME OPENS, beside the ledger's Open and the roster's reset above; a load overwrites the intent from its
	// "Airport" blob. RE-DERIVED SILENTLY - an attach is not a change the player made, and a runway-less new game
	// has nothing to cancel. The first NetworkChanged then finds the status already right.
	// ENFORCED BY: AirportOps.Present.Airport.RunwayComesAndGoes ("the attach re-derives", "publishes no status change")
	Airport->Bus = &Bus;
	Airport->ResetForNewGame();
	if (Target->Network != nullptr)
	{
		Airport->Reseat(*Target->Network);
	}
	JobBoard->Ledger = Ledger;
	JobBoard->Pricing = Pricing;
	JobBoard->Bus = &Bus;
	Ledger->Pricing = Pricing;
	Ledger->Clock = Clock;

	// THE LEDGER IS THE PURSE the build tools spend from. Handed to the facade here and
	// nowhere else, so design-time building - which has no runtime and therefore no purse -
	// stays free. See IBuildPurse.
	if (URoadEditFacade* Facade = Target->GetEditFacade())
	{
		Facade->SetPurse(Ledger);
		// THE FACADE'S SILENT REFUSAL, bridged: "cannot afford" at commit (spec 2026-09-29-ops-alerts §2).
		RefusedHandle = Facade->OnRefused.AddUObject(this, &UOpsRuntime::OnBuildRefused);
	}

	// THE ONE PRODUCTION DISPATCHER. Weak, because the board outlives a level change and a
	// captured raw pointer would keep a dead actor alive - or worse, be used.
	TWeakObjectPtr<ARoadNetworkActor> WeakTarget = Target;
	FlightBoard->Dispatcher = [WeakTarget](const FVector2D& Near, const FAirframe& Airframe)
	{
		ARoadNetworkActor* Actor = WeakTarget.Get();
		return Actor != nullptr && Actor->DispatchArrival(Near, Airframe);
	};

	// ONE GENERATOR MINUTE, every game minute, for every airline - replacing one fixed-interval
	// timer whose interval was computed here once, from the fee at Attach, and never again
	// (spec 2026-09-28 problem 2). Each airline's own curve and the live fee are read inside
	// the tick - see UOfferGenerator::TickMinute.
	AirlineOffers = AirlineOffersFromCatalog();
	SeedAirlines();

	// THE AIRLINE'S MOOD reaches demand through the generator's one reader - READ, not subscribed:
	// a rate is a value asked for when it is needed (spec 2026-09-29 section 3). Weak, for the
	// dispatcher's reason below.
	// ENFORCED BY: AirportOps.Present.Bus.SatisfactionMovesRate
	TWeakObjectPtr<UAirlineRoster> WeakAirlines = Airlines;
	OfferGenerator->AirlineFactorOf = [WeakAirlines](const UAirlineDefinition& Airline)
	{
		const UAirlineRoster* Roster = WeakAirlines.Get();
		return Roster != nullptr ? Roster->RateMultiplier(Airline.GetFName(), Airline.bIsFloor) : 1.0;
	};
	RearmRepeatingSchedules();
	{
		// THE DAY'S EXPECTED TOTAL, integrated from the same RateAt the generator follows, so
		// the banner is a measurement of the mechanism rather than a figure typed beside it.
		double Expected = 0.0;
		for (int32 Hour = 0; Hour < 24; ++Hour)
		{
			const double Midpoint = (Hour + 0.5) * 3600.0;
			Expected += UOfferGenerator::TotalRateAt(AirlineOffers, Midpoint,
				Clock->IsDaylight(Midpoint), OfferGenerator->DemandFactor(),
				[this](const UAirlineDefinition& Airline) { return OfferGenerator->AirlineFactor(Airline); });
		}
		FString Floors;
		for (const FAirlineOffers& Each : AirlineOffers)
		{
			if (Each.Airline != nullptr && Each.Airline->bIsFloor)
			{
				Floors += (Floors.IsEmpty() ? TEXT("") : TEXT(", ")) + Each.Airline->DisplayName.ToString();
			}
		}
		UE_LOG(LogAirportOps, Log, TEXT("Offers: ~%.0f expected today across %d airline(s), inbox holds %d (floor: %s)"),
			Expected, AirlineOffers.Num(), OfferGenerator->MaxPendingOffers,
			Floors.IsEmpty() ? TEXT("none") : *Floors);
		if (Expected <= 0.0)
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Offers: no airline offers anything, so the inbox will stay empty"));
		}
		else if (Floors.IsEmpty())
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Offers: no airline is the floor - the airport can go silent"));
		}
	}

	ApplySpeed(Clock->GetSpeed());
	// EVERY PASS ONCE after an attach, as after a load (spec 2026-09-29 §4): nothing that is true of the airport now
	// arrived as an event. The first NetworkChanged used to be the only catch-up; this says so where the attach is.
	Bus.MarkAllDirty();
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntime attached to %s"), *Target->GetName());
}

void UOpsRuntime::RearmRepeatingSchedules()
{
	if (UpkeepHandle != INDEX_NONE) { Clock->Cancel(UpkeepHandle); }
	if (OfferHandle != INDEX_NONE) { Clock->Cancel(OfferHandle); }

	// ONE ENTRY A DAY, not one per object: a hundred-stand airport would otherwise write a
	// hundred rows a day into a saved array, and RollUp would spend its life folding them.
	UpkeepHandle = Clock->Every(USimClock::SecondsPerDay, [this]() { PostDailyUpkeep(); });

	// ONE GENERATOR MINUTE, every game minute - see Attach for why this replaced a single
	// fixed-interval timer.
	OfferHandle = Clock->Every(UOfferGenerator::TickSeconds, [this]() { OfferTick(); });
}

void UOpsRuntime::Detach()
{
	// THE PURSE, UN-WIRED - the other half of Attach's "handed to the facade here and
	// nowhere else" (issue #193): Attach calls Facade->SetPurse(Ledger), but until this fix
	// nothing here ever called SetPurse(nullptr) to match. The facade's Purse is a raw
	// IBuildPurse* precisely because it does not own the ledger and outlives no attach - see
	// its own comment - so a Detach that left it set kept pointing at THIS runtime's Ledger
	// after Target (and, on a level change, this whole object) could be gone, and
	// URoadEditFacade::CanAfford dereferences it on every quote design time is supposed to
	// treat as free again.
	if (Target != nullptr)
	{
		if (URoadEditFacade* Facade = Target->GetEditFacade())
		{
			Facade->SetPurse(nullptr);
			Facade->OnRefused.Remove(RefusedHandle);
		}
	}

	if (Target != nullptr && Target->GetTraffic() != nullptr)
	{
		Target->GetTraffic()->OnAgentPhaseChanged.Remove(PhaseHandle);
		Target->GetTraffic()->OnArrivalRefused.Remove(RefusalHandle);
		Target->GetTraffic()->OnRunwayFreed.Remove(RunwayFreedHandle);
		Target->GetTraffic()->OnStandsFreed.Remove(StandsFreedHandle);
	}
	if (UpkeepHandle != INDEX_NONE)
	{
		Clock->Cancel(UpkeepHandle);
		UpkeepHandle = INDEX_NONE;
	}
	if (OfferHandle != INDEX_NONE)
	{
		Clock->Cancel(OfferHandle);
		OfferHandle = INDEX_NONE;
	}
	AirlineOffers.Reset();
	if (JobBoardDeadlineHandle != INDEX_NONE)
	{
		Clock->Cancel(JobBoardDeadlineHandle);
		JobBoardDeadlineHandle = INDEX_NONE;
	}
	ArmQueueSafetyNet(false);
	SeenNetwork.Reset();
	// THE BUS POINTERS GO WITH THE ATTACH: the bus is this runtime's, and a subobject left pointing at it
	// after a detach is a publish into whatever comes next (stage 3 review). Every one Attach set.
	// ENFORCED BY: AirportOps.Present.Bus.DetachUnhooksEveryPublisher
	FlightBoard->Bus = nullptr;
	Alerts->Bus = nullptr;
	Ledger->Bus = nullptr;
	JobBoard->Bus = nullptr;
	Airlines->Bus = nullptr;
	Airport->Bus = nullptr;
	// THE QUEUE IS THE OLD ACTOR'S. A new level's traffic numbers its agents from 1 again, so a
	// stale Parked for agent k would land on the new level's agent k. Dropped, and the price is
	// that a re-Attach to the SAME actor loses at most one step of its events.
	// ENFORCED BY: AirportOps.Present.Bus.DetachDiscardsQueue
	Bus.Discard();
	// Cleared rather than left pointing at the old actor: a dispatcher that still answers
	// after a detach would put an aeroplane on a field this runtime no longer drives.
	FlightBoard->Dispatcher = nullptr;
	// THE PURCHASE HOOKS GO WITH THE ACTOR, the dispatcher's reason: a hook that still answered after a
	// detach would build into a field this runtime no longer drives.
	FacilityPurchases->ReservedSlotsOf = nullptr;
	FacilityPurchases->ApplyModulePurchase = nullptr;
	ReservationMemo.Reset();
	ReservationMemoNetwork.Reset();
	Target = nullptr;
}

void UOpsRuntime::Tick(double RealDeltaSeconds)
{
	Clock->Advance(RealDeltaSeconds);

	// THE NETWORK, COMPARED ONCE A FRAME: a different object (ClearNetwork, a load) or a new guideline
	// revision is FNetworkChangedEvent. One pointer and one integer - the whole cost of the job board
	// no longer re-scanning every depot every frame.
	if (Target != nullptr && Target->Network != nullptr)
	{
		const URoadNetwork* Network = Target->Network;
		const uint32 Revision = Network->GetGuidelineRevision();
		if (SeenNetwork.Get() != Network || SeenGuidelineRevision != Revision)
		{
			SeenNetwork = Network;
			SeenGuidelineRevision = Revision;
			Bus.Publish(FNetworkChangedEvent{ Revision });
		}
	}

	// ONE DRAIN, after the clock: the queue holds Airside's events from the motion tick in publish
	// order, then anything the clock just fired - so a flight that came due this frame is handled
	// this frame (spec 2026-09-29 §1). Counted, for the arrival queue's ONE CLEARANCE A FRAME.
	++DrainFrame;
	Bus.Drain();

	if (Target != nullptr)
	{
		// NO SetSimTimeScale HERE since ops batch 3 PR E - it re-set the same double every frame. The actor's scale is
		// set where the speed can change, all of which end in ApplySpeed: Attach (a new actor, whose Transient scale
		// starts at 1), StepSpeed, TogglePause, LoadFromSlot (the clock's speed is saved). A network clear replaces the
		// network, not the actor. Checked 2026-09-30: ApplySpeed is the only production caller of SetSimTimeScale.
		// ENFORCED BY: AirportOps.Present.SimTimeScale.SetOnlyWhenItChanges; Check-Architecture rule 34 (scale-on-change)

		// THE NETWORK IS READ FRESH, never cached: URoadEditFacade::ClearNetwork replaces the
		// actor's network OBJECT rather than draining it, so a pointer held across a clear is
		// stale - the same reason LoadFromSlot re-reads it.
		//
		// AND NOTHING IS SCALED HERE. The fuel service reads the clock advanced above for its
		// pumping and refills (game time, spec 2026-09-28-fuel-litres) and the traffic model's
		// movement for its trucks, both already at the player's speed; scaling again here would
		// run them at the square of it.
		if (Target->Network != nullptr && Target->GetTraffic() != nullptr)
		{
			if (UGroundTraffic* Model = Target->GetTraffic()->GetModel())
			{
				// NO JobBoard->Tick HERE since stage 3: it is the bus's "JobBoard" pass, run by the drain
				// above when an event, its deadline or its own unresolved work says so - see WireBus.

				// THE RAW FRAME TIME, for the one countdown that runs in real seconds - see
				// UFlightBoard::TickOffers. It checks the pause itself.
				FlightBoard->TickOffers(*Model, *Target->Network, *Clock, RealDeltaSeconds);

				// NO FlightBoard->TickQueue HERE since ops batch 3 PR D: it is the bus's "ArrivalQueue" pass, run by
				// the drain above when an event dirties it - see WireBus and RunArrivalQueue.
				// ENFORCED BY: Check-Architecture rule 33 (queue-is-a-pass), AirportOps.Present.ArrivalQueue.QuietQueueRunsNothing
			}
		}
	}

	// Offers lapse in TickOffers above, on REAL seconds (spec 2026-09-28). Before that they
	// were a Clock.At at a game-time ExpiresAt (issue #105 item 9), and before THAT a per-frame
	// poll here; the game-time window shrank with the speed setting, which is why it went.
}

void UOpsRuntime::ApplySpeed(ESimSpeed Speed)
{
	Clock->SetSpeed(Speed);
	if (Target != nullptr)
	{
		// The MULTIPLIER, not TimeScale(): movement runs at the player's speed setting,
		// never at the day compression. See USimClock's class comment.
		++TimeScaleSets;
		Target->SetSimTimeScale(USimClock::Multiplier(Speed));
	}
	Bus.Publish(FSpeedChangedEvent{ Speed });
}

void UOpsRuntime::StepSpeed(int32 Delta)
{
	// THE LADDER WALK AND ResumeSpeed ARE THE CLOCK'S OWN NOW (issue #191): it is the object
	// that is actually saved, and it is the one with the day lengths and every other
	// speed-adjacent figure already. This is left to push the RESULT into the actor and the
	// event bus, which is Present/'s job - ApplySpeed re-applies Clock->GetSpeed() to itself
	// (a no-op; StepSpeed already set it) purely to reach the push/notify half in one call.
	Clock->StepSpeed(Delta);
	ApplySpeed(Clock->GetSpeed());
}

void UOpsRuntime::TogglePause()
{
	Clock->TogglePause();
	ApplySpeed(Clock->GetSpeed());
}

void UOpsRuntime::OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// PUBLISHED, NOT HANDLED. This runs inside UGroundTraffic's broadcast, and nothing may act
	// there (#193's re-entrancy contract exists because a listener that did could retire any agent
	// mid-loop). The rule this function used to keep by hand - THE SERVICE FIRST, THEN THE BUS, so a
	// Blueprint listener that asked the fuel service what an aircraft was doing never saw the state
	// from BEFORE the event that woke it - is now the tier order in WireBus: Sim, then Presentation.
	Bus.Publish(FAgentPhaseEvent{ AgentId, From, To });
}

void UOpsRuntime::OnArrivalRefused(EArrivalRefusal Why)
{
	Bus.Publish(FArrivalRefusedEvent{ Why });
}

void UOpsRuntime::PostDailyUpkeep()
{
	if (Target == nullptr || Target->Network == nullptr || Ledger == nullptr)
	{
		return;
	}

	// THE ONE CONTENT DEFAULT THIS FUNCTION RESOLVES, and the one reason it still exists as
	// more than a Clock->Every(...) line: BuildCost::DailyUpkeep lives in Build/, which
	// Model/ - where the posting and the skip-if-zero rule now live, see ULedger::
	// PostDailyUpkeep - may not include (Check-Architecture rule 1).
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	const double Base = BuildCost::DailyUpkeep(*Target->Network,
		Settings != nullptr ? Settings->ApronUpkeepPerSquareMetrePerDay : 0.0);
	// THE FACILITIES' SHARE, from the one service that knows what a module and a vehicle cost to keep
	// (facility-upgrades spec R6), as lines of their own so the finance screen can say where it went.
	const FFacilityUpkeep Facilities = FacilityPurchases->DailyUpkeep(*Target->Network);
	const FUpkeepLine Lines[] = {
		{ Base, NSLOCTEXT("Ledger", "DailyUpkeep", "Upkeep") },
		{ Facilities.Modules, NSLOCTEXT("Ledger", "FacilityUpkeep", "Facility upkeep") },
		{ Facilities.Fleet, NSLOCTEXT("Ledger", "FleetUpkeep", "Fleet upkeep") } };
	Ledger->PostDailyUpkeep(Lines, Clock->Now());

	// SAME BEAT, SAME REASON (issue #188): FlightBoard's own History needs no schedule of its
	// own either, and a second daily timer here would just be a second place for the two to
	// drift out of step with each other. UNCONDITIONAL, matching ULedger::PostDailyUpkeep's own
	// decision (issue #191) - a zero-upkeep day is not a reason to defer FlightBoard's
	// housekeeping either; the previous shape returned before reaching either RollUp whenever
	// Base was zero, which PR #213 flagged as "Not done" and left unresolved.
	FlightBoard->RollUp(Clock->Now());

	// THE DAY'S END, announced on the same beat rather than on a second daily timer - for the same
	// reason RollUp rides it (issue #188). The airlines forgive a little on it.
	Bus.Publish(FDayEndedEvent{ Clock->Day() });

	UE_LOG(LogAirportOps, Log, TEXT("Upkeep day %d: %.0f (+%.0f facilities, +%.0f fleet); balance %.0f"),
		Clock->Day(), Base, Facilities.Modules, Facilities.Fleet, Ledger->Balance());
}

TArray<IOpsPersistent*> UOpsRuntime::Persistents() const
{
	// ORDER IS THE SAME ON BOTH SIDES and that is all it has to be: every blob is keyed by its
	// own SaveBlobName, and no object's restore reads a neighbour's. OpsSave::Restore runs each
	// object's OnBeforeRestore, blob and OnAfterRestore in turn (not every OnBeforeRestore first,
	// as this comment used to say); anything that needs two restored systems at once - the
	// loaded clock's Now for #404's re-queue - runs in LoadFromSlot, after Restore, instead.
	TArray<IOpsPersistent*> Out;
	Out.Add(Clock);
	Out.Add(JobBoard);
	Out.Add(FlightBoard);
	Out.Add(Ledger);
	Out.Add(Pricing);
	Out.Add(OfferGenerator);
	Out.Add(Airlines);
	Out.Add(Airport);
	return Out;
}

bool UOpsRuntime::SaveToSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save refused: no network attached"));
		return false;
	}
	// DRAINED BEFORE THE SNAPSHOT: an Airside event queued since the last step would otherwise be
	// handled after the save, and its effect missing from it. NOT FROM INSIDE A DRAIN - a Blueprint
	// bound to a UOpsEvents delegate may save (an autosave on a notification), and a nested Drain
	// would assert; that save simply misses what is still queued, and says so.
	// ENFORCED BY: AirportOps.Present.Bus.SaveFromAHandler
	// THE STATUS FIRST (review M5): NetworkChanged is published by Tick, so an edit in the frame of the save would
	// otherwise snapshot an airport still open over a network with no runway - and a load re-derives silently, so its
	// flights would never be cancelled. A change published here is handled by the drain below.
	// ENFORCED BY: AirportOps.Present.Airport.SaveRefreshesTheStatus
	Airport->Refresh(*Target->Network);
	if (Bus.IsDraining())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save '%s' from inside an ops event handler: %d queued event(s) are not in it"),
			*SlotName, Bus.QueuedCount());
	}
	else
	{
		Bus.Drain();
	}
	FOpsSnapshot Snapshot;
	const TArray<IOpsPersistent*> Saved = Persistents();
	OpsSave::Capture(Saved, *Target->Network, Snapshot);
	const bool bOk = OpsSave::WriteSlot(SlotName, Snapshot);
	Bus.Publish(FNotificationEvent{ bOk ? FString::Printf(TEXT("Saved '%s'"), *SlotName)
	                                    : FString::Printf(TEXT("Save to '%s' failed"), *SlotName) });
	return bOk;
}

EArrivalRefusal UOpsRuntime::LandNear(const FVector2D& Focus, const FAirframe* Override)
{
	// THE VIEW FOCUS RATHER THAN THE CURSOR: the bar's Land button is clicked with the cursor
	// on the bar, where "nearest the cursor" is meaningless, and the focus is where the player
	// is looking either way - both drivers resolve Focus themselves before calling this.
	UE_LOG(LogAirportOps, Log, TEXT("Land: nearest runway to the view focus (%.0f, %.0f)"),
		Focus.X, Focus.Y);

	UGroundTraffic* Traffic = Target != nullptr ? Target->GetGroundTraffic() : nullptr;
	if (Target == nullptr || Target->Network == nullptr || Traffic == nullptr)
	{
		// NoRunway rather than a new refusal of its own: there is nothing here to check a
		// runway AGAINST, which is the same fact that refusal already names.
		UE_LOG(LogAirportOps, Warning, TEXT("Land: no attached network to land on."));
		Bus.Publish(FLandRefusedEvent{ EArrivalRefusal::NoRunway });
		return EArrivalRefusal::NoRunway;
	}

	// A CLOSED AIRPORT ADMITS NO ARRIVALS, THE DEBUG ONE INCLUDED (ruling I1, 2026-09-30) - asked BEFORE AcceptImmediate,
	// which leaves a refused flight in the inbox as an offer. NoRunway is Airside's own refusal and toasts as one; a
	// player's closure has no Airside word, so it says "Airport closed" and returns NotAdmitted ("may not use it").
	// ENFORCED BY: AirportOps.Present.Airport.LandRefusedWhileClosed
	if (Airport->Status() == EAirportStatus::NoRunway)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Land: refused - the airport has no runway"));
		Bus.Publish(FLandRefusedEvent{ EArrivalRefusal::NoRunway });
		return EArrivalRefusal::NoRunway;
	}
	if (Airport->Status() == EAirportStatus::ClosedByPlayer)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Land: refused - the airport is closed"));
		Bus.Publish(FNotificationEvent{ TEXT("Airport closed") });
		return EArrivalRefusal::NotAdmitted;
	}

	// THE SAME RESOLVER EVERY DISPATCH FALLS BACK TO, for the same reason: an aircraft that
	// approached as one airframe and taxied as another would be two different aircraft
	// depending on which phase you were watching - see UAirsideSettings::
	// ResolveDefaultAirframe.
	//
	// UNLESS THE CALLER PASSED ONE. Override exists so a particular aeroplane can be put on the
	// runway without waiting for the board to offer one - the driver's Land panel is the one
	// place that choice is made (ARoadBuildController::LandAircraftNearViewFocus's argument,
	// DefaultGame.ini's LandAircraftType until 2026-09-27), and it stays there: a player's pick
	// on the driver, not a fact about the airport this class owns.
	const FAirframe Airframe = Override != nullptr ? *Override : UAirsideSettings::ResolveDefaultAirframe();
	if (Override != nullptr)
	{
		UE_LOG(LogAirportOps, Log,
			TEXT("Land: using the chosen type (%s) rather than the default"),
			*Airframe.TypeCode.ToString());
	}
	else
	{
		UE_LOG(LogAirportOps, Log, TEXT("Land: using the content default airframe (%s)"),
			*Airframe.TypeCode.ToString());
	}

	// ONE CALL: make, aim, add and accept the debug flight are all UFlightBoard's job - see
	// AcceptImmediate's own header (issue #96). THROUGH THE BOARD WHEN THERE IS ONE, and there
	// always is one here - this class owns it - which is exactly what makes a direct dispatch
	// two doors onto arrival: an aeroplane landed straight onto the traffic model would belong
	// to no flight, so nothing would ever give its stand back or know it had landed.
	const EArrivalRefusal Why = FlightBoard->AcceptImmediate(*Traffic, *Target->Network, *Clock,
		Airframe, Focus, NSLOCTEXT("AirportOps", "DebugAirline", "(key 7)"));
	if (Why != EArrivalRefusal::None)
	{
		// SAID TO THE PLAYER, not only the log: AcceptImmediate refuses BEFORE any dispatch, so Airside's
		// OnArrivalRefused - the toast key 7 used to rely on - never fires (spec 2026-09-29-ops-alerts §2).
		// ENFORCED BY: AirportOps.Present.Alerts.LandRefusalReachesUi
		Bus.Publish(FLandRefusedEvent{ Why });
		// The key used to do nothing at all when the airport was full. Now it says which of the
		// seven refusals it was, in the sentence the inbox would show.
		UE_LOG(LogAirportOps, Warning, TEXT("Land: no flight. %s"),
			*ArrivalPlanner::DescribeRefusal(Why));
	}
	return Why;
}

bool UOpsRuntime::LoadFromSlot(const FString& SlotName)
{
	// Target->Network is read HERE, not cached: URoadEditFacade::ClearNetwork replaces the
	// actor's network OBJECT rather than draining it, so a pointer held across a clear is
	// stale. The actor's field is the one authority on which network is current.
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	if (!OpsSave::ReadSlot(SlotName, Snapshot))
	{
		Bus.Publish(FNotificationEvent{ FString::Printf(TEXT("No save '%s'"), *SlotName) });
		return false;
	}
	// Agents first: they were never saved, and one mid-taxi on a network about to be
	// replaced would be following a polyline through pavement that no longer exists.
	Target->GetTraffic()->ClearAgents();
	// THE QUEUE GOES WITH THEM, before Restore and whether or not it succeeds. The Gone events
	// ClearAgents just queued name agents that no longer exist, and handling them after the
	// boards are restored would un-hold restored flights' stands; a failed Restore must not leave
	// them for next frame either. The COST, taken on purpose: a Blueprint bound to
	// OnAgentPhaseChanged does not hear Gone for agents a load clears - it hears "Loaded" instead.
	// ENFORCED BY: AirportOps.Present.Bus.LoadDiscardsQueue (the queue is dropped, nothing hears it)
	Bus.Discard();
	// THE ALERTS GO WITH IT: they name agents and flights of the airport being replaced, and are derived -
	// the MarkAllDirty below re-raises whatever is true of the loaded one.
	// ENFORCED BY: AirportOps.Present.Alerts.PassRaisesThroughTheRuntime
	Alerts->Reset();
	const TArray<IOpsPersistent*> Loaded = Persistents();
	if (!OpsSave::Restore(Snapshot, Loaded, *Target->Network))
	{
		return false;
	}
	// #404, BESIDE ITS CAUSE (review ruling M5): the ClearAgents above threw away every aeroplane, so a flight saved
	// landing or taxiing in goes round again and one on the ground retires as departed - dated by the LOADED clock,
	// restored just now. Before OnGraphRebuilt below, which holds the re-queued flights' stands (after the genuine
	// holds), and before RearmSchedules, which touches Accepted flights only and so leaves the re-queue alone.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires
	const TArray<UFlight*> Requeued = FlightBoard->DemoteRestoredMidFlight(Clock->Now());
	// A SNAPSHOT FROM BEFORE THE "Airlines" BLOB restores no rows (OnBeforeRestore cleared them);
	// every catalog airline comes back at the tuning's start.
	SeedAirlines();
	// THE LOAD-TIME REPAIRS A LEVEL GETS FROM PostLoad AND PostRegisterAllComponents, which a
	// save game does not get - OpsSave::Restore is Serialize alone (final review C2).
	// Outlines first: a stand saved before stands had them gets its Code C box, and only a
	// stand with an outline has a letter to rebind by. Then every stand's definition, which
	// for D/E/F is a path to an object this session never built.
	Target->Network->EnsureStandOutlines();
	Target->RebindStandDefinitions();
	// THROUGH THE FACADE, not Target->History->Clear() directly (issue #191): the history is
	// URoadEditFacade's undo state to manage, and reaching past it from another plugin's
	// composition root is exactly the layering slip the facade's ClearHistory doc comment
	// names. The undo stack holds Mementos of the PRE-load network; an undo now would revert
	// the player to an airport they just replaced on purpose. A load is a new baseline.
	if (URoadEditFacade* Facade = Target->GetEditFacade())
	{
		Facade->ClearHistory();
	}
	// Present rebuilds from model: the mesh and the derived guideline graph are both
	// produced by the presenter's Rebuild, which is what RebuildMesh runs.
	Target->RebuildMesh();

	// THE STATUS RE-DERIVED FROM THE LOADED INTENT AND NETWORK, WITH NO EVENT (spec 2026-09-29-ops-batch3 §3): a
	// published change would reach the flight board's handler and re-run the closure's cancellation - scored, and
	// over every unarrived flight - when everything the closure cancelled was cancelled when this was saved. The
	// generator and the alerts read the result.
	// ENFORCED BY: AirportOps.Present.Airport.LoadRederivesWithoutCancelling
	Airport->Reseat(*Target->Network);

	// THE ONE THING A LOAD DOES HAVE TO CANCEL (review ruling I2): the flights the demotion above put back in the
	// queue were on the ground when the closure happened, so the closure left them - and at an airport that is not
	// open they can never land again. Cancelled HERE, silently and UNSCORED (CancelRequeued publishes nothing), not
	// by the event above: the airline did not lose them to the closure but to the save.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport
	if (Airport->Status() != EAirportStatus::Open)
	{
		FlightBoard->CancelRequeued(Requeued, Clock->Now());
	}

	// IN THIS ORDER, and both are needed. RebuildMesh regenerates the guideline graph, which
	// takes every node claim with it (FTrafficOccupancy::ReleaseGuidelineClaims), so the
	// restored flights' stand holds have to be re-made against the new nodes BEFORE anything
	// can allocate. Then the arrivals go back on the clock, whose queue was never saved.
	if (UGroundTraffic* Model = Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr)
	{
		FlightBoard->OnGraphRebuilt(*Model, *Target->Network, Requeued);
		FlightBoard->RearmSchedules(*Model, *Target->Network, *Clock);
	}
	// THE REPEATERS TOO, from the loaded Now - see RearmRepeatingSchedules (review I1).
	RearmRepeatingSchedules();
	// AND THE QUEUE'S SAFETY NET, for the same reason: its entry was booked against the pre-load clock. Cancelled;
	// the MarkAllDirty below runs the pass, which re-arms it if the loaded queue holds anyone.
	ArmQueueSafetyNet(false);

	// EVERY PASS ONCE after a load - the one catch-up, since nothing that happened before the load
	// is an event any more (spec 2026-09-29 §4). No passes exist until stage 3; the rule is here first.
	// THE ARRIVAL QUEUE'S among them: DemoteRestoredMidFlight (#404) sets a flight Inbound directly rather
	// than through UFlightBoard::Enqueue, so it publishes no FlightInbound, and this run is what dispatches it.
	Bus.MarkAllDirty();

	ApplySpeed(Clock->GetSpeed());
	Bus.Publish(FNotificationEvent{ FString::Printf(TEXT("Loaded '%s'"), *SlotName) });
	return true;
}
