#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "RouteFollower.generated.h"

/**
 * Walks a route plan at a constant speed.
 *
 * Deliberately knows nothing about actors, ticks or worlds: it is a distance and a
 * polyline, so the whole of "does the cube go the right way" is testable by calling
 * Advance in a loop with no world at all. The actor that carries one is a view.
 *
 * It walks Plan.Polyline - the very array the overlay draws - so the agent cannot drift
 * off the line the player was shown. See GuidelineGeom.
 *
 * No acceleration, no turn rate, and no awareness of any other agent: heading snaps to the
 * direction of travel and two agents pass straight through each other. Hold-short nodes and
 * the right-of-way rules the graph already carries are NOT consulted. Those are a traffic
 * model, and this is the thing that has to work before a traffic model means anything.
 */
USTRUCT()
struct ROADNET_API FRouteFollower
{
	GENERATED_BODY()

	UPROPERTY() FRoutePlan Plan;

	/** How far along Plan.Polyline, in uu. */
	UPROPERTY() double Travelled = 0.0;

	/** uu per second. 1000 is 10 m/s, a brisk taxi. */
	UPROPERTY() double Speed = 1000.0;

	void Start(const FRoutePlan& InPlan, double InSpeed);

	/**
	 * Moves forward by DeltaSeconds and reports where that leaves the agent.
	 *
	 * False when there is no valid route to walk, leaving the outputs untouched - so a
	 * caller that ignores the return value leaves its agent where it was rather than
	 * teleporting it to the origin, which is this project's most-repeated bug.
	 */
	bool Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading);

	/** True once the whole polyline has been walked. Always true for an invalid plan. */
	bool HasArrived() const;
};
