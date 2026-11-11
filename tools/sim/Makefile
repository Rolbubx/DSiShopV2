# Simulateur PC de l'interface (sans matériel DS). Nécessite gcc.
SRC := sim_main.c net_sim.c \
       ../../source/app.c ../../source/gfx.c ../../source/ui.c \
       ../../source/keyboard.c ../../source/font_data.c

dsishop_sim: $(SRC) $(wildcard ../../source/*.h)
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o $@ $(SRC)

clean:
	rm -f dsishop_sim

.PHONY: clean
