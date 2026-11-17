CC        = gcc
CFLAGS    = -Wall -Wextra -O2
TARGET    = bin/quark
SRCS      = src/quarkgtk.c src/quark_formato.c
OBJS      = $(SRCS:.c=.o)
GTK4      = $(shell pkg-config --cflags --libs gtk4)
GSV       = $(shell pkg-config --cflags --libs gtksourceview-5)
GLIB      = $(shell pkg-config --cflags --libs glib-2.0 gio-2.0)

all: $(TARGET)

$(TARGET): $(OBJS) | bin
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(GTK4) $(GSV) $(GLIB)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@ $(GTK4) $(GSV) $(GLIB)

bin:
	@mkdir -p bin

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/quark
	install -Dm644 src/quarkgtk.ui /usr/local/share/quark/quarkgtk.ui
	install -Dm644 src/quark.css    /usr/local/share/quark/quark.css

uninstall:
	rm -f /usr/local/bin/quark
	rm -rf /usr/local/share/quark

clean:
	rm -f src/*.o $(TARGET)
	rm -rf bin

.PHONY: all run install uninstall clean