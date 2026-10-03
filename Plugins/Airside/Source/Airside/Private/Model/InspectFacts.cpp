#include "Model/InspectFacts.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAdmission.h"
#include "Model/TaxiPlanning.h"
#include "Model/TaxiwayRestriction.h"
#include "Profiles/RoadProfile.h"
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
				// BY NUMBER, the one painted at the stand's turn-off (FEntityInstance::StandNumber),
				// never by index - RoadSlot recycles slots, so an index names a different stand
				// after a delete. 0 should not survive PlaceEntity and PostLoad's backfill; if it
				// does, the index is better than "Stand 0", and the log says the invariant broke.
				// ENFORCED BY: Airside.Model.StandNumbers (placement and the PostLoad backfill).
				const int32 Number = Network->GetEntities()[Stand].StandNumber;
				if (Number > 0)
				{
					return FString::Printf(TEXT("Stand %d"), Number);
				}
				static bool bWarned = false;
				if (!bWarned)
				{
					bWarned = true;
					UE_LOG(LogAirside, Warning,
						TEXT("InspectFacts: entity %d is an unnumbered destination - EnsureStandNumbers did not run?"), Stand);
				}
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

	namespace
	{
		/**
		 * The card's line for what the agent is WAITING FOR (EAgentWait), or null when it says nothing - no wait, or a
		 * taxi out marked stale while the push that leads to it is still running (it is waited for from the push's end,
		 * FRoadAgent::IsHoldingForTaxiOut). Outranks motion: a waiting agent is stood still, whatever its phase.
		 * A HELD TAXI-OUT HAD NO LINE (issue #444) and read "Taxiing", stood still at the end of a dead route.
		 * ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (a wait added is a build error here until it has a line);
		 * Airside.Model.InspectFacts.HeldTaxiOutIsNotTaxiing
		 */
		AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
		const TCHAR* WaitLine(const FRoadAgent& Agent)
		{
			switch (Agent.GetWait())
			{
			case EAgentWait::None:
				return nullptr;
			case EAgentWait::ForStand:
				return TEXT("No stand - waiting");
			case EAgentWait::ForTaxiOutRoute:
				return Agent.IsHoldingForTaxiOut() ? TEXT("No way to the runway - waiting") : nullptr;
			}
			return nullptr;
		}
		AIRSIDE_EXHAUSTIVE_SWITCH_END

		/** The three facts that OUTRANK MOTION on the card - a wait, an armed departure, a hold - asked in one place by
		 *  StatusOf (to choose StatusWithHold) and by StatusWithHold's own fall-through check, so the two cannot disagree. */
		bool OutranksMotion(const FRoadAgent& Agent)
		{
			return WaitLine(Agent) != nullptr || (Agent.bDepartureArmed && Agent.Phase == EAgentPhase::Taxiing)
				|| Agent.GetWaitingOn() != 0;
		}

		/**
		 * StatusOf's precedence, with the hold line composed from Hold and BlockerName and
		 * bOutIsHold saying whether it won. ONE BODY for the public StatusOf and DescribeAgent -
		 * the latter knows the runway and the blocker's class, the former neither - so the
		 * order is written once (see FAgentFacts::bStatusIsHold).
		 */
		FString StatusWithHold(const FRoadAgent& Agent, const FAgentHold& Hold, const FString& BlockerName, bool& bOutIsHold)
		{
			bOutIsHold = false;
			if (const TCHAR* Waiting = WaitLine(Agent))
			{
				return Waiting;
			}
			if (Agent.bDepartureArmed && Agent.Phase == EAgentPhase::Taxiing)
			{
				return TEXT("Departure armed");
			}
			if (Hold.IsSet())
			{
				bOutIsHold = true;
				// NO DURATION: the stall clock is movement time and Airside has no game clock to turn
				// it into the card's units - see HoldLine. The inspector re-says it with one.
				return HoldLine(Hold, BlockerName, FString());
			}
			// NO RECURSION BACK: StatusOf only calls here when one of the three branches above will
			// return - a wait, armed, or waiting on another (HoldOf sets the hold from GetWaitingOn,
			// the very test StatusOf made). The check() pins that the fall-through cannot re-enter.
			check(!OutranksMotion(Agent));
			return StatusOf(Agent);
		}

		/** "aircraft 7" / "vehicle 7" - the blocker as Airside can name it: no registrations here. */
		FString BlockerNoun(const UGroundTraffic* Traffic, int32 BlockerId)
		{
			const FRoadAgent* Blocker = Traffic != nullptr ? Traffic->FindAgent(BlockerId) : nullptr;
			return Blocker != nullptr && Blocker->Class != ETraversalClass::Aircraft
				? FString::Printf(TEXT("vehicle %d"), BlockerId)
				: FString::Printf(TEXT("aircraft %d"), BlockerId);
		}
	}

	// EVERY PHASE NAMED, no default (issue #444): `case Taxiing: default: return "Taxiing"` put a push and a reverse on
	// the card as taxiing while the flight board said Manoeuvring. The phase's own word is FAgentPhaseTraits'; the switch
	// says only which phases refine it from a sub-state.
	// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function); Check-Architecture rule 81;
	// Airside.Model.InspectFacts.StatusDistinctPerPhase
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	FString StatusOf(const FRoadAgent& Agent)
	{
		if (OutranksMotion(Agent))
		{
			// THE THREE THAT OUTRANK MOTION go through StatusWithHold's one ordering; a bare agent
			// has no network to name a runway by and no traffic to class its blocker.
			bool bIsHold = false;
			const FAgentHold Hold = HoldOf(Agent, nullptr);
			return StatusWithHold(Agent, Hold, BlockerNoun(nullptr, Hold.WaitingOn), bIsHold);
		}
		if (Agent.IsCrossing())
		{
			return TEXT("Crossing runway");
		}
		const TCHAR* Word = Agent.PhaseTraits().DisplayText;
		switch (Agent.Phase)
		{
		case EAgentPhase::Parked:
			return Agent.ShutdownCountdown > 0.0
				? FString::Printf(TEXT("Shutting down (%.0fs)"), Agent.ShutdownCountdown)
				: FString(Word);
		case EAgentPhase::Arriving:
			return Agent.LastMotion.bAirborne ? FString(TEXT("On final")) : FString(Word);
		case EAgentPhase::Departing:
			return Agent.LastMotion.bAirborne ? FString(TEXT("Climbing")) : FString(Word);
		case EAgentPhase::Taxiing:
		case EAgentPhase::Manoeuvring:
		case EAgentPhase::Reversing:
		case EAgentPhase::Gone:
		case EAgentPhase::Stranded:
			return Word;
		}
		return Word;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
		if (Agent == nullptr)
		{
			return false;
		}
		Out.Id = Agent->Id;
		Out.TypeName = TypeNameOf(*Agent);
		Out.Phase = Agent->Phase;
		// Model heading is radians yaw from +X (east), anticlockwise. Compass is degrees from
		// north, clockwise: 90 - yaw, wrapped.
		const double Yaw = FMath::RadiansToDegrees(Agent->LastMotion.Heading);
		Out.HeadingDegrees = FMath::Fmod(FMath::Fmod(90.0 - Yaw, 360.0) + 360.0, 360.0);
		Out.GroundSpeed = Agent->LastMotion.GroundSpeed;
		Out.VerticalSpeed = Agent->LastMotion.VerticalSpeed;
		Out.Altitude = Agent->LastMotion.Altitude;
		Out.Destination = DestinationOf(*Agent, Network);
		Out.On = Network != nullptr ? WhereIs(*Agent, *Network) : FString();
		Out.Hold = HoldOf(*Agent, Network);
		Out.Status = StatusWithHold(*Agent, Out.Hold, BlockerNoun(&Traffic, Out.Hold.WaitingOn), Out.bStatusIsHold);

		// THE RING, from the alert's own list (UGroundTraffic::CurrentDeadlocks), so the card and
		// the Deadlock alert cannot disagree. Asked only of a waiter - every cycle member waits on
		// the next, so a mover is never in one - which keeps the walk off the common frame. That
		// walk is linear in agents (one out-edge each; ~20 agents on 2026-09-30).
		// ENFORCED BY: Airside.Model.InspectFacts.HoldAndDeadlockPartners
		Out.DeadlockedWith.Reset();
		if (Out.Hold.IsSet())
		{
			TArray<TArray<int32>> Cycles;
			Traffic.CurrentDeadlocks(Cycles);
			for (const TArray<int32>& Cycle : Cycles)
			{
				if (Cycle.Contains(Agent->Id))
				{
					for (const int32 Member : Cycle)
					{
						if (Member != Agent->Id) { Out.DeadlockedWith.Add(Member); }
					}
					break;
				}
			}
		}
		Out.bEngineRunning = Agent->bEngineRunning;
		Out.bCanDepart = Agent->Phase == EAgentPhase::Parked;
		const FTaxiUnplanned* Unplanned = Traffic.GetTaxiPlanning() != nullptr ? Traffic.GetTaxiPlanning()->FindUnplanned(Agent->Id) : nullptr;
		Out.TaxiUnplanned = Unplanned != nullptr ? Unplanned->Why : FString();
		if (const FAirframe* Aircraft = Agent->AsAircraft())
		{
			Out.Pushback = PushbackText(Aircraft->PushbackNeed);
		}
		return true;
	}

	FString WhereIs(const FRoadAgent& Agent, const URoadNetwork& Network)
	{
		const FRoutePlan& Plan = Agent.PlanInProgress();
		if (Plan.Steps.Num() == 0)
		{
			return FString();
		}
		const int32 Step = UGroundTraffic::CurrentStep(Plan, Agent.DistanceAlongPlan());
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Plan.Steps[Step].Edge);
		if (Edge == nullptr)
		{
			return FString();   // a plan against an edge a rebuild has since replaced
		}
		if (Network.GetSegment(Edge->DerivedFrom) != nullptr)
		{
			if (Network.IsRunwaySegment(Edge->DerivedFrom))
			{
				FRunwayCardFacts Card;
				return DescribeRunway(Network, Edge->DerivedFrom.Index, Card) ? Card.Pair : FString();
			}
			return Network.TaxiwayDisplayName(Network.TaxiwayOf(Edge->DerivedFrom));
		}
		return Edge->AtJunction.IsSet() ? Network.JunctionName(Edge->AtJunction) : FString();
	}

	FAgentHold HoldOf(const FRoadAgent& Agent, const URoadNetwork* Network)
	{
		FAgentHold Hold;
		Hold.WaitingOn = Agent.GetWaitingOn();
		if (Hold.WaitingOn == 0)
		{
			return Hold;
		}
		Hold.StalledSeconds = Agent.GetStalledSeconds();
		// READ WHENEVER WaitingOn IS SET, although BlockedResource's own comment calls it
		// meaningless with BlockedStep -1: Refuse writes the resource and the blocker together
		// (issue #174), so a set WaitingOn always came with the resource that refused it.
		const FTrafficResource& Resource = Agent.GetBlockedResource();
		switch (Resource.Kind)
		{
		case ETrafficResourceKind::Surface:
		{
			Hold.At = EHoldAt::Runway;
			// THE RUNWAY CARD'S OWN PAIR (DescribeRunway), so "holding short of 09/27" names the
			// strip exactly as clicking it would - not a second designator derivation here.
			FRunwayCardFacts Card;
			if (Network != nullptr && Resource.Surface.IsSet()
				&& DescribeRunway(*Network, Resource.Surface.Index, Card))
			{
				Hold.RunwayPair = Card.Pair;
			}
			break;
		}
		case ETrafficResourceKind::Edge:
			Hold.At = EHoldAt::Behind;
			break;
		case ETrafficResourceKind::Node:
		default:
			Hold.At = EHoldAt::Crossing;
			break;
		}
		return Hold;
	}

	FString HoldLine(const FAgentHold& Hold, const FString& BlockerName, const FString& Duration)
	{
		if (!Hold.IsSet())
		{
			return FString();
		}
		// FString::Format over NSLOCTEXT, not Printf - the aircraft card's own reason (issue #192):
		// UE 5.8's Printf wants a literal format, and these words are translatable.
		FString Line;
		switch (Hold.At)
		{
		case EHoldAt::Runway:
			Line = Hold.RunwayPair.IsEmpty()
				? FString::Format(*NSLOCTEXT("Airside", "HoldRunwayBare", "Holding short of runway for {0}").ToString(), { BlockerName })
				: FString::Format(*NSLOCTEXT("Airside", "HoldRunway", "Holding short of runway {0} for {1}").ToString(),
					{ Hold.RunwayPair, BlockerName });
			break;
		case EHoldAt::Behind:
			Line = FString::Format(*NSLOCTEXT("Airside", "HoldBehind", "Waiting behind {0}").ToString(), { BlockerName });
			break;
		case EHoldAt::Crossing:
		case EHoldAt::None:
		default:
			// NONE READS AS A CROSSING: HoldOf only leaves it None for an unset hold, returned above;
			// a node is also what a default FTrafficResource is.
			Line = FString::Format(*NSLOCTEXT("Airside", "HoldCrossing", "Waiting at crossing for {0}").ToString(), { BlockerName });
			break;
		}
		// The turnaround line's separator (UArrivalRowViewModel::DescribeTurnaround), so the card reads as one voice.
		return Duration.IsEmpty() ? Line : Line + TEXT(" · ") + Duration;
	}

	FString TypeNameOf(const FRoadAgent& Agent)
	{
		return !Agent.TypeCode().IsNone()
			? Agent.TypeCode().ToString()
			: FString(Agent.Class == ETraversalClass::Aircraft ? TEXT("Aircraft") : TEXT("Vehicle"));
	}

	FString PushbackText(EPushbackNeed Need)
	{
		switch (Need)
		{
		case EPushbackNeed::SelfManoeuvre: return TEXT("reverses itself");
		case EPushbackNeed::HandTug:       return TEXT("needs a hand tug");
		case EPushbackNeed::VehicleTug:    return TEXT("needs a tug");
		}
		return FString();
	}

	namespace
	{
		/** See DescribeRunwayCountForTest. Game thread only. Prefixed for the unity build. */
		int32 GDescribeRunwayCalls = 0;
	}

	int32 DescribeRunwayCountForTest()
	{
		return GDescribeRunwayCalls;
	}

	bool DescribeRunway(const URoadNetwork& Network, int32 SegmentIndex, FRunwayCardFacts& Out)
	{
		++GDescribeRunwayCalls;
		const FRoadSegmentId Segment = Network.SegmentIdAt(SegmentIndex);
		const FRoadSegment* Found = Segment.IsSet() ? Network.GetSegment(Segment) : nullptr;
		const FRoadNode* A = Found != nullptr ? Network.GetNode(Found->A) : nullptr;
		FRunwayEnd End;
		if (A == nullptr || !Network.IsRunwaySegment(Segment) || !Network.InUseRunwayAt(A->Position, End))
		{
			return false;
		}
		const FRunwayFacts Facts = Network.RunwayFactsFor(Segment);
		Out.Pair = RunwayDesignator::ToPairText(End.Direction);
		Out.InUse = RunwayDesignator::Designate(End.Direction);
		Out.Other = RunwayDesignator::Reciprocal(Out.InUse);
		Out.Surface = Facts.Surface;
		Out.Approach = Facts.Approach;
		Out.Use = RunwayUse::Resolve(Facts.Use);
		Out.Length = End.Length;
		return true;
	}

	bool DescribeTaxiway(const URoadNetwork& Network, int32 SegmentIndex, FTaxiwayCardFacts& Out)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(SegmentIndex);
		if (!Id.IsSet() || !TaxiwayStrip::HasStrip(Network, Id))
		{
			return false;
		}
		const FRoadSegment& Segment = *Network.GetSegment(Id);
		const double Width = Network.ProfileFor(Segment)->GetTotalWidth();   // HasStrip proved it non-null
		const EIcaoCode Own = IcaoCode::TaxiwayLetterForWidth(Width);
		// THE STORED LETTER, what the guideline builder capped the edges with - so the card says
		// what routing does, not what a fresh pass might say mid-edit.
		const EIcaoCode Operates = TaxiwayRestriction::EffectiveLetterOf(Network, Id).Get(Own);
		Out.Index = SegmentIndex;
		Out.Letter = IcaoCode::ToLetter(Own);
		Out.Width = Width;
		Out.Strip = TaxiwayStrip::StripWidthOf(Network, Id);   // THE strip - see its ruling
		Out.MaxWingspan = IcaoCode::MaxWingspanForLetter(Operates);
		Out.Surface = Segment.Surface;
		// THE NAME AND THE CHAIN (taxiway naming spec 2026-10-02): the card is the taxiway's, not just this segment's.
		Out.Taxiway = Network.TaxiwayOf(Id);
		Out.Name = Network.TaxiwayDisplayName(Out.Taxiway);
		Out.Length = Out.Taxiway != INDEX_NONE ? Network.TaxiwayChainOf(Out.Taxiway).Length : 0.0;
		Out.Connectors = Network.TaxiwayConnectorCount(Out.Taxiway);
		Out.RestrictedTo.Reset();
		Out.RestrictedBy.Reset();
		if (Operates != Own)
		{
			Out.RestrictedTo = FString(IcaoCode::ToLetter(Operates));
			TaxiwayRestriction::FObstruction Worst;
			if (TaxiwayRestriction::RestrictionOf(Network, Id, &Worst).IsSet())
			{
				Out.RestrictedBy = TaxiwayRestriction::Describe(Network, Worst);
			}
		}
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
		Out.Number = E.StandNumber;
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

		// CLOSED BY A STRIP (strip stage 6) - admission's own rule, so the card and the refusal
		// agree. A depot is never "closed": the strip closes stands to ARRIVALS.
		Out.ClosedBecause.Reset();
		if (E.IsStand())
		{
			if (const TOptional<TaxiwayStrip::FIntrusion> Closure = StandAdmission::StripClosure(Network, E))
			{
				Out.ClosedBecause = StandAdmission::DescribeClosure(Closure.GetValue());
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
