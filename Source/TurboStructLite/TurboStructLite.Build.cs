using UnrealBuildTool;

public class TurboStructLite : ModuleRules
{
	public TurboStructLite(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
			}
			);

		bool bUseOpenSSL =
			Target.Platform == UnrealTargetPlatform.Win64 ||
			Target.Platform == UnrealTargetPlatform.Mac ||
			Target.Platform == UnrealTargetPlatform.Linux;

		if (bUseOpenSSL)
		{
			PrivateDependencyModuleNames.Add("OpenSSL");
			PublicDefinitions.Add("TURBOSTRUCTLITE_USE_OPENSSL=1");
		}
		else
		{
			PrivateDependencyModuleNames.Add("PlatformCrypto");
			PrivateDependencyModuleNames.Add("PlatformCryptoTypes");
			PublicDefinitions.Add("TURBOSTRUCTLITE_USE_OPENSSL=0");
		}
	}
}
