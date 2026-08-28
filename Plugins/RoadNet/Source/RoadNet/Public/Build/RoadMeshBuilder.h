#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
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

	/** Append a solved junction's triangle fan. */
	void AddJunction(const FJunctionResult& Junction);

	/**
	 * Append a segment's ribbon between its two stored cut lines.
	 * RibbonSegments is the number of quads along the segment; 1 is correct for a
	 * straight segment, more for a curve.
	 */
	void AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments = 8);

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
