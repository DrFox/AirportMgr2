#include "Model/InspectFacts.h"

#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Solve/IcaoCode.h"
#include "Solve/RunwayDesignator.h"

namespace InspectFacts
{
	namespace
	{
		FString DestinationOf(const FRoadAgent& Agent, const URoadNetwork* Network)
		{
			if (Network == nullptr || !Agent.GoalNode.IsSet())
			{
				return TEXT("-");
			}
			const int32 Stand = Network->FindEntityIndexByPoseNode(Agent.GoalNode);
			if (Stand != INDEX_NONE)
			{
				return FString::Printf(TEXT("Stand %d"), Stand);
			}
			const FGuidelineNode* Node = Network->GetGuidelineNode(Agent.GoalNode);
			if (Node == nullptr)
			{
				return TEXT("-");
			}
			FRunwayEnd End;
			if (Network->RunwayExtentAt(Node->Position, End))
			{
				// The direction it will ROLL when a departure is armed; the pair otherwise,
				// because until the planner has spoken the strip has two names.
				if (Agent.bDepartureArmed)
				{
					return FString::Printf(TEXT("Runway %s"),
						*RunwayDesignator::ToText(RunwayDesignator::Designate(Agent.DepartureOrder.End.Direction)));
				}
				return FString::Printf(TEXT("Runway %s"), *RunwayDesignator::ToPairText(End.Direction));
			}
			return FString::Printf(TEXT("Node %d"), Agent.GoalNode.Index);
		}
	}

	FString StatusOf(const FRoadAgent& Agent)
	{
		if (Agent.bAwaitingStand)
		{
			return TEXT("No stand - waiting");
		}
		if (Agent.bDepartureArmed && Agent.Phase == EAgentPhase::Taxiing)
		{
			return TEXT("Departure armed");
		}
		if (Agent.GetWaitingOn() != 0)
		{
			return FString::Printf(TEXT("Holding for aircraft %d"), Agent.GetWaitingOn());
		}
		if (Agent.IsCrossing())
		{
			return TEXT("Crossing runway");
		}
		switch (Agent.Phase)
		{
		case EAgentPhase::Parked:
			return Agent.ShutdownCountdown > 0.0
				? FString::Printf(TEXT("Shutting down (%.0fs)"), Agent.ShutdownCountdown)
				: FString(TEXT("Parked"));
		case EAgentPhase::Arriving:
			return Agent.LastMotion.bAirborne ? TEXT("On final") : TEXT("Landing roll");
		case EAgentPhase::Departing:
			return Agent.LastMotion.bAirborne ? TEXT("Climbing") : TEXT("Rolling");
		case EAgentPhase::Gone:
			return TEXT("Gone");
		case EAgentPhase::Taxiing:
		default:
			return TEXT("Taxiing");
		}
	}

	bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
		if (Agent == nullptr)
		{
			return false;
		}
		Out.Id = Agent->Id;
		Out.TypeName = !Agent->TypeCode().IsNone()
			? Agent->TypeCode().ToString()
			: FString(Agent->Class == ETraversalClass::Aircraft ? TEXT("Aircraft") : TEXT("Vehicle"));
		Out.Phase = Agent->Phase;
		// Model heading is radians yaw from +X (east), anticlockwise. Compass is degrees from
		// north, clockwise: 90 - yaw, wrapped.
		const double Yaw = FMath::RadiansToDegrees(Agent->LastMotion.Heading);
		Out.HeadingDegrees = FMath::Fmod(FMath::Fmod(90.0 - Yaw, 360.0) + 360.0, 360.0);
		Out.GroundSpeed = Agent->LastMotion.GroundSpeed;
		Out.Altitude = Agent->LastMotion.Altitude;
		Out.Destination = DestinationOf(*Agent, Network);
		Out.Status = StatusOf(*Agent);
		Out.bEngineRunning = Agent->bEngineRunning;
		Out.bCanDepart = Agent->Phase == EAgentPhase::Parked;
		return true;
	}

	bool DescribeStand(const UGroundTraffic* Traffic, const URoadNetwork& Network, int32 EntityIndex, FStandFacts& Out)
	{
		const TArray<FEntityInstance>& Entities = Network.GetEntities();
		if (!Entities.IsValidIndex(EntityIndex) || !Entities[EntityIndex].bAlive)
		{
			return false;
		}
		const FEntityInstance& E = Entities[EntityIndex];
		Out.Index = EntityIndex;
		Out.DesignWingspan = E.DesignWingspan;
		// The table itself is Solve/IcaoCode.h - shared with RunwayAdmission (width ->
		// wingspan) and AnchorLink (letter -> stand radius). See #85, and #292 for the
		// one-caller forwarder this used to go through.
		Out.SizeClass = IcaoCode::LetterForWingspan(E.DesignWingspan);
		Out.AnchorCount = E.ResolvedAnchors.Num();

		// Captured at placement - see FEntityInstance::PoseRole. It is how the panel tells a
		// stand from a service installation without this layer knowing what either is for.
		Out.PoseRole = E.PoseRole;
		// Any live edge touching the pose node - Incident, not a scan of every edge in the
		// graph: RoadGuideline.h says URoadNetwork maintains it (add/remove/retarget all keep
		// it live-edges-only), and InspectFacts has no business re-deriving a fact the model
		// already guarantees (#104).
		const FGuidelineNode* Node = Network.GetGuidelineNode(E.PoseNode);
		Out.bReachable = Node != nullptr && Node->Incident.Num() > 0;
		Out.OccupantAgent = 0;
		Out.bOccupantParked = false;
		if (Traffic != nullptr && E.PoseNode.IsSet())
		{
			// THE CLAIM, not a scan of goals: the table is what the planner refuses on, so
			// the panel shows the same answer the next arrival will get.
			int32 Holder = 0;
			if (Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(E.PoseNode), 0, &Holder))
			{
				Out.OccupantAgent = Holder;
				const FRoadAgent* Agent = Traffic->FindAgent(Holder);
				Out.bOccupantParked = Agent != nullptr && Agent->Phase == EAgentPhase::Parked;
			}
		}
		return true;
	}
}
