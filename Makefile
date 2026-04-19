CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
LDFLAGS  = -lasound -lncursesw -lpthread

TARGET   = midi-hw-player
SRC      = midiplayer.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Build OK: ./$(TARGET)"

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	@echo "Installed to ~/.local/bin/$(TARGET)"

clean:
	rm -f $(TARGET)
