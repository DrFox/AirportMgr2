#include "Model/PushbackPlanner.h"

#include "AirsideLog.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

namespace
{
	/** Where a guideline node sits, or the zero vector if the handle is dead. */
	FVector2D PositionOf(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
		return Found != nullptr ? Found->Position : FVector2D::ZeroVector;
	}

	/** The end of Edge that is not From. */
	FGuidelineNodeId FarEndOf(const URoadNetwork& Network, FGuidelineEdgeId Edge, FGuidelineNodeId From)
	{
		const FGuidelineEdge* Found = Network.GetGuidelineEdge(Edge);
		if (Found == nullptr)
		{
			return FGuidelineNodeId();
		}
		return Found->A == From ? Found->B : Found->A;
	}

	/**
	 * The arm of the junction the departure does NOT take.
	 *
	 * A stand's lead-in ends where its two entry sweeps begin - three edges meet there, which
	 * Airside.Build.AnchorLink asserts by name. One is the lead-in the aeroplane came down,
	 * one is the sweep the departure route turns onto, and the third is the one a pushback
	 * wants: the same corner taken the other way.
	 *
	 * Unset when there is no such edge, which is a stand with nowhere to be pushed.
	 */
	FGuidelineEdgeId OppositeArm(const URoadNetwork& Network, FGuidelineNodeId LeadEnd,
		FGuidelineEdgeId LeadIn, FGuidelineEdgeId Taken)
	{
		const FGuidelineNode* Junction = Network.GetGuidelineNode(LeadEnd);
		if (Junction == nullptr)
		{
			return FGuidelineEdgeId();
		}

		for (const FGuidelineEdgeId Candidate : Junction->Incident)
		{
			if (Candidate != LeadIn && Candidate != Taken)
			{
				return Candidate;
			}
		}
		return FGuidelineEdgeId();
	}

	/**
	 * Nodes along one arm, in order, walking away from the junction.
	 *
	 * STRAIGHTEST CONTINUATION AT EACH STEP, because a taxiway is sampled into several edges
	 * and any of them may meet a stand or a service road on the way past. Following the
	 * heading the aeroplane is already being pulled in keeps the push on the taxiway rather
	 * than turning it up somebody else's lead-in - which a plain "any edge but the one I came
	 * from" rule would do at the first stand it passed.
	 *
	 * Bounded at a handful of steps: a push is tens of metres, and a walk that has not found
	 * its room by then is on a layout this cannot help with.
	 */
	void WalkArm(const URoadNetwork& Network, FGuidelineNodeId From, FGuidelineEdgeId Along,
		TArray<FGuidelineNodeId>& OutNodes)
	{
		constexpr int32 MaxSteps = 6;

		FGuidelineNodeId Previous = From;
		FGuidelineEdgeId Edge = Along;

		for (int32 Step = 0; Step < MaxSteps; ++Step)
		{
			const FGuidelineNodeId Next = FarEndOf(Network, Edge, Previous);
			if (!Next.IsSet())
			{
				return;
			}
			OutNodes.Add(Next);

			const FGuidelineNode* At = Network.GetGuidelineNode(Next);
			if (At == nullptr)
			{
				return;
			}

			FVector2D Heading = PositionOf(Network, Next) - PositionOf(Network, Previous);
			if (!Heading.Normalize())
			{
				return;
			}

			FGuidelineEdgeId Best;
			double BestDot = -2.0;
			for (const FGuidelineEdgeId Candidate : At->Incident)
			{
				if (Candidate == Edge)
				{
					continue;
				}

				const FGuidelineNodeId Beyond = FarEndOf(Network, Candidate, Next);
				FVector2D Onward = PositionOf(Network, Beyond) - PositionOf(Network, Next);
				if (!Onward.Normalize())
				{
					continue;
				}

				const double Dot = FVector2D::DotProduct(Heading, Onward);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					Best = Candidate;
				}
			}

			if (!Best.IsSet())
			{
				return;
			}
			Previous = Next;
			Edge = Best;
		}
	}
}

FPushbackPlan PushbackPlanner::Plan(const URoadNetwork& Network, FGuidelineNodeId PoseNode,
	const FDeparturePlan& Departure, const FAirframe& Airframe, ETraversalClass Class,
	double ClearBy)
{
	FPushbackPlan Out;

	// TWO STEPS AT LEAST: a lead-in and something to turn onto. A route that leaves the stand
	// and arrives in one step has no junction, so there is no second arm and nothing to plan.
	if (!Departure.Route.IsValid() || Departure.Route.Steps.Num() < 2)
	{
		return Out;
	}

	const FGuidelineEdgeId LeadIn = Departure.Route.Steps[0].Edge;
	const FGuidelineNodeId LeadEnd = Departure.Route.Steps[0].To;
	const FGuidelineEdgeId Taken = Departure.Route.Steps[1].Edge;

	const FGuidelineEdgeId Arm = OppositeArm(Network, LeadEnd, LeadIn, Taken);
	if (!Arm.IsSet())
	{
		// THE STAND HAS NOWHERE TO BE PUSHED. Refused, deliberately - see the header.
		return Out;
	}

	TArray<FGuidelineNodeId> Along;
	WalkArm(Network, LeadEnd, Arm, Along);
	if (Along.Num() == 0)
	{
		return Out;
	}

	// THE FIRST NODE THAT LEAVES THE AEROPLANE CLEAR OF THE TURN, and the last one walked if
	// none does. Measured on the ROUTE the search actually returns rather than on the edges,
	// because that is the distance FPushbackRun will walk and the two must not disagree.
	//
	// THE DEPARTURE'S OWN ARM IS BANNED so the search cannot simply turn the way the taxi
	// will go and come back round: the whole point is to finish on the other side.
	const double LeadInLength = Departure.Route.Steps[0].EndDistance;

	for (const FGuidelineNodeId Goal : Along)
	{
		FRouteQuery Query = FRouteQuery::For(PoseNode, Goal, Airframe, Class);
		Query.BannedEdge = Taken;

		const FRoutePlan Candidate = RouteSearch::Find(Network, Query);
		if (!Candidate.IsValid())
		{
			continue;
		}

		Out.PushRoute = Candidate;
		if (Candidate.Length >= LeadInLength + ClearBy)
		{
			break;
		}
	}

	if (!Out.PushRoute.IsValid())
	{
		return Out;
	}

	// AND THE TAXI OUT FROM WHERE THE PUSH ENDS, not from the stand. The aeroplane will be
	// standing somewhere the departure route never visits, so the route it was planned with
	// no longer starts where it is - and an aeroplane pushed somewhere it cannot taxi out of
	// is worse than one that never left, because the stand is occupied either way and this
	// one is in the middle of a taxiway.
	const FGuidelineNodeId PushEnd = Out.PushRoute.Steps.Last().To;
	const FGuidelineNodeId Goal = Departure.Route.Steps.Last().To;

	FRouteQuery Onward = FRouteQuery::For(PushEnd, Goal, Airframe, Class);
	Out.TaxiOutRoute = RouteSearch::Find(Network, Onward);
	if (!Out.TaxiOutRoute.IsValid())
	{
		return Out;
	}

	Out.Why = EDepartureRefusal::None;
	return Out;
}

FString PushbackPlanner::Describe(const FPushbackPlan& Plan)
{
	if (!Plan.IsValid())
	{
		return TEXT("Pushback refused: the stand has no arm to reverse onto.");
	}

	return FString::Printf(
		TEXT("Pushback: %.0f uu back onto the far arm, then %.0f uu taxiing out."),
		Plan.PushRoute.Length, Plan.TaxiOutRoute.Length);
}
