# Tests PC de splitfile.c, zip.c et sysupdate.c. Nécessite gcc, zlib et Python 3.
SRC := test_zip.c ../../source/splitfile.c ../../source/zip.c
OUT ?= /tmp/dsishop_tests

CFLAGS_TEST := -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined

test_zip: $(SRC) $(wildcard ../../source/*.h)
	$(CC) $(CFLAGS_TEST) -o $@ $(SRC) -lz

test_remote: test_remote.c ../../source/games.c ../../source/remote_proto.c $(wildcard ../../source/*.h)
	$(CC) $(CFLAGS_TEST) -o $@ test_remote.c ../../source/games.c ../../source/remote_proto.c

LREMOTE_SRC := lremote_pc.c ../../source/lremote.c ../../source/lremote_net.c \
               ../../source/lremote_page.c ../../source/sha256.c ../../source/games.c

lremote_pc: $(LREMOTE_SRC) $(wildcard ../../source/*.h)
	$(CC) $(CFLAGS_TEST) -o $@ $(LREMOTE_SRC)

SYSUPD_SRC := test_sysupdate.c ../../source/sysupdate.c ../../source/zip.c \
              ../../source/splitfile.c ../../source/sha256.c

test_sysupdate: $(SYSUPD_SRC) $(wildcard ../../source/*.h)
	$(CC) $(CFLAGS_TEST) -o $@ $(SYSUPD_SRC) -lz

check: test_zip test_remote lremote_pc test_sysupdate
	python3 gen_zips.py $(OUT) $(BIG)
	./test_zip $(OUT) $(BIG)
	rm -rf $(OUT)/games_test
	./test_remote $(OUT)
	python3 test_lremote.py ./lremote_pc $(OUT)
	python3 test_sysupdate.py ./test_sysupdate $(OUT) $(TWL_RELEASE)

clean:
	rm -f test_zip test_remote lremote_pc test_sysupdate

.PHONY: check clean
