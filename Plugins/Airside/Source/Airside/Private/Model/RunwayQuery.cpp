#include "Model/RunwayQuery.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** RunwayExtentAt and NearestRunwayThreshold, which differ only in the proximity test. */
	bool RunwayExtentInternal(const URoadNetwork& Network, const FVector2D& Near, bool bRequireOnRunway, FRunwayEnd& OutEnd)
	{
		const TArray<FRoadSegment>& Segments = Network.GetSegments();

		// The runway segment with an END nearest the query. Ends rather than centres: a threshold
		// is an end, and a long runway's midpoint can be closer to a query than the end that
		// actually matters.
		int32 Best = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegment& Segment = Segments[Index];
			const FRoadSegmentId Id{Index, Segment.Generation};
			if (!Network.IsRunwaySegment(Id))
			{
				continue;
			}

			const FRoadNode* A = Network.GetNode(Segment.A);
			const FRoadNode* B = Network.GetNode(Segment.B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			const double Distance = FMath::Min(
				FVector2D::Distance(Near, A->Position), FVector2D::Distance(Near, B->Position));
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Index;
			}
		}

		if (Best == INDEX_NONE)
		{
			return false;
		}

		// AND IT HAS TO BE NEAR. Without this the search kept the nearest threshold and never
		// asked how near, so it answered "yes, a runway" for every point on the airport as soon
		// as one runway existed - and every dispatched route armed a departure at it. An aircraft
		// would taxi correctly to a stand on the far side and then jump to the runway and roll.
		//
		// The tolerance is the RUNWAY'S OWN WIDTH, so it scales with the strip rather than being
		// a number chosen to make one airport work: a wider runway is correspondingly more
		// forgiving about where its threshold is considered to begin, and a taxiway a hundred
		// metres away is never mistaken for one.
		//
		// ON THE STRIP, not merely near an end (2026-09-07): since the exit arcs a taxi joins the
		// runway at a split node ExitLength down the centreline, sixty metres from any runway
		// node, and the old "within a width of an end" test refused it - so an intersection
		// departure never armed and the aircraft parked on the runway. IsPointOnRunway is the
		// one implementation of "is this on the strip" that occupancy and the planners share.
		// Either test admits the point: on the strip anywhere along it, OR within a width of an
		// end - the original rule, kept because a route drawn to a threshold ends at the strip's
		// dead-end cut, a half width short of the road node the extent is measured from.
		if (bRequireOnRunway)
		{
			const FRoadSegmentId Seed{Best, Segments[Best].Generation};
			const URoadProfile* SeedProfile = Network.ProfileFor(Segments[Best]);
			const double Reach = SeedProfile != nullptr ? SeedProfile->GetTotalWidth() : 0.0;
			if (!RunwayQuery::IsPointOnRunway(Network, Near, Seed) && BestDistance > Reach)
			{
				return false;
			}
		}

		// THE CHAIN IS THE WALK (#86): RunwayChain already walks out through nodes that join
		// exactly two runway segments, stopping at a threshold or a fork - the same rule this
		// used to walk a second time, node by node, to find the very same two ends. The ends
		// are simply the chain's own nodes touched by exactly one of its segments; a second
		// walk could only ever agree with the first or silently stop doing so.
		//
		// Plain RunwayChain, not RunwayChainOrSeed: Best was found by IsRunwaySegment in the
		// search above, so the chain is never empty here.
		const FRoadSegmentId SeedId{Best, Segments[Best].Generation};
		const TArray<FRoadSegmentId> Chain = RunwayQuery::RunwayChain(Network, SeedId);

		TMap<FRoadNodeId, int32> ChainArms;
		ChainArms.Reserve(Chain.Num() * 2);
		for (const FRoadSegmentId& Member : Chain)
		{
			if (const FRoadSegment* MemberSegment = Network.GetSegment(Member))
			{
				++ChainArms.FindOrAdd(MemberSegment->A);
				++ChainArms.FindOrAdd(MemberSegment->B);
			}
		}

		// The two nodes touched by exactly one chain segment - order from TMap iteration is
		// NOT deterministic, so collect both before choosing which is "EndA".
		TArray<FRoadNodeId, TInlineAllocator<2>> Thresholds;
		for (const TPair<FRoadNodeId, int32>& Arm : ChainArms)
		{
			if (Arm.Value == 1)
			{
				Thresholds.Add(Arm.Key);
			}
		}

		const FRoadNode* NodeA = nullptr;
		const FRoadNode* NodeB = nullptr;
		if (Thresholds.Num() == 2)
		{
			// EndA is explicitly whichever threshold is nearer Segments[Best].A - not "whichever
			// the map iterated first", which a hash reshuffle could change - so a query exactly
			// equidistant from both ends (bNearA below, on <=) resolves the same way every run.
			const FRoadNode* SeedNodeA = Network.GetNode(Segments[Best].A);
			const FRoadNode* First = Network.GetNode(Thresholds[0]);
			const FRoadNode* Second = Network.GetNode(Thresholds[1]);
			const bool bFirstIsA = SeedNodeA != nullptr && First != nullptr && Second != nullptr
				&& FVector2D::DistSquared(SeedNodeA->Position, First->Position)
					<= FVector2D::DistSquared(SeedNodeA->Position, Second->Position);
			NodeA = bFirstIsA ? First : Second;
			NodeB = bFirstIsA ? Second : First;
		}
		if (NodeA == nullptr || NodeB == nullptr)
		{
			return false;
		}

		// The threshold is the end you are AT; you depart away from it.
		const bool bNearA = FVector2D::Distance(Near, NodeA->Position)
			<= FVector2D::Distance(Near, NodeB->Position);

		const FVector2D Threshold = bNearA ? NodeA->Position : NodeB->Position;
		const FVector2D Far = bNearA ? NodeB->Position : NodeA->Position;

		const FVector2D Along = Far - Threshold;
		const double Length = Along.Size();
		if (Length <= 0.0)
		{
			return false;
		}

		// EVERY FIELD WRITTEN HERE, ON THE SUCCESS PATH ONLY - a caller that discards the bool
		// (RunwayAdmission.cpp does, deliberately: a missing node leaves Length 0 and Judge
		// reads that as "no claim") must never see a half-filled OutEnd from an earlier return.
		OutEnd.Seed.Index = Best;
		OutEnd.Seed.Generation = Segments[Best].Generation;
		OutEnd.Threshold = Threshold;
		OutEnd.Length = Length;
		OutEnd.Direction = Along / Length;
		return true;
	}
}

namespace RunwayQuery
{
	TArray<FRoadSegmentId> RunwayChain(const URoadNetwork& Network, FRoadSegmentId Seed)
	{
		TArray<FRoadSegmentId> Out;
		if (!Network.IsRunwaySegment(Seed))
		{
			return Out;
		}
		Out.Add(Seed);

		// RunwayExtentAt reads its thresholds off this chain (#86) rather than walking a second
		// time: from each end of Seed, step through nodes that join exactly two runway segments,
		// and stop at a threshold (one arm) or anything stranger (a fork).
		auto WalkFrom = [&Network, &Out](FRoadNodeId At, FRoadSegmentId Along)
		{
			for (int32 Guard = 0; Guard < 1024; ++Guard)
			{
				const FRoadNode* Node = Network.GetNode(At);
				if (Node == nullptr)
				{
					return;
				}
				FRoadSegmentId Next;
				int32 RunwayArms = 0;
				for (const FRoadSegmentId& Incident : Node->Incident)
				{
					if (!Network.IsRunwaySegment(Incident))
					{
						continue;
					}
					++RunwayArms;
					if (Incident != Along)
					{
						Next = Incident;
					}
				}
				if (RunwayArms != 2 || !Next.IsSet() || Out.Contains(Next))
				{
					return;
				}
				Out.Add(Next);
				At = Network.GetOtherEnd(Next, At);
				Along = Next;
			}
		};

		const FRoadSegment* SeedSegment = Network.GetSegment(Seed);
		WalkFrom(SeedSegment->A, Seed);
		WalkFrom(SeedSegment->B, Seed);
		return Out;
	}

	TArray<FRoadSegmentId> RunwayChainOrSeed(const URoadNetwork& Network, FRoadSegmentId Seed)
	{
		// The idiom four call sites spelled out separately (#86): RunwayChain is empty when
		// Seed is not a live runway (a rebuild dropped it to a taxiway under a stored claim,
		// say), and dropping the claim entirely reads as "nothing to protect" rather than
		// "protect the one segment I still know about" - so the seed itself stands in.
		TArray<FRoadSegmentId> Chain = RunwayChain(Network, Seed);
		if (Chain.Num() == 0)
		{
			Chain.Add(Seed);
		}
		return Chain;
	}

	FRunwayFacts RunwayFactsFor(const URoadNetwork& Network, FRoadSegmentId Seed)
	{
		// The seed's own, not a walk: SetRunwayFacts and the split keep every member of a
		// chain equal, so the first member is as good as any and cheaper than the chain walk
		// the marking builder would otherwise make per runway per rebuild.
		const FRoadSegment* Segment = Network.GetSegment(Seed);
		return Segment != nullptr && Segment->bAlive ? Segment->Runway : FRunwayFacts();
	}

	bool IsGuidelineNodeOnRunway(const URoadNetwork& Network, FGuidelineNodeId Node,
		FRoadSegmentId Seed, double* OutChainHalfWidth)
	{
		// SAME SPLIT AS IsPointOnRunway BELOW, and for the same reason (#170): a caller with
		// only a seed pays for the walk here, and a caller already holding the chain (an
		// FRunwayChainCache entry) does not, through the Chain overload beside it.
		return IsGuidelineNodeOnRunway(Network, Node, RunwayChain(Network, Seed), OutChainHalfWidth);
	}

	bool IsGuidelineNodeOnRunway(const URoadNetwork& Network, FGuidelineNodeId Node,
		const TArray<FRoadSegmentId>& Chain, double* OutChainHalfWidth)
	{
		// A NODE IS A POSITION HERE and nothing else, so the geometry lives in one function and
		// the two callers cannot drift apart. An unknown node reports false with the half width
		// still zeroed, which is what IsPointOnRunway does for a chain that is not a runway.
		const FGuidelineNode* Point = Network.GetGuidelineNode(Node);
		if (Point == nullptr)
		{
			if (OutChainHalfWidth != nullptr)
			{
				*OutChainHalfWidth = 0.0;
			}
			return false;
		}
		return IsPointOnRunway(Network, Point->Position, Chain, OutChainHalfWidth);
	}

	bool IsPointOnRunway(const URoadNetwork& Network, const FVector2D& Position, FRoadSegmentId Seed,
		double* OutChainHalfWidth)
	{
		return IsPointOnRunway(Network, Position, RunwayChain(Network, Seed), OutChainHalfWidth);
	}

	bool IsPointOnRunway(const URoadNetwork& Network, const FVector2D& Position, const TArray<FRoadSegmentId>& Chain,
		double* OutChainHalfWidth)
	{
		if (OutChainHalfWidth != nullptr)
		{
			*OutChainHalfWidth = 0.0;
		}

		bool bOnStrip = false;
		for (const FRoadSegmentId& Id : Chain)
		{
			const FRoadSegment* Segment = Network.GetSegment(Id);
			if (Segment == nullptr)
			{
				continue;
			}
			const FRoadNode* A = Network.GetNode(Segment->A);
			const FRoadNode* B = Network.GetNode(Segment->B);
			const URoadProfile* Profile = Network.ProfileFor(*Segment);
			if (A == nullptr || B == nullptr || Profile == nullptr)
			{
				continue;
			}

			// THE SEGMENT'S OWN HALF WIDTH, not a constant: a chain may mix profiles, and the
			// bound has to scale with the strip it is currently walking.
			const double HalfWidth = Profile->GetTotalWidth() * 0.5;
			if (OutChainHalfWidth != nullptr)
			{
				*OutChainHalfWidth = FMath::Max(*OutChainHalfWidth, HalfWidth);
			}
			if (bOnStrip)
			{
				// Still walking the chain, but only to finish the half-width maximum above.
				continue;
			}

			// THE ROAD NODES' POSITIONS, deliberately, not the sampled ribbon: this asks about
			// the SURFACE model, and the surface's centreline is the segment A..B. A runway is
			// straight in every case the game admits (bContinuousThroughJunctions), so the
			// Bezier control point cannot bend it away from this line.
			const FVector2D Axis = B->Position - A->Position;
			const double Length = Axis.Size();
			if (Length <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector2D Along = Axis / Length;
			const FVector2D Offset = Position - A->Position;
			const double Distance = FVector2D::DotProduct(Offset, Along);
			const double Lateral = FMath::Abs(FVector2D::CrossProduct(Along, Offset));

			bOnStrip = Lateral <= HalfWidth
				&& Distance >= -HalfWidth && Distance <= Length + HalfWidth;
		}
		return bOnStrip;
	}

	bool RunwayExtentAt(const URoadNetwork& Network, const FVector2D& Near, FRunwayEnd& OutEnd)
	{
		return RunwayExtentInternal(Network, Near, true, OutEnd);
	}

	bool NearestRunwayThreshold(const URoadNetwork& Network, const FVector2D& Near, FRunwayEnd& OutEnd)
	{
		// NO PROXIMITY TEST, and that is the difference between the two. RunwayExtentAt answers
		// "is this point ON a runway", which a departure asks of the place its taxi ended and
		// which must say no for the rest of the airport. This answers "which runway would you
		// land on", which is asked of a click that is deliberately nowhere near one.
		return RunwayExtentInternal(Network, Near, false, OutEnd);
	}

	TArray<FGuidelineNodeId> RunwayExitNodes(const URoadNetwork& Network, FRoadSegmentId Seed,
		const FVector2D& Threshold, const FVector2D& Direction, double MinDistance)
	{
		TArray<FGuidelineNodeId> Out;
		if (Direction.IsNearlyZero())
		{
			return Out;
		}

		// Walked ONCE here rather than once per guideline node inside IsPointOnRunway: the
		// walk is the same answer for every point asked of this seed, so paying for it per
		// point would be re-deriving one fact about the network as many times as there are
		// candidate exits.
		const TArray<FRoadSegmentId> Chain = RunwayChain(Network, Seed);

		// Sorted by distance down the runway, because the CALLER's rule is "the first exit I can
		// take". Collected with the distance and sorted at the end rather than inserted in order:
		// the guideline node array is in creation order, which has nothing to do with geometry.
		TArray<TPair<double, FGuidelineNodeId>> Found;

		const TArray<FGuidelineNode>& GuidelineNodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < GuidelineNodes.Num(); ++Index)
		{
			const FGuidelineNode& Node = GuidelineNodes[Index];
			if (!Node.bAlive)
			{
				continue;
			}

			// ONE EVALUATOR OF "IS THIS ON THE STRIP" (#87), the same test the crossing hold and
			// occupancy already share - tested per SEGMENT of the chain against that segment's
			// OWN width, not a HalfWidth the caller measured across the whole airport. This is
			// what makes the far end's own slack, and a multi-width chain, correct without this
			// function knowing either detail.
			if (!IsPointOnRunway(Network, Node.Position, Chain))
			{
				continue;
			}

			const double Distance = FVector2D::DotProduct(Node.Position - Threshold, Direction);

			// Beyond the point the aircraft could have slowed to taxi speed. An exit before that
			// is one it cannot take, which is the whole reason MinDistance is a parameter.
			if (Distance < MinDistance)
			{
				continue;
			}

			Found.Add(TPair<double, FGuidelineNodeId>(Distance, Network.GuidelineNodeIdAt(Index)));
		}

		Found.Sort([](const TPair<double, FGuidelineNodeId>& A, const TPair<double, FGuidelineNodeId>& B)
		{
			return A.Key < B.Key;
		});

		Out.Reserve(Found.Num());
		for (const TPair<double, FGuidelineNodeId>& Entry : Found)
		{
			Out.Add(Entry.Value);
		}
		return Out;
	}
}

const FRunwayChainCache::FEntry& FRunwayChainCache::EntryFor(const URoadNetwork& Network, FRoadSegmentId Seed)
{
	// SAME SHAPE AS FNodeReachCache::Get: the whole table is dropped rather than
	// per-entry-checked, because a stale entry under the OLD revision is exactly as wrong as
	// a missing one and dropping everything is one comparison instead of one per entry.
	if (For != &Network || Revision != Network.GetEditRevision())
	{
		Entries.Reset();
		For = &Network;
		Revision = Network.GetEditRevision();
	}

	if (const FEntry* Found = Entries.Find(Seed.Index))
	{
		return *Found;
	}

	// THE ONE WALK an entry ever pays for: Chain and OrSeed are both derived from it here,
	// so asking this cache for either question about the same seed in the same revision
	// never costs a second one.
	FEntry NewEntry;
	NewEntry.Chain = RunwayQuery::RunwayChain(Network, Seed);
	NewEntry.OrSeed = NewEntry.Chain;
	if (NewEntry.OrSeed.Num() == 0)
	{
		NewEntry.OrSeed.Add(Seed);
	}
	++WalksForTest;
	return Entries.Add(Seed.Index, MoveTemp(NewEntry));
}

const TArray<FRoadSegmentId>& FRunwayChainCache::Get(const URoadNetwork& Network, FRoadSegmentId Seed)
{
	return EntryFor(Network, Seed).Chain;
}

const TArray<FRoadSegmentId>& FRunwayChainCache::GetOrSeed(const URoadNetwork& Network, FRoadSegmentId Seed)
{
	return EntryFor(Network, Seed).OrSeed;
}

void FRunwayChainCache::Invalidate()
{
	Entries.Reset();
	For = nullptr;
	Revision = 0;
}
