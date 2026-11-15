CC        = gcc
CFLAGS    = -Wall -Wextra -O2
TARGET    = bin/quarkgtk
SRC       = quarkgtk.c
GTK4      = $(shell pkg-config --cflags --libs gtk4)
GSV       = $(shell pkg-config --cflags --libs gtksourceview-5)

all: $(TARGET)

$(TARGET): $(SRC) | bin
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(GTK4) $(GSV)

bin:
	@mkdir -p bin

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/quark
	install -Dm644 quarkgtk.ui /usr/local/share/quark/quarkgtk.ui
	install -Dm644 quark.css    /usr/local/share/quark/quark.css

uninstall:
	rm -f /usr/local/bin/quark
	rm -rf /usr/local/share/quark

clean:
	rm -rf bin

.PHONY: all run install uninstall clean