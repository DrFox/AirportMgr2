#include "Build/DepotKit.h"

PlotYard::FFootprint DepotFootprint(EDepotModule Module)
{
	// THEY DIFFER, and that is the point. All three were one bay and drew at bay size, so
	// scattering them would give a jumble of IDENTICAL boxes - varied placement is what
	// makes differing footprints legible, where the grid hid them. Grey-box figures chosen
	// for LEGIBILITY like the heights in PlotPresenter, not measured off the concept sheet;
	// the real ones arrive with the meshes.
	PlotYard::FFootprint Out;
	switch (Module)
	{
	case EDepotModule::Shed:
		Out.LengthUu = 800.0;
		Out.WidthUu = 400.0;
		// THE ONLY ONE THAT FRONTS THE GATE. A truck drives out of it, so its heading is
		// functional and may not be turned for looks.
		Out.bFrontsTheGate = true;
		return Out;
	case EDepotModule::Tank:
		Out.LengthUu = 500.0;
		Out.WidthUu = 500.0;
		return Out;
	case EDepotModule::Pump:
		Out.LengthUu = 300.0;
		Out.WidthUu = 200.0;
		return Out;
	}

	// A module added to the enum with no entry here gets a bay-sized box rather than a
	// zero-sized one, which would sample into every other module without ever colliding.
	Out.LengthUu = 400.0;
	Out.WidthUu = 400.0;
	return Out;
}

int32 DepotYardSeed(FVector2D Where)
{
	const int32 X = FMath::RoundToInt(Where.X);
	const int32 Y = FMath::RoundToInt(Where.Y);
	return static_cast<int32>(HashCombine(GetTypeHash(X), GetTypeHash(Y)));
}
