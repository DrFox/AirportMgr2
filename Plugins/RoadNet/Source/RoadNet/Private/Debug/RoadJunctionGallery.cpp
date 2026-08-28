#include "Debug/RoadJunctionGallery.h"

#include "Debug/RoadDebugDraw.h"
#include "DrawDebugHelpers.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

ARoadJunctionGallery::ARoadJunctionGallery()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ARoadJunctionGallery::BeginPlay()
{
	Super::BeginPlay();
	BuildGallery();
}

void ARoadJunctionGallery::BuildGallery()
{
	Network = NewObject<URoadNetwork>(this);
	Profile = URoadProfile::MakeTransient(TaxiwayWidth, FilletRadius);

	CellBearings = {
		{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.25 },
		{ 0.0, UE_DOUBLE_PI * 0.5 },
		{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },
		{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },
		{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8, UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }
	};

	CellCentres.Reset();
	constexpr int32 Columns = 4;
	for (int32 Index = 0; Index < CellBearings.Num(); ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		CellCentres.Add(FVector2D(Column * CellSpacing, Row * CellSpacing));
	}
}

void ARoadJunctionGallery::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (Network == nullptr || RoadDebug::GetDebugDrawLevel() <= 0)
	{
		return;
	}

	const double ZHeight = GetActorLocation().Z + 10.0;

	for (int32 CellIndex = 0; CellIndex < CellCentres.Num(); ++CellIndex)
	{
		FJunctionInput Input;
		Input.Position = CellCentres[CellIndex];
		Input.ArcSegments = 12;

		for (const double Bearing : CellBearings[CellIndex])
		{
			FJunctionArm Arm;
			Arm.Tangent = FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing));
			Arm.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Arm.HalfWidthRight = Profile->GetHalfWidthRight();
			Arm.FilletRadius = Profile->PreferredFilletRadius;
			Input.Arms.Add(Arm);
		}

		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		RoadDebug::DrawJunction(GetWorld(), Input, Result, ZHeight);

		// Draw each arm's ribbon edges from its cut out to ArmLength, in blue.
		for (int32 ArmIndex = 0; ArmIndex < Input.Arms.Num(); ++ArmIndex)
		{
			const FJunctionArm& Arm = Input.Arms[ArmIndex];
			const FVector2D Normal = FVector2D(-Arm.Tangent.Y, Arm.Tangent.X);

			// The arm must extend past its own cut, or it renders inside-out.
			const double DrawLength =
				FMath::Max(ArmLength, Result.Arms[ArmIndex].CutDistance + 3000.0);
			const FVector2D FarCentre = Input.Position + Arm.Tangent * DrawLength;

			const FVector2D FarLeft  = FarCentre + Normal * Arm.HalfWidthLeft;
			const FVector2D FarRight = FarCentre - Normal * Arm.HalfWidthRight;

			auto ToWorld = [ZHeight](const FVector2D& P) { return FVector(P.X, P.Y, ZHeight); };

			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].LeftCut),  ToWorld(FarLeft),
				FColor::Blue, false, -1.0f, 0, 5.0f);
			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].RightCut), ToWorld(FarRight),
				FColor::Blue, false, -1.0f, 0, 5.0f);
			DrawDebugLine(GetWorld(), ToWorld(FarLeft), ToWorld(FarRight),
				FColor::Blue, false, -1.0f, 0, 3.0f);
		}
	}
}
