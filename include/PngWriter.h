#pragma once
// SpectroCraft - Minimal PNG writer (RGBA8, stored deflate blocks, no external deps)
#include <JuceHeader.h>
#include <vector>
#include <cstdint>

class PngWriter
{
public:
    static bool writePng (const juce::File& file,
                          int width,
                          int height,
                          const std::vector<uint8_t>& rgbaPixels)
    {
        if (width <= 0 || height <= 0 || rgbaPixels.size() != (size_t) width * height * 4)
            return false;

        // Build raw scanlines with filter byte 0 (None) per row
        std::vector<uint8_t> raw;
        raw.reserve ((size_t) (width + 1) * height);
        for (int y = 0; y < height; ++y)
        {
            raw.push_back (0); // filter type: None
            const uint8_t* row = rgbaPixels.data() + (size_t) y * width * 4;
            raw.insert (raw.end(), row, row + (size_t) width * 4);
        }

        std::vector<uint8_t> png;
        // Signature
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        png.insert (png.end(), sig, sig + 8);

        // IHDR
        std::vector<uint8_t> ihdr;
        appendU32 (ihdr, (uint32_t) width);
        appendU32 (ihdr, (uint32_t) height);
        ihdr.push_back (8);   // bit depth
        ihdr.push_back (6);   // colour type: RGBA
        ihdr.push_back (0);   // compression
        ihdr.push_back (0);   // filter
        ihdr.push_back (0);   // interlace
        writeChunk (png, "IHDR", ihdr.data(), (uint32_t) ihdr.size());

        // IDAT: zlib header + stored deflate blocks
        std::vector<uint8_t> idat;
        idat.push_back (0x78); // CMF: 7-bit deflate, 32K window
        idat.push_back (0x9C); // CINFO such that (CMF*256+CINFO) % 31 == 0

        const uint8_t* p = raw.data();
        size_t remaining = raw.size();
        bool firstBlock = true;
        while (remaining > 0 || firstBlock)
        {
            const uint32_t blockLen = (uint32_t) std::min ((size_t) 65535, remaining);
            // BFINAL bit set on last block only
            const bool isLast = (remaining <= (size_t) blockLen);
            idat.push_back (isLast ? 1 : 0); // BFINAL + type(00 stored) => byte value 1 or 0
            appendU16le (idat, blockLen);
            const uint32_t nlen = ~blockLen;
            appendU16le (idat, (uint16_t) (nlen & 0xffff));
            idat.insert (idat.end(), p, p + blockLen);
            p += blockLen;
            remaining -= blockLen;
            firstBlock = false;
        }

        // Adler-32 checksum of the raw data (zlib wrapper requirement), big-endian
        {
            uint32_t a = 1, b = 0;
            for (uint8_t x : raw)
            {
                a += x; if (a >= 65521u) a -= 65521u;
                b += a; if (b >= 65521u) b -= 65521u;
            }
            const uint32_t adler = (b << 16) | a;
            idat.push_back ((uint8_t) ((adler >> 24) & 0xff));
            idat.push_back ((uint8_t) ((adler >> 16) & 0xff));
            idat.push_back ((uint8_t) ((adler >> 8) & 0xff));
            idat.push_back ((uint8_t) (adler & 0xff));
        }

        writeChunk (png, "IDAT", idat.data(), (uint32_t) idat.size());

        // IEND
        writeChunk (png, "IEND", nullptr, 0);

        file.deleteFile(); // FileOutputStream appends to existing files!
        juce::FileOutputStream out (file);
        if (out.failedToOpen())
            return false;
        return out.write (png.data(), png.size());
    }

private:
    static void appendU16le (std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back ((uint8_t) (x & 0xff));
        v.push_back ((uint8_t) ((x >> 8) & 0xff));
    }

    // PNG multi-byte fields are big-endian (network byte order)
    static void appendU32 (std::vector<uint8_t>& v, uint32_t x)
    {
        v.push_back ((uint8_t) ((x >> 24) & 0xff));
        v.push_back ((uint8_t) ((x >> 16) & 0xff));
        v.push_back ((uint8_t) ((x >> 8) & 0xff));
        v.push_back ((uint8_t) (x & 0xff));
    }

    static uint32_t computeCrc32 (const uint8_t* data, size_t length)
    {
        static uint32_t table[256];
        static bool inited = false;
        if (!inited)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            inited = true;
        }

        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < length; ++i)
            crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    static void writeChunk (std::vector<uint8_t>& png, const char* type,
                            const uint8_t* data, uint32_t length)
    {
        appendU32 (png, length);
        std::vector<uint8_t> crcInput;
        for (int i = 0; i < 4; ++i)
            crcInput.push_back ((uint8_t) type[i]);
        if (data && length > 0)
            crcInput.insert (crcInput.end(), data, data + length);

        png.insert (png.end(), crcInput.begin(), crcInput.end());
        const uint32_t crc = computeCrc32 (crcInput.data(), (size_t) crcInput.size());
        appendU32 (png, crc);
    }
};
