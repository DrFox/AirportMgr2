#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Content/AirportOpsSettings.h"
#include "Content/AirsideSettings.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/StandAllocator.h"
#include "Model/OpsEvents.h"
#include "Model/Pricing.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditHistory.h"

UOpsRuntime::UOpsRuntime()
{
	Clock = CreateDefaultSubobject<USimClock>(TEXT("Clock"));
	Events = CreateDefaultSubobject<UOpsEvents>(TEXT("Events"));
	Catalog = CreateDefaultSubobject<UOpsCatalog>(TEXT("Catalog"));
	FuelService = CreateDefaultSubobject<UFuelService>(TEXT("FuelService"));

	// GROWS BY FORWARDING, as UFuelService established: the board and the generator are
	// subobjects this class feeds and ticks, and it holds no logic of theirs.
	FlightBoard = CreateDefaultSubobject<UFlightBoard>(TEXT("FlightBoard"));
	FlightBoard->Allocator = CreateDefaultSubobject<UStandAllocator>(TEXT("StandAllocator"));
	OfferGenerator = CreateDefaultSubobject<UOfferGenerator>(TEXT("OfferGenerator"));
	FlightBoard->Generator = OfferGenerator;

	// The money, and the same forwarding shape: this class gains two pointers and a line in
	// Attach, and every decision about what things cost lives in UPricing, not here.
	Ledger = CreateDefaultSubobject<ULedger>(TEXT("Ledger"));
	Pricing = CreateDefaultSubobject<UPricing>(TEXT("Pricing"));
}

TArray<FOfferCandidate> UOpsRuntime::CandidatesFromCatalog() const
{
	// THE CROSSING. Model/ may not read a UAircraftType, so the definitions are flattened
	// into airframes here, where Entities/ is legal - the same division of labour
	// URoadNetwork::PlaceEntity uses for a design wingspan.
	TArray<FOfferCandidate> Out;
	for (const UAirlineDefinition* Airline : Catalog->All<UAirlineDefinition>())
	{
		if (Airline == nullptr)
		{
			continue;
		}
		for (const TObjectPtr<UAircraftType>& Type : Airline->Fleet)
		{
			if (Type == nullptr)
			{
				continue;
			}
			FOfferCandidate Candidate;
			Candidate.Airframe = Type->Airframe();
			Candidate.AirlineName = Airline->DisplayName;
			Candidate.TypeName = Type->DisplayName;
			Out.Add(Candidate);
		}
	}
	return Out;
}

void UOpsRuntime::GenerateOffer()
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		return;
	}

	const TArray<FOfferCandidate> Candidates = CandidatesFromCatalog();
	if (Candidates.Num() == 0)
	{
		// No airlines loaded. Almost always the missing PrimaryAssetTypesToScan line rather
		// than an empty world - see UAirlineDefinition's header.
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

	UFlight* Offer = OfferGenerator->MakeOffer(*Target->Network, FlightBoard->ApproachFocus,
		Candidates, Clock->Now(), FlightBoard->TakeNextId());
	if (Offer == nullptr)
	{
		// MakeOffer has already logged which refusal, and for which aeroplane.
		return;
	}

	FlightBoard->AddOffer(*Clock, Offer);
	UE_LOG(LogAirportOps, Log, TEXT("Offer %d: %s, %s, landing at %.0f"),
		Offer->Id, *Offer->AirlineName.ToString(), *Offer->TypeName.ToString(), Offer->ArrivesAt);
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
	UAirsideTraffic* Traffic = Target->GetTraffic();
	PhaseHandle = Traffic->OnAgentPhaseChanged.AddUObject(this, &UOpsRuntime::OnAgentPhase);
	RefusalHandle = Traffic->OnArrivalRefused.AddUObject(this, &UOpsRuntime::OnArrivalRefused);

	// Content is resolved ONCE, here, and applied to the clock and the ledger.
	if (Catalog->Num() == 0)
	{
		Catalog->LoadFromAssetManager();
	}
	// Never null - it falls back to UScenario's CDO, so the defaults are actually applied
	// rather than skipped. See ResolveDefaultScenario.
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Catalog))
	{
		Clock->RealSecondsPerGameDay = Scenario->RealSecondsPerGameDay;

		// BEFORE THE OFFER SCHEDULE BELOW, and that ordering is load-bearing: Every() books
		// its first firing at Now() + Interval, so moving the clock after scheduling would
		// leave the first offer due at a time that no longer means what it did.
		Clock->StartAtHour(Scenario->StartHour);

		// The designer figures set from the same asset in the same breath, so none of them
		// is the one somebody forgot to copy.
		FuelService->DwellSeconds = Scenario->FuelDwellSeconds;

		// THE BALANCE A NEW GAME OPENS AT. The comment that used to stand at the top of this
		// block said this would happen "when the ledger exists (M3)"; this is that. A LOAD
		// overwrites it moments later from the saved entries, which is why Open is safe here:
		// it is the new-game path, and OpsSave::Restore is the other one.
		Ledger->Open(Scenario->StartingBalance);

		UE_LOG(LogAirportOps, Log,
			TEXT("Scenario '%s': %.0f real s per game day, starts %02.0f:00, %.0f s fuel dwell, opens at %.0f"),
			*Scenario->GetName(), Scenario->RealSecondsPerGameDay, Scenario->StartHour,
			Scenario->FuelDwellSeconds, Scenario->StartingBalance);
	}

	// TruckAirframe resolved HERE, once, not by FuelService at every dispatch (#104): this is
	// Present/, where every other content default gets resolved, and Model/ has no business
	// reaching Content/ for it.
	FuelService->TruckAirframe = UAirsideSettings::ResolveDefaultVehicle();

	// THE MONEY, wired in one breath like the scenario figures above, so none of these is the
	// one somebody forgot to connect. Each of the three posts to the ledger for its own part of
	// a flight: the generator prices the offer, the board banks landing and parking, the fuel
	// service banks a completed fuelling.
	OfferGenerator->Pricing = Pricing;
	FlightBoard->Ledger = Ledger;
	FlightBoard->Pricing = Pricing;
	FuelService->Ledger = Ledger;
	FuelService->Pricing = Pricing;
	// THE ONE PRODUCTION DISPATCHER. Weak, because the board outlives a level change and a
	// captured raw pointer would keep a dead actor alive - or worse, be used.
	TWeakObjectPtr<ARoadNetworkActor> WeakTarget = Target;
	FlightBoard->Dispatcher = [WeakTarget](const FVector2D& Near, const FAirframe& Airframe)
	{
		ARoadNetworkActor* Actor = WeakTarget.Get();
		return Actor != nullptr && Actor->DispatchArrival(Near, Airframe);
	};

	// One repeating offer for the airport as a whole, at the average rate the airlines ask
	// for between them. Per-airline scheduling is a refinement the inbox cannot yet show.
	//
	// FORWARDED: the rate-to-interval arithmetic is UOfferGenerator's now, against
	// USimClock::SecondsPerDay rather than a local figure duplicating it - see
	// UOfferGenerator::OfferIntervalSeconds (issue #98).
	const TArray<UAirlineDefinition*> Airlines = Catalog->All<UAirlineDefinition>();
	const double Interval = UOfferGenerator::OfferIntervalSeconds(Airlines, Pricing->DemandFactor());
	if (Interval > 0.0)
	{
		OfferHandle = Clock->Every(Interval, [this]() { GenerateOffer(); });
		LastOfferIntervalSeconds = Interval;
		UE_LOG(LogAirportOps, Log, TEXT("Offers: %.1f per game day across %d airline(s)"),
			USimClock::SecondsPerDay / Interval, Airlines.Num());
	}
	else
	{
		LastOfferIntervalSeconds = 0.0;
		UE_LOG(LogAirportOps, Warning,
			TEXT("Offers: no airline offers anything, so the inbox will stay empty"));
	}

	ApplySpeed(Clock->GetSpeed());
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntime attached to %s"), *Target->GetName());
}

void UOpsRuntime::Detach()
{
	if (Target != nullptr && Target->GetTraffic() != nullptr)
	{
		Target->GetTraffic()->OnAgentPhaseChanged.Remove(PhaseHandle);
		Target->GetTraffic()->OnArrivalRefused.Remove(RefusalHandle);
	}
	if (OfferHandle != INDEX_NONE)
	{
		Clock->Cancel(OfferHandle);
		OfferHandle = INDEX_NONE;
		LastOfferIntervalSeconds = 0.0;
	}
	// Cleared rather than left pointing at the old actor: a dispatcher that still answers
	// after a detach would put an aeroplane on a field this runtime no longer drives.
	FlightBoard->Dispatcher = nullptr;
	Target = nullptr;
}

void UOpsRuntime::Tick(double RealDeltaSeconds)
{
	Clock->Advance(RealDeltaSeconds);
	if (Target != nullptr)
	{
		// The MULTIPLIER, not TimeScale(): movement runs at the player's speed setting,
		// never at the day compression. See USimClock's class comment.
		Target->SetSimTimeScale(USimClock::Multiplier(Clock->GetSpeed()));

		// THE NETWORK IS READ FRESH, never cached: URoadEditFacade::ClearNetwork replaces the
		// actor's network OBJECT rather than draining it, so a pointer held across a clear is
		// stale - the same reason LoadFromSlot re-reads it.
		//
		// AND NOTHING IS SCALED HERE. The fuel service's own clock is
		// UGroundTraffic::GetSimSeconds, which the actor's tick has already advanced by the
		// speed multiplier; scaling again would run the dwell at the square of the player's
		// speed setting.
		if (Target->Network != nullptr && Target->GetTraffic() != nullptr)
		{
			if (UGroundTraffic* Model = Target->GetTraffic()->GetModel())
			{
				FuelService->Tick(*Model, *Target->Network, *Clock);
			}
		}
	}

	// Offers used to lapse here via UFlightBoard::Tick's per-frame poll (issue #105 item 9);
	// now Clock->Advance above already fired any expiry due this frame - see
	// UFlightBoard::ScheduleExpiry, armed from AddOffer and re-armed by RearmSchedules.
}

void UOpsRuntime::ApplySpeed(ESimSpeed Speed)
{
	Clock->SetSpeed(Speed);
	if (Target != nullptr)
	{
		Target->SetSimTimeScale(USimClock::Multiplier(Speed));
	}
	Events->NotifySpeedChanged(Speed);
}

TArrayView<const ESimSpeed> UOpsRuntime::SpeedLadder()
{
	// A SECOND LIST THAT MUST AGREE WITH ESimSpeed, and exactly the kind CLAUDE.md names.
	// A speed added to the enum but not to this ladder compiles, runs, and is simply
	// unreachable: the player presses "faster" at the top rung and nothing happens, with
	// no error anywhere. It is exposed rather than a static local precisely so a test can
	// read it - AirportOps.Model.SimClock.SpeedLadderCoversEveryRung walks StaticEnum and
	// fails if the two ever drift apart.
	static const ESimSpeed Ladder[] = {
		ESimSpeed::X1, ESimSpeed::X2, ESimSpeed::X4, ESimSpeed::X8, ESimSpeed::X16, ESimSpeed::X32 };
	return MakeArrayView(Ladder, UE_ARRAY_COUNT(Ladder));
}

void UOpsRuntime::StepSpeed(int32 Delta)
{
	const TArrayView<const ESimSpeed> Ladder = SpeedLadder();
	const int32 Rungs = Ladder.Num();

	// Stepping while paused steps from ResumeSpeed, which is what a player pressing
	// "faster" while paused means: resume, one notch up from where they were.
	const ESimSpeed From = Clock->GetSpeed() == ESimSpeed::Paused ? ResumeSpeed : Clock->GetSpeed();
	int32 Index = 0;
	for (int32 I = 0; I < Rungs; ++I)
	{
		if (Ladder[I] == From) { Index = I; }
	}
	Index = FMath::Clamp(Index + Delta, 0, Rungs - 1);
	ResumeSpeed = Ladder[Index];
	ApplySpeed(ResumeSpeed);
}

void UOpsRuntime::TogglePause()
{
	if (Clock->GetSpeed() == ESimSpeed::Paused)
	{
		ApplySpeed(ResumeSpeed);
	}
	else
	{
		ResumeSpeed = Clock->GetSpeed();
		ApplySpeed(ESimSpeed::Paused);
	}
}

void UOpsRuntime::OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// THE SERVICE FIRST, THEN THE BUS. A Blueprint listener that asked the fuel service what
	// an aircraft was doing would otherwise see the state from BEFORE the event it was woken
	// by - one frame stale, and only sometimes, which is the worst kind.
	if (Target != nullptr && Target->Network != nullptr && Target->GetTraffic() != nullptr)
	{
		if (UGroundTraffic* Model = Target->GetTraffic()->GetModel())
		{
			FuelService->OnAgentPhase(*Model, *Target->Network, *Clock, AgentId, From, To);
			FlightBoard->OnAgentPhase(*Model, *Target->Network, *Clock, AgentId, From, To);
		}
	}

	Events->NotifyAgentPhaseChanged(AgentId, From, To);
}

void UOpsRuntime::OnArrivalRefused(EArrivalRefusal Why)
{
	Events->NotifyArrivalRefused(Why);
}

TArray<IOpsPersistent*> UOpsRuntime::Persistents() const
{
	// ORDER IS THE SAME ON BOTH SIDES and that is all it has to be: every blob is keyed by its
	// own SaveBlobName, and OnBeforeRestore runs for all of them before any is deserialised, so
	// nothing here depends on a neighbour having been restored first.
	TArray<IOpsPersistent*> Out;
	Out.Add(Clock);
	Out.Add(FuelService);
	Out.Add(FlightBoard);
	Out.Add(Ledger);
	Out.Add(Pricing);
	return Out;
}

bool UOpsRuntime::SaveToSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	const TArray<IOpsPersistent*> Saved = Persistents();
	OpsSave::Capture(Saved, *Target->Network, Snapshot);
	const bool bOk = OpsSave::WriteSlot(SlotName, Snapshot);
	Events->NotifyNotification(bOk ? FString::Printf(TEXT("Saved '%s'"), *SlotName)
	                               : FString::Printf(TEXT("Save to '%s' failed"), *SlotName));
	return bOk;
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
		Events->NotifyNotification(FString::Printf(TEXT("No save '%s'"), *SlotName));
		return false;
	}
	// Agents first: they were never saved, and one mid-taxi on a network about to be
	// replaced would be following a polyline through pavement that no longer exists.
	Target->GetTraffic()->ClearAgents();
	const TArray<IOpsPersistent*> Loaded = Persistents();
	if (!OpsSave::Restore(Snapshot, Loaded, *Target->Network))
	{
		return false;
	}
	// The undo stack holds Mementos of the PRE-load network; an undo now would revert the
	// player to an airport they just replaced on purpose. A load is a new baseline.
	if (Target->History != nullptr)
	{
		Target->History->Clear();
	}
	// Present rebuilds from model: the mesh and the derived guideline graph are both
	// produced by the presenter's Rebuild, which is what RebuildMesh runs.
	Target->RebuildMesh();

	// IN THIS ORDER, and both are needed. RebuildMesh regenerates the guideline graph, which
	// takes every node claim with it (FTrafficOccupancy::ReleaseGuidelineClaims), so the
	// restored flights' stand holds have to be re-made against the new nodes BEFORE anything
	// can allocate. Then the arrivals go back on the clock, whose queue was never saved.
	if (UGroundTraffic* Model = Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr)
	{
		FlightBoard->OnGraphRebuilt(*Model, *Target->Network);
		FlightBoard->RearmSchedules(*Model, *Target->Network, *Clock);
	}

	ApplySpeed(Clock->GetSpeed());
	Events->NotifyNotification(FString::Printf(TEXT("Loaded '%s'"), *SlotName));
	return true;
}
