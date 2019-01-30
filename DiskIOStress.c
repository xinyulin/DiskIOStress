/*===================================================
Author: Lin Xin-Yu
E-mail: xinyu0123@gmail.com
====================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <linux/fs.h>

#include "nvme.h"

/*===================================================
| Constant
===================================================*/
#define TRUE                1
#define FALSE               0

#define SIZE_1K             (1024)
#define SIZE_1M             (1024 * SIZE_1K)
#define SIZE_1G             (1024 * SIZE_1M)

#define MAX_THREAD_NUM      256
#define MAX_LOOP_NUM        10000000
#define MAX_TRUNK_SIZE      (20 * SIZE_1M)
#define MAX_TEST_TIME       (86400 * 7)
#define MAX_STREAM_NUM      128
#define MAX_SECTOR_COUNT    2048

#define MAX_SLEEP_TIME      15
#define MAX_SLEEP_DELAY     300
#define MIN_SLEEP_DELAY     30

#define MAX_OTF_DELAY       30
#define MIN_OTF_DELAY       15

#define COLOR_RED           "\x1b[31m"
#define COLOR_GREEN         "\x1b[32m"
#define COLOR_YELLOW        "\x1b[33m"
#define COLOR_BLUE          "\x1b[34m"
#define COLOR_MAGENTA       "\x1b[35m"
#define COLOR_CYAN          "\x1b[36m"
#define COLOR_RESET         "\x1b[0m"

#define U8_MAX              0xFF
#define U16_MAX             0xFFFF
#define U32_MAX             0xFFFFFFFF
#define U64_MAX             0xFFFFFFFFFFFFFFFF

#define S8_MAX              0x7F
#define S16_MAX             0x7FFF
#define S32_MAX             0x7FFFFFFF
#define S64_MAX             0x7FFFFFFFFFFFFFFF

#define LOG2(X)             (31 - __builtin_clz(X))

#define CELSIUS_TO_KELVIN(C) (C + 273)
#define KELVIN_TO_CELSIUS(K) (K - 273)

#define dbg_printf(color, format, ...)   {int tm = get_timeval_sec(gDiskIOInfo.timeval); fprintf(stdout, "\r%sTime(%2dh:%2dm:%2ds) S(%d) O(%d) => ", color, tm / 3600, (tm / 60) % 60, tm % 60, gDiskIOInfo.rtc_delay / 4, gDiskIOInfo.otf_delay / 4);fprintf(stdout, format, ##__VA_ARGS__);fprintf(stdout, "%s", COLOR_RESET); fflush(stdout);}

typedef unsigned long long  U64;
typedef unsigned int        U32;
typedef unsigned short      U16;
typedef unsigned char       U8;

typedef long long           S64;
typedef int                 S32;
typedef short               S16;
typedef char                S8;

enum
{
    STATUS_RUNNING = 0,
    STATUS_PASS,
    STATUS_COMPARE_ERROR,
    STATUS_READ_ERROR,
    STATUS_WRITE_ERROR,
    STATUS_OPEN_ERROR,
    STATUS_FORCE_STOP
};

enum
{
    OTF_DONE = 0,
    OTF_HALT
};

enum
{
    THREAD_STATUS_PAUSE = 0,
    THREAD_STATUS_RUNNING
};

enum
{
    WORKLOAD_SEQ_WRC = 0,
    WORKLOAD_SEQ_WRRC,
    WORKLOAD_SEQ_W1RCN,
    WORKLOAD_RAND_WRC,
    WORKLOAD_MAX
};

enum
{
    PATTERN_ALLZERO = 0,
    PATTERN_ALLONE,
    PATTERN_WORKING_ZERO,
    PATTERN_WORKING_ONE,
    PATTERN_SEQU_INC_BYTE,
    PATTERN_SEQU_DEC_BYTE,
    PATTERN_SEQU_INC_WORD,
    PATTERN_SEQU_DEC_WORD,
    PATTERN_SEQU_INC_DWORD,
    PATTERN_SEQU_DEC_DWORD,
    PATTERN_RANDOM,
    PATTERN_MAX
};

const char* pattern_str[]=
{
    "All Zero",
    "All One",
    "Working Zero",
    "Working One",
    "Sequential Byte Increased",
    "Sequential Byte Decreased",
    "Sequential Word Increased",
    "Sequential Word Decreased",
    "Sequential DWord Increased",
    "Sequential DWord Decreased",
    "Random",
};

enum
{
    OPS_WRITE = 0,
    OPS_READ,
    OPS_NULL,
};

#define IO_ENGINE_SYSTEM    0
#define IO_ENGINE_FILE      1
#define IO_ENGINE_NVME      2

#define DEFAULT_PATTERN     PATTERN_RANDOM
#define DEFAULT_WORKLOAD    WORKLOAD_RAND_WRC
#define DISK_IO_ENGINE      IO_ENGINE_NVME

#define SUPPORT_BLKDISCARD  TRUE
#define SUPPORT_BLKFLUSH    FALSE
#define SUPPORT_DATA_TAG    TRUE
#define SUPPORT_RE_READ     TRUE
#define SUPPORT_DATA_VERIFY TRUE
#define SUPPORT_SLEEP       FALSE
#define SUPPORT_OTF_FW_UPD  FALSE

/*===================================================
| Structure
===================================================*/
typedef struct
{
    U16 enable;
    U16 msl;
    U16 nsa;
    U16 nso;
} StreamDirective_t;

typedef struct
{
    U16 openCnt;
    U16 idTable[MAX_STREAM_NUM];
    U16 rsvd;
} StreamStatus_t;

typedef struct
{
    struct timeval timeval;
    U32 status;
    U32 nr_thread;
    U32 nr_loop;
    U32 nr_trunk;
    U64 sz_trunk;
    U64 nr_block;
    U32 sz_block;
    U32 max_sector;
    U32 slot;
    U32 nsid;
    U32 stream_support;
    struct nvme_id_ctrl id_ctrl;
    struct nvme_id_ns id_ns;
    struct streams_directive_params stream_param;
    StreamStatus_t stream_status;
    U32 rtc_cycle;
    U32 rtc_delay;
    U32 otf_delay;
    U32 otf_flag;
} DiskIOInfo_t;

typedef struct
{
    U32 id;
    U32 fd;
    U32 ops;
    U32 status;
    U32 nr_loop;
    U32 nr_trunk;
    U32 cr_loop;
    U32 cr_trunk;
    U64 block_per_trunk;
    U64 block_start;
    U64 block_end;
    U32 block_count;
    U32 pattern_type;
    U32 workload;
    unsigned char device_path[256];
    unsigned char* bufR;
    unsigned char* bufW;
} ThreadInfo_t;

typedef int (*CmdFunc_t)(char* device, int argc, char* argv[]);

typedef struct
{
    char* cmdStr;
    char* helpStr;
    char* fmtStr;
    CmdFunc_t pFunc;
} CmdTbl_t;

typedef struct
{
                                               /// indicates critical warnings for the state of the controller (bytes[00])
    U32 criticalWarningSpareSpace:1;           ///< available spare space has fallen below the threshold (bits[00])
    U32 criticalWarningTemperature:1;          ///< temperature has exceeded a critical threshold (bits[01])
    U32 criticalWarningMediaInternalError:1;   ///< device reliability degraded due to media related errors or internal error (bits[02])
    U32 criticalWarningReadOnlyMode:1;         ///< media has been placed in read only mode (bits[03])
    U32 criticalWarningVolatileFail:1;         ///< volatile memory backup device has failed (bits[04])
    U32 reserved:3;                            ///< Reserved (bits[07:05])
    U32 temperature:16;                        ///< Contains the temperature of the overall device (bytes[2:1])
    U32 availableSpare:8;                      ///< Contains normalized percentage of the remaining spare capacity available with (bytes[3])

    U8  availableSpareThreshold;               ///< When the Available Spare falls below the threshold indicated in this field  (bytes[4])
    U8  percentageUsed;                        ///< Contains a vendor specific estimate of the percentage of device life used (bytes[5])
    U8  reserved6[26];                         ///<  Reserved (bytes[31:6])

    U64 dataUnitsRead[2];                      ///<  Contains the number of 512 byte data units the host has read from the controller (bytes[47:32])

    U64 dataUnitsWritten[2];                   ///< the number of 512 byte data units the host has written to the controller (bytes[63:48])
    U64 hostReadCommands[2];                   ///< the number of read commands completed by the controller (bytes[79:64])
    U64 hostWriteCommands[2];                  ///< the number of write commands completed by the controller (bytes[95:80])
    U64 controllerBusyTime[2];                 ///< the amount of time the controller is busy with I/O commands (bytes[111:96])
    U64 powerCycles[2];                        ///< the number of power cycles (bytes[127:112])
    U64 powerOnHours[2];                       ///< the number of power-on hours (bytes[143:128])
    U64 unsafeShutdowns[2];                    ///< the number of unsafe shutdowns (bytes[159:144])
    U64 mediaErrors[2];                        ///< the number of occurrences where controller detected unrecovered data integrity error (bytes[175:160])
    U64 numberofErrorInformationLogEntries[2]; ///< the number of Error Information log entries over life of controller (bytes[191:176])
    U32 WarningTempTime;                       ///< Warning Composite Temperature Time (bytes[195:192])
    U32 CriticalTempTime;                      ///< Critical Composite Temperature Time (bytes[199:196])
    U16 TempSensor1;                           ///< Temperature Sensor 1 (bytes[201:200])
    U16 TempSensor2;                           ///< Temperature Sensor 2 (bytes[203:202])
    U8  reserved202[308];                      ///< Reserved (bytes[511:204])
} LogPageSmart_t;

/*===================================================
| Prototype
===================================================*/
void    sig_handler(int signo);
void*   io_thread_handler(void *data);
void*   timer_thread_handler(void *data);
void*   rtc_thread_handler(void *data);
void*   otf_fwupd_thread_handler(void* data);
U32     get_disk_info(char* device);
void    show_result(struct tm* tm_info);
void    set_process_priority(S32 priority);
U32     get_timeval_sec(struct timeval base_timeval);
double  get_timeval_sec_usec(struct timeval base_timeval);
U32     get_core_number(void);
void    dump_compare_error_buffer(ThreadInfo_t* pThrInfo, unsigned char* bufR, unsigned char* bufW, U64 lba);
void    generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type);
void    generate_tag(unsigned char* bufW, ThreadInfo_t* pThrInfo, U64 curr_block);
void    show_usage(int argc, char* argv[]);

int     wl_seq_wrc(ThreadInfo_t* pThrInfo);
int     wl_seq_wrrc(ThreadInfo_t* pThrInfo);
int     wl_seq_w1rcn(ThreadInfo_t* pThrInfo);
int     wl_rand_wrc(ThreadInfo_t* pThrInfo);

int     thread_write(ThreadInfo_t* pThrInfo, U64 lba, U32 len, U32 write_hint);
int     thread_read(ThreadInfo_t* pThrInfo, U64 lba, U32 len);

int     disk_read (int fd, char* buf, U64 lba, U32 len);
int     disk_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint);
int     disk_flush(int fd);
int     disk_trim (int fd, U64 lba, U32 len);
int     disk_reset(int fd);

int     nvme_read (int fd, char* buf, U64 lba, U32 len);
int     nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint);
int     nvme_flush(int fd, int nsid);
int     nvme_trim(int fd, U64 lba, U32 len, U32 nsid);
int     nvme_reset(int fd);
int     nvme_identify(int fd, int cns, int nsid, void* pBuff);
int     nvme_fw_download(int fd, char* pBuff, int data_len, int offset);
int     nvme_fw_commit(int fd, int action, int slot);
int     nvme_get_log(int fd, int logId, char* pBuff, int data_len);
int     nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status);
int     nvme_stream_get_param(int fd, int nsid, struct streams_directive_params* params);
int     nvme_stream_enable(int fd, int nsid, int enable);
int     nvme_stream_alloc_resource(int fd, int nsid, int num);
int     nvme_stream_rel_resource(int fd, int nsid);
int     nvme_stream_rel_id(int fd, int nsid, int id);

U64     hex2dec(char* buf);
void    toLowerCase(char* src, char* dest, int len);
void    dump_buffer(char* buf, U32 len);

int     command_parser(int argc, char* argv[]);
int     single_read      (char* device, int argc, char* argv[]);
int     single_write     (char* device, int argc, char* argv[]);
int     single_trim      (char* device, int argc, char* argv[]);
int     sequential_read  (char* device, int argc, char* argv[]);
int     sequential_write (char* device, int argc, char* argv[]);
int     ramdom_read      (char* device, int argc, char* argv[]);
int     ramdom_write     (char* device, int argc, char* argv[]);
int     reset_controller (char* device, int argc, char* argv[]);
int     fw_activation    (char* device, int argc, char* argv[]);
int     log_page         (char* device, int argc, char* argv[]);

/*===================================================
| Static Variables
===================================================*/
pthread_attr_t attr;
pthread_mutex_t mutex_msg;
pthread_mutex_t mutex_ops;

pthread_t* gThreads        = NULL;
pthread_t* gTimerThread    = NULL;
pthread_t* gRtcThread      = NULL;
pthread_t* gOtfFwUpdThread = NULL;

ThreadInfo_t gThreadInfo[MAX_THREAD_NUM];
DiskIOInfo_t gDiskIOInfo;

const CmdTbl_t cmdList[] =
{
    {"w",       "single write cmd",     "[device] [lba] [len]",                   single_write},
    {"r",       "single read cmd",      "[device] [lba] [len]",                   single_read},
    {"t",       "single trim cmd",      "[device] [lba] [len]",                   single_trim},
    {"rw",      "random write",         "[device] [lba start] [lba end] [count]", ramdom_write},
    {"rr",      "random read",          "[device] [lba start] [lba end] [count]", ramdom_read},
    {"sw",      "sequential write",     "[device] [lba start] [lba end] [count]", sequential_write},
    {"sr",      "sequential read",      "[device] [lba start] [lba end] [count]", sequential_read},
    {"rst",     "reset controller",     "[device]",                               reset_controller},
    {"log",     "log page",             "[device] [log id]",                      log_page},
    {"fwact",   "fw activation",        "[device] [action] [slot]",               fw_activation},
    {"",        "",                     "",                                       NULL}
};

int (*gWorkloadList[WORKLOAD_MAX])(ThreadInfo_t* pThrInfo) =
{
    wl_seq_wrc,
    wl_seq_wrrc,
    wl_seq_w1rcn,
    wl_rand_wrc,
};

/*===================================================
| MAIN FUNCTION
===================================================*/
int main(int argc, char *argv[])
{
    U32 t, rc;
    void *status;
    time_t timer;
    struct tm* tm_info;
    char option;

    srand(time(NULL));
    memset(&gDiskIOInfo, 0x00, sizeof(gDiskIOInfo));

    if (argc <= 1)
    {
        show_usage(argc, argv);
        exit(1);
    }
    else
    {
        if (strcmp(argv[1], "/dev/sda") == 0)
        {
            system("lsblk");
            printf("%sDo you really want to perform this opertion on /dev/sda? (y/n)%s\n", COLOR_RED, COLOR_RESET);
            option = fgetc(stdin);

            switch(option)
            {
                case 'y':
                case 'Y':
                    break;
                default:
                    exit(1);
            }
        }
    }

    //=== turn off swap memory
    system("swapoff -a");

    time(&timer);
    tm_info = localtime(&timer);
    gettimeofday(&gDiskIOInfo.timeval, NULL);
    signal(SIGINT, sig_handler);
    srand(time(0));

    if (argc > 2)
    {
        if (command_parser(argc, argv))
        {
            printf("Undefined parameter!\n");
            show_usage(argc, argv);
            exit(1);
        }
    }
    else
    {
        if (get_disk_info(argv[1]) == -1)
        {
            printf("Device[%s] not found!\n", argv[1]);
            exit(1);
        }

        //----------------------------------------------------------------------
        gDiskIOInfo.nr_loop   = MAX_LOOP_NUM;
        gDiskIOInfo.sz_trunk  = MAX_TRUNK_SIZE;
        gDiskIOInfo.nr_thread = MAX_THREAD_NUM;
        gDiskIOInfo.nr_trunk  = (gDiskIOInfo.nr_block * gDiskIOInfo.sz_block) / gDiskIOInfo.nr_thread / gDiskIOInfo.sz_trunk;
        gDiskIOInfo.status    = STATUS_RUNNING;

        if (gDiskIOInfo.nr_trunk == 0)
        {
            gDiskIOInfo.sz_trunk  = (10 * SIZE_1M);
            gDiskIOInfo.nr_trunk  = 2;
            gDiskIOInfo.nr_thread = (gDiskIOInfo.nr_block * gDiskIOInfo.sz_block) / gDiskIOInfo.nr_trunk / gDiskIOInfo.sz_trunk;
        }

        printf("=== Configuration =========================\n");
        printf("= Loops            : %d\n", gDiskIOInfo.nr_loop);
        printf("= Threads          : %d\n", gDiskIOInfo.nr_thread);
        printf("= Test Time        : %dd %2dh %2dm %2ds\n", MAX_TEST_TIME / 86400, (MAX_TEST_TIME / 3600) % 24, (MAX_TEST_TIME / 60) % 60, MAX_TEST_TIME % 60);
        printf("= Trunk Size       : %lld MB \n", gDiskIOInfo.sz_trunk / SIZE_1M);
        printf("= Data Pattern     : %s\n", pattern_str[DEFAULT_PATTERN]);
        printf("= Device           : %s\n", argv[1]);
        printf("= Max Sector       : %d (%d KB)\n", gDiskIOInfo.max_sector, gDiskIOInfo.max_sector * gDiskIOInfo.sz_block / 1024);
        printf("= Sector Size      : %d Bytes\n", gDiskIOInfo.sz_block);
        printf("= Capacity         : %.2f GB (0x%llX)\n", (double)gDiskIOInfo.nr_block * gDiskIOInfo.sz_block / (1024 * 1024 * 1024), gDiskIOInfo.nr_block);
        printf("= Stream Directive : %s (MSL:%d, SWS:%d KB, SGS:%d MB)\n", (gDiskIOInfo.stream_support)?"SUPPORT":"NOT SUPPORT", gDiskIOInfo.stream_param.msl, gDiskIOInfo.stream_param.sws, gDiskIOInfo.stream_param.sgs);
        printf("= S3 Sleep         : %s (Sleep:%d, Delay:%d ~ %d Sec)\n", (SUPPORT_SLEEP)?"ON":"OFF", MAX_SLEEP_TIME, MIN_SLEEP_DELAY, MAX_SLEEP_DELAY);
        printf("= OTF Update       : %s (Delay:%d ~ %d Sec)\n", (SUPPORT_OTF_FW_UPD)?"ON":"OFF", MIN_OTF_DELAY, MAX_OTF_DELAY);
        printf("=== Start Testing =========================\n");

        //----------------------------------------------------------------------
        pthread_mutex_init(&mutex_msg, NULL);
        pthread_mutex_init(&mutex_ops, NULL);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        gThreads = (pthread_t*)calloc(gDiskIOInfo.nr_thread, sizeof(pthread_t));

        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            gThreadInfo[t].id              = t;
            gThreadInfo[t].nr_trunk        = gDiskIOInfo.nr_trunk;
            gThreadInfo[t].block_per_trunk = (gDiskIOInfo.sz_trunk / gDiskIOInfo.sz_block);
            gThreadInfo[t].block_count     = t + 1;
            gThreadInfo[t].block_count    %= gDiskIOInfo.max_sector;
            if (gThreadInfo[t].block_count == 0) gThreadInfo[t].block_count = gDiskIOInfo.max_sector;
            gThreadInfo[t].block_start     =  t      * gThreadInfo[t].block_per_trunk * gThreadInfo[t].nr_trunk;
            gThreadInfo[t].block_end       = (t + 1) * gThreadInfo[t].block_per_trunk * gThreadInfo[t].nr_trunk;
            gThreadInfo[t].nr_loop         = gDiskIOInfo.nr_loop;
            gThreadInfo[t].pattern_type    = DEFAULT_PATTERN;
            gThreadInfo[t].workload        = DEFAULT_WORKLOAD;

            memcpy(gThreadInfo[t].device_path, argv[1], strlen(argv[1]));

            rc = pthread_create(&gThreads[t], NULL, io_thread_handler, (void*)&gThreadInfo[t]);

            if (rc)
            {
                printf("ERROR(%d) pthread_create() for IO(%d)\n", rc, t);
                exit(-1);
            }
        }

        gTimerThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gTimerThread, NULL, timer_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for Timer\n", rc);
            exit(-1);
        }

    #if SUPPORT_SLEEP == TRUE
        gRtcThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gRtcThread, NULL, rtc_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for RTC\n", rc);
            exit(-1);
        }
    #endif

    #if SUPPORT_OTF_FW_UPD == TRUE
        gOtfFwUpdThread = (pthread_t*)calloc(1, sizeof(pthread_t));
        rc = pthread_create(gOtfFwUpdThread, NULL, otf_fwupd_thread_handler, NULL);

        if (rc)
        {
            printf("ERROR(%d) pthread_create() for OTF\n", rc);
            exit(-1);
        }
    #endif
        //----------------------------------------------------------------------

        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            rc = pthread_join(gThreads[t], &status);

            if (rc)
            {
                printf("ERROR(%d) pthread_join() for IO(%d)\n", rc, t);
            }
        }

        free(gThreads);

        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            gDiskIOInfo.status = STATUS_PASS;
        }

        rc = pthread_join(*gTimerThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for Timer\n", rc);
        }

    #if SUPPORT_SLEEP == TRUE
        rc = pthread_join(*gRtcThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for RTC\n", rc);
        }
    #endif

    #if SUPPORT_OTF_FW_UPD == TRUE
        rc = pthread_join(*gOtfFwUpdThread, &status);

        if (rc)
        {
            printf("ERROR(%d) pthread_join() for OTF\n", rc);
        }
    #endif

        pthread_attr_destroy(&attr);
        pthread_mutex_destroy(&mutex_msg);
        pthread_mutex_destroy(&mutex_ops);
        //----------------------------------------------------------------------

        show_result(tm_info);
    }

    return 0;
}

/*===================================================
| THREAD FUNCTION
===================================================*/
void* io_thread_handler(void *data)
{
    ThreadInfo_t* pThrInfo;

    pThrInfo = (ThreadInfo_t*)data;

    pThrInfo->bufR = calloc(1, SIZE_1M);
    pThrInfo->bufW = calloc(1, SIZE_1M);

    pThrInfo->fd = open(pThrInfo->device_path, O_RDWR);

    if (pThrInfo->fd == -1)
    {
        gDiskIOInfo.status = STATUS_OPEN_ERROR;

        pthread_mutex_lock(&mutex_msg);
        dbg_printf(COLOR_RED, "can not open: %s\n", pThrInfo->device_path);
        pthread_mutex_unlock(&mutex_msg);
    }
    else
    {
        pThrInfo->status = THREAD_STATUS_RUNNING;
        gWorkloadList[pThrInfo->workload](pThrInfo);
        close(pThrInfo->fd);
    }

    free(pThrInfo->bufR);
    free(pThrInfo->bufW);
    pthread_exit(NULL);
}

/*===================================================
| Workload: Sequential Write Read Compare
===================================================*/
int wl_seq_wrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % (gDiskIOInfo.stream_param.msl + 1));
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type);

            // === Write ================================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_WRITE;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if DISK_IO_ENGINE != IO_ENGINE_NVME
            ioctl(pThrInfo->fd, BLKFLSBUF, 0);
        #endif

        #if (SUPPORT_BLKFLUSH == TRUE)
            if ((rand() % 100) == 0)
            {
                disk_flush(pThrInfo->fd);
            }
        #endif

            // === Verify ===============================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_READ;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if SUPPORT_BLKDISCARD == TRUE
            disk_trim(pThrInfo->fd, pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk, pThrInfo->block_per_trunk);
        #endif
        }
    }

    return 0;
}

/*===================================================
| Workload: Sequential Write Once and Read Compare
===================================================*/
int wl_seq_w1rcn(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % gDiskIOInfo.stream_param.msl) + 1;
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            if (pThrInfo->cr_loop == 0)
            {
                generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type);

                // === Write ================================
                cmds          = 0;
                curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
                pThrInfo->ops = OPS_WRITE;
                while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
                {
                    generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                    thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                    cmds++;
                    curr_block += pThrInfo->block_count;

                    if (gDiskIOInfo.status) return 1;
                }

            #if DISK_IO_ENGINE != IO_ENGINE_NVME
                ioctl(pThrInfo->fd, BLKFLSBUF, 0);
            #endif

            #if (SUPPORT_BLKFLUSH == TRUE)
                if ((rand() % 100) == 0)
                {
                    disk_flush(pThrInfo->fd);
                }
            #endif
            }

            // === Verify ===============================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_READ;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

        #if SUPPORT_BLKDISCARD == TRUE
            disk_trim(pThrInfo->fd, pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk, pThrInfo->block_per_trunk);
        #endif
        }
    }

    return 0;
}

/*==================================================================
| Workload: Sequential Write Read(Repeat previous 10 trunks) Compare
===================================================================*/
#define REPEAT_READ_DEPTH   10
int wl_seq_wrrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U32 ret;
    U32 idx;
    U32 ridx;
    U32 write_hint;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % gDiskIOInfo.stream_param.msl) + 1;
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        for (pThrInfo->cr_trunk = 0; pThrInfo->cr_trunk < pThrInfo->nr_trunk; pThrInfo->cr_trunk++)
        {
            generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type);

            // === Write ================================
            cmds          = 0;
            curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk;
            pThrInfo->ops = OPS_WRITE;
            while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
            {
                generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

                cmds++;
                curr_block += pThrInfo->block_count;

                if (gDiskIOInfo.status) return 1;
            }

            if (pThrInfo->cr_trunk >= REPEAT_READ_DEPTH)    ridx = pThrInfo->cr_trunk - REPEAT_READ_DEPTH;
            else                                            ridx = 0;

            // === Verify ===============================
            for (idx = ridx; idx <= pThrInfo->cr_trunk; idx++)
            {
                cmds          = 0;
                curr_block    = pThrInfo->block_start + pThrInfo->block_per_trunk * idx;
                pThrInfo->ops = OPS_READ;
                while (cmds < pThrInfo->block_per_trunk / pThrInfo->block_count)
                {
                    generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
                    thread_read(pThrInfo, curr_block, pThrInfo->block_count);

                    cmds++;
                    curr_block += pThrInfo->block_count;

                    if (gDiskIOInfo.status) return 1;
                }
            }
        }
    }

    return 0;
}

/*===================================================
| Workload: Random Write Read Compare
===================================================*/
int wl_rand_wrc(ThreadInfo_t* pThrInfo)
{
    U64 cmds;
    U64 curr_block;
    U64 start_block;
    U64 end_block;
    U64 cmd_count;
    U32 write_hint = 0;

    if (gDiskIOInfo.stream_support)
    {
        write_hint = (pThrInfo->id % (gDiskIOInfo.stream_param.msl + 1));
    }

    for (pThrInfo->cr_loop = 0; pThrInfo->cr_loop < pThrInfo->nr_loop; pThrInfo->cr_loop++)
    {
        generate_pattern(pThrInfo->bufW, SIZE_1M, pThrInfo->pattern_type);

        pThrInfo->block_count = (rand() % gDiskIOInfo.max_sector) + 1;

        start_block = pThrInfo->block_start + (rand() % (pThrInfo->block_end - pThrInfo->block_start));

        do {
            end_block = start_block + (rand() % pThrInfo->block_per_trunk) + 1;
        } while((start_block + pThrInfo->block_count) > end_block);

        end_block = (end_block > pThrInfo->block_end) ? pThrInfo->block_end: end_block;
        cmd_count = (end_block - start_block) / pThrInfo->block_count;

        // === Sequentail Write ================================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT)
            {
                cmd_count = cmds;
                break;
            }
        }

        // === Random Write ================================
        cmds          = 0;
        pThrInfo->ops = OPS_WRITE;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_write(pThrInfo, curr_block, pThrInfo->block_count, write_hint);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) break;
        }

    #if DISK_IO_ENGINE != IO_ENGINE_NVME
        ioctl(pThrInfo->fd, BLKFLSBUF, 0);
    #endif

    #if (SUPPORT_BLKFLUSH == TRUE)
        if ((rand() % 100) == 0)
        {
            disk_flush(pThrInfo->fd);
        }
    #endif

wl_rand_wrc_halt:

    #if SUPPORT_OTF_FW_UPD == TRUE
        if (gDiskIOInfo.otf_flag == OTF_HALT)
        {
            pThrInfo->status = THREAD_STATUS_PAUSE;

            while (gDiskIOInfo.otf_flag == OTF_HALT && gDiskIOInfo.status == STATUS_RUNNING) usleep(10000);

            pThrInfo->status = THREAD_STATUS_RUNNING;
        }
    #endif

        // === Sequentail read verfiy ===============================
        curr_block    = start_block;
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_read(pThrInfo, curr_block, pThrInfo->block_count);

            curr_block += pThrInfo->block_count;
            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wrc_halt;
        }

        // === Random read verfiy ===============================
        cmds          = 0;
        pThrInfo->ops = OPS_READ;
        while (cmds++ < cmd_count)
        {
            curr_block = start_block + (rand() % cmd_count) * pThrInfo->block_count;
            generate_tag(pThrInfo->bufW, pThrInfo, curr_block);
            thread_read(pThrInfo, curr_block, pThrInfo->block_count);

            if (gDiskIOInfo.status) return 1;
            else if (gDiskIOInfo.otf_flag == OTF_HALT) goto wl_rand_wrc_halt;
        }

    #if SUPPORT_BLKDISCARD == TRUE
        disk_trim(pThrInfo->fd, start_block, end_block - start_block);
    #endif
    }

    return 0;
}

void* timer_thread_handler(void *data)
{
    U32 index = 0;
    char* ops_str[] = {"Write", "Read ", "N/A  "};
    int tm;

    while (1)
    {
        usleep(250000);

        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            dbg_printf(COLOR_RESET, "%d:%d:%d %s   ", gThreadInfo[index].id, gThreadInfo[index].cr_loop, gThreadInfo[index].cr_trunk, ops_str[gThreadInfo[index].ops]);

            index = (++index == gDiskIOInfo.nr_thread) ? 0: index;

            tm = get_timeval_sec(gDiskIOInfo.timeval);

            if (tm >= MAX_TEST_TIME)
            {
                gDiskIOInfo.status = STATUS_FORCE_STOP;
            }
        }
        else
        {
            pthread_exit(NULL);
        }
    }
}

void* rtc_thread_handler(void *data)
{
    char cmd[80];
    sprintf(cmd, "sudo rtcwake -m mem -s %d >rtc.log", MAX_SLEEP_TIME);

    gDiskIOInfo.rtc_delay = MIN_SLEEP_DELAY * 4;

    while (1)
    {
        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            if (gDiskIOInfo.rtc_delay == 0)
            {
                pthread_mutex_lock(&mutex_ops);
                system(cmd);
                gDiskIOInfo.rtc_delay = (MIN_SLEEP_DELAY + (rand() % (MAX_SLEEP_DELAY - MIN_SLEEP_DELAY))) * 4;
                gDiskIOInfo.rtc_cycle++;
                pthread_mutex_unlock(&mutex_ops);
            }
            else
            {
                usleep(250000);
                gDiskIOInfo.rtc_delay--;
            }
        }
        else
        {
            pthread_exit(NULL);
        }
    }
}

void* otf_fwupd_thread_handler(void* data)
{
    U32 flag;
    U32 idx;
    U32 slot = 0;
    int fd = open(gThreadInfo[0].device_path, O_RDWR);
    int rst_fd;
    char baseDev[11] = {0};

    strncpy(baseDev, gThreadInfo[0].device_path, 10);
    rst_fd = open(baseDev, O_SYNC | O_RDWR);

    gDiskIOInfo.otf_delay = MIN_OTF_DELAY * 4;

    while (1)
    {
        if (gDiskIOInfo.status == STATUS_RUNNING)
        {
            if (gDiskIOInfo.otf_delay < 2)
            {
                gDiskIOInfo.otf_flag = OTF_HALT;

                flag = TRUE;
                for (idx = 0; idx < gDiskIOInfo.nr_thread; idx++)
                {
                    if (gThreadInfo[idx].status == THREAD_STATUS_RUNNING)
                    {
                        flag = FALSE;
                        break;
                    }
                }

                if (flag)
                {
                    pthread_mutex_lock(&mutex_ops);
                    nvme_fw_commit(fd, 2, ((slot++) % 2) + 1);
                    nvme_reset(rst_fd);

                    gDiskIOInfo.otf_delay = (MIN_OTF_DELAY + (rand() % (MAX_OTF_DELAY - MIN_OTF_DELAY))) * 4;
                    gDiskIOInfo.otf_flag  = OTF_DONE;
                    pthread_mutex_unlock(&mutex_ops);
                }

                usleep(250000);
            }
            else
            {
                usleep(250000);
                gDiskIOInfo.otf_delay--;
            }
        }
        else
        {
            close(fd);
            close(rst_fd);
            pthread_exit(NULL);
        }
    }
}

/*===================================================
| SINGAL FUNCTION
===================================================*/
void sig_handler(int signo)
{
    U32 t, rc;
    void *status;

    gDiskIOInfo.status = STATUS_FORCE_STOP;

    dbg_printf(COLOR_BLUE, "Destroy all threads, please wait a while...");

    if (gThreads)
    {
        for (t = 0; t < gDiskIOInfo.nr_thread; t++)
        {
            rc = pthread_join(gThreads[t], &status);
        }

        free(gThreads);
    }

    if (gTimerThread)
    {
        rc = pthread_join(*gTimerThread, &status);
    }

    dbg_printf(COLOR_BLUE, "Manual Stop!                               \n");

#if SUPPORT_SLEEP == TRUE
    if (gRtcThread)
    {
        rc = pthread_join(*gRtcThread, &status);
    }
#endif

#if SUPPORT_OTF_FW_UPD == TRUE
    if (gOtfFwUpdThread)
    {
        rc = pthread_join(*gOtfFwUpdThread, &status);
    }
#endif

    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&mutex_msg);
    pthread_mutex_destroy(&mutex_ops);
    exit(1);
}

/*===================================================
| IMPLEMENTATION
===================================================*/
U32 get_disk_info(char* device)
{
    int fd;
    U32 idx;
    U32 sectorSize;
    struct streams_directive_params params;

    #if DISK_IO_ENGINE == IO_ENGINE_NVME
    sscanf(device, "/dev/nvme%dn%d", &gDiskIOInfo.slot, &gDiskIOInfo.nsid);
    #endif

    fd = open(device, O_RDONLY);

    if (fd != -1)
    {
        U32 allocCnt;

        ioctl(fd, BLKSSZGET, &gDiskIOInfo.sz_block);
        ioctl(fd, BLKGETSIZE, &gDiskIOInfo.nr_block);
        ioctl(fd, BLKSECTGET, &gDiskIOInfo.max_sector);

        if (gDiskIOInfo.max_sector > 2048) gDiskIOInfo.max_sector = 2048;

        gDiskIOInfo.max_sector = gDiskIOInfo.max_sector * 512 / gDiskIOInfo.sz_block;

        #if DISK_IO_ENGINE == IO_ENGINE_NVME
            nvme_identify(fd, 1, 0, (void*)&gDiskIOInfo.id_ctrl);
            nvme_identify(fd, 0, gDiskIOInfo.nsid, (void*)&gDiskIOInfo.id_ns);

            if (gDiskIOInfo.id_ctrl.oacs & NVME_CTRL_OACS_DIRECTIVES)
            {
                gDiskIOInfo.stream_support = TRUE;

                nvme_stream_enable(fd, gDiskIOInfo.nsid, TRUE);
                nvme_stream_get_param(fd, gDiskIOInfo.nsid, &gDiskIOInfo.stream_param);

                gDiskIOInfo.stream_param.sws = (1 << gDiskIOInfo.id_ns.lbaf[gDiskIOInfo.id_ns.flbas & NVME_NS_FLBAS_LBA_MASK].ds) * gDiskIOInfo.stream_param.sws / 1024;
                gDiskIOInfo.stream_param.sgs = gDiskIOInfo.stream_param.sws * gDiskIOInfo.stream_param.sgs / 1024;

                allocCnt = gDiskIOInfo.stream_param.msl - gDiskIOInfo.stream_param.nsa;

                if (allocCnt)
                {
                    nvme_stream_rel_resource(fd, gDiskIOInfo.nsid);
                    nvme_stream_alloc_resource(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_param.msl);
                }

                if (gDiskIOInfo.stream_param.nso)
                {
                    nvme_stream_get_status(fd,  gDiskIOInfo.nsid, &gDiskIOInfo.stream_status);

                    for (idx = 0; idx < gDiskIOInfo.stream_status.openCnt; idx++)
                    {
                        nvme_stream_rel_id(fd, gDiskIOInfo.nsid, gDiskIOInfo.stream_status.idTable[idx]);
                    }
                }
            }
        #endif

        close(fd);

        gDiskIOInfo.nr_block /= (gDiskIOInfo.sz_block / 512);
    }

    return fd;
}

void show_result(struct tm* tm_info)
{
    U32 tm;
    FILE* logFp;
    char buffer[26];

    logFp = fopen("log.txt", "a+");

    strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", tm_info);

    tm = get_timeval_sec(gDiskIOInfo.timeval);

    switch (gDiskIOInfo.status)
    {
        case STATUS_PASS:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => COMPARE PASS\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s COMPARE PASS %s==========================\n", COLOR_GREEN, COLOR_RESET);
            break;
        case STATUS_COMPARE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => Compare ERROR!\n", buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s COMPARE ERROR %s===========================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_WRITE_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => WRITE ERROR!\n",   buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s WRITE ERROR %s=============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_READ_ERROR:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => READ ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s READ ERROR %s==============================\n", COLOR_RED, COLOR_RESET);
            break;
        case STATUS_FORCE_STOP:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => TIMOUET COMPLETE!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("\n===%s TIMOUET COMPLETE %s======================\n", COLOR_CYAN, COLOR_RESET);
            break;
        default:
            fprintf(logFp, "=== Start Time:%s Testing Time:(%2dh:%2dm:%2ds) => UNKNOWN ERROR!\n",    buffer, tm / 3600, (tm / 60) % 60, tm % 60);
            printf("===%s UNKNOWN ERROR:%d %s=============================\n", COLOR_RED, gDiskIOInfo.status, COLOR_RESET);
            break;
    }

    fclose(logFp);
}

void set_process_priority(S32 priority)
{
    int which = PRIO_PROCESS;
    id_t pid;
    int ret;

    pid = getpid();
    ret = setpriority(which, pid, priority);
}

U32 get_timeval_sec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return curr_timeval.tv_sec - base_timeval.tv_sec;
}

double get_timeval_sec_usec(struct timeval base_timeval)
{
    struct timeval curr_timeval;

    gettimeofday(&curr_timeval, NULL);

    return (curr_timeval.tv_sec + curr_timeval.tv_usec / 1000000.0) - (base_timeval.tv_sec + base_timeval.tv_usec / 1000000.0);
}

U32 get_core_number(void)
{
    FILE* fp = popen("cat /proc/cpuinfo | grep processor | wc -l", "r");
    U32 nr_core;

    fscanf(fp, "%d", &nr_core);

    pclose(fp);

    return nr_core;
}

void dump_compare_error_buffer(ThreadInfo_t* pThrInfo, unsigned char* bufR, unsigned char* bufW, U64 lba)
{
    FILE* fp;
    U32 i, offset, lba_offset;
    U32 dump_offset;
    U32 lba_tag, hash_tag;
    char filename[256];
    char* bufXOR;

    bufXOR = calloc(1, SIZE_1M);

    for (i = 0; i < pThrInfo->block_count * gDiskIOInfo.sz_block; i++)
    {
        if (bufW[i] != bufR[i])
        {
            offset = i;
            break;
        }
    }

    #if SUPPORT_DATA_TAG == TRUE
        dump_offset = offset & 0xFFFFFFF0;
    #else
        dump_offset = offset;
    #endif

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_good.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufW, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_bad.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufR, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    for (i = 0; i < (pThrInfo->block_count * gDiskIOInfo.sz_block); i++)
    {
        bufXOR[i] = bufR[i] ^ bufW[i];
    }

    sprintf(filename, "%08llX_%03X_%X_%d_%d_%d_xor.dat", lba, pThrInfo->block_count, offset, pThrInfo->id, pThrInfo->cr_loop, pThrInfo->cr_trunk);

    fp = fopen(filename, "wb+");
    fwrite(bufXOR, 1, pThrInfo->block_count * gDiskIOInfo.sz_block, fp);
    fclose(fp);

    lba_offset = offset / gDiskIOInfo.sz_block;
    lba += lba_offset;

    pthread_mutex_lock(&mutex_msg);
    printf("%s=== GOOD Pattern === LBA:%08llX (LAA:%08llX) Offset:%08X", COLOR_GREEN, lba, lba / 8, offset);

    for (i = 0; i < 512; i++)
    {
        if ((i % 16) == 0)  printf("\n[%08X]:", dump_offset + i);
        printf(" %02X", bufW[dump_offset + i] & 0xFF);
    }

    printf("%s\n", COLOR_RESET);

    printf("%s=== BAD  Pattern === LBA:%08llX (LAA:%08llX) Offset:%08X", COLOR_MAGENTA, lba, lba / 8, offset);
    for (i = 0; i < 512; i++)
    {
        if ((i % 16) == 0)  printf("\n[%08X]:", dump_offset + i);
        printf(" %02X", bufR[dump_offset + i] & 0xFF);

        #if SUPPORT_DATA_TAG == TRUE
            if (((dump_offset + i) & 0x1FF) == 0xF)
            {
                lba_tag = *((U32*)&bufR[(dump_offset + i - 0xF)]);
                printf("%s|%08X", COLOR_RESET, lba_tag);

                if (lba_tag == lba) printf(":OK%s", COLOR_MAGENTA);
                else                printf(":ERROR%s", COLOR_MAGENTA);
            }

            if (((dump_offset + i) & 0x1FF) == 0x1FF)
            {
                hash_tag = *((U32*)&bufR[(dump_offset + i - 3)]);
                printf("%s|%08X", COLOR_RESET, hash_tag);

                if (hash_tag == ((~((U32)lba + 0x12345678)) & 0xFFFFFFFF))  printf(":OK%s", COLOR_MAGENTA);
                else                                                        printf(":ERROR%s", COLOR_MAGENTA);

                lba++;
            }
        #endif
    }

    printf("%s\n", COLOR_RESET);
    pthread_mutex_unlock(&mutex_msg);

    free(bufXOR);
}

void generate_tag(unsigned char* bufW, ThreadInfo_t* pThrInfo, U64 curr_block)
{
#if SUPPORT_DATA_TAG == TRUE
    U32 *ptr;
    U32 tag;
    U32 idx;

    for (idx = 0; idx < pThrInfo->block_count; idx++)
    {
        tag  = curr_block + idx;
        ptr  = (U32*)(bufW + gDiskIOInfo.sz_block * idx);
        *ptr = tag;

        ptr  = (U32*)(bufW + gDiskIOInfo.sz_block * idx + gDiskIOInfo.sz_block - 4);
        *ptr = ~(tag + 0x12345678);
    }
#endif
}

void generate_pattern(unsigned char* bufW, U32 size, U32 pattern_type)
{
    U32 i;

    switch (pattern_type)
    {
        case PATTERN_ALLZERO:
            memset(bufW, 0x00, size);
            break;
        case PATTERN_ALLONE:
            memset(bufW, 0xFF, size);
            break;
        case PATTERN_WORKING_ONE:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = 1 << i;
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_WORKING_ZERO:
            for (i = 0; i < 32; i++)
            {
                *(((U32*)bufW) + i) = ~(1 << i);
            }

            for (i = 1; i < size / 32 / 4; i++)
            {
                memcpy(&bufW[i * 32 * 4], &bufW[0], 32 * 4);
            }

            break;
        case PATTERN_SEQU_INC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_DWORD:
            for (i = 0; i < size / 4; i++)
            {
                *(((U32*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_WORD:
            for (i = 0; i < size / 2; i++)
            {
                *(((U16*)bufW) + i) = U32_MAX - i;
            }
            break;
        case PATTERN_SEQU_INC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = i;
            }
            break;
        case PATTERN_SEQU_DEC_BYTE:
            for (i = 0; i < size; i++)
            {
                *(bufW + i) = U32_MAX - i;
            }
            break;
        case PATTERN_RANDOM:
        {
            for (i = 0; i < (32 * SIZE_1K); i++)
            {
                bufW[i] = rand() % 0xFF;
            }

            for (i = 1; i < size / 32 / SIZE_1K; i++)
            {
                memcpy(&bufW[i * SIZE_1K * 32], &bufW[0], SIZE_1K * 32);
            }
            break;
        }
        default:
            printf("Error:Invalid Pattern Type\n");
            break;
    }
}

int thread_write(ThreadInfo_t* pThrInfo, U64 lba, U32 len, U32 write_hint)
{
    if (disk_write(pThrInfo->fd, pThrInfo->bufW, lba, len, write_hint))
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_WRITE_ERROR;
        dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] WRITE ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        return 1;
    }

    return 0;
}

int thread_read(ThreadInfo_t* pThrInfo, U64 lba, U32 len)
{
    U32 ret;

    if (disk_read(pThrInfo->fd, pThrInfo->bufR, lba, len))
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_READ_ERROR;
        dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] READ ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        return 1;
    }

#if SUPPORT_DATA_VERIFY == TRUE
    ret = memcmp(pThrInfo->bufR, pThrInfo->bufW, len * gDiskIOInfo.sz_block);

    if (ret)
    {
        pthread_mutex_lock(&mutex_msg);
        gDiskIOInfo.status = STATUS_COMPARE_ERROR;
        dbg_printf(COLOR_RED, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] COMPARE ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
        pthread_mutex_unlock(&mutex_msg);

        dump_compare_error_buffer(pThrInfo, pThrInfo->bufR, pThrInfo->bufW, lba);

    #if SUPPORT_RE_READ == TRUE
        if (disk_read(pThrInfo->fd, pThrInfo->bufR, lba, pThrInfo->block_count))
        {
            pthread_mutex_lock(&mutex_msg);
            gDiskIOInfo.status = STATUS_READ_ERROR;
            dbg_printf(COLOR_YELLOW, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] READ ERROR\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
            pthread_mutex_unlock(&mutex_msg);

            return 1;
        }

        ret = memcmp(pThrInfo->bufR, pThrInfo->bufW, len * gDiskIOInfo.sz_block);

        if (ret)
        {
            pthread_mutex_lock(&mutex_msg);
            dbg_printf(COLOR_RED, "Loop[%d] Thread[%3d] LBA[%08llX] Len[%03X] COMPARE ERROR(2)\n", pThrInfo->cr_loop, pThrInfo->id, lba, len);
            pthread_mutex_unlock(&mutex_msg);

            dump_compare_error_buffer(pThrInfo, pThrInfo->bufR, pThrInfo->bufW, lba);
        }
    #endif

        return 2;
    }
#endif

    return 0;
}

int disk_read(int fd, char* buf, U64 lba, U32 len)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_read(fd, buf, lba, len)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        if (pread(fd, buf, len * gDiskIOInfo.sz_block, lba * gDiskIOInfo.sz_block) != len * gDiskIOInfo.sz_block)  return TRUE;
    #else
        printf("read not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_write(fd, buf, lba, len, write_hint)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        if (pwrite(fd, buf, len * gDiskIOInfo.sz_block, lba * gDiskIOInfo.sz_block) != len * gDiskIOInfo.sz_block)  return TRUE;
    #else
        printf("write not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_flush(int fd)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_flush(fd, gDiskIOInfo.nsid)) return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        fdatasync(fd);
    #else
        printf("flush not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_trim(int fd, U64 lba, U32 len)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        if (nvme_trim(fd, lba, len, gDiskIOInfo.nsid))    return TRUE;
    #elif DISK_IO_ENGINE == IO_ENGINE_SYSTEM
        #if 0 // something wrong
        U64 range[2];

        range[0] = (U64)(pThrInfo->block_start + pThrInfo->block_per_trunk * pThrInfo->cr_trunk);
        range[1] = (U64)pThrInfo->block_per_trunk;
        ioctl (fd, BLKDISCARD, range);
        #endif
    #else
        printf("trim not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int disk_reset(int fd)
{
    #if   DISK_IO_ENGINE == IO_ENGINE_NVME
        nvme_reset(fd);
    #else
        printf("reset controller not supported for IO engine:%d\n", DISK_IO_ENGINE);
        exit(1);
    #endif

    return FALSE;
}

int nvme_read(int fd, char* buf, U64 lba, U32 len)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode  = nvme_cmd_read;
    io.slba    = lba;
    io.addr    = (unsigned long)buf;
    io.nblocks = len - 1;

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode   = nvme_cmd_write;
    io.slba     = lba;
    io.addr     = (unsigned long)buf;
    io.nblocks  = len - 1;

    if (write_hint)
    {
        io.control |= NVME_RW_DTYPE_STREAMS;
        io.dsmgmt  |= (write_hint << 16);
    }

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_flush(int fd, int nsid)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));
    cmd.opcode = nvme_cmd_flush;
    cmd.nsid   = nsid;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_trim(int fd, U64 lba, U32 len, U32 nsid)
{
    int ret;
    U32 nr_range = 1;
    struct nvme_dsm_range dsm_range;
    struct nvme_passthru_cmd cmd;

    dsm_range.slba = lba;
    dsm_range.nlb  = len;

    memset(&cmd, 0x0, sizeof(cmd));

    cmd.opcode   = nvme_cmd_dsm;
    cmd.nsid     = nsid;
    cmd.cdw10    = nr_range - 1;
    cmd.cdw11    = NVME_DSMGMT_AD;
    cmd.data_len = nr_range * sizeof(struct nvme_dsm_range);
    cmd.addr     = (unsigned long)&dsm_range;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_reset(int fd)
{
    return ioctl(fd, NVME_IOCTL_RESET);
}

int nvme_identify(int fd, int cns, int nsid, void* pBuff)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_identify;
    cmd.nsid     = nsid;
    cmd.data_len = 4096;
    cmd.cdw10    = cns;
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_download(int fd, char* pBuff, int data_len, int offset)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_download_fw;
    cmd.data_len = data_len;
    cmd.cdw10    = (data_len >> 2) - 1,
    cmd.cdw11    = offset >> 2,
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_commit(int fd, int action, int slot)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_activate_fw;
    cmd.cdw10    = (action << 3) | slot,

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_get_log(int fd, int logId, char* pBuff, int data_len)
{
    int ret;
    struct nvme_passthru_cmd cmd;
    U32 numd  = (data_len >> 2) - 1;
    U16 numdu = numd >> 16;
    U16 numdl = numd & 0xffff;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_get_log_page;
    cmd.nsid     = 0xFFFFFFFF;
    cmd.data_len = data_len;
    cmd.cdw10    = (numdl << 16) | logId;
    cmd.cdw11    = numdu;
    cmd.addr     = (unsigned long)&pBuff[0];

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(StreamStatus_t) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)status;
    pcmd->data_len = sizeof(StreamStatus_t);
    cmd.doper      = NVME_DIR_RCV_ST_OP_STATUS;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_get_param(int fd, int nsid, struct streams_directive_params* params)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(struct streams_directive_params) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)params;
    pcmd->data_len = sizeof(struct streams_directive_params);
    cmd.doper      = NVME_DIR_RCV_ST_OP_PARAM;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_enable(int fd, int nsid, int enable)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ID_OP_ENABLE;
    cmd.dtype  = NVME_DIR_IDENTIFY;
    cmd.tdtype = NVME_DIR_STREAMS;
    cmd.endir  = enable;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_alloc_resource(int fd, int nsid, int num)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_recv;
    cmd.numd   = 0;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_RCV_ST_OP_RESOURCE;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.nsr    = num;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }
}

int nvme_stream_rel_resource(int fd, int nsid)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_RSC;
    cmd.dtype  = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_rel_id(int fd, int nsid, int id)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_ID;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.dspec  = id;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int command_parser(int argc, char** argv)
{
    char  option[256];
    const CmdTbl_t* pCmd;
    CmdFunc_t pFunc = NULL;

    toLowerCase(argv[2], option, strlen(argv[2]));

    pCmd = &cmdList[0];

    while (pCmd->pFunc != NULL)
    {
        if (strcmp(pCmd->cmdStr, option) == 0)
        {
            pFunc = pCmd->pFunc;
            break;
        }

        pCmd++;
    }

    if (pFunc)
    {
        if (strcmp(pCmd->cmdStr, "rst"))
        {
            if (get_disk_info(argv[1]) == -1)
            {
                printf("Device[%s] not found!\n", argv[1]);
                exit(1);
            }
        }

        pFunc(argv[1], argc - 3, (char**)&argv[3]);
    }

    return 0;
}

int single_read(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 32);
        U64 lba   = hex2dec(argv[0]);
        U32 len   = hex2dec(argv[1]);

        printf("=== Read LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_read(fd, buf, lba, len);
        dump_buffer(buf, len * gDiskIOInfo.sz_block);

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int single_write(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 32);
        U64 lba        = hex2dec(argv[0]);
        U32 len        = hex2dec(argv[1]);
        U32 write_hint = 0;

        printf("=== Write LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_write(fd, buf, lba, len, write_hint);
        dump_buffer(buf, len * gDiskIOInfo.sz_block);

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int single_trim(char* device, int argc, char* argv[])
{
    int fd;

    fd  = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        U64 lba = hex2dec(argv[0]);
        U32 len = hex2dec(argv[1]);

        printf("=== Trim LBA[0x%08llX] Len[0x%04X] ==========\n", lba, len);

        disk_trim(fd, lba, len);

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int sequential_read(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_read = 0;
        U32 tm;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x100;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Read Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ===================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            if ((lba + len) >= gDiskIOInfo.nr_block) break;

            disk_read(fd, buf, lba, len);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_read += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Read: %lld MB, (%.1f MB/s)\n", host_read / 2 / 1024, host_read / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int sequential_write(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_write = 0;
        U32 tm;
        U32 write_hint = 0;

        memset(buf, 0x5A, 1 * SIZE_1M);

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x100;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Write Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ========================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            if ((lba + len) >= gDiskIOInfo.nr_block) break;

            disk_write(fd, buf, lba, len, write_hint);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_write += len;
            lba        += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Write: %lld MB, (%.1f MB/s)\n", host_write / 2 / 1024, host_write / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int ramdom_read(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_read = 0;
        U32 tm;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x4;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Sequential Read Profile =============\n");
        printf("Block Start : %08llX\n", block_start);
        printf("Block End   : %08llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing =======================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            do
            {
                lba = rand() % gDiskIOInfo.nr_block;
            } while ((lba + len) >= gDiskIOInfo.nr_block);

            disk_read(fd, buf, lba, len);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_read += len;
            lba += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Read: %lld MB, (%.1f MB/s)\n", host_read / 2 / 1024, host_read / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int ramdom_write(char* device, int argc, char* argv[])
{
    int fd;

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        char* buf = calloc(1, SIZE_1M * 4);
        U64 block_start;
        U64 block_end;
        U32 block_count;
        U64 lba;
        U32 len;

        U64 idx;
        U64 cmd_count;
        U64 host_write = 0;
        U32 tm;
        U32 write_hint = 0;

        if (argc == 3)
        {
            block_start = hex2dec(argv[0]);
            block_end   = hex2dec(argv[1]);
            block_count = hex2dec(argv[2]);
        }
        else
        {
            block_start = 0;
            block_end   = gDiskIOInfo.nr_block;
            block_count = 0x4;
        }

        if (block_count) cmd_count = (block_end - block_start) / block_count;
        else             cmd_count = (block_end - block_start) / gDiskIOInfo.max_sector;

        printf("=== Random Read Profile =============\n");
        printf("Block Start : %8llX\n", block_start);
        printf("Block End   : %8llX\n", block_end);
        printf("Block Count : %X (%d Bytes)\n", block_count, block_count * gDiskIOInfo.sz_block);
        printf("Range       : %08llX (%lld MB)\n", (block_end - block_start), (block_end - block_start) / 2 / 1024);
        printf("Cmd Count   : %lld\n", cmd_count);
        printf("=== Start Testing ===================\n");

        lba = block_start;
        len = block_count;

        for (idx = 0; idx < cmd_count; idx++)
        {
            if (block_count == 0) len = (rand() % gDiskIOInfo.max_sector) + 1;

            do
            {
                lba = rand() % gDiskIOInfo.nr_block;
            } while ((lba + len) >= gDiskIOInfo.nr_block);

            disk_write(fd, buf, lba, len, write_hint);

            if ((idx & 0xFF) == 0) fprintf(stderr, "\r%lld%c", idx * 100 / cmd_count, '%');

            host_write += len;
        }

        tm = get_timeval_sec(gDiskIOInfo.timeval);

        printf("\r=== Test Completed (%2dh:%2dm:%2ds) ====\n", tm / 3600, (tm / 60) % 60, tm % 60);
        printf("Host Write: %lld MB, (%.1f MB/s)\n", host_write / 2 / 1024, host_write / 2 / 1024 / get_timeval_sec_usec(gDiskIOInfo.timeval));

        free(buf);
        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int fw_activation(char* device, int argc, char* argv[])
{
    int fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        struct timeval work_tm;
        struct nvme_firmware_log_page fw_log;
        char baseDev[11];
        int rst_fd;
        U32 loop   = atoi(argv[0]);
        U32 action = atoi(argv[1]);
        U32 slot   = atoi(argv[2]);
        FILE* fp;
        U32 binSize;
        char buffer[1 * SIZE_1M];
        U32 idx;
        U32 nr_slots = 0;

        strncpy(baseDev, device, 10);

        rst_fd = open(baseDev, O_SYNC | O_RDWR);

        if (action == 1)
        {
            if (argc == 4)
            {
                fp = fopen(argv[3], "rb");

                fseek(fp, 0, SEEK_END);
                binSize = ftell(fp);
                rewind(fp);

                fread(buffer, 1, binSize, fp);
            }
            else
            {
                printf ("L%d: parameter error\n", __LINE__);
                return 1;
            }
        }

        nvme_get_log(fd, 3, (char*)&fw_log, sizeof(fw_log));

        for (idx = 0; idx < 7; idx++)
        {
            if (fw_log.frs[idx]) nr_slots++;
            else break;
        }

        for (idx = 0; idx < loop; idx++)
        {
            if (action == 1)
            {
                U32 currSize = 0;
                U32 bIdx;

                for (bIdx = 0; bIdx < binSize / (128 * SIZE_1K); bIdx++)
                {
                    nvme_fw_download(fd, &buffer[currSize], 128 * SIZE_1K, currSize);
                    currSize += (128 * SIZE_1K);
                }

                if (currSize != binSize)
                {
                    nvme_fw_download(fd, &buffer[currSize], binSize - currSize, currSize);
                }
            }

            slot = (slot - 1) % nr_slots + 1;
            printf ("%4d) fw commit action:%d slot:%d ", idx + 1, action, slot);
            gettimeofday(&work_tm, NULL);
            nvme_fw_commit(fd, action, slot);
            nvme_reset(rst_fd);

            printf ("=> %.2f seconds\n", get_timeval_sec_usec(work_tm));
            if (loop > 1) sleep(1);
            slot++;
        }

        if (fp != NULL) fclose(fp);

        close(fd);
        close(rst_fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int reset_controller(char* device, int argc, char* argv[])
{
    int fd;
    int idx;
    int loop;

    fd = open(device, O_RDWR);

    if (fd != -1)
    {
        if (argc)   loop = atoi(argv[0]);
        else        loop = 1;

        for (idx = 0; idx < loop; idx++)
        {
            printf("%3d) controller reset:%s\n", idx + 1, device);
            disk_reset(fd);
            usleep(500000);
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

int log_page(char* device, int argc, char* argv[])
{
    int fd;
    int i;
    int logId = hex2dec(argv[0]);
    char buff[4096];

    memset(buff, 0xFF, sizeof(buff));

    fd = open(device, O_SYNC | O_RDWR);

    if (fd != -1)
    {
        nvme_get_log(fd, logId, &buff[0], sizeof(buff));

        switch (logId)
        {
            case 0x02:
            {
                LogPageSmart_t* pSmartLog = (LogPageSmart_t*)&buff[0];

                printf("TempSensor1:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor1));
                printf("TempSensor2:%d\n", KELVIN_TO_CELSIUS(pSmartLog->TempSensor2));

                break;
            }
            default:
                dump_buffer((char*)&buff[0], sizeof(buff));
                break;
        }

        close(fd);
    }
    else
    {
        printf("can not open: %s\n", device);
    }

    return 0;
}

void toLowerCase(char* src, char* dest, int len)
{
    int i;

    for (i = 0; i < len ; i++)
    {
        if (src[i] >= 'A' && src[i] <= 'Z') dest[i] = 'a' + (src[i] - 'A');
        else                                dest[i] = src[i];
    }

    dest[i] = '\0';
}

U64 hex2dec(char* buf)
{
    U32 len;
    S32 i;
    U64 base = 1;
    U64 num = 0;

    len = strlen(buf);

    // to lower case
    for (i = 0; i < len; i++)
    {
        if (buf[i] >= 'A' && buf[i] <= 'Z')
        {
            buf[i] = 'a' + buf[i] - 'A';
        }
    }

    for (i = len - 1; i >= 0; i--)
    {
        if (buf[i] >= '0' && buf[i] <= '9')
        {
            num += (buf[i] - '0') * base;
        }
        else
        {
            num += (buf[i] - 'a' + 10) * base;
        }

        base *= 16;
    }

    return num;
}

void dump_buffer(char* buf, U32 len)
{
    U32 i, j;

    for (i = 0; i < len / 16; i++)
    {
        for (j = 0; j < 16; j++)
        {
            printf(" %02X", buf[i * 16 + j] & 0xFF);
        }

        printf (" | ");

        for (j = 0; j < 16; j++)
        {
            if (buf[i * 16 + j] > 0x1F && buf[i * 16 + j] < 0x7F)
            {
                printf("%c", buf[i * 16 + j] & 0xFF);
            }
            else
            {
                printf(".");
            }
        }

        printf("\n");
    }

    printf("===========================================================\n");
}

void show_usage(int argc, char* argv[])
{
    const CmdTbl_t* pCmd;

    printf("Usage: %s [device_path] (example: %s /dev/sdb)\n", argv[0], argv[0]);
    printf("Usage: %s [device_path] [options]\n", argv[0]);

    pCmd = &cmdList[0];

    printf("-------------------------------------------------------------------------------\n");
    printf("  %-10s%-20s%-15s\n", "[Option]", "[Description]", "[Parameters]");

    while (pCmd->pFunc != NULL)
    {
        printf("  %-10s%-20s%-45s\n", pCmd->cmdStr, pCmd->helpStr, pCmd->fmtStr);
        pCmd++;
    }

    printf("-------------------------------------------------------------------------------\n");
}
