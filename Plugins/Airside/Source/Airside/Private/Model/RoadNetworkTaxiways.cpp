#include "Model/RoadNetwork.h"

#include "Algo/Reverse.h"
#include "AirsideLog.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Solve/TaxiwayLetters.h"

// TAXIWAY NAMES (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): URoadNetwork's members that judge
// geometry and name segments, in a file of their own. RoadNetwork.cpp is held to its line figure by Check-Architecture
// rule 77, and rule 103 holds every write of FRoadSegment::TaxiwayId to THIS file - which a shared file could not be
// scoped to. The table's lookups, display names, issuing and retirement are in RoadNetworkTaxiwayTable.cpp (rule 77's
// 800-line default, reached 2026-10-02).

namespace
{
	// Prefixed against the UNITY build, like every file-local helper in this module.

	double TaxiwayNamesLength(const URoadNetwork& Net, FRoadSegmentId Id)
	{
		// THE ONE LENGTH a road has here: GuidelineGeom's sampled length of its own Bezier, the figure the route search
		// already costs - not the chord, which would rank a bent taxiway shorter than it is drawn.
		FVector2D A;
		FVector2D B;
		const FRoadSegment* Segment = Net.GetSegment(Id);
		return Segment != nullptr && Net.SegmentEnds(Id, A, B) ? GuidelineGeom::Length(A, Segment->Control, B) : 0.0;
	}

	/** Does the road carry on from From to Other through Node? A bend (two roads at the node) within BendDegrees, a
	 *  junction within RoadGeom::InLineDegrees (plan D1). OutAngle is the straightness, PI being straight. */
	bool TaxiwayNamesCarriesOn(const URoadNetwork& Net, FRoadNodeId Node, FRoadSegmentId From, FRoadSegmentId Other,
		const FTaxiwayNamingRules& Rules, double& OutAngle)
	{
		const FRoadNode* At = Net.GetNode(Node);
		const FVector2D OutFrom = Net.GetOutgoingTangent(From, Node);
		const FVector2D OutOther = Net.GetOutgoingTangent(Other, Node);
		OutAngle = RoadGeom::AngleBetween(OutFrom, OutOther);
		if (At == nullptr)
		{
			return false;
		}
		return At->Incident.Num() == 2
			? OutAngle >= UE_DOUBLE_PI - FMath::DegreesToRadians(Rules.BendDegrees)
			: RoadGeom::IsInLine(OutFrom, OutOther);
	}

	TArray<FRoadSegmentId> TaxiwayNamesOwnAt(const URoadNetwork& Net, FRoadNodeId Node, int32 TaxiwayId)
	{
		TArray<FRoadSegmentId> Own;
		if (const FRoadNode* At = Net.GetNode(Node))
		{
			for (const FRoadSegmentId& Each : At->Incident)
			{
				if (Net.TaxiwayOf(Each) == TaxiwayId)
				{
					Own.Add(Each);
				}
			}
		}
		return Own;
	}

	/** Walk both ways from Seed, NextAt choosing each step; Taken stops a loop and keeps chains disjoint. */
	FTaxiwayChain TaxiwayNamesWalk(const URoadNetwork& Net, FRoadSegmentId Seed,
		TFunctionRef<FRoadSegmentId(FRoadNodeId, FRoadSegmentId)> NextAt, TSet<FRoadSegmentId>& Taken)
	{
		FTaxiwayChain Chain;
		const FRoadSegment* Start = Net.GetSegment(Seed);
		if (Start == nullptr)
		{
			return Chain;
		}
		Taken.Add(Seed);
		TArray<FRoadSegmentId> Back;
		TArray<FRoadSegmentId> Ahead;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			FRoadNodeId Node = Side == 0 ? Start->A : Start->B;
			FRoadSegmentId Previous = Seed;
			TArray<FRoadSegmentId>& Into = Side == 0 ? Back : Ahead;
			for (;;)
			{
				const FRoadSegmentId Next = NextAt(Node, Previous);
				if (!Next.IsSet() || Taken.Contains(Next))
				{
					break;
				}
				Taken.Add(Next);
				Into.Add(Next);
				Node = Net.GetOtherEnd(Next, Node);
				Previous = Next;
			}
			(Side == 0 ? Chain.First : Chain.Last) = Node;
		}
		Algo::Reverse(Back);
		Chain.Segments = MoveTemp(Back);
		Chain.Segments.Add(Seed);
		Chain.Segments.Append(Ahead);
		for (const FRoadSegmentId& Id : Chain.Segments)
		{
			Chain.Length += TaxiwayNamesLength(Net, Id);
			Chain.LowestIndex = FMath::Min(Chain.LowestIndex, Id.Index);
		}
		return Chain;
	}

	/** The LETTERED taxiways at Node outside Chain - a connector there counts as its parent. Sorted, so lowest id first. */
	TArray<int32> TaxiwayNamesRootsAt(const URoadNetwork& Net, FRoadNodeId Node, const FTaxiwayChain& Chain)
	{
		TArray<int32> Roots;
		if (const FRoadNode* At = Net.GetNode(Node))
		{
			for (const FRoadSegmentId& Other : At->Incident)
			{
				const FTaxiway* Taxiway = Chain.Segments.Contains(Other) ? nullptr : Net.GetTaxiway(Net.TaxiwayOf(Other));
				if (Taxiway != nullptr)
				{
					Roots.AddUnique(Taxiway->IsConnector() ? Taxiway->ParentId : Taxiway->Id);
				}
			}
		}
		Roots.Sort();
		return Roots;
	}

	/** An end a connector may stop at: a taxiway, a runway, or a dead end - nothing but Chain there (plan D2). A taxiway
	 *  STILL UNNAMED counts: every taxiway segment is named by the end of the pass that meets it (AssignUnnamedTaxiways),
	 *  so "named" alone made a backfill judge a link's first half against its second, unnamed yet - not anchored, a
	 *  letter - where drawing the same halves click by click gave A1, A2 (PR #524 review). Once a pass ends the two
	 *  readings agree, so RejudgeTaxiway and a draw-time click see no difference. */
	bool TaxiwayNamesAnchored(const URoadNetwork& Net, FRoadNodeId Node, const FTaxiwayChain& Chain)
	{
		const FRoadNode* At = Net.GetNode(Node);
		if (At == nullptr)
		{
			return false;
		}
		bool bDeadEnd = true;
		for (const FRoadSegmentId& Other : At->Incident)
		{
			if (Chain.Segments.Contains(Other))
			{
				continue;
			}
			bDeadEnd = false;
			if (Net.IsRunwaySegment(Other) || Net.TaxiwayOf(Other) != INDEX_NONE || TaxiwayStrip::HasStrip(Net, Other))
			{
				return true;
			}
		}
		return bDeadEnd;
	}

	/** The parent Chain would be a connector of, or INDEX_NONE (a letter). Except: a taxiway that may not parent it (itself). */
	int32 TaxiwayNamesConnectorParent(const URoadNetwork& Net, const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules,
		int32 Except)
	{
		if (Chain.Segments.Num() == 0 || Chain.Length >= Rules.ConnectorMaxLength
			|| !TaxiwayNamesAnchored(Net, Chain.First, Chain) || !TaxiwayNamesAnchored(Net, Chain.Last, Chain))
		{
			return INDEX_NONE;
		}
		// FIRST END FIRST (spec: "connector of the taxiway it leaves (first end)"); a first end on a runway has no root,
		// so the taxiway at the other end parents it - "if it leaves a runway and lands on a taxiway, the taxiway is the parent".
		for (const FRoadNodeId& End : { Chain.First, Chain.Last })
		{
			for (const int32 Root : TaxiwayNamesRootsAt(Net, End, Chain))
			{
				if (Root != Except)
				{
					return Root;
				}
			}
		}
		return INDEX_NONE;
	}

	/** The taxiway Chain carries on from: one that ENDS at an end of Chain (one of its segments there) and lines up
	 *  with Chain's terminal segment. The straightest wins, then the lowest id; the first end before the last. */
	int32 TaxiwayNamesInheritable(const URoadNetwork& Net, const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules)
	{
		for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex)
		{
			const FRoadNodeId End = EndIndex == 0 ? Chain.First : Chain.Last;
			const FRoadSegmentId Terminal = EndIndex == 0 ? Chain.Segments[0] : Chain.Segments.Last();
			const FRoadNode* At = Net.GetNode(End);
			if (At == nullptr)
			{
				continue;
			}
			int32 Best = INDEX_NONE;
			double BestAngle = -1.0;
			for (const FRoadSegmentId& Other : At->Incident)
			{
				const int32 Owner = Net.TaxiwayOf(Other);
				double Angle = 0.0;
				if (Owner == INDEX_NONE || Chain.Segments.Contains(Other) || TaxiwayNamesOwnAt(Net, End, Owner).Num() != 1
					|| !TaxiwayNamesCarriesOn(Net, End, Terminal, Other, Rules, Angle))
				{
					continue;
				}
				if (Angle > BestAngle || (Angle == BestAngle && Owner < Best))
				{
					Best = Owner;
					BestAngle = Angle;
				}
			}
			if (Best != INDEX_NONE)
			{
				return Best;
			}
		}
		return INDEX_NONE;
	}
}

void URoadNetwork::WriteTaxiwayId(FRoadSegmentId Segment, int32 TaxiwayId)
{
	if (FRoadSegment* Found = GetSegmentMutable(Segment))
	{
		Found->TaxiwayId = TaxiwayId;
	}
}

void FRoadNetworkTestAccess::ClearTaxiwayNamesForTest()
{
	for (FRoadSegment& Segment : Network.Segments)
	{
		Segment.TaxiwayId = INDEX_NONE;
	}
	Network.Taxiways.Reset();
}

int32 URoadNetwork::AssignTaxiway(const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules)
{
	if (Chain.Segments.Num() == 0)
	{
		return INDEX_NONE;
	}
	int32 Named = TaxiwayNamesInheritable(*this, Chain, Rules);
	const bool bInherited = Named != INDEX_NONE;
	if (!bInherited)
	{
		Named = MintTaxiway(TaxiwayNamesConnectorParent(*this, Chain, Rules, INDEX_NONE));
	}
	for (const FRoadSegmentId& Segment : Chain.Segments)
	{
		WriteTaxiwayId(Segment, Named);
	}
	if (bInherited)
	{
		RejudgeTaxiway(Named, Rules);
	}
	return Named;
}

void URoadNetwork::RejudgeTaxiway(int32 TaxiwayId, const FTaxiwayNamingRules& Rules)
{
	// "AS DRAWN SO FAR" (plan D3): a gesture is many commits, one per click, so a taxiway that just grew is judged again
	// by the same table, on its whole chain - but only an AUTO-named one, and only across the letter/connector line.
	const FTaxiway* Judged = GetTaxiway(TaxiwayId);
	if (Judged == nullptr || Judged->bPlayerNamed)
	{
		return;
	}
	const FTaxiwayChain Whole = TaxiwayChainOf(TaxiwayId);
	const FString Was = TaxiwayDisplayName(TaxiwayId);
	if (!Judged->IsConnector())
	{
		// A LETTER WITH CONNECTORS STAYS ONE: its connectors' names derive from it.
		const int32 Parent = HasTaxiwayConnectors(TaxiwayId) ? INDEX_NONE
			: TaxiwayNamesConnectorParent(*this, Whole, Rules, TaxiwayId);
		if (Parent == INDEX_NONE)
		{
			return;
		}
		const int32 Number = IssueConnectorNumber(Parent);
		FTaxiway& Becomes = Taxiways[TaxiwayId];
		Becomes.Name.Reset();
		Becomes.ParentId = Parent;
		Becomes.ConnectorNumber = Number;
	}
	else if (Whole.Length >= Rules.ConnectorMaxLength)
	{
		const FString Letter = NextFreeTaxiwayLetter(TaxiwayId);
		FTaxiway& Becomes = Taxiways[TaxiwayId];
		Becomes.ParentId = INDEX_NONE;
		Becomes.ConnectorNumber = 0;
		Becomes.Name = Letter;
	}
	else
	{
		return;
	}
	UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s is now %s (as drawn so far)"), *Was, *TaxiwayDisplayName(TaxiwayId));
}

int32 URoadNetwork::AssignUnnamedTaxiways(const FTaxiwayNamingRules& Rules)
{
	const auto Unnamed = [this](FRoadSegmentId Id) { return TaxiwayStrip::HasStrip(*this, Id) && TaxiwayOf(Id) == INDEX_NONE; };
	TSet<FRoadSegmentId> Taken;
	TArray<FTaxiwayChain> Chains;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (!Id.IsSet() || Taken.Contains(Id) || !Unnamed(Id))
		{
			continue;
		}
		Chains.Add(TaxiwayNamesWalk(*this, Id, [this, &Rules, &Unnamed](FRoadNodeId Node, FRoadSegmentId From)
		{
			// THE STRAIGHTEST unnamed continuation; Incident's own order (bearing-sorted) settles an exact tie.
			FRoadSegmentId Best;
			double BestAngle = -1.0;
			if (const FRoadNode* At = GetNode(Node))
			{
				for (const FRoadSegmentId& Other : At->Incident)
				{
					double Angle = 0.0;
					if (Other != From && Unnamed(Other) && TaxiwayNamesCarriesOn(*this, Node, From, Other, Rules, Angle)
						&& Angle > BestAngle)
					{
						Best = Other;
						BestAngle = Angle;
					}
				}
			}
			return Best;
		}, Taken));
	}
	// LONGEST FIRST (spec, backfill): the long parallels take the early letters, and a connector is judged after the
	// taxiways it joins are named. Ties by lowest segment index, so the order never depends on a hash.
	Chains.Sort([](const FTaxiwayChain& L, const FTaxiwayChain& R)
	{
		return L.Length != R.Length ? L.Length > R.Length : L.LowestIndex < R.LowestIndex;
	});
	// TWO PASSES (PR #524 review). Longest-first alone named a CONNECTOR-SHAPED chain (short, both ends anchored) before
	// the chains at its ends: a link of two halves round a corner backfilled as C + C1, where drawing it from A gave A1,
	// A2. So everything else is named first, in that order; then each connector-shaped chain waits until no unnamed
	// taxiway is left at its FIRST end - the end AssignTaxiway takes its parent from, as a click from that end would.
	// Its last end may still be unnamed: anchored either way (TaxiwayNamesAnchored), and the parent is the first end's.
	// A ring of them all waiting takes the first in order, so every pass ends. Not a different rule from a click: a
	// click's normalise has one unnamed chain, which never waits on itself (the same AssignTaxiway, the same order).
	// ENFORCED BY: Airside.Model.TaxiwayNames.BackfillCorneredLinkIsConnector (backfill == click by click, name by name).
	const auto ConnectorShaped = [this, &Rules](const FTaxiwayChain& Chain)
	{
		return Chain.Length < Rules.ConnectorMaxLength && TaxiwayNamesAnchored(*this, Chain.First, Chain)
			&& TaxiwayNamesAnchored(*this, Chain.Last, Chain);
	};
	const auto FirstEndWaits = [this, &Unnamed](const FTaxiwayChain& Chain)
	{
		const FRoadNode* At = GetNode(Chain.First);
		return At != nullptr && At->Incident.ContainsByPredicate([&Chain, &Unnamed](const FRoadSegmentId& Other)
			{ return !Chain.Segments.Contains(Other) && Unnamed(Other); });
	};
	TArray<int32> Waiting;
	for (int32 Index = 0; Index < Chains.Num(); ++Index)
	{
		if (ConnectorShaped(Chains[Index]))
		{
			Waiting.Add(Index);
		}
		else
		{
			AssignTaxiway(Chains[Index], Rules);
		}
	}
	// A LINEAR RESCAN per pick, so quadratic in the connector-shaped chains: 7 on the Gatwick fixture, a few dozen on a
	// whole backfilled airport (2026-10-02), and a click's normalise has one.
	while (Waiting.Num() > 0)
	{
		const int32 Ready = Waiting.IndexOfByPredicate([&Chains, &FirstEndWaits](int32 Index) { return !FirstEndWaits(Chains[Index]); });
		const int32 Pick = Ready != INDEX_NONE ? Ready : 0;
		AssignTaxiway(Chains[Waiting[Pick]], Rules);
		Waiting.RemoveAt(Pick);
	}
	return Chains.Num();
}

TArray<FTaxiwayRename> URoadNetwork::NormaliseTaxiways(const FTaxiwayNamingRules& Rules)
{
	TArray<FTaxiwayRename> Renames;
	int32 Changes = 0;

	// 0. A NAME ONLY ON A LIVE TAXIWAY SEGMENT. A merge can keep the WIDER arm (MergeNodes' CASE 2) and an upgrade can
	// never change kind (SetSegmentProfile), but a load or a hand edit can still hand a road a stale id.
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (Id.IsSet() && Segments[Index].TaxiwayId != INDEX_NONE
			&& (!TaxiwayStrip::HasStrip(*this, Id) || GetTaxiway(Segments[Index].TaxiwayId) == nullptr))
		{
			WriteTaxiwayId(Id, INDEX_NONE);
			++Changes;
		}
	}

	// 1. EVERY NEW CHAIN NAMED - first, so a heal that rejoins two pieces of A inherits A before step 3 would split them.
	Changes += AssignUnnamedTaxiways(Rules);

	// 2-3. ONE CHAIN PER TAXIWAY. Indices, not a range-for: a split mints into Taxiways while this walks it, and a
	// split-off is a simple path that the same two steps then leave alone.
	for (int32 TaxiwayId = 0; TaxiwayId < Taxiways.Num(); ++TaxiwayId)
	{
		if (Taxiways[TaxiwayId].bAlive)
		{
			SplitTaxiwayBranches(TaxiwayId, Renames);
			SplitTaxiwayPieces(TaxiwayId, Renames);
		}
	}

	// 4. EMPTY - after the splits, so a taxiway emptied by step 1's inheritance is seen too.
	Changes += RetireEmptyTaxiways();

	Changes += Renames.Num();
	for (const FTaxiwayRename& Rename : Renames)
	{
		// A RE-PARENTED CONNECTOR logged itself, in its own words ("C1 was A2"); the event below is shared.
		if (!Rename.bReparented)
		{
			UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s split off from %s"), *Rename.SplitOff, *Rename.From);
		}
	}
	if (Changes > 0)
	{
		NoteFactChanged();
	}
	return Renames;
}

FTaxiwayChain URoadNetwork::TaxiwayChainOf(int32 TaxiwayId) const
{
	FRoadSegmentId Seed;
	FRoadSegmentId Lowest;
	for (int32 Index = 0; Index < Segments.Num() && !Seed.IsSet(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (!Id.IsSet() || TaxiwayOf(Id) != TaxiwayId)
		{
			continue;
		}
		if (!Lowest.IsSet())
		{
			Lowest = Id;
		}
		if (TaxiwayNamesOwnAt(*this, Segments[Index].A, TaxiwayId).Num() == 1
			|| TaxiwayNamesOwnAt(*this, Segments[Index].B, TaxiwayId).Num() == 1)
		{
			Seed = Id;
		}
	}
	if (!Seed.IsSet())
	{
		Seed = Lowest;
	}
	if (!Seed.IsSet())
	{
		return FTaxiwayChain();
	}
	TSet<FRoadSegmentId> Taken;
	return TaxiwayNamesWalk(*this, Seed, [this, TaxiwayId](FRoadNodeId Node, FRoadSegmentId From)
	{
		const TArray<FRoadSegmentId> Own = TaxiwayNamesOwnAt(*this, Node, TaxiwayId);
		return Own.Num() == 2 ? (Own[0] == From ? Own[1] : Own[0]) : FRoadSegmentId();
	}, Taken);
}

void URoadNetwork::SplitTaxiwayBranches(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames)
{
	// EACH ROUND takes one arm off one node, so it ends within the taxiway's segment count; the guard only says so.
	for (int32 Guard = 0; Guard <= Segments.Num(); ++Guard)
	{
		FRoadNodeId At;
		TArray<FRoadSegmentId> Arms;
		for (int32 Index = 0; Index < Nodes.Num() && !At.IsSet(); ++Index)
		{
			const FRoadNodeId Node = NodeIdAt(Index);
			TArray<FRoadSegmentId> Own = Node.IsSet() ? TaxiwayNamesOwnAt(*this, Node, TaxiwayId) : TArray<FRoadSegmentId>();
			if (Own.Num() >= 3)
			{
				At = Node;
				Arms = MoveTemp(Own);
			}
		}
		if (!At.IsSet())
		{
			return;
		}
		TArray<FRoadSegmentId> Shortest;
		double ShortestLength = TNumericLimits<double>::Max();
		int32 ShortestFirst = MAX_int32;
		for (const FRoadSegmentId& Arm : Arms)
		{
			// THE BRANCH: from At along Arm, through every node that is a plain pass-through of this taxiway, stopping at
			// a fork, an end, or back at At (a loop's two arms walk the same loop).
			TArray<FRoadSegmentId> Branch;
			double Length = 0.0;
			FRoadSegmentId Through = Arm;
			FRoadNodeId Node = At;
			while (Through.IsSet() && !Branch.Contains(Through))
			{
				Branch.Add(Through);
				Length += TaxiwayNamesLength(*this, Through);
				Node = GetOtherEnd(Through, Node);
				if (Node == At)
				{
					break;
				}
				const TArray<FRoadSegmentId> Own = TaxiwayNamesOwnAt(*this, Node, TaxiwayId);
				Through = Own.Num() == 2 ? (Own[0] == Through ? Own[1] : Own[0]) : FRoadSegmentId();
			}
			if (Length < ShortestLength || (Length == ShortestLength && Arm.Index < ShortestFirst))
			{
				Shortest = MoveTemp(Branch);
				ShortestLength = Length;
				ShortestFirst = Arm.Index;
			}
		}
		const int32 Fresh = MintTaxiway(INDEX_NONE);
		for (const FRoadSegmentId& Segment : Shortest)
		{
			WriteTaxiwayId(Segment, Fresh);
		}
		OutRenames.Add({ TaxiwayDisplayName(Fresh), TaxiwayDisplayName(TaxiwayId) });
		ReparentSplitConnectors(TaxiwayId, Fresh, OutRenames);
	}
}

void URoadNetwork::SplitTaxiwayPieces(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames)
{
	TArray<FTaxiwayChain> Pieces;
	TSet<FRoadSegmentId> Seen;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Seed = SegmentIdAt(Index);
		if (!Seed.IsSet() || Seen.Contains(Seed) || TaxiwayOf(Seed) != TaxiwayId)
		{
			continue;
		}
		FTaxiwayChain& Piece = Pieces.AddDefaulted_GetRef();
		TArray<FRoadSegmentId> Frontier = { Seed };
		Seen.Add(Seed);
		while (Frontier.Num() > 0)
		{
			const FRoadSegmentId Each = Frontier.Pop();
			Piece.Segments.Add(Each);
			Piece.Length += TaxiwayNamesLength(*this, Each);
			Piece.LowestIndex = FMath::Min(Piece.LowestIndex, Each.Index);
			const FRoadSegment* Segment = GetSegment(Each);
			for (const FRoadNodeId& End : { Segment->A, Segment->B })
			{
				for (const FRoadSegmentId& Next : TaxiwayNamesOwnAt(*this, End, TaxiwayId))
				{
					if (!Seen.Contains(Next))
					{
						Seen.Add(Next);
						Frontier.Add(Next);
					}
				}
			}
		}
	}
	if (Pieces.Num() <= 1)
	{
		return;
	}
	Pieces.Sort([](const FTaxiwayChain& L, const FTaxiwayChain& R)
	{
		return L.Length != R.Length ? L.Length > R.Length : L.LowestIndex < R.LowestIndex;
	});
	for (int32 Piece = 1; Piece < Pieces.Num(); ++Piece)
	{
		const int32 Fresh = MintTaxiway(INDEX_NONE);
		for (const FRoadSegmentId& Segment : Pieces[Piece].Segments)
		{
			WriteTaxiwayId(Segment, Fresh);
		}
		OutRenames.Add({ TaxiwayDisplayName(Fresh), TaxiwayDisplayName(TaxiwayId) });
		ReparentSplitConnectors(TaxiwayId, Fresh, OutRenames);
	}
}

void URoadNetwork::ReparentSplitConnectors(int32 TaxiwayId, int32 Fresh, TArray<FTaxiwayRename>& OutRenames)
{
	// OWNER RULING 2026-10-02 (PR #524): a connector of TaxiwayId that touches none of its segments now and some of
	// Fresh's goes with Fresh - "A2" hanging off a piece that reads "C" would send a player looking for it on A. One that
	// still touches TaxiwayId stays: it is still A's. "Touches" is ANY end of ANY of its segments, not its chain's two
	// ends: a connector later in Taxiways is not split yet, and may be in pieces itself at this point of the pass.
	// A LINEAR SCAN of every segment per connector, per split: a split is one player edit's worth, and under 200 taxiways
	// and a few thousand segments on a 40-stand airport (2026-10-02 estimate).
	const auto Touches = [this](int32 Connector, int32 Owner)
	{
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = SegmentIdAt(Index);
			if (Id.IsSet() && TaxiwayOf(Id) == Connector
				&& (TaxiwayNamesOwnAt(*this, Segments[Index].A, Owner).Num() > 0
					|| TaxiwayNamesOwnAt(*this, Segments[Index].B, Owner).Num() > 0))
			{
				return true;
			}
		}
		return false;
	};
	for (int32 Connector = 0; Connector < Taxiways.Num(); ++Connector)
	{
		if (!Taxiways[Connector].bAlive || Taxiways[Connector].ParentId != TaxiwayId || Touches(Connector, TaxiwayId)
			|| !Touches(Connector, Fresh))
		{
			continue;
		}
		const FString Was = TaxiwayDisplayName(Connector);
		// A NUMBER FROM THE NEW PARENT even under a player's override: the override keeps its text (the ruling), and the
		// number is what it reads if the override is ever cleared - A's old number could collide with one C issues later.
		const int32 Number = IssueConnectorNumber(Fresh);
		FTaxiway& Moved = Taxiways[Connector];
		Moved.ParentId = Fresh;
		Moved.ConnectorNumber = Number;
		const FString Now = TaxiwayDisplayName(Connector);
		UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s was %s - re-parented onto %s"), *Now, *Was, *TaxiwayDisplayName(Fresh));
		// AN OVERRIDE READS THE SAME, so there is nothing to tell the player.
		if (Now != Was)
		{
			OutRenames.Add({ Now, Was, true });
		}
	}
}

FString URoadNetwork::WhyTaxiwayNameRefused(int32 TaxiwayId, const FString& Requested) const
{
	const FTaxiway* Taxiway = GetTaxiway(TaxiwayId);
	if (Taxiway == nullptr)
	{
		return TEXT("That taxiway is gone");
	}
	const FString Name = Requested.TrimStartAndEnd().ToUpper();
	if (Name.Len() < 1 || Name.Len() > 3)
	{
		return TEXT("A taxiway name is 1 to 3 letters or digits");
	}
	for (const TCHAR Character : Name)
	{
		if (!FChar::IsAlnum(Character))
		{
			return TEXT("A taxiway name is 1 to 3 letters or digits");
		}
		if (TaxiwayLetters::IsAvoided(Character))
		{
			return TEXT("I, O and X are avoided: they read as 1, 0 and closed");
		}
	}
	if (Name.Equals(TaxiwayDisplayName(TaxiwayId), ESearchCase::IgnoreCase))
	{
		return FString();
	}
	if (IsTaxiwayNameTaken(Name, TaxiwayId))
	{
		return FString::Printf(TEXT("%s is taken"), *Name);
	}
	// A LETTERED TAXIWAY'S RENAME RENAMES EVERY CONNECTOR THAT DERIVES FROM IT (spec: "renaming A to K makes A1..A4 read
	// K1..K4") - so each derived name must be free too, or the rename would show one name twice.
	if (!Taxiway->IsConnector())
	{
		for (const FTaxiway& Each : Taxiways)
		{
			if (Each.bAlive && Each.ParentId == TaxiwayId && Each.Name.IsEmpty())
			{
				const FString Derived = Name + FString::FromInt(Each.ConnectorNumber);
				if (IsTaxiwayNameTaken(Derived, Each.Id))
				{
					return FString::Printf(TEXT("%s is taken"), *Derived);
				}
			}
		}
	}
	return FString();
}

bool URoadNetwork::RenameTaxiway(int32 TaxiwayId, const FString& Requested)
{
	FTaxiway* Taxiway = FindTaxiwayMutable(TaxiwayId);
	if (Taxiway == nullptr || !WhyTaxiwayNameRefused(TaxiwayId, Requested).IsEmpty())
	{
		return false;
	}
	Taxiway->Name = Requested.TrimStartAndEnd().ToUpper();
	Taxiway->bPlayerNamed = true;
	NoteFactChanged();
	return true;
}
