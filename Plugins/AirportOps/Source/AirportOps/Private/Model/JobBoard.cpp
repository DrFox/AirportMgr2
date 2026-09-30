#include "Model/JobBoard.h"

#include "Model/Flight.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"

#include "AirportOpsLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/SimClock.h"
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
	++RevisionCount;   // See Revision: every public mutator.
	Jobs.Reset();
	Turnarounds.Reset();
	Vehicles.Reset();
	// CLEARED, THEN RESTORED FROM THE BLOB (it is saved): a snapshot without it re-seeds each depot once, which is the old behaviour.
	SeededDepots.Reset();
	++FleetRevision;
}

void UJobBoard::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	if (Ar.IsLoading())
	{
		++RevisionCount;   // See Revision: a restore is a change.
	}
	if (!Ar.IsLoading() || Vehicles.Num() == 0)
	{
		return;
	}
	for (FServiceVehicle& Vehicle : Vehicles)
	{
		Vehicle.State = EServiceVehicleState::Idle;
		Vehicle.AgentId = 0;
		Vehicle.CurrentJob = 0;
		Vehicle.Queue.Reset();
		Vehicle.StepStartedAt = 0.0;
		Vehicle.StepEndsAt = 0.0;
		// ITS DEPOT HAS BEEN SEEN: the placeholder must not add a second fleet beside a restored one.
		// ENFORCED BY: AirportOps.Fuel.RestoredFleetIsNotReseeded
		if (Vehicle.Home.IsSet())
		{
			SeededDepots.Add(Vehicle.Home);
		}
	}
	++FleetRevision;
	UE_LOG(LogAirportOps, Log, TEXT("Restore: %d vehicle(s) idle at home"), Vehicles.Num());
}

const TCHAR* UJobBoard::RefusalText(EServiceRefusal Why)
{
	switch (Why)
	{
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
	default:                             return TEXT("unserviceable");
	}
}

void UJobBoard::ResolveVehicles(TFunctionRef<FVehicle(EIcaoCode)> Resolve)
{
	++RevisionCount;   // See Revision: every public mutator.
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

TArray<FName> UJobBoard::FleetTypes() const
{
	if (DefaultFleetTypes.Num() > 0)
	{
		return DefaultFleetTypes;
	}
	TArray<FName> Types;
	for (const FVehicle& Vehicle : VehiclesByLetter)
	{
		if (!Vehicle.TypeCode.IsNone())
		{
			Types.AddUnique(Vehicle.TypeCode);
		}
	}
	return Types;
}

FServiceVehicleType UJobBoard::TypeFor(FName TypeCode) const
{
	FServiceVehicleType Type;
	Type.TypeCode = TypeCode;
	Type.Role = EServiceRole::Fuel;
	// THE FIRST LETTER THAT SENDS IT, by declaration order - the table is the catalogue until the
	// fleet is bought (spec §3.4), and a test that widens a letter's vehicle widens this one.
	for (const FVehicle& Vehicle : VehiclesByLetter)
	{
		if (Vehicle.TypeCode == TypeCode)
		{
			Type.Vehicle = Vehicle;
			break;
		}
	}
	const FFuelVehicleSpec Spec = SpecFor(TypeCode);
	Type.Capacity = Spec.CapacityLitres;
	Type.RatePerMinute = Spec.FlowLitresPerMinute;
	return Type;
}

FFuelVehicleSpec UJobBoard::SpecFor(FName TypeCode) const
{
	if (const FFuelVehicleSpec* Found = VehicleSpecs.Find(TypeCode))
	{
		return *Found;
	}
	// ONCE PER CODE: a vehicle nobody gave fuel figures to still fuels, at the trailer's rate, and says
	// so where the designer will look.
	if (VehicleSpecs.Num() > 0 && !WarnedSpecs.Contains(TypeCode))
	{
		WarnedSpecs.Add(TypeCode);
		UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %s has no fuel figures; using %.0f L at %.0f L/min"),
			*TypeCode.ToString(), FallbackSpec.CapacityLitres, FallbackSpec.FlowLitresPerMinute);
	}
	return FallbackSpec;
}

bool UJobBoard::HasWorkingPump(const FEntityInstance& Depot)
{
	// NO MODULES IS NOT "NO PUMP". A depot placed without a plot - every depot in every save written
	// before plots existed, and every one a test places through the old signature - fuels exactly as it
	// always did. Answering false here would break the fuel loop for all of them at once, which is how
	// a feature nobody asked about stops an existing one.
	if (Depot.Modules.Num() == 0)
	{
		return true;
	}
	for (const EDepotModule Module : Depot.Modules)
	{
		if (Module == EDepotModule::Pump)
		{
			return true;
		}
	}
	return false;
}

int32 UJobBoard::PumpsAt(const FEntityInstance& Depot)
{
	int32 Pumps = 0;
	for (const EDepotModule Module : Depot.Modules)
	{
		if (Module == EDepotModule::Pump)
		{
			++Pumps;
		}
	}
	// A plotless depot counts as one pump - see HasWorkingPump for why its empty module list is not a
	// claim about pumps.
	return FMath::Max(Pumps, 1);
}

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

void UJobBoard::RegisterPolicyForTest(TSharedRef<IServiceRolePolicy> Policy)
{
	Policies.Add(Policy->Role(), Policy);
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
	++RevisionCount;   // See Revision: every public mutator.
	FServiceVehicle& Vehicle = Vehicles.AddDefaulted_GetRef();
	Vehicle.Id = NextVehicleId++;
	Vehicle.TypeCode = TypeCode;
	Vehicle.Home = Home;
	Vehicle.State = State;
	Vehicle.Cargo = Cargo;
	if (Home.IsSet())
	{
		SeededDepots.Add(Home);
	}
	++FleetRevision;
	return Vehicle;
}

void UJobBoard::ReopenRefusedJob(FServiceJob& Job)
{
	Job.State = EServiceJobState::Open;
	Job.Why = EServiceRefusal::None;
}

int32 UJobBoard::AddPurchasedVehicle(FName TypeCode, FEntityInstanceId Home)
{
	if (!Home.IsSet() || TypeCode.IsNone())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Fleet: purchase of '%s' for depot %d refused - no depot or no type"),
			*TypeCode.ToString(), Home.Index);
		return 0;
	}
	const FServiceVehicleType Type = TypeFor(TypeCode);
	FServiceVehicle& Vehicle = Vehicles.AddDefaulted_GetRef();
	Vehicle.Id = NextVehicleId++;
	Vehicle.TypeCode = TypeCode;
	Vehicle.Role = Type.Role;
	Vehicle.Home = Home;
	Vehicle.State = EServiceVehicleState::Idle;
	Vehicle.Cargo = FFuelRolePolicy::CapacityOf(Type);
	const int32 Id = Vehicle.Id;
	++FleetRevision;

	// A NEW VEHICLE IS A CHANGE A REFUSED JOB CAN ANSWER DIFFERENTLY - see the header. Re-opened, not bid
	// here: the next Step bids it, in its one sequence (the bus's FleetChanged wakes that pass).
	// ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard
	int32 Reopened = 0;
	for (FServiceJob& Job : Jobs)
	{
		if (Job.State == EServiceJobState::Unserviceable && Job.Role == Type.Role)
		{
			ReopenRefusedJob(Job);
			++Reopened;
		}
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d gains bought vehicle %d %s (%.0f L at %.0f L/min); %d refused job(s) ask again"),
		Home.Index, Id, *TypeCode.ToString(), FFuelRolePolicy::CapacityOf(Type), Type.RatePerMinute, Reopened);
	return Id;
}

bool UJobBoard::CanRemoveVehicle(int32 VehicleId) const
{
	const FServiceVehicle* Vehicle = FindVehicle(VehicleId);
	return Vehicle != nullptr && Vehicle->State == EServiceVehicleState::Idle && Vehicle->AgentId == 0
		&& Vehicle->CurrentJob == 0 && Vehicle->Queue.Num() == 0;
}

bool UJobBoard::RemoveVehicle(int32 VehicleId)
{
	if (!CanRemoveVehicle(VehicleId))
	{
		return false;
	}
	const int32 Index = Vehicles.IndexOfByPredicate([VehicleId](const FServiceVehicle& V) { return V.Id == VehicleId; });
	UE_LOG(LogAirportOps, Log, TEXT("Fleet: vehicle %d %s leaves depot %d"),
		VehicleId, *Vehicles[Index].TypeCode.ToString(), Vehicles[Index].Home.Index);
	Vehicles.RemoveAt(Index);
	++FleetRevision;
	return true;
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

FString UJobBoard::VehicleLine(const FServiceVehicle& Vehicle) const
{
	const FString Dot = TEXT(" · ");
	return FString::Printf(TEXT("%s #%d"), *Vehicle.TypeCode.ToString(), Vehicle.Id) + Dot + VehicleDoing(Vehicle)
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
	Vehicle.CurrentJob = 0;
	Vehicle.Queue.Reset();
	return Released;
}

bool UJobBoard::RecallVehicleOfAgent(int32 AgentId, bool bRetire, UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	++RevisionCount;   // See Revision: every public mutator.
	const FServiceVehicle* Found = AgentId != 0 ? VehicleForAgent(AgentId) : nullptr;
	FServiceVehicle* Vehicle = Found != nullptr ? FindVehicleMutable(Found->Id) : nullptr;
	if (Vehicle == nullptr)
	{
		return false;
	}
	const int32 Released = ReleaseJobsOf(*Vehicle);
	++FleetRevision;
	if (bRetire)
	{
		// RETIRED, THEN UNHOOKED: RetireAgent's Gone broadcast reaches OnAgentPhase, which ignores a
		// non-Parked phase, and SyncFleet then finds nothing lost - the vehicle is already home.
		Traffic.RetireAgent(AgentId);
		Vehicle->AgentId = 0;
		Vehicle->State = EServiceVehicleState::Idle;
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d (agent %d) despawned by the player; %d job(s) back to the board, Idle at depot %d"),
			Vehicle->Id, AgentId, Released, Vehicle->Home.Index);
		return true;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d (agent %d) sent home by the player; %d job(s) back to the board"),
		Vehicle->Id, AgentId, Released);
	GoToFacility(*Vehicle, Traffic, Network, Clock);
	return true;
}

void UJobBoard::SyncFleet(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// NEW DEPOTS GET THEIR PLACEHOLDER FLEET (spec §3.4): Trucks of every kind in FleetTypes, Idle and
	// full. Once per depot, so a vehicle that is out never gets a twin at home.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Depot = Entities[Index];
		if (!Depot.bAlive || Depot.PoseRole != EServiceRole::Fuel || Depot.Trucks <= 0)
		{
			continue;
		}
		const FEntityInstanceId DepotId = Network.EntityIdAt(Index);
		if (SeededDepots.Contains(DepotId))
		{
			continue;
		}
		SeededDepots.Add(DepotId);
		for (int32 Count = 0; Count < Depot.Trucks; ++Count)
		{
			for (const FName TypeCode : FleetTypes())
			{
				const FServiceVehicleType Type = TypeFor(TypeCode);
				FServiceVehicle& Vehicle = Vehicles.AddDefaulted_GetRef();
				Vehicle.Id = NextVehicleId++;
				Vehicle.TypeCode = TypeCode;
				Vehicle.Role = Type.Role;
				Vehicle.Home = DepotId;
				Vehicle.State = EServiceVehicleState::Idle;
				Vehicle.Cargo = FFuelRolePolicy::CapacityOf(Type);
				UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d gains vehicle %d %s (%.0f L at %.0f L/min)"),
					DepotId.Index, Vehicle.Id, *TypeCode.ToString(), Vehicle.Cargo, Type.RatePerMinute);
			}
		}
		++FleetRevision;
	}

	// VEHICLES WHOSE DEPOT IS GONE are withdrawn, and their jobs go back to the board - which will say
	// "no fuel depot" if that was the only one, the reason the player needs.
	for (int32 Index = Vehicles.Num() - 1; Index >= 0; --Index)
	{
		FServiceVehicle& Vehicle = Vehicles[Index];
		const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
		if (Home != nullptr && Home->bAlive)
		{
			// AN AGENT THAT VANISHED UNDER IT - retired by somebody else, a cleared traffic model. The
			// vehicle is not lost with it: it is back at home, and its job goes back to the board.
			if (Vehicle.AgentId != 0 && Traffic.FindAgent(Vehicle.AgentId) == nullptr)
			{
				UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %d lost its agent %d; back at depot %d"),
					Vehicle.Id, Vehicle.AgentId, Vehicle.Home.Index);
				Vehicle.AgentId = 0;
				if (FServiceJob* Job = FindJob(Vehicle.CurrentJob))
				{
					Reopen(*Job);
				}
				Vehicle.CurrentJob = 0;
				Vehicle.State = EServiceVehicleState::Idle;
				++FleetRevision;
			}
			continue;
		}
		int32 Reopened = 0;
		for (FServiceJob& Job : Jobs)
		{
			if (Job.VehicleId == Vehicle.Id && Job.State != EServiceJobState::Done)
			{
				Reopen(Job);
				++Reopened;
			}
		}
		if (Vehicle.AgentId != 0)
		{
			Traffic.RetireAgent(Vehicle.AgentId);
		}
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d removed; vehicle %d %s withdrawn, %d job(s) back to the board"),
			Vehicle.Home.Index, Vehicle.Id, *Vehicle.TypeCode.ToString(), Reopened);
		// PAID FOR, AS A SALE WOULD BE (ruled 2026-09-30): the player bought it, and removing its depot
		// - a bulldoze, an undo of the placement - is not a reason to lose its value. Resale, not the
		// price: a vehicle that leaves for money leaves at one rate (FFuelVehicleSpec::ResaleValue).
		// Undo never touches this (R8): a re-placed depot does not buy the vehicle back.
		// ENFORCED BY: AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles
		const FFuelVehicleSpec Spec = SpecFor(Vehicle.TypeCode);
		const double Credit = Spec.ResaleValue();
		if (Ledger != nullptr && Credit > 0.0)
		{
			const FText Name = Spec.DisplayName.IsEmpty() ? FText::FromName(Vehicle.TypeCode) : Spec.DisplayName;
			Ledger->Post(Clock.Now(), ELedgerCategory::Fleet, Credit,
				FText::Format(NSLOCTEXT("Ledger", "DepotRemovedVehicle", "{0} #{1} - depot removed"), Name, FText::AsNumber(Vehicle.Id)));
			UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d removed; vehicle %d %s credited %.0f"),
				Vehicle.Home.Index, Vehicle.Id, *Vehicle.TypeCode.ToString(), Credit);
		}
		Vehicles.RemoveAt(Index);
		++FleetRevision;
	}
}

void UJobBoard::BeginFacility(FServiceVehicle& Vehicle, const URoadNetwork& Network, const USimClock& Clock)
{
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	const double Seconds = Policy != nullptr && Home != nullptr
		? Policy->FacilitySeconds(Vehicle.Cargo, TypeFor(Vehicle.TypeCode), PumpsAt(*Home)) : 0.0;
	Vehicle.State = EServiceVehicleState::AtFacility;
	Vehicle.StepStartedAt = Clock.Now();
	Vehicle.StepEndsAt = Clock.Now() + Seconds;
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
				Vehicle.State = EServiceVehicleState::Idle;
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
		Vehicle.CurrentJob = Job.Id;
		Job.State = EServiceJobState::Underway;
		Job.LastDepot = Vehicle.Home;
		if (Anchor.IsSet() && DriveVehicleTo(Vehicle, Anchor, /*bToFacility=*/false, Traffic, Network))
		{
			UE_LOG(LogAirportOps, Log, TEXT("Vehicle %d %s: %s -> ToJob (job %d, aircraft %d, stand %d, %.0f L on board)"),
				Vehicle.Id, *Vehicle.TypeCode.ToString(), JobBoardText::StateName(Vehicle.State), Job.Id, Job.AircraftId,
				Job.Stand.Index, Vehicle.Cargo);
			Vehicle.State = EServiceVehicleState::ToJob;
			return;
		}

		Vehicle.CurrentJob = 0;

		// REFUSED AT HOME WITH A ROUTE THERE: the traffic model would not take a plan the search
		// called valid (final review #5). Nothing about the AIRPORT is wrong, so the job stays at the
		// head of this vehicle's queue and the vehicle, Idle, retries next tick - rather than going
		// back to the board, which re-bid it, re-dispatched and re-logged three lines every tick.
		if (bAtHome && Vehicle.AgentId == 0 && Anchor.IsSet() && HomePose(Network, Vehicle).IsSet()
			&& DepotRoute(Network, HomePose(Network, Vehicle), Anchor, Type.Vehicle).IsValid())
		{
			Job.State = EServiceJobState::Queued;
			Vehicle.Queue.Insert(Job.Id, 0);
			Vehicle.State = EServiceVehicleState::Idle;
			return;
		}

		// COULD NOT SET OFF: the job goes back to the board, which will choose again - this vehicle
		// included, if it is still the best - and this vehicle tries the next thing on its queue.
		Reopen(Job);
		if (Vehicle.AgentId == 0 && !bAtHome)
		{
			// Retired where it stood by the failed drive: at home now, whatever state it was in.
			Vehicle.State = EServiceVehicleState::Idle;
		}
	}
}

void UJobBoard::FinishServe(FServiceVehicle& Vehicle, const USimClock& Clock)
{
	FServiceJob* Job = FindJob(Vehicle.CurrentJob);
	Vehicle.CurrentJob = 0;
	++FleetRevision;
	if (Job == nullptr)
	{
		return;
	}
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	Job->QuantityDelivered += Job->TripQuantity;
	Job->QuantityOwed = FMath::Max(Job->QuantityOwed - Job->TripQuantity, 0.0);
	++Job->Trips;
	if (Policy != nullptr)
	{
		Vehicle.Cargo = Policy->CargoAfterServe(Vehicle.Cargo, Job->TripQuantity);
	}
	Job->TripQuantity = 0.0;
	Job->VehicleId = 0;

	if (Job->QuantityOwed > FuelledWithinLitres)
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

void UJobBoard::OnVehicleArrived(FServiceVehicle& Vehicle, const FRoadAgent& Agent, UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	++FleetRevision;
	const FGuidelineNodeId Home = HomePose(Network, Vehicle);

	if (Vehicle.State == EServiceVehicleState::ToFacility)
	{
		if (Home.IsSet() && Agent.GoalNode == Home)
		{
			// HOME. Retired here and nowhere else: a truck does not fly away, so nothing in the traffic
			// model would ever remove it (UGroundTraffic::RetireAgent exists for exactly this). The
			// VEHICLE stays - Idle once its refill is done.
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d home at depot %d; retired"), Vehicle.AgentId, Vehicle.Home.Index);
			Traffic.RetireAgent(Vehicle.AgentId);
			Vehicle.AgentId = 0;
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
	if (Job == nullptr || Agent.GoalNode != Anchor)
	{
		// AT THE WRONG SERVICE POINT - it was sent on while on its last leg to another, and finished that
		// leg. On from here, parked, exactly as the recall's own last-leg case goes home.
		Vehicle.CurrentJob = 0;
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
	Vehicle.State = EServiceVehicleState::Serving;
	Vehicle.StepStartedAt = Job->TripStartedAt;
	Vehicle.StepEndsAt = Job->TripEndsAt;
	UE_LOG(LogAirportOps, Log,
		TEXT("Fuel: truck %d at stand %d for aircraft %d: %.0f L of %.0f L (trip %d), %.1f game min"),
		Vehicle.AgentId, Job->Stand.Index, Job->AircraftId, Quantity, Job->QuantityOwed + Job->QuantityDelivered,
		Job->Trips + 1, Seconds / 60.0);
}

EFuelOutcome UJobBoard::FuelOutcomeOf(double Delivered, double Wanted)
{
	if (Wanted <= 0.0 || Wanted - Delivered <= FuelledWithinLitres)
	{
		return EFuelOutcome::Fuelled;
	}
	return Delivered <= 0.0 ? EFuelOutcome::Unfuelled : EFuelOutcome::PartFuelled;
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
	// for ever. Collected first: StartNext can dispatch, and a dispatch broadcasts a phase change that
	// re-enters this class.
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
				Vehicle->CurrentJob = 0;
				Recalled.Add(Vehicle->Id);
			}
		}
	}
	Jobs.RemoveAll([&JobIds](const FServiceJob& Job) { return JobIds.Contains(Job.Id); });

	for (const int32 VehicleId : Recalled)
	{
		if (FServiceVehicle* Vehicle = FindVehicleMutable(VehicleId))
		{
			++FleetRevision;
			StartNext(*Vehicle, Traffic, Network, Clock);
		}
	}
}

void UJobBoard::OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	++RevisionCount;   // See Revision: every public mutator.
	// AN AIRCRAFT LEAVING ITS STAND, first: it departs, or is retired, or is deleted under the player's
	// hand. Its turnaround and jobs go, and any vehicle out for it moves on - its next job, or home.
	if (From == EAgentPhase::Parked && To != EAgentPhase::Parked && TurnaroundFor(AgentId) != nullptr)
	{
		// A DEPARTING PHASE, named rather than "not Gone": Gone is a retire and Stranded a road lost under
		// it, and neither is the aircraft leaving under its own power.
		const bool bDeparted = To == EAgentPhase::Manoeuvring || To == EAgentPhase::Reversing
			|| To == EAgentPhase::Taxiing || To == EAgentPhase::Departing;
		DropAircraft(AgentId, bDeparted, Traffic, Network, Clock);
		return;
	}

	// A DEPARTURE THAT WAS NEVER TURNED AROUND (whole-stack review M4, ruling 2026-09-30): an aircraft parked on the
	// fallback junction - no stand, so no turnaround and no fuel job - that the inspector's Depart sends off. It
	// leaves Unfuelled, owed what its flight was offered at: LitresOwedFor, the same source a turnaround's fuel job
	// takes its load from (OnAgentPhase's Parked branch below), so the two cannot be owed different amounts. ARMED FOR
	// A RUNWAY, not merely moving: a Parked -> Taxiing that is the re-offer taking it to a stand (FallbackParkStaysTaxiIn)
	// is not a departure, and its turnaround at the stand will end it properly. The Parked event is heard a step
	// late, so the agent is asked as it is now - which is where a departure is still taxiing out, armed.
	// ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut, AirportOps.Model.Bus.FallbackParkStaysTaxiIn
	if (From == EAgentPhase::Parked && (To == EAgentPhase::Manoeuvring || To == EAgentPhase::Reversing
		|| To == EAgentPhase::Taxiing || To == EAgentPhase::Departing))
	{
		const FRoadAgent* Leaving = Traffic.FindAgent(AgentId);
		const FAirframe* Airframe = Leaving != nullptr ? Leaving->AsAircraft() : nullptr;
		if (Airframe != nullptr && Leaving->bDepartureArmed)
		{
			const double Wanted = FMath::Max(LitresOwedFor ? LitresOwedFor(AgentId, *Airframe) : DefaultLitres(*Airframe), 0.0);
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departed without a turnaround - unfuelled, %.0f L owed"),
				AgentId, Wanted);
			EndTurnaround(AgentId, FEntityInstanceId(), 0.0, Wanted, Clock);
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
	if (To == EAgentPhase::Stranded)
	{
		if (const FServiceVehicle* Found = VehicleForAgent(AgentId))
		{
			FServiceVehicle& Vehicle = *FindVehicleMutable(Found->Id);
			const int32 Released = ReleaseJobsOf(Vehicle);
			Vehicle.State = EServiceVehicleState::ToFacility;
			++FleetRevision;
			UE_LOG(LogAirportOps, Warning, TEXT("Fuel: vehicle %d (agent %d) stranded; %d job(s) back to the board, it waits for the player"),
				Vehicle.Id, AgentId, Released);
		}
		return;
	}

	if (To != EAgentPhase::Parked)
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
	// Parked event for the real stand follows.
	// ENFORCED BY: AirportOps.Present.Bus.StaleParkedOpensNoTurnaround
	if (Agent->Phase != EAgentPhase::Parked)
	{
		return;
	}

	// A VEHICLE ARRIVING - at a stand, or home. Its own state says which it was heading for.
	if (const FServiceVehicle* Found = VehicleForAgent(AgentId))
	{
		OnVehicleArrived(*FindVehicleMutable(Found->Id), *Agent, Traffic, Network, Clock);
		return;
	}

	// AN AIRCRAFT THAT HAS PARKED. Its goal must be a STAND's pose - an aircraft parked on a taxiway
	// junction (the stand-death fallback) is at no stand and demands nothing, which falls out of this
	// same lookup rather than needing a rule of its own - StandAtGoal, the one UFlightBoard asks too, so the flight
	// enters Turnaround exactly where a turnaround opens (review M8). AsAircraft AS WELL AS Class: the turnaround
	// below is an aeroplane's figure, and only an agent started with an FAirframe carries one.
	const FAirframe* Aircraft = Agent->AsAircraft();
	if (Agent->Class != ETraversalClass::Aircraft || Aircraft == nullptr || TurnaroundFor(AgentId) != nullptr)
	{
		return;
	}
	const FEntityInstanceId Stand = StandAtGoal(Network, *Agent);
	if (!Stand.IsSet())
	{
		return;
	}

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
	const double Litres = FMath::Max(LitresOwedFor ? LitresOwedFor(AgentId, *Aircraft) : DefaultLitres(*Aircraft), 0.0);
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
		const bool bTimed = (Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob != 0)
			|| Vehicle.State == EServiceVehicleState::AtFacility;
		if (bTimed && Vehicle.StepEndsAt > Now)
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
	++RevisionCount;   // See Revision: every public mutator.
	SyncFleet(Traffic, Network, Clock);

	// TIMED STEPS THAT ARE DUE: a trip's pumping, a refill. GAME TIME - a pause stops both. The vehicle
	// is left parked and jobless, and decides where next below, AFTER the bids: a trip's remainder is
	// bid first, so the vehicle that just pumped can win it back and price its own refill.
	TArray<int32> Deciding;
	for (FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob != 0 && Clock.Now() >= Vehicle.StepEndsAt)
		{
			FinishServe(Vehicle, Clock);
			Deciding.Add(Vehicle.Id);
		}
		else if (Vehicle.State == EServiceVehicleState::AtFacility && Clock.Now() >= Vehicle.StepEndsAt)
		{
			const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
			if (Policy != nullptr)
			{
				Vehicle.Cargo = Policy->CargoAfterFacility(Vehicle.Cargo, TypeFor(Vehicle.TypeCode));
			}
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: vehicle %d refilled at depot %d"), Vehicle.Id, Vehicle.Home.Index);
			Vehicle.State = EServiceVehicleState::Idle;
			++FleetRevision;
			Deciding.Add(Vehicle.Id);
		}
		else if (Vehicle.State == EServiceVehicleState::Idle && Vehicle.Queue.Num() > 0)
		{
			// A DISPATCH REFUSED LAST TICK, or a job queued while it sat at home: a free retry.
			Deciding.Add(Vehicle.Id);
		}
		else if (Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob == 0)
		{
			// PARKED AT A STAND WITH NOTHING TO DO - a decision that did not land (a prerequisite still
			// open, or a drive refused). Asked again every tick until it does: the backstop for the
			// wedge final review #1 found, whatever the next path to it is.
			Deciding.Add(Vehicle.Id);
		}
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
			Deciding.AddUnique(Vehicle.Id);
		}
	}
	for (const int32 VehicleId : Deciding)
	{
		// RE-FOUND EACH TIME: a StartNext can dispatch, the dispatch broadcasts, and the broadcast may
		// re-enter this class and grow the vehicle array.
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

	// WHAT IS STILL UNRESOLVED - see the header. Each of these used to be retried simply because Tick ran
	// every frame; now it is retried because it said so, and nothing else is.
	const double Now = Clock.Now();
	const bool bVehicleWaiting = Vehicles.ContainsByPredicate([Now](const FServiceVehicle& Vehicle)
		{
			// A STEP DUE ALREADY - set this Step, after the due loop above ran (BeginFacility with nothing
			// to refill books StepEndsAt = Now): NextDeadline looks strictly AFTER Now, so without this it
			// would be neither handled nor scheduled (stage 3 review #1).
			const bool bTimedAndDue = ((Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob != 0)
				|| Vehicle.State == EServiceVehicleState::AtFacility) && Vehicle.StepEndsAt <= Now;
			return bTimedAndDue
				|| (Vehicle.State == EServiceVehicleState::Idle && Vehicle.Queue.Num() > 0)
				|| (Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob == 0);
		});
	const bool bJobOpen = Jobs.ContainsByPredicate([](const FServiceJob& Job) { return Job.State == EServiceJobState::Open; });
	// ONLY A DUE TURNAROUND NOTHING IS SERVING - one DepartAgent refused (a busy runway). One still being
	// served is not polled: its jobs finish inside a Step (FinishServe, AssignOpenJobs) or a phase handler
	// (DropAircraft), and DepartTheReady runs after both, so it leaves on the step that finishes it
	// (stage 3 review #3 - a late fuel job used to run a whole Step every frame for minutes).
	const bool bDepartureWaiting = Turnarounds.ContainsByPredicate([this, Now](const FTurnaround& Turnaround)
		{
			return Now >= Turnaround.TurnaroundEndsAt && !IsBeingServed(Turnaround);
		});
	return bVehicleWaiting || bJobOpen || bDepartureWaiting;
}

void UJobBoard::DepartTheReady(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// GATHERED FIRST, DEPARTED AFTER. UGroundTraffic::DepartAgent broadcasts the phase change
	// synchronously, OnAgentPhase is on the other end of that broadcast, and it drops the aircraft's
	// turnaround - so departing inside a loop over Turnarounds would mutate the array being walked.
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
		// aircraft's phase, OnAgentPhase drops the turnaround, and DropAircraft - the one site the inspector's
		// manual Depart reaches too - posts the part-fuelled fee and publishes FTurnaroundEndedEvent. A refusal
		// changes no phase, so it ends nothing, however often it is retried.
		// ENFORCED BY: AirportOps.Fuel.RefusedDepartureEndsNoTurnaround
		const EDepartureRefusal Refusal = Traffic.DepartAgent(AircraftId, Network);
		if (Refusal != EDepartureRefusal::None)
		{
			// LOGGED ON A CHANGE OF REASON, not every tick. A runway the player has left occupied
			// refuses this for as long as they leave it, and a line a tick would bury every other line
			// in the file. Re-found because DepartAgent may have moved the array.
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
		const bool bPartFuelled = bUnfuelled && Delivered > 0.0;
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departs stand %d%s"), AircraftId, Stand,
			bPartFuelled
				? *FString::Printf(TEXT(" PART-FUELLED %.0f of %.0f L - %s"), Delivered, Wanted, RefusalText(Why))
				: bUnfuelled
					? *FString::Printf(TEXT(" UNFUELLED - %s"), RefusalText(Why))
					: TEXT(" after its turnaround"));

	}
}

FString UJobBoard::VehicleDoing(const FServiceVehicle& Vehicle) const
{
	const FServiceJob* Job = FindJob(Vehicle.CurrentJob);
	const int32 Stand = Job != nullptr ? Job->Stand.Index : INDEX_NONE;
	switch (Vehicle.State)
	{
	case EServiceVehicleState::ToJob:      return FString::Printf(TEXT("to stand %d"), Stand);
	case EServiceVehicleState::Serving:    return FString::Printf(TEXT("fuelling at stand %d"), Stand);
	case EServiceVehicleState::ToFacility: return FString::Printf(TEXT("to depot %d"), Vehicle.Home.Index);
	case EServiceVehicleState::AtFacility: return FString::Printf(TEXT("refilling at depot %d"), Vehicle.Home.Index);
	default:                               return FString::Printf(TEXT("at depot %d"), Vehicle.Home.Index);
	}
}

FString UJobBoard::DescribeVehicle(const FServiceVehicle& Vehicle) const
{
	const FString Dot = TEXT(" · ");
	const FString Cargo = FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
	const FString Queued = Vehicle.Queue.Num() > 0 ? Dot + FString::Printf(TEXT("%d queued"), Vehicle.Queue.Num()) : FString();
	return Vehicle.TypeCode.ToString() + Dot + VehicleDoing(Vehicle) + Dot + Cargo + Queued;
}

FDepotBacklog UJobBoard::DescribeDepot(FEntityInstanceId Depot, double Now) const
{
	const FString Dot = TEXT(" · ");
	auto Litres = [](double L) { return FText::AsNumber(FMath::RoundToInt(L)).ToString() + TEXT(" L"); };
	// WHOLE MINUTES, rounded: with the inspector passing the minute's start as Now (see the header), its card
	// redraws at most once a game minute.
	auto Minutes = [](double Seconds) { return FMath::RoundToInt(FMath::Max(Seconds, 0.0) / 60.0); };

	FDepotBacklog Out;
	TArray<FString> Lines;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.Home != Depot)
		{
			continue;
		}
		Lines.Add(VehicleLine(Vehicle));

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
			FString Line = FString::Printf(TEXT("  stand %d"), Job->Stand.Index) + Dot + Litres(Job->QuantityOwed)
				+ Dot + FString::Printf(TEXT("+%d min"), Minutes(Job->PromisedFinish - Now));

			// LATE is the promise landing after the aircraft's turnaround: the one number that says the
			// backlog is costing the airport, not merely keeping the depot busy.
			const FTurnaround* Turnaround = TurnaroundFor(Job->AircraftId);
			if (Turnaround != nullptr && Job->PromisedFinish > Turnaround->TurnaroundEndsAt)
			{
				++Out.LateJobs;
				Line += Dot + FString::Printf(TEXT("late %d min"), Minutes(Job->PromisedFinish - Turnaround->TurnaroundEndsAt));
			}
			Lines.Add(Line);
		}
	}
	Out.Detail = FString::Join(Lines, TEXT("\n"));

	if (Out.Jobs == 0)
	{
		Out.Summary = TEXT("No jobs");
		return Out;
	}
	Out.Summary = FString::Printf(TEXT("%d job%s"), Out.Jobs, Out.Jobs == 1 ? TEXT("") : TEXT("s"))
		+ Dot + FString::Printf(TEXT("clears in %d min"), Minutes(Out.ClearsAt - Now))
		+ (Out.LateJobs > 0 ? Dot + FString::Printf(TEXT("%d late"), Out.LateJobs) : FString());
	return Out;
}

FString UJobBoard::DescribeAgent(int32 AgentId, double Now) const
{
	bool bMovesWithClock = false;
	return DescribeAgent(AgentId, Now, bMovesWithClock);
}

FString UJobBoard::DescribeAgent(int32 AgentId, double Now, bool& bOutMovesWithClock) const
{
	bOutMovesWithClock = false;
	if (const FServiceVehicle* Vehicle = VehicleForAgent(AgentId))
	{
		return DescribeVehicle(*Vehicle);
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
