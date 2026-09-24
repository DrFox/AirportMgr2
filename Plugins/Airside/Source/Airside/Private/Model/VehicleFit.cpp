#include "Model/VehicleFit.h"

#include "Model/RoadGuideline.h"
#include "Model/Vehicle.h"
#include "Solve/VehicleSweep.h"

bool VehicleFit::Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle)
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
	if (Widest <= 0.0)
	{
		return true;
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
	const VehicleSweep::FEnvelope Envelope = VehicleSweep::Envelope(Body, Edge.MinRadius);
	if (!Envelope.bHolds)
	{
		return false;
	}
	return (Edge.ClearInner < 0.0 || Envelope.Inner <= Edge.ClearInner)
		&& (Edge.ClearOuter < 0.0 || Envelope.Outer <= Edge.ClearOuter);
}
