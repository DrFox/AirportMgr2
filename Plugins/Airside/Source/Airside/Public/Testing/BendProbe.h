#pragma once

// TEST-ONLY, guarded like AirsideTestWorld.h: public here because AirsideTests (the per-tier
// bend fixture) and AirportMgr (the rig course) both measure bends with it, and neither may
// depend on the other's test code.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
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
 * leaves the tarmac driving it - driven by VehicleSweep::Drive, the router's own pursuit (Trace's), with
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
		TArray<FVector2D> Points;
		Out.bTraced = VehicleSweep::Drive(VehicleFit::BodyOf(Vehicle), Line,
			[&Points](const VehicleSweep::FCorners& Corners) { Points.Append(Corners); });
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

	/** Every body corner a vehicle reaches driving Chain.Driven - VehicleSweep::Drive's, as Overrun takes them. */
	inline TArray<FVector2D> BodyPoints(const FTurnChain& Chain, const FVehicle& Vehicle)
	{
		TArray<FVector2D> Points;
		if (Chain.Driven.Num() >= 2)
		{
			VehicleSweep::Drive(VehicleFit::BodyOf(Vehicle), Chain.Driven,
				[&Points](const VehicleSweep::FCorners& Corners) { Points.Append(Corners); });
		}
		return Points;
	}

	/**
	 * The least distance from any of Points to the junction's RIM - every boundary edge but the
	 * cut lines, which are the seam to a ribbon, not an edge of the tarmac. How close a body comes
	 * to the pavement's edge through the bend: a widening laid "only by what is left" leaves this
	 * near its margin, a generous one far above it.
	 */
	inline double RimClearance(const FJunctionResult& Junction, const TArray<FVector2D>& Points)
	{
		if (Junction.Boundary.Num() < 4) { return TNumericLimits<double>::Max(); }
		const int32 Rim = Junction.Boundary.Num() - 1;
		auto IsCutLine = [&Junction](const FVector2D& A, const FVector2D& B)
		{
			for (const FJunctionArmResult& Arm : Junction.Arms)
			{
				if ((A == Arm.LeftCut && B == Arm.RightCut) || (A == Arm.RightCut && B == Arm.LeftCut)) { return true; }
			}
			return false;
		};
		double Least = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Rim; ++I)
		{
			const FVector2D A = Junction.Boundary[I];
			const FVector2D B = Junction.Boundary[(I + 1) % Rim];
			if (IsCutLine(A, B)) { continue; }
			const FVector2D AB = B - A;
			for (const FVector2D& P : Points)
			{
				const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1e-12), 0.0, 1.0);
				Least = FMath::Min(Least, FVector2D::Distance(P, A + AB * T));
			}
		}
		return Least;
	}

	/**
	 * THE WORST TANGENT KINK along a polyline, degrees: at each vertex, how far its turn lies OUTSIDE
	 * the range of its two neighbours' turns. Along a tangent-continuous curve sampled finer than its
	 * features the turn varies smoothly, so each vertex's lies between its neighbours' and scores 0 -
	 * an arc, a straight meeting an arc wherever the join falls between samples, a ramp's
	 * inflection. A crease - two edges meeting at an angle with no curve between - turns more than
	 * either neighbour by about its own angle and scores that. So it measures a break in the TANGENT,
	 * not the curvature (TightestRadius does that) and not the sampling. The first version scored
	 * the departure from the neighbours' MEAN, and read a curvature step as a kink - up to half a
	 * step's turn, 1.4 degrees on a Narrow bend's plain arc at 40 uu (2026-09-25). Segments under
	 * half a uu are merged, not measured.
	 */
	inline double TangentKinkDegrees(const TArray<FVector2D>& Polyline, FVector2D* OutWhere = nullptr)
	{
		TArray<FVector2D> P;
		for (const FVector2D& Point : Polyline)
		{
			if (P.Num() == 0 || FVector2D::Distance(P.Last(), Point) > 0.5) { P.Add(Point); }
		}
		TArray<double> Turn;
		TArray<FVector2D> At;
		for (int32 I = 1; I + 1 < P.Num(); ++I)
		{
			const FVector2D A = (P[I] - P[I - 1]).GetSafeNormal();
			const FVector2D B = (P[I + 1] - P[I]).GetSafeNormal();
			Turn.Add(FMath::RadiansToDegrees(FMath::Atan2(A.X * B.Y - A.Y * B.X, FVector2D::DotProduct(A, B))));
			At.Add(P[I]);
		}
		double Worst = 0.0;
		for (int32 I = 0; I < Turn.Num(); ++I)
		{
			const double Before = I > 0 ? Turn[I - 1] : 0.0;
			const double After = I + 1 < Turn.Num() ? Turn[I + 1] : 0.0;
			const double Kink = FMath::Max3(0.0, Turn[I] - FMath::Max(Before, After), FMath::Min(Before, After) - Turn[I]);
			if (Kink > Worst)
			{
				Worst = Kink;
				if (OutWhere != nullptr) { *OutWhere = At[I]; }
			}
		}
		return Worst;
	}

	/**
	 * THE TIGHTEST LOCAL RADIUS along a polyline, uu: the smallest circle through three consecutive
	 * vertices (collinear ones are skipped). Where TangentKinkDegrees judges the tangent, this judges
	 * the curvature - a smooth wiggle has no kink and a small radius.
	 */
	inline double TightestRadius(const TArray<FVector2D>& Polyline, FVector2D* OutWhere = nullptr)
	{
		double Tightest = TNumericLimits<double>::Max();
		for (int32 I = 1; I + 1 < Polyline.Num(); ++I)
		{
			const FVector2D A = Polyline[I - 1], B = Polyline[I], C = Polyline[I + 1];
			const double Twice = FMath::Abs(FVector2D::CrossProduct(B - A, C - A));
			if (Twice < 1e-6) { continue; }
			const double Radius = FVector2D::Distance(A, B) * FVector2D::Distance(B, C) * FVector2D::Distance(C, A) / (2.0 * Twice);
			if (Radius < Tightest)
			{
				Tightest = Radius;
				if (OutWhere != nullptr) { *OutWhere = B; }
			}
		}
		return Tightest;
	}

	/**
	 * A two-arm junction's pavement edge round corner Corner (between arm Corner's LEFT cut and
	 * arm Corner+1's RIGHT cut), led in and out by Lead uu of each arm's straight edge - so the
	 * joins to the ribbons are judged with the rim. Empty when the rim does not hold both cuts.
	 */
	inline TArray<FVector2D> CornerEdge(const FJunctionResult& Junction, int32 Corner, double Lead = 300.0)
	{
		TArray<FVector2D> Out;
		const int32 Rim = Junction.Boundary.Num() - 1;
		const int32 ArmCount = Junction.Arms.Num();
		if (Rim < 3 || !Junction.Arms.IsValidIndex(Corner)) { return Out; }
		const FJunctionArmResult& From = Junction.Arms[Corner];
		const FJunctionArmResult& To = Junction.Arms[(Corner + 1) % ArmCount];
		auto TangentOf = [](const FJunctionArmResult& Arm)
		{
			const FVector2D N = (Arm.LeftCut - Arm.RightCut).GetSafeNormal();
			return FVector2D(N.Y, -N.X);
		};
		int32 Start = INDEX_NONE;
		int32 End = INDEX_NONE;
		for (int32 I = 0; I < Rim; ++I)
		{
			if (Junction.Boundary[I] == From.LeftCut) { Start = I; }
			if (Junction.Boundary[I] == To.RightCut) { End = I; }
		}
		if (Start == INDEX_NONE || End == INDEX_NONE) { return Out; }
		Out.Add(From.LeftCut + TangentOf(From) * Lead);
		for (int32 I = Start; ; I = (I + 1) % Rim)
		{
			Out.Add(Junction.Boundary[I]);
			if (I == End) { break; }
		}
		Out.Add(To.RightCut + TangentOf(To) * Lead);
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
	/**
	 * The tangent-kink ceiling for a bend's pavement edges and lanes, degrees (TangentKinkDegrees).
	 * 1.5: a tangent-continuous curve scores ~0 however it is sampled, and the creases the user saw
	 * on 33d6f49b - a width step's at the arc, a widening polygon's corners - score 13 to 35
	 * (measured 2026-09-25).
	 */
	constexpr double KinkThreshold = 1.5;

	/**
	 * The tightest an edge may bend, as a fraction of its inner arc's radius (FBendOuter::
	 * InnerArcRadius). One half: a ramp running onto the arc bends no tighter than the arc itself
	 * (SmoothBend's ShortestRamp), so the two add to at most twice the arc's curvature. Catches the
	 * smooth wiggle the kink cannot - a 100 uu step leant over 400 uu bent at 196 uu (2026-09-25).
	 */
	constexpr double TightestFraction = 0.5;

	/** A lane chain with a straight lead of its own lanes either side, so the joins are judged too. */
	inline TArray<FVector2D> LaneWithLeads(const URoadNetwork& Net, const FTurnChain& Chain, double Lead = 300.0)
	{
		TArray<FVector2D> Out;
		FVector2D FarIn, FarOut;
		if (Chain.Path.Num() == 0) { return Out; }
		if (LaneAt(Net, Chain.From, FarIn)) { Out.Add(Chain.Path[0] + (FarIn - Chain.Path[0]).GetSafeNormal() * Lead); }
		Out.Append(Chain.Path);
		if (LaneAt(Net, Chain.To, FarOut)) { Out.Add(Chain.Path.Last() + (FarOut - Chain.Path.Last()).GetSafeNormal() * Lead); }
		return Out;
	}

	/** One two-arm road bend's smoothness, measured - see MeasureSmoothness. */
	struct FSmoothness
	{
		double InnerKink = 0.0;
		double OuterKink = 0.0;
		FVector2D InnerAt = FVector2D::ZeroVector;
		FVector2D OuterAt = FVector2D::ZeroVector;
		/** The tightest local radius of either edge, and where. */
		double Tightest = TNumericLimits<double>::Max();
		FVector2D TightAt = FVector2D::ZeroVector;
		double LaneKink = 0.0;
		int32 Lanes = 0;
		/** The longest and shortest mean CURVED piece length over the bend's lanes (THE LENGTH RULE). */
		double LongestStep = 0.0;
		double ShortestStep = TNumericLimits<double>::Max();
		/** Per lane: "N nodes (C curved) L uu, kink K; ". */
		FString LaneText;

		/** Whether it meets every rule: KinkThreshold, TightestFraction, BendPieceLength. */
		bool Holds(double InnerArcRadius) const
		{
			return InnerKink <= KinkThreshold && OuterKink <= KinkThreshold && LaneKink <= KinkThreshold
				&& Tightest >= TightestFraction * InnerArcRadius - 1.0
				&& (Lanes == 0 || (LongestStep <= GuidelineGeom::BendPieceLength + 1.0 && ShortestStep >= 0.5 * GuidelineGeom::BendPieceLength));
		}
	};

	/**
	 * ONE SMOOTH SHAPE, measured at a two-arm bend (user, PIE on 33d6f49b: "three variants"): both
	 * pavement edges from 300 uu up one arm to 300 uu down the other (CornerEdge), and every lane
	 * turn with a lead of its lane either side. The course test and the per-tier fixture both judge
	 * with this, so they cannot disagree about what smooth means.
	 */
	inline FSmoothness MeasureSmoothness(const URoadNetwork& Net, const FJunctionResult& Junction, FRoadNodeId Node)
	{
		FSmoothness Out;
		if (Junction.Corners.Num() != 2) { return Out; }
		const int32 Inner = Junction.Corners[0].Theta < UE_DOUBLE_PI ? 0 : 1;
		const TArray<FVector2D> InnerEdge = CornerEdge(Junction, Inner);
		const TArray<FVector2D> OuterEdge = CornerEdge(Junction, 1 - Inner);
		Out.InnerKink = TangentKinkDegrees(InnerEdge, &Out.InnerAt);
		Out.OuterKink = TangentKinkDegrees(OuterEdge, &Out.OuterAt);
		FVector2D OuterTightAt;
		const double InnerTight = TightestRadius(InnerEdge, &Out.TightAt);
		const double OuterTight = TightestRadius(OuterEdge, &OuterTightAt);
		Out.Tightest = FMath::Min(InnerTight, OuterTight);
		if (OuterTight < InnerTight) { Out.TightAt = OuterTightAt; }
		for (const FTurnChain& Chain : TurnsAt(Net, Node))
		{
			++Out.Lanes;
			const double Kink = TangentKinkDegrees(LaneWithLeads(Net, Chain));
			Out.LaneKink = FMath::Max(Out.LaneKink, Kink);
			double Length = 0.0;
			for (int32 I = 1; I < Chain.Path.Num(); ++I) { Length += FVector2D::Distance(Chain.Path[I - 1], Chain.Path[I]); }
			int32 Curved = 0;
			double CurvedLength = 0.0;
			for (const FGuidelineEdgeId Id : Chain.Pieces)
			{
				const FGuidelineEdge* Piece = Net.GetGuidelineEdge(Id);
				if (Piece != nullptr && Piece->MinRadius > 0.0) { ++Curved; CurvedLength += Piece->Length; }
			}
			if (Curved > 0)
			{
				Out.LongestStep = FMath::Max(Out.LongestStep, CurvedLength / Curved);
				Out.ShortestStep = FMath::Min(Out.ShortestStep, CurvedLength / Curved);
			}
			Out.LaneText += FString::Printf(TEXT("%d nodes (%d curved) %.0f uu, kink %.1f; "), Chain.Pieces.Num() + 1, Curved, Length, Kink);
		}
		return Out;
	}

	/**
	 * THE SMOOTH-SHAPE ASSERTIONS, held once (#301): RigTestCourseTest's BendsAreSmooth and
	 * BendLaneTest's EveryTierIsSmooth each ran these five TestTrue calls by hand, in step,
	 * naming the same rules (KinkThreshold, TightestFraction, GuidelineGeom::BendPieceLength)
	 * in two files a change to one had to remember to repeat in the other. Runs the SAME FIVE
	 * checks either caller ran inline before, so a test's own assertion count is unchanged -
	 * only the source is not duplicated.
	 *
	 * Label names which bend a failure is about ("bend (12000, 6000)", "Narrow -> Wide"), and
	 * [MinLanes, MaxLanes] is the one thing that legitimately differs between the two callers:
	 * the course's tiers see a variable lane count (>= 1) where a synthetic two-arm bend always
	 * sees exactly two - everything else here is the shared rule.
	 */
	inline void JudgeBend(FAutomationTestBase& Test, const FString& Label, const FSmoothness& S,
		double InnerArcRadius, int32 MinLanes, int32 MaxLanes = MAX_int32)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: the inner edge is smooth (kink %.2f deg at (%.0f, %.0f))"),
			*Label, S.InnerKink, S.InnerAt.X, S.InnerAt.Y), S.InnerKink <= KinkThreshold);
		Test.TestTrue(FString::Printf(TEXT("%s: the outer edge is smooth (kink %.2f deg at (%.0f, %.0f))"),
			*Label, S.OuterKink, S.OuterAt.X, S.OuterAt.Y), S.OuterKink <= KinkThreshold);
		Test.TestTrue(FString::Printf(TEXT("%s: no edge bends tighter than half its inner arc (%.0f of %.0f)"),
			*Label, S.Tightest, InnerArcRadius), S.Tightest >= TightestFraction * InnerArcRadius - 1.0);
		Test.TestTrue(FString::Printf(TEXT("%s: every lane is smooth (kink %.2f deg)"), *Label, S.LaneKink),
			S.LaneKink <= KinkThreshold);
		Test.TestTrue(FString::Printf(TEXT("%s: %d lane(s), laid by one length rule (%.0f - %.0f uu)"),
			*Label, S.Lanes, S.ShortestStep, S.LongestStep),
			S.Lanes >= MinLanes && S.Lanes <= MaxLanes
				&& S.LongestStep <= GuidelineGeom::BendPieceLength + 1.0 && S.ShortestStep >= 0.5 * GuidelineGeom::BendPieceLength);
	}
}

#endif
