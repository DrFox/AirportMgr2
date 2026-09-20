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

TArray<PlotYard::FKitSpec> DepotKitSpecs(const UAirsideContent* Content)
{
	TArray<PlotYard::FKitSpec> Specs;

	// WALKED, not listed. Pump is the last value; adding a module after it extends this loop
	// with no edit, and adding one BEFORE it cannot be forgotten because the index into this
	// array is the enum value itself.
	for (int32 Raw = 0; Raw <= static_cast<int32>(EDepotModule::Pump); ++Raw)
	{
		const EDepotModule Module = static_cast<EDepotModule>(Raw);

		PlotYard::FKitSpec Spec;
		Spec.Footprint = DepotFootprint(Module, Content);

		// THE GREY-BOX MIX, and it lives here rather than only on the assets so that an
		// unauthored game still reserves a sensible depot instead of one of everything. A
		// shed comes up three times a cycle and runs three bays; a pump comes up once and
		// never groups. These are the figures the design doc names.
		switch (Module)
		{
		case EDepotModule::Shed: Spec.ReserveWeight = 3; Spec.RunCap = 3; break;
		case EDepotModule::Tank: Spec.ReserveWeight = 2; Spec.RunCap = 1; break;
		case EDepotModule::Pump: Spec.ReserveWeight = 1; Spec.RunCap = 1; break;
		default:                 Spec.ReserveWeight = 1; Spec.RunCap = 1; break;
		}

		// AN AUTHORED KIT OVERRIDES BOTH. Tuning the mix is then one number on an asset
		// rather than a recompile, which is the whole reason the weight lives on the kit.
		if (Content != nullptr)
		{
			if (const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(Module))
			{
				if (const UPlotModuleKit* Kit = *Found)
				{
					Spec.ReserveWeight = FMath::Max(Kit->ReserveWeight, 0);
					Spec.RunCap = FMath::Max(Kit->RunCap, 1);
					Spec.ApronUu = Kit->ApronUu;
				}
			}
		}

		Specs.Add(Spec);
	}

	return Specs;
}

FString DepotKitLabel(EDepotModule Module)
{
	switch (Module)
	{
	case EDepotModule::Shed: return TEXT("Sheds");
	case EDepotModule::Tank: return TEXT("Tanks");
	case EDepotModule::Pump: return TEXT("Pumps");
	}

	// A module added to the enum with no label here says SOMETHING rather than nothing: a
	// blank row on the bar reads as a broken readout, and the number beside it is still true.
	return TEXT("Modules");
}

int32 DepotYardSeed(FVector2D Where)
{
	const int32 X = FMath::RoundToInt(Where.X);
	const int32 Y = FMath::RoundToInt(Where.Y);
	return static_cast<int32>(HashCombine(GetTypeHash(X), GetTypeHash(Y)));
}
