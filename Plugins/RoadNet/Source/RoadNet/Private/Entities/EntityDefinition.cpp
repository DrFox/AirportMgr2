#include "Entities/EntityDefinition.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildCodeCStand(Definition);

	// A stand with no design aircraft draws no envelope and offers no service positions,
	// which in a test reads as "the feature is broken" rather than "the fixture is thin".
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);
	Definition->DesignAircraft = A320;

	return Definition;
}

void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// A Code C contact stand: the ground half of a turnaround.
	//
	// ORIGIN is the NOSE GEAR STOP POSITION - the mark painted on the apron that a docking
	// guidance system stops the aircraft at. An aircraft parked here shares this pose
	// exactly, which is why composing a stand with an aircraft needs no offset.
	//
	// WHAT IS HERE is ground-fixed only: plant dug into the concrete and boxes painted on
	// it. Where a service connects to the AIRCRAFT is on UAircraftType, because an A320 and
	// a 737-800 both park here and their doors are metres apart.
	//
	// +X faces the terminal, +Y is starboard of a parked aircraft. Units are uu, and a uu
	// here is a centimetre.
	auto AddFixture = [Definition](const TCHAR* Id, double X, double Y, double HeadingDegrees,
		EServiceRole Role)
	{
		FEntityAnchor Fixture;
		Fixture.Id = FName(Id);
		Fixture.LocalPosition = FVector2D(X, Y);
		Fixture.LocalHeading = FMath::DegreesToRadians(HeadingDegrees);
		Fixture.Role = Role;
		Definition->Anchors.Add(Fixture);
	};

	// Fuel hydrant pit, under where the starboard wing falls. A stand with one takes a
	// dispenser; a stand without needs a bowser, which is a different journey entirely.
	AddFixture(TEXT("HydrantPit"), -1200.0, 700.0, -90.0, EServiceRole::Fuel);

	// Fixed ground power at the bridge, off the port bow.
	AddFixture(TEXT("FixedGPU"), 300.0, -600.0, 90.0, EServiceRole::GPU);

	// Painted equipment boxes. These are on the concrete, so they belong to the stand even
	// though what parks in them serves the aircraft.
	AddFixture(TEXT("EquipmentFwd"), -300.0, 1100.0, -90.0, EServiceRole::Baggage);
	AddFixture(TEXT("EquipmentAft"), -2100.0, 1100.0, -90.0, EServiceRole::Baggage);

	// Where the tug waits before pushback, clear of the nose.
	AddFixture(TEXT("TugStand"), 1400.0, -600.0, 180.0, EServiceRole::Tug);

	// What this stand can provide at all. A contact stand does the lot.
	Definition->AvailableServices = {
		EServiceRole::Aircraft, EServiceRole::Fuel, EServiceRole::Baggage,
		EServiceRole::Tug, EServiceRole::GPU, EServiceRole::Passenger, EServiceRole::Crew };
}

bool UEntityDefinition::HasUsableAnchorIds(const UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return false;
	}

	TSet<FName> Seen;
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		if (Anchor.Id.IsNone() || Seen.Contains(Anchor.Id))
		{
			return false;
		}
		Seen.Add(Anchor.Id);
	}
	return true;
}


