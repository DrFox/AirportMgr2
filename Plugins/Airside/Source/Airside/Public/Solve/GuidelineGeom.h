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
	AIRSIDE_API bool IsStraight(
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
	AIRSIDE_API double ParamAtSample(int32 Index, double Fraction, int32 Count);

	/** Summed length of an already-sampled polyline. */
	AIRSIDE_API double PolylineLength(const TArray<FVector2D>& Points);

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
	AIRSIDE_API void VertexHeadings(
		const TArray<FVector2D>& Points, int32 Vertex,
		double& OutArriving, double& OutLeaving);

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
}
