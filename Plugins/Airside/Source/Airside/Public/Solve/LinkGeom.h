#pragma once

#include "CoreMinimal.h"

/**
 * The join geometry FAnchorLink::Join lays, pulled out of that function's own if/else ladder
 * (#306 - regression of #76). ILinkFinder (AnchorLinkFinder.h) is a strategy for FINDING what
 * a link should join; this is the strategy for what SHAPE the join itself takes once something
 * is found, and it had never had one - Join dispatched on Link.Kind, Link.LaneOwner.IsSet() and
 * LaneRadius > 0.0 inline, and Param/Corner/Dir/OutLaneControl/LaneRun were mutated by whichever
 * branch ran, so the fillet code below depended on which shape had executed with no name to ask.
 *
 * Dependency-free beyond CoreMinimal.h, like the rest of Solve/ (Check-Architecture's
 * solve-purity rule): takes plain points and a sampled polyline, never an FGuidelineEdge, a
 * URoadNetwork or an ELinkKind, so it is testable with no world and no graph. The one thing
 * this module cannot do - walk a lane's incident edges to find its departure candidates
 * (EntryDeparture's gather half) - stays in Build/AnchorLink.cpp, because that needs the graph;
 * ChooseDeparture below is EntryDeparture's DECISION half, which does not.
 */
namespace LinkGeom
{
	/** One candidate a lane's entry may leave along - EntryDeparture's own gather, boiled down
	 *  to what its decision needs: a direction, whether the leg it leaves is a bend or a
	 *  straight, and how much room that leg has. */
	struct FDepartureCandidate
	{
		FVector2D Along = FVector2D::ZeroVector;
		bool bBend = false;
		double Room = 0.0;
	};

	/**
	 * Picks the best of Candidates to leave a lane's entry along, given Toward - the direction
	 * the road actually lies in. Returns false, leaving OutAlong untouched, when Candidates is
	 * empty.
	 *
	 * THE ORDER OF THE THREE TESTS IS THE RULE ITSELF: toward the road first, then the corner
	 * side, then room. The second and third only ever decide a TIE in the first, because a
	 * candidate's two senses (see EntryDeparture's own gather, which offers both) have exact
	 * opposite dot products with Toward and can never tie against each other.
	 *
	 * AND TOWARD THE CORNER WHEN THE ROAD IS SQUARE ABEAM, which is a tie the dot product cannot
	 * break and the TRAFFIC can. A tangential join is smooth in ONE direction: a vehicle coming
	 * up the connector arrives at the entry heading AWAY from the control, so whichever side the
	 * control is on is the side that gets a U-turn. Putting it on the corner (bend) side leaves
	 * the STRAIGHT smooth - which is the side a stand's plant is usually on, a run of 47 m
	 * against a bend of 3 m - and that is the journey every service vehicle actually makes.
	 * Measured the other way round first: the route from a road alongside to the ground power
	 * came in at the near entry and reversed through 179 degrees to get to it, which a truck
	 * cannot do at all under the rolling-steer law.
	 *
	 * HOW MUCH ROOM each leg has decides nothing but that TIE, and is not itself reported: the
	 * control goes on the tangent line at the entry, and a transition wide enough to clear a
	 * truck's lock regularly wants more run than the leg it leaves on is long. Capping the run to
	 * the leg instead is what delivered 67 uu of radius where 699 was needed.
	 */
	AIRSIDE_API bool ChooseDeparture(TConstArrayView<FDepartureCandidate> Candidates,
		const FVector2D& Toward, FVector2D& OutAlong);

	/** Which join shape Plan decided on - replacing "which optional field is set" as the tag,
	 *  the same reasoning ELinkKind's own header gives for the strategy one level up. */
	enum class ELinkShape : uint8
	{
		/** The ordinary case: a straight lead-in run onto the corner Resolve (or a ray cast)
		 *  already found, with no re-aim - an aircraft's painted line, or any service link that
		 *  does not leave a stand's lane. */
		StraightRay,

		/** A lane's entry runs ON to where its own heading meets the road, and rounds the
		 *  corner there - preferred whenever that crossing exists within the link's reach and
		 *  its corner fits in front of the entry, because it leaves the lead-in dead straight
		 *  and gives BOTH sweeps their radius rather than one. */
		Crossing,

		/** The lane runs alongside its road with no usable crossing in reach - an S of two
		 *  curves at the deflection GuidelineGeom::ShiftDeflectionFor sizes for the gap. */
		LaneChange,

		/**
		 * A FOURTH SHAPE, NOT YET DECIDED BY Plan - #286 (the articulated rig cannot turn at a
		 * road dead end; its trailer folds in every balloon sized for a straight vehicle).
		 * Named here so the switch in Plan's own .cpp has a case to warn on rather than a
		 * silent fallthrough the day something starts asking for it, and so a caller checking
		 * Shape by name does not have to invent one first. NOTHING PRODUCES THIS YET - it is
		 * the shape of the work #306 leaves ready, not the work itself.
		 */
		DeadEndTurn,
	};

	/** What Plan decided, replacing the mutable Param/Corner/Dir/OutLaneControl/LaneRun that
	 *  used to come back out of Join's own if/else with no other record of which branch set
	 *  them. */
	struct FLinkGeometry
	{
		ELinkShape Shape = ELinkShape::StraightRay;

		/** The road's own curve parameter the join lands at - unchanged from the input Param on
		 *  StraightRay, re-aimed on Crossing and LaneChange. */
		double Param = 0.0;

		/** GuidelineGeom::Eval(PositionA, Control, PositionB, Param) - handed back rather than
		 *  re-evaluated, since Plan already paid for it wherever Param moved. */
		FVector2D Corner = FVector2D::ZeroVector;

		/** The direction the lead-in leaves At along - unchanged on StraightRay's Ray case,
		 *  Corner-At normalised otherwise. */
		FVector2D Dir = FVector2D(1.0, 0.0);

		/** Set ONLY on LaneChange - the lane's own control point, a run back along Along from
		 *  At. Unset everywhere else, where the straight lead-in's control is the plain
		 *  midpoint its own caller already knows how to lay. */
		TOptional<FVector2D> Control;

		/** How much of the lane-parallel run a LaneChange already spent reaching Control - zero
		 *  on every other shape. */
		double LaneRun = 0.0;
	};

	/** Everything Plan needs to decide a link's join shape - one struct rather than the eight
	 *  parameters it replaces (CLAUDE.md's "one struct per thing"), and named for what it IS
	 *  (the link's approach to its road) rather than for the function that consumes it. */
	struct FLinkApproach
	{
		/** The road as Join already has it: its own sampled polyline (NearestOnPolyline's
		 *  domain) and its three Bezier control points. */
		TArray<FVector2D> Curve;
		FVector2D PositionA = FVector2D::ZeroVector;
		FVector2D Control = FVector2D::ZeroVector;
		FVector2D PositionB = FVector2D::ZeroVector;

		/** Where the search first landed on Curve, before any re-aim. */
		double Param = 0.0;

		/** The link's own anchor, and the direction it currently leaves along. */
		FVector2D At = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D(1.0, 0.0);

		/** False for a Ray link - an aircraft's cast heading already IS the answer, and Plan
		 *  must not touch it. True for everything found by proximity, whose Dir is only known
		 *  once Corner is (Join's own "the direction the join actually needs" step, folded in
		 *  here so Plan is the one place that decides it). */
		bool bRecomputeDirFromCorner = false;

		/** Set true, with Along the winner ChooseDeparture picked, only for a link that leaves
		 *  a stand's lane. Along is the direction, at the entry, the departing lead-in takes -
		 *  EntryDeparture's own gather stays in Build/, but its answer is plain geometry. */
		bool bHasLaneAlong = false;
		FVector2D Along = FVector2D::ZeroVector;

		/** The radius a ground-vehicle link's curves are laid at (FAnchorLink::ServiceLaneRadius)
		 *  - zero for an aircraft lead-in, which never reaches the lane branch below. */
		double LaneRadius = 0.0;

		/** How far this link may reach (FPendingLink::Reach) - doing two jobs here exactly as
		 *  it does in Join: the player's own knob for how far a stand may sit from its road, AND
		 *  the bound past which a crossing is better answered as a lane change instead. */
		double Reach = 0.0;

		/** FAnchorLink::LeadInWeldTolerance, handed in rather than hard-coded so this stays
		 *  free of Build/'s constants. */
		double WeldTolerance = 0.0;
	};

	/**
	 * Decides Approach's join shape and where it lands - StraightRay, Crossing or LaneChange,
	 * never DeadEndTurn (see that enumerator's own comment). Pure: reads Approach, writes
	 * nothing, and Curve is never re-sampled - only ever walked with GuidelineGeom's own
	 * NearestOnPolyline/Eval/Tangent, the one evaluator this graph allows (CLAUDE.md's "the
	 * guideline graph samples once").
	 *
	 * ONE FUNCTION, THREE OUTCOMES, replacing the if/else in FAnchorLink::Join that used to
	 * decide the same thing by mutating Param/Corner/Dir/OutLaneControl/LaneRun in place - see
	 * this header's own top comment. FAnchorLink::Join calls this, then lays the fillet and
	 * splits the graph from what comes back; nothing here touches a graph, because nothing here
	 * can see one.
	 */
	AIRSIDE_API FLinkGeometry Plan(const FLinkApproach& Approach);
}
