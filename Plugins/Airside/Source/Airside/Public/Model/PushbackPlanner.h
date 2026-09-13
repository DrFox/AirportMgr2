#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/RouteSearch.h"
#include "PushbackPlanner.generated.h"

class URoadNetwork;
struct FAirframe;

/**
 * Everything a push off a stand needs, decided before anything moves - the mirror of
 * FDeparturePlan, and planned in the same breath as one.
 *
 * TWO ROUTES, AND THAT IS THE POINT. A pushback does not reverse along the route the
 * aeroplane is about to taxi; it reverses onto the OTHER arm of the taxiway, so that driving
 * forward afterwards carries it through the junction and away. The aeroplane therefore ends
 * somewhere the departure route never visits, and the taxi out has to be planned FROM THERE.
 *
 * WHY THAT IS NOT OBVIOUS, and cost a PIE session to see: walking a prefix of the departure
 * route while facing backwards looks right at the stand - the aeroplane reverses down its
 * lead-in correctly - and is wrong the moment it reaches the taxiway, because it then keeps
 * reversing the way it meant to taxi. It finishes past the junction, facing the right way,
 * on the wrong side of the turn it was supposed to make. Reported as "it crabbed around
 * using the wrong arm", with photographs.
 */
USTRUCT()
struct AIRSIDE_API FPushbackPlan
{
	GENERATED_BODY()

	UPROPERTY() EDepartureRefusal Why = EDepartureRefusal::NoPushbackRoute;

	/** Stand to the far side of the junction, walked FORWARDS by FPushbackRun with the body
	 *  reversed - so the aeroplane travels this way and faces the other. */
	UPROPERTY() FRoutePlan PushRoute;

	/** Where the push ends to wherever the departure was going. Planned now, at dispatch, for
	 *  the reason FRoadAgent::TaxiInPlan is: an aeroplane must never be pushed somewhere it
	 *  cannot then taxi out of. */
	UPROPERTY() FRoutePlan TaxiOutRoute;

	bool IsValid() const
	{
		return Why == EDepartureRefusal::None && PushRoute.IsValid() && TaxiOutRoute.IsValid();
	}
};

namespace PushbackPlanner
{
	/**
	 * Plans the push and the taxi that follows it, or says why it cannot.
	 *
	 * REFUSES RATHER THAN IMPROVISES when the stand has no second arm to reverse onto - a
	 * stand at the end of a dead-end taxiway. The alternative considered and rejected was to
	 * fall back on reversing along the departure's own arm, which is exactly the behaviour
	 * that was reported as wrong; doing it only sometimes would make it a defect that appears
	 * on some layouts and not others. A stand nothing can leave is a layout problem the
	 * player can see and fix, and EDepartureRefusal::NoPushbackRoute says so.
	 *
	 * ClearBy is how far past the junction the aeroplane must finish to be out of the turn -
	 * footprint plus gap, the same pair the traffic model already means by "clear of".
	 */
	AIRSIDE_API FPushbackPlan Plan(const URoadNetwork& Network, FGuidelineNodeId PoseNode,
		const FDeparturePlan& Departure, const FAirframe& Airframe, ETraversalClass Class,
		double ClearBy);

	/** One line naming what was planned or why it was not, for the log. */
	AIRSIDE_API FString Describe(const FPushbackPlan& Plan);
}
