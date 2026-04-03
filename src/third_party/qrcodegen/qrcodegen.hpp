/*
 * QR Code generator library (C++)
 *
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * - The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 * - The Software is provided "as is", without warranty of any kind, express or
 *   implied, including but not limited to the warranties of merchantability,
 *   fitness for a particular purpose and noninfringement. In no event shall the
 *   authors or copyright holders be liable for any claim, damages or other
 *   liability, whether in an action of contract, tort or otherwise, arising from,
 *   out of or in connection with the Software or the use or other dealings in the
 *   Software.
 */

#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>


namespace qrcodegen {

/*
 * A segment of character/binary/control data in a QR Code symbol.
 * Instances of this class are immutable.
 */
class QrSegment final {

public: class Mode final {

public: static const Mode NUMERIC;
public: static const Mode ALPHANUMERIC;
public: static const Mode BYTE;
public: static const Mode KANJI;
public: static const Mode ECI;

private: int modeBits;
private: int numBitsCharCount[3];

private: Mode(int mode, int cc0, int cc1, int cc2);

public: int getModeBits() const;
public: int numCharCountBits(int ver) const;
};


public: static QrSegment makeBytes(const std::vector<std::uint8_t> &data);
public: static QrSegment makeNumeric(const char *digits);
public: static QrSegment makeAlphanumeric(const char *text);
public: static std::vector<QrSegment> makeSegments(const char *text);
public: static QrSegment makeEci(long assignVal);

public: static bool isNumeric(const char *text);
public: static bool isAlphanumeric(const char *text);

private: const Mode *mode;
private: int numChars;
private: std::vector<bool> data;

public: QrSegment(const Mode &md, int numCh, const std::vector<bool> &dt);
public: QrSegment(const Mode &md, int numCh, std::vector<bool> &&dt);

public: const Mode &getMode() const;
public: int getNumChars() const;
public: const std::vector<bool> &getData() const;
public: static int getTotalBits(const std::vector<QrSegment> &segs, int version);

private: static const char *ALPHANUMERIC_CHARSET;
};



/*
 * A QR Code symbol, which is a type of two-dimension barcode.
 */
class QrCode final {

public: enum class Ecc {
	LOW = 0,
	MEDIUM,
	QUARTILE,
	HIGH,
};

private: static int getFormatBits(Ecc ecl);

public: static QrCode encodeText(const char *text, Ecc ecl);
public: static QrCode encodeBinary(const std::vector<std::uint8_t> &data, Ecc ecl);
public: static QrCode encodeSegments(const std::vector<QrSegment> &segs, Ecc ecl,
	int minVersion=1, int maxVersion=40, int mask=-1, bool boostEcl=true);


private: int version;
private: int size;
private: Ecc errorCorrectionLevel;
private: int mask;
private: std::vector<std::vector<bool>> modules;
private: std::vector<std::vector<bool>> isFunction;

public: QrCode(int ver, Ecc ecl, const std::vector<std::uint8_t> &dataCodewords, int msk);

public: int getVersion() const;
public: int getSize() const;
public: Ecc getErrorCorrectionLevel() const;
public: int getMask() const;
public: bool getModule(int x, int y) const;

private: void drawFunctionPatterns();
private: void drawFormatBits(int msk);
private: void drawVersion();
private: void drawFinderPattern(int x, int y);
private: void drawAlignmentPattern(int x, int y);
private: void setFunctionModule(int x, int y, bool isDark);
private: bool module(int x, int y) const;

private: std::vector<std::uint8_t> addEccAndInterleave(const std::vector<std::uint8_t> &data) const;
private: void drawCodewords(const std::vector<std::uint8_t> &data);
private: void applyMask(int msk);
private: long getPenaltyScore() const;

private: std::vector<int> getAlignmentPatternPositions() const;
private: static int getNumRawDataModules(int ver);
private: static int getNumDataCodewords(int ver, Ecc ecl);
private: static std::vector<std::uint8_t> reedSolomonComputeDivisor(int degree);
private: static std::vector<std::uint8_t> reedSolomonComputeRemainder(const std::vector<std::uint8_t> &data, const std::vector<std::uint8_t> &divisor);
private: static std::uint8_t reedSolomonMultiply(std::uint8_t x, std::uint8_t y);

private: int finderPenaltyCountPatterns(const std::array<int,7> &runHistory) const;
private: int finderPenaltyTerminateAndCount(bool currentRunColor, int currentRunLength, std::array<int,7> &runHistory) const;
private: void finderPenaltyAddHistory(int currentRunLength, std::array<int,7> &runHistory) const;
private: static bool getBit(long x, int i);

public: static constexpr int MIN_VERSION =  1;
public: static constexpr int MAX_VERSION = 40;

private: static const int PENALTY_N1;
private: static const int PENALTY_N2;
private: static const int PENALTY_N3;
private: static const int PENALTY_N4;

private: static const std::int8_t ECC_CODEWORDS_PER_BLOCK[4][41];
private: static const std::int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41];
};



class data_too_long : public std::length_error {
public: explicit data_too_long(const std::string &msg);
};



class BitBuffer final : public std::vector<bool> {
public: BitBuffer();
public: void appendBits(std::uint32_t val, int len);
};

}  // namespace qrcodegen
