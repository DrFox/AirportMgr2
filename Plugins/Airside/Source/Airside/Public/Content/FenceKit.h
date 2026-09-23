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
};
