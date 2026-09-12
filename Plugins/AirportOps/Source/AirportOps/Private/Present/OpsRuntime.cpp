#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Content/AirportOpsSettings.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "Model/OfferGenerator.h"
#include "Model/StandAllocator.h"
#include "Model/OpsEvents.h"
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

	FlightBoard->AddOffer(Offer);
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

	// Content is resolved ONCE, here, and applied to the clock. Balance goes to the ledger
	// when it exists (M3); until then the scenario's day length is the only field consumed.
	if (Catalog->Num() == 0)
	{
		Catalog->LoadFromAssetManager();
	}
	// Never null - it falls back to UScenario's CDO, so the defaults are actually applied
	// rather than skipped. See ResolveDefaultScenario.
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario())
	{
		Clock->RealSecondsPerGameDay = Scenario->RealSecondsPerGameDay;

		// BEFORE THE OFFER SCHEDULE BELOW, and that ordering is load-bearing: Every() books
		// its first firing at Now() + Interval, so moving the clock after scheduling would
		// leave the first offer due at a time that no longer means what it did.
		Clock->StartAtHour(Scenario->StartHour);

		// The designer figures set from the same asset in the same breath, so none of them
		// is the one somebody forgot to copy.
		FuelService->DwellSeconds = Scenario->FuelDwellSeconds;
		UE_LOG(LogAirportOps, Log,
			TEXT("Scenario '%s': %.0f real s per game day, starts %02.0f:00, %.0f s fuel dwell"),
			*Scenario->GetName(), Scenario->RealSecondsPerGameDay, Scenario->StartHour,
			Scenario->FuelDwellSeconds);
	}
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
	const double Interval = UOfferGenerator::OfferIntervalSeconds(Airlines);
	if (Interval > 0.0)
	{
		OfferHandle = Clock->Every(Interval, [this]() { GenerateOffer(); });
		UE_LOG(LogAirportOps, Log, TEXT("Offers: %.1f per game day across %d airline(s)"),
			USimClock::SecondsPerDay / Interval, Airlines.Num());
	}
	else
	{
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

	// Offers lapse on the game clock whether or not a network is attached.
	FlightBoard->Tick(*Clock);
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
			FlightBoard->OnAgentPhase(*Model, *Target->Network, AgentId, From, To);
		}
	}

	Events->NotifyAgentPhaseChanged(AgentId, From, To);
}

void UOpsRuntime::OnArrivalRefused(EArrivalRefusal Why)
{
	Events->NotifyArrivalRefused(Why);
}

bool UOpsRuntime::SaveToSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Target->Network, *FlightBoard, Snapshot);
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
	if (!OpsSave::Restore(Snapshot, *Clock, *Target->Network, *FlightBoard))
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
