#include "TurboStructLiteProjectSettings.h"

UTurboStructLiteProjectSettings::UTurboStructLiteProjectSettings()
{
	DefaultEncryptionKey = TEXT("TurboStructLiteDefaultKey");
}

FText UTurboStructLiteProjectSettings::GetSectionText() const
{
	return NSLOCTEXT("TurboStructLiteProjectSettings", "SectionText", "TurboStructLite");
}


