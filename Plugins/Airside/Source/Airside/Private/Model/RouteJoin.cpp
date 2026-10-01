#include "Model/RouteJoin.h"

#include "Solve/GuidelineGeom.h"

bool RouteJoin::Prepend(const FRoutePlan& Route, const FVector2D& From, double JoinAlong, FRoutePlan& OutJoined)
{
	FVector2D JoinAt = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (JoinAlong < 0.0 || JoinAlong > Route.Length || !GuidelineGeom::PointAtDistance(Route.Polyline, JoinAlong, JoinAt, Tangent))
	{
		return false;
	}
	// THE FIRST VERTEX PAST THE JOIN, by the arc length PointAtDistance and every EndDistance were measured with.
	int32 Keep = Route.Polyline.Num();
	double Walked = 0.0;
	for (int32 Index = 1; Index < Route.Polyline.Num(); ++Index)
	{
		Walked += FVector2D::Distance(Route.Polyline[Index - 1], Route.Polyline[Index]);
		if (Walked > JoinAlong + UE_KINDA_SMALL_NUMBER)
		{
			Keep = Index;
			break;
		}
	}

	const double Leg = FVector2D::Distance(From, JoinAt);
	FRoutePlan Joined = Route;
	Joined.Polyline.Reset();
	Joined.Polyline.Add(From);
	Joined.Polyline.Add(JoinAt);
	for (int32 Index = Keep; Index < Route.Polyline.Num(); ++Index)
	{
		Joined.Polyline.Add(Route.Polyline[Index]);
	}
	for (FRouteStep& Step : Joined.Steps)
	{
		// The first step may end AT the join (a push's JoinTo clamped to it): its end is then the join's own vertex.
		const bool bEndsAtJoin = Step.EndDistance <= JoinAlong + UE_KINDA_SMALL_NUMBER;
		Step.EndVertex = bEndsAtJoin ? 1 : Step.EndVertex - Keep + 2;
		Step.EndDistance = bEndsAtJoin ? Leg : Leg + (Step.EndDistance - JoinAlong);
	}
	// THE LEG PLUS WHAT WAS LEFT, as the held taxi out summed it, not PolylineLength as the push's join re-measured it: the
	// two differ by rounding only, and this way the last step's EndDistance is the length exactly, as on any planned route.
	Joined.Length = Leg + (Route.Length - JoinAlong);
	OutJoined = Joined;
	return true;
}
