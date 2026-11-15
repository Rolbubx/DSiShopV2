// Tests de splitfile.c et zip.c sur PC.
//   make -C tools/test && ./tools/test/test_zip <dossier> [--big]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/splitfile.h"
#include "../../source/zip.h"

static int failures;
static char dir[512];

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC : " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static unsigned char *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(*len);
    if (fread(d, 1, *len, f) != *len) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static int progress_calls;
static int on_progress(void *ud, uint64_t done, uint64_t total)
{
    (void)ud; (void)done; (void)total;
    progress_calls++;
    return 0;
}

static void test_zip(const char *name, uint64_t chunk, zip_err_t expected,
                     const char *expected_rom_name)
{
    printf("zip %-8s (morceaux de %llu octets)\n", name, (unsigned long long)chunk);

    char base[600], out[600], romname[128] = "";
    snprintf(base, sizeof(base), "%s/%s", dir, name);
    snprintf(out, sizeof(out), "%s/%s.out", dir, name);

    sfile_t sf;
    if (!sfile_open_read(&sf, base, chunk))
    {
        CHECK(0, "ouverture de %s", base);
        return;
    }
    printf("  %u morceau(x), %llu octets\n", sfile_part_count(&sf),
           (unsigned long long)sf.size);

    progress_calls = 0;
    zip_err_t err = zip_extract_rom(&sf, out, romname, sizeof(romname),
                                    on_progress, NULL);
    sfile_close(&sf);

    printf("  -> %s\n", zip_strerror(err));
    CHECK(err == expected, "attendu \"%s\"", zip_strerror(expected));

    if (expected == ZIP_OK && err == ZIP_OK)
    {
        CHECK(strcmp(romname, expected_rom_name) == 0, "nom %s au lieu de %s",
              romname, expected_rom_name);
        size_t a, b;
        char rom_path[600];
        snprintf(rom_path, sizeof(rom_path), "%s/rom.bin", dir);
        unsigned char *x = load(out, &a), *y = load(rom_path, &b);
        CHECK(x && y && a == b && memcmp(x, y, a) == 0, "contenu extrait différent");
        CHECK(progress_calls > 0, "progression jamais appelée");
        free(x);
        free(y);
        remove(out);
    }
}

static void test_split_write(uint64_t chunk)
{
    printf("splitfile écriture/lecture (morceaux de %llu octets)\n",
           (unsigned long long)chunk);

    char base[600];
    snprintf(base, sizeof(base), "%s/split", dir);

    unsigned char data[10007];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (unsigned char)(i * 31 + 7);

    sfile_t sf;
    CHECK(sfile_open_write(&sf, base, chunk), "ouverture en écriture");
    // Écritures de tailles irrégulières, à cheval sur les morceaux
    size_t pos = 0, step = 1;
    while (pos < sizeof(data))
    {
        size_t n = step < sizeof(data) - pos ? step : sizeof(data) - pos;
        CHECK(sfile_write(&sf, data + pos, n) == n, "écriture");
        pos += n;
        step = step * 3 + 1;
    }
    unsigned int parts = sfile_part_count(&sf);
    CHECK(sfile_close(&sf), "fermeture");
    printf("  %u morceau(x)\n", parts);
    CHECK(parts == (sizeof(data) + chunk - 1) / chunk, "nombre de morceaux");

    unsigned char back[sizeof(data)];
    CHECK(sfile_open_read(&sf, base, chunk), "ouverture en lecture");
    CHECK(sf.size == sizeof(data), "taille relue %llu", (unsigned long long)sf.size);
    CHECK(sfile_read(&sf, back, sizeof(back)) == sizeof(back), "lecture complète");
    CHECK(memcmp(back, data, sizeof(data)) == 0, "contenu relu différent");

    // Lecture aléatoire avec seek
    unsigned char b;
    CHECK(sfile_seek(&sf, 9999) && sfile_read(&sf, &b, 1) == 1 && b == data[9999],
          "seek");
    CHECK(sfile_read(&sf, back, 100) == sizeof(data) - 10000, "lecture en fin de fichier");
    sfile_close(&sf);

    sfile_remove(base);
    CHECK(!sfile_open_read(&sf, base, chunk), "morceaux non supprimés");
}

// Au-delà de 1000 morceaux, l'écriture doit échouer proprement
static void test_too_many_parts(void)
{
    printf("splitfile limite de %d morceaux\n", SPLIT_MAX_PARTS);
    char base[600];
    snprintf(base, sizeof(base), "%s/toomany", dir);
    unsigned char data[1001] = { 0 };
    sfile_t sf;
    CHECK(sfile_open_write(&sf, base, 1), "ouverture");
    CHECK(sfile_write(&sf, data, sizeof(data)) == SPLIT_MAX_PARTS, "écrit plus que la limite");
    CHECK(!sfile_close(&sf), "l'erreur n'est pas signalée");
    sfile_remove(base);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage : %s <dossier> [--big]\n", argv[0]);
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    test_split_write(13);
    test_too_many_parts();
    test_split_write(4096);
    test_split_write(1 << 20);

    test_zip("deflate", 1000, ZIP_OK, "MonJeu.nds");
    test_zip("stored", 4096, ZIP_OK, "Truc.nds");
    test_zip("norom", 1 << 20, ZIP_ERR_NO_ROM, NULL);
    test_zip("corrupt", 1 << 20, ZIP_ERR_CORRUPT, NULL);

    if (argc > 2 && strcmp(argv[2], "--big") == 0)
        test_zip("big", SPLIT_CHUNK_DEFAULT, ZIP_OK, "MonGrosJeu.nds");

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
