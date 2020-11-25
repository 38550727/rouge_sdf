#include "BasicFile.hpp"
#include "utils.h"
#include <zstd.h>
#include <boost\filesystem.hpp>
#include <boost\format.hpp>

#pragma pack(push,1)
struct SdfTocHeader
{
    uint32_t fileTag; //0x54534557
    uint32_t fileVersion;
    uint32_t decompressedSize;
    uint32_t compressedSize;
    uint32_t zero;
    uint32_t block1count;
    uint32_t ddsHeaderBlockCount;
};
struct SdfTocId
{
    uint64_t massive;
    uint8_t data[0x20];
    uint64_t ubisoft;
};

struct SdfDdsHeader
{
    uint32_t usedBytes;
    uint8_t bytes[200];
};

#pragma pack(pop)

static const size_t CHUNK_SIZE = 0x10000;

struct FileTree
{
    template <typename Callback>
    static void ParseNames(File data, const Callback &cbFunction, std::string name="")
    {

        auto readVariadicInteger = [&data](uint32_t count)
        {
            uint64_t result = 0;

            for (uint32_t i = 0; i < count; i++)
            {
                result |= uint64_t(data.Read<uint8_t>()) << (i * 8);
            }
            return result;
        };

        auto ch = data.Read<char>();
        if (ch == 0)
        {
            std::cout << "Error: Unexcepted byte in file tree!\n";
            return;
        }

        if (ch >= 1 && ch <= 0x1f) //string part
        {
            while (ch--)
            {
                name += data.Read<char>();
            }
            ParseNames(data, cbFunction, name);
        }
        else if (ch >= 'A' && ch <= 'Z') //file entry
        {
            ch = ch - 'A';
            char count1 = ch & 7;
            //auto flag1 = (ch >> 3) & 1;

            if (count1)
            {
                uint32_t strangeId = data.Read<uint32_t>();
                uint8_t ch2 = data.Read<uint8_t>();
                auto byteCount = ch2 & 3;
                uint64_t ddsType = readVariadicInteger(byteCount);

                for (int chunkIndex = 0; chunkIndex < count1; chunkIndex++)
                {
                    auto ch3 = data.Read<uint8_t>();
                    if (ch3 == 0)
                    {
                        break;
                    }

                    auto compressedSizeByteCount = (ch3 & 3) + 1;
                    auto packageOffsetByteCount = (ch3 >> 2) & 7;
                    auto hasCompression = (ch3 >> 5) & 1;

                    uint64_t decompressedSize = readVariadicInteger(compressedSizeByteCount);
					//putvarchr decompressedSize 8 0
					//getvarchr decompressedSize decompressedSize 0 long
                    decompressedSize &= 0x00000000FFFFFFFFull;
                    uint64_t compressedSize = 0;
                    if (hasCompression)
                    {
                        compressedSize = readVariadicInteger(compressedSizeByteCount);
						//putvarchr compressedSize 8 0
						//getvarchr compressedSize compressedSize 0 long
                        compressedSize &= 0x00000000FFFFFFFFull;
                    }

                    uint64_t packageOffset = 0;
                    if (packageOffsetByteCount)
                    {
                        packageOffset = readVariadicInteger(packageOffsetByteCount);
						//putvarchr packageOffset 8 0
						//getvarchr packageOffset packageOffset 0 longlong
                        packageOffset &= 0x00FFFFFFFFFFFFFFull;
                    }
                    uint64_t packageId = readVariadicInteger(2);

                    std::vector<uint64_t> compSizeArray;
                    if (hasCompression)
                    {
                        //uint64_t pageCount = (decompressedSize + 0xffff) >> 16;
                        uint64_t pageCount = (decompressedSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
                        if (pageCount > 1)
                        {
                            for (uint64_t page = 0; page < pageCount; page++)
                            {
                                uint64_t compSize = readVariadicInteger(2);
                                compSizeArray.push_back(compSize);
                            }
                        }
                        else if (pageCount == 1)
                        {
                            compSizeArray.push_back(compressedSize);
                        }
                    }

					//void Foo(const std::string& name, uint64_t packageId, uint64_t packageOffset, uint64_t decompressedSize, const std::vector<uint64_t>& compSizeArray, uint64_t ddsType, bool append, bool useDDS);
                    cbFunction(name, packageId, packageOffset, decompressedSize, compSizeArray, ddsType, chunkIndex != 0, byteCount != 0 && chunkIndex == 0);
                    uint64_t fileId = readVariadicInteger(4);
                }
            }

            if (ch & 8) //if (flag1)
            {
                auto ch3 = data.Read<uint8_t>();
                readVariadicInteger(ch3);
            }
        }
        else //search tree entry
        {
            File data2 = data;
            uint32_t offset = data.Read<uint32_t>();
            data2.Seek(offset);
            ParseNames(data, cbFunction, name);
            ParseNames(data2, cbFunction, name);
        }
    }
};

std::wstring sdfTocFile;
std::wstring outputDir;
DataArray<SdfDdsHeader> ddsHeaderBlock;

void DumpFile(const std::string& name, uint32_t packageId, uint64_t packageOffset,
	            uint64_t decompressedSize, const std::vector<uint64_t>& compSizeArray,
	            uint64_t ddsType, bool append, bool useDDS)
{
	boost::filesystem::path sdfTocPath(sdfTocFile);
    std::wstring layer = L"A";
    layer[0] += size_t(packageId / 1000);
	typedef  boost::basic_format<wchar_t >  wformat;
	std::wstring dataFormated = boost::str(boost::wformat(L"-%s-%04i.sdfdata") % layer % packageId);
	std::wstring sdfDataPath = sdfTocPath.parent_path().append(sdfTocPath.stem().wstring()).wstring() + dataFormated;

	if (!IsFileExist(sdfDataPath))
	{
        std::wcout << L"!!!Error: Can't open the file: " << sdfDataPath << std::endl;
		return;
	}

	BlockPtr fileBlock = MakeBlockDisk(sdfDataPath);
	if (fileBlock->Size() <= 5)
	{
		// Skip 'Dummy' file
		return;
	}
	std::wcout << L"Open file: " << sdfDataPath << std::endl;

	std::wstring outFileName = outputDir + AnsiToUnicode(name);
	std::replace(outFileName.begin(), outFileName.end(), L'/', L'\\');
	CreateDirectoryRecursively(ExtractFilePath(outFileName));
	std::wcout << L"Extract asset: " << outFileName << std::endl;

	BlockPtr resultBlock;
	if (compSizeArray.size() == 0)
	{
		// uncompressed data
		try
		{
			resultBlock = MakeBlockPart(fileBlock, packageOffset, decompressedSize);
		}
		catch (const std::exception& ex)
		{
			std::cout << "!!!Error: call MakeBlockPart. Exception: " << ex.what();
			return;
		}

	}
	else
	{
		// need decompress
		std::unique_ptr<uint8_t[]> decompressed = std::make_unique<uint8_t[]>(decompressedSize);
		if (compSizeArray.size() == 1)
		{
			size_t sizeCompressed = compSizeArray[0];
			auto dataCompressed = fileBlock->Get<uint8_t>(packageOffset, sizeCompressed);
			size_t dSize = ZSTD_decompress(decompressed.get(), decompressedSize, dataCompressed.get(), sizeCompressed);
			if (dSize != decompressedSize)
			{
				//throw std::exception("Uncompress error");
				std::cout << "!!!Error: Uncompress error!!!\n";
				return;
			}
		}
		else
		{
			uint64_t decompOffset = 0;
			uint64_t compOffset = 0;
			uint64_t MySize = decompressedSize;
			size_t chunkSize = CHUNK_SIZE;
			for (uint64_t compSizePart : compSizeArray)
			{
				if (MySize < chunkSize)
				{
					chunkSize = MySize;
				}

				if (compSizePart == 0 || compSizePart >= chunkSize)
				{
					fileBlock->Get<uint8_t>(decompressed.get() + decompOffset, packageOffset, chunkSize);
					packageOffset += chunkSize;
					decompOffset += chunkSize;
				}
				else
				{
					auto dataCompressed = fileBlock->Get<uint8_t>(packageOffset, compSizePart);
					size_t dSize = ZSTD_decompress(decompressed.get() + decompOffset, chunkSize, dataCompressed.get(), compSizePart);
					if (dSize != chunkSize)
					{
						//throw std::exception("Uncompress error");
						std::cout << "!!!Error: Uncompress error!!!\n";
						return;
					}
					packageOffset += compSizePart;
					decompOffset += chunkSize;
				}
				MySize -= chunkSize;
			}
		}

		try
		{
			resultBlock = MakeBlockMemory(std::move(decompressed), decompressedSize);
		}
		catch (const std::exception& ex2)
		{
			std::cout << "!!!Error:" << ex2.what() << "!!!\n";
			return;
		}
	}

	if (useDDS)
	{
		SdfDdsHeader ddsHeader = ddsHeaderBlock[ddsType];
		const int ddsHeaderDataSize = ddsHeader.usedBytes;
		auto fullDataBlockSize = ddsHeaderDataSize + resultBlock->Size();
		auto fullDataBlock = std::make_unique<uint8_t[]>(fullDataBlockSize);
		std::memcpy(fullDataBlock.get(), ddsHeader.bytes, ddsHeaderDataSize);
		resultBlock->Get(fullDataBlock.get() + ddsHeaderDataSize, 0, resultBlock->Size());
		try
		{
			resultBlock = MakeBlockMemory(std::move(fullDataBlock), fullDataBlockSize);
		}
		catch (const std::exception& ex2)
		{
			std::cout << "!!!Error:" << ex2.what() << "!!!\n";
			return;
		}
	}

	if (append)
	{
		WriteBlockApp(resultBlock, outFileName);
	}
	else
	{
		WriteBlock(resultBlock, outFileName);
	}

}


int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::cout << "Mario + Rabbids Kingdom Battle .sdftoc extractor" << std::endl;
        std::cout << "usage: rouge_sdf.exe <.sdftoc path> <output directory>" << std::endl;
        return 0;
    }

    try
    {

        sdfTocFile = argv[1];
        outputDir = argv[2];

        outputDir = boost::filesystem::path(outputDir).remove_trailing_separator().wstring() + L"\\";

        auto file = MakeFileDisk(sdfTocFile);

        SdfTocHeader header = file.Read<SdfTocHeader>();
        SdfTocId id = file.Read<SdfTocId>();
        uint8_t signExistFlag = file.Read<uint8_t>();
        if (signExistFlag)
        {
            file.Seek(0x140, FileOriginCurrent);
        }

        auto block1 = file.Array<uint32_t>(header.block1count);
        auto SdfTocIdBlock = file.Array<SdfTocId>(header.block1count);
        ddsHeaderBlock = file.Array<SdfDdsHeader>(header.ddsHeaderBlockCount);
        // display all dds header info:
        std::cout << "\nFound dds header infos:\n";
        for (size_t ddsIdx = 0; ddsIdx < ddsHeaderBlock.Size(); ++ddsIdx)
        {
            const SdfDdsHeader& DDSHeader = ddsHeaderBlock[ddsIdx];
            std::cout << "[" << ddsIdx << "] Header Size = " << DDSHeader.usedBytes << std::endl;
#if 0
			std::wstring ddsHeaderFile = boost::str(boost::wformat(L"%s_DDSHeader/%d.dat") % outputDir % ddsIdx);
			std::replace(ddsHeaderFile.begin(), ddsHeaderFile.end(), L'/', L'\\');
			CreateDirectoryRecursively(ExtractFilePath(ddsHeaderFile));
            auto headerBlock = MakeBlockMemory(DDSHeader.bytes, DDSHeader.usedBytes);
            WriteBlock(headerBlock, ddsHeaderFile);
#endif
        }

        // find the compressed bulk data
        size_t CompressDataOffset = file.Size() - 0x30 - header.compressedSize;
        file.Seek(CompressDataOffset);
        std::unique_ptr<uint8_t[]> decompressed = std::make_unique<uint8_t[]>(header.decompressedSize);
        std::unique_ptr<uint8_t[]> compressed = std::make_unique<uint8_t[]>(header.compressedSize);

        file.Read<uint8_t>(compressed.get(), header.compressedSize);
        // use zstd method
        size_t decompSize = ZSTD_decompress(decompressed.get(), header.decompressedSize, compressed.get(), header.compressedSize);
        File memoryFile = File(MakeBlockMemory(std::move(decompressed), decompSize));
        
        FileTree::ParseNames(memoryFile, DumpFile);
    }
    catch (const std::exception & ex)
    {
        std::cout << "Error: " << ex.what() << std::endl;
    }
    return 0;
}