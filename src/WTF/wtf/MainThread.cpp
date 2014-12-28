/*
 * Copyright (C) 2007, 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "LabSoundConfig.h"
#include "MainThread.h"

#include <WTF/CurrentTime.h>
#include <WTF/StdLibExtras.h>
#include <WTF/Threading.h>
#include <deque>
#include <WTF/ThreadSpecific.h>

namespace WTF {

struct FunctionWithContext {
    MainThreadFunction* function;
    void* context;
    ThreadCondition* syncFlag;

    FunctionWithContext(MainThreadFunction* function = 0, void* context = 0, ThreadCondition* syncFlag = 0)
        : function(function)
        , context(context)
        , syncFlag(syncFlag)
    { 
    }
    bool operator == (const FunctionWithContext& o)
    {
        return function == o.function
            && context == o.context
            && syncFlag == o.syncFlag;
    }
};

class FunctionWithContextFinder {
public:
    FunctionWithContextFinder(const FunctionWithContext& m) : m(m) {}
    bool operator() (FunctionWithContext& o) { return o == m; }
    FunctionWithContext m;
};


typedef std::deque<FunctionWithContext> FunctionQueue;

static bool callbacksPaused; // This global variable is only accessed from main thread.
#if !PLATFORM(MAC)
static ThreadIdentifier mainThreadIdentifier;
#endif

static Mutex& mainThreadFunctionQueueMutex()
{
    DEFINE_STATIC_LOCAL(Mutex, staticMutex, ());
    return staticMutex;
}

static FunctionQueue& functionQueue()
{
    DEFINE_STATIC_LOCAL(FunctionQueue, staticFunctionQueue, ());
    return staticFunctionQueue;
}


#if !PLATFORM(MAC)

void initializeMainThread()
{
    static bool initializedMainThread;
    if (initializedMainThread)
        return;
    initializedMainThread = true;

    mainThreadIdentifier = currentThread();

    mainThreadFunctionQueueMutex();
    initializeMainThreadPlatform();
    initializeGCThreads();
}

#else

static pthread_once_t initializeMainThreadKeyOnce = PTHREAD_ONCE_INIT;

static void initializeMainThreadOnce()
{
    mainThreadFunctionQueueMutex();
    initializeMainThreadPlatform();
}

void initializeMainThread()
{
    pthread_once(&initializeMainThreadKeyOnce, initializeMainThreadOnce);
}

static void initializeMainThreadToProcessMainThreadOnce()
{
    mainThreadFunctionQueueMutex();
    initializeMainThreadToProcessMainThreadPlatform();
}

void initializeMainThreadToProcessMainThread()
{
    pthread_once(&initializeMainThreadKeyOnce, initializeMainThreadToProcessMainThreadOnce);
}
#endif

// 0.1 sec delays in UI is approximate threshold when they become noticeable. Have a limit that's half of that.
static const double maxRunLoopSuspensionTime = 0.05;

void dispatchFunctionsFromMainThread()
{
    ASSERT(isMainThread());

    if (callbacksPaused)
        return;

    double startTime = currentTime();

    FunctionWithContext invocation;
    while (true) {
        {
            MutexLocker locker(mainThreadFunctionQueueMutex());
            if (!functionQueue().size())
                break;
            invocation = functionQueue().front();
            functionQueue().pop_front();
        }

        invocation.function(invocation.context);
        if (invocation.syncFlag) {
            MutexLocker locker(mainThreadFunctionQueueMutex());
            invocation.syncFlag->signal();
        }

        // If we are running accumulated functions for too long so UI may become unresponsive, we need to
        // yield so the user input can be processed. Otherwise user may not be able to even close the window.
        // This code has effect only in case the scheduleDispatchFunctionsOnMainThread() is implemented in a way that
        // allows input events to be processed before we are back here.
        if (currentTime() - startTime > maxRunLoopSuspensionTime) {
            scheduleDispatchFunctionsOnMainThread();
            break;
        }
    }
}

void callOnMainThread(MainThreadFunction* function, void* context)
{
    ASSERT(function);
    bool needToSchedule = false;
    {
        MutexLocker locker(mainThreadFunctionQueueMutex());
        needToSchedule = functionQueue().size() == 0;
        functionQueue().push_back(FunctionWithContext(function, context));
    }
    if (needToSchedule)
        scheduleDispatchFunctionsOnMainThread();
}

void callOnMainThreadAndWait(MainThreadFunction* function, void* context)
{
    ASSERT(function);

    if (isMainThread()) {
        function(context);
        return;
    }

    ThreadCondition syncFlag;
    Mutex& functionQueueMutex = mainThreadFunctionQueueMutex();
    MutexLocker locker(functionQueueMutex);
    functionQueue().push_back(FunctionWithContext(function, context, &syncFlag));
    if (functionQueue().size() == 1)
        scheduleDispatchFunctionsOnMainThread();
    syncFlag.wait(functionQueueMutex);
}

void cancelCallOnMainThread(MainThreadFunction* function, void* context)
{
    ASSERT(function);

    MutexLocker locker(mainThreadFunctionQueueMutex());

    FunctionWithContextFinder pred(FunctionWithContext(function, context));

    functionQueue().erase( std::remove_if(functionQueue().begin(), functionQueue().end(), pred), functionQueue().end() );
#if 0
    while (true) {
        // We must redefine 'i' each pass, because the itererator's operator= 
        // requires 'this' to be valid, and remove() invalidates all iterators

        FunctionQueue::iterator i(functionQueue().findIf(pred));
        if (i == functionQueue().end())
            break;
        functionQueue().remove(i);
    }
#endif
}

void setMainThreadCallbacksPaused(bool paused)
{
    ASSERT(isMainThread());

    if (callbacksPaused == paused)
        return;

    callbacksPaused = paused;

    if (!callbacksPaused)
        scheduleDispatchFunctionsOnMainThread();
}

#if !PLATFORM(MAC)
bool isMainThread()
{
    return currentThread() == mainThreadIdentifier;
}
#endif

#if ENABLE(PARALLEL_GC)
static ThreadSpecific<bool>* isGCThread;
#endif

void initializeGCThreads()
{
#if ENABLE(PARALLEL_GC)
    isGCThread = new ThreadSpecific<bool>();
#endif
}

#if ENABLE(PARALLEL_GC)
void registerGCThread()
{
    if (!isGCThread) {
        // This happens if we're running in a process that doesn't care about
        // MainThread.
        return;
    }

    **isGCThread = true;
}

bool isMainThreadOrGCThread()
{
    if (isGCThread->isSet() && **isGCThread)
        return true;

    return isMainThread();
}
#elif PLATFORM(MAC)
// This is necessary because JavaScriptCore.exp doesn't support preprocessor macros.
bool isMainThreadOrGCThread()
{
    return isMainThread();
}
#endif

} // namespace WTF
