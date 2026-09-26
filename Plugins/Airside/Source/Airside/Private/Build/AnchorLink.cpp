#include "Build/AnchorLink.h"

#include "AirsideLog.h"
#include "Build/AnchorLinkFinder.h"
#include "Build/StandLayoutBuild.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/IcaoCode.h"
#include "Solve/LinkGeom.h"
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
	// carries Entry waypoints, and FStandLayoutBuild::FResult::Entries reports the node each one
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
		//
		// NONE IS NOT A TYPO. A fuel depot has no DesignAircraft (see BuildFuelDepot) and
		// passes FName() here on purpose, falling back to Code C's radius exactly as that
		// header says - so an unset code stays silent. Anything else that fails to parse is a
		// human's UAircraftType::Code typed wrong, which is the ONE place that can happen: the
		// table functions used to take the fallback silently on any bad string, so a typo
		// became a lead-in painted for the wrong aeroplane with no line in the log to say so.
		if (Code.IsNone())
		{
			return IcaoCode::RadiusForLetter(EIcaoCode::C);
		}

		if (const TOptional<EIcaoCode> Parsed = IcaoCode::Parse(Code.ToString()))
		{
			return IcaoCode::RadiusForLetter(*Parsed);
		}

		UE_LOG(LogAirside, Warning,
			TEXT("UAircraftType::Code '%s' is not an ICAO letter A-F; its painted lead-in "
			     "radius falls back to Code C's."),
			*Code.ToString());
		return IcaoCode::RadiusForLetter(EIcaoCode::C);
	}

	/** What a stand's aircraft lead-in is laid to: the widest wing it takes, and its radius. */
	struct FLeadInSizing
	{
		/** uu; 0 is UNLIMITED, FProfileGuideline::MaxWingspan's own convention. */
		double MaxWingspan = 0.0;
		/** uu - see RadiusForCode above. */
		double Radius = 0.0;
	};

	/**
	 * THE ONE RULE for sizing a lead-in: the stand's own LETTER - the one admission reads
	 * (IcaoCode::StandAdmits compares by letter against the captured DesignWingspan) - for
	 * both the painted radius and the span limit.
	 *
	 * FROM THE CAPTURED DesignWingspan, not the definition's design aircraft (final review
	 * I5). A drawn D, E or F stand has NO design aircraft (UAirsideSettings::
	 * ResolveLargestAircraftOfLetter returns null for every letter today), and reading the
	 * aircraft gave it Code C's 25 m radius and no span limit at all. And even with one,
	 * the aircraft's RAW span was a second rule disagreeing with the first: a Code C stand
	 * designed around the A320 (34.1 m) limited its lead-in to 34.1 m, so a 737-800 (35.8 m,
	 * also C) was ADMITTED to the stand and then TooWide on the only line into it.
	 * MaxWingspanForLetter is the letter's own ceiling - exclusive for A-E, so an edge's
	 * "Wingspan > MaxWingspan" refuses exactly what StandAdmits refuses, bar the one span AT
	 * the ceiling, which admission has already turned away.
	 *
	 * AN UNMEASURED STAND (DesignWingspan 0, the raw PlaceEntity fixtures and every depot)
	 * keeps the old reading - the design aircraft if there is one, else unlimited at Code C's
	 * radius - because 0 says nothing about a letter, and "unknown admits anything" is
	 * StandAdmits' own contract for it.
	 */
	FLeadInSizing LeadInSizingFor(const FEntityInstance& Instance)
	{
		FLeadInSizing Out;
		if (Instance.IsStand() && Instance.DesignWingspan > 0.0)
		{
			// CodeForWingspan, not Parse(LetterForWingspan(...)) - that FString round trip was
			// typed at this site, StandMarkingBuilder's glyph letter and IcaoCode.cpp's own
			// StandAdmits (#292); CodeForWingspan is the primitive all three share, and it is
			// total over a positive span, so there is no TOptional left to read through.
			const EIcaoCode Letter = IcaoCode::CodeForWingspan(Instance.DesignWingspan);
			Out.MaxWingspan = IcaoCode::MaxWingspanForLetter(Letter);
			Out.Radius = IcaoCode::RadiusForLetter(Letter);
			return Out;
		}
		const UAircraftType* Design = Instance.Definition != nullptr ? Instance.Definition->DesignAircraft.Get() : nullptr;
		Out.MaxWingspan = Design != nullptr ? Design->Footprint.Wingspan : 0.0;
		Out.Radius = RadiusForCode(Design != nullptr ? Design->Code : FName());
		return Out;
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
	 * other than FStandLayoutBuild. The caller then falls back to the straight lead-in rather
	 * than guessing a heading.
	 *
	 * THE GATHER ONLY, since 2026-09-26 (#306): the DECISION - which of the candidates below
	 * wins, and why - is LinkGeom::ChooseDeparture now, a pure function over plain geometry that
	 * has its own world-free test. This half stays here because walking Node->Incident needs the
	 * graph, which Solve/ may not see.
	 */
	bool EntryDeparture(const URoadNetwork& Network, FGuidelineNodeId NodeId,
		const FVector2D& Toward, FVector2D& OutAlong)
	{
		const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
		if (Node == nullptr)
		{
			return false;
		}

		TArray<LinkGeom::FDepartureCandidate> Candidates;
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
			const FVector2D Leaving = (bFromB ? -Tangent : Tangent).GetSafeNormal();
			if (Leaving.IsNearlyZero())
			{
				continue;
			}

			const bool bBend = !GuidelineGeom::IsStraight(A->Position, Edge->Control, B->Position);
			const double Room = bBend
				? FVector2D::Distance(bFromB ? B->Position : A->Position, Edge->Control)
				: FVector2D::Distance(A->Position, B->Position);

			// BOTH SENSES OF EVERY EDGE, since 2026-09-17, and this is the fix for a defect the
			// shape change exposed rather than caused. What is wanted is the LINE the lead-in
			// should leave along, and a line has two directions; the connector takes whichever
			// of them faces the road.
			//
			// IT USED TO FALL OUT FOR FREE. A lane entry sat mid-cycle with TWO incident edges
			// whose headings were exact opposites, so offering one sense of each offered both
			// senses of the line. A LAYOUT entry begins its bay's arrive leg and has exactly
			// ONE edge, pointing into the stand - so the only candidate faced away from the
			// road, the lead-in was built leaving the entry in that direction, and it hairpinned
			// back: measured as R=93 uu against a lock of 699, with five vertices turning
			// instantly, the sharpest at 71 degrees.
			Candidates.Add({ Leaving, bBend, Room });
			Candidates.Add({ -Leaving, bBend, Room });
		}
		return LinkGeom::ChooseDeparture(Candidates, Toward, OutAlong);
	}

	/**
	 * True when a road link is ALREADY attached at this node.
	 *
	 * ASKED BY KIND, NOT BY COUNT, and that distinction is the whole of this function. It used
	 * to be "more than two incident edges", which was true only while every entry sat mid-cycle
	 * on a lane with exactly two layout edges through it. The four-leg layout breaks that in
	 * both directions at once: an ENTRY node begins its bay's arrive leg and has ONE edge, and
	 * an EXIT node is shared by every bay on its side and has THREE - so a stand's exits read as
	 * already joined and were skipped in silence, which is 1 of 7 lead-ins joined.
	 *
	 * A link is the edge that is NOT part of a layout: FStandLayoutBuild stamps
	 * StandGeometryOwner on everything it lays and FAnchorLink deliberately leaves its lead-ins
	 * unowned, so "unowned edge here" and "a road has been joined here" are the same statement.
	 */
	bool AlreadyJoined(const URoadNetwork& Network, FGuidelineNodeId NodeId)
	{
		const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
		if (Node == nullptr)
		{
			return true;
		}

		for (const FGuidelineEdgeId& Id : Node->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id);
			if (Edge != nullptr && Edge->bAlive && !Edge->StandGeometryOwner.IsSet())
			{
				return true;
			}
		}
		return false;
	}
}

void FAnchorLink::Gather(URoadNetwork& Network, double MaxLeadIn, double ServiceLinkRadius,
	TArray<FPendingLink>& OutPending, TSet<FGuidelineNodeId>& OutAnchorNodes)
{
	// THE LANES FIRST. A service anchor is a waypoint ON its stand's lane, so it is already
	// joined by the time the walk below asks, and is skipped there rather than cast at a road
	// on the far side of the aeroplane - and the LANE becomes the thing that links.
	const FStandLayoutBuild::FResult Lanes = FStandLayoutBuild::Build(Network);

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

		// Network.EntityIdAt, not a hand-built handle (#79, #173): the slot map is the one
		// place allowed to know a handle is {index, generation}.
		const FEntityInstanceId EntityId = Network.EntityIdAt(Index);

		// The stand's LETTER is what its lead-in can take - see LeadInSizingFor for why the
		// letter and not the design aircraft. A Code C stand's link then refuses a widebody by
		// the ordinary wingspan rule rather than by a special case, and the search reports
		// TooWide instead of a bare "no route".
		//
		// The same letter decides how wide the painted line sweeps: aerodromes are dimensioned
		// by code letter, and the line is laid once for the largest aircraft the stand admits.
		const FLeadInSizing Sizing = LeadInSizingFor(Instance);
		const double StandWingspan = Sizing.MaxWingspan;
		const double StandRadius = Sizing.Radius;

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
			// wingspan limit, the reach and the strategy in step - and FPendingLink::For (#306)
			// is that same step, kept once for every caller rather than once per lambda.
			const auto AddPoseLink = [&](double Out)
			{
				OutPending.Add(FPendingLink::For(Instance.PoseNode, Pose->Position,
					FVector2D(FMath::Cos(Out), FMath::Sin(Out)), TraversalForRole(Instance.PoseRole),
					StandRadius, StandWingspan, MaxLeadIn, ServiceLinkRadius));
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

			// A GROUND VEHICLE REACHES A STAND WITH A LAYOUT THROUGH THE LAYOUT, OR NOT AT ALL.
			//
			// THIS IS StandIsEnteredWhereItDeclares APPLIED TO ANCHORS, and the loop reaches
			// here only for one the layout did NOT join - the Incident test above has already
			// passed over every anchor a serve leg ends at. So what is left is an anchor with
			// no bay, and a direct link to it is the shortcut the declared entries exist to
			// make unrepresentable: TugStand sits at (350, -1400), its link ran dead straight
			// from the road to it, and that line crosses the wing keep-out at |y| = 1400 -
			// under the wing, which NO vehicle may do. The careful legs and the shortcut were
			// in one graph together and RouteSearch costs by length.
			//
			// AND IT WAS TAKING THE ROAD THE ENTRIES NEED. That same link split the road 506 uu
			// from the port contact and clamped its square corner to 404 uu against a lock of
			// 699. Two claims on one stretch of road, one of which nothing drives.
			//
			// WARNED, NOT DROPPED IN SILENCE. Nothing routes to TugStand today - FPushbackRun
			// couples at the AIRCRAFT's own NoseGear service point, not at the stand's anchor -
			// but an anchor that becomes unreachable is a fact about the stand, and a stand
			// that grows a service with no bay should say so rather than look connected.
			if (TraversalForRole(Declared->Role) == ETraversalClass::GroundVehicle
				&& !Instance.Definition->ServiceBays.IsEmpty())
			{
				UE_LOG(LogAirside, Warning,
					TEXT("Anchor '%s' has no service bay on a stand that has a layout, so no "
					     "road link is cast to it: a direct one would cross the wing keep-out. "
					     "Give it a bay in UEntityDefinition::BuildStandTemplate to make it "
					     "reachable."),
					*Resolved.Id.ToString());
				continue;
			}

			// FPendingLink::For (#306): the ray-vs-proximity strategy, the span cap and the
			// reach all fall out of the anchor's own traversal class - see its own doc comment.
			OutPending.Add(FPendingLink::For(Resolved.Node, Node->Position,
				FVector2D(FMath::Cos(Heading), FMath::Sin(Heading)), TraversalForRole(Declared->Role),
				StandRadius, StandWingspan, MaxLeadIn, ServiceLinkRadius));
		}

		// THE LANE'S OWN LINK TO A ROAD IS NOT CAST HERE. It leaves a DECLARED ENTRY, and every
		// declared entry of every stand is gathered in one pass of its own below - see there
		// for why it cannot be done inside this loop.
	}

	// A ROAD JOINS A STAND WHERE THE STAND SAYS IT MAY, 2026-09-16.
	//
	// ONE LINK PER DECLARED ENTRY. UEntityDefinition::ServiceLane authors Entry waypoints and
	// FStandLayoutBuild::FResult::Entries reports the node each one became, so this asks only
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

		// The same painted radius the stand's other links sweep at - see RadiusForCode.
		//
		// DEAD ON EVERY LANE LINK THAT LEAVES ALONG ITS LANE, and said here because tuning
		// IcaoCode::RadiusForLetter and watching a stand's entry for the effect is a session
		// lost. Join sizes its fillet from LaneRadius wherever it has one, and it has one on
		// every entry link whose EntryDeparture resolves - which is all of them on a lane laid
		// by FStandLayoutBuild. What is left for this figure is the one case that has no lane
		// heading to leave along: an entry whose departure cannot be read, where Join falls
		// back to the circular fillet Link.Radius / tan(theta/2). It is carried rather than
		// dropped because that fallback still needs a radius, and 2500 uu for a Code C is the
		// painted line's own.
		const double StandRadius = LeadInSizingFor(*Instance).Radius;

		// ONE STATEMENT OF WHAT AN ENTRY LINK IS, used by the probe below and by the link
		// finally emitted. Written twice they could differ, and the probe would then be
		// measuring something other than the link it decides.
		auto EntryLink = [&Declared, StandRadius, ServiceLinkRadius](
			FGuidelineNodeId NodeId, const FVector2D& At)
		{
			// GroundVehicle, not a ray: a vehicle may genuinely arrive from any side, and the
			// player has no authored heading to aim at - the same reasoning that governs every
			// other service link, and the reason a declared entry needs no rule of its own.
			// FPendingLink::For (#306) turns that class alone into Proximity/no-span-cap/
			// ServiceLinkRadius; MaxLeadIn is passed 0.0 because the Aircraft branch that would
			// read it can never be taken here.
			FPendingLink Link = FPendingLink::For(NodeId, At, FVector2D(1.0, 0.0),
				ETraversalClass::GroundVehicle, StandRadius, /*StandWingspan=*/0.0,
				/*MaxLeadIn=*/0.0, ServiceLinkRadius);

			// WHICH STAND, which is what makes this a lane link: Join reads it to lay the
			// lead-in along the lane, and Build reads it to report a stand that joined nothing
			// once rather than once per entry.
			Link.LaneOwner = Declared.Key;
			return Link;
		};

		// ONE LINK PER DECLARED ENTRY, and no grouping at all.
		//
		// THE PAIRING IS DELETED WITH THE LANE THAT NEEDED IT. A lane's entries were authored
		// at CORNERS, and a rounded corner's own point carries no node - the bend's control
		// sits there and its two ends sit back along the two legs - so an entry reached the
		// graph as a PAIR and something had to choose between them. The layout authors every
		// entry on a STRAIGHT, so there is exactly one node at it and nothing to choose.
		//
		// AND THE NEAREST-ENTRY REFUSAL IS DELETED TOO, which is the bigger change. It existed
		// because a cycle offered entries on all four sides of a stand, and a road alongside
		// could be "in reach" of the far ones, whose connectors would then run the whole depth
		// of the stand across the lane's own crossings. The layout declares entries only where
		// a road is meant to meet it - all on the aft edge - and the user's ruling of
		// 2026-09-17 is that EVERY service bay gets its own way in, so that a vehicle never
		// threads past a parked one. Refusing all but the nearest is exactly the behaviour that
		// ruling forbids. What bounds a connector now is ServiceLinkRadius alone, which is the
		// question actually being asked: is a road within reach of THIS entry.
		//
		// ONE LOOP, NOT TWO, since #177. There used to be a first pass that called Resolve for
		// EVERY declared node - including one already joined by hand, which this loop's
		// AlreadyJoined check was always going to throw away - purely to fill a
		// TMap<FGuidelineNodeId, FEntryReach> with a Contact/Distance/bReaches that a second
		// pass over the SAME nodes then read only Node/At from. The probe's OUTCOME never
		// decided anything: the comment on OutPending.Add below is unchanged from before this
		// fix and says so - a link is emitted "EVEN WHEN THE PROBE FOUND NOTHING". So the
		// search itself is gone, not merely the fields it filled; FAnchorLink::Build below
		// still calls Resolve exactly once for the link this loop emits, the same as it always
		// did for a pose or an anchor.
		//
		// NOT CACHED HERE EITHER, and that is deliberate rather than an oversight: a link's
		// FLinkHit names an EDGE, and an earlier entry's Join a few hundred lines below can
		// split the very edge a later entry would resolve to - all four of one stand's entries
		// regularly share the single road behind it. A Hit taken NOW, before any of this pass's
		// joins, would be stale by the time Build's loop reaches a later entry; Build must
		// resolve against the graph AS IT STANDS AT THAT MOMENT, which only a fresh Resolve
		// there can see.
		for (const FGuidelineNodeId& NodeId : Declared.Value)
		{
			if (AlreadyJoined(Network, NodeId))
			{
				continue;
			}

			const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
			if (Node == nullptr)
			{
				continue;
			}

			// EMITTED EVEN WHEN THE PROBE FOUND NOTHING, deliberately: Build is what reports a
			// stand that joined nothing at all, and it can only do that for a link it was
			// given. A stand out of reach of every road therefore gets one warning line naming
			// it, not silence.
			OutPending.Add(EntryLink(NodeId, Node->Position));
		}
	}
}

FLinkHit FAnchorLink::Resolve(const URoadNetwork& Network, const FPendingLink& Link,
	const TSet<FGuidelineNodeId>& AnchorNodes)
{
	// STRATEGY DISPATCH. Left unset (FLinkHit::IsSet false) when the finder finds nothing -
	// Find never writes to Hit unless it claims, the same contract IRoadSnapRule's Resolve
	// documents for the same reason.
	//
	// COUNTED HERE, for #177: this is the one call site of ILinkFinder::Find, so a test that
	// wants to know whether a link was searched once or twice reads
	// URoadNetwork::AnchorLinkFindCallCountForTest rather than instrumenting either finder.
	Network.NoteAnchorLinkFind();
	FLinkHit Hit;
	LinkFinderFor(Link.Kind).Find(Network, Link, AnchorNodes, Hit);
	return Hit;
}

FGuidelineNodeId FAnchorLink::Join(URoadNetwork& Network, FPendingLink& Link, const FLinkHit& Hit,
	TSet<FGuidelineNodeId>& AnchorNodes, const FChassis& LargestServiceVehicle)
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
	// polyline to do it. Everything after measures against this same array, held in Approach
	// (below) rather than in a local of its own.
	TArray<FVector2D> Curve;
	if (!Network.SampleGuideline(Hit.Edge, Curve))
	{
		return FGuidelineNodeId();
	}

	// WHICH SHAPE THE JOIN TAKES - StraightRay, Crossing or LaneChange - IS LinkGeom::Plan's
	// DECISION NOW (#306, regression of #76). Join used to hide this as an if/else on
	// Link.Kind, Link.LaneOwner.IsSet() and LaneRadius > 0.0, mutating Param/Corner/Dir/
	// OutLaneControl/LaneRun in place with no name for which branch had run; see Solve/LinkGeom.h
	// for the full account (why the join has to MOVE off the search's own hit, why a road across
	// a lane's end wants a different shape from one alongside it, and why EntryDeparture's
	// DECISION half moved out from under this function while its GATHER half - which needs the
	// graph - stayed).
	//
	// THE RADIUS THIS LINK OWES IS DECIDED BY WHAT DRIVES IT, not by whether it owns a lane -
	// the lock of the LARGEST VEHICLE ADMITTED, never the one driving now, the same call the
	// lane's own corners are rounded by (FStandLayoutBuild::Build). An AIRCRAFT link keeps
	// Link.Radius instead, the PAINTED radius of its lead-in line.
	double LaneRadius = 0.0;
	if (Link.Class == ETraversalClass::GroundVehicle)
	{
		// ISSUE #190: the caller's resolved vehicle, not a fresh resolve - this ran twice
		// per link (here and at the warning below) before Build started passing one down.
		// The tenth of slack is ServiceLaneRadius's, shared with PoseSetbackFor.
		LaneRadius = ServiceLaneRadius(LargestServiceVehicle);
	}

	LinkGeom::FLinkApproach Approach;
	Approach.PositionA = PositionA;
	Approach.Control = Original.Control;
	Approach.PositionB = PositionB;
	Approach.Param = Hit.Param;
	Approach.At = Link.At;
	Approach.Dir = Link.Dir;
	// THE DIRECTION THE JOIN ACTUALLY NEEDS, for every link that was found by DISTANCE rather
	// than by RAY: a service link has no ray of its own, but the fillet and the two sweeps below
	// are written in terms of a direction of arrival, and Plan derives it from Corner once this
	// is set - see Solve/LinkGeom.h's FLinkApproach::bRecomputeDirFromCorner.
	Approach.bRecomputeDirFromCorner = Link.Kind != ELinkKind::Ray;
	Approach.LaneRadius = LaneRadius;
	Approach.Reach = Link.Reach;
	Approach.WeldTolerance = LeadInWeldTolerance;

	if (Link.LaneOwner.IsSet() && LaneRadius > 0.0)
	{
		// EntryDeparture STILL NEEDS Toward, which is Corner - Link.At at the search's OWN hit -
		// the same value Plan will separately compute once it runs; asking it here too, rather
		// than threading Plan's own copy back out, is one extra Eval call against a Curve
		// re-derivation of Plan's control flow, and this function already samples Curve once
		// more than it would like (see the top of this function).
		const FVector2D UnaimedCorner =
			GuidelineGeom::Eval(PositionA, Original.Control, PositionB, Hit.Param);
		Approach.bHasLaneAlong =
			EntryDeparture(Network, Link.Node, UnaimedCorner - Link.At, Approach.Along);
	}

	Approach.Curve = Curve;
	const LinkGeom::FLinkGeometry Planned = LinkGeom::Plan(Approach);

	double Param = Planned.Param;
	FVector2D Corner = Planned.Corner;
	Link.Dir = Planned.Dir;
	// SET ONLY ON LaneChange - see FLinkGeometry::Control's own comment. Everything else keeps
	// the straight midpoint control, because a straight lead-in is what it is.
	TOptional<FVector2D> OutLaneControl = Planned.Control;
	double LaneRun = Planned.LaneRun;

	const FVector2D TaxiDir =
		GuidelineGeom::Tangent(PositionA, Original.Control, PositionB, Param);

	// How far back along each line the tangent points sit, for a fillet of this radius.
	// The lead-in makes two corners with the taxiway - theta one way, 180 - theta the
	// other - and the SHARPER one sizes the offset, so it gets at least its radius and
	// the shallower one simply sweeps more gently.
	// AngleBetween, not acos: the guard below is a thousandth of a radian and acos was never
	// wrong by that much, but one idiom for "the angle between two directions" is the one
	// Check-Architecture rule 18 can hold (see RoadGeom::AngleBetween for the one that bit).
	const double Theta = RoadGeom::AngleBetween(-Link.Dir, TaxiDir);
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
	// tolerance of room left over so no split produces a stub - MeasureJoinRoom (#306), shared
	// with PoseSetbackFor so the room asked for there is the room measured here.
	//
	// MEASURED FROM THE CONTROL WHEN THERE IS ONE, not from the node. LeadEnd is laid at
	// Corner - Dir * Offset and Dir points from the control, so an Offset longer than THAT
	// distance puts LeadEnd behind the control: the lead-in folds back on itself and the two
	// sweeps leave from the wrong side of it. Measured from the node instead, a curved lead-in
	// that swings wide reads as having more room than it has. It only ever binds when the road
	// is close - at 54 m the fillet asks for 2500 against 4590 of room - which is exactly where
	// the hook MeasureJoinRoom's own comment describes was being built.
	const FVector2D LeadFrom = OutLaneControl.IsSet() ? OutLaneControl.GetValue() : Link.At;
	Offset = FMath::Min(Offset, MeasureJoinRoom(Curve, Param, Corner, LeadFrom, LaneRun).Available());

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

	// MEASURED, NOT ASSUMED, for every link that is sized to a radius in the first place. The
	// lead-in's DELIVERED radius is what a truck drives; the run that was asked for is not. This
	// project has paid three sessions for that distinction once already (8be494c, where a test
	// checked the radius requested while the follower drove the radius delivered), so the figure
	// the warning below prints comes off the edge as laid.
	//
	// ON LaneRadius AND NOT LaneRun, which is the difference between measuring one branch and
	// both. LaneRun belongs to the lane CHANGE; a link that ran on to a crossing has none, and
	// gating on it left the crossing branch unmeasured and unable to warn - the same shape as
	// the defect above, one branch over. Analytically a crossing is exact while it is unclamped,
	// but Behind and Ahead clamp it whenever the road is short or ends near the stand, and an
	// analytic argument is not a measurement.
	//
	// A STRAIGHT LEAD-IN REPORTS THE MAXIMUM and so never trips the warning, which is right: the
	// crossing branch leaves the lane along the lane and has no curve to be tight.
	const FVector2D LeadEndAt = Network.GetGuidelineNode(LeadEnd)->Position;
	double Tightest = TNumericLimits<double>::Max();
	if (LaneRadius > 0.0)
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
		// and to the road at the other and both are cut back the SAME Offset; what differs is
		// the angle each turns through. The one that carries on the way the lead-in was pointing
		// takes the gentler of the two - the one Offset above was sized for - and the OTHER
		// takes its supplement. It is laid anyway, because without it a truck arriving from the
		// far side has no way in at all.
		//
		// TRUE OF THE LANE CHANGE, AND ONLY OF IT. There the deflection is a function of the
		// GAP: merging onto a road 4 m away needs a shallow slant, and turning back the other
		// way out of a shallow slant is a U-turn no geometry fixes. Past 2.83 times the lock the
		// deflection reaches its right-angle cap and BOTH sweeps clear - GuidelineGeom::
		// ShiftDeflectionFor, measured at 769 and 769 uu on the 54 m fixture of
		// Airside.Build.StandLinkClearsTheTruckLock against 38 uu on the 4 m one. So the gap IS
		// the lever there, and it is the lever the warning below names.
		//
		// ON A CROSSING THE TURN-BACK IS UNBOUNDED, and it is a different fact that the
		// paragraph above used to cover as though it were the same one. Here the deflection is
		// not a function of the gap at all: it is the fixed angle at which the lane's crossing
		// meets the road - 45 degrees on the shipping Code C stand - so the same Offset delivers
		// LaneRadius on the 135-degree sweep and Offset*sin^2(22.5)/cos(22.5) on the 45-degree
		// one, which is a fourteenth of it. MEASURED in the same test's crossing block: merge
		// 769 uu, turn-back 55 uu, IDENTICAL at a 4 m gap and at a 20 m one. Moving the road
		// does not move that figure, and nothing STEERS a truck to the entry that would spare
		// it. The crossing's two entries depart on MIRRORED legs - (NoseX,BoxY) leaves (+1,-1)
		// and (NoseX,PortY) leaves (+1,+1), see UEntityDefinition::BuildCodeCStandFor - so each
		// merge carries on a different way along the road, and the entry a truck did not use
		// has the gentle 769 uu approach for the direction the other one turns back on. Nothing
		// routes it there: RouteSearch::EdgeCost is sampled length plus congestion, no
		// curvature term, so whichever entry is nearer wins regardless of which way its merge
		// faces.
		//
		// IT IS STILL EXCLUDED, and the reason is what the warning is FOR. The line below names
		// the gap, because the gap is the one thing the player can act on; on this branch it is
		// not the lever, so the same line would be false advice - and it would fire on every
		// stand with a road across its nose, which is an ordinary layout and not a fault. What
		// reports the crossing turn-back instead is FSpeedProfile, at the moment a route
		// actually uses it: "Route asks for R=55 uu ... The body will crab through it", with the
		// place on the route. Per journey rather than per build, which is when it is true.
		// Making the curve itself takeable is a change to what this builder LAYS - a sweep it
		// declines to lay, or a second entry whose slant faces the other way - and wants its own
		// design, not a warning bolted to the existing one.
		const FVector2D SweepEndAt = Network.GetGuidelineNode(SweepEnd)->Position;
		if (LaneRadius > 0.0
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
	// THE GAP IS IN THE LINE because on the shape that reaches here it is the only lever - the
	// radius is the truck's and the entry is the stand's. Moving the road out fixes a LANE
	// CHANGE and nothing else will: past 2.83 times the lock (about 20 m for the shipping
	// dispenser) that transition stops being constrained at all. Tightest holds the lead-in and
	// the merge, never the crossing's turn-back, whose figure the gap does not move - see the
	// sweep loop above for the measurement and for why a line naming the gap would be a lie
	// about it.
	if (const double Lock = LargestServiceVehicle.TightestFollowableRadius();
		LaneRadius > 0.0 && Lock > 0.0 && Tightest < Lock)
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

int32 FAnchorLink::Build(URoadNetwork& Network, const FChassis& LargestServiceVehicle,
	double MaxLeadIn, double ServiceLinkRadius)
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
		// THE ONLY RESOLVE A DECLARED ENTRY GETS, since #177 deleted the one Gather used to run
		// on the same link to fill a probe nothing read (see the WHY comment on Gather's
		// declared-entry loop). Resolved fresh, here, against the graph as THIS pass has left
		// it so far - which matters when several of a stand's entries share one road: an
		// earlier entry's Join a few lines down can split that road, and a Hit taken before
		// this pass started would not see the split.
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

		// Read BEFORE Join, which splits Hit.Edge and retires its id.
		const FGuidelineEdge* HitEdge = Network.GetGuidelineEdge(Hit.Edge);
		const FRoadSegmentId JoinedSegment = HitEdge ? HitEdge->DerivedFrom : FRoadSegmentId();
		const int32 JoinedIndex = HitEdge ? HitEdge->DerivedGuidelineIndex : INDEX_NONE;

		const FGuidelineNodeId LeadEnd = Join(Network, Link, Hit, AnchorNodes, LargestServiceVehicle);
		if (!LeadEnd.IsSet())
		{
			continue;
		}

		// THE OTHER LANE TOO (spec 2026-09-23 §5). Joining only the nearest guideline was right
		// while a road was one line; on a two-lane road it made the anchor reachable from one
		// direction only, and traffic the other way drove to the next dead end and back. A
		// driveway serves both lanes; so does this. Aircraft are excluded: a taxiway is one
		// line, and a ray-cast lead-in has no second lane to find. Not counted in Joined - the
		// census counts anchors, and this is still one anchor.
		//
		// KNOWN GAP (review of 2026-09-23, left as is): the far lane's lead-in CROSSES the near
		// lane with no graph node where they cross, so the claim model - per edge and per node -
		// cannot see a truck turning across it and one driving along it. Two trucks can overlap
		// at a driveway. Splitting both at the crossing would fix it, and would also make the
		// crossing a junction trucks could turn at; not worth that until it is seen in play.
		// ENFORCED BY: Airside.Build.TwoWay.AnchorBothLanes
		if (Link.Class != ETraversalClass::Aircraft && JoinedSegment.IsSet())
		{
			const FLinkHit Sibling = FindSiblingLane(Network, Link, AnchorNodes, JoinedSegment, JoinedIndex);
			if (Sibling.IsSet())
			{
				FPendingLink Again = Link;
				Join(Network, Again, Sibling, AnchorNodes, LargestServiceVehicle);
			}
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

	// THE FUEL-DEPOT MODULE CENSUS MOVED OUT, 2026-09-26 (#306): it warned about a depot
	// missing a shed or a pump, which is a fact about a PLOT's modules and has nothing to do
	// with joining a lead-in - it only ever ran here because this was the last thing a Topology
	// rebuild touched Network with. It is DepotKit::ReportIncomplete now, called from
	// URoadSurfacePresenter::RebuildInternal's Topology branch right after this Build, so it
	// still runs exactly once per rebuild and still runs again after every edit.

	return Joined;
}

double FAnchorLink::ServiceLaneRadius(const FChassis& LargestServiceVehicle)
{
	// PLUS A TENTH ON THE LOCK, measured rather than chosen - see Join's own comment on the 4 m
	// fixture that came out at 707 against 699.4 when sized at exactly the lock.
	constexpr double Slack = 1.1;
	return LargestServiceVehicle.TightestFollowableRadius() * Slack;
}

FAnchorLink::FJoinRoom FAnchorLink::MeasureJoinRoom(const TArray<FVector2D>& Curve, double Param,
	const FVector2D& Corner, const FVector2D& LeadFrom, double LaneRun)
{
	FJoinRoom Room;

	// AND THE TANGENT RUN IS SPENT ROOM. A curved lead-in has TWO legs - the run down the lane
	// and the approach to the road - and a fillet that takes everything up to the weld
	// tolerance leaves the second one a few uu long, which is a HOOK: the lead goes 313 uu out
	// along the lane and doubles back 10. Measured on the suite's own fixture before this
	// subtraction: 67 degrees of instant turn at the far end of a lead-in whose whole purpose
	// is to have none.
	Room.LeadRoom = FVector2D::Distance(LeadFrom, Corner) - LeadInWeldTolerance - LaneRun;

	const double TotalLength = GuidelineGeom::PolylineLength(Curve);
	Room.Behind = TotalLength * Param - LeadInWeldTolerance;
	Room.Ahead = TotalLength * (1.0 - Param) - LeadInWeldTolerance;
	return Room;
}

double FAnchorLink::PoseSetbackFor(const URoadNetwork& Network, const FVector2D& At,
	const FVector2D& Inward, const FChassis& LargestServiceVehicle, double ServiceLinkRadius)
{
	// THE LINK Gather WOULD MAKE for a service pose: found by proximity, within the service
	// radius, for a ground vehicle - FPendingLink::For (#306), the same assembly Gather itself
	// uses. Radius, StandWingspan and MaxLeadIn are unused off a probe that never reaches Join.
	const FPendingLink Link = FPendingLink::For(FGuidelineNodeId(), At, FVector2D(1.0, 0.0),
		ETraversalClass::GroundVehicle, /*Radius=*/2500.0, /*StandWingspan=*/0.0,
		/*MaxLeadIn=*/0.0, ServiceLinkRadius);

	const FLinkHit Hit = Resolve(Network, Link, TSet<FGuidelineNodeId>());
	const FGuidelineEdge* Edge = Hit.IsSet() ? Network.GetGuidelineEdge(Hit.Edge) : nullptr;
	const FGuidelineNode* A = Edge != nullptr ? Network.GetGuidelineNode(Edge->A) : nullptr;
	const FGuidelineNode* B = Edge != nullptr ? Network.GetGuidelineNode(Edge->B) : nullptr;
	TArray<FVector2D> Curve;
	if (A == nullptr || B == nullptr || Inward.IsNearlyZero() || !Network.SampleGuideline(Hit.Edge, Curve))
	{
		return 0.0;
	}

	const FVector2D Meets = GuidelineGeom::Eval(A->Position, Edge->Control, B->Position, Hit.Param);

	// THE ROOM JOIN WILL ACTUALLY MEASURE (#306), not an idealised square corner on an
	// unboundedly long road. MeasureJoinRoom's Behind/Ahead are a property of THIS road's own
	// length either side of the hit, and no setback moves them: Inward is square to the road by
	// construction (this function's own header comment), so the point Resolve found does not
	// move as At does. What setback buys is LeadRoom alone - so the corner run actually asked
	// for is capped at what Behind and Ahead can hold, not at a raw CornerRunFor a short spur
	// could never deliver. LeadFrom=Corner=Meets: this call is not asking about the lead-in's
	// own length (Have, below, measures that directly), only about the road's.
	const FJoinRoom RoadRoom = MeasureJoinRoom(Curve, Hit.Param, Meets, Meets);

	// A SECOND TOLERANCE ON TOP, so the fillet is never clamped by rounding alone.
	const double Needs = FMath::Min(
		GuidelineGeom::CornerRunFor(ServiceLaneRadius(LargestServiceVehicle), UE_DOUBLE_HALF_PI),
		FMath::Min(RoadRoom.Behind, RoadRoom.Ahead)) + 2.0 * LeadInWeldTolerance;

	// ALONG INWARD ONLY: the part of the gap that setting back actually lengthens.
	const double Have = FVector2D::DotProduct(At - Meets, Inward.GetSafeNormal());
	return FMath::Max(Needs - Have, 0.0);
}
