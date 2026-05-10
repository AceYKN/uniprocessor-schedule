#include <iostream>
#include <iomanip>
#include <string>
#include <list>
#include <vector>
#include <array>
#include <algorithm>
#include <limits>

constexpr int MAX_PROC = 64;     // 最大进程数
constexpr int DEFAULT_SLICE = 2; // 默认时间片大小
constexpr int MFQ_LEVELS = 3;    // 多级反馈队列层数

// 进程状态用字符串表示：FREE / READY / RUNNING / BLOCKED / FINISHED

struct PCB
{
    int pid = 0;
    std::string name; // 进程名称

    /* 状态与运行时间 */
    std::string state = "FREE";
    int total_time = 0;     /* 总 CPU 时间 */
    int remaining_time = 0; /* 剩余 CPU 时间 */
    int slice_used = 0;     /* 当前时间片已用 */
    int dispatch_count = 0; /* 被调度次数（第 j 个时间片） */

    /* I/O 信息 */
    int io_at = -1; /* 在第几个 CPU 时间触发 I/O（-1=无） */
    int io_duration = 0;
    int io_remaining = 0;
    int elapsed_time = 0; /* 已消耗 CPU 时间（I/O 判断用） */

    /* 现场（模拟寄存器） */
    int reg_pc = 0;
    int reg_acc = 0;

    /* 多级反馈队列层次 */
    int mfq_level = 0;
};
using ProcQueue = std::list<PCB *>;
static std::vector<PCB> pcb_pool(MAX_PROC); /* PCB 内存池 */

static ProcQueue free_queue;
static ProcQueue ready_queue;
static ProcQueue run_queue; /* 最多 1 个进程 */
static ProcQueue blocked_queue;
static ProcQueue finished_queue;

static std::array<ProcQueue, MFQ_LEVELS> mfq; /* 多级反馈ready队列 */
static std::array<int, MFQ_LEVELS> mfq_slice{};

static int next_pid = 1;
static int sys_clock = 0;
static int time_slice = DEFAULT_SLICE;
static bool use_mfq = false;

/*入队*/
static void enqueue(ProcQueue &q, PCB *p) { q.push_back(p); }

/* 返回出队的进程指针并出队 */
static PCB *dequeue(ProcQueue &q)
{
    if (q.empty())
        return nullptr;
    PCB *p = q.front();
    q.pop_front();
    return p;
}

static void print_queue(const std::string &label, const ProcQueue &q)
{
    std::cout << "  [" << label << "] (" << q.size() << "): ";
    for (const auto *p : q)
        std::cout << p->name << "(P" << p->pid << ") ";
    std::cout << "\n";
}

/*打印pcb */
static void print_pcb(const PCB *p)
{
    std::cout << "  PCB " << p->name << "(P" << p->pid << ")"
              << " 状态=" << p->state
              << " 总时=" << p->total_time
              << " 剩余=" << p->remaining_time
              << " 调度次=" << p->dispatch_count
              << " PC=" << p->reg_pc << " ACC=" << p->reg_acc;
    if (use_mfq) std::cout << " MFQ=" << p->mfq_level;
    std::cout << "\n";
}

/* 打印队列快照 */
static void print_snapshot()
{
    std::cout << "\n[快照] 时钟=" << sys_clock << "\n";
    if (!use_mfq)
        print_queue("就绪", ready_queue);
    else
        for (int i = 0; i < MFQ_LEVELS; i++)
            print_queue("就绪MFQ[" + std::to_string(i) + "]", mfq[i]);
    print_queue("运行", run_queue);
    print_queue("阻塞", blocked_queue);
    print_queue("完成", finished_queue);
    std::cout << "\n";
}

/* 初始化*/
static void init_system()
{
    for (int i = 0; i < MAX_PROC; i++)
    {
        pcb_pool[i] = PCB{};
        enqueue(free_queue, &pcb_pool[i]);
    }
    for (int i = 0; i < MFQ_LEVELS; i++)
        mfq_slice[i] = DEFAULT_SLICE * (1 << i); // MFQ每层时间片为前一层的两倍
}

/* 获取当前时间片大小 */
static int get_slice(const PCB *p)
{
    return use_mfq ? mfq_slice[p->mfq_level] : time_slice;
}

/*原语：进程创建 */
static PCB *create_process(const std::string &name, int total_time, int io_at, int io_duration)
{
    PCB *p = dequeue(free_queue); // 从空闲队列获取一个 PCB
    if (!p)
    {
        std::cerr << "[ERROR] 无可用 PCB，进程创建失败\n";
        return nullptr;
    }
    p->pid = next_pid++;
    p->name = name;
    p->state = "READY";
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
    p->mfq_level = 0;

    if (!use_mfq)
        enqueue(ready_queue, p);
    else
        enqueue(mfq[0], p);

    std::cout << "[创建] 进程 " << p->name << " (PID=" << p->pid
              << ") 已创建，总时间=" << p->total_time
              << "，I/O触发时刻=" << p->io_at
              << "，I/O时长=" << p->io_duration << "\n";
    return p;
}

/* ======================== 原语：进程调度（选进程） ======================== */
static PCB *schedule()
{
    if (!use_mfq)
        return dequeue(ready_queue);
    for (int i = 0; i < MFQ_LEVELS; i++)
    {
        PCB *p = dequeue(mfq[i]);
        if (p)
            return p;
    }
    return nullptr;
}

/* ======================== 原语：进程阻塞 ======================== */
static void block_process(PCB *p)
{
    if (!p || p->state != "RUNNING")
        return;
    p->reg_pc += p->slice_used; // 保存现场
    p->reg_acc += p->slice_used * 2;
    run_queue.pop_front();
    p->state = "BLOCKED";
    p->io_remaining = p->io_duration;
    enqueue(blocked_queue, p);
    std::cout << "[阻塞] 进程 " << p->name << " (PID=" << p->pid << ") 进入阻塞状态\n";
    print_snapshot();
}

/* ======================== 原语：进程唤醒 ======================== */
static void wakeup_process(PCB *p)
{
    if (!p || p->state != "BLOCKED")
        return;
    blocked_queue.remove(p);
    p->state = "READY";
    p->io_remaining = 0;
    if (!use_mfq)
        enqueue(ready_queue, p);
    else
        enqueue(mfq[p->mfq_level], p);
    std::cout << "[唤醒] 进程 " << p->name << " (PID=" << p->pid << ") 回到就绪状态\n";
    print_snapshot();
}

/* ======================== 进程完成 ======================== */
static void finish_process(PCB *p)
{
    run_queue.pop_front();
    p->state = "FINISHED";
    p->remaining_time = 0;
    enqueue(finished_queue, p);
    std::cout << "[完成] 进程 " << p->name << " (PID=" << p->pid << ") 运行结束\n";
    print_snapshot();
}

/* ======================== 时间片用完（抢占） ======================== */
static void preempt_process(PCB *p)
{
    run_queue.pop_front();
    p->reg_pc += p->slice_used;
    p->reg_acc += p->slice_used * 2;
    p->slice_used = 0;
    p->state = "READY";
    if (!use_mfq)
    {
        enqueue(ready_queue, p);
    }
    else
    {
        if (p->mfq_level < MFQ_LEVELS - 1)
            p->mfq_level++;
        enqueue(mfq[p->mfq_level], p);
    }
    std::cout << "[抢占] 进程 " << p->name << " (PID=" << p->pid
              << ") 时间片用完，回就绪队列(MFQ层=" << p->mfq_level << ")\n";
    print_snapshot();
}

/* 调度上 CPU */
static void dispatch(PCB *p)
{
    p->state = "RUNNING";
    p->dispatch_count++;
    p->slice_used = 0;
    enqueue(run_queue, p);
    std::cout << "\n[调度] This is Process '" << p->name
              << "', I am running in time-slice '" << p->dispatch_count << "'\n";
    print_pcb(p);
}

/* ======================== 统计存活进程数 ======================== */
static int alive_count()
{
    int n = run_queue.size() + blocked_queue.size();
    if (!use_mfq)
    {
        n += ready_queue.size();
    }
    else
    {
        for (int i = 0; i < MFQ_LEVELS; i++)
            n += mfq[i].size();
    }
    return n;
}

/* ======================== 推进阻塞队列 I/O ======================== */
static void advance_io()
{
    std::vector<PCB *> to_wake;
    for (auto *bp : blocked_queue)
    {
        if (bp->io_remaining > 0)
        {
            bp->io_remaining--;
            if (bp->io_remaining == 0)
                to_wake.push_back(bp);
        }
    }
    for (auto *bp : to_wake)
        wakeup_process(bp);
}

/* ======================== 场景一：自动仿真 ======================== */
static void auto_simulate()
{
    std::cout << "\n自动仿真（" << (use_mfq ? "MFQ" : "RR") << "）\n";

    PCB *running = nullptr;

    while (true)
    {
        if (alive_count() == 0 and !running)
            break;

        if (!running)
        {
            running = schedule();
            if (running)
            {
                dispatch(running);
            }
            else
            {
                sys_clock++;
                advance_io();
                continue;
            }
        }

        /* 运行一个时间单位 */
        sys_clock++;
        running->slice_used++;
        running->remaining_time--;
        running->elapsed_time++;

        std::cout << "  时钟=" << sys_clock << " : 进程 " << running->name
                  << " 运行 (剩余=" << running->remaining_time
                  << ", 片内=" << running->slice_used << "/" << get_slice(running) << ")\n";

        advance_io();

        /* 判断 I/O 触发 */
        if (running->io_at >= 0 && running->elapsed_time == running->io_at)
        {
            std::cout << "  => 进程 " << running->name << " 触发 I/O\n";
            std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
            print_pcb(running);
            PCB *tmp = running;
            running = nullptr;
            block_process(tmp);
            continue;
        }

        /* 判断运行完成 */
        if (running->remaining_time <= 0)
        {
            std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
            print_pcb(running);
            PCB *tmp = running;
            running = nullptr;
            finish_process(tmp);
            continue;
        }

        /* 判断时间片用完 */
        if (running->slice_used >= get_slice(running))
        {
            std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
            print_pcb(running);
            PCB *tmp = running;
            running = nullptr;
            preempt_process(tmp);
            running = schedule();
            if (running)
                dispatch(running);
        }
    }

    std::cout << "\n仿真结束，时钟=" << sys_clock << "\n已完成进程：\n";
    for (const auto *p : finished_queue)
        print_pcb(p);
}

/* ======================== 场景二：人工干预 ======================== */
static void manual_simulate()
{
    std::cout << "\n人工干预（" << (use_mfq ? "MFQ" : "RR") << "）\n"
              << "命令：enter | esc | wakeup | finished | tick | show | quit\n\n";

    PCB *running = schedule();
    if (running)
        dispatch(running);
    print_snapshot();

    std::string cmd;
    while (true)
    {
        std::cout << ">> ";
        if (!(std::cin >> cmd))
            break;

        if (cmd == "quit")
        {
            std::cout << "退出人工干预模式。\n";
            break;
        }
        else if (cmd == "enter")
        {
            if (running)
            {
                std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
                print_pcb(running);
                run_queue.pop_front();
                running->state = "READY";
                running->slice_used = 0;
                running->reg_pc += 1;
                running->reg_acc += 2;
                if (!use_mfq)
                    enqueue(ready_queue, running);
                else
                    enqueue(mfq[running->mfq_level], running);
                running = nullptr;
            }
            running = schedule();
            if (running)
                dispatch(running);
            else
                std::cout << "[调度] 就绪队列为空\n";
            print_snapshot();
        }
        else if (cmd == "esc")
        {
            if (!running)
            {
                std::cout << "[提示] 当前无运行进程\n";
                continue;
            }
            PCB *tmp = running;
            running = nullptr;
            block_process(tmp);
            running = schedule();
            if (running)
                dispatch(running);
            else
                std::cout << "[调度] 就绪队列为空\n";
        }
        else if (cmd == "wakeup")
        {
            if (blocked_queue.empty())
            {
                std::cout << "[提示] 阻塞队列为空\n";
                continue;
            }
            std::cout << "阻塞进程列表：\n";
            int idx = 1;
            for (const auto *bp : blocked_queue)
                std::cout << "  " << idx++ << ". " << bp->name << " (PID=" << bp->pid << ")\n";
            std::cout << "输入 PID 以唤醒：";
            int wpid;
            if (std::cin >> wpid)
            {
                auto it = std::find_if(blocked_queue.begin(), blocked_queue.end(),
                                       [wpid](PCB *p) { return p->pid == wpid; });
                if (it != blocked_queue.end())
                    wakeup_process(*it);
                else
                    std::cout << "[错误] 未找到 PID=" << wpid << " 的阻塞进程\n";
            }
        }
        else if (cmd == "finished")
        {
            if (!running)
            {
                std::cout << "[提示] 当前无运行进程\n";
                continue;
            }
            std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
            print_pcb(running);
            PCB *tmp = running;
            running = nullptr;
            finish_process(tmp);
            running = schedule();
            if (running)
                dispatch(running);
            else
                std::cout << "[调度] 就绪队列为空\n";
        }
        else if (cmd == "tick")
        {
            sys_clock++;
            if (running)
            {
                running->slice_used++;
                running->remaining_time--;
                running->elapsed_time++;
                std::cout << "  时钟=" << sys_clock << " : 进程 " << running->name
                          << " 运行 (剩余=" << running->remaining_time
                          << ", 片内=" << running->slice_used << "/" << get_slice(running) << ")\n";

                if (running->slice_used >= get_slice(running))
                {
                    std::cout << "  => 时间片用完，触发抢占\n";
                    std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
                    print_pcb(running);
                    PCB *tmp = running;
                    running = nullptr;
                    preempt_process(tmp);
                    running = schedule();
                    if (running)
                        dispatch(running);
                    else
                        std::cout << "[调度] 就绪队列为空\n";
                }
                if (running && running->remaining_time <= 0)
                {
                    std::cout << "[退出CPU] 进程 " << running->name << " PCB:\n";
                    print_pcb(running);
                    PCB *tmp = running;
                    running = nullptr;
                    finish_process(tmp);
                    running = schedule();
                    if (running)
                        dispatch(running);
                    else
                        std::cout << "[调度] 就绪队列为空\n";
                }
            }
            else
            {
                std::cout << "  时钟=" << sys_clock << " : CPU 空闲\n";
            }
            advance_io();
        }
        else if (cmd == "show")
        {
            print_snapshot();
        }
        else
        {
            std::cout << "[提示] 未知命令：" << cmd << "\n";
        }

        if (alive_count() == 0 && !running)
        {
            std::cout << "\n所有进程已完成，仿真结束。\n";
            print_snapshot();
            break;
        }
    }
}

/* ======================== 输入辅助 ======================== */
static int read_int(const std::string &prompt, int lo, int hi)
{
    int val;
    while (true)
    {
        std::cout << prompt << " [" << lo << "~" << hi << "]: ";
        if (std::cin >> val && val >= lo && val <= hi)
            return val;
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "  输入无效，请重新输入。\n";
    }
}

static void setup_processes()
{
    int n = read_int("请输入要创建的进程数量", 1, MAX_PROC - 1);
    for (int i = 0; i < n; i++)
    {
        std::cout << "进程 " << i + 1 << ":\n";
        std::string pname;
        std::cout << "  进程名称: ";
        std::cin >> pname;

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

/* ======================== main ======================== */
int main()
{
    std::cout << "单处理机进程调度模拟\n\n";

    init_system();

    std::cout << "调度策略：\n  1. 时间片轮转（RR）\n  2. 多级反馈队列（MFQ）\n";
    use_mfq = (read_int("请选择", 1, 2) == 2);

    if (!use_mfq)
    {
        time_slice = read_int("请输入时间片大小", 1, 20);
    }
    else
    {
        std::cout << "MFQ 共 " << MFQ_LEVELS << " 层，各层时间片：";
        for (int i = 0; i < MFQ_LEVELS; i++)
            std::cout << " [" << i << "]=" << mfq_slice[i];
        std::cout << "\n";
    }

    setup_processes();
    print_snapshot();

    std::cout << "\n运行模式：\n  1. 自动仿真\n  2. 人工干预\n";
    int mode = read_int("请选择", 1, 2);

    if (mode == 1)
        auto_simulate();
    else
        manual_simulate();

    return 0;
}
