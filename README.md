# coffin/goroutine

coffin/goroutine is a goroutine-like concurrency support library
```C++:sample.cpp
template<class value_type>
using MyChannel = cfn::BasicChannel<MyScheduler, value_type>;

cfn::Task<> recv(MyChannel<int>&ch, MyChannel<int>&done_ch){
    co_await cfn::select(
        done_ch.recv([&](auto){
            done = true;
        }),
        ch.recv([&](auto x){
            tmp.push_back(*x);
        })
    );
}
```