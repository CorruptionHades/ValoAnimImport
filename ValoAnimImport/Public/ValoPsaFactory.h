#pragma once
#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "ValoPsaFactory.generated.h"

class USkeleton;

UCLASS()
class UValoPsaFactory : public UFactory
{
    GENERATED_BODY()
public:
    UValoPsaFactory();

    /** Skeleton to bind. If unset, first skeleton found under /Game. */
    UPROPERTY(EditAnywhere, Category = ValoImport)
    TObjectPtr<USkeleton> TargetSkeleton;

    virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
                                       const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
};
