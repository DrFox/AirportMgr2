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
			FVector2D Threshold, Direction;
			double Length = 0.0;
			if (Network->RunwayExtentAt(Node->Position, Threshold, Direction, Length))
			{
				// The direction it will ROLL when a departure is armed; the pair otherwise,
				// because until the planner has spoken the strip has two names.
				if (Agent.bDepartureArmed)
				{
					return FString::Printf(TEXT("Runway %s"),
						*RunwayDesignator::ToText(RunwayDesignator::Designate(Agent.DepartureOrder.Direction)));
				}
				return FString::Printf(TEXT("Runway %s"), *RunwayDesignator::ToPairText(Direction));
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
		if (Agent.WaitingOn != 0)
		{
			return FString::Printf(TEXT("Holding for aircraft %d"), Agent.WaitingOn);
		}
		if (Agent.CrossingPhase != ECrossingPhase::None)
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

	FString IcaoCodeForWingspan(double WingspanUu)
	{
		// The table itself is Solve/IcaoCode.h now - shared with RunwayAdmission (width ->
		// wingspan) and AnchorLink (letter -> stand radius). See #85.
		return IcaoCode::LetterForWingspan(WingspanUu);
	}

	bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
		if (Agent == nullptr)
		{
			return false;
		}
		Out.Id = Agent->Id;
		Out.TypeName = !Agent->Airframe.TypeCode.IsNone()
			? Agent->Airframe.TypeCode.ToString()
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
		Out.SizeClass = IcaoCodeForWingspan(E.DesignWingspan);
		Out.AnchorCount = E.ResolvedAnchors.Num();

		// Captured at placement - see FEntityInstance::PoseRole. It is how the panel tells a
		// stand from a service installation without this layer knowing what either is for.
		Out.PoseRole = E.PoseRole;
		Out.bReachable = false;
		if (Network.GetGuidelineNode(E.PoseNode) != nullptr)
		{
			// Any live edge touching the pose node. Walk the edges rather than trust a
			// degree field: the node struct's edge list, if it has one, is derived state.
			for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
			{
				if (Edge.bAlive && (Edge.A == E.PoseNode || Edge.B == E.PoseNode))
				{
					Out.bReachable = true;
					break;
				}
			}
		}
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
