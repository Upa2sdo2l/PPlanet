// PlanetPlayerController.h
// Player controller for walking on a planet: the camera's yaw and pitch are
// relative to the local "up" (the pawn's gravity), so looking around works
// the same everywhere on the sphere, and the view turns smoothly with the
// planet as you walk. Without it, mouse yaw spins around world Z, which on
// most of the planet is not "up".
//
// Based on Epic's "Custom Gravity in UE 5.4" tutorial. Set it as the Player
// Controller Class of your GameMode.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "PlanetPlayerController.generated.h"

UCLASS()
class FASTNOISETEST_API APlanetPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    virtual void UpdateRotation(float DeltaTime) override;

    // Rotation in a frame whose "down" is GravityDirection, and back.
    UFUNCTION(BlueprintPure, Category="Planet Gravity")
    static FRotator GetGravityRelativeRotation(FRotator Rotation, FVector GravityDirection);

    UFUNCTION(BlueprintPure, Category="Planet Gravity")
    static FRotator GetGravityWorldRotation(FRotator Rotation, FVector GravityDirection);

private:
    FVector GetPawnGravityDirection() const;

    FVector LastFrameGravity = FVector::ZeroVector;
};
