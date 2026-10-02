#include "Model/TaxiwayLabels.h"

#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

TArray<FTaxiwayLabel> TaxiwayLabels::Anchors(const URoadNetwork& Network, double RepeatEvery)
{
	TArray<FTaxiwayLabel> Out;
	// FLOORED at 10 m: FTaxiwayNamingRules::LabelRepeatDistance clamps at 1000 uu in the details panel, but a value set from
	// code or an old save does not pass the clamp, and a zero step would loop forever below.
	const double Step = FMath::Max(RepeatEvery, 1000.0);
	for (const FTaxiway& Taxiway : Network.GetTaxiways())
	{
		if (!Taxiway.bAlive)
		{
			continue;
		}
		const FTaxiwayChain Chain = Network.TaxiwayChainOf(Taxiway.Id);
		if (Chain.Segments.Num() == 0)
		{
			continue;   // an empty parent kept for its connectors: nothing to stand on
		}
		// EACH SEGMENT IN CHAIN DIRECTION: its start node, its length, where it starts along the chain.
		struct FPiece { FVector2D From; FVector2D Control; FVector2D To; double Start = 0.0; double Length = 0.0; };
		TArray<FPiece> Pieces;
		FRoadNodeId Node = Chain.First;
		double Along = 0.0;
		int32 Longest = 0;
		for (const FRoadSegmentId& Id : Chain.Segments)
		{
			const FRoadSegment* Segment = Network.GetSegment(Id);
			const FRoadNodeId Next = Network.GetOtherEnd(Id, Node);
			const FRoadNode* A = Network.GetNode(Node);
			const FRoadNode* B = Network.GetNode(Next);
			if (Segment == nullptr || A == nullptr || B == nullptr)
			{
				break;
			}
			FPiece& Piece = Pieces.AddDefaulted_GetRef();
			Piece.From = A->Position;
			Piece.Control = Segment->Control;
			Piece.To = B->Position;
			Piece.Start = Along;
			Piece.Length = GuidelineGeom::Length(Piece.From, Piece.Control, Piece.To);
			Along += Piece.Length;
			// LONGER BY MORE THAN 1 uu, not merely longer: equal segments sampled at different positions differ in the last
			// bits of their polyline sums, and a tie must go to the first in chain order (the spec's "stable" placement),
			// not to whichever rounding came out larger.
			Longest = Piece.Length > Pieces[Longest].Length + 1.0 ? Pieces.Num() - 1 : Longest;
			Node = Next;
		}
		if (Pieces.Num() == 0)
		{
			continue;
		}
		const FString Name = Network.TaxiwayDisplayName(Taxiway.Id);
		const double Centre = Pieces[Longest].Start + Pieces[Longest].Length * 0.5;
		const int32 Before = FMath::FloorToInt(Centre / Step);
		const int32 After = FMath::FloorToInt((Along - Centre) / Step);
		for (int32 K = -Before; K <= After; ++K)
		{
			const double S = Centre + K * Step;
			for (const FPiece& Piece : Pieces)
			{
				if (S <= Piece.Start + Piece.Length || &Piece == &Pieces.Last())
				{
					// A CURVE PARAMETER, not an arc length - close enough for a tag, and GuidelineGeom's one evaluator.
					const double T = Piece.Length > 0.0 ? FMath::Clamp((S - Piece.Start) / Piece.Length, 0.0, 1.0) : 0.5;
					Out.Add({ Taxiway.Id, Name, GuidelineGeom::Eval(Piece.From, Piece.Control, Piece.To, T) });
					break;
				}
			}
		}
	}
	return Out;
}
