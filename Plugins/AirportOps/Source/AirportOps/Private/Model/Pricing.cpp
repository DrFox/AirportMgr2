#include "Model/Pricing.h"

#include "AirportOpsLog.h"
#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"

double UPricing::BaseLandingFeeForLetter(const FString& Letter) const
{
	// FIRST-PASS FIGURES, roughly doubling per letter (spec 2026-09-13 §7). The ORDERING is
	// what to defend when these are tuned - a bigger aeroplane pays more, and by a widening
	// margin; the magnitudes themselves are unplayed guesses and are expected to move.
	if (Letter == TEXT("A")) { return 150.0; }
	if (Letter == TEXT("B")) { return 400.0; }
	if (Letter == TEXT("C")) { return 1200.0; }
	if (Letter == TEXT("D")) { return 2600.0; }
	if (Letter == TEXT("E")) { return 4500.0; }
	if (Letter == TEXT("F")) { return 7000.0; }

	// C, for the same reason IcaoCode::RadiusForLetter falls back to it: the commonest stand in
	// the world. Erring here prices an unknown as an airliner rather than as a light aircraft,
	// which is the safer way round for a fee.
	return 1200.0;
}

double UPricing::LandingFee(const FAirframe& Airframe) const
{
	const FString Letter = IcaoCode::LetterForWingspan(Airframe.Wingspan);
	return BaseLandingFeeForLetter(Letter) * LandingFeeMultiplier;
}

void UPricing::StepLandingFee(int32 Direction)
{
	if (Direction == 0)
	{
		return;
	}

	// TEN PER CENT A STEP, and clamped at both ends - see this method's own header comment for
	// why. MOVED FROM ARoadBuildController::StepLandingFee by issue #191.
	constexpr double Step = 0.1;
	constexpr double Floor = 0.5;
	constexpr double Ceiling = 2.0;

	const double Was = LandingFeeMultiplier;
	LandingFeeMultiplier = FMath::Clamp(Was + (Direction > 0 ? Step : -Step), Floor, Ceiling);

	// LOGGED, because the lever changes the offer cadence for the rest of the game and "why did
	// the offers dry up" is otherwise a question the log cannot answer.
	UE_LOG(LogAirportOps, Log, TEXT("Landing fee %.0f%% -> %.0f%%"),
		Was * 100.0, LandingFeeMultiplier * 100.0);
}

double UPricing::ParkingFeePerHour(const FAirframe& Airframe) const
{
	// A TENTH OF THE LANDING FEE rather than a second authored table: one row per code letter
	// drives all three fees, so there is no second list to drift out of agreement with the
	// first when somebody adds a letter or retunes a number.
	return LandingFee(Airframe) * 0.1;
}

double UPricing::FuelServiceFee(const FAirframe& Airframe) const
{
	// Deliberately reads the BASE fee rather than LandingFee() - see the header for why the
	// player's landing lever must not silently reprice a service they performed.
	const FString Letter = IcaoCode::LetterForWingspan(Airframe.Wingspan);
	return BaseLandingFeeForLetter(Letter) * 0.5;
}

double UPricing::DemandFactor() const
{
	// GUARDED, not trusted. A zero or negative multiplier would make Pow() infinite or NaN, and
	// the offer cadence computed from it would silently stop or divide by zero. A free landing
	// is a real thing a player might set; an infinite demand is not.
	const double Multiplier = FMath::Max(LandingFeeMultiplier, UE_KINDA_SMALL_NUMBER);
	return FMath::Pow(Multiplier, -Elasticity);
}

double UPricing::PriceOfBuild(double BaseAmount, const UObject* /*Source*/) const
{
	// NO MODIFIER TODAY. This is the seam research and contracts arrive through in M4 - see the
	// header on why Source is already a parameter rather than something to thread through later.
	return BaseAmount;
}

double UPricing::ScrapValue(double BaseAmount, const UObject* Source) const
{
	return PriceOfBuild(BaseAmount, Source) * RefundFraction;
}

FText UPricing::Format(double Amount) const
{
	FNumberFormattingOptions Options;
	Options.SetMaximumFractionalDigits(0);
	Options.SetUseGrouping(true);

	// The sign goes OUTSIDE the symbol - "-¤400", not "¤-400" - because the minus is about the
	// number and the symbol is about the unit, and the other way round reads as a negative
	// currency rather than a negative amount.
	const FText Number = FText::AsNumber(FMath::Abs(Amount), &Options);
	const TCHAR* Sign = Amount < 0.0 ? TEXT("-") : TEXT("");
	return FText::FromString(FString::Printf(TEXT("%s%s%s"),
		Sign, *CurrencySymbol, *Number.ToString()));
}
