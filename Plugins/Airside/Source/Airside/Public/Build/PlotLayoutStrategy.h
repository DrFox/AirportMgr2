#pragma once

#include "CoreMinimal.h"
#include "Solve/PlotYard.h"
#include "UObject/Object.h"
#include "PlotLayoutStrategy.generated.h"

/**
 * Where a plot is and how big. Plain data: no UObject, no EDepotModule, no entity.
 *
 * A STRUCT RATHER THAN FIVE PARAMETERS, because PlotYard::Reserve already takes five and
 * FEntityPlacement's own comment warned that its three trailing defaults would not survive a
 * fourth. Every strategy needs the same five and none of them needs a sixth yet.
 */
struct FPlotSite
{
	TArrayView<const FVector2D> Outline;
	FVector2D FrontageA = FVector2D::ZeroVector;
	FVector2D FrontageB = FVector2D::ZeroVector;

	/** Where the fence is left open, which is the entity's own pose. */
	FVector2D Gate = FVector2D::ZeroVector;

	/** Makes a sampling strategy repeatable. A prescriptive one may ignore it. */
	int32 Seed = 0;
};

/**
 * How a plot decides where its modules stand.
 *
 * IN Build/ RATHER THAN Solve/, and not by preference: Solve/ takes CoreMinimal.h and
 * nothing else and holds no UObject, which is what lets its tests run with no world, no
 * actor and no NewObject. Check-Architecture enforces both. Build/ is the layer that already
 * serves Present/ and Tool/ alike - the reason DepotFootprint lives there - so it is where a
 * UObject strategy belongs.
 *
 * A STRATEGY RETURNS STANDS. IT DOES NOT RETURN A CAPACITY. How many of something a plot
 * holds is read off what came back, by FReservation::CeilingFor, and no strategy states a
 * rule for it. See the 2026-09-20 layout-strategies design section 3: deriving a capacity
 * model from the first layout's arithmetic would bury a throwaway decision where a later
 * strategy has to dig it out again.
 */
UCLASS(Abstract)
class AIRSIDE_API UPlotLayoutStrategy : public UObject
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
		PURE_VIRTUAL(UPlotLayoutStrategy::Solve, return PlotYard::FReservation(););
};

/**
 * The sampled yard: modules scattered at quarter turns with a bounded jitter.
 *
 * KEPT, NOT RETIRED. It produced a fuel depot nobody could work in - 65% coverage, 44
 * buildings wall to wall on a 45 m plot - and that is a statement about a FUEL DEPOT rather
 * than about scattering. A plot type that is meant to look unplanned still wants exactly
 * this.
 *
 * A WRAPPER OVER PlotYard::Reserve AND NOTHING MORE. Seven tests are written against that
 * function; giving this behaviour of its own would leave them describing code nobody runs.
 */
UCLASS()
class AIRSIDE_API UScatterLayoutStrategy : public UPlotLayoutStrategy
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const override;
};
