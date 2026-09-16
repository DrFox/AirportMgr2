#include "Build/AnchorLink.h"

#include "AirsideLog.h"
#include "Build/AnchorLinkFinder.h"
#include "Build/StandLaneBuild.h"
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
	// question. Task 5 of the stand routing work links through those entries; between this
	// task and that one a stand is laid but UNLINKED, which is a planned intermediate state
	// and not a regression. Every test that needs a road-to-stand connection fails here.
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

		// NO LINK FROM THE LANE TO A ROAD, 2026-09-16, and that is deliberate for one task only.
		//
		// A whole block stood here that measured how near each side of the stand's ring came to
		// a road and cast one link per qualifying side. It is gone with the ring's side-finding
		// helpers above, for the reason given there: the entrance is DECLARED now, not
		// discovered, and FStandLaneBuild::FResult::Entries already says which node each
		// authored Entry waypoint became. Task 5 of the stand routing work reads that map and
		// links through it.
		//
		// UNTIL IT DOES, A STAND IS LAID BUT UNREACHABLE. Its lane is in the graph and its
		// anchors are on the lane, so a truck already standing on the lane can drive to any of
		// them; nothing joins that lane to the airport. Airside.Build.RoadAlongsideARowOfStands
		// and the fuel service suite fail on exactly that, by design, for one commit.
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

	// WHERE THE LEAD-IN RAY STRIKES IS THE CORNER, NOT THE JOIN.
	//
	// Joining here is what produced a hard turn: the ray meets the taxiway at whatever
	// angle the stand happens to face, and a stand square to the taxiway makes it 90
	// degrees. A curve cannot fix that in place - a quadratic's end tangents both point
	// at its control, so being tangent to the lead-in AND to the taxiway would put the
	// control exactly here, which is the straight line again. The join has to MOVE.
	const FVector2D Corner =
		GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Hit.Param);

	// SET ONLY BY A LANE LINK, and no link is a Lane link between this task and Task 5 of the
	// stand routing work - see Gather, which no longer casts one. So the lead-in below is
	// always the straight midpoint case for now.
	//
	// THE BLOCK THAT FILLED IT IS DELETED, NOT COMMENTED OUT. It split the ring at the road's
	// nearest point, walked the ring both ways looking for room to sweep, and kept whichever
	// direction bent least - an apparatus that existed because the entrance was DISCOVERED.
	// An entrance is DECLARED now (UEntityDefinition::ServiceLane's Entry waypoints, reported
	// by FStandLaneBuild::FResult::Entries), so what replaces it is a different construction
	// and not a tidier version of this one. The reason it must sweep at all survives and is
	// worth restating where it lands: a square-on entrance is a tight turn WE BUILT, and
	// however well the router is taught to avoid tight turns it can only choose among the ones
	// that exist - a lane whose entrances crossed at a right angle left exactly one tight
	// option on every stand, and that is the one it kept taking. Reported from play
	// 2026-09-15, twice.
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

	const FVector2D TaxiDir =
		GuidelineGeom::Tangent(PositionA, Original.Control, PositionB, Hit.Param);

	// THE ONE DELIBERATE EXCEPTION to URoadNetwork::SampleGuideline being the one graph-edge
	// call of GuidelineGeom::Sample (PR #137 review, issue #105 item 5): there is no live
	// FGuidelineEdgeId left to sample by the time this runs. A Lane join above may already
	// have called SplitGuidelineEdge, which replaces Hit.Edge with two new head/tail pieces -
	// Original/PositionA/PositionB are a snapshot of the edge as it stood BEFORE that split
	// (captured at line ~421, before anything could reallocate the slot array from under
	// Found), and this curve is exactly that pre-split geometry, which SampleGuideline has no
	// id left to look up.
	TArray<FVector2D> Curve;
	GuidelineGeom::Sample(PositionA, Original.Control, PositionB, Curve);

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
		Offset = Link.Radius / FMath::Tan(Sharper * 0.5);
	}

	// It has to fit: on the lead-in, and on the taxiway BOTH ways, with a weld
	// tolerance of room left over so no split produces a stub.
	const double LeadRoom = FVector2D::Distance(Link.At, Corner) - LeadInWeldTolerance;
	const double TotalLength = GuidelineGeom::PolylineLength(Curve);
	const double Behind = TotalLength * Hit.Param - LeadInWeldTolerance;
	const double Ahead = TotalLength * (1.0 - Hit.Param) - LeadInWeldTolerance;

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
		if (!Network.SplitGuidelineEdge(Hit.Edge, Hit.Param, LeadInWeldTolerance,
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
		const double ParamBack = GuidelineGeom::ParamAtArcOffset(Curve, Hit.Param, -Offset);
		const double ParamFwd  = GuidelineGeom::ParamAtArcOffset(Curve, Hit.Param, +Offset);

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
		Network.AddGuidelineEdge(MoveTemp(Sweep));
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

	// WHICH RINGS GOT IN, and where the first side that did not was, so a ring that joined
	// nothing at all can be named once after the loop rather than four times inside it.
	TSet<FEntityInstanceId> RingsJoined;
	TMap<FEntityInstanceId, FVector2D> RingsRefused;

	// MUTABLE, because a link found by PROXIMITY has no direction of its own until the search
	// says which way the road lies, and a LANE link has no node until the lane is split.
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
				// DEFERRED, not dropped - see FPendingLink::LaneOwner. Three sides of four
				// finding no road is what a stand beside one service road looks like.
				RingsRefused.FindOrAdd(Link.LaneOwner, Link.At);
				continue;
			}

			++Unjoined;
			UE_LOG(LogAirside, Warning,
				TEXT("%s at (%.0f, %.0f) joins nothing: no derived %s guideline within %.0f uu%s"),
				Link.Lane.Num() > 0 ? TEXT("Service lane") : TEXT("Anchor"),
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
			RingsJoined.Add(Link.LaneOwner);
		}
	}

	// THE RINGS THAT JOINED NOTHING AT ALL, one line each. Reported here because a side is
	// only known to be the last hope once every side has been tried, and because the ORDER
	// the sides were tried in must not decide whether the stand is reported.
	for (const TPair<FEntityInstanceId, FVector2D>& Refused : RingsRefused)
	{
		if (RingsJoined.Contains(Refused.Key))
		{
			continue;
		}

		++Unjoined;
		UE_LOG(LogAirside, Warning,
			TEXT("Service lane at (%.0f, %.0f) joins nothing: no derived vehicle guideline ")
			TEXT("within %.0f uu in any direction, on any of its sides"),
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
