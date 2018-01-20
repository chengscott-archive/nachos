// scheduler.cc 
//	Routines to choose the next thread to run, and to dispatch to
//	that thread.
//
// 	These routines assume that interrupts are already disabled.
//	If interrupts are disabled, we can assume mutual exclusion
//	(since we are on a uniprocessor).
//
// 	NOTE: We can't use Locks to provide mutual exclusion here, since
// 	if we needed to wait for a lock, and the lock was busy, we would 
//	end up calling FindNextToRun(), and that would put us in an 
//	infinite loop.
//
// 	Very simple implementation -- no priorities, straight FIFO.
//	Might need to be improved in later assignments.
//
// Copyright (c) 1992-1996 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "debug.h"
#include "scheduler.h"
#include "main.h"

//----------------------------------------------------------------------
// Scheduler::Scheduler
// 	Initialize the list of ready but not running threads.
//	Initially, no ready threads.
//----------------------------------------------------------------------

Scheduler::Scheduler()
{ 
    L1Q_ = new SortedList<Thread *>(Thread::cmpBurstTime);
    L2Q_ = new SortedList<Thread *>(Thread::cmpPriority);
    L3Q_ = new List<Thread *>;
    toBeDestroyed = NULL;
} 

//----------------------------------------------------------------------
// Scheduler::~Scheduler
// 	De-allocate the list of ready threads.
//----------------------------------------------------------------------

Scheduler::~Scheduler()
{ 
    delete L1Q_;
    delete L2Q_;
    delete L3Q_;
} 

//----------------------------------------------------------------------
// Scheduler::ReadyToRun
// 	Mark a thread as ready, but not running.
//	Put it on the ready list, for later scheduling onto the CPU.
//
//	"thread" is the thread to be put on the ready list.
//----------------------------------------------------------------------

void
Scheduler::ReadyToRun (Thread *thread)
{
    ASSERT(kernel->interrupt->getLevel() == IntOff);
    DEBUG(dbgThread, "Putting thread on ready list: " << thread->getName());
	//cout << "Putting thread on ready list: " << thread->getName() << endl ;
    thread->setStatus(READY);

    const int p = thread->getPriority(),
        ticks = kernel->stats->totalTicks;
    thread->setWaitTime(ticks);
    if (p < 50) {
        L3Q_->Append(thread);
        printf("Tick %d: Thread %d is inserted into queue L%d\n",
            ticks, thread->getID(), 3);
    } else if (p < 100) {
        L2Q_->Insert(thread);
        printf("Tick %d: Thread %d is inserted into queue L%d\n",
            ticks, thread->getID(), 2);
        // L2
        if (p > kernel->currentThread->getPriority()
          && kernel->currentThread->getID() != 0)
            kernel->interrupt->Preempt();
    } else {
        L1Q_->Insert(thread);
        printf("Tick %d: Thread %d is inserted into queue L%d\n",
            ticks, thread->getID(), 1);
        int ti = 0.5 * kernel->currentThread->getBurstTime() +
          0.5 * (ticks - kernel->currentThread->getBurstStart());
        printf("[ReadyToRun] appoximated burst time (%d,%d)=(%d,%d)\n",
            kernel->currentThread->getID(), thread->getID(),
            ti, thread->getBurstTime());
        if (thread->getID() != kernel->currentThread->getID()
          && kernel->currentThread->getID() != 0) {
            if (kernel->currentThread->getPriority() >= 100) {
                if (thread->getBurstTime() < ti)
                    kernel->interrupt->Preempt(); // L1
            } else
                kernel->interrupt->Preempt(); // L2
        }
    }
}

//----------------------------------------------------------------------
// Scheduler::FindNextToRun
// 	Return the next thread to be scheduled onto the CPU.
//	If there are no ready threads, return NULL.
// Side effect:
//	Thread is removed from the ready list.
//----------------------------------------------------------------------

Thread *
Scheduler::FindNextToRun ()
{
    ASSERT(kernel->interrupt->getLevel() == IntOff);
    Thread *ret = NULL;
    int level;
    if (!L1Q_->IsEmpty())
        ret = L1Q_->RemoveFront(), level = 1;
    else if (!L2Q_->IsEmpty())
        ret = L2Q_->RemoveFront(), level = 2;
    else if (!L3Q_->IsEmpty())
        ret = L3Q_->RemoveFront(), level = 3;
    if (ret != NULL)
        printf("Tick %d: Thread %d is removed from queue L%d\n",
            kernel->stats->totalTicks, ret->getID(), level);
    return ret;
}

//----------------------------------------------------------------------
// Scheduler::Run
// 	Dispatch the CPU to nextThread.  Save the state of the old thread,
//	and load the state of the new thread, by calling the machine
//	dependent context switch routine, SWITCH.
//
//      Note: we assume the state of the previously running thread has
//	already been changed from running to blocked or ready (depending).
// Side effect:
//	The global variable kernel->currentThread becomes nextThread.
//
//	"nextThread" is the thread to be put into the CPU.
//	"finishing" is set if the current thread is to be deleted
//		once we're no longer running on its stack
//		(when the next thread starts running)
//----------------------------------------------------------------------

void
Scheduler::Run (Thread *nextThread, bool finishing)
{
    Thread *oldThread = kernel->currentThread;
    
    ASSERT(kernel->interrupt->getLevel() == IntOff);
    if (oldThread->getID() == nextThread->getID())
        return;

    if (finishing) {	// mark that we need to delete current thread
         ASSERT(toBeDestroyed == NULL);
	 toBeDestroyed = oldThread;
    }
    
    if (oldThread->space != NULL) {	// if this thread is a user program,
        oldThread->SaveUserState(); 	// save the user's CPU registers
	oldThread->space->SaveState();
    }
    
    oldThread->CheckOverflow();		    // check if the old thread
					    // had an undetected stack overflow

    kernel->currentThread = nextThread;  // switch to the next thread
    nextThread->setStatus(RUNNING);      // nextThread is now running

    // Update oldThread Ticks
    const int ticks = kernel->stats->totalTicks,
        duration = ticks - oldThread->getBurstStart(),
        ti = (oldThread->getBurstTime() + duration) * 0.5;
    kernel->currentThread->setBurstStart(ticks);
    printf("Tick %d: Thread %d is now selected for execution\n",
        ticks, nextThread->getID());
    oldThread->setBurstTime(ti);
    printf("Tick %d: Thread %d is replaced, and it has executed %d ticks\n",
        ticks, oldThread->getID(), duration);

    DEBUG(dbgThread, "Switching from: " << oldThread->getName() << " to: " << nextThread->getName());
    
    // This is a machine-dependent assembly language routine defined 
    // in switch.s.  You may have to think
    // a bit to figure out what happens after this, both from the point
    // of view of the thread and from the perspective of the "outside world".

    SWITCH(oldThread, nextThread);

    // we're back, running oldThread
      
    // interrupts are off when we return from switch!
    ASSERT(kernel->interrupt->getLevel() == IntOff);

    DEBUG(dbgThread, "Now in thread: " << oldThread->getName());

    CheckToBeDestroyed();		// check if thread we were running
					// before this one has finished
					// and needs to be cleaned up
    
    if (oldThread->space != NULL) {	    // if there is an address space
        oldThread->RestoreUserState();     // to restore, do it.
	oldThread->space->RestoreState();
    }
}

//----------------------------------------------------------------------
// Scheduler::CheckToBeDestroyed
// 	If the old thread gave up the processor because it was finishing,
// 	we need to delete its carcass.  Note we cannot delete the thread
// 	before now (for example, in Thread::Finish()), because up to this
// 	point, we were still running on the old thread's stack!
//----------------------------------------------------------------------

void
Scheduler::CheckToBeDestroyed()
{
    if (toBeDestroyed != NULL) {
        delete toBeDestroyed;
	toBeDestroyed = NULL;
    }
}
 
//----------------------------------------------------------------------
// Scheduler::Print
// 	Print the scheduler state -- in other words, the contents of
//	the ready list.  For debugging.
//----------------------------------------------------------------------
void
Scheduler::Print()
{
    cout << "L1 Queue contents:\n";
    L1Q_->Apply(ThreadPrint);
    cout << "L2 Queue contents:\n";
    L2Q_->Apply(ThreadPrint);
    cout << "L3 Queue contents:\n";
    L3Q_->Apply(ThreadPrint);
}

void
Scheduler::Aging()
{
    ListIterator<Thread *> *it1 = new ListIterator<Thread *>(L1Q_),
        *it2 = new ListIterator<Thread *>(L2Q_);
    Thread *thread;
    const int ticks = kernel->stats->totalTicks;
    int priority, new_priority;
    while (!it1->IsDone()) {
        thread = it1->Item();
        if (ticks - thread->getWaitTime() > 1500) {
            //L1Q_->Remove(thread);
            priority = thread->getPriority();
            new_priority = min(priority + 10, 149);
            thread->setPriority(new_priority);
            thread->setWaitTime(ticks);
            if (priority < 149)
                printf("Tick %d: Thread %d changes its priority from %d to %d\n",
                    ticks, thread->getID(), priority, new_priority);
            //printf("wtf Tick %d: Thread %d is removed from queue L%d\n",
            //    ticks, thread->getID(), 1);
            //ReadyToRun(thread);
        }
        it1->Next();
    }
    while (!it2->IsDone()) {
        thread = it2->Item();
        if (ticks - thread->getWaitTime() > 1500) {
            L2Q_->Remove(thread);
            priority = thread->getPriority();
            new_priority = min(priority + 10, 149);
            thread->setPriority(new_priority);
            thread->setWaitTime(ticks);
            printf("Tick %d: Thread %d changes its priority from %d to %d\n",
                ticks, thread->getID(), priority, new_priority);
            printf("test Tick %d: Thread %d is removed from queue L%d\n",
                ticks, thread->getID(), 2);
            ReadyToRun(thread);
        }
        it2->Next();
    }
}
