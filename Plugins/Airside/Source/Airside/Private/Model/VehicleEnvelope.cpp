#include "Model/VehicleEnvelope.h"

#include "Model/VehicleFit.h"
#include "Solve/IcaoCode.h"

FVehicleEnvelope FVehicleEnvelope::Of(TConstArrayView<FVehicle> Vehicles)
{
	FVehicleEnvelope Out;
	for (const FVehicle& Vehicle : Vehicles)
	{
		// THE SAME FOUR FIGURES VehicleFit::NoLargerThan compares, computed by the same functions, so
		// a member is admitted by its own envelope exactly - no tolerance between two evaluators.
		const double Forward = Vehicle.Chassis.TightestFollowableRadius();
		if (Out.bEmpty || Forward > Out.ForwardRadius)
		{
			Out.ForwardRadius = Forward;
			Out.WidestTurning = Vehicle.Chassis;
		}
		Out.Width = FMath::Max(Out.Width, Vehicle.WidestBody());
		Out.ReverseRadius = FMath::Max(Out.ReverseRadius, VehicleFit::TightestReverseRadius(Vehicle));
		const double Chain = VehicleFit::ChainLength(Vehicle);
		double& Axis = Vehicle.HasTrailer() ? Out.TrailerChain : Out.RigidChain;
		Axis = FMath::Max(Axis, Chain);
		Out.bEmpty = false;
	}
	return Out;
}

bool FVehicleEnvelope::Admits(const FVehicle& Kind) const
{
	if (bEmpty)
	{
		return false;
	}
	const double KindChain = VehicleFit::ChainLength(Kind);
	return Kind.WidestBody() <= Width
		&& Kind.Chassis.TightestFollowableRadius() <= ForwardRadius
		&& VehicleFit::TightestReverseRadius(Kind) <= ReverseRadius
		&& KindChain <= (Kind.HasTrailer() ? TrailerChain : RigidChain);
}

TArray<FVehicle> VehicleEnvelope::WithDesignFirst(const FVehicle& First, TConstArrayView<FVehicle> Rest)
{
	TArray<FVehicle> Out;
	Out.Add(First);
	for (const FVehicle& Vehicle : Rest)
	{
		// BY TypeCode, the identity the catalogue and the bid key a kind on. Two letters naming the
		// same vehicle (A and B both the tow) are one member, not two.
		if (!Out.ContainsByPredicate([&Vehicle](const FVehicle& Have) { return Have.TypeCode == Vehicle.TypeCode; }))
		{
			Out.Add(Vehicle);
		}
	}
	return Out;
}

TArray<FVehicle> VehicleEnvelope::AdmittedUpTo(EIcaoCode Letter, TFunctionRef<FVehicle(EIcaoCode)> DesignOf)
{
	// SMALLEST LETTER LAST: Letter's own first (it is the stand's design vehicle, what an inspect line
	// names), then down the table. EIcaoCode is declared A..F, so the ordinal walk is the size walk.
	TArray<FVehicle> Smaller;
	for (int32 Index = static_cast<int32>(Letter) - 1; Index >= 0; --Index)
	{
		Smaller.Add(DesignOf(static_cast<EIcaoCode>(Index)));
	}
	return WithDesignFirst(DesignOf(Letter), Smaller);
}
