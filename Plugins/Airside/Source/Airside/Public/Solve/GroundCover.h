#pragma once

#include "CoreMinimal.h"

/**
 * Where grass tufts stand: pure, world-free and deterministic. A cell of a world-aligned grid
 * and a density layer always give the same tufts, so walking the camera away and back shows
 * the same grass rather than a fresh shuffle, and a test can assert exact positions.
 *
 * WHY CELLS AND LAYERS (spec 2026-10-01-ground-cover-grass-design.md, 4.2-4.3): the grass
 * exists only near the camera, so it is generated in cells that stream in and out; and the
 * density near the camera is a SUM of layers rather than one varying density, so a nearer
 * layer only ever ADDS tufts and nothing already standing pops out as the camera approaches.
 *
 * THE GROUND IS THE PLANE Z = 0. The landscape is flat at 0 (spec 2026-09-12, 4.2; asserted by
 * Tools/Python/verify_landscape.py) and every built surface sits SurfaceZ above it.
 */
namespace GroundCover
{
	/** One tuft: where it stands on the ground, its turn, its size, and which mesh it wears. */
	struct FTuft
	{
		FVector2D Position = FVector2D::ZeroVector;
		float YawDegrees = 0.0f;
		float Scale = 1.0f;
		int32 Variant = 0;
	};

	/** One density layer: how many tufts per square metre, shown out to how far from the viewer. */
	struct FLayerSpec
	{
		double TuftsPerSquareMetre = 0.0;
		double ShowWithinUu = 0.0;
	};

	/** The cell of a CellSizeUu world grid that Point lies in. Floor, so negative coordinates are cells too. */
	AIRSIDE_API FIntPoint CellOf(const FVector2D& Point, double CellSizeUu);

	/** 3D distance from Viewer to the nearest point of Cell's square on the ground plane. */
	AIRSIDE_API double DistanceToCell(const FVector& Viewer, const FIntPoint& Cell, double CellSizeUu);

	/**
	 * Every cell whose nearest ground point lies within RadiusUu of Viewer, replacing Out.
	 * EMPTY WHEN THE VIEWER IS HIGHER THAN THE RADIUS - which is how "grass only when the camera
	 * is low" works with no switch of its own to keep in step with the camera rig.
	 */
	AIRSIDE_API void CellsWithin(const FVector& Viewer, double RadiusUu, double CellSizeUu, TArray<FIntPoint>& Out);

	/**
	 * The tufts of one layer of one cell, APPENDED to Out: round(density x cell area) of them,
	 * uniformly random inside the cell square - not a jittered grid, whose regularity read as
	 * planted rows in the spike; the liked look was scattered singles.
	 */
	AIRSIDE_API void ScatterCell(const FIntPoint& Cell, int32 Layer, double CellSizeUu, double TuftsPerSquareMetre,
		int32 NumVariants, TArray<FTuft>& Out);
}
