// Test PC de sysupdate.c : applique des paquets pris dans un dossier (ceux
// que fabrique catalog-server/catalog.py) sur une fausse carte SD.
//
//   test_sysupdate <dossier des paquets> <racine SD> apply
//   test_sysupdate <dossier des paquets> <racine SD> cancel:<n>   annule pendant le paquet n
//   test_sysupdate <dossier des paquets> <racine SD> corrupt:<n>  abîme le paquet n
//
// Affiche "fetch <nom>" pour chaque paquet téléchargé. Code de sortie : 0 si
// la mise à jour est allée au bout, 2 si elle a échoué (message sur stdout).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/sysupdate.h"

static const char *packs_dir;
static int cancel_at = -1, corrupt_at = -1, fetch_index;
static sysupd_manifest_t man;

static int fetch(void *ud, const char *path, const char *dest_base, char *err, size_t err_size)
{
    (void)ud;
    const char *name = strrchr(path, '/') + 1;
    printf("fetch %s\n", name);

    char src[700], dst[700];
    snprintf(src, sizeof(src), "%s/%s", packs_dir, name);
    snprintf(dst, sizeof(dst), "%s.000", dest_base);
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (!in || !out)
    {
        snprintf(err, err_size, "copie de %s impossible", src);
        return -1;
    }
    int c, n = 0;
    while ((c = fgetc(in)) != EOF)
    {
        if (fetch_index == corrupt_at && n == 40)
            c ^= 0x55;
        fputc(c, out);
        n++;
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int progress(void *ud, int pack, sysupd_phase_t phase, uint64_t done, uint64_t total)
{
    (void)ud; (void)done; (void)total;
    fetch_index = pack;
    return pack == cancel_at && phase == SYSUPD_EXTRACT;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        printf("usage : %s <paquets> <racine SD> apply|cancel:<n>|corrupt:<n>\n", argv[0]);
        return 1;
    }
    packs_dir = argv[1];
    const char *root = argv[2];
    if (sscanf(argv[3], "cancel:%d", &cancel_at) != 1)
        sscanf(argv[3], "corrupt:%d", &corrupt_at);

    char path[700];
    static char text[32 * 1024];
    snprintf(path, sizeof(path), "%s/manifest.txt", packs_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        printf("manifeste introuvable\n");
        return 1;
    }
    text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
    fclose(f);

    // Manifestes invalides : tous refusés
    static const char *const bad[] = {
        "", "twl2\tv1\t1\t0\na.zip\t1\t1\t", "twl1\t../x\t1\t0\n",
        "twl1\tv1\t2\t0\na.zip\t5\t5\t"
        "0000000000000000000000000000000000000000000000000000000000000000\t-\n",
        "twl1\tv1\t1\t0\n../a.zip\t5\t5\t"
        "0000000000000000000000000000000000000000000000000000000000000000\t-\n",
        "twl1\tv1\t1\t0\na.zip\t5\t5\t"
        "0000000000000000000000000000000000000000000000000000000000000000\t../etc\n",
        "twl1\tv1\t1\t0\na.zip\t5\t5\tabc\t-\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        char copy[400];
        snprintf(copy, sizeof(copy), "%s", bad[i]);
        if (sysupd_parse_manifest(copy, &man))
        {
            printf("manifeste invalide n°%zu accepté\n", i);
            return 1;
        }
    }

    if (!sysupd_parse_manifest(text, &man))
    {
        printf("manifeste refusé\n");
        return 1;
    }

    char ver[64] = "";
    printf("present %d\n", sysupd_present(root));
    printf("installed %s\n", sysupd_installed_version(root, ver, sizeof(ver)) ? ver : "?");
    printf("latest %s\n", man.tag);
    printf("resume %d\n", sysupd_resume_index(root, man.tag));

    sysupd_io_t io = { fetch, progress, NULL };
    char err[192] = "";
    if (sysupd_apply(root, &man, &io, err, sizeof(err)) != 0)
    {
        printf("error %s\n", err);
        return 2;
    }
    printf("done\n");
    return 0;
}
