#include "UIStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/TextBlock.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "Engine/FontFace.h"
#include "Engine/Texture2D.h"
#include "Fonts/CompositeFont.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIStyle, Log, All);

void UUIStyle::ApplyText(UTextBlock& TextBlock, EUITextRole Role, FLinearColor Colour) const
{
	// Heading, Title and Clock SemiBold; Label and Body Regular - the weight split the old
	// TitleFont/LabelFont pair made, now read off one composite. No faces (the CDO) keeps the
	// widget's own engine font, same as every site this replaced did.
	const bool bHeavy = (Role == EUITextRole::Heading || Role == EUITextRole::Title || Role == EUITextRole::Clock);
	FSlateFontInfo Font = TextBlock.GetFont();
	if (const TSharedPtr<const FCompositeFont> Inter = Composite())
	{
		Font = FSlateFontInfo(Inter, Font.Size, bHeavy ? FName(TEXT("SemiBold")) : FName(TEXT("Regular")));
	}
	Font.LetterSpacing = 0;

	// Only Heading is tracked out; every other role is Inter's own 0. This used to defer to
	// whatever the asset's TitleFont/LabelFont carried, so as not to override a value nothing
	// told it to touch - those two fields are gone (nothing ever set them), so there is no
	// asset value left to respect, and a block reused across roles must not keep Heading's 120.
	switch (Role)
	{
	case EUITextRole::Heading:
		Font.Size = HeadingSize;
		// A HEADING READS AS A HEADING, NOT AS A SHORT LABEL. Carried over from
		// UBuildBarWidget's original comment on this exact literal.
		Font.LetterSpacing = 120;
		break;
	case EUITextRole::Label: Font.Size = LabelSize; break;
	case EUITextRole::Body:  Font.Size = BodySize;  break;
	case EUITextRole::Title: Font.Size = TitleSize; break;
	case EUITextRole::Clock: Font.Size = ClockSize; break;
	}

	TextBlock.SetFont(Font);
	TextBlock.SetColorAndOpacity(FSlateColor(Colour));
}

FSlateBrush UUIStyle::ControlFill() const
{
	if (!ControlFillInstance.IsValid())
	{
		if (UMaterialInterface* Material = ButtonMaterial.LoadSynchronous())
		{
			// The transient package, not this asset: a MID outered to a saved asset would be
			// dragged into its package on the next save.
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
			Instance->SetScalarParameterValue(TEXT("RadiusPx"), ControlRadius);
			ControlFillInstance.Reset(Instance);
		}
	}
	if (ControlFillInstance.IsValid())
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(ControlFillInstance.Get());
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2D(32.0, 32.0);
		Brush.TintColor = FSlateColor(FLinearColor::White);
		return Brush;
	}
	return FSlateRoundedBoxBrush(FLinearColor::White, ControlRadius);
}

#if WITH_EDITOR
void UUIStyle::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	// ALL of them, whichever property changed: which fields feed which cache is exactly the kind
	// of knowledge that goes stale, and rebuilding one MID and one composite is free.
	ControlFillInstance.Reset();
	CompositeFontCache.Reset();
	RegularFaceRef.Reset();
	SemiBoldFaceRef.Reset();
}
#endif

TSharedPtr<const FCompositeFont> UUIStyle::Composite() const
{
	if (CompositeFontCache.IsValid())
	{
		return CompositeFontCache;
	}
	UFontFace* Regular = FontRegular.LoadSynchronous();
	UFontFace* SemiBold = FontSemiBold.LoadSynchronous();
	if (Regular == nullptr || SemiBold == nullptr)
	{
		return nullptr;
	}
	RegularFaceRef.Reset(Regular);
	SemiBoldFaceRef.Reset(SemiBold);
	TSharedRef<FCompositeFont> Built = MakeShared<FCompositeFont>();
	auto Add = [&Built](const TCHAR* Name, UFontFace* Face)
	{
		FTypefaceEntry& Entry = Built->DefaultTypeface.Fonts.AddDefaulted_GetRef();
		Entry.Name = FName(Name);
		Entry.Font = FFontData(Face);
	};
	// Regular FIRST: a typeface name that matches nothing falls back to the first entry.
	Add(TEXT("Regular"), Regular);
	Add(TEXT("SemiBold"), SemiBold);
	CompositeFontCache = Built;
	UE_LOG(LogUIStyle, Log, TEXT("UI font: Inter composite built from %s and %s"), *Regular->GetName(), *SemiBold->GetName());
	return CompositeFontCache;
}

UTexture2D* UUIStyle::IconFor(FName ActionId) const
{
	const TSoftObjectPtr<UTexture2D>* Found = IconsByActionId.Find(ActionId);
	if (Found == nullptr)
	{
		return nullptr;
	}
	// Synchronous because the bar builds once, at construction, and an icon that streams in
	// later would pop after the player has already looked at the button.
	return Found->LoadSynchronous();
}

int32 UAirportMgrUISettings::CallCountForTest = 0;

const UUIStyle* UAirportMgrUISettings::ResolveStyle()
{
	++CallCountForTest;
	const UAirportMgrUISettings* Settings = GetDefault<UAirportMgrUISettings>();
	if (Settings->Style.IsNull())
	{
		// Once, not per widget construction: an unconfigured style is a supported state.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogUIStyle, Log, TEXT("No UI Style configured; using UUIStyle's built-in defaults"));
		}
		return GetDefault<UUIStyle>();
	}

	const UUIStyle* Loaded = Settings->Style.LoadSynchronous();
	if (Loaded == nullptr)
	{
		UE_LOG(LogUIStyle, Error,
			TEXT("UI Style '%s' is configured but failed to load; falling back to built-in defaults"),
			*Settings->Style.ToString());
		return GetDefault<UUIStyle>();
	}
	return Loaded;
}
