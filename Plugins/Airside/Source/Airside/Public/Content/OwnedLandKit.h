#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UMaterialParameterCollection;
class UStaticMesh;

/**
 * What the owned land is drawn with, resolved once - UAirsideSettings::ResolveOwnedLandKit is the
 * only producer outside tests. RAW POINTERS, like FFenceKit: whoever keeps them past the call holds
 * them in a UPROPERTY (the wall components do, by being assigned them).
 */
struct FOwnedLandKit
{
	UMaterialParameterCollection* Collection = nullptr;
	UStaticMesh* WallMesh = nullptr;
	UMaterialInterface* WallMaterial = nullptr;
};
