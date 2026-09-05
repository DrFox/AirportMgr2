#include "Model/OpsDefinition.h"

FPrimaryAssetId UOpsDefinition::GetPrimaryAssetId() const
{
	// GetClass()->GetFName() is already "Scenario": UHT strips the U prefix from the
	// reflected class name. Stated because it is easy to expect "UScenario" here.
	return FPrimaryAssetId(GetClass()->GetFName(), GetFName());
}
