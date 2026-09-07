#include "CoreMinimal.h"
#include "Build/RunwayMarkingBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using B = FRunwayMarkingBuilder;

	/** A straight runway from (0,0) toward +X - north, so it is runway 36/18 - with Facts. */
	URoadNetwork* MakeRunwayNetwork(double Length, double TotalWidth, const FRunwayFacts& Facts)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Runway = URoadProfile::MakeTransient(TotalWidth, 1500.0, TotalWidth * 0.1);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId E = Net->AddNode(FVector2D(Length, 0.0));
		const FRoadSegmentId Seg = Net->AddStraightSegment(A, E, Runway);
		Net->SetRunwayFacts(Seg, Facts);
		return Net;
	}

	FRunwayFacts Facts(ERunwaySurface Surface, ERunwayApproach Approach)
	{
		FRunwayFacts Out;
		Out.Surface = Surface;
		Out.Approach = Approach;
		return Out;
	}

	/** One quad's axis-aligned extent in the runway frame (along = X, across = Y). */
	struct FQuadBox
	{
		double Along0 = 0.0, Along1 = 0.0, Across0 = 0.0, Across1 = 0.0;
		double AlongExtent() const { return Along1 - Along0; }
		double AcrossExtent() const { return Across1 - Across0; }
		bool Is(double Along, double Across) const
		{
			return FMath::IsNearlyEqual(AlongExtent(), Along, 1.0e-6) && FMath::IsNearlyEqual(AcrossExtent(), Across, 1.0e-6);
		}
	};

	TArray<FQuadBox> Quads(const FRoadMeshBuffers& Buffers)
	{
		TArray<FQuadBox> Out;
		for (int32 Base = 0; Base + 3 < Buffers.Positions.Num(); Base += 4)
		{
			FQuadBox Box;
			Box.Along0 = Box.Along1 = Buffers.Positions[Base].X;
			Box.Across0 = Box.Across1 = Buffers.Positions[Base].Y;
			for (int32 Index = 1; Index < 4; ++Index)
			{
				const FVector3d& P = Buffers.Positions[Base + Index];
				Box.Along0 = FMath::Min(Box.Along0, P.X); Box.Along1 = FMath::Max(Box.Along1, P.X);
				Box.Across0 = FMath::Min(Box.Across0, P.Y); Box.Across1 = FMath::Max(Box.Across1, P.Y);
			}
			Out.Add(Box);
		}
		return Out;
	}

	int32 CountOf(const TArray<FQuadBox>& Boxes, double Along, double Across)
	{
		int32 Count = 0;
		for (const FQuadBox& Box : Boxes) { if (Box.Is(Along, Across)) { ++Count; } }
		return Count;
	}
}

/**
 * THE FULL SET, MEASURED. A 45 m precision runway 2 km long: every marking the standard
 * asks for is counted and placed by the buffers the component receives, in the road
 * plane at the Z asked for, UV1 zero so the material paints it, and facing UP the way
 * Unreal measures it - the check the holding-position paint once failed 112 times over.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsPrecision45Test,
	"Airside.Build.RunwayMarkings.Precision45",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsPrecision45Test::RunTest(const FString& Parameters)
{
	constexpr double Length = 200000.0;
	constexpr double Width = 4500.0;
	constexpr double Z = 10.5;
	URoadNetwork* Net = MakeRunwayNetwork(Length, Width, Facts(ERunwaySurface::Tarmac, ERunwayApproach::Precision));

	FRoadMeshBuffers Buffers;
	FRunwayMarkingCensus Census;
	TestEqual(TEXT("one runway painted"), B::Build(*Net, Z, Buffers, &Census), 1);
	TestEqual(TEXT("four vertices per quad, whole quads only"), Buffers.Positions.Num() % 4, 0);
	TestEqual(TEXT("one material id per triangle"), Buffers.MaterialIDs.Num(), Buffers.Indices.Num() / 3);
	const TArray<FQuadBox> Boxes = Quads(Buffers);

	// THRESHOLD STRIPES: 12 at each end, 30 m x 1.8 m, 6 m in, symmetric, 3 m inside the edge.
	int32 NearStripes = 0, FarStripes = 0;
	double OuterMost = 0.0, SumAcross = 0.0;
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(B::StripeLength, B::StripeWidth)) { continue; }
		if (FMath::IsNearlyEqual(Box.Along0, B::StripeStart, 1.0e-6)) { ++NearStripes; SumAcross += Box.Across0 + Box.Across1; }
		else if (FMath::IsNearlyEqual(Box.Along1, Length - B::StripeStart, 1.0e-6)) { ++FarStripes; }
		else { AddError(FString::Printf(TEXT("a 3000 x 180 stripe at neither threshold: along %.0f"), Box.Along0)); }
		OuterMost = FMath::Max(OuterMost, FMath::Max(FMath::Abs(Box.Across0), FMath::Abs(Box.Across1)));
	}
	TestEqual(TEXT("12 threshold stripes at the near end"), NearStripes, 12);
	TestEqual(TEXT("and 12 at the far end"), FarStripes, 12);
	TestEqual(TEXT("the census agrees"), Census.ThresholdStripes, 24);
	TestTrue(TEXT("the near set is symmetric about the centreline"), FMath::Abs(SumAcross) < 1.0e-6);
	TestEqual(TEXT("the outermost stripe's edge is 3 m inside the pavement edge"), OuterMost, Width * 0.5 - B::StripeInset, 1.0e-6);

	// DESIGNATION: 36 at the near end (north is +X), 18 at the far end; each two digits
	// 9 m tall, feet 6 m past the stripes. Glyph strokes are the quads that are none of
	// the standard rectangles, bounded per end.
	// The far designation reads the other way: its feet are its greatest along, its top its least.
	double NearFoot = 1.0e9, NearTop = -1.0e9, FarFoot = -1.0e9, FarTop = 1.0e9, GlyphAcross = 0.0;
	int32 Strokes = 0;
	for (const FQuadBox& Box : Boxes)
	{
		if (Box.Is(B::StripeLength, B::StripeWidth) || Box.Is(B::DashOn, B::CentrelineWidthWide)
			|| Box.Is(B::AimingPointLength, B::AimingPointWidth) || Box.Is(B::TouchdownStripeLength, B::TouchdownStripeWidth)
			|| Box.Is(Length, B::SideStripeWidth))
		{
			continue;
		}
		++Strokes;
		GlyphAcross = FMath::Max(GlyphAcross, FMath::Max(FMath::Abs(Box.Across0), FMath::Abs(Box.Across1)));
		if (Box.Along0 < Length * 0.5) { NearFoot = FMath::Min(NearFoot, Box.Along0); NearTop = FMath::Max(NearTop, Box.Along1); }
		else { FarFoot = FMath::Max(FarFoot, Box.Along1); FarTop = FMath::Min(FarTop, Box.Along0); }
	}
	const double DigitFoot = B::StripeStart + B::StripeLength + B::DigitGapAfterStripes;
	TestEqual(TEXT("the census counts the strokes"), Census.DesignatorStrokes, Strokes);
	TestTrue(TEXT("strokes were painted at both ends"), Strokes >= 6);
	TestEqual(TEXT("the near designation's feet are 6 m past the stripes"), NearFoot, DigitFoot, 1.0e-6);
	TestEqual(TEXT("and it is 9 m tall"), NearTop - NearFoot, B::DigitHeight, 1.0e-6);
	TestEqual(TEXT("the far designation's feet are 6 m past its stripes"), FarFoot, Length - DigitFoot, 1.0e-6);
	TestEqual(TEXT("and it is 9 m tall, reading the other way"), FarFoot - FarTop, B::DigitHeight, 1.0e-6);
	const double DigitWidth = B::DigitHeight / 1.6;
	TestEqual(TEXT("two digits and a gap, centred on the centreline"), GlyphAcross, DigitWidth + B::DigitSpacing * DigitWidth * 0.5, 1.0e-6);

	// CENTRELINE: floor((L - 2 x 5700) / 5000) dashes of 30 m x 0.9 m (the wide runway's), on the centreline.
	const double Clear = DigitFoot + B::DigitHeight + B::DashGapAfterDigits;
	const int32 DashesExpected = FMath::FloorToInt32((Length - 2.0 * Clear) / (B::DashOn + B::DashOff));
	int32 Dashes = 0;
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(B::DashOn, B::CentrelineWidthWide)) { continue; }
		++Dashes;
		TestTrue(TEXT("a dash is centred on the centreline"), FMath::Abs(Box.Across0 + Box.Across1) < 1.0e-6);
		TestTrue(TEXT("and lies between the designations"), Box.Along0 >= Clear - 1.0e-6 && Box.Along1 <= Length - Clear + 1.0e-6);
	}
	TestEqual(FString::Printf(TEXT("%d centreline dashes"), DashesExpected), Dashes, DashesExpected);
	TestEqual(TEXT("the census agrees"), Census.CentrelineDashes, DashesExpected);

	// AIMING POINT: two bars 45 m x 6 m, 18 m apart, 400 m from each threshold.
	int32 AimingBars = 0;
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(B::AimingPointLength, B::AimingPointWidth)) { continue; }
		++AimingBars;
		const bool bNear = FMath::IsNearlyEqual(Box.Along0, B::AimingPointAt, 1.0e-6);
		const bool bFar = FMath::IsNearlyEqual(Box.Along1, Length - B::AimingPointAt, 1.0e-6);
		TestTrue(TEXT("an aiming bar starts 400 m from one threshold"), bNear || bFar);
		TestEqual(TEXT("its inner edge is 9 m from the centreline"), FMath::Min(FMath::Abs(Box.Across0), FMath::Abs(Box.Across1)), B::AimingPointGap * 0.5, 1.0e-6);
	}
	TestEqual(TEXT("four aiming-point bars, two per end"), AimingBars, 4);

	// TOUCHDOWN ZONE: pairs at 150/300/450 m from each end, one, two, three stripes a side.
	int32 TdzByPair[3] = { 0, 0, 0 };
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(B::TouchdownStripeLength, B::TouchdownStripeWidth)) { continue; }
		bool bPlaced = false;
		for (int32 Pair = 0; Pair < 3; ++Pair)
		{
			if (FMath::IsNearlyEqual(Box.Along0, B::TouchdownPairsAt[Pair], 1.0e-6)
				|| FMath::IsNearlyEqual(Box.Along1, Length - B::TouchdownPairsAt[Pair], 1.0e-6))
			{
				++TdzByPair[Pair];
				bPlaced = true;
			}
		}
		TestTrue(TEXT("a touchdown stripe sits at one of the three distances from an end"), bPlaced);
	}
	TestEqual(TEXT("150 m: one stripe a side, both ends"), TdzByPair[0], 4);
	TestEqual(TEXT("300 m: two a side, both ends"), TdzByPair[1], 8);
	TestEqual(TEXT("450 m: three a side, both ends"), TdzByPair[2], 12);
	TestEqual(TEXT("the census agrees"), Census.TouchdownStripes, 24);

	// SIDE STRIPES: the full length, 0.9 m, at each edge.
	int32 Sides = 0;
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(Length, B::SideStripeWidth)) { continue; }
		++Sides;
		TestEqual(TEXT("a side stripe's outer edge is the pavement edge"), FMath::Max(FMath::Abs(Box.Across0), FMath::Abs(Box.Across1)), Width * 0.5, 1.0e-6);
	}
	TestEqual(TEXT("two side stripes"), Sides, 2);

	// EVERY VERTEX in the plane, painted as marking, and facing up.
	bool bAllAtZ = true, bAllUV1Zero = true;
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		bAllAtZ = bAllAtZ && FMath::Abs(Buffers.Positions[Index].Z - Z) < 1.0e-6;
		bAllUV1Zero = bAllUV1Zero && Buffers.UV1[Index].IsNearlyZero();
	}
	TestTrue(TEXT("every vertex is in the road plane at the Z asked for"), bAllAtZ);
	TestTrue(TEXT("every vertex carries UV1 = 0, so the road material paints it as marking"), bAllUV1Zero);
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions) { Mesh.AppendVertex(Position); }
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			Mesh.AppendTriangle(Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);
		}
		UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
		int32 Downward = 0;
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			if (Mesh.GetVertexNormal(VertexId).Z <= 0.0f) { ++Downward; }
		}
		TestEqual(TEXT("no vertex normal points down - the paint is visible from above"), Downward, 0);
		TestEqual(TEXT("and FDynamicMesh3 refused none of the triangles"), Mesh.TriangleCount(), Buffers.Indices.Num() / 3);
	}
	return true;
}

/** The stripe count and the centreline width follow the width, per ICAO's table. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsByWidthTest,
	"Airside.Build.RunwayMarkings.ByWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsByWidthTest::RunTest(const FString& Parameters)
{
	struct FCase { double Width; int32 Stripes; double Centreline; };
	const FCase Cases[] = { { 1800.0, 4, 45.0 }, { 2300.0, 6, 45.0 }, { 3000.0, 8, 45.0 }, { 6000.0, 16, 90.0 } };
	for (const FCase& Case : Cases)
	{
		URoadNetwork* Net = MakeRunwayNetwork(200000.0, Case.Width, Facts(ERunwaySurface::Tarmac, ERunwayApproach::Precision));
		FRoadMeshBuffers Buffers;
		FRunwayMarkingCensus Census;
		B::Build(*Net, 0.0, Buffers, &Census);
		const TArray<FQuadBox> Boxes = Quads(Buffers);
		TestEqual(FString::Printf(TEXT("%.0f wide: %d threshold stripes per end"), Case.Width, Case.Stripes),
			Census.ThresholdStripes, Case.Stripes * 2);
		TestEqual(FString::Printf(TEXT("%.0f wide: %d stripes measured at the near end"), Case.Width, Case.Stripes),
			CountOf(Boxes, B::StripeLength, B::StripeWidth), Case.Stripes * 2);
		TestEqual(FString::Printf(TEXT("%.0f wide: centreline %.0f wide"), Case.Width, Case.Centreline),
			CountOf(Boxes, B::DashOn, Case.Centreline), Census.CentrelineDashes);
		TestTrue(TEXT("and there are dashes"), Census.CentrelineDashes > 0);
	}
	return true;
}

/** Visual: no aiming point, no touchdown zone. Non-precision: aiming point only. Precision: all. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsByApproachTest,
	"Airside.Build.RunwayMarkings.ByApproach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsByApproachTest::RunTest(const FString& Parameters)
{
	auto Paint = [](ERunwayApproach Approach)
	{
		URoadNetwork* Net = MakeRunwayNetwork(200000.0, 4500.0, Facts(ERunwaySurface::Concrete, Approach));
		FRoadMeshBuffers Buffers;
		FRunwayMarkingCensus Census;
		B::Build(*Net, 0.0, Buffers, &Census);
		return Census;
	};
	const FRunwayMarkingCensus Visual = Paint(ERunwayApproach::Visual);
	TestEqual(TEXT("visual: threshold stripes"), Visual.ThresholdStripes, 24);
	TestTrue(TEXT("visual: a designation"), Visual.DesignatorStrokes > 0);
	TestTrue(TEXT("visual: a centreline"), Visual.CentrelineDashes > 0);
	TestEqual(TEXT("visual: no aiming point"), Visual.AimingPointBars, 0);
	TestEqual(TEXT("visual: no touchdown zone"), Visual.TouchdownStripes, 0);
	TestEqual(TEXT("visual: no side stripes"), Visual.SideStripes, 0);

	const FRunwayMarkingCensus NonPrecision = Paint(ERunwayApproach::NonPrecision);
	TestEqual(TEXT("non-precision: the aiming point"), NonPrecision.AimingPointBars, 4);
	TestEqual(TEXT("non-precision: no touchdown zone"), NonPrecision.TouchdownStripes, 0);
	TestEqual(TEXT("non-precision: no side stripes"), NonPrecision.SideStripes, 0);

	const FRunwayMarkingCensus Precision = Paint(ERunwayApproach::Precision);
	TestEqual(TEXT("precision: the aiming point"), Precision.AimingPointBars, 4);
	TestEqual(TEXT("precision: the touchdown zone"), Precision.TouchdownStripes, 24);
	TestEqual(TEXT("precision: side stripes"), Precision.SideStripes, 2);
	TestEqual(TEXT("concrete paints like tarmac - no grass markers"), Precision.GrassMarkers, 0);
	return true;
}

/** Grass: edge markers every 60 m and a corner square at each corner, and nothing else. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsGrassTest,
	"Airside.Build.RunwayMarkings.Grass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsGrassTest::RunTest(const FString& Parameters)
{
	constexpr double Length = 200000.0;
	URoadNetwork* Net = MakeRunwayNetwork(Length, 3000.0, Facts(ERunwaySurface::Grass, ERunwayApproach::Precision));
	FRoadMeshBuffers Buffers;
	FRunwayMarkingCensus Census;
	TestEqual(TEXT("one runway painted"), B::Build(*Net, 0.0, Buffers, &Census), 1);
	const TArray<FQuadBox> Boxes = Quads(Buffers);
	const int32 Expected = 2 * (FMath::FloorToInt32(Length / B::GrassMarkerSpacing) + 1) + 4;
	TestEqual(FString::Printf(TEXT("%d markers: two per 60 m plus four corners"), Expected), Census.GrassMarkers, Expected);
	TestEqual(TEXT("and every quad is a marker"), Boxes.Num(), Expected);
	TestEqual(TEXT("0.6 m markers"), CountOf(Boxes, B::GrassMarker, B::GrassMarker), Expected - 4);
	TestEqual(TEXT("3 m corners"), CountOf(Boxes, B::GrassCorner, B::GrassCorner), 4);
	TestEqual(TEXT("grass has no threshold stripes even at precision"), Census.ThresholdStripes, 0);
	TestEqual(TEXT("and no centreline"), Census.CentrelineDashes, 0);
	TestEqual(TEXT("and no designation on the ground"), Census.DesignatorStrokes, 0);
	for (const FQuadBox& Box : Boxes)
	{
		TestEqual(TEXT("a marker's outer edge is the strip's edge"), FMath::Max(FMath::Abs(Box.Across0), FMath::Abs(Box.Across1)), 1500.0, 1.0e-6);
	}
	return true;
}

/** A 600 m precision runway paints what fits: the 150 m pairs only, and no aiming point. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsShortPrecisionTest,
	"Airside.Build.RunwayMarkings.ShortPrecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsShortPrecisionTest::RunTest(const FString& Parameters)
{
	constexpr double Length = 60000.0;
	URoadNetwork* Net = MakeRunwayNetwork(Length, 3000.0, Facts(ERunwaySurface::Tarmac, ERunwayApproach::Precision));
	FRoadMeshBuffers Buffers;
	FRunwayMarkingCensus Census;
	B::Build(*Net, 0.0, Buffers, &Census);
	const TArray<FQuadBox> Boxes = Quads(Buffers);
	// The 150 m pair (ends at 172.5 m) is inside the near half; the 300 m pair would end at
	// 322.5 m, past the midpoint at 300 m, and meets the far end's own 300 m pair.
	TestEqual(TEXT("only the 150 m pairs: one stripe a side at each end"), Census.TouchdownStripes, 4);
	for (const FQuadBox& Box : Boxes)
	{
		if (!Box.Is(B::TouchdownStripeLength, B::TouchdownStripeWidth)) { continue; }
		TestTrue(TEXT("a stripe stays in its own half"), Box.Along1 <= Length * 0.5 + 1.0e-6 || Box.Along0 >= Length * 0.5 - 1.0e-6);
	}
	// The aiming point at 300 m (the short-runway distance) would end at 345 m: omitted.
	TestEqual(TEXT("no aiming point fits on 600 m"), Census.AimingPointBars, 0);
	TestEqual(TEXT("threshold stripes still paint"), Census.ThresholdStripes, 16);
	TestEqual(TEXT("side stripes still paint"), Census.SideStripes, 2);
	return true;
}

#endif
