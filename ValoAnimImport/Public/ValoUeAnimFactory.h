#pragma once
#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "ValoUeAnimFactory.generated.h"

class USkeleton;

UCLASS()
class UValoUeAnimFactory : public UFactory
{
    GENERATED_BODY()
public:
    UValoUeAnimFactory();

    UPROPERTY(EditAnywhere, Category = ValoImport)
    TObjectPtr<USkeleton> TargetSkeleton;

    virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
                                       const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
};
