#include "ValoImportServer.h"
#include "ValoPsaFactory.h"
#include "ValoUeAnimFactory.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "UObject/SavePackage.h"

namespace
{
    USkeleton* ResolveSkeleton(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        return LoadObject<USkeleton>(nullptr, *Path);
    }

    USkeleton* FindAnySkeleton()
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        TArray<FAssetData> Assets;
        ARM.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Assets, true);
        return Assets.Num() ? Cast<USkeleton>(Assets[0].GetAsset()) : nullptr;
    }

    UFactory* FactoryForFile(const FString& File)
    {
        const FString Ext = FPaths::GetExtension(File).ToLower();
        if (Ext == TEXT("psa")) return NewObject<UValoPsaFactory>();
        if (Ext == TEXT("ueanim")) return NewObject<UValoUeAnimFactory>();
        return nullptr;
    }

    bool SaveAnim(UAnimSequence* Anim)
    {
        if (!Anim) return false;
        const FString PackageName = Anim->GetOutermost()->GetName();
        const FString PackageFileName = FPackageName::LongPackageNameToFilename(
            PackageName, FPackageName::GetAssetPackageExtension());
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        return UPackage::SavePackage(Anim->GetOutermost(), Anim, *PackageFileName, SaveArgs);
    }

    FString ImportOne(const FString& File, const FString& Package, const FString& Name, const FString& SkelPath)
    {
        UFactory* Factory = FactoryForFile(File);
        if (!Factory)
        {
            return FString::Printf(TEXT("unsupported extension: %s"), *FPaths::GetExtension(File));
        }

        USkeleton* Skel = ResolveSkeleton(SkelPath);
        if (!Skel)
        {
            // Hard-fail rather than bind to the wrong (e.g. FP) skeleton.
            return FString::Printf(TEXT("skeleton not found: %s"), *SkelPath);
        }

        if (UValoPsaFactory* Psa = Cast<UValoPsaFactory>(Factory)) Psa->TargetSkeleton = Skel;
        if (UValoUeAnimFactory* Uea = Cast<UValoUeAnimFactory>(Factory)) Uea->TargetSkeleton = Skel;

        FString AssetName = Name.IsEmpty() ? FPaths::GetBaseFilename(File) : Name;
        if (Package.IsEmpty()) return TEXT("package path required, e.g. /Game/.../Walk");

        UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Package, *AssetName));
        bool bCancel = false;
        UObject* Result = Factory->FactoryCreateFile(
            UAnimSequence::StaticClass(), Pkg, FName(*AssetName),
                                                     RF_Public | RF_Standalone, File, nullptr, GWarn, bCancel);

        UAnimSequence* Anim = Cast<UAnimSequence>(Result);
        if (!Anim) return TEXT("factory returned null (parse/skeleton failure)");

        SaveAnim(Anim);
        return FString::Printf(TEXT("%s/%s"), *Package, *AssetName);
    }
}

bool FValoImportServer::Start(int32 Port)
{
    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sockets) return false;

    ListenSocket = Sockets->CreateSocket(NAME_Stream, TEXT("ValoImportListen"), false);
    if (!ListenSocket) return false;

    ListenSocket->SetReuseAddr(true);
    const TSharedRef<FInternetAddr> Addr = Sockets->CreateInternetAddr();
    bool bValid = false;
    Addr->SetIp(TEXT("127.0.0.1"), bValid);
    if (!bValid)
    {
        Sockets->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
        return false;
    }
    Addr->SetPort(Port);
    if (!ListenSocket->Bind(*Addr) || !ListenSocket->Listen(8))
    {
        Sockets->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
        return false;
    }

    // non-blocking accept loop
    ListenSocket->SetNonBlocking(true);
    bStopping = false;
    Thread.Reset(FRunnableThread::Create(this, TEXT("ValoImportServer"), 0, TPri_Normal));
    UE_LOG(LogTemp, Log, TEXT("ValoImportServer listening on 127.0.0.1:%d"), Port);
    return true;
}

void FValoImportServer::StopServer()
{
    bStopping = true;
    if (Thread)
    {
        Thread->Kill(true);
        Thread.Reset();
    }
    if (ListenSocket)
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
    }
}

uint32 FValoImportServer::Run()
{
    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    while (!bStopping && ListenSocket)
    {
        bool bPending = false;
        if (ListenSocket->HasPendingConnection(bPending) && bPending)
        {
            TSharedRef<FInternetAddr> Remote = Sockets->CreateInternetAddr();
            FSocket* Client = ListenSocket->Accept(*Remote, TEXT("ValoImportClient"));
            if (Client)
            {
                Client->SetNonBlocking(false);
                Client->SetRecvErr(true);
                HandleClient(Client);
            }
        }
        else
        {
            FPlatformProcess::Sleep(0.05f);
        }
    }
    return 0;
}

void FValoImportServer::Stop()
{
    bStopping = true;
}

void FValoImportServer::HandleClient(FSocket* Client)
{
    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    TArray<uint8> Buffer;
    FString Acc;
    uint8 Chunk[4096];
    const double Start = FPlatformTime::Seconds();

    while (!bStopping && FPlatformTime::Seconds() - Start < 60.0)
    {
        int32 BytesRead = 0;
        if (!Client->Recv(Chunk, sizeof(Chunk), BytesRead) || BytesRead <= 0)
        {
            break;
        }
        Acc.Append(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Chunk), BytesRead).Get());

        int32 Newline;
        while (Acc.FindChar(TEXT('\n'), Newline))
        {
            FString Line = Acc.Left(Newline).TrimStartAndEnd();
            Acc.RightChopInline(Newline + 1);
            if (Line.IsEmpty()) continue;

            const FString Response = ProcessLine(Line) + TEXT("\n");
            FTCHARToUTF8 Utf8(*Response);
            int32 Sent = 0;
            Client->Send((const uint8*)Utf8.Get(), Utf8.Length(), Sent);
            Client->Close();
            Sockets->DestroySocket(Client);
            return;
        }
    }
    Client->Close();
    Sockets->DestroySocket(Client);
}

FString FValoImportServer::ProcessLine(const FString& Line)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return TEXT("{\"ok\":false,\"error\":\"invalid json\"}");
    }

    const FString Cmd = Root->GetStringField(TEXT("cmd"));
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

    if (Cmd == TEXT("ping"))
    {
        Out->SetBoolField(TEXT("ok"), true);
        Out->SetStringField(TEXT("pong"), TEXT("valo-anim-import"));
    }
    else if (Cmd == TEXT("import") || Cmd == TEXT("import_batch"))
    {
        TArray<FString> Files;
        if (Cmd == TEXT("import_batch"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if (Root->TryGetArrayField(TEXT("files"), Arr) && Arr)
            {
                for (const TSharedPtr<FJsonValue>& V : *Arr) Files.Add(V->AsString());
            }
        }
        else
        {
            Files.Add(Root->GetStringField(TEXT("file")));
        }

        const FString Package = Root->GetStringField(TEXT("package"));
        const FString Name = Root->HasField(TEXT("name")) ? Root->GetStringField(TEXT("name")) : FString();
        const FString Skel = Root->GetStringField(TEXT("skeleton"));

        TArray<TSharedPtr<FJsonValue>> Assets;
        TArray<FString> Errors;
        FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);

        AsyncTask(ENamedThreads::GameThread, [Files, Package, Name, Skel, &Assets, &Errors, Done]()
        {
            for (int32 i = 0; i < Files.Num(); ++i)
            {
                const FString AssetName = Files.Num() > 1 ? FString() : Name;
                const FString Result = ImportOne(Files[i], Package, AssetName, Skel);
                if (Result.StartsWith(TEXT("/Game/")))
                {
                    Assets.Add(MakeShared<FJsonValueString>(Result));
                }
                else
                {
                    Errors.Add(Result);
                }
            }
            Done->Trigger();
        });
        Done->Wait();
        FPlatformProcess::ReturnSynchEventToPool(Done);

        Out->SetBoolField(TEXT("ok"), Errors.Num() == 0);
        Out->SetArrayField(TEXT("assets"), Assets);
        if (Errors.Num())
        {
            Out->SetStringField(TEXT("error"), FString::Join(Errors, TEXT("; ")));
        }
    }
    else
    {
        Out->SetBoolField(TEXT("ok"), false);
        Out->SetStringField(TEXT("error"), FString::Printf(TEXT("unknown cmd %s"), *Cmd));
    }

    FString OutStr;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutStr);
    FJsonSerializer::Serialize(Out.ToSharedRef(), Writer);
    return OutStr;
}
