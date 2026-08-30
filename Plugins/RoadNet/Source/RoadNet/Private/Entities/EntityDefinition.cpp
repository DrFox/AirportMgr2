#include "Entities/EntityDefinition.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());

	auto AddAnchor = [Definition](double X, double Y, EServiceRole Role)
	{
		FEntityAnchor Anchor;
		Anchor.LocalPosition = FVector2D(X, Y);
		Anchor.LocalHeading = 0.0;
		Anchor.Role = Role;
		Definition->Anchors.Add(Anchor);
	};

	// Nose at the origin, aircraft pointing along local +X.
	AddAnchor(   0.0,    0.0, EServiceRole::Aircraft);
	AddAnchor(-1200.0,  900.0, EServiceRole::Fuel);
	AddAnchor(-1800.0, -900.0, EServiceRole::Baggage);
	AddAnchor( 1500.0,    0.0, EServiceRole::Tug);      // ahead, for pushback
	AddAnchor(-1200.0, -900.0, EServiceRole::GPU);
	AddAnchor(-2400.0,  600.0, EServiceRole::Passenger);

	return Definition;
}
