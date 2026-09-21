#include "Model/RoadEntity.h"

ETraversalClass TraversalForRole(EServiceRole Role)
{
	switch (Role)
	{
	case EServiceRole::Aircraft:
		return ETraversalClass::Aircraft;

	// Every one of these is a vehicle that obeys identical movement rules and differs only
	// in the job it arrives to do. That is the whole reason the two enums are separate.
	case EServiceRole::Fuel:
	case EServiceRole::Baggage:
	case EServiceRole::Tug:
	case EServiceRole::GPU:
		return ETraversalClass::GroundVehicle;

	case EServiceRole::Passenger:
	case EServiceRole::Crew:
		return ETraversalClass::Pedestrian;
	}

	// No default above, so adding a role makes the compiler point here rather than letting
	// the new one quietly become a pedestrian.
	return ETraversalClass::GroundVehicle;
}
