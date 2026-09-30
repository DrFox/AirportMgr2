#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UStaticMesh;

/**
 * What UAirsideSettings::ResolveFenceKit resolved: the two post meshes, the fabric material,
 * and the distance the posts have faded out by.
 *
 * EACH MAY BE NULL, and the presenter falls back per member - an engine cube scaled to the
 * post, or the sink's default surface material for the fabric. A test run with no content
 * set draws a grey-box fence rather than nothing.
 *
 * RAW POINTERS, NOT A USTRUCT: resolved and consumed within one UPlotPresenter::RebuildFrom
 * call, with no garbage collection between the two, and the soft pointers on UAirsideContent
 * are what actually hold the assets.
 */
struct FFenceKit
{
	UStaticMesh* LinePost = nullptr;
	UStaticMesh* HeavyPost = nullptr;
	UMaterialInterface* Fabric = nullptr;

	/**
	 * uu from the camera at which a post has fully dithered out: MPC_FenceFade's PostFadeEnd,
	 * read from the collection so the cull and the material cannot disagree. ZERO means no
	 * collection was resolved, and the presenter then leaves the posts unculled - a post that
	 * never fades must never be culled either.
	 */
	double PostFadeEndUu = 0.0;

	/**
	 * THE CHAINLINK KIT'S ASSET CONTRACT, uu - the kit README's figures, typed ONCE (#449). They were typed in
	 * PlotPresenter.cpp (fabric 240, post 245) and again as FenceLayout::FSpec::TileUu (240), with nothing to say the two
	 * 240s were one decision. They are: chainlink.png's V range IS the fabric height, so the fabric must tile along the
	 * run at that same length or the diamonds stop being square - hence TileUu is DEFINED as FabricHeightUu, and
	 * changing it means regenerating the texture. Tools/Python/build_fence_content.py measures PostHeightUu off the
	 * mesh at import (a Python copy this header cannot see; it names this struct).
	 * ENFORCED BY: the static_assert in PlotPresenter.cpp (FenceLayout's Solve/-side default is the kit's)
	 */
	static constexpr double PostHeightUu = 245.0;
	static constexpr double FabricHeightUu = 240.0;
	static constexpr double LinePostDiameterUu = 6.0;
	static constexpr double HeavyPostDiameterUu = 9.0;
	static constexpr double TileUu = FabricHeightUu;
};
