#include "Present/AirsideTraffic.h"

#include "Present/TyreSmoke.h"

#include "AirsideLog.h"
#include "Content/AirsideSettings.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadAgentActor.h"

UAirsideTraffic::UAirsideTraffic()
{
	Model = CreateDefaultSubobject<UGroundTraffic>(TEXT("GroundTraffic"));
}

void UAirsideTraffic::PostInitProperties()
{
	Super::PostInitProperties();

	// See the header. A duplicate arrives holding the CDO's subobject; the constructor has
	// already created the right one by name, so this puts the pointer back on it. The relay
	// is bound HERE rather than in the constructor so it is bound to whichever model this
	// object actually ends up with - IsBoundToObject because PostInitProperties can run more
	// than once over a lifetime and a second bind would double every event.
	Model = Cast<UGroundTraffic>(GetDefaultSubobjectByName(TEXT("GroundTraffic")));
	if (Model != nullptr && !Model->OnAgentPhaseChanged.IsBoundToObject(this))
	{
		Model->OnAgentPhaseChanged.AddUObject(this, &UAirsideTraffic::OnModelPhaseChanged);
		Model->OnArrivalRefused.AddUObject(this, &UAirsideTraffic::OnModelArrivalRefused);
	}
}

void UAirsideTraffic::OnModelPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// The view follows the agent's LIFE, which the phase events already describe: born on
	// Gone -> anything, dead on anything -> Gone. Spawning inline in Dispatch* would be a
	// second place that knows when an agent exists.
	if (From == EAgentPhase::Gone)
	{
		SpawnView(AgentId);
	}
	if (To == EAgentPhase::Gone)
	{
		DestroyView(AgentId);
	}
	OnAgentPhaseChanged.Broadcast(AgentId, From, To);
}

void UAirsideTraffic::OnModelArrivalRefused(EArrivalRefusal Why)
{
	OnArrivalRefused.Broadcast(Why);
}

void UAirsideTraffic::SpawnView(int32 AgentId)
{
	// GetWorld() on a subobject walks Outer->GetWorld() by default (see UObject::GetWorld),
	// and this object's Outer is the actor that created it with CreateDefaultSubobject - so
	// this reaches the same world GetOuter()->GetWorld() would, without a second lookup.
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		// A WARNING, NOT A REFUSAL. The old code refused the dispatch outright when there was
		// no world; it cannot any more - the model has already admitted the agent by the time
		// this runs - and a model-only agent is the truer picture anyway: the traffic exists
		// and is being simulated, there is simply nowhere to draw it. Refusing would have
		// meant the model and the view disagreeing about whether an aircraft exists.
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d has no world to show in; model only."), AgentId);
		return;
	}

	const FRoadAgent* Agent = Model->FindAgent(AgentId);
	if (Agent == nullptr)
	{
		// A MISSING VIEW MUST NEVER BE SILENT. This is reached only if the model announced a
		// phase change for an agent it no longer holds, which is a broken invariant rather
		// than a situation - and the symptom on screen is an aeroplane that simply is not
		// drawn, which no amount of staring at the level will explain.
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("Agent %d announced a phase change but is not in the model; no view spawned."), AgentId);
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = GetTypedOuter<AActor>();
	Params.ObjectFlags |= RF_Transient;
	ARoadAgentActor* View = World->SpawnActor<ARoadAgentActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (View == nullptr)
	{
		// SpawnActor declines rather than throwing (a world tearing down, a class that
		// failed to load), and the model keeps simulating the agent regardless. Said out
		// loud for the same reason as above: the alternative is an invisible aeroplane and
		// nothing anywhere saying why.
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("Agent %d: SpawnActor returned null; model only, nothing to draw."), AgentId);
		return;
	}

	// The MESH, not to be confused with the FAirframe performance struct. Pushed in, like the
	// pose: a view that fetched its own mesh by path was how a content move turned every
	// aircraft into a cube - see ARoadAgentActor::SetAirframe.
	//
	// BY WHAT THE AGENT IS, because an aircraft and a truck are different assets of different
	// KINDS - one skeletal, one static. Decided HERE rather than inside the view, so the view
	// still knows nothing about traffic classes and stays the dumb thing its header promises.
	//
	// BY BODY, NOT BY CLASS, since 2026-09-23: the mesh fields live on the FAirframe, and only
	// an agent started with one has them. Class is a routing fact (see EAgentBody).
	if (const FAirframe* Aircraft = Agent->AsAircraft())
	{
		// THE AGENT'S OWN AIRFRAME FIRST, the content default only as a fallback - see
		// UAirsideSettings::ResolveAgentView, the one place this three-way fallback is typed
		// (#104). This used to read Content->AgentMesh unconditionally - ONE mesh for every
		// aircraft in the game - so a Twin Otter was offered, dispatched and landed as a
		// Meridian: the name, the figures and the refusal reasons were all the right type's,
		// and only the aeroplane on the runway was not. It went unnoticed for as long as
		// there was exactly one aircraft model to wear.
		const FResolvedAgentView Resolved = UAirsideSettings::ResolveAgentView(*Aircraft);
		if (Resolved.Mesh != nullptr)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d wears %s (%s)"),
				AgentId, *Resolved.Mesh->GetName(),
				Aircraft->Mesh.IsNull() ? TEXT("content default") : TEXT("its own type"));
		}
		View->SetAirframe(Resolved.Mesh, Resolved.AnimClass);
	}
	else
	{
		// THE BOX IS THE FOOTPRINT THE ARBITER RESERVES, read off the rules rather than
		// written here a second time: what the player sees stopping at a junction is then the
		// length that actually stopped. Half the length across and half again tall, which is
		// a van's proportions.
		const double Length = Model->Rules.FootprintFor(Agent->Class);
		const FVector Box(Length, Length * 0.5, Length * 0.5);

		// WHICH LOOK is UAirsideSettings' call, not this branch's: ResolveVehicleViewFor loads
		// what the vehicle itself names (FVehicle::Mesh, and Tow[].Mesh), so the rig wears
		// truckCab1 and a truck the fuel truck, and nothing here names a vehicle or an asset.
		const FVehicle* Vehicle = Agent->AsVehicle();
		const FResolvedTowView Look = Vehicle != nullptr
			? UAirsideSettings::ResolveVehicleViewFor(*Vehicle) : FResolvedTowView();

		// RIGGED FIRST, STATIC SECOND. A vehicle with a skeleton has wheels that turn and
		// steer; one without is a body that slides. Asking for the rigged one and falling
		// back leaves the choice in the content set rather than in this branch.
		if (Look.Cab.Mesh != nullptr)
		{
			View->SetVehicleAirframe(Look.Cab.Mesh, Look.Cab.AnimClass, Box);
		}
		else
		{
			View->SetVehicleBody(UAirsideSettings::ResolveVehicleMesh(), Box);
		}

		// THE TOW: one mesh per BODY-carrying link. Look.Links answers Vehicle->Tow index for
		// index (FResolvedTowView's contract), and two lists that must agree are checked here,
		// where they are consumed: a mismatch would stand trailer meshes on the wrong poses.
		if (Vehicle != nullptr && Vehicle->HasTrailer())
		{
			if (Look.Links.Num() != Vehicle->Tow.Num())
			{
				UE_LOG(LogAirsideTraffic, Warning,
					TEXT("Agent %d (%s): %d tow links but a look for %d; the tow is not drawn."),
					AgentId, *Vehicle->TypeCode.ToString(), Vehicle->Tow.Num(), Look.Links.Num());
			}
			else
			{
				for (int32 Link = 0; Link < Vehicle->Tow.Num(); ++Link)
				{
					// A bar has no body to draw; it swings the NEXT link's front axle instead.
					if (Vehicle->Tow[Link].IsBar())
					{
						continue;
					}
					const int32 TowbarLink = (Link > 0 && Vehicle->Tow[Link - 1].IsBar()) ? Link - 1 : INDEX_NONE;
					View->SetVehicleTrailer(Link, Look.Links[Link].Mesh, Look.Links[Link].AnimClass, TowbarLink);
				}
			}
		}
	}

	// Posed before its first tick, so it appears at the start of its route rather than at the
	// origin for one frame. LastMotion is a full pose by now: DispatchAgent ran a zero-second
	// Advance before admitting, and StartArrival seeds it from the approach's own start.
	View->SetMotion(Agent->LastMotion, SurfaceZ);

	Views.Add(AgentId, View);
}

void UAirsideTraffic::DestroyView(int32 AgentId)
{
	// The aircraft has gone, so the actor goes with it. An agent's cube that stayed in the
	// world would accumulate one per departure, hanging above the airport for ever.
	if (TObjectPtr<ARoadAgentActor>* View = Views.Find(AgentId))
	{
		if (*View != nullptr)
		{
			(*View)->Destroy();
		}
		Views.Remove(AgentId);
	}
}

bool UAirsideTraffic::DispatchArrival(const URoadNetwork& Network, const FVector2D& Near,
	const FAirframe& Airframe, double InSurfaceZ, double ShutdownPauseSeconds)
{
	// Recorded BEFORE the dispatch, because the view is spawned off the model's broadcast
	// from inside it and SpawnView poses with this very number.
	SurfaceZ = InSurfaceZ;
	return Model->DispatchArrival(Network, Near, Airframe, ShutdownPauseSeconds) != 0;
}

bool UAirsideTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FAirframe& Airframe, double InSurfaceZ, double ShutdownPauseSeconds, ETraversalClass Class)
{
	SurfaceZ = InSurfaceZ;
	return Model->DispatchAgent(Network, Plan, Airframe, Class, ShutdownPauseSeconds) != 0;
}

bool UAirsideTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FVehicle& Vehicle, double InSurfaceZ, double ShutdownPauseSeconds, ETraversalClass Class)
{
	// Recorded before the dispatch for the reason the FAirframe overload gives.
	SurfaceZ = InSurfaceZ;
	return Model->DispatchAgent(Network, Plan, Vehicle, Class, ShutdownPauseSeconds) != 0;
}

bool UAirsideTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)
{
	return Model->RedirectAgent(AgentId, Network, Plan);
}

bool UAirsideTraffic::ExtendRoute(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Tail,
	double KeepBehind, double* OutDropped)
{
	return Model->ExtendRoute(AgentId, Network, Tail, KeepBehind, OutDropped);
}

EDepartureRefusal UAirsideTraffic::DepartAgent(int32 AgentId, const URoadNetwork* Network)
{
	if (Network == nullptr)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d: no network to plan over."), AgentId);
		return EDepartureRefusal::NoRoute;
	}
	return Model->DepartAgent(AgentId, *Network);
}

ARoadAgentActor* UAirsideTraffic::GetAgentView(int32 AgentId) const
{
	const TObjectPtr<ARoadAgentActor>* Found = Views.Find(AgentId);
	return Found != nullptr ? Found->Get() : nullptr;
}

void UAirsideTraffic::OnGraphRebuilt(const URoadNetwork& Network)
{
	Model->OnGraphRebuilt(Network);
}

bool UAirsideTraffic::RetireAgent(int32 AgentId)
{
	return Model->RetireAgent(AgentId);
}

void UAirsideTraffic::ClearAgents()
{
	// The views die on the model's own Gone events, through the relay - the one place that
	// knows an agent has ended, rather than a second sweep here that could disagree with it.
	Model->ClearAgents();
}

int32 UAirsideTraffic::GetAgentCount() const
{
	return Model->GetAgentCount();
}

int32 UAirsideTraffic::GetNewestAgentId() const
{
	return Model->GetNewestAgentId();
}

ARoadAgentActor* UAirsideTraffic::GetNewestAgent() const
{
	return Views.FindRef(Model->GetNewestAgentId());
}

void UAirsideTraffic::Advance(double DeltaSeconds, double InSurfaceZ, const URoadNetwork* Network,
	const FTrafficRules& Rules)
{
	SurfaceZ = InSurfaceZ;

	// EIGHT DOUBLES, EVERY TICK, and copied rather than pointed at. The model is Transient
	// and re-pointed on a PIE duplication (see ARoadNetworkActor::PostInitProperties), so a
	// value copied once at construction would be the CDO's for the whole play session while
	// the Details panel showed the level's - the exact shape of the 2026-09-06 PIE bug, one
	// layer down. A pointer back to the actor would work and is what this class does not do:
	// UGroundTraffic is world-free and holds nothing that can outlive a graph.
	Model->Rules = Rules;

	Model->Advance(DeltaSeconds, Network);

	// One pass over the model's own array, not the map: an agent the tick removed is already
	// out of it (and its view already destroyed through the Gone event), so this can never
	// pose a view for an agent that no longer exists.
	for (const FRoadAgent& Agent : Model->GetAgents())
	{
		if (TObjectPtr<ARoadAgentActor>* View = Views.Find(Agent.Id))
		{
			if (*View != nullptr)
			{
				(*View)->SetMotion(Agent.LastMotion, SurfaceZ);
			}
		}

		// TOUCHDOWN. An EDGE, true for one Advance only, so this fires once per landing -
		// see FLandingRun::bTouchedDown, which is cleared at the top of every Advance for
		// exactly this reason.
		const FAirframe* Aircraft = Agent.AsAircraft();
		if (Agent.Arrival.bTouchedDown && Smoke != nullptr && Aircraft != nullptr)
		{
			const FVector2D Along(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading));
			// Same convention as FRunwayMarkingBuilder's runway frame: across is the along
			// vector turned a quarter turn. Two copies of a rotation that disagreed would
			// put the puffs on the wrong side of an aircraft landing the other way.
			const FVector2D Across(-Along.Y, Along.X);

			// The MAINS, not the origin. FAirframe's origin is the nose gear, so the main
			// gear is FixedAxleX along the fuselage from it - a negative number, 14 m on the
			// Q400. Smoking at the origin would put the puffs under the nose, which touches
			// down seconds later and somewhere else.
			const FVector2D Mains = Agent.LastMotion.Position + Along * Aircraft->Chassis.FixedAxleX;
			const double HalfTrack = Aircraft->Chassis.MainGearTrack * 0.5;

			// UNMEASURED TRACK MEANS ONE PUFF, on the centreline, rather than a fabricated
			// pair - the same discipline FChassis::HasAxles applies to the steering law. A
			// made-up track puts smoke where the aeroplane has no wheels.
			if (Aircraft->Chassis.HasMainGearTrack())
			{
				Smoke->Puff(FVector(Mains - Across * HalfTrack, SurfaceZ), Aircraft->Wingspan);
				Smoke->Puff(FVector(Mains + Across * HalfTrack, SurfaceZ), Aircraft->Wingspan);
			}
			else
			{
				Smoke->Puff(FVector(Mains, SurfaceZ), Aircraft->Wingspan);
			}

			// INSTRUMENTED AT THE BOUNDARY, because the alternative is asking for another
			// PIE session. This says the touchdown was seen, where the wheels were, and
			// whether the track was measured - the three things that would have to be
			// guessed at otherwise if no smoke appeared.
			UE_LOG(LogAirside, Log,
				TEXT("Touchdown smoke: agent %d, %d puff(s) at (%.0f, %.0f), track %.0f uu, span %.0f uu."),
				Agent.Id, Aircraft->Chassis.HasMainGearTrack() ? 2 : 1, Mains.X, Mains.Y,
				Aircraft->Chassis.MainGearTrack, Aircraft->Wingspan);
		}
	}
}

EAgentPhase UAirsideTraffic::LastAgentPhaseForTest() const
{
	const FRoadAgent* Agent = Model->FindAgent(Model->GetNewestAgentId());
	return Agent != nullptr ? Agent->Phase : EAgentPhase::Gone;
}

double UAirsideTraffic::LastAgentTaxiSpeedCapForTest() const
{
	const FRoadAgent* Agent = Model->FindAgent(Model->GetNewestAgentId());
	// NOT Agent->Airframe.Ground.Taxi.SpeedCap. Issue #83 removed the follower's own copy of
	// Ground, but reading the agent's own airframe back here would be a tautology - it is
	// exactly what the test handed in, whether or not the Vacated handover ever passed it to
	// Follower.Start. Profile.Fallback is the one follower-side figure left after #83:
	// FSpeedProfile::Build sets it to Ground.Taxi.SpeedCap at Start/Replace time (see
	// SpeedProfile.cpp), so this reads back what the follower was ACTUALLY started with.
	return Agent != nullptr ? Agent->Follower.Profile.GetFallback() : 0.0;
}

FVector2D UAirsideTraffic::LastAgentPositionForTest() const
{
	const FRoadAgent* Agent = Model->FindAgent(Model->GetNewestAgentId());
	return Agent != nullptr ? Agent->LastMotion.Position : FVector2D::ZeroVector;
}

double UAirsideTraffic::EvenDelta(double RawDeltaSeconds, double SmoothingRate, double MaxOwedSeconds)
{
	return DeltaSmoother.Advance(RawDeltaSeconds, SmoothingRate, MaxOwedSeconds);
}
