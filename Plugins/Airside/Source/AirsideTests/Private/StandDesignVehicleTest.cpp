#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Vehicle.h"
#include "Model/VehicleEnvelope.h"
#include "Model/VehicleFit.h"
#include "Solve/IcaoCode.h"
#include "Solve/TowReverse.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// PER-LETTER STAND DESIGN VEHICLE (spec 2026-09-26 §1): what a stand's own geometry is
// sized for, by its letter - the utility tow for A/B, the fuel truck from C up - and the two
// VehicleFit helpers a stand's layout compares vehicles with: NoLargerThan (may a smaller
// vehicle serve a bigger stand) and TightestReverseRadius (how tight a tow may back).
//
// FVehicleNoLargerThanTest DOES NOT MATCH THE DESIGN DOC'S PREDICTION - see its own comment
// and the spec's §3, which records the ruling (2026-09-26). Left in, pinning the measured
// fact, rather than adjusted to force the doc's guess true.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandDesignVehiclePerLetterTest,
	"Airside.Content.StandDesignVehicle.PerLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandDesignVehiclePerLetterTest::RunTest(const FString& Parameters)
{
	// THE USER'S RULING 2026-09-26: a letter is designed for the largest vehicle it admits -
	// the utility tow on A/B, the fuel truck from C up.
	const FName Tow = UAirsideSettings::ResolveUtilityTowVehicle().TypeCode;
	const FName Truck = UAirsideSettings::ResolveDefaultVehicle().TypeCode;
	TestEqual(TEXT("A is the tow's"), UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::A).TypeCode, Tow);
	TestEqual(TEXT("B is the tow's"), UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::B).TypeCode, Tow);
	for (EIcaoCode L : { EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		TestEqual(FString::Printf(TEXT("%s is the truck's"), IcaoCode::ToLetter(L)),
			UAirsideSettings::ResolveStandDesignVehicle(L).TypeCode, Truck);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmitsEverySmallerLetterTest,
	"Airside.Content.StandDesignVehicle.EveryLetterAdmitsEverySmallerLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandAdmitsEverySmallerLetterTest::RunTest(const FString& Parameters)
{
	// THE USER'S RULING 2026-10-03: "A stand should not refuse the lower vehicles; they are just less
	// efficient at doing their job." Every letter's stand - the transient template the drawn-stand tool
	// places, asked through the production reader the bid's hook calls - admits its own letter's design
	// vehicle AND every smaller letter's. Measured on the stand, not on the resolve: a set that only
	// the resolve knew about would admit a tow onto lanes nobody laid for it.
	//
	// RED BEFORE THE FIX: a C-F stand was laid for and admitted only the truck, and the utility tow's
	// chain (575) is longer than the truck's wheelbase (355) - VehicleTooLarge on every C-F stand,
	// "Accept (no fuel)" on every C+ offer in PIE with a depot full of fuel (2026-10-03).
	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	const FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	for (int32 Index = 0; Index <= static_cast<int32>(EIcaoCode::F); ++Index)
	{
		const EIcaoCode Letter = static_cast<EIcaoCode>(Index);
		const UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(Letter);
		const TArray<FVehicle> Admitted = UAirsideSettings::ResolveStandVehiclesOf(Stand, Letter);
		const FVehicleEnvelope Ceiling = FVehicleEnvelope::Of(Admitted);
		if (!TestTrue(*FString::Printf(TEXT("Code %s: the stand carries its admitted set"), IcaoCode::ToLetter(Letter)),
			Stand->AdmittedVehicles.Num() > 0))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Code %s: its design vehicle is the set's first"), IcaoCode::ToLetter(Letter)),
			Stand->AdmittedVehicles[0].TypeCode, Stand->DesignVehicle.TypeCode);
		for (int32 Smaller = 0; Smaller <= Index; ++Smaller)
		{
			const FVehicle Kind = UAirsideSettings::ResolveStandDesignVehicle(static_cast<EIcaoCode>(Smaller));
			TestTrue(*FString::Printf(TEXT("Code %s's stand admits Code %s's design vehicle, %s"), IcaoCode::ToLetter(Letter),
				IcaoCode::ToLetter(static_cast<EIcaoCode>(Smaller)), *Kind.TypeCode.ToString()), Ceiling.Admits(Kind));
		}
		// AND STILL REFUSES WHAT IT WAS NOT LAID FOR: a B stand's lanes are the tow's, and the truck turns wider and is
		// wider - the ruling admits SMALLER vehicles, not every vehicle.
		if (Letter <= EIcaoCode::B)
		{
			TestFalse(*FString::Printf(TEXT("Code %s's stand still refuses the truck"), IcaoCode::ToLetter(Letter)),
				Ceiling.Admits(Truck));
		}
	}
	// NOT VACUOUS: the case the report was about, by name.
	TestTrue(TEXT("a Code C stand admits the utility tow"),
		FVehicleEnvelope::Of(UAirsideSettings::ResolveStandVehiclesOf(UEntityDefinition::MakeStandTransient(EIcaoCode::C), EIcaoCode::C)).Admits(Tow));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleNoLargerThanTest,
	"Airside.Model.VehicleFit.NoLargerThanChecksEveryAxis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVehicleNoLargerThanTest::RunTest(const FString& Parameters)
{
	// MEASURED, NOT ASSUMED (2026-09-26 design doc §2 predicted "NoLargerThan orders tow <
	// truck"; it does not, for these authored figures, and this test PINS THE FACT rather than
	// forcing the prediction - see the spec's §3 for the ruling that keeps it strict, and what it
	// leaves). The tow is narrower (WidestBody 172.6 vs 226.0) and turns tighter both forward
	// (211 vs 502) and backward (272 vs 355), but its drawbar combination reaches 575 uu from
	// steered axle to rearmost axle (ChainLength - the reverse pull-past a route needs) against
	// the rigid truck's bare 355 uu wheelbase. NoLargerThan is an AND of all four axes (the
	// design doc's own list), so the longer chain alone refuses the substitution either way.
	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	const FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	TestFalse(TEXT("the tow's own chain (575) outreaches the truck's wheelbase (355), so it does not fit a truck stand"),
		VehicleFit::NoLargerThan(Tow, Truck));
	TestFalse(TEXT("truck never fits a tow stand"), VehicleFit::NoLargerThan(Truck, Tow));
	TestTrue(TEXT("reflexive"), VehicleFit::NoLargerThan(Truck, Truck));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseRadiusTest,
	"Airside.Model.VehicleFit.TowReverseRadiusHoldsTheHitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTowReverseRadiusTest::RunTest(const FString& Parameters)
{
	// A trailer reversing on this arc holds a steady hitch inside the margin - the figure the
	// stand's reverse leg is laid at, so a tighter one would be a leg TowReverse refuses.
	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	const double R = VehicleFit::TightestReverseRadius(Tow);
	TestTrue(TEXT("positive"), R > 0.0);
	TestTrue(TEXT("no tighter than the rigid limit"), R >= Tow.Chassis.TightestReversibleRadius());
	const VehicleSweep::FBody Rev = TowReverse::ReverseBody(VehicleFit::BodyOf(Tow));
	const double Crit = TowReverse::CriticalHitchRadians(Rev,
		FMath::DegreesToRadians(Tow.Chassis.Ground.MaxSteerDegrees));
	TestTrue(TEXT("hitch inside margin at R"),
		FMath::Abs(TowReverse::SteadyHitchRadians(Rev, 1.0 / R)) <= VehicleFit::TowReverseHitchMargin * Crit + 1e-6);
	const FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	TestEqual(TEXT("rigid = L/tan"), VehicleFit::TightestReverseRadius(Truck), Truck.Chassis.TightestReversibleRadius());
	return true;
}

#endif
