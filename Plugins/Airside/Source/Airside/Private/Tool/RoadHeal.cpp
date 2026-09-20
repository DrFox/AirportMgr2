#include "Tool/RoadHeal.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadSnap.h"

int32 RoadHeal::DegreeWithout(const URoadNetwork& Network, FRoadNodeId Neighbour, FRoadNodeId Target)
{
	const FRoadNode* Node = Network.GetNode(Neighbour);
	if (Node == nullptr)
	{
		return 0;
	}

	int32 Kept = 0;
	for (const FRoadSegmentId& Incident : Node->Incident)
	{
		if (Network.GetOtherEnd(Incident, Neighbour) != Target)
		{
			++Kept;
		}
	}
	return Kept;
}

namespace
{
	/** A snap result naming an existing node, which is what Validate needs to judge a rejoin. */
	FRoadSnapResult AsNodeSnap(const URoadNetwork& Network, FRoadNodeId Node)
	{
		FRoadSnapResult Snap;
		Snap.Kind = ERoadSnapKind::Node;
		Snap.Node = Node;
		if (const FRoadNode* Live = Network.GetNode(Node))
		{
			Snap.Position = Live->Position;
		}
		return Snap;
	}
}

FRoadDeletionPlan RoadHeal::PlanNodeDeletion(const URoadNetwork& Network,
	FRoadNodeId Target, const FRoadPlacementLimits& Limits)
{
	FRoadDeletionPlan Plan;
	Plan.Target = Target;

	const FRoadNode* Node = Network.GetNode(Target);
	if (Node == nullptr)
	{
		Plan.Refusal = ERoadPlacement::NoStart;
		return Plan;
	}

	const FVector2D TargetPosition = Node->Position;

	// Walked in incidence order, which URoadNetwork keeps sorted by bearing - so the plan
	// is the same every time it is computed for the same graph, and the overlay does not
	// flicker between two equally good answers.
	TArray<FRoadNodeId> Neighbours;

	// Taken from the live graph, because by the time a rejoin is added the arms it is
	// replacing are gone. A healed road inherits the profile of an arm that met here; with
	// arms of differing profiles that is a genuine merge policy, and this picks the first
	// rather than pretending to decide. Invisible while one profile is in use.
	URoadProfile* HealProfile = nullptr;

	for (const FRoadSegmentId& Incident : Node->Incident)
	{
		Plan.Doomed.Add(Incident);

		if (HealProfile == nullptr)
		{
			if (const FRoadSegment* Arm = Network.GetSegment(Incident))
			{
				HealProfile = Arm->Profile;
			}
		}

		const FRoadNodeId Other = Network.GetOtherEnd(Incident, Target);
		if (Other.IsSet())
		{
			Neighbours.AddUnique(Other);
		}
	}

	// Nothing to rejoin to. Whatever is left holding no road goes with it.
	// A RUNWAY IS A THROUGH-ROUTE, and its junctions are branches off it. Reported from
	// play as "once you have connected a runway to a taxiway there is no way to disconnect
	// it": the generic rule below picks the anchor by "keeps the most roads, then nearest",
	// and at a runway junction every neighbour keeps zero - so it grafted the TAXIWAY onto
	// a runway THRESHOLD and refused the whole deletion when that turn came out too sharp.
	// Nothing ever rejoined the runway to itself, so a deletion that did succeed severed it.
	//
	// Both are the same mistake: treating three arms as interchangeable when two of them
	// are one road passing through. So the runway is named as the through-route here, and
	// the branch is simply let go - which IS "delete the taxiway, keep the runway", from
	// the gesture the player already makes.
	//
	// EXACTLY TWO RUNWAY ARMS is what makes a node an interior junction of a strip. One arm
	// is a threshold with something attached, and it has no through-route to preserve; more
	// than two cannot happen on a chain that URoadEditFacade::MoveNode keeps straight.
	TArray<FRoadNodeId> RunwayNeighbours;
	URoadProfile* RunwayProfile = nullptr;
	for (const FRoadSegmentId& Incident : Node->Incident)
	{
		if (!Network.IsRunwaySegment(Incident))
		{
			continue;
		}
		const FRoadNodeId Other = Network.GetOtherEnd(Incident, Target);
		if (Other.IsSet())
		{
			RunwayNeighbours.AddUnique(Other);
			if (RunwayProfile == nullptr)
			{
				if (const FRoadSegment* Arm = Network.GetSegment(Incident))
				{
					RunwayProfile = Arm->Profile;
				}
			}
		}
	}
	const bool bRunwayThrough = RunwayNeighbours.Num() == 2;

	if (bRunwayThrough)
	{
		// THE RUNWAY'S OWN PROFILE, not whichever arm was stored first. HealProfile above
		// takes the first incident segment, which at this junction may well be the taxiway -
		// and relaying a runway with a taxiway's cross-section would quietly stop it being a
		// runway at all, since URoadNetwork::IsRunwaySegment reads exactly that.
		HealProfile = RunwayProfile;
	}

	if (Neighbours.Num() <= 1)
	{
		for (const FRoadNodeId& Lone : Neighbours)
		{
			if (DegreeWithout(Network, Lone, Target) == 0)
			{
				Plan.Swept.Add(Lone);
			}
		}
		Plan.bValid = true;
		return Plan;
	}

	// Who keeps the most roads, nearest winning ties. At degree 2 this is just "the better
	// connected end", and the other end rejoins it whether or not it was going to be
	// stranded - see the header for why that case is not restricted to orphans.
	// THE RUNWAY PICKS ITS OWN ANCHOR: one end of the strip, so the other rejoins to it and
	// the strip is whole again. Left to the generic rule below, the anchor would be whichever
	// neighbour happened to be nearest - the taxiway, as often as not.
	int32 BestKept = -1;
	double BestDistanceSquared = 0.0;
	if (bRunwayThrough)
	{
		Plan.Anchor = RunwayNeighbours[0];
	}
	for (const FRoadNodeId& Candidate : bRunwayThrough ? TArray<FRoadNodeId>() : Neighbours)
	{
		const FRoadNode* Live = Network.GetNode(Candidate);
		if (Live == nullptr)
		{
			continue;
		}

		const int32 Kept = DegreeWithout(Network, Candidate, Target);
		const double DistanceSquared = FVector2D::DistSquared(Live->Position, TargetPosition);
		if (Kept > BestKept || (Kept == BestKept && DistanceSquared < BestDistanceSquared))
		{
			BestKept = Kept;
			BestDistanceSquared = DistanceSquared;
			Plan.Anchor = Candidate;
		}
	}

	if (!Plan.Anchor.IsSet())
	{
		Plan.Refusal = ERoadPlacement::NoStart;
		return Plan;
	}

	// CARRIED OUT WITH THE PLAN, so the segment the facade lays is the one this validated.
	Plan.HealProfile = HealProfile;

	const bool bAlwaysHeal = Neighbours.Num() == 2;

	// The graph as it will be. Every judgement below is made against this, never against
	// the live one - see the header.
	URoadNetwork* Scratch = DuplicateObject<URoadNetwork>(&Network, GetTransientPackage());
	if (Scratch == nullptr)
	{
		Plan.Refusal = ERoadPlacement::NoStart;
		return Plan;
	}
	Scratch->RemoveNode(Target);

	for (const FRoadNodeId& Neighbour : Neighbours)
	{
		if (Neighbour == Plan.Anchor)
		{
			continue;
		}

		// At degree 3 or more, a neighbour that keeps roads of its own is not stranded and
		// wants no new road inventing for it.
		if (!bAlwaysHeal && DegreeWithout(Network, Neighbour, Target) > 0)
		{
			continue;
		}

		// ONLY THE FAR END OF THE RUNWAY REJOINS. Every other arm is a branch off the strip
		// and is let go - grafting one onto a threshold is the nonsense this case exists to
		// stop. A branch left holding nothing is swept by the loop below, exactly as before.
		if (bRunwayThrough && Neighbour != RunwayNeighbours[1])
		{
			continue;
		}

		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Scratch, Neighbour, AsNodeSnap(*Scratch, Plan.Anchor), Limits);

		// Already joined is not a failure - the road that would have been added is already
		// there, so the neighbour is not stranded and there is nothing to do for it.
		if (Judgement == ERoadPlacement::AlreadyJoined)
		{
			continue;
		}

		if (Judgement != ERoadPlacement::Valid)
		{
			Plan.Refusal = Judgement;
			Plan.RefusedNeighbour = Neighbour;
			Plan.Rejoin.Reset();
			return Plan;
		}

		Plan.Rejoin.Add(Neighbour);

		// Applied before the next is judged, so a second arm landing on top of this one at
		// the anchor is caught rather than waved through.
		Scratch->AddStraightSegment(Neighbour, Plan.Anchor, HealProfile);
	}

	// Anything the plan leaves holding no road at all.
	for (const FRoadNodeId& Neighbour : Neighbours)
	{
		if (DegreeWithout(Network, Neighbour, Target) == 0
			&& Neighbour != Plan.Anchor
			&& !Plan.Rejoin.Contains(Neighbour))
		{
			Plan.Swept.Add(Neighbour);
		}
	}

	Plan.bValid = true;
	return Plan;
}
