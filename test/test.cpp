#define IUTEST_USE_MAIN 1
#include <coffin/goroutine.hpp>
#include <iutest.hpp>
#include <thread>
#include<vector>

IUTEST(Goroutine, execute){
    int n;
    auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
        for(int i=0;i<3;++i){
            n = i;
            co_await std::experimental::suspend_always{};
        }
    }());
    for(int i=0;i<3;++i){
        g->execute();
        IUTEST_ASSERT_EQ(i,n);
    }
}

IUTEST(Goroutine, subtask_call){
    int n;
    auto gen_subtask = [&](int x) -> cfn::Task<> {
        for (int i = 0; i < 3; ++i){
            n = i + x;
            co_await std::experimental::suspend_always{};
        }
    };
    auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
        co_await gen_subtask(0);
        co_await gen_subtask(3);
    }());
    // co_await subtask()でsuspendせず
    // 6回で呼びきれる
    for(int i=0;i<6;++i){
        g->execute();
        IUTEST_ASSERT_EQ(i,n);
    }
}

IUTEST(Goroutine, subtask_return){
    int n;
    auto gen_subtask = []() -> cfn::Task<std::unique_ptr<int>> {
        co_return std::make_unique<int>(42);
    };
    auto g = std::make_shared<cfn::Goroutine>([&]() -> cfn::Task<> {
        n = *(co_await gen_subtask());
    }());
    g->execute();
    IUTEST_ASSERT_EQ(42, n);
}

struct MyScheduler{
    static inline std::deque<std::shared_ptr<cfn::Goroutine>> queue;
    static inline std::mutex mutex;
    static inline std::atomic<bool> on_ready = true;
    void spown_task(cfn::Task<>&&task){
        std::scoped_lock lock{mutex};
        auto g = std::make_shared<cfn::Goroutine>(std::move(task));
        queue.push_back(g);
    }
    void push_goroutine(std::shared_ptr<cfn::Goroutine> g, std::size_t wakeup_id){
        std::scoped_lock lock{mutex};
        g->wakeup_id = wakeup_id;
        queue.push_back(g);
    }
    std::shared_ptr<cfn::Goroutine> dequeue_task(){
        std::scoped_lock lock{mutex};
        if(queue.empty()){
            return nullptr;
        }
        auto p = queue.front();
        queue.pop_front();
        return p;
    }
    void stop(){
        on_ready.store(false);
    }
    void run(std::size_t num){
        on_ready.store(true);
        std::vector<std::thread> worker;
        for(int i=0;i<3;++i){
            std::thread th{[&]{
            while(on_ready.load()){
                auto task = MyScheduler::dequeue_task();
                if(task){
                    task->execute();
                }else{
                    std::this_thread::yield();
                }
            }
            }};
            worker.emplace_back(std::move(th));
        }
        for(auto&th:worker){
            th.join();
        }
    }
};
static MyScheduler gsc;

template<class value_type>
using MyChannel = cfn::BasicChannel<MyScheduler, value_type>;

template<class T>
MyChannel<T> make_channel(std::size_t queue_size){
    return {gsc, queue_size};
}


IUTEST(BasicChannel, SendRecv){
    std::vector<int> tmp;
    auto ch = make_channel<int>(0);
    gsc.spown_task(
        [&]() -> cfn::Task<> {
            for(int i=0;i<10;++i){
                co_await ch.send(i);
            }
            ch.close();
        }()
    );
    gsc.spown_task(
        [&]() -> cfn::Task<> {
            for(;;){
                auto r = co_await ch.recv();
                if(!r)break;
                tmp.push_back(*r);
            }
            gsc.stop();
        }()
    );
    gsc.run(1);
    IUTEST_ASSERT_EQ((std::vector{0,1,2,3,4,5,6,7,8,9}), tmp);
}

/*
cfn::Task<> send1(MyChannel<int>&ch){
    for(int i=0;i<3;++i){
        co_await ch.send(i);
    }
}

cfn::Task<> send2(MyChannel<int>&ch, MyChannel<int>&done_ch){
    co_await send1(ch);
    co_await send1(ch);
    co_await send1(ch);

    done_ch.close();
}

cfn::Task<> recv(MyChannel<int>&ch, MyChannel<int>&done_ch, std::atomic<bool> & all_done, std::vector<int>&tmp){
    int  done = false;
    while(!done){
        co_await cfn::select(
            done_ch.recv([&](auto){
                done = true;
            }),
            ch.recv([&](auto x){
                tmp.push_back(*x);
            })
        );
    }
    all_done.store(true);
}

// move only type
cfn::Task<> send_move(MyChannel<std::unique_ptr<int>>&ch){
    co_await ch.send(std::make_unique<int>(32));
}

cfn::Task<> recv_move(MyChannel<int>&ch){
    auto x = co_await ch.recv();
}


IUTEST(test_common, Ch)
{
    MyChannel<int> ch(MyScheduler{},0);
    MyChannel<int> done_ch(MyScheduler{},0);
    std::atomic<bool> all_done = false;
    std::vector<int> tmp;

    MyScheduler::spown_task(send2(ch, done_ch));
    MyScheduler::spown_task(recv(ch, done_ch, all_done, tmp));
    
    // ガバガバスレッドプール
    std::vector<std::thread> worker;
    for(int i=0;i<3;++i){
        std::thread th{[&]{
           while(!all_done.load()){
               auto task = MyScheduler::dequeue_task();
               if(task){
                   task->execute();
               }else{
                   std::this_thread::yield();
               }
           }
        }};
        worker.emplace_back(std::move(th));
    }
    for(auto&th:worker){
        th.join();
    }
    assert(tmp == (std::vector{0,1,2,0,1,2,0,1,2})); // 本当に？
}
*/