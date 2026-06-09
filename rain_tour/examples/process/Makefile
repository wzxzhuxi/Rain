CXX ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O0 -g

PROGRAMS := crash_supervisor

.PHONY: all clean run-supervisor

all: $(PROGRAMS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run-supervisor: crash_supervisor
	./crash_supervisor

clean:
	rm -f $(PROGRAMS)
