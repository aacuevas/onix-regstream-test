#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <time.h>
#include <conio.h>

#include "oni.h"
#include "onix.h"
#include <windows.h>
#pragma comment(lib, "liboni")
#include <stdio.h>
#include <stdlib.h>

#define REG_WAIT_USEC 100
#define PRINT_WAIT_MSEC 2000
#define LOADTEST_SIZE 16*4*8
#define LOADTEST_HZ 30000
#define REG_COUNT 16
const oni_size_t block_read_size = 2048;


//if defined, REG_WAIT_USEC will count time between the end of a read and the beginning of the next.
//if undefined, REG_WAIT_USEC will include the time required to read the registers
//#define REG_INTERVAL_IS_DELAY 

#define LOADTEST_ADDR 11
#define MEMUSAGE_ADDR 10

#define LOADTEST_EN_REG 0
#define LOADTEST_DIV_REG 1
#define LOADTEST_HZ_REG 2
#define LOADTEST_SIZE_REG 3
#define MEMUSAGE_USAGE_REG 4

#define CHECK_ONI_RC(exp) {int rc = exp; if (rc != ONI_ESUCCESS){printf("ONI error %s in line %d:" #exp "\n",oni_error_str(rc),__LINE__); exit(-1);}}

volatile uint64_t fcount, lcount, mcount;
volatile oni_reg_val_t memusage;

typedef struct
{
    oni_ctx ctx;
    HANDLE terminationEvent;
} thread_ctx;

DWORD WINAPI stream_loop(LPVOID lpParam)
{
    thread_ctx* tctx = (thread_ctx*)lpParam;
    oni_ctx ctx = tctx->ctx;

	while (WaitForSingleObject(tctx->terminationEvent,0) != WAIT_OBJECT_0)
	{
        int rc = 0;
        oni_frame_t* frame = NULL;
        rc = oni_read_frame(ctx, &frame);
        if (rc < 0) {
            printf("Stream error: %s\n", oni_error_str(rc));
            SetEvent(tctx->terminationEvent);
            return 1;
        }
        fcount++;
        if (frame->dev_idx == LOADTEST_ADDR) lcount++;
        oni_destroy_frame(frame);
	}
    return 0;
}

DWORD WINAPI reg_loop(LPVOID lpParam)
{
    thread_ctx* tctx = (thread_ctx*)lpParam;
    oni_ctx ctx = tctx->ctx;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);

    while (1)
    {
        int rc = 0;
        oni_reg_val_t val = 0;
        oni_reg_val_t mem = 0;

#ifndef REG_INTERVAL_IS_DELAY

        QueryPerformanceCounter(&start);
#endif // !REG_INTERVAL_IS_DELAY

        for (int i = 0; i < REG_COUNT; i++)
        {
            rc = oni_read_reg(ctx, MEMUSAGE_ADDR, MEMUSAGE_USAGE_REG, &val);
            if (rc != ONI_ESUCCESS) {
                printf("Reg error: %s\n", oni_error_str(rc));
                SetEvent(tctx->terminationEvent);
                return 1;
            }
            mem += val;
        }
        memusage = mem;
        mcount++;

#ifdef REG_INTERVAL_IS_DELAY

        QueryPerformanceCounter(&start);
#endif // !REG_INTERVAL_IS_DELAY

        do {
            QueryPerformanceCounter(&now);
        } while ((now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart < REG_WAIT_USEC);


        if (WaitForSingleObject(tctx->terminationEvent, 0) == WAIT_OBJECT_0)
            break;
    }
    return 0;
}

DWORD WINAPI print_loop(LPVOID lpParam)
{
    thread_ctx* tctx = (thread_ctx*)lpParam;
    while (WaitForSingleObject(tctx->terminationEvent, PRINT_WAIT_MSEC) != WAIT_OBJECT_0)
    {
        printf("Frames: %" PRIu64 "/%" PRIu64 " - Mem: %" PRIu32 " (%" PRIu64 ")\n",lcount,fcount,memusage,mcount);
    }
    return 0;
}

void start_threads(thread_ctx* ctx, HANDLE threads[])
{
    ctx->terminationEvent = CreateEvent(NULL, TRUE, FALSE, TEXT("Termination Event"));

    threads[0] = CreateThread(NULL, 0, stream_loop, (LPVOID)ctx, 0, NULL);
    threads[1] = CreateThread(NULL, 0, reg_loop, (LPVOID)ctx, 0, NULL);
    threads[2] = CreateThread(NULL, 0, print_loop, (LPVOID)ctx, 0, NULL);
}

void stop_threads(thread_ctx* ctx, HANDLE threads[])
{
    SetEvent(ctx->terminationEvent);
    WaitForMultipleObjects(3, threads, TRUE, INFINITE);
    for (int i = 0; i < 3; i++) CloseHandle(threads[i]);
    CloseHandle(ctx->terminationEvent);
}

void reset_context(oni_ctx ctx)
{
    uint32_t val = 1;
    CHECK_ONI_RC(oni_set_opt(ctx, ONI_OPT_RESET, &val, sizeof(val)));
    size_t block_size_sz = sizeof(block_read_size);
    CHECK_ONI_RC(oni_set_opt(ctx, ONI_OPT_BLOCKREADSIZE, &block_read_size, block_size_sz));
}

void configure_loadtest(oni_ctx ctx)
{
    uint32_t clk_hz;
    CHECK_ONI_RC(oni_read_reg(ctx, LOADTEST_ADDR, LOADTEST_HZ_REG, &clk_hz));
    uint32_t val = clk_hz / LOADTEST_HZ;
    CHECK_ONI_RC(oni_write_reg(ctx, LOADTEST_ADDR, LOADTEST_DIV_REG, val));
    val = 1;
    CHECK_ONI_RC(oni_write_reg(ctx, LOADTEST_ADDR, LOADTEST_EN_REG, val));
    val = LOADTEST_SIZE;
    CHECK_ONI_RC(oni_write_reg(ctx, LOADTEST_ADDR, LOADTEST_SIZE_REG, val));
    //reset context to update device table
    reset_context(ctx);
}

void start_acquisition(oni_ctx ctx)
{
    uint32_t val = 2;
    CHECK_ONI_RC(oni_set_opt(ctx, ONI_OPT_RUNNING, &val, sizeof(val)));
}

void stop_acquisition(oni_ctx ctx)
{
    uint32_t val = 0;
    CHECK_ONI_RC(oni_set_opt(ctx, ONI_OPT_RUNNING, &val, sizeof(val)));
}

int main(int argc, char* argv[])
{

    HANDLE threads[3];
    thread_ctx tctx;
    oni_ctx ctx;

    ctx = oni_create_ctx("riffa");
    if (!ctx) 
    {
        printf("Failed to create context\n");
        exit(-1);
    }

    CHECK_ONI_RC(oni_init_ctx(ctx, -1));
    tctx.ctx = ctx;
    
    stop_acquisition(ctx); //just in case
    reset_context(ctx);

    configure_loadtest(ctx);

    start_acquisition(ctx); //start acquisition before the threads, so we are sure no register access is being made
    start_threads(&tctx, threads);

    while (WaitForSingleObject(tctx.terminationEvent, 0) != WAIT_OBJECT_0)
    {
        if (_kbhit())
        {
            if (_getch() == 'q')
            {
                SetEvent(tctx.terminationEvent);
            }
        }
    }
    stop_threads(&tctx, threads);
    stop_acquisition(ctx);
    oni_destroy_ctx(ctx);
    return 0;
}
