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

		// EVERY DECLARED BAY ENTRY, walked off ResolvedAnchors rather than
		// Definition->ServiceBays: this is Model/, which must not dereference the Entities
		// layer (see RoadEntity.h's own comment on FEntityInstance::Definition), so the only
		// way to tell "a bay a vehicle drives to" from the pose and from a walked-to
		// (Pedestrian) fixture is the Role each anchor already captured, through the same
		// TraversalForRole a stand's own service links were cast with (AnchorLink.cpp).
		//
		// ROLE ALONE IS NOT ENOUGH. A GroundVehicle-role anchor with no matching FServiceBay
		// (AnchorLink.cpp's own "has no service bay on a stand that has a layout" warning -
		// a content gap, e.g. an authored TugStand fixture BuildStandTemplate never gave a
		// bay to) never gets a lane laid at all and sits with no edge, forever - found the
		// hard way when it forced bServiceable false on a fully-joined stand. The same
		// Incident check bReachable already runs on the pose node, run here per anchor, is
		// what tells "a laid bay" from "a declared fixture with none": a bay's own legs put
		// edges on its node at PLACEMENT, before any road is ever drawn near it.
		//
		// WALKED FROM THE SERVICE POINT, NOT FROM FServiceBay::EntryLocal's OWN NODE - the
		// same layering reason as above: EntryLocal lives on Definition->ServiceBays, and this
		// is Model/. That reads the right answer only because StandLayoutBuild::LayLeg lays
		// every bay as ONE CONTINUOUS stand-owned chain, Entry -> Park -> Service -> Cleared ->
		// Exit, so a walk from the service point in the middle of that chain reaches the entry
		// (and the road beyond it) without ever crossing an unowned edge first.
		// ENFORCED BY: Airside.Entities.EveryBayEntryReachesItsServicePoint, which walks
		// stand-owned edges only, from the service point to the entry, for every letter and
		// bay - it goes red the day a layout ever stops running one chain through both.
		int32 BayCount = 0;
		bool bAllJoined = true;
		for (const FResolvedAnchor& Anchor : E.ResolvedAnchors)
		{
			if (TraversalForRole(Anchor.Role) != ETraversalClass::GroundVehicle)
			{
				continue;
			}
			const FGuidelineNode* BayNode = Network.GetGuidelineNode(Anchor.Node);
			if (BayNode == nullptr || BayNode->Incident.Num() == 0)
			{
				continue;
			}
			++BayCount;
			bAllJoined &= Network.IsServiceNodeConnected(Anchor.Node);
		}
		// FALSE FOR NO BAYS: see FStandFacts::bServiceable - "every" over an empty set would
		// otherwise read as trivially true.
		Out.bServiceable = BayCount > 0 && bAllJoined;

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
