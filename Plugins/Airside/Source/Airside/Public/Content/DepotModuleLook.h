#pragma once

#include "CoreMinimal.h"
#include "Entities/PlotModuleKit.h"

class UStaticMesh;

/**
 * What UAirsideSettings::ResolveDepotLooks resolved for ONE depot module: the meshes its kit
 * names, loaded. Indexed like DepotKitSpecs - by EDepotModule - so the presenter reads the
 * look for a stand with the KitIndex it already has.
 *
 * APART FROM PlotYard::FKitSpec, deliberately: a spec is what the solver needs and stays
 * dependency-free in Solve/, while this carries meshes the solver must never see. Two arrays
 * indexed by one enum, built by walking the same enum - see DepotKitSpecs' own comment.
 *
 * RAW POINTERS, NOT A USTRUCT, for FFenceKit's reason: resolved and consumed within one
 * UPlotPresenter::RebuildFrom call, and the soft pointers on the kit hold the assets.
 */
struct FDepotModuleLook
{
	EKitAssembly Assembly = EKitAssembly::Baked;

	/** Baked: index = bay count - 1. Empty means grey box. */
	TArray<UStaticMesh*> Baked;

	/** Parts: both, or the kit draws as a grey box. */
	UStaticMesh* Cap = nullptr;
	UStaticMesh* Bay = nullptr;

	/** UPlotModuleKit::MeshYawDeg, snapped to a quarter turn. */
	double MeshYawDeg = 0.0;

	/** True when this module draws meshes rather than a grey box. */
	bool HasMeshes() const
	{
		return Assembly == EKitAssembly::Parts
			? (Cap != nullptr && Bay != nullptr)
			: Baked.Num() > 0;
	}
};
