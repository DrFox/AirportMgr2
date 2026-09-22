#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UStaticMesh;

/**
 * What UAirsideSettings::ResolveFenceKit resolved: the two post meshes and the fabric material.
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
};
