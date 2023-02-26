iutest/include/iutest.hpp:
	git clone -b v1.17.1 --depth 1 https://github.com/srz-zumix/iutest.git 

.PHONY:test
test: iutest/include/iutest.hpp
	clang++-14 test.cpp -I iutest/include/ -I ../include/ -lpthread -std=c++2a -g -o test
	./test
