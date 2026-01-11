/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

/*
 * Descripcion (ES):
 * Reglas de build del modulo de editor de TurboStruct. Configura dependencias de UI, StructUtils
 * y define el flag TURBOSTRUCT_HAS_EDITOR_UI segun la version de Unreal.
 *
 * Description (EN):
 * Build rules for the TurboStruct editor module. Configures UI/StructUtils dependencies and
 * sets the TURBOSTRUCT_HAS_EDITOR_UI flag based on the Unreal version.
 */

using UnrealBuildTool;

public class TurboStructLiteEditor : ModuleRules
{
	public TurboStructLiteEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		bool bIsUE55OrNewer = (Target.Version.MajorVersion > 5) || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 5);
		bool bHasEditorUI = (Target.Version.MajorVersion > 5) || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 1);
		PublicDefinitions.Add($"TURBOSTRUCT_HAS_EDITOR_UI={(bHasEditorUI ? 1 : 0)}");

		string StructUtilsRuntimeModule = bIsUE55OrNewer ? "StructUtilsEngine" : "StructUtils";

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"TurboStructLite",
			StructUtilsRuntimeModule,
			"StructUtilsEditor",
			"Slate",
			"SlateCore",
			"EditorStyle",
			"InputCore",
			"ApplicationCore",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"AssetRegistry",
			"PropertyEditor",
			"UnrealEd",
			"AppFramework",
			"Projects"
		});
	}
}
