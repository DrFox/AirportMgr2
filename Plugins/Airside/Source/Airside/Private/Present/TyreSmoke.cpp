#include "Present/TyreSmoke.h"

#include "AirsideLog.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/**
	 * The engine's own sphere. NOT an authored asset, for the reason
	 * ARoadNetworkActor::PlotBoxes gives about its cube: a puff of smoke is a primitive, and
	 * making a .uasset for it would be one more thing to keep in a content folder for no
	 * gain. It also means this effect works in a project with no art in it at all, which is
	 * what let it be tested before the material existed.
	 */
	const TCHAR* const PuffMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	/** The material's Age parameter - named once rather than retyped at each Set call. */
	const TCHAR* const AgeParameter = TEXT("Age");

	/** BasicShapes/Sphere is 100 uu ACROSS, so its radius at scale 1 is 50 uu. A scale is a
	 *  radius divided by this, and getting it wrong doubles every puff in the game. */
	constexpr double SphereRadiusAtUnitScale = 50.0;
}

void UTyreSmoke::Initialise(AActor* InOwner, UMaterialInterface* InMaterial)
{
	Owner = InOwner;
	Material = InMaterial;

	// Re-entrant on purpose: the actor calls this from PostRegisterAllComponents, which runs
	// again for a PIE duplicate. Rebuilding the pool each time would leak the last one's
	// components, so an existing pool is kept and only re-pointed at the material.
	if (Puffs.Num() == PoolSize && Owner != nullptr)
	{
		for (FPuff& Puff : Puffs)
		{
			if (Puff.Instance != nullptr && Material != nullptr)
			{
				Puff.Mesh->SetMaterial(0, Puff.Instance);
			}
		}
		return;
	}

	Puffs.Reset();
	PuffMeshes.Reset();
	PuffInstances.Reset();
	if (Owner == nullptr)
	{
		return;
	}

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, PuffMeshPath);
	if (Sphere == nullptr)
	{
		// Loud, not silent. A missing engine primitive is not a state to degrade through -
		// it means the engine content this build was made against is not the one running.
		UE_LOG(LogAirside, Warning, TEXT("Tyre smoke: no %s, so no puffs will be drawn."), PuffMeshPath);
		return;
	}

	for (int32 Index = 0; Index < PoolSize; ++Index)
	{
		FPuff Puff;
		Puff.Mesh = NewObject<UStaticMeshComponent>(Owner);
		Puff.Mesh->SetStaticMesh(Sphere);
		Puff.Mesh->SetupAttachment(Owner->GetRootComponent());
		Puff.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// No shadow. A translucent puff casting one would put a grey disc on the pavement
		// beside the rubber, which is the one thing on the runway it must not be confused
		// with. It is also a shadow nobody would look for and everybody would notice.
		Puff.Mesh->SetCastShadow(false);
		// ABSOLUTE, so a puff stays where it was laid while the actor it hangs off does
		// whatever it likes. The attachment exists only to give the components an owner.
		Puff.Mesh->SetAbsolute(true, true, true);
		Puff.Mesh->SetVisibility(false);
		Puff.Mesh->RegisterComponent();

		if (Material != nullptr)
		{
			Puff.Instance = UMaterialInstanceDynamic::Create(Material, this);
			Puff.Mesh->SetMaterial(0, Puff.Instance);
		}

		PuffMeshes.Add(Puff.Mesh);
		PuffInstances.Add(Puff.Instance);
		Puffs.Add(Puff);
	}
}

int32 UTyreSmoke::ClaimSlot()
{
	int32 Oldest = 0;
	double OldestAge = -1.0;
	for (int32 Index = 0; Index < Puffs.Num(); ++Index)
	{
		if (!Puffs[Index].bLive)
		{
			return Index;
		}
		if (Puffs[Index].Age > OldestAge)
		{
			OldestAge = Puffs[Index].Age;
			Oldest = Index;
		}
	}
	// STEAL THE OLDEST rather than refuse. A refused puff is a landing with no smoke, which
	// reads as the feature being broken; a stolen one is a puff that ended early, which
	// nobody can see because it was already the faintest thing on screen.
	return Oldest;
}

void UTyreSmoke::Puff(const FVector& Where, double Wingspan)
{
	if (Puffs.IsEmpty() || Material == nullptr)
	{
		return;
	}

	const int32 Slot = ClaimSlot();
	FPuff& Chosen = Puffs[Slot];

	// Wingspan as a stand-in for mass - see the header. Clamped so an unmeasured span (zero)
	// cannot collapse the puff to nothing, and so a freak value cannot fill the screen.
	const double Scale = ReferenceWingspan > 0.0
		? FMath::Clamp(Wingspan / ReferenceWingspan, 0.35, 2.5)
		: 1.0;

	Chosen.Age = 0.0;
	Chosen.Born = Where;
	Chosen.Radius = BirthRadius * Scale;
	Chosen.bLive = true;

	Chosen.Mesh->SetWorldLocation(Where);
	Chosen.Mesh->SetWorldScale3D(FVector(Chosen.Radius / SphereRadiusAtUnitScale));
	Chosen.Mesh->SetVisibility(true);
	if (Chosen.Instance != nullptr)
	{
		Chosen.Instance->SetScalarParameterValue(AgeParameter, 0.0f);
	}
}

void UTyreSmoke::Advance(double DeltaSeconds)
{
	if (PuffSeconds <= 0.0)
	{
		return;
	}

	for (FPuff& Puff : Puffs)
	{
		if (!Puff.bLive)
		{
			continue;
		}

		Puff.Age += DeltaSeconds;
		const double Fraction = Puff.Age / PuffSeconds;
		if (Fraction >= 1.0)
		{
			Puff.bLive = false;
			Puff.Mesh->SetVisibility(false);
			continue;
		}

		// GROWTH IS A TRANSFORM, not a vertex shader: scaling a component is free and moving
		// vertices is not, and there are eight of these. The material is handed only Age and
		// decides everything it does from that one number - see build_puff_material.py.
		const double Scale = ReferenceWingspan > 0.0 ? Puff.Radius / BirthRadius : 1.0;
		const double Radius = FMath::Lerp(BirthRadius, DeathRadius, Fraction) * Scale;

		Puff.Mesh->SetWorldScale3D(FVector(Radius / SphereRadiusAtUnitScale));
		Puff.Mesh->SetWorldLocation(Puff.Born + FVector(0.0, 0.0, RiseHeight * Fraction));
		if (Puff.Instance != nullptr)
		{
			Puff.Instance->SetScalarParameterValue(AgeParameter, static_cast<float>(Fraction));
		}
	}
}

int32 UTyreSmoke::LivePuffCountForTest() const
{
	int32 Live = 0;
	for (const FPuff& Puff : Puffs)
	{
		if (Puff.bLive)
		{
			++Live;
		}
	}
	return Live;
}
