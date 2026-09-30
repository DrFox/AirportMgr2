#include "Model/JobBoard.h"

#include "Model/Flight.h"
#include "Model/GameTimeText.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsNames.h"
#include "Model/Pricing.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/SimClock.h"
#include "Model/VehicleFit.h"
#include "Solve/StandBox.h"

namespace JobBoardText
{
	const TCHAR* StateName(EServiceVehicleState State)
	{
		switch (State)
		{
		case EServiceVehicleState::Idle:       return TEXT("Idle");
		case EServiceVehicleState::ToJob:      return TEXT("ToJob");
		case EServiceVehicleState::Serving:    return TEXT("Serving");
		case EServiceVehicleState::ToFacility: return TEXT("ToFacility");
		case EServiceVehicleState::AtFacility: return TEXT("AtFacility");
		case EServiceVehicleState::Deciding:   return TEXT("Deciding");
		default:                               return TEXT("?");
		}
	}
}

// The literal VehiclesByLetter[6] in the header, which UHT has to parse, against the enum.
static_assert(static_cast<int32>(EIcaoCode::F) + 1 == UJobBoard::LetterCount,
	"one vehicle table entry per ICAO letter");

UJobBoard::UJobBoard()
	: FuelPolicy(MakeShared<FFuelRolePolicy>())
{
	Policies.Add(EServiceRole::Fuel, FuelPolicy);
}

void UJobBoard::OnBeforeRestore()
{
	++RevisionCount;   // See Revision: a load clears the jobs and turnarounds (the fleet's own clear moves FleetRevision).
	Jobs.Reset();
	Turnarounds.Reset();
	// THE FLEET AND THE SEEN-DEPOT SET, through the door that owns them: cleared, then restored from the blob (it is
	// saved), and a snapshot without one re-seeds each depot once, which is the old behaviour.
	Fleet().Clear();
}

void UJobBoard::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	if (!Ar.IsLoading())
	{
		return;
	}
	// A LOAD'S OWN LINE (#458's rule 41 asks every persistent object with a Revision to bump on IsLoading): the vehicles
	// were replaced - Restored moves FleetRevision as well, so this is the redundant half, kept because a load is a change
	// whatever the archive held and the rule reads the declaration, not which counter the body moves.
	++RevisionCount;
	for (FServiceVehicle& Vehicle : Vehicles)
	{
		// EVERY VEHICLE IDLE AT HOME with nothing held - agents, and the jobs of aircraft that are not restored,
		// are things a load clears. The lifecycle moves FleetRevision (once per vehicle).
		Lifecycle(Vehicle).ResetForRestore();
	}
	// THE ARCHIVE REPLACED THE VEHICLES - even with none: each restored vehicle's depot has been seen (the placeholder must
	// not add a second fleet beside a restored one), and the composition has changed, which the fleet's door records.
	Fleet().Restored();
	if (Vehicles.Num() > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Restore: %d vehicle(s) idle at home"), Vehicles.Num());
	}
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a refusal added to EServiceRefusal must say what the player is
// told. It had a `default:` that answered "unserviceable" for every value it did not name, so a new reason silently read as
// the bare word and the player was sent nowhere - the thing this enum exists to avoid. ENFORCED BY: the build, checked by
// adding a stray enumerator to EServiceRefusal and watching it fail here (2026-09-30).
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
const TCHAR* UJobBoard::RefusalText(EServiceRefusal Why)
{
	switch (Why)
	{
	// NOT REFUSED: there is no reason to give, and the bare word is what this has always answered for it.
	case EServiceRefusal::None:          return TEXT("unserviceable");
	case EServiceRefusal::NoDepot:       return TEXT("no fuel depot");
	case EServiceRefusal::NoRoad:        return TEXT("depot not on a road");
	// THE ENTRANCES, NAMED, since 2026-09-16 - the message the stand-routing spec promised and the
	// only one of the four that can say something more useful than the bare fact. A stand's lane
	// declares its entrances and a road reaches them or does not, so "not on a road" was true and sent
	// the player looking at the stand's own sides, where there is nothing to draw: the road has to
	// reach an ENTRANCE, which on a Code C is a corner of the nose or tail crossing. How many there are
	// is per-definition, so the count is not in the text - a number that has to agree with an asset is
	// a number that drifts.
	case EServiceRefusal::StandUnjoined: return TEXT("no road within reach of the stand's entrances");
	case EServiceRefusal::NoRoute:       return TEXT("no road from depot");
	case EServiceRefusal::TooNarrow:     return TEXT("no road wide enough for the fuel vehicle");
	case EServiceRefusal::NoPump:        return TEXT("depot has no pump");
	// THE VEHICLE, NOT THE ROAD OR THE STAND: the fix is a depot with a smaller vehicle, which is why
	// this does not share TooNarrow's text.
	case EServiceRefusal::VehicleTooLarge: return TEXT("the depot's vehicle is too large for this stand");
	// THE FIX, NAMED: the depot card has the buy button (facility-upgrades spec §4).
	case EServiceRefusal::NoVehicles:    return TEXT("depot has no vehicles - buy one");
	// NOT "buy one": the card lists the vehicle, and it is the listed one that cannot be used (#478) - sell it, buy a kind
	// the scenario has.
	case EServiceRefusal::UnknownVehicleKind: return TEXT("the depot's vehicles are of a kind this scenario no longer has - sell them and buy new");
	}
	// A VALUE OUTSIDE THE ENUM (a corrupt save's Why): not one of the cases above, and still worded.
	return TEXT("unserviceable");
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UJobBoard::ResolveVehicles(TFunctionRef<FVehicle(EIcaoCode)> Resolve)
{
	// NO BUMP (#443, "changes, not calls"): it fills the letter table, which no job, vehicle or turnaround holds and no
	// Describe reads - the vehicle line names a kind, not its chassis. It used to bump as "every public mutator".
	for (int32 Index = 0; Index < LetterCount; ++Index)
	{
		VehiclesByLetter[Index] = Resolve(static_cast<EIcaoCode>(Index));
	}
}

EIcaoCode UJobBoard::LetterOfStand(const FEntityInstance& Stand)
{
	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Stand.Outline);
	return Letter.IsSet() ? *Letter : EIcaoCode::C;
}

FVehicle UJobBoard::VehicleFor(const FEntityInstance& Stand) const
{
	return VehiclesFor(LetterOfStand(Stand));
}

FVehicle UJobBoard::DesignVehicleFor(const FEntityInstance& Stand) const
{
	return DesignVehicleOf ? DesignVehicleOf(Stand) : VehicleFor(Stand);
}

bool UJobBoard::AnyStandAdmits(const FVehicle& Kind, const URoadNetwork& Network) const
{
	// THE LETTERS FIRST: a stand with no authored design vehicle is built for its letter's, and there are six entries and no
	// network walk to read them - so a kind any letter admits never pays for the placed stands.
	for (const FVehicle& Design : VehiclesByLetter)
	{
		if (VehicleFit::NoLargerThan(Kind, Design))
		{
			return true;
		}
	}
	// THEN EACH LIVE STAND'S OWN, which a definition may author bigger than its letter's (UEntityDefinition::DesignVehicle):
	// a kind one placed stand admits is never refused, whatever the letter table says. The same ceiling Judge applies.
	for (const FEntityInstance& Stand : Network.GetEntities())
	{
		if (Stand.bAlive && Stand.IsStandCandidate() && VehicleFit::NoLargerThan(Kind, DesignVehicleFor(Stand)))
		{
			return true;
		}
	}
	return false;
}

FServiceVehicleType UJobBoard::TypeFor(FName TypeCode) const
{
	// THE CATALOGUE'S ROW, joined at attach and after every load (#430, #449). It WAS joined here on every call: the chassis from the first
	// stand letter that sent the code ("the table is the catalogue until the fleet is bought", spec §3.4 - and the fleet
	// is bought since #417) and the figures from the scenario map, so a kind no letter was designed for came back with a
	// zero chassis, and a test that widened a letter's vehicle widened this one.
	if (const FServiceVehicleType* Row = Catalogue.Find(TypeCode))
	{
		return *Row;
	}
	// NO ROW: a None code and an empty chassis, which the bid's candidate filter skips, rather than the trailer's figures on
	// a zero-size vehicle that every fit gate passed. ONCE PER CODE, where the designer will look - and not for a bare board
	// whose catalogue nobody resolved, which is a fixture's business, as the old fallback's warning was not either.
	if (Catalogue.Num() > 0 && !WarnedTypes.Contains(TypeCode))
	{
		WarnedTypes.Add(TypeCode);
		UE_LOG(LogAirportOps, Warning, TEXT("Fleet: vehicle kind %s has no catalogue row (no scenario figures, or no chassis); it serves nothing"),
			*TypeCode.ToString());
	}
	return FServiceVehicleType();
}

// HasWorkingPump AND PumpsAt ARE FDepotCapability'S NOW (#443, ruled 2026-09-30). They were a walk of the OWNED module
// list here, beside a second rule for bays in UFacilityPurchases and a third in the depot census, so a module the presenter
// could not seat still granted a pump. The legacy reasoning moved with the rule: NO MODULES IS NOT "NO PUMP". A depot
// placed without a plot - every depot in every save written before plots existed, and every one a test places through
// the old signature - fuels exactly as it always did, and answering false for it would break the fuel loop for all of
// them at once, which is how a feature nobody asked about stops an existing one. A plotless depot counts as one pump.

void UJobBoard::PostServiceFee(double Now, double Litres)
{
	if (Ledger == nullptr || Pricing == nullptr)
	{
		return;
	}
	const double Fee = Pricing->FuelFee(Litres);
	if (Fee <= 0.0)
	{
		return;
	}
	Ledger->Post(Now, ELedgerCategory::ServiceFee, Fee, NSLOCTEXT("Ledger", "Fuelling", "Fuelling"));
}

const IServiceRolePolicy* UJobBoard::PolicyFor(EServiceRole Role) const
{
	// THE BOARD'S FIGURE, pushed in every time rather than copied once: a test or the scenario sets
	// RefillLitresPerMinutePerPump on the board, and the rule that reads it lives in the policy.
	FuelPolicy->RefillLitresPerMinutePerPump = RefillLitresPerMinutePerPump;
	const TSharedRef<IServiceRolePolicy>* Found = Policies.Find(Role);
	return Found != nullptr ? &Found->Get() : nullptr;
}

FServiceJob* UJobBoard::FindJob(int32 JobId)
{
	return JobId == 0 ? nullptr : Jobs.FindByPredicate([JobId](const FServiceJob& Job) { return Job.Id == JobId; });
}

const FServiceJob* UJobBoard::FindJob(int32 JobId) const
{
	return JobId == 0 ? nullptr : Jobs.FindByPredicate([JobId](const FServiceJob& Job) { return Job.Id == JobId; });
}

FServiceVehicle* UJobBoard::FindVehicleMutable(int32 VehicleId)
{
	return VehicleId == 0 ? nullptr
		: Vehicles.FindByPredicate([VehicleId](const FServiceVehicle& Vehicle) { return Vehicle.Id == VehicleId; });
}

const FServiceVehicle* UJobBoard::FindVehicle(int32 VehicleId) const
{
	return VehicleId == 0 ? nullptr
		: Vehicles.FindByPredicate([VehicleId](const FServiceVehicle& Vehicle) { return Vehicle.Id == VehicleId; });
}

const FServiceVehicle* UJobBoard::VehicleForAgent(int32 AgentId) const
{
	return AgentId == 0 ? nullptr
		: Vehicles.FindByPredicate([AgentId](const FServiceVehicle& Vehicle) { return Vehicle.AgentId == AgentId; });
}

FTurnaround* UJobBoard::FindTurnaround(int32 AircraftId)
{
	return Turnarounds.FindByPredicate([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });
}

const FTurnaround* UJobBoard::TurnaroundFor(int32 AircraftId) const
{
	return Turnarounds.FindByPredicate([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });
}

const FServiceJob* UJobBoard::JobForAircraft(int32 AircraftId, EServiceRole Role) const
{
	return Jobs.FindByPredicate([AircraftId, Role](const FServiceJob& Job)
		{
			return Job.AircraftId == AircraftId && Job.Role == Role;
		});
}

int32 UJobBoard::AgentForJob(const FServiceJob& Job) const
{
	const FServiceVehicle* Vehicle = FindVehicle(Job.VehicleId);
	return Vehicle != nullptr && Vehicle->CurrentJob == Job.Id ? Vehicle->AgentId : 0;
}

int32 UJobBoard::TrucksGoingHomeForTest() const
{
	int32 Count = 0;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		Count += Vehicle.State == EServiceVehicleState::ToFacility ? 1 : 0;
	}
	return Count;
}

int32 UJobBoard::TrucksOutForTest(FEntityInstanceId Depot) const
{
	int32 Count = 0;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		Count += (Vehicle.Home == Depot && Vehicle.State != EServiceVehicleState::Idle) ? 1 : 0;
	}
	return Count;
}

int32 UJobBoard::RefillingForTest() const
{
	int32 Count = 0;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		Count += Vehicle.State == EServiceVehicleState::AtFacility ? 1 : 0;
	}
	return Count;
}

FServiceVehicle& UJobBoard::AddVehicleForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo)
{
	// THROUGH THE FLEET'S HANDLE, so a staged vehicle is made by the same creation site as a bought one: the door moves
	// both counters, and Revision (which sums FleetRevision) with them.
	return Fleet().AddForTest(TypeCode, Home, State, Cargo);
}

void UJobBoard::ReopenRefusedJob(FServiceJob& Job)
{
	// A CHANGE TO A JOB WITH NO VEHICLE TRANSITION BEHIND IT, so it moves RevisionCount itself (see Revision).
	++RevisionCount;
	Job.State = EServiceJobState::Open;
	Job.Why = EServiceRefusal::None;
}

bool UJobBoard::CanRemoveVehicle(int32 VehicleId) const
{
	const FServiceVehicle* Vehicle = FindVehicle(VehicleId);
	return Vehicle != nullptr && Vehicle->State == EServiceVehicleState::Idle && Vehicle->AgentId == 0
		&& Vehicle->CurrentJob == 0 && Vehicle->Queue.Num() == 0;
}

int32 UJobBoard::VehiclesAt(FEntityInstanceId Depot) const
{
	int32 Count = 0;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		Count += Vehicle.Home == Depot ? 1 : 0;
	}
	return Count;
}

FString UJobBoard::VehicleLine(const FServiceVehicle& Vehicle, const URoadNetwork* Network) const
{
	const FString Dot = TEXT(" · ");
	// THE KIND'S NAME, the one the shop sold it under (#430) - not its code: the card listed "FUEL #7" beside a "Bowser".
	return FString::Printf(TEXT("%s #%d"), *FServiceFleet::NameOf(*this, Vehicle.TypeCode).ToString(), Vehicle.Id) + Dot + VehicleDoing(Vehicle, Network)
		+ Dot + FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
}

FServiceJob& UJobBoard::AddJobForTest(int32 AircraftId, EServiceJobState State, EServiceRefusal Why, uint32 RefusedAtRevision)
{
	++RevisionCount;   // See Revision: every public mutator.
	FServiceJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = NextJobId++;
	Job.AircraftId = AircraftId;
	Job.State = State;
	Job.Why = Why;
	Job.RefusedAtRevision = RefusedAtRevision;
	return Job;
}

FGuidelineNodeId UJobBoard::ServiceAnchorOf(const URoadNetwork& Network, FEntityInstanceId Stand, EServiceRole Role)
{
	// BY ROLE, THEN BY ID, AND THE FIRST ONE ONLY (issue #190). Role is a category - a stand may one
	// day have two hydrants - so this answers "where can the service be worked" and takes the first;
	// never by array position, which is the invariant FResolvedAnchor exists to remove.
	// FirstAnchorIdForRole, not GetAnchorIdsForRole: this runs for every bid of every job, and
	// building a whole TArray<FName> just to read element 0 was the busy-wait's own share of the
	// allocation #190 found.
	const FName AnchorId = Network.FirstAnchorIdForRole(Stand, Role);
	if (AnchorId.IsNone())
	{
		return FGuidelineNodeId();
	}
	if (const FResolvedAnchor* Resolved = Network.FindResolvedAnchor(Stand, AnchorId))
	{
		return Resolved->Node;
	}
	return FGuidelineNodeId();
}

FGuidelineNodeId UJobBoard::HomePose(const URoadNetwork& Network, const FServiceVehicle& Vehicle)
{
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	return Home != nullptr && Home->bAlive ? Home->PoseNode : FGuidelineNodeId();
}

void UJobBoard::Reopen(FServiceJob& Job)
{
	++RevisionCount;   // See Revision: a job changed, whether or not a vehicle transition follows.
	Job.State = EServiceJobState::Open;
	Job.VehicleId = 0;
	Job.TripQuantity = 0.0;
}

bool UJobBoard::IsStranded(const FServiceVehicle& Vehicle, const UGroundTraffic& Traffic)
{
	const FRoadAgent* Agent = Vehicle.AgentId != 0 ? Traffic.FindAgent(Vehicle.AgentId) : nullptr;
	return Agent != nullptr && Agent->Phase == EAgentPhase::Stranded;
}

int32 UJobBoard::ReleaseJobsOf(FServiceVehicle& Vehicle)
{
	int32 Released = 0;
	for (FServiceJob& Job : Jobs)
	{
		if (Job.VehicleId == Vehicle.Id && Job.State != EServiceJobState::Done)
		{
			Reopen(Job);
			++Released;
		}
	}
	Lifecycle(Vehicle).ReleaseCurrentJob();
	Vehicle.Queue.Reset();
	return Released;
}

void UJobBoard::RetireAgentOf(FServiceVehicle& Vehicle, UGroundTraffic& Traffic)
{
	const int32 AgentId = Vehicle.AgentId;
	// A STRANDED VEHICLE LEAVING THE ROAD IS IT COUNTING AGAIN (Idle at home, it bids): asked BEFORE the unhook, which is
	// when the agent still says so, and said by the composition counter because the Gone this retire publishes finds no
	// vehicle with that agent (OnAgentPhase's own stranded-exit bump cannot see it).
	// ENFORCED BY: AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict (the despawn row)
	if (IsStranded(Vehicle, Traffic))
	{
		++FleetCompositionRevision;
	}
	Lifecycle(Vehicle).LeaveRoad();
	// RetireAgent's bool is deliberately dropped: false means the agent was already gone, which is the state this
	// wants - the vehicle is unhooked either way, and a missing agent is not an error here.
	Traffic.RetireAgent(AgentId);
}

int32 UJobBoard::LoseAgent(FServiceVehicle& Vehicle)
{
	// IDEMPOTENT HERE, not only in its callers: a vehicle with no agent has nothing to lose (the Gone event for an
	// agent the board already unhooked finds no vehicle at all, but a caller holding the vehicle could still ask).
	if (Vehicle.AgentId == 0)
	{
		return 0;
	}
	const int32 Released = ReleaseJobsOf(Vehicle);
	Lifecycle(Vehicle).LeaveRoad();
	return Released;
}

bool UJobBoard::RecallVehicleOfAgent(int32 AgentId, bool bRetire, UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	// NO BUMP ON ENTRY (#443): a recall of an agent that drives no vehicle of this board changes nothing. One that does
	// releases its jobs (Reopen) and moves the vehicle (a transition), and both move Revision where they happen.
	const FServiceVehicle* Found = AgentId != 0 ? VehicleForAgent(AgentId) : nullptr;
	FServiceVehicle* Vehicle = Found != nullptr ? FindVehicleMutable(Found->Id) : nullptr;
	if (Vehicle == nullptr)
	{
		return false;
	}
	const int32 Released = ReleaseJobsOf(*Vehicle);
	if (bRetire)
	{
		// UNHOOKED, THEN RETIRED (RetireAgentOf): RetireAgent's Gone broadcast reaches OnAgentPhase, which now
		// treats a Gone for a vehicle's agent as the agent being lost - and the vehicle is already home.
		RetireAgentOf(*Vehicle, Traffic);
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d (agent %d) despawned by the player; %d job(s) back to the board, Idle at depot %d"),
			Vehicle->Id, AgentId, Released, Vehicle->Home.Index);
		return true;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d (agent %d) sent home by the player; %d job(s) back to the board"),
		Vehicle->Id, AgentId, Released);
	GoToFacility(*Vehicle, Traffic, Network, Clock);
	return true;
}

int32 UJobBoard::SeedStarterFleets(const URoadNetwork& Network, const USimClock& Clock)
{
	// NEW DEPOTS GET THEIR PLACEHOLDER FLEET (spec §3.4): Trucks of every kind in StarterFleet, Idle and full - through the
	// fleet's door (#443), so a seeded vehicle is announced and re-opens refused jobs like any other, and CouldServe can
	// read real vehicles instead of predicting these. RUN BY THE "FleetSeed" PASS, NOT BY STEP (#443): SyncFleet used to
	// open with this, so every job board pass walked every entity to find a depot it had not seen, although placement is
	// announced (FNetworkChangedEvent) - see the declaration for why the announcement and not the facade's placement path.
	// ENFORCED BY: Check-Architecture rule 60 (starter-fleet-seeded-on-announcement: SyncFleet and Step do not seed)
	return Fleet().SeedStarterFleets(Network, Clock.Now());
}

void UJobBoard::SyncFleet(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// VEHICLES WHOSE DEPOT IS GONE are withdrawn, and their jobs go back to the board - which will say
	// "no fuel depot" if that was the only one, the reason the player needs.
	for (int32 Index = Vehicles.Num() - 1; Index >= 0; --Index)
	{
		FServiceVehicle& Vehicle = Vehicles[Index];
		const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
		if (Home != nullptr && Home->bAlive)
		{
			// AN AGENT THAT VANISHED UNDER IT - retired by somebody else, a cleared traffic model. The
			// vehicle is not lost with it: it is back at home, and every job it held goes back to the board.
			// THE NET, NOT THE CHANNEL: OnAgentPhase's Gone branch hears of this from the traffic model's own
			// event and takes the same body (LoseAgent), so this fires only for a loss no event reached - a Gone
			// delivered while no live model was attached, a model swapped under the board. It used to be the ONLY
			// way the board heard, polled per vehicle per Step, and re-opened just the current job.
			// ENFORCED BY: AirportOps.Fuel.Lifecycle.LostAgentNetRecallsTheSameWay (the log line's job count: both jobs, not one)
			if (Vehicle.AgentId != 0 && Traffic.FindAgent(Vehicle.AgentId) == nullptr)
			{
				const int32 LostAgent = Vehicle.AgentId;
				const int32 Released = LoseAgent(Vehicle);
				UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %d lost its agent %d with no Gone heard; %d job(s) back to the board, Idle at depot %d"),
					Vehicle.Id, LostAgent, Released, Vehicle.Home.Index);
			}
			continue;
		}
		const int32 Reopened = ReleaseJobsOf(Vehicle);
		if (Vehicle.AgentId != 0)
		{
			RetireAgentOf(Vehicle, Traffic);
		}
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d removed; vehicle %d %s withdrawn, %d job(s) back to the board"),
			Vehicle.Home.Index, Vehicle.Id, *Vehicle.TypeCode.ToString(), Reopened);
		// THE FLEET'S DOOR takes it from here: it credits the vehicle's resale value as a sale would be, announces the
		// withdrawal and removes it (ruled 2026-09-30). The job board used to post that money itself, in its own wording,
		// and tell nobody. Withdraw unhooks nothing - the jobs and the agent went just above, which is this function's.
		// ENFORCED BY: AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles, AirportOps.Model.Fleet.DepotRemovalPublishesFleetChanged
		const int32 WithdrawnId = Vehicle.Id;
		if (!Fleet().Withdraw(WithdrawnId, EFleetReason::DepotRemoved, Clock.Now()))
		{
			// NOT EXPECTED - the vehicle was just read off this array - but a vehicle left behind would be retried and
			// re-released every Step, so it is said rather than assumed away.
			UE_LOG(LogAirportOps, Warning, TEXT("Fleet: vehicle %d of a removed depot could not be withdrawn"), WithdrawnId);
		}
	}
}

void UJobBoard::BeginFacility(FServiceVehicle& Vehicle, const URoadNetwork& Network, const USimClock& Clock)
{
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	const double Seconds = Policy != nullptr && Home != nullptr
		? Policy->FacilitySeconds(Vehicle.Cargo, TypeFor(Vehicle.TypeCode), PumpsAt(Vehicle.Home, *Home)) : 0.0;
	Lifecycle(Vehicle).BeginFacility(Clock.Now() + Seconds);
	if (Seconds > 0.0)
	{
		// REFILL BEFORE IT IS FREE (spec 2026-09-28-fuel-litres): what it pumped out, at the depot's
		// pumps. Nothing pumped (a recall mid-leg) makes this zero, and it is free on the next tick.
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: depot %d refilling vehicle %d (%.0f L, %.1f game min)"),
			Vehicle.Home.Index, Vehicle.Id, FFuelRolePolicy::CapacityOf(TypeFor(Vehicle.TypeCode)) - Vehicle.Cargo, Seconds / 60.0);
	}
}

void UJobBoard::StartNext(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock)
{
	if (Vehicle.CurrentJob != 0)
	{
		return;
	}
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	if (Policy == nullptr)
	{
		// NO RULE TO DECIDE BY. A vehicle at home stays as it is; one on the road cannot be left Deciding - that
		// is a vehicle with an agent and nothing to do, the shape the per-Step backstop used to catch - so it
		// goes home, which is what "nothing to do" means everywhere else in this function.
		if (Vehicle.AgentId != 0)
		{
			GoToFacility(Vehicle, Traffic, Network, Clock);
		}
		return;
	}
	const FServiceVehicleType Type = TypeFor(Vehicle.TypeCode);

	// A BOUNDED LOOP: each pass either returns or drops a job it could not reach from the queue, so a
	// queue of unreachable jobs cannot spin. THE BOUND IS TAKEN ONCE (final review #1, 2026-09-28): it
	// read Queue.Num() each pass, which the failures themselves shrink, and with two unreachable jobs
	// the loop ended before the "nothing left - go home" branch, leaving the vehicle parked on the
	// hydrant as Serving with no job, for the session.
	// ENFORCED BY: AirportOps.Fuel.UnreachableQueueSendsItHome
	const int32 Bound = Vehicle.Queue.Num() + 1;
	for (int32 Guard = 0; Guard <= Bound; ++Guard)
	{
		// JOBS GONE FROM UNDER THE QUEUE - their aircraft left, or a re-bid moved them - are dropped.
		while (Vehicle.Queue.Num() > 0)
		{
			const FServiceJob* Head = FindJob(Vehicle.Queue[0]);
			if (Head != nullptr && Head->State == EServiceJobState::Queued && Head->VehicleId == Vehicle.Id)
			{
				break;
			}
			Vehicle.Queue.RemoveAt(0);
			++RevisionCount;   // See Revision: a queue trimmed is a change no transition reports.
		}

		const bool bAtHome = Vehicle.AgentId == 0;
		if (Vehicle.Queue.Num() == 0)
		{
			// NOTHING TO DO: home, and there the facility - for fuel, the refill of what it pumped out.
			if (bAtHome)
			{
				if (Vehicle.State != EServiceVehicleState::Idle)
				{
					UE_LOG(LogAirportOps, Log, TEXT("Vehicle %d %s: %s -> Idle"), Vehicle.Id, *Vehicle.TypeCode.ToString(),
						JobBoardText::StateName(Vehicle.State));
				}
				Lifecycle(Vehicle).BecomeIdle();
				return;
			}
			GoToFacility(Vehicle, Traffic, Network, Clock);
			return;
		}

		FServiceJob& Job = *FindJob(Vehicle.Queue[0]);

		// PREREQUISITES are the job board's ordering rule (systems map §3.5): a job whose earlier
		// services are not Done waits at the head of its queue. None exist while fuel is the only role.
		const bool bWaiting = Job.Prerequisites.ContainsByPredicate([this](int32 Prior)
			{
				const FServiceJob* Before = FindJob(Prior);
				return Before != nullptr && Before->State != EServiceJobState::Done;
			});
		if (bWaiting)
		{
			// AT HOME IT WAITS THERE - Idle with its queue, which Step retries each pass. ON THE ROAD IT CANNOT
			// WAIT WHERE IT STANDS: it would be a vehicle with an agent and no job or trip (this branch used
			// to leave it "Serving" a job it had finished, holding the hydrant, with a backstop re-running the
			// whole board pass every frame until the prerequisite closed), so it goes home and waits there. The
			// blocked job stays at the head of its queue. Dead in play today - nothing populates Prerequisites
			// while fuel is the only role - so this is the shape, staged by the test, not a measured behaviour.
			// ENFORCED BY: AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob
			if (!bAtHome)
			{
				GoToFacility(Vehicle, Traffic, Network, Clock);
			}
			return;
		}

		// THE POLICY'S RULE, the one the bid priced: straight there, or the facility first.
		if (Policy->NextStep(Vehicle.Cargo, Type, Job.QuantityOwed) == EServiceStep::ViaFacility)
		{
			if (bAtHome)
			{
				BeginFacility(Vehicle, Network, Clock);
			}
			else
			{
				GoToFacility(Vehicle, Traffic, Network, Clock);
			}
			return;
		}

		const FGuidelineNodeId Anchor = ServiceAnchorOf(Network, Job.Stand, Vehicle.Role);
		Vehicle.Queue.RemoveAt(0);
		Job.State = EServiceJobState::Underway;
		Job.LastDepot = Vehicle.Home;
		// THE JOB IS THE VEHICLE'S ONLY WHEN THE DRIVE HOLDS (SetOff, below): CurrentJob is no longer set before the
		// attempt and cleared on each of its failures, which left a window where a vehicle "held" a job it was not
		// going to. The state it leaves is read first, for the log line's "from".
		const EServiceVehicleState Was = Vehicle.State;
		if (Anchor.IsSet() && DriveVehicleTo(Vehicle, Anchor, /*bToFacility=*/false, Traffic, Network))
		{
			UE_LOG(LogAirportOps, Log, TEXT("Vehicle %d %s: %s -> ToJob (job %d, aircraft %d, stand %d, %.0f L on board)"),
				Vehicle.Id, *Vehicle.TypeCode.ToString(), JobBoardText::StateName(Was), Job.Id, Job.AircraftId,
				Job.Stand.Index, Vehicle.Cargo);
			Lifecycle(Vehicle).SetOff(Job.Id);
			return;
		}

		// REFUSED AT HOME WITH A ROUTE THERE: the traffic model would not take a plan the search
		// called valid (final review #5). Nothing about the AIRPORT is wrong, so the job stays at the
		// head of this vehicle's queue and the vehicle, Idle, retries next tick - rather than going
		// back to the board, which re-bid it, re-dispatched and re-logged three lines every tick.
		if (bAtHome && Vehicle.AgentId == 0 && Anchor.IsSet() && HomePose(Network, Vehicle).IsSet()
			&& DepotRoute(Network, HomePose(Network, Vehicle), Anchor, Type.Vehicle).IsValid())
		{
			Job.State = EServiceJobState::Queued;
			Vehicle.Queue.Insert(Job.Id, 0);
			Lifecycle(Vehicle).BecomeIdle();
			return;
		}

		// COULD NOT SET OFF: the job goes back to the board, which will choose again - this vehicle
		// included, if it is still the best - and this vehicle tries the next thing on its queue. A drive
		// that retired the agent where it stood has already put the vehicle Idle at home (DriveVehicleTo's
		// LeaveRoad), so the next pass finds it there whatever state it was in.
		Reopen(Job);
	}

	// THE LOOP CANNOT END HERE: each pass returns, or removes one job from a queue whose length bounded it, and an
	// empty queue returns from the first branch. Said, and settled, rather than assumed: a vehicle reaching this
	// line Deciding would be the wedge again, so it goes home and the ensure names the defect.
	// ENFORCED BY: AirportOps.Fuel.UnreachableQueueSendsItHome (the bound), the ensure below
	if (!ensureAlwaysMsgf(Vehicle.State != EServiceVehicleState::Deciding, TEXT("Vehicle %d: StartNext's loop ended with the decision unmade"), Vehicle.Id))
	{
		GoToFacility(Vehicle, Traffic, Network, Clock);
	}
}

void UJobBoard::FinishServe(FServiceVehicle& Vehicle, const USimClock& Clock)
{
	FServiceJob* Job = FindJob(Vehicle.CurrentJob);
	// THE SOURCE OF THE "SERVING WITH NO JOB" WEDGE (issue #428): this used to clear CurrentJob and leave the state
	// Serving until StartNext decided, so any decision that did not land left an illegal vehicle for a per-Step
	// backstop to find. EndServe leaves it Deciding - parked at the stand with its agent, no job - which the re-bid
	// prices as standing here and StartNext then always resolves. Moves FleetRevision, as this did.
	Lifecycle(Vehicle).EndServe();
	if (Job == nullptr)
	{
		return;
	}
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	// THE POLICY'S "DONE WITHIN" - the figure the bid priced the trips by and the vehicle's NextStep judges by, so the job
	// is called Done by the number the promise was made with (#443). Fuel's own, with no policy to ask.
	const double DoneWithin = Policy != nullptr ? Policy->DoneWithin() : FFuelRolePolicy::FuelledWithinLitres;
	Job->QuantityDelivered += Job->TripQuantity;
	Job->QuantityOwed = FMath::Max(Job->QuantityOwed - Job->TripQuantity, 0.0);
	++Job->Trips;
	if (Policy != nullptr)
	{
		Vehicle.Cargo = Policy->CargoAfterServe(Vehicle.Cargo, Job->TripQuantity);
	}
	Job->TripQuantity = 0.0;
	Job->VehicleId = 0;

	if (Job->QuantityOwed > DoneWithin)
	{
		// MORE THAN ONE TANKFUL (spec 2026-09-28-fuel-litres): the REMAINDER goes back to the board
		// (user's ruling 5) and is bid afresh - this vehicle, having priced its own refill, or a bowser
		// that has come free since.
		Job->State = EServiceJobState::Open;
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel: aircraft %d at stand %d has %.0f L of %.0f L after trip %d; another trip needed"),
			Job->AircraftId, Job->Stand.Index, Job->QuantityDelivered, Job->QuantityDelivered + Job->QuantityOwed, Job->Trips);
		return;
	}

	Job->State = EServiceJobState::Done;
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d fuelled at stand %d: %.0f L in %d trip(s)"),
		Job->AircraftId, Job->Stand.Index, Job->QuantityDelivered, Job->Trips);

	// EARNED HERE AND NOWHERE ELSE, by the litre. An Unserviceable job never reaches this branch, which
	// IS the forfeit.
	PostServiceFee(Clock.Now(), Job->QuantityDelivered);
}

void UJobBoard::OnVehicleArrived(FServiceVehicle& Vehicle, FGuidelineNodeId ParkedOn, UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	// NO BUMP OF ITS OWN: every branch below is a transition, and each moves FleetRevision (an arrival that
	// changes nothing - a state that expects none - no longer moves it either).
	const FGuidelineNodeId Home = HomePose(Network, Vehicle);

	if (Vehicle.State == EServiceVehicleState::ToFacility)
	{
		if (Home.IsSet() && ParkedOn == Home)
		{
			// HOME. Retired here and nowhere else: a truck does not fly away, so nothing in the traffic
			// model would ever remove it (UGroundTraffic::RetireAgent exists for exactly this). The
			// VEHICLE stays - Idle once its refill is done.
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d home at depot %d; retired"), Vehicle.AgentId, Vehicle.Home.Index);
			RetireAgentOf(Vehicle, Traffic);
			BeginFacility(Vehicle, Network, Clock);
			return;
		}
		// RECALLED ON ITS LAST LEG, AND NOW AT THE SERVICE POINT - see DriveVehicleTo's on-the-road
		// branch: it finished the leg rather than turn where no turn held, and turns for home from here,
		// parked, by the ordinary cycle's own path.
		// ENFORCED BY: AirportOps.Fuel.TowRecalledOnItsLastLegGetsHome
		GoToFacility(Vehicle, Traffic, Network, Clock);
		return;
	}

	if (Vehicle.State != EServiceVehicleState::ToJob)
	{
		return;
	}
	FServiceJob* Job = FindJob(Vehicle.CurrentJob);
	const FGuidelineNodeId Anchor = Job != nullptr ? ServiceAnchorOf(Network, Job->Stand, Vehicle.Role) : FGuidelineNodeId();
	if (Job == nullptr || ParkedOn != Anchor)
	{
		// AT THE WRONG SERVICE POINT - it was sent on while on its last leg to another, and finished that
		// leg. On from here, parked, exactly as the recall's own last-leg case goes home.
		Lifecycle(Vehicle).ReleaseCurrentJob();
		if (Job != nullptr)
		{
			Vehicle.Queue.Insert(Job->Id, 0);
			Job->State = EServiceJobState::Queued;
		}
		StartNext(Vehicle, Traffic, Network, Clock);
		return;
	}

	// AT THE STAND: this trip pumps what it holds or what is still owed, whichever is less, at the
	// vehicle's own rate (spec 2026-09-28-fuel-litres), timed on the game clock. The policy's rule, the
	// one the bid priced.
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	const FServiceVehicleType Type = TypeFor(Vehicle.TypeCode);
	const double Quantity = Policy != nullptr ? Policy->TripQuantity(Vehicle.Cargo, Type, Job->QuantityOwed) : 0.0;
	const double Seconds = Policy != nullptr ? Policy->ServeSeconds(Type, Quantity) : 0.0;
	Job->State = EServiceJobState::Serving;
	Job->TripQuantity = Quantity;
	Job->TripStartedAt = Clock.Now();
	Job->TripEndsAt = Clock.Now() + Seconds;
	Job->TankLitres = FFuelRolePolicy::CapacityOf(Type);
	Lifecycle(Vehicle).BeginServe(Job->TripEndsAt);
	UE_LOG(LogAirportOps, Log,
		TEXT("Fuel: truck %d at stand %d for aircraft %d: %.0f L of %.0f L (trip %d), %.1f game min"),
		Vehicle.AgentId, Job->Stand.Index, Job->AircraftId, Quantity, Job->QuantityOwed + Job->QuantityDelivered,
		Job->Trips + 1, Seconds / 60.0);
}

EFuelOutcome UJobBoard::FuelOutcomeOf(double Delivered, double Wanted)
{
	if (Wanted <= 0.0 || Wanted - Delivered <= FFuelRolePolicy::FuelledWithinLitres)
	{
		return EFuelOutcome::Fuelled;
	}
	return Delivered <= 0.0 ? EFuelOutcome::Unfuelled : EFuelOutcome::PartFuelled;
}

double UJobBoard::LitresWanted(int32 AgentId, const FAirframe& Airframe) const
{
	return FMath::Max(LitresOwedFor ? LitresOwedFor(AgentId, Airframe) : DefaultLitres(Airframe), 0.0);
}

void UJobBoard::EndTurnaround(int32 AircraftId, FEntityInstanceId Stand, double Delivered, double Wanted, const USimClock& Clock)
{
	const EFuelOutcome Outcome = FuelOutcomeOf(Delivered, Wanted);
	// PART-FUELLED PAYS FOR WHAT IT GOT (review, 2026-09-28), HERE AT THE ONE SITE: a job that became
	// impossible after a trip, or one the player cut short with Depart, leaves with what it got and pays
	// for it. Never twice: FinishServe pays only a job it calls Done, which FuelOutcomeOf calls Fuelled.
	// What the shortfall costs the airline is the roster's to score, not the fee's.
	// ENFORCED BY: AirportOps.Fuel.PartFuelledPaysForWhatItGot, AirportOps.Fuel.ManualDepartEndsTurnaroundOnce
	if (Outcome == EFuelOutcome::PartFuelled)
	{
		PostServiceFee(Clock.Now(), Delivered);
	}
	if (Bus != nullptr)
	{
		Bus->Publish(FTurnaroundEndedEvent{ AircraftId, Stand, Outcome, Delivered, Wanted });
	}
}

void UJobBoard::DropAircraft(int32 AircraftId, bool bDeparted, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	const FTurnaround* Turnaround = TurnaroundFor(AircraftId);
	if (Turnaround == nullptr)
	{
		return;
	}
	const TArray<int32> JobIds = Turnaround->JobIds;
	const int32 StandIndex = Turnaround->Stand.Index;
	const FEntityInstanceId StandId = Turnaround->Stand;
	++RevisionCount;   // See Revision: the turnaround and its jobs are about to go.

	// THE TURNAROUND'S END, read BEFORE the jobs go (batch 3 review I1) - the one site, see the header.
	if (bDeparted)
	{
		const FServiceJob* Fuel = JobForAircraft(AircraftId, EServiceRole::Fuel);
		const double Delivered = Fuel != nullptr ? Fuel->QuantityDelivered : 0.0;
		const double Wanted = Fuel != nullptr ? Fuel->QuantityDelivered + Fuel->QuantityOwed : 0.0;
		EndTurnaround(AircraftId, StandId, Delivered, Wanted, Clock);
	}
	Turnarounds.RemoveAll([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });

	// THE VEHICLES OUT FOR IT MOVE ON - a truck left at a hydrant nobody is using would hold that node
	// for ever. Collected first: StartNext can dispatch, and a dispatch broadcasts a phase change that a
	// synchronous listener may answer by re-entering this class (UGroundTraffic's re-entrancy contract - production's
	// listener hears it a drain later, the fixtures' too since #436, but the guard is for the contract).
	TArray<int32> Recalled;
	for (const int32 JobId : JobIds)
	{
		const FServiceJob* Job = FindJob(JobId);
		if (Job == nullptr)
		{
			continue;
		}
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d left stand %d; job %d dropped (was %d), truck %d recalled"),
			AircraftId, StandIndex, JobId, static_cast<int32>(Job->State), AgentForJob(*Job));
		if (FServiceVehicle* Vehicle = FindVehicleMutable(Job->VehicleId))
		{
			Vehicle->Queue.Remove(JobId);
			if (Vehicle->CurrentJob == JobId)
			{
				// DECIDING until the StartNext below settles it - the jobs are removed first, so the vehicle's
				// next job is chosen from what is left.
				Lifecycle(*Vehicle).ReleaseCurrentJob();
				Recalled.Add(Vehicle->Id);
			}
		}
	}
	Jobs.RemoveAll([&JobIds](const FServiceJob& Job) { return JobIds.Contains(Job.Id); });

	for (const int32 VehicleId : Recalled)
	{
		if (FServiceVehicle* Vehicle = FindVehicleMutable(VehicleId))
		{
			StartNext(*Vehicle, Traffic, Network, Clock);
		}
	}
}

namespace JobBoardPhase
{
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	/**
	 * Did this transition take the agent off where it stood UNDER ITS OWN POWER - sent somewhere - as against being
	 * removed (Retired, Cleared) or losing its road (Stranded)? THE ONE LIST, by cause (#436): OnAgentPhase typed it
	 * twice as a phase set, {Manoeuvring, Reversing, Taxiing, Departing}, once per branch that needed it. Every cause
	 * by name and no default, so a cause added to EAgentEvent is a BUILD ERROR here (C4062, raised around this
	 * function - see ExhaustiveSwitch.h), not a silent "no".
	 * ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (checked 2026-09-30 by a stray enumerator: the build failed here)
	 */
	bool LeftUnderItsOwnPower(EAgentEvent Cause)
	{
		switch (Cause)
		{
		case EAgentEvent::DepartOrdered:
		case EAgentEvent::Redirected:
		case EAgentEvent::ReOffered:
		case EAgentEvent::Rescued:
			return true;
		case EAgentEvent::None:
		case EAgentEvent::Vacated:
		case EAgentEvent::LinedUp:
		case EAgentEvent::Parked:
		case EAgentEvent::PushedBack:
		case EAgentEvent::Airborne:
		case EAgentEvent::Gone:
		case EAgentEvent::Stranded:
		case EAgentEvent::BackingIn:
		case EAgentEvent::BackedOut:
		case EAgentEvent::TouchedDown:
		case EAgentEvent::Dispatched:
		case EAgentEvent::Retired:
		case EAgentEvent::Cleared:
			return false;
		}
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END
}

void UJobBoard::OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, const FAgentTransition& Transition)
{
	const int32 AgentId = Transition.AgentId;
	const EAgentPhase From = Transition.From;
	const EAgentPhase To = Transition.To;
	const bool bLeftUnderItsOwnPower = JobBoardPhase::LeftUnderItsOwnPower(Transition.Cause);

	// NO BUMP ON ENTRY (#443): this hears every phase change of every agent in the airport, and most are none of the
	// board's business. What changes the board moves Revision where it happens: DropAircraft (an aircraft's turnaround and
	// jobs go), the lifecycle transitions and Reopen (a vehicle's agent parked, lost or stranded), and the turnaround
	// opened below.
	// ENFORCED BY: AirportOps.Fuel.RevisionHoldsStillWhenNothingChanged (a foreign agent's phase), AirportOps.Fuel.RevisionMovesOnEveryChange
	//
	// A VEHICLE'S AGENT ENTERING OR LEAVING STRANDED IS A CHANGE OF WHAT COULDSERVE COUNTS (#443): a stranded vehicle is not a
	// candidate (CandidatesFor), so the offer row's "fuel" answer - dated by the composition counter - must be asked again
	// when one strands and when it moves again (a rescue, an unstick, a retire). Only a vehicle's own agent: an aircraft's
	// stranding is none of the fleet's. THE BOARD'S OWN RETIRE is RetireAgentOf's, which unhooks the vehicle first.
	// ENFORCED BY: AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict
	// BOTH COUNTERS, as every composition change moves both: a vehicle that counts again is a bidder the re-bid has not met.
	if ((Transition.Cause == EAgentEvent::Stranded || From == EAgentPhase::Stranded) && VehicleForAgent(AgentId) != nullptr)
	{
		++FleetCompositionRevision;
		++FleetRevision;
	}

	// AN AIRCRAFT LEAVING ITS STAND, first: it departs, or is retired, or is deleted under the player's
	// hand. Its turnaround and jobs go, and any vehicle out for it moves on - its next job, or home.
	if (From == EAgentPhase::Parked && To != EAgentPhase::Parked && TurnaroundFor(AgentId) != nullptr)
	{
		// A DEPARTURE, named by its cause rather than "not Gone": a retire and a road lost under it (Retired, Cleared,
		// Stranded) are not the aircraft leaving under its own power - see LeftUnderItsOwnPower.
		DropAircraft(AgentId, bLeftUnderItsOwnPower, Traffic, Network, Clock);
		return;
	}

	// A DEPARTURE THAT WAS NEVER TURNED AROUND (whole-stack review M4, ruling 2026-09-30): an aircraft parked on the
	// fallback junction - no stand, so no turnaround and no fuel job - that the inspector's Depart sends off. It
	// leaves Unfuelled, owed what its flight was offered at: LitresOwedFor, the same source a turnaround's fuel job
	// takes its load from (OnAgentPhase's Parked branch below), so the two cannot be owed different amounts. SENT OFF
	// FOR A RUNWAY, not merely moving: a Parked -> Taxiing that is the re-offer taking it to a stand (FallbackParkStaysTaxiIn)
	// is not a departure, and its turnaround at the stand will end it properly. THE CAUSE SAYS WHICH (#436) -
	// DepartOrdered, UGroundTraffic::DepartAgent's own - where this used to ask the live agent, a step late, whether
	// it was still armed for a departure. The agent is still asked for its AIRFRAME, which is identity, not state:
	// the litres owed are an aeroplane's figure, and only an agent started with an FAirframe carries one.
	// ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut, AirportOps.Model.Bus.FallbackParkStaysTaxiIn
	if (From == EAgentPhase::Parked && bLeftUnderItsOwnPower)
	{
		const FRoadAgent* Leaving = Traffic.FindAgent(AgentId);
		const FAirframe* Airframe = Leaving != nullptr ? Leaving->AsAircraft() : nullptr;
		if (Airframe != nullptr && Transition.Cause == EAgentEvent::DepartOrdered)
		{
			const double Wanted = LitresWanted(AgentId, *Airframe);
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departed without a turnaround - %s, %.0f L owed"),
				AgentId, *UEnum::GetValueAsString(FuelOutcomeOf(0.0, Wanted)), Wanted);
			EndTurnaround(AgentId, FEntityInstanceId(), 0.0, Wanted, Clock);
		}
		return;
	}

	// A VEHICLE'S AGENT GONE - retired by somebody else (the player's despawn through another door, a cleared
	// traffic model), so it will never arrive and never drive again. The vehicle is not lost with it: it is Idle at
	// home, and EVERY job it held - the one it was on and its whole queue - goes back to the board for a bid, the same
	// body as the player's Unstick (LoseAgent; RecallVehicleOfAgent is its explicit form). This used to be dropped by
	// the `To != Parked` return below and found a Step later by a poll that re-opened only the current job.
	// THE BOARD'S OWN RETIREMENTS FIND NOTHING HERE: they unhook the vehicle before retiring (RetireAgentOf), so by the
	// time this fires - a drain later, in production and in the fixtures alike since #436 - no vehicle has that agent.
	// ENFORCED BY: AirportOps.Fuel.Lifecycle.AgentRetiredElsewhereRebidsWholeQueue, AirportOps.Fuel.Lifecycle.OwnRetirementsAreNotLosses,
	// AirportOps.Fuel.DepotGoneBeforeRecallLeavesNoAgent
	if (To == EAgentPhase::Gone)
	{
		if (const FServiceVehicle* Found = VehicleForAgent(AgentId))
		{
			FServiceVehicle& Vehicle = *FindVehicleMutable(Found->Id);
			const int32 Released = LoseAgent(Vehicle);
			UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %d lost its agent %d (Gone); %d job(s) back to the board, Idle at depot %d"),
				Vehicle.Id, AgentId, Released, Vehicle.Home.Index);
		}
		return;
	}

	// A VEHICLE STRANDED - the road under it went (#399's follow-up). It will never arrive, so its jobs
	// would sit assigned to it for the session and no other vehicle would bid. They go back to the board
	// now. THE VEHICLE STAYS WHERE IT IS, for the player's Unstick: nothing automatic moves a stranded
	// agent (UGroundTraffic::RescueStranded's contract), and retiring it here would make every truck on a
	// deleted road vanish. ToFacility, so whatever the player does next - a Replan to its old goal, Send
	// home - ends at home: OnVehicleArrived turns a ToFacility vehicle for home wherever it parks.
	// ENFORCED BY: AirportOps.Model.AgentRescue.StrandedVehicleReleasesJobs
	if (Transition.Cause == EAgentEvent::Stranded)
	{
		if (const FServiceVehicle* Found = VehicleForAgent(AgentId))
		{
			FServiceVehicle& Vehicle = *FindVehicleMutable(Found->Id);
			const int32 Released = ReleaseJobsOf(Vehicle);
			Lifecycle(Vehicle).HeadHome();
			UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %d (agent %d) stranded; %d job(s) back to the board, it waits for the player"),
				Vehicle.Id, AgentId, Released);
		}
		return;
	}

	if (Transition.Cause != EAgentEvent::Parked)
	{
		return;
	}

	const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
	if (Agent == nullptr)
	{
		return;
	}
	// A PARKED EVENT IS A FACT ABOUT THE PAST: the ops bus delivers it on the next ops step (spec
	// 2026-09-29 §1), and the agent may have moved on since. UGroundTraffic::ReofferStands redirects an
	// aircraft parked on a fallback junction to a stand that freed in the same frame, and its GoalNode is
	// then the NEW stand - acting on it would open a turnaround for an aircraft still taxiing in. Its own
	// Parked event for the real stand follows. SINCE #436 THE AIRCRAFT CASE IS THE EVENT'S OWN: GoalAtEvent is the
	// node it parked ON, the fallback junction, which is no stand - so no turnaround opens, and the live agent is no
	// longer asked whether it is still parked (see the aircraft branch below).
	// ENFORCED BY: AirportOps.Present.Bus.StaleParkedOpensNoTurnaround
	//
	// A VEHICLE ARRIVING - at a stand, or home. Its own state says which it was heading for, and GoalAtEvent says
	// where it parked. STILL ASKED WHETHER IT IS PARKED NOW, and only here: the board acts on a vehicle's arrival by
	// DRIVING it on (serve, refill, the next leg), and an agent something has sent on since the event is not standing
	// where the event says - acting on it would start a serve at a stand it has left.
	if (const FServiceVehicle* Found = VehicleForAgent(AgentId))
	{
		if (Agent->Phase == EAgentPhase::Parked)
		{
			OnVehicleArrived(*FindVehicleMutable(Found->Id), Transition.GoalAtEvent, Traffic, Network, Clock);
		}
		return;
	}

	// AN AIRCRAFT THAT HAS PARKED. The node it parked on must be a STAND's pose - an aircraft parked on a taxiway
	// junction (the stand-death fallback) is at no stand and demands nothing, which falls out of this
	// same lookup rather than needing a rule of its own - StandAtNode, the one UFlightBoard asks too, so the flight
	// enters Turnaround exactly where a turnaround opens (review M8). Asked of the EVENT's node (#436), so a Parked
	// heard after a same-frame re-offer finds the junction it parked on, not the stand it has been sent to since.
	// AsAircraft AS WELL AS Class: the turnaround below is an aeroplane's figure, and only an agent started with an
	// FAirframe carries one - the live agent is read for that, its identity, and for nothing that moves.
	const FAirframe* Aircraft = Agent->AsAircraft();
	if (Agent->Class != ETraversalClass::Aircraft || Aircraft == nullptr || TurnaroundFor(AgentId) != nullptr)
	{
		return;
	}
	const FEntityInstanceId Stand = StandAtNode(Network, Transition.GoalAtEvent);
	if (!Stand.IsSet())
	{
		return;
	}

	++RevisionCount;   // See Revision: a turnaround (and below, usually a job) opens.
	FTurnaround& Turnaround = Turnarounds.AddDefaulted_GetRef();
	Turnaround.AircraftId = AgentId;
	Turnaround.Stand = Stand;

	// THE CLOCK STARTS WHEN THE WHEELS STOP, not when the fuelling finishes. A turnaround is the time on
	// stand, and the services happen INSIDE it - which is what lets baggage and catering be added later
	// without lengthening anything. The figure rides on the AGENT, in its airframe bundle, because this
	// class may not include Entities/ and so cannot ask the aircraft's type. See
	// FAirframe::TurnaroundSeconds.
	Turnaround.TurnaroundEndsAt = Clock.Now() + Aircraft->TurnaroundSeconds;

	// THE LOAD, from the flight's own offer (LitresOwedFor), or the one fallback.
	const double Litres = LitresWanted(AgentId, *Aircraft);
	if (Litres <= 0.0)
	{
		// WANTS NOTHING, BUT STILL TURNS ROUND: a turnaround with no job, which DepartTheReady sends at
		// its deadline. UFuelService faked this as a Done demand for zero litres, because its departure
		// pass walked demands; the turnaround is the thing that departs now.
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d parked at stand %d; wants no fuel, away in %.0f game s"),
			AgentId, Stand.Index, Aircraft->TurnaroundSeconds);
		return;
	}

	FServiceJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = NextJobId++;
	Job.AircraftId = AgentId;
	Job.Role = EServiceRole::Fuel;
	Job.Stand = Stand;
	Job.State = EServiceJobState::Open;
	Job.QuantityOwed = Litres;
	Turnaround.JobIds.Add(Job.Id);
	UE_LOG(LogAirportOps, Log,
		TEXT("Fuel: aircraft %d parked at stand %d; needs %.0f L, away in %.0f game s at the earliest"),
		AgentId, Stand.Index, Litres, Aircraft->TurnaroundSeconds);
}

bool UJobBoard::IsBeingServed(const FTurnaround& Turnaround) const
{
	// STILL BEING SERVED, so the deadline does not apply - see DepartTheReady. Open and Queued count: the
	// airport may be about to gain the depot the job waits for, and a queued vehicle is coming.
	return Turnaround.JobIds.ContainsByPredicate([this](int32 JobId)
		{
			const FServiceJob* Job = FindJob(JobId);
			return Job != nullptr && Job->State != EServiceJobState::Done && Job->State != EServiceJobState::Unserviceable;
		});
}

double UJobBoard::NextDeadline(double Now) const
{
	double Next = TNumericLimits<double>::Max();
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		if (FServiceVehicleLifecycle::IsTimed(Vehicle) && Vehicle.StepEndsAt > Now)
		{
			Next = FMath::Min(Next, Vehicle.StepEndsAt);
		}
	}
	for (const FTurnaround& Turnaround : Turnarounds)
	{
		if (Turnaround.TurnaroundEndsAt > Now)
		{
			Next = FMath::Min(Next, Turnaround.TurnaroundEndsAt);
		}
	}
	return Next;
}

bool UJobBoard::Step(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	++StepCount;
	// NO BUMP ON ENTRY (#443): a Step with nothing to do changed nothing, and the inspector's card, keyed on Revision,
	// should not be rebuilt for it. Everything a Step changes moves Revision at the change (a vehicle's transition or the
	// fleet's membership, a job assigned, refused, re-opened or dropped, a queue trimmed, a re-bid that ran).
	LastStepDeparted.Reset();
	SyncFleet(Traffic, Network, Clock);

	// TIMED STEPS THAT ARE DUE: a trip's pumping, a refill. GAME TIME - a pause stops both. The vehicle
	// is left parked and jobless - Deciding - and decides where next below, AFTER the bids: a trip's remainder is
	// bid first, so the vehicle that just pumped can win it back and price its own refill.
	TArray<int32> ToDecide;
	for (FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.State == EServiceVehicleState::Serving && Clock.Now() >= Vehicle.StepEndsAt)
		{
			FinishServe(Vehicle, Clock);
			ToDecide.Add(Vehicle.Id);
		}
		else if (Vehicle.State == EServiceVehicleState::AtFacility && Clock.Now() >= Vehicle.StepEndsAt)
		{
			const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
			if (Policy != nullptr)
			{
				Vehicle.Cargo = Policy->CargoAfterFacility(Vehicle.Cargo, TypeFor(Vehicle.TypeCode));
			}
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d refilled at depot %d"), Vehicle.Id, Vehicle.Home.Index);
			Lifecycle(Vehicle).BecomeIdle();
			ToDecide.Add(Vehicle.Id);
		}
		else if (Vehicle.State == EServiceVehicleState::Idle && Vehicle.Queue.Num() > 0)
		{
			// A DISPATCH REFUSED LAST TICK, or a job queued while it sat at home: a free retry.
			ToDecide.Add(Vehicle.Id);
		}
		// NO BRANCH FOR "SERVING WITH NO JOB": it was the backstop for the wedge final review #1 found - parked at a
		// stand with a decision that did not land, asked again every tick until it did, which kept this whole pass
		// running every frame. The state no longer exists (EndServe leaves the vehicle Deciding and StartNext always
		// settles it), and the Error below says so if a path to it comes back.
		// ENFORCED BY: AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob
	}

	AssignOpenJobs(Traffic, Network, Clock);

	// AFTER THE NEW JOBS ARE PLACED, so a job opened this tick is not re-bid in the pass that placed it,
	// and BEFORE the idle vehicles start, so one that a re-bid just gave work sets off this tick.
	RebidQueued(Traffic, Network, Clock);

	// EVERY IDLE VEHICLE THE BIDS JUST GAVE WORK, and every one whose step just ended.
	for (FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.State == EServiceVehicleState::Idle && Vehicle.Queue.Num() > 0)
		{
			ToDecide.AddUnique(Vehicle.Id);
		}
	}
	for (const int32 VehicleId : ToDecide)
	{
		// RE-FOUND EACH TIME: a StartNext can dispatch, the dispatch broadcasts, and a synchronous listener on
		// the broadcast may re-enter this class and grow the vehicle array (the contract DropAircraft names).
		if (FServiceVehicle* Vehicle = FindVehicleMutable(VehicleId))
		{
			StartNext(*Vehicle, Traffic, Network, Clock);
		}
	}
	// THE PLAYER MAY HAVE DRAWN THE ROAD. A refused job is re-offered only when the graph has actually
	// changed SINCE THIS JOB'S OWN REFUSAL, which its own RefusedAtRevision reports without walking the
	// graph - otherwise a job nothing can serve is retried thirty times a second, logging as it goes.
	// PER JOB (issue #193), not one board-level fact. AFTER THE BIDS, so a job re-offered here is bid on
	// the next tick, never refused again in the pass that re-offered it.
	const uint32 Revision = Network.GetGuidelineRevision();
	for (FServiceJob& Job : Jobs)
	{
		if (Job.State == EServiceJobState::Unserviceable && Revision != Job.RefusedAtRevision)
		{
			ReopenRefusedJob(Job);
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: the airport changed; aircraft %d asks again"), Job.AircraftId);
		}
	}

	DepartTheReady(Traffic, Network, Clock);

	// A DECISION ALWAYS LANDS: no vehicle finishes a Step Deciding. Walked last, after DepartTheReady, because a
	// departure's phase change can recall a vehicle out for that aircraft (DropAircraft). The walk is O(fleet): 2 vehicles
	// per starter depot on 2026-09-30 (Trucks 1 x the UTILITY and FUEL types, FFuelFixture's default), and a bought fleet
	// grows it by the player's purchases, like the walks above it. A vehicle that does finish Deciding is a path to the old
	// wedge - said, not polled: the Error names it in the log the moment it is made, where the backstop hid it behind a
	// board pass that ran every frame - AND SETTLED, as StartNext's own tail settles it: a future path must not park a
	// vehicle on the hydrant for good with the Error merely firing every Step. Ids first, then acted on: GoToFacility can
	// dispatch, a synchronous listener on its broadcast may re-enter, and the vehicle array can move under a live reference.
	// AN ERROR LOG, NOT AN ENSURE (2026-09-30, after #454): an ensure prints a [Callstack], and Tools/Run-AirsideTests.ps1
	// fails the whole run on any [Callstack] in the log as a crash during teardown (issue #291) - so the one test that
	// stages this on purpose turned every full-suite run red with every test green. The detection is kept: the automation
	// framework fails any test that logs an Error it has not declared, so a leak of Deciding in another test still goes red
	// there, and StepEndSettlesAStrandedDecision declares exactly one.
	// ENFORCED BY: AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob (FFuelFixture's per-Step check),
	// AirportOps.Fuel.Lifecycle.StepEndSettlesAStrandedDecision (the recovery)
	TArray<int32> Undecided;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.State == EServiceVehicleState::Deciding)
		{
			Undecided.Add(Vehicle.Id);
		}
	}
	for (const int32 VehicleId : Undecided)
	{
		UE_LOG(LogAirportOps, Error, TEXT("Vehicle %d ends a Step Deciding - a decision that never landed"), VehicleId);
		if (FServiceVehicle* Vehicle = FindVehicleMutable(VehicleId))
		{
			GoToFacility(*Vehicle, Traffic, Network, Clock);
		}
	}

	// WHAT IS STILL UNRESOLVED - see the header. Each of these used to be retried simply because Tick ran
	// every frame; now it is retried because it said so, and nothing else is.
	const double Now = Clock.Now();
	const bool bVehicleWaiting = Vehicles.ContainsByPredicate([Now](const FServiceVehicle& Vehicle)
		{
			// A STEP DUE ALREADY - set this Step, after the due loop above ran (BeginFacility with nothing
			// to refill books StepEndsAt = Now): NextDeadline looks strictly AFTER Now, so without this it
			// would be neither handled nor scheduled (stage 3 review #1).
			const bool bTimedAndDue = FServiceVehicleLifecycle::IsTimed(Vehicle) && Vehicle.StepEndsAt <= Now;
			return bTimedAndDue || (Vehicle.State == EServiceVehicleState::Idle && Vehicle.Queue.Num() > 0);
		});
	const bool bJobOpen = Jobs.ContainsByPredicate([](const FServiceJob& Job) { return Job.State == EServiceJobState::Open; });
	// NO "DEPARTURE WAITING" (ops push-ground-freed, 2026-09-30). A due turnaround nothing is serving, refused by
	// DepartAgent, used to make this whole Step run every frame. PR D measured what it waits on: never a runway
	// (PlanAny ranks a held runway, it does not refuse one) - PushbackBlocked, which Airside's push watch now
	// announces (FPushGroundFreedEvent), or a layout refusal only a graph edit changes (FNetworkChangedEvent). Both
	// dirty this pass (UOpsRuntime::WireBus). One still being served was never polled: its jobs finish inside a Step
	// (FinishServe, AssignOpenJobs) or a phase handler (DropAircraft), and DepartTheReady runs after both, so it
	// leaves on the step that finishes it (stage 3 review #3). One not yet due is the deadline's (NextDeadline).
	// ENFORCED BY: AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet, AirportOps.Present.PushGroundFreed.DepartsTheFrameAfter
	return bVehicleWaiting || bJobOpen;
}

bool UJobBoard::HasRefusedDeparture(double Now) const
{
	return Turnarounds.ContainsByPredicate([this, Now](const FTurnaround& Turnaround)
		{
			return Now >= Turnaround.TurnaroundEndsAt && !IsBeingServed(Turnaround)
				&& Turnaround.LastDepartureRefusal != EDepartureRefusal::None;
		});
}

void UJobBoard::DepartTheReady(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// GATHERED FIRST, DEPARTED AFTER. UGroundTraffic::DepartAgent broadcasts the phase change
	// synchronously, and OnAgentPhase drops the aircraft's turnaround when it hears it - which since the
	// ops bus is a drain later (UOpsRuntime::OnAgentPhase only publishes), not inside this loop. Kept
	// anyway, and it costs one small array: a synchronous listener is legal (UGroundTraffic's re-entrancy
	// contract), and a loop over Turnarounds that one could mutate is a crash waiting on a wiring change.
	// Ids, not pointers, for the same reason.
	TArray<int32> Ready;
	for (const FTurnaround& Turnaround : Turnarounds)
	{
		if (Clock.Now() < Turnaround.TurnaroundEndsAt)
		{
			continue;
		}
		// STILL BEING SERVED, so the deadline does not apply. A vehicle that is on its way or at the
		// hydrant is finishing a job the aircraft asked for, and cutting it off would strand the vehicle
		// at a stand nobody is at. Open and Queued are here too: the airport may be about to gain the
		// depot the job is waiting for, and a queued vehicle is coming.
		if (!IsBeingServed(Turnaround))
		{
			Ready.Add(Turnaround.AircraftId);
		}
	}

	for (const int32 AircraftId : Ready)
	{
		const FTurnaround* Turnaround = TurnaroundFor(AircraftId);
		if (Turnaround == nullptr)
		{
			continue;
		}
		// READ BEFORE THE DEPARTURE, which drops the turnaround and its jobs.
		const FServiceJob* Fuel = JobForAircraft(AircraftId, EServiceRole::Fuel);
		const bool bUnfuelled = Fuel != nullptr && Fuel->State == EServiceJobState::Unserviceable;
		const EServiceRefusal Why = Fuel != nullptr ? Fuel->Why : EServiceRefusal::None;
		const double Delivered = Fuel != nullptr ? Fuel->QuantityDelivered : 0.0;
		const double Wanted = Fuel != nullptr ? Fuel->QuantityDelivered + Fuel->QuantityOwed : 0.0;
		const int32 Stand = Turnaround->Stand.Index;

		// NOTHING IS PUBLISHED OR PAID HERE (batch 3 review I1): a departure DepartAgent accepts changes the
		// aircraft's phase, OnAgentPhase drops the turnaround, and DropAircraft - which the inspector's manual Depart
		// reaches too - calls EndTurnaround, the one publisher of FTurnaroundEndedEvent and poster of the
		// part-fuelled fee (its other caller is OnAgentPhase, for a departure never turned around). A refusal
		// changes no phase, so it ends nothing, however often it is retried.
		// ENFORCED BY: AirportOps.Fuel.RefusedDepartureEndsNoTurnaround
		const EDepartureRefusal Refusal = Traffic.DepartAgent(AircraftId, Network);
		if (Refusal != EDepartureRefusal::None)
		{
			// LOGGED ON A CHANGE OF REASON, not every retry. A taxiway the player has left busy, or a
			// stand with no arm to push onto, refuses this for as long as they leave it, and the safety
			// net retries every 30 s besides the events (Step's header). Re-found because DepartAgent
			// may have moved the array.
			if (FTurnaround* Still = FindTurnaround(AircraftId); Still != nullptr && Still->LastDepartureRefusal != Refusal)
			{
				Still->LastDepartureRefusal = Refusal;
				UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d is ready to leave stand %d but cannot: %s"),
					AircraftId, Stand, *UEnum::GetValueAsString(Refusal));
			}
			continue;
		}

		// SAID WHEN IT LEAVES WITHOUT FUEL. The 'cannot be served' warning fired when the job went
		// Unserviceable and named what was missing; this says the airport lost the turnaround rather
		// than the stand, which is the consequence the player sees.
		//
		// PART-FUELLED is not UNFUELLED (review, 2026-09-28): a job that became impossible after a trip
		// or two - a depot deleted, a road cut - leaves with what it got, and PAYS for it. Fuel sold is
		// fuel paid for - PAID IN DropAircraft since batch 3, when the phase change DepartAgent announced
		// reaches OnAgentPhase; this line only says so in the log.
		// ENFORCED BY: AirportOps.Fuel.PartFuelledPaysForWhatItGot
		LastStepDeparted.Add(AircraftId);
		const bool bPartFuelled = bUnfuelled && Delivered > 0.0;
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departs stand %d%s"), AircraftId, Stand,
			bPartFuelled
				? *FString::Printf(TEXT(" PART-FUELLED %.0f of %.0f L - %s"), Delivered, Wanted, RefusalText(Why))
				: bUnfuelled
					? *FString::Printf(TEXT(" UNFUELLED - %s"), RefusalText(Why))
					: TEXT(" after its turnaround"));

	}
}

FString UJobBoard::VehicleDoing(const FServiceVehicle& Vehicle, const URoadNetwork* Network) const
{
	const FServiceJob* Job = FindJob(Vehicle.CurrentJob);
	// THE STAND BY ITS NUMBER, as the stand card and the sign painted at its turn-off say it (OpsNames::StandLabel) - not the entity index,
	// which a delete recycles and which starts at 0 (#447). No job, no stand: INDEX_NONE, as before.
	const FString Stand = Job != nullptr ? OpsNames::StandLabel(Network, Job->Stand) : FString::FromInt(INDEX_NONE);
	switch (Vehicle.State)
	{
	case EServiceVehicleState::ToJob:      return FString::Printf(TEXT("to stand %s"), *Stand);
	case EServiceVehicleState::Serving:    return FString::Printf(TEXT("fuelling at stand %s"), *Stand);
	case EServiceVehicleState::ToFacility: return FString::Printf(TEXT("to depot %d"), Vehicle.Home.Index);
	case EServiceVehicleState::AtFacility: return FString::Printf(TEXT("refilling at depot %d"), Vehicle.Home.Index);
	case EServiceVehicleState::Deciding:   return FString(TEXT("deciding where next"));
	default:                               return FString::Printf(TEXT("at depot %d"), Vehicle.Home.Index);
	}
}

FString UJobBoard::DescribeVehicle(const FServiceVehicle& Vehicle, const URoadNetwork* Network) const
{
	const FString Dot = TEXT(" · ");
	const FString Cargo = FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
	const FString Queued = Vehicle.Queue.Num() > 0 ? Dot + FString::Printf(TEXT("%d queued"), Vehicle.Queue.Num()) : FString();
	return FServiceFleet::NameOf(*this, Vehicle.TypeCode).ToString() + Dot + VehicleDoing(Vehicle, Network) + Dot + Cargo + Queued;
}

FDepotBacklog UJobBoard::DescribeDepot(FEntityInstanceId Depot, double Now, const URoadNetwork* Network) const
{
	const FString Dot = TEXT(" · ");
	auto Litres = [](double L) { return FText::AsNumber(FMath::RoundToInt(L)).ToString() + TEXT(" L"); };
	// A SPAN IN THE CLOCK'S OWN WORDS (GameTimeText::Duration, #447): "+1 h 35 min", as the aircraft card says it, where this printed "+95 min"
	// beside it. Still WHOLE MINUTES, rounded: with the inspector passing the minute's start as Now (see the header), its card
	// redraws at most once a game minute.
	auto Span = [](double Seconds) { return GameTimeText::Duration(Seconds).ToString(); };

	FDepotBacklog Out;
	TArray<FString> Lines;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.Home != Depot)
		{
			continue;
		}
		Lines.Add(VehicleLine(Vehicle, Network));

		// ITS JOBS IN THE ORDER IT WILL DO THEM: the one it is on, then its queue.
		TArray<int32> Order;
		if (Vehicle.CurrentJob != 0)
		{
			Order.Add(Vehicle.CurrentJob);
		}
		Order.Append(Vehicle.Queue);
		for (const int32 JobId : Order)
		{
			const FServiceJob* Job = FindJob(JobId);
			if (Job == nullptr)
			{
				continue;
			}
			++Out.Jobs;
			Out.ClearsAt = FMath::Max(Out.ClearsAt, Job->PromisedFinish);
			FString Line = FString::Printf(TEXT("  stand %s"), *OpsNames::StandLabel(Network, Job->Stand)) + Dot + Litres(Job->QuantityOwed)
				+ Dot + TEXT("+") + Span(Job->PromisedFinish - Now);

			// LATE is the promise landing after the aircraft's turnaround: the one number that says the
			// backlog is costing the airport, not merely keeping the depot busy.
			const FTurnaround* Turnaround = TurnaroundFor(Job->AircraftId);
			if (Turnaround != nullptr && Job->PromisedFinish > Turnaround->TurnaroundEndsAt)
			{
				++Out.LateJobs;
				Line += Dot + TEXT("late ") + Span(Job->PromisedFinish - Turnaround->TurnaroundEndsAt);
			}
			Lines.Add(Line);
		}
	}
	Out.Detail = FString::Join(Lines, TEXT("\n"));

	if (Out.Jobs == 0)
	{
		// NO VEHICLE AT ALL IS THE FIX THE CARD NAMES (facility-upgrades spec section 4): every job sits on a vehicle, so a depot with
		// none has no jobs, and "No jobs" would read as a healthy idle depot. The widget used to lay this over the summary in its own
		// wording (#447); the board owns both sentences now - RefusalText's "depot has no vehicles - buy one" is the same fact as a clause.
		Out.Summary = VehiclesAt(Depot) == 0 ? FString(TEXT("No vehicles \u2014 buy one")) : FString(TEXT("No jobs"));
		return Out;
	}
	Out.Summary = FString::Printf(TEXT("%d job%s"), Out.Jobs, Out.Jobs == 1 ? TEXT("") : TEXT("s"))
		+ Dot + TEXT("clears in ") + Span(Out.ClearsAt - Now)
		+ (Out.LateJobs > 0 ? Dot + FString::Printf(TEXT("%d late"), Out.LateJobs) : FString());
	return Out;
}

FString UJobBoard::DescribeAgent(int32 AgentId, double Now, const URoadNetwork* Network) const
{
	bool bMovesWithClock = false;
	return DescribeAgent(AgentId, Now, bMovesWithClock, Network);
}

FString UJobBoard::DescribeAgent(int32 AgentId, double Now, bool& bOutMovesWithClock, const URoadNetwork* Network) const
{
	bOutMovesWithClock = false;
	if (const FServiceVehicle* Vehicle = VehicleForAgent(AgentId))
	{
		return DescribeVehicle(*Vehicle, Network);
	}
	const FServiceJob* Job = JobForAircraft(AgentId, EServiceRole::Fuel);
	const FString Dot = TEXT(" · ");
	if (Job == nullptr)
	{
		// A TURNAROUND WITH NO FUEL JOB is an aircraft that wanted none - it still says so.
		return TurnaroundFor(AgentId) != nullptr ? TEXT("Fuel") + Dot + TEXT("none needed") : FString();
	}

	// THE CARD'S FUEL LINE (2026-09-28): the load, what is left, where the job has got to. Numbers
	// through FText::AsNumber so they group ("2,900") as the rest of the UI's do.
	auto Litres = [](double L) { return FText::AsNumber(FMath::RoundToInt(L)).ToString(); };
	const double Total = Job->QuantityOwed + Job->QuantityDelivered;
	if (Total <= 0.0)
	{
		return TEXT("Fuel") + Dot + TEXT("none needed");
	}
	const FString Head = FString::Printf(TEXT("Fuel %s L"), *Litres(Total));

	if (Job->State == EServiceJobState::Unserviceable)
	{
		return Head + Dot + RefusalText(Job->Why);
	}
	if (Job->State == EServiceJobState::Done)
	{
		return Head + Dot + (Job->Trips > 1 ? FString::Printf(TEXT("done in %d trips"), Job->Trips) : FString(TEXT("done")));
	}

	// LIVE WHILE PUMPING: QuantityOwed only moves when a trip ends, so the part of this trip's load
	// already pumped is the elapsed fraction of its pumping time - on the game clock, so a pause freezes
	// it. Rounded to 10 L so the card is not rebuilt every frame.
	double Left = Job->QuantityOwed;
	if (Job->State == EServiceJobState::Serving && Job->TripEndsAt > Job->TripStartedAt)
	{
		// THE ONE ANSWER THAT MOVES WITH THE CLOCK - the three-argument overload's flag, which lets a caller keep
		// every other answer until Revision moves.
		bOutMovesWithClock = true;
		const double Fraction = FMath::Clamp((Now - Job->TripStartedAt) / (Job->TripEndsAt - Job->TripStartedAt), 0.0, 1.0);
		Left -= Job->TripQuantity * Fraction;
	}
	Left = FMath::RoundToDouble(FMath::Max(Left, 0.0) / 10.0) * 10.0;

	const TCHAR* Stage = Job->State == EServiceJobState::Serving ? TEXT("fuelling")
		: Job->State == EServiceJobState::Underway ? TEXT("truck en route")
		: TEXT("waiting for a truck");
	// TRIPS ONLY WHEN THERE IS MORE THAN ONE: this one plus what the rest will take in the tank that is
	// coming (or came).
	const int32 TotalTrips = Job->TankLitres > 0.0
		? Job->Trips + FMath::CeilToInt(Job->QuantityOwed / Job->TankLitres) : 0;
	const FString Trips = TotalTrips > 1
		? FString::Printf(TEXT(" (trip %d of %d)"), Job->Trips + 1, TotalTrips) : FString();
	return Head + Dot + FString::Printf(TEXT("%s L left"), *Litres(Left)) + Dot + Stage + Trips;
}


void UJobBoard::AddTurnaroundForTest(int32 AircraftId, double TurnaroundEndsAt, int32 JobId)
{
	++RevisionCount;   // See Revision: every public mutator.
	FTurnaround& Turnaround = Turnarounds.AddDefaulted_GetRef();
	Turnaround.AircraftId = AircraftId;
	Turnaround.TurnaroundEndsAt = TurnaroundEndsAt;
	Turnaround.JobIds.Add(JobId);
}
