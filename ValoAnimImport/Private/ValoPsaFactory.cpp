#include "ValoPsaFactory.h"
#include "ValoSkeletonPicker.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ActorX PSA: ANIMHEAD | BONENAMES | ANIMINFO | ANIMKEYS [| SCALEKEYS]
// Keys frame-major: index = frame * TotalBones + bone. Quats (x, y, z, w).

namespace
{
    struct FChunkHeader
    {
        char ChunkID[20];
        int32 TypeFlag;
        int32 DataSize;
        int32 DataCount;
    };

    struct FPsaKey
    {
        float Pos[3];
        float Quat[4];
        float Time;
    };

    struct FPsaBoneRest
    {
        FName Name;
        FQuat4f Rot = FQuat4f::Identity;
        FVector3f Pos = FVector3f::ZeroVector;
        FVector3f Scale = FVector3f(1.f, 1.f, 1.f);
        int32 ParentIndex = INDEX_NONE;
    };

    USkeleton* FindAnySkeleton()
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        TArray<FAssetData> Assets;
        ARM.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Assets, true);
        USkeleton* Tp = nullptr;
        USkeleton* Any = nullptr;
        for (const FAssetData& Data : Assets)
        {
            USkeleton* Skel = Cast<USkeleton>(Data.GetAsset());
            if (!Skel) continue;
            const FString Name = Skel->GetName();
            // Never bind 3P locomotion to a first-person arms skeleton.
            if (Name.Contains(TEXT("FP_"), ESearchCase::IgnoreCase)) continue;
            if (Name.StartsWith(TEXT("TP_")) || Name.Contains(TEXT("_TP_")))
            {
                Tp = Skel;
                break;
            }
            if (!Any) Any = Skel;
        }
        return Tp ? Tp : Any;
    }
}

UValoPsaFactory::UValoPsaFactory()
{
    SupportedClass = UAnimSequence::StaticClass();
    bCreateNew = false;
    bEditorImport = true;
    Formats.Add(TEXT("psa; ActorX Animation Set"));
}

UObject* UValoPsaFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
                                            const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        return nullptr;
    }

    const uint8* Data = Bytes.GetData();
    const int64 Size = Bytes.Num();
    int64 Off = 0;

    auto ReadHeader = [&](FChunkHeader& H) -> bool
    {
        if (Off + (int64)sizeof(FChunkHeader) > Size) return false;
        FMemory::Memcpy(&H, Data + Off, sizeof(FChunkHeader));
        Off += sizeof(FChunkHeader);
        return true;
    };

    FChunkHeader FileHead;
    if (!ReadHeader(FileHead) || FCStringAnsi::Strncmp(FileHead.ChunkID, "ANIMHEAD", 8) != 0)
    {
        return nullptr;
    }

    // BONENAMES — name + rest local transform (needed to re-base keys onto the target skeleton)
    FChunkHeader Chunk;
    if (!ReadHeader(Chunk)) return nullptr;
    const int32 BoneCount = Chunk.DataCount;
    const int32 BoneSize = Chunk.DataSize;
    TArray<FPsaBoneRest> PsaRest;
    PsaRest.SetNum(BoneCount);
    for (int32 i = 0; i < BoneCount; ++i)
    {
        const uint8* Rec = Data + Off + (int64)i * BoneSize;
        char NameRaw[64];
        FMemory::Memcpy(NameRaw, Rec, 64);
        NameRaw[63] = 0;
        PsaRest[i].Name = FName(UTF8_TO_TCHAR(NameRaw));
        int32 BoneFlags = 0, NumChildren = 0, Parent = INDEX_NONE;
        FMemory::Memcpy(&BoneFlags, Rec + 64, 4);
        FMemory::Memcpy(&NumChildren, Rec + 68, 4);
        FMemory::Memcpy(&Parent, Rec + 72, 4);
        PsaRest[i].ParentIndex = Parent;
        // VJointPos: quat(x,y,z,w), pos(x,y,z), length, size
        float Q[4], P[3], Len, Sz[3];
        FMemory::Memcpy(Q, Rec + 76, 16);
        FMemory::Memcpy(P, Rec + 92, 12);
        PsaRest[i].Rot = FQuat4f(Q[0], Q[1], Q[2], Q[3]);
        // BONENAMES rest is written in raw UE local space by CUE4Parse/FModel
        // (only ANIMKEYS are mirrored, see below). Names/parents are all we use.
        PsaRest[i].Rot.Normalize();
        PsaRest[i].Pos = FVector3f(P[0], P[1], P[2]);
    }
    Off += (int64)BoneSize * BoneCount;

    // ANIMINFO
    if (!ReadHeader(Chunk) || Chunk.DataCount < 1 || Chunk.DataSize < 168) return nullptr;
    const uint8* Info = Data + Off;
    int32 TotalBones = 0, NumRawFrames = 0;
    float AnimRate = 30.f;
    FMemory::Memcpy(&TotalBones, Info + 128, 4);
    FMemory::Memcpy(&AnimRate, Info + 144 + 8, 4);
    FMemory::Memcpy(&NumRawFrames, Info + 156 + 8, 4);
    Off += (int64)Chunk.DataSize * Chunk.DataCount;

    if (TotalBones <= 0 || NumRawFrames <= 0) return nullptr;
    if (AnimRate <= 0.f) AnimRate = 30.f;

    // ANIMKEYS
    if (!ReadHeader(Chunk)) return nullptr;
    const int32 KeyCount = Chunk.DataCount;
    const int32 KeySize = Chunk.DataSize >= 32 ? Chunk.DataSize : 32;
    if (KeyCount < TotalBones * NumRawFrames) return nullptr;

    TArray<FPsaKey> Keys;
    Keys.SetNum(TotalBones * NumRawFrames);
    for (int32 i = 0; i < Keys.Num(); ++i)
    {
        const uint8* K = Data + Off + (int64)i * KeySize;
        FMemory::Memcpy(Keys[i].Pos, K, 12);
        FMemory::Memcpy(Keys[i].Quat, K + 12, 16);
        FMemory::Memcpy(&Keys[i].Time, K + 28, 4);
    }
    Off += (int64)Chunk.DataSize * Chunk.DataCount;

    // optional SCALEKEYS (3f + pad/time)
    TArray<FVector3f> ScaleKeys;
    if (Off + 32 <= Size)
    {
        FChunkHeader ScaleChunk;
        const int64 SavedOff = Off;
        if (ReadHeader(ScaleChunk) && FCStringAnsi::Strncmp(ScaleChunk.ChunkID, "SCALEKEYS", 9) == 0)
        {
            const int32 ScaleSize = ScaleChunk.DataSize >= 12 ? ScaleChunk.DataSize : 16;
            ScaleKeys.SetNum(ScaleChunk.DataCount);
            for (int32 i = 0; i < ScaleChunk.DataCount; ++i)
            {
                const uint8* S = Data + Off + (int64)i * ScaleSize;
                FMemory::Memcpy(&ScaleKeys[i], S, 12);
            }
        }
        else
        {
            Off = SavedOff;
        }
    }

    USkeleton* Skeleton = TargetSkeleton ? TargetSkeleton.Get() : ValoSkeletonPicker::Pick(FPaths::GetBaseFilename(Filename));
    if (!Skeleton) return nullptr;

    UAnimSequence* Anim = NewObject<UAnimSequence>(InParent, InName, Flags);
    Anim->SetSkeleton(Skeleton);

    IAnimationDataController& Controller = Anim->GetController();
    Controller.OpenBracket(NSLOCTEXT("ValoAnimImport", "ImportPsa", "Import PSA"));
    Controller.InitializeModel();

    // Clean 30 Hz grid (compression asserts on subframe PlayLength * Rate).
    // UE stores NumberOfKeys == NumberOfFrames + 1 (see AnimSequenceHelpers).
    const int32 DstRate = 30;
    const double SrcRate = AnimRate > 0.0 ? (double)AnimRate : 30.0;
    const int32 NumKeys = FMath::Max(2, FMath::RoundToInt((NumRawFrames - 1) * DstRate / SrcRate) + 1);
    const int32 NumFrames = NumKeys - 1;
    Controller.SetFrameRate(FFrameRate(DstRate, 1));
    Controller.SetNumberOfFrames(FFrameNumber(NumFrames));

    const TArray<FTransform>& RefPose = Skeleton->GetReferenceSkeleton().GetRefBonePose();
    const TArray<FMeshBoneInfo>& BoneInfos = Skeleton->GetReferenceSkeleton().GetRefBoneInfo();
    TMap<FName, int32> SkelIndexByName;
    for (int32 i = 0; i < BoneInfos.Num(); ++i)
    {
        SkelIndexByName.Add(BoneInfos[i].Name, i);
    }

    for (int32 PsaIdx = 0; PsaIdx < PsaRest.Num() && PsaIdx < TotalBones; ++PsaIdx)
    {
        const FName BoneName = PsaRest[PsaIdx].Name;
        if (BoneName == FName(TEXT("Skeleton")))
        {
            continue; // dummy root wrapper; Root track is fine
        }
        if (!SkelIndexByName.Contains(BoneName))
        {
            continue;
        }

        Controller.AddBoneCurve(BoneName);

        TArray<FVector3f> Pos;
        TArray<FQuat4f> Rot;
        TArray<FVector3f> Scl;
        Pos.Reserve(NumKeys);
        Rot.Reserve(NumKeys);
        Scl.Reserve(NumKeys);

        for (int32 F = 0; F < NumKeys; ++F)
        {
            const int32 Src = FMath::Clamp(FMath::RoundToInt(F * SrcRate / DstRate), 0, NumRawFrames - 1);
            const FPsaKey& K = Keys[Src * TotalBones + PsaIdx];
            // CUE4Parse/FModel mirror every key on export (MIRROR_MESH in
            // ActorXAnim.cs): quat Y and position Y are negated vs UE local space,
            // plus W of PSA bone 0. Undo exactly that — verified to reproduce the
            // game's own local tracks bit-exact. Do NOT rebase against BONENAMES;
            // that rest is unmirrored raw UE space.
            FQuat4f Q(K.Quat[0], -K.Quat[1], K.Quat[2], PsaIdx == 0 ? -K.Quat[3] : K.Quat[3]);
            Q.Normalize();
            Pos.Add(FVector3f(K.Pos[0], -K.Pos[1], K.Pos[2]));
            Rot.Add(Q);
            Scl.Add(FVector3f(1.f, 1.f, 1.f));
        }

        Controller.SetBoneTrackKeys(BoneName, Pos, Rot, Scl);
    }

    // Required so the model is considered populated (without this the clip can look static).
    Controller.NotifyPopulated();
    Controller.CloseBracket();
    Anim->PostEditChange();
    FAssetRegistryModule::AssetCreated(Anim);
    return Anim;
}
