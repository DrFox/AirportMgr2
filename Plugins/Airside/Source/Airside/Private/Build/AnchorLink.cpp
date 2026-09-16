#include "Build/AnchorLink.h"

#include "AirsideLog.h"
#include "Build/AnchorLinkFinder.h"
#include "Build/StandLaneBuild.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"

namespace
{
	// WHERE THE RING'S SIDE-FINDING APPARATUS WENT, 2026-09-16.
	//
	// Four helpers stood here - NearestRoadApproach, IsLaneBend, WholeSide and
	// SideAlreadyLinked - and together they answered "which sides of this stand's ring are
	// beside a road, and which of them has an entrance already". They existed because the
	// entrance was DISCOVERED: the ring was searched for its nearest approach to a road and
	// cut wherever that fell, so the pass had to reconstruct what a "side" even was from a
	// graph that anchor spurs had already chopped into pieces.
	//
	// A stand's lane now DECLARES where a road may join - UEntityDefinition::ServiceLane
	// carries Entry waypoints, and FStandLaneBuild::FResult::Entries reports the node each one
	// became - so there is nothing left to reconstruct and the whole apparatus goes with the
	// question. Gather links through those entries instead; what survives of the choosing is
	// two measurements with no threshold between them, both in the entries loop there.
	//
	// The deleted code is in git at the commit before this one; it is not worth carrying
	// commented out, because what replaces it is a different question and not a better answer
	// to this one.

	/**
	 * Minimum centreline curve radius, in uu, for an ICAO aerodrome code letter.
	 *
	 * A taxi line is PAINTED, and the pilot follows it with the nose wheel - so its radius
	 * belongs to the STAND, sized for the largest aircraft that stand admits, and never to
	 * whichever aircraft happens to be taxiing. One line, one curve, every type follows it.
	 *
	 * PROVENANCE, stated plainly as UAircraftType does for its door stations: these are
	 * standard aerodrome design values by code letter, not figures lifted from a specific
	 * Annex 14 edition. They are what to check first if a real layout looks wrong - but the
	 * SHAPE of the rule, radius by code letter, is how aerodromes are actually dimensioned.
	 */
	double RadiusForCode(FName Code)
	{
		// The table itself is Solve/IcaoCode.h now - shared with RunwayAdmission (width ->
		// wingspan) and InspectFacts (wingspan -> letter). See #85.
		return IcaoCode::RadiusForLetter(Code.ToString());
	}

	/**
	 * Which way a link LEAVES a declared entry, and how much lane there is that way.
	 *
	 * A LINE JOINS A LANE ALONG IT, NOT ACROSS IT - see GuidelineGeom::ShiftDeflectionFor for
	 * the construction and Join for what it delivers - so the one thing the join needs from the
	 * graph here is the direction of the lane AT the entry. The lane offers two, one per
	 * incident leg, and they are exact opposites: the lane is tangent-continuous at an entry by
	 * construction, because a bend's control IS the corner it rounds.
	 *
	 * TOWARD THE ROAD WHEN THE LANE POINTS THAT WAY AT ALL. An entry across the END of a stand
	 * has a road roughly ahead of it, and leaving backwards to reach it is a curve that swings
	 * out over the lane's own crossing before it gets anywhere.
	 *
	 * AND TOWARD THE CORNER WHEN THE ROAD IS SQUARE ABEAM, which is a tie the dot product
	 * cannot break and the TRAFFIC can. A tangential join is smooth in ONE direction: a vehicle
	 * coming up the connector arrives at the entry heading AWAY from the control, so whichever
	 * side the control is on is the side that gets a U-turn. Putting it on the corner side
	 * leaves the STRAIGHT smooth - which is the side the stand's plant is on, a run of 47 m
	 * against a bend of 3 - and that is the journey every service vehicle actually makes.
	 * Measured the other way round first: the route from a road alongside to the ground power
	 * came in at the near entry and reversed through 179 degrees to get to it, which a truck
	 * cannot do at all under the rolling-steer law.
	 *
	 * HOW MUCH ROOM each leg has decides nothing but that TIE, and is not reported: the control
	 * goes on the tangent LINE at the entry, and a transition wide enough to clear a truck's
	 * lock regularly wants more run than the leg it leaves on is long. Past the corner that line
	 * leaves a lane whose every turn bends the same way, so it lands OUTSIDE the cycle on the
	 * side the road is, and the curve's own hull stays between the two. Capping the run to the
	 * leg instead is what delivered 67 uu of radius where 699 was needed.
	 *
	 * ANALYTIC, never a difference of samples: a quadratic's first sampled chord is a degree or
	 * two off its true tangent, and a heading taken from the chord describes a turn no follower
	 * makes.
	 *
	 * False when the node has no live lane edge on it at all, which is a lane laid by something
	 * other than FStandLaneBuild. The caller then falls back to the straight lead-in rather
	 * than guessing a heading.
	 */
	bool EntryDeparture(const URoadNetwork& Network, FGuidelineNodeId NodeId,
		const FVector2D& Toward, FVector2D& OutAlong)
	{
		const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
		if (Node == nullptr)
		{
			return false;
		}

		bool bFound = false;
		bool bBestIsBend = false;
		double BestToward = 0.0;
		double BestRoom = 0.0;
		for (const FGuidelineEdgeId& Id : Node->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id);
			if (Edge == nullptr || !Edge->bAlive || !Edge->StandGeometryOwner.IsSet())
			{
				// The lane only. A link edge already on this node - the second pass's case -
				// is not a leg of anything and has no heading to offer.
				continue;
			}

			const FGuidelineNode* A = Network.GetGuidelineNode(Edge->A);
			const FGuidelineNode* B = Network.GetGuidelineNode(Edge->B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			const bool bFromB = Edge->B == NodeId;
			const FVector2D Tangent = GuidelineGeom::Tangent(
				A->Position, Edge->Control, B->Position, bFromB ? 1.0 : 0.0);
			const FVector2D Along = (bFromB ? -Tangent : Tangent).GetSafeNormal();
			if (Along.IsNearlyZero())
			{
				continue;
			}

			const bool bBend = !GuidelineGeom::IsStraight(A->Position, Edge->Control, B->Position);
			const double Room = bBend
				? FVector2D::Distance(bFromB ? B->Position : A->Position, Edge->Control)
				: FVector2D::Distance(A->Position, B->Position);

			// THE ORDER OF THE THREE TESTS IS THE RULE ITSELF: toward the road first, then the
			// corner side, then room. The second and third only ever decide a tie in the first,
			// because the two legs' headings are exact opposites and so are their dot products.
			const double Dot = FVector2D::DotProduct(Along, Toward);
			const double Margin = Dot - BestToward;
			const bool bBetter = !bFound
				|| Margin > UE_DOUBLE_KINDA_SMALL_NUMBER
				|| (FMath::Abs(Margin) <= UE_DOUBLE_KINDA_SMALL_NUMBER
					&& (bBend != bBestIsBend ? bBend : Room > BestRoom));
			if (bBetter)
			{
				OutAlong = Along;
				BestRoom = Room;
				BestToward = Dot;
				bBestIsBend = bBend;
				bFound = true;
			}
		}
		return bFound;
	}

	/**
	 * The other end of the BEND at this entry's corner, when that end is a declared entry too.
	 *
	 * ONE ENTRY, TWO NODES. The pair is recognised off the graph rather than reported by the
	 * builder because the graph already says it exactly - the lane edge joining two entry nodes
	 * IS the bend that rounds their corner - and a second statement of the same fact would be
	 * one more thing to keep in step with FStandLaneBuild::FResult::Entries, which is itself
	 * re-gathered from graph order on an idempotent pass.
	 *
	 * Unset for a straight-through entry, which is one node and no bend.
	 */
	FGuidelineNodeId BendSibling(const URoadNetwork& Network, FGuidelineNodeId NodeId,
		const TArray<FGuidelineNodeId>& Entries)
	{
		const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
		if (Node == nullptr)
		{
			return FGuidelineNodeId();
		}

		for (const FGuidelineEdgeId& Id : Node->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id);
			if (Edge == nullptr || !Edge->bAlive || !Edge->StandGeometryOwner.IsSet())
			{
				continue;
			}

			const FGuidelineNode* A = Network.GetGuidelineNode(Edge->A);
			const FGuidelineNode* B = Network.GetGuidelineNode(Edge->B);
			if (A == nullptr || B == nullptr
				|| GuidelineGeom::IsStraight(A->Position, Edge->Control, B->Position))
			{
				continue;
			}

			const FGuidelineNodeId Other = Edge->A == NodeId ? Edge->B : Edge->A;
			if (Entries.Contains(Other))
			{
				return Other;
			}
		}
		return FGuidelineNodeId();
	}
}

void FAnchorLink::Gather(URoadNetwork& Network, double MaxLeadIn, double ServiceLinkRadius,
	TArray<FPendingLink>& OutPending, TSet<FGuidelineNodeId>& OutAnchorNodes)
{
	// THE LANES FIRST. A service anchor is a waypoint ON its stand's lane, so it is already
	// joined by the time the walk below asks, and is skipped there rather than cast at a road
	// on the far side of the aeroplane - and the LANE becomes the thing that links.
	const FStandLaneBuild::FResult Lanes = FStandLaneBuild::Build(Network);

	// EVERY NODE A LINK MUST NOT TARGET: anchor and pose nodes as always, PLUS every node of
	// every service lane. A lane is itself a vehicle guideline, so without the second half a
	// lane would join ITSELF - four metres away, across its own box - every stand would read
	// as connected, and no truck would ever route anywhere.
	OutAnchorNodes = Lanes.Nodes;

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		// The stand's design aircraft is what its lead-in can take. A Code C stand's link
		// then refuses a widebody by the ordinary wingspan rule rather than by a special
		// case, and the search reports TooWide instead of a bare "no route".
		const double StandWingspan = Instance.Definition->DesignAircraft != nullptr
			? Instance.Definition->DesignAircraft->Footprint.Wingspan
			: 0.0;

		// The same design aircraft decides how wide the painted line sweeps. Its CODE
		// LETTER, not its span: aerodromes are dimensioned by code letter, and the line is
		// laid once for the largest aircraft the stand admits.
		const double StandRadius = RadiusForCode(
			Instance.Definition->DesignAircraft != nullptr
				? Instance.Definition->DesignAircraft->Code
				: FName());

		// The pose first. It is not an anchor - see FEntityInstance::PoseNode - but it needs
		// a lead-in for exactly the same reason, and it is the one the entity's OWN traffic
		// is routed to: an aircraft to a stand's stop mark, a truck to a depot's bay.
		//
		// The ray leaves along the entity's heading PLUS 180: +X faces the terminal (or, for
		// a depot, its own building), so the lead-in runs back out of it to the movement
		// area. Cast the other way and every stand would try to join a guideline inside the
		// terminal.
		//
		// WHICH CLASS OF LINE IT MAY JOIN comes from the instance's PoseRole. This was
		// Aircraft unconditionally, which is right for a stand and silently wrong for
		// anything else: a fuel depot's pose found no aircraft guideline, joined nothing, and
		// logged "joins nothing" on every rebuild for ever.
		OutAnchorNodes.Add(Instance.PoseNode);
		if (const FGuidelineNode* Pose = Network.GetGuidelineNode(Instance.PoseNode);
			Pose != nullptr && Pose->Incident.Num() == 0)
		{
			// ONE RAY, PARAMETERISED BY DIRECTION, because a taxi-through stand casts two and
			// everything about them but the heading is identical. Written as a lambda rather
			// than duplicated: two copies of this block would be two places to keep the
			// wingspan limit, the reach and the strategy in step.
			const auto AddPoseLink = [&](double Out)
			{
				FPendingLink Link;
				Link.Node = Instance.PoseNode;
				Link.At = Pose->Position;
				Link.Dir = FVector2D(FMath::Cos(Out), FMath::Sin(Out));
				Link.Class = TraversalForRole(Instance.PoseRole);

				// THE STRATEGY, decided once here rather than re-derived at search time - see
				// ELinkKind. Aircraft casts a ray along Dir; everything else measures proximity.
				Link.Kind = Link.Class == ETraversalClass::Aircraft ? ELinkKind::Ray : ELinkKind::Proximity;

				// A span limit on a line no wing uses could never bind - 0 is UNLIMITED (see
				// FProfileGuideline::MaxWingspan), and the CLASS has already refused aircraft.
				Link.MaxWingspan = Link.Class == ETraversalClass::Aircraft ? StandWingspan : 0.0;
				Link.Radius = StandRadius;

				// WHICH RULE, and therefore how far. An aircraft casts its painted line 200 m; a
				// service pose - a depot's truck bay - measures 50 m in any direction, because a
				// van is not following paint and the player has no way to see an authored heading.
				Link.Reach = Link.Class == ETraversalClass::Aircraft ? MaxLeadIn : ServiceLinkRadius;
				OutPending.Add(Link);
			};

			AddPoseLink(Instance.Heading + UE_DOUBLE_PI);

			// A SECOND RAY, FORWARD, for a stand with pavement on both sides. The first runs
			// BACK out of the entity because +X faces the terminal; this one is the declared
			// exception - there is no terminal that side, so +X finds pavement too, and an
			// aeroplane that parks here drives straight out instead of being pushed.
			//
			// THE FLAG IS AUTHORED AND NOT MEASURED HERE. Casting speculatively and keeping
			// whatever it hit would join a stand to a taxiway on the far side of a terminal
			// building, which the graph has no way to know is not pavement.
			//
			// WHETHER A GIVEN AEROPLANE THEN NEEDS A PUSH IS NOT DECIDED HERE EITHER: that is
			// measured off the route it is actually given - see UGroundTraffic::DepartAgent -
			// which is why nothing in Model/ reads this flag. All this does is give the pose
			// node a way OUT as well as a way in.
			if (Instance.Definition != nullptr && Instance.Definition->bTaxiThrough)
			{
				AddPoseLink(Instance.Heading);
			}
		}

		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			OutAnchorNodes.Add(Resolved.Node);

			const FGuidelineNode* Node = Network.GetGuidelineNode(Resolved.Node);
			if (Node == nullptr || Node->Incident.Num() > 0)
			{
				// Already joined - by a previous pass that survived, or by hand. Either
				// way, adding a second lead-in would leave two lines into one nose-stop.
				continue;
			}

			double Heading = 0.0;
			if (!Network.GetAnchorWorldHeading(EntityId, Resolved.Id, Heading))
			{
				continue;
			}

			// The role lives on the definition, addressed by id - never by position in the
			// array, which is the invariant FResolvedAnchor exists to remove.
			const FEntityAnchor* Declared = Instance.Definition->Anchors.FindByPredicate(
				[&Resolved](const FEntityAnchor& Candidate) { return Candidate.Id == Resolved.Id; });
			if (Declared == nullptr)
			{
				continue;
			}

			FPendingLink Link;
			Link.Node = Resolved.Node;
			Link.At = Node->Position;
			Link.Dir = FVector2D(FMath::Cos(Heading), FMath::Sin(Heading));
			Link.Class = TraversalForRole(Declared->Role);
			Link.Kind = Link.Class == ETraversalClass::Aircraft ? ELinkKind::Ray : ELinkKind::Proximity;
			Link.MaxWingspan = Link.Class == ETraversalClass::Aircraft ? StandWingspan : 0.0;
			Link.Radius = StandRadius;
			Link.Reach = Link.Class == ETraversalClass::Aircraft ? MaxLeadIn : ServiceLinkRadius;
			OutPending.Add(Link);
		}

		// THE LANE'S OWN LINK TO A ROAD IS NOT CAST HERE. It leaves a DECLARED ENTRY, and every
		// declared entry of every stand is gathered in one pass of its own below - see there
		// for why it cannot be done inside this loop.
	}

	// A ROAD JOINS A STAND WHERE THE STAND SAYS IT MAY, 2026-09-16.
	//
	// ONE LINK PER DECLARED ENTRY. UEntityDefinition::ServiceLane authors Entry waypoints and
	// FStandLaneBuild::FResult::Entries reports the node each one became, so this asks only
	// whether a road is within reach of each - never WHERE on the lane an entrance should go,
	// which is what the deleted per-side search spent four helpers and three mutually-tuned
	// thresholds answering. See the note at the top of this file for what went and why.
	//
	// A SECOND PASS, after the loop above, for a reason worth stating: OutAnchorNodes is not
	// complete until every entity's pose and anchor nodes are in it, and a link resolved
	// against a partial exclusion set would depend on the order entities happen to sit in the
	// array. The lane nodes were already there from the top; the pose and anchor nodes were not.
	//
	// MEASURED BEFORE ANYTHING IS JOINED, which is the same property one level up: joining
	// splits the road it joins, so an entry measured after that is measuring a different
	// airport from the one measured before it. Resolve is read-only precisely so this is legal.
	for (const TPair<FEntityInstanceId, TArray<FGuidelineNodeId>>& Declared : Lanes.Entries)
	{
		const FEntityInstance* Instance = Network.GetEntity(Declared.Key);
		if (Instance == nullptr || Instance->Definition == nullptr)
		{
			continue;
		}

		// The same painted radius the stand's other links sweep at - see RadiusForCode. It
		// sizes the fillet where this connector meets the ROAD, not the lane.
		const double StandRadius = RadiusForCode(
			Instance->Definition->DesignAircraft != nullptr
				? Instance->Definition->DesignAircraft->Code
				: FName());

		// ONE STATEMENT OF WHAT AN ENTRY LINK IS, used by the probe below and by the link
		// finally emitted. Written twice they could differ, and the probe would then be
		// measuring something other than the link it decides.
		auto EntryLink = [&Declared, StandRadius, ServiceLinkRadius](
			FGuidelineNodeId NodeId, const FVector2D& At)
		{
			FPendingLink Link;

			// PROXIMITY, not a ray. A vehicle may genuinely arrive from any side, and the
			// player has no authored heading to aim at - the same reasoning that governs every
			// other service link, and the reason a declared entry needs no rule of its own.
			Link.Kind = ELinkKind::Proximity;
			Link.Node = NodeId;
			Link.At = At;
			Link.Class = ETraversalClass::GroundVehicle;

			// WHICH STAND, which is what makes this a lane link: Join reads it to lay the
			// lead-in along the lane, and Build reads it to report a stand that joined nothing
			// once rather than once per entry.
			Link.LaneOwner = Declared.Key;

			// 0 is UNLIMITED (see FProfileGuideline::MaxWingspan). A span limit on a line no
			// wing uses could never bind, and the class has already refused aircraft.
			Link.MaxWingspan = 0.0;
			Link.Radius = StandRadius;
			Link.Reach = ServiceLinkRadius;
			return Link;
		};

		// WHAT EACH ENTRY NODE CAN REACH: the road point it would join, and how far off it is.
		struct FEntryReach
		{
			FGuidelineNodeId Node;
			FVector2D At = FVector2D::ZeroVector;
			FVector2D Contact = FVector2D::ZeroVector;
			double Distance = TNumericLimits<double>::Max();
			bool bReaches = false;
		};

		TMap<FGuidelineNodeId, FEntryReach> Reach;
		for (const FGuidelineNodeId& NodeId : Declared.Value)
		{
			const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
			if (Node == nullptr)
			{
				continue;
			}

			FEntryReach Found;
			Found.Node = NodeId;
			Found.At = Node->Position;

			const FPendingLink Probe = EntryLink(NodeId, Node->Position);
			if (const FLinkHit Hit = Resolve(Network, Probe, OutAnchorNodes); Hit.IsSet())
			{
				const FGuidelineEdge* Road = Network.GetGuidelineEdge(Hit.Edge);
				const FGuidelineNode* RoadA =
					Road != nullptr ? Network.GetGuidelineNode(Road->A) : nullptr;
				const FGuidelineNode* RoadB =
					Road != nullptr ? Network.GetGuidelineNode(Road->B) : nullptr;
				if (RoadA != nullptr && RoadB != nullptr)
				{
					Found.Contact = GuidelineGeom::Eval(
						RoadA->Position, Road->Control, RoadB->Position, Hit.Param);
					Found.Distance = FVector2D::Distance(Found.At, Found.Contact);
					Found.bReaches = true;
				}
			}
			Reach.Add(NodeId, Found);
		}

		// EVERY DECLARED ENTRY, AS THE PAIR OR THE SINGLE NODE IT REACHED THE GRAPH AS. See
		// BendSibling for why the pair is read off the bend rather than reported by the builder.
		TArray<TArray<FGuidelineNodeId>> Corners;
		TSet<FGuidelineNodeId> Grouped;
		for (const FGuidelineNodeId& NodeId : Declared.Value)
		{
			if (Grouped.Contains(NodeId))
			{
				continue;
			}
			Grouped.Add(NodeId);

			TArray<FGuidelineNodeId>& Corner = Corners.AddDefaulted_GetRef();
			Corner.Add(NodeId);

			const FGuidelineNodeId Other = BendSibling(Network, NodeId, Declared.Value);
			if (Other.IsSet() && !Grouped.Contains(Other))
			{
				Grouped.Add(Other);
				Corner.Add(Other);
			}
		}

		for (const TArray<FGuidelineNodeId>& Corner : Corners)
		{
			// ALREADY JOINED, ASKED OF THE WHOLE CORNER AND NOT OF ONE NODE OF IT. Two lane
			// edges is an unjoined node; more means a road is already on it, by a previous pass
			// that survived or by hand. Asked of one node, the OTHER end of a joined corner
			// reads as free and takes a second entrance 300 uu from the first - which the
			// rebuild-on-every-edit contract would then do again on every pass.
			bool bJoined = false;
			for (const FGuidelineNodeId& NodeId : Corner)
			{
				const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
				bJoined = bJoined || (Node == nullptr || Node->Incident.Num() > 2);
			}
			if (bJoined)
			{
				continue;
			}

			// THE NEARER OF THE PAIR. A rounded corner offers one node on each of its two legs,
			// each with a clean heading along its own straight, and which of them a road should
			// take depends on which side that road is: one approaching along the run wants the
			// run's node, one approaching across the end wants the crossing's. Nearest says
			// that exactly, and both would be two entrances at one declared entry.
			const FEntryReach* Best = Reach.Find(Corner[0]);
			for (const FGuidelineNodeId& NodeId : Corner)
			{
				const FEntryReach* Other = Reach.Find(NodeId);
				if (Other != nullptr && (Best == nullptr || Other->Distance < Best->Distance))
				{
					Best = Other;
				}
			}
			if (Best == nullptr)
			{
				continue;
			}

			// AND THE ENTRY NEAREST A POINT OF ROAD IS THE ONE THAT GETS IT.
			//
			// THIS IS WHAT REFUSES THE FAR SIDE, and it refuses it by measuring rather than by
			// a threshold. A stand's far entries are within reach of a road alongside it - the
			// lane is only 17 m deep - and their connectors would run the whole depth of the
			// stand, across the lane's own crossings, to reach a road the near entry is 4 m
			// from. It refuses the same way a road at one END of the stand is refused by the
			// far end's entries, which no distance threshold on its own ever got right: the
			// question is not "how far is this entry" but "is this entry the nearest thing to
			// the road it is claiming".
			//
			// AGAINST EVERY ENTRY NODE, not only the ones selected above, because the answer
			// must not depend on which corner is considered first. The globally nearest
			// candidate can never be refused by it - anything nearer to ITS contact point would
			// have a shorter reach of its own - so a stand with a road in reach always keeps at
			// least one entrance.
			if (Best->bReaches)
			{
				bool bNearerEntry = false;
				for (const FGuidelineNodeId& NodeId : Declared.Value)
				{
					const FGuidelineNode* Node =
						NodeId != Best->Node ? Network.GetGuidelineNode(NodeId) : nullptr;
					bNearerEntry = bNearerEntry
						|| (Node != nullptr
							&& FVector2D::Distance(Node->Position, Best->Contact) < Best->Distance);
				}
				if (bNearerEntry)
				{
					continue;
				}
			}

			// EMITTED EVEN WHEN THE PROBE FOUND NOTHING, deliberately: Build is what reports a
			// stand that joined nothing at all, and it can only do that for a link it was
			// given. A stand out of reach of every road therefore gets one warning line naming
			// it, not silence.
			OutPending.Add(EntryLink(Best->Node, Best->At));
		}
	}
}

FLinkHit FAnchorLink::Resolve(const URoadNetwork& Network, const FPendingLink& Link,
	const TSet<FGuidelineNodeId>& AnchorNodes)
{
	// STRATEGY DISPATCH. Left unset (FLinkHit::IsSet false) when the finder finds nothing -
	// Find never writes to Hit unless it claims, the same contract IRoadSnapRule's Resolve
	// documents for the same reason.
	FLinkHit Hit;
	LinkFinderFor(Link.Kind).Find(Network, Link, AnchorNodes, Hit);
	return Hit;
}

FGuidelineNodeId FAnchorLink::Join(URoadNetwork& Network, FPendingLink& Link, const FLinkHit& Hit,
	TSet<FGuidelineNodeId>& AnchorNodes)
{
	const FGuidelineEdge* Found = Network.GetGuidelineEdge(Hit.Edge);
	const FGuidelineNode* EndA = Found != nullptr ? Network.GetGuidelineNode(Found->A) : nullptr;
	const FGuidelineNode* EndB = Found != nullptr ? Network.GetGuidelineNode(Found->B) : nullptr;
	if (Found == nullptr || EndA == nullptr || EndB == nullptr)
	{
		return FGuidelineNodeId();
	}

	// Copied before anything is removed: Found points into the slot array, and adding
	// the halves can reallocate it.
	const FGuidelineEdge Original = *Found;
	const FVector2D PositionA = EndA->Position;
	const FVector2D PositionB = EndB->Position;

	// THROUGH THE GRAPH AGAIN, 2026-09-16, and SAMPLED FIRST. A direct GuidelineGeom::Sample
	// stood further down - the one deliberate exception to URoadNetwork::SampleGuideline being
	// the only graph-edge caller of it (PR #137 review, issue #105 item 5) - because a Lane join
	// had already split the LANE and the snapshot in Original was of an edge whose id no longer
	// resolved. Nothing splits before this point now: an entry is DECLARED, so there is no lane
	// cut to make, and Hit.Edge is still the road this link resolved against.
	//
	// UP HERE because a link leaving a lane RE-AIMS along the road below, and needs the
	// polyline to do it. Everything after measures against this same array.
	TArray<FVector2D> Curve;
	if (!Network.SampleGuideline(Hit.Edge, Curve))
	{
		return FGuidelineNodeId();
	}

	// WHERE THE LEAD-IN RAY STRIKES IS THE CORNER, NOT THE JOIN.
	//
	// Joining here is what produced a hard turn: the ray meets the taxiway at whatever
	// angle the stand happens to face, and a stand square to the taxiway makes it 90
	// degrees. A curve cannot fix that in place - a quadratic's end tangents both point
	// at its control, so being tangent to the lead-in AND to the taxiway would put the
	// control exactly here, which is the straight line again. The join has to MOVE.
	//
	// MUTABLE, AND SO IS THE PARAM IT COMES FROM, for one caller: a link leaving a lane is the
	// only one that re-aims. The finder answers "where is this road NEAREST me", which is the
	// right question for an anchor casting at a taxiway and the WRONG one for a lane change -
	// the nearest point is square abeam, and square abeam is the one place a transition has no
	// room to turn in. See the lane block below.
	double Param = Hit.Param;
	FVector2D Corner = GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Param);

	// SET ONLY BY A LINK THAT LEAVES A STAND'S LANE - see the block below, which is the only
	// writer. Everything else keeps the straight midpoint control, because a straight lead-in
	// is what it is.
	TOptional<FVector2D> OutLaneControl;

	if (Link.Kind != ELinkKind::Ray)
	{
		// The direction the join actually needs. A service link has no ray of its own - it
		// was found by DISTANCE - but the fillet and the two sweeps below are all written
		// in terms of a direction of arrival, and this is it.
		const FVector2D Toward = Corner - Link.At;
		if (!Toward.IsNearlyZero())
		{
			Link.Dir = Toward.GetSafeNormal();
		}
	}

	// A LINE JOINS A LANE ALONG IT, NOT ACROSS IT.
	//
	// A SQUARE-ON ENTRANCE IS A TIGHT TURN WE BUILT. However well the router is taught to avoid
	// tight turns it can only choose among the ones that exist, and a lane whose entrances met
	// the road at a right angle left exactly one tight option on every stand - which is the one
	// it kept taking. Reported from play 2026-09-15, twice. So the control point goes a RUN
	// along the lane's own heading at the entry and the lead-in LEAVES tangentially: a truck
	// turning off the road drives down the lane, and there is no heading change at the junction
	// at all. Which of the two headings, and how far, is EntryDeparture's business.
	//
	// THE OLD CONSTRUCTION SLID THE JOIN, because the ring's entrance was wherever the road
	// came nearest and the join could be put anywhere. This one cannot slide - the entry is
	// DECLARED - so it is the same triangle read the other way round: the declared node is the
	// join, the control is the run ahead of it, and Dir is taken from the CONTROL rather than
	// from the node, which is what keeps the fillet below tangent to this curve instead of to a
	// chord the curve never follows.
	//
	// THE RADIUS IS THE INPUT, NOT THE RUN, and that is the correction of 2026-09-16.
	// FStandLaneBuild::TangentRunFor governed the run for one round - it is deleted, and
	// StandLaneBuild.h says where its answer comes from now - and its 800 uu was measured
	// against a SPUR's 1000-1400 uu gaps. A road is three to five times that, and the same 800
	// delivered 40 uu of radius at 4 m and 67 at 54 against the 699.4 a real 8.5 m dispenser's
	// steering lock demands. A curve tighter than the lock is untakeable AT ANY SPEED, which is
	// the defect this whole piece of work exists to delete, so the figure cannot be a constant
	// tuned against one gap.
	//
	// TWO SHAPES, AND WHICH ONE IS A QUESTION ABOUT THE ROAD, not a preference. A road drawn
	// ALONGSIDE the stand is parallel to the lane and the two lines never meet, so the connector
	// is a lane change: an S of two curves, sized by GuidelineGeom::ShiftDeflectionFor. A road
	// drawn ACROSS the end of the stand CROSSES the lane's heading, and that wants the ordinary
	// thing - run on to where they meet and round the corner. The crossing is preferred wherever
	// it exists within this link's own reach and its corner fits in front of the entry, because
	// it leaves the lead-in dead straight and gives BOTH sweeps their radius rather than one: an
	// S has to slant onto the road, and turning the other way out of a slant is a U-turn that no
	// geometry fixes. Tried the other way round first, and a road across the nose came out with
	// a transition longer than the distance to the road and no link at all.
	//
	// PLUS A TENTH ON THE LOCK, measured rather than chosen. Sized at exactly the lock, the 4 m
	// fixture came out at 707 uu against 699.4 - a 1% margin, and the weld tolerance the fillet
	// gives up below is enough to eat it. A tenth is what the stand layout allows itself for the
	// same reason, at UEntityDefinition::BuildCodeCStandFor's LegSlack.
	//
	// KEPT AFTERWARDS: LaneRadius is what the fillet at the road must deliver (see Offset), and
	// LaneRun is the room the S has already spent along the lane (see LeadRoom).
	double LaneRun = 0.0;
	double LaneRadius = 0.0;
	if (Link.LaneOwner.IsSet())
	{
		FVector2D Along = FVector2D::ZeroVector;
		if (EntryDeparture(Network, Link.Node, Corner - Link.At, Along))
		{
			// THE LOCK OF THE LARGEST VEHICLE ADMITTED, never the one driving now - the same
			// call the lane's own corners are rounded by (FStandLaneBuild::Build) and the same
			// rule all this airport's ground geometry follows.
			constexpr double Slack = 1.1;
			LaneRadius = UAirsideSettings::ResolveLargestServiceVehicle()
				.TightestFollowableRadius() * Slack;

			// WHERE THE LANE'S OWN HEADING MEETS THE ROAD, if it does at all. Parallel lines
			// give a vanishing cross product and no crossing; a road BEHIND the entry gives a
			// negative distance, which is not a crossing this link can use either.
			const FVector2D RoadDir =
				GuidelineGeom::Tangent(PositionA, Original.Control, PositionB, Param);
			const double Converge = Along.X * RoadDir.Y - Along.Y * RoadDir.X;
			const FVector2D ToRoad = Corner - Link.At;
			const double MeetsAt = FMath::Abs(Converge) > UE_DOUBLE_KINDA_SMALL_NUMBER
				? (ToRoad.X * RoadDir.Y - ToRoad.Y * RoadDir.X) / Converge
				: -1.0;

			// AND WHETHER ITS CORNER FITS IN FRONT OF THE ENTRY. The turn onto the road is the
			// GENTLER of the two corners the connector makes with it - the one a truck joining
			// the traffic takes - and CornerRunFor says how much run that needs.
			const double Turn = FMath::Acos(FMath::Clamp(
				FMath::Abs(FVector2D::DotProduct(Along, RoadDir)), -1.0, 1.0));
			const double Needs = GuidelineGeom::CornerRunFor(LaneRadius, UE_DOUBLE_PI - Turn);

			if (MeetsAt > 0.0 && MeetsAt <= Link.Reach && Needs + LeadInWeldTolerance <= MeetsAt)
			{
				// THE CROSSING. Re-asked of the road rather than taken from the tangent line, so
				// the corner sits ON a road that bends; Dir then runs from the entry to it, and
				// the lead-in is straight - which is tangent to the lane at the entry by
				// construction, and needs no control point of its own to be so.
				int32 Span = 0;
				double Fraction = 0.0;
				GuidelineGeom::NearestOnPolyline(Curve, Link.At + Along * MeetsAt, Span, Fraction);
				Param = GuidelineGeom::ParamAtSample(Span, Fraction, Curve.Num());
				Corner = GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Param);

				const FVector2D Out = Corner - Link.At;
				if (!Out.IsNearlyZero())
				{
					Link.Dir = Out.GetSafeNormal();
				}
			}
			else
			{
				// THE LANE CHANGE. The entry cannot simply run on to the road - it is beside it,
				// not aimed at it - so the connector leaves at the deflection that clears the
				// lock across this gap and meets the road at the same angle on the other side.
				double Run = 0.0;
				const double Deflect =
					GuidelineGeom::ShiftDeflectionFor(LaneRadius, ToRoad.Size(), Run);
				if (Run > LeadInWeldTolerance)
				{
					// WHICH WAY IT BENDS is which side the road is on, and the cross product
					// says so: positive when the road lies to the left of the way this link
					// leaves.
					const double Side = FMath::Sign(Along.X * ToRoad.Y - Along.Y * ToRoad.X);
					const FVector2D Turned =
						Along.GetRotated(FMath::RadiansToDegrees(Deflect) * Side);

					// AND THE ROAD IS MET WHERE THE TRANSITION ARRIVES, not where it is nearest.
					// Both curves get the same tangent length, so the meeting point sits two
					// runs along the aim - and re-asking the road for the point nearest THAT is
					// what keeps this honest when the road bends or ends: the fillet below still
					// works from a point on the road, and Dir is taken from the control
					// afterwards so the lead-in stays tangent to whatever came back.
					const FVector2D Control = Link.At + Along * Run;
					const FVector2D Aim = Control + Turned * (2.0 * Run);

					int32 Span = 0;
					double Fraction = 0.0;
					GuidelineGeom::NearestOnPolyline(Curve, Aim, Span, Fraction);
					Param = GuidelineGeom::ParamAtSample(Span, Fraction, Curve.Num());
					Corner = GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Param);

					const FVector2D Out = Corner - Control;
					if (!Out.IsNearlyZero())
					{
						OutLaneControl = Control;
						Link.Dir = Out.GetSafeNormal();
						LaneRun = Run;
					}
				}
			}
		}
	}

	const FVector2D TaxiDir =
		GuidelineGeom::Tangent(PositionA, Original.Control, PositionB, Param);

	// How far back along each line the tangent points sit, for a fillet of this radius.
	// The lead-in makes two corners with the taxiway - theta one way, 180 - theta the
	// other - and the SHARPER one sizes the offset, so it gets at least its radius and
	// the shallower one simply sweeps more gently.
	const double Cosine = FMath::Clamp(
		FVector2D::DotProduct(-Link.Dir, TaxiDir), -1.0, 1.0);
	const double Theta = FMath::Acos(Cosine);
	const double Sharper = FMath::Min(Theta, UE_DOUBLE_PI - Theta);

	double Offset = 0.0;
	if (Sharper > UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		// A LANE LINK SIZES ITS FILLET FROM THE RADIUS IT OWES, and for the GENTLER of the two
		// corners it makes with the road - which is pi minus the sharper, and is the one a truck
		// joining the traffic takes. CornerRunFor is the quadratic's own inverse; Link.Radius
		// over a tangent is the CIRCULAR fillet's, which is the 40%-short mistake 8be494c made
		// and which this project has already paid three sessions for. The other corner is the
		// turn-back, and at a slant no run clears the lock on both - see the sweep loop below.
		//
		// LEFT AS IT WAS FOR AN AIRCRAFT: Link.Radius is the PAINTED radius of its line, 2500 uu
		// for a Code C, and a lead-in that generous is not what this change is about.
		Offset = LaneRadius > 0.0
			? GuidelineGeom::CornerRunFor(LaneRadius, UE_DOUBLE_PI - Sharper)
			: Link.Radius / FMath::Tan(Sharper * 0.5);
	}

	// It has to fit: on the lead-in, and on the taxiway BOTH ways, with a weld
	// tolerance of room left over so no split produces a stub.
	//
	// MEASURED FROM THE CONTROL WHEN THERE IS ONE, not from the node. LeadEnd is laid at
	// Corner - Dir * Offset and Dir points from the control, so an Offset longer than THAT
	// distance puts LeadEnd behind the control: the lead-in folds back on itself and the two
	// sweeps leave from the wrong side of it. Measured from the node instead, a curved lead-in
	// that swings wide reads as having more room than it has.
	//
	// AND THE TANGENT RUN IS SPENT ROOM. A curved lead-in has TWO legs - the run down the lane
	// and the approach to the road - and a fillet that takes everything up to the weld
	// tolerance leaves the second one a few uu long, which is a HOOK: the lead goes 313 uu out
	// along the lane and doubles back 10. Measured on the suite's own fixture before this
	// subtraction: 67 degrees of instant turn at the far end of a lead-in whose whole purpose
	// is to have none. It only ever binds when the road is close - at 54 m the fillet asks for
	// 2500 against 4590 of room - which is exactly where a hook was being built.
	const FVector2D LeadFrom = OutLaneControl.IsSet() ? OutLaneControl.GetValue() : Link.At;
	const double LeadRoom =
		FVector2D::Distance(LeadFrom, Corner) - LeadInWeldTolerance - LaneRun;
	const double TotalLength = GuidelineGeom::PolylineLength(Curve);
	const double Behind = TotalLength * Param - LeadInWeldTolerance;
	const double Ahead = TotalLength * (1.0 - Param) - LeadInWeldTolerance;

	Offset = FMath::Min(Offset, FMath::Min(LeadRoom, FMath::Min(Behind, Ahead)));

	FGuidelineNodeId LeadEnd;
	TArray<FGuidelineNodeId> SweepEnds;

	if (Offset <= LeadInWeldTolerance)
	{
		// No room to sweep - a stand crammed against a taxiway end, or one whose
		// lead-in is barely longer than the weld tolerance. Fall back to the hard join
		// rather than emit folded geometry: an ugly corner is recoverable, an inverted
		// arc is not. The junction solver clamps its fillets for the same reason.
		FGuidelineNodeId JoinNode;
		FGuidelineEdgeId JoinHead, JoinTail;
		if (!Network.SplitGuidelineEdge(Hit.Edge, Param, LeadInWeldTolerance,
			JoinNode, JoinHead, JoinTail))
		{
			return FGuidelineNodeId();
		}

		LeadEnd = JoinNode;
	}
	else
	{
		// Cut the taxiway at BOTH tangent points and keep the piece between them: an
		// aircraft taxiing PAST the stand still needs a way through. Two chained
		// SplitGuidelineEdge calls rather than one three-way split: the second cut's
		// param is taken in the REMAINING piece's own parameter space, because that is
		// the curve it is now being taken from, and OutTail from the first call IS that
		// piece - see FRoadGuidelineBuilder's SplitFromEnd for the same technique.
		//
		// WeldTolerance is 0, NOT LeadInWeldTolerance, on both cuts - unlike every other
		// SplitGuidelineEdge call in this file. The sweep needs two REAL nodes to hang its
		// arcs on: a weld here would return an existing endpoint as BackNode or FwdNode,
		// leaving one arc with no far end (SweepEnds gets the wrong node, or - if the
		// first cut welds to B - RestEdge comes back unset and the second call fails
		// outright). It would also silently break ParamFwdInRest's assumption that the
		// remaining piece STARTS at ParamBack, true only when the first cut actually made
		// a cut. Old code never welded here either: Behind/Ahead already keep both cuts at
		// least LeadInWeldTolerance of ARC LENGTH from either endpoint, but arc length
		// exceeds chord length on a bend, so that bound alone would not stop the WELD
		// TEST - which compares the chord - from firing anyway.
		const double ParamBack = GuidelineGeom::ParamAtArcOffset(Curve, Param, -Offset);
		const double ParamFwd  = GuidelineGeom::ParamAtArcOffset(Curve, Param, +Offset);

		FGuidelineNodeId BackNode;
		FGuidelineEdgeId HeadEdge, RestEdge;
		if (!Network.SplitGuidelineEdge(Hit.Edge, ParamBack, /*WeldTolerance=*/0.0,
			BackNode, HeadEdge, RestEdge))
		{
			return FGuidelineNodeId();
		}

		const double ParamFwdInRest = (ParamFwd - ParamBack) / FMath::Max(1.0 - ParamBack, UE_DOUBLE_SMALL_NUMBER);

		FGuidelineNodeId FwdNode;
		FGuidelineEdgeId MiddleEdge, TailEdge;
		if (!Network.SplitGuidelineEdge(RestEdge, ParamFwdInRest, /*WeldTolerance=*/0.0,
			FwdNode, MiddleEdge, TailEdge))
		{
			return FGuidelineNodeId();
		}

		// The straight lead-in now stops short of the corner; the sweeps take over.
		const FVector2D LeadEndAt = Corner - Link.Dir * Offset;
		LeadEnd = Network.AddGuidelineNode(LeadEndAt, /*bDerived=*/true);

		SweepEnds.Add(BackNode);
		SweepEnds.Add(FwdNode);
	}

	// The lead-in itself: still straight, because it is. Only the ENTRY sweeps.
	//
	// PAINTED for an aircraft, and merely a connector for a service link - the lane it
	// leaves is invisible, so nothing about this one is drawn either. It deliberately
	// carries NO StandGeometryOwner: leaving it unowned is what lets
	// URoadNetwork::IsServiceNodeConnected tell a lane that reaches a road from one that
	// only reaches itself.
	FGuidelineEdge Lead;
	Lead.A = Link.Node;
	Lead.B = LeadEnd;

	// STRAIGHT FOR AN AIRCRAFT LEAD-IN, which is a painted line cast along the stand's own
	// heading and has no business curving. A SERVICE LINK sweeps: its control sits back along
	// the ring at the join, so it leaves the lane along the lane and there is no turn at the
	// junction for a truck to take - or for the router to choose.
	Lead.Control = OutLaneControl.IsSet()
		? OutLaneControl.GetValue()
		: (Link.At + Network.GetGuidelineNode(LeadEnd)->Position) * 0.5;

	Lead.AllowedTraffic = FTrafficMask::Only(Link.Class);
	Lead.AllowedTraffic.Add(ETraversalClass::Emergency);
	Lead.Direction = EGuidelineDir::Bidirectional;
	Lead.Width = Original.Width;
	Lead.MaxWingspan = Link.MaxWingspan;
	Lead.bDerived = true;

	// MEASURED, NOT ASSUMED, and only for the link that is sized to a radius in the first place.
	// The lead-in's DELIVERED radius is what a truck drives; the run that was asked for is not.
	// This project has paid three sessions for that distinction once already (8be494c, where a
	// test checked the radius requested while the follower drove the radius delivered), so the
	// figure the warning below prints comes off the edge as laid.
	const FVector2D LeadEndAt = Network.GetGuidelineNode(LeadEnd)->Position;
	double Tightest = TNumericLimits<double>::Max();
	if (LaneRun > 0.0)
	{
		Tightest = GuidelineGeom::TightestRadius(Link.At, Lead.Control, LeadEndAt);
	}
	Network.AddGuidelineEdge(MoveTemp(Lead));

	// One sweep to each side, so the stand is reachable whichever way an aircraft
	// arrives. With only one, an approach from the other side is routed round a hairpin
	// at the tangent point - the same corner, moved a few metres rather than removed.
	//
	// Control AT THE CORNER, which is what makes each arc tangent to the lead-in at one
	// end and to the taxiway at the other: a quadratic's end tangents point at its
	// control, and both tangent points sit the same distance from it. Exactly the
	// construction RoadGuidelineBuilder uses for a junction turn path.
	for (const FGuidelineNodeId SweepEnd : SweepEnds)
	{
		FGuidelineEdge Sweep;
		Sweep.A = LeadEnd;
		Sweep.B = SweepEnd;
		Sweep.Control = Corner;
		Sweep.AllowedTraffic = FTrafficMask::Only(Link.Class);
		Sweep.AllowedTraffic.Add(ETraversalClass::Emergency);
		Sweep.Direction = EGuidelineDir::Bidirectional;
		Sweep.Width = Original.Width;
		Sweep.MaxWingspan = Link.MaxWingspan;
		Sweep.bDerived = true;

		// THE MERGE ONLY, not the turn-back. Both sweeps are tangent to the lead-in at one end
		// and to the road at the other; what differs is how far they turn. The one that carries
		// on the way the lead-in was pointing turns by the deflection, and is the one this
		// construction sizes; the OTHER turns by 180 minus it, and at a shallow deflection that
		// is a hairpin no vehicle can take. It is laid anyway, because without it a truck
		// arriving from the far side has no way in at all, and the router costs it and avoids it
		// wherever another entry will do.
		//
		// NO GEOMETRY FIXES THAT ONE at a close gap, which is why it is excluded here rather
		// than reported: merging onto a road 4 m away needs a shallow slant, and turning back
		// the other way from a shallow slant is a U-turn. At a gap wider than 2.83 times the
		// lock the deflection reaches its right-angle cap and BOTH sweeps clear - see
		// GuidelineGeom::ShiftDeflectionFor, and Airside.Build.StandLinkClearsTheTruckLock,
		// which measures both at both gaps.
		const FVector2D SweepEndAt = Network.GetGuidelineNode(SweepEnd)->Position;
		if (LaneRun > 0.0
			&& FVector2D::DotProduct(SweepEndAt - Corner, Link.Dir) > 0.0)
		{
			Tightest = FMath::Min(Tightest,
				GuidelineGeom::TightestRadius(LeadEndAt, Corner, SweepEndAt));
		}
		Network.AddGuidelineEdge(MoveTemp(Sweep));
	}

	// SAID OUT LOUD WHEN THE GROUND CANNOT TAKE THE VEHICLE IT IS BUILT FOR, and naming the
	// thing a player can actually change: how far the stand sits from its road. A truck cannot
	// follow an arc tighter than its lock at any speed, so a link under it is a stand whose
	// service traffic will cut the corner however slowly it crawls.
	//
	// THE GAP IS IN THE LINE because it is the only lever - the radius is the truck's and the
	// entry is the stand's. Moving the road out fixes this and nothing else will: past 2.83
	// times the lock (about 20 m for the shipping dispenser) the transition stops being
	// constrained at all.
	if (const double Lock =
			UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();
		LaneRun > 0.0 && Lock > 0.0 && Tightest < Lock)
	{
		UE_LOG(LogAirside, Warning,
			TEXT("Service lane entry at (%.0f, %.0f) joins its road %.0f uu away on a %.0f uu "
			     "curve, tighter than the %.0f uu a service vehicle's lock allows. A truck will "
			     "cut that corner; move the road further from the stand."),
			Link.At.X, Link.At.Y, FVector2D::Distance(Link.At, Corner), Tightest, Lock);
	}

	// THIS LINK'S OWN GEOMETRY IS NOT A TARGET FOR THE NEXT ONE. Every edge just added -
	// the lead and both sweeps - has LeadEnd at an end, so one node excludes all three,
	// exactly as the comment on the search's AnchorNodes test says it should.
	//
	// It only ever mattered once a ring could take SEVERAL links. With one per stand
	// there was no next one; with three, the second and third both found the first's
	// sweep lying a few metres off the ring, joined THAT instead of the road, and every
	// entry collapsed onto one node - including, through a spur, a straight run across
	// the parked aircraft.
	AnchorNodes.Add(LeadEnd);

	return LeadEnd;
}

int32 FAnchorLink::Build(URoadNetwork& Network, double MaxLeadIn, double ServiceLinkRadius)
{
	// Gathered up front, because joining one anchor adds and removes edges and an
	// iteration over the graph must not be holding pointers into it while that happens.
	TArray<FPendingLink> Pending;
	TSet<FGuidelineNodeId> AnchorNodes;
	Gather(Network, MaxLeadIn, ServiceLinkRadius, Pending, AnchorNodes);

	int32 Joined = 0;
	int32 Unjoined = 0;

	// WHICH STANDS GOT IN, and where the first entry that did not was, so a stand that joined
	// nothing at all can be named once after the loop rather than four times inside it.
	TSet<FEntityInstanceId> StandsJoined;
	TMap<FEntityInstanceId, FVector2D> StandsRefused;

	// MUTABLE, because a link found by PROXIMITY has no direction of its own until the search
	// says which way the road lies, and one leaving a lane has none until the tangent run
	// along that lane is known.
	for (FPendingLink& Link : Pending)
	{
		const FLinkHit Hit = Resolve(Network, Link, AnchorNodes);
		if (!Hit.IsSet())
		{
			// SAID, NOT SWALLOWED. An anchor with nothing to join is a stand no aircraft can
			// be routed to, and until this line the only symptom was an arrival refused for
			// "no route to a stand" with nothing in the log to say which stand or why. The
			// heading is in degrees because a player reads the details panel in degrees.
			//
			// WHICH RULE RAN IS IN THE LINE, because the two refuse for different reasons and
			// the repair differs: an aircraft lead-in that joins nothing may be AIMED wrong,
			// and a service link that joins nothing is simply too far from any road.
			if (Link.LaneOwner.IsSet())
			{
				// DEFERRED, not dropped - see FPendingLink::LaneOwner. Three declared entries
				// of four finding no road is what a stand beside one service road looks like,
				// and this is the line that makes the fourth reportable.
				StandsRefused.FindOrAdd(Link.LaneOwner, Link.At);
				continue;
			}

			// ALWAYS AN ANCHOR OR A POSE BY HERE. A lane entry took the deferred branch above,
			// so the "service lane" case this line used to choose between is unreachable and is
			// not written as a choice: the sentence would read as evidence that an entry can
			// arrive here, and the reader would look for the path.
			++Unjoined;
			UE_LOG(LogAirside, Warning,
				TEXT("Anchor at (%.0f, %.0f) joins nothing: no derived %s guideline within %.0f uu%s"),
				Link.At.X, Link.At.Y,
				Link.Class == ETraversalClass::Aircraft ? TEXT("aircraft") : TEXT("vehicle"),
				Link.Reach,
				Link.Class == ETraversalClass::Aircraft
					? *FString::Printf(TEXT(" along heading %.0f deg"),
						FMath::RadiansToDegrees(FMath::Atan2(Link.Dir.Y, Link.Dir.X)))
					: TEXT(" in any direction"));
			continue;
		}

		const FGuidelineNodeId LeadEnd = Join(Network, Link, Hit, AnchorNodes);
		if (!LeadEnd.IsSet())
		{
			continue;
		}

		++Joined;
		if (Link.LaneOwner.IsSet())
		{
			StandsJoined.Add(Link.LaneOwner);
		}
	}

	// THE STANDS THAT JOINED NOTHING AT ALL, one line each. Reported here because an entry is
	// only known to be the last hope once every entry has been tried, and because the ORDER
	// they were tried in must not decide whether the stand is reported.
	for (const TPair<FEntityInstanceId, FVector2D>& Refused : StandsRefused)
	{
		if (StandsJoined.Contains(Refused.Key))
		{
			continue;
		}

		++Unjoined;
		UE_LOG(LogAirside, Warning,
			TEXT("Service lane at (%.0f, %.0f) joins nothing: no derived vehicle guideline ")
			TEXT("within %.0f uu in any direction, at any of its declared entries"),
			Refused.Value.X, Refused.Value.Y, ServiceLinkRadius);
	}

	// One census line per pass, beside the guideline builder's: how many lead-ins were
	// cast and how many found a line. Zero pending is the common idle rebuild and stays
	// quiet.
	if (Pending.Num() > 0)
	{
		UE_LOG(LogAirside, Log, TEXT("Anchor links: %d of %d lead-in(s) joined a guideline, %d unjoined"),
			Joined, Pending.Num(), Unjoined);
	}

	return Joined;
}
