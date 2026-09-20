#include "Build/DepotKit.h"

#include "Content/AirsideContent.h"
#include "Entities/PlotModuleKit.h"

PlotYard::FFootprint DepotFootprint(EDepotModule Module, const UAirsideContent* Content)
{
	// THE AUTHORED KIT WINS WHOLE. Taking the footprint from the kit and the back-fence flag
	// from the table below would be two sources for one module's behaviour, and they would
	// disagree the first time a kit was authored for something that does not front the gate.
	if (Content != nullptr)
	{
		if (const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(Module))
		{
			if (const UPlotModuleKit* Kit = *Found)
			{
				PlotYard::FFootprint Out;
				Out.LengthUu = Kit->Footprint.X;
				Out.WidthUu = Kit->Footprint.Y;
				Out.bAgainstTheBackFence = Kit->bAgainstTheBackFence;
				return Out;
			}
		}
	}

	// NO KIT IS A LEGAL ANSWER, not an error: the grey-box table below is what every caller
	// used before kits existed and what they still get until one is authored.
	return DepotFootprint(Module);
}

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
		// THE ONLY ONE STOOD AGAINST THE BACK FENCE. A truck drives out of it, so its heading
		// is functional and may not be turned for looks - and it stands at the back rather
		// than in the gateway, which is where it used to be.
		Out.bAgainstTheBackFence = true;
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
