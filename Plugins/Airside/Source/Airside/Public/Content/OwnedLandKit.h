#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UMaterialParameterCollection;
class UStaticMesh;

/**
 * What a land tile costs (land purchase spec R5): Base x (1 + Growth x Owned). UAirsideSettings::ResolveLandPrice is the
 * only producer outside tests - a figure typed at a second site is a second source of truth (CLAUDE.md, Content/).
 */
struct FLandPrice
{
	double Base = 150000.0;
	double Growth = 0.5;
	double For(int32 Owned) const { return Base * (1.0 + Growth * Owned); }
};

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
