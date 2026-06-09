CXX ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O0 -g -pthread

PROGRAMS := echo_server

.PHONY: all clean run-echo

all: $(PROGRAMS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run-echo: echo_server
	./echo_server

clean:
	rm -f $(PROGRAMS)
