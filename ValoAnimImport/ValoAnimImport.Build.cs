using System.IO;
using UnrealBuildTool;

public class ValoAnimImport : ModuleRules
{
    public ValoAnimImport(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new string[]
            {
                Path.Combine(ModuleDirectory, "ThirdParty/zstd"),
                Path.Combine(ModuleDirectory, "ThirdParty/zstd/common"),
                Path.Combine(ModuleDirectory, "ThirdParty/zstd/decompress")
            }
        );
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AssetTools", "AnimationCore"
        });
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate", "SlateCore", "EditorFramework", "InputCore",
            "Sockets", "Networking", "Json", "JsonUtilities",
            "DirectoryWatcher"
        });
        // UE ships no Zstd codec for FCompression, but UEANIM payloads use it;
        // the decoder is vendored under ThirdParty/zstd and its .c files compile
        // into this module automatically.
    }
}
