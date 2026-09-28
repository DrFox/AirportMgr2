#include "Tool/ModeAxis.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace ModeAxis
{
	FName AxisId()
	{
		return TEXT("Mode");
	}

	FName OptionId(EToolMode Mode)
	{
		return Mode == EToolMode::Upgrade ? FName(TEXT("Upgrade")) : FName(TEXT("Build"));
	}

	void AppendAxis(TArray<FToolVariantAxis>& Out, EToolMode Current)
	{
		FToolVariantAxis& Axis = Out.AddDefaulted_GetRef();
		Axis.Id = AxisId();
		Axis.Label = LOCTEXT("VariantAxisMode", "Mode");
		// ENUM ORDER, so an option's index IS the mode (ModeAt) - no second table to agree with.
		for (const EToolMode Each : { EToolMode::Build, EToolMode::Upgrade })
		{
			FToolVariant& Option = Axis.Options.AddDefaulted_GetRef();
			Option.Id = OptionId(Each);
			Option.Label = FText::FromName(Option.Id);
		}
		Axis.Current = static_cast<int32>(Current);
	}

	EToolMode ModeAt(int32 Option)
	{
		return Option == static_cast<int32>(EToolMode::Upgrade) ? EToolMode::Upgrade : EToolMode::Build;
	}
}

#undef LOCTEXT_NAMESPACE
