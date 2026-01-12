/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

/*
 * Descripcion (ES):
 * Reglas de build del modulo TurboStructLiteDebug. Declara dependencias para utilidades de debug
 * independientes del runtime principal.
 *
 * Description (EN):
 * Build rules for the TurboStructLiteDebug module. Declares dependencies for debug utilities
 * independent from the main runtime.
 */

using UnrealBuildTool;

public class TurboStructLiteDebug : ModuleRules
{
	public TurboStructLiteDebug(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine"
			}
		);
	}
}
