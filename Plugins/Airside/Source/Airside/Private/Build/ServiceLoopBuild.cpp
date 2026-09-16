#include "Build/ServiceLoopBuild.h"

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
	 * One lane side, or one anchor spur. Both carry the owner, and neither is ever a target.
	 *
	 * One function rather than two because the two differ in nothing but their endpoints -
	 * and a spur that disagreed with the lane about which class may use it would be a lane a
	 * truck could reach and not leave.
	 */
	FGuidelineEdge MakeServiceEdge(FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D& PositionA, const FVector2D& PositionB, FEntityInstanceId Owner,
		bool bSpur)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;

		// Straight, spelled the way the builder spells it: the control ON the midpoint, which
		// is what GuidelineGeom::IsStraight tests for and what lets Sample short-circuit to
		// two points.
		Edge.Control = (PositionA + PositionB) * 0.5;

		// GroundVehicle, plus Emergency as every derived guideline carries. NOT Aircraft: an
		// aeroplane routed round the lane would be driving round itself.
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = FServiceLoopBuild::LaneWidth;

		// 0 is UNLIMITED - see FProfileGuideline::MaxWingspan. A span limit on a line no wing
		// uses could never bind, and the class has already refused aircraft.
		Edge.MaxWingspan = 0.0;
		Edge.bDerived = true;
		Edge.ServiceLoopOwner = Owner;

		// WHICH OF THE TWO THIS IS, said rather than worked out from the endpoints later -
		// see FGuidelineEdge::bServiceSpur for what reading it off the endpoints cost. The
		// two splits below copy from Original, so a spur cut in half stays a spur.
		Edge.bServiceSpur = bSpur;
		return Edge;
	}

	/**
	 * How far back along the lane a spur's join sits from the anchor's perpendicular foot, uu.
	 *
	 * A SPUR LEAVES THE LANE ALONG IT, NOT ACROSS IT. Its control point is the foot itself, so
	 * the first leg of the quadratic runs down the lane and the curve is tangent to it - there
	 * is no heading change at the junction at all, and nothing for FSpeedProfile to call a
	 * corner.
	 *
	 * WITH a run of p, a gap of q and the right angle between them that this construction
	 * always has, the quadratic's apex radius is 2p^2q^2 / (p^2+q^2)^(3/2). It peaks at
	 * p = q*sqrt(2), where it is 0.77q, and falls away on both sides.
	 *
	 * NOT THE PEAK, DELIBERATELY. The run is a DETOUR: the truck leaves the lane this far past
	 * the anchor and the curve brings it back, so every uu of run is driven twice. The peak
	 * for the hydrant's 1390 uu gap is a 1966 uu run, and asking for it lengthened a measured
	 * route by about 3000 uu for radius nobody needed. What is needed is the 471 uu a truck's
	 * steering lock allows - FAirframe::TightestFollowableRadius - and 800 uu of run clears
	 * that with margin at every gap a stand presents:
	 *
	 *     gap 1390 (hydrant)      -> 599 uu     gap 990 (equipment) -> 608 uu
	 *     gap 1400 (fixed GPU)    -> 599 uu     gap 757             -> 549 uu
	 *
	 * The curve is widest in the MIDDLE of that range rather than at its end, which is why one
	 * figure serves all of them.
	 *
	 * RUN IS ALSO ROOM, and on a crowded side there is not enough of it for every anchor to
	 * get its pair. Three of a Code C stand's anchors sit on the north side 900 uu apart, and
	 * two joins 800 uu either side of neighbours that close would overrun each other - so the
	 * equipment boxes get one spur apiece and the hydrant, the GPU and the tug get two.
	 *
	 * SHORTENING THE RUN DOES NOT BUY THE MISSING ONES. Joins from anchors 900 uu apart clear
	 * each other only below 450 uu of run, and 450 reaches 309 uu of radius against the 471 a
	 * truck's lock needs - so the choice is one good spur or two bad ones. Tried at 700 and
	 * measured: the same eight. What WOULD buy them is moving an anchor off that side, which
	 * is a change to the stand's layout and not to this.
	 *
	 * CAPPED AT THE PEAK for a close anchor, because past it a longer run makes the curve
	 * TIGHTER as well as longer - both costs, no benefit. TugStand waits 300 uu off the lane
	 * and takes the peak, 424 uu of run for 231 uu of radius; no geometry could do better with
	 * three metres to work in, and it is the last few metres of a journey that ends in a stop.
	 *
	 * WHY NOT ROUND THE JUNCTION AFTERWARDS INSTEAD: there is no room. A 90 degree turn at
	 * 471 uu needs 666 uu of run on each arm, so two spurs sharing a side need 1332 uu between
	 * them; three land on a Code C stand's north side, whose four gaps have 5250 uu to share
	 * and need 5328. Measured 2026-09-15, after a rounding pass was built and failed on
	 * exactly those junctions.
	 *
	 * ONE FORMULA, TWO SPECIALISATIONS: this is the asymmetric right-angled case. The symmetric
	 * case (same cut on both legs) is GuidelineGeom::CornerRunFor in Solve/GuidelineGeom.h, which
	 * holds the general two-leg formula they both derive from.
	 */
	constexpr double PreferredSpurRun = 800.0;







	/**
	 * Walk Distance along the RING from (Edge, Param) and report where it lands. Negative
	 * walks the other way. The sign of Distance is read against Edge's own A-to-B sense.
	 *
	 * ACROSS EDGE BOUNDARIES, which is the whole reason it exists. A ring side is split by
	 * every spur that has already joined it, so "800 uu along the lane" routinely lands on a
	 * different EDGE from the one the anchor is nearest - and a walk that stopped at the end
	 * of its own edge would report no room where the ring has plenty. On a Code C stand that
	 * cost the two equipment boxes their second spur: their joins belonged in the piece next
	 * door, which had 1600 uu going spare. Measured 2026-09-15.
	 *
	 * SPURS ARE NOT THE RING and are stepped over, so a ring node carries exactly two ring
	 * edges and the way onward is never ambiguous. Bounded by the lane's own edge count, so a
	 * ring that somehow did not close cannot spin here.
	 *
	 * OutForward says which way the walk was travelling when it stopped, in the LANDING
	 * edge's own sense - which the caller needs, because the landing edge may be parameterised
	 * the opposite way round from the one it started on.
	 */

}

FServiceLoopBuild::FResult FServiceLoopBuild::Build(URoadNetwork& Network)
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
			if (!Edge.bAlive || !Edge.ServiceLoopOwner.IsSet())
			{
				continue;
			}

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			Result.Lanes.FindOrAdd(Edge.ServiceLoopOwner).Add(Id);
			Result.Nodes.Add(Edge.A);
			Result.Nodes.Add(Edge.B);
		}
	}

	// By index, like FAnchorLink::Build: nothing here adds or removes an ENTITY, so holding
	// this reference across the mutations below is safe, and the handle still has to be built
	// by hand from the slot - the array elements have no stable handle of their own.
	// WHAT THE FLEET NEEDS OF A BEND, resolved once. A spur's run exists for exactly one
	// reason - so the truck that drives it does not have to crawl - so the figure it is sized
	// against is the truck's, not a constant of the lane's own.
	//
	// WITH MARGIN, because the run is chosen from a handful of candidates and the one that
	// merely touches the limit is one a re-measure could put on the wrong side of it.
	const double RequiredSpurRadius =
		UAirsideSettings::ResolveDefaultVehicle().TightestFollowableRadius() * 1.15;

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

		// THE POSITIONS ALONE, for now. UEntityDefinition::ServiceLane carries a KIND on every
		// waypoint - anchor, entry, plain - and this pass still treats the lane as a bare
		// polygon of corners, which is what it was before 2026-09-16. Task 4 of the stand
		// routing work is where the kinds start to mean something: an anchor waypoint reusing
		// the anchor's own node instead of being spurred to, and an entry being where a road
		// may join. Flattening here rather than changing that behaviour in the same breath.
		TArray<FVector2D> Local;
		Local.Reserve(Instance.Definition->ServiceLane.Num());
		for (const FStandWaypoint& Waypoint : Instance.Definition->ServiceLane)
		{
			Local.Add(Waypoint.Local);
		}
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
		// A POLYLINE IS HOW A LANE IS DESCRIBED, NOT HOW IT IS DRIVEN. UEntityDefinition::
		// ServiceLane is a sequence of straights - see its header for why - but a corner where
		// two of them meet is a vertex whose heading changes instantly, and FSpeedProfile
		// calls one of those untakeable at any speed. The bend carries the turn instead,
		// exactly as an anchor spur does one level down.
		//
		// Held as VALUES, which is why the old "read fresh each time" guard has gone: nothing
		// here dereferences a node pointer across an AddGuidelineNode that could have
		// reallocated the slot array under it.
		const int32 CornerCount = Local.Num();
		TArray<FVector2D> World, Back, Onward;
		TArray<double> Interior, Run;
		World.Reserve(CornerCount);
		for (const FVector2D& Point : Local)
		{
			// CLOSED IMPLICITLY: the last corner joins the first, and the definition's array
			// does not repeat it - see UEntityDefinition::ServiceLane for why storing the
			// repeat would be a value that has to agree with another value beside it.
			World.Add(ToWorld(Point));
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
			Run[At] = GuidelineGeom::CornerRunFor(FServiceLoopBuild::LaneTurnRadius, Interior[At]);

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
				// through a straight run would be inventing a corner.
				const FGuidelineNodeId Node =
					Network.AddGuidelineNode(World[At], /*bDerived=*/true);
				Enter.Add(Node);
				Exit.Add(Node);
				EnterAt.Add(World[At]);
				ExitAt.Add(World[At]);
				continue;
			}

			const FVector2D In = World[At] + Back[At] * Run[At];
			const FVector2D Out = World[At] + Onward[At] * Run[At];
			Enter.Add(Network.AddGuidelineNode(In, /*bDerived=*/true));
			Exit.Add(Network.AddGuidelineNode(Out, /*bDerived=*/true));
			EnterAt.Add(In);
			ExitAt.Add(Out);
		}

		TArray<FGuidelineEdgeId>& Lane = Result.Lanes.FindOrAdd(EntityId);
		for (int32 At = 0; At < CornerCount; ++At)
		{
			if (Enter[At] != Exit[At])
			{
				// THE BEND. Both legs' tangent lines meet AT the corner, so the single control
				// point they define is the corner itself - the quadratic case, and what makes
				// the curve leave each side tangentially instead of at an angle to it. The
				// same spelling FRoadGuidelineBuilder uses for a junction turn path.
				FGuidelineEdge Bend = MakeServiceEdge(
					Enter[At], Exit[At], EnterAt[At], ExitAt[At], EntityId, /*bSpur=*/false);
				Bend.Control = World[At];
				Lane.Add(Network.AddGuidelineEdge(MoveTemp(Bend)));
			}

			const int32 Next = (At + 1) % CornerCount;
			Lane.Add(Network.AddGuidelineEdge(MakeServiceEdge(
				Exit[At], Enter[Next], ExitAt[At], EnterAt[Next], EntityId, /*bSpur=*/false)));

			Result.Nodes.Add(Enter[At]);
			Result.Nodes.Add(Exit[At]);
		}
		++Result.LoopsBuilt;

		// SPURS. TWO to each service anchor, one curving each way along the lane.
		//
		// WHY TWO. A spur is tangent to the lane at its join, which makes it smooth for a
		// truck arriving from ONE side and a hairpin from the other. The lane is a closed
		// ring, so a truck CAN come round the long way - but the route search costs length,
		// takes the short way, and the agent crabs into position at the end of it. Reported
		// from play 2026-09-15 with a picture: "the truck comes from the bottom right of the
		// stand and then crabs into its position".
		//
		// A mirrored pair gives every anchor a smooth approach from either direction, and the
		// search picks whichever suits the journey. They meet at the anchor in a V, which is
		// sharp - deliberately so: a truck has no business driving THROUGH a painted
		// equipment box, and FSpeedProfile costs that V at MinSteeringSpeed, which is what makes
		// going round cheaper than cutting through.
		//
		// On a Code C stand none of them crosses the aeroplane - measured on the sampled
		// geometry by Airside.Build.SpursLeaveTheLaneTangentially, not trusted here.
		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			if (TraversalForRole(Resolved.Role) == ETraversalClass::Aircraft)
			{
				// An aircraft anchor is not something a truck drives to. There are none on a
				// Code C stand today, and a definition that grew one would want a painted
				// lead-in of its own, not a spur onto the service lane.
				continue;
			}

			const FGuidelineNode* AnchorNode = Network.GetGuidelineNode(Resolved.Node);
			if (AnchorNode == nullptr || AnchorNode->Incident.Num() > 0)
			{
				// Already joined - by hand, or by a pass that survived. Laying the pair again
				// would leave four lines into one painted box.
				continue;
			}
			const FVector2D At = AnchorNode->Position;

			/**
			 * Lay one spur, curving the way Direction says. False only when the ring cannot
			 * be walked at all, which on a closed ring means the graph is broken.
			 */
			auto LaySpur = [&](double Direction) -> bool
			{
				// Across the lane's CURRENT edges, re-read every time: an earlier spur has
				// split the very side this one is about to meet - its own mirror twin, often -
				// and it must see the halves rather than the edge that is gone.
				FGuidelineEdgeId BestEdge;
				double BestDistance = TNumericLimits<double>::Max();
				double BestParam = 0.0;
				FVector2D BestPoint = FVector2D::ZeroVector;

				for (const FGuidelineEdgeId& EdgeId : Lane)
				{
					// THE RING ONLY, NEVER ANOTHER SPUR. Left to itself the search chains: on
					// a Code C stand EquipmentFwd is 900 uu from HydrantPit's spur against 990
					// from the north side, so three of the five used to hang off each other.
					//
					// TWO REASONS THAT IS WRONG, and the second is why it is fixed here rather
					// than tolerated. A spur is a QUADRATIC, so a spur joining one leaves it at
					// an angle the tangent construction below cannot straighten - 315 uu of
					// radius against the 471 a truck's lock needs, measured 2026-09-15. And
					// Airside.Entities.StandLaneClearsTheAircraft judges the DEFINITION's own
					// segments and nothing else - it stopped judging spurs at all when the
					// anchors moved onto the lane - so a chained spur is geometry no test above
					// this one looks at.
					const FGuidelineEdge* Candidate = Network.GetGuidelineEdge(EdgeId);
					if (Candidate == nullptr || !Candidate->bAlive || Candidate->bServiceSpur)
					{
						continue;
					}

					TArray<FVector2D> Points;
					if (!Network.SampleGuideline(EdgeId, Points))
					{
						continue;
					}

					int32 Span = 0;
					double Fraction = 0.0;
					const double Distance =
						GuidelineGeom::NearestOnPolyline(Points, At, Span, Fraction);
					if (Distance >= BestDistance)
					{
						continue;
					}

					BestDistance = Distance;
					BestEdge = EdgeId;
					BestParam = GuidelineGeom::ParamAtSample(Span, Fraction, Points.Num());
					BestPoint = FMath::Lerp(Points[Span], Points[Span + 1], Fraction);
				}

				if (!BestEdge.IsSet())
				{
					return false;
				}

				// SLIDE THE JOIN ALONG THE LANE, so the spur can leave along it rather than
				// cross it. The anchor does not move: a hydrant pit is where a hydrant pit is,
				// and only the point the line meets the lane at is ours to choose.
				//
				// ALONG THE RING, not along this EDGE. The ring is continuous and its edges
				// are just where earlier spurs happened to cut it, so the join is as free to
				// land on the next piece as on this one - see WalkRing for the two spurs that
				// were refused when it was not.
				//
				// AND THE RUN IS MEASURED, NOT ASSUMED. SpurRunFor solves for a right angle
				// against a straight host, which is what a lane side used to be everywhere; a
				// join that lands on a ROUNDED CORNER has neither, and took 433 uu of radius
				// where the formula promised 599. So several runs are tried and the shortest
				// one that actually reaches FServiceLoopBuild::LaneTurnRadius is kept - run is detour, and the
				// truck drives every uu of it twice.
				FGuidelineEdgeId JoinEdge;
				double JoinParam = 0.0;
				FVector2D SpurControl = BestPoint;
				{
					const double Shortest = FServiceLoopBuild::TangentRunFor(BestDistance);
					bool bClears = false;

					// Six, spanning one to three times the nominal run. Enough to clear a
					// corner landing and few enough that a spur costs six ring walks, which
					// are a handful of additions each.
					constexpr int32 Attempts = 6;
					for (int32 Try = 0; Try < Attempts; ++Try)
					{
						const double Candidate = Shortest * (1.0 + 0.4 * Try);

						FGuidelineEdgeId TryEdge;
						double TryParam = 0.0;
						bool bForward = true;
						if (!FServiceLoopBuild::WalkRing(Network, BestEdge, BestParam, Candidate * Direction,
								TryEdge, TryParam, bForward))
						{
							continue;
						}

						const FGuidelineEdge* Host = Network.GetGuidelineEdge(TryEdge);
						const FGuidelineNode* HostA =
							Host != nullptr ? Network.GetGuidelineNode(Host->A) : nullptr;
						const FGuidelineNode* HostB =
							Host != nullptr ? Network.GetGuidelineNode(Host->B) : nullptr;
						if (HostA == nullptr || HostB == nullptr)
						{
							continue;
						}

						// Analytic, not a difference of samples: a tangent measured off a
						// sampled chord is a second evaluator, and it shows up as a degree or
						// two of turn at exactly the junction this exists to make turnless.
						const FVector2D JoinPoint = GuidelineGeom::Eval(
							HostA->Position, Host->Control, HostB->Position, TryParam);
						const FVector2D Along = GuidelineGeom::Tangent(
							HostA->Position, Host->Control, HostB->Position, TryParam);

						// BACK the way the walk came, by the distance it walked - so the
						// quadratic's first leg lies on the ring and the curve leaves it
						// tangentially.
						const FVector2D Control =
							JoinPoint - (bForward ? Along : -Along) * Candidate;

						// The spur as it will actually be laid: anchor, control, join.
						const double Radius = GuidelineGeom::TightestRadius(At, Control, JoinPoint);

						// THE SHORTEST THAT CLEARS, or the shortest full stop.
						//
						// The first attempt is kept unconditionally, so a spur that can never
						// be made wide enough is at least the least detour - it is the last
						// few metres of a journey that ends in a stop, and a long windy
						// approach to a box three metres off the lane helps nobody. TugStand
						// is exactly that: no run reaches the limit and chasing it turned a
						// 520 uu spur into 1594. Measured 2026-09-15.
						if (Try == 0 || Radius >= RequiredSpurRadius)
						{
							JoinEdge = TryEdge;
							JoinParam = TryParam;
							SpurControl = Control;
							bClears = Radius >= RequiredSpurRadius;
						}

						if (bClears)
						{
							break;
						}
					}

					if (!JoinEdge.IsSet())
					{
						return false;
					}
				}

				BestEdge = JoinEdge;
				BestParam = JoinParam;

				FGuidelineNodeId Join;
				FGuidelineEdgeId Head, Tail;
				if (!Network.SplitGuidelineEdge(
						BestEdge, BestParam, LaneWeldTolerance, Join, Head, Tail))
				{
					return false;
				}

				if (Head.IsSet() && Tail.IsSet())
				{
					// An actual split, not a weld to an existing endpoint: the halves inherit
					// the owner from the original edge (SplitGuidelineEdge copies every field
					// but the endpoint and control that moved), which is what keeps the whole
					// lane recognisable as one entity's after any number of splits.
					Lane.Remove(BestEdge);
					Lane.Add(Head);
					Lane.Add(Tail);
					Result.Nodes.Add(Join);
				}

				const FVector2D JoinAt = Network.GetGuidelineNode(Join)->Position;

				FGuidelineEdge Spur =
					MakeServiceEdge(Resolved.Node, Join, At, JoinAt, EntityId, /*bSpur=*/true);

				// The edge is spelled anchor-to-join, which changes nothing: a quadratic read
				// backwards is the same curve, and GuidelineGeom::Sample takes the ends in
				// whichever order it is given.
				Spur.Control = SpurControl;

				Lane.Add(Network.AddGuidelineEdge(MoveTemp(Spur)));
				++Result.SpursBuilt;
				return true;
			};

			// BOTH WAYS, ALWAYS. The ring is closed, so there is always somewhere to walk to;
			// what used to refuse a direction was a room check that could not see past the
			// edge it started on.
			LaySpur(1.0);
			LaySpur(-1.0);
		}
	}

	if (Result.LoopsBuilt > 0)
	{
		// One census line, beside the guideline builder's and FAnchorLink's. Zero lanes is
		// the common idle rebuild and stays quiet.
		UE_LOG(LogAirside, Log, TEXT("Service loops: %d lane(s) laid, %d anchor spur(s)"),
			Result.LoopsBuilt, Result.SpursBuilt);
	}

	return Result;
}

double FServiceLoopBuild::TangentRunFor(double Gap)
{
	return FMath::Min(PreferredSpurRun, Gap * UE_DOUBLE_SQRT_2);
}

bool FServiceLoopBuild::WalkRing(const URoadNetwork& Network, FGuidelineEdgeId Edge,
	double Param, double Distance, FGuidelineEdgeId& OutEdge, double& OutParam,
	bool& OutForward)
{
	OutEdge = Edge;
	OutParam = Param;
	OutForward = Distance >= 0.0;

	double Remaining = FMath::Abs(Distance);

	for (int32 Step = 0; Step <= Network.GetGuidelineEdges().Num(); ++Step)
	{
		const FGuidelineEdge* Here = Network.GetGuidelineEdge(OutEdge);
		TArray<FVector2D> Points;
		if (Here == nullptr || !Network.SampleGuideline(OutEdge, Points) || Points.Num() < 2)
		{
			return false;
		}

		const double Length = GuidelineGeom::PolylineLength(Points);
		const double Available = OutForward ? Length * (1.0 - OutParam) : Length * OutParam;
		if (Available >= Remaining)
		{
			OutParam = GuidelineGeom::ParamAtArcOffset(
				Points, OutParam, OutForward ? Remaining : -Remaining);
			return true;
		}

		Remaining -= Available;

		// Over the node at that end and onto the ring's next edge, carrying on in the
		// same direction of travel - which is whichever of that edge's ends is NOT the
		// node just arrived at.
		const FGuidelineNodeId At = OutForward ? Here->B : Here->A;
		const FGuidelineNode* Node = Network.GetGuidelineNode(At);
		if (Node == nullptr)
		{
			return false;
		}

		FGuidelineEdgeId Next;
		for (const FGuidelineEdgeId& Id : Node->Incident)
		{
			if (Id == OutEdge)
			{
				continue;
			}
			const FGuidelineEdge* Candidate = Network.GetGuidelineEdge(Id);
			// OWNED AND NOT A SPUR is what makes an edge part of the RING. Testing only
			// for "not a spur" was enough while this ran before any road link existed;
			// a link hangs off a lane node too and carries no owner by design, so the
			// walk would have stepped off the ring and onto the road.
			if (Candidate != nullptr && Candidate->bAlive && !Candidate->bServiceSpur
				&& Candidate->ServiceLoopOwner.IsSet())
			{
				Next = Id;
				break;
			}
		}
		if (!Next.IsSet())
		{
			return false;
		}

		const FGuidelineEdge* NextEdge = Network.GetGuidelineEdge(Next);
		OutForward = NextEdge->A == At;
		OutParam = OutForward ? 0.0 : 1.0;
		OutEdge = Next;
	}
	return false;
}
