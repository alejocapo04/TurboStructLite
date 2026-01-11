/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

/*
 * Descripcion (ES):
 * Reglas de build para el modulo TurboStructProjectSettings. Expone DeveloperSettings con
 * dependencias minimas de runtime.
 *
 * Description (EN):
 * Build rules for the TurboStructProjectSettings module. Exposes DeveloperSettings with minimal
 * runtime dependencies.
 */

using UnrealBuildTool;

public class TurboStructLiteProjectSettings : ModuleRules
{
	public TurboStructLiteProjectSettings(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"TurboStructLite"
		});
	}
}
