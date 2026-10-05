#include "ValoDropListener.h"
#include "ValoPsaFactory.h"
#include "ValoUeAnimFactory.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "Framework/Application/SlateApplication.h"
#include "IDirectoryWatcher.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

const TCHAR* FValoDropListener::DropFolderName = TEXT("ValoImportDrop");
FDelegateHandle FValoDropListener::WatchHandle;
FString FValoDropListener::DropDir;
FString FValoDropListener::ImportedDir;

namespace
{
	bool IsAnimExt(const FString& Ext)
	{
		return Ext == TEXT("psa") || Ext == TEXT("ueanim");
	}

	bool IsMeshExt(const FString& Ext)
	{
		return Ext == TEXT("psk") || Ext == TEXT("pskx") || Ext == TEXT("uemodel");
	}

	FString PackageForFile(const FString& FilePath)
	{
		// Default dest mirrors source name under Jett/TP/Anim/LB/Drops
		return TEXT("/Game/Vademption/Assets/Characters/Jett/TP/Anim/LB/Drops");
	}
}

void FValoDropListener::Start()
{
	DropDir = FPaths::Combine(FPaths::ProjectContentDir(), DropFolderName);
	ImportedDir = FPaths::Combine(DropDir, TEXT("Imported"));
	IFileManager::Get().MakeDirectory(*DropDir, true);
	IFileManager::Get().MakeDirectory(*ImportedDir, true);

	FDirectoryWatcherModule& DW = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = DW.Get();
	if (!Watcher)
	{
		UE_LOG(LogTemp, Warning, TEXT("ValoDropListener: DirectoryWatcher unavailable"));
		return;
	}

	Watcher->RegisterDirectoryChangedCallback_Handle(
		DropDir,
		IDirectoryWatcher::FDirectoryChanged::CreateStatic(&FValoDropListener::OnDirectoryChanged),
		WatchHandle,
		IDirectoryWatcher::WatchOptions::IgnoreChangesInSubtree);

	UE_LOG(LogTemp, Log, TEXT("ValoDropListener watching %s (drop .psk/.psa/.uemodel/.ueanim)"), *DropDir);
}

void FValoDropListener::Stop()
{
	if (!WatchHandle.IsValid()) return;
	if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		FDirectoryWatcherModule& DW = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* Watcher = DW.Get())
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(DropDir, WatchHandle);
		}
	}
	WatchHandle.Reset();
}

void FValoDropListener::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	for (const FFileChangeData& Change : Changes)
	{
		if (Change.Action != FFileChangeData::FCA_Added && Change.Action != FFileChangeData::FCA_Modified)
		{
			continue;
		}
		const FString Ext = FPaths::GetExtension(Change.Filename).ToLower();
		if (!IsAnimExt(Ext) && !IsMeshExt(Ext))
		{
			continue;
		}
		// debounce: only handle files sitting in the drop root (not Imported/)
		if (FPaths::GetPath(Change.Filename) != DropDir)
		{
			continue;
		}
		FString FileCopy = Change.Filename;
		AsyncTask(ENamedThreads::GameThread, [FileCopy]()
		{
			FValoDropListener::HandleFile(FileCopy);
		});
	}
}

void FValoDropListener::HandleFile(const FString& FilePath)
{
	if (!FPaths::FileExists(FilePath)) return;

	const FString Ext = FPaths::GetExtension(FilePath).ToLower();
	const FString BaseName = FPaths::GetBaseFilename(FilePath);
	USkeleton* Skeleton = nullptr;

	if (IsAnimExt(Ext))
	{
		Skeleton = PickSkeletonForAnimation(BaseName);
		if (!Skeleton)
		{
			UE_LOG(LogTemp, Warning, TEXT("ValoDropListener: no skeleton chosen for %s — skipped"), *BaseName);
			return;
		}
	}

	const FString Dest = PackageForFile(FilePath);
	const FString Result = ImportWithFactory(FilePath, Skeleton, Dest);
	if (Result.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogTemp, Log, TEXT("ValoDropListener: imported %s -> %s"), *FPaths::GetCleanFilename(FilePath), *Result);
		MoveToImported(FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ValoDropListener: %s — %s"), *FPaths::GetCleanFilename(FilePath), *Result);
	}
}

USkeleton* FValoDropListener::PickSkeletonForAnimation(const FString& FileName)
{
	// Prefer TP_* (3P body). Never default to FP_ arms.
	TArray<TSharedPtr<FString>> Options;
	TArray<USkeleton*> Skeletons;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Assets, true);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.AssetName.LexicalLess(B.AssetName);
	});

	int32 DefaultIndex = INDEX_NONE;
	for (const FAssetData& Data : Assets)
	{
		USkeleton* Skel = Cast<USkeleton>(Data.GetAsset());
		if (!Skel) continue;
		const FString Name = Skel->GetName();
		const FString Label = FString::Printf(TEXT("%s  (%s)"), *Name, *Data.PackageName.ToString());
		Options.Add(MakeShared<FString>(Label));
		Skeletons.Add(Skel);
		if (DefaultIndex == INDEX_NONE && (Name.StartsWith(TEXT("TP_")) || Name.Contains(TEXT("_TP_"))))
		{
			DefaultIndex = Skeletons.Num() - 1;
		}
	}
	if (Skeletons.Num() == 0)
	{
		return nullptr;
	}
	if (DefaultIndex == INDEX_NONE)
	{
		DefaultIndex = 0;
	}

	// Headless / no Slate: auto-pick preferred
	if (!FSlateApplication::IsInitialized())
	{
		return Skeletons[DefaultIndex];
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(FString::Printf(TEXT("Skeleton for %s"), *FileName)))
		.SizingRule(ESizingRule::Autosized)
		.ClientSize(FVector2D(520, 120));

	TSharedPtr<SComboBox<TSharedPtr<FString>>> Combo;
	TSharedPtr<STextBlock> Preview;
	bool bConfirmed = false;
	USkeleton* Chosen = nullptr;
	int32 Selected = DefaultIndex;

	TSharedRef<SVerticalBox> Content =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(STextBlock).Text(FText::FromString(TEXT("This file is an animation. Pick the skeleton it belongs to:")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
		[
			SAssignNew(Combo, SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&Options)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock).Text(FText::FromString(*Item));
			})
			.OnSelectionChanged_Lambda([&](TSharedPtr<FString> Item, ESelectInfo::Type)
			{
				if (Item.IsValid())
				{
					for (int32 i = 0; i < Options.Num(); ++i)
					{
						if (Options[i] == Item) { Selected = i; break; }
					}
				}
			})
			[
				SAssignNew(Preview, STextBlock).Text(FText::FromString(*Options[DefaultIndex]))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(8)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(4)
			[
				SNew(SButton).Text(FText::FromString(TEXT("Import")))
				.OnClicked_Lambda([&]() -> FReply
				{
					bConfirmed = true;
					if (Skeletons.IsValidIndex(Selected)) Chosen = Skeletons[Selected];
					Window->RequestDestroyWindow();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4)
			[
				SNew(SButton).Text(FText::FromString(TEXT("Cancel")))
				.OnClicked_Lambda([&]() -> FReply
				{
					bConfirmed = false;
					Window->RequestDestroyWindow();
					return FReply::Handled();
				})
			]
		];

	Combo->SetSelectedItem(Options[DefaultIndex]);
	Window->SetContent(Content);
	FSlateApplication::Get().AddModalWindow(Window, TSharedPtr<const SWidget>());

	return bConfirmed ? Chosen : nullptr;
}

FString FValoDropListener::ImportWithFactory(const FString& FilePath, USkeleton* Skeleton, const FString& DestPackage)
{
	const FString Ext = FPaths::GetExtension(FilePath).ToLower();
	const FString AssetName = FPaths::GetBaseFilename(FilePath);

	UFactory* Factory = nullptr;
	if (Ext == TEXT("psa"))
	{
		UValoPsaFactory* Psa = NewObject<UValoPsaFactory>();
		Psa->TargetSkeleton = Skeleton;
		Factory = Psa;
	}
	else if (Ext == TEXT("ueanim"))
	{
		UValoUeAnimFactory* Uea = NewObject<UValoUeAnimFactory>();
		Uea->TargetSkeleton = Skeleton;
		Factory = Uea;
	}
	else
	{
		return FString::Printf(TEXT("no factory for .%s yet (psa/ueanim supported for drop)"), *Ext);
	}

	UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("%s/%s"), *DestPackage, *AssetName));
	bool bCancel = false;
	UObject* Result = Factory->FactoryCreateFile(
		UAnimSequence::StaticClass(), Pkg, FName(*AssetName),
		RF_Public | RF_Standalone, FilePath, nullptr, GWarn, bCancel);

	UAnimSequence* Anim = Cast<UAnimSequence>(Result);
	if (!Anim)
	{
		return TEXT("factory returned null");
	}
	return FString::Printf(TEXT("%s/%s"), *DestPackage, *AssetName);
}

void FValoDropListener::MoveToImported(const FString& FilePath)
{
	const FString Dest = FPaths::Combine(ImportedDir, FPaths::GetCleanFilename(FilePath));
	IFileManager::Get().Move(*Dest, *FilePath, true, true);
}
