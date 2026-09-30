#include "OpsRuntimeResolver.h"

#include "Engine/World.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"

namespace
{
	using FRuntimeOverrides = TMap<TWeakObjectPtr<const UWorld>, TWeakObjectPtr<UOpsRuntime>>;

	/** A function-local static, constructed on first use - the order two translation units' statics would be in is nobody's to
	 *  rely on. Game thread only, like the subsystem it stands in for. */
	FRuntimeOverrides& OpsRuntimeOverrides()
	{
		static FRuntimeOverrides Overrides;
		return Overrides;
	}
}

UOpsRuntime* OpsRuntimeResolver::Resolve(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	if (const TWeakObjectPtr<UOpsRuntime>* Found = OpsRuntimeOverrides().Find(TWeakObjectPtr<const UWorld>(World)))
	{
		if (UOpsRuntime* Override = Found->Get())
		{
			return Override;
		}
	}
	return UOpsRuntimeSubsystem::Get(World);
}

void OpsRuntimeResolver::SetOverrideForTest(const UWorld* World, UOpsRuntime* Runtime)
{
	FRuntimeOverrides& Overrides = OpsRuntimeOverrides();
	// A torn-down world's entry is dead weight and nothing else will remove it, so each set sweeps the ones whose world or runtime
	// has gone - the map stays as long as the tests that are alive.
	for (auto It = Overrides.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !It.Value().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	if (World == nullptr)
	{
		return;
	}
	if (Runtime == nullptr)
	{
		Overrides.Remove(TWeakObjectPtr<const UWorld>(World));
		return;
	}
	Overrides.Add(TWeakObjectPtr<const UWorld>(World), TWeakObjectPtr<UOpsRuntime>(Runtime));
}
