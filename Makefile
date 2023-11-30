TARGET := btwatch.exe
CXXFLAGS := -std=c++23 -O2 -static -municode

$(TARGET): main.cpp
	g++ -o $@ main.cpp $(CXXFLAGS)

.PHONY: debug
debug: main.cpp
	g++ -o btwatch-debug.exe main.cpp $(CXXFLAGS) -g -O

.PHONY: run
run: $(TARGET)
	./$<

.PHONY: release
release: release/$(TARGET)

release/$(TARGET): main.cpp
	@test -d "release" || mkdir release
	g++ -o $@ main.cpp $(CXXFLAGS) -mwindows -DNO_CONSOLE

.PHONY: clean
clean:
	$(RM) $(TARGET)

.PHONY: all
all: clean $(TARGET)
