#pragma once

#include "CoreMinimal.h"
#include "LandGrid.generated.h"

/** One straight stretch of the owned land's boundary, A to B, with the normal pointing into the void. */
struct FLandEdgeRun
{
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	FVector2D Outward = FVector2D::ZeroVector;
};

/**
 * The land the player owns, as tiles of a fixed grid (land purchase spec 2026-10-02 R1-R4). THE ONE RECORD:
 * the ground clip, the plinth walls, the build camera, the grass and every build refusal read this, through
 * URoadNetwork::GetOwnedLand. URoadEditFacade is the only writer.
 *
 * AN INVALID GRID OWNS EVERYTHING - the default, so a network saved before land existed, and every map
 * that never set one, keeps building anywhere.
 *
 * uint64 AND 8x8 ARE ONE FACT: a bigger grid is a different mask type, and IsValid refuses one rather
 * than letting bits wrap. Tiles are FIntPoint(X = column along world X, Y = row along world Y), bit
 * Y * Columns + X.
 * ENFORCED BY: Airside.Model.LandGrid.DefaultOwnsEverything
 */
USTRUCT()
struct AIRSIDE_API FLandGrid
{
	GENERATED_BODY()

	static constexpr int32 MaxTiles = 64;

	/** South-west (min X, min Y) corner of tile (0,0), uu. */
	UPROPERTY() FVector2D Origin = FVector2D::ZeroVector;
	UPROPERTY() double TileSize = 60000.0;
	UPROPERTY() int32 Columns = 8;
	UPROPERTY() int32 Rows = 8;
	UPROPERTY() uint64 Owned = 0;

	static FLandGrid Make(FVector2D InOrigin, double InTileSize, int32 InColumns, int32 InRows, TConstArrayView<FIntPoint> Start);

	/** At least one tile owned, a real tile size, and at most MaxTiles tiles. */
	bool IsValid() const;
	bool IsOnGrid(FIntPoint Tile) const { return Tile.X >= 0 && Tile.Y >= 0 && Tile.X < Columns && Tile.Y < Rows; }
	int32 BitOf(FIntPoint Tile) const { return Tile.Y * Columns + Tile.X; }
	bool IsTileOwned(FIntPoint Tile) const;
	void SetTileOwned(FIntPoint Tile, bool bOwned);
	int32 NumOwned() const;
	/** R4: on the grid, unowned, and sharing an EDGE (not a corner) with an owned tile. */
	bool IsBuyable(FIntPoint Tile) const;
	FIntPoint TileAt(FVector2D Point) const;
	FBox2D TileBox(FIntPoint Tile) const;

	/**
	 * Is this ground the player's? A point ON a tile boundary is owned only if every tile it touches is - the
	 * inclusive-covered rule FGroundCoverMask uses for slabs: a tuft or a wall centred there leans over the cut.
	 * Invalid grid: true.
	 */
	bool IsOwned(FVector2D Point) const;

	/** The nearest owned point - per tile, not the bounding box, so an L cannot leave the camera over its notch. */
	FVector2D ClampToOwned(FVector2D Point) const;

	/** The boundary as straight runs: unit tile edges with an unowned neighbour, merged where collinear and touching. */
	TArray<FLandEdgeRun> Outline() const;

	/** Bits 16*Word .. 16*Word+15 - four of these carry the mask to the material, one float each (exact to 2^24). */
	uint16 MaskWord(int32 Word) const { return static_cast<uint16>((Owned >> (16 * Word)) & 0xFFFFu); }
};
