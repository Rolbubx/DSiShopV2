# Tests PC de splitfile.c et zip.c. Nécessite gcc, zlib et Python 3.
SRC := test_zip.c ../../source/splitfile.c ../../source/zip.c
OUT ?= /tmp/dsishop_tests

test_zip: $(SRC) $(wildcard ../../source/*.h)
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ $(SRC) -lz

check: test_zip
	python3 gen_zips.py $(OUT) $(BIG)
	./test_zip $(OUT) $(BIG)

clean:
	rm -f test_zip

.PHONY: check clean
