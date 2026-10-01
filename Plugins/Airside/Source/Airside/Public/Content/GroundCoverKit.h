#pragma once

#include "CoreMinimal.h"
#include "Solve/GroundCover.h"

class UStaticMesh;

/**
 * Everything the ground-cover grass is drawn from, resolved once - the meshes, the layers in
 * uu, the cell size. UAirsideSettings::ResolveGroundCover is the only producer outside tests,
 * so a density typed anywhere else is a second source of truth (CLAUDE.md, Content/).
 *
 * RAW POINTERS, NOT TObjectPtr: a plain struct, like FFenceKit. Whoever KEEPS the meshes past
 * the call holds them in a UPROPERTY (UGroundCoverPresenter::TuftMeshes does).
 */
struct FGroundCoverKit
{
	TArray<UStaticMesh*> Tufts;
	TArray<GroundCover::FLayerSpec> Layers;
	double CellSizeUu = 3200.0;

	/** Something to draw: at least one mesh, one layer and a real cell. */
	bool IsUsable() const { return Tufts.Num() > 0 && Layers.Num() > 0 && CellSizeUu > 0.0; }
};
