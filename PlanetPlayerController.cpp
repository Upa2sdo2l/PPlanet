// PlanetPlayerController.cpp
#include "PlanetPlayerController.h"

#include "PlanetGravityComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

FVector APlanetPlayerController::GetPawnGravityDirection() const
{
    const APawn* P = GetPawn();
    if (!P) return FVector::DownVector;
    if (const UPlanetGravityComponent* G = P->FindComponentByClass<UPlanetGravityComponent>())
    {
        return G->GetGravityDirection();
    }
    if (const ACharacter* C = Cast<ACharacter>(P))
    {
        if (const UCharacterMovementComponent* Move = C->GetCharacterMovement())
        {
            return Move->GetGravityDirection();
        }
    }
    return FVector::DownVector;
}

void APlanetPlayerController::UpdateRotation(float DeltaTime)
{
    const FVector GravityDirection = GetPawnGravityDirection();

    // Current control rotation (world space).
    FRotator ViewRotation = GetControlRotation();

    // Carry the view along when "down" turns (walking around the planet).
    if (!LastFrameGravity.IsNearlyZero())
    {
        const FQuat DeltaGravity = FQuat::FindBetweenNormals(LastFrameGravity, GravityDirection);
        ViewRotation = (DeltaGravity * FQuat(ViewRotation)).Rotator();
    }
    LastFrameGravity = GravityDirection;

    // Work in the gravity-relative frame: there yaw/pitch/roll mean what they
    // mean on flat ground.
    ViewRotation = GetGravityRelativeRotation(ViewRotation, GravityDirection);

    FRotator DeltaRot(RotationInput);
    if (PlayerCameraManager)
    {
        PlayerCameraManager->ProcessViewRotation(DeltaTime, ViewRotation, DeltaRot);
    }
    ViewRotation.Roll = 0.f;   // horizon level relative to the planet

    SetControlRotation(GetGravityWorldRotation(ViewRotation, GravityDirection));

    // Pawns that turn with the controller face its yaw, upright on the
    // planet. (Characters with "Orient Rotation to Movement" are turned by
    // their movement component, which already follows custom gravity.)
    if (APawn* P = GetPawnOrSpectator())
    {
        if (P->bUseControllerRotationYaw)
        {
            P->SetActorRotation(GetGravityWorldRotation(FRotator(0.f, ViewRotation.Yaw, 0.f), GravityDirection));
        }
    }
}

FRotator APlanetPlayerController::GetGravityRelativeRotation(FRotator Rotation, FVector GravityDirection)
{
    if (!GravityDirection.Equals(FVector::DownVector))
    {
        const FQuat GravityRotation = FQuat::FindBetweenNormals(GravityDirection, FVector::DownVector);
        return (GravityRotation * Rotation.Quaternion()).Rotator();
    }
    return Rotation;
}

FRotator APlanetPlayerController::GetGravityWorldRotation(FRotator Rotation, FVector GravityDirection)
{
    if (!GravityDirection.Equals(FVector::DownVector))
    {
        const FQuat GravityRotation = FQuat::FindBetweenNormals(FVector::DownVector, GravityDirection);
        return (GravityRotation * Rotation.Quaternion()).Rotator();
    }
    return Rotation;
}
