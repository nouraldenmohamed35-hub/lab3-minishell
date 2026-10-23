CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g

all: myshell

myshell: command.o tokenizer.o
	$(CXX) $(CXXFLAGS) -o myshell command.o tokenizer.o

%.o: %.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f *.o myshell

.PHONY: all clean

