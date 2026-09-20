#pragma once

#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
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
	/**
	 * THE OUTLINE IS A VIEW, AND IT MUST OUTLIVE THE SOLVE. Assigning a function's returned
	 * TArray straight into it binds the view to a temporary that dies at the semicolon, and
	 * the strategy then reads freed memory - which does not crash, it returns a plausible
	 * number. Bind a named local first. (Cost one debugging session on 2026-09-20.)
	 */
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

/**
 * Sheds across the back, tanks down the left, pumps down the right.
 *
 * THIS IS SCAFFOLDING. It exists to make a fuel depot usable now, not to be the fuel depot's
 * final arrangement, and it wastes ground on purpose - the middle of the plot is left empty
 * because that is what a real yard has and what the sampler never left. Nothing here is a
 * capacity rule: how many fit is whatever fitted.
 *
 * IT REPLACES A SAMPLED YARD THAT WAS PREFERRED ON 2026-09-16, and that preference was right
 * about this: every fuel depot built this way will share its bones. The evidence changed -
 * the sampled yard reached 65% coverage and 44 buildings wall to wall on a 45 m plot - and a
 * usable yard that repeats beats a varied one that does not.
 */
UCLASS()
class AIRSIDE_API UFuelYardBandsStrategy : public UPlotLayoutStrategy
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const override;
};

/**
 * The strategy for a layout. Never null for a declared value.
 *
 * ONE INSTANCE PER LAYOUT, held for the life of the process. A strategy is a pure function
 * wearing a UObject - it holds no state between calls and takes its whole world as
 * arguments - so allocating one per plot per rebuild would be churn for nothing.
 */
AIRSIDE_API const UPlotLayoutStrategy* PlotLayoutFor(EPlotLayout Layout);
