#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/Level.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideAgentAnim.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS: the test module is a unity build, and two files' anonymous helpers of
// one name collide once they share a blob.
namespace RigActorTest
{
	/**
	 * One CURVED vehicle road, so a tow folds off the cab's line. On a straight every link
	 * shares the cab's heading, and a trailer drawn at the CAB's yaw would pass a yaw check -
	 * the test would measure nothing (memory: a green test may measure nothing). A 90 degree
	 * bend of about 40 m: a gentler 300 m curve folded the rig under 1 degree in 30 s.
	 */
	FRoutePlan CurvedRoad(ARoadNetworkActor& Actor)
	{
		Actor.PlaceNode(FVector2D(0.0, 60000.0));
		URoadNetwork& Net = *Actor.Network;
		const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived=*/false);
		const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(4000.0, 4000.0), /*bDerived=*/false);
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = FVector2D(4000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));

		FRouteQuery Query;
		Query.Errand = ERouteErrand::GraphProbe;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.Start = A;
		Query.Goal = B;
		Query.Class = ETraversalClass::GroundVehicle;
		return RouteSearch::Find(Net, Query);
	}

	/** Dispatches Vehicle on Plan and returns its view, or null. */
	ARoadAgentActor* Dispatch(ARoadNetworkActor& Actor, const FRoutePlan& Plan, const FVehicle& Vehicle)
	{
		if (!Actor.DispatchAgent(Plan, Vehicle, ETraversalClass::GroundVehicle))
		{
			return nullptr;
		}
		return Actor.GetTraffic()->GetAgentView(Actor.GetTraffic()->GetNewestAgentId());
	}

	/** The last link's heading off the cab's, degrees - how far the tow has folded. */
	double FoldDegrees(const FAgentMotion& Motion)
	{
		return Motion.Tow.Num() == 0 ? 0.0
			: FMath::Abs(FMath::RadiansToDegrees(
				FMath::FindDeltaAngleRadians(Motion.Heading, Motion.Tow.Last().Heading)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigActorTrailerOnItsLinkTest,
	"Airside.Present.RigActor.TrailerOnItsLink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigActorTrailerOnItsLinkTest::RunTest(const FString& Parameters)
{
	// THE VIEW DRAWS THE MODEL'S TRAILER, NOT ITS OWN. Each body-carrying link's mesh must stand
	// on that link's axle at that link's heading, as FRoadAgent::DescribeMotion put them - both
	// meshes' origins are their link's axle (tankTrailer1's tandem centre, fuelTrailer1's rear
	// axle). A trailer at the cab's yaw, or on the hitch, fails here by metres or degrees.
	struct FCase { const TCHAR* Name; FVehicle Vehicle; FResolvedTowView Look; };
	const FCase Cases[] = {
		{ TEXT("rig"), UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveRigView() },
		{ TEXT("utility + trailer"), UAirsideSettings::ResolveUtilityTowVehicle(),
			UAirsideSettings::ResolveUtilityTowView() },
	};

	for (const FCase& Case : Cases)
	{
		FAirsideTestWorld TestWorld;
		if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
		ARoadNetworkActor* Actor = TestWorld.Actor;
		const FRoutePlan Plan = RigActorTest::CurvedRoad(*Actor);
		if (!TestTrue(FString::Printf(TEXT("%s: the curve routes"), Case.Name), Plan.IsValid())) { return false; }

		ARoadAgentActor* View = RigActorTest::Dispatch(*Actor, Plan, Case.Vehicle);
		if (!TestNotNull(FString::Printf(TEXT("%s: a view"), Case.Name), View)) { return false; }

		// THE DRESSER PICKED THIS VEHICLE'S LOOK, not the fuel truck's - see
		// UAirsideSettings::ResolveVehicleViewFor, the one place that choice is made.
		const USkeletalMeshComponent* Cab = Cast<USkeletalMeshComponent>(View->GetRootComponent());
		TestEqual(FString::Printf(TEXT("%s: the cab wears its own mesh"), Case.Name),
			Cab != nullptr ? Cab->GetSkeletalMeshAsset() : nullptr, Case.Look.Cab.Mesh.Get());

		// Driven until the tow has visibly folded, so a yaw match is a real one, and on through
		// most of the bend, so the trailer's axle has cut in by a measurable distance. The CAB's
		// travel and each link AXLE's own path are summed alongside, for the wheels to be judged
		// against. (Stopping at 2 degrees, the utility had gone 4.8 m and its axle matched the
		// cab's distance to the tenth of a uu.)
		double CabTravel = 0.0;
		FVector2D CabWas = View->GetMotion().Position;
		TArray<double> AxleTravel;
		TArray<FVector2D> AxleWas;
		for (const FTowPose& Pose : View->GetMotion().Tow)
		{
			AxleTravel.Add(0.0);
			AxleWas.Add(Pose.Axle);
		}
		for (int32 I = 0; I < 1800 && (RigActorTest::FoldDegrees(View->GetMotion()) < 2.0 || CabTravel < 4500.0); ++I)
		{
			Actor->Tick(1.0f / 30.0f);
			CabTravel += FVector2D::Distance(View->GetMotion().Position, CabWas);
			CabWas = View->GetMotion().Position;
			for (int32 L = 0; L < AxleWas.Num() && L < View->GetMotion().Tow.Num(); ++L)
			{
				AxleTravel[L] += FVector2D::Distance(View->GetMotion().Tow[L].Axle, AxleWas[L]);
				AxleWas[L] = View->GetMotion().Tow[L].Axle;
			}
		}
		const FAgentMotion& Motion = View->GetMotion();
		TestTrue(FString::Printf(TEXT("%s: the tow folded (%.2f deg) so yaw is measured"), Case.Name,
			RigActorTest::FoldDegrees(Motion)), RigActorTest::FoldDegrees(Motion) >= 2.0);
		if (!TestEqual(FString::Printf(TEXT("%s: one pose per link"), Case.Name),
			Motion.Tow.Num(), Case.Vehicle.Tow.Num())) { return false; }

		int32 Bodies = 0;
		for (int32 Link = 0; Link < Case.Vehicle.Tow.Num(); ++Link)
		{
			USkeletalMeshComponent* Trailer = View->TrailerForTest(Link);
			if (Case.Vehicle.Tow[Link].IsBar())
			{
				TestNull(FString::Printf(TEXT("%s: bar link %d draws nothing of its own"), Case.Name, Link), Trailer);
				continue;
			}
			++Bodies;
			if (!TestNotNull(FString::Printf(TEXT("%s: body link %d has a mesh"), Case.Name, Link), Trailer)) { continue; }
			TestEqual(FString::Printf(TEXT("%s: link %d wears the resolved trailer"), Case.Name, Link),
				Trailer->GetSkeletalMeshAsset(), Case.Look.Links[Link].Mesh.Get());

			const FTowPose& Pose = Motion.Tow[Link];
			const FVector At = Trailer->GetComponentLocation();
			TestEqual(FString::Printf(TEXT("%s: link %d X on its axle"), Case.Name, Link), At.X, Pose.Axle.X, 0.01);
			TestEqual(FString::Printf(TEXT("%s: link %d Y on its axle"), Case.Name, Link), At.Y, Pose.Axle.Y, 0.01);
			TestEqual(FString::Printf(TEXT("%s: link %d yaw is its link's heading"), Case.Name, Link),
				FMath::FindDeltaAngleDegrees(Trailer->GetComponentRotation().Yaw,
					FMath::RadiansToDegrees(Pose.Heading)), 0.0, 0.01);

			// THE ANIMATION SEAM: the trailer's anim instance must read ITS link, not the cab.
			// A drawbar body's towbar angle is the bar's heading off the body's; a semi-trailer
			// has no bar and reads zero. The wheels roll by the axle's own travel.
			UAirsideAgentAnim* Anim = Cast<UAirsideAgentAnim>(Trailer->GetAnimInstance());
			if (!TestNotNull(FString::Printf(TEXT("%s: link %d animates with UAirsideAgentAnim"), Case.Name, Link), Anim)) { continue; }
			Anim->UpdateAnimation(1.0f / 30.0f, /*bNeedsValidRootMotion=*/false);
			const bool bDrawbar = Link > 0 && Case.Vehicle.Tow[Link - 1].IsBar();
			const float WantTowbar = bDrawbar
				? UAirsideAgentAnim::RelativeYawDegrees(Motion.Tow[Link - 1].Heading, Pose.Heading) : 0.0f;
			TestEqual(FString::Printf(TEXT("%s: link %d towbar angle"), Case.Name, Link),
				Anim->TowbarAngleDegrees, WantTowbar, 0.01f);
			if (bDrawbar)
			{
				TestTrue(FString::Printf(TEXT("%s: the bar has swung (%.2f deg)"), Case.Name, WantTowbar),
					FMath::Abs(WantTowbar) > 0.1f);
			}
			TestEqual(FString::Printf(TEXT("%s: link %d does not steer by the cab's wheel"), Case.Name, Link),
				Anim->SteerAngleDegrees, 0.0f);
			// THE WHEELS ROLL BY THIS AXLE'S OWN TRAVEL, over this rig's own radius. Forward
			// travel is positive (a flipped sign fails), and round a bend the trailer axle cuts
			// in, so it covers LESS than the cab (the cab's speed would fail).
			const FTowLinkView* TowView = View->FindTowLinkView(Trailer);
			if (!TestNotNull(FString::Printf(TEXT("%s: link %d found by its component"), Case.Name, Link), TowView)) { continue; }
			TestEqual(FString::Printf(TEXT("%s: link %d wheel angle is its axle travel over its radius"), Case.Name, Link),
				Anim->WheelAngleDegrees,
				UAirsideAgentAnim::WheelAngleFromTravel(TowView->RolledUu, Anim->WheelRadius), 0.01f);
			TestTrue(FString::Printf(TEXT("%s: link %d rolled forward (%.1f uu)"), Case.Name, Link, TowView->RolledUu),
				TowView->RolledUu > 0.0);
			TestEqual(FString::Printf(TEXT("%s: link %d rolled its own axle's path"), Case.Name, Link),
				TowView->RolledUu, AxleTravel.IsValidIndex(Link) ? AxleTravel[Link] : -1.0, 0.5);
			TestTrue(FString::Printf(TEXT("%s: link %d axle cut in: %.1f uu against the cab's %.1f"), Case.Name, Link,
				TowView->RolledUu, CabTravel), TowView->RolledUu < CabTravel);
			AddInfo(FString::Printf(TEXT("%s: link %d rolled %.1f uu, its axle's path %.1f, the cab %.1f"), Case.Name, Link,
				TowView->RolledUu, AxleTravel.IsValidIndex(Link) ? AxleTravel[Link] : -1.0, CabTravel));
		}
		TestEqual(FString::Printf(TEXT("%s: one mesh per body-carrying link"), Case.Name),
			View->TrailerCountForTest(), Bodies);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigActorTrailerWheelRadiusTest,
	"Airside.Present.RigActor.TrailerWheelRadiusIsMeasured",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigActorTrailerWheelRadiusTest::RunTest(const FString& Parameters)
{
	// THE WHEEL THE ANIMATION SPINS IS THE WHEEL THE MESH CARRIES - Airside.Content.
	// AirframeAxles' rule, for trailers. The hub's reference-pose height IS the radius (z = 0 is
	// the contact plane). Measured here off a NAMED bone per mesh rather than through
	// UAirsideAgentAnim::WheelHubRadius, so the rule that picks the bone is checked too. On the
	// 21 uu default the tank trailer's 53.8 uu wheels spun about 2.5x too fast.
	struct FCase { FVehicle Vehicle; FName Bone; };
	const FCase Cases[] = {
		{ UAirsideSettings::ResolveRigVehicle(), TEXT("wheel_1L") },
		{ UAirsideSettings::ResolveUtilityTowVehicle(), TEXT("wheel_RL") },
	};
	for (const FCase& Case : Cases)
	{
		FAirsideTestWorld TestWorld;
		if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
		const FRoutePlan Plan = RigActorTest::CurvedRoad(*TestWorld.Actor);
		ARoadAgentActor* View = RigActorTest::Dispatch(*TestWorld.Actor, Plan, Case.Vehicle);
		if (!TestNotNull(TEXT("a view"), View)) { return false; }

		for (int32 Link = 0; Link < Case.Vehicle.Tow.Num(); ++Link)
		{
			USkeletalMeshComponent* Trailer = View->TrailerForTest(Link);
			if (Trailer == nullptr) { continue; }
			const FString Who = Trailer->GetSkeletalMeshAsset()->GetName();
			const FReferenceSkeleton& Rig = Trailer->GetSkeletalMeshAsset()->GetRefSkeleton();
			const int32 Wheel = Rig.FindBoneIndex(Case.Bone);
			if (!TestTrue(FString::Printf(TEXT("%s has %s to measure against"), *Who, *Case.Bone.ToString()),
				Wheel != INDEX_NONE)) { continue; }
			const double Hub = FAnimationRuntime::GetComponentSpaceTransformRefPose(Rig, Wheel).GetTranslation().Z;
			const UAirsideAgentAnim* Anim = Cast<UAirsideAgentAnim>(Trailer->GetAnimInstance());
			if (!TestNotNull(FString::Printf(TEXT("%s animates"), *Who), Anim)) { continue; }
			TestEqual(FString::Printf(TEXT("%s rolls on the hub height its rig carries"), *Who),
				static_cast<double>(Anim->WheelRadius), Hub, 0.5);

			// THE TURNTABLE IS NOT DOUBLED: steer_FL/FR and towbar_yaw each take the towbar angle,
			// which is right only while the steer bones are NOT children of towbar_yaw - were they,
			// the front wheels would turn by twice the bar's angle.
			const int32 TowbarYaw = Rig.FindBoneIndex(TEXT("towbar_yaw"));
			if (TowbarYaw != INDEX_NONE)
			{
				for (const TCHAR* Steer : { TEXT("steer_FL"), TEXT("steer_FR") })
				{
					const int32 Bone = Rig.FindBoneIndex(Steer);
					TestTrue(FString::Printf(TEXT("%s: %s exists"), *Who, Steer), Bone != INDEX_NONE);
					for (int32 Up = Bone; Up != INDEX_NONE; Up = Rig.GetParentIndex(Up))
					{
						TestNotEqual(FString::Printf(TEXT("%s: %s does not hang under towbar_yaw"), *Who, Steer), Up, TowbarYaw);
					}
				}
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigActorRigidHasNoTrailerTest,
	"Airside.Present.RigActor.RigidHasNoTrailerComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigActorRigidHasNoTrailerTest::RunTest(const FString& Parameters)
{
	// A RIGID VEHICLE IS UNCHANGED: no trailer component, and exactly the one skeletal mesh it
	// always had. A trailer created for every truck would be an invisible, ticking skeletal
	// component per vehicle for nothing.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	const FRoutePlan Plan = RigActorTest::CurvedRoad(*TestWorld.Actor);
	if (!TestTrue(TEXT("the road routes"), Plan.IsValid())) { return false; }
	ARoadAgentActor* View = RigActorTest::Dispatch(*TestWorld.Actor, Plan, UAirsideSettings::ResolveDefaultVehicle());
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestWorld.Actor->Tick(1.0f / 30.0f);

	TestEqual(TEXT("no trailer"), View->TrailerCountForTest(), 0);
	TArray<USkeletalMeshComponent*> Skeletal;
	View->GetComponents(Skeletal);
	TestEqual(TEXT("only the body's own skeletal component"), Skeletal.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigActorTrailerSurvivesDuplicationTest,
	"Airside.Present.RigActor.TrailerSurvivesDuplication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigActorTrailerSurvivesDuplicationTest::RunTest(const FString& Parameters)
{
	// PIE DUPLICATES THE LEVEL (memory: transient subobject pointers reset on duplication). A
	// trailer pointer that came back naming the SOURCE's component, or the CDO's, would place a
	// mesh in another actor - or in no world. Duplicated the way PIE does, as
	// Airside.Present.DuplicatedActorOwnsItsSubobjects does for the network actor.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	const FRoutePlan Plan = RigActorTest::CurvedRoad(*TestWorld.Actor);
	if (!TestTrue(TEXT("the road routes"), Plan.IsValid())) { return false; }
	ARoadAgentActor* Source = RigActorTest::Dispatch(*TestWorld.Actor, Plan, UAirsideSettings::ResolveRigVehicle());
	if (!TestNotNull(TEXT("a rig view"), Source)) { return false; }
	USkeletalMeshComponent* SourceTrailer = Source->TrailerForTest(0);
	if (!TestNotNull(TEXT("the source has its trailer"), SourceTrailer)) { return false; }

	ARoadAgentActor* Dup = DuplicateObject<ARoadAgentActor>(Source, TestWorld.World->PersistentLevel);
	if (!TestNotNull(TEXT("the actor duplicates"), Dup)) { return false; }
	USkeletalMeshComponent* DupTrailer = Dup->TrailerForTest(0);
	if (!TestNotNull(TEXT("the duplicate has a trailer"), DupTrailer)) { return false; }
	TestNotEqual(TEXT("its own, not the source's"), DupTrailer, SourceTrailer);
	TestEqual(TEXT("owned by the duplicate"), DupTrailer->GetOuter(), static_cast<UObject*>(Dup));
	TestEqual(TEXT("wearing the same mesh"), DupTrailer->GetSkeletalMeshAsset(), SourceTrailer->GetSkeletalMeshAsset());
	TestNotNull(TEXT("and still found as link 0's view (the anim's lookup)"), Dup->FindTowLinkView(DupTrailer));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigActorTowbarAngleWrapsTest,
	"Airside.Present.RigActor.TowbarAngleWraps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigActorTowbarAngleWrapsTest::RunTest(const FString& Parameters)
{
	// Headings wrap at +-PI. A bar at 179 deg behind a body at -179 deg has swung 2 deg, not 358:
	// an unwrapped difference would spin the towbar and front wheels a full turn at the seam.
	TestEqual(TEXT("across the seam"),
		UAirsideAgentAnim::RelativeYawDegrees(FMath::DegreesToRadians(179.0), FMath::DegreesToRadians(-179.0)),
		-2.0f, 0.001f);
	TestEqual(TEXT("signed the way Heading turns"),
		UAirsideAgentAnim::RelativeYawDegrees(FMath::DegreesToRadians(30.0), FMath::DegreesToRadians(10.0)),
		20.0f, 0.001f);

	// Wheels roll by travel over radius; a zero radius is guarded, not divided by.
	TestEqual(TEXT("one circumference is a full turn"),
		UAirsideAgentAnim::WheelAngleFromTravel(2.0 * UE_DOUBLE_PI * 21.0 * 1.25, 21.0f), 90.0f, 0.01f);
	TestEqual(TEXT("backwards rolls backwards"),
		UAirsideAgentAnim::WheelAngleFromTravel(-2.0 * UE_DOUBLE_PI * 21.0 * 0.25, 21.0f), -90.0f, 0.01f);
	TestEqual(TEXT("zero radius"), UAirsideAgentAnim::WheelAngleFromTravel(100.0, 0.0f), 0.0f);
	return true;
}

#endif
