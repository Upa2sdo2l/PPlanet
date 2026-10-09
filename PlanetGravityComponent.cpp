// PlanetGravityComponent.cpp
#include "PlanetGravityComponent.h"

#include "ProceduralPlanet.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/PrimitiveComponent.h"

UPlanetGravityComponent::UPlanetGravityComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    // Before movement and physics read the gravity direction this frame.
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

AProceduralPlanet* UPlanetGravityComponent::FindNearestPlanet() const
{
    const AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World) return nullptr;

    AProceduralPlanet* Best = nullptr;
    double BestDistSq = TNumericLimits<double>::Max();
    for (TActorIterator<AProceduralPlanet> It(World); It; ++It)
    {
        const double D = FVector::DistSquared(It->GetActorLocation(), Owner->GetActorLocation());
        if (D < BestDistSq) { BestDistSq = D; Best = *It; }
    }
    return Best;
}

void UPlanetGravityComponent::BeginPlay()
{
    Super::BeginPlay();
    if (!Planet) Planet = FindNearestPlanet();

    // The character's movement must run after this component sets the
    // direction, or it would use last frame's.
    if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
    {
        if (UCharacterMovementComponent* Move = Character->GetCharacterMovement())
        {
            Move->AddTickPrerequisiteComponent(this);
        }
    }
    UpdateGravityDirection();
}

void UPlanetGravityComponent::UpdateGravityDirection()
{
    const AActor* Owner = GetOwner();
    if (!Owner || !Planet) return;
    const FVector ToCentre = Planet->GetActorLocation() - Owner->GetActorLocation();
    if (!ToCentre.IsNearlyZero()) GravityDir = ToCentre.GetSafeNormal();
}

void UPlanetGravityComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                            FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!Planet) Planet = FindNearestPlanet();
    if (!Planet) return;

    UpdateGravityDirection();
    AActor* Owner = GetOwner();

    // Characters: custom gravity of the movement component.
    if (ACharacter* Character = Cast<ACharacter>(Owner))
    {
        if (UCharacterMovementComponent* Move = Character->GetCharacterMovement())
        {
            Move->SetGravityDirection(GravityDir);
        }
        return;
    }

    // Physics bodies: replace engine gravity (always world -Z) by the pull
    // toward the planet's centre, as an acceleration (mass-independent).
    if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Owner ? Owner->GetRootComponent() : nullptr))
    {
        if (Body->IsSimulatingPhysics())
        {
            if (Body->IsGravityEnabled()) Body->SetEnableGravity(false);
            const double G = FMath::Abs(GetWorld()->GetGravityZ()) * GravityScale;
            Body->AddForce(GravityDir * G, NAME_None, /*bAccelChange*/ true);
        }
    }
}

void UPlanetGravityComponent::GetMovementBasis(FVector& Forward, FVector& Right) const
{
    const FVector Up = -GravityDir;

    FRotator Control = FRotator::ZeroRotator;
    if (const APawn* Pawn = Cast<APawn>(GetOwner()))
    {
        Control = Pawn->GetControlRotation();
    }
    const FRotationMatrix M(Control);

    // Camera forward laid flat on the surface. Looking straight down (or up)
    // it vanishes: then the camera's own up (or down) gives the heading.
    FVector F = FVector::VectorPlaneProject(M.GetUnitAxis(EAxis::X), Up);
    if (F.SizeSquared() < 1e-4)
    {
        const FVector CamUp = M.GetUnitAxis(EAxis::Z);
        F = FVector::VectorPlaneProject(FVector::DotProduct(M.GetUnitAxis(EAxis::X), Up) < 0.0 ? CamUp : -CamUp, Up);
    }
    Forward = F.GetSafeNormal();
    Right   = FVector::CrossProduct(Up, Forward).GetSafeNormal();
}
