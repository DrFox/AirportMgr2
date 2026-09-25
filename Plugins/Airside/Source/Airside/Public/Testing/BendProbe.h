#pragma once

// TEST-ONLY, guarded like AirsideTestWorld.h: public here because AirsideTests (the per-tier
// bend fixture) and AirportMgr (the rig course) both measure bends with it, and neither may
// depend on the other's test code.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"
#include "Solve/VehicleSweep.h"

/**
 * WHAT A BEND HANDS A VEHICLE, measured (bend lanes, 2026-09-25): the pavement's two fillets,
 * each lane's turn path as the chain of derived edges it is, and how far a vehicle's body
 * leaves the tarmac driving it - traced by VehicleSweep::Trace, the router's own pursuit, with
 * every body corner put against the pavement polygons themselves rather than against the
 * per-sample clearances (which are marched along one normal and only exist on turn edges).
 */
namespace BendProbe
{
	/** One lane-to-lane turn: its pieces in travel order and their samples, joints once. */
	struct FTurnChain
	{
		FGuidelineNodeId From;
		FGuidelineNodeId To;
		TArray<FGuidelineEdgeId> Pieces;
		TArray<FVector2D> Path;
		TArray<double> ClearIn;
		TArray<double> ClearOut;
		/** The tightest piece's MinRadius. */
		double MinRadius = TNumericLimits<double>::Max();
		/** Width of the narrower piece: the lane. */
		double Width = 0.0;
		/**
		 * The path a vehicle drives through the turn: up to LaneRun of the arriving lane, the turn,
		 * up to LaneRun of the leaving lane, at most 100 uu apart. The lanes are the lead-in and
		 * lead-out, so Trace's own straight lead runs down the lane rather than along the turn's
		 * first chord (a few degrees off it, which put a 3 m lead a trailer's width off the road).
		 */
		TArray<FVector2D> Driven;
	};

	/** How much of each lane Driven includes, uu: longer than any train. */
	constexpr double LaneRun = 4000.0;

	/** The straight lane edge ending (or starting) at Node, and its far end. */
	inline bool LaneAt(const URoadNetwork& Net, FGuidelineNodeId Node, FVector2D& OutFar)
	{
		const FGuidelineNode* At = Net.GetGuidelineNode(Node);
		if (At == nullptr) { return false; }
		for (const FGuidelineEdgeId Id : At->Incident)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
			if (Edge != nullptr && Edge->bAlive && Edge->DerivedFrom.IsSet())
			{
				OutFar = Net.GetGuidelineNode(Edge->A == Node ? Edge->B : Edge->A)->Position;
				return true;
			}
		}
		return false;
	}

	/** Points from From toward To (exclusive of To), at most 100 uu apart. */
	inline void Walk(const FVector2D& From, const FVector2D& To, TArray<FVector2D>& Out)
	{
		const double Length = FVector2D::Distance(From, To);
		const FVector2D Dir = (To - From).GetSafeNormal();
		const int32 Count = FMath::Max(1, FMath::CeilToInt32(Length / 100.0));
		for (int32 I = 0; I < Count; ++I)
		{
			Out.Add(From + Dir * (Length * I / Count));
		}
	}

	inline bool IsTurnPiece(const FGuidelineEdge& Edge)
	{
		return Edge.bAlive && Edge.bDerived && !Edge.DerivedFrom.IsSet();
	}

	/** Every turn chain that starts at a lane end of Node's arms. */
	inline TArray<FTurnChain> TurnsAt(const URoadNetwork& Net, FRoadNodeId Node)
	{
		TArray<FTurnChain> Out;
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FGuidelineNode& Start = Nodes[Index];
			if (!Start.bAlive || !Start.Origin.IsSet()) { continue; }
			const FRoadSegment* Seg = Net.GetSegment(Start.Origin.Segment);
			if (Seg == nullptr || (Start.Origin.bEndA ? Seg->A : Seg->B) != Node) { continue; }
			const FGuidelineNodeId StartId = Net.GuidelineNodeIdAt(Index);
			for (const FGuidelineEdgeId FirstId : Start.Incident)
			{
				const FGuidelineEdge* First = Net.GetGuidelineEdge(FirstId);
				if (First == nullptr || !IsTurnPiece(*First) || First->A != StartId) { continue; }
				FTurnChain Chain;
				Chain.From = StartId;
				FGuidelineEdgeId PieceId = FirstId;
				for (int32 Guard = 0; Guard < 64 && PieceId.IsSet(); ++Guard)
				{
					const FGuidelineEdge* Piece = Net.GetGuidelineEdge(PieceId);
					const FVector2D A = Net.GetGuidelineNode(Piece->A)->Position;
					const FVector2D B = Net.GetGuidelineNode(Piece->B)->Position;
					TArray<FVector2D> Points;
					GuidelineGeom::Sample(A, Piece->Control, B, Points);
					const bool bJoint = Chain.Path.Num() > 0;
					for (int32 S = 0; S < Points.Num(); ++S)
					{
						const double In = Piece->ClearInnerAt.IsValidIndex(S) ? Piece->ClearInnerAt[S] : -1.0;
						const double Outer = Piece->ClearOuterAt.IsValidIndex(S) ? Piece->ClearOuterAt[S] : -1.0;
						if (S == 0 && bJoint)
						{
							Chain.ClearIn.Last() = FMath::Min(Chain.ClearIn.Last(), In);
							Chain.ClearOut.Last() = FMath::Min(Chain.ClearOut.Last(), Outer);
							continue;
						}
						Chain.Path.Add(Points[S]);
						Chain.ClearIn.Add(In);
						Chain.ClearOut.Add(Outer);
					}
					Chain.Pieces.Add(PieceId);
					Chain.MinRadius = FMath::Min(Chain.MinRadius, Piece->MinRadius);
					Chain.Width = Chain.Width > 0.0 ? FMath::Min(Chain.Width, Piece->Width) : Piece->Width;
					const FGuidelineNode* End = Net.GetGuidelineNode(Piece->B);
					Chain.To = Piece->B;
					PieceId = FGuidelineEdgeId();
					if (End == nullptr || End->Origin.IsSet()) { break; }
					for (const FGuidelineEdgeId NextId : End->Incident)
					{
						const FGuidelineEdge* Next = Net.GetGuidelineEdge(NextId);
						if (Next != nullptr && IsTurnPiece(*Next) && Next->A == Piece->B) { PieceId = NextId; break; }
					}
				}
				FVector2D InFar, OutFar;
				if (LaneAt(Net, Chain.From, InFar) && LaneAt(Net, Chain.To, OutFar))
				{
					const FVector2D TurnStart = Chain.Path[0];
					const double InLength = FMath::Min(FVector2D::Distance(InFar, TurnStart), LaneRun);
					Walk(TurnStart + (InFar - TurnStart).GetSafeNormal() * InLength, TurnStart, Chain.Driven);
					Chain.Driven.Append(Chain.Path);
					const double OutLength = FMath::Min(FVector2D::Distance(OutFar, Chain.Path.Last()), LaneRun);
					const FVector2D Dir = (OutFar - Chain.Path.Last()).GetSafeNormal();
					const int32 Count = FMath::Max(1, FMath::CeilToInt32(OutLength / 100.0));
					for (int32 I = 1; I <= Count; ++I)
					{
						Chain.Driven.Add(Chain.Path.Last() + Dir * (OutLength * I / Count));
					}
				}
				else
				{
					Chain.Driven = Chain.Path;
				}
				Out.Add(MoveTemp(Chain));
			}
		}
		return Out;
	}

	/** The tarmac at a node: its junction polygon and every arm's ribbon, as FRoadGuidelineBuilder measures clearances on. */
	struct FPavement
	{
		TArray<TArray<FVector2D>> Polygons;

		bool Contains(const FVector2D& P) const
		{
			for (const TArray<FVector2D>& Poly : Polygons)
			{
				if (Poly.Num() >= 3 && RoadGeom::PointInPolygon(Poly, P)) { return true; }
			}
			return false;
		}

		/** 0 on the tarmac, else how far off it. */
		double Outside(const FVector2D& P) const
		{
			if (Contains(P)) { return 0.0; }
			double Best = TNumericLimits<double>::Max();
			for (const TArray<FVector2D>& Poly : Polygons)
			{
				for (int32 I = 0; I < Poly.Num(); ++I)
				{
					const FVector2D A = Poly[I];
					const FVector2D AB = Poly[(I + 1) % Poly.Num()] - A;
					const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1e-12), 0.0, 1.0);
					Best = FMath::Min(Best, FVector2D::Distance(P, A + AB * T));
				}
			}
			return Best;
		}
	};

	inline FPavement PavementAt(const URoadNetwork& Net, const FRoadSolveResult& Solved, FRoadNodeId Node)
	{
		FPavement Out;
		if (const FJunctionResult* Junction = Solved.NodeResults.Find(Node.Index); Junction && Junction->Boundary.Num() > 3)
		{
			TArray<FVector2D> Rim = Junction->Boundary;
			Rim.Pop();
			Out.Polygons.Add(MoveTemp(Rim));
		}
		if (const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Node.Index))
		{
			for (const FRoadSegmentId& ArmId : *Arms)
			{
				if (const FRoadSegment* Arm = Net.GetSegment(ArmId))
				{
					Out.Polygons.Add({ Arm->LeftCutA, Arm->RightCutA, Arm->LeftCutB, Arm->RightCutB });
				}
			}
		}
		return Out;
	}

	/** Every junction polygon and every segment ribbon in the network: the whole tarmac. */
	inline FPavement PavementAll(const URoadNetwork& Net, const FRoadSolveResult& Solved)
	{
		FPavement Out;
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			if (Pair.Value.bValid && Pair.Value.Boundary.Num() > 3)
			{
				TArray<FVector2D> Rim = Pair.Value.Boundary;
				Rim.Pop();
				Out.Polygons.Add(MoveTemp(Rim));
			}
		}
		for (const FRoadSegment& Seg : Net.GetSegments())
		{
			if (Seg.bAlive && Seg.bSolvedA && Seg.bSolvedB)
			{
				Out.Polygons.Add({ Seg.LeftCutA, Seg.RightCutA, Seg.LeftCutB, Seg.RightCutB });
			}
		}
		return Out;
	}

	/** A 2-arm node's two fillets: the one inside the bend (Theta < pi) and the one outside. */
	inline bool Fillets(const FRoadSolveResult& Solved, FRoadNodeId Node, RoadGeom::FFillet& OutInner, RoadGeom::FFillet& OutOuter)
	{
		const FJunctionResult* Junction = Solved.NodeResults.Find(Node.Index);
		if (Junction == nullptr || Junction->Corners.Num() != 2) { return false; }
		for (const RoadGeom::FFillet& Corner : Junction->Corners)
		{
			if (!Corner.bValid || Corner.bStraightThrough) { return false; }
			(Corner.Theta < UE_DOUBLE_PI ? OutInner : OutOuter) = Corner;
		}
		return true;
	}

	/** How far a vehicle's body leaves the tarmac driving a turn chain, and where. */
	struct FOverrun
	{
		/** Deepest body point off the tarmac on the INSIDE of the bend, uu; 0 when none is. */
		double Inner = 0.0;
		/** And on the outside. */
		double Outer = 0.0;
		FVector2D InnerAt = FVector2D::ZeroVector;
		bool bTraced = false;
		/** Every body point off the tarmac on the inside, and how far off: where the pavement falls short. */
		TArray<FVector2D> InnerPoints;
		TArray<double> InnerDepths;
	};

	/**
	 * Traces Vehicle along Chain.Driven (the lanes either side are its lead-in and lead-out) and
	 * puts every body point against Pavement. Inside is the side of the driven line the turn
	 * bends toward, taken at the point's nearest span of it.
	 */
	inline FOverrun Overrun(const FTurnChain& Chain, const FVehicle& Vehicle, const FPavement& Pavement)
	{
		FOverrun Out;
		const TArray<FVector2D>& Line = Chain.Driven;
		if (Line.Num() < 2 || Chain.Path.Num() < 2) { return Out; }
		TArray<double> Inner, Outer;
		TArray<FVector2D> Points;
		Out.bTraced = VehicleSweep::Trace(VehicleFit::BodyOf(Vehicle), Line, Inner, Outer, nullptr, &Points);
		const FVector2D Mid = Chain.Path[Chain.Path.Num() / 2];
		const double TurnSign = FVector2D::CrossProduct(Mid - Chain.Path[0], Chain.Path.Last() - Mid) >= 0.0 ? 1.0 : -1.0;
		for (const FVector2D& P : Points)
		{
			const double Off = Pavement.Outside(P);
			if (Off <= 0.0) { continue; }
			double Best = TNumericLimits<double>::Max();
			double Side = 0.0;
			bool bBeyond = false;
			for (int32 I = 0; I + 1 < Line.Num(); ++I)
			{
				const FVector2D AB = Line[I + 1] - Line[I];
				const double Raw = FVector2D::DotProduct(P - Line[I], AB) / FMath::Max(AB.SizeSquared(), 1e-12);
				const double T = FMath::Clamp(Raw, 0.0, 1.0);
				const FVector2D Foot = Line[I] + AB * T;
				const double D = FVector2D::DistSquared(P, Foot);
				if (D < Best)
				{
					Best = D;
					Side = FVector2D::CrossProduct(AB, P - Foot) * TurnSign;
					bBeyond = (I == 0 && Raw < 0.0) || (I + 2 == Line.Num() && Raw > 1.0);
				}
			}
			// BEYOND EITHER END is Trace's own straight lead, past the lanes Driven names - road this
			// measure does not know (a dead end's cap, the next junction), as Trace itself skips it.
			if (bBeyond) { continue; }
			if (Side > 0.0)
			{
				if (Off > Out.Inner) { Out.Inner = Off; Out.InnerAt = P; }
				Out.InnerPoints.Add(P);
				Out.InnerDepths.Add(Off);
			}
			else
			{
				Out.Outer = FMath::Max(Out.Outer, Off);
			}
		}
		return Out;
	}

	/** Spread of a chain's samples' distances from Centre: max - min. 0 is concentric. */
	inline void RadiusAbout(const FTurnChain& Chain, const FVector2D& Centre, double& OutMin, double& OutMax)
	{
		OutMin = TNumericLimits<double>::Max();
		OutMax = 0.0;
		for (const FVector2D& P : Chain.Path)
		{
			const double D = FVector2D::Distance(P, Centre);
			OutMin = FMath::Min(OutMin, D);
			OutMax = FMath::Max(OutMax, D);
		}
	}
}

#endif
