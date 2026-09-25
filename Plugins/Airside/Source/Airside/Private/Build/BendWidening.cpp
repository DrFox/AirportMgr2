#include "Build/BendWidening.h"

#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Solve/VehicleSweep.h"

namespace
{
	/**
	 * A two-arm bend's inner edge as ONE line, u uu along it: back up arm K's inner (left) edge
	 * for u < 0, round the fillet for 0 <= u <= Radius * Sweep, and on up arm K1's inner (right)
	 * edge beyond. Depth is how far a point lies past that edge, into the grass.
	 */
	struct FInnerEdge
	{
		int32 K = INDEX_NONE;
		int32 K1 = INDEX_NONE;
		FVector2D Node = FVector2D::ZeroVector;
		FVector2D Centre = FVector2D::ZeroVector;
		FVector2D RadialA = FVector2D::ZeroVector;
		double Radius = 0.0;
		double Sweep = 0.0;
		double Sign = 1.0;
		/** Arm K: its tangent, the normal into the grass beyond its inner edge, that edge's offset, and the fillet's tangent point along it. */
		FVector2D T0 = FVector2D::ZeroVector;
		FVector2D G0 = FVector2D::ZeroVector;
		double Half0 = 0.0;
		double AlongA = 0.0;
		/** And arm K1's. */
		FVector2D T1 = FVector2D::ZeroVector;
		FVector2D G1 = FVector2D::ZeroVector;
		double Half1 = 0.0;
		double AlongB = 0.0;

		double ArcLength() const { return Radius * Sweep; }

		void Locate(const FVector2D& P, double& OutU, double& OutDepth) const
		{
			const FVector2D D = P - Centre;
			const double Phi = Sign * FMath::Atan2(FVector2D::CrossProduct(RadialA, D), FVector2D::DotProduct(RadialA, D));
			if (Phi < 0.0)
			{
				OutU = AlongA - FVector2D::DotProduct(P - Node, T0);
				OutDepth = FVector2D::DotProduct(P - Node, G0) - Half0;
			}
			else if (Phi > Sweep)
			{
				OutU = ArcLength() + FVector2D::DotProduct(P - Node, T1) - AlongB;
				OutDepth = FVector2D::DotProduct(P - Node, G1) - Half1;
			}
			else
			{
				OutU = Radius * Phi;
				OutDepth = Radius - D.Size();
			}
		}

		/** The edge at U, pushed W into the grass. */
		FVector2D At(double U, double W) const
		{
			if (U < 0.0)
			{
				return Node + T0 * (AlongA - U) + G0 * (Half0 + W);
			}
			if (U > ArcLength())
			{
				return Node + T1 * (AlongB + U - ArcLength()) + G1 * (Half1 + W);
			}
			const double Phi = U / Radius;
			const FVector2D Dir = RadialA * FMath::Cos(Phi) + RoadGeom::PerpCCW(RadialA) * (Sign * FMath::Sin(Phi));
			return Centre + Dir * (Radius - W);
		}
	};

	/** The inner edge of a solved two-arm bend, or false when it has no rounded inner corner. */
	bool InnerEdgeOf(const FJunctionInput& Input, const FJunctionResult& Result, FInnerEdge& Out)
	{
		if (!Result.bValid || Input.Arms.Num() != 2 || Result.Corners.Num() != 2 || Result.Arms.Num() != 2
			|| Input.Arms[0].bContinuous || Input.Arms[1].bContinuous)
		{
			return false;
		}
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const RoadGeom::FFillet& Corner = Result.Corners[Index];
			if (Corner.bValid && !Corner.bStraightThrough && Corner.Theta < UE_DOUBLE_PI && Corner.Radius > 0.0)
			{
				Out.K = Index;
			}
		}
		if (Out.K == INDEX_NONE)
		{
			return false;
		}
		Out.K1 = (Out.K + 1) % 2;
		const RoadGeom::FFillet& Fillet = Result.Corners[Out.K];
		Out.Node = Input.Position;
		Out.Centre = Fillet.Centre;
		Out.Radius = Fillet.Radius;
		// Corner K runs from arm K's LEFT edge to arm K1's RIGHT edge (FJunctionResult::Corners).
		Out.T0 = Input.Arms[Out.K].Tangent;
		Out.G0 = RoadGeom::PerpCCW(Out.T0);
		Out.Half0 = FMath::Max(Input.Arms[Out.K].HalfWidthLeft, 0.0);
		Out.AlongA = FVector2D::DotProduct(Fillet.TangentA - Out.Node, Out.T0);
		Out.T1 = Input.Arms[Out.K1].Tangent;
		Out.G1 = -RoadGeom::PerpCCW(Out.T1);
		Out.Half1 = FMath::Max(Input.Arms[Out.K1].HalfWidthRight, 0.0);
		Out.AlongB = FVector2D::DotProduct(Fillet.TangentB - Out.Node, Out.T1);
		Out.RadialA = (Fillet.TangentA - Fillet.Centre) / Fillet.Radius;
		const FVector2D RadialB = (Fillet.TangentB - Fillet.Centre) / Fillet.Radius;
		const double Cross = FVector2D::CrossProduct(Out.RadialA, RadialB);
		Out.Sign = Cross >= 0.0 ? 1.0 : -1.0;
		Out.Sweep = FMath::Atan2(FMath::Abs(Cross), FVector2D::DotProduct(Out.RadialA, RadialB));
		return true;
	}

	/** The envelope W at U, linear between bin centres and nothing beyond them. */
	double WidthAt(const BendWidening::FWidening& Widening, double U)
	{
		const TArray<double>& Us = Widening.U;
		if (Us.Num() == 0 || U < Us[0] - BendWidening::Bin || U > Us.Last() + BendWidening::Bin)
		{
			return 0.0;
		}
		for (int32 I = 0; I + 1 < Us.Num(); ++I)
		{
			if (U >= Us[I] && U <= Us[I + 1])
			{
				const double T = (U - Us[I]) / FMath::Max(Us[I + 1] - Us[I], 1e-9);
				return FMath::Lerp(Widening.W[I], Widening.W[I + 1], T);
			}
		}
		// Within a bin of either end: lean to nothing across it.
		return U < Us[0] ? Widening.W[0] * (1.0 - (Us[0] - U) / BendWidening::Bin)
			: Widening.W.Last() * (1.0 - (U - Us.Last()) / BendWidening::Bin);
	}
}

bool BendWidening::Measure(const FJunctionInput& Input, const FJunctionResult& Result, const TArray<FLane> (&Lanes)[2],
	const FVehicle& Body, FWidening& Out)
{
	Out = FWidening();
	FInnerEdge Edge;
	if (Body.BodyWidth <= 0.0 || !InnerEdgeOf(Input, Result, Edge))
	{
		return false;
	}
	const VehicleSweep::FBody Swept = VehicleFit::BodyOf(Body);
	// Far enough up the leaving lane for the tow to settle back onto its line: the train's own
	// length twice over (a tractrix has converged to within a few percent by then), and Drive
	// then adds its own lead-out beyond that.
	double Train = Swept.Wheelbase;
	for (const VehicleSweep::FLink& Link : Swept.Tow)
	{
		Train += FMath::Abs(Link.HitchX) + Link.Length;
	}
	const double LaneOut = 2.0 * Train;

	TMap<int32, double> Needed;
	double Deepest = -TNumericLimits<double>::Max();
	for (int32 From = 0; From < 2; ++From)
	{
		const int32 To = 1 - From;
		const FJunctionArm& ArriveArm = Input.Arms[From];
		const FJunctionArm& LeaveArm = Input.Arms[To];
		for (const FLane& In : Lanes[From])
		{
			for (const FLane& Out2 : Lanes[To])
			{
				if (!In.bArrives || !Out2.bLeaves)
				{
					continue;
				}
				// A lane end sits on its arm's cut line, the line the builder lays it from.
				const FVector2D PA = Edge.Node + ArriveArm.Tangent * Result.Arms[From].CutDistance
					+ RoadGeom::PerpCCW(ArriveArm.Tangent) * In.Lateral;
				const FVector2D PB = Edge.Node + LeaveArm.Tangent * Result.Arms[To].CutDistance
					+ RoadGeom::PerpCCW(LeaveArm.Tangent) * Out2.Lateral;
				TArray<GuidelineGeom::FArcPiece> Pieces;
				if (!GuidelineGeom::BendLane(PA, -ArriveArm.Tangent, PB, LeaveArm.Tangent, Edge.Centre, Pieces))
				{
					continue;
				}
				// NOT DRIVEN WHERE IT CANNOT WIDEN (review of 75d3cbc0): VehicleSweep::Envelope is the
				// STEADY-STATE reach toward the centre, which a 90 degree turn never settles into - an
				// upper bound on what the drive would find. Held at the arc's tightest (a piece of
				// BendArcPieceSweep delivers cos of half of it), a body that still keeps Margin off the
				// inner edge needs nothing here: the bowser on every tier, measured 2026-09-25.
				const double Off = From == Edge.K ? Edge.Half0 - In.Lateral : In.Lateral + Edge.Half1;
				const VehicleSweep::FEnvelope Steady = VehicleSweep::Envelope(Swept,
					(Edge.Radius + Off) * FMath::Cos(0.5 * GuidelineGeom::BendArcPieceSweep));
				if (Steady.bHolds && Steady.Inner + Margin <= Off)
				{
					continue;
				}
				++Out.Drives;
				// The driven line: a lane point behind PA (so Drive's lead-in runs down the lane, not
				// along the arc's first chord), the turn as GuidelineGeom samples it, then the leaving lane.
				TArray<FVector2D> Path;
				Path.Add(PA + ArriveArm.Tangent * 100.0);
				Path.Add(PA);
				FVector2D Prev = PA;
				for (const GuidelineGeom::FArcPiece& Piece : Pieces)
				{
					TArray<FVector2D> Samples;
					GuidelineGeom::Sample(Prev, Piece.Control, Piece.End, Samples);
					Path.Append(Samples.GetData() + 1, Samples.Num() - 1);
					Prev = Piece.End;
				}
				for (double Along = 100.0; Along <= LaneOut; Along += 100.0)
				{
					Path.Add(PB + LeaveArm.Tangent * Along);
				}
				// Held until the drive finishes: a turn that folds the tow is not paved for - the
				// router refuses it (VehicleFit), and a folding trailer's corners are nowhere a road goes.
				TMap<int32, double> Turn;
				double TurnDeepest = -TNumericLimits<double>::Max();
				const bool bDriven = VehicleSweep::Drive(Swept, Path, [&](const VehicleSweep::FCorners& Corners)
				{
					for (const FVector2D& Corner : Corners)
					{
						double U = 0.0, Depth = 0.0;
						Edge.Locate(Corner, U, Depth);
						TurnDeepest = FMath::Max(TurnDeepest, Depth);
						if (Depth > -Margin)
						{
							double& Slot = Turn.FindOrAdd(FMath::FloorToInt32(U / Bin), 0.0);
							Slot = FMath::Max(Slot, Depth + Margin);
						}
					}
				});
				if (!bDriven)
				{
					continue;
				}
				Deepest = FMath::Max(Deepest, TurnDeepest);
				for (const TPair<int32, double>& Pair : Turn)
				{
					double& Slot = Needed.FindOrAdd(Pair.Key, 0.0);
					Slot = FMath::Max(Slot, Pair.Value);
				}
			}
		}
	}
	if (Needed.Num() == 0)
	{
		return false;
	}

	// THE ENVELOPE, LEANED: each bin's need, less Taper per uu away from it, at its worst.
	int32 Lo = TNumericLimits<int32>::Max(), Hi = TNumericLimits<int32>::Lowest();
	double Most = 0.0;
	for (const TPair<int32, double>& Pair : Needed)
	{
		Lo = FMath::Min(Lo, Pair.Key);
		Hi = FMath::Max(Hi, Pair.Key);
		Most = FMath::Max(Most, Pair.Value);
	}
	const int32 Reach = FMath::CeilToInt32(Most / (Taper * Bin)) + 1;
	for (int32 Key = Lo - Reach; Key <= Hi + Reach; ++Key)
	{
		double W = 0.0;
		for (const TPair<int32, double>& Pair : Needed)
		{
			W = FMath::Max(W, Pair.Value - FMath::Abs(Key - Pair.Key) * Bin * Taper);
		}
		if (W > 0.0)
		{
			Out.U.Add((Key + 0.5) * Bin);
			Out.W.Add(W);
		}
	}
	if (Out.U.Num() == 0)
	{
		return false;
	}
	Out.Corner = Edge.K;
	Out.Deepest = Deepest;
	// The arms must reach past the envelope's ends, a bin of lean beyond its last centre.
	const double First = Out.U[0] - Bin;
	const double Last = Out.U.Last() + Bin;
	Out.NeededCut[Edge.K] = First < 0.0 ? Edge.AlongA - First : 0.0;
	Out.NeededCut[Edge.K1] = Last > Edge.ArcLength() ? Edge.AlongB + (Last - Edge.ArcLength()) : 0.0;
	return true;
}
