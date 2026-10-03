#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Build/BuildCost.h"
#include "Build/DepotKit.h"
#include "Content/AirportOpsSettings.h"
#include "Content/AirsideSettings.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ExhaustiveSwitch.h"
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
#include "Model/FlightBilling.h"
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

namespace
{
	/**
	 * The purchase toast a fleet change is, false for none (#445 item 7). EVERY CHANGE BY NAME, NO default: a fourth way
	 * a vehicle can join or leave the fleet is a BUILD ERROR here (C4062), not a change the feed silently never says.
	 * ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	 */
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	bool PurchaseKindOf(EFleetChange Change, EOpsPurchaseKind& OutKind)
	{
		switch (Change)
		{
		case EFleetChange::Bought:    OutKind = EOpsPurchaseKind::VehicleBought; return true;
		case EFleetChange::Sold:      OutKind = EOpsPurchaseKind::VehicleSold; return true;
		case EFleetChange::Withdrawn: OutKind = EOpsPurchaseKind::VehicleWithdrawn; return true;
		// A STARTER VEHICLE changed no hands - see the FleetChanged subscriber in WireBus.
		case EFleetChange::Seeded:    return false;
		}
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END
}

UOpsRuntime::UOpsRuntime()
{
	Clock = CreateDefaultSubobject<USimClock>(TEXT("Clock"));
	// THE NET'S TWO COLLABORATORS ARE THIS OBJECT'S OWN FOR ITS LIFE - the bus a member, the clock a subobject - so it is bound here,
	// once, and not at each attach (#445); a detach cancels its entry, which is all it ever needs undoing.
	SafetyNet.Bind(Bus, *Clock, SafetyNetSeconds);
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

	// The money, and the same forwarding shape: this class gains two pointers and the wiring
	// below, and every decision about what things cost lives in UPricing, not here.
	Ledger = CreateDefaultSubobject<ULedger>(TEXT("Ledger"));
	Pricing = CreateDefaultSubobject<UPricing>(TEXT("Pricing"));

	// THE MONEY, wired in one breath, so none of these is the one somebody forgot to connect.
	// Each of the three posts to the ledger for its own part of a flight: the generator prices
	// the offer, billing banks landing and parking, the fuel service banks a completed fuelling. BILLING'S LEDGER IS NOT
	// WIRED HERE (#506 review): the "Billing" subscription in WireBus hands it this runtime's Ledger on every call, so the
	// flight board holds no money pointer at all.
	// HERE, NOT IN Attach, since #425: every pointer is one of this runtime's own subobjects, constant for its
	// life, and Transient on the object that holds it - a save must not carry a path to it - so nothing sets it
	// again after a load, and an unattached runtime is wired too. OfferGenerator->Airport, below, is the precedent.
	// ENFORCED BY: AirportOps.Present.FlightBoardIsComposedByTheRuntime
	OfferGenerator->Pricing = Pricing;
	FlightBoard->Pricing = Pricing;
	FlightBoard->Fuel = JobBoard;
	JobBoard->Ledger = Ledger;
	JobBoard->Pricing = Pricing;
	Ledger->Pricing = Pricing;
	Ledger->Clock = Clock;

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
		return Live == nullptr || Live->AdmitsArrivals();
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
	// ITS BUS IS NOT SET HERE (#445): it was, and never cleared - the seventh publisher, in none of Attach, Detach or the Detach test.
	// Every publisher is pointed at the bus by Attach's loop over Publishers(), and taken back by Detach's.
	// ENFORCED BY: Check-Architecture rule 71 (every-bus-publisher-is-listed), AirportOps.Present.Bus.DetachUnhooksEveryPublisher
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

bool UOpsRuntime::CancelFlight(int32 FlightId)
{
	UGroundTraffic* Model = LiveModel();
	if (Model == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("CancelFlight %d refused: no airport attached"), FlightId);
		return false;
	}
	// THE RULE IS THE BOARD'S. What this adds is the wake-up: a cancel is a command, not an event, and what it frees - a stand a
	// holding flight was waiting for, a queue place, the alert that offered it - is read by passes that run on events. The
	// FFlightCancelled it publishes dirties the alerts pass (WireBus); the queue is told here, as every other
	// dirtier tells it.
	const bool bCancelled = FlightBoard->CancelByPlayer(*Model, *Clock, FlightId);
	if (bCancelled)
	{
		Bus.MarkDirty(TEXT("ArrivalQueue"));
	}
	return bCancelled;
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
	// THE POINTER AND THE REVISION, not the pointer alone: an in-place restore (a rolled-back edit) keeps
	// the network object and changes what is in it, and only the revision - which RestoreFrom moves forward -
	// says so. See ReservedSlotsOf's header.
	const uint32 Revision = Target->Network != nullptr ? Target->Network->GetEditRevision() : 0;
	if (ReservationMemoNetwork.Get() != Target->Network || ReservationMemoRevision != Revision)
	{
		ReservationMemo.Reset();
		ReservationMemoNetwork = Target->Network;
		ReservationMemoRevision = Revision;
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
	// NO ALERTS MARK HERE ANY MORE (#446): an airline's "can it come?" is re-judged inside TickMinute below, which used to announce
	// nothing, so the alerts pass looked again every offer minute - about every frame at x32 - whether or not anything had moved. It
	// announces now (FAirlineAdmissionChangedEvent, which WireBus routes to the pass) and the deadlock condition has a clock entry
	// of its own (ArmDeadlockLook).
	// ENFORCED BY: AirportOps.Present.Alerts.QuietMinutesRunNoAlertsPass
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

	// FORWARDED: choosing the point the runways are ordered from is UFlightBoard's decision now, not this class's - see
	// UFlightBoard::DefaultRunwayPreference (issue #98; named DefaultApproachFocus until #442). Left untouched when the
	// airport has no runway yet, same as before.
	FVector2D Focus;
	if (UFlightBoard::DefaultRunwayPreference(*Target->Network, Focus))
	{
		FlightBoard->RunwayPreference = Focus;
	}

	const TArray<UFlight*> Made = OfferGenerator->TickMinute(*Target->Network,
		FlightBoard->RunwayPreference, AirlineOffers, *Clock, FlightBoard->PendingOfferCount(),
		[this]() { return FlightBoard->TakeNextId(); });
	for (UFlight* Offer : Made)
	{
		FlightBoard->AddOffer(*Clock, Offer);
		UE_LOG(LogAirportOps, Log, TEXT("Offer %d: %s %s, %s, %.0f s to answer"),
			Offer->Id, *Offer->Callsign, *Offer->AirlineName.ToString(), *Offer->TypeName.ToString(),
			Offer->OfferSecondsLeft);
	}
}

TArray<FVehicle> UOpsRuntime::StandVehiclesOf(const FEntityInstance& Stand)
{
	return UAirsideSettings::ResolveStandVehiclesOf(Stand.Definition.Get(), UJobBoard::LetterOfStand(Stand));
}

void UOpsRuntime::ResolveVehicleCatalogue(UJobBoard& Board, const UScenario& Scenario)
{
	Board.Fleet().ResolveCatalogue(Scenario.FuelVehicles, Scenario.StarterFleet,
		[](FName TypeCode) { return UAirsideSettings::ResolveVehicle(TypeCode); });
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

	// THE STARTER FLEET OF A DEPOT THE AIRPORT GAINED (#443), a pass - see UJobBoard::SeedStarterFleets. It was the first loop
	// of every job board Step, finding a placed depot by walking every entity; placement is ANNOUNCED now, so it is woken by
	// the announcement (FNetworkChangedEvent, published in the rebuild that committed the depot, whichever door it came
	// through: a placement, an undo, a load) and by the attach's and a load's MarkAllDirty, the catch-up for the depots that
	// were there before anything was announced. REGISTERED BEFORE THE "JobBoard" PASS, so a drain seeds and then bids: a
	// starter depot's CouldServe verdict and its first bid are made from the same real vehicles in the same drain.
	// A LOAD DOES NOT RE-SEED: SeededDepots is saved and restored, so the pass the load wakes finds every depot seen.
	// ENFORCED BY: AirportOps.Present.Fleet.PlacedDepotIsSeededByTheAnnouncement, AirportOps.Present.Fleet.LoadDoesNotReseedADepotThatHasVehicles,
	// AirportOps.Present.Fleet.AttachSeedsTheStarterFleetOnTheFirstDrain; Check-Architecture rule 60 (starter-fleet-seeded-on-announcement)
	Bus.RegisterPass(TEXT("FleetSeed"), [this](const FPassRun&)
	{
		if (Target != nullptr && Target->Network != nullptr)
		{
			JobBoard->SeedStarterFleets(*Target->Network, *Clock);
		}
	});
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("FleetSeed"),
		[this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("FleetSeed")); });

	// THE UNPLACED-MODULE REPAIR (#266, owner 2026-09-30, option b: "there must never be unplaced modules"), a pass - see
	// UFacilityPurchases::RemoveUnseated. REGISTERED BEFORE THE JOB BOARD'S PASS, so a drain that repairs runs that pass after it and
	// the board bids with the pumps that are standing. DIRTIED by the attach's and a load's MarkAllDirty (the catch-up:
	// an old save, a level whose kits changed since it was built) and by every network change, the other moment a plot's
	// seat can move. Not by a purchase: the shop refuses one past the ceiling, so it can never leave an excess.
	// ENFORCED BY: AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated, AirportOps.Present.Facility.RepairRunsAfterALoad;
	// the purchase half by AirportOps.Model.Facility.UnseatedModulesGrantNeitherBaysNorPumps (a buy past the ceiling: NoSlotReserved)
	Bus.RegisterPass(TEXT("ModuleRepair"), [this](const FPassRun&)
	{
		if (Target != nullptr && Target->Network != nullptr)
		{
			FacilityPurchases->RemoveUnseated(*Target->Network);
		}
	});
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("ModuleRepair"),
		[this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("ModuleRepair")); });

	// SIM: the boards, job board first - the order OnAgentPhase kept by hand before the bus.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("JobBoard"), [this](const FAgentPhaseEvent& E)
	{
		if (UGroundTraffic* Model = LiveModel())
		{
			JobBoard->OnAgentPhase(*Model, *Target->Network, *Clock, E);
		}
		// ANY PHASE CHANGE may be the board's business - an aircraft parked, a vehicle arrived or lost
		// its agent - and deciding which here would be a second copy of OnAgentPhase's own rules.
		Bus.MarkDirty(TEXT("JobBoard"));
	});
	// THE PLAYER DREW SOMETHING: a refused job may be servable now, and so may a refused departure - a runway, a route or
	// the push arm it had none of. Every refusal but PushbackBlocked waits on this. (A new depot's starter fleet is NOT this
	// pass's: the "FleetSeed" pass, woken by the same event and registered ahead of this one, seeds it in the same drain, so
	// the bids below meet the vehicles - Check-Architecture rule 60.)
	// ENFORCED BY: AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet ("drawing the arm")
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	// A VEHICLE BOUGHT OR SOLD, A MODULE BOUGHT: the board's candidates changed, and a job waiting on an
	// empty depot meets the new vehicle on this drain's pass - nothing polls (facility-upgrades spec §3).
	// ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFleetChangedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFacilityUpgradedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	// A MODULE REMOVED BY THE REPAIR (#266): the capability did not change - an unseated module granted nothing - but the
	// board's pass is where a changed depot is looked at, and a repair is rare enough that asking costs nothing.
	Bus.Subscribe<FModulesRefundedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FModulesRefundedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	// A PUSH NO LONGER BLOCKED (Airside's push watch, bridged in Attach): the refused departure it names can go now.
	// What bDepartureWaiting used to find by re-running the whole Step every frame (ops push-ground-freed).
	// ENFORCED BY: AirportOps.Present.PushGroundFreed.DepartsTheFrameAfter
	Bus.Subscribe<FPushGroundFreedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FPushGroundFreedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });

	// THE JOB BOARD'S WHOLE SEQUENCE, as one pass (stage 3 - see UJobBoard::Step for why it stays one
	// sequence). It runs when something above marked it, when its deadline comes due, while it says a vehicle or
	// job is unresolved - once each frame - and on the safety net while a due turnaround's departure is refused. AFTER the two passes
	// that put vehicles under it: the fleet a new depot starts with and the modules the repair leaves standing - it bids with both.
	// ENFORCED BY: AirportOps.Present.Bus.QuietBoardDoesNoWork, AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet,
	// AirportOps.Present.Bus.WiringOrderIsDeclared
	Bus.RegisterPass(TEXT("JobBoard"), [this](const FPassRun& Run)
	{
		UGroundTraffic* Model = LiveModel();
		if (Model == nullptr)
		{
			return;
		}
		const bool bUnresolved = JobBoard->Step(*Model, *Target->Network, *Clock);
		// A RUN ONLY THE NET ASKED FOR - RunArrivalQueue's rule, for the other pass the net watches.
		if (Run.IsSafetyOnly())
		{
			for (const int32 AircraftId : JobBoard->DepartedLastStep())
			{
				// THE DEFECT, NAMED: nothing published said this aircraft could go, yet it could. Whatever freed its
				// push ground, runway or route needs an event of its own.
				UE_LOG(LogOpsBus, Warning, TEXT("safety pass departed aircraft %d - no event covered it"), AircraftId);
			}
		}
		ArmJobBoardDeadline();
		// A STEP MAY HAVE MADE A JOB UNSERVICEABLE, or servable again - the alerts pass reads the result.
		Bus.MarkDirty(TEXT("Alerts"));
		if (bUnresolved)
		{
			// A RETRY (EPassCause::Retry, MarkDirtyNextDrain's default): the tail of a run something asked for, so a run of it that
			// finds work is not the net's find.
			Bus.MarkDirtyNextDrain(TEXT("JobBoard"));
		}
		// ENFORCED BY: AirportOps.Present.PushGroundFreed.NetArmedAndCancelled
		SafetyNet.Want(TEXT("JobBoard"), JobBoard->HasRefusedDeparture(Clock->Now()));
	}, { TEXT("FleetSeed"), TEXT("ModuleRepair") });
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAgentPhaseEvent& E)
	{
		// ASKED FOR THE LIVE AIRPORT, not handed the model: the flight board maps the event's Cause and reads no agent
		// (#436), so the model is only the sign that an airport is attached and its network is the one to ask.
		if (LiveModel() != nullptr)
		{
			FlightBoard->OnAgentPhase(*Target->Network, *Clock, E);
		}
	});
	// BILLING, A REACTION TO THE PHASE (#442 item 4): the landing fee, the parking clock and the parking fee, posted by FlightBilling
	// when a flight ENTERS Landing, Turnaround or TaxiOut - which the board's TransitionTo announces for every change. It was the tail of
	// UFlightBoard::OnAgentPhase, inline, after every agent event; the board no longer knows money. SIM, not Reaction: it writes state
	// (the ledger, and the flight's paid flags) - and what reads it (the alerts' overdrawn check, through FMoneyPostedEvent; the bar's
	// balance after Tick) reads it settled. A ROUND LATER than inline, in the same drain: Landing is entered in the "ArrivalQueue" pass
	// and Turnaround/TaxiOut in the handler above, both inside Tick's drain - so Tick still returns with the fee posted.
	// ENFORCED BY: AirportOps.Present.Bus.BillingIsWired (red with this line gone), AirportOps.Model.FlightFees.BilledOnTheBusARoundLater
	Bus.Subscribe<FFlightPhaseChangedEvent>(EOpsTier::Sim, TEXT("Billing"), [this](const FFlightPhaseChangedEvent& E)
	{
		// THIS RUNTIME'S LEDGER, handed in (#506 review) - the board does not hold one, so it cannot be the one forgotten.
		FlightBilling::OnFlightPhaseChanged(Ledger, *FlightBoard, E);
	});

	// THE ARRIVAL QUEUE, as a pass (ops batch 3 §5) - see RunArrivalQueue. After the job board's pass and before the
	// alerts' (the alerts pass declares After this one), so a dispatch and the alerts that read it land in the same round. Every
	// dirtier below is an event that can let a holding flight land; each marks the pass with the default cause, which is how a safety
	// run knows it was not asked for. AFTER THE CLOCK in Tick, still: a flight that came due this frame publishes FlightInbound from the
	// clock's callback, so it is holding and can be cleared this frame if its runway is free.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.EachEventDirtiesIt
	Bus.RegisterPass(TEXT("ArrivalQueue"), [this](const FPassRun& Run) { RunArrivalQueue(Run); }, { TEXT("JobBoard") });
	// A RUNWAY OR A STAND FREED: what a holding flight waits for (Airside's diff, bridged in Attach).
	Bus.Subscribe<FRunwayFreedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FRunwayFreedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	Bus.Subscribe<FStandsFreedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FStandsFreedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// AN ACCEPT: a zero-lead accept (key 7's AcceptImmediate) is due at once, and the queue re-reserves.
	Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FOfferAcceptedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// A FLIGHT JOINS THE QUEUE - its ETA came (FArrivalQueue::Enqueue, UFlightBoard's until #442 item 4).
	Bus.Subscribe<FFlightInboundEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FFlightInboundEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// THE PLAYER BUILT OR DELETED something: a new stand, exit or runway may be the one a flight was refused for.
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// A REOPEN admits the queue again (a closure cancels it through the flight board's handler above).
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FAirportStatusChangedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// A RESUME: TickQueue clears nobody while paused, and a pass run then has consumed its dirt - so the speed change
	// that un-pauses is itself a dirtier. Not in the spec's list; its omission would hold the queue until the net.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.EachEventDirtiesIt ("a resume")
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FSpeedChangedEvent&) { Bus.MarkDirty(TEXT("ArrivalQueue")); });
	// THE NEW AGENT'S ARRIVING, the other half of ONE CLEARANCE A FRAME: after a dispatch the pass does not re-dirty
	// itself; this does, and the queue defers it to the next frame (FQueueTick::bDeferred) - so a second runway gets its flight then.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("ArrivalQueue"), [this](const FAgentPhaseEvent& E)
	{
		if (E.To == EAgentPhase::Arriving)
		{
			Bus.MarkDirty(TEXT("ArrivalQueue"));
		}
	});

	// A REOPEN FORGETS EVERY AIRLINE'S VERDICT (review M1): while closed no airline was judged, so a verdict from
	// before the closure is stale - the player may have built what it wanted. Sim, so the alerts pass this change
	// dirties reads the forgotten state; the next offer minute judges afresh.
	// ENFORCED BY: AirportOps.Present.Airport.ReopenForgetsAirlineVerdicts
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("Offers"), [this](const FAirportStatusChangedEvent& E)
	{
		if (UAirport::AdmitsArrivals(E.New))
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
		if (UAirport::AdmitsArrivals(E.New))
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
	// UE_LOG lines, so the log is unchanged. NO FACE FOR THE PHASE OR THE SPEED (#445): both delegates had no listener - nothing bound
	// them outside a test - and the wiring test counted this very line as their consumer; the boards and the queue consume both in the
	// Sim tier, and every event has one. A delegate is added here WITH its listener.
	// ENFORCED BY: AirportMgr.UI.EveryOpsEventDelegateHasAListener
	Bus.Subscribe<FArrivalRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FArrivalRefusedEvent& E) { Events->NotifyArrivalRefused(E.Why, E.Sentence); });
	// A SAVE OR A LOAD, BY CASE (#445 item 7): the face carries the outcome and the slot, and the toast words them.
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FSaveSlotEvent& E) { Events->NotifySaveSlot(E.Outcome, E.Slot); });

	// THE ALERTS PASS (spec 2026-09-29-ops-alerts §1) - AFTER the job board's and the arrival queue's (declared), so a Step, a dispatch
	// and the alerts that follow from them land in the same round. Dirtied, in the Reaction tier, by the events below and by the job
	// board pass. THERE IS NO CATCH-ALL ANY MORE (#446): the offer minute used to dirty it every game minute - about every frame at
	// x32 - for the two conditions that start with no event of their own. An airline's admission re-judged now announces itself
	// (FAirlineAdmissionChangedEvent, below) and a deadlock's stall time passing its threshold is a clock entry (ArmDeadlockLook); a
	// balance crossing zero is FMoneyPostedEvent. A separate stall backstop on a 10 game s timer was cut in review: at the day
	// compression it fired about seven times a real second whenever any agent queued, recomputing every alert.
	// ENFORCED BY: AirportOps.Present.Alerts.PassRaisesThroughTheRuntime, AirportOps.Present.Alerts.QuietMinutesRunNoAlertsPass,
	// AirportOps.Present.Bus.WiringOrderIsDeclared
	Bus.RegisterPass(TEXT("Alerts"), [this](const FPassRun&) { RecomputeAlerts(); }, { TEXT("JobBoard"), TEXT("ArrivalQueue") });
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FAgentPhaseEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FNetworkChangedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FNetworkChangedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferExpiredEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferDeclinedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FFlightAirborneEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// A FLIGHT JOINS THE QUEUE, OR LEAVES IT BY A CANCEL (#442): FlightCannotLand reads the clearance the ArrivalQueue pass
	// computes for a holding flight, and it runs before this pass in the same round - so the alert is raised the round the flight
	// is judged, not at the next offer minute; and a cancel clears it the round it happens. A closure's cancel is covered by
	// its own AirportStatusChanged below, which these do not replace.
	// ENFORCED BY: AirportOps.Model.Alerts.UnlandableHoldingFlightRaisesAnAlert, AirportOps.Present.Alerts.CancelFlightForwardsToTheBoard
	Bus.Subscribe<FFlightInboundEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FFlightInboundEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FFlightCancelledEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// OVERDRAWN, as soon as money moves - no longer waiting for the offer minute (stage 3). Every post, not
	// only a sign change: the pass is coalesced, and only Overdrawn reads the balance.
	Bus.Subscribe<FMoneyPostedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FMoneyPostedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// AN ACCEPT CHANGES BOARD STATE, so the pass that reads the board runs (batch 3 §2) - for the pass's own
	// correctness, not for any alert today: no alert kind can be raised or cleared BY an accept (Hold holds
	// only a stand with a pose, and HeldStandLost needs one without - review M2). It is here so the next
	// condition about accepted flights is right without anyone remembering this line; the arrival queue's pass
	// joins this event in PR D.
	// ENFORCED BY: AirportOps.Present.Alerts.AcceptDirtiesAlerts
	Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FOfferAcceptedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// THE STATUS IS READ BY THE PASS (NoRunway; airlines judged only while Open), and a closure changes it with no
	// network change of its own.
	// ENFORCED BY: AirportOps.Present.Airport.StatusChangeDirtiesAlerts
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FAirportStatusChangedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });
	// AN AIRLINE'S VERDICT MOVED (#446): the one condition the offer-minute catch-all was for. Published by the generator's own judgement.
	// ENFORCED BY: AirportOps.Model.Offers.AdmissionChangeIsAnnounced, AirportOps.Present.Alerts.QuietMinutesRunNoAlertsPass
	Bus.Subscribe<FAirlineAdmissionChangedEvent>(EOpsTier::Reaction, TEXT("Alerts"), [this](const FAirlineAdmissionChangedEvent&) { Bus.MarkDirty(TEXT("Alerts")); });

	// PRESENTATION: the new UOpsEvents faces.
	Bus.Subscribe<FAlertRaisedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertRaisedEvent& E) { Events->OnAlertRaised.Broadcast(E.Alert); });
	Bus.Subscribe<FAlertClearedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertClearedEvent& E) { Events->OnAlertCleared.Broadcast(E.Key); });
	// A STANDING ALERT'S WORDS MOVED (#445): the window re-reads the model - the event names only the key.
	Bus.Subscribe<FAlertChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertChangedEvent& E) { Events->OnAlertChanged.Broadcast(E.Key); });
	Bus.Subscribe<FAlertsResetEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAlertsResetEvent&) { Events->OnAlertsReset.Broadcast(); });
	Bus.Subscribe<FBuildRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FBuildRefusedEvent& E) { Events->OnBuildRefused.Broadcast(E.What, E.Price, E.Balance); });
	Bus.Subscribe<FLandRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FLandRefusedEvent& E) { Events->OnLandRefused.Broadcast(E.Why, E.Sentence); });
	Bus.Subscribe<FBalanceSignChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FBalanceSignChangedEvent& E) { Events->OnBalanceSignChanged.Broadcast(E.bOverdrawn); });

	// THE PURCHASE TOASTS - through one typed face, FOpsPurchase (§6 deviation 4 kept: nothing would bind a delegate per event,
	// and the inspector re-reads the quote anyway). THE FACTS AND THE NOUNS GO, THE SENTENCES DO NOT (#445 item 7): each name
	// is its owner's one function - FServiceFleet::NameOf, the offer's DisplayName/PluralName, UPricing::Format - and the
	// toast widget words them. These handlers used to build "Bought <name> - <price>" here, beside a widget whose job is to
	// decide the player's words.
	// ENFORCED BY: Check-Architecture rule 4 ('one-string dynamic delegate (a sentence face)'), AirportMgr.UI.ToastsWordSavesAndPurchases
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFleetChangedEvent& E)
	{
		// A SEEDED VEHICLE IS NOT A PURCHASE: the player did nothing and nothing was paid, so there is nothing to tell them. A
		// WITHDRAWN one is (#443): its depot went and the fleet's door credited it, and the feed should say where that money
		// came from. NOT A FILTER ON WORDS, which would be the widget's: a seeding changed no hands, so it is not on the face.
		FOpsPurchase Purchase;
		if (!PurchaseKindOf(E.Change, Purchase.Kind))
		{
			return;
		}
		Purchase.Name = FServiceFleet::NameOf(*JobBoard, E.TypeCode);
		// A SALE WORTH NOTHING SAYS NO MONEY (#487: a seeded vehicle fetches no resale), the way a removal that credited nothing
		// says "withdrawn" rather than "credited $0" - the widget reads Amount for that, so it travels as posted.
		Purchase.Amount = E.Amount;
		Purchase.Money = Pricing->Format(E.Amount);
		Events->NotifyPurchase(Purchase);
	});
	// A LAND TILE BOUGHT: a receipt, the noun is "land" (land purchase spec R10).
	Bus.Subscribe<FLandPurchasedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FLandPurchasedEvent& E)
	{
		FOpsPurchase Purchase;
		Purchase.Kind = EOpsPurchaseKind::LandBought;
		Purchase.Name = NSLOCTEXT("AirportOps", "LandNoun", "land");
		Purchase.Amount = E.Amount;
		Purchase.Money = Pricing->Format(E.Amount);
		Events->NotifyPurchase(Purchase);
	});
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFacilityUpgradedEvent& E)
	{
		const FModuleOffer* Offer = FacilityPurchases->ModuleOffers.Find(E.Module);
		FOpsPurchase Purchase;
		Purchase.Kind = EOpsPurchaseKind::ModuleBought;
		Purchase.Name = Offer != nullptr ? Offer->DisplayName : FText::FromString(UEnum::GetValueAsString(E.Module));
		Purchase.Amount = E.Amount;
		Purchase.Money = Pricing->Format(E.Amount);
		Events->NotifyPurchase(Purchase);
	});
	// THE REPAIR'S TOAST, A WARNING (#266): the player did not ask for it, and the depot now holds less than it did. The log's
	// Warning (RemoveUnseated's) carries the depot and the counts; the toast is what the player reads, and its severity is the
	// widget's to give, by EOpsPurchaseKind::ModulesRefunded.
	// ENFORCED BY: AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated ("the toast"), AirportMgr.UI.ToastsWordSavesAndPurchases
	Bus.Subscribe<FModulesRefundedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FModulesRefundedEvent& E)
	{
		const FModuleOffer* Offer = FacilityPurchases->ModuleOffers.Find(E.Module);
		FOpsPurchase Purchase;
		Purchase.Kind = EOpsPurchaseKind::ModulesRefunded;
		// THE OFFER'S OWN WORDS, singular or plural as data (FModuleOffer::PluralName), not an appended "s".
		Purchase.Name = Offer == nullptr ? FText::FromString(UEnum::GetValueAsString(E.Module))
			: (E.Count == 1 ? Offer->DisplayName : Offer->PluralName);
		Purchase.Count = E.Count;
		Purchase.Amount = E.Amount;
		Purchase.Money = Pricing->Format(E.Amount);
		Events->NotifyPurchase(Purchase);
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
	// THE NEXT LOOK AT A STALL, booked from what is on the ground NOW - after every run, whatever woke it.
	ArmDeadlockLook();
}

void UOpsRuntime::ArmDeadlockLook()
{
	if (DeadlockLookHandle != INDEX_NONE)
	{
		Clock->Cancel(DeadlockLookHandle);
		DeadlockLookHandle = INDEX_NONE;
	}
	const UGroundTraffic* Model = LiveModel();
	if (Model == nullptr)
	{
		return;
	}
	// THE BOOKING RULE (#445 review, round 3). Nothing announces the ONSET of a stall - an agent becoming blocked is no event - so a look booked only
	// for an agent already SEEN stalled would leave a jam that forms with no later event unalerted for good (two aircraft nose to nose and nothing else
	// happening; the old offer-minute catch-all found it within a game minute). But a first stall can only begin on a ROUTE, so:
	//  - WHILE ANY AGENT IS ON ONE (FRoadAgent::IsOnRoute: Taxiing, Manoeuvring, Reversing) a look is kept booked at the stall-threshold cadence, so
	//    an onset is caught within about two thresholds of game movement time. This is armed by the phase event itself: an agent entering a route
	//    phase publishes FAgentPhaseEvent, which dirties the alerts pass (WireBus), whose run ends here - and it is cancelled the same way, by the run
	//    the last route agent's leaving causes. A field of PARKED or stranded aircraft, or none, books NOTHING (it did, while "an agent exists" was the
	//    rule: a parked aircraft kept the pass firing at the old catch-all's rate).
	//  - A STALLED AGENT (one whose stall clock is running: FRoadAgent::IsStoppedAndWaiting, #429's one definition of the clock's feed) books the
	//    moment its stall would cross the threshold, when that is sooner than the cadence. The threshold is the traffic's own (CurrentDeadlocks reads a stall strictly past it); one already past it is looked at again a threshold
	//    on, since a ring is made by the agents not yet stalled joining it.
	// THE COST, said: about one alerts recompute per threshold (3 motion s: ~63 game s by day) for as long as anything moves - the old catch-all's rate
	// less the time nothing moves.
	// ENFORCED BY: AirportOps.Present.Alerts.NoseToNoseJamIsAlertedWithNoOtherEvent (the first stall, no other event),
	// AirportOps.Present.Alerts.DeadlockLookKeepsWatchWhileAnAgentMoves, AirportOps.Present.Alerts.DeadlockLookIgnoresAFieldWithNothingMoving,
	// AirportOps.Present.Alerts.DeadlockLookIsAClockEntryNotAnOfferMinuteTick
	const double Threshold = FMath::Max(Model->Rules.StallSeconds, 0.1);
	double SoonestMotionSeconds = TNumericLimits<double>::Max();
	for (const FRoadAgent& Agent : Model->GetAgents())
	{
		if (Agent.IsOnRoute())
		{
			SoonestMotionSeconds = FMath::Min(SoonestMotionSeconds, Threshold);
		}
		if (!Agent.IsStoppedAndWaiting())
		{
			continue;
		}
		// THE CLOCK READ AS A VALUE, to say how long is left - not compared to a bound (rule 4's 'stall clock compared' is for that).
		const double Stalled = Agent.GetStalledSeconds();
		SoonestMotionSeconds = FMath::Min(SoonestMotionSeconds, Stalled < Threshold ? Threshold - Stalled : Threshold);
	}
	if (SoonestMotionSeconds == TNumericLimits<double>::Max())
	{
		return;
	}
	// IN GAME SECONDS - USimClock::GameSecondsOfMovement (#447), the clock's one conversion between the two time bases: motion runs at the speed
	// multiplier and the game clock at the multiplier times the day's compression, so the ratio is the compression alone - whatever the speed, and a
	// paused clock simply never reaches it. 0.1 OF A MOTION SECOND PAST, so the stall is strictly over the threshold when the look reads it.
	// THE RATE IS THE BAND'S NOW, so the look is capped at the band's edge (USimClock::GameSecondsToBandEdge) and re-booked there at the new
	// rate: booked on the night rate of 75 game s per motion s across dawn, it would fire up to 3.6x late against the day's 21 (the default
	// scenario, 2026-09-30). Floored at half a game second, so a stall a hair short of the threshold cannot book a tight loop.
	const double Period = FMath::Max(FMath::Min(Clock->GameSecondsOfMovement(SoonestMotionSeconds + 0.1), Clock->GameSecondsToBandEdge()), 0.5);
	DeadlockLookHandle = Clock->At(Clock->Now() + Period, [this]()
	{
		DeadlockLookHandle = INDEX_NONE;
		Bus.MarkDirty(TEXT("Alerts"));
	});
}

void UOpsRuntime::OnLandBought(FIntPoint Tile, const FBuildQuote& Quote)
{
	// PRICED BY THE PURSE (OnBuildRefused's reason): the ledger charged UPricing's price for each line, so the event
	// carries that figure - Airside knows only the base amount.
	double Amount = 0.0;
	for (const FBuildLine& Line : Quote.Lines)
	{
		Amount += Pricing->PriceOfBuild(Line.Amount(), Line.Source.Get());
	}
	Bus.Publish(FLandPurchasedEvent{ Tile, Amount });
}

void UOpsRuntime::OnBuildRefused(const FBuildQuote& Quote, EBuildRefusal Why)
{
	// PRICED BY THE PURSE, which is the ledger: Airside knows only the base amount (FBuildQuote's own
	// comment - "what it COSTS is AirportOps' answer").
	Bus.Publish(FBuildRefusedEvent{ Quote.What.ToString(), Why, Ledger->Describe(Quote).ToString(),
		Pricing->Format(Ledger->Balance()).ToString() });
}

void UOpsRuntime::RunArrivalQueue(const FPassRun& Run)
{
	UGroundTraffic* Model = LiveModel();
	if (Model == nullptr)
	{
		return;
	}
	const FQueueTick Result = FlightBoard->TickQueue(*Model, *Target->Network, *Clock);
	if (Result.bDeferred)
	{
		// ONE CLEARANCE A FRAME, ACROSS ROUNDS (the queue's own rule - UFlightBoard::BeginQueueFrame): the Arriving event of the flight
		// just cleared is dispatched in this same drain's next round and dirties the pass again; a second clearance now would be
		// decided in the frame the first was. Deferred, not dropped - and with the SAME CAUSE this run had: a run the net alone asked for
		// that is deferred is still the net's when it finally runs, and an event's is still an event's.
		Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue"), Run.Cause);
		return;
	}
	// THE ALERTS READ WHAT THIS RUN DECIDED (#445 review): FlightCannotLand is derived from the clearances the queue keeps (UFlightBoard::
	// JudgeUnarrived, asked first in TickQueue), and the offer-minute catch-all that used to look again is gone - so a queue run that decided
	// something marks the pass that reads it, itself, rather than relying on whatever else happened to. The alerts pass is declared After this
	// one, so it runs in the same round. Not on a deferral: a deferred tick decided nothing.
	// ENFORCED BY: AirportOps.Present.Alerts.UnlandableAlertSurvivesAPausedEdit
	Bus.MarkDirty(TEXT("Alerts"));
	if (Result.Cleared != nullptr && Run.IsSafetyOnly())
	{
		// THE DEFECT, NAMED: nothing published said this flight could land, yet it could. Whatever freed its
		// runway or stand needs an event of its own.
		UE_LOG(LogOpsBus, Warning, TEXT("safety pass dispatched flight %d (%s) - no event covered it"),
			Result.Cleared->Id, *Result.Cleared->Callsign);
	}
	if (Result.bRetry)
	{
		// A DISPATCH REFUSED in a same-frame race (FArrivalQueue::DispatchNow): retried next frame, never dropped - and a RETRY
		// (EPassCause::Retry, the default of MarkDirtyNextDrain), review M1: the tail of a run something asked for, not the net's own find.
		// ENFORCED BY: AirportOps.Present.ArrivalQueue.RetryStaysCovered
		Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue"));
	}
	// NO NET WHILE CLOSED (whole-stack review I1): a closed airport clears nobody, so a net would tick every 30 s to
	// find the same answer. Reopening is an AirportStatusChanged, which dirties this pass (WireBus), and the run that
	// follows re-arms it if anyone is still holding.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing ("no safety net ticks")
	SafetyNet.Want(TEXT("ArrivalQueue"), !Result.bClosed && Result.Waiting > (Result.Cleared != nullptr ? 1 : 0));
}

TArray<FName> UOpsRuntime::AirsideBridgeNamesForTest()
{
	TArray<FName> Out;
	for (const FAirsideBridge& Bridge : AirsideBridges()) { Out.Add(Bridge.Name); }
	return Out;
}

TArray<UOpsRuntime::FAirsideBridge> UOpsRuntime::AirsideBridges()
{
	// EVERY ENTRY PUBLISHES, NEVER HANDLES (the reason the first one, OnAgentPhase, gave): each of these runs inside UGroundTraffic's Advance - or
	// a release, or a facade's commit - where nothing may act (#193's re-entrancy contract exists because a listener that did could retire any
	// agent mid-loop). The handling is a drain away, in the tier order WireBus makes. The bind lambdas are weak on the runtime: a delegate that
	// outlived it must not call into it. Traffic is asked NULL-SAFELY on the way back: a detach may find the actor without it.
	TArray<FAirsideBridge> Out;

	// THE WHOLE TRANSITION, Cause and GoalAtEvent with it (#436) - so a handler a drain later decides on what was true when the change was made,
	// and the delay this publish introduces costs nothing. (The rule this used to keep by hand - THE SERVICE FIRST, THEN THE BUS - is the tier
	// order in WireBus now: Sim, then Reaction, then Presentation.)
	Out.Add({ TEXT("AgentPhase"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			return Actor.GetTraffic()->OnAgentPhaseChanged.AddWeakLambda(&Runtime,
				[&Runtime](const FAgentTransition& Transition) { Runtime.Bus.Publish(FAgentPhaseEvent{ Transition }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (UAirsideTraffic* Traffic = Actor.GetTraffic()) { Traffic->OnAgentPhaseChanged.Remove(Handle); } } });
	// THE FOUR PURE NOTIFICATIONS ARE BOUND ON THEIR OWNER, the traffic model (#445 item 6): UAirsideTraffic used to re-declare and
	// re-broadcast each from a one-line handler that added nothing, so these bound a relay of a relay. Only AgentPhase stays on the
	// presenter, whose relay adds the view (see UAirsideTraffic::OnAgentPhaseChanged). Asked for through the actor's GetGroundTraffic()
	// at the moment of the bind, and again on the way back, rather than held: the presenter owns which model it has.
	// ENFORCED BY: AirportOps.Present.Bus.ReattachDoesNotDouble (broadcasts each on the model), Check-Architecture rule 87
	Out.Add({ TEXT("ArrivalRefused"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			UGroundTraffic* Model = Actor.GetGroundTraffic();
			return Model == nullptr ? FDelegateHandle() : Model->OnArrivalRefused.AddWeakLambda(&Runtime,
				[&Runtime](EArrivalRefusal Why, const FString& Sentence) { Runtime.Bus.Publish(FArrivalRefusedEvent{ Why, Sentence }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (UGroundTraffic* Model = Actor.GetGroundTraffic()) { Model->OnArrivalRefused.Remove(Handle); } } });

	// AIRSIDE'S DERIVED FREEDOM (ops batch 3 §5) - Airside never learns ops exists; it fires native delegates and these bridge them.
	// ENFORCED BY: Check-Architecture rule 1b (cross-plugin) for "never learns"; AirportOps.Present.Bus.FreedIsBridged for the bridges
	Out.Add({ TEXT("RunwayFreed"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			UGroundTraffic* Model = Actor.GetGroundTraffic();
			return Model == nullptr ? FDelegateHandle() : Model->OnRunwayFreed.AddWeakLambda(&Runtime,
				[&Runtime](FRoadSegmentId Seed) { Runtime.Bus.Publish(FRunwayFreedEvent{ Seed }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (UGroundTraffic* Model = Actor.GetGroundTraffic()) { Model->OnRunwayFreed.Remove(Handle); } } });
	Out.Add({ TEXT("StandsFreed"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			UGroundTraffic* Model = Actor.GetGroundTraffic();
			return Model == nullptr ? FDelegateHandle() : Model->OnStandsFreed.AddWeakLambda(&Runtime,
				[&Runtime](const TArray<FGuidelineNodeId>& PoseNodes) { Runtime.Bus.Publish(FStandsFreedEvent{ PoseNodes }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (UGroundTraffic* Model = Actor.GetGroundTraffic()) { Model->OnStandsFreed.Remove(Handle); } } });
	// ENFORCED BY: AirportOps.Present.Bus.PushGroundFreedIsBridged
	Out.Add({ TEXT("PushGroundFreed"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			UGroundTraffic* Model = Actor.GetGroundTraffic();
			return Model == nullptr ? FDelegateHandle() : Model->OnPushGroundFreed.AddWeakLambda(&Runtime,
				[&Runtime](int32 AgentId) { Runtime.Bus.Publish(FPushGroundFreedEvent{ AgentId }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (UGroundTraffic* Model = Actor.GetGroundTraffic()) { Model->OnPushGroundFreed.Remove(Handle); } } });

	// "THE NETWORK CHANGED", BRIDGED LIKE THE ABOVE (#446) - it was a per-frame poll in Tick. See OnNetworkChanged, which adds behaviour (Geometry is no change).
	Out.Add({ TEXT("NetworkChanged"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor) { return Actor.OnNetworkChanged.AddUObject(&Runtime, &UOpsRuntime::OnNetworkChanged); },
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { Actor.OnNetworkChanged.Remove(Handle); } });

	// THE FACADE'S SILENT REFUSAL, bridged: "cannot afford" at commit (spec 2026-09-29-ops-alerts §2). An actor with no facade bridges nothing.
	Out.Add({ TEXT("BuildRefused"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			URoadEditFacade* Facade = Actor.GetEditFacade();
			return Facade != nullptr ? Facade->OnRefused.AddUObject(&Runtime, &UOpsRuntime::OnBuildRefused) : FDelegateHandle();
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (URoadEditFacade* Facade = Actor.GetEditFacade()) { Facade->OnRefused.Remove(Handle); } } });

	// A LAND PURCHASE, bridged (land purchase spec R10): the facade bought and charged; ops announces it.
	Out.Add({ TEXT("LandBought"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			URoadEditFacade* Facade = Actor.GetEditFacade();
			return Facade != nullptr ? Facade->OnLandBought.AddUObject(&Runtime, &UOpsRuntime::OnLandBought) : FDelegateHandle();
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (URoadEditFacade* Facade = Actor.GetEditFacade()) { Facade->OnLandBought.Remove(Handle); } } });
	return Out;
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

void UOpsRuntime::ApplyScenarioFigures(const UScenario& Scenario)
{
	// THE ROSTER'S RULING, FOR EVERY RECEIVER (#449): a design figure is the scenario's, and a save made before a retune
	// must not carry the old one forward. Every field written here is Transient on its receiver, and this runs at
	// Attach AND after every load - so a load keeps the game (clock time, vehicles, balance, flights) and takes the
	// design from today's asset. It was six copies at Attach into four saved receivers beside one Transient one (the
	// roster's): a retune plus a load priced the depot card's modules from the scenario and its vehicles from the save.
	// The designer figures set from the same asset in the same breath, so none of them is the one somebody forgot to copy.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios (the re-apply), AirportOps.Model.Save.DesignFiguresAreNotSaved (nothing saved)
	Clock->RealSecondsDaylight = Scenario.RealSecondsDaylight;
	Clock->RealSecondsNight = Scenario.RealSecondsNight;
	Clock->DawnHour = Scenario.DawnHour;
	Clock->DuskHour = Scenario.DuskHour;
	// THE VEHICLE CATALOGUE (#430) is joined here, not copied: each row's figures meet Content's chassis for its code
	// once, and a row with no chassis is dropped with a Warning, instead of TypeFor re-assembling it per call from this
	// map and the stand-letter table. HERE, THE ONE CALL SITE, since #449: Transient like every figure in this function,
	// so a load resolves it again from today's scenario - the depot card's vehicles and modules priced by one asset.
	ResolveVehicleCatalogue(*JobBoard, Scenario);
	JobBoard->RefillLitresPerMinutePerPump = Scenario.DepotRefillLitresPerMinutePerPump;
	FacilityPurchases->ModuleOffers = Scenario.ModuleOffers;
	OfferGenerator->MaxPendingOffers = Scenario.MaxPendingOffers;
	Airlines->Tuning = Scenario.AirlineSatisfaction;
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
	// EVERY AIRSIDE DELEGATE, BRIDGED IN ONE LOOP over the table (#445) - see FAirsideBridge. The build-refused bridge binds here too, where it
	// used to bind after the purse: nothing is refused while an attach runs.
	Bridges = AirsideBridges();
	for (FAirsideBridge& Bridge : Bridges)
	{
		Bridge.Handle = Bridge.Bind(*this, *Target);
		// A BRIDGE THAT BINDS NOTHING IS SAID, not silent (#499 review): an invalid handle means the actor lacks what the entry
		// bridges (no facade, no traffic model), and every event of that delegate would then never reach the bus - a deaf board
		// with nothing in the log to say why. A Warning, not a refusal: the rest of the airport still runs.
		if (!Bridge.Handle.IsValid())
		{
			UE_LOG(LogAirportOps, Warning, TEXT("OpsRuntime: the '%s' bridge bound nothing on %s - its Airside events will not reach the ops bus"),
				Bridge.Name, *Target->GetName());
		}
	}

	// Content is resolved ONCE, here, and applied to the clock and the ledger.
	if (Catalog->Num() == 0)
	{
		Catalog->LoadFromAssetManager();
	}
	// Never null - it falls back to UScenario's CDO, so the defaults are actually applied
	// rather than skipped. See ResolveDefaultScenario.
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Catalog))
	{
		// THE DESIGN FIGURES, through ApplyScenarioFigures, which a load runs again (#449) - clock day, the vehicle
		// catalogue and starter fleet, refill, module offers, inbox cap, airline tuning. BEFORE StartAtHour, which reads
		// the day's shape.
		ApplyScenarioFigures(*Scenario);

		// BEFORE THE OFFER SCHEDULE BELOW, and that ordering is load-bearing: Every() books
		// its first firing at Now() + Interval, so moving the clock after scheduling would
		// leave the first offer due at a time that no longer means what it did.
		Clock->StartAtHour(Scenario->StartHour);

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
	// that only the tow was proven to drive. THE DESIGN VEHICLE ONLY since #430: the table was also
	// the vehicle catalogue, and the catalogue is ResolveVehicleCatalogue's now (above).
	// ENFORCED BY: AirportOps.Fuel.RuntimeResolvesPerStand
	JobBoard->ResolveVehicles([](EIcaoCode Letter) { return UAirsideSettings::ResolveStandDesignVehicle(Letter); });
	// AND WHAT EACH STAND WAS BUILT FOR, read off its own definition when the guard asks - see
	// UJobBoard::StandVehiclesOf for why the read is handed down rather than made there.
	// ENFORCED BY: AirportOps.Fuel.RuntimeResolvesPerStand (A sent the truck still reads as tow-built)
	JobBoard->StandVehiclesOf = &UOpsRuntime::StandVehiclesOf;

	// THE PURCHASE SERVICE'S TWO WORLD HOOKS (facility-upgrades spec §3; UJobBoard::StandVehiclesOf's
	// pattern): the plot's ceiling from Airside's one solve, and the module write through the facade's one
	// door - which rebuilds the yard and checkpoints undo. `this` for the ceiling, which the runtime memoises
	// and outlives nothing; weak for the actor, the dispatcher's reason below.
	// ENFORCED BY: AirportOps.Present.Facility.ShedPurchaseRelightsASlot
	FacilityPurchases->ReservedSlotsOf = [this](FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)
	{
		return ReservedSlotsOf(Id, Depot, Module);
	};
	// THE BOARD SEATS A DEPOT'S MODULES AGAINST THE SAME CEILING (#443, ruled 2026-09-30): its pumps are the placed ones,
	// as the shop's bays are. Copied from the shop's, so the two cannot read two plots.
	JobBoard->ModuleCeilingOf = FacilityPurchases->ReservedSlotsOf;
	{
		TWeakObjectPtr<ARoadNetworkActor> WeakActor = Target;
		FacilityPurchases->ApplyModulePurchase = [WeakActor](FEntityInstanceId Id, EDepotModule Module)
		{
			ARoadNetworkActor* Actor = WeakActor.Get();
			URoadEditFacade* Facade = Actor != nullptr ? Actor->GetEditFacade() : nullptr;
			return Facade != nullptr && Facade->AddEntityModule(Id, Module);
		};
		// AND THE REPAIR'S WRITE (#266): unseated modules leave through the facade's door, which rebuilds the yard and
		// checkpoints undo without pushing a step - UFacilityPurchases::RemoveUnseated refunds what this removed.
		// ENFORCED BY: AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated
		FacilityPurchases->ApplyModuleRemoval = [WeakActor](FEntityInstanceId Id, EDepotModule Module, int32 Count)
		{
			ARoadNetworkActor* Actor = WeakActor.Get();
			URoadEditFacade* Facade = Actor != nullptr ? Actor->GetEditFacade() : nullptr;
			return Facade != nullptr ? Facade->RemoveUnseatedModules(Id, Module, Count) : 0;
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

	// THE BUS, per attach, like the scenario figures above - Detach takes it back. ONE LOOP over Publishers() (#445): it was a line
	// per publisher here, a line per publisher in Detach and a list in the test, and the seventh was in none of them. The money these
	// objects post is wired once, in the constructor (#425), and a load no longer overwrites it. AFTER the ledger's Open and the roster's
	// reset above, as each was set after them before: an opening balance posts nothing onto the bus.
	for (const FOpsBusPublisher& Publisher : Publishers())
	{
		*Publisher.Slot = &Bus;
	}
	// A NEW AIRPORT, A NEW SET: the old actor's alerts name its flights and agents.
	Alerts->Reset();
	// A NEW GAME OPENS, beside the ledger's Open and the roster's reset above; a load overwrites the intent from its
	// "Airport" blob. RE-DERIVED SILENTLY - an attach is not a change the player made, and a runway-less new game
	// has nothing to cancel. The first NetworkChanged then finds the status already right.
	// ENFORCED BY: AirportOps.Present.Airport.RunwayComesAndGoes ("the attach re-derives", "publishes no status change")
	Airport->ResetForNewGame();
	if (Target->Network != nullptr)
	{
		Airport->Reseat(*Target->Network);
	}

	// THE LEDGER IS THE PURSE the build tools spend from. Handed to the facade here and
	// nowhere else, so design-time building - which has no runtime and therefore no purse -
	// stays free. See IBuildPurse.
	if (URoadEditFacade* Facade = Target->GetEditFacade())
	{
		Facade->SetPurse(Ledger);
		// (THE FACADE'S SILENT REFUSAL is bridged with the rest, above: the "BuildRefused" entry of AirsideBridges.)
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
	// FIRST DUE AT THE NEXT MIDNIGHT, then a day apart (#442): Every books its first firing a day after NOW, and Now is the last
	// attach or load - so a load at 05:59 pushed the upkeep and FDayEndedEvent to 05:59 the next day, every load moved them
	// again, and a player who reloaded often enough never paid upkeep at all. The day is USimClock::SecondsPerDay game seconds
	// whatever the scenario's real-time day length (ApplyScenarioFigures sets that, and NextDayStart does not read it).
	// ENFORCED BY: AirportOps.Present.Upkeep.PostsAtMidnightAfterALoad, AirportOps.Present.Upkeep.AttachPostsAtTheFirstMidnight
	UpkeepHandle = Clock->EveryFrom(Clock->NextDayStart(), USimClock::SecondsPerDay, [this]() { PostDailyUpkeep(); });

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
		}
	}

	// THE BRIDGES BACK OFF THE ACTOR, one loop (#445) - each entry's own Unbind, null-safe against an actor that has lost what it bound to.
	if (Target != nullptr)
	{
		for (FAirsideBridge& Bridge : Bridges)
		{
			if (Bridge.Handle.IsValid())
			{
				Bridge.Unbind(*Target, Bridge.Handle);
			}
		}
	}
	Bridges.Reset();
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
	SafetyNet.CancelAll();
	if (DeadlockLookHandle != INDEX_NONE)
	{
		Clock->Cancel(DeadlockLookHandle);
		DeadlockLookHandle = INDEX_NONE;
	}
	// NO SeenNetwork RESET any more (#446): the poll it primed for a catch-up event is gone - see the "NetworkChanged" bridge.
	// THE BUS POINTERS GO WITH THE ATTACH: the bus is this runtime's, and a subobject left pointing at it
	// after a detach is a publish into whatever comes next (stage 3 review). Every one Attach set - the same list, one loop.
	// ENFORCED BY: AirportOps.Present.Bus.DetachUnhooksEveryPublisher
	for (const FOpsBusPublisher& Publisher : Publishers())
	{
		*Publisher.Slot = nullptr;
	}
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
	JobBoard->ModuleCeilingOf = nullptr;
	FacilityPurchases->ApplyModulePurchase = nullptr;
	FacilityPurchases->ApplyModuleRemoval = nullptr;
	ReservationMemo.Reset();
	ReservationMemoNetwork.Reset();
	Target = nullptr;
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a fifth EChangeKind must say whether ops hears it.
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UOpsRuntime::OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network)
{
	// THE NETWORK CHANGED, SAID BY THE ACTOR THAT CHANGED IT (#446) - FNetworkChangedEvent's ONE publisher. It
	// was THE NETWORK, COMPARED ONCE A FRAME in Tick: a different object (ClearNetwork, a load) or a new
	// guideline revision. One pointer and one integer, which was the whole cost of the job board no longer
	// re-scanning every depot every frame - but a frame late, which SaveToSlot had to patch over, and blind to a
	// fact edit that re-derived no graph. Every rebuild announces itself now, a new network object's included
	// (ClearNetwork, Undo and a load all end in a Topology rebuild of the network they adopted).
	// Published, not handled: the bus's drain runs the passes, in this frame's Tick or SaveToSlot's own drain.
	// ENFORCED BY: AirportOps.Present.Bus.NetworkChangedPublishedOnceWithNoTick; Check-Architecture rule 51
	switch (Kind)
	{
	case EChangeKind::Geometry:
		// A DRAG FRAME: nothing committed, and the graph a pass would plan over is behind the road until the
		// drag's own Topology rebuild - which does publish. Hearing every frame would re-run the passes each one.
		return;
	case EChangeKind::Markings:
	case EChangeKind::Facts:
	case EChangeKind::Topology:
		Bus.Publish(FNetworkChangedEvent{ Network.GetGuidelineRevision() });
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UOpsRuntime::Tick(double RealDeltaSeconds)
{
	Clock->Advance(RealDeltaSeconds);

	// NO NETWORK POLL HERE since #446 - see OnNetworkChanged, which the actor calls in the rebuild that made
	// the change. ENFORCED BY: Check-Architecture rule 51 (network-change-announced)

	// ONE DRAIN, after the clock: the queue holds Airside's events from the motion tick in publish
	// order, then anything the clock just fired - so a flight that came due this frame is handled
	// this frame (spec 2026-09-29 §1). A NEW QUEUE FRAME first, for the queue's own ONE CLEARANCE A FRAME (#445).
	FlightBoard->BeginQueueFrame();
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
	// THE APRON'S RATE through Content/'s one resolver (#449), not the settings CDO read raw.
	const double Base = BuildCost::DailyUpkeep(*Target->Network,
		UAirsideSettings::ResolveApronRates().UpkeepPerSquareMetrePerDay);
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

TArray<UOpsRuntime::FOpsBusPublisher> UOpsRuntime::Publishers()
{
	// THE ONE LIST (#445), like Persistents(): every object with a raw `FOpsEventBus* Bus`. A class that declares the field and is not
	// here is one that publishes into nothing - or, once attached to a bus that has since gone, into a dead one.
	// ENFORCED BY: Check-Architecture rule 71 (every-bus-publisher-is-listed), AirportOps.Present.Bus.DetachUnhooksEveryPublisher
	TArray<FOpsBusPublisher> Out;
	Out.Add({ TEXT("FlightBoard"), &FlightBoard->Bus });
	Out.Add({ TEXT("Alerts"), &Alerts->Bus });
	Out.Add({ TEXT("Ledger"), &Ledger->Bus });
	Out.Add({ TEXT("JobBoard"), &JobBoard->Bus });
	Out.Add({ TEXT("Airlines"), &Airlines->Bus });
	Out.Add({ TEXT("Airport"), &Airport->Bus });
	Out.Add({ TEXT("FacilityPurchases"), &FacilityPurchases->Bus });
	Out.Add({ TEXT("OfferGenerator"), &OfferGenerator->Bus });
	return Out;
}

TArray<IOpsPersistent*> UOpsRuntime::Persistents() const
{
	// ORDER IS THE SAME ON BOTH SIDES and that is all it has to be: every blob is keyed by its
	// own SaveBlobName, and no object's restore reads a neighbour's. OpsSave::Restore runs each
	// object's OnBeforeRestore, blob and OnAfterRestore in turn (not every OnBeforeRestore first,
	// as this comment used to say); anything that needs two restored systems at once - the
	// loaded clock's Now for #404's re-queue - runs after Restore instead, in UFlightBoard::RestoreAfterLoad, which
	// LoadFromSlot calls once the network is adopted (#426).
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
	// NO STATUS REFRESH OF ITS OWN any more (#446). Review M5 added one because NetworkChanged was published by Tick,
	// a frame late: an edit in the frame of the save would snapshot an airport still open over a network with no
	// runway - and a load re-derives silently, so its flights would never be cancelled. The rebuild that made the edit
	// publishes the event now, so it is already queued, and the drain below runs the Airport handler's Refresh and the
	// cancellation it publishes. PROVABLY NOTHING LOST: the status itself is never saved (only bClosedByPlayer is), so
	// the refresh reached the snapshot only through that drain - and inside a drain, where no drain runs, it reached
	// nothing either way.
	// ENFORCED BY: AirportOps.Present.Airport.SaveRefreshesTheStatus
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
	// THE CASE, NOT A SENTENCE (#445 item 7): "Save to 'X' failed" used to go out as a plain line and show as Info.
	Bus.Publish(FSaveSlotEvent{ bOk ? EOpsSaveOutcome::Saved : EOpsSaveOutcome::SaveFailed, SlotName });
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
		Bus.Publish(FLandRefusedEvent{ EArrivalRefusal::NoRunway, ArrivalPlanner::DescribeRefusal(EArrivalRefusal::NoRunway) });
		return EArrivalRefusal::NoRunway;
	}

	// NO STATUS BRANCHES HERE ANY MORE (#431): a closed airport admits no arrivals, the debug one included (ruling I1,
	// 2026-09-30) - and that is the board's gate now, asked by TryAccept after the plan, so key 7, the inbox and the
	// Land panel refuse a closure with one sentence. This re-checked the status itself because AcceptImmediate reported
	// its refusals by asking the plan again, which a closure passes.
	// ENFORCED BY: AirportOps.Present.Airport.LandRefusedWhileClosed

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
	// to no flight, so nothing would ever give its stand back or know it had landed. AND THROUGH
	// TryAccept (#431): the plan the inbox greys on, so a field with a stand and no exit refuses here too
	// rather than accepting a flight that holds for ever.
	FString Sentence;
	const EArrivalRefusal Why = FlightBoard->AcceptImmediate(*Traffic, *Target->Network, *Clock,
		Airframe, Focus, NSLOCTEXT("AirportOps", "DebugAirline", "(key 7)"), &Sentence);
	if (Why != EArrivalRefusal::None)
	{
		// SAID TO THE PLAYER, not only the log: AcceptImmediate refuses BEFORE any dispatch, so Airside's
		// OnArrivalRefused - the toast key 7 used to rely on - never fires (spec 2026-09-29-ops-alerts §2). WITH THE
		// REFUSAL'S OWN SENTENCE (#456 review): the reason alone reads "not admitted to that runway" for an
		// arrivals-only field, whose real reason is that nothing can take the departure.
		// ENFORCED BY: AirportOps.Present.Alerts.LandRefusalReachesUi
		Bus.Publish(FLandRefusedEvent{ Why, Sentence });
		// The key used to do nothing at all when the airport was full. Now it says which of the
		// refusals it was, in the sentence the inbox would show.
		UE_LOG(LogAirportOps, Warning, TEXT("Land: no flight. %s"), *Sentence);
	}
	return Why;
}

FArrivalQuote UOpsRuntime::QuoteLanding(const FAirframe& Airframe, const FVector2D& Near) const
{
	const UGroundTraffic* Traffic = Target != nullptr ? Target->GetGroundTraffic() : nullptr;
	if (Target == nullptr || Target->Network == nullptr || Traffic == nullptr)
	{
		// LandNear's own refusal for the same fact, worded the same way.
		FArrivalQuote None;
		None.Why = EArrivalRefusal::NoRunway;
		None.Sentence = ArrivalPlanner::DescribeRefusal(EArrivalRefusal::NoRunway);
		return None;
	}
	// THE QUOTE AcceptImmediate's TryAccept will ask - the plan, then the gate - for an arrival that is not a flight yet.
	return FlightBoard->QuoteArrival(*Traffic, *Target->Network, Airframe, Near);
}

bool UOpsRuntime::LoadFromSlot(const FString& SlotName)
{
	// NOT FROM INSIDE A DRAIN (#445), SaveToSlot's guard for the other direction: a Presentation handler bound to a UOpsEvents delegate
	// (a Blueprint "load on this notification") would reach the Discard below while Drain still holds its moved-out batch, and the REST
	// of that batch - events naming the agents and flights of the airport being replaced - would be dispatched against the restored
	// boards. A save can carry on without a drain (it only misses what is queued); a load cannot, so it is REFUSED, said, and the
	// caller may ask again once the drain is over. The guard is for the first caller that does this.
	// ENFORCED BY: AirportOps.Present.Bus.LoadFromAHandlerIsRefused
	if (Bus.IsDraining())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load '%s' refused: called from inside an ops event handler, where discarding the queue would hand the rest of this batch to the restored boards"),
			*SlotName);
		return false;
	}
	// Target->Network is read HERE, not cached: URoadEditFacade::ClearNetwork replaces the
	// actor's network OBJECT rather than draining it, so a pointer held across a clear is
	// stale. The actor's field is the one authority on which network is current.
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load refused: no network attached"));
		return false;
	}
	// THE DOOR THE LOAD GOES THROUGH (RestoreInPlace, below), asked BEFORE anything is torn down: a refusal after
	// ClearAgents, Bus.Discard and Alerts->Reset would have emptied the airport and loaded nothing into it.
	URoadEditFacade* Facade = Target->GetEditFacade();
	if (Facade == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load refused: the network has no edit facade to restore through"));
		return false;
	}
	FOpsSnapshot Snapshot;
	if (!OpsSave::ReadSlot(SlotName, Snapshot))
	{
		Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::NoSave, SlotName });
		return false;
	}
	// Agents first: they were never saved, and one mid-taxi on a network about to be
	// replaced would be following a polyline through pavement that no longer exists. NULL-CHECKED like every other
	// GetTraffic() below (the flight restore's model): an actor with no traffic presenter has no agents to clear.
	if (Target->GetTraffic() != nullptr)
	{
		Target->GetTraffic()->ClearAgents();
	}
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
	// THROUGH URoadEditFacade::RestoreInPlace, the facade's door for a load (issue #426). It announces the
	// replacement (both drivers put the tool down and retire their caches), runs the deserialise below, then EVERY
	// load-time repair a level gets from PostLoad and PostRegisterAllComponents - which a save game does not get,
	// OpsSave::Restore being Serialize alone (final review C2) - clears the undo history (its Mementos are of the
	// airport just replaced on purpose), and adopts the result through OnChanged, so the mesh, the guideline graph and
	// every OnChanged listener (the controller's HasRunway cache, which gates Land) see the loaded airport. This used
	// to be the sequence itself, spelled out here: two of the four repairs, a RebuildMesh no listener heard, and no
	// announcement - see RestoreInPlace for the order and why.
	// ENFORCED BY: AirportMgr.Actions.LoadRetiresTheToolAndCaches, AirportOps.Present.RuntimeLoad.RunsEveryLoadRepair
	const TArray<IOpsPersistent*> Loaded = Persistents();
	const bool bRestored = Facade->RestoreInPlace([&Snapshot, &Loaded](URoadNetwork& Live)
	{
		return OpsSave::Restore(Snapshot, Loaded, Live);
	});
	if (!bRestored)
	{
		return false;
	}
	// THE DESIGN FIGURES ARE TODAY'S SCENARIO'S, NOT THE SAVE'S (#449) - Transient on every receiver, so the restore
	// left them as they were, and this says so explicitly rather than trusting that nothing between changed them. Before
	// SeedAirlines, which seeds new rows at the tuning's start.
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Catalog))
	{
		ApplyScenarioFigures(*Scenario);
	}

	// A SNAPSHOT FROM BEFORE THE "Airlines" BLOB restores no rows (OnBeforeRestore cleared them);
	// every catalog airline comes back at the tuning's start.
	SeedAirlines();

	// THE STATUS RE-DERIVED FROM THE LOADED INTENT AND NETWORK, WITH NO EVENT (spec 2026-09-29-ops-batch3 §3): a
	// published change would reach the flight board's handler and re-run the closure's cancellation - scored, and
	// over every unarrived flight - when everything the closure cancelled was cancelled when this was saved. The
	// generator and the alerts read the result - and so does the flight restore below, which cancels, silently and
	// UNSCORED, what a closed airport can no longer land: the airline did not lose those to the closure but to the save.
	// ENFORCED BY: AirportOps.Present.Airport.LoadRederivesSilently
	Airport->Reseat(*Target->Network);

	// THE FLIGHT HALF OF THE LOAD, in its one order - demote what was mid-flight (#404: agents are never saved),
	// cancel what a closed airport can no longer land, re-hold the stands (the rebuild took every claim), re-arm the
	// arrivals. UFlightBoard owns that order now (issue #426): it was four calls here, each correct only in its position,
	// and FlightSaveTest re-typed them by hand. After the facade's adopt above, which rebuilt the guideline graph the
	// holds are made against.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires, AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport,
	// AirportOps.Present.Airport.ClosedLoadCancelsTheUnarrived
	// A NULL TRAFFIC MODEL still demotes and cancels (steps 1-2 need no model); the board says what it skipped.
	UGroundTraffic* Model = Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr;
	FlightBoard->RestoreAfterLoad(Model, *Target->Network, *Clock, Airport->AdmitsArrivals());
	// THE REPEATERS TOO, from the loaded Now - see RearmRepeatingSchedules (review I1).
	RearmRepeatingSchedules();
	// AND THE SAFETY NET, for every pass that wanted it, for the same reason: its entry was booked against the pre-load clock.
	// Cancelled; the MarkAllDirty below runs the passes, which re-arm it if the loaded queue holds anyone or a departure waits.
	// THE DEADLOCK LOOK LIKEWISE: booked on the old clock, and the alerts run the MarkAllDirty causes books it afresh.
	SafetyNet.CancelAll();
	if (DeadlockLookHandle != INDEX_NONE)
	{
		Clock->Cancel(DeadlockLookHandle);
		DeadlockLookHandle = INDEX_NONE;
	}

	// EVERY PASS ONCE after a load - the one catch-up, since nothing that happened before the load
	// is an event any more (spec 2026-09-29 §4). No passes exist until stage 3; the rule is here first.
	// THE ARRIVAL QUEUE'S among them. DemoteRestoredMidFlight (#404) set a flight Inbound directly until #426, so this
	// run was all that dispatched a re-queued flight; it goes through FArrivalQueue::Enqueue now, whose FlightInbound
	// wakes the pass too - this stays the catch-up for everything that is true of the loaded airport and was never an event.
	Bus.MarkAllDirty();

	ApplySpeed(Clock->GetSpeed());
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Loaded, SlotName });
	return true;
}
