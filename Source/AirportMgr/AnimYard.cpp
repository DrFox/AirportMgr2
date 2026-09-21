#include "AnimYard.h"

#include "AnimYardCatalogue.h"
#include "RoadBuildLog.h"

#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Present/RoadAgentActor.h"

AAnimYard::AAnimYard()
{
	PrimaryActorTick.bCanEverTick = true;

	// THE PRODUCTION ANSWER, set here rather than branched on at the call site. A null
	// resolver and "the catalogue" would be two states for one thing, and AdoptSubjects would
	// have to know which it was looking at.
	RigResolver = &AnimYardCatalogue::FindRigFor;
}

void AAnimYard::BeginPlay()
{
	Super::BeginPlay();

	const int32 Driven = AdoptSubjects();

	// EVERY UNDRIVEN MODEL NAMED, not just counted. "9 of 10 driven" leaves you to work out
	// which one, in a yard where three models are legitimately undriven and a fourth being
	// undriven is the bug. The names are the whole value of the line.
	FString Undriven;
	for (const FYardSubject& Subject : SubjectList)
	{
		if (Subject.Agent == nullptr && Subject.Source != nullptr)
		{
			Undriven += FString::Printf(TEXT("'%s' "), *NameOf(Subject.Source));
		}
	}

	UE_LOG(LogRoadBuild, Log, TEXT("Anim yard ready: %d model(s), %d driven%s%s"),
		SubjectList.Num(), Driven,
		Undriven.IsEmpty() ? TEXT("") : TEXT(". No anim class for: "), *Undriven);
}

void AAnimYard::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// ADVANCE THEN PUSH, unconditionally. Advance is a no-op while paused - see
	// FYardMotion::Advance - and pushing anyway is what keeps a paused yard showing the values
	// the HUD is printing rather than whatever each agent last happened to be told.
	Motion.Advance(DeltaSeconds);
	PushMotion();
}

int32 AAnimYard::AdoptSubjects()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0;
	}

	int32 NewlyDriven = 0;

	for (TActorIterator<ASkeletalMeshActor> It(World); It; ++It)
	{
		ASkeletalMeshActor* Source = *It;
		if (Source == nullptr)
		{
			continue;
		}

		// ALREADY ADOPTED IS SKIPPED, which is the whole of the idempotence BeginPlay's caller
		// is promised. A second pass that spawned a second agent would put two aeroplanes on
		// one mark, z-fighting, and the yard would look like a rendering fault.
		const bool bKnown = SubjectList.ContainsByPredicate(
			[Source](const FYardSubject& Subject) { return Subject.Source == Source; });
		if (bKnown)
		{
			continue;
		}

		FYardSubject Subject;
		Subject.Source = Source;

		const FVector At = Source->GetActorLocation();
		Subject.Mark = FVector2D(At.X, At.Y);
		Subject.MarkZ = At.Z;
		Subject.Heading = FMath::DegreesToRadians(Source->GetActorRotation().Yaw);

		USkeletalMeshComponent* Component = Source->GetSkeletalMeshComponent();
		USkeletalMesh* Mesh = Component != nullptr ? Component->GetSkeletalMeshAsset() : nullptr;

		FYardRig Rig;
		const bool bDrivable = RigResolver && RigResolver(Mesh, Rig) && Rig.AnimClass != nullptr;

		if (bDrivable)
		{
			ARoadAgentActor* Agent = World->SpawnActor<ARoadAgentActor>(At, Source->GetActorRotation());
			if (Agent != nullptr)
			{
				// THE TWO DRESSING CALLS STAY APART, because ARoadAgentActor keeps them apart -
				// see SetVehicleAirframe's own header for why they are not one overload.
				if (Rig.bIsVehicle)
				{
					Agent->SetVehicleAirframe(Mesh, Rig.AnimClass, Rig.BoxSizeUu);
				}
				else
				{
					Agent->SetAirframe(Mesh, Rig.AnimClass);
				}

				// HIDDEN, NOT DESTROYED. The placed actor is the yard's record of where this
				// model stands and what it is called; destroying it would make the adoption
				// one-way and leave nothing to re-adopt from. Hiding it also stops the bind-pose
				// copy standing inside the animated one, which reads as a rig that half-works.
				Source->SetActorHiddenInGame(true);

				Subject.Agent = Agent;
				Subject.Gear = Rig.Gear;
				++NewlyDriven;
			}
		}

		SubjectList.Add(Subject);
	}

	// PUSHED ONCE HERE so a model adopted this frame is posed on its mark immediately. Without
	// it a newly spawned agent sits at the world origin in bind pose until the next Tick -
	// one frame, but the frame a screenshot is most likely to catch.
	PushMotion();

	return NewlyDriven;
}

ARoadAgentActor* AAnimYard::AgentFor(const AActor* Source) const
{
	for (const FYardSubject& Subject : SubjectList)
	{
		if (Subject.Source == Source)
		{
			return Subject.Agent;
		}
	}
	return nullptr;
}

FString AAnimYard::NameOf(const AActor* Actor)
{
	if (Actor == nullptr)
	{
		return TEXT("(none)");
	}
#if WITH_EDITOR
	return Actor->GetActorLabel();
#else
	return Actor->GetName();
#endif
}

void AAnimYard::SetSolo(const AActor* Source)
{
	SoloSource = Source;

	// PUSHED AT ONCE rather than left to the next Tick, so the row parks on the keypress. A
	// frame of the old pose after a deliberate change reads as the key not having worked.
	PushMotion();
}

const AActor* AAnimYard::NearestSubject(FVector2D Point) const
{
	const AActor* Best = nullptr;
	double BestDistanceSq = TNumericLimits<double>::Max();

	for (const FYardSubject& Subject : SubjectList)
	{
		if (Subject.Source == nullptr)
		{
			continue;
		}

		// AGAINST THE MARK, NOT THE MODEL'S BOUNDS. A 737 and a tug stand in the same row and
		// the aeroplane's bounds reach halfway to its neighbour; nearest-by-bounds would make
		// the big models almost impossible to look past. The mark is where the label is, which
		// is what "that one" means when you are pointing a camera at a row.
		const double DistanceSq = FVector2D::DistSquared(Subject.Mark, Point);
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			Best = Subject.Source;
		}
	}

	return Best;
}

void AAnimYard::PushMotion()
{
	// THE RESTING STATE, built once. A default FYardMotion is exactly the parked aeroplane -
	// wheels stopped, nosewheel centred, gear down, engine off - which is what everything the
	// solo key did not pick is set to. See SetSolo for why they park rather than freeze.
	const FYardMotion Parked;

	for (const FYardSubject& Subject : SubjectList)
	{
		if (Subject.Agent == nullptr)
		{
			continue;
		}

		const bool bDriven = SoloSource == nullptr || Subject.Source == SoloSource;

		// THE PARTS COME FROM THE BENCH AND THE POSE FROM THE MARK. FYardMotion deliberately
		// sets no position, heading, altitude or pitch at all (see its header); if it did, ten
		// agents would share one pose and stand in one place.
		FAgentMotion Pose = (bDriven ? Motion : Parked).ToAgentMotion(Subject.Gear);
		Pose.Position = Subject.Mark;
		Pose.Heading = Subject.Heading;

		Subject.Agent->SetMotion(Pose, Subject.MarkZ);
	}
}
