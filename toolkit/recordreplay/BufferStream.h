/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_BufferStream_h
#define mozilla_recordreplay_BufferStream_h

#include "InfallibleVector.h"

namespace mozilla {
namespace recordreplay {

// BufferStream is a simplified form of Stream from Recording.h, allowing
// reading or writing to a stream of data backed by an in memory buffer.
class BufferStream {
  InfallibleVector<char>* mOutput;

  const char* mInput;
  size_t mInputSize;

 public:
  BufferStream(const char* aInput, size_t aInputSize)
      : mOutput(nullptr), mInput(aInput), mInputSize(aInputSize) {}

  explicit BufferStream(InfallibleVector<char>* aOutput)
      : mOutput(aOutput), mInput(nullptr), mInputSize(0) {}

  void WriteBytes(const void* aData, size_t aSize) {
    MOZ_RELEASE_ASSERT(mOutput);
    mOutput->append((char*)aData, aSize);
  }

  void WriteScalar32(uint32_t aValue) { WriteBytes(&aValue, sizeof(aValue)); }
  void WriteScalar(size_t aValue) { WriteBytes(&aValue, sizeof(aValue)); }

  void ReadBytes(void* aData, size_t aSize) {
    if (aSize) {
      MOZ_RELEASE_ASSERT(mInput);
      MOZ_RELEASE_ASSERT(aSize <= mInputSize);
      memcpy(aData, mInput, aSize);
      mInput += aSize;
      mInputSize -= aSize;
    }
  }

  uint32_t ReadScalar32() {
    uint32_t rv;
    ReadBytes(&rv, sizeof(rv));
    return rv;
  }

  size_t ReadScalar() {
    size_t rv;
    ReadBytes(&rv, sizeof(rv));
    return rv;
  }

  bool IsEmpty() {
    MOZ_RELEASE_ASSERT(mInput);
    return mInputSize == 0;
  }
};

}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_BufferStream_h
