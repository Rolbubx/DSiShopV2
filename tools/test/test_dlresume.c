// Tests de dlresume.c sur PC :  ./test_dlresume <dossier>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../source/dlresume.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC : " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    char path[600], tmp[620];
    mkdir(argv[1], 0777);
    snprintf(path, sizeof(path), "%s/.dsishop_resume", argv[1]);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(path);

    printf("dlresume : enregistrer, relire, comparer\n");
    dlresume_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.link, sizeof(a.link), "github.com/auteur/monjeu");
    snprintf(a.url, sizeof(a.url), "https://dsi-shop.duckdns.org/file/auteur/monjeu/v1.2/MonJeu.zip");
    snprintf(a.name, sizeof(a.name), "MonJeu.zip");
    snprintf(a.release, sizeof(a.release), "v1.2");
    snprintf(a.etag, sizeof(a.etag), "\"2026-11-15T10:00:00Z\"");
    a.total = 6764573491ull;
    a.done = 4294967296ull + 123;
    CHECK(dlresume_save(path, &a), "enregistrement");
    CHECK(access(path, 0) == 0, "fichier absent");
    struct stat st;
    CHECK(stat(tmp, &st) != 0, "fichier temporaire resté");

    dlresume_t b;
    CHECK(dlresume_load(path, &b), "lecture");
    CHECK(!strcmp(b.link, a.link) && !strcmp(b.url, a.url) && !strcmp(b.name, a.name) &&
          !strcmp(b.release, a.release) && !strcmp(b.etag, a.etag), "textes différents");
    CHECK(b.total == a.total && b.done == a.done && !b.complete, "nombres différents (>4 Go)");

    a.complete = true;
    a.done = a.total;
    CHECK(dlresume_save(path, &a) && dlresume_load(path, &b) && b.complete && b.done == b.total,
          "remplacement et état terminé");

    printf("dlresume : correspondance avec un téléchargement\n");
    const char *link = a.link, *url = a.url, *rel = a.release, *name = a.name;
    CHECK(dlresume_matches(&b, link, url, rel, name, a.total), "même téléchargement");
    CHECK(dlresume_matches(&b, link, url, rel, name, 0), "taille inconnue du côté du serveur");
    CHECK(!dlresume_matches(&b, link, url, rel, name, a.total + 1), "taille différente");
    CHECK(!dlresume_matches(&b, "github.com/autre/projet", url, rel, name, a.total), "autre lien");
    CHECK(!dlresume_matches(&b, link, "https://autre/x.zip", rel, name, a.total), "autre adresse");
    CHECK(!dlresume_matches(&b, link, url, "v1.3", name, a.total), "autre version");
    CHECK(!dlresume_matches(&b, link, url, rel, "Autre.zip", a.total), "autre nom");

    printf("dlresume : fichiers abîmés ou absents\n");
    CHECK(!dlresume_load("/chemin/absent/.dsishop_resume", &b), "fichier absent");
    FILE *f = fopen(path, "wb");
    fputs("n'importe quoi\nlink=x\nurl=y\nname=z\n", f);
    fclose(f);
    CHECK(!dlresume_load(path, &b), "sans l'en-tête attendu");
    f = fopen(path, "wb");
    fputs("dsishop-resume 1\nlink=x\ntotal=10\ndone=3\n", f);
    fclose(f);
    CHECK(!dlresume_load(path, &b), "sans adresse ni nom");
    f = fopen(path, "wb");
    fputs("dsishop-resume 1\nlink=x\nurl=y\nname=z\ntotal=10\ndone=99\n", f);
    fclose(f);
    CHECK(!dlresume_load(path, &b), "reçu plus que le total");
    f = fopen(path, "wb");
    fputs("dsishop-resume 1\nlink=", f);
    for (int i = 0; i < 2000; i++)
        fputc('a', f);
    fputs("\nurl=y\nname=z\n", f);
    fclose(f);
    CHECK(!dlresume_load(path, &b), "ligne démesurée");
    snprintf(a.name, sizeof(a.name), "a\nb");
    CHECK(!dlresume_save(path, &a), "saut de ligne dans un champ");

    dlresume_clear(path);
    CHECK(access(path, 0) != 0, "effacement");

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
