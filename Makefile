APP = shell
CFLAGS = -Wall -Wvla -fsanitize=address

.PHONY: all clean

all: $(APP)

$(APP): $(APP).c
	gcc $(CFLAGS) -o $@ $^

clean:
	rm -f $(APP)
