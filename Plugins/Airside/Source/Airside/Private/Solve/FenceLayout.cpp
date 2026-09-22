#include "Solve/FenceLayout.h"

#include "Solve/RoadGeom.h"

namespace
{
	/** Closer than this, two consecutive vertices are one repeated point rather than an edge. */
	constexpr double FenceDegenerateEdgeUu = 1.0;

	/**
	 * A mitre further than this many offsets from its corner is a spike, not a corner - a
	 * near-hairpin angle. The span ends at its own offset point instead; the crack that leaves
	 * is at a corner no plot the facade accepts can have.
	 */
	constexpr double FenceMitreLimit = 10.0;

	/** One outline edge, counter-clockwise. */
	struct FFenceEdge
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D(1.0, 0.0);

		/** Right of travel: OUTSIDE for a counter-clockwise ring. */
		FVector2D Out = FVector2D(0.0, -1.0);

		double Length = 0.0;
	};

	/** A stretch of one edge between two fixed posts. */
	struct FFenceRun
	{
		double From = 0.0;
		double To = 0.0;

		/** The run starts at the edge's first vertex, so its first bay mitres into the edge before. */
		bool bFromCorner = false;

		/** The run ends at the edge's last vertex, so its last bay mitres into the edge after. */
		bool bToCorner = false;
	};

	double FenceHeading(const FVector2D& V)
	{
		return FMath::Atan2(V.Y, V.X);
	}

	/**
	 * Where In's offset line meets Next's - the corner the fabric turns at.
	 *
	 * ONE FUNCTION, CALLED WITH THE SAME ARGUMENTS BY BOTH SIDES: the last bay of edge i and the
	 * first bay of edge i+1 each ask for FenceMitre(Edges[i], Edges[i+1]), so the two endpoints
	 * are the same bits and the fabric has no crack at the corner.
	 */
	FVector2D FenceMitre(const FFenceEdge& In, const FFenceEdge& Next, double Offset)
	{
		const FVector2D Fallback = Next.A + Next.Out * Offset;

		FRay2D RayIn;
		RayIn.Origin = In.A + In.Out * Offset;
		RayIn.Dir = In.Dir;
		FRay2D RayNext;
		RayNext.Origin = Fallback;
		RayNext.Dir = Next.Dir;

		// HONOURS THE RETURN: LineIntersect leaves Hit unwritten when the lines are parallel
		// (a collinear vertex), and the fallback IS the right answer there.
		FVector2D Hit = Fallback;
		if (!RoadGeom::LineIntersect(RayIn, RayNext, Hit)
			|| FVector2D::Distance(Hit, Next.A) > FenceMitreLimit * Offset)
		{
			return Fallback;
		}
		return Hit;
	}
}

FenceLayout::FLayout FenceLayout::Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
	const FSpec& Spec)
{
	FLayout Layout;
	if (Spec.SpacingUu <= 0.0 || Spec.TileUu <= 0.0)
	{
		return Layout;
	}

	// DISTINCT VERTICES, COUNTER-CLOCKWISE. Read off the signed area rather than assumed, so
	// "outside" is right-of-travel whichever way the plot was stored.
	TArray<FVector2D> Ring;
	for (const FVector2D& Point : Outline)
	{
		if (Ring.Num() == 0 || FVector2D::Distance(Ring.Last(), Point) >= FenceDegenerateEdgeUu)
		{
			Ring.Add(Point);
		}
	}
	if (Ring.Num() >= 2 && FVector2D::Distance(Ring.Last(), Ring[0]) < FenceDegenerateEdgeUu)
	{
		Ring.Pop();
	}
	if (Ring.Num() < 3)
	{
		return Layout;
	}
	if (RoadGeom::PolygonArea(Ring) < 0.0)
	{
		// Reversed by hand: Solve/ takes nothing beyond CoreMinimal, and Algo/Reverse is not in it.
		TArray<FVector2D> Reversed;
		Reversed.Reserve(Ring.Num());
		for (int32 Index = Ring.Num() - 1; Index >= 0; --Index)
		{
			Reversed.Add(Ring[Index]);
		}
		Ring = MoveTemp(Reversed);
	}

	const int32 N = Ring.Num();
	TArray<FFenceEdge> Edges;
	Edges.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		FFenceEdge& Edge = Edges[I];
		Edge.A = Ring[I];
		const FVector2D Along = Ring[(I + 1) % N] - Ring[I];
		Edge.Length = Along.Size();
		Edge.Dir = Along / Edge.Length;
		Edge.Out = FVector2D(Edge.Dir.Y, -Edge.Dir.X);
	}

	// THE GATE'S EDGE is the one nearest the requested gate; the gate's station is its
	// projection onto that edge.
	int32 GateEdge = INDEX_NONE;
	double GateAt = 0.0;
	if (Spec.GateWidthUu > 0.0)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 I = 0; I < N; ++I)
		{
			const double Along = FMath::Clamp(
				FVector2D::DotProduct(Gate - Edges[I].A, Edges[I].Dir), 0.0, Edges[I].Length);
			const double Distance = FVector2D::Distance(Edges[I].A + Edges[I].Dir * Along, Gate);
			if (Distance < Best)
			{
				Best = Distance;
				GateEdge = I;
				GateAt = Along;
			}
		}

		// A FULL BAY EITHER SIDE, or no gate at all - see bHasGate.
		const double Half = Spec.GateWidthUu * 0.5;
		const double Margin = Spec.SpacingUu + Half;
		if (Edges[GateEdge].Length >= 2.0 * Margin)
		{
			GateAt = FMath::Clamp(GateAt, Margin, Edges[GateEdge].Length - Margin);
			Layout.bHasGate = true;
			Layout.GateCentre = Edges[GateEdge].A + Edges[GateEdge].Dir * GateAt;
		}
		else
		{
			GateEdge = INDEX_NONE;
		}
	}

	for (int32 I = 0; I < N; ++I)
	{
		const FFenceEdge& Edge = Edges[I];
		const FFenceEdge& Before = Edges[(I + N - 1) % N];
		const FFenceEdge& After = Edges[(I + 1) % N];

		// THE CORNER POST at this edge's first vertex, facing out along the bisector. A hairpin
		// (outward normals cancelling) has no bisector; it faces along the edge instead.
		FPost Corner;
		Corner.Position = Edge.A;
		Corner.Kind = EPostKind::Corner;
		const FVector2D Bisector = Before.Out + Edge.Out;
		Corner.YawRad = Bisector.SizeSquared() > UE_DOUBLE_SMALL_NUMBER
			? FenceHeading(Bisector) : FenceHeading(Edge.Dir);
		Layout.Posts.Add(Corner);

		TArray<FFenceRun, TInlineAllocator<2>> Runs;
		if (I == GateEdge)
		{
			const double Half = Spec.GateWidthUu * 0.5;
			Runs.Add({ 0.0, GateAt - Half, true, false });
			Runs.Add({ GateAt + Half, Edge.Length, false, true });
			for (const double At : { GateAt - Half, GateAt + Half })
			{
				FPost GatePost;
				GatePost.Position = Edge.A + Edge.Dir * At;
				GatePost.YawRad = FenceHeading(Edge.Dir);
				GatePost.Kind = EPostKind::Gate;
				Layout.Posts.Add(GatePost);
			}
		}
		else
		{
			Runs.Add({ 0.0, Edge.Length, true, true });
		}

		for (const FFenceRun& Run : Runs)
		{
			const double RunLength = Run.To - Run.From;
			const int32 Bays = FMath::Max(1, FMath::RoundToInt(RunLength / Spec.SpacingUu));
			const double Pitch = RunLength / Bays;

			for (int32 Bay = 0; Bay < Bays; ++Bay)
			{
				const double From = Run.From + Pitch * Bay;
				// THE LAST BAY ENDS ON Run.To ITSELF, not on From + Pitch, so rounding in the
				// division cannot leave the final post a hair short of the fixed one.
				const double To = Bay == Bays - 1 ? Run.To : Run.From + Pitch * (Bay + 1);

				if (Bay > 0)
				{
					FPost Line;
					Line.Position = Edge.A + Edge.Dir * From;
					Line.YawRad = FenceHeading(Edge.Dir);
					Line.Kind = EPostKind::Line;
					Layout.Posts.Add(Line);
				}

				FSpan Span;
				Span.A = Bay == 0 && Run.bFromCorner
					? FenceMitre(Before, Edge, Spec.FaceOffsetUu)
					: Edge.A + Edge.Dir * From + Edge.Out * Spec.FaceOffsetUu;
				Span.B = Bay == Bays - 1 && Run.bToCorner
					? FenceMitre(Edge, After, Spec.FaceOffsetUu)
					: Edge.A + Edge.Dir * To + Edge.Out * Spec.FaceOffsetUu;
				Span.U0 = From / Spec.TileUu;
				Span.U1 = To / Spec.TileUu;
				Layout.Spans.Add(Span);
			}
		}
	}

	return Layout;
}


int32 FenceLayout::FLayout::CountOf(EPostKind Kind) const
{
	int32 Count = 0;
	for (const FPost& Post : Posts)
	{
		Count += Post.Kind == Kind ? 1 : 0;
	}
	return Count;
}
