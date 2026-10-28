#include "win_workercmds.h"
#include <tl/jobqueue/jobqueue_all.h>
#include <universal/com_workercmds.h>
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <universal/dvar.h>
#include <qcommon/common.h>

int __cdecl SetupWorkerThread(jqBatch *batch)
{
    Sys_InitThread(batch->ParamData[0] + 2);
    return 0;
}

volatile unsigned int nuge_physicsLimit = 1;
jqModule nuge_physicsModule =
{
    .Name = "nuge_physics",
    .Type = JQ_WORKER_GENERIC,
    .Code = nuge_physicsCallback,
};
jqWorkerCmd nuge_physicsWorkerCmd = { &nuge_physicsModule, sizeof(jqBatch), 0, 0, &nuge_physicsLimit, NULL, 0u};

// no workerCmd
jqModule setup_worker_threadsModule =
{
    .Name = "setup_worker_threads",
    .Type = JQ_WORKER_GENERIC,
    .Code = SetupWorkerThread,
};

int __cdecl nuge_physicsCallback(jqBatch *batch)
{
    const char *Name; // [esp+8h] [ebp-14h]
    jqBatch *realbatch; // [esp+18h] [ebp-4h]

    realbatch = (jqBatch *)jqLockData(batch);
    Name = va("nuge_physics: %s", BO1_VA(realbatch->Module->Name));
    PROF_SCOPED_RUNTIME_NAME(Name);
    realbatch->Module->Code(realbatch);
    jqUnlockData(batch);
    return 0;
}

void __cdecl IW_task_manager_add_batch(jqBatch *ptr)
{
    if ( !ptr->Module
        && !Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\win32\\win_workercmds.cpp", 51, 0, "%s", "ptr->Module") )
    {
        __debugbreak();
    }
    Sys_AddWorkerCmdInternal(&nuge_physicsWorkerCmd, (unsigned __int8 *)ptr, 0);
}

void __cdecl IW_task_manager_flush()
{
    Sys_WaitWorkerCmdInternal(&nuge_physicsWorkerCmd);
}

// mod (perf): how many job worker threads to start. Retail started two on every PC (jqEnableWorkers(12): processors 2
// and 3, the thread contexts WORKER0 and WORKER1); the job system and the engine have contexts for eight
// (THREAD_CONTEXT_WORKER0..7), and the jobs that do the most work (model skinning, physics) split into batches that
// any number of workers share. bo1_mod_workers (on the command line; read once): 1-8 workers, 0 = retail's two. Two
// stay the default: a profile of Kino on an 8-core Mac with four workers had each of them busy 14% of the time (the
// frame waited for the render thread and the GPU, not for the jobs), and the extra workers only added spinning.
static int R_ModWorkerCount()
{
    const int wanted = _Dvar_RegisterInt("bo1_mod_workers", 0, 0, 8, 0,
        "mod (perf): job worker threads, set on the command line: 1-8, 0 = retail's 2")->current.integer;
    return wanted > 0 ? wanted : 2;
}

void __cdecl R_InitWorkerCmds()
{
    const int workerCount = R_ModWorkerCount();
    jqInit();
    jqEnableWorkers(((1u << workerCount) - 1) << 2); // retail 12: two workers, processors 2 and 3
    jqSetBatchDataHeapSize(0x4000u, 0x10u);
    jqStart();

    //jqBatch::jqBatch(&batch);
    jqBatch batch; // [esp+0h] [ebp-80h] BYREF
    batch.Module = &setup_worker_threadsModule;// `anonymous namespace'::setup_worker_threadsModule;

    // each worker sets up its thread context (WORKER0 + n) on its own queue: retail 0 on processor 4, 1 on processor 8
    for ( int n = 0; n < workerCount; ++n )
    {
        batch.ParamData[0] = n;
        jqAddBatch(&batch, jqGetWorkerQueue(1 << (2 + n)));
    }

    jqFlush(0, 0);
    Sys_InitWorkerThreadContext();
    Sys_WorkerCmdInit();
    Com_Printf(16, "Job workers: %d\n", workerCount);
}

void __cdecl R_InitWorkerThreads()
{
    R_InitWorkerCmds();
}

