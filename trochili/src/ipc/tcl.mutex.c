/*************************************************************************************************
 *                                     Trochili RTOS Kernel                                      *
 *                                  Copyright(C) 2016 LIUXUMING                                  *
 *                                       www.trochili.com                                        *
 *************************************************************************************************/
#include <string.h>

#include "tcl.types.h"
#include "tcl.config.h"
#include "tcl.cpu.h"
#include "tcl.debug.h"
#include "tcl.kernel.h"
#include "tcl.ipc.h"
#include "tcl.mutex.h"

#if ((TCLC_IPC_ENABLE)&&(TCLC_IPC_MUTEX_ENABLE))

/*************************************************************************************************
 *  功能: 尝试使得线程获得互斥互斥量                                                             *
 *  参数: (1) pThread  线程结构地址                                                              *
 *        (2) pMutex   互斥量结构地址                                                            *
 *        (3) pHiRP    是否有更高优先级就绪                                                      *
 *  返回: (1) 无                                                                                 *
 *  说明：本函数一定是当前线程调用，或者当前线程获得互斥量，或者把互斥量交给别的线程             *
 *        其他线某个程优先级可能提高，可以跟当前线程直接比较优先级                               *
 *************************************************************************************************/
/* 1 在线程环境下，本函数必定被当前线程调用
     1.1 当前线程可能会调用本函数(lock)来占用的互斥量；
     1.2 当前线程可能会调用本函数(free)将互斥量交给其他线程(解除阻塞后的就绪状态)
   2 在isr环境下不可能调用本函数 */
static TState AddLock(TThread* pThread, TMutex* pMutex, TBool* pHiRP, TError* pError)
{
    TState state = eSuccess;
    TError error = OS_IPC_ERR_NONE;

    /* 将互斥量加入线程锁队列，按优先级排列 */
    OsObjListAddPriorityNode(&(pThread->LockList), &(pMutex->LockNode));
    pMutex->Nest = 1U;
    pMutex->Owner = pThread;

    /* 如果线程优先级没有被固定 */
    if (!(pThread->Property & OS_THREAD_PROP_PRIORITY_FIXED))
    {
        /* 线程优先级归mutex管理，不许API进行修改 */
        pThread->Property &= ~(OS_THREAD_PROP_PRIORITY_SAFE);

        /* PCP 得到互斥量之后，当前线程实施天花板算法,因为该线程可能获得多个互斥量，
        该线程的当前优先级可能比新获得的互斥量的天花板还高。 所以这里必须比较一下优先级，
        不能直接设置成新互斥量的天花板优先级 */
        if (pThread->Priority > pMutex->Priority)
        {
            state = OsThreadSetPriority(pThread, pMutex->Priority, eFalse, pHiRP, &error);
        }
    }

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 从线程锁队列里删除互斥量                                                               *
 *  参数: (1) pThread 线程结构地址                                                               *
 *        (2) pMutex  互斥量结构地址                                                             *
 *        (3) pHiRP   是否有更高优先级就绪                                                       *
 *  返回: 无                                                                                     *
 *  说明：当前线程优先级降低，只能跟所有线程比较优先级                                           *
 *************************************************************************************************/
static TState RemoveLock(TThread* pThread, TMutex* pMutex, TBool* pHiRP, TError* pError)
{
    TState state = eSuccess;
    TError error = OS_IPC_ERR_NONE;
    TPriority priority = TCLC_LOWEST_PRIORITY;
    TLinkNode* pHead = (TLinkNode*)0;
    TBool     nflag = eFalse;

    /* 将互斥量从线程锁队列中移除 */
    pHead = pThread->LockList;
    OsObjListRemoveNode(&(pThread->LockList), &(pMutex->LockNode));
    pMutex->Owner = (TThread*)0;
    pMutex->Nest = 0U;

    /* 如果线程优先级没有被固定 */
    if (!(pThread->Property & OS_THREAD_PROP_PRIORITY_FIXED))
    {
        /* 如果线程锁队列为空，则线程优先级恢复到基础优先级,
           在mutex里，线程优先级一定不低于线程基础优先级 */
        if (pThread->LockList == (TLinkNode*)0)
        {
            /* 如果线程没有占有别的互斥量上,则设置线程优先级可以被API修改 */
            pThread->Property |= (OS_THREAD_PROP_PRIORITY_SAFE);

            /* 准备恢复线程优先级 */
            priority = pThread->BasePriority;
            nflag = eTrue;
        }
        else
        {
            /* 因为锁队列是按照优先级下降排序，所以线程的下一个优先级一定是相等或者低的,
               注意删除的锁可能在队列里的任何位置，如果不是在队列头，则不需要处理线程优先级 */
            if (pHead == &(pMutex->LockNode))
            {
                /* 准备恢复线程优先级 */
                priority = *((TPriority*)(pThread->LockList->Data));
                nflag = eTrue;
            }
        }

        /* 如果线程优先级有变化(nflag = eTrue)并且需要调整(priority > pThread->Priority) */
        if (nflag && (priority > pThread->Priority))
        {
            /* 修改线程优先级 */
            state = OsThreadSetPriority(pThread, priority, eFalse, pHiRP, &error);

        }
    }

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 线程获得互斥互斥量                                                                     *
 *  参数: (1) pMutex   互斥量结构地址                                                            *
 *        (2) pHiRP    是否有更高优先级就绪                                                      *
 *        (3) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *        (3) eError   操作错误                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
static TState LockMutex(TMutex* pMutex, TBool* pHiRP, TError* pError)
{
    TState state = eSuccess;
    TError error = OS_IPC_ERR_NONE;

    /* 线程获得互斥量流程
     * Priority Ceilings Protocol
     * ●如果成功, PCP方案下当前线程优先级不会降低,直接返回
     * ●如果失败并且是非阻塞方式访问互斥量，直接返回
     * ●如果失败并且是阻塞方式访问互斥量，则将线程阻塞在互斥量的阻塞队列中，然后调度。
    */
    if (pMutex->Owner == (TThread*)0)
    {
        /*
         * 当前线程获得互斥量，优先级即使有变动也依旧保持最高, 不需要线程优先级抢占，
         * HiRP的值此时无用处
         */
        state = AddLock(OsKernelVariable.CurrentThread, pMutex, pHiRP, &error);
    }
    else if (pMutex->Owner == OsKernelVariable.CurrentThread)
    {
        pMutex->Nest++;
    }
    else
    {
        error = OS_IPC_ERR_NORMAL;
        state = eFailure;
    }

    *pError  = error;
    return state;
}

/*************************************************************************************************
 *  功能: 释放互斥互斥量                                                                         *
 *  参数: (1) pMutex   互斥量结构地址                                                            *
 *        (2) pHiRP    是否有更高优先级就绪                                                      *
 *        (3) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：只有当前线程才能够释放某个互斥量，而当前线程一定不是阻塞状态，                         *
 *        也就不存在链式优先级调整的问题                                                         *
 *************************************************************************************************/
static TState FreeMutex(TMutex* pMutex, TBool* pHiRP, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_NORMAL;
    TIpcContext* pContext;
    TThread* pThread;

    /* 只有占有互斥量的线程才能释放该互斥量 */
    if (pMutex->Owner == OsKernelVariable.CurrentThread)
    {
        /* 在线程嵌套占有互斥量的情况下，需要处理处理互斥量嵌套次数 */
        pMutex->Nest--;

        /*
         * 如果互斥量嵌套数值为0则说明应该彻底释放互斥量,
         * 如果当前线程曾发生过优先级天花板协议，则考虑调整线程优先级
         */
        if (pMutex->Nest == 0U)
        {
            /* 将互斥量从线程锁队列中移除,设置互斥量所有者为空. */
            state = RemoveLock(OsKernelVariable.CurrentThread, pMutex, pHiRP, &error);

            /* 尝试从互斥量阻塞队列中选择合适的线程，使得该线程得到互斥量 */
            if (pMutex->Property & OS_IPC_PROP_PRIMQ_AVAIL)
            {
                pContext = (TIpcContext*)(pMutex->Queue.PrimaryHandle->Owner);
                OsIpcUnblockThread(pContext, eSuccess, OS_IPC_ERR_NONE, pHiRP);

                pThread = (TThread*)(pContext->Owner);
                state = AddLock(pThread, pMutex, pHiRP, &error);
            }
        }

        error = OS_IPC_ERR_NONE;
        state = eSuccess;
    }

    *pError = error;
    return state;
}

/*
 * 互斥量操纵不允许在ISR中被调用
 */

/*************************************************************************************************
 *  功能: 释放互斥互斥量                                                                         *
 *  参数: (1) pMutex   互斥量结构地址                                                            *
 *        (2) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：mutex支持所有权的概念，所以线程释放mutex的操作都是立刻返回的,会释放mutex操作不会导致   *
 *        线程阻塞在mutex的线程阻塞队列上                                                        *
 *************************************************************************************************/
TState TclFreeMutex(TMutex* pMutex, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_UNREADY;
    TReg32 imask;
    TBool HiRP = eFalse;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pError != (TError*)0), "");

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (pMutex->Property & OS_IPC_PROP_READY)
    {
        state = FreeMutex(pMutex, &HiRP, &error);
        if (OsKernelVariable.SchedLockTimes == 0U)
        {
            if (state == eSuccess)
            {
                if (HiRP == eTrue)
                {
                    OsThreadSchedule();
                }
            }
        }

    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 线程获得互斥互斥量                                                                     *
 *  参数: (1) pMutex 互斥量结构地址                                                              *
 *        (2) option   访问邮箱的模式                                                            *
 *        (3) timeo    时限阻塞模式下访问互斥量的时限长度                                        *
 *        (4) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *        (3) eError   操作错误                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
/*
 * 线程采用非阻塞方式、阻塞方式或者时限等待方式获得互斥量
 * Priority Ceilings Protocol
 * ●如果成功, PCP方案下当前线程优先级不会降低,直接返回
 * ●如果失败并且是非阻塞方式访问互斥量，直接返回
 * ●如果失败并且是阻塞方式访问互斥量，则将线程阻塞在互斥量的阻塞队列中，然后调度。
 */
TState TclLockMutex(TMutex* pMutex, TOption option, TTimeTick timeo, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_UNREADY;
    TBool HiRP = eFalse;
    TIpcContext context;
    TReg32 imask;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pError != (TError*)0), "");

    /* 调整操作选项，屏蔽不需要支持的选项 */
    option &= OS_USER_MUTEX_OPTION;

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (pMutex->Property & OS_IPC_PROP_READY)
    {
        state = LockMutex(pMutex, &HiRP, &error);
        if (OsKernelVariable.SchedLockTimes == 0U)
        {
            if (state == eFailure)
            {
                if (option & OS_IPC_OPT_WAIT)
                {
                    /* 如果当前线程不能被阻塞则函数直接返回 */
                    if (OsKernelVariable.CurrentThread->ACAPI & OS_THREAD_ACAPI_BLOCK)
                    {
                        /* 设定线程正在等待的资源的信息 */
                        OsIpcInitContext(&context, (void*)pMutex, 0U, 0U,
                                         (option | OS_IPC_OPT_MUTEX), timeo, &state, &error);

                        /*
                         * 当前线程阻塞在该互斥量的阻塞队列，时限或者无限等待，
                         * 由OS_IPC_OPT_TIMEO参数决定
                         */
                        OsIpcBlockThread(&context, &(pMutex->Queue));

                        /* 当前线程被阻塞，其它线程得以执行 */
                        OsThreadSchedule();

                        OsCpuLeaveCritical(imask);
                        /*
                        * 因为当前线程已经阻塞在IPC对象的线程阻塞队列，所以处理器需要执行别的线程。
                        * 当处理器再次处理本线程时，从本处继续运行。
                        */
                        OsCpuEnterCritical(&imask);

                        /* 清除线程挂起信息 */
                        OsIpcCleanContext(&context);
                    }
                    else
                    {
                        error = OS_IPC_ERR_ACAPI;
                    }
                }
            }
        }
    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 初始化互斥量                                                                           *
 *  参数: (1) pMute    互斥量结构地址                                                            *
 *        (2) pName    互斥量的名称                                                              *
 *        (3) property 互斥量的初始属性                                                          *
 *        (4) priority 互斥量的优先级天花板                                                      *
 *        (5) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState TclCreateMutex(TMutex* pMutex, TChar* pName, TProperty property, TPriority priority,
                      TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_FAULT;
    TReg32 imask;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pName != (TChar*)0), "");
    OS_ASSERT((priority <= TCLC_USER_PRIORITY_LOW), "");
    OS_ASSERT((priority >= TCLC_USER_PRIORITY_HIGH), "");
    OS_ASSERT((pError != (TError*)0), "");

    property &= OS_USER_MUTEX_PROP;

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (!(pMutex->Property & OS_IPC_PROP_READY))
    {
        /* 初始化互斥量对象信息 */
        OsKernelAddObject(&(pMutex->Object), pName, OsMutexObject, (void*)pMutex);

        /* 初始化互斥量基本信息 */
        property |= OS_IPC_PROP_READY;
        pMutex->Property = property;
        pMutex->Nest = 0U;
        pMutex->Owner = (TThread*)0;
        pMutex->Priority = priority;

        pMutex->Queue.PrimaryHandle   = (TLinkNode*)0;
        pMutex->Queue.AuxiliaryHandle = (TLinkNode*)0;
        pMutex->Queue.Property        = &(pMutex->Property);

        pMutex->LockNode.Owner = (void*)pMutex;
        pMutex->LockNode.Data = (TBase32*)(&(pMutex->Priority));
        pMutex->LockNode.Next = 0;
        pMutex->LockNode.Prev = 0;
        pMutex->LockNode.Handle = (TLinkNode**)0;

        error = OS_IPC_ERR_NONE;
        state = eSuccess;
    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 互斥量解除初始化                                                                       *
 *  参数: (1) pMutex   互斥量结构地址                                                            *
 *        (2) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState TclDeleteMutex(TMutex* pMutex, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_FAULT;
    TReg32 imask;
    TBool HiRP = eFalse;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pError != (TError*)0), "");

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (pMutex->Property & OS_IPC_PROP_READY)
    {
        /* 只有当互斥量被线程占有的情况下，才有可能存在被互斥量阻塞的线程 */
        if (pMutex->Owner != (TThread*)0)
        {
            /* 将互斥量从线程锁队列中移除 */
            state = RemoveLock(pMutex->Owner, pMutex, &HiRP, &error);

            /* 将阻塞队列上的所有等待线程都释放，所有线程的等待结果都是OS_IPC_ERR_DELETE，
             * 而且这些线程的优先级一定不高于互斥量所有者的优先级
             */
            OsIpcUnblockAll(&(pMutex->Queue), eFailure, OS_IPC_ERR_DELETE,
                            (void**)0, &HiRP);
        }

        /* 从内核中移除互斥量 */
        OsKernelRemoveObject(&(pMutex->Object));

        /* 清除互斥量对象的全部数据 */
        memset(pMutex, 0U, sizeof(TMutex));

        /*
         * 在线程环境下，如果当前线程的优先级已经不再是线程就绪队列的最高优先级，
         * 并且内核此时并没有关闭线程调度，那么就需要进行一次线程抢占
         */
        if (/* (OsKernelVariable.State == OsThreadState) && */
            (OsKernelVariable.SchedLockTimes == 0U) &&
            (HiRP == eTrue))
        {
            OsThreadSchedule();
        }

        state = eSuccess;
        error = OS_IPC_ERR_NONE;
    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能: 重置互斥量                                                                             *
 *  参数: (1) pMutex   互斥量结构地址                                                            *
 *        (2) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState TclResetMutex(TMutex* pMutex, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_UNREADY;
    TReg32 imask;
    TBool HiRP = eFalse;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pError != (TError*)0), "");

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (pMutex->Property & OS_IPC_PROP_READY)
    {

        /* 只有当互斥量被线程占有的情况下，才有可能存在被互斥量阻塞的线程 */
        if (pMutex->Owner != (TThread*)0)
        {
            /* 将互斥量从线程锁队列中移除 */
            state = RemoveLock(pMutex->Owner, pMutex, &HiRP, &error);

            /* 将阻塞队列上的所有等待线程都释放，所有线程的等待结果都是OS_IPC_ERR_RESET，
               而且这些线程的优先级一定不高于互斥量所有者的优先级 */
            OsIpcUnblockAll(&(pMutex->Queue), eFailure, OS_IPC_ERR_RESET,
                            (void**)0, &HiRP);

            /* 恢复互斥量属性 */
            pMutex->Property &= OS_RESET_MUTEX_PROP;
        }

        /* 占有该资源的进程为空 */
        pMutex->Property &= OS_RESET_MUTEX_PROP;
        pMutex->Owner = (TThread*)0;
        pMutex->Nest = 0U;
        /* pMutex->Priority = keep recent value; */
        pMutex->LockNode.Owner = (void*)0;
        pMutex->LockNode.Data = (TBase32*)0;

        /*
         * 在线程环境下，如果当前线程的优先级已经不再是线程就绪队列的最高优先级，
         * 并且内核此时并没有关闭线程调度，那么就需要进行一次线程抢占
         */
        if (/* (OsKernelVariable.State == OsThreadState) && */
            (OsKernelVariable.SchedLockTimes == 0U) &&
            (HiRP == eTrue))
        {
            OsThreadSchedule();
        }

        state = eSuccess;
        error = OS_IPC_ERR_NONE;

    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能：互斥量阻塞终止函数,将指定的线程从互斥量的线程阻塞队列中终止阻塞并唤醒                  *
 *  参数：(1) pMutex   互斥量结构地址                                                            *
 *        (2) pError   详细调用结果                                                              *
 *  返回: (1) eSuccess 操作成功                                                                  *
 *        (2) eFailure 操作失败                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState TclFlushMutex(TMutex* pMutex, TError* pError)
{
    TState state = eFailure;
    TError error = OS_IPC_ERR_UNREADY;
    TReg32 imask;
    TBool HiRP = eFalse;

    OS_ASSERT((pMutex != (TMutex*)0), "");
    OS_ASSERT((pError != (TError*)0), "");

    OsCpuEnterCritical(&imask);

    /* 只允许在线程代码里调用本函数 */
    if (OsKernelVariable.State != OsThreadState)
    {
        OsKernelVariable.Diagnosis |= OS_KERNEL_DIAG_IPC_ERROR;
        OsDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    if (pMutex->Property & OS_IPC_PROP_READY)
    {
        /* 将互斥量阻塞队列上的所有等待线程都释放，所有线程的等待结果都是OS_IPC_ERR_FLUSH */
        OsIpcUnblockAll(&(pMutex->Queue), eFailure, OS_IPC_ERR_FLUSH, (void**)0, &HiRP);

        state = eSuccess;
        error = OS_IPC_ERR_NONE;

        /*
         * 在线程环境下，如果当前线程的优先级已经不再是线程就绪队列的最高优先级，
         * 并且内核此时并没有关闭线程调度，那么就需要进行一次线程抢占
         */
        if (/* (OsKernelVariable.State == OsThreadState) && */
            (OsKernelVariable.SchedLockTimes == 0U) &&
            (HiRP == eTrue))
        {
            OsThreadSchedule();
        }
    }

    OsCpuLeaveCritical(imask);

    *pError = error;
    return state;
}
#endif

