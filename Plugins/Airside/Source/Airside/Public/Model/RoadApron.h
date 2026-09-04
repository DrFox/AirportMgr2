#pragma once

#include "CoreMinimal.h"
#include "RoadApron.generated.h"

/**
 * A polygon of pavement - a terminal apron, a stand area, a cargo ramp.
 *
 * An apron has NO cross-section, which is what separates it from a segment. It carries no
 * URoadProfile, because bands and lanes are meaningless for a polygon, and it never enters
 * the junction solve: it has no arms to trim, no fillets to round, and no cut vertices to
 * share. It names a material slot directly.
 *
 * It also generates NO guidelines. A taxiway's yellow line follows its pavement, so it can
 * be derived; an apron's guidelines fan across it in a pattern with no relationship to its
 * edges, so they are drawn by hand. That asymmetry is why surfaces and guidelines are
 * separate graphs at all.
 */
USTRUCT()
struct AIRSIDE_API FApronSurface
{
	GENERATED_BODY()

	/** Simple polygon, counter-clockwise. */
	UPROPERTY() TArray<FVector2D> Outline;

	/** Concrete, asphalt, and so on. Resolved to a material by the presentation layer. */
	UPROPERTY() FName SurfaceMaterialSlot;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
