/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_recordreplay_Thread_h
#define mozilla_recordreplay_Thread_h

#include "mozilla/Atomics.h"
#include "Recording.h"
#include "Lock.h"
#include "Monitor.h"

#include <pthread.h>

namespace mozilla {
namespace recordreplay {

// Threads Overview.
//
// The main thread and each thread that is spawned when thread events are not
// passed through have their behavior recorded.
//
// While recording, each recorded thread has an associated Thread object which
// can be fetched with Thread::Current and stores the thread's ID, its file for
// storing events that occur in the thread, and some other thread local state.
// Otherwise, threads are spawned and destroyed as usual for the process.
//
// While rewinding, the same Thread structure exists for each recorded thread.
// Several additional changes are needed to facilitate rewinding and IPC:
//
// 1. All recorded threads are spawned early on, before any checkpoint has been
//    reached. These threads idle until the process calls the system's thread
//    creation API, and then they run with the start routine the process
//    provided. After the start routine finishes they idle indefinitely,
//    potentially running new start routines if their thread ID is reused. This
//    allows the process to rewind itself without needing to spawn or destroy
//    any threads.
//
// 2. Some additional number of threads are spawned for use for IPC. These have
//    associated Thread structures but are not recorded and always pass through
//    thread events.
//
// 3. All recorded threads and must be able to enter a particular blocking
//    state, under Thread::Wait, when requested by the main thread calling
//    WaitForIdleThreads. For most recorded threads this happens when the
//    thread attempts to take a recorded lock and blocks in Lock::Wait.
//    For other threads (any thread which has diverged from the recording,
//    or JS helper threads even when no recording divergence has occurred),
//    NotifyUnrecordedWait and MaybeWaitForFork are used to enter this state
//    when the thread performs a blocking operation.
//
// 4. Once all recorded threads are idle, the main thread is able to fork.
//    Additional threads created for #2 above do not idle, but they are designed
//    to avoid interfering with the main thread while it forks.

// The ID used by the process main thread.
static const size_t MainThreadId = 1;

// The maximum ID useable by recorded threads.
static const size_t MaxThreadId = 70;

// Information about the execution state of a recorded thread.
class Thread {
 public:
  // Signature for the start function of a thread.
  typedef void (*Callback)(void*);

  // Actions a thread can take with its owned locks.
  enum OwnedLockState {
    // No action by the thread is needed.
    None,

    // The thread must release all of its owned locks.
    NeedRelease,

    // The thread must acquire all of its owned locks.
    NeedAcquire,
  };

 private:
  // Monitor used to protect various thread information (see Thread.h) and to
  // wait on or signal progress for a thread.
  static Monitor* gMonitor;

  // Thread ID in the recording, fixed at creation.
  size_t mId;

  // Whether to pass events in the thread through without recording/replaying.
  // This is only used by the associated thread.
  bool mPassThroughEvents;

  // Whether to crash if we try to record/replay thread events. This is only
  // used by the associated thread.
  size_t mDisallowEvents;

  // Whether execution has diverged from the recording and the thread's
  // recorded events cannot be accessed.
  bool mDivergedFromRecording;

  // Whether this thread should diverge from the recording at the next
  // opportunity. This can be set from any thread.
  AtomicBool mShouldDivergeFromRecording;

  // Start routine and argument which the thread is currently executing. This
  // is cleared after the routine finishes and another start routine may be
  // assigned to the thread. mNeedsJoin specifies whether the thread must be
  // joined before it is completely dead and can be reused. This is protected
  // by the thread monitor.
  Callback mStart;
  void* mStartArg;
  bool mNeedsJoin;

  // ID for this thread used by the system.
  NativeThreadId mNativeId;

  // On macOS, any thread ID given by mach_thread_self or SYS_thread_selfid.
  // This originates from the recording and does not correspond with a system
  // resource when replaying. It is stored here so that we can provide a
  // consistent ID after the process forks and we diverge from the recording.
  uintptr_t mMachId;
  uintptr_t mThreadSelfId;

  // When replaying, the real thread self ID for the current thread.
  uintptr_t mRealThreadSelfId;

  // Stream with events for the thread. This is only used on the thread itself.
  Stream* mEvents;

  // Stack boundary of the thread, protected by the thread monitor.
  uint8_t* mStackBase;
  size_t mStackSize;

  // File descriptor to block on when the thread is idle, fixed at creation.
  FileHandle mIdlefd;

  // File descriptor to notify to wake the thread up, fixed at creation.
  FileHandle mNotifyfd;

  // Whether the thread should attempt to idle.
  AtomicBool mShouldIdle;

  // Whether the thread is waiting on idlefd.
  AtomicBool mIdle;

  // While the thread is idling, whether to release or acquire its owned locks.
  Atomic<OwnedLockState, SequentiallyConsistent, Behavior::DontPreserve>
      mOwnedLockState;

  // Any callback which should be invoked so the thread can make progress,
  // and whether the callback has been invoked yet while the main thread is
  // waiting for threads to become idle. Protected by the thread monitor.
  std::function<void()> mUnrecordedWaitCallback;
  bool mUnrecordedWaitNotified;

  // Identifier of any atomic which this thread currently holds.
  Maybe<size_t> mAtomicLockId;

  // While replaying, recorded locks which this thread owns.
  InfallibleVector<NativeLock*> mOwnedLocks;

  // Information about any lock which this thread is waiting to acquire.
  Maybe<size_t> mPendingLockId;
  Maybe<size_t> mPendingLockAcquiresPosition;

  // Thread local storage, used for non-main threads when replaying.
  // This emulates pthread TLS entries. By associating these TLS entries with
  // the Thread itself, they will be preserved if we fork and then respawn all
  // threads.
  struct StorageEntry {
    uintptr_t mKey;
    void* mData;
    StorageEntry* mNext;
  };
  StorageEntry* mStorageEntries;
  uint8_t* mStorageCursor;
  uint8_t* mStorageLimit;

  uint8_t* AllocateStorage(size_t aSize);

 public:

  // These are used by certain redirections to convey information from the
  // SaveOutput hook to the MiddlemanCall hook.
  uintptr_t mRedirectionValue;
  InfallibleVector<char> mRedirectionData;

  ///////////////////////////////////////////////////////////////////////////////
  // Public Routines
  ///////////////////////////////////////////////////////////////////////////////

  // Accessors for some members that never change.
  size_t Id() { return mId; }
  NativeThreadId NativeId() { return mNativeId; }
  Stream& Events() { return *mEvents; }
  uint8_t* StackBase() { return mStackBase; }
  size_t StackSize() { return mStackSize; }

  inline bool IsMainThread() const { return mId == MainThreadId; }

  // Access the flag for whether this thread is passing events through.
  void SetPassThrough(bool aPassThrough) {
    MOZ_RELEASE_ASSERT(mPassThroughEvents == !aPassThrough);
    mPassThroughEvents = aPassThrough;
  }
  bool PassThroughEvents() const { return mPassThroughEvents; }

  // Access the counter for whether events are disallowed in this thread.
  void BeginDisallowEvents() { mDisallowEvents++; }
  void EndDisallowEvents() {
    MOZ_RELEASE_ASSERT(mDisallowEvents);
    mDisallowEvents--;
  }
  bool AreEventsDisallowed() const { return mDisallowEvents != 0; }

  // Access the flag for whether this thread's execution has diverged from the
  // recording. Once set, this is only unset by rewinding to a point where the
  // flag is clear.
  void DivergeFromRecording() { mDivergedFromRecording = true; }
  bool HasDivergedFromRecording() const { return mDivergedFromRecording; }

  // Mark this thread as needing to diverge from the recording soon, and wake
  // it up in case it can make progress now. The mShouldDivergeFromRecording
  // flag is separate from mDivergedFromRecording so that the thread can only
  // begin diverging from the recording at calls to MaybeDivergeFromRecording.
  void SetShouldDivergeFromRecording() {
    MOZ_RELEASE_ASSERT(CurrentIsMainThread());
    mShouldDivergeFromRecording = true;
    Notify(mId);
  }
  bool MaybeDivergeFromRecording() {
    if (mShouldDivergeFromRecording) {
      mDivergedFromRecording = true;
    }
    return mDivergedFromRecording;
  }

  // Return whether this thread may read or write to its recorded event stream.
  bool CanAccessRecording() const {
    return !PassThroughEvents() && !AreEventsDisallowed() &&
           !HasDivergedFromRecording();
  }

  // Get alternate identifiers for this thread.
  uintptr_t GetMachId() const { return mMachId; }
  uintptr_t GetThreadSelfId() const { return mThreadSelfId; }

  // The actual start routine at the root of all recorded threads, and of all
  // threads when replaying.
  static void ThreadMain(void* aArgument);

  // Bind this Thread to the current system thread, setting Thread::Current()
  // and some other basic state.
  void BindToCurrent();

  // Initialize thread state.
  static void InitializeThreads();

  // Get the current thread, or null if this is a system thread.
  static Thread* Current();

  // Helper to test if this is the process main thread.
  static bool CurrentIsMainThread();

  // Lookup a Thread by various methods.
  static Thread* GetById(size_t aId);
  static Thread* GetByNativeId(NativeThreadId aNativeId);

  // Spawn all non-main recorded threads used for recording/replaying.
  static void SpawnAllThreads();

  // After forking, the new process will only have a main thread. Respawn all
  // recorded non-main threads and restore them to their state in the original
  // process before the fork.
  static void RespawnAllThreadsAfterFork();

  // Spawn the specified thread.
  static NativeThreadId SpawnThread(Thread* aThread);

  // Spawn a non-recorded thread with the specified start routine/argument.
  static void SpawnNonRecordedThread(Callback aStart, void* aArgument);

  // Start an existing thread, for use when the process has called a thread
  // creation system API when events were not passed through. The return value
  // is the native ID of the result.
  static NativeThreadId StartThread(Callback aStart, void* aArgument,
                                    bool aNeedsJoin);

  // Wait until this thread finishes executing its start routine.
  void Join();

  // Give access to the atomic lock which the thread owns.
  Maybe<size_t>& AtomicLockId() { return mAtomicLockId; }

  // Give access to information about lock the thread is waiting to acquire.
  Maybe<size_t>& PendingLockId() { return mPendingLockId; }
  Maybe<size_t>& PendingLockAcquiresPosition() { return mPendingLockAcquiresPosition; }

  // Mark changes in the recorded locks which this thread owns.
  void AddOwnedLock(NativeLock* aLock);
  void RemoveOwnedLock(NativeLock* aLock);
  void MaybeRemoveDestroyedOwnedLock(NativeLock* aLock);

  // For debugging.
  NativeLock* LastOwnedLock();

  // Release or acquire all locks owned by this thread. This does not affect
  // the set of owned locks.
  void ReleaseOrAcquireOwnedLocks(OwnedLockState aState);

  // Get a pointer to the internal storage for this thread for aKey, creating it
  // if necessary.
  void** GetOrCreateStorage(uintptr_t aKey);

  ///////////////////////////////////////////////////////////////////////////////
  // Thread Coordination
  ///////////////////////////////////////////////////////////////////////////////

  // Basic API for threads to coordinate activity with each other, for use
  // during replay. Each Notify() on a thread ID will cause that thread to
  // return from one call to Wait(). Thus, if a thread Wait()'s and then
  // another thread Notify()'s its ID, the first thread will wake up afterward.
  // Similarly, if a thread Notify()'s another thread which is not waiting,
  // that second thread will return from its next Wait() without needing
  // another Notify().
  //
  // If the main thread has called WaitForIdleThreads, then calling
  // Wait() will put this thread in the desired idle state. WaitNoIdle() will
  // never cause the thread to enter the idle state, and should be used
  // carefully to avoid deadlocks with the main thread.
  static void Wait();
  static void WaitNoIdle();
  static void Notify(size_t aId);

  // Wait indefinitely, until the process is rewound.
  static void WaitForever();

  // Wait indefinitely, without allowing this thread to be rewound.
  static void WaitForeverNoIdle();

  // API for handling unrecorded waits in replaying threads.
  //
  // The callback passed to NotifyUnrecordedWait will be invoked at most once
  // by the main thread whenever the main thread is waiting for other threads to
  // become idle, and at most once after the call to NotifyUnrecordedWait if the
  // main thread is already waiting for other threads to become idle.
  //
  // The callback should poke the thread so that it is no longer blocked on the
  // resource. The thread must call MaybeWaitForFork before blocking again.
  //
  // MaybeWaitForFork takes a callback to release any resources before the
  // thread begins idling. The return value is whether this callback was
  // invoked.
  void NotifyUnrecordedWait(const std::function<void()>& aNotifyCallback);
  bool MaybeWaitForFork(const std::function<void()>& aReleaseCallback);

  // Wait for all other threads to enter the idle state necessary for saving
  // or restoring a checkpoint. This may only be called on the main thread.
  static void WaitForIdleThreads();

  // When all other threads are in an idle state, wait for them to either
  // release or reacquire all locks they own, and then reenter the idle state.
  static void OperateOnIdleThreadLocks(OwnedLockState aState);

  // After WaitForIdleThreads(), the main thread will call this to allow
  // other threads to resume execution.
  static void ResumeIdleThreads();

  // Return whether this thread will remain in the idle state entered after
  // WaitForIdleThreads.
  bool ShouldIdle() { return mShouldIdle; }

  // Return the total amount of data consumed by the event streams of every
  // thread. If this increases over time, at least one thread had made progress.
  static size_t TotalEventProgress();

  // Dump information about all running threads to stderr.
  static void DumpThreads();
};

// Mark a region of code where a thread's event stream can be accessed.
// This class has several properties:
//
// - When recording, all writes to the thread's event stream occur atomically
//   within the class: the end of the stream cannot be hit at an intermediate
//   point.
//
// - When replaying, this checks for the end of the stream, and blocks the
//   thread if necessary.
//
// - When replaying, this is a point where the thread can begin diverging from
//   the recording. Checks for divergence should occur after the constructor
//   finishes.
class MOZ_RAII RecordingEventSection {
  Thread* mThread;

 public:
  explicit RecordingEventSection(Thread* aThread) : mThread(aThread) {
    if (!aThread || !aThread->CanAccessRecording()) {
      return;
    }
    if (IsRecording()) {
      MOZ_RELEASE_ASSERT(!aThread->Events().mInRecordingEventSection);
      aThread->Events().mRecording->mStreamLock.ReadLock();
      aThread->Events().mInRecordingEventSection = true;
    } else {
      while (!aThread->MaybeDivergeFromRecording() &&
             aThread->Events().AtEnd()) {
        HitEndOfRecording();
      }
    }
  }

  ~RecordingEventSection() {
    if (!mThread || !mThread->CanAccessRecording()) {
      return;
    }
    if (IsRecording()) {
      mThread->Events().mRecording->mStreamLock.ReadUnlock();
      mThread->Events().mInRecordingEventSection = false;
    }
  }

  bool CanAccessEvents(bool aTolerateDisallowedEvents = false) {
    if (!mThread || mThread->PassThroughEvents() ||
        mThread->HasDivergedFromRecording()) {
      return false;
    }
    if (aTolerateDisallowedEvents && mThread->AreEventsDisallowed()) {
      return false;
    }
    MOZ_RELEASE_ASSERT(mThread->CanAccessRecording());
    return true;
  }
};

}  // namespace recordreplay
}  // namespace mozilla

#endif  // mozilla_recordreplay_Thread_h
