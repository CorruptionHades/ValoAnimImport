#pragma once
#include "CoreMinimal.h"
#include "IDirectoryWatcher.h"

class USkeleton;

/**
 * Watches Content/ValoImportDrop for .psk/.pskx/.psa/.uemodel/.ueanim.
 * Animations open a modal skeleton picker (prefers TP_*). Successful imports
 * move the source file into Imported/.
 */
class FValoDropListener
{
public:
	static const TCHAR* DropFolderName;

	static void Start();
	static void Stop();

	static void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);

private:
	static void HandleFile(const FString& FilePath);
	static USkeleton* PickSkeletonForAnimation(const FString& FileName);
	static FString ImportWithFactory(const FString& FilePath, USkeleton* Skeleton, const FString& DestPackage);
	static void MoveToImported(const FString& FilePath);

	static FDelegateHandle WatchHandle;
	static FString DropDir;
	static FString ImportedDir;
};
