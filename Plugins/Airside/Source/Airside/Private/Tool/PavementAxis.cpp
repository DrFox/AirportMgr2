#include "Tool/PavementAxis.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace Pavement
{
	void AppendAxis(TArray<FToolVariantAxis>& Out, EPavement Current, TConstArrayView<EPavement> Allowed)
	{
		const TArray<EPavement> Offer = Offered(Allowed);
		FToolVariantAxis& Axis = Out.AddDefaulted_GetRef();
		// "Surface" is the Id #353's popout and #356's Shift+key both look the row up by.
		Axis.Id = TEXT("Surface");
		Axis.Label = LOCTEXT("VariantAxisSurface", "Surface");
		Axis.Current = Offer.IndexOfByKey(Current);
		for (const EPavement Each : Offer)
		{
			const TCHAR* Spelling = Name(Each);
			FToolVariant& Option = Axis.Options.AddDefaulted_GetRef();
			Option.Id = Spelling;
			Option.Label = FText::FromString(Spelling);
		}
	}
}

#undef LOCTEXT_NAMESPACE
