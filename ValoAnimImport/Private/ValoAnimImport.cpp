#include "ValoAnimImport.h"
#include "ValoImportServer.h"
#include "ValoDropListener.h"
#include "HAL/IConsoleManager.h"

static TUniquePtr<FValoImportServer> GValoImportServer;

void FValoAnimImportModule::StartupModule()
{
    // Without this, Interchange routes .psa/.ueanim to Alembic ("not a valid Alembic")
    // and never reaches UValoPsaFactory / UValoUeAnimFactory.
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
            TEXT("Interchange.FeatureFlags.Import.Enable")))
    {
        CVar->Set(0, ECVF_SetByCode);
    }

    GValoImportServer = MakeUnique<FValoImportServer>();
    if (!GValoImportServer->Start())
    {
        UE_LOG(LogTemp, Warning, TEXT("ValoImportServer failed to bind 127.0.0.1:8765"));
    }
    FValoDropListener::Start();
}

void FValoAnimImportModule::ShutdownModule()
{
    FValoDropListener::Stop();
    if (GValoImportServer)
    {
        GValoImportServer->StopServer();
        GValoImportServer.Reset();
    }
}

IMPLEMENT_MODULE(FValoAnimImportModule, ValoAnimImport)
