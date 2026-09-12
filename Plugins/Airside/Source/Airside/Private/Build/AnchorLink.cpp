#include "Build/AnchorLink.h"

#include "AirsideLog.h"
#include "Build/AnchorLinkFinder.h"
#include "Build/ServiceLoopBuild.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

namespace
{
	/**
	 * How near does this ring side come to a road it could actually join?
	 *
	 * Measured on the graph as it stands BEFORE any of this pass's links, which is what makes
	 * the answer independent of the order the sides are taken in - each link splits the road
	 * it joins, and a side measured after that is measuring a different airport from the one
	 * measured before it.
	 *
	 * Reach, not infinity, so a side with nothing near it costs one sweep and not a search of
	 * the whole graph.
	 */
	double NearestRoadApproach(const URoadNetwork& Network, const TSet<FGuidelineNodeId>& AnchorNodes,
		const FGuidelineEdge& Side, ETraversalClass Class, double Reach)
	{
		const FGuidelineNode* SideA = Network.GetGuidelineNode(Side.A);
		const FGuidelineNode* SideB = Network.GetGuidelineNode(Side.B);
		if (SideA == nullptr || SideB == nullptr)
		{
			return TNumericLimits<double>::Max();
		}

		TArray<FVector2D> SidePoints;
		GuidelineGeom::Sample(SideA->Position, Side.Control, SideB->Position, SidePoints);

		double Best = Reach;
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (const FGuidelineEdge& Edge : Edges)
		{
			// The same eligibility the search below applies, and for the same reasons: a lane
			// side that measured against a target the search would refuse would be selected on
			// a distance it can never actually have.
			if (!Edge.bAlive || !Edge.bDerived || Edge.A == Edge.B
				|| AnchorNodes.Contains(Edge.A) || AnchorNodes.Contains(Edge.B)
				|| !Edge.AllowedTraffic.Allows(Class))
			{
				continue;
			}

			const FGuidelineNode* EndA = Network.GetGuidelineNode(Edge.A);
			const FGuidelineNode* EndB = Network.GetGuidelineNode(Edge.B);
			if (EndA == nullptr || EndB == nullptr)
			{
				continue;
			}

			TArray<FVector2D> Points;
			GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

			int32 SideSpan = 0, RoadSpan = 0;
			double SideFraction = 0.0, RoadFraction = 0.0;
			Best = FMath::Min(Best, GuidelineGeom::NearestBetweenPolylines(
				SidePoints, Points, SideSpan, SideFraction, RoadSpan, RoadFraction));
		}
		return Best;
	}

	/**
	 * Does either end of this lane side already carry a link to a road?
	 *
	 * A LOCAL test, deliberately, where URoadNetwork::IsServiceNodeConnected is a WALK. The
	 * walk answers "can this ring reach a road at all", which is the right question when a
	 * ring gets one link and the wrong one when it gets several - the first link would make
	 * it true and suppress every other side. What identifies a link is that it carries no
	 * ServiceLoopOwner: the ring's own sides and spurs all carry one.
	 */
	bool SideAlreadyLinked(const URoadNetwork& Network, const FGuidelineEdge& Side)
	{
		for (const FGuidelineNodeId& End : { Side.A, Side.B })
		{
			const FGuidelineNode* Node = Network.GetGuidelineNode(End);
			if (Node == nullptr)
			{
				continue;
			}
			for (const FGuidelineEdgeId& Incident : Node->Incident)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Incident);
				if (Edge != nullptr && Edge->bAlive && !Edge->ServiceLoopOwner.IsSet())
				{
					return true;
				}
			}
		}
		return false;
	}

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
		const FString Letter = Code.ToString().ToUpper();

		if (Letter == TEXT("A")) { return 1500.0; }
		if (Letter == TEXT("B")) { return 2000.0; }
		if (Letter == TEXT("C")) { return 2500.0; }
		if (Letter == TEXT("D")) { return 4000.0; }
		if (Letter == TEXT("E")) { return 5000.0; }
		if (Letter == TEXT("F")) { return 6000.0; }

		// No code, or one nobody recognises. Code C is the commonest stand in the world, and
		// erring to Code F instead would put a 60 m curve on a light-aircraft apron.
		return 2500.0;
	}

	/**
	 * The curve parameter Offset of ARC LENGTH away from Param, walked on the sampled
	 * polyline. Negative walks backwards. Clamped to the curve's own ends.
	 *
	 * Walked on the SAMPLES rather than integrated in closed form, because the samples are
	 * what every other consumer of this graph measures - the search costs them, the overlay
	 * draws them, a follower walks them. An exact arc length here would be more accurate
	 * and would disagree with all three.
	 */
	double ParamAtArcOffset(const TArray<FVector2D>& Points, double Param, double Offset)
	{
		if (Points.Num() < 2)
		{
			return FMath::Clamp(Param, 0.0, 1.0);
		}

		const int32 Spans = Points.Num() - 1;
		const double Scaled = FMath::Clamp(Param, 0.0, 1.0) * Spans;
		int32 Index = FMath::Clamp(static_cast<int32>(Scaled), 0, Spans - 1);
		double Fraction = Scaled - Index;

		double Remaining = FMath::Abs(Offset);
		const bool bForward = Offset >= 0.0;

		while (Remaining > 0.0)
		{
			const double SpanLength = FVector2D::Distance(Points[Index], Points[Index + 1]);
			const double Available = bForward ? SpanLength * (1.0 - Fraction) : SpanLength * Fraction;

			if (SpanLength <= 0.0 || Available >= Remaining)
			{
				Fraction += (bForward ? 1.0 : -1.0) * (SpanLength > 0.0 ? Remaining / SpanLength : 0.0);
				break;
			}

			Remaining -= Available;
			if (bForward)
			{
				if (Index + 1 >= Spans) { Fraction = 1.0; break; }
				++Index;
				Fraction = 0.0;
			}
			else
			{
				if (Index == 0) { Fraction = 0.0; break; }
				--Index;
				Fraction = 1.0;
			}
		}

		return GuidelineGeom::ParamAtSample(Index, FMath::Clamp(Fraction, 0.0, 1.0), Points.Num());
	}

}

void FAnchorLink::Gather(URoadNetwork& Network, double MaxLeadIn, double ServiceLinkRadius,
	TArray<FPendingLink>& OutPending, TSet<FGuidelineNodeId>& OutAnchorNodes)
{
	// THE LANES FIRST. A service anchor spurred to its stand's loop is already joined by the
	// time the walk below asks, so it is skipped there rather than cast at a road on the far
	// side of the aeroplane - and the LANE becomes the thing that links.
	const FServiceLoopBuild::FResult Loops = FServiceLoopBuild::Build(Network);

	// EVERY NODE A LINK MUST NOT TARGET: anchor and pose nodes as always, PLUS every node of
	// every service lane and spur. A lane is itself a vehicle guideline, so without the
	// second half a lane would join ITSELF - four metres away, across its own box - every
	// stand would read as connected, and no truck would ever route anywhere.
	OutAnchorNodes = Loops.Nodes;

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
			const double Out = Instance.Heading + UE_DOUBLE_PI;

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

		// THE LANE'S LINKS TO A ROAD. ONE PER SIDE, not one per stand.
		//
		// One per stand made the lane a CUL-DE-SAC: a truck entered at the single point where
		// the ring came nearest a road and then drove up to half the ring's perimeter - 94 m
		// on a Code C stand - to reach an anchor a few metres from where it started. A side
		// per entry makes it a drive-through, and the search then picks whichever entry is
		// nearest the anchor it is actually going to.
		//
		// A road within reach of two stands is still joined by BOTH, each getting its own
		// connections - the normal case, one service road serving a row, not a conflict.
		if (const TArray<FGuidelineEdgeId>* Lane = Loops.Lanes.Find(EntityId))
		{
			// HOW NEAR EACH SIDE COMES, measured before anything is linked, so that "which
			// sides are beside the road" is a property of the airport and not of the order
			// this loop happens to visit them in.
			TMap<FGuidelineEdgeId, double> Approach;
			double NearestSide = TNumericLimits<double>::Max();
			for (const FGuidelineEdgeId& SideId : *Lane)
			{
				const FGuidelineEdge* Side = Network.GetGuidelineEdge(SideId);
				if (Side == nullptr || !Side->bAlive || Side->bServiceSpur)
				{
					continue;
				}

				const double Distance = NearestRoadApproach(
					Network, OutAnchorNodes, *Side, ETraversalClass::GroundVehicle, ServiceLinkRadius);
				Approach.Add(SideId, Distance);
				NearestSide = FMath::Min(NearestSide, Distance);
			}

			// BESIDE THE ROAD, OR REACHING FOR IT. A Code C ring is 42 m deep and the service
			// radius 50, so the FAR side is "within reach" of a road running along the near
			// one - and so is any side willing to run a connector diagonally past a corner.
			// Both were seen: a link from the far side's corner to a road point beyond the
			// ring, crossing nothing and helping nobody, beside a near side already touching
			// the same road a tenth of the distance away.
			//
			// TWICE THE BEST SIDE, and not an absolute figure: what counts as beside the road
			// scales with how far the player put the stand from it. On a ring square to a
			// road the near side and both end corners measure the SAME clearance and all
			// three qualify, which is the drive-through this exists to build; the far side
			// measures the clearance plus the depth of the stand and does not.
			//
			// The floor matters for a ring laid ON its road, where twice nearly nothing is
			// still nothing and only one side would ever qualify.
			const double Qualifies = FMath::Max(
				NearestSide * 2.0, NearestSide + FServiceLoopBuild::LaneWidth);

			for (const FGuidelineEdgeId& SideId : *Lane)
			{
				const FGuidelineEdge* Side = Network.GetGuidelineEdge(SideId);
				if (Side == nullptr || !Side->bAlive)
				{
					continue;
				}

				if (const double* Distance = Approach.Find(SideId);
					Distance == nullptr || *Distance > Qualifies)
				{
					continue;
				}

				// SPURS ARE NOT SIDES - a spur is the stub from an anchor to the ring, and
				// linking one to a road would join the road to a hydrant pit directly, across
				// the very ground the ring exists to route a truck around. Already excluded by
				// the measuring pass above, which never puts one in Approach.

				// ALREADY CONNECTED, ASKED OF THIS SIDE. It used to be asked of the whole lane
				// (URoadNetwork::IsServiceNodeConnected, a walk that answers "can this ring
				// reach a road at all") - which is the right question for ONE link per stand
				// and suppresses every link after the first once there are several. Asked
				// locally instead: does either end of this side already carry an edge that no
				// service loop owns, which is exactly what a link edge is.
				if (SideAlreadyLinked(Network, *Side))
				{
					continue;
				}

				const FGuidelineNode* SideA = Network.GetGuidelineNode(Side->A);
				if (SideA == nullptr)
				{
					continue;
				}

				FPendingLink Link;
				Link.Kind = ELinkKind::Lane;
				Link.Lane = { SideId };
				Link.LaneOwner = EntityId;
				Link.Class = ETraversalClass::GroundVehicle;

				// A placeholder until the search says where the road comes nearest, and the
				// side is split there. Only the unjoined WARNING reads it before then, and a
				// corner of the side is the right thing for that to name.
				Link.At = SideA->Position;
				Link.MaxWingspan = 0.0;
				Link.Radius = StandRadius;
				Link.Reach = ServiceLinkRadius;
				OutPending.Add(Link);
			}
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

	// WHERE THE LEAD-IN RAY STRIKES IS THE CORNER, NOT THE JOIN.
	//
	// Joining here is what produced a hard turn: the ray meets the taxiway at whatever
	// angle the stand happens to face, and a stand square to the taxiway makes it 90
	// degrees. A curve cannot fix that in place - a quadratic's end tangents both point
	// at its control, so being tangent to the lead-in AND to the taxiway would put the
	// control exactly here, which is the straight line again. The join has to MOVE.
	const FVector2D Corner =
		GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Hit.Param);

	if (Link.Lane.Num() > 0)
	{
		// THE LANE IS SPLIT TO MAKE THE LINK'S OWN END. Entry in the MIDDLE of a side, not
		// at a corner: a corner-to-road connector would run diagonally across the very
		// ground the lane exists to keep clear, and the split costs one node on a lane
		// nobody can see.
		//
		// Safe here even though Original/PositionA/PositionB/Corner were captured above:
		// those are values, not pointers, and nothing below touches the ROAD edge.
		//
		// SplitGuidelineEdge welds to an existing endpoint within LeadInWeldTolerance, or
		// splits and hands the halves Original's fields (ServiceLoopOwner included), which
		// is what keeps the lane recognisable as this entity's after the split. Link.Node
		// IS the join it reports either way; the piece ids themselves are not needed here.
		FGuidelineEdgeId LaneHead, LaneTail;
		if (!Network.SplitGuidelineEdge(Hit.LaneEdge, Hit.LaneParam, LeadInWeldTolerance,
			Link.Node, LaneHead, LaneTail))
		{
			return FGuidelineNodeId();
		}

		// The link now has a real node to leave from, and LeadRoom below measures from it.
		Link.At = Network.GetGuidelineNode(Link.Node)->Position;
		AnchorNodes.Add(Link.Node);
	}

	if (Link.Class != ETraversalClass::Aircraft)
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
		const double ParamBack = ParamAtArcOffset(Curve, Hit.Param, -Offset);
		const double ParamFwd  = ParamAtArcOffset(Curve, Hit.Param, +Offset);

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
	// carries NO ServiceLoopOwner: leaving it unowned is what lets
	// URoadNetwork::IsServiceNodeConnected tell a lane that reaches a road from one that
	// only reaches itself.
	FGuidelineEdge Lead;
	Lead.A = Link.Node;
	Lead.B = LeadEnd;
	Lead.Control = (Link.At + Network.GetGuidelineNode(LeadEnd)->Position) * 0.5;

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
