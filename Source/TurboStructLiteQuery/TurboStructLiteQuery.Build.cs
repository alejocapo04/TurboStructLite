/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

/*
 * Descripcion (ES):
 * Reglas de build del modulo TurboStructLiteQuery. Declara dependencias para el motor
 * de queries y su integracion con TurboStructLite.
 *
 * Description (EN):
 * Build rules for the TurboStructLiteQuery module. Declares dependencies for the query
 * engine and its TurboStructLite integration.
 */

using UnrealBuildTool;

public class TurboStructLiteQuery : ModuleRules
{
	public TurboStructLiteQuery(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"TurboStructLite"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"TurboStructLiteDebug"
			}
			);
	}
}
