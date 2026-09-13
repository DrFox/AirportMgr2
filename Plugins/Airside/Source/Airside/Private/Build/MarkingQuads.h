#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"

/**
 * The one way a ground marking becomes triangles, shared by the holding-position and
 * runway marking builders. Private to Build/: a header of inline functions rather than a
 * class, because there is no state - and rather than two copies, because the winding
 * lesson below was learnt once and must be applied everywhere a marking is painted.
 */
namespace MarkingQuads
{
	/**
	 * One quad, corners in either rotational order, wound counter-clockwise as seen from +Z
	 * by measurement - the maths convention every builder here works in. Emitted through
	 * FRoadMeshBuffers::AppendTriangleUp, the SAME flip-plus-degenerate-guard
	 * FRoadMeshBuilder::AddTriangle uses (#103) - AddQuad used to apply the flip itself,
	 * unguarded, which is also why UV2 below is now (0,1): RoadMeshSink.h's own contract on
	 * UV2 ("Y is reserved and always 1") was being violated by every marking quad.
	 *
	 * Four consecutive vertices per call, ALWAYS, so a test can walk the buffers four
	 * vertices at a time and measure each marking on its own - but NOT two triangles per
	 * call any more (review of #103): AppendTriangleUp drops a degenerate or sliver one,
	 * which a marking too small or too thin to rasterise can now hit exactly like any other
	 * geometry the builders emit.
	 */
	inline void AddQuad(FRoadMeshBuffers& Out, double Z,
		const FVector2D& P0, const FVector2D& P1, const FVector2D& P2, const FVector2D& P3)
	{
		const int32 Base = Out.Positions.Num();
		FVector2D Corners[4] = { P0, P1, P2, P3 };
		// MEASURED, not trusted. The first cut of the holding-position builder handed its
		// corners over "counter-clockwise" by inspection and every one of them was
		// clockwise: 112 of 112 vertex normals pointed down in
		// Airside.Build.HoldingPositionMarking. A bar's corner order depends on which way
		// its axes happen to point, so the signed area decides here and the caller's order
		// is a hint at most.
		double TwiceArea = 0.0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FVector2D& A = Corners[Index];
			const FVector2D& B = Corners[(Index + 1) % 4];
			TwiceArea += A.X * B.Y - B.X * A.Y;
		}
		if (TwiceArea < 0.0)
		{
			Swap(Corners[1], Corners[3]);
		}
		const FVector2f UV0s[4] = { FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Out.Positions.Add(FVector3d(Corners[Index].X, Corners[Index].Y, Z));
			Out.UV0.Add(UV0s[Index]);
			// Lateral 0 everywhere: the whole quad is centreline as far as the material can
			// tell, which is what paints it MarkingColor - see FHoldingPositionMarkingBuilder.
			Out.UV1.Add(FVector2f(0.f, 0.f));
			Out.UV2.Add(FVector2f(0.f, 1.f));
		}
		// (0,1,2) and (0,2,3) counter-clockwise; AppendTriangleUp applies its own flip.
		Out.AppendTriangleUp(Base + 0, Base + 1, Base + 2, 0);
		Out.AppendTriangleUp(Base + 0, Base + 2, Base + 3, 0);
	}

	/**
	 * An axis-aligned rectangle in a runway's frame: Along is the unit direction down the
	 * strip, Across its perpendicular; [Along0, Along1] and [Across0, Across1] are the
	 * extents from Origin. The shape every runway marking but a glyph stroke is.
	 */
	inline void AddRect(FRoadMeshBuffers& Out, double Z, const FVector2D& Origin,
		const FVector2D& Along, const FVector2D& Across,
		double Along0, double Along1, double Across0, double Across1)
	{
		AddQuad(Out, Z,
			Origin + Along * Along0 + Across * Across0,
			Origin + Along * Along1 + Across * Across0,
			Origin + Along * Along1 + Across * Across1,
			Origin + Along * Along0 + Across * Across1);
	}
}
