#include "Model/VehicleFit.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/VehicleSweep.h"

bool VehicleFit::Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network)
{
	const double Widest = Vehicle.WidestBody();
	if (Widest > 0.0 && Edge.Width > 0.0 && Widest + 2.0 * WidthMargin > Edge.Width)
	{
		return false;
	}
	if (Edge.MinRadius <= 0.0)
	{
		return true;
	}

	const double Lock = Vehicle.Chassis.TightestFollowableRadius();
	if (Lock > Edge.MinRadius)
	{
		return false;
	}
	if (Widest <= 0.0 || Edge.ClearInnerAt.Num() == 0)
	{
		return true;
	}

	const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
	const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
	if (A == nullptr || B == nullptr)
	{
		return true;
	}
	// THE SAMPLES THE BUILDER MEASURED AND THE FOLLOWER WALKS - the same call, so the
	// clearances line up with the simulated path index for index.
	TArray<FVector2D> Path;
	GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Path);
	if (Path.Num() != Edge.ClearInnerAt.Num() || Path.Num() != Edge.ClearOuterAt.Num())
	{
		return true;   // measured against a different sampling: say nothing rather than guess
	}

	VehicleSweep::FBody Body;
	Body.Wheelbase = Vehicle.Chassis.Wheelbase();
	Body.Width = Vehicle.BodyWidth;
	Body.FrontX = Vehicle.BodyFrontX;
	Body.RearX = Vehicle.BodyRearX;
	if (Vehicle.HasTrailer())
	{
		Body.KingpinX = Vehicle.Trailer.KingpinX;
		Body.KingpinToAxle = Vehicle.Trailer.KingpinToAxle;
		Body.TrailerFront = Vehicle.Trailer.FrontAheadOfKingpin;
		Body.TrailerRear = Vehicle.Trailer.RearBehindAxle;
		Body.TrailerWidth = Vehicle.Trailer.Width;
	}
	TArray<double> Inner, Outer;
	if (!VehicleSweep::Trace(Body, Path, Inner, Outer))
	{
		return false;
	}
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		if (Inner[Index] + Outer[Index] > Edge.ClearInnerAt[Index] + Edge.ClearOuterAt[Index])
		{
			return false;
		}
	}
	return true;
}
