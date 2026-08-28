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
	explicit FRoadMeshBuilder(double InZHeight);

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
	/** Returns the index of Point, appending it only if this exact value is new. */
	int32 WeldVertex(const FVector2D& Point);

	void AddTriangle(int32 A, int32 B, int32 C);

	double ZHeight;
	FRoadMeshBuffers Buffers;
	TMap<FVector2D, int32> WeldMap;
};
