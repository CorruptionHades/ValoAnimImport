#include "ValoUeAnimFactory.h"
#include "ValoSkeletonPicker.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Compression.h"
#include "zstd.h"

// Minimal headless UEANIM reader (UEFormat layout). Uncompressed payloads only
// (CUE4Parse UEFormat export defaults to uncompressed when compression=None).

namespace
{
    USkeleton* FindAnySkeleton()
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        TArray<FAssetData> Assets;
        ARM.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Assets, true);
        return Assets.Num() ? Cast<USkeleton>(Assets[0].GetAsset()) : nullptr;
    }

    int32 ReadI32(const TArray<uint8>& B, int32& Off)
    {
        int32 V = 0;
        FMemory::Memcpy(&V, B.GetData() + Off, 4);
        Off += 4;
        return V;
    }
    float ReadF32(const TArray<uint8>& B, int32& Off)
    {
        float V = 0;
        FMemory::Memcpy(&V, B.GetData() + Off, 4);
        Off += 4;
        return V;
    }
    FString ReadFString(const TArray<uint8>& B, int32& Off)
    {
        const int32 Len = ReadI32(B, Off);
        if (Len <= 0 || Off + Len > B.Num()) return FString();
        FUTF8ToTCHAR Conv((const ANSICHAR*)B.GetData() + Off, Len);
        Off += Len;
        return FString(Conv.Length(), Conv.Get());
    }
    FQuat4f ReadQuat(const TArray<uint8>& B, int32& Off)
    {
        // Sequential reads — C++ does not guarantee arg evaluation order.
        const float X = ReadF32(B, Off);
        const float Y = ReadF32(B, Off);
        const float Z = ReadF32(B, Off);
        const float W = ReadF32(B, Off);
        FQuat4f Q(X, Y, Z, W);
        Q.Normalize();
        return Q;
    }

    FVector3f ReadVec(const TArray<uint8>& B, int32& Off)
    {
        const float X = ReadF32(B, Off);
        const float Y = ReadF32(B, Off);
        const float Z = ReadF32(B, Off);
        return FVector3f(X, Y, Z);
    }
}

UValoUeAnimFactory::UValoUeAnimFactory()
{
    SupportedClass = UAnimSequence::StaticClass();
    bCreateNew = false;
    bEditorImport = true;
    Formats.Add(TEXT("ueanim; UEFormat Animation"));
}

UObject* UValoUeAnimFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
                                               const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    TArray<uint8> File;
    if (!FFileHelper::LoadFileToArray(File, *Filename) || File.Num() < 16)
    {
        return nullptr;
    }
    if (FMemory::Memcmp(File.GetData(), "UEFORMAT", 8) != 0)
    {
        return nullptr;
    }

    int32 Off = 8;
    ReadFString(File, Off); // identifier
    Off += 1;               // version
    ReadFString(File, Off); // object name
    ReadFString(File, Off); // object path (CUE4Parse / FModel)
    const bool bCompressed = File[Off++] != 0;

    TArray<uint8> Payload;
    if (bCompressed)
    {
        const FString Comp = ReadFString(File, Off);
        const int32 UncompressedSize = ReadI32(File, Off);
        const int32 CompressedSize = ReadI32(File, Off);
        if (UncompressedSize <= 0 || CompressedSize <= 0 || Off + CompressedSize > File.Num())
        {
            UE_LOG(LogTemp, Error, TEXT("ValoUeAnim: bad compressed sizes in %s"), *Filename);
            return nullptr;
        }
        Payload.SetNumUninitialized(UncompressedSize);
        int32 OutSize = UncompressedSize;
        bool bOk = false;
        if (Comp == TEXT("ZSTD") || Comp == TEXT("Zstd"))
        {
            // UE has no Zstd codec in FCompression; decode with the vendored libzstd.
            const unsigned long long Decoded = ZSTD_decompress(Payload.GetData(), UncompressedSize,
                                                               File.GetData() + Off, CompressedSize);
            bOk = !ZSTD_isError(Decoded);
            if (bOk) OutSize = (int32)Decoded;
        }
        else if (Comp == TEXT("GZIP") || Comp == TEXT("Gzip"))
        {
            bOk = FCompression::UncompressMemory(NAME_Gzip, Payload.GetData(), OutSize, File.GetData() + Off, CompressedSize);
        }
        if (!bOk)
        {
            UE_LOG(LogTemp, Error, TEXT("ValoUeAnim: %s decompress failed"), *Filename);
            return nullptr;
        }
        if (OutSize < UncompressedSize) Payload.SetNum(OutSize);
    }
    else
    {
        Payload.Append(File.GetData() + Off, File.Num() - Off);
    }

    int32 PO = 0;
    int32 NumFrames = 0;
    float FramesPerSecond = 30.f;

    // FDataAttributeSet serializes as [int32 attributeCount][attribute...],
    // each attribute being [FString name][int32 arraySize][int32 byteSize][bytes].
    if (Payload.Num() < 4) return nullptr;
    const int32 AttrCount = ReadI32(Payload, PO);
    if (AttrCount < 1 || AttrCount > 16) return nullptr;

    // Tracks keep their explicit key frames; keys only appear when a value
    // changes, so sampling must hold the last key at or before each frame.
    struct FUEFTrack
    {
        FName Name;
        TArray<int32> PosFrames;
        TArray<FVector3f> Pos;
        TArray<int32> RotFrames;
        TArray<FQuat4f> Rot;
        TArray<int32> SclFrames;
        TArray<FVector3f> Scl;
    };
    TArray<FUEFTrack> Tracks;

    for (int32 A = 0; A < AttrCount && PO + 12 <= Payload.Num(); ++A)
    {
        const FString ChunkName = ReadFString(Payload, PO);
        const int32 ArraySize = ReadI32(Payload, PO);
        const int32 ByteSize = ReadI32(Payload, PO);
        const int32 ChunkEnd = PO + ByteSize;

        if (ChunkName == TEXT("METADATA"))
        {
            NumFrames = ReadI32(Payload, PO);
            FramesPerSecond = ReadF32(Payload, PO);
            ReadFString(Payload, PO);
            PO += 6; // 2 enums + RefFrameIndex
        }
        else if (ChunkName == TEXT("TRACKS"))
        {
            Tracks.SetNum(ArraySize);
            for (int32 T = 0; T < ArraySize; ++T)
            {
                FUEFTrack& Track = Tracks[T];
                Track.Name = FName(*ReadFString(Payload, PO));
                const int32 PosN = ReadI32(Payload, PO);
                Track.PosFrames.SetNum(PosN);
                Track.Pos.SetNum(PosN);
                for (int32 i = 0; i < PosN; ++i)
                {
                    Track.PosFrames[i] = ReadI32(Payload, PO);
                    Track.Pos[i] = ReadVec(Payload, PO);
                }
                const int32 RotN = ReadI32(Payload, PO);
                Track.RotFrames.SetNum(RotN);
                Track.Rot.SetNum(RotN);
                for (int32 i = 0; i < RotN; ++i)
                {
                    Track.RotFrames[i] = ReadI32(Payload, PO);
                    Track.Rot[i] = ReadQuat(Payload, PO);
                }
                const int32 SclN = ReadI32(Payload, PO);
                Track.SclFrames.SetNum(SclN);
                Track.Scl.SetNum(SclN);
                for (int32 i = 0; i < SclN; ++i)
                {
                    Track.SclFrames[i] = ReadI32(Payload, PO);
                    Track.Scl[i] = ReadVec(Payload, PO);
                }
            }
        }
        PO = FMath::Max(PO, ChunkEnd);
    }

    if (NumFrames <= 0 || FramesPerSecond <= 0.f) return nullptr;

    USkeleton* Skeleton = TargetSkeleton ? TargetSkeleton.Get() : ValoSkeletonPicker::Pick(FPaths::GetBaseFilename(Filename));
    if (!Skeleton) return nullptr;

    UAnimSequence* Anim = NewObject<UAnimSequence>(InParent, InName, Flags);
    Anim->SetSkeleton(Skeleton);
    IAnimationDataController& Controller = Anim->GetController();
    Controller.OpenBracket(NSLOCTEXT("ValoAnimImport", "ImportUeAnim", "Import UEANIM"));
    Controller.InitializeModel();
    // NumberOfKeys == NumberOfFrames + 1 (see AnimSequenceHelpers).
    const int32 DstRate = 30;
    const double SrcRate = FramesPerSecond > 0.f ? (double)FramesPerSecond : 30.0;
    const int32 NumKeys = FMath::Max(2, FMath::RoundToInt((NumFrames - 1) * DstRate / SrcRate) + 1);
    Controller.SetFrameRate(FFrameRate(DstRate, 1));
    Controller.SetNumberOfFrames(FFrameNumber(NumKeys - 1));

    // Tracks are raw UE local space (the UEFormat writer mirrors nothing), so
    // quats/positions are used as-is — unlike the PSA path.
    for (const FUEFTrack& Track : Tracks)
    {
        Controller.AddBoneCurve(Track.Name);

        auto HoldIndex = [](int32 Cursor, int32 Count) -> int32
        {
            return Count > 0 ? FMath::Clamp(Cursor - 1, 0, Count - 1) : INDEX_NONE;
        };

        int32 Pi = 0, Ri = 0, Si = 0;
        TArray<FVector3f> P; P.Reserve(NumKeys);
        TArray<FQuat4f> R; R.Reserve(NumKeys);
        TArray<FVector3f> S; S.Reserve(NumKeys);
        for (int32 F = 0; F < NumKeys; ++F)
        {
            const int32 Src = FMath::Clamp(FMath::RoundToInt(F * SrcRate / DstRate), 0, NumFrames - 1);
            while (Pi < Track.PosFrames.Num() && Track.PosFrames[Pi] <= Src) ++Pi;
            while (Ri < Track.RotFrames.Num() && Track.RotFrames[Ri] <= Src) ++Ri;
            while (Si < Track.SclFrames.Num() && Track.SclFrames[Si] <= Src) ++Si;
            const int32 PIx = HoldIndex(Pi, Track.Pos.Num());
            const int32 RIx = HoldIndex(Ri, Track.Rot.Num());
            const int32 SIx = HoldIndex(Si, Track.Scl.Num());
            P.Add(PIx != INDEX_NONE ? Track.Pos[PIx] : FVector3f::ZeroVector);
            R.Add(RIx != INDEX_NONE ? Track.Rot[RIx] : FQuat4f::Identity);
            S.Add(SIx != INDEX_NONE ? Track.Scl[SIx] : FVector3f(1.f, 1.f, 1.f));
        }
        Controller.SetBoneTrackKeys(Track.Name, P, R, S);
    }

    Controller.NotifyPopulated();
    Controller.CloseBracket();
    Anim->PostEditChange();
    FAssetRegistryModule::AssetCreated(Anim);
    return Anim;
}
