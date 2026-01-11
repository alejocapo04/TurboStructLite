#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"

#include "Misc/Compression.h"

FName UTurboStructLiteBPLibrary::GetCompressionName(ETurboStructLiteCompression Method)
{
	if (Method == ETurboStructLiteCompression::ProjectDefault)
	{
		Method = ResolveCompression(Method);
	}
	switch (Method)
	{
		case ETurboStructLiteCompression::LZ4:
			return NAME_LZ4;
		case ETurboStructLiteCompression::Zlib:
			return NAME_Zlib;
		case ETurboStructLiteCompression::Gzip:
			return NAME_Gzip;
		case ETurboStructLiteCompression::Oodle:
			return FName(TEXT("Oodle"));
		default:
			return NAME_None;
	}
}

bool UTurboStructLiteBPLibrary::TurboStructLiteCompress(ETurboStructLiteCompression Method, const TArray<uint8>& InBytes, TArray<uint8>& OutCompressedBytes)
{
	return CompressBuffer(Method, InBytes, OutCompressedBytes, -1, -1);
}

void UTurboStructLiteBPLibrary::TurboStructLiteDecompress(ETurboStructLiteCompression Method, const TArray<uint8>& InCompressedBytes, TArray<uint8>& OutRawBytes)
{
	DecompressBuffer(Method, InCompressedBytes, OutRawBytes);
}

void UTurboStructLiteBPLibrary::TurboStructLiteCompressLZ4(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteCompress(ETurboStructLiteCompression::LZ4, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteDecompressLZ4(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteDecompress(ETurboStructLiteCompression::LZ4, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteCompressZlib(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteCompress(ETurboStructLiteCompression::Zlib, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteDecompressZlib(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteDecompress(ETurboStructLiteCompression::Zlib, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteCompressGzip(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteCompress(ETurboStructLiteCompression::Gzip, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteDecompressGzip(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteDecompress(ETurboStructLiteCompression::Gzip, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteCompressOodle(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteCompress(ETurboStructLiteCompression::Oodle, In, Out);
}

void UTurboStructLiteBPLibrary::TurboStructLiteDecompressOodle(const TArray<uint8>& In, TArray<uint8>& Out)
{
	TurboStructLiteDecompress(ETurboStructLiteCompression::Oodle, In, Out);
}


