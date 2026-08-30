#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadApron.h"
#include "Model/RoadHandles.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;

/**
 * Accumulates junction fans and segment ribbons into one welded triangle soup.
 *
 * Vertices are welded through a map keyed on the EXACT FVector2D value. A junction
 * boundary vertex and a segment end vertex that hold the same bits therefore resolve
 * to the same vertex index, which is what makes a seam unrepresentable rather than
 * merely small. The solver guarantees those bits match by storing the cut vertices
 * rather than letting anyone recompute them.
 */
class ROADNET_API FRoadMeshBuilder
{
public:
	explicit FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit = 512.0);

	/**
	 * A point on a cut line, parameterised from the right cut to the left.
	 *
	 * The ONLY way a band vertex is ever produced. The ribbon and the junction rim both
	 * call this with the same two stored cut vertices and the same alpha, so their results
	 * are bitwise identical and weld to one vertex - the same property slice 2a
	 * established for the outer pair, extended inboard. Never inline this or "simplify"
	 * one caller: two expressions that are algebraically equal are not bitwise equal.
	 */
	static FVector2D CutLinePoint(const FVector2D& RightCut, const FVector2D& LeftCut, double Alpha)
	{
		return FMath::Lerp(RightCut, LeftCut, Alpha);
	}

	/**
	 * Append a solved junction's fan, subdivided to match each arm's profile bands.
	 *
	 * ArmSegments is FRoadSolveResult::NodeArmSegments for this node - parallel to
	 * Junction.Arms. It is passed in rather than re-derived because re-walking
	 * Node.Incident re-applies a skip rule that can put the two out of step, which writes
	 * one arm's bands onto another arm's cut line.
	 */
	void AddJunction(const URoadNetwork& Network, int32 NodeIndex, const FJunctionResult& Junction,
		const TArray<FRoadSegmentId>& ArmSegments);

	/**
	 * Append a segment's ribbon between its two stored cut lines.
	 * RibbonSegments is the number of quads along the segment; 1 is correct for a
	 * straight segment, more for a curve.
	 */
	void AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments = 8);

	/**
	 * Build a whole solved network: every live segment, then every junction.
	 *
	 * Prefer this to calling AddSegment and AddJunction by hand. The order is a contract,
	 * not a preference - a cut vertex is one welded vertex holding one UV1, WeldVertex is
	 * first-writer-wins, and a segment measures `along` from its A end while the junction
	 * at its B end would write 0. Getting it backwards makes every segment's markings jump
	 * at one end, and nothing fails when it happens.
	 *
	 * Enforcing that here rather than in a comment on each caller is the same move as the
	 * weld map itself: make the wrong result unrepresentable instead of documented. The
	 * two element functions stay public because tests need to build partial meshes.
	 */
	void Build(const URoadNetwork& Network, const FRoadSolveResult& Solved, int32 RibbonSegments = 1);

	/**
	 * Append an apron polygon.
	 *
	 * An apron never enters the junction solve - it has no arms to trim, no fillets and no
	 * cut vertices to share - so this takes the outline and nothing else. It goes through
	 * the SAME AddTriangle as everything here, deliberately: that function is the one place
	 * that knows Unreal's winding is left-handed, and "every road faced the ground" is a
	 * bug this project has already shipped once. A separate apron builder that re-derived
	 * winding is how it comes back.
	 *
	 * Aprons belong on their own builder INSTANCE, though, so their vertices cannot weld to
	 * a road's. The two surfaces meet; they are not one surface.
	 *
	 * UV1 is zero throughout: lateral offset and distance along a centreline are meaningless
	 * for a polygon, and a made-up value would be sampled by any material that reads them.
	 */
	void AddApron(const FApronSurface& Apron);

	void Emit(IRoadMeshSink& Sink) const;

	const FRoadMeshBuffers& GetBuffers() const { return Buffers; }
	int32 VertexCount() const { return Buffers.Positions.Num(); }

private:
	/**
	 * Returns the index of Point, appending it only if this exact value is new.
	 *
	 * FIRST WRITER WINS. When the point is already present the incoming UV1 and masks
	 * are discarded, because a welded vertex can only hold one of each. That is why
	 * callers add segments BEFORE junctions: a segment measures `along` from its A end,
	 * so its B-end cut vertices carry along = the ribbon's length, while the junction
	 * standing at that node would write along = 0. Segments must therefore write first
	 * and own the shared attributes; junctions then supply values only for the vertices
	 * they alone introduce - arc samples and the fan apex.
	 */
	int32 WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FVector2f& InUV2);

	void AddTriangle(int32 A, int32 B, int32 C);

	double ZHeight;
	double TexelsPerUnit;
	FRoadMeshBuffers Buffers;
	TMap<FVector2D, int32> WeldMap;
};
