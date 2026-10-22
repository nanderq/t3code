# Builds the desktop harness for the protocol core. Uses the system C compiler.
CC ?= cc
SRC := hostcli.c $(addprefix ../source/,json.c net.c http.c ws.c util.c model.c models.c snooze.c client.c store.c questions.c queue.c history.c tasks.c prdetail.c diff.c launch.c threadops.c)

hostcli: $(SRC) $(wildcard ../source/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -o $@ $(SRC) -lz

feature_tests: feature_tests.c $(filter-out hostcli.c ../source/client.c,$(SRC)) $(wildcard ../source/*.h) ../source/client.c
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -o $@ feature_tests.c $(filter-out hostcli.c ../source/client.c,$(SRC)) -lz

test: feature_tests parser-test tier2b_test diff-test
	./feature_tests
	./parser-test
	./tier2b_test
	./diff-test
	python3 ws_test.py

# Needs libretro.h from libretro-common next to this file.
retroshot: retroshot.c
	$(CC) -std=gnu11 -O1 -o $@ retroshot.c

clean:
	rm -f hostcli retroshot feature_tests parser-test tier2b_test diff-test

.PHONY: clean test

parser-test: parser_test.c $(addprefix ../source/,json.c util.c model.c questions.c queue.c) $(wildcard ../source/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -o $@ $(filter %.c,$^)

# Offline protocol fixtures; no network connection is opened.
tier2b_test: tier2b_test.c $(addprefix ../source/,json.c util.c model.c questions.c queue.c history.c tasks.c prdetail.c) $(wildcard ../source/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -o $@ $(filter %.c,$^)

diff-test: diff_test.c ../source/diff.c ../source/launch.c ../source/json.c ../source/util.c $(wildcard ../source/*.h)
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -o $@ $(filter %.c,$^)
