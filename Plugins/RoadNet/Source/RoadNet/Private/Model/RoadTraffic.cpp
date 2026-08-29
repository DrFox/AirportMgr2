#include "Model/RoadTraffic.h"

int32 TraversalPriority(ETraversalClass Class)
{
	switch (Class)
	{
	case ETraversalClass::Emergency:     return 3;
	case ETraversalClass::Aircraft:      return 2;
	case ETraversalClass::Pedestrian:    return 1;
	case ETraversalClass::GroundVehicle: return 0;
	}

	// Unreachable for any declared value. Returning the lowest rank rather than asserting
	// means a future class added without updating this yields to everything, which is the
	// safe direction to be wrong in.
	return 0;
}

ETraversalClass ResolveRightOfWay(ETraversalClass A, ETraversalClass B)
{
	return TraversalPriority(A) >= TraversalPriority(B) ? A : B;
}
