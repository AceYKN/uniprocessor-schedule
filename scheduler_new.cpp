/*
 * 实验三：单处理机进程调度（RR + MFQ）
 * 状态：FREE / READY / RUNNING / BLOCKED / FINISHED
 */
#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <array>
#include <limits>
#include <algorithm>

constexpr int MAX_PROC = 64;
constexpr int DEF_SLICE = 2;
constexpr int MFQ_LEVELS = 3;

/* ===== PCB ===== */
struct PCB
{
    int pid = 0;
    std::string name;
    std::string state = "FREE"; // 用字符串直接表示状态
    int total_time = 0, remaining_time = 0;
    int slice_used = 0, dispatch_count = 0;
    int io_at = -1, io_duration = 0, io_remaining = 0, elapsed_time = 0;
    int reg_pc = 0, reg_acc = 0;
    int mfq_level = 0;
};

using ProcQueue = std::list<PCB *>;

/* ===== 全局数据 ===== */
static std::vector<PCB> pcb_pool(MAX_PROC);
static ProcQueue free_queue, ready_queue, run_queue, blocked_queue, finished_queue;
static std::array<ProcQueue, MFQ_LEVELS> mfq;
static std::array<int, MFQ_LEVELS> mfq_slice{};
static int next_pid = 1, sys_clock = 0, time_slice = DEF_SLICE;
static bool use_mfq = false;

/* ===== 工具 ===== */
static int get_slice(const PCB *p)
{
    return use_mfq ? mfq_slice[p->mfq_level] : time_slice;
}

// 进程入就绪队列（兼容 RR / MFQ）
static void to_ready(PCB *p)
{
    p->state = "READY";
    (use_mfq ? mfq[p->mfq_level] : ready_queue).push_back(p);
}

// 从就绪队列取下一个进程
static PCB *pick_next()
{
    if (!use_mfq)
    {
        if (ready_queue.empty())
            return nullptr;
        PCB *p = ready_queue.front();
        ready_queue.pop_front();
        return p;
    }
    for (auto &q : mfq)
        if (!q.empty())
        {
            PCB *p = q.front();
            q.pop_front();
            return p;
        }
    return nullptr;
}

static int alive_count()
{
    int n = run_queue.size() + blocked_queue.size();
    if (!use_mfq)
        n += ready_queue.size();
    else
        for (auto &q : mfq)
            n += q.size();
    return n;
}

/* ===== 打印 ===== */
static void print_queue(const std::string &label, const ProcQueue &q)
{
    std::cout << "  [" << label << "](" << q.size() << "): ";
    for (const auto *p : q)
        std::cout << p->name << "(P" << p->pid << ") ";
    std::cout << "\n";
}

static void print_pcb(const PCB *p)
{
    std::cout << "  PCB " << p->name << "(P" << p->pid << ")"
              << " 状态=" << p->state
              << " 剩余=" << p->remaining_time
              << " 调度次=" << p->dispatch_count
              << " PC=" << p->reg_pc << " ACC=" << p->reg_acc;
    if (use_mfq)
        std::cout << " MFQ=" << p->mfq_level;
    std::cout << "\n";
}

static void print_snapshot()
{
    std::cout << "\n  ── 时钟=" << sys_clock << " 队列快照 ──\n";
    if (!use_mfq)
        print_queue("就绪", ready_queue);
    else
        for (int i = 0; i < MFQ_LEVELS; i++)
            print_queue("就绪MFQ[" + std::to_string(i) + "]", mfq[i]);
    print_queue("运行", run_queue);
    print_queue("阻塞", blocked_queue);
    print_queue("完成", finished_queue);
    std::cout << "  ──────────────────\n\n";
}

/* ===== 初始化 ===== */
static void init_system()
{
    for (auto &pcb : pcb_pool)
    {
        pcb = PCB{};
        free_queue.push_back(&pcb);
    }
    for (int i = 0; i < MFQ_LEVELS; i++)
        mfq_slice[i] = DEF_SLICE * (1 << i);
}

/* ===== 原语 ===== */
static void create_process(const std::string &name, int total, int io_at, int io_dur)
{
    if (free_queue.empty())
    {
        std::cerr << "[ERROR] 无可用PCB\n";
        return;
    }
    PCB *p = free_queue.front();
    free_queue.pop_front();
    *p = PCB{};
    p->pid = next_pid++;
    p->name = name;
    p->total_time = p->remaining_time = total;
    p->io_at = io_at;
    p->io_duration = io_dur;
    to_ready(p);
    std::cout << "[创建] " << name << "(P" << p->pid << ") 总时=" << total
              << " IO@" << io_at << " IO时长=" << io_dur << "\n";
}

static void dispatch(PCB *p)
{
    p->state = "RUNNING";
    p->dispatch_count++;
    p->slice_used = 0;
    run_queue.push_back(p);
    std::cout << "\n[调度] '" << p->name << "' 第" << p->dispatch_count << "个时间片\n";
    print_pcb(p);
}

static void block_process(PCB *p)
{
    if (!p || p->state != "RUNNING")
        return;
    p->reg_pc += p->slice_used;
    p->reg_acc += p->slice_used * 2;
    run_queue.pop_front();
    p->state = "BLOCKED";
    p->io_remaining = p->io_duration;
    blocked_queue.push_back(p);
    std::cout << "[阻塞] " << p->name << "(P" << p->pid << ")\n";
    print_snapshot();
}

static void wakeup_process(PCB *p)
{
    if (!p || p->state != "BLOCKED")
        return;
    blocked_queue.remove(p);
    p->io_remaining = 0;
    to_ready(p);
    std::cout << "[唤醒] " << p->name << "(P" << p->pid << ")\n";
    print_snapshot();
}

static void finish_process(PCB *p)
{
    run_queue.pop_front();
    p->state = "FINISHED";
    p->remaining_time = 0;
    finished_queue.push_back(p);
    std::cout << "[完成] " << p->name << "(P" << p->pid << ")\n";
    print_snapshot();
}

static void preempt_process(PCB *p)
{
    run_queue.pop_front();
    p->reg_pc += p->slice_used;
    p->reg_acc += p->slice_used * 2;
    p->slice_used = 0;
    if (use_mfq && p->mfq_level < MFQ_LEVELS - 1)
        p->mfq_level++;
    to_ready(p);
    std::cout << "[抢占] " << p->name << "(P" << p->pid << ") MFQ=" << p->mfq_level << "\n";
    print_snapshot();
}

static void advance_io()
{
    std::vector<PCB *> wake;
    for (auto *bp : blocked_queue)
        if (bp->io_remaining > 0 && --bp->io_remaining == 0)
            wake.push_back(bp);
    for (auto *bp : wake)
        wakeup_process(bp);
}

/* ===== 输入辅助 ===== */
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
        std::cout << "  输入无效，请重试\n";
    }
}

/* ===== 场景一：自动仿真 ===== */
static void auto_simulate()
{
    std::cout << "\n=== 自动仿真(" << (use_mfq ? "MFQ" : "RR") << ") ===\n";
    PCB *running = nullptr;
    while (alive_count() || running)
    {
        if (!running)
        {
            running = pick_next();
            if (running)
                dispatch(running);
            else
            {
                sys_clock++;
                advance_io();
                continue;
            }
        }
        sys_clock++;
        running->slice_used++;
        running->remaining_time--;
        running->elapsed_time++;
        std::cout << "  t=" << sys_clock << " " << running->name
                  << " (剩余=" << running->remaining_time
                  << " 片内=" << running->slice_used << "/" << get_slice(running) << ")\n";
        advance_io();

        if (running->io_at >= 0 && running->elapsed_time == running->io_at)
        {
            std::cout << "  => " << running->name << " 触发I/O\n";
            print_pcb(running);
            PCB *t = running;
            running = nullptr;
            block_process(t);
            continue;
        }
        if (running->remaining_time <= 0)
        {
            print_pcb(running);
            PCB *t = running;
            running = nullptr;
            finish_process(t);
            continue;
        }
        if (running->slice_used >= get_slice(running))
        {
            print_pcb(running);
            PCB *t = running;
            running = nullptr;
            preempt_process(t);
            running = pick_next();
            if (running)
                dispatch(running);
        }
    }
    std::cout << "\n=== 仿真结束 t=" << sys_clock << " ===\n已完成进程:\n";
    for (const auto *p : finished_queue)
        print_pcb(p);
}

/* ===== 场景二：人工干预 ===== */
static void manual_simulate()
{
    std::cout << "\n=== 人工干预(" << (use_mfq ? "MFQ" : "RR") << ") ===\n"
              << "命令: enter | esc | wakeup | finished | tick | show | quit\n\n";
    PCB *running = pick_next();
    if (running)
        dispatch(running);
    print_snapshot();

    std::string cmd;
    while (std::cin >> cmd)
    {
        if (cmd == "quit")
            break;
        else if (cmd == "enter")
        {
            if (running)
            {
                print_pcb(running);
                run_queue.pop_front();
                running->slice_used = 0;
                running->reg_pc++;
                running->reg_acc += 2;
                to_ready(running);
                running = nullptr;
            }
            running = pick_next();
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
                std::cout << "[提示] 无运行进程\n";
                continue;
            }
            PCB *t = running;
            running = nullptr;
            block_process(t);
            running = pick_next();
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
            int idx = 1;
            for (const auto *bp : blocked_queue)
                std::cout << "  " << idx++ << ". " << bp->name << "(P" << bp->pid << ")\n";
            std::cout << "输入PID: ";
            int wpid;
            std::cin >> wpid;
            auto it = std::find_if(blocked_queue.begin(), blocked_queue.end(),
                                   [wpid](PCB *p)
                                   { return p->pid == wpid; });
            if (it != blocked_queue.end())
                wakeup_process(*it);
            else
                std::cout << "[错误] 未找到P" << wpid << "\n";
        }
        else if (cmd == "finished")
        {
            if (!running)
            {
                std::cout << "[提示] 无运行进程\n";
                continue;
            }
            print_pcb(running);
            PCB *t = running;
            running = nullptr;
            finish_process(t);
            running = pick_next();
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
                std::cout << "  t=" << sys_clock << " " << running->name
                          << " 剩余=" << running->remaining_time
                          << " 片内=" << running->slice_used << "/" << get_slice(running) << "\n";
                if (running->slice_used >= get_slice(running))
                {
                    std::cout << "  => 时间片用完\n";
                    print_pcb(running);
                    PCB *t = running;
                    running = nullptr;
                    preempt_process(t);
                    running = pick_next();
                    if (running)
                        dispatch(running);
                    else
                        std::cout << "[调度] 就绪队列为空\n";
                }
                if (running && running->remaining_time <= 0)
                {
                    print_pcb(running);
                    PCB *t = running;
                    running = nullptr;
                    finish_process(t);
                    running = pick_next();
                    if (running)
                        dispatch(running);
                    else
                        std::cout << "[调度] 就绪队列为空\n";
                }
            }
            else
            {
                std::cout << "  t=" << sys_clock << " CPU空闲\n";
            }
            advance_io();
        }
        else if (cmd == "show")
        {
            print_snapshot();
        }
        else
        {
            std::cout << "[提示] 未知命令: " << cmd << "\n";
        }
        if (!alive_count() && !running)
        {
            std::cout << "\n所有进程已完成\n";
            print_snapshot();
            break;
        }
    }
}

/* ===== main ===== */
int main()
{
    std::cout << "=== 单处理机进程调度模拟 ===\n\n";
    init_system();

    std::cout << "调度策略: 1.时间片轮转(RR)  2.多级反馈队列(MFQ)\n";
    use_mfq = (read_int("选择", 1, 2) == 2);
    if (!use_mfq)
    {
        time_slice = read_int("时间片大小", 1, 20);
    }
    else
    {
        std::cout << "MFQ各层时间片:";
        for (int i = 0; i < MFQ_LEVELS; i++)
            std::cout << " [" << i << "]=" << mfq_slice[i];
        std::cout << "\n";
    }

    int n = read_int("进程数量", 1, MAX_PROC - 1);
    for (int i = 0; i < n; i++)
    {
        std::cout << "进程" << i + 1 << " 名称: ";
        std::string nm;
        std::cin >> nm;
        int total = read_int("  总运行时间", 1, 100);
        int io_at = -1, io_dur = 0;
        if (read_int("  有I/O? (1=有 0=无)", 0, 1))
        {
            io_at = read_int("  I/O触发时刻", 1, total);
            io_dur = read_int("  I/O持续时间", 1, 20);
        }
        create_process(nm, total, io_at, io_dur);
    }
    print_snapshot();

    std::cout << "\n运行模式: 1.自动仿真  2.人工干预\n";
    if (read_int("选择", 1, 2) == 1)
        auto_simulate();
    else
        manual_simulate();
    return 0;
}
