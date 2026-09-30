#include "Model/ServiceVehicleLifecycle.h"

namespace ServiceVehicleLifecycleText
{
	/** Its own namespace: the module is a UNITY build, and a second anonymous StateText in the blob would collide. */
	FString StateText(EServiceVehicleState State)
	{
		return UEnum::GetValueAsString(State);
	}
}

FServiceVehicle FServiceVehicleLifecycle::Create(int32 Id, FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo)
{
	// EVERY OTHER FIELD AT ITS DEFAULT, deliberately: Idle, no agent, no job, an empty queue, no timed step. A
	// vehicle is born in the one state that needs nothing else set, which is why this is a factory and not a
	// constructor argument list to keep in step with a second copy.
	FServiceVehicle Born;
	Born.Id = Id;
	Born.TypeCode = TypeCode;
	Born.Role = Role;
	Born.Home = Home;
	Born.Cargo = Cargo;
	return Born;
}

bool FServiceVehicleLifecycle::HasAgent(EServiceVehicleState State)
{
	switch (State)
	{
	case EServiceVehicleState::ToJob:
	case EServiceVehicleState::Serving:
	case EServiceVehicleState::ToFacility:
	case EServiceVehicleState::Deciding:
		return true;
	case EServiceVehicleState::Idle:
	case EServiceVehicleState::AtFacility:
	default:
		return false;
	}
}

bool FServiceVehicleLifecycle::HasJob(EServiceVehicleState State)
{
	return State == EServiceVehicleState::ToJob || State == EServiceVehicleState::Serving;
}

bool FServiceVehicleLifecycle::IsTimed(const FServiceVehicle& Vehicle)
{
	return Vehicle.State == EServiceVehicleState::Serving || Vehicle.State == EServiceVehicleState::AtFacility;
}

FString FServiceVehicleLifecycle::Violation(const FServiceVehicle& Vehicle, bool bSettled)
{
	if (HasAgent(Vehicle.State) != (Vehicle.AgentId != 0))
	{
		return FString::Printf(TEXT("%s with AgentId %d"), *ServiceVehicleLifecycleText::StateText(Vehicle.State), Vehicle.AgentId);
	}
	if (HasJob(Vehicle.State) != (Vehicle.CurrentJob != 0))
	{
		return FString::Printf(TEXT("%s with CurrentJob %d"), *ServiceVehicleLifecycleText::StateText(Vehicle.State), Vehicle.CurrentJob);
	}
	if (bSettled && Vehicle.State == EServiceVehicleState::Deciding)
	{
		return TEXT("Deciding across a Step - a decision that never landed");
	}
	return FString();
}

void FServiceVehicleLifecycle::SeedStateForTest(FServiceVehicle& Vehicle, EServiceVehicleState State)
{
	Vehicle.State = State;
}

bool FServiceVehicleLifecycle::ExpectFrom(std::initializer_list<EServiceVehicleState> From, const TCHAR* Transition) const
{
	for (const EServiceVehicleState Allowed : From)
	{
		if (Vehicle.State == Allowed)
		{
			return true;
		}
	}
	// ensureAlways, not ensure: a second wrong call from the same site is a second defect, and the first report is
	// the only one ensure would give. The message names the transition and the state, which is the whole diagnosis.
	ensureAlwaysMsgf(false, TEXT("Vehicle %d: %s called from %s"), Vehicle.Id, Transition, *ServiceVehicleLifecycleText::StateText(Vehicle.State));
	return false;
}

void FServiceVehicleLifecycle::Enter(EServiceVehicleState To, const TCHAR* Transition)
{
	Vehicle.State = To;
	++FleetRevision;
	const FString Why = Violation(Vehicle);
	ensureAlwaysMsgf(Why.IsEmpty(), TEXT("Vehicle %d: %s left it in an illegal shape: %s"), Vehicle.Id, Transition, *Why);
}

void FServiceVehicleLifecycle::Dispatched(int32 AgentId)
{
	ensureAlwaysMsgf(AgentId != 0, TEXT("Vehicle %d: Dispatched with no agent id"), Vehicle.Id);
	ExpectFrom({ EServiceVehicleState::Idle }, TEXT("Dispatched"));
	Vehicle.AgentId = AgentId;
	Enter(EServiceVehicleState::Deciding, TEXT("Dispatched"));
}

void FServiceVehicleLifecycle::SetOff(int32 JobId)
{
	ensureAlwaysMsgf(JobId != 0, TEXT("Vehicle %d: SetOff for no job"), Vehicle.Id);
	ExpectFrom({ EServiceVehicleState::Deciding }, TEXT("SetOff"));
	Vehicle.CurrentJob = JobId;
	Enter(EServiceVehicleState::ToJob, TEXT("SetOff"));
}

void FServiceVehicleLifecycle::BeginServe(double EndsAt)
{
	ExpectFrom({ EServiceVehicleState::ToJob }, TEXT("BeginServe"));
	Vehicle.StepEndsAt = EndsAt;
	Enter(EServiceVehicleState::Serving, TEXT("BeginServe"));
}

void FServiceVehicleLifecycle::EndServe()
{
	ExpectFrom({ EServiceVehicleState::Serving }, TEXT("EndServe"));
	Vehicle.CurrentJob = 0;
	Vehicle.StepEndsAt = 0.0;
	Enter(EServiceVehicleState::Deciding, TEXT("EndServe"));
}

void FServiceVehicleLifecycle::ReleaseCurrentJob()
{
	if (Vehicle.CurrentJob == 0)
	{
		return;
	}
	ExpectFrom({ EServiceVehicleState::ToJob, EServiceVehicleState::Serving }, TEXT("ReleaseCurrentJob"));
	Vehicle.CurrentJob = 0;
	Vehicle.StepEndsAt = 0.0;
	Enter(EServiceVehicleState::Deciding, TEXT("ReleaseCurrentJob"));
}

void FServiceVehicleLifecycle::HeadHome()
{
	ExpectFrom({ EServiceVehicleState::Deciding, EServiceVehicleState::ToFacility }, TEXT("HeadHome"));
	Enter(EServiceVehicleState::ToFacility, TEXT("HeadHome"));
}

void FServiceVehicleLifecycle::LeaveRoad()
{
	ExpectFrom({ EServiceVehicleState::Deciding, EServiceVehicleState::ToFacility }, TEXT("LeaveRoad"));
	Vehicle.AgentId = 0;
	Vehicle.StepEndsAt = 0.0;
	Enter(EServiceVehicleState::Idle, TEXT("LeaveRoad"));
}

void FServiceVehicleLifecycle::BeginFacility(double EndsAt)
{
	ExpectFrom({ EServiceVehicleState::Idle }, TEXT("BeginFacility"));
	Vehicle.StepEndsAt = EndsAt;
	Enter(EServiceVehicleState::AtFacility, TEXT("BeginFacility"));
}

void FServiceVehicleLifecycle::BecomeIdle()
{
	if (Vehicle.State == EServiceVehicleState::Idle)
	{
		return;
	}
	ExpectFrom({ EServiceVehicleState::AtFacility }, TEXT("BecomeIdle"));
	Vehicle.StepEndsAt = 0.0;
	Enter(EServiceVehicleState::Idle, TEXT("BecomeIdle"));
}

void FServiceVehicleLifecycle::ResetForRestore()
{
	Vehicle.AgentId = 0;
	Vehicle.CurrentJob = 0;
	Vehicle.Queue.Reset();
	Vehicle.StepEndsAt = 0.0;
	Enter(EServiceVehicleState::Idle, TEXT("ResetForRestore"));
}
