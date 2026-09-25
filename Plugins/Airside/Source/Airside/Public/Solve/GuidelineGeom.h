#pragma once

#include "CoreMinimal.h"

/**
 * The shape of a guideline, as points rather than as a curve.
 *
 * A guideline edge stores a quadratic Bezier - two endpoints and one control point - and
 * three separate consumers need to know where it goes: the search needs its LENGTH to cost
 * it, the overlay needs a POLYLINE to draw it, and a follower needs a POSITION AND HEADING
 * part-way along it.
 *
 * All three go through Sample(). That is deliberate and it is the same discipline the
 * surface model uses for its cut vertices: the cube drives exactly the line you can see,
 * because they are the same array of points, not two evaluations of one curve that agree
 * to within a tolerance. Cost the curve one way and drive it another and the cube leaves
 * the line under any curvature at all - visibly, and only on bends.
 *
 * Dependency-free beyond CoreMinimal.h, like the rest of Solve/. Takes raw points, never
 * an FGuidelineEdge, so the model layer can depend on this and never the reverse.
 */
namespace GuidelineGeom
{
	/**
	 * Points per curved guideline, including both endpoints.
	 *
	 * A straight guideline short-circuits to two points regardless - see Sample - so this
	 * is only ever paid on a real bend. Junction turn paths are the overwhelming majority
	 * of those and are a few tens of metres long, where 16 points is well under a
	 * centimetre of chord error.
	 */
	inline constexpr int32 DefaultSamples = 16;

	/** Quadratic Bezier at T in [0,1]. T is a CURVE parameter, not an arc length. */
	AIRSIDE_API FVector2D Eval(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T);

	/**
	 * Unit direction of travel at T. The ANALYTIC derivative of Eval, not a second sampler.
	 *
	 * Deliberately derived from the same three control points Eval uses, so it cannot
	 * disagree with the curve it describes - the single-sampling rule for this graph is
	 * about there being one evaluator, and a tangent measured by differencing sampled
	 * points would be a second one that drifts on exactly the bends that matter.
	 *
	 * Falls back to the chord when the derivative degenerates, which happens when the
	 * control point coincides with an end.
	 */
	AIRSIDE_API FVector2D Tangent(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T);

	/**
	 * True when Control sits on the midpoint, which is how the builder spells "straight".
	 *
	 * Tested against the midpoint rather than against collinearity: a control point that
	 * is collinear but not central still yields a straight LINE traversed at a varying
	 * rate, and short-circuiting that one to two points would change where a follower is
	 * at a given distance. This test admits only the case where it provably cannot.
	 */
	// NOT AIRSIDE_API (issue #191): every caller (AnchorLink.cpp) is inside this module's own
	// Private/, so exporting it bought no consumer anything.
	bool IsStraight(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B);

	/**
	 * The guideline as a polyline, from A to B inclusive.
	 *
	 * Appends; it does not clear. A route is built by sampling each edge in turn into one
	 * array, and the caller drops the duplicated shared endpoint between consecutive
	 * edges - which it must do itself, because only the caller knows an edge was reversed.
	 */
	AIRSIDE_API void Sample(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B,
		TArray<FVector2D>& OutPoints, int32 Samples = DefaultSamples);

	/** Summed length of the sampled polyline - NOT the true arc length of the curve. */
	AIRSIDE_API double Length(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B,
		int32 Samples = DefaultSamples);

	/**
	 * de Casteljau split at T: the two sub-curves that together are the original.
	 *
	 * Needed because joining a lead-in to a guideline SPLITS that guideline, and replacing
	 * a bend with two straight halves would move the taxiway centreline - visibly, exactly
	 * where a stand joins it. The split is exact for a quadratic, so the two halves trace
	 * the original curve rather than approximating it.
	 */
	AIRSIDE_API void Split(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T,
		FVector2D& OutMid, FVector2D& OutControlLeft, FVector2D& OutControlRight);

	/**
	 * Curve parameter of the point Index steps along a polyline of Count points.
	 *
	 * The bridge between a hit reported against the SAMPLED polyline and the curve
	 * parameter a split needs. Exact for a straight guideline, where the polyline is the
	 * curve.
	 */
	// NOT AIRSIDE_API (issue #191): every caller (AnchorLink.cpp, AnchorLinkFinder.cpp) is
	// inside this module's own Private/.
	double ParamAtSample(int32 Index, double Fraction, int32 Count);

	/** Summed length of an already-sampled polyline. */
	AIRSIDE_API double PolylineLength(const TArray<FVector2D>& Points);

	/**
	 * Distance from Query to the nearest point on an already-sampled polyline, with the span
	 * index and the 0..1 fraction along that span written out.
	 *
	 * Against the SAMPLES, not against the curve, and deliberately: the samples are what the
	 * search costs, the overlay draws and a follower walks, and a link measured against a
	 * closed-form curve would be a second evaluator of the same geometry - see this
	 * namespace's own header. Feed the two outputs to ParamAtSample for the curve parameter
	 * a split needs.
	 *
	 * Returns a huge distance, and leaves the outputs at zero, for a polyline with fewer than
	 * two points - which has no nearest point to report.
	 */
	AIRSIDE_API double NearestOnPolyline(const TArray<FVector2D>& Points,
		const FVector2D& Query, int32& OutIndex, double& OutFraction);

	/**
	 * Closest approach between two already-sampled polylines, with the span index and
	 * fraction of the closest point on EACH.
	 *
	 * BOTH DIRECTIONS ARE TRIED - every vertex of A against B, then every vertex of B against
	 * A - because the closest pair of points on two segments contains an endpoint of one of
	 * them only when they are not parallel. A service road drawn ALONGSIDE a row of stands is
	 * parallel to the lane it has to join, and a one-directional search would measure the
	 * corner rather than the side.
	 */
	AIRSIDE_API double NearestBetweenPolylines(
		const TArray<FVector2D>& A, const TArray<FVector2D>& B,
		int32& OutAIndex, double& OutAFraction, int32& OutBIndex, double& OutBFraction);

	/**
	 * The heading a follower is given AT a vertex, arriving at it and leaving it.
	 *
	 * The same function PointAtDistance interpolates between, exposed rather than reimplemented
	 * - a planner that decided where the corners were from its own reading of the polyline
	 * would be free to brake for a bend the driver does not agree is there, which is the
	 * second-evaluator failure this whole namespace is arranged to prevent.
	 *
	 * The two differ ONLY at a real corner. Everywhere else the vertex is a sample of a curve
	 * and both report the smoothed tangent, so "arriving != leaving" is exactly the test for
	 * a genuine change of direction - see the MaxSampledTurn note below.
	 *
	 * Both are left untouched for a polyline with no direction at all.
	 */
	// NOT AIRSIDE_API (issue #191): its only caller (SpeedProfile.cpp) is inside this
	// module's own Private/.
	void VertexHeadings(
		const TArray<FVector2D>& Points, int32 Vertex,
		double& OutArriving, double& OutLeaving);

	/**
	 * The curve parameter Offset of ARC LENGTH away from Param, walked on the sampled
	 * polyline. Negative walks backwards. Clamped to the curve's own ends.
	 *
	 * Walked on the SAMPLES rather than integrated in closed form, because the samples are
	 * what every other consumer of this graph measures - the search costs them, the overlay
	 * draws them, a follower walks them. An exact arc length here would be more accurate and
	 * would disagree with all three.
	 *
	 * HERE RATHER THAN IN AnchorLink.cpp, where it was written, because two passes now slide
	 * a point along a guideline by a distance - FAnchorLink trims an arm back for its lead-in
	 * sweep, and a stand lane's road join slides along the lane - and "how far back
	 * along this curve" has to be the same walk in both or the two disagree by a sample.
	 */
	// NOT AIRSIDE_API (issue #191): its only caller (AnchorLink.cpp) is inside this module's
	// own Private/.
	double ParamAtArcOffset(
		const TArray<FVector2D>& Points, double Param, double Offset);

	/**
	 * The tightest radius anywhere on the quadratic A -> B with control Control.
	 *
	 * Curvature is |B'|^3 / |B' x B''| and the cross product is CONSTANT, so the tightest
	 * point is wherever |B'| is least - and B'(t)/2 traces the straight segment from
	 * (Control - A) to (B - Control). On a symmetric curve the nearest point of that segment
	 * to the origin falls in the middle, which is the apex; on a lopsided one it falls OFF
	 * THE END and the tightest point is an endpoint. Clamping the parameter is what makes
	 * this right in both cases, and the closed-form apex expression wrong in the second.
	 *
	 * HERE rather than in a builder because several KINDS of curve ask it, not a fixed set of
	 * call sites - naming a count is what went stale here twice, so this names the kinds
	 * instead: a road junction's turn path (FRoadGuidelineBuilder), a stand lane's own corner
	 * (StandLaneBuild.cpp), and a stand link's lead-in and both its entry sweeps
	 * (FAnchorLink::Join) - plus the tests that hold a laid lane against a steering lock. One
	 * evaluator, as with everything else in this namespace. The SPUR that used to be the first
	 * of those is deleted - see FStandLayoutBuild.
	 */
	AIRSIDE_API double TightestRadius(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B);

	/**
	 * How far back along each leg a corner must be cut so the quadratic laid across the cut
	 * delivers Radius. Interior is the unsigned angle between the two leg directions, radians.
	 *
	 * ONE FORMULA, TWO SPECIALISATIONS. A quadratic with legs p and q meeting at angle theta has
	 * apex radius:
	 *
	 *     R = 2 p^2 q^2 sin^2(theta) / (p^2 + q^2 + 2pq cos(theta))^(3/2)
	 *
	 * The ASYMMETRIC case - different cuts on the two legs - is what that general form is for,
	 * and NOTHING INVERTS IT ANY MORE: the spur that did (FStandLayoutBuild::TangentRunFor, an
	 * anchor's offset against a run along the lane) went with the anchors onto the lane on
	 * 2026-09-16. It is kept written out because it is where the line below comes from. A
	 * CORNER is the symmetric case, the same cut on both legs, where it collapses to:
	 *
	 *     R = T sin^2(theta/2) / cos(theta/2)
	 *
	 * THE EXACT INVERSE OF TightestRadius, which is why it lives beside it. A corner cut back
	 * Run with its control ON the corner has delivered radius Run*sin^2(t/2)/cos(t/2); solve
	 * for Run and this is what falls out. Airside.Solve.CornerRunRoundTripsToItsRadius measures
	 * the two against each other rather than restating either.
	 *
	 * AT A RIGHT ANGLE THIS IS 1.414 R, NOT R. A circular fillet's tangent length at 90 degrees
	 * equals its radius, and the 2026-09-16 stand spec costed every corner that way and lost
	 * 40% of the run it needed - the same mistake 8be494c made one level up, in the same week,
	 * about the same kind of curve. It was a file-static in the stand lane builder when that
	 * happened - ServiceLoopBuild.cpp then, StandLaneBuild.cpp since - where nothing outside
	 * the builder could find it. For a 750 uu corner (the lane radius that builder typed until
	 * it started deriving one), this costs 1061 uu back along each side; a Code C stand's
	 * shortest side is 4180 uu, so its two corners use half of it between them.
	 *
	 * A HAIRPIN RETURNS THE MAXIMUM rather than an infinity: no cut gives it this radius, and a
	 * caller's proportional clamp asked for an infinity scales BOTH corners of a leg to nothing
	 * instead of cutting this one down to what its legs allow.
	 */
	AIRSIDE_API double CornerRunFor(double Radius, double Interior);

	/**
	 * How far a line may DEFLECT, in radians, to reach another line Shift away across it, and
	 * still deliver Radius everywhere. OutRun is the tangent length each of the two curves gets.
	 *
	 * THE SHAPE IS AN S, and it has to be: two parallel lines never meet, so a single fillet
	 * cannot join them. The transition leaves the first line deflecting by the returned angle,
	 * runs straight across, and deflects back onto the second - two quadratics, and the tighter
	 * of them is what this sizes. A SERVICE ROAD BESIDE A STAND IS EXACTLY THAT CASE: the road
	 * is drawn parallel to the lane because that is how a row of stands is served, and the
	 * connector between them is a lane change, not a junction.
	 *
	 * ONE FORMULA, READ THE OTHER WAY ROUND, which is why it lives here beside CornerRunFor
	 * rather than in the builder that wants it. With a deflection of b, each curve is the
	 * SYMMETRIC case with tangent length s and interior angle (pi - b), and the two together
	 * have to carry the whole shift, so 2 s sin(b) = Shift. Substituting one into the other:
	 *
	 *     R = Shift cos(b/2) / (4 sin^2(b/2))   =   CornerRunFor(Shift/4, b)
	 *
	 * so this is CornerRunFor solved for its SECOND argument, and the two round-trip. Inverting
	 * it is closed form rather than a search: with x = b/2 and k = 4R/Shift, cos x = k sin^2 x
	 * gives k^2 sin^4 x + sin^2 x - 1 = 0, so sin^2 x = (sqrt(1 + 4k^2) - 1) / (2k^2).
	 *
	 * CAPPED AT A RIGHT ANGLE, because past that the line is not shifting off its neighbour any
	 * more, it is leaving. The cap binds whenever Shift is wider than 2.83 R - at a right angle
	 * each curve gets Shift/2 of tangent and delivers 0.354 Shift - so a road a stand's width
	 * away constrains nothing and only a CLOSE one has to be met at a slant.
	 *
	 * A SHIFT OF NOTHING RETURNS NOTHING, with OutRun zero: two lines already on top of one
	 * another need no transition, and a caller that treated a zero run as a curve would lay a
	 * degenerate one. A Radius of zero or less returns the right-angle cap - no constraint -
	 * which is what an airframe with no measured axles asks for (see
	 * FChassis::TightestFollowableRadius, where zero means "nothing to clear").
	 */
	AIRSIDE_API double ShiftDeflectionFor(double Radius, double Shift, double& OutRun);

	/**
	 * THE LANE CHANGE, AS ONE EVALUATOR (review of 5660420c): the S of two symmetric quadratics
	 * that runs Along forward while stepping Shift across. Everything that lays or sizes one goes
	 * through the two functions below, so the length a solver reserves and the curve a builder
	 * lays are one construction, not two that agree by arithmetic.
	 *
	 * LaneChangeLength: the Along an S needs so that each quadratic delivers Radius -
	 * ShiftDeflectionFor's tangent s and deflection b, laid out: Along = 2 s (1 + cos b),
	 * Shift = 2 s sin b. Zero for no shift.
	 *
	 * LaneChange: the S from From to To for a given travel direction - the first curve's control,
	 * the inflection (the midpoint, by symmetry) and the second curve's control. From
	 * 2 s (1 + cos b) = Along and 2 s sin b = Shift: tan(b/2) = Shift / Along and
	 * s = Shift / (2 sin b); its delivered radius is then Shift cos(b/2) / (4 sin^2(b/2)), which
	 * is LaneChangeLength's inverse - Airside.Solve.LaneChangeRoundTrips measures the two against
	 * each other. False, outputs untouched, when Along or Shift is under a uu: nothing to lay.
	 */
	AIRSIDE_API double LaneChangeLength(double Radius, double Shift);

	AIRSIDE_API bool LaneChange(const FVector2D& From, const FVector2D& To, const FVector2D& Travel,
		FVector2D& OutControlIn, FVector2D& OutMid, FVector2D& OutControlOut);

	/**
	 * The most one quadratic of a bend lane's arc may sweep, radians: 22.5 degrees, so a right
	 * angle is four pieces delivering cos(11.25) = 0.981 of the arc's radius (Arc, below). Two
	 * pieces would deliver 0.924 and eight 0.995; four is where the loss is under the lane's own
	 * width in 50 and the graph grows by three nodes per lane per bend. HERE, not in the builder,
	 * because the solver lays the same arc to widen a bend for (BendWidening) - one figure.
	 */
	inline constexpr double BendArcPieceSweep = UE_DOUBLE_PI / 8.0;

	/**
	 * THE LENGTH RULE for a bend lane's curved pieces, uu (2026-09-25, "one density rule"): a lane
	 * through a bend is laid in pieces no longer than this, whatever its tier or radius - so a Wide
	 * bend's outer lane gets more nodes than a Narrow bend's inner one, in proportion. 300: every
	 * course lane is then held to under 22.5 degrees a piece (BendArcPieceSweep, the fidelity
	 * ceiling, still applied) on radii over 764 uu, which every road bend's lanes are.
	 * ENFORCED BY: AirportMgr.RigCourse.BendsAreSmooth (mean curved piece length per lane)
	 */
	inline constexpr double BendPieceLength = 300.0;

	/**
	 * How far two lanes' distances from a bend's inner edge (or a lane end from its tangent point)
	 * may differ, uu, for the lane to be laid concentric. Equal arms give the SAME solver value by
	 * two float paths, so the spread is a rounding error; a real mismatch (a Narrow arm meeting a
	 * Wide one: the lanes sit 210 and 285 uu off their inner edges) is tens of uu and keeps the
	 * quadratic. Shared by the builder that lays the arc and the solver that widens for it.
	 */
	inline constexpr double BendTangentTolerance = 1.0;

	/** One quadratic of an arc, in travel order: from the previous End (or the arc's From) to End. */
	struct FArcPiece
	{
		FVector2D End = FVector2D::ZeroVector;
		FVector2D Control = FVector2D::ZeroVector;
	};

	/**
	 * A CIRCULAR ARC ABOUT Centre, AS QUADRATICS (bend lanes, 2026-09-25): from From, travelling
	 * FromDir, to To, arriving along ToDir, in as few pieces as keep each one's sweep within
	 * MaxPieceSweep radians. Each piece runs between two points of the circle with its control at
	 * their tangents' crossing - the construction UTurnGeom lays its half circle with - so a piece
	 * of sweep p delivers cos(p/2) of the radius at its apex (one quadratic across 90 degrees, what
	 * a junction turn used to be, delivers 0.707).
	 *
	 * THE ENDS ARE THE CALLER'S, VERBATIM: the last End is To bit for bit (the builder joins it to
	 * a lane end BY HANDLE), and the first and last controls sit on the lines through From along
	 * FromDir and through To along ToDir, so the arc is tangent to the straights it joins however
	 * far From and To are off the circle by rounding. The intermediate ends are on the circle of
	 * the MEAN of From's and To's radii. The caller decides From and To ARE tangent points - that
	 * FromDir is perpendicular to From - Centre - and this lays whatever it is given.
	 *
	 * False, OutPieces empty, when the two directions are parallel (no turn to lay) or a tangent
	 * crossing degenerates.
	 * ENFORCED BY: Airside.Build.BendLanes.ConcentricWithPavement (radius and tangency, per tier)
	 */
	AIRSIDE_API bool Arc(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
		const FVector2D& Centre, double MaxPieceSweep, TArray<FArcPiece>& OutPieces);

	/**
	 * A BEND LANE (2026-09-25): from the lane end From, travelling FromDir, to the lane end To,
	 * leaving along ToDir, round Centre - straight on along the arriving lane to the tangent point
	 * of the circle about Centre, Arc round it in BendArcPieceSweep pieces, straight on to To. The
	 * straight leads are there when a lane end sits back from its tangent point: a bend whose
	 * inside is widened cuts its arms back past the fillet (FRoadNetworkSolver), and the lane keeps
	 * its arc. ONE CONSTRUCTION for the builder that lays it and the solver that widens the
	 * pavement for the vehicle driving it.
	 *
	 * False, OutPieces empty, when no circle about Centre touches both lane lines (their distances
	 * from Centre differ by more than BendTangentTolerance), when a tangent point lies BEHIND its
	 * lane end, or when the turn does not go round Centre - a width step's lanes, which the builder
	 * then lays with RampedBendLane.
	 * ENFORCED BY: Airside.Build.BendLanes.ConcentricWithPavement, .EveryTierIsSmooth
	 */
	AIRSIDE_API bool BendLane(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
		const FVector2D& Centre, TArray<FArcPiece>& OutPieces);

	/**
	 * ONE BEND SHAPE, EDGES AND LANES ALIKE (2026-09-25, user in PIE on 33d6f49b: "three variants").
	 * A curve round a two-arm bend is its BASE - the line along the arriving arm, the arc about
	 * Centre at Radius, the line along the leaving arm, tangent to each other - pushed off it
	 * radially by an offset D (outward from Centre positive) that ramps smoothly between three
	 * values: D at From (whatever the arriving end's own line stands off the base: a width step's
	 * narrower arm), DMid through the bend (a widening's depth, negative, or 0) and D at To.
	 *
	 * THE RAMP IS ONE CLOCK FOR EVERY CURVE OF THE BEND: its position is a STATION - distance along
	 * the arm, and angle x RefRadius on the arc - not each curve's own length, so an edge, its
	 * partner and every lane between them are on the same fraction of their ramp at the same angle
	 * and stay evenly spaced through it. LIn and LOut are the ramps' station lengths from each end.
	 * Where the arm holds the ramp (from the cut to the foot of Centre) it lies there, ending before
	 * the arc - a width step's taper OFF the bend; where it does not, the curve runs straight to the
	 * foot and the whole ramp lies on the arc from there. NEVER ACROSS THE JOIN: a station advances
	 * at 1 per uu on the arm but RefRadius / Radius on the arc, so a ramp still leaning at the foot
	 * breaks every curve's tangent but the one at RefRadius - 4.2 degrees on a Standard / Wide
	 * bend's outer edge, measured 2026-09-25.
	 *
	 * SMOOTHERSTEP, NOT THE LANE CHANGE'S TWO QUADRATICS: the ramp's curvature is continuous at
	 * both of its ends and at its middle, so the curve's tangent turns evenly across every join -
	 * the two-quadratic S (LaneChange) jumps its curvature at three points, and a steep one read
	 * as a 3.5 degree crease on the course (measured 2026-09-25).
	 */
	struct FRampedBend
	{
		FVector2D Centre = FVector2D::ZeroVector;
		double Radius = 0.0;
		double DMid = 0.0;
		double RefRadius = 0.0;
		double LIn = 0.0;
		double LOut = 0.0;
	};

	/** The ramp's 0..1 profile at T (smootherstep, clamped): value, slope and curvature 0 at both ends. */
	AIRSIDE_API double RampProgress(double T);

	/**
	 * The radial offset at Station of Total: D0 at 0, D1 at Total, DMid between, each end's ramp
	 * LIn / LOut long (clamped to Total; 0 = no ramp from that end), on the arm when it fits in
	 * RoomIn / RoomOut (the station from the cut to the foot) and from the foot onward when it does
	 * not - see FRampedBend. Overlapping ramps add, so a short bend's two ramps still meet smoothly.
	 */
	AIRSIDE_API double RampOffset(double Station, double Total, double D0, double DMid, double D1, double LIn, double LOut,
		double RoomIn, double RoomOut);

	/** Whether a ramp of Length fits its arm's Room (to 1 uu, so every curve of a bend agrees). */
	inline bool RampFitsArm(double Length, double Room) { return Length <= Room + 1.0; }

	/**
	 * Samples the ramped bend from From (travelling FromDir) to To (leaving along ToDir): the points
	 * about MaxStep apart along the curve and no more than MaxSweep of the arc apart, the first
	 * exactly From and the last exactly To; OutTangents (optional) the unit travel direction at each.
	 * False when the construction does not hold: Centre not inside the turn, a foot of Centre behind
	 * an end, or the offset curve crossing the centre.
	 */
	AIRSIDE_API bool SampleRampedBend(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
		const FRampedBend& Bend, double MaxStep, double MaxSweep, TArray<FVector2D>& OutPoints, TArray<FVector2D>* OutTangents);

	/**
	 * A bend lane on the ramped bend, as quadratic pieces for the builder: sampled by THE LENGTH
	 * RULE (BendPieceLength, under BendArcPieceSweep) and each piece's control on its two ends'
	 * tangents' crossing, so the pieces meet tangent-continuous; a piece whose tangents do not
	 * cross ahead of it (the ramp's inflection) is halved until they do. The last End is To
	 * exactly. The width step's lanes use it: DMid 0, Radius the wider arm's lane line.
	 * ENFORCED BY: AirportMgr.RigCourse.BendsAreSmooth, Airside.Build.BendLanes.EveryTierIsSmooth
	 */
	AIRSIDE_API bool RampedBendLane(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
		const FRampedBend& Bend, TArray<FArcPiece>& OutPieces);

	/**
	 * Position and heading at Distance along a polyline, clamped to both ends.
	 *
	 * Heading is the direction of the segment being walked, in radians, and is held from
	 * the last real segment once the end is passed - so an agent that arrives keeps facing
	 * the way it was going rather than snapping to zero. Returns false only for a polyline
	 * too short to have a direction at all, leaving the outputs untouched.
	 */
	AIRSIDE_API bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading);

	/**
	 * PointAtDistance with a START HINT, for a caller that walks the same polyline with
	 * ever-increasing distances - FRouteFollower::Advance every substep, FClaimPass::
	 * SampleBody's three calls per agent per substep. Without it each of those re-walked
	 * the polyline from vertex 0 to find a span it had already found, or was about to find
	 * again a few uu further on - issue #190.
	 *
	 * InOutHintVertex/InOutHintWalked are a CHECKPOINT of the plain overload's own loop -
	 * the vertex it was about to test and how far the walk had gone to reach it - not a
	 * second way of measuring the polyline. Passing them back in resumes that exact loop
	 * instead of restarting it, so the two overloads share one implementation and can never
	 * disagree - the single-evaluator rule this namespace exists to keep.
	 *
	 * A Distance BEHIND the hint - the one case a monotonically walked route cannot produce -
	 * falls back to a walk from the start rather than trusting a hint that would walk
	 * backwards, so a caller that got this wrong (or a Start/Replace that forgot to reset it)
	 * is merely slow, never wrong. Seed InOutHintVertex/InOutHintWalked at 1 and 0.0 for the
	 * first call on a polyline, and reset them there again whenever the polyline itself
	 * changes - see FRouteFollower::Start and ::Replace.
	 */
	AIRSIDE_API bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading,
		int32& InOutHintVertex, double& InOutHintWalked);
}
