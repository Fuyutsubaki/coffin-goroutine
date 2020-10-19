#include "coffin/goroutine.hpp"
#include "MyScheduler.hpp"
#include <iostream>

namespace app{
struct MyStrategy{
    void post_goroutine(std::shared_ptr<cfn::Goroutine> && g){
        global_scheduler.post([=]()mutable{g->execute();}); 
    }
};
void spown_task(cfn::Task<> && task){
    global_scheduler.post([g = std::make_shared<cfn::Goroutine>(std::move(task))]{
        g->execute();
    });
}

cfn::Task<> example1_1(){
    std::cout<<1<<std::endl;
    co_return;
}
void example1(){
    spown_task(example1_1());
    spown_task([]()->cfn::Task<>{
        std::cout<<2<<std::endl;
        co_return;
    }());
}

cfn::Task<> fibonacci_n(int n, std::shared_ptr<cfn::Sender<MyStrategy,int>> ch){
    int x=0;
    int y=1;
    for(int i=0;i<n;++i){
        co_await ch->send(x);
        int next = x+y;
        x=y;
        y=next;
    }
}
cfn::Task<> example2(){
    auto [sender,recver] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    spown_task(fibonacci_n(10, std::move(sender)));
    for(;;){
        auto ret = co_await recver->recv();
        if(!ret) break;
        std::cout<<*ret<<std::endl;
    }
}
 
cfn::Task<> fibonacci_seq(std::shared_ptr<cfn::Sender<MyStrategy,int>> ret_ch, std::shared_ptr<cfn::Recver<MyStrategy,int>> quit_ch){
    int x=0;
    int y=1;
    for(;;){
        auto [ret, quit] = co_await select(ret_ch->send(x), quit_ch->recv());
        if(ret){
            int next = x+y;
            x=y;
            y=next;
        }else if(quit){
            break;
        }
    }
}
cfn::Task<> example3(){
    auto [ret_send,ret_recv] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    auto [quit_send,quit_recv] = cfn::makeChannel<MyStrategy,int>(MyStrategy{},0);
    spown_task(fibonacci_seq(std::move(ret_send), std::move(quit_recv)));
    for(int i=0;i<10;++i){
        auto x = co_await ret_recv->recv();
        std::cout<<*x<<std::endl;
    }
    quit_send.reset(); // 明示的にclose
}


    
}

int main(){
    std::cout<<"---example1---"<<std::endl;
    app::example1();
    app::spown_task([]()->cfn::Task<>{
        app::global_scheduler.stop();
        co_return;
    }()); 
    app::global_scheduler.run(4);
    
    std::cout<<"---example2---"<<std::endl;
    app::spown_task([]()->cfn::Task<>{
        co_await app::example2();
        app::global_scheduler.stop();
    }()); 
    app::global_scheduler.run(4);
    
    std::cout<<"---example3---"<<std::endl;
    app::spown_task([]()->cfn::Task<>{
        co_await app::example3();
        app::global_scheduler.stop();
    }()); 
    app::global_scheduler.run(4);
}
