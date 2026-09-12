#include "UIStyle.h"

#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIStyle, Log, All);

void UUIStyle::ApplyText(UTextBlock& TextBlock, EUITextRole Role, FLinearColor Colour) const
{
	// Title and Clock read off TitleFont; everything smaller off LabelFont - the same split
	// the eleven call sites made by hand. Falls back to the widget's own font when the asset
	// carries none, same as every site this replaces did.
	const bool bUsesTitleFont = (Role == EUITextRole::Title || Role == EUITextRole::Clock);
	const FSlateFontInfo& BaseFont = bUsesTitleFont ? TitleFont : LabelFont;
	FSlateFontInfo Font = BaseFont.HasValidFont() ? BaseFont : TextBlock.GetFont();

	switch (Role)
	{
	case EUITextRole::Heading:
		Font.Size = HeadingSize;
		// A HEADING READS AS A HEADING, NOT AS A SHORT LABEL. Carried over from
		// UBuildBarWidget's original comment on this exact literal.
		Font.LetterSpacing = 120;
		break;
	case EUITextRole::Label: Font.Size = LabelSize; Font.LetterSpacing = 0; break;
	case EUITextRole::Body:  Font.Size = BodySize;  Font.LetterSpacing = 0; break;
	case EUITextRole::Title: Font.Size = TitleSize; Font.LetterSpacing = 0; break;
	case EUITextRole::Clock: Font.Size = ClockSize; Font.LetterSpacing = 0; break;
	}

	TextBlock.SetFont(Font);
	TextBlock.SetColorAndOpacity(FSlateColor(Colour));
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

const UUIStyle* UAirportMgrUISettings::ResolveStyle()
{
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
