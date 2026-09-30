#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"

/**
 * A transition as UGroundTraffic would announce it, for a test that hands one to a board or a delegate by hand (#436).
 *
 * THE CAUSE IS THE CALLER'S TO NAME, never guessed from the pair: the boards map Cause, and a helper that derived one
 * from (From, To) would be the pair-mapping #436 took out of production, moved into the tests where nothing checks it.
 * GoalAtEvent defaults unset - no stand - which is what every hand-made Parked before #436 meant, since those agents
 * were not in any traffic model and so parked at no stand.
 *
 * INLINE, in a header: the test module is a unity build, and one definition per .cpp would collide.
 */
inline FAgentTransition OpsTestTransition(int32 AgentId, EAgentPhase From, EAgentPhase To, EAgentEvent Cause,
	FGuidelineNodeId GoalAtEvent = FGuidelineNodeId())
{
	FAgentTransition Made;
	Made.AgentId = AgentId;
	Made.From = From;
	Made.To = To;
	Made.Cause = Cause;
	Made.GoalAtEvent = GoalAtEvent;
	return Made;
}
