// Fichier "découpé" : un gros fichier stocké en plusieurs morceaux
// (base.000, base.001...) pour contourner la limite de 4 Go du FAT32.
// On le lit / écrit comme un seul fichier, avec des positions 64 bits.
// Portable (stdio uniquement) : testé sur PC dans tools/test.
#ifndef SPLITFILE_H__
#define SPLITFILE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// 1 Gio : sous la limite FAT32 (4 Gio - 1) et sous celle de fseek() (2 Gio)
#define SPLIT_CHUNK_DEFAULT (1024ull * 1024 * 1024)

// Morceaux numérotés de .000 à .999
#define SPLIT_MAX_PARTS 1000

typedef struct {
    char base[240];
    uint64_t chunk;     // Taille max d'un morceau
    uint64_t pos;       // Position courante
    uint64_t size;      // Taille totale
    FILE *f;            // Morceau ouvert (ou NULL)
    unsigned int part;  // Numéro du morceau ouvert
    bool writing;
    bool error;
} sfile_t;

// Chemin du morceau n : "<base>.000"
void sfile_part_path(const char *base, unsigned int n, char *out, size_t size);

bool sfile_open_write(sfile_t *sf, const char *base, uint64_t chunk);
size_t sfile_write(sfile_t *sf, const void *data, size_t len);

bool sfile_open_read(sfile_t *sf, const char *base, uint64_t chunk);
size_t sfile_read(sfile_t *sf, void *data, size_t len);
bool sfile_seek(sfile_t *sf, uint64_t pos);

// Renvoie false si une erreur d'écriture a eu lieu
bool sfile_close(sfile_t *sf);

unsigned int sfile_part_count(const sfile_t *sf);

// Supprime tous les morceaux
void sfile_remove(const char *base);

#endif // SPLITFILE_H__
