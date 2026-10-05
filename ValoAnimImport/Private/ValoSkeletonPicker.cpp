#include "ValoSkeletonPicker.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace ValoSkeletonPicker
{
	static void Gather(TArray<USkeleton*>& OutSkel, TArray<TSharedPtr<FString>>& OutLabels, int32& OutDefault)
	{
		OutDefault = INDEX_NONE;
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Assets;
		ARM.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Assets, true);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });
		for (const FAssetData& Data : Assets)
		{
			USkeleton* Skel = Cast<USkeleton>(Data.GetAsset());
			if (!Skel) continue;
			const FString Name = Skel->GetName();
			OutSkel.Add(Skel);
			OutLabels.Add(MakeShared<FString>(FString::Printf(TEXT("%s  (%s)"), *Name, *Data.PackageName.ToString())));
			if (OutDefault == INDEX_NONE && (Name.StartsWith(TEXT("TP_")) || Name.Contains(TEXT("_TP_"))))
			{
				OutDefault = OutSkel.Num() - 1;
			}
		}
		if (OutDefault == INDEX_NONE && OutSkel.Num() > 0)
		{
			OutDefault = 0;
		}
	}

	USkeleton* PickDefault()
	{
		TArray<USkeleton*> Skels;
		TArray<TSharedPtr<FString>> Labels;
		int32 Def = INDEX_NONE;
		Gather(Skels, Labels, Def);
		return Skels.IsValidIndex(Def) ? Skels[Def] : nullptr;
	}

	USkeleton* Pick(const FString& FileName)
	{
		TArray<USkeleton*> Skels;
		TArray<TSharedPtr<FString>> Labels;
		int32 Def = INDEX_NONE;
		Gather(Skels, Labels, Def);
		if (Skels.Num() == 0) return nullptr;

		if (!FSlateApplication::IsInitialized())
		{
			return Skels[Def];
		}

		bool bOk = false;
		int32 Selected = Def;
		USkeleton* Chosen = nullptr;
		FString Filter;

		// Filtered view of Labels/Skels
		TArray<TSharedPtr<FString>> Visible;
		TArray<int32> VisibleToIdx;

		auto RebuildVisible = [&]()
		{
			Visible.Reset();
			VisibleToIdx.Reset();
			for (int32 i = 0; i < Labels.Num(); ++i)
			{
				if (Filter.IsEmpty() || Labels[i]->Contains(Filter, ESearchCase::IgnoreCase))
				{
					Visible.Add(Labels[i]);
					VisibleToIdx.Add(i);
				}
			}
		};
		RebuildVisible();

		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::FromString(FString::Printf(TEXT("ValoAnimImport — skeleton for %s"), *FileName)))
			.SizingRule(ESizingRule::Autosized);

		TSharedPtr<SComboBox<TSharedPtr<FString>>> Combo;
		TSharedPtr<SEditableTextBox> SearchBox;

		TSharedRef<SVerticalBox> Body =
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(10, 10, 10, 4)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Which skeleton does this animation belong to?")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(10, 4, 10, 4)
			[
				SAssignNew(SearchBox, SEditableTextBox)
				.HintText(FText::FromString(TEXT("Search skeletons… (e.g. TP_Wushu)")))
				.OnTextChanged_Lambda([&](const FText& Text)
				{
					Filter = Text.ToString();
					RebuildVisible();
					if (Combo.IsValid())
					{
						Combo->RefreshOptions();
						if (Visible.Num() > 0)
						{
							Combo->SetSelectedItem(Visible[0]);
						}
					}
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(10, 4, 10, 8)
			[
				SAssignNew(Combo, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&Visible)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
				})
				.OnSelectionChanged_Lambda([&](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					if (!Item.IsValid()) return;
					for (int32 v = 0; v < Visible.Num(); ++v)
					{
						if (Visible[v].Get() == Item.Get())
						{
							Selected = VisibleToIdx[v];
							break;
						}
					}
				})
				[
					SNew(STextBlock).Text(FText::FromString(Labels.IsValidIndex(Def) ? *Labels[Def] : TEXT("Select…")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(8, 0, 8, 10)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(4)
				[
					SNew(SButton).Text(FText::FromString(TEXT("Import")))
					.OnClicked_Lambda([&]() -> FReply
					{
						bOk = true;
						if (Skels.IsValidIndex(Selected)) Chosen = Skels[Selected];
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4)
				[
					SNew(SButton).Text(FText::FromString(TEXT("Cancel")))
					.OnClicked_Lambda([&]() -> FReply
					{
						bOk = false;
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
			];

		Window->SetContent(Body);

		if (Labels.IsValidIndex(Def))
		{
			Combo->SetSelectedItem(Labels[Def]);
		}

		TSharedPtr<const SWidget> Parent;
		TSharedPtr<SWindow> Active = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (Active.IsValid())
		{
			Parent = Active;
		}
		FSlateApplication::Get().AddModalWindow(Window, Parent);

		return bOk ? Chosen : nullptr;
	}
}
