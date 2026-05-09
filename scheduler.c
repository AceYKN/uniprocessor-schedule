/*
 * 实验三：单处理机进程调度
 * 调度策略：时间片轮转（RR） + 多级反馈队列（扩展）
 *
 * PCB 状态：FREE(空闲) / READY(就绪) / RUNNING(运行) / BLOCKED(阻塞) / FINISHED(完成)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================== 常量定义 ======================== */
#define MAX_PROC 64     /* 最大进程数 */
#define MAX_NAME 32     /* 进程名最大长度 */
#define DEFAULT_SLICE 2 /* 默认时间片大小（时间单位） */

/* 多级反馈队列层数 */
#define MFQ_LEVELS 3

/* ======================== 枚举：进程状态 ======================== */
typedef enum
{
    STATE_FREE = 0,
    STATE_READY = 1,
    STATE_RUNNING = 2,
    STATE_BLOCKED = 3,
    STATE_FINISHED = 4
} ProcState;

static const char *STATE_NAME[] = {
    "FREE", "READY", "RUNNING", "BLOCKED", "FINISHED"};

/* ======================== PCB 结构 ======================== */
typedef struct PCB
{
    /* 标识信息 */
    int pid;             /* 进程 ID */
    char name[MAX_NAME]; /* 进程名 */

    /* 状态与运行时间 */
    ProcState state;    /* 当前状态 */
    int total_time;     /* 需要的总运行时间 */
    int remaining_time; /* 剩余运行时间 */
    int slice_used;     /* 当前时间片已用时间 */
    int dispatch_count; /* 被调度次数（第 j 个时间片） */

    /* I/O 信息（场景一自动模拟用） */
    int io_at;        /* 在哪个已运行时刻触发 I/O（-1 表示无） */
    int io_duration;  /* I/O 持续时间 */
    int io_remaining; /* I/O 剩余时间 */
    int elapsed_time; /* 已运行 CPU 时间（触发 I/O 判断用） */

    /* 现场信息（模拟寄存器） */
    int reg_pc;  /* 模拟程序计数器 */
    int reg_acc; /* 模拟累加器 */

    /* 管理信息 */
    int priority;  /* 优先级（多级反馈队列层次，0最高） */
    int mfq_level; /* 当前所在 MFQ 层（0-based） */

    struct PCB *next; /* 队列链接指针 */
} PCB;

/* ======================== 队列结构 ======================== */
typedef struct
{
    PCB *head;
    PCB *tail;
    int count;
} Queue;

/* ======================== 全局数据 ======================== */
static PCB pcb_pool[MAX_PROC]; /* PCB 内存池 */
static Queue free_queue;       /* 空闲队列 */
static Queue ready_queue;      /* 就绪队列（普通 RR 模式） */
static Queue run_queue;        /* 运行队列（最多1个进程） */
static Queue blocked_queue;    /* 阻塞队列 */
static Queue finished_queue;   /* 完成队列 */

/* 多级反馈队列 */
static Queue mfq[MFQ_LEVELS];

static int next_pid = 1;
static int sys_clock = 0; /* 系统时钟 */
static int time_slice = DEFAULT_SLICE;
static int use_mfq = 0; /* 0=RR, 1=MFQ */

/* MFQ 每级时间片大小：第 i 级 = DEFAULT_SLICE * 2^i */
static int mfq_slice[MFQ_LEVELS];

/* ======================== 队列操作 ======================== */
static void queue_init(Queue *q)
{
    q->head = q->tail = NULL;
    q->count = 0;
}

static void enqueue(Queue *q, PCB *p)
{
    p->next = NULL;
    if (q->tail)
        q->tail->next = p;
    else
        q->head = p;
    q->tail = p;
    q->count++;
}

static PCB *dequeue(Queue *q)
{
    if (!q->head)
        return NULL;
    PCB *p = q->head;
    q->head = p->next;
    if (!q->head)
        q->tail = NULL;
    p->next = NULL;
    q->count--;
    return p;
}

/* 从队列中按 pid 摘取 */
static PCB *dequeue_by_pid(Queue *q, int pid)
{
    PCB *prev = NULL, *cur = q->head;
    while (cur)
    {
        if (cur->pid == pid)
        {
            if (prev)
                prev->next = cur->next;
            else
                q->head = cur->next;
            if (cur == q->tail)
                q->tail = prev;
            cur->next = NULL;
            q->count--;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    return NULL;
}

/* 打印队列中所有进程标识 */
static void print_queue(const char *label, Queue *q)
{
    printf("  [%s] (%d): ", label, q->count);
    PCB *p = q->head;
    while (p)
    {
        printf("%s(P%d) ", p->name, p->pid);
        p = p->next;
    }
    printf("\n");
}

/* ======================== PCB 打印 ======================== */
static void print_pcb(const PCB *p)
{
    printf("  ┌─── PCB: %s (PID=%d) ───\n", p->name, p->pid);
    printf("  │ 状态      : %s\n", STATE_NAME[p->state]);
    printf("  │ 总运行时  : %d\n", p->total_time);
    printf("  │ 剩余时间  : %d\n", p->remaining_time);
    printf("  │ 已调度次  : %d\n", p->dispatch_count);
    printf("  │ reg_PC    : %d\n", p->reg_pc);
    printf("  │ reg_ACC   : %d\n", p->reg_acc);
    if (use_mfq)
        printf("  │ MFQ层次   : %d\n", p->mfq_level);
    printf("  └────────────────────────\n");
}

/* ======================== 打印系统快照 ======================== */
static void print_snapshot(void)
{
    printf("\n  ─── 系统时钟=%d  队列快照 ───\n", sys_clock);
    if (!use_mfq)
    {
        print_queue("就绪", &ready_queue);
    }
    else
    {
        for (int i = 0; i < MFQ_LEVELS; i++)
        {
            char label[32];
            snprintf(label, sizeof(label), "就绪MFQ[%d]", i);
            print_queue(label, &mfq[i]);
        }
    }
    print_queue("运行", &run_queue);
    print_queue("阻塞", &blocked_queue);
    print_queue("完成", &finished_queue);
    printf("  ────────────────────────────\n\n");
}

/* ======================== 初始化 ======================== */
static void init_system(void)
{
    queue_init(&free_queue);
    queue_init(&ready_queue);
    queue_init(&run_queue);
    queue_init(&blocked_queue);
    queue_init(&finished_queue);
    for (int i = 0; i < MFQ_LEVELS; i++)
    {
        queue_init(&mfq[i]);
        mfq_slice[i] = DEFAULT_SLICE * (1 << i);
    }
    /* 所有 PCB 入空闲队列 */
    for (int i = 0; i < MAX_PROC; i++)
    {
        memset(&pcb_pool[i], 0, sizeof(PCB));
        pcb_pool[i].state = STATE_FREE;
        enqueue(&free_queue, &pcb_pool[i]);
    }
}

/* ======================== 原语：进程创建 ======================== */
/*
 * create_process - 创建新进程
 *   name        : 进程名
 *   total_time  : 需要的总 CPU 时间
 *   io_at       : 在第几个 CPU 时间单位触发 I/O（-1=不触发）
 *   io_duration : I/O 持续时间
 * 返回新进程的 PCB 指针，失败返回 NULL
 */
PCB *create_process(const char *name, int total_time, int io_at, int io_duration)
{
    /* 1. 从空闲队列申请 PCB */
    PCB *p = dequeue(&free_queue);
    if (!p)
    {
        fprintf(stderr, "[ERROR] 无可用 PCB，进程创建失败\n");
        return NULL;
    }
    /* 2. 填写 PCB */
    p->pid = next_pid++;
    strncpy(p->name, name, MAX_NAME - 1);
    p->name[MAX_NAME - 1] = '\0';
    p->state = STATE_READY;
    p->total_time = total_time;
    p->remaining_time = total_time;
    p->slice_used = 0;
    p->dispatch_count = 0;
    p->io_at = io_at;
    p->io_duration = io_duration;
    p->io_remaining = 0;
    p->elapsed_time = 0;
    p->reg_pc = 0;
    p->reg_acc = 0;
    p->mfq_level = 0; /* 新进程从最高优先级队列进入 */
    p->next = NULL;

    /* 3. 挂就绪队列 */
    if (!use_mfq)
        enqueue(&ready_queue, p);
    else
        enqueue(&mfq[0], p);

    printf("[创建] 进程 %s (PID=%d) 已创建，总时间=%d，I/O触发时刻=%d，I/O时长=%d\n",
           p->name, p->pid, p->total_time, p->io_at, p->io_duration);
    return p;
}

/* ======================== 原语：进程调度（选择下一个运行进程） ======================== */
static PCB *schedule(void)
{
    PCB *p = NULL;
    if (!use_mfq)
    {
        p = dequeue(&ready_queue);
    }
    else
    {
        for (int i = 0; i < MFQ_LEVELS && !p; i++)
            p = dequeue(&mfq[i]);
    }
    return p;
}

/* 获取当前时间片大小 */
static int get_slice(PCB *p)
{
    if (!use_mfq)
        return time_slice;
    return mfq_slice[p->mfq_level];
}

/* ======================== 原语：进程阻塞 ======================== */
static void block_process(PCB *p)
{
    if (!p || p->state != STATE_RUNNING)
        return;
    /* 保存现场（模拟） */
    p->reg_pc += p->slice_used;
    p->reg_acc += p->slice_used * 2;

    /* 从运行队列摘除 */
    dequeue_by_pid(&run_queue, p->pid);

    p->state = STATE_BLOCKED;
    p->io_remaining = p->io_duration;
    enqueue(&blocked_queue, p);

    printf("[阻塞] 进程 %s (PID=%d) 进入阻塞状态\n", p->name, p->pid);
    print_snapshot();
}

/* ======================== 原语：进程唤醒 ======================== */
static void wakeup_process(PCB *p)
{
    if (!p || p->state != STATE_BLOCKED)
        return;
    dequeue_by_pid(&blocked_queue, p->pid);

    p->state = STATE_READY;
    p->io_remaining = 0;

    if (!use_mfq)
        enqueue(&ready_queue, p);
    else
        enqueue(&mfq[p->mfq_level], p);

    printf("[唤醒] 进程 %s (PID=%d) 回到就绪状态\n", p->name, p->pid);
    print_snapshot();
}

/* ======================== 进程完成处理 ======================== */
static void finish_process(PCB *p)
{
    dequeue_by_pid(&run_queue, p->pid);
    p->state = STATE_FINISHED;
    p->remaining_time = 0;
    enqueue(&finished_queue, p);
    printf("[完成] 进程 %s (PID=%d) 运行结束\n", p->name, p->pid);
    print_snapshot();
}

/* ======================== 时间片用完（抢占） ======================== */
static void preempt_process(PCB *p)
{
    dequeue_by_pid(&run_queue, p->pid);

    /* 保存现场 */
    p->reg_pc += p->slice_used;
    p->reg_acc += p->slice_used * 2;
    p->slice_used = 0;

    p->state = STATE_READY;

    if (!use_mfq)
    {
        enqueue(&ready_queue, p);
    }
    else
    {
        /* 降低优先级（降到下一层，但不超过最低层） */
        if (p->mfq_level < MFQ_LEVELS - 1)
            p->mfq_level++;
        enqueue(&mfq[p->mfq_level], p);
    }
    printf("[抢占] 进程 %s (PID=%d) 时间片用完，回就绪队列(MFQ层=%d)\n",
           p->name, p->pid, p->mfq_level);
    print_snapshot();
}

/* ======================== 调度上 CPU ======================== */
static void dispatch(PCB *p)
{
    p->state = STATE_RUNNING;
    p->dispatch_count++;
    p->slice_used = 0;
    enqueue(&run_queue, p);

    /* 恢复现场（模拟） */
    printf("\n[调度] This is Process '%s', I am running in time-slice '%d'\n",
           p->name, p->dispatch_count);
    print_pcb(p);
}

/* ======================== 场景一：自动仿真 ======================== */
static void auto_simulate(void)
{
    printf("\n========== 自动仿真模式（时间片轮转%s）==========\n",
           use_mfq ? " + 多级反馈队列" : "");

    int total_proc = ready_queue.count;
    if (use_mfq)
    {
        for (int i = 0; i < MFQ_LEVELS; i++)
            total_proc += mfq[i].count;
    }
    total_proc += run_queue.count;

    PCB *running = NULL;

    while (1)
    {
        /* 检查是否全部完成 */
        int alive = 0;
        if (!use_mfq)
            alive += ready_queue.count;
        else
            for (int i = 0; i < MFQ_LEVELS; i++)
                alive += mfq[i].count;
        alive += run_queue.count + blocked_queue.count;
        if (alive == 0)
            break;

        /* 如果没有运行中进程，调度一个 */
        if (!running)
        {
            running = schedule();
            if (running)
            {
                dispatch(running);
            }
            else
            {
                /* 无就绪进程，时钟推进，处理阻塞 */
                sys_clock++;
                PCB *bp = blocked_queue.head;
                while (bp)
                {
                    PCB *next_bp = bp->next;
                    if (bp->io_remaining > 0)
                    {
                        bp->io_remaining--;
                        if (bp->io_remaining == 0)
                            wakeup_process(bp);
                    }
                    bp = next_bp;
                }
                continue;
            }
        }

        /* 运行一个时间单位 */
        sys_clock++;
        running->slice_used++;
        running->remaining_time--;
        running->elapsed_time++;
        running->reg_pc++;

        printf("  时钟=%d : 进程 %s 运行 (剩余=%d, 片内=%d/%d)\n",
               sys_clock, running->name,
               running->remaining_time,
               running->slice_used,
               get_slice(running));

        /* 推进阻塞队列 I/O 计时 */
        PCB *bp = blocked_queue.head;
        while (bp)
        {
            PCB *next_bp = bp->next;
            if (bp->io_remaining > 0)
            {
                bp->io_remaining--;
                if (bp->io_remaining == 0)
                    wakeup_process(bp);
            }
            bp = next_bp;
        }

        /* 判断是否触发 I/O */
        if (running->io_at >= 0 && running->elapsed_time == running->io_at)
        {
            printf("  => 进程 %s 触发 I/O\n", running->name);
            PCB *prev_running = running;
            running = NULL;
            block_process(prev_running);
            /* 打印上 CPU 前的进程 PCB（退出信息） */
            printf("[退出CPU] 进程 %s PCB:\n", prev_running->name);
            print_pcb(prev_running);
            continue;
        }

        /* 判断是否运行完成 */
        if (running->remaining_time <= 0)
        {
            PCB *prev_running = running;
            running = NULL;
            printf("[退出CPU] 进程 %s PCB:\n", prev_running->name);
            print_pcb(prev_running);
            finish_process(prev_running);
            continue;
        }

        /* 判断时间片是否用完 */
        if (running->slice_used >= get_slice(running))
        {
            PCB *prev_running = running;
            running = NULL;
            printf("[退出CPU] 进程 %s PCB:\n", prev_running->name);
            print_pcb(prev_running);
            preempt_process(prev_running);
            /* 调度下一个 */
            running = schedule();
            if (running)
                dispatch(running);
        }
    }

    printf("\n========== 仿真结束，系统时钟=%d ==========\n", sys_clock);
    printf("已完成进程：\n");
    PCB *p = finished_queue.head;
    while (p)
    {
        print_pcb(p);
        p = p->next;
    }
}

/* ======================== 场景二：人工干预模式 ======================== */
static void manual_simulate(void)
{
    printf("\n========== 人工干预模式（时间片轮转%s）==========\n",
           use_mfq ? " + 多级反馈队列" : "");
    printf("命令说明：\n");
    printf("  enter    - 调度下一个进程上 CPU（当前进程回就绪队列）\n");
    printf("  esc      - 当前进程阻塞\n");
    printf("  wakeup   - 唤醒一个阻塞进程\n");
    printf("  finished - 当前进程运行完成\n");
    printf("  tick     - 时钟推进一格（消耗一个时间单位）\n");
    printf("  show     - 显示当前队列快照\n");
    printf("  quit     - 退出仿真\n\n");

    PCB *running = NULL;
    char cmd[64];

    /* 先调度一个进程 */
    running = schedule();
    if (running)
        dispatch(running);
    print_snapshot();

    while (1)
    {
        printf(">> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin))
            break;
        /* 去掉换行 */
        cmd[strcspn(cmd, "\r\n")] = '\0';

        if (strcmp(cmd, "quit") == 0)
        {
            printf("退出人工干预模式。\n");
            break;
        }
        else if (strcmp(cmd, "enter") == 0)
        {
            /* 当前进程回就绪，调度下一个 */
            if (running)
            {
                printf("[退出CPU] 进程 %s PCB:\n", running->name);
                print_pcb(running);
                dequeue_by_pid(&run_queue, running->pid);
                running->state = STATE_READY;
                running->slice_used = 0;
                /* 保存现场 */
                running->reg_pc += 1;
                running->reg_acc += 2;
                if (!use_mfq)
                    enqueue(&ready_queue, running);
                else
                    enqueue(&mfq[running->mfq_level], running);
                running = NULL;
            }
            running = schedule();
            if (running)
                dispatch(running);
            else
                printf("[调度] 就绪队列为空，无进程可调度\n");
            print_snapshot();
        }
        else if (strcmp(cmd, "esc") == 0)
        {
            if (!running)
            {
                printf("[提示] 当前无运行进程\n");
                continue;
            }
            PCB *tmp = running;
            running = NULL;
            block_process(tmp);
            /* 自动调度下一个 */
            running = schedule();
            if (running)
                dispatch(running);
            else
                printf("[调度] 就绪队列为空，无进程可调度\n");
        }
        else if (strcmp(cmd, "wakeup") == 0)
        {
            if (blocked_queue.count == 0)
            {
                printf("[提示] 阻塞队列为空\n");
                continue;
            }
            printf("阻塞队列中的进程：\n");
            PCB *bp = blocked_queue.head;
            int idx = 1;
            while (bp)
            {
                printf("  %d. %s (PID=%d)\n", idx++, bp->name, bp->pid);
                bp = bp->next;
            }
            printf("输入 PID 以唤醒：");
            fflush(stdout);
            int wpid;
            if (scanf("%d", &wpid) == 1)
            {
                while (getchar() != '\n')
                    ;
                PCB *wp = NULL;
                bp = blocked_queue.head;
                while (bp)
                {
                    if (bp->pid == wpid)
                    {
                        wp = bp;
                        break;
                    }
                    bp = bp->next;
                }
                if (wp)
                    wakeup_process(wp);
                else
                    printf("[错误] 未找到 PID=%d 的阻塞进程\n", wpid);
            }
        }
        else if (strcmp(cmd, "finished") == 0)
        {
            if (!running)
            {
                printf("[提示] 当前无运行进程\n");
                continue;
            }
            printf("[退出CPU] 进程 %s PCB:\n", running->name);
            print_pcb(running);
            PCB *tmp = running;
            running = NULL;
            finish_process(tmp);
            running = schedule();
            if (running)
                dispatch(running);
            else
                printf("[调度] 就绪队列为空，无进程可调度\n");
        }
        else if (strcmp(cmd, "tick") == 0)
        {
            sys_clock++;
            if (running)
            {
                running->slice_used++;
                running->remaining_time--;
                running->elapsed_time++;
                printf("  时钟=%d : 进程 %s 运行 (剩余=%d, 片内=%d/%d)\n",
                       sys_clock, running->name,
                       running->remaining_time,
                       running->slice_used,
                       get_slice(running));
                /* 检查时间片用完 */
                if (running->slice_used >= get_slice(running))
                {
                    printf("  => 时间片用完，触发抢占\n");
                    printf("[退出CPU] 进程 %s PCB:\n", running->name);
                    print_pcb(running);
                    PCB *tmp = running;
                    running = NULL;
                    preempt_process(tmp);
                    running = schedule();
                    if (running)
                        dispatch(running);
                    else
                        printf("[调度] 就绪队列为空，无进程可调度\n");
                }
                /* 检查运行完成 */
                if (running && running->remaining_time <= 0)
                {
                    printf("[退出CPU] 进程 %s PCB:\n", running->name);
                    print_pcb(running);
                    PCB *tmp = running;
                    running = NULL;
                    finish_process(tmp);
                    running = schedule();
                    if (running)
                        dispatch(running);
                    else
                        printf("[调度] 就绪队列为空，无进程可调度\n");
                }
            }
            else
            {
                printf("  时钟=%d : CPU 空闲\n", sys_clock);
            }
            /* 推进阻塞队列 */
            PCB *bp = blocked_queue.head;
            while (bp)
            {
                PCB *next_bp = bp->next;
                if (bp->io_remaining > 0)
                {
                    bp->io_remaining--;
                    if (bp->io_remaining == 0)
                        wakeup_process(bp);
                }
                bp = next_bp;
            }
        }
        else if (strcmp(cmd, "show") == 0)
        {
            print_snapshot();
        }
        else if (strlen(cmd) > 0)
        {
            printf("[提示] 未知命令：%s\n", cmd);
        }

        /* 检查所有进程是否完成 */
        int alive = 0;
        if (!use_mfq)
            alive += ready_queue.count;
        else
            for (int i = 0; i < MFQ_LEVELS; i++)
                alive += mfq[i].count;
        alive += run_queue.count + blocked_queue.count;
        if (alive == 0 && !running)
        {
            printf("\n所有进程已完成，仿真结束。\n");
            print_snapshot();
            break;
        }
    }
}

/* ======================== 输入辅助 ======================== */
static int read_int(const char *prompt, int min_val, int max_val)
{
    int val;
    while (1)
    {
        printf("%s [%d~%d]: ", prompt, min_val, max_val);
        fflush(stdout);
        if (scanf("%d", &val) == 1 && val >= min_val && val <= max_val)
        {
            while (getchar() != '\n')
                ;
            return val;
        }
        /* 清空输入缓冲 */
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        printf("  输入无效，请重新输入。\n");
    }
}

/* ======================== 主菜单 ======================== */
static void setup_processes(void)
{
    int n = read_int("请输入要创建的进程数量", 1, MAX_PROC - 1);
    for (int i = 0; i < n; i++)
    {
        char pname[MAX_NAME];
        printf("进程 %d:\n", i + 1);
        printf("  进程名称: ");
        fflush(stdout);
        if (scanf("%31s", pname) != 1)
        {
            i--;
            continue;
        }
        while (getchar() != '\n')
            ;

        int total = read_int("  总运行时间", 1, 100);
        int io_at = -1, io_dur = 0;
        int has_io = read_int("  是否有 I/O 操作？(1=有, 0=无)", 0, 1);
        if (has_io)
        {
            io_at = read_int("  I/O 触发时刻（已运行 CPU 时间）", 1, total);
            io_dur = read_int("  I/O 持续时间", 1, 20);
        }
        create_process(pname, total, io_at, io_dur);
    }
}

int main(void)
{
    init_system();

    printf("╔══════════════════════════════════════╗\n");
    printf("║     单处理机进程调度模拟系统         ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* 选择调度策略 */
    printf("调度策略：\n");
    printf("  1. 时间片轮转（RR）\n");
    printf("  2. 多级反馈队列（MFQ，扩展功能）\n");
    use_mfq = read_int("请选择", 1, 2) - 1;

    if (!use_mfq)
    {
        time_slice = read_int("请输入时间片大小", 1, 20);
    }
    else
    {
        printf("MFQ 共 %d 层，各层时间片：", MFQ_LEVELS);
        for (int i = 0; i < MFQ_LEVELS; i++)
            printf(" [%d]=%d", i, mfq_slice[i]);
        printf("\n");
    }

    /* 创建进程 */
    setup_processes();
    print_snapshot();

    /* 选择运行模式 */
    printf("\n运行模式：\n");
    printf("  1. 自动仿真（系统自动控制时钟推进）\n");
    printf("  2. 人工干预（手动输入命令控制状态切换）\n");
    int mode = read_int("请选择", 1, 2);

    if (mode == 1)
        auto_simulate();
    else
        manual_simulate();

    return 0;
}
