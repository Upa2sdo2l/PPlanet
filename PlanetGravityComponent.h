// PlanetGravityComponent.h
// Pulls its owner toward the centre of a procedural planet.
//
//  * A Character: the CharacterMovementComponent's gravity direction is set
//    every tick (custom gravity, UE 5.4+). Walking, jumping, floor detection
//    and the capsule's orientation all follow it. Magnitude unchanged:
//    world gravity x the movement component's GravityScale.
//  * Any other actor whose root component simulates physics: engine gravity
//    is switched off for that body and the planet's pull is applied as an
//    acceleration (world gravity x GravityScale).
//
// Add it to the character Blueprint (and to physics props). For the camera to
// turn with the planet's "up", use APlanetPlayerController. For movement
// input, take the directions from GetMovementBasis instead of the template's
// "Get Forward/Right Vector of the control rotation's yaw".
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PlanetGravityComponent.generated.h"

class AProceduralPlanet;

UCLASS(ClassGroup=(Planet), meta=(BlueprintSpawnableComponent))
class FASTNOISETEST_API UPlanetGravityComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UPlanetGravityComponent();

    // The planet to fall toward. Empty = the nearest AProceduralPlanet.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet Gravity")
    TObjectPtr<AProceduralPlanet> Planet;

    // Physics bodies only: multiplier of the world gravity. Characters use
    // their movement component's GravityScale.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet Gravity", meta=(ClampMin="0.0"))
    float GravityScale = 1.f;

    // Unit vector toward the planet's centre.
    UFUNCTION(BlueprintPure, Category="Planet Gravity")
    FVector GetGravityDirection() const { return GravityDir; }

    // Local "up": away from the planet's centre.
    UFUNCTION(BlueprintPure, Category="Planet Gravity")
    FVector GetUpVector() const { return -GravityDir; }

    // Directions for movement input: the forward and right of the owner's
    // control rotation, laid flat on the planet's surface. In the Move input
    // event use these instead of the yaw-based forward/right vectors.
    UFUNCTION(BlueprintPure, Category="Planet Gravity")
    void GetMovementBasis(FVector& Forward, FVector& Right) const;

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

private:
    AProceduralPlanet* FindNearestPlanet() const;
    void UpdateGravityDirection();

    FVector GravityDir = FVector::DownVector;
};
