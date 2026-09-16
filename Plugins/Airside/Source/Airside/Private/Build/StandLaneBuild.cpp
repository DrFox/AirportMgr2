#include "Build/StandLaneBuild.h"

#include "AirsideLog.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * Within this of an endpoint, join the endpoint rather than splitting off a stub.
	 *
	 * The same figure FAnchorLink welds at, and for the same reason: a split that leaves a
	 * centimetre of edge behind is a node nothing can usefully be at.
	 */
	constexpr double LaneWeldTolerance = 10.0;

	/**
	 * One edge of the lane cycle - a straight run, or the bend that rounds a corner.
	 *
	 * One function for both because they differ in nothing but their control point, and a
	 * bend that disagreed with its two straights about which class may use it would be a lane
	 * a truck could reach and not leave.
	 */
	FGuidelineEdge MakeStandLaneEdge(FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D& PositionA, const FVector2D& PositionB, FEntityInstanceId Owner)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;

		// Straight, spelled the way the builder spells it: the control ON the midpoint, which
		// is what GuidelineGeom::IsStraight tests for and what lets Sample short-circuit to
		// two points. A bend overwrites this with the corner itself.
		Edge.Control = (PositionA + PositionB) * 0.5;

		// GroundVehicle, plus Emergency as every derived guideline carries. NOT Aircraft: an
		// aeroplane routed round the lane would be driving round itself.
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = FStandLaneBuild::LaneWidth;

		// 0 is UNLIMITED - see FProfileGuideline::MaxWingspan. A span limit on a line no wing
		// uses could never bind, and the class has already refused aircraft.
		Edge.MaxWingspan = 0.0;
		Edge.bDerived = true;
		Edge.StandGeometryOwner = Owner;

		// NEVER AN APPROACH. Every edge this builder lays is part of the cycle itself; see
		// FGuidelineEdge::bStandApproach for what reading that off the endpoints instead cost,
		// and for who is expected to set it. Stated rather than left to the default, because
		// the two splits FAnchorLink makes copy from Original and a mark nobody writes is a
		// mark nobody can trust.
		Edge.bStandApproach = false;
		return Edge;
	}

	/**
	 * How far back along the lane a line joining it puts its join, uu.
	 *
	 * A LINE JOINS A LANE ALONG IT, NOT ACROSS IT. Its control point is the point it was
	 * nearest, so the first leg of the quadratic runs down the lane and the curve is tangent
	 * to it - there is no heading change at the junction at all, and nothing for FSpeedProfile
	 * to call a corner.
	 *
	 * WITH a run of p, a gap of q and the right angle between them that this construction
	 * always has, the quadratic's apex radius is 2p^2q^2 / (p^2+q^2)^(3/2). It peaks at
	 * p = q*sqrt(2), where it is 0.77q, and falls away on both sides.
	 *
	 * NOT THE PEAK, DELIBERATELY. The run is a DETOUR: the vehicle leaves the lane this far
	 * past where it wanted to be and the curve brings it back, so every uu of run is driven
	 * twice. The peak for the hydrant's 1390 uu gap is a 1966 uu run, and asking for it
	 * lengthened a measured route by about 3000 uu for radius nobody needed. What is needed is
	 * the 471 uu a truck's steering lock allows - FAirframe::TightestFollowableRadius - and 800
	 * uu of run clears that with margin at every gap a stand presents:
	 *
	 *     gap 1390 (hydrant)      -> 599 uu     gap 990 (equipment) -> 608 uu
	 *     gap 1400 (fixed GPU)    -> 599 uu     gap 757             -> 549 uu
	 *
	 * The curve is widest in the MIDDLE of that range rather than at its end, which is why one
	 * figure serves all of them.
	 *
	 * CAPPED AT THE PEAK for a close join, because past it a longer run makes the curve
	 * TIGHTER as well as longer - both costs, no benefit. A box three metres off the lane took
	 * the peak, 424 uu of run for 231 uu of radius; no geometry could do better with three
	 * metres to work in, and it is the last few metres of a journey that ends in a stop.
	 *
	 * ONE FORMULA, TWO SPECIALISATIONS: this is the asymmetric right-angled case. The symmetric
	 * case (same cut on both legs) is GuidelineGeom::CornerRunFor in Solve/GuidelineGeom.h,
	 * which holds the general two-leg formula they both derive from and which is what rounds
	 * this lane's own corners.
	 */
	constexpr double PreferredTangentRun = 800.0;
}

FStandLaneBuild::FResult FStandLaneBuild::Build(URoadNetwork& Network)
{
	FResult Result;

	// WHAT IS ALREADY THERE, gathered before anything is added so the idempotence check
	// cannot see this pass's own output. An owner whose entity has since died leaves its
	// edges here until the next sweep: they are still excluded as targets, which is right,
	// and are never adopted by anything, which is also right.
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || !Edge.StandGeometryOwner.IsSet())
			{
				continue;
			}

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			Result.Lanes.FindOrAdd(Edge.StandGeometryOwner).Add(Id);
			Result.Nodes.Add(Edge.A);
			Result.Nodes.Add(Edge.B);
		}
	}

	// THE RADIUS EVERY CORNER OF THE LANE IS ROUNDED TO, resolved once for the whole pass.
	//
	// A POLYLINE IS HOW A LANE IS DESCRIBED, NOT HOW IT IS DRIVEN. UEntityDefinition::
	// ServiceLane is a sequence of straights, but a corner where two of them meet is a vertex
	// whose heading changes instantly, and FSpeedProfile calls one of those untakeable at any
	// speed. The bend carries the turn instead.
	//
	// DERIVED, NEVER TYPED. This was a constant 750 uu - the service road's junction fillet -
	// chosen so a lane was never tighter than the road feeding it. That was defensible while
	// the lane was a rectangle outboard of the wingtips with a metre to spare at every corner;
	// it is not, now that the lane dips inboard to run through the hydrant pit. The dip's flat
	// is sized by UEntityDefinition::BuildCodeCStandFor from CornerRunFor at the LARGEST
	// SERVICE VEHICLE's own radius, so a builder rounding those same corners to 750 asks for
	// more run than the definition allowed for and the proportional clamp below shaves it back
	// - which is a lane whose shape is decided by a disagreement between two figures.
	//
	// ResolveLargestServiceVehicle, not ResolveDefaultVehicle, for the reason that function's
	// own header gives: ground geometry is sized for the largest vehicle ADMITTED, never for
	// the one driving now. It is the same call the definition makes, so the two agree by
	// construction rather than by coincidence.
	//
	// NOT IcaoCode::RadiusForLetter, which is what a PAINTED taxi line is swept at, sized for
	// the largest aircraft a stand admits - 2500 uu for a Code C, on a lane four metres wide.
	const double LaneTurnRadius =
		UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();

	// By index, like FAnchorLink::Build: nothing here adds or removes an ENTITY, so holding
	// this reference across the mutations below is safe, and the handle still has to be built
	// by hand from the slot - the array elements have no stable handle of their own.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr
			|| Instance.Definition->ServiceLane.Num() < 3)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		if (Result.Lanes.Contains(EntityId))
		{
			// Already laid, by a pass whose output nothing swept. See the header.
			continue;
		}

		// THE WAYPOINTS THEMSELVES, kinds and all. This used to flatten them to bare positions
		// and spur each anchor to the nearest point of the resulting polygon; with the anchors
		// moved ONTO the lane that spur is degenerate, and the anchor node joined nothing. A
		// waypoint's Kind is now what decides which node it gets.
		const TArray<FStandWaypoint>& Waypoints = Instance.Definition->ServiceLane;

		const double Cosine = FMath::Cos(Instance.Heading);
		const double Sine = FMath::Sin(Instance.Heading);
		auto ToWorld = [&Instance, Cosine, Sine](const FVector2D& Point)
		{
			return Instance.Position
				+ FVector2D(Point.X * Cosine - Point.Y * Sine, Point.X * Sine + Point.Y * Cosine);
		};

		// EVERY CORNER IN WORLD SPACE FIRST, with both leg directions and the angle between
		// them. Rounding one reaches back along the legs it SHARES with its two neighbours, so
		// no corner can be decided alone and none laid until all have been measured.
		//
		// Held as VALUES, which is why the old "read fresh each time" guard has gone: nothing
		// here dereferences a node pointer across an AddGuidelineNode that could have
		// reallocated the slot array under it.
		const int32 CornerCount = Waypoints.Num();
		TArray<FVector2D> World, Back, Onward;
		TArray<double> Interior, Run;
		World.Reserve(CornerCount);
		for (const FStandWaypoint& Waypoint : Waypoints)
		{
			// CLOSED IMPLICITLY: the last corner joins the first, and the definition's array
			// does not repeat it - see UEntityDefinition::ServiceLane for why storing the
			// repeat would be a value that has to agree with another value beside it.
			World.Add(ToWorld(Waypoint.Local));
		}

		Back.SetNumZeroed(CornerCount);
		Onward.SetNumZeroed(CornerCount);
		Interior.SetNumZeroed(CornerCount);
		Run.SetNumZeroed(CornerCount);
		for (int32 At = 0; At < CornerCount; ++At)
		{
			const FVector2D& Previous = World[(At + CornerCount - 1) % CornerCount];
			const FVector2D& Next = World[(At + 1) % CornerCount];
			Back[At] = (Previous - World[At]).GetSafeNormal();
			Onward[At] = (Next - World[At]).GetSafeNormal();

			// UNSIGNED, and that is right for a convex corner and a reflex one alike: the
			// quadratic bulges toward its control point either way, so which side of the lane
			// the bend falls on changes nothing about how far back it has to reach.
			Interior[At] = FMath::Acos(
				FMath::Clamp(FVector2D::DotProduct(Back[At], Onward[At]), -1.0, 1.0));
			Run[At] = GuidelineGeom::CornerRunFor(LaneTurnRadius, Interior[At]);

			// Never past either neighbour, before the shared-leg clamp below sees it. A
			// near-hairpin corner asks for an unbounded run, and an infinity reaching that
			// clamp would scale BOTH corners of its leg to nothing rather than cutting this
			// one down to what its own legs can give.
			Run[At] = FMath::Min(Run[At],
				FMath::Min(FVector2D::Distance(World[At], Previous),
				           FVector2D::Distance(World[At], Next)));
		}

		// THE CLAMP. Two corners sharing a leg cannot both reach further than half of it, and
		// when they ask for more they SHRINK IN PROPORTION rather than one being cut to fit -
		// which would make the lane's shape depend on which corner the definition happened to
		// be authored from. A weld tolerance of straight is kept between them, because two
		// cuts that met exactly would leave a zero-length side edge.
		for (int32 At = 0; At < CornerCount; ++At)
		{
			const int32 Next = (At + 1) % CornerCount;
			const double Room = FMath::Max(
				FVector2D::Distance(World[At], World[Next]) - LaneWeldTolerance, 0.0);
			const double Want = Run[At] + Run[Next];
			if (Want > Room && Want > 0.0)
			{
				const double Scale = Room / Want;
				Run[At] *= Scale;
				Run[Next] *= Scale;
			}
		}

		// AN ANCHOR WAYPOINT DOES NOT GET A NEW NODE. The anchor already owns one, made at
		// placement, and FuelService routes to THAT handle - a lane that laid a fresh node at
		// the same position would look right in the overlay and route nothing.
		//
		// An anchor on the lane is mid-run by construction, so it is the no-bend case: one
		// node, used as both enter and exit. A definition that put an anchor ON a corner would
		// be asking for the bend to start inside the painted box, and is refused at the call
		// site below rather than laid crooked.
		auto NodeAt = [&Network, &Instance](
			const FStandWaypoint& Waypoint, const FVector2D& At) -> FGuidelineNodeId
		{
			if (Waypoint.Kind == EStandWaypointKind::Anchor)
			{
				for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
				{
					if (Resolved.Id == Waypoint.AnchorId)
					{
						return Resolved.Node;
					}
				}
				UE_LOG(LogAirside, Warning,
					TEXT("Stand lane names anchor '%s', which the instance does not have. "
					     "Laying a plain node; nothing will route to that service."),
					*Waypoint.AnchorId.ToString());
			}
			return Network.AddGuidelineNode(At, /*bDerived=*/true);
		};

		// TWO NODES PER ROUNDED CORNER - where the bend leaves the incoming leg and where it
		// rejoins the outgoing one - and ONE where there is no bend to lay.
		TArray<FGuidelineNodeId> Enter, Exit;
		TArray<FVector2D> EnterAt, ExitAt;
		Enter.Reserve(CornerCount); Exit.Reserve(CornerCount);
		EnterAt.Reserve(CornerCount); ExitAt.Reserve(CornerCount);

		for (int32 At = 0; At < CornerCount; ++At)
		{
			if (Run[At] < LaneWeldTolerance)
			{
				// Collinear, or clamped to nothing. One node and no bend: putting a curve
				// through a straight run would be inventing a corner. This is the case every
				// anchor takes, which is what "the lane runs THROUGH the box" means.
				const FGuidelineNodeId Node = NodeAt(Waypoints[At], World[At]);
				Enter.Add(Node);
				Exit.Add(Node);
				EnterAt.Add(World[At]);
				ExitAt.Add(World[At]);
				continue;
			}

			if (Waypoints[At].Kind == EStandWaypointKind::Anchor)
			{
				// AN AUTHORING ERROR, SAID OUT LOUD. A bend here would start inside the painted
				// box and the anchor's own node would sit on neither end of it, so the anchor
				// silently stops being on the lane - which is the defect this whole redesign
				// removed, reappearing one level up. Laid as a plain corner rather than refused
				// outright: a lane with a misplaced box is still a lane, and the log is what
				// gets it fixed. Airside.Entities.StandLaneCornersClearTheTruckLock is what
				// keeps the shipping definition out of this branch.
				UE_LOG(LogAirside, Warning,
					TEXT("Stand lane puts anchor '%s' on a %.0f degree corner, so the bend that "
					     "rounds it cannot start at the anchor's own node. Laying a plain "
					     "corner; nothing will route to that service."),
					*Waypoints[At].AnchorId.ToString(),
					FMath::RadiansToDegrees(UE_DOUBLE_PI - Interior[At]));
			}

			const FVector2D In = World[At] + Back[At] * Run[At];
			const FVector2D Out = World[At] + Onward[At] * Run[At];
			Enter.Add(Network.AddGuidelineNode(In, /*bDerived=*/true));
			Exit.Add(Network.AddGuidelineNode(Out, /*bDerived=*/true));
			EnterAt.Add(In);
			ExitAt.Add(Out);
		}

		TArray<FGuidelineEdgeId>& Lane = Result.Lanes.FindOrAdd(EntityId);
		TArray<FGuidelineNodeId>& Entries = Result.Entries.FindOrAdd(EntityId);
		for (int32 At = 0; At < CornerCount; ++At)
		{
			if (Enter[At] != Exit[At])
			{
				// THE BEND. Both legs' tangent lines meet AT the corner, so the single control
				// point they define is the corner itself - the quadratic case, and what makes
				// the curve leave each side tangentially instead of at an angle to it. The
				// same spelling FRoadGuidelineBuilder uses for a junction turn path.
				FGuidelineEdge Bend = MakeStandLaneEdge(
					Enter[At], Exit[At], EnterAt[At], ExitAt[At], EntityId);
				Bend.Control = World[At];
				Lane.Add(Network.AddGuidelineEdge(MoveTemp(Bend)));
			}

			const int32 Next = (At + 1) % CornerCount;
			Lane.Add(Network.AddGuidelineEdge(MakeStandLaneEdge(
				Exit[At], Enter[Next], ExitAt[At], EnterAt[Next], EntityId)));

			Result.Nodes.Add(Enter[At]);
			Result.Nodes.Add(Exit[At]);

			if (Waypoints[At].Kind == EStandWaypointKind::Entry)
			{
				// WHERE A ROAD MAY JOIN, recorded for the pass that links. See FResult::Entries.
				//
				// BOTH ENDS OF A ROUNDED ENTRY, not one of them. The Code C stand authors its
				// entries at the four corners where a crossing meets a run - the only points a
				// road outside the stand can reach at a heading the lane actually has - so an
				// entry is normally a CORNER, and a corner's own point carries no node at all
				// once it is rounded: the bend's control sits there and its two ends sit back
				// along the two legs. Which of those two a road should join depends on which
				// side the road is, which is the linking pass's question and not this one's, so
				// both are offered. AddUnique because a straight-through entry - what a
				// pivoting vehicle's zero run leaves - is one node offered twice.
				Entries.AddUnique(Enter[At]);
				Entries.AddUnique(Exit[At]);
			}
		}
		++Result.LanesBuilt;
	}

	if (Result.LanesBuilt > 0)
	{
		// One census line, beside the guideline builder's and FAnchorLink's. Zero lanes is
		// the common idle rebuild and stays quiet.
		//
		// THE ENTRY COUNT IS IN IT because nothing in this pass reads Result.Entries, so the
		// log is the only evidence the declared entries reached the graph at all - and a lane
		// laid with none of them is a stand no road can be joined to.
		int32 EntryNodes = 0;
		for (const TPair<FEntityInstanceId, TArray<FGuidelineNodeId>>& Entry : Result.Entries)
		{
			EntryNodes += Entry.Value.Num();
		}
		UE_LOG(LogAirside, Log, TEXT("Stand lanes: %d lane(s) laid, %d entry node(s)"),
			Result.LanesBuilt, EntryNodes);
	}

	return Result;
}

double FStandLaneBuild::TangentRunFor(double Gap)
{
	return FMath::Min(PreferredTangentRun, Gap * UE_DOUBLE_SQRT_2);
}
