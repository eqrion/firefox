/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A class which represents a fragment of text (eg inside a text
 * node); if only codepoints below 256 are used, the text is stored as
 * a char*; otherwise the text is stored as a char16_t*
 */

#include "CharacterDataBuffer.h"

#include <algorithm>

#include "CharacterDataBufferImpl.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Latin1.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/SSE.h"
#include "mozilla/ppc.h"
#include "nsBidiUtils.h"
#include "nsCRT.h"
#include "nsReadableUtils.h"
#include "nsUnicharUtils.h"

#define TEXTFRAG_WHITE_AFTER_NEWLINE 50
#define TEXTFRAG_MAX_NEWLINES 7

// Static buffer used for common fragments
static char* sSpaceSharedString[TEXTFRAG_MAX_NEWLINES + 1];
static char* sTabSharedString[TEXTFRAG_MAX_NEWLINES + 1];
static char sSingleCharSharedString[256];

using namespace mozilla::dom;

// static
nsresult CharacterDataBuffer::Init() {
  // Create whitespace strings
  uint32_t i;
  for (i = 0; i <= TEXTFRAG_MAX_NEWLINES; ++i) {
    sSpaceSharedString[i] = new char[1 + i + TEXTFRAG_WHITE_AFTER_NEWLINE];
    sTabSharedString[i] = new char[1 + i + TEXTFRAG_WHITE_AFTER_NEWLINE];
    sSpaceSharedString[i][0] = ' ';
    sTabSharedString[i][0] = ' ';
    uint32_t j;
    for (j = 1; j < 1 + i; ++j) {
      sSpaceSharedString[i][j] = '\n';
      sTabSharedString[i][j] = '\n';
    }
    for (; j < (1 + i + TEXTFRAG_WHITE_AFTER_NEWLINE); ++j) {
      sSpaceSharedString[i][j] = ' ';
      sTabSharedString[i][j] = '\t';
    }
  }

  // Create single-char strings
  for (i = 0; i < 256; ++i) {
    sSingleCharSharedString[i] = i;
  }

  return NS_OK;
}

// static
void CharacterDataBuffer::Shutdown() {
  uint32_t i;
  for (i = 0; i <= TEXTFRAG_MAX_NEWLINES; ++i) {
    delete[] sSpaceSharedString[i];
    delete[] sTabSharedString[i];
    sSpaceSharedString[i] = nullptr;
    sTabSharedString[i] = nullptr;
  }
}

CharacterDataBuffer::~CharacterDataBuffer() {
  ReleaseBuffer();
  MOZ_COUNT_DTOR(CharacterDataBuffer);
}

void CharacterDataBuffer::ReleaseBuffer() {
  if (mState.mIs2b) {
    NS_RELEASE(m2b);
  } else if (mState.mLength && m1b && mState.mInHeap) {
    free(const_cast<char*>(m1b));
  }

  m1b = nullptr;
  mState.mIsBidi = false;

  // Set mState.mIs2b, mState.mInHeap, and mState.mLength = 0 with mAllBits;
  mAllBits = 0;
}

CharacterDataBuffer& CharacterDataBuffer::operator=(
    const CharacterDataBuffer& aOther) {
  ReleaseBuffer();

  if (aOther.mState.mLength) {
    if (!aOther.mState.mInHeap) {
      MOZ_ASSERT(!aOther.mState.mIs2b);
      m1b = aOther.m1b;
    } else if (aOther.mState.mIs2b) {
      m2b = aOther.m2b;
      NS_ADDREF(m2b);
    } else {
      m1b = static_cast<char*>(malloc(aOther.mState.mLength));
      if (m1b) {
        memcpy(const_cast<char*>(m1b), aOther.m1b, aOther.mState.mLength);
      } else {
        // allocate a buffer for a single REPLACEMENT CHARACTER
        m2b = StringBuffer::Alloc(sizeof(char16_t) * 2).take();
        if (!m2b) {
          MOZ_CRASH("OOM!");
        }
        char16_t* data = static_cast<char16_t*>(m2b->Data());
        data[0] = 0xFFFD;  // REPLACEMENT CHARACTER
        data[1] = char16_t(0);
        mState.mIs2b = true;
        mState.mInHeap = true;
        mState.mLength = 1;
        return *this;
      }
    }

    mAllBits = aOther.mAllBits;
  }

  return *this;
}

static inline int32_t FirstNon8BitUnvectorized(const char16_t* str,
                                               const char16_t* end) {
  using p = Non8BitParameters<sizeof(size_t)>;
  const size_t mask = p::mask();
  const uint32_t alignMask = p::alignMask();
  const uint32_t numUnicharsPerWord = p::numUnicharsPerWord();
  const int32_t len = end - str;
  int32_t i = 0;

  // Align ourselves to a word boundary.
  int32_t alignLen = std::min(
      len, int32_t(((-NS_PTR_TO_INT32(str)) & alignMask) / sizeof(char16_t)));
  for (; i < alignLen; i++) {
    if (str[i] > 255) return i;
  }

  // Check one word at a time.
  const int32_t wordWalkEnd =
      ((len - i) / numUnicharsPerWord) * numUnicharsPerWord;
  for (; i < wordWalkEnd; i += numUnicharsPerWord) {
    const size_t word = *reinterpret_cast<const size_t*>(str + i);
    if (word & mask) return i;
  }

  // Take care of the remainder one character at a time.
  for (; i < len; i++) {
    if (str[i] > 255) return i;
  }

  return -1;
}

#if defined(MOZILLA_MAY_SUPPORT_SSE2)
#  include "CharacterDataBufferGenericFwd.h"
#endif

#ifdef __powerpc__
namespace mozilla {
namespace VMX {
int32_t FirstNon8Bit(const char16_t* str, const char16_t* end);
}  // namespace VMX
}  // namespace mozilla
#endif

/*
 * This function returns -1 if all characters in str are 8 bit characters.
 * Otherwise, it returns a value less than or equal to the index of the first
 * non-8bit character in str. For example, if first non-8bit character is at
 * position 25, it may return 25, or for example 24, or 16. But it guarantees
 * there is no non-8bit character before returned value.
 */
static inline int32_t FirstNon8Bit(const char16_t* str, const char16_t* end) {
#ifdef MOZILLA_MAY_SUPPORT_SSE2
  if (mozilla::supports_sse2()) {
    return mozilla::FirstNon8Bit<xsimd::sse2>(str, end);
  }
#elif defined(__powerpc__)
  if (mozilla::supports_vmx()) {
    return mozilla::VMX::FirstNon8Bit(str, end);
  }
#endif

  return FirstNon8BitUnvectorized(str, end);
}

bool CharacterDataBuffer::SetTo(const char16_t* aBuffer, uint32_t aLength,
                                bool aUpdateBidi, bool aForce2b) {
  if (MOZ_UNLIKELY(aLength > NS_MAX_CHARACTER_DATA_BUFFER_LENGTH)) {
    return false;
  }

  if (aForce2b && mState.mIs2b && !m2b->IsReadonly()) {
    // Try to re-use our existing StringBuffer.
    uint32_t storageSize = m2b->StorageSize();
    uint32_t neededSize = aLength * sizeof(char16_t);
    if (!neededSize) {
      if (storageSize < AutoStringDefaultStorageSize) {
        // If we're storing small enough StringBuffer, let's preserve it.
        static_cast<char16_t*>(m2b->Data())[0] = char16_t(0);
        mState.mLength = 0;
        mState.mIsBidi = false;
        return true;
      }
    } else if (neededSize < storageSize &&
               (storageSize / 2) <
                   (neededSize + AutoStringDefaultStorageSize)) {
      // Don't try to reuse the existing StringBuffer, if it would have lots of
      // unused space.
      memcpy(m2b->Data(), aBuffer, neededSize);
      static_cast<char16_t*>(m2b->Data())[aLength] = char16_t(0);
      mState.mLength = aLength;
      mState.mIsBidi = false;
      if (aUpdateBidi) {
        UpdateBidiFlag(aBuffer, aLength);
      }
      return true;
    }
  }

  if (aLength == 0) {
    ReleaseBuffer();
    return true;
  }

  char16_t firstChar = *aBuffer;
  if (!aForce2b && aLength == 1 && firstChar < 256) {
    ReleaseBuffer();
    m1b = sSingleCharSharedString + firstChar;
    mState.mInHeap = false;
    mState.mIs2b = false;
    mState.mLength = 1;
    return true;
  }

  const char16_t* ucp = aBuffer;
  const char16_t* uend = aBuffer + aLength;

  // Check if we can use a shared string
  if (!aForce2b &&
      aLength <= 1 + TEXTFRAG_WHITE_AFTER_NEWLINE + TEXTFRAG_MAX_NEWLINES &&
      (firstChar == ' ' || firstChar == '\n' || firstChar == '\t')) {
    if (firstChar == ' ') {
      ++ucp;
    }

    const char16_t* start = ucp;
    while (ucp < uend && *ucp == '\n') {
      ++ucp;
    }
    const char16_t* endNewLine = ucp;

    char16_t space = ucp < uend && *ucp == '\t' ? '\t' : ' ';
    while (ucp < uend && *ucp == space) {
      ++ucp;
    }

    if (ucp == uend && endNewLine - start <= TEXTFRAG_MAX_NEWLINES &&
        ucp - endNewLine <= TEXTFRAG_WHITE_AFTER_NEWLINE) {
      ReleaseBuffer();
      char** strings = space == ' ' ? sSpaceSharedString : sTabSharedString;
      m1b = strings[endNewLine - start];

      // If we didn't find a space in the beginning, skip it now.
      if (firstChar != ' ') {
        ++m1b;
      }

      mState.mInHeap = false;
      mState.mIs2b = false;
      mState.mLength = aLength;

      return true;
    }
  }

  // See if we need to store the data in ucs2 or not
  int32_t first16bit = aForce2b ? 0 : ::FirstNon8Bit(ucp, uend);

  if (first16bit != -1) {  // aBuffer contains no non-8bit character
    // Use ucs2 storage because we have to
    CheckedUint32 size = CheckedUint32(aLength) + 1;
    if (!size.isValid()) {
      return false;
    }
    size *= sizeof(char16_t);
    if (!size.isValid()) {
      return false;
    }

    RefPtr<StringBuffer> newBuffer = StringBuffer::Alloc(size.value());
    if (!newBuffer) {
      return false;
    }

    ReleaseBuffer();
    memcpy(newBuffer->Data(), aBuffer, aLength * sizeof(char16_t));
    static_cast<char16_t*>(newBuffer->Data())[aLength] = char16_t(0);

    m2b = newBuffer.forget().take();
    mState.mIs2b = true;
    if (aUpdateBidi) {
      UpdateBidiFlag(aBuffer + first16bit, aLength - first16bit);
    }
  } else {
    // Use 1 byte storage because we can
    char* buff = static_cast<char*>(malloc(aLength));
    if (!buff) {
      return false;
    }

    ReleaseBuffer();
    // Copy data
    LossyConvertUtf16toLatin1(Span(aBuffer, aLength), Span(buff, aLength));
    m1b = buff;
    mState.mIs2b = false;
  }

  // Setup our fields
  mState.mInHeap = true;
  mState.mLength = aLength;

  return true;
}

/* static */
bool CharacterDataBuffer::BuildFromUtf8(Span<const char> aUtf8, bool aForce2b,
                                        CharacterDataBuffer& aOut) {
  MOZ_ASSERT(aOut.mState.mLength == 0 && !aOut.mState.mIs2b && !aOut.m1b,
             "aOut must be a freshly constructed, empty buffer");

  const size_t byteLen = aUtf8.Length();

  // A UTF-8 sequence of N bytes decodes to at most N UTF-16 code units (the
  // best ratio, 1:1, is ASCII; every other encoding uses more bytes per
  // unit), so this bounds the resulting length exactly like
  // NS_MAX_CHARACTER_DATA_BUFFER_LENGTH bounds aLength in SetTo() above.
  if (MOZ_UNLIKELY(byteLen > NS_MAX_CHARACTER_DATA_BUFFER_LENGTH)) {
    return false;
  }
  if (byteLen == 0) {
    return true;  // aOut stays empty, matching SetTo()'s aLength == 0 case.
  }

  const char* buf = aUtf8.Elements();
  const auto first = static_cast<unsigned char>(buf[0]);

  if (!aForce2b) {
    // Shared single-char string. Mirrors SetTo()'s aLength == 1 case, plus
    // the 2-byte lead-byte case, which SetTo() gets for free since a
    // char16_t already holds U+0080..U+00FF directly.
    if (byteLen == 1 && first < 0x80) {
      aOut.m1b = sSingleCharSharedString + first;
      aOut.mState.mInHeap = false;
      aOut.mState.mIs2b = false;
      aOut.mState.mLength = 1;
      return true;
    }
    if (byteLen == 2 && (first == 0xC2 || first == 0xC3) &&
        (static_cast<unsigned char>(buf[1]) & 0xC0) == 0x80) {
      const auto ch = static_cast<unsigned char>(
          ((first & 0x1F) << 6) | (static_cast<unsigned char>(buf[1]) & 0x3F));
      aOut.m1b = sSingleCharSharedString + ch;
      aOut.mState.mInHeap = false;
      aOut.mState.mIs2b = false;
      aOut.mState.mLength = 1;
      return true;
    }

    // Shared whitespace-run string. Mirrors the scan in SetTo() above: it
    // only ever matches ' ', '\n' and '\t', all single ASCII bytes, so the
    // scan is byte-exact on UTF-8 input, and a match implies the whole run
    // is pure ASCII (so the UTF-16 length equals the byte length).
    if (byteLen <= 1 + TEXTFRAG_WHITE_AFTER_NEWLINE + TEXTFRAG_MAX_NEWLINES &&
        (first == ' ' || first == '\n' || first == '\t')) {
      const char* cp = buf;
      const char* const end = buf + byteLen;
      if (first == ' ') {
        ++cp;
      }

      const char* start = cp;
      while (cp < end && *cp == '\n') {
        ++cp;
      }
      const char* endNewLine = cp;

      char space = cp < end && *cp == '\t' ? '\t' : ' ';
      while (cp < end && *cp == space) {
        ++cp;
      }

      if (cp == end && endNewLine - start <= TEXTFRAG_MAX_NEWLINES &&
          cp - endNewLine <= TEXTFRAG_WHITE_AFTER_NEWLINE) {
        char** strings = space == ' ' ? sSpaceSharedString : sTabSharedString;
        aOut.m1b = strings[endNewLine - start];
        if (first != ' ') {
          ++aOut.m1b;
        }
        aOut.mState.mInHeap = false;
        aOut.mState.mIs2b = false;
        aOut.mState.mLength = byteLen;
        return true;
      }
    }
  }

  // A byte offset into aUtf8: the length of its longest Latin1-only prefix
  // (0 when aForce2b, so the bidi scan below covers the whole string).
  const size_t latin1UpTo = aForce2b ? 0 : Utf8Latin1UpTo(aUtf8);

  if (!aForce2b && latin1UpTo == byteLen) {
    // The whole string is Latin1 (a superset of ASCII): store as 1b.
    char* out = static_cast<char*>(malloc(byteLen));
    if (!out) {
      return false;
    }
    // Safe: LossyConvertUtf8toLatin1() only produces garbage for code points
    // above U+00FF, and Utf8Latin1UpTo() just proved there are none.
    const size_t written = LossyConvertUtf8toLatin1(aUtf8, Span(out, byteLen));
    MOZ_ASSERT(written <= byteLen);
    if (written != byteLen) {
      // written < byteLen iff some 2-byte UTF-8 sequences (U+0080..U+00FF)
      // collapsed to one byte each; hand back the slack. A shrinking
      // realloc realistically cannot fail; keep the original allocation if
      // it somehow does.
      if (char* shrunk = static_cast<char*>(realloc(out, written))) {
        out = shrunk;
      }
    }
    aOut.m1b = out;
    aOut.mState.mInHeap = true;
    aOut.mState.mIs2b = false;
    aOut.mState.mLength = written;
    // Latin1 text has no RTL characters, so mIsBidi stays false here,
    // matching SetTo() above, which only ever calls UpdateBidiFlag() on the
    // 2b branch.
    return true;
  }

  // General case: decode through UTF-16. Use nsString, not nsAutoString, so
  // the result owns a real StringBuffer that SetTo() below can just AddRef
  // instead of copying again.
  nsString utf16;
  if (!CopyUTF8toUTF16(aUtf8, utf16, fallible)) {
    return false;
  }
  if (!aOut.SetTo(utf16, /* aUpdateBidi = */ false, /* aForce2b = */ true)) {
    return false;
  }

  // Latin1 can't be RTL, so only the non-Latin1 suffix needs scanning.
  // latin1UpTo is a byte offset, but Utf8Latin1UpTo() only ever stops on a
  // scalar-value boundary, so slicing aUtf8 there is safe.
  if (HasRTLCharsUtf8(aUtf8.From(latin1UpTo))) {
    aOut.mState.mIsBidi = true;
  }
  return true;
}

void CharacterDataBuffer::CopyTo(char16_t* aDest, uint32_t aOffset,
                                 uint32_t aCount) {
  const CheckedUint32 endOffset = CheckedUint32(aOffset) + aCount;
  if (!endOffset.isValid() || endOffset.value() > GetLength()) {
    aCount = mState.mLength - aOffset;
  }

  if (aCount) {
    if (mState.mIs2b) {
      memcpy(aDest, Get2b() + aOffset, sizeof(char16_t) * aCount);
    } else {
      const char* cp = m1b + aOffset;
      ConvertLatin1toUtf16(Span(cp, aCount), Span(aDest, aCount));
    }
  }
}

bool CharacterDataBuffer::Append(const char16_t* aBuffer, uint32_t aLength,
                                 bool aUpdateBidi, bool aForce2b) {
  if (!aLength) {
    return true;
  }

  // This is a common case because some callsites create a textnode
  // with a value by creating the node and then calling AppendData.
  if (mState.mLength == 0) {
    return SetTo(aBuffer, aLength, aUpdateBidi, aForce2b);
  }

  // Should we optimize for aData.Length() == 0?

  // FYI: Don't use CheckedInt in this method since here is very hot path
  //      in some performance tests.
  if (NS_MAX_CHARACTER_DATA_BUFFER_LENGTH - mState.mLength < aLength) {
    return false;  // Would be overflown if we'd keep handling.
  }

  if (mState.mIs2b) {
    size_t size = mState.mLength + aLength + 1;
    if (SIZE_MAX / sizeof(char16_t) < size) {
      return false;  // Would be overflown if we'd keep handling.
    }
    size *= sizeof(char16_t);

    // Already a 2-byte string so the result will be too
    StringBuffer* buff = nullptr;
    StringBuffer* bufferToRelease = nullptr;
    if (m2b->IsReadonly()) {
      buff = StringBuffer::Alloc(size).take();
      if (!buff) {
        return false;
      }
      bufferToRelease = m2b;
      memcpy(static_cast<char16_t*>(buff->Data()), m2b->Data(),
             mState.mLength * sizeof(char16_t));
    } else {
      buff = StringBuffer::Realloc(m2b, size);
      if (!buff) {
        return false;
      }
    }

    char16_t* data = static_cast<char16_t*>(buff->Data());
    memcpy(data + mState.mLength, aBuffer, aLength * sizeof(char16_t));
    mState.mLength += aLength;
    m2b = buff;
    data[mState.mLength] = char16_t(0);

    NS_IF_RELEASE(bufferToRelease);

    if (aUpdateBidi) {
      UpdateBidiFlag(aBuffer, aLength);
    }

    return true;
  }

  // Current string is a 1-byte string, check if the new data fits in one byte
  // too.
  int32_t first16bit =
      aForce2b ? 0 : ::FirstNon8Bit(aBuffer, aBuffer + aLength);

  if (first16bit != -1) {  // aBuffer contains no non-8bit character
    size_t size = mState.mLength + aLength + 1;
    if (SIZE_MAX / sizeof(char16_t) < size) {
      return false;  // Would be overflown if we'd keep handling.
    }
    size *= sizeof(char16_t);

    // The old data was 1-byte, but the new is not so we have to expand it
    // all to 2-byte
    StringBuffer* buff = StringBuffer::Alloc(size).take();
    if (!buff) {
      return false;
    }

    // Copy data into buff
    char16_t* data = static_cast<char16_t*>(buff->Data());
    ConvertLatin1toUtf16(Span(m1b, mState.mLength), Span(data, mState.mLength));

    memcpy(data + mState.mLength, aBuffer, aLength * sizeof(char16_t));
    mState.mLength += aLength;
    mState.mIs2b = true;

    if (mState.mInHeap) {
      free(const_cast<char*>(m1b));
    }
    data[mState.mLength] = char16_t(0);
    m2b = buff;

    mState.mInHeap = true;

    if (aUpdateBidi) {
      UpdateBidiFlag(aBuffer + first16bit, aLength - first16bit);
    }

    return true;
  }

  // The new and the old data is all 1-byte
  size_t size = mState.mLength + aLength;
  MOZ_ASSERT(sizeof(char) == 1);
  char* buff;
  if (mState.mInHeap) {
    buff = static_cast<char*>(realloc(const_cast<char*>(m1b), size));
    if (!buff) {
      return false;
    }
  } else {
    buff = static_cast<char*>(malloc(size));
    if (!buff) {
      return false;
    }

    memcpy(buff, m1b, mState.mLength);
    mState.mInHeap = true;
  }

  // Copy aBuffer into buff.
  LossyConvertUtf16toLatin1(Span(aBuffer, aLength),
                            Span(buff + mState.mLength, aLength));

  m1b = buff;
  mState.mLength += aLength;

  return true;
}

/* virtual */
size_t CharacterDataBuffer::SizeOfExcludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  if (Is2b()) {
    return m2b->SizeOfIncludingThisIfUnshared(aMallocSizeOf);
  }

  if (mState.mInHeap) {
    return aMallocSizeOf(m1b);
  }

  return 0;
}

// To save time we only do this when we really want to know, not during
// every allocation
void CharacterDataBuffer::UpdateBidiFlag(const char16_t* aBuffer,
                                         uint32_t aLength) {
  if (mState.mIs2b && !mState.mIsBidi) {
    if (HasRTLChars(Span(aBuffer, aLength))) {
      mState.mIsBidi = true;
    }
  }
}

bool CharacterDataBuffer::BufferEquals(
    const CharacterDataBuffer& aOther) const {
  if (!Is2b()) {
    // We're 1-byte.
    if (!aOther.Is2b()) {
      nsDependentCSubstring ourStr(Get1b(), GetLength());
      return ourStr.Equals(
          nsDependentCSubstring(aOther.Get1b(), aOther.GetLength()));
    }

    // We're 1-byte, the other thing is 2-byte.  Instead of implementing a
    // separate codepath for this, just use our code below.
    return aOther.BufferEquals(*this);
  }

  nsDependentSubstring ourStr(Get2b(), GetLength());
  if (aOther.Is2b()) {
    return ourStr.Equals(
        nsDependentSubstring(aOther.Get2b(), aOther.GetLength()));
  }

  // We can't use EqualsASCII here, because the other string might not
  // actually be ASCII.  Just roll our own compare; do it in the simple way.
  // Bug 1532356 tracks not having to roll our own.
  if (GetLength() != aOther.GetLength()) {
    return false;
  }

  const char16_t* ourChars = Get2b();
  const char* otherChars = aOther.Get1b();
  for (uint32_t i = 0; i < GetLength(); ++i) {
    if (ourChars[i] != static_cast<char16_t>(otherChars[i])) {
      return false;
    }
  }

  return true;
}
