#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Pricing.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Wingspans, uu, that land squarely inside a code letter's band. */
	FAirframe AirframeOfSpan(double WingspanMetres)
	{
		FAirframe Airframe;
		Airframe.Wingspan = WingspanMetres * 100.0;
		return Airframe;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPricingByCodeLetterTest,
	"AirportOps.Model.PricingByCodeLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPricingByCodeLetterTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = NewObject<UPricing>();

	// 28 m span is code C, 10 m is code A. The band edges are IcaoCode's, not this test's -
	// that is the point of keying the fee on the letter rather than on a per-type figure.
	const double CodeC = Pricing->LandingFee(AirframeOfSpan(28.0));
	const double CodeA = Pricing->LandingFee(AirframeOfSpan(10.0));

	TestEqual(TEXT("a code C landing is the authored code C fee"), CodeC, 1200.0, 1e-6);
	TestEqual(TEXT("a code A landing is the authored code A fee"), CodeA, 150.0, 1e-6);
	TestTrue(TEXT("a bigger aeroplane pays more, which is the whole reason the fee is keyed on "
		"the code letter rather than being flat"), CodeC > CodeA);

	TestEqual(TEXT("parking is a tenth of the landing fee per hour - ONE row per letter drives "
		"all three fees, so there is no second table to drift"),
		Pricing->ParkingFeePerHour(AirframeOfSpan(28.0)), 120.0, 1e-6);
	TestEqual(TEXT("and a fuelling is half of it"),
		Pricing->FuelServiceFee(AirframeOfSpan(28.0)), 600.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPricingMultiplierTest,
	"AirportOps.Model.PricingMultiplier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPricingMultiplierTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = NewObject<UPricing>();
	const FAirframe Airframe = AirframeOfSpan(28.0);

	Pricing->LandingFeeMultiplier = 1.5;
	TestEqual(TEXT("the player's lever scales what a landing earns"),
		Pricing->LandingFee(Airframe), 1800.0, 1e-6);
	TestEqual(TEXT("and scales parking with it, so one lever cannot split the two fees apart"),
		Pricing->ParkingFeePerHour(Airframe), 180.0, 1e-6);

	// THE DELIBERATE EXCEPTION (spec D7 and the header): fuelling is a service performed, not
	// permission to land, so the landing lever must not quietly reprice it.
	TestEqual(TEXT("but a fuelling is NOT repriced by the landing lever"),
		Pricing->FuelServiceFee(Airframe), 600.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPricingElasticityTest,
	"AirportOps.Model.PricingElasticity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPricingElasticityTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = NewObject<UPricing>();
	Pricing->Elasticity = 1.0;

	Pricing->LandingFeeMultiplier = 1.0;
	TestEqual(TEXT("at the default fee, demand is unchanged"),
		Pricing->DemandFactor(), 1.0, 1e-9);

	Pricing->LandingFeeMultiplier = 2.0;
	TestEqual(TEXT("double the fee halves the offers - constant elasticity of 1"),
		Pricing->DemandFactor(), 0.5, 1e-9);

	// THE DESIGN CLAIM, ASSERTED RATHER THAN TRUSTED. At elasticity 1 the lever is revenue-
	// neutral, which is what makes it pay only when the airport is capacity-bound (spec D8). If
	// this ever fails, the lever has silently become a slider with one correct setting and the
	// decision it was built to pose has stopped existing.
	Pricing->LandingFeeMultiplier = 1.0;
	const double RevenueAtOne =
		Pricing->LandingFee(AirframeOfSpan(28.0)) * Pricing->DemandFactor();
	Pricing->LandingFeeMultiplier = 2.0;
	const double RevenueAtTwo =
		Pricing->LandingFee(AirframeOfSpan(28.0)) * Pricing->DemandFactor();
	TestEqual(TEXT("fee times demand is flat at elasticity 1, so raising the fee pays nothing "
		"while there are stands to spare"), RevenueAtTwo, RevenueAtOne, 1e-6);

	Pricing->Elasticity = 0.0;
	TestEqual(TEXT("elasticity 0 is perfectly inelastic demand: the fee does not move it"),
		Pricing->DemandFactor(), 1.0, 1e-9);

	// A FREE LANDING IS A REAL SETTING; an infinite demand is not. Without the guard in
	// DemandFactor this is Pow(0, -1) - infinity - and the offer cadence divides by it.
	Pricing->Elasticity = 1.0;
	Pricing->LandingFeeMultiplier = 0.0;
	TestTrue(TEXT("a zero fee yields a finite demand factor rather than an infinity the offer "
		"cadence would then divide by"), FMath::IsFinite(Pricing->DemandFactor()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPricingStepLandingFeeTest,
	"AirportOps.Model.PricingStepLandingFee",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPricingStepLandingFeeTest::RunTest(const FString& Parameters)
{
	// WORLD-FREE, which is the whole point of issue #191's move: StepLandingFee used to be
	// ARoadBuildController::StepLandingFee, reachable only by driving PIE with a live
	// OpsRuntime and Pricing attached.
	UPricing* Pricing = NewObject<UPricing>();
	TestEqual(TEXT("starts at the authored default"), Pricing->LandingFeeMultiplier, 1.0, 1e-9);

	Pricing->StepLandingFee(+1);
	TestEqual(TEXT("one step up is ten percent"), Pricing->LandingFeeMultiplier, 1.1, 1e-9);

	Pricing->StepLandingFee(-1);
	TestEqual(TEXT("one step down undoes it"), Pricing->LandingFeeMultiplier, 1.0, 1e-9);

	for (int32 I = 0; I < 20; ++I) { Pricing->StepLandingFee(+1); }
	TestEqual(TEXT("stepping up hits the ceiling and stops there, rather than pricing the "
		"inbox empty"), Pricing->LandingFeeMultiplier, 2.0, 1e-9);

	for (int32 I = 0; I < 20; ++I) { Pricing->StepLandingFee(-1); }
	TestEqual(TEXT("stepping down hits the floor and stops there, rather than reaching zero and "
		"making DemandFactor meaningless"), Pricing->LandingFeeMultiplier, 0.5, 1e-9);

	const double Before = Pricing->LandingFeeMultiplier;
	Pricing->StepLandingFee(0);
	TestEqual(TEXT("a zero direction is a no-op, the same guard the controller used to make "
		"before calling this"), Pricing->LandingFeeMultiplier, Before, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPricingBuildAndScrapTest,
	"AirportOps.Model.PricingBuildAndScrap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPricingBuildAndScrapTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = NewObject<UPricing>();
	Pricing->RefundFraction = 0.5;

	TestEqual(TEXT("with no modifier in play a build costs what Airside quoted"),
		Pricing->PriceOfBuild(1000.0, nullptr), 1000.0, 1e-9);
	TestEqual(TEXT("scrap is the refund fraction of TODAY's price, not of what was paid - which "
		"is what keeps a BuiltFor field out of the model"),
		Pricing->ScrapValue(1000.0, nullptr), 500.0, 1e-9);

	const FString Money = Pricing->Format(1234.0).ToString();
	TestTrue(TEXT("formatted money carries the currency symbol, so Airside never has to know "
		"what the currency is"), Money.Contains(Pricing->CurrencySymbol));

	const FString Negative = Pricing->Format(-400.0).ToString();
	TestTrue(TEXT("a negative amount reads as a negative AMOUNT, sign outside the symbol"),
		Negative.StartsWith(TEXT("-")));
	return true;
}

#endif
